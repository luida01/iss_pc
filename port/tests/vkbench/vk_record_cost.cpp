// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: what the Vulkan driver costs when the port records its commands.
//
// The draw profile (TLG_DRAW_PROFILE=1) says that the Vulkan recording thread spends
// 20.6 % of the window inside the driver's command recording, and that the port emits
// about 11.6 commands per draw (5.9 of them dynamic state, 1.6 barriers). This bench
// measures what each of those commands costs in the driver on this machine, so the
// report can say where that 20.6 % is and how much of it "record fewer commands" could
// remove.
//
// The measurement records the command many times into a command buffer that is already
// recording (the port's situation) and divides the wall time by the number of
// repetitions. The state around the command is the port's: a render pass open (dynamic
// rendering), the pipeline bound, the vertex and index buffers bound.
//
// No game and no guest: only the driver. The numbers are per command in nanoseconds.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define VK_ENABLE_BETA_EXTENSIONS
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#define VULKAN_HPP_NO_STRUCT_CONSTRUCTORS
#define VULKAN_HPP_NO_STRUCT_SETTERS
#include <vulkan/vulkan.hpp>

#include <cstdint>
#ifdef _WIN32
#endif

#include "TRI_SPIRV_VERT.h"
#include "TRI_SPIRV_FRAG.h"

// tlg: the default dispatcher of Vulkan-Hpp lives in its own library sources; the bench
// does not link it, so the definition is here.
namespace vk::detail {
VULKAN_HPP_STORAGE_API DispatchLoaderDynamic defaultDispatchLoaderDynamic;
} // namespace vk::detail

namespace {

using Clock = std::chrono::steady_clock;

/// Repetitions of each command (the warm-up ones are not counted).
constexpr uint32_t kWarmup = 64;
constexpr uint32_t kIterations = 4096;

struct Ctx {
    vk::Instance instance;
    vk::PhysicalDevice physical;
    vk::Device device;
    vk::Queue queue;
    uint32_t family = 0;
    vk::CommandPool pool;
    vk::Image color_image;
    vk::DeviceMemory color_memory;
    vk::ImageView color_view;
    vk::Image depth_image;
    vk::DeviceMemory depth_memory;
    vk::ImageView depth_view;
    vk::Buffer vertex_buffer;
    vk::DeviceMemory vertex_memory;
    vk::Buffer index_buffer;
    vk::DeviceMemory index_memory;
    vk::Buffer descriptor_buffer;
    vk::DeviceMemory descriptor_memory;
    vk::DescriptorSetLayout set_layout;
    vk::PipelineLayout pipeline_layout;
    vk::Pipeline pipeline;

    bool vertex_input_dynamic = false;
    bool dynamic_state_3 = false;
    bool color_write_mask = false;
    bool feedback_loop = false;
};

bool HasExtension(const std::vector<vk::ExtensionProperties>& available, const char* name) {
    return std::any_of(available.begin(), available.end(), [&](const auto& ext) {
        return std::strcmp(ext.extensionName, name) == 0;
    });
}

uint32_t FindMemoryType(const Ctx& ctx, uint32_t type_bits, vk::MemoryPropertyFlags properties) {
    const vk::PhysicalDeviceMemoryProperties mem_props = ctx.physical.getMemoryProperties();
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}


void CreateBuffer(Ctx& ctx, vk::BufferUsageFlags usage, vk::MemoryPropertyFlags props,
                  vk::DeviceSize size, vk::Buffer& buffer, vk::DeviceMemory& memory) {
    const vk::BufferCreateInfo ci{
        .size = size,
        .usage = usage,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    buffer = ctx.device.createBuffer(ci);
    const vk::MemoryRequirements req = ctx.device.getBufferMemoryRequirements(buffer);
    const vk::MemoryAllocateInfo ai{
        .allocationSize = req.size,
        .memoryTypeIndex = FindMemoryType(ctx, req.memoryTypeBits, props),
    };
    memory = ctx.device.allocateMemory(ai);
    ctx.device.bindBufferMemory(buffer, memory, 0);
}

/// The dynamic state the port's pipelines declare (vk_graphics_pipeline.cpp).
std::vector<vk::DynamicState> PortDynamicStates(const Ctx& ctx) {
    std::vector<vk::DynamicState> states = {
        vk::DynamicState::eViewportWithCount,  vk::DynamicState::eScissorWithCount,
        vk::DynamicState::eBlendConstants,     vk::DynamicState::eDepthTestEnable,
        vk::DynamicState::eDepthWriteEnable,   vk::DynamicState::eDepthCompareOp,
        vk::DynamicState::eDepthBiasEnable,    vk::DynamicState::eDepthBias,
        vk::DynamicState::eStencilTestEnable,  vk::DynamicState::eStencilReference,
        vk::DynamicState::eStencilCompareMask, vk::DynamicState::eStencilWriteMask,
        vk::DynamicState::eStencilOp,          vk::DynamicState::eCullMode,
        vk::DynamicState::eFrontFace,          vk::DynamicState::eRasterizerDiscardEnable,
        vk::DynamicState::eLineWidth,          vk::DynamicState::ePrimitiveRestartEnable,
    };
    if (ctx.dynamic_state_3) {
        states.push_back(vk::DynamicState::eDepthBoundsTestEnable);
        states.push_back(vk::DynamicState::eDepthBounds);
    }
    if (ctx.color_write_mask) {
        states.push_back(vk::DynamicState::eColorWriteMaskEXT);
    }
    if (ctx.vertex_input_dynamic) {
        states.push_back(vk::DynamicState::eVertexInputEXT);
    }
    return states;
}

void CreatePipeline(Ctx& ctx) {
    const vk::ShaderModuleCreateInfo vert_ci{
        .codeSize = sizeof(TRI_SPIRV_VERT),
        .pCode = TRI_SPIRV_VERT,
    };
    const vk::ShaderModule vert = ctx.device.createShaderModule(vert_ci);
    const vk::ShaderModuleCreateInfo frag_ci{
        .codeSize = sizeof(TRI_SPIRV_FRAG),
        .pCode = TRI_SPIRV_FRAG,
    };
    const vk::ShaderModule frag = ctx.device.createShaderModule(frag_ci);

    const std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {{
        {.stage = vk::ShaderStageFlagBits::eVertex, .module = vert, .pName = "main"},
        {.stage = vk::ShaderStageFlagBits::eFragment, .module = frag, .pName = "main"},
    }};

    // The storage buffer descriptors: the port pushes its buffers and textures this
    // way (a scene draw has a dozen or more).
    static constexpr uint32_t kBindings = 32;
    std::array<vk::DescriptorSetLayoutBinding, kBindings> set_bindings{};
    for (uint32_t i = 0; i < kBindings; i++) {
        set_bindings[i] = vk::DescriptorSetLayoutBinding{
            .binding = i,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eVertex,
        };
    }
    const vk::DescriptorSetLayoutCreateInfo set_ci{
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = kBindings,
        .pBindings = set_bindings.data(),
    };
    ctx.set_layout = ctx.device.createDescriptorSetLayout(set_ci);
    const vk::PipelineLayoutCreateInfo layout_ci{
        .setLayoutCount = 1,
        .pSetLayouts = &ctx.set_layout,
    };
    ctx.pipeline_layout = ctx.device.createPipelineLayout(layout_ci);

    const std::array<vk::VertexInputBindingDescription, 1> bindings = {{
        {.binding = 0, .stride = 12, .inputRate = vk::VertexInputRate::eVertex},
    }};
    const std::array<vk::VertexInputAttributeDescription, 1> attributes = {{
        {.location = 0, .binding = 0, .format = vk::Format::eR32G32B32Sfloat, .offset = 0},
    }};
    const vk::PipelineVertexInputStateCreateInfo vertex_input{
        .vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size()),
        .pVertexBindingDescriptions = bindings.data(),
        .vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size()),
        .pVertexAttributeDescriptions = attributes.data(),
    };
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
        .primitiveRestartEnable = vk::True,
    };
    const vk::PipelineViewportStateCreateInfo viewport_state{
        .viewportCount = 1,
        .scissorCount = 1,
    };
    const vk::PipelineRasterizationStateCreateInfo raster{
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eClockwise,
        .lineWidth = 1.0f,
    };
    const vk::PipelineMultisampleStateCreateInfo multisample{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };
    const vk::PipelineDepthStencilStateCreateInfo depth_stencil{
        .depthTestEnable = vk::True,
        .depthWriteEnable = vk::True,
        .depthCompareOp = vk::CompareOp::eLess,
    };
    const vk::PipelineColorBlendAttachmentState blend_attachment{
        .blendEnable = vk::False,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };
    const vk::PipelineColorBlendStateCreateInfo color_blend{
        .attachmentCount = 1,
        .pAttachments = &blend_attachment,
    };
    const auto dynamic_states = PortDynamicStates(ctx);
    const vk::PipelineDynamicStateCreateInfo dynamic_state{
        .dynamicStateCount = static_cast<uint32_t>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };
    static constexpr std::array<vk::Format, 1> color_format{vk::Format::eR8G8B8A8Unorm};
    const vk::PipelineRenderingCreateInfo rendering_ci{
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = color_format.data(),
    };
    const vk::GraphicsPipelineCreateInfo ci{
        .pNext = &rendering_ci,
        .stageCount = static_cast<uint32_t>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depth_stencil,
        .pColorBlendState = &color_blend,
        .pDynamicState = &dynamic_state,
        .layout = ctx.pipeline_layout,
    };
    auto [pipeline_result, pipeline] = ctx.device.createGraphicsPipeline(nullptr, ci);
    if (pipeline_result != vk::Result::eSuccess) {
        std::printf("error: the pipeline could not be created (%s)\n",
                    vk::to_string(pipeline_result).c_str());
        return;
    }
    ctx.pipeline = pipeline;
}

/// A command buffer that is recording (fresh for every measurement).
vk::CommandBuffer BeginRecording(Ctx& ctx) {
    ctx.device.resetCommandPool(ctx.pool);
    const vk::CommandBufferAllocateInfo ai{
        .commandPool = ctx.pool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 1,
    };
    vk::CommandBuffer cmd = ctx.device.allocateCommandBuffers(ai)[0];
    cmd.begin(vk::CommandBufferBeginInfo{});
    return cmd;
}

/// Records `fn` `count` times and returns the nanoseconds each call took.
template <typename Fn>
double Measure(vk::CommandBuffer cmd, Fn&& fn, uint32_t count = kIterations) {
    for (uint32_t i = 0; i < kWarmup; ++i) {
        fn(cmd);
    }
    const auto start = Clock::now();
    for (uint32_t i = 0; i < count; ++i) {
        fn(cmd);
    }
    const auto end = Clock::now();
    return std::chrono::duration<double, std::nano>(end - start).count() / count;
}

/// The measurement with the state the port records in: a render pass open (dynamic
/// rendering) and the pipeline bound.
template <typename Fn>
double MeasureInPass(Ctx& ctx, Fn&& fn, uint32_t count = kIterations) {
    vk::CommandBuffer cmd = BeginRecording(ctx);
    const vk::RenderingAttachmentInfo color{
        .imageView = ctx.color_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingAttachmentInfo depth{
        .imageView = ctx.depth_view,
        .imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo info{
        .renderArea = {{0, 0}, {1280, 720}},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color,
        .pDepthAttachment = &depth,
    };
    cmd.beginRendering(info);
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, ctx.pipeline);
    const double ns = Measure(cmd, fn, count);
    cmd.endRendering();
    cmd.end();
    return ns;
}

/// The same, without the render pass (the state of the commands recorded outside a draw).
template <typename Fn>
double MeasurePlain(Ctx& ctx, Fn&& fn, uint32_t count = kIterations) {
    vk::CommandBuffer cmd = BeginRecording(ctx);
    const double ns = Measure(cmd, fn, count);
    cmd.end();
    return ns;
}

struct Row {
    std::string name;
    double ns;
};

void Print(const char* title, const std::vector<Row>& rows) {
    std::printf("\n%s\n", title);
    std::printf("%-36s %9s %12s\n", "command", "ns", "cycles@2.6GHz");
    for (const auto& row : rows) {
        std::printf("%-36s %9.1f %12.0f\n", row.name.c_str(), row.ns, row.ns * 2.6);
    }
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("tlg: the Vulkan command recording cost\n");
    static vk::detail::DynamicLoader dl;
    const auto get_proc = dl.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    if (!get_proc) {
        std::puts("SKIP: no Vulkan loader");
        return 0;
    }
    VULKAN_HPP_DEFAULT_DISPATCHER.init(get_proc);

    try {
        Ctx ctx;
        // The instance extensions the loader knows.
        const std::vector<vk::ExtensionProperties> instance_available =
            vk::enumerateInstanceExtensionProperties();
        std::vector<const char*> instance_extensions;
        for (const char* wanted : {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_SWAPCHAIN_EXTENSION_NAME}) {
            const bool found = std::any_of(
                instance_available.begin(), instance_available.end(),
                [&](const auto& ext) { return std::strcmp(ext.extensionName, wanted) == 0; });
            if (found) {
                instance_extensions.push_back(wanted);
            }
        }
        const vk::ApplicationInfo app{
            .pApplicationName = "tlg vk record cost",
            .apiVersion = VK_API_VERSION_1_3,
        };
        const vk::InstanceCreateInfo instance_ci{
            .pApplicationInfo = &app,
            .enabledExtensionCount = static_cast<uint32_t>(instance_extensions.size()),
            .ppEnabledExtensionNames = instance_extensions.data(),
        };
        ctx.instance = vk::createInstance(instance_ci);
        VULKAN_HPP_DEFAULT_DISPATCHER.init(ctx.instance);

        const std::vector<vk::PhysicalDevice> devices = ctx.instance.enumeratePhysicalDevices();
        if (devices.empty()) {
            std::puts("SKIP: no Vulkan device");
            return 0;
        }
        // The discrete GPU (the port runs on the NVIDIA one): the integrated one is
        // another device with another driver and other costs.
        for (const auto& device : devices) {
            std::printf("device found: %s (%s)\n", device.getProperties().deviceName.data(),
                        vk::to_string(device.getProperties().deviceType).c_str());
        }
        ctx.physical = devices.front();
        for (const auto& device : devices) {
            if (device.getProperties().deviceType == vk::PhysicalDeviceType::eDiscreteGpu) {
                ctx.physical = device;
                break;
            }
        }
        std::printf("device: %s\n", ctx.physical.getProperties().deviceName.data());

        const auto available = ctx.physical.enumerateDeviceExtensionProperties();
        const bool has_push_descriptor = HasExtension(available, "VK_KHR_push_descriptor");
        const bool has_vertex_input_dynamic =
            HasExtension(available, "VK_EXT_vertex_input_dynamic_state");
        const bool has_dynamic_state_3 = HasExtension(available, "VK_EXT_extended_dynamic_state3");
        const bool has_feedback_loop =
            HasExtension(available, "VK_EXT_attachment_feedback_loop_dynamic_state");

        std::vector<const char*> extensions;
        if (has_push_descriptor) {
            extensions.push_back("VK_KHR_push_descriptor");
        }
        if (has_vertex_input_dynamic) {
            extensions.push_back("VK_EXT_vertex_input_dynamic_state");
        }
        if (has_dynamic_state_3) {
            extensions.push_back("VK_EXT_extended_dynamic_state3");
        }
        if (has_feedback_loop) {
            extensions.push_back("VK_EXT_attachment_feedback_loop_layout");
            extensions.push_back("VK_EXT_attachment_feedback_loop_dynamic_state");
        }

        const vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT ds3{
            .extendedDynamicState3ColorWriteMask = has_dynamic_state_3 ? vk::True : vk::False,
        };
        const vk::PhysicalDeviceVertexInputDynamicStateFeaturesEXT vertex_input_dynamic{
            .vertexInputDynamicState = has_vertex_input_dynamic ? vk::True : vk::False,
        };
        const vk::PhysicalDeviceVulkan13Features features13{
            .synchronization2 = vk::True,
            .dynamicRendering = vk::True,
        };
        vk::StructureChain feature_chain = {
            vk::PhysicalDeviceFeatures2{},
            features13,
            ds3,
            vertex_input_dynamic,
        };
        // The queue the device needs (a device without queues makes the NVIDIA driver
        // crash when it allocates memory).
        const auto families = ctx.physical.getQueueFamilyProperties();
        uint32_t family = 0;
        for (uint32_t i = 0; i < families.size(); i++) {
            if (families[i].queueFlags & vk::QueueFlagBits::eGraphics) {
                family = i;
                break;
            }
        }
        const float priority = 1.0f;
        const vk::DeviceQueueCreateInfo queue_ci{
            .queueFamilyIndex = family,
            .queueCount = 1,
            .pQueuePriorities = &priority,
        };
        const vk::DeviceCreateInfo device_ci{
            .pNext = &feature_chain.get(),
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queue_ci,
            .enabledExtensionCount = static_cast<uint32_t>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data(),
        };
        ctx.device = ctx.physical.createDevice(device_ci);
        VULKAN_HPP_DEFAULT_DISPATCHER.init(ctx.device);
        ctx.vertex_input_dynamic = has_vertex_input_dynamic;
        ctx.dynamic_state_3 = has_dynamic_state_3;
        ctx.color_write_mask = has_dynamic_state_3;
        ctx.feedback_loop = has_feedback_loop;

        ctx.family = family;
        ctx.queue = ctx.device.getQueue(ctx.family, 0);

        const vk::CommandPoolCreateInfo pool_ci{
            .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            .queueFamilyIndex = ctx.family,
        };
        ctx.pool = ctx.device.createCommandPool(pool_ci);

        // The buffers (the vertex, the index and the one the descriptors point at).
        {
            CreateBuffer(ctx, vk::BufferUsageFlagBits::eVertexBuffer,
                         vk::MemoryPropertyFlagBits::eHostVisible, 4096, ctx.vertex_buffer,
                         ctx.vertex_memory);
        }

        // The attachments of the render pass.
        {
            const vk::ImageCreateInfo color_ci{
                .imageType = vk::ImageType::e2D,
                .format = vk::Format::eR8G8B8A8Unorm,
                .extent = {1280, 720, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
            };
            ctx.color_image = ctx.device.createImage(color_ci);
            const vk::MemoryRequirements req =
                ctx.device.getImageMemoryRequirements(ctx.color_image);
            ctx.color_memory = ctx.device.allocateMemory(vk::MemoryAllocateInfo{
                .allocationSize = req.size,
                .memoryTypeIndex = FindMemoryType(ctx, req.memoryTypeBits,
                                                 vk::MemoryPropertyFlagBits::eDeviceLocal),
            });
            ctx.device.bindImageMemory(ctx.color_image, ctx.color_memory, 0);
            ctx.color_view = ctx.device.createImageView(vk::ImageViewCreateInfo{
                .image = ctx.color_image,
                .viewType = vk::ImageViewType::e2D,
                .format = vk::Format::eR8G8B8A8Unorm,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            });
        }
        {
            const vk::ImageCreateInfo depth_ci{
                .imageType = vk::ImageType::e2D,
                .format = vk::Format::eD32Sfloat,
                .extent = {1280, 720, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment,
            };
            ctx.depth_image = ctx.device.createImage(depth_ci);
            const vk::MemoryRequirements req =
                ctx.device.getImageMemoryRequirements(ctx.depth_image);
            ctx.depth_memory = ctx.device.allocateMemory(vk::MemoryAllocateInfo{
                .allocationSize = req.size,
                .memoryTypeIndex = FindMemoryType(ctx, req.memoryTypeBits,
                                                 vk::MemoryPropertyFlagBits::eDeviceLocal),
            });
            ctx.device.bindImageMemory(ctx.depth_image, ctx.depth_memory, 0);
            ctx.depth_view = ctx.device.createImageView(vk::ImageViewCreateInfo{
                .image = ctx.depth_image,
                .viewType = vk::ImageViewType::e2D,
                .format = vk::Format::eD32Sfloat,
                .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
            });
        }

        CreateBuffer(ctx, vk::BufferUsageFlagBits::eVertexBuffer,
                     vk::MemoryPropertyFlagBits::eHostVisible, 4096, ctx.vertex_buffer,
                     ctx.vertex_memory);
        CreateBuffer(ctx, vk::BufferUsageFlagBits::eIndexBuffer,
                     vk::MemoryPropertyFlagBits::eHostVisible, 4096, ctx.index_buffer,
                     ctx.index_memory);
        CreateBuffer(ctx, vk::BufferUsageFlagBits::eStorageBuffer,
                     vk::MemoryPropertyFlagBits::eHostVisible, 4096, ctx.descriptor_buffer,
                     ctx.descriptor_memory);
        CreatePipeline(ctx);

        // The state the commands set.
        const vk::Viewport viewport{0.0f, 0.0f, 1280.0f, 720.0f, 0.0f, 1.0f};
        const vk::Rect2D scissor{{0, 0}, {1280, 720}};
        const std::array<vk::Viewport, 1> viewports{viewport};
        const std::array<vk::Rect2D, 1> scissors{scissor};
        const std::array<vk::DeviceSize, 1> offsets{0};
        const std::array<vk::DeviceSize, 1> sizes{4096};
        const std::array<vk::DeviceSize, 1> strides{12};
        const std::array<vk::VertexInputBindingDescription2EXT, 1> bindings = {{
            {.binding = 0, .stride = 12, .inputRate = vk::VertexInputRate::eVertex},
        }};
        const std::array<vk::VertexInputAttributeDescription2EXT, 1> attributes = {{
            {.location = 0, .binding = 0, .format = vk::Format::eR32G32B32Sfloat, .offset = 0},
        }};
        const std::array<vk::DescriptorBufferInfo, 1> buffer_infos = {{
            {.buffer = ctx.descriptor_buffer, .offset = 0, .range = 4096},
        }};
        std::array<vk::WriteDescriptorSet, 1> writes = {{
            {.dstSet = VK_NULL_HANDLE,
             .dstBinding = 0,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eUniformBuffer,
             .pBufferInfo = buffer_infos.data()},
        }};
        std::array<char, 128> push_data{};

        std::vector<Row> dynamic_rows;
        std::vector<Row> input_rows;
        std::vector<Row> draw_rows;
        std::vector<Row> barrier_rows;

        // The dynamic state (the port emits 5.9 commands of these per draw).
        dynamic_rows.push_back({"setViewportWithCount", MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                   c.setViewportWithCount(viewports);
                               })});
        dynamic_rows.push_back({"setScissorWithCount", MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                   c.setScissorWithCount(scissors);
                               })});
        dynamic_rows.push_back({"setDepthTestEnable", MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                   c.setDepthTestEnable(vk::True);
                               })});
        dynamic_rows.push_back({"setDepthWriteEnable", MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                   c.setDepthWriteEnable(vk::True);
                               })});
        dynamic_rows.push_back({"setDepthCompareOp", MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                   c.setDepthCompareOp(vk::CompareOp::eLess);
                               })});
        dynamic_rows.push_back({"setDepthBiasEnable", MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                   c.setDepthBiasEnable(vk::False);
                               })});
        dynamic_rows.push_back({"setDepthBias", MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                   c.setDepthBias(0.0f, 0.0f, 0.0f);
                               })});
        dynamic_rows.push_back({"setStencilTestEnable", MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                   c.setStencilTestEnable(vk::False);
                               })});
        dynamic_rows.push_back({"setStencilOp", MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                   c.setStencilOp(vk::StencilFaceFlagBits::eFrontAndBack,
                                                  vk::StencilOp::eKeep, vk::StencilOp::eKeep,
                                                  vk::StencilOp::eKeep, vk::CompareOp::eAlways);
                               })});
        dynamic_rows.push_back({"setStencilReference", MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                   c.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack, 0);
                               })});
        dynamic_rows.push_back({"setStencilWriteMask", MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                   c.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack, 0xFF);
                               })});
        dynamic_rows.push_back({"setStencilCompareMask",
                                MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                    c.setStencilCompareMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                                           0xFF);
                                })});
        dynamic_rows.push_back({"setPrimitiveRestartEnable",
                                MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                    c.setPrimitiveRestartEnable(vk::False);
                                })});
        dynamic_rows.push_back({"setRasterizerDiscardEnable",
                                MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                    c.setRasterizerDiscardEnable(vk::False);
                                })});
        dynamic_rows.push_back({"setCullMode", MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                   c.setCullMode(vk::CullModeFlagBits::eNone);
                               })});
        dynamic_rows.push_back({"setFrontFace", MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                   c.setFrontFace(vk::FrontFace::eClockwise);
                               })});
        dynamic_rows.push_back({"setBlendConstants", MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                   c.setBlendConstants(std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f});
                               })});
        dynamic_rows.push_back({"setLineWidth", MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                   c.setLineWidth(1.0f);
                               })});
        if (ctx.color_write_mask) {
            dynamic_rows.push_back({"setColorWriteMaskEXT",
                                    MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                        c.setColorWriteMaskEXT(
                                            0, vk::ColorComponentFlagBits::eR |
                                                   vk::ColorComponentFlagBits::eG |
                                                   vk::ColorComponentFlagBits::eB |
                                                   vk::ColorComponentFlagBits::eA);
                                    })});
        }
        if (ctx.feedback_loop) {
            dynamic_rows.push_back({"setAttachmentFeedbackLoopEnableEXT",
                                    MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                        c.setAttachmentFeedbackLoopEnableEXT(vk::ImageAspectFlagBits::eNone);
                                    })});
        }

        // The vertex and index input.
        if (ctx.vertex_input_dynamic) {
            input_rows.push_back({"setVertexInputEXT", MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                       c.setVertexInputEXT(bindings, attributes);
                                   })});
        }
        input_rows.push_back({"bindVertexBuffers (1)", MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                  c.bindVertexBuffers(0, 1, &ctx.vertex_buffer, offsets.data());
                              })});
        input_rows.push_back({"bindVertexBuffers2 (1)",
                              MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                  c.bindVertexBuffers2(0, 1, &ctx.vertex_buffer, offsets.data(),
                                                       sizes.data(), strides.data());
                              })});
        input_rows.push_back({"bindIndexBuffer", MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                  c.bindIndexBuffer(ctx.index_buffer, 0, vk::IndexType::eUint32);
                              })});

        // The draw itself.
        draw_rows.push_back({"bindPipeline", MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                c.bindPipeline(vk::PipelineBindPoint::eGraphics, ctx.pipeline);
                            })});
        draw_rows.push_back({"drawIndexed", MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                c.drawIndexed(3, 1, 0, 0, 0);
                            })});
        draw_rows.push_back({"pushConstants (128 B)", MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                c.pushConstants(ctx.pipeline_layout,
                                                vk::ShaderStageFlagBits::eCompute, 0,
                                                sizeof(push_data), push_data.data());
                            })});
        if (has_push_descriptor) {
            draw_rows.push_back({"pushDescriptorSetKHR (1 write)",
                                 MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                     c.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,
                                                            ctx.pipeline_layout, 0, writes);
                                 })});
            // The port pushes every descriptor of the draw: the buffers, the textures
            // and the samplers of its shaders (a scene draw has a dozen or more).
            for (uint32_t count : {4u, 8u, 16u, 32u}) {
                std::vector<vk::WriteDescriptorSet> many(count);
                std::vector<vk::DescriptorBufferInfo> infos(count);
                for (uint32_t i = 0; i < count; i++) {
                    infos[i] = {.buffer = ctx.descriptor_buffer, .offset = 0, .range = 4096};
                    many[i] = vk::WriteDescriptorSet{
                        .dstSet = VK_NULL_HANDLE,
                        .dstBinding = i,
                        .descriptorCount = 1,
                        .descriptorType = vk::DescriptorType::eUniformBuffer,
                        .pBufferInfo = &infos[i],
                    };
                }
                char name[64];
                std::snprintf(name, sizeof(name), "pushDescriptorSetKHR (%u writes)", count);
                draw_rows.push_back({name, MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                            c.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,
                                                                   ctx.pipeline_layout, 0, many);
                                        })});
            }
        }
        draw_rows.push_back({"beginRendering+endRendering",
                             MeasurePlain(ctx, [&](vk::CommandBuffer c) {
                                 const vk::RenderingAttachmentInfo color{
                                     .imageView = ctx.color_view,
                                     .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
                                     .loadOp = vk::AttachmentLoadOp::eLoad,
                                     .storeOp = vk::AttachmentStoreOp::eStore,
                                 };
                                 const vk::RenderingInfo info{
                                     .renderArea = {{0, 0}, {1280, 720}},
                                     .layerCount = 1,
                                     .colorAttachmentCount = 1,
                                     .pColorAttachments = &color,
                                 };
                                 c.beginRendering(info);
                                 c.endRendering();
                             })});

        // The barriers (the port emits 1.6 per draw).
        for (uint32_t count : {0u, 1u, 4u, 16u, 32u}) {
            std::vector<vk::ImageMemoryBarrier2> images(count);
            for (uint32_t i = 0; i < count; i++) {
                images[i] = vk::ImageMemoryBarrier2{
                    .srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                    .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
                    .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                    .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                    .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                    .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                    .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                    .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                    .image = ctx.color_image,
                    .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
                };
            }
            char name[64];
            std::snprintf(name, sizeof(name), "pipelineBarrier2 (%u image barriers)", count);
            barrier_rows.push_back({name, MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                                        const vk::DependencyInfo info{
                                            .imageMemoryBarrierCount = count,
                                            .pImageMemoryBarriers = images.data(),
                                        };
                                        c.pipelineBarrier2(info);
                                    })});
        }

        Print("The dynamic state (the port emits 5.9 commands of these per draw)",
              dynamic_rows);
        Print("The vertex and index input", input_rows);
        Print("The draw itself", draw_rows);
        Print("The barriers (the port emits 1.6 per draw)", barrier_rows);

        // The whole draw, as the port emits it: the commands of the profile's counts
        // (5.9 dynamic, 0.7 of a vertex input, 0.7 of a push constants, 0.7 of a
        // descriptor push, 1.6 barriers, 0.7 of a draw and 0.7 of a render pass per
        // draw) with the shapes a scene draw has.
        {
            std::vector<vk::DescriptorBufferInfo> infos(16);
            for (uint32_t i = 0; i < 16; i++) {
                infos[i] = {.buffer = ctx.descriptor_buffer, .offset = 0, .range = 4096};
            }
            const std::vector<vk::WriteDescriptorSet> many(1, vk::WriteDescriptorSet{
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = 0,
                .descriptorCount = 16,
                .descriptorType = vk::DescriptorType::eUniformBuffer,
                .pBufferInfo = infos.data(),
            });
            std::vector<vk::ImageMemoryBarrier2> images(8);
            for (uint32_t i = 0; i < images.size(); i++) {
                images[i] = vk::ImageMemoryBarrier2{
                    .srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                    .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
                    .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                    .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                    .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                    .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                    .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                    .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                    .image = ctx.color_image,
                    .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
                };
            }
            const double whole = MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                // The dynamic state (5.9 commands: the viewport, the scissor, the depth
                // test, the depth write, the compare op, the cull mode and the front face).
                c.setViewportWithCount(viewports);
                c.setScissorWithCount(scissors);
                c.setDepthTestEnable(vk::True);
                c.setDepthWriteEnable(vk::True);
                c.setDepthCompareOp(vk::CompareOp::eLess);
                c.setCullMode(vk::CullModeFlagBits::eNone);
                // The vertex input and the index buffer.
                c.setVertexInputEXT(bindings, attributes);
                c.bindVertexBuffers2(0, 1, &ctx.vertex_buffer, offsets.data(), sizes.data(),
                                      strides.data());
                c.bindIndexBuffer(ctx.index_buffer, 0, vk::IndexType::eUint32);
                // The push constants and the descriptors.
                c.pushConstants(ctx.pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                                sizeof(push_data), push_data.data());
                c.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, ctx.pipeline_layout, 0,
                                       many);
                // The barrier (8 image barriers).
                const vk::DependencyInfo dep{
                    .imageMemoryBarrierCount = static_cast<uint32_t>(images.size()),
                    .pImageMemoryBarriers = images.data(),
                };
                c.pipelineBarrier2(dep);
                // The draw.
                c.bindPipeline(vk::PipelineBindPoint::eGraphics, ctx.pipeline);
                c.drawIndexed(3, 1, 0, 0, 0);
            });
            std::printf("\nThe whole draw (the mix the port records)\n");
            std::printf("%-36s %9.0f ns\n", "per draw", whole);
        }

        // The same, with the values changing every time: the driver may skip a
        // command whose value is already the current one.
        {
            std::vector<vk::DescriptorBufferInfo> infos(16);
            for (uint32_t i = 0; i < 16; i++) {
                infos[i] = {.buffer = ctx.descriptor_buffer, .offset = 0, .range = 4096};
            }
            const std::vector<vk::WriteDescriptorSet> many(1, vk::WriteDescriptorSet{
                .dstSet = VK_NULL_HANDLE,
                .dstBinding = 0,
                .descriptorCount = 16,
                .descriptorType = vk::DescriptorType::eUniformBuffer,
                .pBufferInfo = infos.data(),
            });
            float counter = 0.0f;
            const double changing = MeasureInPass(ctx, [&](vk::CommandBuffer c) {
                counter += 1.0f;
                vk::Viewport viewport{counter, 0.0f, 1280.0f, 720.0f, 0.0f, 1.0f};
                const std::array<vk::Viewport, 1> one{viewport};
                c.setViewportWithCount(one);
                for (auto& info : infos) {
                    info.offset = static_cast<vk::DeviceSize>(counter);
                }
                c.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, ctx.pipeline_layout, 0,
                                       many);
                c.drawIndexed(3, 1, 0, 0, 0);
            });
            std::printf("\nThe same commands with the values changing\n");
            std::printf("%-36s %9.0f ns (3 commands)\n", "per iteration", changing);
        }

        std::puts("\nvk record cost: ok");
        return 0;
    } catch (const std::exception& e) {
        std::printf("error: %s\n", e.what());
        return 1;
    }
}

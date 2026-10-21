// SPDX-License-Identifier: GPL-2.0-or-later
// The divergent exchange, on the real GPU: does every lane of the wave64 get the value of
// the wave's first active lane (and not the value of the first active lane of its own host
// half)?
//
// The port now exchanges the ReadFirstLane of a divergent region through the LDS: each host
// half takes the lanes its own ballot of the exec mask marks, shuffles the value of the
// first of them, and the two halves read both (the even half's value when it has a
// candidate, the odd half's otherwise). The two workgroup barriers that order it sit in
// non uniform control flow.
//
// The shader below is the same exchange written in GLSL, and the probe runs it on the GPU
// and checks that the 64 lanes agree and that the value is the reference (the wave's first
// active lane, not the half's).
//
// Build (MSYS2 CLANG64):
//   clang++ -std=c++20 -O2 -I port/gpu/third_party/vulkan-headers/include \
//       port/tests/wave64_rfl_divergent_probe.cpp -lvulkan-1 -o wave64-rfl-divergent-probe
//   glslang --target-env vulkan1.1 -o wave64_rfl_divergent.spv wave64_rfl_divergent.comp
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>
#include <vulkan/vulkan.h>

namespace {
constexpr uint32_t kWaveLanes = 64;  // the guest's wave64
constexpr uint32_t kHostLanes = 32;  // the host's subgroup size (NVIDIA)

void Check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "%s failed: %d\n", what, static_cast<int>(result));
        std::exit(1);
    }
}

struct Device {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t queue_family{};
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceDriverProperties driver{};
};

Device CreateDevice() {
    Device out{};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "wave64-rfl-divergent-probe";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    Check(vkCreateInstance(&ici, nullptr, &out.instance), "vkCreateInstance");
    uint32_t count = 0;
    Check(vkEnumeratePhysicalDevices(out.instance, &count, nullptr), "vkEnumeratePhysicalDevices");
    std::vector<VkPhysicalDevice> devices(count);
    Check(vkEnumeratePhysicalDevices(out.instance, &count, devices.data()),
          "vkEnumeratePhysicalDevices");
    const char* wanted = std::getenv("WAVE64_PROBE_DEVICE");
    for (const auto& candidate : devices) {
        if (wanted && *wanted) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(candidate, &props);
            if (std::strstr(props.deviceName, wanted) == nullptr) {
                continue;
            }
        }
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
        std::vector<VkQueueFamilyProperties> props(families);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, props.data());
        for (uint32_t i = 0; i < families; ++i) {
            if (props[i].queueCount > 0 && (props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) {
                out.physical = candidate;
                out.queue_family = i;
                break;
            }
        }
        if (out.physical != VK_NULL_HANDLE) {
            break;
        }
    }
    if (out.physical == VK_NULL_HANDLE) {
        std::fprintf(stderr, "no compute queue\n");
        return out;
    }
    vkGetPhysicalDeviceProperties(out.physical, &out.properties);
    out.driver = VkPhysicalDeviceDriverProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &out.driver;
    vkGetPhysicalDeviceProperties2(out.physical, &props2);
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = out.queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    Check(vkCreateDevice(out.physical, &dci, nullptr, &out.device), "vkCreateDevice");
    vkGetDeviceQueue(out.device, out.queue_family, 0, &out.queue);
    return out;
}

std::vector<uint32_t> ReadSpirv(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(1);
    }
    const auto bytes = static_cast<size_t>(file.tellg());
    std::vector<uint32_t> code(bytes / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), bytes);
    return code;
}

/// Runs the shader and returns what every lane wrote.
std::vector<uint32_t> Run(const Device& dev, const std::vector<uint32_t>& code) {
    const VkDeviceSize size = kWaveLanes * sizeof(uint32_t);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    Check(vkCreateBuffer(dev.device, &bci, nullptr, &buffer), "vkCreateBuffer");
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(dev.device, buffer, &req);
    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(dev.physical, &mem_props);
    uint32_t memory_type = UINT32_MAX;
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        const auto flags = mem_props.memoryTypes[i].propertyFlags;
        if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 &&
            (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0) {
            memory_type = i;
            break;
        }
    }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memory_type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    Check(vkAllocateMemory(dev.device, &mai, nullptr, &memory), "vkAllocateMemory");
    Check(vkBindBufferMemory(dev.device, buffer, memory, 0), "vkBindBufferMemory");

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 1;
    dslci.pBindings = &binding;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    Check(vkCreateDescriptorSetLayout(dev.device, &dslci, nullptr, &layout),
          "vkCreateDescriptorSetLayout");
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &pool_size;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    Check(vkCreateDescriptorPool(dev.device, &dpci, nullptr, &pool), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    Check(vkAllocateDescriptorSets(dev.device, &dsai, &set), "vkAllocateDescriptorSets");
    VkDescriptorBufferInfo buffer_info{buffer, 0, size};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(dev.device, 1, &write, 0, nullptr);

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &layout;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    Check(vkCreatePipelineLayout(dev.device, &plci, nullptr, &pipeline_layout),
          "vkCreatePipelineLayout");
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = code.size() * sizeof(uint32_t);
    smci.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    Check(vkCreateShaderModule(dev.device, &smci, nullptr, &module), "vkCreateShaderModule");
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage = stage;
    cpci.layout = pipeline_layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    Check(vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline),
          "vkCreateComputePipelines");

    VkCommandPoolCreateInfo pool_ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_ci.queueFamilyIndex = dev.queue_family;
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    Check(vkCreateCommandPool(dev.device, &pool_ci, nullptr, &cmd_pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbi.commandPool = cmd_pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    Check(vkAllocateCommandBuffers(dev.device, &cbi, &cmd), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    Check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(cmd, 1, 1, 1);
    Check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    VkFenceCreateInfo fence_ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    Check(vkCreateFence(dev.device, &fence_ci, nullptr, &fence), "vkCreateFence");
    Check(vkQueueSubmit(dev.queue, 1, &submit, fence), "vkQueueSubmit");
    Check(vkWaitForFences(dev.device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");

    std::vector<uint32_t> result(kWaveLanes, 0xffffffffu);
    void* mapped = nullptr;
    Check(vkMapMemory(dev.device, memory, 0, size, 0, &mapped), "vkMapMemory");
    std::memcpy(result.data(), mapped, size);
    vkUnmapMemory(dev.device, memory);

    vkDestroyFence(dev.device, fence, nullptr);
    vkDestroyCommandPool(dev.device, cmd_pool, nullptr);
    vkDestroyPipeline(dev.device, pipeline, nullptr);
    vkDestroyShaderModule(dev.device, module, nullptr);
    vkDestroyPipelineLayout(dev.device, pipeline_layout, nullptr);
    vkDestroyDescriptorPool(dev.device, pool, nullptr);
    vkDestroyDescriptorSetLayout(dev.device, layout, nullptr);
    vkDestroyBuffer(dev.device, buffer, nullptr);
    vkFreeMemory(dev.device, memory, nullptr);
    return result;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <shader.spv>\n", argv[0]);
        return 1;
    }
    const Device dev = CreateDevice();
    if (dev.device == VK_NULL_HANDLE) {
        std::puts("SKIP: no Vulkan device");
        return 0;
    }
    std::printf("device: %s (driver %s), subgroup size %u\n", dev.properties.deviceName,
                dev.driver.driverName, kHostLanes);
    const auto values = Run(dev, ReadSpirv(argv[1]));

    // The reference: the wave's first active lane is the lane 0 (the even lanes are active),
    // and its value is 100. The per subgroup broadcast would give the upper half the value
    // of the lane 32 (132). Only the lanes with the exec mask set are in the region (the
    // ReadFirstLane is inside it), so only they have to agree.
    constexpr uint32_t kReference = 100;
    constexpr uint32_t kPerHalf = 100 + kHostLanes;
    uint32_t matching = 0;
    uint32_t per_half = 0;
    uint32_t in_region = 0;
    for (uint32_t lane = 0; lane < kWaveLanes; ++lane) {
        if (lane % 2 != 0) {
            continue; // not in the region
        }
        ++in_region;
        matching += values[lane] == kReference ? 1 : 0;
        per_half += values[lane] == kPerHalf ? 1 : 0;
    }
    std::printf("  lanes in the region: %u of %u\n", in_region, kWaveLanes);
    std::printf("  lanes with the wave's first active lane's value (%u): %u of %u\n", kReference,
                matching, in_region);
    std::printf("  lanes with their own half's first active lane's value (%u): %u of %u\n", kPerHalf,
                per_half, in_region);
    std::printf("  the first 8 values:");
    for (uint32_t lane = 0; lane < 8; ++lane) {
        std::printf(" %u", values[lane]);
    }
    std::printf("\n");
    if (matching != in_region) {
        std::printf("RESULT: the halves disagree\n");
        return 1;
    }
    std::printf("RESULT: every lane of the region reads the wave's first active lane\n");
    return 0;
}

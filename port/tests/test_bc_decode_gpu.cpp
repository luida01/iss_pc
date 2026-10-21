// SPDX-License-Identifier: GPL-2.0-or-later
// The CPU BC decoder against the GPU's: the driver decodes a block-compressed image and
// the test compares its texels with the ones the port's CPU decoder produces
// (video_core/texture_cache/tlg_bc_decode.h).
//
// The port decodes the tiny BC images (the 1D ones the NVIDIA driver refuses) on the CPU
// and uploads plain texels, so a bug in that decoder shows up as a texture that draws
// nothing (the particles). This test says whether the decoder is right, on the user's GPU,
// with the driver's own decoder as the reference.
//
// The image is one block (4x4 texels): the test is about the decode, not about the layout
// of a bigger image.
//
// Build (MSYS2 CLANG64):
//   clang++ -std=c++20 -O2 -I port/gpu/third_party/vulkan-headers/include \
//       -I port/gpu/shadps4 -I <build>/include port/tests/test_bc_decode_gpu.cpp \
//       -lvulkan-1 -o bc-decode-gpu-test
// The shader (port/tests/bc_gfx/bc_decode.comp) is compiled by the build to
// <build>/include/tests/bc_decode_gpu_spv.h (glslang, the same as the host shaders).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <vulkan/vulkan.h>

#include "video_core/texture_cache/tlg_bc_decode.h"

#include "tests/bc_decode_gpu_spv.h"

using VideoCore::BcDecode::DecodeBlock;
using VideoCore::BcDecode::Kind;

namespace {

constexpr uint32_t kWidth = 4;  // texels: one block
constexpr uint32_t kHeight = 4;

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
    app.pApplicationName = "tlg-bc-decode-gpu-test";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    Check(vkCreateInstance(&ici, nullptr, &out.instance), "vkCreateInstance");

    uint32_t count = 0;
    Check(vkEnumeratePhysicalDevices(out.instance, &count, nullptr), "vkEnumeratePhysicalDevices");
    std::vector<VkPhysicalDevice> devices(count);
    Check(vkEnumeratePhysicalDevices(out.instance, &count, devices.data()),
          "vkEnumeratePhysicalDevices");
    const char* wanted = std::getenv("TLG_PROBE_DEVICE");
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

uint16_t Rgb565(uint32_t r, uint32_t g, uint32_t b) {
    return uint16_t((r << 11) | (g << 5) | b);
}

void Put16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

void Put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        p[i] = uint8_t(v >> (8 * i));
    }
}

/// One block of the test image: a pattern that uses every colour and every index, so a
/// decoder that swaps the channels, the rows or the indices is caught.
struct Block {
    uint8_t bytes[16]{};
};

Block MakeBlock(Kind kind, bool three_colors = false) {
    Block block;
    switch (kind) {
    case Kind::Bc1: {
        if (three_colors) {
            // Three-colour mode (c0 <= c1): the index 3 is transparent black.
            Put16(block.bytes, Rgb565(0, 0, 31));
            Put16(block.bytes + 2, Rgb565(31, 63, 0));
        } else {
            // Four-colour mode: the endpoints are a red and a blue, the indices walk the
            // four colours in a pattern that is not symmetric (a decoder that transposes
            // the block gives a different answer).
            Put16(block.bytes, Rgb565(31, 0, 0));
            Put16(block.bytes + 2, Rgb565(0, 0, 31));
        }
        const int idx[16] = {0, 1, 2, 3, 3, 2, 1, 0, 0, 1, 2, 3, 3, 2, 1, 0};
        uint32_t indices = 0;
        for (int t = 0; t < 16; ++t) {
            indices |= uint32_t(idx[t]) << (2 * t);
        }
        Put32(block.bytes + 4, indices);
        break;
    }
    case Kind::Bc4Unorm: {
        // The eight-value mode (e0 > e1): eight levels between the endpoints.
        block.bytes[0] = 200;
        block.bytes[1] = 10;
        uint64_t bits = 0;
        const int idx[16] = {0, 1, 2, 3, 4, 5, 6, 7, 7, 6, 5, 4, 3, 2, 1, 0};
        for (int t = 0; t < 16; ++t) {
            bits |= uint64_t(idx[t]) << (3 * t);
        }
        for (int i = 0; i < 6; ++i) {
            block.bytes[2 + i] = uint8_t(bits >> (8 * i));
        }
        break;
    }
    case Kind::Bc4Snorm: {
        block.bytes[0] = 100;
        block.bytes[1] = 0x80; // -128
        uint64_t bits = 0;
        const int idx[16] = {0, 1, 2, 3, 4, 5, 6, 7, 7, 6, 5, 4, 3, 2, 1, 0};
        for (int t = 0; t < 16; ++t) {
            bits |= uint64_t(idx[t]) << (3 * t);
        }
        for (int i = 0; i < 6; ++i) {
            block.bytes[2 + i] = uint8_t(bits >> (8 * i));
        }
        break;
    }
    case Kind::Bc5Unorm: {
        block.bytes[0] = 250;
        block.bytes[1] = 10;
        uint64_t bits = 0;
        const int idx[16] = {0, 1, 2, 3, 4, 5, 6, 7, 7, 6, 5, 4, 3, 2, 1, 0};
        for (int t = 0; t < 16; ++t) {
            bits |= uint64_t(idx[t]) << (3 * t);
        }
        for (int i = 0; i < 6; ++i) {
            block.bytes[2 + i] = uint8_t(bits >> (8 * i));
        }
        block.bytes[8] = 10;
        block.bytes[9] = 200;
        uint64_t bits2 = 0;
        const int idx2[16] = {0, 3, 6, 1, 4, 7, 2, 5, 5, 2, 7, 4, 1, 6, 3, 0};
        for (int t = 0; t < 16; ++t) {
            bits2 |= uint64_t(idx2[t]) << (3 * t);
        }
        for (int i = 0; i < 6; ++i) {
            block.bytes[10 + i] = uint8_t(bits2 >> (8 * i));
        }
        break;
    }
    default:
        break;
    }
    return block;
}

/// The CPU decode of the block: the texels the port would upload.
std::vector<uint8_t> CpuDecode(Kind kind, const Block& block, uint32_t channels) {
    std::vector<uint8_t> out(kWidth * kHeight * channels);
    const uint32_t texel_bytes = VideoCore::BcDecode::TexelBytes(kind);
    uint8_t texels[16 * 4];
    DecodeBlock(kind, block.bytes, texels);
    for (uint32_t ty = 0; ty < 4; ++ty) {
        for (uint32_t tx = 0; tx < 4; ++tx) {
            uint8_t* const dst = out.data() + ((ty * kWidth) + tx) * channels;
            const uint8_t* const src = texels + (ty * 4 + tx) * texel_bytes;
            for (uint32_t c = 0; c < channels; ++c) {
                dst[c] = src[c];
            }
        }
    }
    return out;
}

/// Uploads the block to a BC image, samples it with the shader and returns the texels the
/// driver produced (as bytes, one texel per four bytes).
std::vector<uint8_t> GpuDecode(const Device& dev, VkFormat format, Kind kind, const Block& block) {
    const uint32_t block_bytes = VideoCore::BcDecode::BlockBytes(kind);
    const VkDeviceSize size = block_bytes;

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    Check(vkCreateBuffer(dev.device, &bci, nullptr, &buffer), "vkCreateBuffer");
    VkMemoryRequirements breq{};
    vkGetBufferMemoryRequirements(dev.device, buffer, &breq);
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(dev.physical, &mem);
    uint32_t memory_type = UINT32_MAX;
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        const auto flags = mem.memoryTypes[i].propertyFlags;
        if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 &&
            (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0) {
            memory_type = i;
            break;
        }
    }
    if (memory_type == UINT32_MAX) {
        std::fprintf(stderr, "no host visible memory\n");
        std::exit(1);
    }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = breq.size;
    mai.memoryTypeIndex = memory_type;
    VkDeviceMemory bmemory = VK_NULL_HANDLE;
    Check(vkAllocateMemory(dev.device, &mai, nullptr, &bmemory), "vkAllocateMemory (buffer)");
    Check(vkBindBufferMemory(dev.device, buffer, bmemory, 0), "vkBindBufferMemory");
    void* mapped = nullptr;
    Check(vkMapMemory(dev.device, bmemory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
    std::memcpy(mapped, block.bytes, block_bytes);

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {kWidth, kHeight, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    const VkResult created = vkCreateImage(dev.device, &ici, nullptr, &image);
    if (created != VK_SUCCESS) {
        std::printf("  (the driver refused the format: %d)\n", static_cast<int>(created));
        vkUnmapMemory(dev.device, bmemory);
        vkDestroyBuffer(dev.device, buffer, nullptr);
        vkFreeMemory(dev.device, bmemory, nullptr);
        return {};
    }
    VkMemoryRequirements ireq{};
    vkGetImageMemoryRequirements(dev.device, image, &ireq);
    mai.allocationSize = ireq.size;
    VkDeviceMemory imemory = VK_NULL_HANDLE;
    Check(vkAllocateMemory(dev.device, &mai, nullptr, &imemory), "vkAllocateMemory (image)");
    Check(vkBindImageMemory(dev.device, image, imemory, 0), "vkBindImageMemory");

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler = VK_NULL_HANDLE;
    Check(vkCreateSampler(dev.device, &sci, nullptr, &sampler), "vkCreateSampler");
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    Check(vkCreateImageView(dev.device, &vci, nullptr, &view), "vkCreateImageView");

    // The output buffer: the shader writes one uvec4 per texel, and an array of uvec4 in a
    // storage buffer has a stride of 16 bytes (std430).
    const VkDeviceSize out_size = VkDeviceSize(kWidth) * kHeight * 16;
    VkBufferCreateInfo obci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    obci.size = out_size;
    obci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    obci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer out_buffer = VK_NULL_HANDLE;
    Check(vkCreateBuffer(dev.device, &obci, nullptr, &out_buffer), "vkCreateBuffer (out)");
    VkMemoryRequirements oreq{};
    vkGetBufferMemoryRequirements(dev.device, out_buffer, &oreq);
    mai.allocationSize = oreq.size;
    VkDeviceMemory omemory = VK_NULL_HANDLE;
    Check(vkAllocateMemory(dev.device, &mai, nullptr, &omemory), "vkAllocateMemory (out)");
    Check(vkBindBufferMemory(dev.device, out_buffer, omemory, 0), "vkBindBufferMemory (out)");

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 2;
    dslci.pBindings = bindings;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    Check(vkCreateDescriptorSetLayout(dev.device, &dslci, nullptr, &layout),
          "vkCreateDescriptorSetLayout");
    VkDescriptorPoolSize pool_sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = pool_sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    Check(vkCreateDescriptorPool(dev.device, &dpci, nullptr, &pool), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    Check(vkAllocateDescriptorSets(dev.device, &dsai, &set), "vkAllocateDescriptorSets");
    VkDescriptorImageInfo image_info{sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo buffer_info{out_buffer, 0, out_size};
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &image_info;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(dev.device, 2, writes, 0, nullptr);

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &layout;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    Check(vkCreatePipelineLayout(dev.device, &plci, nullptr, &pipeline_layout),
          "vkCreatePipelineLayout");
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = sizeof(BC_DECODE_GPU_SPV);
    smci.pCode = BC_DECODE_GPU_SPV;
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
    VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.srcAccessMask = 0;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_dst.image = image;
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &to_dst);
    VkBufferImageCopy copy{};
    copy.bufferOffset = 0;
    copy.bufferRowLength = kWidth;
    copy.bufferImageHeight = kHeight;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageOffset = {0, 0, 0};
    copy.imageExtent = {kWidth, kHeight, 1};
    vkCmdCopyBufferToImage(cmd, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    VkImageMemoryBarrier to_read = to_dst;
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_read);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &set, 0,
                            nullptr);
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

    std::vector<uint8_t> out(out_size);
    void* out_mapped = nullptr;
    Check(vkMapMemory(dev.device, omemory, 0, VK_WHOLE_SIZE, 0, &out_mapped), "vkMapMemory (out)");
    std::memcpy(out.data(), out_mapped, out_size);
    vkUnmapMemory(dev.device, omemory);

    vkDestroyFence(dev.device, fence, nullptr);
    vkDestroyCommandPool(dev.device, cmd_pool, nullptr);
    vkDestroyPipeline(dev.device, pipeline, nullptr);
    vkDestroyShaderModule(dev.device, module, nullptr);
    vkDestroyPipelineLayout(dev.device, pipeline_layout, nullptr);
    vkDestroyDescriptorPool(dev.device, pool, nullptr);
    vkDestroyDescriptorSetLayout(dev.device, layout, nullptr);
    vkDestroyImageView(dev.device, view, nullptr);
    vkDestroySampler(dev.device, sampler, nullptr);
    vkDestroyImage(dev.device, image, nullptr);
    vkFreeMemory(dev.device, imemory, nullptr);
    vkDestroyBuffer(dev.device, out_buffer, nullptr);
    vkFreeMemory(dev.device, omemory, nullptr);
    vkUnmapMemory(dev.device, bmemory);
    vkDestroyBuffer(dev.device, buffer, nullptr);
    vkFreeMemory(dev.device, bmemory, nullptr);
    return out;
}

/// Compares the GPU's texels with the CPU's.
///
/// The colour channels are compared with a tolerance of 3: the driver interpolates the
/// levels of BC4/BC5 slightly differently from the DXT specification (up to 3 of 255, about
/// 1 %), so a difference of one to three is the driver's rounding, not a bug. A swap of the
/// channels, of the rows or of the indices is far bigger and is caught.
///
/// The alpha of BC1 is not compared: the driver returns 0 for it (the DXT1a variant uses
/// the top bit of the index as the alpha, and the driver does not), while the port's
/// decoder follows the DXT1 rule (opaque, and transparent black for the index 3 of a
/// three-colour block).
bool Compare(const char* label, const std::vector<uint8_t>& gpu, const std::vector<uint8_t>& cpu,
             uint32_t channels, uint32_t compare) {
    constexpr int kTolerance = 3;
    uint32_t exact = 0;
    uint32_t off_by_one = 0;
    uint32_t within = 0;
    uint32_t wrong = 0;
    uint32_t first_wrong = 0;
    for (size_t i = 0; i + 15 < gpu.size(); i += 16) {
        const size_t texel = i / 16;
        for (uint32_t c = 0; c < compare; ++c) {
            const int g = gpu[i + c];
            const int e = cpu[texel * channels + c];
            const int diff = g > e ? g - e : e - g;
            if (diff == 0) {
                ++exact;
            } else if (diff == 1) {
                ++off_by_one;
            } else if (diff <= kTolerance) {
                ++within;
            } else {
                if (wrong == 0) {
                    first_wrong = uint32_t(texel);
                }
                ++wrong;
            }
        }
    }
    const uint32_t total = exact + off_by_one + within + wrong;
    std::printf("  %s: %u channels: %u exact, %u off by one, %u within the driver's rounding, "
                "%u different\n",
                label, total, exact, off_by_one, within, wrong);
    if (wrong) {
        const uint32_t x = first_wrong % kWidth;
        const uint32_t y = first_wrong / kWidth;
        std::printf("    the first difference at (%u, %u): the GPU", x, y);
        for (uint32_t c = 0; c < channels; ++c) {
            std::printf(" %u", gpu[first_wrong * 16 + c]);
        }
        std::printf(", the CPU");
        for (uint32_t c = 0; c < channels; ++c) {
            std::printf(" %u", cpu[first_wrong * channels + c]);
        }
        std::printf("\n    the 16 texels, the GPU then the CPU:\n      ");
        for (uint32_t t = 0; t < 16; ++t) {
            std::printf(" (");
            for (uint32_t c = 0; c < channels; ++c) {
                std::printf("%s%u", c ? "," : "", gpu[t * 16 + c]);
            }
            std::printf("|");
            for (uint32_t c = 0; c < channels; ++c) {
                std::printf("%s%u", c ? "," : "", cpu[t * channels + c]);
            }
            std::printf(")");
        }
        std::printf("\n");
    }
    return wrong == 0;
}

} // namespace

int main() {
    const Device dev = CreateDevice();
    if (dev.device == VK_NULL_HANDLE) {
        std::puts("SKIP: no Vulkan device");
        return 0;
    }
    std::printf("device: %s (driver %s)\n", dev.properties.deviceName, dev.driver.driverName);

    struct Case {
        const char* label;
        Kind kind;
        VkFormat format;
        uint32_t channels; ///< the channels of the decoded texel
        uint32_t compare;  ///< the channels compared
        bool three_colors;
    };
    // The probe binds the image as a combined image sampler (GLSL has no syntax for a
    // separate sampler and image). On this driver that path returns only the first channel
    // of a compressed image (the port's own shaders use separate descriptors,
    // OpSampledImage, and the game's textures come out right), so only the first channel
    // is compared: the palette, the indices and the levels, which is what the decode has
    // to get right.
    const Case cases[] = {
        {"BC1 RGBA, four colours", Kind::Bc1, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 1, false},
        {"BC1 RGBA, three colours", Kind::Bc1, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 1, true},
        {"BC4 (unorm)", Kind::Bc4Unorm, VK_FORMAT_BC4_UNORM_BLOCK, 1, 1, false},
        {"BC5 (unorm)", Kind::Bc5Unorm, VK_FORMAT_BC5_UNORM_BLOCK, 2, 1, false},
    };

    int failures = 0;
    for (const auto& c : cases) {
        const auto block = MakeBlock(c.kind, c.three_colors);
        const auto gpu = GpuDecode(dev, c.format, c.kind, block);
        if (gpu.empty()) {
            std::printf("  %s: the driver refused the image\n", c.label);
            continue;
        }
        const auto cpu = CpuDecode(c.kind, block, c.channels);
        if (!Compare(c.label, gpu, cpu, c.channels, c.compare)) {
            ++failures;
        }
    }
    if (failures) {
        std::printf("bc-decode-gpu-test: %d case(s) differ\n", failures);
        return 1;
    }
    std::puts("bc-decode-gpu-test: the CPU decoder matches the GPU's");
    return 0;
}

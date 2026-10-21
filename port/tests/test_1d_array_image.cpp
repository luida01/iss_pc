// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: what the driver does with a 1D image that has more than one layer.
//
// Vulkan has no 1D array image: VK_IMAGE_TYPE_1D must have arrayLayers == 1
// (VUID-VkImageCreateInfo-arrayLayers-00981). A 1D *array* on the host is a 2D image of
// height 1 with N layers, seen as VK_IMAGE_VIEW_TYPE_2D_ARRAY.
//
// inFAMOUS binds a 512x1, 32-layer, 10-mip BC1 texture (the game log:
//   "tlg: BC image 512x1 Bc1RgbaSrgbBlock type 12 tile Thin1DThin layers 32 mips 10").
// The port creates it with imageType = e1D and arrayLayers = 32
// (video_core/texture_cache/image.cpp: ConvertImageType(info.type) with info.type =
// AmdGpu::ImageType::Color1D, the base type of a Color1DArray guest image).
//
// This probe answers on the user's GPU: does the creation succeed, does the data land in
// the 32 layers, and can a 1D-array view be made of it. The 2D image of height 1 (what
// Vulkan wants) is the reference.
//
// Build (MSYS2 CLANG64):
//   clang++ -std=c++20 -O2 -I port/gpu/third_party/vulkan-headers/include \
//       port/tests/test_1d_array_image.cpp -lvulkan-1 -o 1d-array-test
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <vulkan/vulkan.h>

namespace {

void Check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "%s failed: %d\n", what, static_cast<int>(result));
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
    app.pApplicationName = "tlg-1d-array-probe";
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

constexpr uint32_t kWidth = 512;
constexpr uint32_t kLayers = 32;

/// One layer of the test image: every texel of layer L is (L, L, L, 255).
std::vector<uint8_t> LayerData(uint32_t layer) {
    std::vector<uint8_t> data(kWidth * 4);
    for (uint32_t x = 0; x < kWidth; ++x) {
        data[x * 4 + 0] = uint8_t(layer);
        data[x * 4 + 1] = uint8_t(layer);
        data[x * 4 + 2] = uint8_t(layer);
        data[x * 4 + 3] = 255;
    }
    return data;
}

/// Creates an image of the given Vulkan type, uploads the 32 layers and reads them back.
/// Returns the number of layers whose data came back as it was uploaded.
uint32_t RunImage(const Device& dev, VkImageType type, const char* label) {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = type;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {kWidth, 1, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = kLayers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    const VkResult created = vkCreateImage(dev.device, &ici, nullptr, &image);
    std::printf("%s: vkCreateImage(imageType=%s, arrayLayers=%u) -> %d\n", label,
                type == VK_IMAGE_TYPE_1D ? "1D" : "2D", kLayers, static_cast<int>(created));
    if (created != VK_SUCCESS) {
        return 0;
    }

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dev.device, image, &req);
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
        std::printf("%s: no host visible memory\n", label);
        vkDestroyImage(dev.device, image, nullptr);
        return 0;
    }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memory_type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    Check(vkAllocateMemory(dev.device, &mai, nullptr, &memory), "vkAllocateMemory");
    Check(vkBindImageMemory(dev.device, image, memory, 0), "vkBindImageMemory");

    // One host-visible staging buffer with the 32 layers.
    const VkDeviceSize size = VkDeviceSize(kWidth) * 4 * kLayers;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    Check(vkCreateBuffer(dev.device, &bci, nullptr, &buffer), "vkCreateBuffer");
    VkMemoryRequirements breq{};
    vkGetBufferMemoryRequirements(dev.device, buffer, &breq);
    mai.allocationSize = breq.size;
    VkDeviceMemory bmemory = VK_NULL_HANDLE;
    Check(vkAllocateMemory(dev.device, &mai, nullptr, &bmemory), "vkAllocateMemory (buffer)");
    Check(vkBindBufferMemory(dev.device, buffer, bmemory, 0), "vkBindBufferMemory");
    void* mapped = nullptr;
    Check(vkMapMemory(dev.device, bmemory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
    uint8_t* bytes = static_cast<uint8_t*>(mapped);
    for (uint32_t layer = 0; layer < kLayers; ++layer) {
        const auto data = LayerData(layer);
        std::memcpy(bytes + VkDeviceSize(layer) * kWidth * 4, data.data(), data.size());
    }

    VkCommandPoolCreateInfo pool_ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_ci.queueFamilyIndex = dev.queue_family;
    VkCommandPool pool = VK_NULL_HANDLE;
    Check(vkCreateCommandPool(dev.device, &pool_ci, nullptr, &pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbi.commandPool = pool;
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
    to_dst.image = image;
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kLayers};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &to_dst);

    VkBufferImageCopy copy{};
    copy.bufferOffset = 0;
    copy.bufferRowLength = 0;
    copy.bufferImageHeight = 0;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, kLayers};
    copy.imageOffset = {0, 0, 0};
    copy.imageExtent = {kWidth, 1, 1};
    vkCmdCopyBufferToImage(cmd, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    VkImageMemoryBarrier to_src = to_dst;
    to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &to_src);
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);

    Check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    VkFenceCreateInfo fence_ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    Check(vkCreateFence(dev.device, &fence_ci, nullptr, &fence), "vkCreateFence");
    Check(vkQueueSubmit(dev.queue, 1, &submit, fence), "vkQueueSubmit");
    Check(vkWaitForFences(dev.device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");

    uint32_t good = 0;
    uint32_t first_bad = 0;
    bool have_bad = false;
    for (uint32_t layer = 0; layer < kLayers; ++layer) {
        const uint8_t* row = bytes + VkDeviceSize(layer) * kWidth * 4;
        const bool ok = row[0] == uint8_t(layer) && row[1] == uint8_t(layer) &&
                        row[2] == uint8_t(layer) && row[3] == 255;
        good += ok ? 1 : 0;
        if (!ok && !have_bad) {
            have_bad = true;
            first_bad = layer;
        }
    }
    std::printf("%s: %u of %u layers read back as uploaded", label, good, kLayers);
    if (have_bad) {
        std::printf(" (first mismatch: layer %u has %u %u %u %u, wanted %u %u %u 255)", first_bad,
                    bytes[VkDeviceSize(first_bad) * kWidth * 4 + 0],
                    bytes[VkDeviceSize(first_bad) * kWidth * 4 + 1],
                    bytes[VkDeviceSize(first_bad) * kWidth * 4 + 2],
                    bytes[VkDeviceSize(first_bad) * kWidth * 4 + 3], first_bad, first_bad, first_bad);
    }
    std::printf("\n");

    // The view: a 1D array view of a 1D image, a 2D array view of a 2D image.
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image;
    vci.viewType = type == VK_IMAGE_TYPE_1D ? VK_IMAGE_VIEW_TYPE_1D_ARRAY
                                             : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kLayers};
    VkImageView view = VK_NULL_HANDLE;
    const VkResult view_created = vkCreateImageView(dev.device, &vci, nullptr, &view);
    std::printf("%s: vkCreateImageView(viewType=%s) -> %d\n", label,
                type == VK_IMAGE_TYPE_1D ? "1D_ARRAY" : "2D_ARRAY",
                static_cast<int>(view_created));

    vkUnmapMemory(dev.device, bmemory);
    vkDestroyFence(dev.device, fence, nullptr);
    vkDestroyCommandPool(dev.device, pool, nullptr);
    if (view != VK_NULL_HANDLE) {
        vkDestroyImageView(dev.device, view, nullptr);
    }
    vkDestroyBuffer(dev.device, buffer, nullptr);
    vkFreeMemory(dev.device, bmemory, nullptr);
    vkDestroyImage(dev.device, image, nullptr);
    vkFreeMemory(dev.device, memory, nullptr);
    return good;
}

} // namespace

int main() {
    const Device dev = CreateDevice();
    if (dev.device == VK_NULL_HANDLE) {
        std::puts("SKIP: no Vulkan device");
        return 0;
    }
    std::printf("device: %s (driver %s)\n", dev.properties.deviceName, dev.driver.driverName);
    std::printf("Vulkan: a 1D image with %u layers is invalid (VUID-VkImageCreateInfo-arrayLayers-"
                "00981);\n"
                "        the 2D image of height 1 with %u layers is what Vulkan wants.\n\n",
                kLayers, kLayers);

    const uint32_t one_d = RunImage(dev, VK_IMAGE_TYPE_1D, "1D  (what the port creates)");
    const uint32_t two_d = RunImage(dev, VK_IMAGE_TYPE_2D, "2D  (what Vulkan wants)");

    std::printf("\n");
    if (two_d != kLayers) {
        std::printf("RESULT: the 2D reference itself is broken (%u of %u layers)\n", two_d, kLayers);
        return 1;
    }
    if (one_d != kLayers) {
        std::printf("RESULT: the 1D image with %u layers does not hold its layers (%u of %u)\n",
                    kLayers, one_d, kLayers);
        return 1;
    }
    std::printf("RESULT: the driver accepts the 1D image with %u layers and keeps the data\n",
                kLayers);
    return 0;
}

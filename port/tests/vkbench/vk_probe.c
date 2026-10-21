// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: a minimal Vulkan probe (the raw API): does the driver allocate device_memory on the
// discrete GPU? The bench of the command recording needs a working device.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

static int has_ext(VkPhysicalDevice device, const char* name) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, NULL, &count, NULL) != VK_SUCCESS) {
        return 0;
    }
    VkExtensionProperties* list = calloc(count ? count : 1, sizeof(*list));
    int found = 0;
    if (list && vkEnumerateDeviceExtensionProperties(device, NULL, &count, list) == VK_SUCCESS) {
        for (uint32_t i = 0; i < count && !found; ++i) {
            found = strcmp(list[i].extensionName, name) == 0;
        }
    }
    free(list);
    return found;
}

int main(void) {
    const VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "tlg probe",
        .apiVersion = VK_API_VERSION_1_3,
    };
    const char* instance_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME};
    const VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = instance_extensions,
    };
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, NULL, &instance) != VK_SUCCESS) {
        printf("no instance\n");
        return 1;
    }
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, NULL);
    VkPhysicalDevice* devices = calloc(count, sizeof(*devices));
    vkEnumeratePhysicalDevices(instance, &count, devices);
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[i], &props);
        printf("device: %s (%d)\n", props.deviceName, props.deviceType);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            physical = devices[i];
        }
    }
    if (!physical) {
        physical = devices[0];
    }
    printf("using: ");
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical, &props);
        printf("%s\n", props.deviceName);
    }
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_ci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const char* all_extensions[] = {"VK_KHR_push_descriptor",
                                    "VK_EXT_vertex_input_dynamic_state",
                                    "VK_EXT_extended_dynamic_state3",
                                    "VK_EXT_attachment_feedback_loop_layout",
                                    "VK_EXT_attachment_feedback_loop_dynamic_state"};
    uint32_t ext_count = 0;
    const char** extensions = all_extensions;
    const char* only = getenv("VK_PROBE_ONE_EXT");
    if (only) {
        ext_count = 1;
        for (uint32_t i = 0; i < 5; ++i) {
            if (strcmp(all_extensions[i], only) == 0) {
                extensions = &all_extensions[i];
                break;
            }
        }
    } else {
        ext_count = 1;
    }
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_ci,
        .enabledExtensionCount = ext_count,
        .ppEnabledExtensionNames = extensions,
    };
    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(physical, &dci, NULL, &device) != VK_SUCCESS) {
        printf("no device\n");
        return 1;
    }
    printf("device created\n");
    if (getenv("VK_PROBE_POOL")) {
        VkCommandPoolCreateInfo pool_ci = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = 0,
        };
        VkCommandPool pool = VK_NULL_HANDLE;
        VkResult pool_result = vkCreateCommandPool(device, &pool_ci, NULL, &pool);
        printf("command pool: %d %p\n", pool_result, (void*)pool);
    }
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(physical, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        printf("  type %u: flags %x heap %u\n", i, mem.memoryTypes[i].propertyFlags,
               mem.memoryTypes[i].heapIndex);
    }
    // A plain buffer.
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 4096,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
    };
    VkBuffer buffer = VK_NULL_HANDLE;
    vkCreateBuffer(device, &bci, NULL, &buffer);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buffer, &req);
    printf("buffer %p, %llu bytes\n", (void*)buffer, (unsigned long long)req.size);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            type = i;
            break;
        }
    }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = type,
    };
    VkDeviceMemory device_memory = VK_NULL_HANDLE;
    printf("allocating the buffer device_memory (type %u)...\n", type);
    PFN_vkAllocateMemory allocate_memory = vkAllocateMemory;
    if (getenv("VK_PROBE_DEVICE_PROC")) {
        allocate_memory = (PFN_vkAllocateMemory)vkGetDeviceProcAddr(device, "vkAllocateMemory");
        printf("vkAllocateMemory via vkGetDeviceProcAddr: %p\n", (void*)allocate_memory);
    }
    VkResult result = allocate_memory(device, &mai, NULL, &device_memory);
    printf("allocateMemory: %d, device_memory %p\n", result, (void*)device_memory);
    if (result == VK_SUCCESS) {
        vkBindBufferMemory(device, buffer, device_memory, 0);
        printf("bound\n");
    }
    // An image.
    VkImageCreateInfo ici2 = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {1280, 720, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
    };
    VkImage image = VK_NULL_HANDLE;
    vkCreateImage(device, &ici2, NULL, &image);
    printf("image %p\n", (void*)image);
    vkGetImageMemoryRequirements(device, image, &req);
    type = UINT32_MAX;
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type = i;
            break;
        }
    }
    mai.memoryTypeIndex = type;
    mai.allocationSize = req.size;
    printf("allocating the image device_memory (type %u, %llu bytes)...\n", type,
           (unsigned long long)req.size);
    result = vkAllocateMemory(device, &mai, NULL, &device_memory);
    printf("allocateMemory: %d, device_memory %p\n", result, (void*)device_memory);
    return 0;
}

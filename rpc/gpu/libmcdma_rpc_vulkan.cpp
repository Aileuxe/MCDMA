#include "mcdma_rpc_vulkan.h"
#include "gpu_internal.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <new>

struct mcdma_rpc_vulkan {
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    size_t length = 0;
    void *host = nullptr;
    bool mapped = false;
    PFN_vkGetMemoryFdKHR get_fd = nullptr;
};

static int vulkan_error(mcdma_rpc_gpu_error *error, VkResult result, const char *operation,
                        int status = MCDMA_RPC_GPU_BACKEND) {
    char message[256];
    std::snprintf(message, sizeof(message), "%s returned VkResult %d", operation, static_cast<int>(result));
    return gpu_error(error, status, static_cast<int>(result), message);
}

/* Before publication to the caller this buffer has never been submitted. */
static void discard(mcdma_rpc_vulkan *window) {
    if (window->buffer) vkDestroyBuffer(window->device, window->buffer, nullptr);
    if (window->mapped) vkUnmapMemory(window->device, window->memory);
    if (window->memory) vkFreeMemory(window->device, window->memory, nullptr);
    delete window;
}

extern "C" uint32_t mcdma_rpc_vulkan_abi(void) { return MCDMA_RPC_GPU_ABI; }

extern "C" int mcdma_rpc_vulkan_wrap(VkPhysicalDevice physical, VkDevice device,
    void *memory, size_t length, VkBufferUsageFlags usage,
    mcdma_rpc_vulkan **out, mcdma_rpc_gpu_error *error) {
    if (!out) return gpu_error(error, MCDMA_RPC_GPU_INVALID, 0, "An output handle is required");
    *out = nullptr;
    int status = gpu_span(memory, length, error);
    if (status) return status;
    constexpr VkBufferUsageFlags allowed = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!physical || !device || !(usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || (usage & ~allowed))
        return gpu_error(error, MCDMA_RPC_GPU_INVALID, 0, "Matching Vulkan devices and storage/optional transfer usage are required");
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_1 || properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
        return gpu_error(error, MCDMA_RPC_GPU_UNSUPPORTED, 0, "A hardware Vulkan 1.1 device is required");
    auto host_properties = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
        vkGetDeviceProcAddr(device, "vkGetMemoryHostPointerPropertiesEXT"));
    if (!host_properties)
        return gpu_error(error, MCDMA_RPC_GPU_UNSUPPORTED, 0, "Enable VK_EXT_external_memory_host on the logical device");
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_limits{};
    host_limits.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT;
    VkPhysicalDeviceProperties2 properties2{};
    properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2; properties2.pNext = &host_limits;
    vkGetPhysicalDeviceProperties2(physical, &properties2);
    VkDeviceSize alignment = host_limits.minImportedHostPointerAlignment;
    if (!alignment) return gpu_error(error, MCDMA_RPC_GPU_UNSUPPORTED, 0, "The device reports no host-import alignment");
    if (reinterpret_cast<uintptr_t>(memory) % alignment || length % alignment)
        return gpu_error(error, MCDMA_RPC_GPU_ALIGNMENT, 0, "The caller span must satisfy the device's host-import alignment");
    VkPhysicalDeviceExternalBufferInfo external_info{};
    external_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;
    external_info.usage = usage;
    external_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkExternalBufferProperties external{};
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
    vkGetPhysicalDeviceExternalBufferProperties(physical, &external_info, &external);
    const auto &external_memory = external.externalMemoryProperties;
    if (!(external_memory.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
        !(external_memory.compatibleHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT))
        return gpu_error(error, MCDMA_RPC_GPU_UNSUPPORTED, 0, "The requested buffer usage cannot import a host allocation");
    auto *window = new(std::nothrow) mcdma_rpc_vulkan;
    if (!window) return gpu_error(error, MCDMA_RPC_GPU_NOMEM, 0, "Cannot allocate the wrapper");
    window->device = device; window->length = length; window->host = memory;
    VkExternalMemoryBufferCreateInfo external_create{};
    external_create.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    external_create.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; create.pNext = &external_create;
    create.size = length; create.usage = usage; create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // Failed creation/allocation outputs are undefined, even when pre-zeroed.
    // Publish a handle into owned state only after the API reports success.
    VkBuffer created_buffer = VK_NULL_HANDLE;
    VkResult result = vkCreateBuffer(device, &create, nullptr, &created_buffer);
    if (result != VK_SUCCESS) { discard(window); return vulkan_error(error, result, "vkCreateBuffer"); }
    window->buffer = created_buffer;
    VkMemoryDedicatedRequirements dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
    VkMemoryRequirements2 requirements{};
    requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2; requirements.pNext = &dedicated;
    VkBufferMemoryRequirementsInfo2 buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2; buffer_info.buffer = window->buffer;
    vkGetBufferMemoryRequirements2(device, &buffer_info, &requirements);
    bool needs_dedicated = dedicated.requiresDedicatedAllocation ||
        (external_memory.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT);
    if (requirements.memoryRequirements.size > length ||
        (needs_dedicated && requirements.memoryRequirements.size != length)) {
        discard(window);
        return gpu_error(error, MCDMA_RPC_GPU_ALIGNMENT, 0, "The buffer requirements do not fit the exact caller-owned span");
    }
    VkMemoryHostPointerPropertiesEXT host{};
    host.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT;
    result = host_properties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, memory, &host);
    if (result != VK_SUCCESS) { discard(window); return vulkan_error(error, result, "vkGetMemoryHostPointerPropertiesEXT"); }
    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    uint32_t compatible = requirements.memoryRequirements.memoryTypeBits & host.memoryTypeBits;
    constexpr VkMemoryPropertyFlags coherent = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
        if ((compatible & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & coherent) == coherent) { type = i; break; }
    if (type == UINT32_MAX) {
        discard(window);
        return gpu_error(error, MCDMA_RPC_GPU_UNSUPPORTED, 0, "No compatible HOST_VISIBLE|HOST_COHERENT memory type; no fallback");
    }
    VkMemoryDedicatedAllocateInfo dedicated_allocate{};
    dedicated_allocate.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO; dedicated_allocate.buffer = window->buffer;
    VkImportMemoryHostPointerInfoEXT import{};
    import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
    import.pNext = needs_dedicated ? &dedicated_allocate : nullptr;
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT; import.pHostPointer = memory;
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; allocate.pNext = &import;
    allocate.allocationSize = length; allocate.memoryTypeIndex = type;
    VkDeviceMemory imported_memory = VK_NULL_HANDLE;
    result = vkAllocateMemory(device, &allocate, nullptr, &imported_memory);
    if (result != VK_SUCCESS) { discard(window); return vulkan_error(error, result, "vkAllocateMemory host import"); }
    window->memory = imported_memory;
    result = vkBindBufferMemory(device, window->buffer, window->memory, 0);
    if (result != VK_SUCCESS) { discard(window); return vulkan_error(error, result, "vkBindBufferMemory"); }
    *out = window;
    return gpu_error(error, MCDMA_RPC_GPU_OK, 0, "");
}

static int export_extensions(VkPhysicalDevice physical, mcdma_rpc_gpu_error *error) {
    uint32_t count = 0;
    VkResult result = vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
    if (result != VK_SUCCESS) return vulkan_error(error, result, "vkEnumerateDeviceExtensionProperties count");
    if (!count) return gpu_error(error, MCDMA_RPC_GPU_UNSUPPORTED, 0, "The device has no DMA_BUF export extensions");
    std::unique_ptr<VkExtensionProperties[]> properties(new(std::nothrow) VkExtensionProperties[count]);
    if (!properties) return gpu_error(error, MCDMA_RPC_GPU_NOMEM, 0, "Cannot query device extension properties");
    result = vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, properties.get());
    if (result != VK_SUCCESS) return vulkan_error(error, result, "vkEnumerateDeviceExtensionProperties");
    bool external_fd = false, dma_buf = false;
    for (uint32_t i = 0; i < count; ++i) {
        external_fd |= std::strcmp(properties[i].extensionName, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) == 0;
        dma_buf |= std::strcmp(properties[i].extensionName, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) == 0;
    }
    if (!external_fd || !dma_buf)
        return gpu_error(error, MCDMA_RPC_GPU_UNSUPPORTED, 0, "VK_KHR_external_memory_fd and VK_EXT_external_memory_dma_buf are required");
    return MCDMA_RPC_GPU_OK;
}

static uint32_t export_memory_type(const VkPhysicalDeviceMemoryProperties &properties, uint32_t compatible) {
    constexpr VkMemoryPropertyFlags required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    constexpr VkMemoryPropertyFlags allowed = required | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t selected = UINT32_MAX;
    int best = -1;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if (!(compatible & (1u << i)) || (flags & required) != required || (flags & ~allowed)) continue;
        int score = (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? 0 : 2) +
            (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? 1 : 0);
        if (score > best) { best = score; selected = i; }
    }
    return selected;
}

extern "C" int mcdma_rpc_vulkan_allocate_export(VkPhysicalDevice physical, VkDevice device,
    size_t length, VkBufferUsageFlags usage, mcdma_rpc_vulkan **out, mcdma_rpc_gpu_error *error) {
    if (!out) return gpu_error(error, MCDMA_RPC_GPU_INVALID, 0, "An output handle is required");
    *out = nullptr;
    constexpr VkBufferUsageFlags allowed = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!physical || !device || !length || !(usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || (usage & ~allowed))
        return gpu_error(error, MCDMA_RPC_GPU_INVALID, 0, "Matching Vulkan devices, a length, and storage/optional transfer usage are required");
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) return gpu_error(error, MCDMA_RPC_GPU_BACKEND, 0, "Cannot determine the system page size");
    if (length % static_cast<size_t>(page))
        return gpu_error(error, MCDMA_RPC_GPU_ALIGNMENT, 0, "The allocation length must be a whole number of system pages");
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_1 || properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
        return gpu_error(error, MCDMA_RPC_GPU_UNSUPPORTED, 0, "A hardware Vulkan 1.1 device is required");
    int status = export_extensions(physical, error);
    if (status) return status;
    auto get_fd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR"));
    if (!get_fd)
        return gpu_error(error, MCDMA_RPC_GPU_UNSUPPORTED, 0, "Enable VK_KHR_external_memory_fd and VK_EXT_external_memory_dma_buf on the logical device");
    VkPhysicalDeviceExternalBufferInfo external_info{};
    external_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;
    external_info.usage = usage; external_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkExternalBufferProperties external{};
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
    vkGetPhysicalDeviceExternalBufferProperties(physical, &external_info, &external);
    const auto &capabilities = external.externalMemoryProperties;
    if (!(capabilities.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) ||
        !(capabilities.compatibleHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT))
        return gpu_error(error, MCDMA_RPC_GPU_UNSUPPORTED, 0, "The requested buffer usage cannot export DMA_BUF memory");
    auto *window = new(std::nothrow) mcdma_rpc_vulkan;
    if (!window) return gpu_error(error, MCDMA_RPC_GPU_NOMEM, 0, "Cannot allocate the wrapper");
    window->device = device; window->length = length; window->get_fd = get_fd;
    VkExternalMemoryBufferCreateInfo external_create{};
    external_create.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    external_create.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkBufferCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; create.pNext = &external_create;
    create.size = length; create.usage = usage; create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkResult result = vkCreateBuffer(device, &create, nullptr, &buffer);
    if (result != VK_SUCCESS) { discard(window); return vulkan_error(error, result, "vkCreateBuffer DMA_BUF"); }
    window->buffer = buffer;
    VkMemoryDedicatedRequirements dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
    VkMemoryRequirements2 requirements{};
    requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2; requirements.pNext = &dedicated;
    VkBufferMemoryRequirementsInfo2 buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2; buffer_info.buffer = window->buffer;
    vkGetBufferMemoryRequirements2(device, &buffer_info, &requirements);
    if (requirements.memoryRequirements.size < length) {
        discard(window);
        return gpu_error(error, MCDMA_RPC_GPU_UNSUPPORTED, 0, "The device reports buffer memory smaller than its logical length");
    }
    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    uint32_t type = export_memory_type(memory_properties, requirements.memoryRequirements.memoryTypeBits);
    if (type == UINT32_MAX) {
        discard(window);
        return gpu_error(error, MCDMA_RPC_GPU_UNSUPPORTED, 0, "No standard HOST_VISIBLE|HOST_COHERENT memory type; no fallback");
    }
    bool needs_dedicated = dedicated.requiresDedicatedAllocation || dedicated.prefersDedicatedAllocation ||
        (capabilities.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT);
    VkMemoryDedicatedAllocateInfo dedicated_allocate{};
    dedicated_allocate.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO; dedicated_allocate.buffer = window->buffer;
    VkExportMemoryAllocateInfo export_allocate{};
    export_allocate.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_allocate.pNext = needs_dedicated ? &dedicated_allocate : nullptr;
    export_allocate.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; allocate.pNext = &export_allocate;
    allocate.allocationSize = requirements.memoryRequirements.size; allocate.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    result = vkAllocateMemory(device, &allocate, nullptr, &memory);
    if (result != VK_SUCCESS) { discard(window); return vulkan_error(error, result, "vkAllocateMemory DMA_BUF"); }
    window->memory = memory;
    result = vkBindBufferMemory(device, window->buffer, window->memory, 0);
    if (result != VK_SUCCESS) { discard(window); return vulkan_error(error, result, "vkBindBufferMemory DMA_BUF"); }
    void *mapped = nullptr;
    result = vkMapMemory(device, window->memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (result != VK_SUCCESS) { discard(window); return vulkan_error(error, result, "vkMapMemory DMA_BUF"); }
    window->mapped = true; window->host = mapped;
    status = gpu_span(mapped, length, error);
    if (status) { discard(window); return status; }
    *out = window;
    return gpu_error(error, MCDMA_RPC_GPU_OK, 0, "");
}

extern "C" VkBuffer mcdma_rpc_vulkan_buffer(const mcdma_rpc_vulkan *window) { return window ? window->buffer : VK_NULL_HANDLE; }
extern "C" size_t mcdma_rpc_vulkan_size(const mcdma_rpc_vulkan *window) { return window ? window->length : 0; }
extern "C" void *mcdma_rpc_vulkan_host_pointer(const mcdma_rpc_vulkan *window) { return window ? window->host : nullptr; }

extern "C" int mcdma_rpc_vulkan_export_fd(const mcdma_rpc_vulkan *window, int *out_fd, mcdma_rpc_gpu_error *error) {
    if (!out_fd) return gpu_error(error, MCDMA_RPC_GPU_INVALID, 0, "An output fd is required");
    *out_fd = -1;
    if (!window || !window->get_fd || !window->mapped)
        return gpu_error(error, MCDMA_RPC_GPU_INVALID, 0, "An allocate_export handle is required");
    VkMemoryGetFdInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    info.memory = window->memory; info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    int fd = -1;
    VkResult result = window->get_fd(window->device, &info, &fd);
    if (result != VK_SUCCESS) return vulkan_error(error, result, "vkGetMemoryFdKHR DMA_BUF");
    if (fd < 0) return gpu_error(error, MCDMA_RPC_GPU_BACKEND, 0, "vkGetMemoryFdKHR returned no valid fd");
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        int saved = errno;
        close(fd);
        char message[160];
        std::snprintf(message, sizeof(message), "Cannot set close-on-exec on the exported fd: errno %d", saved);
        return gpu_error(error, MCDMA_RPC_GPU_BACKEND, 0, message);
    }
    *out_fd = fd;
    return gpu_error(error, MCDMA_RPC_GPU_OK, 0, "");
}

extern "C" int mcdma_rpc_vulkan_release(mcdma_rpc_vulkan **handle, mcdma_rpc_gpu_error *error) {
    if (!handle) return gpu_error(error, MCDMA_RPC_GPU_INVALID, 0, "A handle address is required");
    if (!*handle) return gpu_error(error, MCDMA_RPC_GPU_OK, 0, "");
    mcdma_rpc_vulkan *window = *handle;
    VkResult result = vkDeviceWaitIdle(window->device);
    if (result != VK_SUCCESS) return vulkan_error(error, result, "vkDeviceWaitIdle; retain the allocation and any caller mapping", MCDMA_RPC_GPU_CLEANUP);
    discard(window); *handle = nullptr;
    return gpu_error(error, MCDMA_RPC_GPU_OK, 0, "");
}

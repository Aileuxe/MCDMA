/* Real Vulkan headers, API stubs: no loader, driver or GPU is used. */
#include "../mcdma_rpc_vulkan.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <unistd.h>

static VkPhysicalDevice physical = (VkPhysicalDevice)(uintptr_t)1;
static VkDevice device = (VkDevice)(uintptr_t)2;
static void *expected;
static size_t expected_size;
static VkDeviceSize alignment, requirement_size;
static bool hardware = true, extension = true, importable = true, coherent = true, dedicated;
static bool exporting, fd_extension = true, dma_buf_extension = true, fd_function = true, exportable = true,
    compatible_export = true, dedicated_external, preferred_dedicated;
static uint32_t api_version = VK_API_VERSION_1_1, type_bits = 1, selected_type;
static VkMemoryPropertyFlags type_flags[8];
static uint32_t type_count = 1;
static unsigned buffers, allocations, waits, destroyed_buffers, freed_allocations, mapped_count, map_calls, unmaps,
    allocate_calls, export_calls;
static int poison_fd = -1;
static void *map_pointer;
static VkResult create_result = VK_SUCCESS, allocate_result = VK_SUCCESS,
    bind_result = VK_SUCCESS, wait_result = VK_SUCCESS, map_result = VK_SUCCESS, fd_result = VK_SUCCESS,
    enumerate_result = VK_SUCCESS;
static VkBuffer valid_buffer = (VkBuffer)(uintptr_t)3;
static VkDeviceMemory valid_memory = (VkDeviceMemory)(uintptr_t)4;

static VkResult VKAPI_CALL host_properties(VkDevice dev, VkExternalMemoryHandleTypeFlagBits type,
                                           const void *pointer, VkMemoryHostPointerPropertiesEXT *out) {
    assert(dev == device && type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT && pointer == expected);
    out->memoryTypeBits = 1; return VK_SUCCESS;
}
static VkResult VKAPI_CALL get_fd(VkDevice dev, const VkMemoryGetFdInfoKHR *info, int *out) {
    assert(dev == device && info->memory == valid_memory && allocations == 1 && mapped_count == 1);
    assert(info->sType == VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR &&
           info->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
    ++export_calls;
    if (fd_result != VK_SUCCESS) { *out = poison_fd; return fd_result; }
    *out = dup(poison_fd); assert(*out >= 0); return VK_SUCCESS;
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice dev, VkPhysicalDeviceProperties *out) {
    assert(dev == physical); *out = {}; out->apiVersion = api_version;
    out->deviceType = hardware ? VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU : VK_PHYSICAL_DEVICE_TYPE_CPU;
}
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice dev, const char *name) {
    assert(dev == device);
    if (!std::strcmp(name, "vkGetMemoryHostPointerPropertiesEXT"))
        return extension ? reinterpret_cast<PFN_vkVoidFunction>(host_properties) : nullptr;
    assert(!std::strcmp(name, "vkGetMemoryFdKHR"));
    return fd_function ? reinterpret_cast<PFN_vkVoidFunction>(get_fd) : nullptr;
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice dev,
    const char *layer, uint32_t *count, VkExtensionProperties *out) {
    assert(dev == physical && !layer);
    if (enumerate_result != VK_SUCCESS) return enumerate_result;
    uint32_t available = static_cast<uint32_t>(fd_extension) + static_cast<uint32_t>(dma_buf_extension);
    if (!out) { *count = available; return VK_SUCCESS; }
    assert(*count >= available); *count = available;
    if (fd_extension) std::strcpy((out++)->extensionName, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    if (dma_buf_extension) std::strcpy(out->extensionName, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    return VK_SUCCESS;
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice dev, VkPhysicalDeviceProperties2 *out) {
    assert(dev == physical);
    static_cast<VkPhysicalDeviceExternalMemoryHostPropertiesEXT *>(out->pNext)->minImportedHostPointerAlignment = alignment;
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice dev,
    const VkPhysicalDeviceExternalBufferInfo *info, VkExternalBufferProperties *out) {
    assert(dev == physical);
    if (exporting) {
        assert(info->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
        out->externalMemoryProperties.externalMemoryFeatures = exportable ? VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT : 0;
        if (dedicated_external) out->externalMemoryProperties.externalMemoryFeatures |= VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT;
        out->externalMemoryProperties.compatibleHandleTypes = compatible_export ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT : 0;
        return;
    }
    assert(info->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT);
    out->externalMemoryProperties.externalMemoryFeatures = importable ? VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT : 0;
    if (dedicated) out->externalMemoryProperties.externalMemoryFeatures |= VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT;
    out->externalMemoryProperties.compatibleHandleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice dev, const VkBufferCreateInfo *info,
    const VkAllocationCallbacks *, VkBuffer *out) {
    assert(dev == device && info->size == expected_size && !buffers);
    auto *external = static_cast<const VkExternalMemoryBufferCreateInfo *>(info->pNext);
    assert(external && external->handleTypes == (exporting ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
                                                          : VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT));
    if (create_result != VK_SUCCESS) {
        *out = (VkBuffer)(uintptr_t)0xbad;
        return create_result;
    }
    *out = valid_buffer; ++buffers; return VK_SUCCESS;
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice dev, VkBuffer buffer, const VkAllocationCallbacks *) {
    assert(dev == device && buffer == valid_buffer && buffers == 1);
    --buffers; ++destroyed_buffers;
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements2(VkDevice dev,
    const VkBufferMemoryRequirementsInfo2 *, VkMemoryRequirements2 *out) {
    assert(dev == device); out->memoryRequirements.size = requirement_size;
    out->memoryRequirements.alignment = alignment; out->memoryRequirements.memoryTypeBits = type_bits;
    static_cast<VkMemoryDedicatedRequirements *>(out->pNext)->requiresDedicatedAllocation = dedicated;
    static_cast<VkMemoryDedicatedRequirements *>(out->pNext)->prefersDedicatedAllocation = preferred_dedicated;
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice dev, VkPhysicalDeviceMemoryProperties *out) {
    assert(dev == physical); *out = {};
    if (exporting) {
        out->memoryTypeCount = type_count;
        for (uint32_t i = 0; i < type_count; ++i) out->memoryTypes[i].propertyFlags = type_flags[i];
        return;
    }
    out->memoryTypeCount = 1;
    out->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | (coherent ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0);
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice dev, const VkMemoryAllocateInfo *info,
    const VkAllocationCallbacks *, VkDeviceMemory *out) {
    assert(dev == device && !allocations); ++allocate_calls;
    if (exporting) {
        assert(info->allocationSize == requirement_size && info->memoryTypeIndex == selected_type);
        auto *export_info = static_cast<const VkExportMemoryAllocateInfo *>(info->pNext);
        assert(export_info && export_info->sType == VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO &&
               export_info->handleTypes == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
        assert(!!export_info->pNext == (dedicated || dedicated_external || preferred_dedicated));
        if (export_info->pNext) {
            auto *dedicated_info = static_cast<const VkMemoryDedicatedAllocateInfo *>(export_info->pNext);
            assert(dedicated_info->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO && dedicated_info->buffer == valid_buffer);
        }
    } else {
        assert(info->allocationSize == expected_size && info->memoryTypeIndex == 0);
        auto *import = static_cast<const VkImportMemoryHostPointerInfoEXT *>(info->pNext);
        assert(import && import->pHostPointer == expected && import->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT);
        assert(!!import->pNext == dedicated);
    }
    if (allocate_result != VK_SUCCESS) {
        *out = (VkDeviceMemory)(uintptr_t)0xbad;
        return allocate_result;
    }
    *out = valid_memory; ++allocations; return VK_SUCCESS;
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice dev, VkDeviceMemory memory, const VkAllocationCallbacks *) {
    assert(dev == device && memory == valid_memory && allocations == 1 && !buffers && !mapped_count);
    --allocations; ++freed_allocations;
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice dev, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
    assert(dev == device && buffer == valid_buffer && memory == valid_memory && !offset); return bind_result;
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice dev) { assert(dev == device); ++waits; return wait_result; }
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice dev, VkDeviceMemory memory,
    VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void **out) {
    assert(dev == device && memory == valid_memory && allocations == 1 && buffers == 1 && !mapped_count);
    assert(!offset && size == VK_WHOLE_SIZE && !flags); ++map_calls;
    if (map_result != VK_SUCCESS) { *out = reinterpret_cast<void *>(uintptr_t(0xbad)); return map_result; }
    *out = map_pointer; ++mapped_count; return VK_SUCCESS;
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice dev, VkDeviceMemory memory) {
    assert(dev == device && memory == valid_memory && allocations == 1 && mapped_count == 1 && !buffers);
    --mapped_count; ++unmaps;
}

int main() {
    size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    alignment = expected_size = requirement_size = page;
    assert(!posix_memalign(&expected, page * 2, expected_size)); std::memset(expected, 0x91, expected_size);
    mcdma_rpc_vulkan *window = nullptr;
    mcdma_rpc_gpu_error error{};
    auto wrap = [&] { return mcdma_rpc_vulkan_wrap(physical, device, expected, expected_size,
                                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &window, &error); };
    assert(mcdma_rpc_vulkan_abi() == MCDMA_RPC_GPU_ABI);
    hardware = false; assert(wrap() == MCDMA_RPC_GPU_UNSUPPORTED); hardware = true;
    extension = false; assert(wrap() == MCDMA_RPC_GPU_UNSUPPORTED); extension = true;
    importable = false; assert(wrap() == MCDMA_RPC_GPU_UNSUPPORTED); importable = true;
    alignment = page * 2; assert(wrap() == MCDMA_RPC_GPU_ALIGNMENT); alignment = page;
    create_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    assert(wrap() == MCDMA_RPC_GPU_BACKEND);
    assert(!window && !buffers && !allocations && !destroyed_buffers && !freed_allocations);
    create_result = VK_SUCCESS;
    requirement_size += page; assert(wrap() == MCDMA_RPC_GPU_ALIGNMENT); requirement_size = page;
    coherent = false; assert(wrap() == MCDMA_RPC_GPU_UNSUPPORTED); coherent = true;
    unsigned prior_destroyed = destroyed_buffers, prior_freed = freed_allocations;
    allocate_result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
    assert(wrap() == MCDMA_RPC_GPU_BACKEND);
    assert(!window && !buffers && !allocations && destroyed_buffers == prior_destroyed + 1 && freed_allocations == prior_freed);
    allocate_result = VK_SUCCESS;
    prior_destroyed = destroyed_buffers;
    bind_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    assert(wrap() == MCDMA_RPC_GPU_BACKEND);
    assert(!window && !buffers && !allocations && destroyed_buffers == prior_destroyed + 1 && freed_allocations == prior_freed + 1);
    bind_result = VK_SUCCESS;
    assert(!window && !buffers && !allocations && !waits);
    assert(!wrap()); assert(window && buffers == 1 && allocations == 1);
    assert(mcdma_rpc_vulkan_buffer(window) && mcdma_rpc_vulkan_size(window) == expected_size &&
           mcdma_rpc_vulkan_host_pointer(window) == expected);
    int fd = 999;
    assert(mcdma_rpc_vulkan_export_fd(window, &fd, &error) == MCDMA_RPC_GPU_INVALID && fd == -1);
    wait_result = VK_ERROR_DEVICE_LOST;
    assert(mcdma_rpc_vulkan_release(&window, &error) == MCDMA_RPC_GPU_CLEANUP);
    assert(window && buffers == 1 && allocations == 1); wait_result = VK_SUCCESS;
    assert(!mcdma_rpc_vulkan_release(&window, &error)); assert(!window && !buffers && !allocations);
    assert(!mcdma_rpc_vulkan_release(&window, nullptr));
    dedicated = true; assert(!wrap()); assert(!mcdma_rpc_vulkan_release(&window, &error));
    for (size_t i = 0; i < expected_size; ++i) assert(static_cast<unsigned char *>(expected)[i] == 0x91);
    dedicated = false;
    exporting = true; map_pointer = expected;
    constexpr auto host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    type_flags[0] = host;
    auto allocate_export = [&] { return mcdma_rpc_vulkan_allocate_export(physical, device, expected_size,
                                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &window, &error); };
    assert(mcdma_rpc_vulkan_allocate_export(physical, device, expected_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, nullptr, &error) == MCDMA_RPC_GPU_INVALID);
    window = reinterpret_cast<mcdma_rpc_vulkan *>(uintptr_t(0xbad));
    assert(mcdma_rpc_vulkan_allocate_export(physical, device, 0,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &window, &error) == MCDMA_RPC_GPU_INVALID && !window);
    assert(mcdma_rpc_vulkan_allocate_export(physical, device, expected_size - 1,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &window, &error) == MCDMA_RPC_GPU_ALIGNMENT && !window);
    assert(mcdma_rpc_vulkan_allocate_export(physical, device, expected_size,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &window, &error) == MCDMA_RPC_GPU_INVALID && !window);
    hardware = false; assert(allocate_export() == MCDMA_RPC_GPU_UNSUPPORTED); hardware = true;
    api_version = VK_API_VERSION_1_0; assert(allocate_export() == MCDMA_RPC_GPU_UNSUPPORTED); api_version = VK_API_VERSION_1_1;
    fd_extension = false; assert(allocate_export() == MCDMA_RPC_GPU_UNSUPPORTED); fd_extension = true;
    dma_buf_extension = false; assert(allocate_export() == MCDMA_RPC_GPU_UNSUPPORTED); dma_buf_extension = true;
    fd_function = false; assert(allocate_export() == MCDMA_RPC_GPU_UNSUPPORTED); fd_function = true;
    enumerate_result = VK_ERROR_OUT_OF_HOST_MEMORY; assert(allocate_export() == MCDMA_RPC_GPU_BACKEND); enumerate_result = VK_SUCCESS;
    exportable = false; assert(allocate_export() == MCDMA_RPC_GPU_UNSUPPORTED); exportable = true;
    compatible_export = false; assert(allocate_export() == MCDMA_RPC_GPU_UNSUPPORTED); compatible_export = true;
    assert(!window && !buffers && !allocations && !mapped_count);
    unsigned before_buffers = destroyed_buffers, before_memory = freed_allocations;
    create_result = VK_ERROR_OUT_OF_HOST_MEMORY; assert(allocate_export() == MCDMA_RPC_GPU_BACKEND); create_result = VK_SUCCESS;
    assert(!window && destroyed_buffers == before_buffers && freed_allocations == before_memory);
    requirement_size = page - 1; assert(allocate_export() == MCDMA_RPC_GPU_UNSUPPORTED); requirement_size = page;
    // A valid standard type is selected once; no AMD/protected/lazy feature is assumed enabled.
    for (auto excluded : {VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD, VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD,
                         VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, VK_MEMORY_PROPERTY_PROTECTED_BIT}) {
        type_flags[0] = host | excluded;
        assert(allocate_export() == MCDMA_RPC_GPU_UNSUPPORTED && !window);
    }
    type_flags[0] = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    assert(allocate_export() == MCDMA_RPC_GPU_UNSUPPORTED);
    type_count = 4; type_bits = 15;
    type_flags[0] = host | VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD;
    type_flags[1] = host | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    type_flags[2] = host | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    type_flags[3] = host | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    selected_type = 3;
    unsigned before_calls = allocate_calls;
    allocate_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    assert(allocate_export() == MCDMA_RPC_GPU_BACKEND && allocate_calls == before_calls + 1);
    assert(!window && !buffers && !allocations && !mapped_count); allocate_result = VK_SUCCESS;
    before_memory = freed_allocations;
    bind_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    assert(allocate_export() == MCDMA_RPC_GPU_BACKEND && freed_allocations == before_memory + 1);
    bind_result = VK_SUCCESS;
    before_memory = freed_allocations;
    unsigned before_unmaps = unmaps;
    map_result = VK_ERROR_MEMORY_MAP_FAILED;
    assert(allocate_export() == MCDMA_RPC_GPU_BACKEND && freed_allocations == before_memory + 1 && unmaps == before_unmaps);
    map_result = VK_SUCCESS;
    map_pointer = static_cast<char *>(expected) + 1;
    assert(allocate_export() == MCDMA_RPC_GPU_ALIGNMENT && unmaps == before_unmaps + 1 && !window);
    map_pointer = expected;
    requirement_size = page * 2; // Allocate driver padding while retaining the logical public length.
    assert(!allocate_export() && mcdma_rpc_vulkan_size(window) == page && mcdma_rpc_vulkan_host_pointer(window) == expected);
    int pipe_fds[2]; assert(!pipe(pipe_fds)); poison_fd = pipe_fds[0]; close(pipe_fds[1]);
    fd = 999; fd_result = VK_ERROR_TOO_MANY_OBJECTS;
    assert(mcdma_rpc_vulkan_export_fd(window, &fd, &error) == MCDMA_RPC_GPU_BACKEND && fd == -1);
    assert(fcntl(poison_fd, F_GETFD) >= 0); // Failed Vulkan output is undefined and must not be closed.
    fd_result = VK_SUCCESS;
    int fd2 = -1;
    assert(!mcdma_rpc_vulkan_export_fd(window, &fd, &error));
    assert(!mcdma_rpc_vulkan_export_fd(window, &fd2, &error));
    assert(fd >= 0 && fd2 >= 0 && fd != fd2 && (fcntl(fd, F_GETFD) & FD_CLOEXEC));
    before_unmaps = unmaps; before_memory = freed_allocations;
    wait_result = VK_ERROR_DEVICE_LOST;
    assert(mcdma_rpc_vulkan_release(&window, &error) == MCDMA_RPC_GPU_CLEANUP && window);
    assert(mapped_count == 1 && unmaps == before_unmaps && freed_allocations == before_memory);
    wait_result = VK_SUCCESS;
    assert(!mcdma_rpc_vulkan_release(&window, &error) && !window && !mapped_count && unmaps == before_unmaps + 1);
    assert(fcntl(fd, F_GETFD) >= 0 && fcntl(fd2, F_GETFD) >= 0); // Release never closes caller-owned exports.
    close(fd); close(fd2); close(poison_fd); poison_fd = -1;
    requirement_size = page;
    dedicated = true; assert(!allocate_export()); assert(!mcdma_rpc_vulkan_release(&window, &error)); dedicated = false;
    dedicated_external = true; assert(!allocate_export()); assert(!mcdma_rpc_vulkan_release(&window, &error)); dedicated_external = false;
    preferred_dedicated = true; assert(!allocate_export()); assert(!mcdma_rpc_vulkan_release(&window, &error)); preferred_dedicated = false;
    // Compatibility bits override preference, and ordinary non-cached memory remains supported.
    type_bits = 4; selected_type = 2;
    assert(!allocate_export()); assert(!mcdma_rpc_vulkan_release(&window, &error));
    type_flags[3] = host; type_bits = 12; selected_type = 3;
    assert(!allocate_export()); assert(!mcdma_rpc_vulkan_release(&window, &error));
    assert(mcdma_rpc_vulkan_export_fd(nullptr, &fd, &error) == MCDMA_RPC_GPU_INVALID && fd == -1);
    assert(mcdma_rpc_vulkan_export_fd(nullptr, nullptr, &error) == MCDMA_RPC_GPU_INVALID);
    assert(!mcdma_rpc_vulkan_host_pointer(nullptr) && !mcdma_rpc_vulkan_size(nullptr));
    assert(!buffers && !allocations && !mapped_count && map_calls && export_calls);
    for (size_t i = 0; i < expected_size; ++i) assert(static_cast<unsigned char *>(expected)[i] == 0x91);
    std::free(expected);
    std::puts("Vulkan import/export/ownership/failure checks passed using API stubs");
}

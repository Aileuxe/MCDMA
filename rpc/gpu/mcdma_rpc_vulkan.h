#ifndef MCDMA_RPC_VULKAN_H
#define MCDMA_RPC_VULKAN_H
#include "mcdma_rpc_gpu.h"
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif

struct mcdma_rpc_vulkan;
MCDMA_RPC_GPU_API uint32_t mcdma_rpc_vulkan_abi(void);

/* Import a caller-owned host span into a storage buffer without copying it.
 * Use a matching physical/device pair, Vulkan >=1.1, and a logical device with
 * VK_EXT_external_memory_host enabled. CPU/software devices are refused.
 * The span must be mapped, writable, system-page aligned, aligned/sized for
 * minImportedHostPointerAlignment, and large enough for the buffer requirements.
 * usage must include STORAGE_BUFFER and may include TRANSFER_SRC/TRANSFER_DST.
 * No fallback to noncoherent memory, staging, or another GPU is performed.
 * On any failure *out=NULL; no caller memory is freed or unmapped. */
MCDMA_RPC_GPU_API int mcdma_rpc_vulkan_wrap(VkPhysicalDevice physical, VkDevice device,
    void *memory, size_t length, VkBufferUsageFlags usage,
    struct mcdma_rpc_vulkan **out, struct mcdma_rpc_gpu_error *error);

/* Allocate one Vulkan storage buffer and its own coherent host-visible memory,
 * mapped into this process and exportable as DMA_BUF. This is an explicit
 * allocation mode, never a fallback from wrap(). length must be nonzero and
 * a whole number of system pages. The allocation may include driver padding;
 * size() returns the caller's logical length, and host_pointer() is page aligned.
 * The caller must enable VK_KHR_external_memory_fd and
 * VK_EXT_external_memory_dma_buf on the matching Vulkan >=1.1 logical device.
 * Physical support and vkGetMemoryFdKHR availability are checked; Vulkan cannot
 * retrospectively query whether a no-entrypoint extension was enabled.
 * Memory types with nonstandard, protected, lazy or AMD-specific property bits
 * are excluded. One type is selected, preferring non-DEVICE_LOCAL and then
 * HOST_CACHED; allocation failure is returned without retry or payload copying.
 * On every failure *out=NULL and any valid resources are released. */
MCDMA_RPC_GPU_API int mcdma_rpc_vulkan_allocate_export(VkPhysicalDevice physical, VkDevice device,
    size_t length, VkBufferUsageFlags usage,
    struct mcdma_rpc_vulkan **out, struct mcdma_rpc_gpu_error *error);

MCDMA_RPC_GPU_API VkBuffer mcdma_rpc_vulkan_buffer(const struct mcdma_rpc_vulkan *window);
MCDMA_RPC_GPU_API size_t mcdma_rpc_vulkan_size(const struct mcdma_rpc_vulkan *window);
/* Same allocation as buffer(); the imported caller pointer or owned CPU mapping.
 * The pointer becomes invalid when the owned allocation is successfully released. */
MCDMA_RPC_GPU_API void *mcdma_rpc_vulkan_host_pointer(const struct mcdma_rpc_vulkan *window);

/* Only for allocate_export() handles. Each success creates a fresh DMA_BUF fd
 * owned by the caller, with close-on-exec set. Failure always sets *out_fd=-1.
 * The library never closes a successfully returned fd, including on release.
 * Complete GPU work before NIC access and retain the allocation through all
 * NIC accesses; exporting a descriptor provides no synchronization or MR. */
MCDMA_RPC_GPU_API int mcdma_rpc_vulkan_export_fd(const struct mcdma_rpc_vulkan *window,
    int *out_fd, struct mcdma_rpc_gpu_error *error);

/* Stop new GPU submissions and complete all daemon/NIC accesses first.
 * Waits for submitted device work, destroys the buffer, unmaps any owned CPU
 * mapping, frees the memory object, then sets *window=NULL. An imported original
 * host mapping remains caller-owned; exported fds remain caller-owned.
 * A wait failure preserves the handle for retry. Device/physical/instance must
 * outlive it, and calls concerning this handle must be serialized. */
MCDMA_RPC_GPU_API int mcdma_rpc_vulkan_release(struct mcdma_rpc_vulkan **window,
    struct mcdma_rpc_gpu_error *error);

#ifdef __cplusplus
}
#endif
#endif

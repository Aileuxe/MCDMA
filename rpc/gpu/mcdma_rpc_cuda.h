#ifndef MCDMA_RPC_CUDA_H
#define MCDMA_RPC_CUDA_H
#include "mcdma_rpc_gpu.h"
#ifdef __cplusplus
extern "C" {
#endif

struct mcdma_rpc_cuda;
MCDMA_RPC_GPU_API uint32_t mcdma_rpc_cuda_abi(void);

/* Import exactly [memory, memory + length), with no fallback or payload copy.
 * Both ends must be system-page aligned; the caller owns the entire mapped span.
 * device must already be the calling thread's current CUDA device, with an
 * active CUDA context made current by the application; no context is created
 * or switched here. The wrapper records that exact CUcontext.
 * The span must not already have a CUDA host registration.
 * On failure *out is NULL, except CLEANUP may return a retained handle that
 * must be passed to release; never unmap caller memory while a handle exists. */
MCDMA_RPC_GPU_API int mcdma_rpc_cuda_wrap(void *memory, size_t length, int device,
    struct mcdma_rpc_cuda **out, struct mcdma_rpc_gpu_error *error);

/* NULL/zero for a NULL handle; a retained failed-wrap handle may have no pointer.
 * Returned device pointer aliases the input span, and may differ numerically.
 * All GPU uses of this pointer must occur in the original captured context. */
MCDMA_RPC_GPU_API void *mcdma_rpc_cuda_device_pointer(const struct mcdma_rpc_cuda *window);
MCDMA_RPC_GPU_API size_t mcdma_rpc_cuda_size(const struct mcdma_rpc_cuda *window);

/* First stop new GPU submissions, then complete all daemon/NIC accesses.
 * The original CUcontext must be current, even when another context uses the
 * same device; otherwise WRONG_CONTEXT preserves the handle without waiting or
 * unregistering. This waits for submitted work in that context, unregisters the
 * host span, and sets *window=NULL. It never frees/unmaps the caller's memory.
 * A failure preserves *window and its registration for retry. Calls on a single
 * handle must be serialized; device/context must outlive the handle. */
MCDMA_RPC_GPU_API int mcdma_rpc_cuda_release(struct mcdma_rpc_cuda **window,
    struct mcdma_rpc_gpu_error *error);

#ifdef __cplusplus
}
#endif
#endif

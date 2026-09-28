/* Optional GPU imports for caller-owned mcdma-rpcd mailbox memory, ABI 1.
 * These libraries never allocate a replacement payload or copy its bytes. */
#ifndef MCDMA_RPC_GPU_H
#define MCDMA_RPC_GPU_H
#include <stddef.h>
#include <stdint.h>

#define MCDMA_RPC_GPU_ABI 1u
#if defined(__GNUC__)
#define MCDMA_RPC_GPU_API __attribute__((visibility("default")))
#else
#define MCDMA_RPC_GPU_API
#endif

enum mcdma_rpc_gpu_status {
    MCDMA_RPC_GPU_OK = 0,
    MCDMA_RPC_GPU_INVALID = 1,
    MCDMA_RPC_GPU_ALIGNMENT = 2,
    MCDMA_RPC_GPU_OVERFLOW = 3,
    MCDMA_RPC_GPU_UNSUPPORTED = 4,
    MCDMA_RPC_GPU_NOMEM = 5,
    MCDMA_RPC_GPU_WRONG_DEVICE = 6,
    MCDMA_RPC_GPU_BACKEND = 7,
    MCDMA_RPC_GPU_CLEANUP = 8,
    MCDMA_RPC_GPU_WRONG_CONTEXT = 9
};

/* Optional caller-owned error output; no global/thread-local error storage.
 * backend_code is a CUDA runtime/driver error or VkResult, or zero for argument checks. */
struct mcdma_rpc_gpu_error {
    int status;
    int backend_code;
    char message[256];
};
#endif

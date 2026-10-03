#ifndef MCDMA_RPC_GPU_INTERNAL_HPP
#define MCDMA_RPC_GPU_INTERNAL_HPP
#include "mcdma_rpc_gpu.h"
#include <cstdio>
#include <cstdint>
#include <unistd.h>

static inline int gpu_error(mcdma_rpc_gpu_error *error, int status, int backend, const char *message) {
    if (error) {
        error->status = status; error->backend_code = backend;
        std::snprintf(error->message, sizeof(error->message), "%s", message ? message : "");
    }
    return status;
}

/* This validates arithmetic and alignment, not ownership or mapping permissions:
 * only the caller knows the lifetime and actual extent of its mapping. */
static inline int gpu_span(void *memory, size_t length, mcdma_rpc_gpu_error *error) {
    if (!memory || !length) return gpu_error(error, MCDMA_RPC_GPU_INVALID, 0, "A nonempty mapped span is required");
    uintptr_t address = reinterpret_cast<uintptr_t>(memory);
    if (length > UINTPTR_MAX - address) return gpu_error(error, MCDMA_RPC_GPU_OVERFLOW, 0, "The span overflows the address space");
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) return gpu_error(error, MCDMA_RPC_GPU_BACKEND, 0, "Cannot determine the system page size");
    if (address % static_cast<size_t>(page) || length % static_cast<size_t>(page))
        return gpu_error(error, MCDMA_RPC_GPU_ALIGNMENT, 0, "The address and length must be system-page aligned");
    return MCDMA_RPC_GPU_OK;
}
#endif

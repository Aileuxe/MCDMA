#include "mcdma_rpc_cuda.h"
#include "gpu_internal.hpp"
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <new>

struct mcdma_rpc_cuda {
    void *memory = nullptr;
    void *device_pointer = nullptr;
    size_t length = 0;
    int device = -1;
    CUcontext context = nullptr;
};

static int cuda_error(mcdma_rpc_gpu_error *error, cudaError_t code, int status = MCDMA_RPC_GPU_BACKEND) {
    return gpu_error(error, status, static_cast<int>(code), cudaGetErrorString(code));
}

static int current_context(CUcontext expected, CUcontext *actual, mcdma_rpc_gpu_error *error) {
    CUresult result = cuCtxGetCurrent(actual);
    if (result != CUDA_SUCCESS) {
        const char *message = nullptr;
        (void)cuGetErrorString(result, &message);
        return gpu_error(error, MCDMA_RPC_GPU_BACKEND, static_cast<int>(result),
                         message ? message : "cuCtxGetCurrent failed");
    }
    if (!*actual || (expected && *actual != expected))
        return gpu_error(error, MCDMA_RPC_GPU_WRONG_CONTEXT, 0,
                         expected ? "Make the wrapper's original CUDA context current before release"
                                  : "Make a CUDA context current before wrapping memory");
    return MCDMA_RPC_GPU_OK;
}

static int current_device(int expected, mcdma_rpc_gpu_error *error) {
    int current = -1;
    cudaError_t result = cudaGetDevice(&current);
    if (result != cudaSuccess) return cuda_error(error, result);
    if (current != expected) return gpu_error(error, MCDMA_RPC_GPU_WRONG_DEVICE, 0, "Select the wrapper's CUDA device before calling this function");
    return MCDMA_RPC_GPU_OK;
}

extern "C" uint32_t mcdma_rpc_cuda_abi(void) { return MCDMA_RPC_GPU_ABI; }

extern "C" int mcdma_rpc_cuda_wrap(void *memory, size_t length, int device,
                                   mcdma_rpc_cuda **out, mcdma_rpc_gpu_error *error) {
    if (!out) return gpu_error(error, MCDMA_RPC_GPU_INVALID, 0, "An output handle is required");
    *out = nullptr;
    int status = gpu_span(memory, length, error);
    if (status) return status;
    if (device < 0) return gpu_error(error, MCDMA_RPC_GPU_INVALID, 0, "The CUDA device index must be nonnegative");
    CUcontext context = nullptr;
    if ((status = current_context(nullptr, &context, error))) return status;
    if ((status = current_device(device, error))) return status;
    int supported = 0;
    cudaError_t result = cudaDeviceGetAttribute(&supported, cudaDevAttrCanMapHostMemory, device);
    if (result != cudaSuccess) return cuda_error(error, result);
    if (!supported) return gpu_error(error, MCDMA_RPC_GPU_UNSUPPORTED, 0, "The CUDA device cannot map host memory");
    auto *window = new(std::nothrow) mcdma_rpc_cuda;
    if (!window) return gpu_error(error, MCDMA_RPC_GPU_NOMEM, 0, "Cannot allocate the wrapper");
    window->memory = memory; window->length = length; window->device = device;
    window->context = context;
    result = cudaHostRegister(memory, length, cudaHostRegisterMapped);
    if (result != cudaSuccess) { delete window; return cuda_error(error, result); }
    result = cudaHostGetDevicePointer(&window->device_pointer, memory, 0);
    if (result != cudaSuccess) {
        window->device_pointer = nullptr;
        cudaError_t cleanup = cudaHostUnregister(memory);
        if (cleanup != cudaSuccess) {
            *out = window;
            return cuda_error(error, cleanup, MCDMA_RPC_GPU_CLEANUP);
        }
        delete window; return cuda_error(error, result);
    }
    *out = window;
    return gpu_error(error, MCDMA_RPC_GPU_OK, 0, "");
}

extern "C" void *mcdma_rpc_cuda_device_pointer(const mcdma_rpc_cuda *window) { return window ? window->device_pointer : nullptr; }
extern "C" size_t mcdma_rpc_cuda_size(const mcdma_rpc_cuda *window) { return window ? window->length : 0; }

extern "C" int mcdma_rpc_cuda_release(mcdma_rpc_cuda **handle, mcdma_rpc_gpu_error *error) {
    if (!handle) return gpu_error(error, MCDMA_RPC_GPU_INVALID, 0, "A handle address is required");
    if (!*handle) return gpu_error(error, MCDMA_RPC_GPU_OK, 0, "");
    mcdma_rpc_cuda *window = *handle;
    CUcontext context = nullptr;
    int status = current_context(window->context, &context, error);
    if (status) return status;
    status = current_device(window->device, error);
    if (status) return status;
    cudaError_t result = cudaDeviceSynchronize();
    if (result != cudaSuccess) return cuda_error(error, result, MCDMA_RPC_GPU_CLEANUP);
    result = cudaHostUnregister(window->memory);
    if (result != cudaSuccess) return cuda_error(error, result, MCDMA_RPC_GPU_CLEANUP);
    delete window; *handle = nullptr;
    return gpu_error(error, MCDMA_RPC_GPU_OK, 0, "");
}

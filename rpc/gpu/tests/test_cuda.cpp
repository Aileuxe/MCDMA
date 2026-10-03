#include "../mcdma_rpc_cuda.h"
#include "cuda_runtime_api.h"
#include "cuda.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

static int current = 2, supports_mapping = 1, registrations, unregisters, waits;
static bool registered;
static CUcontext context_a = reinterpret_cast<CUcontext>(uintptr_t(1));
static CUcontext context_b = reinterpret_cast<CUcontext>(uintptr_t(2));
static CUcontext current_context = context_a;
static CUresult context_result = CUDA_SUCCESS;
static void *expected;
static size_t expected_size;
static cudaError_t register_error = cudaSuccess, pointer_error = cudaSuccess,
    unregister_error = cudaSuccess, wait_error = cudaSuccess;

extern "C" CUresult cuCtxGetCurrent(CUcontext *out) { *out = current_context; return context_result; }
extern "C" CUresult cuGetErrorString(CUresult, const char **out) { *out = "injected CUDA driver error"; return CUDA_SUCCESS; }

extern "C" cudaError_t cudaGetDevice(int *out) { *out = current; return cudaSuccess; }
extern "C" cudaError_t cudaDeviceGetAttribute(int *out, cudaDeviceAttr attribute, int device) {
    assert(attribute == cudaDevAttrCanMapHostMemory && device == current); *out = supports_mapping; return cudaSuccess;
}
extern "C" cudaError_t cudaHostRegister(void *memory, size_t length, unsigned flags) {
    assert(memory == expected && length == expected_size && flags == cudaHostRegisterMapped);
    ++registrations;
    if (register_error != cudaSuccess) return register_error;
    assert(!registered); registered = true; return cudaSuccess;
}
extern "C" cudaError_t cudaHostGetDevicePointer(void **out, void *memory, unsigned flags) {
    assert(registered && memory == expected && !flags);
    if (pointer_error != cudaSuccess) return pointer_error;
    *out = memory; return cudaSuccess;
}
extern "C" cudaError_t cudaHostUnregister(void *memory) {
    assert(memory == expected && registered); ++unregisters;
    if (unregister_error != cudaSuccess) return unregister_error;
    registered = false; return cudaSuccess;
}
extern "C" cudaError_t cudaDeviceSynchronize(void) { ++waits; return wait_error; }
extern "C" const char *cudaGetErrorString(cudaError_t) { return "injected CUDA error"; }

int main() {
    expected_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    assert(!posix_memalign(&expected, expected_size, expected_size));
    std::memset(expected, 0x73, expected_size);
    mcdma_rpc_cuda *window = nullptr;
    mcdma_rpc_gpu_error error{};
    assert(mcdma_rpc_cuda_abi() == MCDMA_RPC_GPU_ABI);
    current_context = nullptr;
    assert(mcdma_rpc_cuda_wrap(expected, expected_size, 2, &window, &error) == MCDMA_RPC_GPU_WRONG_CONTEXT);
    assert(!window && !registrations); current_context = context_a;
    context_result = CUDA_ERROR_NOT_INITIALIZED;
    assert(mcdma_rpc_cuda_wrap(expected, expected_size, 2, &window, &error) == MCDMA_RPC_GPU_BACKEND);
    assert(!window && !registrations); context_result = CUDA_SUCCESS;
    assert(mcdma_rpc_cuda_wrap(expected, expected_size, 1, &window, &error) == MCDMA_RPC_GPU_WRONG_DEVICE);
    assert(!window && !registrations);
    supports_mapping = 0;
    assert(mcdma_rpc_cuda_wrap(expected, expected_size, 2, &window, &error) == MCDMA_RPC_GPU_UNSUPPORTED);
    assert(!window && !registrations); supports_mapping = 1;
    register_error = cudaErrorInvalidValue;
    assert(mcdma_rpc_cuda_wrap(expected, expected_size, 2, &window, &error) == MCDMA_RPC_GPU_BACKEND);
    assert(!window && !registered && !unregisters); register_error = cudaSuccess;
    assert(!mcdma_rpc_cuda_wrap(expected, expected_size, 2, &window, &error));
    assert(window && registered && mcdma_rpc_cuda_device_pointer(window) == expected);
    assert(mcdma_rpc_cuda_size(window) == expected_size);
    // A device ordinal match does not prove that synchronization covers the
    // context whose GPU work owns this mapping.
    current_context = context_b;
    assert(mcdma_rpc_cuda_release(&window, &error) == MCDMA_RPC_GPU_WRONG_CONTEXT);
    assert(window && registered && !waits && !unregisters);
    current_context = nullptr;
    assert(mcdma_rpc_cuda_release(&window, &error) == MCDMA_RPC_GPU_WRONG_CONTEXT);
    assert(window && registered && !waits && !unregisters);
    current_context = context_a;
    current = 1;
    assert(mcdma_rpc_cuda_release(&window, &error) == MCDMA_RPC_GPU_WRONG_DEVICE);
    assert(window && registered && !waits); current = 2;
    wait_error = cudaErrorUnknown;
    assert(mcdma_rpc_cuda_release(&window, &error) == MCDMA_RPC_GPU_CLEANUP);
    assert(window && registered && !unregisters); wait_error = cudaSuccess;
    unregister_error = cudaErrorUnknown;
    assert(mcdma_rpc_cuda_release(&window, &error) == MCDMA_RPC_GPU_CLEANUP);
    assert(window && registered); unregister_error = cudaSuccess;
    assert(!mcdma_rpc_cuda_release(&window, &error)); assert(!window && !registered);
    assert(!mcdma_rpc_cuda_release(&window, nullptr));
    pointer_error = cudaErrorInvalidValue;
    assert(mcdma_rpc_cuda_wrap(expected, expected_size, 2, &window, &error) == MCDMA_RPC_GPU_BACKEND);
    assert(!window && !registered);
    unregister_error = cudaErrorUnknown;
    assert(mcdma_rpc_cuda_wrap(expected, expected_size, 2, &window, &error) == MCDMA_RPC_GPU_CLEANUP);
    assert(window && registered && !mcdma_rpc_cuda_device_pointer(window));
    unregister_error = cudaSuccess;
    assert(!mcdma_rpc_cuda_release(&window, &error)); assert(!window && !registered);
    for (size_t i = 0; i < expected_size; ++i) assert(static_cast<unsigned char *>(expected)[i] == 0x73);
    std::free(expected);
    std::puts("CUDA wrapper failure/ownership checks passed using API stubs");
}

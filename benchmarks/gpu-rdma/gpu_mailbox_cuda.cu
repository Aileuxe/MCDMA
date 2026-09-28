// SPDX-License-Identifier: Apache-2.0
#include "mailbox_support.hpp"
#include "mcdma_rpc_cuda.h"
#include <cuda_runtime.h>

static void cuda_ok(cudaError_t result) {
    if (result != cudaSuccess) throw std::runtime_error(cudaGetErrorString(result));
}
struct GPUBuffer {
    mcdma_rpc_cuda *wrapper = nullptr;
    unsigned char *gpu = nullptr;
    unsigned *errors = nullptr;
    unsigned bytes = 0;
};
static unsigned gpu_index;
static void gpu_init(unsigned index, const char *) {
    gpu_index = index;
    cuda_ok(cudaSetDevice(index));
    cudaDeviceProp properties{};
    cuda_ok(cudaGetDeviceProperties(&properties, index));
    mailbox_require(mcdma_rpc_cuda_abi() == MCDMA_RPC_GPU_ABI, "CUDA wrapper ABI mismatch");
    std::printf("GPU_MAILBOX_BACKEND backend=cuda gpu=%u name=%s wrapper_abi=%u\n",
                index, properties.name, mcdma_rpc_cuda_abi());
}
static void gpu_wrap(GPUBuffer &buffer, unsigned char *memory, unsigned bytes) {
    mcdma_rpc_gpu_error error{};
    int status = mcdma_rpc_cuda_wrap(memory, bytes, gpu_index, &buffer.wrapper, &error);
    if (status) throw std::runtime_error(error.message);
    buffer.gpu = static_cast<unsigned char *>(mcdma_rpc_cuda_device_pointer(buffer.wrapper));
    buffer.bytes = bytes;
    mailbox_require(buffer.gpu && mcdma_rpc_cuda_size(buffer.wrapper) == bytes, "CUDA wrapper span mismatch");
    cuda_ok(cudaMalloc(reinterpret_cast<void **>(&buffer.errors), sizeof(unsigned)));
    std::printf("GPU_MAILBOX_IMPORT backend=cuda bytes=%u same_mailbox_pages=1 payload_cpu_copy_bytes=0\n", bytes);
}
__device__ static unsigned char mailbox_value(unsigned i, unsigned length, unsigned seq, unsigned kind) {
    if (i >= length) return 0xd3;
    unsigned salt = (seq ^ (seq >> 8) ^ (seq >> 16) ^ (seq >> 24));
    return static_cast<unsigned char>(i * (kind == 1 ? 37u : 53u) + salt * (kind == 1 ? 19u : 101u) + kind * 13u);
}
__global__ static void mailbox_kernel(unsigned char *memory, unsigned bytes, unsigned length,
                                      unsigned seq, unsigned kind, bool verify, unsigned *errors) {
    unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= bytes) return;
    unsigned char value = mailbox_value(i, length, seq, kind);
    if (verify) {
        if (memory[i] != value) atomicAdd(errors, 1u);
    } else memory[i] = value;
}
static void gpu_fill(GPUBuffer &buffer, unsigned length, unsigned seq, unsigned kind) {
    mailbox_kernel<<<(buffer.bytes + 255) / 256, 256>>>(buffer.gpu, buffer.bytes, length, seq, kind, false, buffer.errors);
    cuda_ok(cudaGetLastError());
    cuda_ok(cudaDeviceSynchronize());
}
static void gpu_verify(GPUBuffer &buffer, unsigned length, unsigned seq, unsigned kind, const char *phase) {
    cuda_ok(cudaMemset(buffer.errors, 0, sizeof(unsigned)));
    mailbox_kernel<<<(buffer.bytes + 255) / 256, 256>>>(buffer.gpu, buffer.bytes, length, seq, kind, true, buffer.errors);
    cuda_ok(cudaGetLastError());
    cuda_ok(cudaDeviceSynchronize());
    unsigned errors = UINT_MAX;
    cuda_ok(cudaMemcpy(&errors, buffer.errors, sizeof(errors), cudaMemcpyDeviceToHost));
    std::printf("GPU_MAILBOX_VERIFY phase=%s seq=%u payload_bytes=%u checked_bytes=%u errors=%u\n",
                phase, seq, length, buffer.bytes, errors);
    mailbox_require(errors == 0, "GPU mailbox verification failed");
}
static void gpu_release(GPUBuffer &buffer) {
    if (buffer.errors) cuda_ok(cudaFree(buffer.errors));
    mcdma_rpc_gpu_error error{};
    if (mcdma_rpc_cuda_release(&buffer.wrapper, &error)) throw std::runtime_error(error.message);
    buffer.errors = nullptr;
}
static void gpu_shutdown() {}
#include "mailbox_app.hpp"

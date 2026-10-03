#include "../gpu_internal.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main() {
    size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    void *memory = nullptr; assert(!posix_memalign(&memory, page, page));
    std::memset(memory, 0x5a, page);
    mcdma_rpc_gpu_error error{};
    assert(gpu_span(memory, page, &error) == MCDMA_RPC_GPU_OK);
    assert(gpu_span(nullptr, page, &error) == MCDMA_RPC_GPU_INVALID);
    assert(gpu_span(memory, 0, &error) == MCDMA_RPC_GPU_INVALID);
    assert(gpu_span(static_cast<char *>(memory) + 1, page, &error) == MCDMA_RPC_GPU_ALIGNMENT);
    assert(gpu_span(memory, page - 1, &error) == MCDMA_RPC_GPU_ALIGNMENT);
    assert(gpu_span(reinterpret_cast<void *>(UINTPTR_MAX - 3), 8, &error) == MCDMA_RPC_GPU_OVERFLOW);
    assert(gpu_span(nullptr, 1, nullptr) == MCDMA_RPC_GPU_INVALID);
    for (size_t i = 0; i < page; ++i) assert(static_cast<unsigned char *>(memory)[i] == 0x5a);
    std::free(memory); std::puts("gpu span checks passed");
}

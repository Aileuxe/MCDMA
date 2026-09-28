// SPDX-License-Identifier: Apache-2.0
// Offline protocol test only: this CPU backend is never a GPU build target.
#include "mailbox_support.hpp"
#include <fcntl.h>
#include <sys/mman.h>
#define MCDMA_MAILBOX_EXPORT 1
static unsigned char *test_owner = nullptr;
static int test_fd = -1;
static size_t test_size = 0;
static void gpu_set_export_mode(bool) {}
static void gpu_owned_allocate(size_t bytes) {
    char path[] = "/tmp/mcdma-gpu-owned-test-XXXXXX";
    test_fd = mkstemp(path);
    mailbox_require(test_fd >= 0 && !unlink(path) && !fcntl(test_fd, F_SETFD, FD_CLOEXEC) && !ftruncate(test_fd, bytes), "cannot create test-owned mapping");
    test_owner = static_cast<unsigned char *>(mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, test_fd, 0));
    mailbox_require(test_owner != MAP_FAILED, "cannot map test-owned allocation");
    test_size = bytes;
}
static unsigned char *gpu_owned_pointer() { return test_owner; }
static int gpu_owned_fd() { return test_fd; }
static void gpu_owned_release() {
    mailbox_require(*reinterpret_cast<uint64_t *>(test_owner + 320) == 0xdecaf, "allocation released before daemon cleanup proof");
    munmap(test_owner, test_size); close(test_fd); test_owner = nullptr; test_fd = -1;
    std::puts("OFFLINE_OWNED_RELEASE after_child_cleanup=1");
}
struct GPUBuffer { unsigned char *memory = nullptr; unsigned bytes = 0; };
static void gpu_init(unsigned, const char *) { std::puts("OFFLINE_TEST_BACKEND hardware_gpu=0"); }
static void gpu_wrap(GPUBuffer &buffer, unsigned char *memory, unsigned bytes) { buffer = {memory, bytes}; }
static unsigned char test_value(unsigned i, unsigned length, unsigned seq, unsigned kind) {
    if (i >= length) return 0xd3;
    unsigned salt = seq ^ (seq >> 8) ^ (seq >> 16) ^ (seq >> 24);
    return static_cast<unsigned char>(i * (kind == 1 ? 37u : 53u) + salt * (kind == 1 ? 19u : 101u) + kind * 13u);
}
static void gpu_fill(GPUBuffer &buffer, unsigned length, unsigned seq, unsigned kind) {
    for (unsigned i = 0; i < buffer.bytes; ++i) buffer.memory[i] = test_value(i, length, seq, kind);
}
static void gpu_verify(GPUBuffer &buffer, unsigned length, unsigned seq, unsigned kind, const char *) {
    for (unsigned i = 0; i < buffer.bytes; ++i)
        mailbox_require(buffer.memory[i] == test_value(i, length, seq, kind), "simulated verification failed");
}
static void gpu_release(GPUBuffer &) {}
static void gpu_shutdown() {}
#include "mailbox_app.hpp"

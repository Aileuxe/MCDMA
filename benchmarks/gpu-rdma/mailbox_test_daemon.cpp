// SPDX-License-Identifier: Apache-2.0
// Offline DMA-BUF child-lifetime simulation using a regular test file.
#include "mailbox_support.hpp"
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
constexpr size_t half = 4u << 20, control = 4096;
static unsigned char value(size_t i, size_t length, unsigned seq, unsigned kind) {
    if (i >= length) return 0xd3;
    unsigned salt = seq ^ (seq >> 8) ^ (seq >> 16) ^ (seq >> 24);
    return static_cast<unsigned char>(i * (kind == 1 ? 37 : 53) + salt * (kind == 1 ? 19 : 101) + kind * 13);
}
int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    alarm(10);
    try {
        mailbox_require(argc == 7 && !std::strcmp(argv[1], "connect") && !std::strcmp(argv[2], "--buffer-fd") &&
                        !std::strcmp(argv[3], "3") && !std::strcmp(argv[4], "--parent-fd") && !std::strcmp(argv[5], "4"), "incorrect spawn arguments");
        struct stat lease{};
        mailbox_require(!fstat(4, &lease) && S_ISFIFO(lease.st_mode) && (fcntl(4, F_GETFL) & O_ACCMODE) == O_RDONLY, "lease must be a pipe reader");
        for (int fd = 5; fd < 256; ++fd) {
            struct stat info{};
            mailbox_require(fstat(fd, &info) || info.st_ino != lease.st_ino || info.st_dev != lease.st_dev || !S_ISFIFO(info.st_mode), "inherited a second lease descriptor");
        }
        const char *socket_path = std::getenv("MCDMA_RPCD_SOCKET");
        const char *expected_socket = std::getenv("MCDMA_TEST_EXPECT_SOCKET");
        mailbox_require(socket_path && expected_socket && !std::strcmp(socket_path, expected_socket), "per-link socket was not propagated");
        auto *base = static_cast<unsigned char *>(mmap(nullptr, 2 * half, PROT_READ | PROT_WRITE, MAP_SHARED, 3, 0));
        mailbox_require(base != MAP_FAILED, "cannot map test descriptor");
        auto word = [&](size_t offset) { return reinterpret_cast<uint64_t *>(base + offset); };
        auto load = [&](size_t offset) { return __atomic_load_n(word(offset), __ATOMIC_ACQUIRE); };
        auto store = [&](size_t offset, uint64_t v) { __atomic_store_n(word(offset), v, __ATOMIC_RELEASE); };
        const char *mode = std::getenv("MCDMA_TEST_DAEMON_MODE");
        bool never_up = mode && !std::strcmp(mode, "never-up");
        std::printf("OFFLINE_DAEMON_STARTED pid=%ld lease_writer_inherited=0\n", static_cast<long>(getpid()));
        if (!never_up) {
            store(256, half); store(264, half); store(72, 1); store(64, 1);
            unsigned last = 0;
            uint64_t deadline = mailbox_now() + 5000000000ull;
            while (last < 13) {
                mailbox_require(mailbox_now() < deadline, "test payload loop timed out");
                uint64_t request = load(0);
                unsigned seq = request >> 32, length = static_cast<unsigned>(request);
                if (seq == last) { timespec pause{0, 100000}; nanosleep(&pause, nullptr); continue; }
                mailbox_require(seq == last + 1, "out of order test request");
                if (seq == 13) mailbox_require(length == 0, "last request must be empty");
                else {
                    for (unsigned i = 0; i < half - control; ++i)
                        mailbox_require(base[control + i] == value(i, length, seq, 1), "test request payload mismatch");
                    for (unsigned i = 0; i < half - control; ++i) base[half + control + i] = value(i, length, seq, 2);
                }
                store(half + 64, request);
                last = seq;
            }
        }
        pollfd lease_poll{4, POLLIN, 0};
        mailbox_require(poll(&lease_poll, 1, 5000) > 0, "parent did not close its lease");
        char byte;
        mailbox_require(read(4, &byte, 1) == 0, "parent lease did not reach EOF");
        store(320, 0xdecaf);
        munmap(base, 2 * half); close(3); close(4);
        std::puts("OFFLINE_DAEMON_CLEAN_EXIT lease_eof=1");
        return mode && !std::strcmp(mode, "nonzero-exit") ? 7 : 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "OFFLINE_DAEMON_ERROR reason=%s\n", error.what());
        return 2;
    }
}

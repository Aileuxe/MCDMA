// SPDX-License-Identifier: Apache-2.0
// Included after one backend's GPUBuffer and GPU operations.
#include "mcdma_rpc.h"
#include "spawned_daemon.hpp"
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

constexpr uint64_t mailbox_control = 4096, mailbox_half = 4u << 20;
constexpr unsigned mailbox_calls = 12;
static unsigned call_bytes(unsigned call) {
    constexpr unsigned sizes[] = {64, 4096, 1u << 20};
    return sizes[call % 3];
}
struct Options {
    bool service = false;
    std::string link, mailbox, socket, shader, daemon, peer;
    unsigned gpu = 0, timeout = 30;
};
static unsigned unsigned_option(const char *text, unsigned maximum) {
    char *end = nullptr;
    errno = 0;
    unsigned long value = std::strtoul(text, &end, 10);
    mailbox_require(*text && *text != '-' && !errno && !*end && value <= maximum, "invalid numeric option");
    return static_cast<unsigned>(value);
}
static Options options(int argc, char **argv) {
    mailbox_require(argc >= 2 && (!std::strcmp(argv[1], "client") || !std::strcmp(argv[1], "service")),
                    "usage: gpu-mailbox-{cuda,vulkan} client|service --link NAME [--gpu N] [--timeout SECONDS] [--mailbox PATH] [--socket PATH] [--shader PATH] [--daemon PATH --connect PEER]");
    Options result;
    result.service = !std::strcmp(argv[1], "service");
    for (int i = 2; i < argc; i += 2) {
        mailbox_require(i + 1 < argc, "option requires a value");
        std::string name = argv[i];
        if (name == "--link") result.link = argv[i + 1];
        else if (name == "--gpu") result.gpu = unsigned_option(argv[i + 1], 255);
        else if (name == "--timeout") result.timeout = unsigned_option(argv[i + 1], 300);
        else if (name == "--mailbox") result.mailbox = argv[i + 1];
        else if (name == "--socket") result.socket = argv[i + 1];
        else if (name == "--shader") result.shader = argv[i + 1];
        else if (name == "--daemon") result.daemon = argv[i + 1];
        else if (name == "--connect") result.peer = argv[i + 1];
        else throw std::runtime_error("unknown option");
    }
    mailbox_require(!result.link.empty() && result.link.size() <= 20 && result.timeout, "a valid --link and positive --timeout are required");
    for (char c : result.link)
        mailbox_require((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_',
                        "invalid link name");
    mailbox_require(result.daemon.empty() == result.peer.empty(), "--daemon and --connect must be supplied together");
    if (!result.daemon.empty()) {
        mailbox_require(!result.service && result.mailbox.empty(), "owned DMA-BUF mode requires client role and no --mailbox");
        mailbox_require(result.peer.substr(0, result.peer.find(',')) == result.link, "--connect must use the selected link name");
#ifndef MCDMA_MAILBOX_EXPORT
        throw std::runtime_error("owned DMA-BUF mode is available only in the Vulkan client");
#endif
    }
    if (result.mailbox.empty()) result.mailbox = "/dev/shm/mcdma-rpc." + result.link;
    if (result.socket.empty()) result.socket = "/tmp/mcdma-rpcd." + result.link + ".sock";
    return result;
}
static void watchdog(int) {
    const char message[] = "GPU_MAILBOX_FAILED reason=application-watchdog cleanup=unverified\n";
    ssize_t written = write(STDERR_FILENO, message, sizeof(message) - 1);
    (void)written;
    _exit(2);
}
struct Mailbox {
    unsigned char *mapping = nullptr;
    int file = -1, lock = -1, service = -1;
    uint64_t generation = 0;
    uint32_t sequence = 0;
    unsigned timeout = 30;
    bool is_service = false, external_mapping = false;
    SpawnedDaemon *child = nullptr;

    volatile uint64_t *word(uint64_t offset) { return reinterpret_cast<volatile uint64_t *>(mapping + offset); }
    uint64_t load(uint64_t offset) { return __atomic_load_n(word(offset), __ATOMIC_ACQUIRE); }
    void health() {
        if (child) mailbox_require(child->running(), "owned daemon exited during transfer");
        if (is_service) {
            pollfd fd{service, POLLIN, 0};
            int result = poll(&fd, 1, 0);
            mailbox_require(result == 0 || (result < 0 && errno == EINTR), "daemon ended service registration");
        } else mailbox_require(load(64) == 1 && load(72) == generation, "link down or generation changed; cannot resume");
    }
    uint64_t wait(uint64_t offset, uint32_t wanted, uint32_t previous, bool final_service_ready = false) {
        uint64_t deadline = mailbox_now() + static_cast<uint64_t>(timeout) * 1000000000ull;
        for (;;) {
            uint64_t current = load(offset);
            if (final_service_ready && static_cast<uint32_t>(current >> 32) == wanted) return current;
            health();
            uint32_t actual = current >> 32;
            mailbox_require(actual == wanted || actual == previous, "out-of-order mailbox sequence");
            if (actual == wanted) { health(); return current; }
            uint64_t now = mailbox_now();
            mailbox_require(now < deadline, "mailbox operation timed out");
            (void)mcdma_rpc_wait_word(word(offset), wanted, 1, 100000,
                                      std::min<uint64_t>(10000000, deadline - now));
        }
    }
    void publish(uint64_t offset, uint32_t seq, unsigned bytes) {
        health();
        mcdma_rpc_store_word(word(offset), static_cast<uint64_t>(seq) << 32 | bytes);
    }
    void claim_client(const Options &o) {
        // Share the existing Python proxy's lock, so the two applications
        // cannot publish concurrently through the same client mailbox.
        std::string path = "/tmp/mcdma-llama." + o.link + ".client.lock";
        lock = ::open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        struct stat info{};
        mailbox_require(lock >= 0 && !fstat(lock, &info) && S_ISREG(info.st_mode) && info.st_uid == geteuid()
                        && info.st_nlink == 1 && !flock(lock, LOCK_EX | LOCK_NB), "cannot acquire exclusive client lock");
    }
    void validate_layout() {
        mailbox_require(load(256) == mailbox_half && load(264) == mailbox_half, "expected 4 MiB request and reply halves");
        generation = load(72);
        sequence = load(0) >> 32;
        mailbox_require(sequence <= UINT32_MAX - mailbox_calls - 1, "sequence exhausted; restart link before reuse");
        uint32_t completed = load(mailbox_half + (is_service ? 0 : 64)) >> 32;
        mailbox_require(completed == sequence, "mailbox has an incomplete previous call");
        health();
        std::printf("GPU_MAILBOX_CONFIG protocol=gpu-mailbox-v1 role=%s request_bytes=%llu reply_bytes=%llu helper_abi=%u calls=%u\n",
                    is_service ? "service" : "client", static_cast<unsigned long long>(mailbox_half),
                    static_cast<unsigned long long>(mailbox_half), mcdma_rpc_abi(), mailbox_calls);
    }
    void attach_owned(const Options &o, unsigned char *memory, SpawnedDaemon &daemon) {
        timeout = o.timeout; mapping = memory; external_mapping = true; child = &daemon;
        mailbox_require(mcdma_rpc_abi() == MCDMA_RPC_ABI, "mailbox helper ABI mismatch");
        uint64_t deadline = mailbox_now() + static_cast<uint64_t>(timeout) * 1000000000ull;
        for (;;) {
            mailbox_require(child->running(), "owned daemon exited before link readiness");
            if (load(256) == mailbox_half && load(264) == mailbox_half && load(64) == 1 && load(72)) break;
            mailbox_require(mailbox_now() < deadline, "owned daemon link readiness timed out");
            timespec pause{0, 10000000}; nanosleep(&pause, nullptr);
        }
        validate_layout();
    }
    void open(const Options &o) {
        timeout = o.timeout; is_service = o.service;
        mailbox_require(mcdma_rpc_abi() == MCDMA_RPC_ABI, "mailbox helper ABI mismatch");
        if (!is_service) {
            claim_client(o);
        } else {
            service = socket(AF_UNIX, SOCK_STREAM, 0);
            mailbox_require(service >= 0 && !fcntl(service, F_SETFD, FD_CLOEXEC), "cannot open service socket");
            timeval limit{static_cast<time_t>(timeout), 0};
            mailbox_require(!setsockopt(service, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit)) &&
                            !setsockopt(service, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit)), "cannot bound service socket waits");
            sockaddr_un address{}; address.sun_family = AF_UNIX;
            mailbox_require(o.socket.size() < sizeof(address.sun_path), "service socket path is too long");
            std::memcpy(address.sun_path, o.socket.c_str(), o.socket.size() + 1);
            mailbox_require(!connect(service, reinterpret_cast<sockaddr *>(&address), sizeof(address)), "cannot connect service socket");
            constexpr char request[] = "MODE poll\n";
            mailbox_require(send(service, request, sizeof(request) - 1, MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(request) - 1),
                            "cannot register service");
            char answer[3];
            mailbox_require(recv(service, answer, sizeof(answer), MSG_WAITALL) == static_cast<ssize_t>(sizeof(answer)) && !std::memcmp(answer, "OK\n", 3),
                            "daemon refused exclusive service registration");
        }
        file = ::open(o.mailbox.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
        struct stat info{};
        mailbox_require(file >= 0 && !fstat(file, &info) && S_ISREG(info.st_mode) && info.st_uid == geteuid() &&
                        info.st_size == static_cast<off_t>(2 * mailbox_half), "expected an owned regular 8 MiB mailbox");
        void *mapped = mmap(nullptr, 2 * mailbox_half, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
        mailbox_require(mapped != MAP_FAILED, "cannot map mailbox");
        mapping = static_cast<unsigned char *>(mapped);
        validate_layout();
    }
    void close() {
        if (mapping && !external_mapping) mailbox_require(!munmap(mapping, 2 * mailbox_half), "cannot unmap mailbox");
        mapping = nullptr;
        if (file >= 0) { ::close(file); file = -1; }
        if (service >= 0) { ::close(service); service = -1; }
        if (lock >= 0) { ::close(lock); lock = -1; }
    }
};

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    SpawnedDaemon child;
    unsigned stop_timeout = 45;
    try {
        Options config = options(argc, argv);
        signal(SIGALRM, watchdog);
        alarm(config.timeout * (mailbox_calls + 2) + 30);
        Mailbox box;
        bool owned = !config.daemon.empty();
        stop_timeout = config.timeout + 15;
#ifdef MCDMA_MAILBOX_EXPORT
        gpu_set_export_mode(owned);
        if (owned) {
            box.claim_client(config);
            gpu_init(config.gpu, config.shader.empty() ? nullptr : config.shader.c_str());
            gpu_owned_allocate(2 * mailbox_half);
            // Only newly owned control pages are initialized by the CPU;
            // payload spans are populated and checked exclusively by the GPU.
            std::memset(gpu_owned_pointer(), 0, mailbox_control);
            std::memset(gpu_owned_pointer() + mailbox_half, 0, mailbox_control);
            child.start(config.daemon, config.peer, config.socket, gpu_owned_fd());
            box.attach_owned(config, gpu_owned_pointer(), child);
        } else
#endif
        {
            box.open(config);
            gpu_init(config.gpu, config.shader.empty() ? nullptr : config.shader.c_str());
        }
        GPUBuffer request{}, reply{};
        unsigned capacity = mailbox_half - mailbox_control;
        gpu_wrap(request, box.mapping + mailbox_control, capacity);
        gpu_wrap(reply, box.mapping + mailbox_half + mailbox_control, capacity);
        gpu_fill(request, 0, 0, 0); gpu_fill(reply, 0, 0, 0);
        gpu_verify(request, 0, 0, 0, "initial-request-guards");
        gpu_verify(reply, 0, 0, 0, "initial-reply-guards");
        std::puts("GPU_MAILBOX_READY");
        uint32_t previous = box.sequence;
        for (unsigned call = 0; call < mailbox_calls; ++call) {
            uint32_t seq = previous + 1;
            unsigned bytes = call_bytes(call);
            if (config.service) {
                uint64_t received = box.wait(0, seq, previous);
                mailbox_require(static_cast<uint32_t>(received) == bytes, "unexpected request length");
                gpu_verify(request, bytes, seq, 1, "received-request");
                gpu_fill(reply, bytes, seq, 2);
                // Complete the reset before replying: the client cannot issue
                // its next request until it receives and verifies this reply.
                gpu_fill(request, 0, 0, 0);
                box.publish(mailbox_half + 128, seq, bytes);
            } else {
                gpu_fill(request, bytes, seq, 1);
                gpu_fill(reply, 0, 0, 0);
                box.publish(0, seq, bytes);
                uint64_t received = box.wait(mailbox_half + 64, seq, previous);
                mailbox_require(static_cast<uint32_t>(received) == bytes, "unexpected reply length");
                gpu_verify(reply, bytes, seq, 2, "received-reply");
                gpu_verify(request, bytes, seq, 1, "request-unchanged");
            }
            box.health();
            std::printf("GPU_MAILBOX_CALL role=%s call=%u seq=%u bytes=%u gpu_verified=1 payload_cpu_copy_bytes=0\n",
                        config.service ? "service" : "client", call + 1, seq, bytes);
            previous = seq;
        }
        // A separate empty control call proves the client received and GPU-
        // verified the final payload before either application releases it.
        uint32_t ack = previous + 1;
        if (config.service) {
            mailbox_require(static_cast<uint32_t>(box.wait(0, ack, previous)) == 0, "invalid final acknowledgment");
            gpu_verify(request, 0, 0, 0, "final-request-guards");
            box.publish(mailbox_half + 128, ack, 0);
            mailbox_require(static_cast<uint32_t>(box.wait(mailbox_half, ack, previous, true)) == 0, "invalid final daemon reply");
        } else {
            box.publish(0, ack, 0);
            mailbox_require(static_cast<uint32_t>(box.wait(mailbox_half + 64, ack, previous)) == 0, "invalid final service reply");
        }
        if (owned) mailbox_require(child.stop(stop_timeout), "owned daemon did not exit cleanly; GPU allocation retained");
        gpu_release(reply); gpu_release(request);
#ifdef MCDMA_MAILBOX_EXPORT
        if (owned) gpu_owned_release();
#endif
        gpu_shutdown();
        box.close();
        alarm(0);
        std::puts("GPU_MAILBOX_COMPLETE calls=12 final_control_ack=1 cleanup=ok payload_cpu_copy_bytes=0");
        return 0;
    } catch (const std::exception &error) {
        // Close the lease and wait before any process teardown releases this
        // application's GPU objects. No explicit GPU release on a failed run;
        // a still-running child retains its DMA-BUF/MR backing references.
        try { (void)child.stop(stop_timeout); } catch (...) {}
        std::fprintf(stderr, "GPU_MAILBOX_FAILED reason=%s cleanup=unverified\n", error.what());
        return 2;
    }
}

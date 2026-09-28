// Linux Vulkan correctness check; not a throughput benchmark.
// Adapted from peer/verbs_peer.c at commit 719219272c9ce6fc091b4eab6214eac510b5e387.
// SPDX-License-Identifier: Apache-2.0
// The wire/control commands match peer/verbs_peer.c's four-way test.
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#include <time.h>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "vulkan_buffer.hpp"

static uint64_t monotonic_ns() {
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now)) { std::perror("clock_gettime"); std::exit(2); }
    return static_cast<uint64_t>(now.tv_sec) * 1000000000ull + now.tv_nsec;
}

static void fail(const char *operation) { std::perror(operation); std::exit(2); }

static void line(char *out, size_t capacity) {
    uint64_t deadline = monotonic_ns() + 60000000000ull;
    size_t size = 0;
    for (;;) {
        uint64_t now = monotonic_ns();
        if (now >= deadline) { std::fprintf(stderr, "Control input timed out after 60 seconds\n"); std::exit(2); }
        pollfd fd{STDIN_FILENO, POLLIN, 0};
        int wait = poll(&fd, 1, static_cast<int>((deadline - now + 999999) / 1000000));
        if (wait < 0 && errno == EINTR) continue;
        if (wait <= 0) { std::fprintf(stderr, "Control input unavailable or timed out\n"); std::exit(2); }
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n != 1) { std::fprintf(stderr, "Control input closed\n"); std::exit(2); }
        if (c == '\n') { out[size] = 0; return; }
        if (size + 1 >= capacity) { std::fprintf(stderr, "Control line too long\n"); std::exit(2); }
        out[size++] = c;
    }
}

static void modify(ibv_qp *qp, ibv_qp_attr &attr, int mask, const char *stage) {
    int result = ibv_modify_qp(qp, &attr, mask);
    if (result) {
        std::fprintf(stderr, "QP_ERROR stage=%s result=%d reason=%s\n", stage, result, std::strerror(result > 0 ? result : errno));
        std::exit(2);
    }
}

static bool post_and_wait(ibv_qp *qp, ibv_cq *cq, ibv_mr *mr, unsigned char *memory,
                          uint64_t remote, uint32_t rkey, bool read_operation) {
    ibv_sge sge{};
    sge.addr = reinterpret_cast<uintptr_t>(memory); sge.length = payload_bytes; sge.lkey = mr->lkey;
    ibv_send_wr wr{}, *bad = nullptr;
    wr.wr_id = ++wr_sequence; wr.sg_list = &sge; wr.num_sge = 1;
    wr.opcode = read_operation ? IBV_WR_RDMA_READ : IBV_WR_RDMA_WRITE; wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = remote; wr.wr.rdma.rkey = rkey;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    int result = ibv_post_send(qp, &wr, &bad);
    if (result) { std::fprintf(stderr, "POST_ERROR result=%d\n", result); return false; }
    uint64_t deadline = monotonic_ns() + 10000000000ull;
    while (monotonic_ns() < deadline) {
        ibv_wc wc{};
        int n = ibv_poll_cq(cq, 1, &wc);
        if (n < 0) { std::fprintf(stderr, "POLL_ERROR result=%d\n", n); return false; }
        if (n == 1) {
            if (wc.status != IBV_WC_SUCCESS || wc.wr_id != wr.wr_id ||
                wc.opcode != (read_operation ? IBV_WC_RDMA_READ : IBV_WC_RDMA_WRITE)) {
                std::fprintf(stderr, "CQ_ERROR status=%u vendor=%u opcode=%u wr_id=%llu\n", wc.status,
                             wc.vendor_err, wc.opcode, static_cast<unsigned long long>(wc.wr_id));
                return false;
            }
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            return true;
        }
        timespec pause{0, 100000}; nanosleep(&pause, nullptr);
    }
    std::fprintf(stderr, "CQ_TIMEOUT wr_id=%llu\n", static_cast<unsigned long long>(wr.wr_id));
    return false;
}

static unsigned number(const char *text, unsigned maximum, const char *name) {
    char *end = nullptr; errno = 0;
    unsigned long value = std::strtoul(text, &end, 10);
    if (!*text || *text == '-' || errno || *end || value > maximum) {
        std::fprintf(stderr, "Invalid %s\n", name); std::exit(2);
    }
    return static_cast<unsigned>(value);
}

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 5 || argc > 6) {
        std::fprintf(stderr, "Usage: gpu-verbs-peer DEVICE GID_INDEX mapped-host|device-probe initiator|responder|probe [GPU_INDEX]\n");
        return 2;
    }
    bool device_probe = !std::strcmp(argv[3], "device-probe");
    bool probe = !std::strcmp(argv[4], "probe"), initiator = !std::strcmp(argv[4], "initiator");
    if ((!device_probe && std::strcmp(argv[3], "mapped-host")) ||
        (!probe && !initiator && std::strcmp(argv[4], "responder")) || (device_probe && !probe)) {
        std::fprintf(stderr, "Invalid mode; device-probe supports only probe and never falls back\n"); return 2;
    }
    unsigned gid_index = number(argv[2], 255, "GID index");
    const char *port_setting = std::getenv("MCDMA_RDMA_PORT");
    unsigned rdma_port = port_setting ? number(port_setting, 255, "RDMA port") : 1;
    if (!rdma_port) return 2;
    unsigned gpu_index = argc == 6 ? number(argv[5], 255, "GPU index") : 0;
    const char *bytes = std::getenv("MCDMA_PAYLOAD_BYTES"), *value_seed = std::getenv("MCDMA_GPU_SEED");
    if (bytes) payload_bytes = number(bytes, 4096, "payload bytes");
    if (payload_bytes != 1024 && payload_bytes != 4096) { std::fprintf(stderr, "Payload must be 1024 or 4096\n"); return 2; }
    if (value_seed) seed = number(value_seed, UINT_MAX, "seed");
    ibv_mtu path_mtu = IBV_MTU_1024;
    const char *mtu = std::getenv("MCDMA_PATH_MTU");
    if (mtu && std::strcmp(mtu, "1024") && std::strcmp(mtu, "4096")) return 2;
    if (mtu && !std::strcmp(mtu, "4096")) path_mtu = IBV_MTU_4096;

    select_vulkan(gpu_index);
    std::fprintf(stderr, "VULKAN_TEST_CONFIG mode=%s seed=%u payload_bytes=%u path_mtu=%u cpu_payload_copies=0\n",
                 argv[3], seed, payload_bytes, 128u << path_mtu);
    if (device_probe) {
        std::fprintf(stderr, "REGISTRATION mode=device-probe attempted=0 reason=no-verbs-address-for-opaque-VkDeviceMemory fallback=0 transfer_test=0\n");
        vkDestroyDevice(vulkan.device, nullptr); vkDestroyInstance(vulkan.instance, nullptr);
        return 3;
    }

    int count = 0; ibv_device **devices = ibv_get_device_list(&count); if (!devices) fail("device list");
    ibv_context *ctx = nullptr;
    for (int i = 0; i < count; ++i) if (!std::strcmp(ibv_get_device_name(devices[i]), argv[1])) ctx = ibv_open_device(devices[i]);
    ibv_free_device_list(devices); if (!ctx) fail("open selected RDMA device");
    ibv_device_attr hardware{}; if (ibv_query_device(ctx, &hardware)) fail("query device");
    std::fprintf(stderr, "RDMA_CONFIG device=%s port=%u gid_index=%u vendor=%u part=%u firmware=%s\n", argv[1], rdma_port, gid_index, hardware.vendor_id, hardware.vendor_part_id, hardware.fw_ver);
    ibv_pd *pd = ibv_alloc_pd(ctx); if (!pd) fail("allocate PD");
    Buffer buffer; allocate_buffer(buffer, device_probe);
    if (!gpu_verify(buffer, "initial-guards", GUARD, GUARD, GUARD)) return 2;
    errno = 0;
    ibv_mr *mr = ibv_reg_mr(pd, buffer.network, allocation_bytes,
                           IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
    if (!mr) {
        int error = errno;
        std::fprintf(stderr, "REGISTRATION mode=%s registered=0 errno=%d reason=%s fallback=0 transfer_test=0\n", argv[3], error, std::strerror(error));
        free_buffer(buffer); ibv_dealloc_pd(pd); ibv_close_device(ctx);
        return device_probe ? 3 : 2;
    }
    std::fprintf(stderr, "REGISTRATION mode=%s registered=1 bytes=%u fallback=0\n", argv[3], allocation_bytes);
    if (probe) {
        if (ibv_dereg_mr(mr)) fail("deregister probe MR");
        free_buffer(buffer);
        if (ibv_dealloc_pd(pd) || ibv_close_device(ctx)) fail("release probe resources");
        std::printf("GPU_REGISTRATION_PROBE mode=%s registered=1 transfer_test=0\n", argv[3]);
        return 0;
    }

    ibv_port_attr port{}; if (ibv_query_port(ctx, rdma_port, &port)) fail("query port");
    if (port.state != IBV_PORT_ACTIVE || port.link_layer != IBV_LINK_LAYER_ETHERNET ||
        path_mtu > port.active_mtu || path_mtu > port.max_mtu) {
        std::fprintf(stderr, "Selected port is not active Ethernet at requested MTU\n"); return 2;
    }
    ibv_gid gid{}; if (ibv_query_gid(ctx, rdma_port, gid_index, &gid)) fail("query GID");
    ibv_cq *cq = ibv_create_cq(ctx, 16, nullptr, nullptr, 0); if (!cq) fail("create CQ");
    ibv_qp_init_attr init{};
    init.send_cq = cq; init.recv_cq = cq; init.qp_type = IBV_QPT_RC;
    init.cap.max_send_wr = 8; init.cap.max_recv_wr = 8; init.cap.max_send_sge = 1; init.cap.max_recv_sge = 1;
    ibv_qp *qp = ibv_create_qp(pd, &init); if (!qp) fail("create QP");
    ibv_qp_attr attr{}; attr.qp_state = IBV_QPS_INIT; attr.port_num = rdma_port;
    attr.qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    modify(qp, attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS, "INIT");
    char gid_text[INET6_ADDRSTRLEN]; if (!inet_ntop(AF_INET6, &gid, gid_text, sizeof(gid_text))) fail("format GID");
    unsigned char *memory = buffer.network + guard_bytes;
    std::printf("ENDPOINT %u %u %u %llu %u %s\n", qp->qp_num, 0x654321u, mr->rkey,
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(memory)), window_bytes, gid_text);
    char command[512], remote_gid[80], extra; unsigned remote_qpn, remote_psn;
    line(command, sizeof(command));
    if (std::sscanf(command, "%u %u %79s %c", &remote_qpn, &remote_psn, remote_gid, &extra) != 3 ||
        remote_qpn > 0xffffff || remote_psn > 0xffffff) return 2;
    attr = {}; attr.qp_state = IBV_QPS_RTR; attr.path_mtu = path_mtu;
    attr.dest_qp_num = remote_qpn; attr.rq_psn = remote_psn; attr.max_dest_rd_atomic = 1; attr.min_rnr_timer = 12;
    attr.ah_attr.is_global = 1; attr.ah_attr.port_num = rdma_port; attr.ah_attr.grh.sgid_index = gid_index; attr.ah_attr.grh.hop_limit = 64;
    if (inet_pton(AF_INET6, remote_gid, &attr.ah_attr.grh.dgid) != 1) return 2;
    modify(qp, attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
           IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER, "RTR");
    attr = {}; attr.qp_state = IBV_QPS_RTS; attr.timeout = 14; attr.retry_cnt = 7; attr.rnr_retry = 7;
    attr.sq_psn = 0x654321; attr.max_rd_atomic = 1;
    modify(qp, attr, IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC, "RTS");
    std::puts("READY");
    line(command, sizeof(command));
    char operation[32]; unsigned remote_key, remote_length; unsigned long long remote_address;
    if (std::sscanf(command, "%31s %u %llu %u %c", operation, &remote_key, &remote_address, &remote_length, &extra) != 4 ||
        !remote_key || remote_length < window_bytes || remote_address > UINT64_MAX - remote_length) return 2;
    bool good = true;
    if (initiator && !std::strcmp(operation, "INITIATE")) {
        gpu_fill(buffer, 0, FORWARD); gpu_fill(buffer, 8192, POISON);
        bool wrote = post_and_wait(qp, cq, mr, memory, remote_address, remote_key, false);
        bool read = wrote && post_and_wait(qp, cq, mr, memory + 8192, remote_address, remote_key, true);
        good = read && gpu_verify(buffer, "forward-roundtrip", FORWARD, GUARD, FORWARD);
        std::printf("NATIVE_FORWARD write=%d read=%d verified=%u\n", wrote, read, good ? payload_bytes : 0);
        if (!good) return 2; // Do not free registered storage after uncertain completion.
        line(command, sizeof(command));
        if (std::strcmp(command, "CHECKREVERSE")) return 2;
        good = gpu_verify(buffer, "received-reverse", FORWARD, REVERSE, FORWARD);
        std::printf("NATIVE_REVERSE verified=%u\n", good ? payload_bytes : 0);
    } else if (!initiator && (!std::strcmp(operation, "ROUNDTRIP") || !std::strcmp(operation, "REVERSE"))) {
        bool reverse_only = !std::strcmp(operation, "REVERSE");
        Pattern first = reverse_only ? GUARD : FORWARD;
        good = gpu_verify(buffer, "received-forward", first, GUARD, GUARD);
        if (!good) return 2;
        gpu_fill(buffer, 4096, REVERSE); gpu_fill(buffer, 8192, POISON);
        bool wrote = post_and_wait(qp, cq, mr, memory + 4096, remote_address + 4096, remote_key, false);
        bool read = wrote && post_and_wait(qp, cq, mr, memory + 8192, remote_address + 4096, remote_key, true);
        good = read && gpu_verify(buffer, "reverse-roundtrip", first, REVERSE, REVERSE);
        std::printf("PEER_RESULT forward=%u write=%d read=%d reverse=%u\n",
                    good && !reverse_only ? payload_bytes : 0, wrote, read, good ? payload_bytes : 0);
        if (!good) return 2;
    } else { std::fprintf(stderr, "Unsupported control command or role; benchmarks are deliberately absent\n"); return 2; }

    // Never explicitly release the allocation after a refused verbs teardown.
    if (ibv_destroy_qp(qp) || ibv_destroy_cq(cq) || ibv_dereg_mr(mr)) fail("RDMA cleanup");
    free_buffer(buffer);
    if (ibv_dealloc_pd(pd) || ibv_close_device(ctx)) fail("context cleanup");
    std::fprintf(stderr, "GPU_RDMA_COMPLETE mode=mapped-host cleanup=ok payload_cpu_copy_bytes=0\n");
    return good ? 0 : 2;
}

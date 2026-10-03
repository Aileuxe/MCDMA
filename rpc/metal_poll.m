/* metal-poll: can a resident Metal kernel see words that another agent writes into host memory, with no event wake?
 * Every 16-byte line of a 4 KiB packet is three data words and a round tag, so a line is checked on its own and no
 * write order is assumed (Thunderbolt reports data in order 0). The kernel polls each line's tag, checks its data,
 * then stores an answer word that a host thread spins on. Spins are capped, so the kernel always ends.
 *   metal-poll cpu ROUNDS                          the CPU writes the packets: a GPU handoff baseline, one machine
 *   metal-poll nic-gpu DEVICE GID VIA PORT ROUNDS  packets arrive by Thunderbolt RDMA into receives the GPU polls
 *   metal-poll nic-send DEVICE GID VIA PORT ROUNDS sends them and times each round trip, answered by the GPU side */
#import <Metal/Metal.h>

#include "link.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINES 256                   /* one 4 KiB packet */
#define SLOTS 64                    /* packets in the ring */
#define BATCH 1000                  /* rounds a dispatch, so no command buffer runs long */
#define SPIN (1u << 22)             /* polls before a line is given up on */
#define WAIT_NS 2000000000ull

static const char *kernel_source =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "kernel void poll(device atomic_uint *ring [[buffer(0)]], device atomic_uint *ctl [[buffer(1)]],\n"
    "                 constant uint *a [[buffer(2)]], uint tid [[thread_position_in_threadgroup]],\n"
    "                 uint threads [[threads_per_threadgroup]]) {\n"
    "    for (uint r = a[0]; r < a[0] + a[1]; ++r) {\n"
    "        device atomic_uint *pkt = ring + ((r - 1) % a[2]) * a[3] * 4;\n"
    "        bool late = false;\n"
    "        for (uint l = tid; l < a[3] && !late; l += threads) {\n"
    "            uint spins = 0;\n"
    "            while (!late && atomic_load_explicit(&pkt[l * 4 + 3], memory_order_relaxed) != r)\n"
    "                late = ++spins >= a[4] ||\n"
    "                       ((spins & 1023) == 0 && atomic_load_explicit(&ctl[1], memory_order_relaxed));\n"
    "            if (late) { atomic_fetch_add_explicit(&ctl[2], 1u, memory_order_relaxed); break; }\n"
    "            for (uint w = 0; w < 3; ++w)\n"
    "                if (atomic_load_explicit(&pkt[l * 4 + w], memory_order_relaxed) != r * 2654435761u + l * 3 + w)\n"
    "                    atomic_fetch_add_explicit(&ctl[3], 1u, memory_order_relaxed);\n"
    "        }\n"
    "        // every thread reaches the barrier; then all leave together if any line was given up on\n"
    "        threadgroup_barrier(mem_flags::mem_device);\n"
    "        if (atomic_load_explicit(&ctl[2], memory_order_relaxed)) return;\n"
    "        if (tid == 0) atomic_store_explicit(&ctl[0], r, memory_order_relaxed);\n"
    "    }\n"
    "}\n";

struct gpu {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLComputePipelineState> pipeline;
    id<MTLBuffer> ring, ctl;
    uint32_t *ring_words, *ctl_words;
};

void link_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "metal-poll: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* The packet for `round`: line l holds three words derived from (round, l), then the round as its tag. */
static void fill(uint32_t *pkt, uint32_t round) {
    for (uint32_t l = 0; l < LINES; ++l) {
        for (uint32_t w = 0; w < 3; ++w) pkt[l * 4 + w] = round * 2654435761u + l * 3 + w;
        __atomic_store_n(&pkt[l * 4 + 3], round, __ATOMIC_RELEASE);
    }
}

/* Compiled at run time, as the engine compiles its kernels; the ring may be memory the NIC also writes. */
static int gpu_open(struct gpu *g, void *ring, size_t ring_bytes) {
    NSError *error = nil;
    g->device = MTLCreateSystemDefaultDevice();
    if (!g->device) return -1;
    id<MTLLibrary> library = [g->device newLibraryWithSource:[NSString stringWithUTF8String:kernel_source]
                                                     options:[MTLCompileOptions new]
                                                       error:&error];
    id<MTLFunction> fn = [library newFunctionWithName:@"poll"];
    g->pipeline = fn ? [g->device newComputePipelineStateWithFunction:fn error:&error] : nil;
    g->queue = [g->device newCommandQueue];
    g->ring = [g->device newBufferWithBytesNoCopy:ring
                                           length:ring_bytes
                                          options:MTLResourceStorageModeShared
                                      deallocator:nil];
    g->ctl = [g->device newBufferWithLength:16384 options:MTLResourceStorageModeShared];
    if (!g->pipeline || !g->queue || !g->ring || !g->ctl || [g->ring contents] != ring) {
        const char *why = error ? error.localizedDescription.UTF8String : "no buffer";
        fprintf(stderr, "metal-poll: Metal setup failed: %s\n", why);
        return -1;
    }
    g->ring_words = ring;
    g->ctl_words = [g->ctl contents];
    memset(g->ctl_words, 0, 16384);
    return 0;
}

/* Rounds [first, first + count) run in one dispatch of one threadgroup. */
static id<MTLCommandBuffer> gpu_launch(struct gpu *g, uint32_t first, uint32_t count) {
    uint32_t args[5] = {first, count, SLOTS, LINES, SPIN};
    id<MTLCommandBuffer> cb = [g->queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:g->pipeline];
    [enc setBuffer:g->ring offset:0 atIndex:0];
    [enc setBuffer:g->ctl offset:0 atIndex:1];
    [enc setBytes:args length:sizeof(args) atIndex:2];
    [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [enc endEncoding];
    [cb commit];
    return cb;
}

/* Spin until the kernel's answer word reaches `round`; after WAIT_NS, tell the kernel to stop and return -1. */
static int await_answer(struct gpu *g, uint32_t round) {
    uint64_t began = link_now_ns();
    while (__atomic_load_n(&g->ctl_words[0], __ATOMIC_ACQUIRE) != round)
        if (link_now_ns() - began > WAIT_NS) {
            __atomic_store_n(&g->ctl_words[1], 1, __ATOMIC_RELEASE);
            return -1;
        }
    return 0;
}

static int by_value(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void summary(const char *mode, uint64_t *t, uint32_t n, uint32_t missed, const struct gpu *g) {
    qsort(t, n, sizeof(uint64_t), by_value);
    uint32_t timeouts = g ? g->ctl_words[2] : 0, torn = g ? g->ctl_words[3] : 0;
    printf("metal-poll: %s rounds=%u missed=%u line_timeouts=%u torn_lines=%u rtt_p50_us=%.2f rtt_p99_us=%.2f "
           "rtt_max_us=%.2f %s\n",
           mode, n, missed, timeouts, torn, n ? t[n / 2] / 1e3 : 0.0, n ? t[n * 99 / 100] / 1e3 : 0.0,
           n ? t[n - 1] / 1e3 : 0.0, missed || timeouts || torn ? "FAIL" : "PASS");
}

/* The CPU plays the NIC: it writes each round's packet, and the kernel answers. */
static int run_cpu(uint32_t rounds) {
    struct gpu g;
    void *ring = NULL;
    uint64_t *t = calloc(rounds ? rounds : 1, sizeof(uint64_t));
    uint32_t missed = 0;
    if (!t || posix_memalign(&ring, 16384, SLOTS * LINES * 16) || gpu_open(&g, ring, SLOTS * LINES * 16)) return 2;
    memset(ring, 0xff, SLOTS * LINES * 16);
    for (uint32_t first = 1; first <= rounds && !missed; first += BATCH) {
        uint32_t count = rounds - first + 1 < BATCH ? rounds - first + 1 : BATCH;
        id<MTLCommandBuffer> cb = gpu_launch(&g, first, count);
        for (uint32_t r = first; r < first + count && !missed; ++r) {
            uint64_t began = link_now_ns();
            fill(g.ring_words + ((r - 1) % SLOTS) * LINES * 4, r);
            missed += await_answer(&g, r) != 0;
            t[r - 1] = link_now_ns() - began;
        }
        [cb waitUntilCompleted];
    }
    summary("cpu", t, rounds, missed, &g);
    return missed || g.ctl_words[2] || g.ctl_words[3];
}

/* One UC queue pair over the Thunderbolt port, met through MCDMA's link-local exchange. */
static int nic_link(struct ep *e, struct xchg *x, const char *device, int gid, const char *via, int port, void *ring,
                    size_t ring_bytes, struct ibv_mr **mr) {
    if (ep_open(e, device, gid, 4096) || e->kind != LINK_TB || via_check(via, e) ||
        xchg_open(x, via, port, 0, "metal-poll")) {
        fprintf(stderr, "metal-poll: %s is not an active Thunderbolt RDMA device on %s\n", device, via);
        return -1;
    }
    struct ibv_qp_init_attr init;
    struct ibv_qp_attr a;
    memset(&init, 0, sizeof(init));
    memset(&a, 0, sizeof(a));
    e->cq = ibv_create_cq(e->ctx, 4096, NULL, NULL, 0);
    init.send_cq = init.recv_cq = e->cq;
    init.qp_type = IBV_QPT_UC;
    init.cap.max_send_wr = 1024, init.cap.max_recv_wr = SLOTS;
    init.cap.max_send_sge = init.cap.max_recv_sge = 1;
    a.qp_state = IBV_QPS_INIT, a.port_num = 1;
    *mr = ibv_reg_mr(e->pd, ring, ring_bytes, IBV_ACCESS_LOCAL_WRITE);
    if (!e->cq || !*mr || !(e->qp = ibv_create_qp(e->pd, &init)) ||
        ibv_modify_qp(e->qp, &a, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
        fprintf(stderr, "metal-poll: cannot create a UC queue pair on %s\n", device);
        return -1;
    }
    /* every ring slot gets a one-packet receive, posted in ring order */
    for (uint32_t s = 0; s < SLOTS; ++s) {
        struct ibv_sge sge = {(uintptr_t)ring + (uint64_t)s * LINES * 16, LINES * 16, (*mr)->lkey};
        struct ibv_recv_wr wr = {.wr_id = s, .sg_list = &sge, .num_sge = 1}, *bad = NULL;
        if (ibv_post_recv(e->qp, &wr, &bad)) return -1;
    }
    link_random(&e->psn, sizeof(e->psn));
    e->psn &= 0xffffff;
    struct xmsg mine, got;
    xchg_prepare(x, &mine, X_OFFER, ROLE_PEER, 0);
    mine.info.transport = LINK_TB, mine.info.lid = e->lid, mine.info.qpn = e->qp->qp_num, mine.info.psn = e->psn;
    mine.info.frames = SLOTS, mine.info.req = 8, mine.info.table.seg = 8, mine.info.table.length = 8;
    memcpy(mine.info.gid, e->gid.raw, 16);
    uint64_t deadline = link_now_ns() + 120 * 1000000000ull, resend = 0;
    for (int have = 0, heard = 0; !(have && heard);) {
        if (link_now_ns() > deadline) return -1;
        if (link_now_ns() >= resend) (void)xchg_send(x, &mine), resend = link_now_ns() + 100000000ull;
        if (xchg_recv(x, &got, 10) != 1 || got.kind != X_OFFER || got.role != ROLE_PEER) continue;
        if (!have) {
            memcpy(x->peer_session, got.from, 16);
            a.qp_state = IBV_QPS_RTR, a.path_mtu = IBV_MTU_4096, a.rq_psn = got.info.psn, a.dest_qp_num = got.info.qpn;
            a.ah_attr.dlid = got.info.lid, a.ah_attr.port_num = 1, a.ah_attr.is_global = 1;
            a.ah_attr.grh.hop_limit = 1, a.ah_attr.grh.sgid_index = (uint8_t)gid;
            memcpy(a.ah_attr.grh.dgid.raw, got.info.gid, 16);
            if (ibv_modify_qp(e->qp, &a, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN))
                return -1;
            memset(&a, 0, sizeof(a));
            a.qp_state = IBV_QPS_RTS, a.sq_psn = e->psn;
            if (ibv_modify_qp(e->qp, &a, IBV_QP_STATE | IBV_QP_SQ_PSN)) return -1;
            have = 1, mine.flags = X_HAVE;
            memcpy(mine.to, got.from, 16);
            (void)xchg_send(x, &mine);
        }
        heard |= (got.flags & X_HAVE) && xchg_ours(x, &got);
    }
    return 0;
}

static int post_send(struct ep *e, void *addr, uint32_t len, uint32_t lkey) {
    struct ibv_sge sge = {(uintptr_t)addr, len, lkey};
    struct ibv_send_wr wr = {.wr_id = 1u << 31, .sg_list = &sge, .num_sge = 1, .opcode = IBV_WR_SEND,
                             .send_flags = IBV_SEND_SIGNALED},
                       *bad = NULL;
    return ibv_post_send(e->qp, &wr, &bad);
}

static int post_recv(struct ep *e, void *ring, uint32_t slot, uint32_t lkey) {
    struct ibv_sge sge = {(uintptr_t)ring + (uint64_t)slot * LINES * 16, LINES * 16, lkey};
    struct ibv_recv_wr wr = {.wr_id = slot, .sg_list = &sge, .num_sge = 1}, *bad = NULL;
    return ibv_post_recv(e->qp, &wr, &bad);
}

/* Reap completions; with `want_recv`, wait until a receive completes. -1 on an error or after WAIT_NS. */
static int reap(struct ep *e, int want_recv) {
    struct ibv_wc wc[16];
    uint64_t began = link_now_ns();
    for (;;) {
        int n = ibv_poll_cq(e->cq, 16, wc), got = 0;
        for (int i = 0; i < n; ++i) {
            if (wc[i].status != IBV_WC_SUCCESS) return -1;
            got |= !(wc[i].wr_id & (1u << 31));
        }
        if (n < 0) return -1;
        if (!want_recv || got) return 0;
        if (link_now_ns() - began > WAIT_NS) return -1;
    }
}

static int run_nic(int sender, const char *device, int gid, const char *via, int port, uint32_t rounds) {
    struct ep e;
    struct xchg x;
    struct ibv_mr *mr = NULL;
    struct gpu g;
    void *ring = NULL;
    size_t bytes = (SLOTS + 1) * LINES * 16;
    uint64_t *t = calloc(rounds ? rounds : 1, sizeof(uint64_t));
    uint32_t missed = 0;
    if (!t || posix_memalign(&ring, 16384, bytes)) return 2;
    memset(ring, 0xff, bytes);
    if (nic_link(&e, &x, device, gid, via, port, ring, bytes, &mr)) return 1;
    uint32_t *out = (uint32_t *)ring + SLOTS * LINES * 4;
    if (sender) {
        /* send round r's packet, wait for the GPU side's answer, which lands in our ring */
        for (uint32_t r = 1; r <= rounds && !missed; ++r) {
            uint64_t began = link_now_ns();
            fill(out, r);
            if (post_send(&e, out, LINES * 16, mr->lkey) || reap(&e, 1)) missed++;
            t[r - 1] = link_now_ns() - began;
            if (post_recv(&e, ring, (r - 1) % SLOTS, mr->lkey)) missed++;
        }
        summary("nic-send", t, rounds, missed, NULL);
        return missed != 0;
    }
    if (gpu_open(&g, ring, bytes)) return 2;
    for (uint32_t first = 1; first <= rounds && !missed; first += BATCH) {
        uint32_t count = rounds - first + 1 < BATCH ? rounds - first + 1 : BATCH;
        id<MTLCommandBuffer> cb = gpu_launch(&g, first, count);
        for (uint32_t r = first; r < first + count && !missed; ++r) {
            /* the GPU sees the NIC's lines; the CPU only answers once the kernel has, and puts the receive back */
            uint64_t began = link_now_ns();
            missed += await_answer(&g, r) != 0;
            t[r - 1] = link_now_ns() - began;
            if (post_send(&e, out, 64, mr->lkey) || reap(&e, 0) || post_recv(&e, ring, (r - 1) % SLOTS, mr->lkey))
                missed++;
        }
        [cb waitUntilCompleted];
    }
    summary("nic-gpu", t, rounds, missed, &g);
    return missed || g.ctl_words[2] || g.ctl_words[3];
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    @autoreleasepool {
        if (argc == 3 && !strcmp(argv[1], "cpu")) return run_cpu((uint32_t)strtoul(argv[2], NULL, 10));
        if (argc == 7 && (!strcmp(argv[1], "nic-gpu") || !strcmp(argv[1], "nic-send")))
            return run_nic(!strcmp(argv[1], "nic-send"), argv[2], atoi(argv[3]), argv[4], atoi(argv[5]),
                           (uint32_t)strtoul(argv[6], NULL, 10));
    }
    fprintf(stderr, "usage: metal-poll cpu ROUNDS\n"
                    "       metal-poll nic-gpu|nic-send DEVICE GID_INDEX VIA PORT ROUNDS\n");
    return 2;
}

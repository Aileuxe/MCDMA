/* mcdma-rpcd: the verbs endpoint and ordered transfers; every object is torn down on every exit path. */
#include "rpcd.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#if defined(__linux__) && !defined(MCDMA_RPC_TEST_DMABUF)
#include <dlfcn.h>
#endif

static struct ep *g_eps[MAX_PEERS + 1];
static pthread_mutex_t g_eps_lock = PTHREAD_MUTEX_INITIALIZER;

static void ep_register(struct ep *e) {
    pthread_mutex_lock(&g_eps_lock);
    for (int i = 0; i <= MAX_PEERS; ++i)
        if (!g_eps[i]) {
            g_eps[i] = e;
            break;
        }
    pthread_mutex_unlock(&g_eps_lock);
}

struct ibv_mr *ep_reg(struct ep *e, void *addr, size_t len, int access) {
    if (e->cleanup_failed || e->nmr >= MAX_MRS) { errno = EBUSY; return NULL; }
    struct ibv_mr *mr = ibv_reg_mr(e->pd, addr, len, access);
    if (mr) e->mr[e->nmr++] = mr;
    return mr;
}

struct ibv_mr *ep_reg_dmabuf(struct ep *e, int fd, uint64_t offset, void *addr, size_t len, int access) {
#ifdef MCDMA_RPC_HAS_DMABUF
    if (e->cleanup_failed || e->nmr >= MAX_MRS) { errno = EBUSY; return NULL; }
    typedef struct ibv_mr *(*register_fn)(struct ibv_pd *, uint64_t, size_t, uint64_t, int, int);
#ifdef MCDMA_RPC_TEST_DMABUF
    /* No newer verbs declaration is required by the Apple SDK used for tests. */
    extern struct ibv_mr *ibv_reg_dmabuf_mr(struct ibv_pd *, uint64_t, size_t, uint64_t, int, int);
    register_fn register_buffer = ibv_reg_dmabuf_mr;
#else
    register_fn register_buffer = (register_fn)dlsym(RTLD_DEFAULT, "ibv_reg_dmabuf_mr");
    if (!register_buffer) {
        logf_("DMA-BUF registration is unavailable in the loaded verbs library; no fallback");
        errno = ENOSYS;
        return NULL;
    }
#endif
    struct ibv_mr *mr = register_buffer(e->pd, offset, len, (uintptr_t)addr, fd, access);
    if (mr) e->mr[e->nmr++] = mr;
    return mr;
#else
    (void)e; (void)fd; (void)offset; (void)addr; (void)len; (void)access;
    errno = ENOTSUP;
    return NULL;
#endif
}

static void cleanup_failed(struct ep *e, const char *operation, int error) {
    logf_("%s: %s failed (%d); retaining resources until process teardown", e->device, operation, error);
    e->cleanup_failed = 1;
    g_stop = 1;
}

/* Wait out whatever is still posted (bounded), so a QP is never destroyed with work in flight. */
static void ep_drain(struct ep *e, uint64_t budget_ns) {
    struct ibv_wc wc;
    uint64_t deadline = now_ns() + budget_ns;
    while (e->cq && e->outstanding > 0 && now_ns() < deadline) {
        int n = ibv_poll_cq(e->cq, 1, &wc);
        if (n < 0) break;
        if (n == 1) e->outstanding--;
    }
}

void ep_destroy_qp(struct ep *e) {
    if (e->cleanup_failed) return;
    if (e->qp) {
        ep_drain(e, 20000000000ull);   /* retries to a dead peer end with an error completion within ~15 s */
        struct ibv_qp_attr a = {.qp_state = IBV_QPS_ERR};
        int error = ibv_modify_qp(e->qp, &a, IBV_QP_STATE);
        if (error) { cleanup_failed(e, "QP error transition", error); return; }
        struct ibv_wc wc;
        for (int i = 0; i < 256 && e->cq && ibv_poll_cq(e->cq, 1, &wc) > 0; ++i) {
        }
        error = ibv_destroy_qp(e->qp);
        if (error) { cleanup_failed(e, "destroy QP", error); return; }
        e->qp = NULL;
        e->outstanding = 0;
    }
    if (e->cq) {
        int error = ibv_destroy_cq(e->cq);
        if (error) { cleanup_failed(e, "destroy CQ", error); return; }
        e->cq = NULL;
    }
}

static void ep_close(struct ep *e) {
    ep_destroy_qp(e);
    if (e->cleanup_failed) return;
    for (int i = 0; i < e->nmr; ++i)
        if (e->mr[i]) {
            int error = ibv_dereg_mr(e->mr[i]);
            if (error) { cleanup_failed(e, "deregister MR", error); return; }
            e->mr[i] = NULL;
        }
    e->nmr = 0;
    if (e->pd) {
        int error = ibv_dealloc_pd(e->pd);
        if (error) { cleanup_failed(e, "deallocate PD", error); return; }
        e->pd = NULL;
    }
    if (e->ctx) {
        int error = ibv_close_device(e->ctx);
        if (error) { cleanup_failed(e, "close verbs device", error); return; }
        e->ctx = NULL;
    }
}

void teardown_all(void) {
    int failed = 0;
    for (int i = 0; i <= MAX_PEERS; ++i)
        if (g_eps[i]) {
            ep_close(g_eps[i]);
            failed |= g_eps[i]->cleanup_failed;
        }
    if (failed) {
        /* Callers must not unmap/free a mailbox or close its retained DMA-BUF
         * after uncertain cleanup. Kernel process teardown owns these refs. */
        logf_("verbs cleanup incomplete; mailbox mappings and DMA-BUF references retained until exit");
        _exit(2);
    }
}

/* SIGINT and SIGTERM ask for the same orderly stop as SHUTDOWN; nothing is torn down inside the handler. */
void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

int ep_open(struct ep *e, const char *device, int gid_index, int mtu) {
    memset(e, 0, sizeof(*e));
    snprintf(e->device, sizeof(e->device), "%s", device);
    e->gid_index = gid_index;
    e->mtu = mtu == 4096 ? IBV_MTU_4096 : mtu == 2048 ? IBV_MTU_2048 : IBV_MTU_1024;
    ep_register(e);
    int n = 0;
    struct ibv_device **list = ibv_get_device_list(&n);
    if (!list) {
        logf_("no RDMA devices");
        return -1;
    }
    for (int i = 0; i < n; ++i)
        if (!strcmp(ibv_get_device_name(list[i]), device)) e->ctx = ibv_open_device(list[i]);
    ibv_free_device_list(list);
    if (!e->ctx) {
        logf_("cannot open %s", device);
        return -1;
    }
    struct ibv_port_attr port;
    if (ibv_query_port(e->ctx, 1, &port) || port.state != IBV_PORT_ACTIVE || e->mtu > port.active_mtu) {
        logf_("%s: port not active or path MTU above active MTU", device);
        return -1;
    }
    if (ibv_query_gid(e->ctx, 1, gid_index, &e->gid)) {
        logf_("%s: gid %d", device, gid_index);
        return -1;
    }
    if (!(e->pd = ibv_alloc_pd(e->ctx))) {
        logf_("%s: pd", device);
        return -1;
    }
    return 0;
}

int ep_create_qp(struct ep *e) {
    ep_destroy_qp(e);
    if (e->cleanup_failed) return -1;
    /* 31 entries: the MCDMA provider refuses 63 */
    if (!(e->cq = ibv_create_cq(e->ctx, 31, NULL, NULL, 0))) {
        logf_("%s: cq", e->device);
        return -1;
    }
    struct ibv_qp_init_attr init;
    memset(&init, 0, sizeof(init));
    init.send_cq = e->cq;
    init.recv_cq = e->cq;
    init.qp_type = IBV_QPT_RC;
    init.cap.max_send_wr = 31;
    init.cap.max_recv_wr = 1;
    init.cap.max_send_sge = 1;
    init.cap.max_recv_sge = 1;
    if (!(e->qp = ibv_create_qp(e->pd, &init))) {
        logf_("%s: qp", e->device);
        return -1;
    }
    struct ibv_qp_attr a;
    memset(&a, 0, sizeof(a));
    a.qp_state = IBV_QPS_INIT;
    a.port_num = 1;
    a.qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    if (ibv_modify_qp(e->qp, &a, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
        logf_("%s: INIT", e->device);
        return -1;
    }
    return 0;
}

int ep_connect(struct ep *e, unsigned qpn, unsigned psn, const char *gid) {
    struct ibv_qp_attr a;
    memset(&a, 0, sizeof(a));
    a.qp_state = IBV_QPS_RTR;
    a.path_mtu = e->mtu;
    a.dest_qp_num = qpn;
    a.rq_psn = psn;
    a.max_dest_rd_atomic = 1;
    a.min_rnr_timer = 12;
    a.ah_attr.is_global = 1;
    a.ah_attr.port_num = 1;
    a.ah_attr.grh.sgid_index = (uint8_t)e->gid_index;
    a.ah_attr.grh.hop_limit = 64;
    if (inet_pton(AF_INET6, gid, &a.ah_attr.grh.dgid) != 1) return -1;
    int err = ibv_modify_qp(e->qp, &a, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                                           IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
    if (err) {
        logf_("%s: RTR %d", e->device, err);
        return -1;
    }
    memset(&a, 0, sizeof(a));
    a.qp_state = IBV_QPS_RTS;
    a.timeout = 14;
    a.retry_cnt = 7;
    a.rnr_retry = 7;
    a.sq_psn = PSN;
    a.max_rd_atomic = 1;
    err = ibv_modify_qp(e->qp, &a, IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                                       IBV_QP_MAX_QP_RD_ATOMIC);
    if (err) {
        logf_("%s: RTS %d", e->device, err);
        return -1;
    }
    return 0;
}

void gid_string(const union ibv_gid *g, char *out, size_t n) { inet_ntop(AF_INET6, g, out, (socklen_t)n); }

/* Post the pieces in order as signaled WRITEs or READs, at most `window` outstanding, and wait for all of them.
   RC executes them in order at the target, so a word posted last lands after everything before it. */
int post_pieces(struct ep *e, enum ibv_wr_opcode op, const struct piece *p, int n, int window,
                       uint64_t timeout_ns) {
    int posted = 0, done = 0;
    struct ibv_wc wc;
    uint64_t start = now_ns();
    unsigned spins = 0;
    while (done < n) {
        while (posted < n && posted - done < window) {
            struct ibv_sge sge = {.addr = (uintptr_t)p[posted].local, .length = p[posted].len, .lkey = p[posted].lkey};
            struct ibv_send_wr wr, *bad = NULL;
            memset(&wr, 0, sizeof(wr));
            wr.wr_id = (uint64_t)posted;
            wr.sg_list = &sge;
            wr.num_sge = 1;
            wr.opcode = op;
            wr.send_flags = IBV_SEND_SIGNALED;
            wr.wr.rdma.remote_addr = p[posted].remote;
            wr.wr.rdma.rkey = p[posted].rkey;
            if (ibv_post_send(e->qp, &wr, &bad)) {
                logf_("%s: post failed", e->device);
                return -1;
            }
            e->outstanding++;
            posted++;
        }
        int got = ibv_poll_cq(e->cq, 1, &wc);
        if (got < 0) return -1;
        if (got == 1) {
            e->outstanding--;
            done++;
            if (wc.status != IBV_WC_SUCCESS) {
                logf_("%s: completion status %d", e->device, (int)wc.status);
                return -1;
            }
            continue;
        }
        if (++spins == 1024) {
            spins = 0;
            if (now_ns() - start > timeout_ns) {
                logf_("%s: completion timeout", e->device);
                return -1;
            }
        }
    }
    return 0;
}

uint32_t box_lkey(const struct box *b, uint64_t off) { return b->all ? b->all->lkey : b->seg[off / SEG]->lkey; }

/* Cut [off, off + len) of the local box into pieces that stay inside one local segment, one remote segment
   (when `rseg_rkey` is given: the peer's reply-half segments, `roff` is the offset inside that half) and `max`. */
int cut(const struct box *b, uint64_t off, uint64_t len, uint64_t remote_base, uint32_t rkey,
               const uint32_t *rseg_rkey, const uint64_t *rseg_addr, uint64_t roff, uint64_t max, struct piece *out,
               int cap) {
    int n = 0;
    while (len) {
        uint64_t take = len < max ? len : max;
        uint64_t local_room = SEG - (off % SEG);
        if (!b->all && take > local_room) take = local_room;
        if (rseg_rkey) {
            uint64_t remote_room = SEG - (roff % SEG);
            if (take > remote_room) take = remote_room;
        }
        if (n == cap) return -1;
        out[n].local = b->base + off;
        out[n].lkey = box_lkey(b, off);
        out[n].len = (uint32_t)take;
        if (rseg_rkey) {
            out[n].remote = rseg_addr[roff / SEG] + roff % SEG;
            out[n].rkey = rseg_rkey[roff / SEG];
        } else {
            out[n].remote = remote_base + off;
            out[n].rkey = rkey;
        }
        n++;
        off += take;
        roff += take;
        len -= take;
    }
    return n;
}

void write_sizes(struct box *b) {
    volatile uint64_t *w = (volatile uint64_t *)(b->base + 256);
    store_word(&w[0], b->req);
    store_word(&w[1], b->rep);
}

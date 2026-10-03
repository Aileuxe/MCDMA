/* Verbs endpoints for both link kinds: devices, segmented registrations, RoCE queue pairs and ordered transfers. */
#include "link.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/random.h>
#endif

uint64_t link_now_ns(void) {
#ifdef __APPLE__
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
#endif
}

void link_random(void *out, size_t n) {
#ifdef __APPLE__
    arc4random_buf(out, n);
#else
    unsigned char *p = out;
    size_t got = 0;
    while (got < n) {
        ssize_t r = getrandom(p + got, n - got, 0);
        if (r > 0) got += (size_t)r;
        else if (errno != EINTR) break;
    }
    int fd = got < n ? open("/dev/urandom", O_RDONLY | O_CLOEXEC) : -1;
    while (fd >= 0 && got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    if (fd >= 0) close(fd);
    /* without either source, sessions still differ by time and address; PSNs need no secrecy */
    for (uint64_t x = link_now_ns() ^ (uintptr_t)out; got < n; ++got) {
        x ^= x << 13, x ^= x >> 7, x ^= x << 17;
        p[got] = (unsigned char)x;
    }
#endif
}

const char *link_kind_name(int kind) { return kind == LINK_TB ? "thunderbolt" : "roce"; }

int ep_open(struct ep *e, const char *device, int gid_index, int mtu) {
    memset(e, 0, sizeof(*e));
    snprintf(e->device, sizeof(e->device), "%s", device);
    e->gid_index = gid_index;
    e->mtu = mtu == 4096 ? IBV_MTU_4096 : mtu == 2048 ? IBV_MTU_2048 : IBV_MTU_1024;
    int n = 0;
    struct ibv_device **list = ibv_get_device_list(&n);
    if (!list) {
        link_log("no RDMA devices");
        return -1;
    }
    for (int i = 0; i < n && !e->ctx; ++i)
        if (!strcmp(ibv_get_device_name(list[i]), device)) e->ctx = ibv_open_device(list[i]);
    ibv_free_device_list(list);
    if (!e->ctx) {
        link_log("cannot open %s", device);
        return -1;
    }
    struct ibv_port_attr port;
    memset(&port, 0, sizeof(port));
    if (ibv_query_port(e->ctx, 1, &port) || port.state != IBV_PORT_ACTIVE || e->mtu > port.active_mtu) {
        link_log("%s: port not active or path MTU above active MTU", device);
        return -1;
    }
    e->kind = port.link_layer == LINK_LAYER_TB ? LINK_TB : LINK_ROCE;
    e->lid = port.lid;
    if (ibv_query_gid(e->ctx, 1, gid_index, &e->gid)) {
        link_log("%s: gid %d", device, gid_index);
        return -1;
    }
    if (!(e->pd = ibv_alloc_pd(e->ctx))) {
        link_log("%s: pd", device);
        return -1;
    }
    return 0;
}

struct ibv_mr *ep_reg(struct ep *e, void *addr, size_t len, int access) {
    if (e->nmr >= LINK_MRS) return NULL;
    struct ibv_mr *mr = ibv_reg_mr(e->pd, addr, len, access);
    if (mr) e->mr[e->nmr++] = mr;
    return mr;
}

/* Register [base, base + length) in pieces of `seg` bytes (0: one piece); Thunderbolt allows only local access. */
int ep_reg_region(struct ep *e, struct region *r, void *base, uint64_t length, uint64_t seg, uint64_t split,
                  int below, int above) {
    memset(r, 0, sizeof(*r));
    r->base = base, r->length = length, r->seg = seg ? seg : length, r->split = split;
    r->below = e->kind == LINK_TB ? IBV_ACCESS_LOCAL_WRITE : below;
    r->above = e->kind == LINK_TB ? IBV_ACCESS_LOCAL_WRITE : above;
    for (uint64_t off = 0; off < length; off += r->seg) {
        uint64_t len = length - off < r->seg ? length - off : r->seg;
        if (r->n == LINK_REGION_MAX || !(r->mr[r->n] = ep_reg(e, r->base + off, len, off < split ? r->below : r->above))) {
            link_log("%s: registering %llu bytes at offset %llu failed (errno %d)", e->device, (unsigned long long)len,
                     (unsigned long long)off, errno);
            return -1;
        }
        r->n++;
    }
    return 0;
}

uint32_t region_lkey(const struct region *r, uint64_t off) { return r->mr[off / r->seg]->lkey; }

int ep_create_qp(struct ep *e) {
    ep_destroy_qp(e);
    link_random(&e->psn, sizeof(e->psn));
    e->psn &= 0xffffff;
    if (e->kind == LINK_TB) return tb_init(e);
    if (!(e->cq = ibv_create_cq(e->ctx, LINK_RC_DEPTH, NULL, NULL, 0))) {
        link_log("%s: cq", e->device);
        return -1;
    }
    struct ibv_qp_init_attr init;
    memset(&init, 0, sizeof(init));
    init.send_cq = init.recv_cq = e->cq;
    init.qp_type = IBV_QPT_RC;
    init.cap.max_send_wr = LINK_RC_DEPTH;
    init.cap.max_recv_wr = 1;
    init.cap.max_send_sge = init.cap.max_recv_sge = 1;
    if (!(e->qp = ibv_create_qp(e->pd, &init))) {
        link_log("%s: qp", e->device);
        return -1;
    }
    struct ibv_qp_attr a;
    memset(&a, 0, sizeof(a));
    a.qp_state = IBV_QPS_INIT;
    a.port_num = 1;
    a.qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    if (ibv_modify_qp(e->qp, &a, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
        link_log("%s: INIT", e->device);
        return -1;
    }
    return 0;
}

int ep_connect(struct ep *e, const struct xinfo *peer) {
    if (peer->transport != e->kind) {
        link_log("%s: the peer's link is %s, this one is %s", e->device, link_kind_name(peer->transport),
                 link_kind_name(e->kind));
        return -1;
    }
    if (e->kind == LINK_TB) return tb_connect(e, peer);
    struct ibv_qp_attr a;
    memset(&a, 0, sizeof(a));
    a.qp_state = IBV_QPS_RTR;
    a.path_mtu = e->mtu;
    a.dest_qp_num = peer->qpn;
    a.rq_psn = peer->psn;
    a.max_dest_rd_atomic = 1;
    a.min_rnr_timer = 12;
    a.ah_attr.is_global = 1;
    a.ah_attr.port_num = 1;
    a.ah_attr.grh.sgid_index = (uint8_t)e->gid_index;
    a.ah_attr.grh.hop_limit = 64;
    memcpy(a.ah_attr.grh.dgid.raw, peer->gid, 16);
    int err = ibv_modify_qp(e->qp, &a, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                                           IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
    if (err) {
        link_log("%s: RTR %d", e->device, err);
        return -1;
    }
    memset(&a, 0, sizeof(a));
    a.qp_state = IBV_QPS_RTS;
    a.timeout = 14;
    a.retry_cnt = 7;
    a.rnr_retry = 7;
    a.sq_psn = e->psn;
    a.max_rd_atomic = 1;
    err = ibv_modify_qp(e->qp, &a, IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                                       IBV_QP_MAX_QP_RD_ATOMIC);
    if (err) {
        link_log("%s: RTS %d", e->device, err);
        return -1;
    }
    return 0;
}

/* Wait out whatever is still posted (bounded), so a RoCE QP is never destroyed with work in flight. */
static void ep_drain(struct ep *e, uint64_t budget_ns) {
    struct ibv_wc wc;
    uint64_t deadline = link_now_ns() + budget_ns;
    while (e->cq && e->outstanding > 0 && link_now_ns() < deadline) {
        int n = ibv_poll_cq(e->cq, 1, &wc);
        if (n < 0) break;
        if (n == 1) e->outstanding--;
    }
}

void ep_destroy_qp(struct ep *e) {
    if (e->tb) {
        tb_destroy(e);
        return;
    }
    if (e->qp) {
        ep_drain(e, 20000000000ull);   /* retries to a dead peer end with an error completion within ~15 s */
        struct ibv_qp_attr a = {.qp_state = IBV_QPS_ERR};
        ibv_modify_qp(e->qp, &a, IBV_QP_STATE);
        struct ibv_wc wc;
        for (int i = 0; i < 256 && e->cq && ibv_poll_cq(e->cq, 1, &wc) > 0; ++i) {
        }
        if (ibv_destroy_qp(e->qp)) link_log("%s: destroy qp failed", e->device);
        e->qp = NULL;
        e->outstanding = 0;
    }
    if (e->cq) {
        if (ibv_destroy_cq(e->cq)) link_log("%s: destroy cq failed", e->device);
        e->cq = NULL;
    }
}

void ep_close(struct ep *e) {
    ep_destroy_qp(e);
    for (int i = 0; i < e->nmr; ++i)
        if (e->mr[i]) {
            ibv_dereg_mr(e->mr[i]);
            e->mr[i] = NULL;
        }
    e->nmr = 0;
    if (e->pd) {
        ibv_dealloc_pd(e->pd);
        e->pd = NULL;
    }
    if (e->ctx) {
        ibv_close_device(e->ctx);
        e->ctx = NULL;
    }
}

/* This side's offer: queue pairs, address and the region the peer will address. */
void ep_info(const struct ep *e, const struct region *r, struct xinfo *out) {
    memset(out, 0, sizeof(*out));
    out->transport = (uint8_t)e->kind;
    out->lid = e->lid;
    out->qpn = e->qp ? e->qp->qp_num : 0;
    out->psn = e->psn;
    memcpy(out->gid, e->gid.raw, 16);
    if (e->kind == LINK_TB) tb_info(e, out);
    out->table.base = (uint64_t)(uintptr_t)r->base;
    out->table.length = r->length;
    out->table.seg = r->seg;
    if (e->kind == LINK_TB) return;
    out->table.n = (uint32_t)r->n;
    for (int i = 0; i < r->n; ++i) {
        int access = (uint64_t)i * r->seg < r->split ? r->below : r->above;
        /* a key the peer cannot use is not handed out */
        out->table.rkey[i] = access & (IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ) ? r->mr[i]->rkey : 0;
    }
}

/* A peer table describes `need` bytes in whole registrations; RoCE tables must carry a key for every one. */
int table_valid(const struct table *t, uint64_t need, int keys) {
    if (!t->seg || t->length < need || t->n > LINK_REGION_MAX) return 0;
    return !keys || (t->n && t->n >= (t->length + t->seg - 1) / t->seg);
}

int ep_post(struct ep *e, enum ibv_wr_opcode op, const struct piece *p) {
    struct ibv_sge sge = {.addr = (uintptr_t)p->local, .length = p->len, .lkey = p->lkey};
    struct ibv_send_wr wr, *bad = NULL;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = (uint64_t)e->outstanding;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = op;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = p->remote;
    wr.wr.rdma.rkey = p->rkey;
    if (ibv_post_send(e->qp, &wr, &bad)) {
        link_log("%s: post failed", e->device);
        return -1;
    }
    e->outstanding++;
    return 0;
}

/* Reap at least `want` completions within timeout_ns (0: only those ready); the count, or -1 on a failed one. */
int ep_reap(struct ep *e, int want, uint64_t timeout_ns) {
    struct ibv_wc wc;
    uint64_t start = link_now_ns();
    unsigned spins = 0;
    for (int got = 0;;) {
        int n = e->outstanding > 0 ? ibv_poll_cq(e->cq, 1, &wc) : 0;
        if (n < 0) return -1;
        if (n == 1) {
            e->outstanding--;
            if (wc.status != IBV_WC_SUCCESS) {
                link_log("%s: completion status %d", e->device, (int)wc.status);
                return -1;
            }
            got++;
            continue;
        }
        if (got >= want) return got;
        if (++spins == 1024) {
            spins = 0;
            if (link_now_ns() - start > timeout_ns) {
                link_log("%s: completion timeout", e->device);
                return -1;
            }
        }
    }
}

/* Post the pieces in order as signaled WRITEs or READs, at most `window` outstanding, and wait for all of them.
   RC executes them in order at the target, so a word posted last lands after everything before it. */
int post_pieces(struct ep *e, enum ibv_wr_opcode op, const struct piece *p, int n, int window, uint64_t timeout_ns) {
    for (int posted = 0; posted < n;) {
        while (posted < n && e->outstanding < window)
            if (ep_post(e, op, &p[posted++])) return -1;
        if (ep_reap(e, 1, timeout_ns) < 0) return -1;
    }
    return ep_reap(e, e->outstanding, timeout_ns) < 0 ? -1 : 0;
}

/* Cut [off, off + len) of the local region into pieces that stay inside one local registration, one remote one
   (when `remote` is given; `roff` is the matching offset in it) and `max` bytes. */
int cut(const struct region *local, uint64_t off, uint64_t len, const struct table *remote, uint64_t roff,
        uint64_t max, struct piece *out, int cap) {
    int n = 0;
    while (len) {
        uint64_t take = len < max ? len : max, lroom = local->seg - off % local->seg;
        take = take < lroom ? take : lroom;
        if (remote) {
            uint64_t rroom = remote->seg - roff % remote->seg;
            take = take < rroom ? take : rroom;
        }
        if (n == cap) return -1;
        out[n].local = local->base + off;
        out[n].lkey = region_lkey(local, off);
        out[n].len = (uint32_t)take;
        out[n].remote = remote ? remote->base + roff : 0;
        out[n].rkey = remote ? remote->rkey[roff / remote->seg] : 0;
        n++, off += take, roff += take, len -= take;
    }
    return n;
}

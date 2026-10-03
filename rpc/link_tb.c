/* One-sided writes over Apple Thunderbolt RDMA, which offers only SEND and RECV on UC queue pairs (TN3205).
 * A write sends a one-frame control message naming its destination, then its bytes on a data queue pair; the receiver
 * posts a receive of exactly that length at the destination, so the bytes land in place. Small writes, signals and
 * fences travel inside control messages. Control effects apply in arrival order, each only after every earlier data
 * receive has landed, so a signal lands after the writes posted before it, as RC ordering gives on RoCE. */
#include "link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CTL_MAGIC 0x4254434du     /* "MCTB" */
#define CTL_HEAD 32
#define TB_INLINE (TB_FRAME - CTL_HEAD)
#define TAG(kind, index) (((uint64_t)(kind) << 56) | (uint64_t)(index))
enum { C_WRITE = 1, C_INLINE, C_SIGNAL, C_FENCE, C_FENCE_ACK };
enum { T_CTL_TX = 1, T_CTL_RX, T_DATA_TX, T_DATA_RX };

struct ctl {
    uint32_t magic, seq;
    uint16_t kind, pad;
    uint32_t len;
    uint64_t off, value;
};

struct entry {
    uint16_t kind, slot;
    uint32_t len;
    uint64_t off, value;
    uint64_t recv;                /* data receive count once this write's receive is posted */
};

struct tb {
    struct ibv_qp *data;
    unsigned char *area;          /* TB_SLOTS receive slots, then TB_SLOTS send slots, one frame each */
    struct ibv_mr *area_mr;
    uint32_t send_frames, recv_frames, peer_frames, slots;
    uint32_t tx_seq, rx_seq, fences, fence_acked;
    uint64_t ctl_posted, ctl_done, data_posted, data_done;
    uint32_t data_frames;
    uint16_t sent_frames[TB_DATA_WR];
    struct entry fifo[TB_SLOTS];  /* control messages in arrival order, until they take effect */
    uint64_t head, post, tail;
    uint64_t rx_posted, rx_done;
    uint32_t rx_frames;
    uint32_t rx_len[TB_DATA_WR];
    const struct region *rx;      /* the peer may place bytes in [lo, hi) of this region */
    uint64_t lo, hi;
    int poisoned;
};

static uint32_t frames(uint64_t len) { return len ? (uint32_t)((len + TB_FRAME - 1) / TB_FRAME) : 1; }

static void poison(struct ep *e, const char *why) {
    if (!e->tb->poisoned) link_log("%s: thunderbolt link failed: %s", e->device, why);
    e->tb->poisoned = 1;
}

static int post_recv(struct ibv_qp *qp, uint64_t id, void *addr, uint32_t len, uint32_t lkey) {
    struct ibv_sge sge = {.addr = (uintptr_t)addr, .length = len, .lkey = lkey};
    struct ibv_recv_wr wr, *bad = NULL;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = id, wr.sg_list = &sge, wr.num_sge = 1;
    return ibv_post_recv(qp, &wr, &bad);
}

static int post_send(struct ibv_qp *qp, uint64_t id, void *addr, uint32_t len, uint32_t lkey) {
    struct ibv_sge sge = {.addr = (uintptr_t)addr, .length = len, .lkey = lkey};
    struct ibv_send_wr wr, *bad = NULL;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = id, wr.sg_list = &sge, wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;
    return ibv_post_send(qp, &wr, &bad);
}

static struct ibv_qp *uc_qp(struct ep *e, uint32_t depth) {
    struct ibv_qp_init_attr init;
    memset(&init, 0, sizeof(init));
    init.send_cq = init.recv_cq = e->cq;
    init.qp_type = IBV_QPT_UC;
    init.cap.max_send_wr = init.cap.max_recv_wr = depth;
    init.cap.max_send_sge = init.cap.max_recv_sge = 1;
    struct ibv_qp *qp = ibv_create_qp(e->pd, &init);
    if (!qp) return NULL;
    struct ibv_qp_attr a;
    memset(&a, 0, sizeof(a));
    a.qp_state = IBV_QPS_INIT;
    a.pkey_index = 0;
    a.port_num = 1;
    a.qp_access_flags = 0;
    if (ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
        ibv_destroy_qp(qp);
        return NULL;
    }
    return qp;
}

/* The depths the device granted, which may be less than asked (TN3205). */
static int granted(struct ibv_qp *qp, uint32_t *send, uint32_t *recv) {
    struct ibv_qp_attr a;
    struct ibv_qp_init_attr init;
    memset(&a, 0, sizeof(a));
    memset(&init, 0, sizeof(init));
    if (ibv_query_qp(qp, &a, IBV_QP_CAP, &init)) return -1;
    *send = a.cap.max_send_wr, *recv = a.cap.max_recv_wr;
    return *send && *recv ? 0 : -1;
}

static int repost(struct ep *e, uint16_t slot) {
    struct tb *t = e->tb;
    if (!post_recv(e->qp, TAG(T_CTL_RX, slot), t->area + (size_t)slot * TB_FRAME, TB_FRAME, t->area_mr->lkey))
        return 0;
    poison(e, "posting a control receive failed");
    return -1;
}

int tb_init(struct ep *e) {
    struct tb *t = calloc(1, sizeof(*t));
    void *area = NULL;
    if (!t || posix_memalign(&area, 16384, 2 * TB_SLOTS * TB_FRAME)) {
        free(t);
        return -1;
    }
    memset(area, 0, 2 * TB_SLOTS * TB_FRAME);
    t->area = area;
    e->tb = t;
    uint32_t ctl_send = 0, ctl_recv = 0;
    if (!(t->area_mr = ibv_reg_mr(e->pd, area, 2 * TB_SLOTS * TB_FRAME, IBV_ACCESS_LOCAL_WRITE)) ||
        !(e->cq = ibv_create_cq(e->ctx, 2 * TB_SLOTS + 2 * TB_DATA_WR + 8, NULL, NULL, 0)) ||
        !(e->qp = uc_qp(e, TB_SLOTS)) || !(t->data = uc_qp(e, TB_DEPTH)) || granted(e->qp, &ctl_send, &ctl_recv) ||
        granted(t->data, &t->send_frames, &t->recv_frames) || ctl_send < 4 || ctl_recv < 4) {
        link_log("%s: cannot set up thunderbolt queue pairs (registration, queue or depth refused)", e->device);
        tb_destroy(e);
        return -1;
    }
    /* the control ring is as deep as the device allows, up to TB_SLOTS */
    t->slots = ctl_send < ctl_recv ? ctl_send : ctl_recv;
    t->slots = t->slots < TB_SLOTS ? t->slots : TB_SLOTS;
    for (uint16_t slot = 0; slot < t->slots; ++slot)
        if (repost(e, slot)) {
            tb_destroy(e);
            return -1;
        }
    return 0;
}

static int rtr(struct ep *e, struct ibv_qp *qp, uint32_t qpn, const struct xinfo *peer) {
    struct ibv_qp_attr a;
    memset(&a, 0, sizeof(a));
    a.qp_state = IBV_QPS_RTR;
    a.path_mtu = e->mtu;
    a.rq_psn = peer->psn;
    a.dest_qp_num = qpn;
    a.ah_attr.dlid = peer->lid;
    a.ah_attr.port_num = 1;
    a.ah_attr.is_global = 1;
    a.ah_attr.grh.hop_limit = 1;
    a.ah_attr.grh.sgid_index = (uint8_t)e->gid_index;
    memcpy(a.ah_attr.grh.dgid.raw, peer->gid, 16);
    return ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN);
}

static int rts(struct ep *e, struct ibv_qp *qp) {
    struct ibv_qp_attr a;
    memset(&a, 0, sizeof(a));
    a.qp_state = IBV_QPS_RTS;
    a.sq_psn = e->psn;
    return ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_SQ_PSN);
}

int tb_connect(struct ep *e, const struct xinfo *peer) {
    struct tb *t = e->tb;
    if (!t || !peer->frames || rtr(e, e->qp, peer->qpn, peer) || rtr(e, t->data, peer->qpn2, peer) ||
        rts(e, e->qp) || rts(e, t->data)) {
        link_log("%s: thunderbolt queue pairs refused RTR or RTS", e->device);
        return -1;
    }
    t->peer_frames = peer->frames;
    return 0;
}

void tb_info(const struct ep *e, struct xinfo *out) {
    out->qpn2 = e->tb ? e->tb->data->qp_num : 0;
    out->frames = e->tb ? e->tb->recv_frames : 0;
}

/* Error state flushes every posted request; then nothing is destroyed with work in flight. */
void tb_destroy(struct ep *e) {
    struct tb *t = e->tb;
    if (!t) return;
    struct ibv_qp_attr err = {.qp_state = IBV_QPS_ERR};
    if (e->qp) ibv_modify_qp(e->qp, &err, IBV_QP_STATE);
    if (t->data) ibv_modify_qp(t->data, &err, IBV_QP_STATE);
    struct ibv_wc wc[64];
    uint64_t deadline = link_now_ns() + 200000000ull;
    for (int quiet = 0; e->cq && quiet < 3 && link_now_ns() < deadline;) {
        int n = ibv_poll_cq(e->cq, 64, wc);
        quiet = n > 0 ? 0 : quiet + 1;
        if (n < 0) break;
    }
    int failed = t->data && ibv_destroy_qp(t->data);
    failed = (e->qp && ibv_destroy_qp(e->qp)) || failed;
    failed = (e->cq && ibv_destroy_cq(e->cq)) || failed;
    /* a registration that would not go away, or a queue that would not die, keeps its memory: leak it */
    if (failed || (t->area_mr && ibv_dereg_mr(t->area_mr)))
        link_log("%s: thunderbolt teardown failed; keeping its memory", e->device);
    else free(t->area);
    free(t);
    e->qp = NULL, e->cq = NULL, e->tb = NULL;
}

void tb_accept(struct ep *e, const struct region *rx, uint64_t lo, uint64_t hi) {
    e->tb->rx = rx;
    e->tb->lo = lo;
    e->tb->hi = hi < rx->length ? hi : rx->length;
}

/* 1 sent, 0 no control slot free yet, -1 failed. */
static int send_ctl(struct ep *e, uint16_t kind, uint64_t off, uint32_t len, uint64_t value, const void *bytes) {
    struct tb *t = e->tb;
    if (t->poisoned) return -1;
    if (t->ctl_posted - t->ctl_done >= t->slots) return 0;
    uint32_t slot = (uint32_t)(t->ctl_posted % t->slots);
    unsigned char *buf = t->area + (size_t)(TB_SLOTS + slot) * TB_FRAME;
    struct ctl c = {CTL_MAGIC, t->tx_seq, kind, 0, len, off, value};
    memcpy(buf, &c, sizeof(c));
    if (bytes) memcpy(buf + CTL_HEAD, bytes, len);
    /* always a whole frame, so every receive matches its send byte for byte */
    if (post_send(e->qp, TAG(T_CTL_TX, slot), buf, TB_FRAME, t->area_mr->lkey)) {
        poison(e, "posting a control send failed");
        return -1;
    }
    t->tx_seq++, t->ctl_posted++;
    return 1;
}

static int data_room(const struct tb *t, uint32_t f) {
    return t->data_posted - t->data_done < TB_DATA_WR && t->data_frames + f <= t->send_frames;
}

static uint64_t msg_max(const struct tb *t) {
    uint64_t f = t->send_frames / 2;
    if (t->peer_frames / 2 < f) f = t->peer_frames / 2;
    uint64_t max = f * TB_FRAME;
    return max > TB_MSG ? TB_MSG : max < TB_FRAME ? TB_FRAME : max;
}

/* Progress until there is room for one control message and, unless `f` is 0, a data message of `f` frames. */
static int room(struct ep *e, uint32_t f, uint64_t deadline) {
    struct tb *t = e->tb;
    for (;;) {
        if (t->poisoned) return -1;
        if (t->ctl_posted - t->ctl_done < t->slots && (!f || data_room(t, f))) return 0;
        if (tb_progress(e) < 0) return -1;
        if (link_now_ns() > deadline) {
            poison(e, "no room to send within the timeout: the peer stopped taking messages");
            return -1;
        }
    }
}

int tb_write(struct ep *e, const struct region *src, uint64_t off, uint64_t roff, uint64_t len, uint64_t rseg,
             uint64_t timeout_ns) {
    struct tb *t = e->tb;
    uint64_t deadline = link_now_ns() + timeout_ns, max = msg_max(t);
    if (!rseg || off + len > src->length) return -1;
    while (len) {
        uint64_t take = len < max ? len : max, lroom = src->seg - off % src->seg, rroom = rseg - roff % rseg;
        take = take < lroom ? take : lroom;
        take = take < rroom ? take : rroom;
        int inline_bytes = take <= TB_INLINE;
        if (room(e, inline_bytes ? 0 : frames(take), deadline)) return -1;
        if (inline_bytes) {
            if (send_ctl(e, C_INLINE, roff, (uint32_t)take, 0, src->base + off) != 1) return -1;
        } else {
            uint32_t i = (uint32_t)(t->data_posted % TB_DATA_WR), f = frames(take);
            if (send_ctl(e, C_WRITE, roff, (uint32_t)take, 0, NULL) != 1) return -1;
            if (post_send(t->data, TAG(T_DATA_TX, i), src->base + off, (uint32_t)take, region_lkey(src, off))) {
                poison(e, "posting a data send failed");
                return -1;
            }
            t->sent_frames[i] = (uint16_t)f, t->data_frames += f, t->data_posted++;
        }
        off += take, roff += take, len -= take;
    }
    return 0;
}

int tb_signal(struct ep *e, uint64_t roff, uint64_t value, uint64_t timeout_ns) {
    if (room(e, 0, link_now_ns() + timeout_ns)) return -1;
    return send_ctl(e, C_SIGNAL, roff, 8, value, NULL) == 1 ? 0 : -1;
}

/* Returns once the peer has applied every write and signal posted before this call. */
int tb_fence(struct ep *e, uint64_t timeout_ns) {
    struct tb *t = e->tb;
    uint64_t deadline = link_now_ns() + timeout_ns;
    uint32_t mine = ++t->fences;
    if (room(e, 0, deadline) || send_ctl(e, C_FENCE, 0, 0, mine, NULL) != 1) return -1;
    while ((int32_t)(t->fence_acked - mine) < 0) {
        if (tb_progress(e) < 0) return -1;
        if (link_now_ns() > deadline) {
            poison(e, "the peer did not acknowledge a fence within the timeout");
            return -1;
        }
    }
    return 0;
}

static const char *refuse(const struct tb *t, const struct ctl *c) {
    if (!t->rx) return "the peer wrote before this end accepted writes";
    uint64_t span = c->kind == C_SIGNAL ? 8 : c->len;
    if (c->kind == C_FENCE) return NULL;
    if (c->kind == C_WRITE && (!c->len || c->len > TB_MSG)) return "a data message of an impossible size";
    if (c->kind == C_INLINE && (!c->len || c->len > TB_INLINE)) return "an inline write of an impossible size";
    if (c->kind == C_SIGNAL && c->off % 8) return "an unaligned signal";
    if (c->kind < C_WRITE || c->kind > C_SIGNAL) return "an unknown control message";
    if (c->off < t->lo || c->off > t->hi || span > t->hi - c->off) return "a write outside what the peer may write";
    if (c->kind == C_WRITE && c->off / t->rx->seg != (c->off + c->len - 1) / t->rx->seg)
        return "a data message spanning two registrations";
    return NULL;
}

static void arrive(struct ep *e, uint64_t slot, uint32_t byte_len) {
    struct tb *t = e->tb;
    struct ctl c;
    if (slot >= t->slots) {
        poison(e, "a control completion for no slot");
        return;
    }
    memcpy(&c, t->area + slot * TB_FRAME, sizeof(c));
    const char *why = byte_len != TB_FRAME || c.magic != CTL_MAGIC ? "a malformed control message"
                      : c.seq != t->rx_seq                       ? "a control message was lost or reordered"
                                                                 : NULL;
    if (why) {
        poison(e, why);
        return;
    }
    t->rx_seq++;
    if (c.kind == C_FENCE_ACK) {
        t->fence_acked = (uint32_t)c.value;
        repost(e, (uint16_t)slot);
        return;
    }
    if ((why = refuse(t, &c))) {
        poison(e, why);
        return;
    }
    struct entry *n = &t->fifo[t->tail++ % TB_SLOTS];
    *n = (struct entry){c.kind, (uint16_t)slot, c.len, c.off, c.value, 0};
}

static void complete(struct ep *e, const struct ibv_wc *wc) {
    struct tb *t = e->tb;
    unsigned kind = (unsigned)(wc->wr_id >> 56);
    uint64_t index = wc->wr_id & ((1ull << 56) - 1);
    if (wc->status != IBV_WC_SUCCESS) {
        char why[96];
        snprintf(why, sizeof(why), "completion status %d (%s)", (int)wc->status, ibv_wc_status_str(wc->status));
        poison(e, why);
        return;
    }
    if (kind == T_CTL_TX) {
        t->ctl_done++;
    } else if (kind == T_DATA_TX) {
        t->data_frames -= t->sent_frames[t->data_done++ % TB_DATA_WR];
    } else if (kind == T_CTL_RX) {
        arrive(e, index, wc->byte_len);
    } else if (kind == T_DATA_RX && index == t->rx_done % TB_DATA_WR) {
        uint32_t want = t->rx_len[index];
        if (wc->byte_len != want) {
            poison(e, "a data message's length differs from its announcement");
            return;
        }
        t->rx_frames -= frames(want);
        t->rx_done++;
    } else {
        poison(e, "a completion this link never posted");
    }
}

/* 1 when [off, off + len) overlaps an inline write or signal still waiting to apply. */
static int overlaps_pending(const struct tb *t, uint64_t off, uint64_t len) {
    for (uint64_t k = t->head; k < t->post; ++k) {
        const struct entry *n = &t->fifo[k % TB_SLOTS];
        uint64_t span = n->kind == C_SIGNAL ? 8 : n->kind == C_INLINE ? n->len : 0;
        if (span && n->off < off + len && off < n->off + span) return 1;
    }
    return 0;
}

static void drain(struct ep *e) {
    struct tb *t = e->tb;
    /* post data receives in announcement order, but never past a pending effect on the same bytes */
    while (t->post < t->tail) {
        struct entry *n = &t->fifo[t->post % TB_SLOTS];
        if (n->kind == C_WRITE) {
            uint32_t f = frames(n->len), i = (uint32_t)(t->rx_posted % TB_DATA_WR);
            if (t->rx_posted - t->rx_done >= TB_DATA_WR || t->rx_frames + f > t->recv_frames ||
                overlaps_pending(t, n->off, n->len))
                break;
            if (post_recv(t->data, TAG(T_DATA_RX, i), t->rx->base + n->off, n->len, region_lkey(t->rx, n->off))) {
                poison(e, "posting a data receive failed");
                return;
            }
            t->rx_len[i] = n->len, t->rx_frames += f;
            n->recv = ++t->rx_posted;
        }
        t->post++;
    }
    /* then take effect in order, each after every earlier data receive has landed */
    while (t->head < t->post) {
        struct entry *n = &t->fifo[t->head % TB_SLOTS];
        if (n->kind == C_WRITE && t->rx_done < n->recv) break;
        if (n->kind == C_INLINE)
            memcpy(t->rx->base + n->off, t->area + (size_t)n->slot * TB_FRAME + CTL_HEAD, n->len);
        if (n->kind == C_SIGNAL) __atomic_store_n((uint64_t *)(void *)(t->rx->base + n->off), n->value, __ATOMIC_RELEASE);
        if (n->kind == C_FENCE) {
            int sent = send_ctl(e, C_FENCE_ACK, 0, 0, n->value, NULL);
            if (sent <= 0) return;
        }
        if (repost(e, n->slot)) return;
        t->head++;
    }
}

/* Handle what has completed and apply what can take effect: the number of completions, or -1 once failed. */
int tb_progress(struct ep *e) {
    struct tb *t = e->tb;
    if (!t) return -1;
    struct ibv_wc wc[32];
    int handled = 0;
    for (int round = 0; round < 8 && !t->poisoned; ++round) {
        int n = ibv_poll_cq(e->cq, 32, wc);
        if (n < 0) poison(e, "polling the completion queue failed");
        for (int i = 0; i < n && !t->poisoned; ++i) complete(e, &wc[i]);
        handled += n > 0 ? n : 0;
        if (n < 32) break;
    }
    if (!t->poisoned) drain(e);
    return t->poisoned ? -1 : handled;
}

int tb_busy(const struct ep *e) {
    const struct tb *t = e->tb;
    return t && (t->ctl_posted != t->ctl_done || t->data_posted != t->data_done || t->head != t->tail ||
                 t->rx_posted != t->rx_done);
}

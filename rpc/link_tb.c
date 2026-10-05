/* One-sided writes over Apple Thunderbolt RDMA, as qualified on macOS 27.0: UC queue pairs, three a device, carry
 * only SENDs; each SEND is cut into 4 KiB packets that fill the receiver's posted receives in order, and every packet
 * but a message's last completes with IBV_WC_LOC_LEN_ERR. One queue pair a link. The receiver keeps a ring of
 * one-packet receives posted, so every message lands there without waiting on the receiver. A write is a header
 * message, then its bytes as a message of their own, which the receiver copies into place; small writes ride inside
 * the header. Messages take effect in arrival order, so a signal lands after the writes before it, as on RoCE. A write
 * and its signal can also travel as one message: head, signal offset, bytes. */
#include "link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAGIC 0x4254434du         /* "MCTB" */
#define HEAD 32
#define INLINE_MAX (TB_PACKET - HEAD)
#define TAG(kind, index) (((uint64_t)(kind) << 56) | (uint64_t)(index))
enum { C_WRITE = 1, C_INLINE, C_SIGNAL, C_FENCE, C_FENCE_ACK, C_WRITE_SIGNAL, C_BOND_SIGNAL, C_BOND_WRITE_SIGNAL };
#define WS_HEAD 48                /* a write-and-signal message's head and signal offset, before its bytes */
#define BOND_SIGNAL_HEAD 56
#define BOND_WS_HEAD 64
enum { T_SEND = 1, T_RECV };

struct head {
    uint32_t magic, seq;
    uint16_t kind, pad;
    uint32_t len;
    uint64_t off, value;
};
_Static_assert(sizeof(struct head) == HEAD, "Thunderbolt wire header size");

struct tb {
    unsigned char *ring;          /* TB_RING receive slots, then TB_STAGE send slots, one packet each */
    struct ibv_mr *ring_mr;
    uint32_t slots, depth, peer_slots;
    /* sending: posted and completed sends, packets in flight, header staging slots in use */
    uint32_t tx_seq, packets, fences, fence_acked;
    uint64_t tx_posted, tx_done, staged, unstaged;
    uint16_t tx_packets[TB_SEND_WR];
    uint8_t tx_staged[TB_SEND_WR];
    uint32_t tx_bytes[TB_SEND_WR];
    uint32_t tx_payload[TB_SEND_WR];
    uint64_t sent_bytes, completed_bytes, writes_posted, writes_placed;
    uint64_t sent_payload, completed_payload;
    void *bond_arg;
    void (*bond_placed)(void *arg, uint64_t count);
    int (*bond_signal)(void *arg, uint64_t seq, const uint64_t need[2], uint64_t off, uint64_t value);
    /* receiving: slots whose completion arrived, slots consumed, slots posted */
    uint64_t filled, used, posted;
    uint16_t rx_len[TB_RING];
    uint8_t rx_last[TB_RING];
    uint32_t rx_seq, want_len;    /* want_len: a WRITE header was taken and its bytes are the next message */
    uint64_t want_off;
    const struct region *rx;      /* the peer may write [lo, hi) of this region */
    uint64_t lo, hi;
    int poisoned;
};

static uint32_t packets(uint64_t len) { return (uint32_t)((len + TB_PACKET - 1) / TB_PACKET); }

static void poison(struct ep *e, const char *why) {
    if (!e->tb->poisoned) link_log("%s: thunderbolt link failed: %s", e->device, why);
    e->tb->poisoned = 1;
}

static int post_recv(struct ep *e, uint64_t slot) {
    struct tb *t = e->tb;
    struct ibv_sge sge = {(uintptr_t)(t->ring + slot * TB_PACKET), TB_PACKET, t->ring_mr->lkey};
    struct ibv_recv_wr wr, *bad = NULL;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = TAG(T_RECV, slot), wr.sg_list = &sge, wr.num_sge = 1;
    if (!ibv_post_recv(e->qp, &wr, &bad)) return 0;
    poison(e, "posting a receive failed");
    return -1;
}

/* Post receives for `n` slots from slot counter `first` in one call (each post into the provider costs ~0.4 us). */
static int post_recvs(struct ep *e, uint64_t first, uint32_t n) {
    struct tb *t = e->tb;
    struct ibv_sge sge[64];
    struct ibv_recv_wr wr[64], *bad = NULL;
    if (n > 64) n = 64;
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t slot = (first + i) % t->slots;
        sge[i] = (struct ibv_sge){(uintptr_t)(t->ring + slot * TB_PACKET), TB_PACKET, t->ring_mr->lkey};
        memset(&wr[i], 0, sizeof(wr[i]));
        wr[i].wr_id = TAG(T_RECV, slot), wr[i].sg_list = &sge[i], wr[i].num_sge = 1;
        wr[i].next = i + 1 < n ? &wr[i + 1] : NULL;
    }
    if (!n || !ibv_post_recv(e->qp, wr, &bad)) return (int)n;
    poison(e, "posting a receive failed");
    return -1;
}

/* Post one SEND of a message that is never empty: zero-length SENDs are lost on Thunderbolt. */
static int post_send(struct ep *e, void *addr, uint32_t len, uint32_t lkey, int staged, uint32_t payload) {
    struct tb *t = e->tb;
    uint32_t i = (uint32_t)(t->tx_posted % TB_SEND_WR), n = packets(len);
    struct ibv_sge sge = {(uintptr_t)addr, len, lkey};
    struct ibv_send_wr wr, *bad = NULL;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = TAG(T_SEND, i), wr.sg_list = &sge, wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;
    if (!len || ibv_post_send(e->qp, &wr, &bad)) {
        poison(e, "posting a send failed");
        return -1;
    }
    t->tx_packets[i] = (uint16_t)n, t->tx_staged[i] = (uint8_t)staged;
    t->tx_bytes[i] = len, t->sent_bytes += len;
    t->tx_payload[i] = payload, t->sent_payload += payload;
    t->packets += n, t->tx_posted++;
    return 0;
}

int tb_init(struct ep *e) {
    struct tb *t = calloc(1, sizeof(*t));
    void *ring = NULL;
    size_t bytes = (size_t)(TB_RING + TB_STAGE) * TB_PACKET;
    if (!t || posix_memalign(&ring, 16384, bytes)) {
        free(t);
        return -1;
    }
    memset(ring, 0, bytes);
    t->ring = ring;
    e->tb = t;
    struct ibv_qp_init_attr init;
    struct ibv_qp_attr a;
    memset(&init, 0, sizeof(init));
    memset(&a, 0, sizeof(a));
    init.qp_type = IBV_QPT_UC;
    init.cap.max_send_wr = TB_DEPTH, init.cap.max_recv_wr = TB_RING;
    init.cap.max_send_sge = init.cap.max_recv_sge = 1;
    a.qp_state = IBV_QPS_INIT, a.port_num = 1;
    struct ibv_qp_init_attr got;
    memset(&got, 0, sizeof(got));
    int ok = (t->ring_mr = ibv_reg_mr(e->pd, ring, bytes, IBV_ACCESS_LOCAL_WRITE)) &&
             (e->cq = ibv_create_cq(e->ctx, TB_RING + TB_SEND_WR + 8, NULL, NULL, 0));
    if (ok) {
        init.send_cq = init.recv_cq = e->cq;
        ok = (e->qp = ibv_create_qp(e->pd, &init)) &&
             !ibv_modify_qp(e->qp, &a, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS) &&
             !ibv_query_qp(e->qp, &a, IBV_QP_CAP, &got);
    }
    /* the depths the device granted, which may be less than asked */
    t->depth = ok ? a.cap.max_send_wr : 0;
    t->slots = ok ? (a.cap.max_recv_wr < TB_RING ? a.cap.max_recv_wr : TB_RING) : 0;
    if (!ok || t->depth < 4 || t->slots < 4) {
        link_log("%s: cannot set up a thunderbolt queue pair (registration, queue or depth refused)", e->device);
        tb_destroy(e);
        return -1;
    }
    for (; t->posted < t->slots; ++t->posted)
        if (post_recv(e, t->posted)) {
            tb_destroy(e);
            return -1;
        }
    return 0;
}

int tb_connect(struct ep *e, const struct xinfo *peer) {
    struct tb *t = e->tb;
    struct ibv_qp_attr a;
    memset(&a, 0, sizeof(a));
    a.qp_state = IBV_QPS_RTR;
    a.path_mtu = e->mtu;
    a.rq_psn = peer->psn;
    a.dest_qp_num = peer->qpn;
    a.ah_attr.dlid = peer->lid;
    a.ah_attr.port_num = 1;
    a.ah_attr.is_global = 1;
    a.ah_attr.grh.hop_limit = 1;
    a.ah_attr.grh.sgid_index = (uint8_t)e->gid_index;
    memcpy(a.ah_attr.grh.dgid.raw, peer->gid, 16);
    int bad = !t || peer->frames < 4 ||
              ibv_modify_qp(e->qp, &a, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN);
    if (!bad) {
        memset(&a, 0, sizeof(a));
        a.qp_state = IBV_QPS_RTS;
        a.sq_psn = e->psn;
        bad = ibv_modify_qp(e->qp, &a, IBV_QP_STATE | IBV_QP_SQ_PSN);
    }
    if (bad) {
        link_log("%s: the thunderbolt queue pair refused RTR or RTS", e->device);
        return -1;
    }
    t->peer_slots = peer->frames;
    return 0;
}

void tb_info(const struct ep *e, struct xinfo *out) { out->frames = e->tb ? e->tb->slots : 0; }

/* Error state flushes every posted request; then nothing is destroyed with work in flight. */
void tb_destroy(struct ep *e) {
    struct tb *t = e->tb;
    if (!t) return;
    struct ibv_qp_attr err = {.qp_state = IBV_QPS_ERR};
    if (e->qp) ibv_modify_qp(e->qp, &err, IBV_QP_STATE);
    struct ibv_wc wc[64];
    uint64_t deadline = link_now_ns() + 200000000ull;
    for (int quiet = 0; e->cq && quiet < 3 && link_now_ns() < deadline;) {
        int n = ibv_poll_cq(e->cq, 64, wc);
        quiet = n > 0 ? 0 : quiet + 1;
        if (n < 0) break;
    }
    int failed = e->qp && ibv_destroy_qp(e->qp);
    failed = (e->cq && ibv_destroy_cq(e->cq)) || failed;
    /* a queue that would not die, or a registration that would not go, keeps its memory: leak it */
    if (failed || (t->ring_mr && ibv_dereg_mr(t->ring_mr)))
        link_log("%s: thunderbolt teardown failed; keeping its memory", e->device);
    else free(t->ring);
    free(t);
    e->qp = NULL, e->cq = NULL, e->tb = NULL;
}

void tb_accept(struct ep *e, const struct region *rx, uint64_t lo, uint64_t hi) {
    e->tb->rx = rx;
    e->tb->lo = lo;
    e->tb->hi = hi < rx->length ? hi : rx->length;
}

void tb_bond_hooks(struct ep *e, void *arg, void (*placed)(void *arg, uint64_t count),
                   int (*signal)(void *arg, uint64_t seq, const uint64_t need[2], uint64_t off, uint64_t value)) {
    e->tb->bond_arg = arg;
    e->tb->bond_placed = placed;
    e->tb->bond_signal = signal;
}

uint64_t tb_writes_posted(const struct ep *e) { return e->tb ? e->tb->writes_posted : 0; }
uint64_t tb_writes_placed(const struct ep *e) { return e->tb ? e->tb->writes_placed : 0; }
uint64_t tb_posted_bytes(const struct ep *e) { return e->tb ? e->tb->sent_bytes : 0; }
uint64_t tb_completed_bytes(const struct ep *e) { return e->tb ? e->tb->completed_bytes : 0; }
uint64_t tb_posted_payload(const struct ep *e) { return e->tb ? e->tb->sent_payload : 0; }
uint64_t tb_completed_payload(const struct ep *e) { return e->tb ? e->tb->completed_payload : 0; }

static void placed(struct tb *t) {
    t->writes_placed++;
    if (t->bond_placed) t->bond_placed(t->bond_arg, t->writes_placed);
}

/* Room for `n` more packets in `sends` more sends, holding back one header for a fence's answer unless `reply`. */
static int has_room(const struct tb *t, uint32_t n, uint32_t sends, int reply) {
    uint32_t spare = reply ? 0 : 1;
    return t->packets + n + spare <= t->depth && t->tx_posted - t->tx_done + sends + spare <= TB_SEND_WR &&
           t->staged - t->unstaged + spare < TB_STAGE;
}

/* A header message from the staging ring, with up to INLINE_MAX bytes after it. */
static int send_head(struct ep *e, uint16_t kind, uint64_t off, uint32_t len, uint64_t value, const void *bytes) {
    struct tb *t = e->tb;
    unsigned char *slot = t->ring + (size_t)(TB_RING + t->staged % TB_STAGE) * TB_PACKET;
    struct head h = {MAGIC, t->tx_seq, kind, 0, len, off, value};
    memcpy(slot, &h, sizeof(h));
    if (bytes) memcpy(slot + HEAD, bytes, len);
    if (post_send(e, slot, HEAD + (bytes ? len : 0), t->ring_mr->lkey, 1, kind == C_INLINE ? len : 0)) return -1;
    t->tx_seq++, t->staged++;
    return 0;
}

/* Progress until there is room for `n` packets in `sends` sends. */
static int room(struct ep *e, uint32_t n, uint32_t sends, uint64_t deadline) {
    struct tb *t = e->tb;
    while (!has_room(t, n, sends, 0)) {
        if (tb_progress(e) < 0) return -1;
        if (link_now_ns() > deadline) {
            poison(e, "no room to send within the timeout: the peer stopped taking messages");
            return -1;
        }
    }
    return t->poisoned ? -1 : 0;
}

/* The largest message the peer's ring takes whole, at most TB_MSG. */
static uint64_t msg_max(const struct tb *t) {
    uint64_t max = (uint64_t)(t->peer_slots / 2) * TB_PACKET, depth = (uint64_t)(t->depth / 2) * TB_PACKET;
    max = max < depth ? max : depth;
    return max > TB_MSG ? TB_MSG : max;
}

uint64_t tb_write_limit(const struct ep *e) {
    uint64_t max = e->tb ? msg_max(e->tb) : 0;
    return max < TB_TRY_MAX ? max : TB_TRY_MAX;
}

int tb_write(struct ep *e, const struct region *src, uint64_t off, uint64_t roff, uint64_t len, uint64_t timeout_ns) {
    struct tb *t = e->tb;
    uint64_t deadline = link_now_ns() + timeout_ns, max = msg_max(t);
    if (off > src->length || len > src->length - off) return -1;
    while (len) {
        uint64_t take = len < max ? len : max, lroom = src->seg - off % src->seg;
        take = take < lroom ? take : lroom;
        if (take <= INLINE_MAX) {
            if (room(e, 1, 1, deadline) || send_head(e, C_INLINE, roff, (uint32_t)take, 0, src->base + off)) return -1;
        } else if (room(e, 1 + packets(take), 2, deadline) || send_head(e, C_WRITE, roff, (uint32_t)take, 0, NULL) ||
                   post_send(e, src->base + off, (uint32_t)take, region_lkey(src, off), 0, (uint32_t)take)) {
            return -1;
        }
        t->writes_posted++;
        off += take, roff += take, len -= take;
    }
    return 0;
}

/* Reserve the whole bounded operation, including every header at registration/message boundaries. The regular
 * room check reserves only one header, whereas this preflight must reserve all of them without making progress. */
int tb_can_write(const struct ep *e, const struct region *src, uint64_t off, uint64_t len) {
    const struct tb *t = e->tb;
    if (!t || t->poisoned || !src || !src->seg || off > src->length || len > src->length - off || len > TB_TRY_MAX)
        return -1;
    uint64_t max = msg_max(t);
    if (!max) return -1;
    uint32_t n = 0, sends = 0, stages = 0;
    while (len) {
        uint64_t take = len < max ? len : max, lroom = src->seg - off % src->seg;
        take = take < lroom ? take : lroom;
        n += take <= INLINE_MAX ? 1 : 1 + packets(take);
        sends += take <= INLINE_MAX ? 1 : 2;
        stages++;
        /* An arbitrary region can have very small segments; stop counting once no possible room remains. */
        if (n >= t->depth || sends >= TB_SEND_WR || stages >= TB_STAGE) return 0;
        off += take, len -= take;
    }
    return t->packets + n + 1 <= t->depth && t->tx_posted - t->tx_done + sends + 1 <= TB_SEND_WR &&
           t->staged - t->unstaged + stages + 1 <= TB_STAGE;
}

int tb_write_try(struct ep *e, const struct region *src, uint64_t off, uint64_t roff, uint64_t len) {
    if (len > UINT64_MAX - roff) return -1;
    int can = tb_can_write(e, src, off, len);
    if (can <= 0) return can < 0 ? -1 : 1;
    struct tb *t = e->tb;
    uint64_t max = msg_max(t);
    while (len) {
        uint64_t take = len < max ? len : max, lroom = src->seg - off % src->seg;
        take = take < lroom ? take : lroom;
        if (take <= INLINE_MAX) {
            if (send_head(e, C_INLINE, roff, (uint32_t)take, 0, src->base + off)) return -1;
        } else if (send_head(e, C_WRITE, roff, (uint32_t)take, 0, NULL) ||
                   post_send(e, src->base + off, (uint32_t)take, region_lkey(src, off), 0, (uint32_t)take)) return -1;
        t->writes_posted++;
        off += take, roff += take, len -= take;
    }
    return 0;
}

int tb_signal(struct ep *e, uint64_t roff, uint64_t value, uint64_t timeout_ns) {
    if (room(e, 1, 1, link_now_ns() + timeout_ns)) return -1;
    return send_head(e, C_SIGNAL, roff, 8, value, NULL);
}

/* A write and then a signal as one message from the window, its head in the WS_HEAD bytes before `off`; a message
 * that would span two registrations or outgrow the peer's ring goes as the write, then the signal. */
int tb_write_signal(struct ep *e, const struct region *src, uint64_t off, uint64_t roff, uint64_t len, uint64_t soff,
                    uint64_t value, uint64_t timeout_ns) {
    struct tb *t = e->tb;
    if (off < WS_HEAD || off > src->length || len > src->length - off || !len) return -1;
    if (len + WS_HEAD > msg_max(t) || (off - WS_HEAD) / src->seg != (off + len - 1) / src->seg)
        return tb_write(e, src, off, roff, len, timeout_ns) || tb_signal(e, soff, value, timeout_ns) ? -1 : 0;
    if (room(e, packets(WS_HEAD + len), 1, link_now_ns() + timeout_ns)) return -1;
    unsigned char *m = src->base + off - WS_HEAD;
    struct head h = {MAGIC, t->tx_seq, C_WRITE_SIGNAL, 0, (uint32_t)len, roff, value};
    memset(m, 0, WS_HEAD);
    memcpy(m, &h, sizeof(h));
    memcpy(m + HEAD, &soff, sizeof(soff));
    if (post_send(e, m, (uint32_t)(WS_HEAD + len), region_lkey(src, off - WS_HEAD), 0, (uint32_t)len)) return -1;
    t->tx_seq++;
    t->writes_posted++;
    return 0;
}

int tb_can_bond_signal(const struct ep *e) {
    const struct tb *t = e->tb;
    return !t || t->poisoned || !t->peer_slots ? -1 : has_room(t, 1, 1, 0);
}

int tb_bond_signal(struct ep *e, uint64_t soff, uint64_t value, uint64_t seq, const uint64_t need[2],
                    uint64_t timeout_ns) {
    if (!e->tb || !need || !seq || soff % 8 || soff > UINT64_MAX - 8) return -1;
    if (room(e, 1, 1, link_now_ns() + timeout_ns)) return -1;
    struct tb *t = e->tb;
    unsigned char *slot = t->ring + (size_t)(TB_RING + t->staged % TB_STAGE) * TB_PACKET;
    struct head h = {MAGIC, t->tx_seq, C_BOND_SIGNAL, 0, 8, soff, value};
    memcpy(slot, &h, HEAD);
    memcpy(slot + HEAD, &seq, 8);
    memcpy(slot + HEAD + 8, need, 16);
    if (post_send(e, slot, BOND_SIGNAL_HEAD, t->ring_mr->lkey, 1, 0)) return -1;
    t->tx_seq++, t->staged++;
    return 0;
}

int tb_can_bond_write_signal(const struct ep *e, const struct region *src, uint64_t off, uint64_t len) {
    const struct tb *t = e->tb;
    if (!t || t->poisoned || !src || !src->seg || off > src->length || !len || len > src->length - off || !msg_max(t))
        return -1;
    if (off < BOND_WS_HEAD || len > msg_max(t) || BOND_WS_HEAD > msg_max(t) - len ||
        (off - BOND_WS_HEAD) / src->seg != (off + len - 1) / src->seg) return 2;
    /* The fused header lives in source memory, so it consumes no staging slot. Keep one slot/SEND/packet for ACKs. */
    return t->packets + packets(BOND_WS_HEAD + len) + 1 <= t->depth &&
           t->tx_posted - t->tx_done + 2 <= TB_SEND_WR && t->staged - t->unstaged + 1 <= TB_STAGE;
}

int tb_bond_write_signal(struct ep *e, const struct region *src, uint64_t off, uint64_t roff, uint64_t len,
                          uint64_t soff, uint64_t value, uint64_t seq, const uint64_t need[2], uint64_t timeout_ns) {
    struct tb *t = e->tb;
    if (!t || t->poisoned || !src || !src->seg || !need || !seq || soff % 8 || soff > UINT64_MAX - 8 ||
        off > src->length || !len || len > src->length - off || len > UINT64_MAX - roff) return -1;
    int can = tb_can_bond_write_signal(e, src, off, len);
    if (can < 0) return -1;
    if (can == 2) return 1;
    uint64_t deadline = link_now_ns() + timeout_ns;
    while (!can) {
        if (tb_progress(e) < 0) return -1;
        if (link_now_ns() > deadline) {
            poison(e, "no room for a bonded write-and-signal within the timeout");
            return -1;
        }
        can = tb_can_bond_write_signal(e, src, off, len);
        if (can < 0) return -1;
    }
    unsigned char *m = src->base + off - BOND_WS_HEAD;
    struct head h = {MAGIC, t->tx_seq, C_BOND_WRITE_SIGNAL, 0, (uint32_t)len, roff, value};
    memcpy(m, &h, HEAD);
    memcpy(m + HEAD, &soff, 8);
    memcpy(m + HEAD + 8, &seq, 8);
    memcpy(m + HEAD + 16, need, 16);
    if (post_send(e, m, (uint32_t)(BOND_WS_HEAD + len), region_lkey(src, off - BOND_WS_HEAD), 0, (uint32_t)len)) return -1;
    t->tx_seq++, t->writes_posted++;
    return 0;
}

/* Returns once the peer has applied every write and signal posted before this call. */
int tb_fence(struct ep *e, uint64_t timeout_ns) {
    struct tb *t = e->tb;
    uint64_t deadline = link_now_ns() + timeout_ns;
    uint32_t mine = ++t->fences;
    if (room(e, 1, 1, deadline) || send_head(e, C_FENCE, 0, 0, mine, NULL)) return -1;
    while ((int32_t)(t->fence_acked - mine) < 0) {
        if (tb_progress(e) < 0) return -1;
        if (link_now_ns() > deadline) {
            poison(e, "the peer did not acknowledge a fence within the timeout");
            return -1;
        }
    }
    return 0;
}

/* Copy `len` bytes of ring starting at slot counter `first` to `dst`; a message may wrap the ring once. */
static void copy_out(const struct tb *t, uint64_t first, unsigned char *dst, uint64_t len) {
    uint64_t at = (first % t->slots) * TB_PACKET, room = (uint64_t)t->slots * TB_PACKET - at;
    uint64_t head = len < room ? len : room;
    memcpy(dst, t->ring + at, head);
    if (len > head) memcpy(dst + head, t->ring, len - head);
}

/* Copy `len` bytes from `skip` bytes into the message at slot counter `first`; the ring may wrap once. */
static void copy_skip(const struct tb *t, uint64_t first, uint64_t skip, unsigned char *dst, uint64_t len) {
    uint64_t ring = (uint64_t)t->slots * TB_PACKET, at = ((first % t->slots) * TB_PACKET + skip) % ring;
    uint64_t head = len < ring - at ? len : ring - at;
    memcpy(dst, t->ring + at, head);
    if (len > head) memcpy(dst + head, t->ring, len - head);
}

static int inside(const struct tb *t, uint64_t off, uint64_t len) {
    return off >= t->lo && off <= t->hi && len <= t->hi - off;
}

/* Take one whole message from the ring: 1 taken, 0 not yet (a fence's answer waits for room), -1 failed. */
static int take(struct ep *e, uint64_t first, uint64_t count, uint64_t bytes) {
    struct tb *t = e->tb;
    if (t->want_len) {
        if (bytes != t->want_len) {
            poison(e, "a write's bytes differ in length from its header");
            return -1;
        }
        copy_out(t, first, t->rx->base + t->want_off, bytes);
        t->want_len = 0;
        placed(t);
        return 1;
    }
    struct head h;
    if (bytes < HEAD) {
        poison(e, "a malformed header message");
        return -1;
    }
    memcpy(&h, t->ring + (first % t->slots) * TB_PACKET, sizeof(h));
    if (h.magic == MAGIC && (h.kind == C_WRITE_SIGNAL || h.kind == C_BOND_WRITE_SIGNAL) && h.seq == t->rx_seq && t->rx) {
        /* its bytes, then its signal: the signal lands after them, as after a write of their own */
        uint64_t soff = 0, seq = 0, need[2] = {0, 0};
        uint64_t head = h.kind == C_WRITE_SIGNAL ? WS_HEAD : BOND_WS_HEAD;
        if (bytes >= head) {
            const unsigned char *m = t->ring + (first % t->slots) * TB_PACKET;
            memcpy(&soff, m + HEAD, sizeof(soff));
            if (h.kind == C_BOND_WRITE_SIGNAL) {
                memcpy(&seq, m + HEAD + 8, 8);
                memcpy(need, m + HEAD + 16, 16);
            }
        }
        if (bytes != head + (uint64_t)h.len || !h.len || soff % 8 || !inside(t, h.off, h.len) || !inside(t, soff, 8) ||
            (h.kind == C_BOND_WRITE_SIGNAL && (!seq || !t->bond_signal || !t->bond_placed))) {
            poison(e, "a malformed write-and-signal message");
            return -1;
        }
        t->rx_seq++;
        copy_skip(t, first, head, t->rx->base + h.off, h.len);
        placed(t);
        if (h.kind == C_WRITE_SIGNAL)
            __atomic_store_n((uint64_t *)(void *)(t->rx->base + soff), h.value, __ATOMIC_RELEASE);
        else if (t->bond_signal(t->bond_arg, seq, need, soff, h.value)) {
            poison(e, "the bonded signal was refused");
            return -1;
        }
        return 1;
    }
    if (h.magic == MAGIC && h.kind == C_BOND_SIGNAL && h.seq == t->rx_seq && t->rx) {
        uint64_t seq = 0, need[2] = {0, 0};
        if (bytes == BOND_SIGNAL_HEAD) {
            const unsigned char *m = t->ring + (first % t->slots) * TB_PACKET;
            memcpy(&seq, m + HEAD, 8);
            memcpy(need, m + HEAD + 8, 16);
        }
        if (count != 1 || bytes != BOND_SIGNAL_HEAD || h.len != 8 || h.off % 8 || !inside(t, h.off, 8) ||
            !seq || !t->bond_signal || !t->bond_placed) {
            poison(e, "a malformed bonded signal message");
            return -1;
        }
        t->rx_seq++;
        if (t->bond_signal(t->bond_arg, seq, need, h.off, h.value)) {
            poison(e, "the bonded signal was refused");
            return -1;
        }
        return 1;
    }
    if (count != 1) {
        poison(e, "a malformed header message");
        return -1;
    }
    const char *why = h.magic != MAGIC                                     ? "a malformed header message"
                      : h.seq != t->rx_seq                                 ? "a message was lost or reordered"
                      : !t->rx && h.kind != C_FENCE_ACK                    ? "the peer wrote before this end accepted"
                      : h.kind == C_INLINE && h.len != bytes - HEAD        ? "an inline write of the wrong length"
                      : h.kind == C_WRITE && (!h.len || h.len > TB_MSG)    ? "a write of an impossible size"
                      : h.kind == C_SIGNAL && h.off % 8                    ? "an unaligned signal"
                      : h.kind < C_WRITE || h.kind > C_FENCE_ACK           ? "an unknown message"
                      : h.kind <= C_INLINE && !inside(t, h.off, h.len)     ? "a write outside what the peer may write"
                      : h.kind == C_SIGNAL && !inside(t, h.off, 8)         ? "a signal outside what the peer may write"
                                                                            : NULL;
    if (why) {
        poison(e, why);
        return -1;
    }
    if (h.kind == C_FENCE) {
        if (!has_room(t, 1, 1, 1)) return 0;
        if (send_head(e, C_FENCE_ACK, 0, 0, h.value, NULL)) return -1;
    }
    t->rx_seq++;
    if (h.kind == C_INLINE) {
        memcpy(t->rx->base + h.off, t->ring + (first % t->slots) * TB_PACKET + HEAD, h.len);
        placed(t);
    }
    if (h.kind == C_WRITE) t->want_off = h.off, t->want_len = h.len;
    if (h.kind == C_SIGNAL) __atomic_store_n((uint64_t *)(void *)(t->rx->base + h.off), h.value, __ATOMIC_RELEASE);
    if (h.kind == C_FENCE_ACK) t->fence_acked = (uint32_t)h.value;
    return 1;
}

/* Take every whole message in order, then post the slots they used again, in ring order. */
static void drain(struct ep *e) {
    struct tb *t = e->tb;
    while (t->used < t->filled && !t->poisoned) {
        uint64_t end = t->used;
        while (end < t->filled && !t->rx_last[end % t->slots]) end++;
        if (end == t->filled) {
            if (t->filled - t->used == t->slots) poison(e, "a message larger than the receive ring");
            break;
        }
        uint64_t count = end - t->used + 1, bytes = (count - 1) * TB_PACKET + t->rx_len[end % t->slots];
        if (take(e, t->used, count, bytes) <= 0) break;
        t->used = end + 1;
    }
    while (!t->poisoned && t->posted < t->used + t->slots) {
        int n = post_recvs(e, t->posted, (uint32_t)(t->used + t->slots - t->posted));
        if (n <= 0) break;
        t->posted += (uint64_t)n;
    }
}

static void complete(struct ep *e, const struct ibv_wc *wc) {
    struct tb *t = e->tb;
    unsigned kind = (unsigned)(wc->wr_id >> 56);
    uint64_t index = wc->wr_id & ((1ull << 56) - 1);
    if (kind == T_RECV && index == t->filled % t->slots && t->filled < t->posted &&
        ((wc->status == IBV_WC_SUCCESS && wc->byte_len >= 1 && wc->byte_len <= TB_PACKET) ||
         (wc->status == IBV_WC_LOC_LEN_ERR && wc->byte_len == TB_PACKET))) {
        /* only a message's last packet completes cleanly; the others report a length error and a full packet */
        t->rx_len[index] = (uint16_t)wc->byte_len;
        t->rx_last[index] = wc->status == IBV_WC_SUCCESS;
        t->filled++;
    } else if (kind == T_SEND && wc->status == IBV_WC_SUCCESS && index == t->tx_done % TB_SEND_WR &&
               t->tx_done < t->tx_posted) {
        t->packets -= t->tx_packets[index];
        t->unstaged += t->tx_staged[index];
        t->completed_bytes += t->tx_bytes[index];
        t->completed_payload += t->tx_payload[index];
        t->tx_done++;
    } else {
        char why[112];
        snprintf(why, sizeof(why), "an unexpected completion (status %d, %u bytes)", (int)wc->status, wc->byte_len);
        poison(e, why);
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
    return t && (t->tx_posted != t->tx_done || t->used != t->filled || t->want_len);
}

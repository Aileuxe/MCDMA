/* Verbs endpoints over RoCE or Apple Thunderbolt RDMA and the link-local setup exchange, shared by mcdma-rpcd and
 * libmcdma-fabric. */
#ifndef MCDMA_LINK_H
#define MCDMA_LINK_H

#ifdef __APPLE__
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#define __APPLE_USE_RFC_3542 1
#else
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#endif
#include <infiniband/verbs.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

#define LINK_ROCE 1                 /* RC queue pair with RDMA WRITE and READ: MCDMA's CX5 provider, Linux mlx5 */
#define LINK_TB 2                   /* Apple Thunderbolt RDMA: UC queue pairs with SEND and RECV only */
#define LINK_LAYER_TB 100           /* IBV_LINK_LAYER_THUNDERBOLT in Apple's verbs.h */
#define LINK_SEG (4ull << 20)       /* RoCE registration unit on a Mac, the largest the CX5 provider has run */
#define TB_SEG (12ull << 20)        /* Thunderbolt registration unit: a 4 MiB multiple under max_mr_size 0xfa0000 */
#define TB_PACKET 4096u             /* a SEND crosses as packets of this size, each filling one posted receive */
#define TB_RING 2048                /* one-packet receives each link keeps posted: 8 MiB */
#define TB_STAGE 64                 /* header messages in flight */
#define TB_SEND_WR 1024             /* sends in flight */
#define TB_MSG (4ull << 20)         /* largest message */
#define TB_DEPTH 4095               /* send queue depth in packets that Thunderbolt allows */
#define TB_TRY_MAX (256ull << 10)    /* largest bounded, nonblocking bonded write chunk */
#define LINK_REGION_MAX 128         /* registrations per region, and keys per exchange datagram */
#define LINK_MRS (LINK_REGION_MAX + 4)
#define LINK_RC_DEPTH 31            /* RoCE CQ and send queue: the CX5 provider refuses 63 */
#define MODE_DIRECT 1
#define MODE_PULL 2

/* A registered region: [base, base + length) in registrations of `seg` bytes; the last may be shorter. */
struct region {
    unsigned char *base;
    uint64_t length, seg, split;    /* registrations below split get access `below`, the rest `above` */
    int n, below, above;
    struct ibv_mr *mr[LINK_REGION_MAX];
};

/* The peer's region as its offer describes it: RoCE needs its keys, Thunderbolt only its geometry. */
struct table {
    uint64_t base, length, seg;
    uint32_t n;
    uint32_t rkey[LINK_REGION_MAX];
};

/* One side's queue pairs and region, everything the other side needs to connect and to address it. */
struct xinfo {
    uint8_t transport, mode;
    uint16_t lid;
    uint32_t qpn, qpn2, psn;    /* qpn2 is reserved and zero */
    uint32_t frames;            /* Thunderbolt: receives kept posted, which bound the peer's messages */
    uint8_t gid[16];
    uint64_t req, rep;          /* mailbox halves; a fabric window sends its length as req and rep 0 */
    struct table table;
};

enum { X_OFFER = 1, X_PING, X_PONG, X_BYE, X_ERR };
enum { ROLE_CONNECT = 1, ROLE_LISTEN, ROLE_PEER };
#define X_HAVE 1                    /* flag: I hold your offer and my queue pairs are at RTS */
#define X_NAME 24
#define X_MAX 1232                  /* a datagram never needs IPv6 fragments */

struct xmsg {
    uint8_t kind, role, flags;
    char name[X_NAME];
    uint8_t from[16], to[16];   /* the sender's session; the receiver's session as the sender knows it */
    struct xinfo info;          /* X_OFFER */
    char text[96];              /* X_ERR */
};

/* One end of the exchange: a UDP socket on one Thunderbolt IP interface, admitting only on-link link-local peers. */
struct xchg {
    int fd, port, peer_port, pinned, learned, v4;   /* v4: the pinned peer is IPv4, `peer` holds it mapped */
    unsigned ifindex;
    char ifname[32];
    char name[X_NAME];
    struct in6_addr peer;
    uint8_t session[16], peer_session[16];
    uint64_t rejected, warned_ns;
};

struct tb;

struct ep {
    char device[64];
    int gid_index, kind;
    enum ibv_mtu mtu;
    uint16_t lid;
    uint32_t psn;
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;          /* the link's one queue pair: RC on RoCE, UC on Thunderbolt */
    struct tb *tb;              /* Thunderbolt: receive ring, header staging and ordering state */
    struct ibv_mr *mr[LINK_MRS];
    int nmr;
    union ibv_gid gid;
    int outstanding;            /* RoCE work requests posted, not yet completed; nothing is destroyed while > 0 */
};

struct piece {
    void *local;
    uint32_t lkey;
    uint64_t remote;
    uint32_t rkey;
    uint32_t len;
};

/* Each binary supplies its own logger. */
void link_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* link_verbs.c */
uint64_t link_now_ns(void);
void link_random(void *out, size_t n);
const char *link_kind_name(int kind);
int ep_open(struct ep *e, const char *device, int gid_index, int mtu);
struct ibv_mr *ep_reg(struct ep *e, void *addr, size_t len, int access);
int ep_reg_region(struct ep *e, struct region *r, void *base, uint64_t length, uint64_t seg, uint64_t split,
                  int below, int above);
uint32_t region_lkey(const struct region *r, uint64_t off);
int ep_create_qp(struct ep *e);
int ep_connect(struct ep *e, const struct xinfo *peer);
void ep_destroy_qp(struct ep *e);
void ep_close(struct ep *e);
void ep_info(const struct ep *e, const struct region *r, struct xinfo *out);
int table_valid(const struct table *t, uint64_t need, int keys);
int ep_post(struct ep *e, enum ibv_wr_opcode op, const struct piece *p);
int ep_reap(struct ep *e, int want, uint64_t timeout_ns);
int post_pieces(struct ep *e, enum ibv_wr_opcode op, const struct piece *p, int n, int window, uint64_t timeout_ns);
int cut(const struct region *local, uint64_t off, uint64_t len, const struct table *remote, uint64_t roff,
        uint64_t max, struct piece *out, int cap);

/* link_tb.c */
int tb_init(struct ep *e);
int tb_connect(struct ep *e, const struct xinfo *peer);
void tb_info(const struct ep *e, struct xinfo *out);
void tb_destroy(struct ep *e);
void tb_accept(struct ep *e, const struct region *rx, uint64_t lo, uint64_t hi);
int tb_write(struct ep *e, const struct region *src, uint64_t off, uint64_t roff, uint64_t len, uint64_t timeout_ns);
int tb_signal(struct ep *e, uint64_t roff, uint64_t value, uint64_t timeout_ns);
int tb_write_signal(struct ep *e, const struct region *src, uint64_t off, uint64_t roff, uint64_t len, uint64_t soff,
                    uint64_t value, uint64_t timeout_ns);
int tb_fence(struct ep *e, uint64_t timeout_ns);
int tb_progress(struct ep *e);
int tb_busy(const struct ep *e);
/* Bond hooks run under the endpoint's lock, after copying a complete write. The signal hook must copy `need`
 * if retaining it, return 0 on success/-1 on failure, and arrange the final release-store itself. No hook may
 * reenter this endpoint. Install both hooks before accepting bonded messages; ordinary wire messages are unchanged. */
void tb_bond_hooks(struct ep *e, void *arg, void (*placed)(void *arg, uint64_t count),
                   int (*signal)(void *arg, uint64_t seq, const uint64_t need[2], uint64_t off, uint64_t value));
uint64_t tb_writes_posted(const struct ep *e);
uint64_t tb_writes_placed(const struct ep *e);
uint64_t tb_posted_bytes(const struct ep *e);
uint64_t tb_completed_bytes(const struct ep *e);
uint64_t tb_posted_payload(const struct ep *e);
uint64_t tb_completed_payload(const struct ep *e);
uint64_t tb_write_limit(const struct ep *e); /* cap a scheduler's piece by this and its source-registration room */
/* Caller serializes with progress. can_write: 1 room, 0 no room, -1 invalid/failed. write_try: 0 posted,
 * 1 no room, -1 invalid/failed; no progress, waits or partial posting on insufficient room. len <= TB_TRY_MAX. */
int tb_can_write(const struct ep *e, const struct region *src, uint64_t off, uint64_t len);
int tb_write_try(struct ep *e, const struct region *src, uint64_t off, uint64_t roff, uint64_t len);
int tb_bond_signal(struct ep *e, uint64_t soff, uint64_t value, uint64_t seq, const uint64_t need[2],
                    uint64_t timeout_ns);
int tb_can_bond_signal(const struct ep *e); /* -1 invalid/failed, 0 no staged-header room, 1 ready */
/* -1 invalid/failed, 0 no room, 1 fused ready, 2 not representable; no progress or source changes. */
int tb_can_bond_write_signal(const struct ep *e, const struct region *src, uint64_t off, uint64_t len);
/* Fused only: 1 means not representable (nothing posted), 0 posted, -1 invalid/failed. need includes this write
 * on the selected lane, whose posted count increases by exactly one. The source reserves 64 bytes before off. */
int tb_bond_write_signal(struct ep *e, const struct region *src, uint64_t off, uint64_t roff, uint64_t len,
                          uint64_t soff, uint64_t value, uint64_t seq, const uint64_t need[2], uint64_t timeout_ns);

/* link_xchg.c */
int xchg_open(struct xchg *x, const char *via, int port, int peer_port, const char *name);
void xchg_close(struct xchg *x);
void xchg_session(struct xchg *x);
void xchg_prepare(const struct xchg *x, struct xmsg *m, int kind, int role, int flags);
int xchg_send(struct xchg *x, const struct xmsg *m);
int xchg_recv(struct xchg *x, struct xmsg *m, int timeout_ms);
int xchg_ours(const struct xchg *x, const struct xmsg *m);
int xchg_encode(const struct xmsg *m, uint8_t *out, size_t cap);
int xchg_decode(const uint8_t *in, size_t len, struct xmsg *m);
int via_parse(const char *via, char *ifname, size_t n, struct in6_addr *peer, int *pinned);
int via_check(const char *via, const struct ep *e);

#endif

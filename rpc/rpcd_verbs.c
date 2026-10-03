/* mcdma-rpcd: how a mailbox is registered, offered to the peer and written into it, on either link kind. */
#include "rpcd.h"

#include <stdio.h>
#include <string.h>

/* SIGINT and SIGTERM ask for the same orderly stop as SHUTDOWN; nothing is torn down inside the handler. */
void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

void write_sizes(struct box *b) {
    volatile uint64_t *w = (volatile uint64_t *)(b->base + 256);
    store_word(&w[0], b->req);
    store_word(&w[1], b->rep);
}

/* RoCE peers write requests or replies and read replies, each half keyed apart; Thunderbolt peers can only send. */
int box_register(struct ep *e, struct box *b, int listen) {
    uint64_t total = b->req + b->rep;
    int rw = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
    if (e->kind == LINK_TB) return ep_reg_region(e, &b->r, b->base, total, TB_SEG, total, 0, 0);
    if (!listen) return ep_reg_region(e, &b->r, b->base, total, SEG, b->req, IBV_ACCESS_LOCAL_WRITE, rw);
#ifdef __APPLE__
    return ep_reg_region(e, &b->r, b->base, total, SEG, b->req, rw, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);
#else
    return ep_reg_region(e, &b->r, b->base, total, 0, total, rw | IBV_ACCESS_REMOTE_READ, 0);
#endif
}

void box_offer(const struct xchg *x, const struct ep *e, const struct box *b, int role, int mode, int flags,
               struct xmsg *m) {
    xchg_prepare(x, m, X_OFFER, role, flags);
    ep_info(e, &b->r, &m->info);
    m->info.mode = (uint8_t)mode;
    m->info.req = b->req;
    m->info.rep = b->rep;
}

/* Why the peer's offer cannot serve this mailbox, or NULL. */
const char *box_refuse(const struct ep *e, const struct box *b, const struct xinfo *peer) {
    if (peer->transport != e->kind) return "the two ends use different link kinds";
    if (peer->req != b->req || peer->rep != b->rep) return "mailbox sizes differ";
    if (e->kind == LINK_TB && peer->mode != MODE_DIRECT) return "pull mode needs RDMA READ, which Thunderbolt lacks";
    if (!table_valid(&peer->table, b->req + b->rep, e->kind == LINK_ROCE)) return "the peer's registrations are malformed";
    return NULL;
}

/* A goodbye, refusal or answer to one particular session of the peer. */
void xchg_answer(struct xchg *x, const uint8_t to[16], int kind, int role, const char *text) {
    struct xmsg m;
    xchg_prepare(x, &m, kind, role, 0);
    memcpy(m.to, to, 16);
    if (text) snprintf(m.text, sizeof(m.text), "%s", text);
    (void)xchg_send(x, &m);
}

/* Payload to the same peer offsets, then `word` at word_dst: RoCE sends it last from word_src, Thunderbolt signals it. */
int box_transfer(struct ep *e, struct box *b, const struct table *remote, uint64_t off, uint32_t len, uint64_t word_src,
                 uint64_t word_dst, uint64_t word, struct piece *pieces, int cap, int window) {
    if (e->kind == LINK_TB)
        return tb_write(e, &b->r, off, off, len, remote->seg, XFER_NS) || tb_signal(e, word_dst, word, XFER_NS) ? -1 : 0;
    int n = len ? cut(&b->r, off, len, remote, off, SEG, pieces, cap - 1) : 0;
    if (n < 0) return -1;
    pieces[n] = (struct piece){b->base + word_src, region_lkey(&b->r, word_src), remote->base + word_dst,
                               remote->rkey[word_dst / remote->seg], 8};
    return post_pieces(e, IBV_WR_RDMA_WRITE, pieces, n + 1, window, XFER_NS);
}

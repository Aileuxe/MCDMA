/* Offline verbs for the link, daemon and fabric tests: RoCE devices roce0 and roce1 (RC, WRITE and READ, CX5 limits)
 * and Apple Thunderbolt devices tb0-tb11 as the 3 October qualification measured them on macOS 27.0: UC only, three
 * queue pairs a device, SEND only, each SEND cut into 4 KiB packets that fill the peer's posted receives in order,
 * every packet but the last completing with IBV_WC_LOC_LEN_ERR. Packets wait for receives, queue pairs are delivered
 * in a random order (STUB_SEED), and every key and range is checked. Misuse prints STUB-VIOLATION and exits 86. */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#else
#define _GNU_SOURCE 1
#endif
#include <infiniband/verbs.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NDEV 14
#define TB_FIRST 2
#define MAXQP 512
#define MAXMR 8192
#define PACKET 4096u
#define TB_MR_MAX 0xfa0000u
#define TB_MAX_MSG 16773120u
#define TB_QPS 3

struct sctx { struct ibv_context c; int dev; };
struct spd { struct ibv_pd pd; int dev; };
struct smr { struct ibv_mr mr; int dev, access, live; };
struct scq { struct ibv_cq cq; struct ibv_wc *ring; int cap, head, count, destroyed; };
struct swr { uint64_t id; unsigned char *addr; uint32_t len, sent; };
struct sq { struct swr *v; uint32_t cap, head, count, packets; };
struct sqp { struct ibv_qp qp; int dev, destroyed; uint32_t dest, cap_send, cap_recv; struct sq send, recv; };

static const char *g_names[NDEV] = {"roce0", "roce1", "tb0", "tb1", "tb2", "tb3", "tb4",
                                    "tb5", "tb6", "tb7", "tb8", "tb9", "tb10", "tb11"};
static struct ibv_device g_dev[NDEV];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct smr *g_mr[MAXMR];
static struct sqp *g_qp[MAXQP];
static int g_nmr, g_nqp, g_mrs[NDEV], g_uc[NDEV];
static uint64_t g_rand;
static int g_hold[NDEV], g_hold_completions[NDEV], g_failed[NDEV];
static uint64_t g_sent_bytes[NDEV], g_received_bytes[NDEV];

static void die(const char *what) {
    fprintf(stderr, "STUB-VIOLATION: %s\n", what);
    fflush(stderr);
    _exit(86);
}

static int is_tb(int dev) { return dev >= TB_FIRST; }
static uint32_t packets(uint32_t len) { return len ? (len + PACKET - 1) / PACKET : 1; }

static int device_index(const char *name) {
    for (int i = 0; i < NDEV; ++i)
        if (!strcmp(name, g_names[i])) return i;
    die("a test selected a missing stub device");
    return -1;
}

/* Deterministic cross-link delivery and failure controls, used only by the offline fabric tests. */
void stub_tb_hold(const char *name, int hold) {
    pthread_mutex_lock(&g_mu);
    g_hold[device_index(name)] = !!hold;
    pthread_mutex_unlock(&g_mu);
}

void stub_tb_hold_completions(const char *name, int hold) {
    pthread_mutex_lock(&g_mu);
    g_hold_completions[device_index(name)] = !!hold;
    pthread_mutex_unlock(&g_mu);
}

uint64_t stub_tb_sent_bytes(const char *name) {
    pthread_mutex_lock(&g_mu);
    uint64_t n = g_sent_bytes[device_index(name)];
    pthread_mutex_unlock(&g_mu);
    return n;
}

uint64_t stub_tb_received_bytes(const char *name) {
    pthread_mutex_lock(&g_mu);
    uint64_t n = g_received_bytes[device_index(name)];
    pthread_mutex_unlock(&g_mu);
    return n;
}

static unsigned rnd(void) {
    if (!g_rand) {
        const char *seed = getenv("STUB_SEED");
        g_rand = seed ? strtoull(seed, NULL, 10) * 2654435761u + 1 : 88172645463325252ull;
    }
    g_rand ^= g_rand << 13, g_rand ^= g_rand >> 7, g_rand ^= g_rand << 17;
    return (unsigned)(g_rand >> 11);
}

static struct smr *find(uint32_t key) {
    if (key < 1000 || key - 1000 >= (uint32_t)g_nmr) return NULL;
    return g_mr[key - 1000];
}

static void check_local(uint32_t key, uint64_t addr, uint32_t len, int dev, int writes) {
    struct smr *m = find(key);
    if (!m || !m->live || m->dev != dev) die("an lkey that is not a live registration on this device");
    uint64_t lo = (uint64_t)(uintptr_t)m->mr.addr;
    if (addr < lo || addr + len > lo + m->mr.length) die("a local buffer outside its registration");
    if (writes && !(m->access & IBV_ACCESS_LOCAL_WRITE)) die("a receive into memory registered without LOCAL_WRITE");
}

static int check_remote(uint32_t key, uint64_t addr, uint32_t len, int bit) {
    struct smr *m = find(key);
    if (!m) {
        if (getenv("STUB_STRICT")) die("an rkey that was never registered");
        return 0;
    }
    uint64_t lo = (uint64_t)(uintptr_t)m->mr.addr;
    if (!m->live) die("an rkey whose registration is gone");
    if (!(m->access & bit)) die("remote access that the registration does not allow");
    if (addr < lo || addr + len > lo + m->mr.length) die("a remote range outside its registration");
    return 1;
}

static void push(struct ibv_cq *cq, uint64_t id, enum ibv_wc_status st, enum ibv_wc_opcode op, uint32_t len,
                 uint32_t qpn) {
    struct scq *c = (struct scq *)cq;
    if (c->destroyed) die("a completion for a destroyed CQ");
    if (c->count == c->cap) die("completion queue overrun");
    struct ibv_wc *w = &c->ring[(c->head + c->count++) % c->cap];
    memset(w, 0, sizeof(*w));
    w->wr_id = id, w->status = st, w->opcode = op, w->byte_len = len, w->qp_num = qpn;
}

static void enqueue(struct sq *q, uint64_t id, uint64_t addr, uint32_t len) {
    q->v[(q->head + q->count++) % q->cap] = (struct swr){id, (unsigned char *)(uintptr_t)addr, len, 0};
    q->packets += packets(len);
}

static struct swr dequeue(struct sq *q) {
    struct swr w = q->v[q->head];
    q->head = (q->head + 1) % q->cap, q->count--, q->packets -= packets(w.len);
    return w;
}

static void flush(struct sqp *q) {
    while (q->send.count) push(q->qp.send_cq, dequeue(&q->send).id, IBV_WC_WR_FLUSH_ERR, IBV_WC_SEND, 0, q->qp.qp_num);
    while (q->recv.count) push(q->qp.recv_cq, dequeue(&q->recv).id, IBV_WC_WR_FLUSH_ERR, IBV_WC_RECV, 0, q->qp.qp_num);
}

void stub_tb_fail(const char *name) {
    pthread_mutex_lock(&g_mu);
    int dev = device_index(name);
    g_failed[dev] = 1;
    for (int i = 0; i < g_nqp; ++i) {
        struct sqp *q = g_qp[i];
        if (!q->destroyed && q->dev == dev) {
            q->qp.state = IBV_QPS_ERR;
            flush(q);
        }
    }
    pthread_mutex_unlock(&g_mu);
}

/* Packet by packet, queue pairs in random order; all but a message's last complete with LOC_LEN_ERR. */
static void pump(int polling) {
    static int lazy = -1;
    if (lazy < 0) lazy = getenv("STUB_LAZY") != NULL;
    if (lazy && !polling) return;
    int order[MAXQP], n = 0;
    for (int i = 0; i < g_nqp; ++i)
        if (!g_qp[i]->destroyed && is_tb(g_qp[i]->dev) && g_qp[i]->qp.state == IBV_QPS_RTS && g_qp[i]->send.count)
            order[n++] = i;
    for (int i = n - 1; i > 0; --i) {
        int j = (int)(rnd() % (unsigned)(i + 1)), t = order[i];
        order[i] = order[j], order[j] = t;
    }
    for (int k = 0; k < n; ++k) {
        struct sqp *q = g_qp[order[k]], *d = q->dest >= 100 && q->dest - 100 < (uint32_t)g_nqp ? g_qp[q->dest - 100] : NULL;
        for (unsigned burst = rnd() % 9; burst && q->send.count; --burst) {
            if (!d || d->destroyed || g_hold[d->dev] ||
                (d->qp.state != IBV_QPS_RTR && d->qp.state != IBV_QPS_RTS) || !d->recv.count) break;
            struct swr *s = &q->send.v[q->send.head], r = d->recv.v[d->recv.head];
            uint32_t len = s->len - s->sent < PACKET ? s->len - s->sent : PACKET;
            int last = s->sent + len == s->len;
            if (r.len < len) die("a receive smaller than the packet that fills it");
            memcpy(r.addr, s->addr + s->sent, len);
            g_received_bytes[d->dev] += len;
            s->sent += len;
            dequeue(&d->recv);
            push(d->qp.recv_cq, r.id, last ? IBV_WC_SUCCESS : IBV_WC_LOC_LEN_ERR, IBV_WC_RECV, len, d->qp.qp_num);
            if (last) {
                struct swr done = dequeue(&q->send);
                push(q->qp.send_cq, done.id, IBV_WC_SUCCESS, IBV_WC_SEND, done.len, q->qp.qp_num);
            }
        }
    }
}

static int stub_poll(struct ibv_cq *cq, int n, struct ibv_wc *wc) {
    struct scq *c = (struct scq *)cq;
    pthread_mutex_lock(&g_mu);
    if (c->destroyed) die("poll_cq on a destroyed CQ");
    pump(1);
    int got = 0;
    int dev = ((struct sctx *)c->cq.context)->dev;
    for (; !g_hold_completions[dev] && got < n && c->count; ++got) {
        wc[got] = c->ring[c->head];
        c->head = (c->head + 1) % c->cap, c->count--;
    }
    pthread_mutex_unlock(&g_mu);
    return got;
}

static int stub_post_send(struct ibv_qp *qp, struct ibv_send_wr *wr, struct ibv_send_wr **bad) {
    struct sqp *q = (struct sqp *)qp;
    (void)bad;
    pthread_mutex_lock(&g_mu);
    if (q->destroyed) die("post_send on a destroyed QP");
    if (g_failed[q->dev]) {
        pthread_mutex_unlock(&g_mu);
        return EIO;
    }
    if (qp->state != IBV_QPS_RTS) die("post_send before RTS");
    for (; wr; wr = wr->next) {
        if (wr->num_sge != 1) die("a send with other than one scatter entry");
        if (!(wr->send_flags & IBV_SEND_SIGNALED)) die("an unsignaled send");
        struct ibv_sge *s = &wr->sg_list[0];
        check_local(s->lkey, s->addr, s->length, q->dev, 0);
        void *local = (void *)(uintptr_t)s->addr, *remote = (void *)(uintptr_t)wr->wr.rdma.remote_addr;
        if (is_tb(q->dev)) {
            if (wr->opcode != IBV_WR_SEND) die("Thunderbolt RDMA lands every request in the next receive: only SEND");
            if (!s->length) die("a zero-length SEND is lost on Thunderbolt and never completes");
            if (s->length > TB_MAX_MSG) die("a message larger than Thunderbolt allows");
            if (q->send.packets + packets(s->length) > q->cap_send || q->send.count == q->send.cap) {
                pthread_mutex_unlock(&g_mu);
                return ENOMEM;   /* Thunderbolt's send queue counts packets and refuses past its depth */
            }
            enqueue(&q->send, wr->wr_id, s->addr, s->length);
            g_sent_bytes[q->dev] += s->length;
        } else if (wr->opcode == IBV_WR_RDMA_WRITE) {
            if (check_remote(wr->wr.rdma.rkey, wr->wr.rdma.remote_addr, s->length, IBV_ACCESS_REMOTE_WRITE))
                memcpy(remote, local, s->length);
            push(qp->send_cq, wr->wr_id, IBV_WC_SUCCESS, IBV_WC_RDMA_WRITE, s->length, qp->qp_num);
        } else if (wr->opcode == IBV_WR_RDMA_READ) {
            if (check_remote(wr->wr.rdma.rkey, wr->wr.rdma.remote_addr, s->length, IBV_ACCESS_REMOTE_READ))
                memcpy(local, remote, s->length);
            push(qp->send_cq, wr->wr_id, IBV_WC_SUCCESS, IBV_WC_RDMA_READ, s->length, qp->qp_num);
        } else {
            die("these RoCE tests post only WRITE and READ");
        }
    }
    pump(0);
    pthread_mutex_unlock(&g_mu);
    return 0;
}

static int stub_post_recv(struct ibv_qp *qp, struct ibv_recv_wr *wr, struct ibv_recv_wr **bad) {
    struct sqp *q = (struct sqp *)qp;
    (void)bad;
    pthread_mutex_lock(&g_mu);
    if (q->destroyed) die("post_recv on a destroyed QP");
    if (g_failed[q->dev]) {
        pthread_mutex_unlock(&g_mu);
        return EIO;
    }
    if (!is_tb(q->dev)) die("these RoCE tests post no receives");
    if (qp->state == IBV_QPS_RESET) die("post_recv before INIT");
    for (; wr; wr = wr->next) {
        if (wr->num_sge != 1) die("a receive with other than one scatter entry");
        struct ibv_sge *s = &wr->sg_list[0];
        check_local(s->lkey, s->addr, s->length, q->dev, 1);
        if (s->length > PACKET) die("a receive larger than 4096 bytes: Thunderbolt completes it with an error");
        if (q->recv.count == q->recv.cap) die("a Thunderbolt receive queue overrun");
        enqueue(&q->recv, wr->wr_id, s->addr, s->length);
    }
    if (qp->state == IBV_QPS_ERR) flush(q);
    pump(0);
    pthread_mutex_unlock(&g_mu);
    return 0;
}

struct ibv_device **ibv_get_device_list(int *n) {
    struct ibv_device **l = calloc(NDEV + 1, sizeof(*l));
    for (int i = 0; i < NDEV; ++i) {
        snprintf(g_dev[i].name, sizeof(g_dev[i].name), "%s", g_names[i]);
        l[i] = &g_dev[i];
    }
    if (n) *n = NDEV;
    return l;
}
void ibv_free_device_list(struct ibv_device **l) { free(l); }
const char *ibv_get_device_name(struct ibv_device *d) { return d->name; }
const char *ibv_wc_status_str(enum ibv_wc_status status) { return status == IBV_WC_SUCCESS ? "success" : "failure"; }

struct ibv_context *ibv_open_device(struct ibv_device *d) {
    struct sctx *c = calloc(1, sizeof(*c));
    c->dev = (int)(d - g_dev);
    c->c.device = d;
    c->c.ops.poll_cq = stub_poll;
    c->c.ops.post_send = stub_post_send;
    c->c.ops.post_recv = stub_post_recv;
    return &c->c;
}
int ibv_close_device(struct ibv_context *c) {
    free(c);
    return 0;
}

#undef ibv_query_port
int ibv_query_port(struct ibv_context *c, uint8_t port, struct _compat_ibv_port_attr *a) {
    struct ibv_port_attr *p = (struct ibv_port_attr *)a;
    int dev = ((struct sctx *)c)->dev;
    if (port != 1) die("only port 1 exists");
    memset(p, 0, sizeof(*p));
    p->state = IBV_PORT_ACTIVE;
    p->max_mtu = p->active_mtu = IBV_MTU_4096;
    p->link_layer = is_tb(dev) ? 100 : IBV_LINK_LAYER_ETHERNET;
    p->lid = is_tb(dev) ? 1 : 0;
    return 0;
}
int ibv_query_gid(struct ibv_context *c, uint8_t port, int index, union ibv_gid *g) {
    (void)port, (void)index;
    memset(g, 0, sizeof(*g));
    g->raw[0] = 0xfe, g->raw[1] = 0x80, g->raw[15] = (uint8_t)(((struct sctx *)c)->dev + 1);
    return 0;
}

struct ibv_pd *ibv_alloc_pd(struct ibv_context *c) {
    struct spd *p = calloc(1, sizeof(*p));
    p->pd.context = c;
    p->dev = ((struct sctx *)c)->dev;
    return &p->pd;
}
int ibv_dealloc_pd(struct ibv_pd *p) {
    free(p);
    return 0;
}

struct ibv_mr *ibv_reg_mr_iova2(struct ibv_pd *pd, void *addr, size_t len, uint64_t iova, unsigned int access) {
    int dev = ((struct spd *)pd)->dev;
    (void)iova;
    pthread_mutex_lock(&g_mu);
    if (is_tb(dev) && (access != IBV_ACCESS_LOCAL_WRITE || len > TB_MR_MAX || g_mrs[dev] >= 100)) {
        pthread_mutex_unlock(&g_mu);
        errno = EINVAL;
        return NULL;
    }
    if (g_nmr == MAXMR) die("too many registrations for the stub");
    struct smr *m = calloc(1, sizeof(*m));
    m->mr.context = pd->context, m->mr.pd = pd, m->mr.addr = addr, m->mr.length = len;
    m->mr.lkey = m->mr.rkey = (uint32_t)(1000 + g_nmr);
    m->dev = dev, m->access = (int)access, m->live = 1;
    g_mr[g_nmr++] = m;
    g_mrs[dev]++;
    pthread_mutex_unlock(&g_mu);
    return &m->mr;
}
#undef ibv_reg_mr
struct ibv_mr *ibv_reg_mr(struct ibv_pd *pd, void *addr, size_t len, int access) {
    return ibv_reg_mr_iova2(pd, addr, len, (uintptr_t)addr, (unsigned)access);
}
int ibv_dereg_mr(struct ibv_mr *mr) {
    struct smr *m = (struct smr *)mr;
    pthread_mutex_lock(&g_mu);
    if (!m->live) die("double deregistration");
    uint64_t lo = (uint64_t)(uintptr_t)mr->addr, hi = lo + mr->length;
    for (int i = 0; i < g_nqp; ++i) {
        struct sqp *q = g_qp[i];
        for (int pass = 0; pass < 2 && !q->destroyed; ++pass) {
            struct sq *s = pass ? &q->recv : &q->send;
            for (uint32_t k = 0; k < s->count; ++k) {
                uint64_t a = (uint64_t)(uintptr_t)s->v[(s->head + k) % s->cap].addr;
                if (a >= lo && a < hi) die("deregistering memory that a posted request still uses");
            }
        }
    }
    m->live = 0;
    g_mrs[m->dev]--;
    pthread_mutex_unlock(&g_mu);
    return 0;
}

struct ibv_cq *ibv_create_cq(struct ibv_context *ctx, int cqe, void *cq_context, struct ibv_comp_channel *ch, int vec) {
    (void)cq_context, (void)ch, (void)vec;
    if (cqe > (is_tb(((struct sctx *)ctx)->dev) ? 4096 : 31) || cqe < 1) return NULL;
    struct scq *c = calloc(1, sizeof(*c));
    c->cq.context = ctx, c->cq.cqe = cqe, c->cap = cqe;
    c->ring = calloc((size_t)cqe, sizeof(*c->ring));
    return &c->cq;
}
int ibv_destroy_cq(struct ibv_cq *cq) {
    struct scq *c = (struct scq *)cq;
    pthread_mutex_lock(&g_mu);
    if (c->destroyed) die("double destroy of a CQ");
    for (int i = 0; i < g_nqp; ++i)
        if (!g_qp[i]->destroyed && (g_qp[i]->qp.send_cq == cq || g_qp[i]->qp.recv_cq == cq))
            die("destroying a CQ that a live QP still uses");
    c->destroyed = 1;
    pthread_mutex_unlock(&g_mu);
    return 0;
}

struct ibv_qp *ibv_create_qp(struct ibv_pd *pd, struct ibv_qp_init_attr *init) {
    int dev = ((struct spd *)pd)->dev;
    const char *depth = getenv("STUB_TB_DEPTH");
    uint32_t limit = depth ? (uint32_t)atoi(depth) : 4095, send = init->cap.max_send_wr, recv = init->cap.max_recv_wr;
    pthread_mutex_lock(&g_mu);
    if (is_tb(dev) ? init->qp_type != IBV_QPT_UC || init->cap.max_send_sge > 1 || init->cap.max_recv_sge > 1 ||
                         g_uc[dev] >= TB_QPS
                   : init->qp_type != IBV_QPT_RC || send > 31) {
        pthread_mutex_unlock(&g_mu);
        return NULL;
    }
    if (g_nqp == MAXQP) die("too many queue pairs for the stub");
    if (is_tb(dev)) {
        send = send < limit ? send : limit, recv = recv < limit ? recv : limit;
        g_uc[dev]++;
    }
    struct sqp *q = calloc(1, sizeof(*q));
    q->qp.context = pd->context, q->qp.pd = pd, q->qp.send_cq = init->send_cq, q->qp.recv_cq = init->recv_cq;
    q->qp.qp_num = (uint32_t)(100 + g_nqp), q->qp.qp_type = init->qp_type, q->qp.state = IBV_QPS_RESET;
    q->dev = dev, q->cap_send = send, q->cap_recv = recv ? recv : 1;
    q->send.cap = q->cap_send, q->recv.cap = q->cap_recv;
    q->send.v = calloc(q->send.cap, sizeof(struct swr));
    q->recv.v = calloc(q->recv.cap, sizeof(struct swr));
    g_qp[g_nqp++] = q;
    pthread_mutex_unlock(&g_mu);
    return &q->qp;
}
int ibv_query_qp(struct ibv_qp *qp, struct ibv_qp_attr *a, int mask, struct ibv_qp_init_attr *init) {
    struct sqp *q = (struct sqp *)qp;
    (void)mask;
    a->qp_state = qp->state;
    a->cap.max_send_wr = init->cap.max_send_wr = q->cap_send;
    a->cap.max_recv_wr = init->cap.max_recv_wr = q->cap_recv;
    a->cap.max_send_sge = a->cap.max_recv_sge = init->cap.max_send_sge = init->cap.max_recv_sge = 1;
    return 0;
}
int ibv_modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *a, int mask) {
    struct sqp *q = (struct sqp *)qp;
    pthread_mutex_lock(&g_mu);
    if (q->destroyed) die("modify_qp on a destroyed QP");
    int refused = 0;
    if ((mask & IBV_QP_STATE) && qp->qp_type == IBV_QPT_UC) {
        /* the attributes TN3205 shows, and none that only RC has */
        if (a->qp_state == IBV_QPS_INIT && a->qp_access_flags) refused = 1;
        if (a->qp_state == IBV_QPS_RTR &&
            ((mask & (IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) || !(mask & IBV_QP_AV) ||
             !(mask & IBV_QP_DEST_QPN) || a->dest_qp_num < 100 || a->dest_qp_num - 100 >= (uint32_t)g_nqp ||
             !is_tb(g_qp[a->dest_qp_num - 100]->dev) || !a->ah_attr.is_global || a->ah_attr.grh.hop_limit != 1))
            refused = 1;
        if (a->qp_state == IBV_QPS_RTS &&
            (mask & (IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC)))
            refused = 1;
    }
    if (!refused && (mask & IBV_QP_STATE)) {
        if (a->qp_state == IBV_QPS_RTR) {
            q->dest = a->dest_qp_num;
            if (getenv("STUB_FAIL_RTR")) refused = 1;
        }
        if (!refused) {
            qp->state = a->qp_state;
            if (a->qp_state == IBV_QPS_ERR) flush(q);
        }
    }
    pthread_mutex_unlock(&g_mu);
    return refused ? EINVAL : 0;
}
int ibv_destroy_qp(struct ibv_qp *qp) {
    struct sqp *q = (struct sqp *)qp;
    pthread_mutex_lock(&g_mu);
    if (q->destroyed) die("double destroy of a QP");
    if (q->send.count || q->recv.count) die("destroying a QP with requests posted; move it to the error state first");
    q->destroyed = 1;
    if (is_tb(q->dev)) g_uc[q->dev]--;
    pthread_mutex_unlock(&g_mu);
    return 0;
}

/* A DMA-BUF registration double: the bytes are the CPU mapping at iova; the descriptor and first offset are kept. */
int stub_dmabuf_fd = -1, stub_dmabuf_count;
uint64_t stub_dmabuf_offset;
struct ibv_mr *ibv_reg_dmabuf_mr(struct ibv_pd *pd, uint64_t offset, size_t length, uint64_t iova, int fd, int access) {
    if (fd < 0) {
        errno = EBADF;
        return NULL;
    }
    if (!stub_dmabuf_count++) stub_dmabuf_offset = offset;
    stub_dmabuf_fd = fd;
    return ibv_reg_mr_iova2(pd, (void *)(uintptr_t)iova, length, iova, (unsigned)access);
}

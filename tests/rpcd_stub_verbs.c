/* Offline stand-in for the verbs library, used only by the link-daemon tests: no device is touched.
 * WRITE and READ are memcpy within the test process (skipped with STUB_NOCOPY=1, for peers that exist only as
 * addresses), every work request completes successfully, and misuse fails loudly: posting before RTS, posting
 * or polling on a destroyed object, and destroying a QP or CQ twice. STUB_DESTROY_US slows QP destruction. A
 * violation exits 86 rather than aborting, so no crash report is raised. */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#else
#define _GNU_SOURCE 1
#endif
#include <infiniband/verbs.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct scq { struct ibv_cq cq; int pending; int destroyed; pthread_mutex_t mu; };
struct sqp { struct ibv_qp qp; int destroyed; int st; };

static struct ibv_device g_dev[2];
static int g_qpn = 100, g_key = 1000;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static void die(const char *what) {
    fprintf(stderr, "STUB-VIOLATION: %s\n", what);
    fflush(stderr);
    _exit(86);
}

static int stub_poll(struct ibv_cq *cq, int n, struct ibv_wc *wc) {
    struct scq *c = (struct scq *)cq;
    if (c->destroyed) die("poll_cq on a destroyed CQ");
    (void)n;
    pthread_mutex_lock(&c->mu);
    int got = 0;
    if (c->pending > 0) {
        c->pending--;
        memset(wc, 0, sizeof(*wc));
        wc->status = IBV_WC_SUCCESS;
        got = 1;
    }
    pthread_mutex_unlock(&c->mu);
    return got;
}

static int stub_post(struct ibv_qp *qp, struct ibv_send_wr *wr, struct ibv_send_wr **bad) {
    struct sqp *q = (struct sqp *)qp;
    (void)bad;
    if (q->destroyed) die("post_send on a destroyed QP");
    if (q->st != IBV_QPS_RTS) die("post_send before RTS");
    struct scq *c = (struct scq *)qp->send_cq;
    if (c->destroyed) die("post_send with a destroyed CQ");
    int nocopy = getenv("STUB_NOCOPY") != NULL;
    for (; wr; wr = wr->next) {
        void *l = (void *)(uintptr_t)wr->sg_list[0].addr, *r = (void *)(uintptr_t)wr->wr.rdma.remote_addr;
        if (!nocopy) {
            if (wr->opcode == IBV_WR_RDMA_WRITE) memcpy(r, l, wr->sg_list[0].length);
            else if (wr->opcode == IBV_WR_RDMA_READ) memcpy(l, r, wr->sg_list[0].length);
        }
        pthread_mutex_lock(&c->mu);
        c->pending++;
        pthread_mutex_unlock(&c->mu);
    }
    return 0;
}

struct ibv_device **ibv_get_device_list(int *n) {
    snprintf(g_dev[0].name, sizeof(g_dev[0].name), "stub0");
    snprintf(g_dev[1].name, sizeof(g_dev[1].name), "stub1");
    struct ibv_device **l = calloc(3, sizeof(*l));
    l[0] = &g_dev[0];
    l[1] = &g_dev[1];
    *n = 2;
    return l;
}
void ibv_free_device_list(struct ibv_device **l) { free(l); }
const char *ibv_get_device_name(struct ibv_device *d) { return d->name; }
struct ibv_context *ibv_open_device(struct ibv_device *d) {
    struct ibv_context *c = calloc(1, sizeof(*c));
    c->device = d;
    c->ops.poll_cq = stub_poll;
    c->ops.post_send = stub_post;
    return c;
}
int ibv_close_device(struct ibv_context *c) { free(c); return 0; }
#undef ibv_query_port
int ibv_query_port(struct ibv_context *c, uint8_t port, struct _compat_ibv_port_attr *a) {
    (void)c; (void)port;
    struct ibv_port_attr *p = (struct ibv_port_attr *)a;
    p->state = IBV_PORT_ACTIVE;
    p->active_mtu = IBV_MTU_4096;
    return 0;
}
int ibv_query_gid(struct ibv_context *c, uint8_t port, int index, union ibv_gid *g) {
    (void)c; (void)port; (void)index;
    memset(g, 0, sizeof(*g));
    g->raw[0] = 0xfe; g->raw[1] = 0x80; g->raw[15] = 1;
    return 0;
}
struct ibv_pd *ibv_alloc_pd(struct ibv_context *c) { struct ibv_pd *p = calloc(1, sizeof(*p)); p->context = c; return p; }
int ibv_dealloc_pd(struct ibv_pd *p) { free(p); return 0; }
struct ibv_mr *ibv_reg_mr_iova2(struct ibv_pd *pd, void *addr, size_t len, uint64_t iova, unsigned int access) {
    (void)iova; (void)access;
    struct ibv_mr *m = calloc(1, sizeof(*m));
    m->context = pd->context; m->pd = pd; m->addr = addr; m->length = len;
    pthread_mutex_lock(&g_mu);
    m->lkey = m->rkey = (uint32_t)g_key++;
    pthread_mutex_unlock(&g_mu);
    return m;
}
#undef ibv_reg_mr
struct ibv_mr *ibv_reg_mr(struct ibv_pd *pd, void *addr, size_t len, int access) {
    return ibv_reg_mr_iova2(pd, addr, len, (uintptr_t)addr, (unsigned)access);
}
int ibv_dereg_mr(struct ibv_mr *m) { free(m); return 0; }
struct ibv_cq *ibv_create_cq(struct ibv_context *ctx, int cqe, void *cq_context, struct ibv_comp_channel *ch, int vec) {
    (void)cq_context; (void)ch; (void)vec;
    if (cqe > 31) return NULL;
    struct scq *c = calloc(1, sizeof(*c));
    pthread_mutex_init(&c->mu, NULL);
    c->cq.context = ctx;
    c->cq.cqe = cqe;
    return &c->cq;
}
int ibv_destroy_cq(struct ibv_cq *cq) {
    struct scq *c = (struct scq *)cq;
    if (c->destroyed) die("double destroy of a CQ");
    c->destroyed = 1;          /* never freed, so later use is detected */
    return 0;
}
struct ibv_qp *ibv_create_qp(struct ibv_pd *pd, struct ibv_qp_init_attr *init) {
    if (init->qp_type != IBV_QPT_RC || init->cap.max_send_wr > 31) return NULL;
    struct sqp *q = calloc(1, sizeof(*q));
    q->qp.context = pd->context; q->qp.pd = pd; q->qp.send_cq = init->send_cq; q->qp.recv_cq = init->recv_cq;
    pthread_mutex_lock(&g_mu);
    q->qp.qp_num = (uint32_t)g_qpn++;
    pthread_mutex_unlock(&g_mu);
    q->st = IBV_QPS_RESET;
    return &q->qp;
}
int ibv_modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *a, int mask) {
    struct sqp *q = (struct sqp *)qp;
    if (q->destroyed) die("modify_qp on a destroyed QP");
    if (mask & IBV_QP_STATE) {
        if (a->qp_state == IBV_QPS_RTR && getenv("STUB_FAIL_RTR")) return 22;
        q->st = a->qp_state;
    }
    return 0;
}
int ibv_destroy_qp(struct ibv_qp *qp) {
    struct sqp *q = (struct sqp *)qp;
    if (q->destroyed) die("double destroy of a QP");
    const char *us = getenv("STUB_DESTROY_US");
    if (us) usleep((useconds_t)atoi(us));
    if (q->destroyed) die("double destroy of a QP (concurrent)");
    q->destroyed = 1;
    return 0;
}
int ibv_query_qp(struct ibv_qp *qp, struct ibv_qp_attr *a, int mask, struct ibv_qp_init_attr *init) {
    (void)qp; (void)mask;
    a->cap.max_send_wr = init->cap.max_send_wr = 31;
    a->cap.max_recv_wr = init->cap.max_recv_wr = 1;
    return 0;
}
const char *ibv_wc_status_str(enum ibv_wc_status status) { return status == IBV_WC_SUCCESS ? "success" : "failure"; }

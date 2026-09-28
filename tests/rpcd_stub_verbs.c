/* Offline stand-in for the verbs library, used only by the link-daemon tests: no device is touched.
 * WRITE and READ are memcpy within the test process (skipped with STUB_NOCOPY=1, for peers that exist only as
 * addresses), every work request completes successfully, and misuse aborts loudly: posting before RTS, posting
 * or polling on a destroyed object, and destroying a QP or CQ twice. STUB_DESTROY_US slows QP destruction. */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#else
#define _GNU_SOURCE 1
#endif
#include <infiniband/verbs.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(MCDMA_RPC_TEST_DMABUF) && defined(__APPLE__)
#include <poll.h>
#include <sys/ioctl.h>
/* Darwin reports empty FIFO EOF as readable as well as hung up. The FD mode
 * exists only on Linux in production; emulate Linux poll events on Mac tests,
 * preserving POLLIN whenever an actual byte remains. This never consumes data
 * or changes flags on the caller's descriptor. */
short mcdma_rpc_test_parent_events(int fd, short events) {
    int pending = -1;
    if ((events & (POLLIN | POLLHUP)) == (POLLIN | POLLHUP) && !ioctl(fd, FIONREAD, &pending) && pending == 0)
        events &= ~POLLIN;
    return events;
}
#endif

struct scq { struct ibv_cq cq; int pending; int destroyed; int qps; pthread_mutex_t mu; };
struct sqp { struct ibv_qp qp; int destroyed; int st; };
struct spd { struct ibv_pd pd; int mrs, qps; };
struct smr { struct ibv_mr mr; int dmabuf_fd; };

static struct ibv_device g_dev[2];
static int g_qpn = 100, g_key = 1000;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static void die(const char *what) {
    fprintf(stderr, "STUB-VIOLATION: %s\n", what);
    fflush(stderr);
    abort();
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
int ibv_close_device(struct ibv_context *c) {
    if (getenv("STUB_FAIL_CLOSE_DEVICE")) return EBUSY;
    free(c); return 0;
}
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
struct ibv_pd *ibv_alloc_pd(struct ibv_context *c) { struct spd *p = calloc(1, sizeof(*p)); p->pd.context = c; return &p->pd; }
int ibv_dealloc_pd(struct ibv_pd *pd) {
    struct spd *p = (struct spd *)pd;
    if (p->mrs || p->qps) die("deallocate PD with retained MR/QP");
    if (getenv("STUB_FAIL_DEALLOC_PD")) return EBUSY;
    free(p); return 0;
}
static struct ibv_mr *make_mr(struct ibv_pd *pd, void *addr, size_t len, int fd) {
    struct smr *storage = calloc(1, sizeof(*storage));
    struct ibv_mr *m = &storage->mr;
    storage->dmabuf_fd = fd;
    m->context = pd->context; m->pd = pd; m->addr = addr; m->length = len;
    ((struct spd *)pd)->mrs++;
    pthread_mutex_lock(&g_mu);
    m->lkey = m->rkey = (uint32_t)g_key++;
    pthread_mutex_unlock(&g_mu);
    return m;
}
struct ibv_mr *ibv_reg_mr_iova2(struct ibv_pd *pd, void *addr, size_t len, uint64_t iova, unsigned int access) {
    (void)iova; (void)access;
    if (getenv("STUB_FORBID_HOST_REG")) die("unexpected host-memory registration fallback");
    return make_mr(pd, addr, len, -1);
}
#undef ibv_reg_mr
struct ibv_mr *ibv_reg_mr(struct ibv_pd *pd, void *addr, size_t len, int access) {
    return ibv_reg_mr_iova2(pd, addr, len, (uintptr_t)addr, (unsigned)access);
}
int ibv_dereg_mr(struct ibv_mr *m) {
    struct smr *storage = (struct smr *)m;
    if (((struct spd *)m->pd)->qps) die("deregister MR while a QP is retained");
    if (storage->dmabuf_fd >= 0) {
        if (fcntl(storage->dmabuf_fd, F_GETFD) < 0) die("DMA-BUF descriptor closed before MR deregistration");
        volatile unsigned char first = *(volatile unsigned char *)m->addr;
        (void)first; /* A premature unmap must fail this test rather than pass. */
        fprintf(stderr, "STUB_DMABUF_DEREG fd_alive=1 mapping_alive=1\n");
    }
    if (getenv("STUB_FAIL_DEREG_MR")) return EBUSY;
    ((struct spd *)m->pd)->mrs--;
    free(storage); return 0;
}
#ifdef MCDMA_RPC_TEST_DMABUF
/* A regular temporary file substitutes for a DMA-BUF in offline ownership
 * tests. This function does not claim hardware import or GPU compatibility. */
struct ibv_mr *ibv_reg_dmabuf_mr(struct ibv_pd *pd, uint64_t offset, size_t len, uint64_t iova, int fd, int access) {
    (void)access;
    static int calls;
    calls++;
    long page = sysconf(_SC_PAGESIZE);
    off_t size = lseek(fd, 0, SEEK_END);
    if (size < 0 || offset > (uint64_t)size || len > (uint64_t)size - offset ||
        page <= 0 || iova % (uint64_t)page != offset % (uint64_t)page) die("invalid DMA-BUF registration geometry");
    const char *fail_at = getenv("STUB_FAIL_DMABUF_AT");
    if (fail_at && calls == atoi(fail_at)) { errno = ENOTSUP; return NULL; }
    fprintf(stderr, "STUB_DMABUF_REG offset=%llu length=%zu\n", (unsigned long long)offset, len);
    return make_mr(pd, (void *)(uintptr_t)iova, len, fd);
}
#endif
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
    if (c->qps) die("destroy CQ while a QP is retained");
    if (getenv("STUB_FAIL_DESTROY_CQ")) return EBUSY;
    c->destroyed = 1;          /* never freed, so later use is detected */
    return 0;
}
struct ibv_qp *ibv_create_qp(struct ibv_pd *pd, struct ibv_qp_init_attr *init) {
    if (init->qp_type != IBV_QPT_RC || init->cap.max_send_wr > 31) return NULL;
    struct sqp *q = calloc(1, sizeof(*q));
    q->qp.context = pd->context; q->qp.pd = pd; q->qp.send_cq = init->send_cq; q->qp.recv_cq = init->recv_cq;
    ((struct spd *)pd)->qps++;
    ((struct scq *)init->send_cq)->qps++;
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
        if (a->qp_state == IBV_QPS_ERR && getenv("STUB_FAIL_QP_ERROR")) return EBUSY;
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
    if (getenv("STUB_FAIL_DESTROY_QP")) return EBUSY;
    q->destroyed = 1;
    ((struct spd *)qp->pd)->qps--;
    ((struct scq *)qp->send_cq)->qps--;
    return 0;
}

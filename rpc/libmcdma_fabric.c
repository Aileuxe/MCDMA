/* libmcdma-fabric: one-sided writes between registered windows (mcdma_fabric.h), on the link code mcdma-rpcd uses.
 * Each peer owns its queue pairs on the device's context and protection domain, where the window is registered once. */
#include "link.h"
#include "mcdma_fabric.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__linux__) && !defined(MCDMA_FABRIC_TEST_DMABUF)
#include <dlfcn.h>
#endif

#define MAX_PEERS 16
#define SLOTS 2048                  /* RoCE signal words: far more than the requests a queue pair holds in flight */
#define RESEND_NS 100000000ull      /* an unanswered offer is sent again this often */
#define SERVICE_NS 1000000ull       /* exchange sockets are read at most this often while progressing */
#define OP_NS 10000000000ull        /* a write or signal that cannot be posted within this fails its peer */
#define PIECE (4ull << 20)
#define READ_PIECE (2ull << 20)     /* the Mac provider's proven READ size, one outstanding at a time */
#ifdef __APPLE__
#define ROCE_WINDOW 4
#else
#define ROCE_WINDOW 16
#endif

struct mcdma_fabric {
    struct ep dev;                  /* the device, protection domain and the window's registrations */
    struct region win;
    pthread_mutex_t lock;
    pthread_t thread;
    int threaded, wanted;           /* callers waiting for the lock; the progress thread steps aside for them */
    volatile int stop;
    uint64_t serviced;
    struct mcdma_fabric_peer *peers[MAX_PEERS];
    int npeers;
};

struct mcdma_fabric_peer {
    struct mcdma_fabric *f;
    struct ep e;                    /* borrows the device's context and domain; owns its queue pairs */
    struct xchg x;
    struct xmsg mine;               /* our offer, sent again whenever the peer offers again */
    struct table remote;
    uint64_t remote_length;
    uint64_t *slots;                /* RoCE: each signal value stays put until its write completes */
    struct ibv_mr *slots_mr;
    uint64_t signals;
    int down;
};

void link_log(const char *fmt, ...) {
    static int quiet = -1;
    if (quiet < 0) quiet = getenv("MCDMA_FABRIC_LOG") && !strcmp(getenv("MCDMA_FABRIC_LOG"), "0");
    if (quiet) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "mcdma-fabric: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

uint32_t mcdma_fabric_abi(void) { return MCDMA_FABRIC_ABI; }

static int valid_name(const char *name) {
    size_t n = strlen(name);
    for (size_t i = 0; i < n; ++i)
        if (!((name[i] >= 'A' && name[i] <= 'Z') || (name[i] >= 'a' && name[i] <= 'z') ||
              (name[i] >= '0' && name[i] <= '9') || name[i] == '_' || name[i] == '-'))
            return 0;
    return n >= 1 && n <= 20;
}

typedef struct ibv_mr *(*dmabuf_fn)(struct ibv_pd *, uint64_t, size_t, uint64_t, int, int);

/* Linux registers DMA-BUFs through libibverbs' ibv_reg_dmabuf_mr when it has one; there is no fallback. */
static dmabuf_fn dmabuf_register(void) {
#if defined(MCDMA_FABRIC_TEST_DMABUF)
    extern struct ibv_mr *ibv_reg_dmabuf_mr(struct ibv_pd *, uint64_t, size_t, uint64_t, int, int);
    return ibv_reg_dmabuf_mr;
#elif defined(__linux__)
    return (dmabuf_fn)dlsym(RTLD_DEFAULT, "ibv_reg_dmabuf_mr");
#else
    return NULL;
#endif
}

/* Thunderbolt: 12 MiB pieces, local access only. RoCE: 4 MiB pieces on a Mac, one registration on Linux. */
static int register_window(struct mcdma_fabric *f, void *window, uint64_t length, int fd, uint64_t offset) {
    struct ep *e = &f->dev;
    int tb = e->kind == LINK_TB, access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
#ifdef __APPLE__
    uint64_t seg = tb ? TB_SEG : LINK_SEG;
#else
    uint64_t seg = tb ? TB_SEG : 0;
#endif
    if (fd < 0) return ep_reg_region(e, &f->win, window, length, seg, length, access, access) ? MCDMA_FABRIC_DEVICE : 0;
    dmabuf_fn reg = dmabuf_register();
    if (!reg || tb) {
        link_log("%s: DMA-BUF registration is unavailable here; no fallback", e->device);
        return MCDMA_FABRIC_UNSUPPORTED;
    }
    struct region *r = &f->win;
    memset(r, 0, sizeof(*r));
    r->base = window, r->length = length, r->seg = seg ? seg : length, r->split = length;
    r->below = r->above = access;
    for (uint64_t off = 0; off < length; off += r->seg) {
        uint64_t len = length - off < r->seg ? length - off : r->seg;
        struct ibv_mr *mr = r->n < LINK_REGION_MAX && e->nmr < LINK_MRS
                                ? reg(e->pd, offset + off, len, (uint64_t)(uintptr_t)r->base + off, fd, access)
                                : NULL;
        if (!mr) {
            link_log("%s: DMA-BUF registration at %llu failed (errno %d); no fallback", e->device,
                     (unsigned long long)off, errno);
            return MCDMA_FABRIC_DEVICE;
        }
        r->mr[r->n++] = e->mr[e->nmr++] = mr;
    }
    return 0;
}

/* Tear one peer down; its queue pairs are its own, the context and domain are the device's. */
static void release_peer(struct mcdma_fabric_peer *p) {
    static const uint8_t zero[16];
    if (p->x.fd >= 0 && memcmp(p->x.peer_session, zero, 16)) {
        struct xmsg bye;
        xchg_prepare(&p->x, &bye, X_BYE, ROLE_PEER, 0);
        (void)xchg_send(&p->x, &bye);
    }
    ep_destroy_qp(&p->e);
    if (p->slots_mr) ibv_dereg_mr(p->slots_mr);
    free(p->slots);
    xchg_close(&p->x);
    free(p);
}

/* Read each peer's exchange socket: offers again after ours was lost, goodbyes, a restarted peer. */
static void service(struct mcdma_fabric_peer *p) {
    struct xmsg m;
    while (!p->down && xchg_recv(&p->x, &m, 0) == 1) {
        if (m.role != ROLE_PEER) continue;
        int current = !memcmp(m.from, p->x.peer_session, 16);
        if ((m.kind == X_BYE || m.kind == X_ERR) && xchg_ours(&p->x, &m)) {
            link_log("%s: the peer ended the link (%s)", p->x.name, m.kind == X_ERR ? m.text : "goodbye");
            p->down = 1;
        } else if (m.kind == X_OFFER && current) {
            (void)xchg_send(&p->x, &p->mine);
        } else if (m.kind == X_OFFER) {
            link_log("%s: the peer restarted; this link is gone", p->x.name);
            p->down = 1;
        }
    }
}

/* Under the lock: place Thunderbolt writes, reap RoCE completions, and every millisecond read exchange sockets. */
static int progress_all(struct mcdma_fabric *f) {
    int handled = 0, read_sockets = link_now_ns() - f->serviced > SERVICE_NS;
    if (read_sockets) f->serviced = link_now_ns();
    for (int i = 0; i < f->npeers; ++i) {
        struct mcdma_fabric_peer *p = f->peers[i];
        if (p->down) continue;
        int n = p->e.kind == LINK_TB ? tb_progress(&p->e) : ep_reap(&p->e, 0, 0);
        if (n < 0) p->down = 1;
        else handled += n;
        if (read_sockets) service(p);
    }
    return handled;
}

/* Callers take the lock through this, so the progress thread's tight loop never starves them. */
static void enter(struct mcdma_fabric *f) {
    __atomic_add_fetch(&f->wanted, 1, __ATOMIC_ACQ_REL);
    pthread_mutex_lock(&f->lock);
    __atomic_sub_fetch(&f->wanted, 1, __ATOMIC_ACQ_REL);
}

static void *progress_main(void *arg) {
    struct mcdma_fabric *f = arg;
    uint64_t active = link_now_ns();
    while (!f->stop) {
        if (__atomic_load_n(&f->wanted, __ATOMIC_ACQUIRE) || pthread_mutex_trylock(&f->lock)) continue;
        int handled = progress_all(f);
        pthread_mutex_unlock(&f->lock);
        if (handled) active = link_now_ns();
        /* idle for a while: back off, as the daemons do */
        if (link_now_ns() - active > 50000000ull) usleep(50);
    }
    return NULL;
}

int mcdma_fabric_open(const char *device, int gid_index, int path_mtu, void *window, size_t length, int dmabuf_fd,
                      uint64_t dmabuf_offset, uint32_t flags, struct mcdma_fabric **out) {
    long page = sysconf(_SC_PAGESIZE);
    if (!out) return MCDMA_FABRIC_INVALID;
    *out = NULL;
    if (!device || !window || !length || page <= 0 || (uintptr_t)window % (uintptr_t)page || length % (size_t)page ||
        (flags & ~MCDMA_FABRIC_PROGRESS_THREAD))
        return MCDMA_FABRIC_INVALID;
    struct mcdma_fabric *f = calloc(1, sizeof(*f));
    if (!f) return MCDMA_FABRIC_NOMEM;
    pthread_mutex_init(&f->lock, NULL);
    int status = ep_open(&f->dev, device, gid_index, path_mtu) ? MCDMA_FABRIC_DEVICE : 0;
    if (!status) status = register_window(f, window, length, dmabuf_fd, dmabuf_offset);
    if (!status && (flags & MCDMA_FABRIC_PROGRESS_THREAD)) {
        f->threaded = !pthread_create(&f->thread, NULL, progress_main, f);
        if (!f->threaded) status = MCDMA_FABRIC_NOMEM;
    }
    if (status) {
        ep_close(&f->dev);
        pthread_mutex_destroy(&f->lock);
        free(f);
        return status;
    }
    *out = f;
    return MCDMA_FABRIC_OK;
}

/* Both sides offer until each holds the other's offer and has heard that the other holds its own. */
static int exchange(struct mcdma_fabric_peer *p, uint64_t timeout_ns) {
    struct mcdma_fabric *f = p->f;
    struct xmsg got;
    xchg_session(&p->x);
    xchg_prepare(&p->x, &p->mine, X_OFFER, ROLE_PEER, 0);
    ep_info(&p->e, &f->win, &p->mine.info);
    p->mine.info.mode = MODE_DIRECT, p->mine.info.req = f->win.length, p->mine.info.rep = 0;
    uint64_t deadline = link_now_ns() + timeout_ns, resend = 0;
    int have = 0, heard = 0;
    while (!(have && heard)) {
        uint64_t now = link_now_ns();
        if (now > deadline) return MCDMA_FABRIC_TIMEOUT;
        if (now >= resend) {
            (void)xchg_send(&p->x, &p->mine);
            resend = now + RESEND_NS;
        }
        int r = xchg_recv(&p->x, &got, 10);
        if (r < 0) return MCDMA_FABRIC_DEVICE;
        if (r == 0 || got.role != ROLE_PEER) continue;
        if (got.kind == X_ERR && xchg_ours(&p->x, &got)) {
            link_log("%s: the peer refused: %s", p->x.name, got.text);
            return MCDMA_FABRIC_PEER;
        }
        if (got.kind != X_OFFER || (have && memcmp(got.from, p->x.peer_session, 16))) continue;
        if (!have) {
            const struct xinfo *i = &got.info;
            const char *why = i->transport != p->e.kind ? "the two ends use different link kinds"
                              : i->rep || i->req < 8 || !table_valid(&i->table, i->req, i->transport == LINK_ROCE)
                                  ? "a malformed window"
                                  : NULL;
            if (why || ep_connect(&p->e, i)) {
                struct xmsg err;
                xchg_prepare(&p->x, &err, X_ERR, ROLE_PEER, 0);
                memcpy(err.to, got.from, 16);
                snprintf(err.text, sizeof(err.text), "%s", why ? why : "queue pair refused RTR or RTS");
                (void)xchg_send(&p->x, &err);
                link_log("%s: cannot connect: %s", p->x.name, err.text);
                return why ? MCDMA_FABRIC_PEER : MCDMA_FABRIC_DEVICE;
            }
            memcpy(p->x.peer_session, got.from, 16);
            p->remote = i->table;
            p->remote_length = i->req;
            have = 1;
            /* now at RTS: say so, addressed to this session */
            p->mine.flags = X_HAVE;
            memcpy(p->mine.to, got.from, 16);
            (void)xchg_send(&p->x, &p->mine);
            resend = link_now_ns() + RESEND_NS;
        }
        if ((got.flags & X_HAVE) && xchg_ours(&p->x, &got)) heard = 1;
    }
    return MCDMA_FABRIC_OK;
}

int mcdma_fabric_connect(struct mcdma_fabric *f, const char *via, int port, int peer_port, const char *name,
                         uint64_t timeout_ns, struct mcdma_fabric_peer **out) {
    if (!out) return MCDMA_FABRIC_INVALID;
    *out = NULL;
    if (!f || !via || !name || !valid_name(name) || port <= 0 || port > 65535 || peer_port < 0 || peer_port > 65535)
        return MCDMA_FABRIC_INVALID;
    struct mcdma_fabric_peer *p = calloc(1, sizeof(*p));
    if (!p) return MCDMA_FABRIC_NOMEM;
    p->f = f;
    p->x.fd = -1;
    p->e = f->dev;
    p->e.qp = NULL, p->e.cq = NULL, p->e.tb = NULL, p->e.nmr = 0, p->e.outstanding = 0;
    memset(p->e.mr, 0, sizeof(p->e.mr));
    int status = via_check(via, &f->dev) || xchg_open(&p->x, via, port, peer_port, name) ? MCDMA_FABRIC_INVALID
                 : ep_create_qp(&p->e)                       ? MCDMA_FABRIC_DEVICE
                                                             : MCDMA_FABRIC_OK;
    if (!status && p->e.kind == LINK_TB) tb_accept(&p->e, &f->win, 0, f->win.length);
    if (!status && p->e.kind == LINK_ROCE) {
        void *slots = NULL;
        if (posix_memalign(&slots, 16384, SLOTS * sizeof(uint64_t))) status = MCDMA_FABRIC_NOMEM;
        p->slots = slots;
        if (!status && !(p->slots_mr = ibv_reg_mr(p->e.pd, slots, SLOTS * sizeof(uint64_t), IBV_ACCESS_LOCAL_WRITE)))
            status = MCDMA_FABRIC_DEVICE;
    }
    if (!status) status = exchange(p, timeout_ns);
    enter(f);
    if (!status && f->npeers == MAX_PEERS) status = MCDMA_FABRIC_NOMEM;
    if (!status) f->peers[f->npeers++] = p;
    pthread_mutex_unlock(&f->lock);
    if (status) {
        release_peer(p);
        return status;
    }
    *out = p;
    return MCDMA_FABRIC_OK;
}

int mcdma_fabric_link(const struct mcdma_fabric_peer *p) {
    return !p ? 0 : p->e.kind == LINK_TB ? MCDMA_FABRIC_THUNDERBOLT : MCDMA_FABRIC_ROCE;
}

uint64_t mcdma_fabric_peer_length(const struct mcdma_fabric_peer *p) { return p ? p->remote_length : 0; }

static int in(uint64_t off, uint64_t len, uint64_t limit) { return off <= limit && len <= limit - off; }

/* Post one RoCE request, first reaping one completion if the send queue is full. */
static int roce_post(struct mcdma_fabric_peer *p, enum ibv_wr_opcode op, const struct piece *piece) {
    if (p->e.outstanding >= ROCE_WINDOW && ep_reap(&p->e, 1, OP_NS) < 0) return -1;
    return ep_post(&p->e, op, piece);
}

static int roce_range(struct mcdma_fabric_peer *p, enum ibv_wr_opcode op, uint64_t off, uint64_t roff, uint64_t len) {
    const struct region *win = &p->f->win;
    uint64_t max = op == IBV_WR_RDMA_READ ? READ_PIECE : PIECE;
    struct piece piece;
    while (len) {
        /* one piece at a time, inside one registration on each side */
        uint64_t take = len < max ? len : max, lroom = win->seg - off % win->seg;
        uint64_t rroom = p->remote.seg - roff % p->remote.seg;
        take = take < lroom ? take : lroom;
        take = take < rroom ? take : rroom;
        if (cut(win, off, take, &p->remote, roff, max, &piece, 1) != 1) return -1;
        if (op == IBV_WR_RDMA_READ ? post_pieces(&p->e, op, &piece, 1, 1, OP_NS) : roce_post(p, op, &piece)) return -1;
        off += take, roff += take, len -= take;
    }
    return 0;
}

static int roce_signal(struct mcdma_fabric_peer *p, uint64_t roff, uint64_t value) {
    uint64_t *slot = &p->slots[p->signals++ % SLOTS];
    *slot = value;
    struct piece piece = {slot, p->slots_mr->lkey, p->remote.base + roff, p->remote.rkey[roff / p->remote.seg], 8};
    return roce_post(p, IBV_WR_RDMA_WRITE, &piece);
}

/* Called with the lock held: a failed operation leaves the peer unusable. */
static int settle(struct mcdma_fabric_peer *p, int failed) {
    if (failed) p->down = 1;
    pthread_mutex_unlock(&p->f->lock);
    return failed ? MCDMA_FABRIC_PEER : MCDMA_FABRIC_OK;
}

int mcdma_fabric_write(struct mcdma_fabric_peer *p, uint64_t local_offset, uint64_t remote_offset, uint64_t length) {
    if (!p) return MCDMA_FABRIC_INVALID;
    if (!in(local_offset, length, p->f->win.length) || !in(remote_offset, length, p->remote_length))
        return MCDMA_FABRIC_BOUNDS;
    enter(p->f);
    int failed = p->down || (length && (p->e.kind == LINK_TB ? tb_write(&p->e, &p->f->win, local_offset, remote_offset,
                                                                         length, OP_NS)
                                                             : roce_range(p, IBV_WR_RDMA_WRITE, local_offset,
                                                                          remote_offset, length)));
    return settle(p, failed);
}

int mcdma_fabric_signal(struct mcdma_fabric_peer *p, uint64_t remote_offset, uint64_t value) {
    if (!p) return MCDMA_FABRIC_INVALID;
    if (remote_offset % 8 || !in(remote_offset, 8, p->remote_length)) return MCDMA_FABRIC_BOUNDS;
    enter(p->f);
    int failed = p->down || (p->e.kind == LINK_TB ? tb_signal(&p->e, remote_offset, value, OP_NS)
                                                  : roce_signal(p, remote_offset, value));
    return settle(p, failed);
}

int mcdma_fabric_read(struct mcdma_fabric_peer *p, uint64_t local_offset, uint64_t remote_offset, uint64_t length) {
    if (!p) return MCDMA_FABRIC_INVALID;
    if (p->e.kind == LINK_TB) return MCDMA_FABRIC_UNSUPPORTED;
    if (!in(local_offset, length, p->f->win.length) || !in(remote_offset, length, p->remote_length))
        return MCDMA_FABRIC_BOUNDS;
    enter(p->f);
    return settle(p, p->down || roce_range(p, IBV_WR_RDMA_READ, local_offset, remote_offset, length));
}

int mcdma_fabric_fetch_add(struct mcdma_fabric_peer *p, uint64_t remote_offset, uint64_t add, uint64_t *old) {
    (void)remote_offset, (void)add;
    if (old) *old = 0;
    return p ? MCDMA_FABRIC_UNSUPPORTED : MCDMA_FABRIC_INVALID;
}

int mcdma_fabric_flush(struct mcdma_fabric_peer *p, uint64_t timeout_ns) {
    if (!p) return MCDMA_FABRIC_INVALID;
    enter(p->f);
    int failed = p->down || (p->e.kind == LINK_TB ? tb_fence(&p->e, timeout_ns)
                                                  : ep_reap(&p->e, p->e.outstanding, timeout_ns) < 0);
    return settle(p, failed);
}

int mcdma_fabric_progress(struct mcdma_fabric *f) {
    if (!f) return MCDMA_FABRIC_INVALID;
    enter(f);
    progress_all(f);
    pthread_mutex_unlock(&f->lock);
    return MCDMA_FABRIC_OK;
}

int mcdma_fabric_wait(struct mcdma_fabric *f, uint64_t offset, uint64_t value, uint64_t timeout_ns) {
    if (!f) return MCDMA_FABRIC_INVALID;
    if (offset % 8 || !in(offset, 8, f->win.length)) return MCDMA_FABRIC_BOUNDS;
    const uint64_t *word = (const uint64_t *)(const void *)(f->win.base + offset);
    uint64_t deadline = link_now_ns() + timeout_ns;
    while (__atomic_load_n(word, __ATOMIC_ACQUIRE) < value) {
        if (link_now_ns() > deadline) return MCDMA_FABRIC_TIMEOUT;
        if (!f->threaded) mcdma_fabric_progress(f);
    }
    return MCDMA_FABRIC_OK;
}

void mcdma_fabric_disconnect(struct mcdma_fabric_peer **pp) {
    if (!pp || !*pp) return;
    struct mcdma_fabric_peer *p = *pp;
    struct mcdma_fabric *f = p->f;
    enter(f);
    for (int i = 0; i < f->npeers; ++i)
        if (f->peers[i] == p) f->peers[i] = f->peers[--f->npeers];
    pthread_mutex_unlock(&f->lock);
    release_peer(p);
    *pp = NULL;
}

void mcdma_fabric_close(struct mcdma_fabric **ff) {
    if (!ff || !*ff) return;
    struct mcdma_fabric *f = *ff;
    if (f->threaded) {
        f->stop = 1;
        pthread_join(f->thread, NULL);
    }
    while (f->npeers) {
        struct mcdma_fabric_peer *p = f->peers[0];
        mcdma_fabric_disconnect(&p);
    }
    ep_close(&f->dev);
    pthread_mutex_destroy(&f->lock);
    free(f);
    *ff = NULL;
}

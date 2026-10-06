/* libmcdma-fabric: one-sided writes between registered windows (mcdma_fabric.h), on the link code mcdma-rpcd uses.
 * Each peer owns its queue pairs on the device's context and protection domain, where the window is registered once. */
#include "link.h"
#include "mcdma_fabric.h"

#include <errno.h>
#include <pthread.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
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
#define BOND_LINKS 2
#define BOND_CHUNK (256ull << 10)
#define BOND_BACKLOG (4 * BOND_CHUNK) /* bound the slow-link tail instead of queueing half a large write there */
#define BOND_SMALL (16ull << 10)
#define BOND_SIGNALS 4096u
#define BOND_RANGES 128u
#define BOND_MODE (MODE_DIRECT | 0x80)
#define WRITE_FAILED "a write failed"   /* reasons for a failure that no link explained */
#define WRITE_SIGNAL_FAILED "a write-and-signal failed"
#define SIGNAL_FAILED "a signal failed"
#define FLUSH_FAILED "a flush failed"
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
    struct mcdma_fabric *part[BOND_LINKS]; /* a bond owns two ordinary fabrics, each with its own progress lock */
    int bonded, wait_poll, waiters;
};

struct bond_signal {
    uint64_t seq, need[BOND_LINKS], off, value;
};

struct bond_range { uint64_t off, len; unsigned lanes; };

struct bond_placed {
    uint64_t count, pad[7];          /* distinct cache lines: the two placement threads do not share a counter */
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
    int failing, why_done, tell;    /* the first failure claimed and its reason written; tell the other end */
    char name[X_NAME];
    char why[96];
    struct mcdma_fabric_peer *part[BOND_LINKS], *bond;
    unsigned lane;
    int bonded;
    uint8_t session[16];
    pthread_mutex_t rx_lock;         /* only signal bookkeeping; never held across payload placement */
    struct bond_signal *pending;
    struct bond_placed placed[BOND_LINKS];
    uint64_t tx_signal, tx_fenced, rx_signal;
    unsigned waiting, nranges;
    int affinity;                   /* -1: least-loaded link; otherwise preserve a prior single-link overwrite */
    struct bond_range ranges[BOND_RANGES];
    uint64_t payload_posted, payload_completed; /* RoCE diagnostics; Thunderbolt counts at SEND completion */
};

static int peer_down(const struct mcdma_fabric_peer *p) {
    return __atomic_load_n(&p->down, __ATOMIC_ACQUIRE) ||
           (p->bond && __atomic_load_n(&p->bond->down, __ATOMIC_ACQUIRE));
}

static void mark_down(struct mcdma_fabric_peer *p) {
    __atomic_store_n(&p->down, 1, __ATOMIC_RELEASE);
    if (p->bond) __atomic_store_n(&p->bond->down, 1, __ATOMIC_RELEASE);
}

/* A peer goes down once and for good. The first reason is kept and logged in one line (a Thunderbolt link that failed
 * has logged its own, so `why` NULL takes that line's reason), and each of its links is marked to tell the other end
 * over the exchange socket, so that end fails at once instead of waiting out a timeout. `tell` 0: the news came from
 * the other end. */
static void peer_fail(struct mcdma_fabric_peer *p, const char *why, int tell) {
    struct mcdma_fabric_peer *top = p->bond ? p->bond : p;
    const char *link = p->e.tb ? tb_failure(&p->e) : NULL;
    if (!__atomic_exchange_n(&top->failing, 1, __ATOMIC_ACQ_REL)) {
        if (why) snprintf(top->why, sizeof(top->why), "%s", why);
        else if (link) snprintf(top->why, sizeof(top->why), "%s: %s", p->e.device, link);
        else snprintf(top->why, sizeof(top->why), "%s%sa link operation failed", p->e.device, *p->e.device ? ": " : "");
        __atomic_store_n(&top->why_done, 1, __ATOMIC_RELEASE);
        if (why || !link) link_log("%s: peer down: %s", top->name, top->why);
        for (unsigned k = 0; tell && k < BOND_LINKS; ++k)
            if (top->bonded ? top->part[k] != NULL : k == 0)
                __atomic_store_n(&(top->bonded ? top->part[k] : top)->tell, 1, __ATOMIC_RELEASE);
    }
    mark_down(p);
    mark_down(top);
}

/* With this link's lock held: send the other end the reason this peer went down, once. */
static void tell_peer(struct mcdma_fabric_peer *p) {
    static const uint8_t zero[16];
    if (!__atomic_load_n(&p->tell, __ATOMIC_ACQUIRE) || !__atomic_exchange_n(&p->tell, 0, __ATOMIC_ACQ_REL)) return;
    const struct mcdma_fabric_peer *top = p->bond ? p->bond : p;
    if (p->x.fd < 0 || !memcmp(p->x.peer_session, zero, 16)) return;
    struct xmsg err;
    xchg_prepare(&p->x, &err, X_ERR, ROLE_PEER, 0);
    snprintf(err.text, sizeof(err.text), "%s", __atomic_load_n(&top->why_done, __ATOMIC_ACQUIRE) ? top->why : "failed");
    (void)xchg_send(&p->x, &err);
}

/* Exactly one optional '+', no whitespace, empty members or duplicate links. Leave provider names otherwise alone. */
static int split_links(const char *s, char out[BOND_LINKS][128], int different) {
    if (!s || !*s) return 0;
    const char *plus = strchr(s, '+');
    if (plus && strchr(plus + 1, '+')) return 0;
    size_t n = plus ? (size_t)(plus - s) : strlen(s), m = plus ? strlen(plus + 1) : 0;
    if (!n || n >= 128 || (plus && (!m || m >= 128))) return 0;
    for (const unsigned char *c = (const unsigned char *)s; *c; ++c)
        if (*c <= ' ' || *c == 127) return 0;
    memcpy(out[0], s, n), out[0][n] = 0;
    if (plus) {
        memcpy(out[1], plus + 1, m + 1);
        if (different && !strcmp(out[0], out[1])) return 0;
    }
    return plus ? BOND_LINKS : 1;
}

/* With rx_lock held, only the next signal may publish, and both placement prefixes must cover its watermark. */
static void bond_drain(struct mcdma_fabric_peer *p) {
    while (!peer_down(p)) {
        struct bond_signal *s = &p->pending[p->rx_signal % BOND_SIGNALS];
        if (s->seq != p->rx_signal ||
            s->need[0] > __atomic_load_n(&p->placed[0].count, __ATOMIC_SEQ_CST) ||
            s->need[1] > __atomic_load_n(&p->placed[1].count, __ATOMIC_SEQ_CST)) break;
        __atomic_store_n((uint64_t *)(void *)(p->f->win.base + s->off), s->value, __ATOMIC_RELEASE);
        s->seq = 0;
        p->rx_signal++;
        __atomic_sub_fetch(&p->waiting, 1, __ATOMIC_SEQ_CST);
    }
}

static void bond_placed(void *arg, uint64_t count) {
    struct mcdma_fabric_peer *lane = arg, *p = lane->bond;
    /* Both publication paths are sequentially consistent: either placement sees waiting, or signal enqueue sees
     * placement. Release/acquire alone permits both to see old values and lose the final notification on ARM. */
    __atomic_store_n(&p->placed[lane->lane].count, count, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&p->waiting, __ATOMIC_SEQ_CST)) {
        pthread_mutex_lock(&p->rx_lock);
        bond_drain(p);
        pthread_mutex_unlock(&p->rx_lock);
    }
}

static int bond_signal(void *arg, uint64_t seq, const uint64_t need[BOND_LINKS], uint64_t off, uint64_t value) {
    struct mcdma_fabric_peer *lane = arg, *p = lane->bond;
    int bad = peer_down(p) || off % 8 || off > p->f->win.length || p->f->win.length - off < 8;
    pthread_mutex_lock(&p->rx_lock);
    if (!bad) bad = seq < p->rx_signal || seq - p->rx_signal >= BOND_SIGNALS;
    struct bond_signal *s = &p->pending[seq % BOND_SIGNALS];
    if (!bad) bad = s->seq != 0;
    if (!bad) {
        *s = (struct bond_signal){seq, {need[0], need[1]}, off, value};
        __atomic_add_fetch(&p->waiting, 1, __ATOMIC_SEQ_CST);
        bond_drain(p);
    } else mark_down(p); /* publish nothing more; the link that carried it fails with the reason */
    pthread_mutex_unlock(&p->rx_lock);
    return bad ? -1 : 0;
}

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

/* Read each peer's exchange socket: offers again after ours was lost, goodbyes, failures, a restarted peer. */
static void service(struct mcdma_fabric_peer *p) {
    struct xmsg m;
    while (!peer_down(p) && xchg_recv(&p->x, &m, 0) == 1) {
        if (m.role != ROLE_PEER) continue;
        int current = !memcmp(m.from, p->x.peer_session, 16);
        if ((m.kind == X_BYE || m.kind == X_ERR) && xchg_ours(&p->x, &m)) {
            char why[sizeof(p->why)];
            snprintf(why, sizeof(why), "the other end %s", m.kind == X_ERR ? "failed: " : "said goodbye");
            if (m.kind == X_ERR) snprintf(why + strlen(why), sizeof(why) - strlen(why), "%s", m.text);
            peer_fail(p, why, 0);
        } else if (m.kind == X_OFFER && current) {
            (void)xchg_send(&p->x, &p->mine);
        } else if (m.kind == X_OFFER) {
            peer_fail(p, "the other end restarted; this link is gone", 0);
        }
    }
}

/* Under its lock: has a waiting link's peer gone? Read its exchange socket to find out. */
static int peer_gone(void *arg) {
    struct mcdma_fabric_peer *p = arg;
    service(p);
    return peer_down(p);
}

/* Under the lock: place Thunderbolt writes, reap RoCE completions, and every millisecond read exchange sockets. */
static int progress_all(struct mcdma_fabric *f) {
    int handled = 0, read_sockets = link_now_ns() - f->serviced > SERVICE_NS;
    if (read_sockets) f->serviced = link_now_ns();
    for (int i = 0; i < f->npeers; ++i) {
        struct mcdma_fabric_peer *p = f->peers[i];
        if (peer_down(p)) {
            tell_peer(p);
            continue;
        }
        int n = p->e.kind == LINK_TB ? tb_progress(&p->e) : ep_reap(&p->e, 0, 0);
        if (n < 0) {
            peer_fail(p, NULL, 1);
            tell_peer(p);
        } else handled += n;
        if (read_sockets) service(p);
    }
    return handled;
}

/* Callers take the lock through this, so the progress thread's tight loop never starves them. They spin: the progress
 * thread holds it for a microsecond or two, and a caller put to sleep on it pays the kernel's wake (~4 us). */
static void enter(struct mcdma_fabric *f) {
    __atomic_add_fetch(&f->wanted, 1, __ATOMIC_ACQ_REL);
    while (pthread_mutex_trylock(&f->lock)) {
#if defined(__aarch64__)
        __asm__ volatile("isb");
#endif
    }
    __atomic_sub_fetch(&f->wanted, 1, __ATOMIC_ACQ_REL);
}

static void *progress_main(void *arg) {
    struct mcdma_fabric *f = arg;
#ifdef __APPLE__
    /* MCDMA_FABRIC_QOS=1 gives the CPU placement thread user-interactive scheduling priority. */
    if (getenv("MCDMA_FABRIC_QOS") && !strcmp(getenv("MCDMA_FABRIC_QOS"), "1"))
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    uint64_t active = link_now_ns();
    while (!__atomic_load_n(&f->stop, __ATOMIC_ACQUIRE)) {
        if (__atomic_load_n(&f->wanted, __ATOMIC_ACQUIRE) || pthread_mutex_trylock(&f->lock)) continue;
        int handled = progress_all(f);
        pthread_mutex_unlock(&f->lock);
        if (handled) active = link_now_ns();
        /* idle for a while: back off, as the daemons do */
        if (!__atomic_load_n(&f->waiters, __ATOMIC_ACQUIRE) && link_now_ns() - active > 50000000ull) usleep(50);
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
    f->wait_poll = !getenv("MCDMA_FABRIC_WAIT_POLL") || strcmp(getenv("MCDMA_FABRIC_WAIT_POLL"), "0");
    char devices[BOND_LINKS][128];
    int count = split_links(device, devices, 1);
    if (!count) {
        pthread_mutex_destroy(&f->lock);
        free(f);
        return MCDMA_FABRIC_INVALID;
    }
    if (count == BOND_LINKS) {
        f->bonded = BOND_LINKS;
        f->win.base = window, f->win.length = length;
        int status = MCDMA_FABRIC_OK;
        for (int k = 0; !status && k < BOND_LINKS; ++k) {
            status = mcdma_fabric_open(devices[k], gid_index, path_mtu, window, length, dmabuf_fd, dmabuf_offset,
                                        flags | MCDMA_FABRIC_PROGRESS_THREAD, &f->part[k]);
            if (!status && f->part[k]->dev.kind != LINK_TB) status = MCDMA_FABRIC_UNSUPPORTED;
        }
        if (status) {
            for (int k = 0; k < BOND_LINKS; ++k) mcdma_fabric_close(&f->part[k]);
            pthread_mutex_destroy(&f->lock);
            free(f);
            return status;
        }
        *out = f;
        return MCDMA_FABRIC_OK;
    }
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
    if (p->bond) memcpy(p->x.session, p->bond->session, sizeof(p->x.session));
    xchg_prepare(&p->x, &p->mine, X_OFFER, ROLE_PEER, 0);
    ep_info(&p->e, &f->win, &p->mine.info);
    p->mine.info.mode = p->bond ? BOND_MODE : MODE_DIRECT;
    p->mine.info.req = f->win.length, p->mine.info.rep = p->bond ? p->lane + 1 : 0;
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
                              : i->rep != p->mine.info.rep || (p->bond && i->mode != BOND_MODE)
                                  ? "the two ends use different fabric link configurations"
                              : i->req < 8 || !table_valid(&i->table, i->req, i->transport == LINK_ROCE)
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

static int connect_lane(struct mcdma_fabric *f, const char *via, int port, int peer_port, const char *name,
                         uint64_t timeout_ns, struct mcdma_fabric_peer *bond, unsigned lane,
                         struct mcdma_fabric_peer **out) {
    if (!out) return MCDMA_FABRIC_INVALID;
    *out = NULL;
    if (!f || !via || !name || (!bond && !valid_name(name)) || (bond && strlen(name) >= X_NAME) ||
        port <= 0 || port > 65535 || peer_port < 0 || peer_port > 65535)
        return MCDMA_FABRIC_INVALID;
    struct mcdma_fabric_peer *p = calloc(1, sizeof(*p));
    if (!p) return MCDMA_FABRIC_NOMEM;
    p->f = f, p->bond = bond, p->lane = lane;
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->x.fd = -1;
    p->e = f->dev;
    p->e.qp = NULL, p->e.cq = NULL, p->e.tb = NULL, p->e.nmr = 0, p->e.outstanding = 0;
    memset(p->e.mr, 0, sizeof(p->e.mr));
    int status = via_check(via, &f->dev) || xchg_open(&p->x, via, port, peer_port, name) ? MCDMA_FABRIC_INVALID
                 : ep_create_qp(&p->e)                       ? MCDMA_FABRIC_DEVICE
                                                             : MCDMA_FABRIC_OK;
    if (!status && p->e.kind == LINK_TB) {
        tb_accept(&p->e, &f->win, 0, f->win.length);
        tb_watch(&p->e, p, peer_gone);
        if (bond) tb_bond_hooks(&p->e, p, bond_placed, bond_signal);
    }
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

int mcdma_fabric_connect(struct mcdma_fabric *f, const char *via, int port, int peer_port, const char *name,
                         uint64_t timeout_ns, struct mcdma_fabric_peer **out) {
    if (!out) return MCDMA_FABRIC_INVALID;
    *out = NULL;
    if (!f || !via || !name || !valid_name(name) || port <= 0 || port > 65535 || peer_port < 0 || peer_port > 65535)
        return MCDMA_FABRIC_INVALID;
    char links[BOND_LINKS][128];
    int count = split_links(via, links, 0);
    if (!count || count != (f->bonded ? BOND_LINKS : 1)) return MCDMA_FABRIC_INVALID;
    if (!f->bonded) return connect_lane(f, via, port, peer_port, name, timeout_ns, NULL, 0, out);
    if (port == 65535 || peer_port == 65535) return MCDMA_FABRIC_INVALID;
    struct mcdma_fabric_peer *p = calloc(1, sizeof(*p));
    if (!p) return MCDMA_FABRIC_NOMEM;
    p->f = f, p->bonded = BOND_LINKS, p->rx_signal = 1;
    snprintf(p->name, sizeof(p->name), "%s", name);
    pthread_mutex_init(&p->rx_lock, NULL);
    p->pending = calloc(BOND_SIGNALS, sizeof(*p->pending));
    int status = p->pending ? MCDMA_FABRIC_OK : MCDMA_FABRIC_NOMEM;
    link_random(p->session, sizeof(p->session));
    uint64_t deadline = link_now_ns() + timeout_ns;
    for (int k = 0; !status && k < BOND_LINKS; ++k) {
        char link_name[X_NAME];
        snprintf(link_name, sizeof(link_name), "%s-%d", name, k);
        uint64_t now = link_now_ns();
        if (now >= deadline) status = MCDMA_FABRIC_TIMEOUT;
        else status = connect_lane(f->part[k], links[k], port + k, (peer_port ? peer_port : port) + k,
                                   link_name, deadline - now, p, (unsigned)k, &p->part[k]);
    }
    if (!status && (memcmp(p->part[0]->x.peer_session, p->part[1]->x.peer_session, 16) ||
                    p->part[0]->remote_length != p->part[1]->remote_length ||
                    p->part[0]->remote.base != p->part[1]->remote.base)) status = MCDMA_FABRIC_PEER;
    if (!status) p->remote_length = p->part[0]->remote_length;
    enter(f);
    if (!status && f->npeers == MAX_PEERS) status = MCDMA_FABRIC_NOMEM;
    if (!status && peer_down(p)) status = MCDMA_FABRIC_PEER;
    if (!status) f->peers[f->npeers++] = p;
    pthread_mutex_unlock(&f->lock);
    if (status) {
        mark_down(p);
        for (int k = 0; k < BOND_LINKS; ++k) mcdma_fabric_disconnect(&p->part[k]);
        free(p->pending);
        pthread_mutex_destroy(&p->rx_lock);
        free(p);
        return status;
    }
    *out = p;
    return MCDMA_FABRIC_OK;
}

int mcdma_fabric_link(const struct mcdma_fabric_peer *p) {
    return !p ? 0 : p->bonded || p->e.kind == LINK_TB ? MCDMA_FABRIC_THUNDERBOLT : MCDMA_FABRIC_ROCE;
}

uint64_t mcdma_fabric_peer_length(const struct mcdma_fabric_peer *p) { return p ? p->remote_length : 0; }

int mcdma_fabric_peer_status(const struct mcdma_fabric_peer *p, char *why, size_t n) {
    if (!p) return MCDMA_FABRIC_INVALID;
    if (why && n) *why = 0;
    if (!peer_down(p)) return MCDMA_FABRIC_OK;
    const struct mcdma_fabric_peer *top = p->bond ? p->bond : p;
    if (why && n) snprintf(why, n, "%s", __atomic_load_n(&top->why_done, __ATOMIC_ACQUIRE) ? top->why : "down");
    return MCDMA_FABRIC_PEER;
}

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

/* Called with the lock held: a failed operation leaves the peer unusable. `what` names a failure no link explained.
 * A single link's other end hears why at once, under this lock; a bond's links tell it from their own threads. */
static int settle(struct mcdma_fabric_peer *p, int failed, const char *what) {
    if (failed) {
        peer_fail(p, p->e.tb && tb_failure(&p->e) ? NULL : what, 1);
        if (!p->bonded) tell_peer(p);
    }
    pthread_mutex_unlock(&p->f->lock);
    return failed ? MCDMA_FABRIC_PEER : MCDMA_FABRIC_OK;
}

/* The transmitter's parent lock is held. Each child retains its independent placement/progress lock. Fencing both
 * prefixes also covers signal callbacks: the last required placement or enqueue drains every ready signal before
 * that lane can answer its fence. No SEND completion alone is treated as remote completion. */
static int bond_flush_locked(struct mcdma_fabric_peer *p, uint64_t timeout_ns) {
    uint64_t deadline = link_now_ns() + timeout_ns;
    for (unsigned k = 0; k < BOND_LINKS; ++k) {
        uint64_t now = link_now_ns();
        if (peer_down(p) || now >= deadline || mcdma_fabric_flush(p->part[k], deadline - now)) {
            peer_fail(p, "a flush of both links did not complete", 1);
            return -1;
        }
    }
    p->nranges = 0;
    p->tx_fenced = p->tx_signal;
    return 0;
}

/* A single-link overwrite stays on that ordered queue, so repeated small write_signal needs no extra fence. A range
 * striped over both links, conflicting affinities, or an overlapping signal word requires both remote prefixes. */
static int bond_prepare(struct mcdma_fabric_peer *p, uint64_t off, uint64_t len) {
    p->affinity = -1;
    if (!len) return 0;
    unsigned lanes = 0;
    for (unsigned i = 0; i < p->nranges; ++i)
        if (off < p->ranges[i].off + p->ranges[i].len && p->ranges[i].off < off + len)
            lanes |= p->ranges[i].lanes;
    if (lanes == 3 || p->nranges + 2 >= BOND_RANGES) return bond_flush_locked(p, OP_NS);
    if (lanes) p->affinity = lanes == 1 ? 0 : 1;
    return 0;
}

static void bond_remember(struct mcdma_fabric_peer *p, uint64_t off, uint64_t len, unsigned lanes) {
    if (!len) return;
    for (unsigned i = 0; i < p->nranges; ++i)
        if (p->ranges[i].off == off && p->ranges[i].len == len) {
            p->ranges[i].lanes |= lanes;
            return;
        }
    p->ranges[p->nranges++] = (struct bond_range){off, len, lanes};
}

struct bond_room { uint64_t backlog, completed, take; int ready; };

static int bond_rooms(struct mcdma_fabric_peer *p, uint64_t off, uint64_t len, int fused,
                       struct bond_room room[BOND_LINKS]) {
    for (unsigned k = 0; k < BOND_LINKS; ++k) {
        struct mcdma_fabric_peer *lane = p->part[k];
        enter(lane->f);
        int bad = peer_down(lane) || tb_progress(&lane->e) < 0;
        if (!bad) {
            room[k].backlog = tb_posted_bytes(&lane->e) - tb_completed_bytes(&lane->e);
            room[k].completed = tb_completed_payload(&lane->e);
            uint64_t limit = tb_write_limit(&lane->e), lroom = lane->f->win.seg - off % lane->f->win.seg;
            room[k].take = len < BOND_CHUNK ? len : BOND_CHUNK;
            if (room[k].take > limit) room[k].take = limit;
            if (room[k].take > lroom) room[k].take = lroom;
            int can = fused ? tb_can_bond_write_signal(&lane->e, &lane->f->win, off, len)
                            : tb_can_write(&lane->e, &lane->f->win, off, room[k].take);
            room[k].ready = fused ? can == 1 : can == 1 && room[k].take != 0;
            if (room[k].backlog >= BOND_BACKLOG) room[k].ready = 0;
            if (can < 0) bad = 1;
        }
        if (bad) peer_fail(lane, NULL, 1);
        pthread_mutex_unlock(&lane->f->lock);
        if (bad) return -1;
    }
    return 0;
}

static int bond_choose(const struct bond_room room[BOND_LINKS]) {
    int best = -1;
    for (int k = 0; k < BOND_LINKS; ++k)
        if (room[k].ready && (best < 0 || room[k].backlog < room[best].backlog ||
                             (room[k].backlog == room[best].backlog && room[k].completed > room[best].completed)))
            best = k;
    return best;
}

static void bond_watermarks(const struct mcdma_fabric_peer *p, uint64_t need[BOND_LINKS]) {
    /* Only callers under this parent lock post writes to its private lane peers. Progress changes completions,
     * not writes_posted, so no child lock or cross-link placement exclusion is needed for this snapshot. */
    for (unsigned k = 0; k < BOND_LINKS; ++k) need[k] = tb_writes_posted(&p->part[k]->e);
}

static int bond_write_locked(struct mcdma_fabric_peer *p, uint64_t off, uint64_t roff, uint64_t len) {
    uint64_t deadline = link_now_ns() + OP_NS, first = roff, length = len;
    unsigned lanes = 0;
    while (len) {
        struct bond_room rooms[BOND_LINKS] = {{0}};
        if (peer_down(p) || bond_rooms(p, off, len, 0, rooms)) return -1;
        int k = p->affinity < 0 ? bond_choose(rooms) : rooms[p->affinity].ready ? p->affinity : -1;
        if (k < 0) {
            if (link_now_ns() >= deadline) return -1;
            continue;
        }
        struct mcdma_fabric_peer *lane = p->part[k];
        enter(lane->f);
        int r = peer_down(lane) ? -1 : tb_write_try(&lane->e, &lane->f->win, off, roff, rooms[k].take);
        if (r < 0) peer_fail(lane, NULL, 1);
        pthread_mutex_unlock(&lane->f->lock);
        if (r < 0) return -1;
        if (r == 1) {
            if (link_now_ns() >= deadline) return -1;
            continue;
        }
        /* Small operations can require several messages on a tiny granted queue or at a registration boundary,
         * but all their messages still use the first selected physical link. */
        if (length <= BOND_SMALL && p->affinity < 0) p->affinity = k;
        off += rooms[k].take, roff += rooms[k].take, len -= rooms[k].take;
        lanes |= 1u << k;
        deadline = link_now_ns() + OP_NS;
    }
    bond_remember(p, first, length, lanes);
    return 0;
}

static int bond_signal_locked(struct mcdma_fabric_peer *p, uint64_t off, uint64_t value) {
    if (peer_down(p) || p->tx_signal == UINT64_MAX) return -1;
    /* A receiver's bounded reorder queue has explicit sender credit, even for repeated signals to one word. */
    if (p->tx_signal - p->tx_fenced >= BOND_SIGNALS && bond_flush_locked(p, OP_NS)) return -1;
    uint64_t need[BOND_LINKS];
    bond_watermarks(p, need);
    uint64_t deadline = link_now_ns() + OP_NS;
    for (;;) {
        struct bond_room rooms[BOND_LINKS] = {{0}};
        /* A header has no source registration. Select only links that can post it without waiting. */
        for (unsigned k = 0; k < BOND_LINKS; ++k) {
            struct mcdma_fabric_peer *lane = p->part[k];
            enter(lane->f);
            int bad = peer_down(lane) || tb_progress(&lane->e) < 0;
            if (!bad) {
                int can = tb_can_bond_signal(&lane->e);
                bad = can < 0;
                rooms[k].ready = can == 1;
                rooms[k].backlog = tb_posted_bytes(&lane->e) - tb_completed_bytes(&lane->e);
                rooms[k].completed = tb_completed_payload(&lane->e);
            }
            if (bad) peer_fail(lane, NULL, 1);
            pthread_mutex_unlock(&lane->f->lock);
            if (bad) return -1;
        }
        int k = bond_choose(rooms);
        if (k < 0) {
            if (link_now_ns() >= deadline) return -1;
            continue;
        }
        struct mcdma_fabric_peer *lane = p->part[k];
        enter(lane->f);
        int can = peer_down(lane) ? -1 : tb_can_bond_signal(&lane->e);
        int bad = can < 0 || (can == 1 && tb_bond_signal(&lane->e, off, value, p->tx_signal + 1, need, OP_NS));
        if (bad) peer_fail(lane, NULL, 1);
        pthread_mutex_unlock(&lane->f->lock);
        if (bad) return -1;
        if (!can) {
            if (link_now_ns() >= deadline) return -1;
            continue;
        }
        p->tx_signal++;
        bond_remember(p, off, 8, 3); /* a deferred signal may store after the same lane has parsed a later write */
        return 0;
    }
}

static int bond_write_signal_locked(struct mcdma_fabric_peer *p, uint64_t off, uint64_t roff, uint64_t len,
                                     uint64_t soff, uint64_t value) {
    if (peer_down(p) || p->tx_signal == UINT64_MAX) return -1;
    if (p->tx_signal - p->tx_fenced >= BOND_SIGNALS && bond_flush_locked(p, OP_NS)) return -1;
    if (len && len <= BOND_SMALL) {
        struct bond_room rooms[BOND_LINKS] = {{0}};
        if (bond_rooms(p, off, len, 1, rooms)) return -1;
        int k = p->affinity < 0 ? bond_choose(rooms) : rooms[p->affinity].ready ? p->affinity : -1;
        if (k >= 0) {
            uint64_t need[BOND_LINKS];
            bond_watermarks(p, need);
            if (need[k] == UINT64_MAX) return -1;
            need[k]++;
            struct mcdma_fabric_peer *lane = p->part[k];
            enter(lane->f);
            int can = peer_down(lane) ? -1 : tb_can_bond_write_signal(&lane->e, &lane->f->win, off, len);
            int r = can < 0 ? -1 : can != 1 ? 1 : tb_bond_write_signal(&lane->e, &lane->f->win, off, roff, len,
                                                                       soff, value, p->tx_signal + 1, need, OP_NS);
            if (r < 0) peer_fail(lane, NULL, 1);
            pthread_mutex_unlock(&lane->f->lock);
            if (r < 0) return -1;
            if (!r) {
                p->tx_signal++;
                bond_remember(p, roff, len, 1u << k);
                bond_remember(p, soff, 8, 3);
                return 0;
            }
        }
    }
    return bond_write_locked(p, off, roff, len) || bond_signal_locked(p, soff, value) ? -1 : 0;
}

unsigned mcdma_fabric_link_count(const struct mcdma_fabric_peer *p) { return !p ? 0 : p->bonded ? BOND_LINKS : 1; }

int mcdma_fabric_link_stats(const struct mcdma_fabric_peer *p, unsigned index,
                            struct mcdma_fabric_link_stats *out) {
    if (!p || !out || index >= mcdma_fabric_link_count(p)) return MCDMA_FABRIC_INVALID;
    const struct mcdma_fabric_peer *lane = p->bonded ? p->part[index] : p;
    enter(lane->f);
    out->posted_bytes = lane->e.kind == LINK_TB ? tb_posted_payload(&lane->e) : lane->payload_posted;
    out->completed_bytes = lane->e.kind == LINK_TB ? tb_completed_payload(&lane->e) : lane->payload_completed;
    pthread_mutex_unlock(&lane->f->lock);
    return MCDMA_FABRIC_OK;
}

int mcdma_fabric_write(struct mcdma_fabric_peer *p, uint64_t local_offset, uint64_t remote_offset, uint64_t length) {
    if (!p) return MCDMA_FABRIC_INVALID;
    if (!in(local_offset, length, p->f->win.length) || !in(remote_offset, length, p->remote_length))
        return MCDMA_FABRIC_BOUNDS;
    enter(p->f);
    if (p->bonded) return settle(p, peer_down(p) || bond_prepare(p, remote_offset, length) ||
                                    bond_write_locked(p, local_offset, remote_offset, length), WRITE_FAILED);
    int failed = peer_down(p) || (length && (p->e.kind == LINK_TB ? tb_write(&p->e, &p->f->win, local_offset, remote_offset,
                                                                         length, OP_NS)
                                                             : roce_range(p, IBV_WR_RDMA_WRITE, local_offset,
                                                                          remote_offset, length)));
    if (!failed && p->e.kind == LINK_ROCE) p->payload_posted += length;
    return settle(p, failed, WRITE_FAILED);
}

int mcdma_fabric_write_signal(struct mcdma_fabric_peer *p, uint64_t local_offset, uint64_t remote_offset,
                              uint64_t length, uint64_t signal_offset, uint64_t value) {
    if (!p) return MCDMA_FABRIC_INVALID;
    if ((length && local_offset < MCDMA_FABRIC_WS_ROOM) || !in(local_offset, length, p->f->win.length) ||
        !in(remote_offset, length, p->remote_length) || signal_offset % 8 || !in(signal_offset, 8, p->remote_length))
        return MCDMA_FABRIC_BOUNDS;
    /* No bytes: the signal alone, on every link kind (a zero-length SEND is lost on Thunderbolt) */
    if (!length) return mcdma_fabric_signal(p, signal_offset, value);
    if (!p->bonded && p->e.kind != LINK_TB) return MCDMA_FABRIC_UNSUPPORTED;
    enter(p->f);
    if (p->bonded) return settle(p, peer_down(p) || bond_prepare(p, remote_offset, length) ||
                                    bond_write_signal_locked(p, local_offset, remote_offset, length, signal_offset, value),
                                 WRITE_SIGNAL_FAILED);
    int failed = peer_down(p) || tb_write_signal(&p->e, &p->f->win, local_offset, remote_offset, length, signal_offset,
                                             value, OP_NS);
    return settle(p, failed, WRITE_SIGNAL_FAILED);
}

int mcdma_fabric_signal(struct mcdma_fabric_peer *p, uint64_t remote_offset, uint64_t value) {
    if (!p) return MCDMA_FABRIC_INVALID;
    if (remote_offset % 8 || !in(remote_offset, 8, p->remote_length)) return MCDMA_FABRIC_BOUNDS;
    enter(p->f);
    if (p->bonded) {
        if (p->nranges + 1 >= BOND_RANGES && bond_flush_locked(p, OP_NS)) return settle(p, 1, SIGNAL_FAILED);
        return settle(p, bond_signal_locked(p, remote_offset, value), SIGNAL_FAILED);
    }
    int failed = peer_down(p) || (p->e.kind == LINK_TB ? tb_signal(&p->e, remote_offset, value, OP_NS)
                                                  : roce_signal(p, remote_offset, value));
    return settle(p, failed, SIGNAL_FAILED);
}

int mcdma_fabric_read(struct mcdma_fabric_peer *p, uint64_t local_offset, uint64_t remote_offset, uint64_t length) {
    if (!p) return MCDMA_FABRIC_INVALID;
    if (p->bonded || p->e.kind == LINK_TB) return MCDMA_FABRIC_UNSUPPORTED;
    if (!in(local_offset, length, p->f->win.length) || !in(remote_offset, length, p->remote_length))
        return MCDMA_FABRIC_BOUNDS;
    enter(p->f);
    return settle(p, peer_down(p) || roce_range(p, IBV_WR_RDMA_READ, local_offset, remote_offset, length),
                  "a read failed");
}

int mcdma_fabric_fetch_add(struct mcdma_fabric_peer *p, uint64_t remote_offset, uint64_t add, uint64_t *old) {
    (void)remote_offset, (void)add;
    if (old) *old = 0;
    return p ? MCDMA_FABRIC_UNSUPPORTED : MCDMA_FABRIC_INVALID;
}

int mcdma_fabric_flush(struct mcdma_fabric_peer *p, uint64_t timeout_ns) {
    if (!p) return MCDMA_FABRIC_INVALID;
    enter(p->f);
    if (p->bonded) return settle(p, bond_flush_locked(p, timeout_ns), FLUSH_FAILED);
    int failed = peer_down(p) || (p->e.kind == LINK_TB ? tb_fence(&p->e, timeout_ns)
                                                  : ep_reap(&p->e, p->e.outstanding, timeout_ns) < 0);
    if (!failed && p->e.kind == LINK_ROCE) p->payload_completed = p->payload_posted;
    return settle(p, failed, FLUSH_FAILED);
}

int mcdma_fabric_progress(struct mcdma_fabric *f) {
    if (!f) return MCDMA_FABRIC_INVALID;
    if (f->bonded) {
        int status = MCDMA_FABRIC_OK;
        for (unsigned k = 0; k < BOND_LINKS; ++k)
            if (mcdma_fabric_progress(f->part[k])) status = MCDMA_FABRIC_PEER;
        return status;
    }
    enter(f);
    progress_all(f);
    pthread_mutex_unlock(&f->lock);
    return MCDMA_FABRIC_OK;
}

/* A waiter can place a packet itself rather than wait for a progress-thread handoff. It never blocks behind another
 * placer: each trylock is independent, and the other link's worker remains free to run during a copy. */
static void wait_progress(struct mcdma_fabric *f) {
    if (f->bonded) {
        for (unsigned k = 0; k < BOND_LINKS; ++k) wait_progress(f->part[k]);
    } else if (!pthread_mutex_trylock(&f->lock)) {
        progress_all(f);
        pthread_mutex_unlock(&f->lock);
    }
}

static int fabric_down(struct mcdma_fabric *f) {
    int down;
    /* The transmitter may hold its lock through a long write. A waiter must not block behind it. */
    if (pthread_mutex_trylock(&f->lock)) return 0;
    /* wait names a window word, not a peer: one failed peer must not poison another peer's flag wait. */
    down = f->npeers != 0;
    for (int i = 0; i < f->npeers; ++i) if (!peer_down(f->peers[i])) down = 0;
    pthread_mutex_unlock(&f->lock);
    return down;
}

static void wait_active(struct mcdma_fabric *f, int delta) {
    if (f->bonded) {
        for (unsigned k = 0; k < BOND_LINKS; ++k) wait_active(f->part[k], delta);
    } else __atomic_add_fetch(&f->waiters, delta, __ATOMIC_ACQ_REL);
}

int mcdma_fabric_wait(struct mcdma_fabric *f, uint64_t offset, uint64_t value, uint64_t timeout_ns) {
    if (!f) return MCDMA_FABRIC_INVALID;
    if (offset % 8 || !in(offset, 8, f->win.length)) return MCDMA_FABRIC_BOUNDS;
    const uint64_t *word = (const uint64_t *)(const void *)(f->win.base + offset);
    uint64_t deadline = link_now_ns() + timeout_ns;
    unsigned polls = 0;
    int status = MCDMA_FABRIC_OK;
    wait_active(f, 1);
    while (__atomic_load_n(word, __ATOMIC_ACQUIRE) < value) {
        if (!(polls++ % 64)) {
            if (link_now_ns() > deadline) { status = MCDMA_FABRIC_TIMEOUT; break; }
            if (fabric_down(f)) { status = MCDMA_FABRIC_PEER; break; }
        }
        if (!f->threaded && !f->bonded) mcdma_fabric_progress(f);
        else if (f->wait_poll) wait_progress(f);
    }
    wait_active(f, -1);
    return status;
}

void mcdma_fabric_disconnect(struct mcdma_fabric_peer **pp) {
    if (!pp || !*pp) return;
    struct mcdma_fabric_peer *p = *pp;
    struct mcdma_fabric *f = p->f;
    enter(f);
    for (int i = 0; i < f->npeers; ++i)
        if (f->peers[i] == p) f->peers[i] = f->peers[--f->npeers];
    pthread_mutex_unlock(&f->lock);
    if (p->bonded) {
        mark_down(p);
        for (unsigned k = 0; k < BOND_LINKS; ++k) mcdma_fabric_disconnect(&p->part[k]);
        free(p->pending);
        pthread_mutex_destroy(&p->rx_lock);
        free(p);
    } else release_peer(p);
    *pp = NULL;
}

void mcdma_fabric_close(struct mcdma_fabric **ff) {
    if (!ff || !*ff) return;
    struct mcdma_fabric *f = *ff;
    if (f->threaded) {
        __atomic_store_n(&f->stop, 1, __ATOMIC_RELEASE);
        pthread_join(f->thread, NULL);
    }
    while (f->npeers) {
        struct mcdma_fabric_peer *p = f->peers[0];
        mcdma_fabric_disconnect(&p);
    }
    if (f->bonded) {
        for (unsigned k = 0; k < BOND_LINKS; ++k) mcdma_fabric_close(&f->part[k]);
    } else ep_close(&f->dev);
    pthread_mutex_destroy(&f->lock);
    free(f);
    *ff = NULL;
}

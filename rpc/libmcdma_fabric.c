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
#define BOND_SPLIT (16ull << 10)    /* a write_signal this long goes as one message on each link */
#define BOND_PART 2048ull           /* the least either link carries of a write_signal cut across both */
#define BOND_RATE 8000ull           /* bytes per microsecond assumed of a link that has not measured itself yet */
#define BOND_SIGNALS 4096u
#define BOND_RANGES 128u
#define BOND_MODE (MODE_DIRECT | 0x90) /* bonded (0x80) with tails (0x10): a bond end without them refuses this one */
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

/* Remote bytes earlier calls wrote or signalled that the receiver may not have placed yet: the latest write on each
 * link into [off, end), 0 for none, and whether a bonded signal stores a word in it. */
struct bond_range {
    uint64_t off, end, ord[BOND_LINKS];
    int signal;
};

/* A joined write's tail as its part announced it to the tail's link: where it goes and which write it is there. */
struct bond_tail {
    uint64_t off;
    uint32_t len, ordinal;
};

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
    unsigned waiting, nranges, turn; /* turn: the link the next tie goes to */
    int affinity;                   /* -1: least-loaded link; otherwise preserve a prior single-link overwrite */
    struct bond_range ranges[BOND_RANGES];
    struct bond_range floor;        /* what a full table forgot; every later call overlaps it until a flush */
    int floored;
    struct bond_tail *tails[BOND_LINKS]; /* each link's announced tails, from the other link's thread */
    uint64_t tail_put[BOND_LINKS], tail_got[BOND_LINKS];
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
        /* after the word: a joined part waiting for earlier signals reads this, then may overwrite the word */
        __atomic_store_n(&p->rx_signal, p->rx_signal + 1, __ATOMIC_RELEASE);
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

/* A write count's low 32 bits, widened against this end's count of the same link's placed writes. */
static uint64_t widen(uint64_t near, uint32_t low) { return near + (uint64_t)(int64_t)(int32_t)(low - (uint32_t)near); }

static int bond_signal(void *arg, uint64_t seq, const uint32_t need[BOND_LINKS], uint64_t off, uint64_t value) {
    struct mcdma_fabric_peer *lane = arg, *p = lane->bond;
    int bad = peer_down(p) || off % 8 || off > p->f->win.length || p->f->win.length - off < 8;
    pthread_mutex_lock(&p->rx_lock);
    if (!bad) bad = seq < p->rx_signal || seq - p->rx_signal >= BOND_SIGNALS;
    struct bond_signal *s = &p->pending[seq % BOND_SIGNALS];
    if (!bad) bad = s->seq != 0;
    if (!bad) {
        uint64_t wide[BOND_LINKS];
        for (unsigned k = 0; k < BOND_LINKS; ++k)
            wide[k] = widen(__atomic_load_n(&p->placed[k].count, __ATOMIC_ACQUIRE), need[k]);
        *s = (struct bond_signal){seq, {wide[0], wide[1]}, off, value};
        __atomic_add_fetch(&p->waiting, 1, __ATOMIC_SEQ_CST);
        bond_drain(p);
    } else mark_down(p); /* publish nothing more; the link that carried it fails with the reason */
    pthread_mutex_unlock(&p->rx_lock);
    return bad ? -1 : 0;
}

/* A joined part lands once the other link has placed the earlier writes it overlaps (BOND_WAIT_LINK) and, if it covers
 * a word an earlier signal stores, once every earlier signal is out (BOND_WAIT_SIGNALS); then later writes win. Its
 * own signal must fall in the queue's window. */
static int bond_ready(void *arg, uint64_t seq, unsigned flags, uint32_t wait) {
    struct mcdma_fabric_peer *lane = arg, *p = lane->bond;
    uint64_t next = __atomic_load_n(&p->rx_signal, __ATOMIC_ACQUIRE);
    if (peer_down(p) || seq < next || seq - next >= BOND_SIGNALS) return -1;
    if ((flags & BOND_WAIT_SIGNALS) && seq != next) return 0;
    uint64_t other = __atomic_load_n(&p->placed[!lane->lane].count, __ATOMIC_ACQUIRE);
    return !(flags & BOND_WAIT_LINK) || (int32_t)((uint32_t)other - wait) >= 0;
}

/* The part's link tells the tail's link where the tail goes. One link's thread writes each queue, the other reads it;
 * signal credit keeps fewer than BOND_SIGNALS announced tails unplaced. */
static int bond_announce(void *arg, uint64_t off, uint32_t len, uint32_t ordinal) {
    struct mcdma_fabric_peer *lane = arg, *p = lane->bond;
    unsigned to = !lane->lane;
    uint64_t put = p->tail_put[to];
    if (put - __atomic_load_n(&p->tail_got[to], __ATOMIC_ACQUIRE) >= BOND_SIGNALS) return 1;
    p->tails[to][put % BOND_SIGNALS] = (struct bond_tail){off, len, ordinal};
    __atomic_store_n(&p->tail_put[to], put + 1, __ATOMIC_RELEASE);
    return 0;
}

static int bond_tail(void *arg, uint64_t *off, uint32_t *len, uint32_t *ordinal) {
    struct mcdma_fabric_peer *lane = arg, *p = lane->bond;
    unsigned me = lane->lane;
    uint64_t got = p->tail_got[me];
    if (got == __atomic_load_n(&p->tail_put[me], __ATOMIC_ACQUIRE)) return 0;
    const struct bond_tail *a = &p->tails[me][got % BOND_SIGNALS];
    *off = a->off, *len = a->len, *ordinal = a->ordinal;
    __atomic_store_n(&p->tail_got[me], got + 1, __ATOMIC_RELEASE);
    return 1;
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
        struct tb_bond hooks = {p, lane, bond_placed, bond_signal, bond_ready, bond_announce, bond_tail};
        if (bond) tb_bond_hooks(&p->e, &hooks);
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
    for (unsigned k = 0; k < BOND_LINKS; ++k) p->tails[k] = calloc(BOND_SIGNALS, sizeof(*p->tails[k]));
    int status = p->pending && p->tails[0] && p->tails[1] ? MCDMA_FABRIC_OK : MCDMA_FABRIC_NOMEM;
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
        for (unsigned k = 0; k < BOND_LINKS; ++k) free(p->tails[k]);
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
    p->nranges = 0, p->floored = 0;
    p->tx_fenced = p->tx_signal;
    return 0;
}

/* Fold range r into the latest writes and signalled words gathered so far. */
static void bond_cover(uint64_t ord[BOND_LINKS], int *signal, const struct bond_range *r) {
    for (unsigned k = 0; k < BOND_LINKS; ++k) ord[k] = r->ord[k] > ord[k] ? r->ord[k] : ord[k];
    *signal |= r->signal;
}

/* The latest write on each link, and any signalled word, that earlier calls put in [off, off + len). */
static void bond_overlap(const struct mcdma_fabric_peer *p, uint64_t off, uint64_t len, uint64_t ord[BOND_LINKS],
                         int *signal) {
    ord[0] = ord[1] = 0, *signal = 0;
    if (len && p->floored) bond_cover(ord, signal, &p->floor);
    for (unsigned i = 0; len && i < p->nranges; ++i)
        if (off < p->ranges[i].end && p->ranges[i].off < off + len) bond_cover(ord, signal, &p->ranges[i]);
}

/* Overlapping ranges merge; a full table folds into the floor, which every later range overlaps until a flush. */
static void bond_remember(struct mcdma_fabric_peer *p, uint64_t off, uint64_t len, const uint64_t ord[BOND_LINKS],
                          int signal) {
    if (!len) return;
    struct bond_range r = {off, off + len, {ord[0], ord[1]}, signal};
    for (unsigned i = 0; i < p->nranges;) {
        struct bond_range *q = &p->ranges[i];
        if (!(q->off < r.end && r.off < q->end)) {
            ++i;
            continue;
        }
        r.off = q->off < r.off ? q->off : r.off;
        r.end = q->end > r.end ? q->end : r.end;
        bond_cover(r.ord, &r.signal, q);
        *q = p->ranges[--p->nranges];
        i = 0; /* the grown range may now meet one already passed */
    }
    if (p->nranges == BOND_RANGES) {
        if (!p->floored) p->floor = (struct bond_range){0, UINT64_MAX, {0, 0}, 0};
        for (unsigned i = 0; i < p->nranges; ++i) bond_cover(p->floor.ord, &p->floor.signal, &p->ranges[i]);
        p->floored = 1, p->nranges = 0;
    }
    p->ranges[p->nranges++] = r;
}

/* For a plain write, which has no wait of its own: overwriting bytes that only one link carried stays on that link's
 * ordered queue; bytes both links carried, or a signalled word, need both remote prefixes first. */
static int bond_prepare(struct mcdma_fabric_peer *p, uint64_t off, uint64_t len) {
    p->affinity = -1;
    if (!len) return 0;
    uint64_t ord[BOND_LINKS];
    int signal;
    bond_overlap(p, off, len, ord, &signal);
    unsigned lanes = (ord[0] ? 1u : 0u) | (ord[1] ? 2u : 0u);
    if (signal || lanes == 3) return bond_flush_locked(p, OP_NS);
    if (lanes) p->affinity = lanes == 1 ? 0 : 1;
    return 0;
}

struct bond_room { uint64_t backlog, take; int ready; };

static int bond_rooms(struct mcdma_fabric_peer *p, uint64_t off, uint64_t len, struct bond_room room[BOND_LINKS]) {
    for (unsigned k = 0; k < BOND_LINKS; ++k) {
        struct mcdma_fabric_peer *lane = p->part[k];
        enter(lane->f);
        int bad = peer_down(lane) || tb_progress(&lane->e) < 0;
        if (!bad) {
            room[k].backlog = tb_posted_bytes(&lane->e) - tb_completed_bytes(&lane->e);
            uint64_t limit = tb_write_limit(&lane->e), lroom = lane->f->win.seg - off % lane->f->win.seg;
            room[k].take = len < BOND_CHUNK ? len : BOND_CHUNK;
            if (room[k].take > limit) room[k].take = limit;
            if (room[k].take > lroom) room[k].take = lroom;
            int can = tb_can_write(&lane->e, &lane->f->win, off, room[k].take);
            room[k].ready = can == 1 && room[k].take != 0 && room[k].backlog < BOND_BACKLOG;
            if (can < 0) bad = 1;
        }
        if (bad) peer_fail(lane, NULL, 1);
        pthread_mutex_unlock(&lane->f->lock);
        if (bad) return -1;
    }
    return 0;
}

/* The ready link with less queued; equal queues alternate. */
static int bond_choose(struct mcdma_fabric_peer *p, const struct bond_room room[BOND_LINKS]) {
    int best = -1;
    for (int k = 0; k < BOND_LINKS; ++k)
        if (room[k].ready && (best < 0 || room[k].backlog < room[best].backlog)) best = k;
    if (best >= 0 && room[!best].ready && room[!best].backlog == room[best].backlog) best = (int)(p->turn ^= 1);
    return best;
}

static void bond_watermarks(const struct mcdma_fabric_peer *p, uint64_t need[BOND_LINKS]) {
    /* Only callers under this parent lock post writes to its private lane peers. Progress changes completions,
     * not writes_posted, so no child lock or cross-link placement exclusion is needed for this snapshot. */
    for (unsigned k = 0; k < BOND_LINKS; ++k) need[k] = tb_writes_posted(&p->part[k]->e);
}

static int bond_write_locked(struct mcdma_fabric_peer *p, uint64_t off, uint64_t roff, uint64_t len) {
    uint64_t deadline = link_now_ns() + OP_NS, first = roff, length = len, ord[BOND_LINKS] = {0, 0};
    while (len) {
        struct bond_room rooms[BOND_LINKS] = {{0}};
        if (peer_down(p) || bond_rooms(p, off, len, rooms)) return -1;
        int k = p->affinity < 0 ? bond_choose(p, rooms) : rooms[p->affinity].ready ? p->affinity : -1;
        if (k < 0) {
            if (link_now_ns() < deadline) continue;
            peer_fail(p, "neither link had room for a write within 10 s", 1);
            return -1;
        }
        struct mcdma_fabric_peer *lane = p->part[k];
        enter(lane->f);
        int r = peer_down(lane) ? -1 : tb_write_try(&lane->e, &lane->f->win, off, roff, rooms[k].take);
        if (r < 0) peer_fail(lane, NULL, 1);
        pthread_mutex_unlock(&lane->f->lock);
        if (r < 0) return -1;
        if (r == 1) {
            if (link_now_ns() < deadline) continue;
            peer_fail(p, "neither link had room for a write within 10 s", 1);
            return -1;
        }
        /* Small operations can require several messages on a tiny granted queue or at a registration boundary,
         * but all their messages still use the first selected physical link. */
        if (length <= BOND_SMALL && p->affinity < 0) p->affinity = k;
        off += rooms[k].take, roff += rooms[k].take, len -= rooms[k].take;
        ord[k] = tb_writes_posted(&lane->e);
        deadline = link_now_ns() + OP_NS;
    }
    bond_remember(p, first, length, ord, 0);
    return 0;
}

struct bond_load { uint64_t backlog, rate, max; };

/* Each link's bytes sent and not yet completed, its recent rate and its largest message, under its own lock and
 * without making progress: the links' threads reap. A link that has not measured itself yet is taken to run like
 * the other. */
static int bond_look(struct mcdma_fabric_peer *p, struct bond_load load[BOND_LINKS]) {
    for (unsigned k = 0; k < BOND_LINKS; ++k) {
        struct mcdma_fabric_peer *lane = p->part[k];
        enter(lane->f);
        int bad = peer_down(lane) || tb_failure(&lane->e) != NULL;
        if (!bad) {
            load[k].backlog = tb_posted_bytes(&lane->e) - tb_completed_bytes(&lane->e);
            load[k].rate = tb_rate(&lane->e);
            load[k].max = tb_message_max(&lane->e);
        }
        if (bad) peer_fail(lane, NULL, 1);
        pthread_mutex_unlock(&lane->f->lock);
        if (bad) return -1;
    }
    for (unsigned k = 0; k < BOND_LINKS; ++k)
        if (!load[k].rate) load[k].rate = load[!k].rate ? load[!k].rate : BOND_RATE;
    return 0;
}

/* Reap both links once, when the next message has no room on the link it needs. */
static int bond_poll(struct mcdma_fabric_peer *p) {
    for (unsigned k = 0; k < BOND_LINKS; ++k) {
        struct mcdma_fabric_peer *lane = p->part[k];
        enter(lane->f);
        int bad = peer_down(lane) || tb_progress(&lane->e) < 0;
        if (bad) peer_fail(lane, NULL, 1);
        pthread_mutex_unlock(&lane->f->lock);
        if (bad) return -1;
    }
    return 0;
}

/* Nanoseconds until a link would finish `bytes` more behind what it has queued, at its recent rate. */
static uint64_t bond_finish(const struct bond_load *l, uint64_t bytes) { return (l->backlog + bytes) * 1000 / l->rate; }

/* The link that would finish `bytes` first. Finishes within a quarter of each other alternate, so traffic that leaves
 * links of one speed idle between messages, such as a ping-pong, uses both instead of whichever won the first tie;
 * a link measured at half the other's rate gets small messages only when the other has a queue. */
static unsigned bond_pick(unsigned *turn, const struct bond_load load[BOND_LINKS], uint64_t bytes) {
    uint64_t a = bond_finish(&load[0], bytes), b = bond_finish(&load[1], bytes);
    unsigned k = b < a;
    uint64_t soon = k ? b : a, late = k ? a : b;
    if (late - soon <= late / 4) k = *turn, *turn ^= 1;
    return k;
}

/* A write_signal as at most one message a link: link `head` carries the first `first` bytes behind a head written in
 * the room before them, and the other link the rest as a tail. */
struct bond_plan { unsigned head; uint64_t first; };

/* Cut `len` bytes from `off` so both links finish together, given what each has queued and its rate:
 * (q0 + x) / r0 = (q1 + len - x) / r1. Each message stays inside one source registration and the peer's ring, the
 * head's message is whole packets where it can be, and the tail does not start with a head's magic. A short write, or
 * one either link would carry almost all of, goes whole on the link that finishes it first. 0 planned; 1 no message
 * a link can carry it, so it goes as writes and then a signal. */
static int bond_plan(const struct region *win, const struct bond_load load[BOND_LINKS], uint64_t off, uint64_t len,
                     unsigned *turn, struct bond_plan *out) {
    uint64_t seg = win->seg, start = off - MCDMA_FABRIC_WS_ROOM;
    if (start / seg != off / seg) return 1;
    uint64_t room = (start / seg + 1) * seg - off; /* bytes the head's message can carry in its registration */
    if (len >= BOND_SPLIT) {
        int64_t r0 = (int64_t)load[0].rate, r1 = (int64_t)load[1].rate;
        int64_t x = (r0 * (int64_t)(load[1].backlog + len) - r1 * (int64_t)load[0].backlog) / (r0 + r1);
        uint64_t zero = x < 0 ? 0 : (uint64_t)x > len ? len : (uint64_t)x;
        unsigned h = *turn;
        uint64_t first = h ? len - zero : zero, last = (off + len - 1) / seg * seg;
        uint64_t lo = BOND_PART, hi = len - BOND_PART;
        if (last > off && last - off > lo) lo = last - off;
        if (len > load[!h].max && len - load[!h].max > lo) lo = len - load[!h].max;
        uint64_t carry = load[h].max > MCDMA_FABRIC_WS_ROOM ? load[h].max - MCDMA_FABRIC_WS_ROOM : 0;
        if (room < hi) hi = room;
        if (carry < hi) hi = carry;
        uint64_t whole = (first + MCDMA_FABRIC_WS_ROOM + TB_PACKET / 2) / TB_PACKET * TB_PACKET;
        if (whole > MCDMA_FABRIC_WS_ROOM) first = whole - MCDMA_FABRIC_WS_ROOM;
        first = first < lo ? lo : first > hi ? hi : first;
        if (!tb_tail_clean(win, off + first)) first = first < hi ? first + 1 : first - 1;
        if (zero >= BOND_PART && len - zero >= BOND_PART && lo <= hi && first >= lo && first <= hi &&
            tb_tail_clean(win, off + first)) {
            *turn ^= 1;
            *out = (struct bond_plan){h, first};
            return 0;
        }
    }
    unsigned k = bond_pick(turn, load, len);
    for (unsigned i = 0; i < BOND_LINKS; ++i, k ^= 1)
        if (len <= room && MCDMA_FABRIC_WS_ROOM + len <= load[k].max) {
            *out = (struct bond_plan){k, len};
            return 0;
        }
    return 1;
}

/* Room on both links first, so a cut write never goes half posted; then the tail, then the head with the signal,
 * whose watermarks count the tail. The head waits at the receiver for the other link's earlier writes that it
 * overlaps, and for earlier signals when it covers a signalled word. 0 posted, 1 no room yet, 2 not representable
 * after all (nothing posted), -1 failed. */
static int bond_post(struct mcdma_fabric_peer *p, const struct bond_plan *plan, uint64_t off, uint64_t roff,
                     uint64_t len, uint64_t soff, uint64_t value) {
    unsigned h = plan->head, t = !h;
    uint64_t first = plan->first, rest = len - first;
    struct mcdma_fabric_peer *head = p->part[h], *tail = p->part[t];
    enter(head->f);
    int can = peer_down(head) ? -1 : tb_can_bond_write_signal(&head->e, &head->f->win, off, first);
    if (can < 0) peer_fail(head, NULL, 1);
    pthread_mutex_unlock(&head->f->lock);
    if (can != 1) return can < 0 ? -1 : can ? 2 : 1;
    if (rest) {
        enter(tail->f);
        can = peer_down(tail) ? -1 : tb_bond_tail(&tail->e, &tail->f->win, off + first, rest);
        if (can < 0) peer_fail(tail, NULL, 1);
        pthread_mutex_unlock(&tail->f->lock);
        if (can) return can;
    }
    uint64_t need[BOND_LINKS], ord[BOND_LINKS], mine[BOND_LINKS] = {0, 0};
    int signal;
    bond_watermarks(p, need);
    need[h]++;
    bond_overlap(p, roff, len, ord, &signal);
    unsigned flags = (ord[t] ? BOND_WAIT_LINK : 0) | (signal ? BOND_WAIT_SIGNALS : 0);
    enter(head->f);
    int r = peer_down(head) ? -1 : tb_bond_write_signal(&head->e, &head->f->win, off, roff, first, soff, value,
                                                         p->tx_signal + 1, need, (uint32_t)rest, flags, ord[t], OP_NS);
    if (r) peer_fail(head, r < 0 ? NULL : "a joined write could not follow its tail", 1);
    pthread_mutex_unlock(&head->f->lock);
    if (r) return -1;
    p->tx_signal++;
    mine[h] = need[h];
    if (rest) mine[t] = need[t];
    bond_remember(p, roff, len, mine, 0);
    bond_remember(p, soff, 8, (uint64_t[BOND_LINKS]){0, 0}, 1);
    return 0;
}

static int bond_signal_locked(struct mcdma_fabric_peer *p, uint64_t off, uint64_t value) {
    if (peer_down(p) || p->tx_signal == UINT64_MAX) return -1;
    /* A receiver's bounded reorder queue has explicit sender credit, even for repeated signals to one word. */
    if (p->tx_signal - p->tx_fenced >= BOND_SIGNALS && bond_flush_locked(p, OP_NS)) return -1;
    uint64_t need[BOND_LINKS], deadline = link_now_ns() + OP_NS;
    bond_watermarks(p, need);
    for (;;) {
        struct bond_load load[BOND_LINKS];
        if (bond_look(p, load)) return -1;
        /* the link that would deliver it first, else the other */
        unsigned k = bond_pick(&p->turn, load, MCDMA_FABRIC_WS_ROOM);
        for (unsigned i = 0; i < BOND_LINKS; ++i, k ^= 1) {
            struct mcdma_fabric_peer *lane = p->part[k];
            enter(lane->f);
            int can = peer_down(lane) ? -1 : tb_can_bond_signal(&lane->e);
            int bad = can < 0 || (can == 1 && tb_bond_signal(&lane->e, off, value, p->tx_signal + 1, need, OP_NS));
            if (bad) peer_fail(lane, NULL, 1);
            pthread_mutex_unlock(&lane->f->lock);
            if (bad) return -1;
            if (can == 1) {
                p->tx_signal++;
                /* a later write over this word waits for the signal at the receiver, or fences */
                bond_remember(p, off, 8, (uint64_t[BOND_LINKS]){0, 0}, 1);
                return 0;
            }
        }
        if (link_now_ns() >= deadline) {
            peer_fail(p, "neither link had room for a signal within 10 s", 1);
            return -1;
        }
        if (bond_poll(p)) return -1;
    }
}

/* One message a link: planned from the links' queues and rates, posted when both have room. A write no message a
 * link can carry goes as writes, then a signal. */
static int bond_write_signal_locked(struct mcdma_fabric_peer *p, uint64_t off, uint64_t roff, uint64_t len,
                                     uint64_t soff, uint64_t value) {
    if (peer_down(p) || p->tx_signal == UINT64_MAX) return -1;
    if (p->tx_signal - p->tx_fenced >= BOND_SIGNALS && bond_flush_locked(p, OP_NS)) return -1;
    uint64_t deadline = link_now_ns() + OP_NS;
    for (;;) {
        struct bond_load load[BOND_LINKS];
        struct bond_plan plan;
        if (bond_look(p, load)) return -1;
        /* both links register the same memory in the same pieces */
        int r = bond_plan(&p->part[0]->f->win, load, off, len, &p->turn, &plan)
                    ? 2
                    : bond_post(p, &plan, off, roff, len, soff, value);
        if (r <= 0) return r;
        if (r == 2) break;
        if (link_now_ns() >= deadline) {
            peer_fail(p, "neither link had room for a write-and-signal within 10 s", 1);
            return -1;
        }
        if (bond_poll(p)) return -1;
    }
    return bond_prepare(p, roff, len) || bond_write_locked(p, off, roff, len) || bond_signal_locked(p, soff, value) ? -1
                                                                                                                  : 0;
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
    if (p->bonded)
        return settle(p, peer_down(p) ||
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
    if (p->bonded) return settle(p, bond_signal_locked(p, remote_offset, value), SIGNAL_FAILED);
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
        for (unsigned k = 0; k < BOND_LINKS; ++k) free(p->tails[k]);
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

#ifdef MCDMA_FABRIC_TESTING
/* Offline tests plan a bonded write_signal without links: each link's queued bytes, rate and largest message. */
int mcdma_fabric_test_plan(const struct region *win, const uint64_t backlog[2], const uint64_t rate[2],
                           const uint64_t max[2], uint64_t off, uint64_t len, unsigned *turn, unsigned *head,
                           uint64_t *first) {
    struct bond_load load[BOND_LINKS];
    struct bond_plan plan;
    for (unsigned k = 0; k < BOND_LINKS; ++k) load[k] = (struct bond_load){backlog[k], rate[k], max[k]};
    if (bond_plan(win, load, off, len, turn, &plan)) return 1;
    *head = plan.head, *first = plan.first;
    return 0;
}
#endif

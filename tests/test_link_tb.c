/* Offline test of the Thunderbolt write protocol under the stub's model of the measured provider, one scenario a run:
 *   order     every signal lands after the writes before it, from 1 byte through packet and message boundaries
 *   joined    the same with each write and its signal as one message (or two, across a registration)
 *   overlap   later writes to the same bytes win, whether each rode inside its header or as a message of its own
 *   random    thousands of random writes and signals both ways at once match a replayed mirror
 *   bounds    a write outside what the receiver accepts fails the link instead of landing
 *   teardown  queue pairs with sends still waiting for credit are torn down without misuse
 *   bond      transport hooks see complete writes and exact signal metadata; nonblocking credit is bounded
 *   tail      a headless tail waits for its announcement, lands where it says and counts as its write; a joined part
 *             waits for the bond's go-ahead, announces its tail, and a mismatched tail fails the link
 *   repost    a progress thread leaves a landed message's receives for a reply's moment, holds them back while a
 *             caller waits for the lock, then puts them back a few a pass; plain progress puts them back at once
 * Exit 0 means pass; STUB_SEED changes the delivery order across queue pairs. */
#include "../rpc/link.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WINDOW (40ull << 20)
#define HALF (WINDOW / 2)
#define TIMEOUT 10000000000ull
#define OPS 2000

struct side {
    struct ep e;
    struct region r;
    unsigned char *mem;
    volatile int run, failed, started;
    pthread_t thread;
};

struct op {
    uint64_t src, dst, len, value;    /* len 0: a signal of `value` at dst */
};

struct job {
    struct side *me;
    const struct op *ops;
    volatile int *done;
};

static const char *g_mode;
static int g_quiet;
static uint64_t g_rng = 0x9e3779b97f4a7c15ull;

void link_log(const char *fmt, ...) {
    if (g_quiet) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "link: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void fail(const char *what) {
    fprintf(stderr, "test_link_tb %s: %s\n", g_mode, what);
    _exit(1);
}

#define CHECK(c, what)        \
    do {                      \
        if (!(c)) fail(what); \
    } while (0)

static uint64_t rnd(void) {
    g_rng ^= g_rng << 13, g_rng ^= g_rng >> 7, g_rng ^= g_rng << 17;
    return g_rng;
}

static void open_side(struct side *s, const char *device, uint64_t accept) {
    void *mem = NULL;
    CHECK(!ep_open(&s->e, device, 0, 4096) && s->e.kind == LINK_TB, "a thunderbolt device opens");
    CHECK(!posix_memalign(&mem, 16384, WINDOW), "window memory");
    s->mem = mem;
    for (uint64_t i = 0; i < WINDOW; i += 8) *(uint64_t *)(void *)(s->mem + i) = rnd();
    CHECK(!ep_reg_region(&s->e, &s->r, s->mem, WINDOW, TB_SEG, 0, 0, 0), "the window registers in pieces");
    CHECK(s->r.n == 4, "a 40 MiB window is four thunderbolt registrations");
    CHECK(!ep_create_qp(&s->e), "thunderbolt queue pairs");
    tb_accept(&s->e, &s->r, 0, accept);
}

static void join_pair(struct side *a, struct side *b) {
    struct xinfo ia, ib;
    ep_info(&a->e, &a->r, &ia);
    ep_info(&b->e, &b->r, &ib);
    CHECK(ia.transport == LINK_TB && ia.qpn && !ia.qpn2 && ia.frames >= 4 && ia.frames <= TB_RING && ia.table.n == 0,
          "a thunderbolt offer is one queue pair, its ring as granted and no keys");
    CHECK(!ep_connect(&a->e, &ib) && !ep_connect(&b->e, &ia), "both sides reach RTS");
}

/* The receiver judges each flag value once, as it appears; the step publishes its geometry before its value. */
static struct {
    volatile uint64_t value, dst, len, judged;
    const unsigned char *src;
    volatile int wrong;
} g_watch;

static void *progress(void *arg) {
    struct side *s = arg;
    while (s->run) {
        if (tb_progress(&s->e) < 0) {
            s->failed = 1;
            break;
        }
        uint64_t want = __atomic_load_n(&g_watch.value, __ATOMIC_ACQUIRE);
        if (want && g_watch.judged != want &&
            __atomic_load_n((uint64_t *)(void *)(s->mem + WINDOW - 64), __ATOMIC_ACQUIRE) == want) {
            g_watch.wrong = memcmp(s->mem + g_watch.dst, g_watch.src, g_watch.len) != 0;
            __atomic_store_n(&g_watch.judged, want, __ATOMIC_RELEASE);
        }
    }
    return NULL;
}

static void start(struct side *s) {
    s->run = 1, s->started = 1;
    CHECK(!pthread_create(&s->thread, NULL, progress, s), "progress thread");
}

static void stop(struct side *s) {
    if (!s->started) return;
    s->run = 0, s->started = 0;
    pthread_join(s->thread, NULL);
}

static void wait_word(struct side *s, uint64_t off, uint64_t want) {
    uint64_t deadline = link_now_ns() + TIMEOUT;
    while (__atomic_load_n((uint64_t *)(void *)(s->mem + off), __ATOMIC_ACQUIRE) != want) {
        CHECK(!s->failed, "the receiver's link failed");
        CHECK(link_now_ns() < deadline, "a signal never landed");
    }
}

static void scenario_order(struct side *a, struct side *b) {
    const uint64_t sizes[] = {1,         7,          4064,     4065,          4096,          8192,   65536 + 3,
                              (1ull << 20) + 5, 4ull << 20, (4ull << 20) + 1, (5ull << 20) + 1, 13ull << 20};
    const uint64_t flag = WINDOW - 64;
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        uint64_t len = sizes[i], src = (rnd() % 64) * 8 + 1;
        /* odd sizes end just past the 12 MiB registration boundary */
        uint64_t dst = i % 2 ? TB_SEG - len / 2 - 3 : 4096 * (uint64_t)i;
        g_watch.dst = dst, g_watch.len = len, g_watch.src = a->mem + src;
        __atomic_store_n(&g_watch.value, 1000 + i, __ATOMIC_RELEASE);
        CHECK(!tb_write(&a->e, &a->r, src, dst, len, TIMEOUT), "a write is posted");
        CHECK(!tb_signal(&a->e, flag, 1000 + i, TIMEOUT), "a signal is posted");
        /* this thread stays out of the verbs layer until the receiver has judged the signal */
        uint64_t deadline = link_now_ns() + TIMEOUT;
        while (__atomic_load_n(&g_watch.judged, __ATOMIC_ACQUIRE) != 1000 + i) {
            CHECK(!b->failed, "the receiver's link failed");
            CHECK(link_now_ns() < deadline, "a signal never landed");
        }
        CHECK(!g_watch.wrong, "a signal landed before the write it follows");
        CHECK(!memcmp(b->mem + dst, a->mem + src, len), "a write landed wrong");
    }
    CHECK(!tb_fence(&a->e, TIMEOUT), "a fence returns");
}

static void scenario_joined(struct side *a, struct side *b) {
    const uint64_t sizes[] = {1, 7, 4016, 4048, 4049, 8192, 65536 + 3, (1ull << 20) + 5, (4ull << 20) - 48,
                              (4ull << 20) + 1, 13ull << 20};
    const uint64_t flag = WINDOW - 64;
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        uint64_t len = sizes[i], src = 64 + (rnd() % 64) * 8 + 1;
        /* even steps start past a registration boundary at the source, so their head would span two */
        if (i % 2 == 0 && len < TB_SEG - 4096) src = TB_SEG + 16;
        uint64_t dst = i % 2 ? TB_SEG - len / 2 - 3 : 4096 * (uint64_t)i;
        g_watch.dst = dst, g_watch.len = len, g_watch.src = a->mem + src;
        __atomic_store_n(&g_watch.value, 2000 + i, __ATOMIC_RELEASE);
        CHECK(!tb_write_signal(&a->e, &a->r, src, dst, len, flag, 2000 + i, TIMEOUT), "a joined write is posted");
        uint64_t deadline = link_now_ns() + TIMEOUT;
        while (__atomic_load_n(&g_watch.judged, __ATOMIC_ACQUIRE) != 2000 + i) {
            CHECK(!b->failed, "the receiver's link failed");
            CHECK(link_now_ns() < deadline, "a joined signal never landed");
        }
        CHECK(!g_watch.wrong, "a joined signal landed before its bytes");
        CHECK(!memcmp(b->mem + dst, a->mem + src, len), "a joined write landed wrong");
    }
    CHECK(!tb_fence(&a->e, TIMEOUT), "a fence returns");
}

static void scenario_overlap(struct side *a, struct side *b) {
    const uint64_t at = 3ull << 20, flag = WINDOW - 64;
    static unsigned char expect[256 << 10];
    for (unsigned round = 0; round < 60; ++round) {
        uint64_t big = 65536 + rnd() % 65536, small = 1 + rnd() % 4000, mid = rnd() % (big - small);
        uint64_t s1 = (rnd() % 256) * 4096, s2 = (rnd() % 256) * 4096 + 7, s3 = (rnd() % 256) * 4096 + 11;
        memcpy(expect, a->mem + s1, big);
        memcpy(expect + mid, a->mem + s2, small);
        CHECK(!tb_write(&a->e, &a->r, s1, at, big, TIMEOUT), "a data write");
        CHECK(!tb_write(&a->e, &a->r, s2, at + mid, small, TIMEOUT), "an inline write over it");
        if (round % 2) {
            memcpy(expect, a->mem + s3, big);
            CHECK(!tb_write(&a->e, &a->r, s3, at, big, TIMEOUT), "a data write over both");
        }
        CHECK(!tb_signal(&a->e, flag, 77 + round, TIMEOUT), "a signal");
        wait_word(b, flag, 77 + round);
        CHECK(!memcmp(b->mem + at, expect, big), "overlapping writes landed out of posting order");
    }
}

static void *writer(void *arg) {
    struct job *j = arg;
    for (int i = 0; i < OPS && !j->me->failed; ++i) {
        const struct op *o = &j->ops[i];
        if (o->len ? tb_write(&j->me->e, &j->me->r, o->src, o->dst, o->len, TIMEOUT)
                   : tb_signal(&j->me->e, o->dst, o->value, TIMEOUT))
            j->me->failed = 1;
    }
    if (!j->me->failed && tb_fence(&j->me->e, TIMEOUT)) j->me->failed = 1;
    __atomic_add_fetch(j->done, 1, __ATOMIC_ACQ_REL);
    /* keep placing the peer's writes until it has fenced too */
    while (__atomic_load_n(j->done, __ATOMIC_ACQUIRE) < 2 && !j->me->failed)
        if (tb_progress(&j->me->e) < 0) j->me->failed = 1;
    return NULL;
}

static void plan(struct op *ops, unsigned char *mirror, const unsigned char *src) {
    for (int i = 0; i < OPS; ++i) {
        uint64_t kind = rnd() % 8, len = kind < 3 ? 1 + rnd() % 4064 : kind < 6 ? 1 + rnd() % (2u << 20) : 0;
        struct op *o = &ops[i];
        o->len = len, o->value = rnd();
        o->dst = len ? rnd() % (HALF - len) : (rnd() % (HALF / 8)) * 8;
        o->src = HALF + rnd() % (HALF - len);
        if (len) memcpy(mirror + o->dst, src + o->src, len);
        else memcpy(mirror + o->dst, &o->value, 8);
    }
}

/* Both sides write into the low half of each other's window, from the high half of their own that nobody writes. */
static void scenario_random(struct side *a, struct side *b) {
    struct op *to_b = calloc(OPS, sizeof(*to_b)), *to_a = calloc(OPS, sizeof(*to_a));
    unsigned char *mirror_a = malloc(HALF), *mirror_b = malloc(HALF);
    CHECK(to_a && to_b && mirror_a && mirror_b, "memory");
    memcpy(mirror_a, a->mem, HALF);
    memcpy(mirror_b, b->mem, HALF);
    plan(to_b, mirror_b, a->mem);
    plan(to_a, mirror_a, b->mem);
    stop(b);
    volatile int done = 0;
    struct job ja = {a, to_b, &done}, jb = {b, to_a, &done};
    pthread_t ta, tb;
    CHECK(!pthread_create(&ta, NULL, writer, &ja), "writer a");
    CHECK(!pthread_create(&tb, NULL, writer, &jb), "writer b");
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    CHECK(!a->failed && !b->failed, "a link failed under random traffic");
    CHECK(!memcmp(a->mem, mirror_a, HALF), "side a's window differs from the replayed mirror");
    CHECK(!memcmp(b->mem, mirror_b, HALF), "side b's window differs from the replayed mirror");
    free(to_a), free(to_b), free(mirror_a), free(mirror_b);
}

static void scenario_bounds(struct side *a, struct side *b) {
    CHECK(!tb_write(&a->e, &a->r, 0, HALF + 4096, 100, TIMEOUT), "the sender cannot see the receiver's limit");
    uint64_t deadline = link_now_ns() + TIMEOUT;
    while (!b->failed) CHECK(link_now_ns() < deadline, "a write past the accepted range did not fail the link");
    stop(b);
    CHECK(tb_progress(&b->e) < 0, "a failed link stays failed");
}

static void scenario_teardown(struct side *a, struct side *b) {
    stop(b);
    /* 12 MiB is more than the receiver's 8 MiB ring, so some packets must wait for receives */
    for (uint64_t i = 0; i < 12; ++i) CHECK(!tb_write(&a->e, &a->r, 0, i << 20, 1u << 20, TIMEOUT), "a write is queued");
    CHECK(tb_busy(&a->e), "sends wait for the receiver's credit");
    ep_destroy_qp(&a->e);
    ep_destroy_qp(&b->e);
    CHECK(!a->e.qp && !a->e.cq && !b->e.qp && !a->e.tb, "queue pairs are gone");
}

struct bond_watch {
    struct side *rx;
    const unsigned char *src;
    uint64_t dst, len, count, signals, seq, need[2], soff, value;
    int wrong, ready;
    /* announcements: what this link's tails should be, as the other link's parts would announce them */
    uint64_t tail_off[8], heard_off;
    uint32_t tail_len[8], tail_ordinal[8], heard_len, heard_ordinal;
    unsigned tails, taken, heard;
};

static void bond_placed(void *arg, uint64_t count) {
    struct bond_watch *w = arg;
    if (count != w->count + 1 || (w->len && memcmp(w->rx->mem + w->dst, w->src, w->len))) w->wrong = 1;
    w->count = count;
}

static int bond_signal(void *arg, uint64_t seq, const uint32_t need[2], uint64_t off, uint64_t value) {
    struct bond_watch *w = arg;
    if (seq != w->seq || need[0] != (uint32_t)w->need[0] || need[1] != (uint32_t)w->need[1] || off != w->soff ||
        value != w->value || w->count != tb_writes_placed(&w->rx->e)) w->wrong = 1;
    w->signals++;
    /* The callback queues the signal rather than releasing it: the transport must never store the flag itself. */
    return 0;
}

static int bond_ready(void *arg, uint64_t seq, unsigned flags, uint32_t wait) {
    struct bond_watch *w = arg;
    (void)seq, (void)flags, (void)wait;
    return __atomic_load_n(&w->ready, __ATOMIC_ACQUIRE);
}

static int bond_announce(void *arg, uint64_t off, uint32_t len, uint32_t ordinal) {
    struct bond_watch *w = arg;
    w->heard_off = off, w->heard_len = len, w->heard_ordinal = ordinal;
    w->heard++;
    return 0;
}

static int bond_tail(void *arg, uint64_t *off, uint32_t *len, uint32_t *ordinal) {
    struct bond_watch *w = arg;
    if (w->taken == __atomic_load_n(&w->tails, __ATOMIC_ACQUIRE)) return 0;
    *off = w->tail_off[w->taken % 8], *len = w->tail_len[w->taken % 8], *ordinal = w->tail_ordinal[w->taken % 8];
    w->taken++;
    return 1;
}

static void install(struct side *b, struct bond_watch *w) {
    struct tb_bond hooks = {w, 0, bond_placed, bond_signal, bond_ready, bond_announce, bond_tail, 2, NULL};
    tb_bond_hooks(&b->e, &hooks);
}

static void bond_poll(struct side *a, struct side *b, struct bond_watch *w, uint64_t writes, uint64_t signals) {
    uint64_t deadline = link_now_ns() + TIMEOUT;
    while (w->count < writes || w->signals < signals) {
        CHECK(tb_progress(&a->e) >= 0 && tb_progress(&b->e) >= 0, "bonded transport progresses");
        CHECK(link_now_ns() < deadline, "bonded transport completion timed out");
    }
    CHECK(!w->wrong, "bond hooks saw incomplete data or changed metadata");
}

static void scenario_bond(struct side *a, struct side *b) {
    stop(b);
    struct bond_watch w = {.rx = b, .count = tb_writes_placed(&b->e), .soff = WINDOW - 128, .ready = 1};
    install(b, &w);
    const uint64_t sizes[] = {7, 4064, 65537, 4032, 4033, 100003};
    uint64_t chunk = tb_write_limit(&a->e);
    CHECK(chunk > 64 && chunk <= TB_TRY_MAX, "scheduler chunk limit follows granted depth");
    CHECK(tb_message_max(&a->e) >= chunk, "a whole message is at least a scheduler chunk");
    uint64_t payload = tb_posted_payload(&a->e), initial_wire = tb_posted_bytes(&a->e);
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        uint64_t src = (1ull << 20) + i * (256ull << 10), before = tb_writes_posted(&a->e), signals = w.signals;
        uint64_t cap = i >= 2 ? chunk - 64 : chunk;
        w.src = a->mem + src, w.dst = (8ull << 20) + i * (256ull << 10), w.len = sizes[i] < cap ? sizes[i] : cap;
        w.seq = (1ull << 40) + i + 1, w.need[0] = before + 1, w.need[1] = (1ull << 48) + i;
        w.value = (1ull << 52) + i;
        __atomic_store_n((uint64_t *)(void *)(b->mem + w.soff), 0, __ATOMIC_RELEASE);
        if (i == 2)
            CHECK(!tb_write_signal(&a->e, &a->r, src, w.dst, w.len, w.soff, 17, TIMEOUT), "ordinary fused wire remains accepted");
        else if (i >= 3) {
            CHECK(tb_can_bond_write_signal(&a->e, &a->r, src, w.len) == 1, "fused preflight reserves its actual packet count");
            CHECK(!tb_bond_write_signal(&a->e, &a->r, src, w.dst, w.len, w.soff, w.value, w.seq, w.need, 0, 0, 0,
                                        TIMEOUT), "a 64-byte bonded fused head posts");
        }
        else {
            CHECK(tb_can_write(&a->e, &a->r, src, w.len) == 1, "nonblocking write has room");
            CHECK(!tb_write_try(&a->e, &a->r, src, w.dst, w.len), "nonblocking write posts");
            CHECK(tb_can_bond_signal(&a->e) == 1, "bond signal preflight checks staged-header room");
            CHECK(!tb_bond_signal(&a->e, w.soff, w.value, w.seq, w.need, TIMEOUT), "a bonded signal posts");
        }
        CHECK(tb_writes_posted(&a->e) == before + 1, "every complete write adds exactly one posted message");
        payload += w.len;
        bond_poll(a, b, &w, before + 1, signals + (i == 2 ? 0 : 1));
        CHECK(*(uint64_t *)(void *)(b->mem + w.soff) == (i == 2 ? 17 : 0), "only ordinary signals store directly");
    }
    CHECK(tb_posted_payload(&a->e) == payload && tb_posted_bytes(&a->e) > initial_wire, "posted payload excludes wire heads");
    uint64_t deadline = link_now_ns() + TIMEOUT;
    while (tb_completed_bytes(&a->e) != tb_posted_bytes(&a->e)) {
        CHECK(tb_progress(&a->e) >= 0 && tb_progress(&b->e) >= 0, "earlier SENDs reap before reservation checks");
        CHECK(link_now_ns() < deadline, "earlier SENDs timed out");
    }
    CHECK(tb_rate(&a->e) > 0, "sends of 64 KiB and more measure the link's rate");
    /* An unrepresentable fused header must leave both source bytes and counters untouched for the caller's fallback. */
    uint64_t before = tb_writes_posted(&a->e), wire = tb_posted_bytes(&a->e);
    unsigned char saved[64];
    memcpy(saved, a->mem + TB_SEG - 48, sizeof(saved));
    CHECK(tb_can_bond_write_signal(&a->e, &a->r, TB_SEG + 16, 128) == 2 &&
          tb_can_bond_write_signal(&a->e, &a->r, 64, 0) < 0, "fused preflight distinguishes fallback from invalid input");
    CHECK(tb_bond_write_signal(&a->e, &a->r, TB_SEG + 16, 0, 128, w.soff, 7, w.seq, w.need, 0, 0, 0, TIMEOUT) == 1,
          "bonded fusion across a registration requests fallback");
    CHECK(!memcmp(saved, a->mem + TB_SEG - 48, sizeof(saved)) && tb_writes_posted(&a->e) == before &&
          tb_posted_bytes(&a->e) == wire, "fallback posts and changes nothing");
    CHECK(tb_bond_write_signal(&a->e, &a->r, 4096, 0, 128, w.soff, 7, w.seq, w.need, 0, 4, 0, TIMEOUT) < 0,
          "unknown wait flags are refused");
    CHECK(tb_write_try(&a->e, &a->r, 0, 0, TB_TRY_MAX + 1) < 0, "oversized nonblocking chunks are refused");
    /* Reserve all headers before a source-registration split, then publish both complete placements. */
    w.len = 0;
    CHECK(!tb_write_try(&a->e, &a->r, TB_SEG - 7, 4096, 64), "a bounded registration split posts without waiting");
    CHECK(tb_writes_posted(&a->e) == before + 2, "a source-registration split counts two writes");
    bond_poll(a, b, &w, before + 2, w.signals);
    CHECK(!memcmp(b->mem + 4096, a->mem + TB_SEG - 7, 64), "the source-registration split places every byte");
    /* Stop reaping even the sender: eventually its finite reservation fills, without blocking or progress. */
    for (unsigned i = 0; i < TB_SEND_WR; ++i) {
        int posted = tb_write_try(&a->e, &a->r, 0, 12ull << 20, chunk);
        CHECK(posted >= 0, "credit exhaustion is backpressure rather than a failed link");
        if (posted == 1) break;
    }
    before = tb_writes_posted(&a->e), wire = tb_posted_bytes(&a->e), payload = tb_posted_payload(&a->e);
    CHECK(tb_can_write(&a->e, &a->r, 0, chunk) == 0 &&
          tb_write_try(&a->e, &a->r, 0, 12ull << 20, chunk) == 1, "full queues return immediately with no room");
    CHECK(tb_writes_posted(&a->e) == before && tb_posted_bytes(&a->e) == wire && tb_posted_payload(&a->e) == payload,
          "no-room writes post no partial payload or header");
    CHECK(tb_posted_bytes(&a->e) > tb_completed_bytes(&a->e), "wire-byte backlog tracks unreaped SENDs");
    bond_poll(a, b, &w, before, w.signals);
    deadline = link_now_ns() + TIMEOUT;
    while (tb_completed_bytes(&a->e) != tb_posted_bytes(&a->e)) {
        CHECK(tb_progress(&a->e) >= 0 && tb_progress(&b->e) >= 0, "SEND completions reap");
        CHECK(link_now_ns() < deadline, "SEND completion counters timed out");
    }
    CHECK(tb_completed_payload(&a->e) == tb_posted_payload(&a->e), "completion payload matches every posted byte");
    tb_bond_hooks(&b->e, NULL);
    start(b);
    CHECK(!tb_fence(&a->e, TIMEOUT), "bond transport leaves the ordinary fence operational");
}

/* Run both ends for a while without expecting anything new to land. */
static void idle(struct side *a, struct side *b, unsigned rounds) {
    for (unsigned i = 0; i < rounds; ++i)
        CHECK(tb_progress(&a->e) >= 0 && tb_progress(&b->e) >= 0, "the links stay up while a message waits");
}

static void scenario_tail(struct side *a, struct side *b) {
    stop(b);
    struct bond_watch w = {.rx = b, .count = tb_writes_placed(&b->e), .soff = WINDOW - 128, .ready = 1};
    install(b, &w);
    const uint64_t src = 3ull << 20, len = 20000;
    uint32_t magic = 0x4254434du;
    memcpy(a->mem + src + 100, &magic, 4);
    CHECK(!tb_tail_clean(&a->r, src + 100) && tb_can_bond_tail(&a->e, &a->r, src + 100, len) == 2 &&
          tb_bond_tail(&a->e, &a->r, src + 100, len) == 2, "a tail never starts with a head's magic");
    CHECK(tb_tail_clean(&a->r, src + 101) && tb_can_bond_tail(&a->e, &a->r, src, len) == 1,
          "other bytes can start one");
    CHECK(tb_can_bond_tail(&a->e, &a->r, TB_SEG - 8, 64) == 2, "a tail stays inside one source registration");
    /* A tail that arrives before its announcement waits, unplaced and unpoisoned. */
    uint64_t before = tb_writes_posted(&a->e);
    w.src = a->mem + src, w.dst = 5ull << 20, w.len = len;
    CHECK(!tb_bond_tail(&a->e, &a->r, src, len) && tb_writes_posted(&a->e) == before + 1, "a tail posts as one write");
    idle(a, b, 2000);
    CHECK(w.count == tb_writes_placed(&b->e) && w.count == before, "an unannounced tail does not land");
    w.tail_off[0] = w.dst, w.tail_len[0] = (uint32_t)len, w.tail_ordinal[0] = (uint32_t)(before + 1);
    __atomic_store_n(&w.tails, 1, __ATOMIC_RELEASE);
    bond_poll(a, b, &w, before + 1, w.signals);
    CHECK(!memcmp(b->mem + w.dst, a->mem + src, len), "an announced tail lands where its head said");
    /* A joined part waits for the bond's go-ahead, then announces its tail and queues its signal. */
    uint64_t part = (4ull << 20) + 64, plen = 30000;
    w.ready = 0;
    w.src = a->mem + part, w.dst = 6ull << 20, w.len = plen;
    w.seq = 9, w.need[0] = before + 2, w.need[1] = 77, w.value = 1234;
    CHECK(!tb_bond_write_signal(&a->e, &a->r, part, w.dst, plen, w.soff, w.value, w.seq, w.need, 5000, BOND_WAIT_LINK,
                                76, TIMEOUT), "a joined part with a tail posts");
    idle(a, b, 2000);
    CHECK(!w.heard && w.count == before + 1, "a joined part waits until the bond says it may land");
    __atomic_store_n(&w.ready, 1, __ATOMIC_RELEASE);
    bond_poll(a, b, &w, before + 2, w.signals + 1);
    CHECK(w.heard == 1 && w.heard_off == w.dst + plen && w.heard_len == 5000 && w.heard_ordinal == 77,
          "it announces its tail: right after its bytes, with the other link's write count");
    /* A tail whose length differs from its announcement fails the link instead of landing. */
    w.len = 0;
    w.tail_off[1] = 7ull << 20, w.tail_len[1] = 999, w.tail_ordinal[1] = (uint32_t)(before + 3);
    __atomic_store_n(&w.tails, 2, __ATOMIC_RELEASE);
    g_quiet = 1;
    CHECK(!tb_bond_tail(&a->e, &a->r, src, 1000), "a mismatched tail posts");
    uint64_t deadline = link_now_ns() + TIMEOUT;
    while (tb_progress(&b->e) >= 0) {
        CHECK(tb_progress(&a->e) >= 0, "the sender stays up");
        CHECK(link_now_ns() < deadline, "a mismatched tail did not fail the link");
    }
    CHECK(tb_failure(&b->e) && strstr(tb_failure(&b->e), "tail"), "the receiver says the tail did not match");
    b->failed = 1;
}

/* Send one write from a to b and progress b with `wanted` until it lands. */
static void land(struct side *a, struct side *b, uint64_t len, const int *wanted) {
    uint64_t before = tb_writes_placed(&b->e), deadline = link_now_ns() + TIMEOUT;
    CHECK(!tb_write(&a->e, &a->r, 1ull << 20, 2ull << 20, len, TIMEOUT), "a write posts");
    while (tb_writes_placed(&b->e) == before) {
        CHECK((wanted ? tb_progress_some(&b->e, wanted) : tb_progress(&b->e)) >= 0 && tb_progress(&a->e) >= 0,
              "both ends progress");
        CHECK(link_now_ns() < deadline, "the write never landed");
    }
}

static void scenario_repost(struct side *a, struct side *b) {
    stop(b);
    int wanted = 0;
    /* the receives a landed message used wait a moment, so a reply posted right away does not queue behind them */
    land(a, b, 40960, &wanted);
    CHECK(tb_unposted(&b->e) >= 10, "a progress thread leaves the landed message's receives for a moment");
    /* while a caller waits for the lock, they stay back, however long it waits */
    wanted = 1;
    uint64_t until = link_now_ns() + 3 * TB_REPOST_WAIT_NS;
    while (link_now_ns() < until) CHECK(tb_progress_some(&b->e, &wanted) >= 0, "progress while a caller waits");
    CHECK(tb_unposted(&b->e) >= 10, "a waiting caller keeps the receives back");
    /* then they go back TB_RECV_BATCH a pass, so a send that comes meanwhile waits behind no more than those */
    wanted = 0;
    uint64_t deadline = link_now_ns() + TIMEOUT;
    while (tb_unposted(&b->e)) {
        uint64_t before = tb_unposted(&b->e);
        CHECK(tb_progress_some(&b->e, &wanted) >= 0, "progress puts the receives back");
        CHECK(tb_unposted(&b->e) + TB_RECV_BATCH >= before, "a pass puts back no more than TB_RECV_BATCH receives");
        CHECK(link_now_ns() < deadline, "the receives never went back");
    }
    /* plain progress, as a sender waiting for room uses, puts them back at once */
    land(a, b, 40960, NULL);
    CHECK(!tb_unposted(&b->e), "plain progress puts every receive back at once");
    /* a ring more than a quarter used refills at once, as a stream needs */
    for (int i = 0; i < 3; ++i) land(a, b, 1ull << 20, &wanted);
    CHECK(tb_unposted(&b->e) < 512, "a quarter-used ring refills without waiting");
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    g_mode = argv[1];
    setvbuf(stderr, NULL, _IOLBF, 0);
    const char *seed = getenv("STUB_SEED");
    if (seed) g_rng ^= strtoull(seed, NULL, 10) * 0x2545f4914f6cdd1dull;
    static struct side a, b;
    int bounds = !strcmp(g_mode, "bounds");
    open_side(&a, "tb0", WINDOW);
    open_side(&b, "tb1", bounds ? HALF : WINDOW);
    join_pair(&a, &b);
    start(&b);
    if (!strcmp(g_mode, "order")) scenario_order(&a, &b);
    else if (!strcmp(g_mode, "overlap")) scenario_overlap(&a, &b);
    else if (!strcmp(g_mode, "joined")) {
        scenario_joined(&a, &b);
        scenario_bond(&a, &b);
    }
    else if (!strcmp(g_mode, "bond")) scenario_bond(&a, &b);
    else if (!strcmp(g_mode, "tail")) scenario_tail(&a, &b);
    else if (!strcmp(g_mode, "repost")) scenario_repost(&a, &b);
    else if (!strcmp(g_mode, "random")) scenario_random(&a, &b);
    else if (bounds) g_quiet = 1, scenario_bounds(&a, &b);
    else if (!strcmp(g_mode, "teardown")) scenario_teardown(&a, &b);
    else return 2;
    stop(&b);
    CHECK(bounds || !strcmp(g_mode, "tail") || !b.failed, "the receiver's link failed");
    ep_close(&a.e);
    ep_close(&b.e);
    printf("test_link_tb %s: ok\n", g_mode);
    return 0;
}

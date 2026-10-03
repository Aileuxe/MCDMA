/* Offline test of the Thunderbolt placement protocol under the stub's TN3205 rules, one scenario per run:
 *   order     every signal lands after the writes before it, from 1 byte to past a registration boundary
 *   overlap   later writes to the same bytes win, whichever path (inline or data queue) each one took
 *   random    thousands of random writes and signals both ways at once match a replayed mirror
 *   bounds    a write outside what the receiver accepts fails the link instead of landing
 *   teardown  queue pairs with sends still waiting for credit are torn down without misuse
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
    CHECK(ia.transport == LINK_TB && ia.qpn2 && ia.frames && ia.table.n == 0, "a thunderbolt offer carries no keys");
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
    const uint64_t sizes[] = {1, 7, 4064, 4065, 8192, 65536 + 3, (1ull << 20) + 5, (5ull << 20) + 1, 13ull << 20};
    const uint64_t flag = WINDOW - 64;
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        uint64_t len = sizes[i], src = (rnd() % 64) * 8 + 1;
        /* odd sizes end just past the 12 MiB registration boundary */
        uint64_t dst = i % 2 ? TB_SEG - len / 2 - 3 : 4096 * (uint64_t)i;
        g_watch.dst = dst, g_watch.len = len, g_watch.src = a->mem + src;
        __atomic_store_n(&g_watch.value, 1000 + i, __ATOMIC_RELEASE);
        CHECK(!tb_write(&a->e, &a->r, src, dst, len, TB_SEG, TIMEOUT), "a write is posted");
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

static void scenario_overlap(struct side *a, struct side *b) {
    const uint64_t at = 3ull << 20, flag = WINDOW - 64;
    static unsigned char expect[256 << 10];
    for (unsigned round = 0; round < 60; ++round) {
        uint64_t big = 65536 + rnd() % 65536, small = 1 + rnd() % 4000, mid = rnd() % (big - small);
        uint64_t s1 = (rnd() % 256) * 4096, s2 = (rnd() % 256) * 4096 + 7, s3 = (rnd() % 256) * 4096 + 11;
        memcpy(expect, a->mem + s1, big);
        memcpy(expect + mid, a->mem + s2, small);
        CHECK(!tb_write(&a->e, &a->r, s1, at, big, TB_SEG, TIMEOUT), "a data write");
        CHECK(!tb_write(&a->e, &a->r, s2, at + mid, small, TB_SEG, TIMEOUT), "an inline write over it");
        if (round % 2) {
            memcpy(expect, a->mem + s3, big);
            CHECK(!tb_write(&a->e, &a->r, s3, at, big, TB_SEG, TIMEOUT), "a data write over both");
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
        if (o->len ? tb_write(&j->me->e, &j->me->r, o->src, o->dst, o->len, TB_SEG, TIMEOUT)
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
    CHECK(!tb_write(&a->e, &a->r, 0, HALF + 4096, 100, TB_SEG, TIMEOUT), "the sender cannot see the receiver's limit");
    uint64_t deadline = link_now_ns() + TIMEOUT;
    while (!b->failed) CHECK(link_now_ns() < deadline, "a write past the accepted range did not fail the link");
    stop(b);
    CHECK(tb_progress(&b->e) < 0, "a failed link stays failed");
}

static void scenario_teardown(struct side *a, struct side *b) {
    stop(b);
    for (uint64_t i = 0; i < 20; ++i)
        CHECK(!tb_write(&a->e, &a->r, 0, i * 16384, 8192 + 100, TB_SEG, TIMEOUT), "a write is queued");
    CHECK(tb_busy(&a->e), "sends wait for the receiver's credit");
    ep_destroy_qp(&a->e);
    ep_destroy_qp(&b->e);
    CHECK(!a->e.qp && !a->e.cq && !b->e.qp && !a->e.tb, "queue pairs are gone");
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
    else if (!strcmp(g_mode, "random")) scenario_random(&a, &b);
    else if (bounds) g_quiet = 1, scenario_bounds(&a, &b);
    else if (!strcmp(g_mode, "teardown")) scenario_teardown(&a, &b);
    else return 2;
    stop(&b);
    CHECK(bounds || !b.failed, "the receiver's link failed");
    ep_close(&a.e);
    ep_close(&b.e);
    printf("test_link_tb %s: ok\n", g_mode);
    return 0;
}

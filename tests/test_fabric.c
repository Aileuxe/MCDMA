/* Offline test of libmcdma-fabric: two ranks in one process over the stub's devices, meeting through the exchange on
 * the loopback's link-local address. One scenario per run:
 *   tb        Thunderbolt: double-buffered collective steps both ways, a progress thread on one side, goodbyes
 *   roce      RoCE with every key checked: the same steps, READ, and RoCE's signal staging
 *   dmabuf    a DMA-BUF window registers through ibv_reg_dmabuf_mr with no fallback; Thunderbolt refuses one
 *   mismatch  a Thunderbolt rank and a RoCE rank refuse each other
 *   args      bad windows, flags, names and interfaces are refused
 *   bond      two Thunderbolt links carry one peer's byte-checked collective
 *   bond-args strict device/via parsing, and local lane counts agree
 *   bond-mismatch a one-link peer cannot be mistaken for the first child of a bond
 *   bond-order signals wait for placement on a delayed link
 *   bond-schedule a delayed link does not stop nonoverlapping work on the faster link
 *   bond-overlap a later overlapping call cannot overtake a delayed earlier write
 *   bond-fail one failed child poisons the whole bonded peer
 *   bond-credit a bounded receiver's pending-signal queue backpressures the sender
 *   bond-small small writes stay on one link, also at tiny queue depths and registration boundaries; a
 *             16 KiB write-and-signal is cut across both
 *   bond-wait a failed peer does not poison an unrelated live peer's flag wait
 *   bond-names public 19- and 20-character names fit the internal lane names
 *   zero      a zero-length write_signal is a signal on every link kind and leaves the peer up
 *   down      a failed link's end tells the other, which reports why within a second, even mid-flush
 *   bond-down one failed link of a bond takes the whole peer down at both ends, with the reason
 *   bond-plan the cut: both links finish together by rate and queue, whole packets, registrations, magic, limits
 *   bond-stripe 16 KiB to 3 MiB write-and-signals go as exactly one message on each link; the flag waits for both
 *   bond-pingpong a tensor-parallel exchange: both ranks write-and-signal 10-160 KB and spin on the flag word while
 *             another thread signals; every byte checked, both links carry the traffic, small ones alternate
 *   bond-later a later write-and-signal cut differently, or over a word an earlier signal stores, still wins
 *   bond-magic a tail whose natural first bytes are a head's magic starts one byte later instead
 *   bond-uneven a link measured several times slower carries a smaller share of each cut write-and-signal
 * Exit 0 means pass, 77 that loopback has no usable IPv6 link-local or IPv4 address. */
#include "../rpc/mcdma_fabric.h"
#include "../rpc/link.h"

#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define WINDOW (32ull << 20)
#define SLOT (4ull << 20)           /* step parity p lands at p * SLOT; sources live in the upper half */
#define FLAG (WINDOW - 4096)
#define STEPS 80
#define SECOND 1000000000ull

struct rank {
    const char *device;
    const char *via;
    const char *name;
    uint32_t flags;
    unsigned char *mem;
    struct mcdma_fabric *f;
    struct mcdma_fabric_peer *p;
    int me, port, peer_port, status;
};

static const char *g_mode;
static char g_lo[64], g_bond_via[128];

void stub_tb_hold(const char *name, int hold);
void stub_tb_hold_completions(const char *name, int hold);
uint64_t stub_tb_sent_bytes(const char *name);
uint64_t stub_tb_received_bytes(const char *name);
void stub_tb_fail(const char *name);
uint64_t stub_tb_sends(const char *name);
void stub_tb_pace(const char *name, unsigned every);
int mcdma_fabric_test_plan(const struct region *win, const uint64_t backlog[2], const uint64_t rate[2],
                           const uint64_t max[2], uint64_t off, uint64_t len, unsigned *turn, unsigned *head,
                           uint64_t *first);

static void fail(const char *what) {
    fprintf(stderr, "test_fabric %s: %s\n", g_mode, what);
    _exit(1);
}

#define CHECK(c, what)        \
    do {                      \
        if (!(c)) fail(what); \
    } while (0)

static int free_ports(int count) {
    int family = strchr(g_lo, '/') ? AF_INET : AF_INET6;
    int fd = socket(family, SOCK_DGRAM, 0), second = -1;
    union { struct sockaddr_in v4; struct sockaddr_in6 v6; } a;
    socklen_t len = family == AF_INET ? sizeof(a.v4) : sizeof(a.v6);
    memset(&a, 0, sizeof(a));
    if (family == AF_INET) a.v4.sin_family = AF_INET;
    else a.v6.sin6_family = AF_INET6;
    if (fd < 0) return -1;
    if (bind(fd, (struct sockaddr *)&a, len) || getsockname(fd, (struct sockaddr *)&a, &len)) {
        close(fd);
        return -1;
    }
    int port = ntohs(family == AF_INET ? a.v4.sin_port : a.v6.sin6_port);
    if (count == 2) {
        if (port == 65535 || (second = socket(family, SOCK_DGRAM, 0)) < 0) {
            close(fd);
            return -1;
        }
        if (family == AF_INET) a.v4.sin_port = htons((uint16_t)(port + 1));
        else a.v6.sin6_port = htons((uint16_t)(port + 1));
        if (bind(second, (struct sockaddr *)&a, len)) {
            close(second);
            close(fd);
            return -1;
        }
        close(second);
    }
    close(fd);
    return port;
}

static int free_port(void) { return free_ports(1); }

static unsigned char *window(void) {
    unsigned char *m = mmap(NULL, WINDOW, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(m != MAP_FAILED, "window memory");
    return m;
}

static unsigned char pattern(int rank, uint64_t step, uint64_t i) {
    return (unsigned char)(i * 7 + step * 13 + rank * 101);
}

static void *connect_rank(void *arg) {
    struct rank *r = arg;
    r->status = mcdma_fabric_connect(r->f, r->via, r->port, r->peer_port, r->name, 5 * SECOND, &r->p);
    return NULL;
}

static void open_rank(struct rank *r, int me, const char *device, uint32_t flags) {
    memset(r, 0, sizeof(*r));
    r->me = me, r->device = device, r->flags = flags, r->mem = window();
    r->via = strchr(device, '+') ? g_bond_via : g_lo;
    r->name = "pair";
    CHECK(!mcdma_fabric_open(device, 0, 4096, r->mem, WINDOW, -1, 0, flags, &r->f), "a window registers");
}

/* Both ranks call connect at once, as two machines would. */
static void meet(struct rank *a, struct rank *b) {
    int lanes = strchr(a->device, '+') || strchr(b->device, '+') ? 2 : 1;
    do { a->port = free_ports(lanes); } while (a->port < 0);
    do { b->port = free_ports(lanes); } while (b->port < 0 || abs(a->port - b->port) < lanes);
    b->peer_port = a->port;
    a->peer_port = b->port;
    pthread_t t;
    CHECK(!pthread_create(&t, NULL, connect_rank, b), "thread");
    connect_rank(a);
    pthread_join(t, NULL);
}

static uint64_t flag_of(const struct rank *r) {
    return __atomic_load_n((uint64_t *)(void *)(r->mem + FLAG), __ATOMIC_ACQUIRE);
}

static volatile int g_finished;

/* One rank's side of the collective: write a chunk into the peer's slot, signal the step, wait for the peer's. */
static void *steps(void *arg) {
    struct rank *r = arg;
    for (uint64_t step = 1; step <= STEPS; ++step) {
        uint64_t rows = 1 + (step * 37) % 300, bytes = rows * 12288 + (step % 3), parity = step % 2;
        unsigned char *src = r->mem + WINDOW / 2 + parity * SLOT;
        for (uint64_t i = 0; i < bytes; ++i) src[i] = pattern(r->me, step, i);
        if (mcdma_fabric_write(r->p, WINDOW / 2 + parity * SLOT, parity * SLOT, bytes) ||
            mcdma_fabric_signal(r->p, FLAG, step))
            fail("a step could not be posted");
        if (mcdma_fabric_wait(r->f, FLAG, step, 10 * SECOND)) fail("the peer's step never arrived");
        for (uint64_t i = 0; i < bytes; ++i)
            if (r->mem[parity * SLOT + i] != pattern(!r->me, step, i)) fail("a step's bytes landed wrong or late");
    }
    if (mcdma_fabric_flush(r->p, 10 * SECOND)) fail("flush");
    __atomic_add_fetch(&g_finished, 1, __ATOMIC_ACQ_REL);
    return NULL;
}

static void collective(struct rank *a, struct rank *b) {
    pthread_t t;
    g_finished = 0;
    CHECK(!pthread_create(&t, NULL, steps, b), "thread");
    steps(a);
    /* a rank without a progress thread keeps progressing until its peer has flushed too */
    while (__atomic_load_n(&g_finished, __ATOMIC_ACQUIRE) < 2) CHECK(!mcdma_fabric_progress(a->f), "progress");
    pthread_join(t, NULL);
    CHECK(flag_of(a) == STEPS && flag_of(b) == STEPS, "every step landed");
}

static void close_pair(struct rank *a, struct rank *b) {
    mcdma_fabric_close(&a->f);
    mcdma_fabric_close(&b->f);
    CHECK(!a->f && !b->f, "closed handles are cleared");
    munmap(a->mem, WINDOW);
    munmap(b->mem, WINDOW);
}

static void scenario_tb(void) {
    struct rank a, b;
    open_rank(&a, 0, "tb0", 0);
    open_rank(&b, 1, "tb1", MCDMA_FABRIC_PROGRESS_THREAD);
    meet(&a, &b);
    CHECK(!a.status && !b.status, "both ranks connect");
    CHECK(mcdma_fabric_link_count(a.p) == 1, "a single link still reports one physical link");
    CHECK(mcdma_fabric_link(a.p) == MCDMA_FABRIC_THUNDERBOLT && mcdma_fabric_peer_length(a.p) == WINDOW, "link facts");
    uint64_t old = 1;
    CHECK(mcdma_fabric_read(a.p, 0, 0, 8) == MCDMA_FABRIC_UNSUPPORTED, "Thunderbolt has no READ");
    CHECK(mcdma_fabric_fetch_add(a.p, 0, 1, &old) == MCDMA_FABRIC_UNSUPPORTED && !old, "no atomics");
    CHECK(mcdma_fabric_write(a.p, 0, WINDOW - 4, 8) == MCDMA_FABRIC_BOUNDS, "a write past the peer's window");
    CHECK(mcdma_fabric_write(a.p, WINDOW - 4, 0, 8) == MCDMA_FABRIC_BOUNDS, "a write past this window");
    CHECK(mcdma_fabric_signal(a.p, FLAG + 4, 1) == MCDMA_FABRIC_BOUNDS, "a misaligned signal");
    collective(&a, &b);
    /* a goodbye reaches the other side, whose next write fails instead of waiting forever */
    mcdma_fabric_disconnect(&a.p);
    CHECK(!a.p, "a disconnected handle is cleared");
    int status = MCDMA_FABRIC_OK;
    for (int i = 0; i < 200 && status == MCDMA_FABRIC_OK; ++i) {
        usleep(10000);
        status = mcdma_fabric_signal(b.p, FLAG, 1);
    }
    CHECK(status == MCDMA_FABRIC_PEER, "the peer that heard goodbye is down");
    close_pair(&a, &b);
}

static void scenario_roce(void) {
    struct rank a, b;
    open_rank(&a, 0, "roce0", 0);
    open_rank(&b, 1, "roce1", 0);
    meet(&a, &b);
    CHECK(!a.status && !b.status, "both ranks connect");
    CHECK(mcdma_fabric_link(a.p) == MCDMA_FABRIC_ROCE, "a RoCE link");
    collective(&a, &b);
    /* READ crosses registration boundaries in 2 MiB pieces */
    for (uint64_t i = 0; i < (9ull << 20); ++i) b.mem[(3ull << 20) + i] = pattern(1, 99, i);
    CHECK(!mcdma_fabric_read(a.p, 20ull << 20, 3ull << 20, 9ull << 20), "a READ");
    for (uint64_t i = 0; i < (9ull << 20); ++i)
        CHECK(a.mem[(20ull << 20) + i] == pattern(1, 99, i), "READ brought the peer's bytes");
    /* signals outnumber the staging words many times over and never reuse one in flight */
    for (uint64_t i = 1; i <= 5000; ++i) CHECK(!mcdma_fabric_signal(a.p, FLAG, i), "a signal");
    CHECK(!mcdma_fabric_flush(a.p, 10 * SECOND) && flag_of(&b) == 5000, "the last signal is the one that stays");
    close_pair(&a, &b);
}

static void scenario_dmabuf(void) {
    extern int stub_dmabuf_fd, stub_dmabuf_count;
    extern uint64_t stub_dmabuf_offset;
    unsigned char *mem = window();
    struct mcdma_fabric *f = NULL;
    int fd = open("/dev/null", O_RDONLY);
    CHECK(fd >= 0, "a descriptor to stand in for a DMA-BUF");
    CHECK(mcdma_fabric_open("tb0", 0, 4096, mem, WINDOW, fd, 0, 0, &f) == MCDMA_FABRIC_UNSUPPORTED && !f,
          "Thunderbolt takes no DMA-BUF");
    CHECK(!mcdma_fabric_open("roce0", 0, 4096, mem, WINDOW, fd, 65536, 0, &f), "a DMA-BUF window registers");
    CHECK(stub_dmabuf_fd == fd && stub_dmabuf_offset == 65536 && stub_dmabuf_count >= 1, "through ibv_reg_dmabuf_mr");
    mcdma_fabric_close(&f);
    close(fd);
    munmap(mem, WINDOW);
}

static void scenario_mismatch(void) {
    struct rank a, b;
    open_rank(&a, 0, "tb0", 0);
    open_rank(&b, 1, "roce0", 0);
    meet(&a, &b);
    CHECK(a.status == MCDMA_FABRIC_PEER && b.status == MCDMA_FABRIC_PEER && !a.p && !b.p, "different links refuse");
    close_pair(&a, &b);
}

static void scenario_args(void) {
    unsigned char *mem = window();
    struct mcdma_fabric *f = NULL;
    struct mcdma_fabric_peer *p = NULL;
    CHECK(mcdma_fabric_abi() == MCDMA_FABRIC_ABI, "ABI 1");
    CHECK(mcdma_fabric_open("tb0", 0, 4096, mem + 64, WINDOW - 16384, -1, 0, 0, &f) == MCDMA_FABRIC_INVALID && !f,
          "an unaligned window");
    CHECK(mcdma_fabric_open("tb0", 0, 4096, mem, WINDOW, -1, 0, 0x80, &f) == MCDMA_FABRIC_INVALID, "unknown flags");
    CHECK(mcdma_fabric_open("nope0", 0, 4096, mem, WINDOW, -1, 0, 0, &f) == MCDMA_FABRIC_DEVICE, "a missing device");
    CHECK(!mcdma_fabric_open("tb0", 0, 4096, mem, WINDOW, -1, 0, 0, &f), "a good window");
    CHECK(mcdma_fabric_connect(f, g_lo, 18000, 0, "bad name!", SECOND, &p) == MCDMA_FABRIC_INVALID && !p, "a bad name");
    CHECK(mcdma_fabric_connect(f, "no-such-if0", 18000, 0, "pair", SECOND, &p) == MCDMA_FABRIC_INVALID, "no interface");
    CHECK(mcdma_fabric_connect(f, g_lo, free_port(), 0, "pair", 300000000ull, &p) == MCDMA_FABRIC_TIMEOUT && !p,
          "nobody answers");
    CHECK(mcdma_fabric_wait(f, 4, 1, 1000) == MCDMA_FABRIC_BOUNDS, "a misaligned wait");
    CHECK(mcdma_fabric_wait(f, 0, ~0ull, 1000000) == MCDMA_FABRIC_TIMEOUT, "a wait times out");
    mcdma_fabric_close(&f);
    munmap(mem, WINDOW);
}

static void open_bond_pair(struct rank *a, struct rank *b) {
    open_rank(a, 0, "tb0+tb2", 0);
    open_rank(b, 1, "tb1+tb3", 0);
    meet(a, b);
    CHECK(!a->status && !b->status, "both bonded ranks connect");
    CHECK(mcdma_fabric_link_count(a->p) == 2 && mcdma_fabric_link_count(b->p) == 2,
          "a bond reports both physical links");
    CHECK(mcdma_fabric_link(a->p) == MCDMA_FABRIC_THUNDERBOLT && mcdma_fabric_peer_length(a->p) == WINDOW,
          "bonded link facts");
}

static void scenario_bond(void) {
    struct rank a, b;
    open_bond_pair(&a, &b);
    uint64_t bytes0 = stub_tb_sent_bytes("tb0"), bytes2 = stub_tb_sent_bytes("tb2");
    collective(&a, &b);
    CHECK(stub_tb_sent_bytes("tb0") > bytes0 + 4096 && stub_tb_sent_bytes("tb2") > bytes2 + 4096,
          "both physical links carry data");
    CHECK(mcdma_fabric_read(a.p, 0, 0, 8) == MCDMA_FABRIC_UNSUPPORTED, "bonded Thunderbolt has no READ");
    unsigned char *src = a.mem + WINDOW / 2 + MCDMA_FABRIC_WS_ROOM;
    for (size_t i = 0; i < 257; ++i) src[i] = pattern(0, 101, i);
    CHECK(!mcdma_fabric_write_signal(a.p, WINDOW / 2 + MCDMA_FABRIC_WS_ROOM, 0, 257, FLAG, STEPS + 1),
          "a bonded small write and signal");
    CHECK(!mcdma_fabric_wait(b.f, FLAG, STEPS + 1, 5 * SECOND), "a bonded joined signal arrives");
    CHECK(!memcmp(src, b.mem, 257), "the joined signal follows its bytes");
    CHECK(!mcdma_fabric_flush(a.p, 5 * SECOND), "the bonded joined write flushes");
    struct mcdma_fabric_link_stats stats[2];
    CHECK(!mcdma_fabric_link_stats(a.p, 0, &stats[0]) && !mcdma_fabric_link_stats(a.p, 1, &stats[1]),
          "both physical links expose counters");
    uint64_t total = 257;
    for (uint64_t step = 1; step <= STEPS; ++step) total += (1 + (step * 37) % 300) * 12288 + step % 3;
    CHECK(stats[0].posted_bytes && stats[1].posted_bytes && stats[0].posted_bytes + stats[1].posted_bytes == total,
          "payload counters exclude framing and signals");
    CHECK(stats[0].completed_bytes == stats[0].posted_bytes && stats[1].completed_bytes == stats[1].posted_bytes,
          "flush completes every physical link's payload");
    CHECK(mcdma_fabric_link_stats(a.p, 2, &stats[0]) == MCDMA_FABRIC_INVALID,
          "an out-of-range physical link is refused");
    CHECK(mcdma_fabric_link_stats(a.p, 0, NULL) == MCDMA_FABRIC_INVALID, "a counter output is required");
    close_pair(&a, &b);
}

static void scenario_bond_args(void) {
    unsigned char *mem = window();
    struct mcdma_fabric *f = NULL;
    struct mcdma_fabric_peer *p = NULL;
    const char *bad[] = {"+tb0", "tb0+", "tb0++tb2", "tb0+tb0", "tb0 +tb2", "tb0+ tb2"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        CHECK(mcdma_fabric_open(bad[i], 0, 4096, mem, WINDOW, -1, 0, 0, &f) == MCDMA_FABRIC_INVALID && !f,
              "a malformed bond device list");
    CHECK(mcdma_fabric_open("tb0+roce0", 0, 4096, mem, WINDOW, -1, 0, 0, &f) == MCDMA_FABRIC_UNSUPPORTED && !f,
          "a mixed transport bond is unsupported");
    CHECK(mcdma_fabric_open("roce0+roce1", 0, 4096, mem, WINDOW, -1, 0, 0, &f) == MCDMA_FABRIC_UNSUPPORTED && !f,
          "a RoCE pair is unsupported");
    CHECK(!mcdma_fabric_open("tb0+tb2", 0, 4096, mem, WINDOW, -1, 0, 0, &f), "a valid bond opens");
    CHECK(mcdma_fabric_connect(f, g_lo, 18000, 18002, "pair", 1000000, &p) == MCDMA_FABRIC_TIMEOUT && !p,
          "one meeting via is accepted but nobody answers");
    const char *bad_via[] = {"+lo", "lo+", "lo++lo", "lo+lo+lo", "lo +lo", "lo+ lo"};
    for (size_t i = 0; i < sizeof(bad_via) / sizeof(bad_via[0]); ++i)
        CHECK(mcdma_fabric_connect(f, bad_via[i], 18000, 18002, "pair", SECOND, &p) == MCDMA_FABRIC_INVALID && !p,
              "a malformed bond via list");
    CHECK(mcdma_fabric_connect(f, g_bond_via, 65535, 18002, "pair", SECOND, &p) == MCDMA_FABRIC_INVALID && !p,
          "two lanes cannot bind past port 65535");
    CHECK(mcdma_fabric_connect(f, g_bond_via, 18000, 65535, "pair", SECOND, &p) == MCDMA_FABRIC_INVALID && !p,
          "two peer lanes cannot run past port 65535");
    mcdma_fabric_close(&f);
    CHECK(!mcdma_fabric_open("tb0", 0, 4096, mem, WINDOW, -1, 0, 0, &f), "a single link still opens");
    CHECK(mcdma_fabric_connect(f, g_bond_via, 18000, 18002, "pair", SECOND, &p) == MCDMA_FABRIC_INVALID && !p,
          "a single device refuses two via entries");
    mcdma_fabric_close(&f);
    munmap(mem, WINDOW);
}

static void scenario_bond_mismatch(void) {
    struct rank a, b;
    open_rank(&a, 0, "tb0+tb2", 0);
    open_rank(&b, 1, "tb1", MCDMA_FABRIC_PROGRESS_THREAD);
    b.name = "pair-0"; /* match the first child's exchange name so its mode, not a name miss, refuses admission */
    meet(&a, &b);
    CHECK(a.status == MCDMA_FABRIC_PEER && b.status == MCDMA_FABRIC_PEER && !a.p && !b.p,
          "a single peer cannot satisfy a bond's first-child exchange");
    close_pair(&a, &b);
}

/* Deliver the fast child's bytes while the other child's receive completions are held. */
static void scenario_bond_order(void) {
    struct rank a, b;
    open_bond_pair(&a, &b);
    const uint64_t bytes = 3ull << 20;
    for (uint64_t i = 0; i < bytes; ++i) a.mem[WINDOW / 2 + i] = pattern(0, 23, i);
    uint64_t before = stub_tb_received_bytes("tb3");
    stub_tb_hold_completions("tb1", 1);
    CHECK(!mcdma_fabric_write(a.p, WINDOW / 2, 0, bytes), "a striped write posts while one receiver is delayed");
    CHECK(!mcdma_fabric_signal(a.p, FLAG, 1) && !mcdma_fabric_signal(a.p, FLAG, 2), "two later signals post");
    for (int i = 0; i < 1000 && stub_tb_received_bytes("tb3") <= before + 4096; ++i) {
        CHECK(!mcdma_fabric_progress(b.f), "the fast child keeps progressing");
        usleep(1000);
    }
    CHECK(stub_tb_received_bytes("tb3") > before + 4096, "the fast link delivers beside the delayed link");
    usleep(30000);
    CHECK(flag_of(&b) == 0, "signals do not use send or receive-fill counts as placement watermarks");
    stub_tb_hold_completions("tb1", 0);
    CHECK(!mcdma_fabric_wait(b.f, FLAG, 2, 5 * SECOND), "both pending signals resolve after placement");
    CHECK(!mcdma_fabric_flush(a.p, 5 * SECOND), "the ordered bond flushes both links");
    for (uint64_t i = 0; i < bytes; ++i) CHECK(b.mem[i] == pattern(0, 23, i), "all striped bytes precede the signal");
    close_pair(&a, &b);
}

static void scenario_bond_schedule(void) {
    struct rank a, b;
    open_bond_pair(&a, &b);
    const uint64_t bytes = 1ull << 20;
    for (uint64_t i = 0; i < bytes; ++i) a.mem[WINDOW / 2 + i] = pattern(0, 37, i);
    uint64_t slow = stub_tb_sent_bytes("tb0"), fast = stub_tb_sent_bytes("tb2");
    stub_tb_hold("tb1", 1);
    for (uint64_t i = 0; i < 4; ++i) {
        CHECK(!mcdma_fabric_write(a.p, WINDOW / 2, i * bytes, bytes), "a later disjoint write posts beside a slow link");
        usleep(30000); /* let the healthy child's completions drain, leaving only the delayed backlog */
    }
    slow = stub_tb_sent_bytes("tb0") - slow;
    fast = stub_tb_sent_bytes("tb2") - fast;
    CHECK(slow > 4096 && fast > 2 * slow, "new work favors the child whose backlog drained");
    CHECK(!mcdma_fabric_signal(a.p, FLAG, 1), "the degraded bond's signal posts");
    CHECK(flag_of(&b) == 0, "fast delivery cannot publish a partially placed collective");
    stub_tb_hold("tb1", 0);
    CHECK(!mcdma_fabric_wait(b.f, FLAG, 1, 5 * SECOND), "the slow child eventually releases the signal");
    CHECK(!mcdma_fabric_flush(a.p, 5 * SECOND), "the degraded bond flushes");
    for (uint64_t i = 0; i < 4 * bytes; ++i)
        CHECK(b.mem[i] == pattern(0, 37, i % bytes), "degraded-link scheduling preserves every byte");
    close_pair(&a, &b);
}

struct overlap_job {
    struct mcdma_fabric_peer *p;
    int started, done, status;
};

static void *post_overlap(void *arg) {
    struct overlap_job *j = arg;
    __atomic_store_n(&j->started, 1, __ATOMIC_RELEASE);
    j->status = mcdma_fabric_write(j->p, WINDOW / 2 + SLOT, 0, 3ull << 20);
    __atomic_store_n(&j->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void scenario_bond_overlap(void) {
    struct rank a, b;
    open_bond_pair(&a, &b);
    const uint64_t bytes = 3ull << 20;
    for (uint64_t i = 0; i < bytes; ++i) {
        a.mem[WINDOW / 2 + i] = pattern(0, 41, i);
        a.mem[WINDOW / 2 + SLOT + i] = pattern(0, 42, i);
    }
    stub_tb_hold_completions("tb1", 1);
    CHECK(!mcdma_fabric_write(a.p, WINDOW / 2, 0, bytes), "the first overlapping range posts");
    struct overlap_job job = { .p = a.p };
    pthread_t thread;
    CHECK(!pthread_create(&thread, NULL, post_overlap, &job), "overlap posting thread");
    while (!__atomic_load_n(&job.started, __ATOMIC_ACQUIRE)) usleep(1000);
    usleep(30000);
    CHECK(!__atomic_load_n(&job.done, __ATOMIC_ACQUIRE), "an overlapping call waits for the earlier placement");
    stub_tb_hold_completions("tb1", 0);
    pthread_join(thread, NULL);
    CHECK(!job.status, "the overlapping call resumes after both links drain");
    CHECK(!mcdma_fabric_signal(a.p, FLAG, 1) && !mcdma_fabric_wait(b.f, FLAG, 1, 5 * SECOND), "the later write signals");
    for (uint64_t i = 0; i < bytes; ++i) CHECK(b.mem[i] == pattern(0, 42, i), "the later overlapping call wins");
    CHECK(!mcdma_fabric_flush(a.p, 5 * SECOND), "the overlapping bond flushes");
    close_pair(&a, &b);
}

static void scenario_bond_fail(void) {
    struct rank a, b;
    open_bond_pair(&a, &b);
    CHECK(!mcdma_fabric_signal(a.p, FLAG, 1) && !mcdma_fabric_wait(b.f, FLAG, 1, 5 * SECOND), "the bond starts live");
    CHECK(!mcdma_fabric_flush(a.p, 5 * SECOND), "the live bond flushes");
    memset(a.mem + WINDOW / 2, 0x5a, 3ull << 20);
    stub_tb_hold("tb1", 1);
    CHECK(!mcdma_fabric_write(a.p, WINDOW / 2, 0, 3ull << 20) && !mcdma_fabric_signal(a.p, FLAG, 2),
          "a signal waits behind a missing child's write");
    usleep(30000);
    CHECK(flag_of(&b) == 1, "the pending signal cannot publish incomplete writes");
    stub_tb_fail("tb1");
    stub_tb_hold("tb1", 0);
    int status = MCDMA_FABRIC_OK;
    for (int i = 0; i < 1000 && status == MCDMA_FABRIC_OK; ++i) {
        status = mcdma_fabric_signal(b.p, FLAG, 2);
        usleep(1000);
    }
    CHECK(status == MCDMA_FABRIC_PEER, "one failed receive link poisons the whole bond");
    CHECK(flag_of(&b) == 1, "a failed bond does not apply a pending signal");
    CHECK(mcdma_fabric_write(b.p, WINDOW / 2, 0, 8) == MCDMA_FABRIC_PEER &&
          mcdma_fabric_signal(b.p, FLAG, 3) == MCDMA_FABRIC_PEER &&
          mcdma_fabric_flush(b.p, SECOND) == MCDMA_FABRIC_PEER,
          "later operations cannot silently use the surviving child");
    close_pair(&a, &b);
}

struct signal_job {
    struct mcdma_fabric_peer *p;
    int started, done, status;
    unsigned posted;
};

static void *post_many_signals(void *arg) {
    struct signal_job *j = arg;
    __atomic_store_n(&j->started, 1, __ATOMIC_RELEASE);
    for (unsigned i = 1; i <= 5000; ++i) {
        j->status = mcdma_fabric_signal(j->p, FLAG, i);
        if (j->status) break;
        __atomic_store_n(&j->posted, i, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&j->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void scenario_bond_credit(void) {
    struct rank a, b;
    open_bond_pair(&a, &b);
    const uint64_t bytes = 3ull << 20;
    for (uint64_t i = 0; i < bytes; ++i) a.mem[WINDOW / 2 + i] = pattern(0, 52, i);
    stub_tb_hold_completions("tb1", 1);
    CHECK(!mcdma_fabric_write(a.p, WINDOW / 2, 0, bytes), "the credit test's earlier write posts");
    struct signal_job job = { .p = a.p };
    pthread_t thread;
    CHECK(!pthread_create(&thread, NULL, post_many_signals, &job), "the signal-credit writer starts");
    for (int i = 0; i < 5000 && __atomic_load_n(&job.posted, __ATOMIC_ACQUIRE) < 4096 &&
                        !__atomic_load_n(&job.done, __ATOMIC_ACQUIRE); ++i) usleep(1000);
    CHECK(__atomic_load_n(&job.posted, __ATOMIC_ACQUIRE) == 4096, "the sender reaches the bounded signal credit");
    CHECK(!__atomic_load_n(&job.done, __ATOMIC_ACQUIRE) && flag_of(&b) == 0,
          "signal credit waits for placement instead of poisoning or publishing");
    stub_tb_hold_completions("tb1", 0);
    pthread_join(thread, NULL);
    CHECK(!job.status && __atomic_load_n(&job.posted, __ATOMIC_ACQUIRE) == 5000,
          "all signals post once receive placement releases credit");
    CHECK(!mcdma_fabric_wait(b.f, FLAG, 5000, 5 * SECOND) && !mcdma_fabric_flush(a.p, 5 * SECOND),
          "the last credited signal and both flushes finish");
    for (uint64_t i = 0; i < bytes; ++i) CHECK(b.mem[i] == pattern(0, 52, i), "credited signals follow every byte");
    close_pair(&a, &b);
}

/* links: 1 when every byte of the call must ride one physical link, 2 when it must be cut across both. */
static void small_lanes(struct rank *a, struct rank *b, uint64_t off, uint64_t len, uint64_t value, int joined,
                        int links) {
    struct mcdma_fabric_link_stats before[2], after[2];
    for (unsigned k = 0; k < 2; ++k) CHECK(!mcdma_fabric_link_stats(a->p, k, &before[k]), "small-write counters before");
    for (uint64_t i = 0; i < len; ++i) a->mem[off + i] = pattern(0, value, i);
    if (joined) CHECK(!mcdma_fabric_write_signal(a->p, off, 0, len, FLAG, value), "a bounded small joined write");
    else CHECK(!mcdma_fabric_write(a->p, off, 0, len) && !mcdma_fabric_signal(a->p, FLAG, value), "a bounded small write");
    CHECK(!mcdma_fabric_wait(b->f, FLAG, value, 5 * SECOND) && !mcdma_fabric_flush(a->p, 5 * SECOND),
          "a bounded small write reaches the peer and flushes");
    CHECK(!memcmp(a->mem + off, b->mem, (size_t)len), "every byte of a bounded small write lands");
    for (unsigned k = 0; k < 2; ++k) CHECK(!mcdma_fabric_link_stats(a->p, k, &after[k]), "small-write counters after");
    uint64_t n0 = after[0].posted_bytes - before[0].posted_bytes;
    uint64_t n1 = after[1].posted_bytes - before[1].posted_bytes;
    if (links == 1)
        CHECK((n0 == len && !n1) || (n1 == len && !n0), "all chunks of one small call use one physical link");
    else CHECK(n0 && n1 && n0 + n1 == len, "a write-and-signal of 16 KiB is cut across both links");
    CHECK(after[0].completed_bytes == after[0].posted_bytes && after[1].completed_bytes == after[1].posted_bytes,
          "bounded small writes leave no outstanding payload");
}

static void scenario_bond_small(void) {
    struct rank a, b;
    open_bond_pair(&a, &b);
    /* Parallel parts keep using both links when a shallow queue cuts each part into smaller payload messages. */
    small_lanes(&a, &b, WINDOW / 2 + MCDMA_FABRIC_WS_ROOM, 16384, 1, 0, 1);
    small_lanes(&a, &b, WINDOW / 2 + MCDMA_FABRIC_WS_ROOM, 16384, 2, 1, 2);
    small_lanes(&a, &b, WINDOW / 2 + MCDMA_FABRIC_WS_ROOM, 16383, 3, 1, 1);
    small_lanes(&a, &b, (12ull << 20) - 4096, 8192, 4, 0, 1);
    small_lanes(&a, &b, (12ull << 20) - 4096, 8192, 5, 1, 1);
    close_pair(&a, &b);
}

struct wait_job {
    struct mcdma_fabric *f;
    uint64_t offset, value;
    int started, done, status;
};

static void *wait_flag(void *arg) {
    struct wait_job *j = arg;
    __atomic_store_n(&j->started, 1, __ATOMIC_RELEASE);
    j->status = mcdma_fabric_wait(j->f, j->offset, j->value, 5 * SECOND);
    __atomic_store_n(&j->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void scenario_bond_wait(void) {
    struct rank a, bad, remote;
    open_bond_pair(&a, &bad);
    open_rank(&remote, 1, "tb4+tb5", 0);
    struct rank shared = a; /* a second peer on the same window and the same two local devices */
    shared.p = NULL;
    shared.name = remote.name = "healthy";
    meet(&shared, &remote);
    CHECK(!shared.status && !remote.status, "the independent healthy peer connects");
    stub_tb_fail("tb1");
    mcdma_fabric_disconnect(&bad.p);
    int status = MCDMA_FABRIC_OK;
    for (int i = 0; i < 1000 && status == MCDMA_FABRIC_OK; ++i) {
        status = mcdma_fabric_signal(a.p, FLAG, 1);
        usleep(1000);
    }
    CHECK(status == MCDMA_FABRIC_PEER, "the unrelated bond is confirmed down");
    struct wait_job job = { .f = a.f, .offset = FLAG + 8, .value = 77 };
    pthread_t thread;
    CHECK(!pthread_create(&thread, NULL, wait_flag, &job), "the healthy flag waiter starts");
    while (!__atomic_load_n(&job.started, __ATOMIC_ACQUIRE)) usleep(1000);
    usleep(30000);
    CHECK(!__atomic_load_n(&job.done, __ATOMIC_ACQUIRE), "one dead peer does not fail a healthy flag wait");
    unsigned char *src = remote.mem + WINDOW / 2 + MCDMA_FABRIC_WS_ROOM;
    for (size_t i = 0; i < 257; ++i) src[i] = pattern(1, 77, i);
    CHECK(!mcdma_fabric_write_signal(remote.p, WINDOW / 2 + MCDMA_FABRIC_WS_ROOM, 0, 257, FLAG + 8, 77),
          "the live peer supplies the waited-for flag");
    pthread_join(thread, NULL);
    CHECK(!job.status && !memcmp(a.mem, src, 257), "the healthy flag wait observes its own completed payload");
    CHECK(!mcdma_fabric_flush(remote.p, 5 * SECOND), "the healthy peer remains usable");
    close_pair(&a, &bad);
    mcdma_fabric_close(&remote.f);
    munmap(remote.mem, WINDOW);
}

static void scenario_bond_names(void) {
    const char *names[] = {"abcdefghijklmnopqrs", "abcdefghijklmnopqrst"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        struct rank a, b;
        open_rank(&a, 0, "tb0+tb2", 0);
        open_rank(&b, 1, "tb1+tb3", 0);
        a.name = b.name = names[i];
        meet(&a, &b);
        CHECK(!a.status && !b.status, "a maximum public name fits each internal lane name");
        CHECK(!mcdma_fabric_signal(a.p, FLAG, 1) && !mcdma_fabric_wait(b.f, FLAG, 1, 5 * SECOND) &&
              !mcdma_fabric_flush(a.p, 5 * SECOND), "a long-name bond transfers a signal");
        close_pair(&a, &b);
    }
}

/* A zero-length write_signal stores its flag like a signal; it needs no room before local_offset and fails nothing. */
static void zero_length(struct rank *a, struct rank *b, uint64_t value, int fused) {
    CHECK(mcdma_fabric_write_signal(a->p, 0, 0, 0, FLAG + 4, value) == MCDMA_FABRIC_BOUNDS,
          "a misaligned empty signal");
    CHECK(mcdma_fabric_write_signal(a->p, WINDOW + 1, 0, 0, FLAG, value) == MCDMA_FABRIC_BOUNDS,
          "an empty write still names a place in this window");
    CHECK(!mcdma_fabric_write_signal(a->p, 0, 0, 0, FLAG, value), "a zero-length write-and-signal posts");
    CHECK(!mcdma_fabric_wait(b->f, FLAG, value, 5 * SECOND) && flag_of(b) == value,
          "its flag lands as a signal's would");
    CHECK(mcdma_fabric_peer_status(a->p, NULL, 0) == MCDMA_FABRIC_OK, "the peer stays up");
    unsigned char *src = a->mem + WINDOW / 2 + MCDMA_FABRIC_WS_ROOM;
    for (size_t i = 0; i < 300; ++i) src[i] = pattern(0, value, i);
    if (fused) {
        CHECK(!mcdma_fabric_write_signal(a->p, WINDOW / 2 + MCDMA_FABRIC_WS_ROOM, 0, 300, FLAG, value + 1),
              "the next write-and-signal still works");
    } else {
        CHECK(mcdma_fabric_write_signal(a->p, WINDOW / 2 + MCDMA_FABRIC_WS_ROOM, 0, 300, FLAG, value + 1) ==
              MCDMA_FABRIC_UNSUPPORTED, "RoCE still has no joined write");
        CHECK(!mcdma_fabric_write(a->p, WINDOW / 2 + MCDMA_FABRIC_WS_ROOM, 0, 300) &&
              !mcdma_fabric_signal(a->p, FLAG, value + 1), "the next write and signal still work");
    }
    CHECK(!mcdma_fabric_wait(b->f, FLAG, value + 1, 5 * SECOND) && !memcmp(b->mem, src, 300), "and land in order");
    CHECK(!mcdma_fabric_flush(a->p, 5 * SECOND), "the peer flushes");
}

static void scenario_zero(void) {
    struct rank a, b;
    open_rank(&a, 0, "tb0", 0);
    open_rank(&b, 1, "tb1", MCDMA_FABRIC_PROGRESS_THREAD);
    meet(&a, &b);
    CHECK(!a.status && !b.status, "both ranks connect");
    zero_length(&a, &b, 10, 1);
    close_pair(&a, &b);
    open_rank(&a, 0, "roce0", 0);
    open_rank(&b, 1, "roce1", 0);
    meet(&a, &b);
    CHECK(!a.status && !b.status, "both RoCE ranks connect");
    zero_length(&a, &b, 20, 0);
    close_pair(&a, &b);
    open_bond_pair(&a, &b);
    zero_length(&a, &b, 30, 1);
    close_pair(&a, &b);
}

/* Wait until `p` reports itself down and return its reason, or fail after `seconds`. */
static void wait_down(struct mcdma_fabric_peer *p, double seconds, char *why, size_t n, const char *what) {
    uint64_t deadline = (uint64_t)(seconds * SECOND), waited = 0;
    while (mcdma_fabric_peer_status(p, why, n) == MCDMA_FABRIC_OK) {
        CHECK(waited < deadline, what);
        usleep(1000);
        waited += 1000000;
    }
    CHECK(mcdma_fabric_peer_status(p, why, n) == MCDMA_FABRIC_PEER && why[0], what);
}

struct flush_job {
    struct mcdma_fabric_peer *p;
    int status;
};

static void *flush_peer(void *arg) {
    struct flush_job *j = arg;
    j->status = mcdma_fabric_flush(j->p, 8 * SECOND);
    return NULL;
}

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * SECOND + (uint64_t)t.tv_nsec;
}

/* b's link fails while a waits in a flush that b will never answer: a hears why and gives up long before its
 * timeout, and both ends report the same cause. */
static void scenario_down(void) {
    struct rank a, b;
    char why[128];
    open_rank(&a, 0, "tb0", 0);
    open_rank(&b, 1, "tb1", MCDMA_FABRIC_PROGRESS_THREAD);
    meet(&a, &b);
    CHECK(!a.status && !b.status, "both ranks connect");
    CHECK(!mcdma_fabric_signal(a.p, FLAG, 1) && !mcdma_fabric_wait(b.f, FLAG, 1, 5 * SECOND), "the link starts live");
    CHECK(!mcdma_fabric_flush(a.p, 5 * SECOND), "the live link flushes");
    stub_tb_hold("tb1", 1);
    CHECK(!mcdma_fabric_signal(a.p, FLAG, 2), "a signal waits for a receiver that has stopped");
    struct flush_job job = {.p = a.p};
    pthread_t thread;
    uint64_t began = now_ns();
    CHECK(!pthread_create(&thread, NULL, flush_peer, &job), "the flushing thread starts");
    usleep(50000);
    stub_tb_fail("tb1");
    pthread_join(thread, NULL);
    CHECK(job.status == MCDMA_FABRIC_PEER && now_ns() - began < 3 * SECOND,
          "a flush waiting on a failed end returns PEER at once, not after its timeout");
    wait_down(b.p, 2, why, sizeof(why), "the failed end reports itself down");
    CHECK(strstr(why, "unexpected completion") != NULL, "the failed end keeps its link's reason");
    wait_down(a.p, 2, why, sizeof(why), "the other end hears the failure");
    CHECK(strstr(why, "other end failed") && strstr(why, "unexpected completion"),
          "and reports the failed end's reason");
    CHECK(mcdma_fabric_signal(a.p, FLAG, 3) == MCDMA_FABRIC_PEER, "later calls on it return PEER");
    CHECK(mcdma_fabric_wait(a.f, FLAG, 99, 5 * SECOND) == MCDMA_FABRIC_PEER,
          "a single link's wait reports PEER once its only peer is down");
    stub_tb_hold("tb1", 0);
    close_pair(&a, &b);
}

static void scenario_bond_down(void) {
    struct rank a, b;
    char why[128];
    open_bond_pair(&a, &b);
    CHECK(!mcdma_fabric_signal(a.p, FLAG, 1) && !mcdma_fabric_wait(b.f, FLAG, 1, 5 * SECOND), "the bond starts live");
    stub_tb_fail("tb3");
    wait_down(b.p, 2, why, sizeof(why), "the end with the failed link reports the bond down");
    CHECK(strstr(why, "tb3") && strstr(why, "unexpected completion"), "naming the link and its reason");
    wait_down(a.p, 2, why, sizeof(why), "the other end of the bond hears it");
    CHECK(strstr(why, "other end failed") && strstr(why, "tb3"), "with the failed link's reason");
    CHECK(mcdma_fabric_write_signal(a.p, 0, 0, 0, FLAG, 2) == MCDMA_FABRIC_PEER &&
          mcdma_fabric_flush(a.p, SECOND) == MCDMA_FABRIC_PEER, "neither end keeps using the surviving link");
    close_pair(&a, &b);
}

/* Link 0's share of a planned write: what it carries of `len` bytes, the head's link carrying the first bytes. */
static uint64_t share0(unsigned head, uint64_t first, uint64_t len) { return head == 0 ? first : len - first; }

static void scenario_bond_plan(void) {
    unsigned char *mem = window();
    struct region win;
    memset(&win, 0, sizeof(win));
    win.base = mem, win.length = WINDOW, win.seg = 12ull << 20;
    const uint64_t max[2] = {4ull << 20, 4ull << 20}, idle[2] = {0, 0}, even[2] = {8000, 8000};
    const uint64_t off = WINDOW / 2 + MCDMA_FABRIC_WS_ROOM;
    unsigned turn = 0, head = 9, prev;
    uint64_t first = 0;
    /* under 16 KiB: whole, and idle links of one speed take turns */
    CHECK(!mcdma_fabric_test_plan(&win, idle, even, max, off, 10240, &turn, &head, &first) && first == 10240,
          "a 10 KB write-and-signal goes whole");
    prev = head;
    CHECK(!mcdma_fabric_test_plan(&win, idle, even, max, off, 10240, &turn, &head, &first) && head != prev,
          "the next one takes the other idle link");
    const uint64_t sizes[] = {16384, 40960, 163840, 1ull << 20, 3ull << 20};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        uint64_t len = sizes[i];
        CHECK(!mcdma_fabric_test_plan(&win, idle, even, max, off, len, &turn, &head, &first), "a cut is planned");
        CHECK(first >= 2048 && len - first >= 2048 && first + 4096 >= len / 2 && first <= len / 2 + 4096,
              "equal links each carry about half");
        CHECK((first + MCDMA_FABRIC_WS_ROOM) % 4096 == 0, "the head's message is whole packets");
        prev = head;
        CHECK(!mcdma_fabric_test_plan(&win, idle, even, max, off, len, &turn, &head, &first) && head != prev,
              "the links take turns carrying the head");
    }
    const uint64_t uneven[2] = {9000, 4500}, len = 1ull << 20;
    CHECK(!mcdma_fabric_test_plan(&win, idle, uneven, max, off, len, &turn, &head, &first), "an uneven cut");
    uint64_t fast = share0(head, first, len);
    CHECK(fast + 4096 >= len * 2 / 3 && fast <= len * 2 / 3 + 4096, "a link twice as fast carries two thirds");
    const uint64_t queued[2] = {256ull << 10, 0};
    CHECK(!mcdma_fabric_test_plan(&win, queued, even, max, off, len, &turn, &head, &first), "a cut behind a queue");
    fast = share0(head, first, len);
    CHECK(fast + 4096 >= (len - (256ull << 10)) / 2 && fast <= (len - (256ull << 10)) / 2 + 4096,
          "the queued link carries less, so both finish together");
    const uint64_t buried[2] = {4ull << 20, 0};
    CHECK(!mcdma_fabric_test_plan(&win, buried, even, max, off, len, &turn, &head, &first) && head == 1 && first == len,
          "a link with far more queued than the write gets none of it");
    /* the cut lands on a registration boundary inside the source, so each message stays in one */
    uint64_t at = (24ull << 20) - 100000;
    CHECK(!mcdma_fabric_test_plan(&win, idle, even, max, at, 300000, &turn, &head, &first) && first == 100000,
          "a source across a registration is cut at the boundary");
    CHECK(mcdma_fabric_test_plan(&win, idle, even, max, (24ull << 20) + 32, 300000, &turn, &head, &first) == 1,
          "a head whose room lies in the previous registration cannot be joined");
    CHECK(mcdma_fabric_test_plan(&win, idle, even, max, off, 9ull << 20, &turn, &head, &first) == 1,
          "more than one message a link can carry goes as writes");
    /* the bytes where the tail would start look like a head: the tail starts one byte later */
    unsigned saved = turn;
    CHECK(!mcdma_fabric_test_plan(&win, idle, even, max, off, 40960, &turn, &head, &first), "a cut");
    uint64_t natural = first;
    uint32_t magic = 0x4254434du;
    memcpy(mem + off + natural, &magic, 4);
    turn = saved;
    CHECK(!mcdma_fabric_test_plan(&win, idle, even, max, off, 40960, &turn, &head, &first) && first == natural + 1 &&
          tb_tail_clean(&win, off + first), "a tail never starts with a head's magic");
    munmap(mem, WINDOW);
}

static void fill(struct rank *r, uint64_t off, uint64_t len, uint64_t step) {
    for (uint64_t i = 0; i < len; ++i) r->mem[off + i] = pattern(r->me, step, i);
}

static int landed(const struct rank *r, uint64_t off, uint64_t len, int from, uint64_t step) {
    for (uint64_t i = 0; i < len; ++i)
        if (r->mem[off + i] != pattern(from, step, i)) return 0;
    return 1;
}

static void spin_flag(const struct rank *r, uint64_t off, uint64_t value, const char *what) {
    uint64_t spins = 0;
    while (__atomic_load_n((uint64_t *)(void *)(r->mem + off), __ATOMIC_ACQUIRE) < value) {
        if (!(++spins % 4096)) {
            CHECK(mcdma_fabric_peer_status(r->p, NULL, 0) == MCDMA_FABRIC_OK, what);
            CHECK(spins < 4096ull * 200000, what);
        }
    }
}

extern int mcdma_fabric_test_no_ask;

static void scenario_bond_stripe(void) {
    struct rank a, b;
    /* with "asleep" the links' threads never take a handed tail, so every one goes by the sender's fallback */
    if (getenv("BOND_ASLEEP")) mcdma_fabric_test_no_ask = 1;
    open_bond_pair(&a, &b);
    /* a granted queue of a few packets takes smaller messages, so larger writes go as several */
    int whole = !getenv("STUB_TB_DEPTH");
    const uint64_t src = WINDOW / 2 + MCDMA_FABRIC_WS_ROOM;
    const uint64_t sizes[] = {16384, 40960, 163840, 1ull << 20, 3ull << 20};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        uint64_t len = sizes[i], sends[2] = {stub_tb_sends("tb0"), stub_tb_sends("tb2")};
        struct mcdma_fabric_link_stats before[2], after[2];
        for (unsigned k = 0; k < 2; ++k) CHECK(!mcdma_fabric_link_stats(a.p, k, &before[k]), "counters before");
        fill(&a, src, len, i + 1);
        CHECK(!mcdma_fabric_write_signal(a.p, src, 0, len, FLAG, i + 1), "a bonded write-and-signal posts");
        CHECK(!whole || (stub_tb_sends("tb0") >= sends[0] + 2 && stub_tb_sends("tb2") >= sends[1] + 2),
              "each link posts a control header and payload");
        spin_flag(&b, FLAG, i + 1, "the flag arrives");
        CHECK(landed(&b, 0, len, 0, i + 1), "every byte precedes the flag");
        for (unsigned k = 0; k < 2; ++k) CHECK(!mcdma_fabric_link_stats(a.p, k, &after[k]), "counters after");
        uint64_t n0 = after[0].posted_bytes - before[0].posted_bytes;
        uint64_t n1 = after[1].posted_bytes - before[1].posted_bytes;
        CHECK(n0 && n1 && n0 + n1 == len, "both links carry a share and nothing else");
    }
    /* the flag waits for the part on a link whose receiver is held, whichever part that is (writes too large to
     * cut would instead fence the overlap and wait for the held link inside the call) */
    for (int held = 0; whole && held < 2; ++held) {
        const char *rx = held ? "tb3" : "tb1";
        uint64_t value = 100 + (uint64_t)held;
        fill(&a, src, 163840, value);
        stub_tb_hold_completions(rx, 1);
        CHECK(!mcdma_fabric_write_signal(a.p, src, 0, 163840, FLAG, value), "a write-and-signal beside a held link");
        usleep(30000);
        CHECK(flag_of(&b) < value, "its flag does not publish before the held link's part is placed");
        stub_tb_hold_completions(rx, 0);
        spin_flag(&b, FLAG, value, "the flag arrives once the held part lands");
        CHECK(landed(&b, 0, 163840, 0, value), "with every byte");
    }
    /* across a registration boundary: still one message a link, cut at the boundary */
    uint64_t at = (24ull << 20) - 100000, sends[2] = {stub_tb_sends("tb0"), stub_tb_sends("tb2")};
    fill(&a, at, 300000, 200);
    CHECK(!mcdma_fabric_write_signal(a.p, at, 4096, 300000, FLAG, 200), "a write-and-signal across a registration");
    CHECK(!whole || (stub_tb_sends("tb0") >= sends[0] + 2 && stub_tb_sends("tb2") >= sends[1] + 2),
          "both links post a payload message");
    spin_flag(&b, FLAG, 200, "its flag arrives");
    CHECK(landed(&b, 4096, 300000, 0, 200), "and its bytes");
    /* larger than one message a link can carry: writes, then the signal, still in order */
    fill(&a, WINDOW / 2 - (9ull << 20), 9ull << 20, 201);
    CHECK(!mcdma_fabric_write_signal(a.p, WINDOW / 2 - (9ull << 20), 0, 9ull << 20, FLAG, 201), "9 MiB posts");
    spin_flag(&b, FLAG, 201, "its flag arrives");
    CHECK(landed(&b, 0, 9ull << 20, 0, 201), "after every byte");
    CHECK(!mcdma_fabric_flush(a.p, 5 * SECOND), "the bond flushes");
    close_pair(&a, &b);
}

/* A tensor-parallel exchange: each rank write-and-signals its rows from a parity source slot to the peer's slot,
 * then spins on its own flag word for the peer's; a second thread signals another word on the same peer. */
#define PP_SRC (WINDOW / 2)
#define PP_SLOT (2ull << 20)
#define PP_WORD (FLAG + 64)

struct exchange_job {
    struct rank *me;
    uint64_t first, rounds, rows; /* rows 0: 1 to 16 by round */
    int signals_done;
};

static void *exchange_rounds(void *arg) {
    struct exchange_job *j = arg;
    struct rank *r = j->me;
    for (uint64_t x = j->first; x < j->first + j->rounds; ++x) {
        uint64_t rows = j->rows ? j->rows : 1 + (x * 7 + (uint64_t)r->me) % 16, len = rows * 10240, parity = x % 2;
        uint64_t src = PP_SRC + parity * (PP_SLOT + 16384) + 16384, dst = parity * PP_SLOT;
        fill(r, src, len, x);
        CHECK(!mcdma_fabric_write_signal(r->p, src, dst, len, FLAG, x), "an exchange posts");
        spin_flag(r, FLAG, x, "the peer's exchange arrives");
        uint64_t peer_rows = j->rows ? j->rows : 1 + (x * 7 + (uint64_t)!r->me) % 16;
        CHECK(landed(r, dst, peer_rows * 10240, !r->me, x), "every byte of an exchange lands before its flag");
    }
    return NULL;
}

static void *signal_rounds(void *arg) {
    struct exchange_job *j = arg;
    uint64_t last = 0;
    /* values rise across calls too: each call's start is above every value an earlier call could reach */
    for (uint64_t v = 1; !__atomic_load_n(&j->signals_done, __ATOMIC_ACQUIRE); ++v) {
        CHECK(!mcdma_fabric_signal(j->me->p, PP_WORD, j->first << 32 | v), "a signal beside the exchanges posts");
        uint64_t seen = __atomic_load_n((uint64_t *)(void *)(j->me->mem + PP_WORD), __ATOMIC_ACQUIRE);
        CHECK(seen >= last, "the peer's signalled word never goes backwards");
        last = seen;
        if (v % 64 == 0) usleep(100);
    }
    return NULL;
}

static void exchanges(struct rank *a, struct rank *b, uint64_t first, uint64_t rounds, uint64_t rows) {
    struct exchange_job ja = {a, first, rounds, rows, 0}, jb = {b, first, rounds, rows, 0};
    pthread_t tb, sa, sb;
    CHECK(!pthread_create(&tb, NULL, exchange_rounds, &jb), "the peer's exchange thread");
    CHECK(!pthread_create(&sa, NULL, signal_rounds, &ja), "a signalling thread");
    CHECK(!pthread_create(&sb, NULL, signal_rounds, &jb), "the peer's signalling thread");
    exchange_rounds(&ja);
    pthread_join(tb, NULL);
    __atomic_store_n(&ja.signals_done, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&jb.signals_done, 1, __ATOMIC_RELEASE);
    pthread_join(sa, NULL);
    pthread_join(sb, NULL);
}

static uint64_t link_bytes(struct rank *r, unsigned k) {
    struct mcdma_fabric_link_stats s;
    CHECK(!mcdma_fabric_link_stats(r->p, k, &s), "link counters");
    return s.posted_bytes;
}

static void scenario_bond_pingpong(void) {
    struct rank a, b;
    open_bond_pair(&a, &b);
    /* one-row exchanges go whole; links of one speed that sit idle between them take turns */
    uint64_t a0 = link_bytes(&a, 0), a1 = link_bytes(&a, 1);
    exchanges(&a, &b, 1, 200, 1);
    a0 = link_bytes(&a, 0) - a0, a1 = link_bytes(&a, 1) - a1;
    CHECK(a0 + a1 == 200 * 10240 && a0 >= 40 * 10240 && a1 >= 40 * 10240, "10 KB exchanges use both links");
    /* 1 to 16 rows: two rows and up are cut across both links */
    a0 = link_bytes(&a, 0), a1 = link_bytes(&a, 1);
    uint64_t b0 = link_bytes(&b, 0), b1 = link_bytes(&b, 1);
    exchanges(&a, &b, 201, 400, 0);
    a0 = link_bytes(&a, 0) - a0, a1 = link_bytes(&a, 1) - a1, b0 = link_bytes(&b, 0) - b0, b1 = link_bytes(&b, 1) - b1;
    /* measured rates can differ up to 3:1, and whole messages favour the faster link */
    CHECK(a0 * 6 > a0 + a1 && a1 * 6 > a0 + a1 && b0 * 6 > b0 + b1 && b1 * 6 > b0 + b1,
          "both links carry at least a sixth of each rank's exchanges");
    CHECK(!mcdma_fabric_flush(a.p, 5 * SECOND) && !mcdma_fabric_flush(b.p, 5 * SECOND), "both ranks flush");
    close_pair(&a, &b);
}

/* Each step overwrites part of the last one through a different cut, while one link's receiver is held. */
static void later_wins(struct rank *a, struct rank *b, const char *held, uint64_t step) {
    const uint64_t src = WINDOW / 2 + MCDMA_FABRIC_WS_ROOM, other = src + (4ull << 20), dst = 1ull << 20;
    const uint64_t big = 96ull << 10, small = 40ull << 10, inner = 8192;
    fill(a, src, big, step);
    fill(a, other, small, step + 1);
    stub_tb_hold_completions(held, 1);
    CHECK(!mcdma_fabric_write_signal(a->p, src, dst, big, FLAG, step), "the earlier write-and-signal posts");
    CHECK(!mcdma_fabric_write_signal(a->p, other, dst + inner, small, FLAG, step + 1), "the later one posts");
    usleep(30000);
    CHECK(flag_of(b) < step, "neither publishes while a link is held");
    stub_tb_hold_completions(held, 0);
    spin_flag(b, FLAG, step + 1, "both flags arrive");
    CHECK(landed(b, dst, inner, 0, step), "the earlier write keeps the bytes the later one does not cover");
    CHECK(landed(b, dst + inner, small, 0, step + 1), "the later write wins where they overlap");
    for (uint64_t i = inner + small; i < big; ++i)
        CHECK(b->mem[dst + i] == pattern(0, step, i), "and the earlier write's end survives");
}

/* A signal stores a word that a later write-and-signal's bytes cover: the later bytes win. */
static void later_than_signal(struct rank *a, struct rank *b, const char *held, uint64_t step) {
    const uint64_t src = WINDOW / 2 + MCDMA_FABRIC_WS_ROOM, dst = 2ull << 20, word = dst + 1024, len = 32768;
    fill(a, src, len, step);
    fill(a, src + (4ull << 20), 65536, step + 1);
    stub_tb_hold_completions(held, 1);
    CHECK(!mcdma_fabric_write(a->p, src + (4ull << 20), 3ull << 20, 65536), "an earlier write the signal waits for");
    CHECK(!mcdma_fabric_signal(a->p, word, 0x5151515151515151ull), "a signal into the later write's range");
    CHECK(!mcdma_fabric_write_signal(a->p, src, dst, len, FLAG, step), "the later write-and-signal over the word");
    usleep(30000);
    stub_tb_hold_completions(held, 0);
    spin_flag(b, FLAG, step, "the later flag arrives");
    CHECK(landed(b, dst, len, 0, step), "the later bytes win over the earlier signal's word");
}

static void scenario_bond_later(void) {
    struct rank a, b;
    open_bond_pair(&a, &b);
    later_wins(&a, &b, "tb1", 10);
    later_wins(&a, &b, "tb3", 20);
    later_wins(&a, &b, "tb1", 30);
    later_wins(&a, &b, "tb3", 40);
    later_than_signal(&a, &b, "tb1", 50);
    later_than_signal(&a, &b, "tb3", 60);
    later_than_signal(&a, &b, "tb1", 70);
    later_than_signal(&a, &b, "tb3", 80);
    /* a plain write over bytes both links carried still lands after them */
    const uint64_t src = WINDOW / 2 + MCDMA_FABRIC_WS_ROOM;
    fill(&a, src, 40960, 90);
    CHECK(!mcdma_fabric_write_signal(a.p, src, 0, 40960, FLAG, 90), "a cut write-and-signal");
    fill(&a, src + (4ull << 20), 20000, 91);
    CHECK(!mcdma_fabric_write(a.p, src + (4ull << 20), 10000, 20000) && !mcdma_fabric_signal(a.p, FLAG, 91),
          "a plain write over its middle");
    spin_flag(&b, FLAG, 91, "the plain write's signal arrives");
    CHECK(landed(&b, 0, 10000, 0, 90) && landed(&b, 10000, 20000, 0, 91), "the plain write wins where it covers");
    for (uint64_t i = 30000; i < 40960; ++i) CHECK(b.mem[i] == pattern(0, 90, i), "the rest stays");
    close_pair(&a, &b);
}

static void scenario_bond_magic(void) {
    struct rank a, b;
    open_bond_pair(&a, &b);
    const uint64_t src = WINDOW / 2 + MCDMA_FABRIC_WS_ROOM, len = 163840;
    const uint32_t magic = 0x4254434du;
    for (uint64_t i = 0; i < len; i += 4) memcpy(a.mem + src + i, &magic, 4);
    for (unsigned round = 1; round <= 4; ++round) {
        uint64_t sends[2] = {stub_tb_sends("tb0"), stub_tb_sends("tb2")};
        CHECK(!mcdma_fabric_write_signal(a.p, src, 0, len, FLAG, round), "a write of nothing but magic posts");
        CHECK(stub_tb_sends("tb0") >= sends[0] + 2 && stub_tb_sends("tb2") >= sends[1] + 2, "framed payloads on both links");
        spin_flag(&b, FLAG, round, "its flag arrives");
        CHECK(!memcmp(b.mem, a.mem + src, len), "every byte lands");
    }
    close_pair(&a, &b);
}

/* tb2, rank a's second link, sends only on every eighth delivery pass: once measured, it carries well under half. */
static void scenario_bond_uneven(void) {
    struct rank a, b;
    open_bond_pair(&a, &b);
    stub_tb_pace("tb2", 8);
    const uint64_t src = WINDOW / 2 + MCDMA_FABRIC_WS_ROOM, len = 256ull << 10, ack = FLAG + 128;
    uint64_t n0 = 0, n1 = 0;
    for (uint64_t x = 1; x <= 300; ++x) {
        uint64_t b0 = link_bytes(&a, 0), b1 = link_bytes(&a, 1);
        fill(&a, src, len, x);
        CHECK(!mcdma_fabric_write_signal(a.p, src, 0, len, FLAG, x), "a write-and-signal over an uneven bond");
        spin_flag(&b, FLAG, x, "it arrives");
        CHECK(landed(&b, 0, len, 0, x), "with every byte");
        CHECK(!mcdma_fabric_signal(b.p, ack, x), "the answer posts");
        spin_flag(&a, ack, x, "the answer arrives");
        if (x > 100) n0 += link_bytes(&a, 0) - b0, n1 += link_bytes(&a, 1) - b1;
    }
    printf("test_fabric bond-uneven: the slower link carried %.3f of the last 200\n", (double)n1 / (double)(n0 + n1));
    CHECK(n0 + n1 == 200 * len && n1 * 10 < (n0 + n1) * 4 && n1 * 20 > n0 + n1,
          "the slower link carries a smaller share, but still carries some");
    stub_tb_pace("tb2", 1);
    close_pair(&a, &b);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    g_mode = argv[1];
    setvbuf(stderr, NULL, _IOLBF, 0);
    struct ifaddrs *all = NULL;
    if (!getifaddrs(&all)) {
        for (struct ifaddrs *a = all; a && !g_lo[0]; a = a->ifa_next)
            if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET6 && (a->ifa_flags & IFF_LOOPBACK) &&
                IN6_IS_ADDR_LINKLOCAL(&((struct sockaddr_in6 *)(void *)a->ifa_addr)->sin6_addr))
                snprintf(g_lo, sizeof(g_lo), "%s", a->ifa_name);
        freeifaddrs(all);
    }
    if (!g_lo[0] && !getifaddrs(&all)) {
        for (struct ifaddrs *a = all; a && !g_lo[0]; a = a->ifa_next)
            if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET && (a->ifa_flags & IFF_LOOPBACK))
                snprintf(g_lo, sizeof(g_lo), "%s/127.0.0.1", a->ifa_name);
        freeifaddrs(all);
    }
    if (!g_lo[0]) return 77;
    snprintf(g_bond_via, sizeof(g_bond_via), "%s+%s", g_lo, g_lo);
    if (!strcmp(g_mode, "tb")) scenario_tb();
    else if (!strcmp(g_mode, "roce")) scenario_roce();
    else if (!strcmp(g_mode, "dmabuf")) scenario_dmabuf();
    else if (!strcmp(g_mode, "mismatch")) scenario_mismatch();
    else if (!strcmp(g_mode, "args")) scenario_args();
    else if (!strcmp(g_mode, "bond")) scenario_bond();
    else if (!strcmp(g_mode, "bond-args")) scenario_bond_args();
    else if (!strcmp(g_mode, "bond-mismatch")) scenario_bond_mismatch();
    else if (!strcmp(g_mode, "bond-order")) scenario_bond_order();
    else if (!strcmp(g_mode, "bond-schedule")) scenario_bond_schedule();
    else if (!strcmp(g_mode, "bond-overlap")) scenario_bond_overlap();
    else if (!strcmp(g_mode, "bond-fail")) scenario_bond_fail();
    else if (!strcmp(g_mode, "bond-credit")) scenario_bond_credit();
    else if (!strcmp(g_mode, "bond-small")) scenario_bond_small();
    else if (!strcmp(g_mode, "bond-wait")) scenario_bond_wait();
    else if (!strcmp(g_mode, "bond-names")) scenario_bond_names();
    else if (!strcmp(g_mode, "zero")) scenario_zero();
    else if (!strcmp(g_mode, "down")) scenario_down();
    else if (!strcmp(g_mode, "bond-down")) scenario_bond_down();
    else if (!strcmp(g_mode, "bond-plan")) scenario_bond_plan();
    else if (!strcmp(g_mode, "bond-stripe")) scenario_bond_stripe();
    else if (!strcmp(g_mode, "bond-pingpong")) scenario_bond_pingpong();
    else if (!strcmp(g_mode, "bond-later")) scenario_bond_later();
    else if (!strcmp(g_mode, "bond-magic")) scenario_bond_magic();
    else if (!strcmp(g_mode, "bond-uneven")) scenario_bond_uneven();
    else return 2;
    printf("test_fabric %s: ok\n", g_mode);
    return 0;
}

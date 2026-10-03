/* Offline test of libmcdma-fabric: two ranks in one process over the stub's devices, meeting through the exchange on
 * the loopback's link-local address. One scenario per run:
 *   tb        Thunderbolt: double-buffered collective steps both ways, a progress thread on one side, goodbyes
 *   roce      RoCE with every key checked: the same steps, READ, and RoCE's signal staging
 *   dmabuf    a DMA-BUF window registers through ibv_reg_dmabuf_mr with no fallback; Thunderbolt refuses one
 *   mismatch  a Thunderbolt rank and a RoCE rank refuse each other
 *   args      bad windows, flags, names and interfaces are refused
 * Exit 0 means pass, 77 that loopback has no link-local address. */
#include "../rpc/mcdma_fabric.h"

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
#include <unistd.h>

#define WINDOW (32ull << 20)
#define SLOT (4ull << 20)           /* step parity p lands at p * SLOT; sources live in the upper half */
#define FLAG (WINDOW - 4096)
#define STEPS 80
#define SECOND 1000000000ull

struct rank {
    const char *device;
    uint32_t flags;
    unsigned char *mem;
    struct mcdma_fabric *f;
    struct mcdma_fabric_peer *p;
    int me, port, peer_port, status;
};

static const char *g_mode;
static char g_lo[16];

static void fail(const char *what) {
    fprintf(stderr, "test_fabric %s: %s\n", g_mode, what);
    _exit(1);
}

#define CHECK(c, what)        \
    do {                      \
        if (!(c)) fail(what); \
    } while (0)

static int free_port(void) {
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    struct sockaddr_in6 a;
    socklen_t len = sizeof(a);
    memset(&a, 0, sizeof(a));
    a.sin6_family = AF_INET6;
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) || getsockname(fd, (struct sockaddr *)&a, &len)) return -1;
    close(fd);
    return ntohs(a.sin6_port);
}

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
    r->status = mcdma_fabric_connect(r->f, g_lo, r->port, r->peer_port, "pair", 5 * SECOND, &r->p);
    return NULL;
}

static void open_rank(struct rank *r, int me, const char *device, uint32_t flags) {
    r->me = me, r->device = device, r->flags = flags, r->mem = window();
    CHECK(!mcdma_fabric_open(device, 0, 4096, r->mem, WINDOW, -1, 0, flags, &r->f), "a window registers");
}

/* Both ranks call connect at once, as two machines would. */
static void meet(struct rank *a, struct rank *b) {
    a->port = b->peer_port = free_port();
    b->port = a->peer_port = free_port();
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
    if (!g_lo[0]) return 77;
    if (!strcmp(g_mode, "tb")) scenario_tb();
    else if (!strcmp(g_mode, "roce")) scenario_roce();
    else if (!strcmp(g_mode, "dmabuf")) scenario_dmabuf();
    else if (!strcmp(g_mode, "mismatch")) scenario_mismatch();
    else if (!strcmp(g_mode, "args")) scenario_args();
    else return 2;
    printf("test_fabric %s: ok\n", g_mode);
    return 0;
}

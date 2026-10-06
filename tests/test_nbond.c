/* CPU-only N-link qualification with the strict stub verbs; no real device is opened. */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#else
#define _DEFAULT_SOURCE 1
#endif
#include "../rpc/mcdma_fabric.h"
#include "../rpc/link.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define WINDOW (16u << 20)
#define FLAG (WINDOW - 16384)
#define NS 1000000000ull
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "N-bond failed at line %d\n", __LINE__); exit(1); } } while (0)

void stub_tb_hold_completions(const char *, int);
void stub_tb_fail(const char *);

struct rank { struct mcdma_fabric *f; struct mcdma_fabric_peer *p; unsigned char *w; unsigned n; uint16_t ports[8], peers[8]; int status; };

static void *connect_peer(void *arg) {
    struct rank *r = arg;
    r->status = mcdma_fabric_connect_links(r->f, "lo0/127.0.0.1", r->ports, r->peers, r->n, "nbond", 5 * NS, &r->p);
    return NULL;
}

static unsigned ports(unsigned n) {
    for (unsigned p = 24000 + (unsigned)getpid() % 10000; p < 62000; p += 53) {
        int fds[16]; unsigned k;
        for (k = 0; k < 2 * n; ++k) {
            fds[k] = socket(AF_INET, SOCK_DGRAM, 0);
            struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)(p + 3 * k))};
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (fds[k] < 0 || bind(fds[k], (struct sockaddr *)&a, sizeof(a))) break;
        }
        if (k < 2 * n && fds[k] >= 0) close(fds[k]);
        for (unsigned i = 0; i < k; ++i) close(fds[i]);
        if (k == 2 * n) return p;
    }
    return 0;
}

static void fill(unsigned char *p, size_t n, unsigned tag) {
    for (size_t i = 0; i < n; ++i) p[i] = (unsigned char)(i * 37 + tag * 11);
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    unsigned n = (unsigned)atoi(argv[1]); CHECK(n >= 2 && n <= 8);
    unsigned char *probe = mmap(NULL, WINDOW, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(probe != MAP_FAILED);
    struct mcdma_fabric *bad = NULL;
    CHECK(!setenv("STUB_GID_SCAN", "1", 1));
    struct ep scan;
    CHECK(!ep_open(&scan, "tb0", -1, 4096));
    CHECK(scan.gid_index == 2 && scan.gid.raw[0] == 0xfe);
    ep_close(&scan); CHECK(!unsetenv("STUB_GID_SCAN"));
    CHECK(!setenv("STUB_ZERO_GIDS", "1", 1));
    CHECK(mcdma_fabric_open("tb0+tb2+tb4", -1, 4096, probe, WINDOW, -1, 0, 0, &bad) == MCDMA_FABRIC_DEVICE && !bad);
    CHECK(!unsetenv("STUB_ZERO_GIDS"));
    CHECK(!setenv("STUB_PORT_DOWN", "tb2", 1));
    CHECK(mcdma_fabric_open("tb0+tb2+tb4", -1, 4096, probe, WINDOW, -1, 0, 0, &bad) == MCDMA_FABRIC_DEVICE && !bad);
    CHECK(!unsetenv("STUB_PORT_DOWN"));
    CHECK(mcdma_fabric_open("tb0+not-a-device+tb4", -1, 4096, probe, WINDOW, -1, 0, 0, &bad) == MCDMA_FABRIC_DEVICE && !bad);
    CHECK(mcdma_fabric_open("tb0+tb1+tb2+tb3+tb4+tb5+tb6+tb7+tb8", -1, 4096, probe, WINDOW, -1, 0, 0, &bad) == MCDMA_FABRIC_INVALID && !bad);
    munmap(probe, WINDOW);
    struct rank r[2] = {{0}};
    unsigned base = ports(n); CHECK(base);
    for (unsigned rank = 0; rank < 2; ++rank) {
        r[rank].n = n;
        r[rank].w = mmap(NULL, WINDOW, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        CHECK(r[rank].w != MAP_FAILED);
        char devices[128] = "";
        for (unsigned k = 0; k < n; ++k) {
            char one[12]; snprintf(one, sizeof(one), "%stb%u", k ? "+" : "", 2 * k + rank);
            strcat(devices, one);
            r[rank].ports[k] = (uint16_t)(base + 3 * (rank * n + k));
            r[rank].peers[k] = (uint16_t)(base + 3 * ((1 - rank) * n + k));
        }
        CHECK(!mcdma_fabric_open(devices, -1, 4096, r[rank].w, WINDOW, -1, 0, 0, &r[rank].f));
    }
    pthread_t th; CHECK(!pthread_create(&th, NULL, connect_peer, &r[1]));
    connect_peer(&r[0]); pthread_join(th, NULL);
    CHECK(!r[0].status && !r[1].status);
    CHECK(mcdma_fabric_link_count(r[0].p) == n);
    const size_t sizes[] = {64, 16384, 32769, 40960, 163840, 1 << 20};
    unsigned tag = 0;
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        size_t len = sizes[i], src = WINDOW / 2 + 64;
        struct mcdma_fabric_link_stats before[8], after[8];
        for (unsigned k = 0; k < n; ++k) CHECK(!mcdma_fabric_link_stats(r[0].p, k, &before[k]));
        fill(r[0].w + src, len, ++tag);
        CHECK(!mcdma_fabric_write_signal(r[0].p, src, 0, len, FLAG, tag));
        CHECK(!mcdma_fabric_wait(r[1].f, FLAG, tag, 5 * NS));
        CHECK(!memcmp(r[0].w + src, r[1].w, len));
        uint64_t total = 0;
        for (unsigned k = 0; k < n; ++k) {
            CHECK(!mcdma_fabric_link_stats(r[0].p, k, &after[k]));
            uint64_t bytes = after[k].posted_bytes - before[k].posted_bytes;
            if (len >= 16384) CHECK(bytes > 0);
            total += bytes;
        }
        CHECK(total == len);
        CHECK(!mcdma_fabric_flush(r[0].p, 5 * NS));
    }
    /* One held member prevents publication, and a later overlapping write posts without a sender-side fence. */
    size_t src = WINDOW / 2 + 64, len = 65536;
    char last[12]; snprintf(last, sizeof(last), "tb%u", 2 * (n - 1) + 1);
    stub_tb_hold_completions(last, 1);
    fill(r[0].w + src, len, 90); fill(r[0].w + src + len, len, 91);
    CHECK(!mcdma_fabric_write_signal(r[0].p, src, 0, len, FLAG, 90));
    CHECK(!mcdma_fabric_write_signal(r[0].p, src + len, 0, len, FLAG, 91));
    usleep(1000);
    CHECK(__atomic_load_n((uint64_t *)(void *)(r[1].w + FLAG), __ATOMIC_ACQUIRE) == tag);
    stub_tb_hold_completions(last, 0);
    CHECK(!mcdma_fabric_wait(r[1].f, FLAG, 91, 5 * NS));
    CHECK(!memcmp(r[0].w + src + len, r[1].w, len));
    CHECK(!mcdma_fabric_flush(r[0].p, 5 * NS));
    /* One failed member poisons the whole peer; no automatic change to its negotiated lane count. */
    stub_tb_fail(last);
    int status = 0;
    for (unsigned i = 0; !status && i < 2000; ++i) {
        status = mcdma_fabric_peer_status(r[1].p, NULL, 0); usleep(1000);
    }
    CHECK(status == MCDMA_FABRIC_PEER);
    CHECK(mcdma_fabric_write(r[1].p, src, 0, 64) == MCDMA_FABRIC_PEER);
    for (unsigned rank = 0; rank < 2; ++rank) { mcdma_fabric_close(&r[rank].f); munmap(r[rank].w, WINDOW); }
    printf("N-bond %u: split, payloads, delayed reassembly, overlap, ports, auto GIDs and fail-closed passed\n", n);
    return 0;
}

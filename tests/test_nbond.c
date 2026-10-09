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

void stub_tb_hold_qp_completions(const char *, unsigned, int);
void stub_tb_fail_qp(const char *, unsigned);
unsigned stub_tb_live_qps(const char *);
unsigned stub_tb_live_mrs(const char *);
unsigned stub_tb_live_contexts(const char *);
void stub_tb_hold_nth_send(const char *, unsigned, unsigned);
void mcdma_fabric_test_observe_signal(struct mcdma_fabric_peer *,
    void (*)(void *, const unsigned char *, uint64_t, uint64_t), void *);
void mcdma_fabric_test_snapshot(struct mcdma_fabric_peer *, unsigned char *, uint64_t, size_t,
                                uint64_t, uint64_t *, uint64_t *);

struct rank { struct mcdma_fabric *f; struct mcdma_fabric_peer *p; unsigned char *w; unsigned n; uint16_t ports[8], peers[8]; int status; };

static void *connect_peer(void *arg) {
    struct rank *r = arg;
    r->status = mcdma_fabric_connect_links(r->f, "lo0/127.0.0.1", r->ports, r->peers, r->n, "nbond", 5 * NS, &r->p);
    return NULL;
}

static unsigned ports(unsigned n, unsigned qps) {
    for (unsigned p = 24000 + (unsigned)getpid() % 10000; p < 62000; p += 53) {
        int fds[48]; unsigned k;
        for (k = 0; k < 2 * n * qps; ++k) {
            fds[k] = socket(AF_INET, SOCK_DGRAM, 0);
            unsigned rank = k / (n * qps), qp = (k / n) % qps, d = k % n;
            unsigned port = p + 3 * (rank * n + d) + qp * (3 * (n - 1) + 1);
            struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (fds[k] < 0 || bind(fds[k], (struct sockaddr *)&a, sizeof(a))) break;
        }
        if (k < 2 * n * qps && fds[k] >= 0) close(fds[k]);
        for (unsigned i = 0; i < k; ++i) close(fds[i]);
        if (k == 2 * n * qps) return p;
    }
    return 0;
}

static void fill(unsigned char *p, size_t n, unsigned tag) {
    for (size_t i = 0; i < n; ++i) p[i] = (unsigned char)(i * 37 + tag * 11);
}

struct watch { const unsigned char *expected; size_t len; uint64_t value; unsigned seen; };
struct overwrite { struct rank *r; size_t src, len; int result; unsigned done; };
static void *plain_overwrite(void *arg) {
    struct overwrite *w = arg;
    w->result = mcdma_fabric_write(w->r->p, w->src, 0, w->len);
    __atomic_store_n(&w->done, 1, __ATOMIC_RELEASE);
    return NULL;
}
static void published(void *arg, const unsigned char *window, uint64_t off, uint64_t value) {
    struct watch *w = arg;
    if (off != FLAG || value != w->value) return;
    CHECK(!memcmp(window, w->expected, w->len)); /* flag is stored, the next signal sequence is still unreleased */
    __atomic_store_n(&w->seen, 1, __ATOMIC_RELEASE);
}

static unsigned flag_order(struct rank *r, unsigned n, unsigned lanes) {
    if (lanes < 2) return 0;
    const size_t len = 65536, src = WINDOW / 2 + 64, second = src + len + MCDMA_FABRIC_WS_ROOM;
    unsigned char *copy = malloc(len); CHECK(copy);
    uint64_t flag, placed[24];
    struct watch w = {r[0].w + src, len, 10, 0};
    fill(r[0].w + src, len, 130); fill(r[0].w + second, len, 131);
    mcdma_fabric_test_observe_signal(r[1].p, published, &w);
    /* Fresh parent turn is 0: this lane's third SEND is the first call's final signal, after its part. */
    stub_tb_hold_nth_send("tb0", 0, 3);
    CHECK(!mcdma_fabric_write_signal(r[0].p, src, 0, len, FLAG, 10));
    uint64_t deadline = link_now_ns() + 5 * NS;
    for (;;) {
        mcdma_fabric_test_snapshot(r[1].p, copy, 0, len, FLAG, &flag, placed);
        unsigned all = 1; for (unsigned k = 0; k < lanes; ++k) all &= placed[k] == 1;
        if (all) break;
        CHECK(link_now_ns() < deadline); usleep(10);
    }
    CHECK(flag == 0 && !memcmp(copy, r[0].w + src, len));
    CHECK(!mcdma_fabric_write_signal(r[0].p, second, 0, len, FLAG, 11));
    usleep(5000);
    mcdma_fabric_test_snapshot(r[1].p, copy, 0, len, FLAG, &flag, placed);
    CHECK(flag == 0 && !memcmp(copy, r[0].w + src, len));
    for (unsigned k = 0; k < lanes; ++k) CHECK(placed[k] == 1);
    stub_tb_hold_nth_send("tb0", 0, 0);
    CHECK(!mcdma_fabric_wait(r[1].f, FLAG, 11, 5 * NS));
    CHECK(__atomic_load_n(&w.seen, __ATOMIC_ACQUIRE) == 1);
    CHECK(!memcmp(r[1].w, r[0].w + second, len));
    CHECK(!mcdma_fabric_flush(r[0].p, 5 * NS));
    mcdma_fabric_test_observe_signal(r[1].p, NULL, NULL);

    /* A standalone signal also covers the earlier plain write's payload range. */
    unsigned lane = 2 % lanes, signal_lane = (lane + 1) % lanes;
    char device[12]; snprintf(device, sizeof(device), "tb%u", 2 * (signal_lane % n));
    struct watch plain = {r[0].w + src, len, 12, 0};
    fill(r[0].w + src, len, 132); fill(r[0].w + second, len, 133);
    mcdma_fabric_test_observe_signal(r[1].p, published, &plain);
    stub_tb_hold_nth_send(device, signal_lane / n, 1);
    CHECK(!mcdma_fabric_write(r[0].p, src, 0, len));
    CHECK(!mcdma_fabric_signal(r[0].p, FLAG, 12));
    deadline = link_now_ns() + 5 * NS;
    for (;;) {
        mcdma_fabric_test_snapshot(r[1].p, copy, 0, len, FLAG, &flag, placed);
        unsigned all = 1; for (unsigned k = 0; k < lanes; ++k) all &= placed[k] == (k == lane ? 3u : 2u);
        if (all) break;
        CHECK(link_now_ns() < deadline); usleep(10);
    }
    CHECK(flag == 11 && !memcmp(copy, r[0].w + src, len));
    CHECK(!mcdma_fabric_write_signal(r[0].p, second, 0, len, FLAG, 13));
    usleep(5000);
    mcdma_fabric_test_snapshot(r[1].p, copy, 0, len, FLAG, &flag, placed);
    CHECK(flag == 11 && !memcmp(copy, r[0].w + src, len));
    stub_tb_hold_nth_send(device, signal_lane / n, 0);
    CHECK(!mcdma_fabric_wait(r[1].f, FLAG, 13, 5 * NS));
    CHECK(__atomic_load_n(&plain.seen, __ATOMIC_ACQUIRE) == 1);
    CHECK(!memcmp(r[1].w, r[0].w + second, len));
    CHECK(!mcdma_fabric_flush(r[0].p, 5 * NS));
    mcdma_fabric_test_observe_signal(r[1].p, NULL, NULL);
    /* Below threshold the first payload uses one lane, while its signal uses another. Plain overwrite must fence. */
    if (lanes > 2) {
        const size_t small = 16384;
        uint64_t before[24];
        mcdma_fabric_test_snapshot(r[1].p, copy, 0, small, FLAG, &flag, before);
        unsigned data_lane = 5 % lanes, flag_lane = 6 % lanes;
        snprintf(device, sizeof(device), "tb%u", 2 * (flag_lane % n));
        struct watch small_watch = {r[0].w + src, small, 14, 0};
        fill(r[0].w + src, small, 134); fill(r[0].w + second, small, 135);
        mcdma_fabric_test_observe_signal(r[1].p, published, &small_watch);
        stub_tb_hold_nth_send(device, flag_lane / n, 1);
        CHECK(!mcdma_fabric_write_signal(r[0].p, src, 0, small, FLAG, 14));
        deadline = link_now_ns() + 5 * NS;
        for (;;) {
            mcdma_fabric_test_snapshot(r[1].p, copy, 0, small, FLAG, &flag, placed);
            if (placed[data_lane] == before[data_lane] + 1) break;
            CHECK(link_now_ns() < deadline); usleep(10);
        }
        CHECK(flag == 13 && !memcmp(copy, r[0].w + src, small));
        struct overwrite w2 = {&r[0], second, small, 0, 0};
        pthread_t writer; CHECK(!pthread_create(&writer, NULL, plain_overwrite, &w2));
        usleep(5000);
        CHECK(!__atomic_load_n(&w2.done, __ATOMIC_ACQUIRE));
        mcdma_fabric_test_snapshot(r[1].p, copy, 0, small, FLAG, &flag, placed);
        CHECK(flag == 13 && !memcmp(copy, r[0].w + src, small));
        stub_tb_hold_nth_send(device, flag_lane / n, 0);
        pthread_join(writer, NULL); CHECK(!w2.result);
        CHECK(__atomic_load_n(&small_watch.seen, __ATOMIC_ACQUIRE) == 1);
        CHECK(!mcdma_fabric_signal(r[0].p, FLAG, 15));
        CHECK(!mcdma_fabric_wait(r[1].f, FLAG, 15, 5 * NS));
        CHECK(!memcmp(r[1].w, r[0].w + second, small));
        CHECK(!mcdma_fabric_flush(r[0].p, 5 * NS));
        mcdma_fabric_test_observe_signal(r[1].p, NULL, NULL);
        free(copy); return 15;
    }
    free(copy); return 13;
}

int main(int argc, char **argv) {
    CHECK(argc == 2 || argc == 3);
    unsigned n = (unsigned)atoi(argv[1]); CHECK(n >= 1 && n <= 8);
    unsigned qps = argc == 3 ? (unsigned)atoi(argv[2]) : 1, lanes = n * qps;
    CHECK(qps >= 1 && qps <= 3);
    if (getenv("NBOND_DEFAULT_QPS")) CHECK(!unsetenv("MCDMA_FABRIC_QPS"));
    unsigned char *probe = mmap(NULL, WINDOW, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(probe != MAP_FAILED);
    struct mcdma_fabric *bad = NULL;
    const char *original = getenv("MCDMA_FABRIC_QPS");
    char saved[8] = ""; if (original) snprintf(saved, sizeof(saved), "%s", original);
    CHECK(!setenv("MCDMA_FABRIC_QPS", "4", 1));
    CHECK(mcdma_fabric_open("tb0", -1, 4096, probe, WINDOW, -1, 0, 0, &bad) == MCDMA_FABRIC_INVALID && !bad);
    CHECK(!setenv("MCDMA_FABRIC_QPS", "3", 1)); CHECK(!setenv("STUB_QP_CAP", "2", 1));
    CHECK(mcdma_fabric_open("tb0", -1, 4096, probe, WINDOW, -1, 0, 0, &bad) == MCDMA_FABRIC_DEVICE && !bad);
    CHECK(!unsetenv("STUB_QP_CAP"));
    if (*saved) CHECK(!setenv("MCDMA_FABRIC_QPS", saved, 1)); else CHECK(!unsetenv("MCDMA_FABRIC_QPS"));
    if (getenv("NBOND_CAP_ONE")) CHECK(!setenv("STUB_QP_CAP", "1", 1));
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
    unsigned base = ports(n, qps); CHECK(base);
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
        if (rank == 1 && getenv("NBOND_MISMATCH"))
            CHECK(!mcdma_fabric_open_qps(devices, -1, 4096, r[rank].w, WINDOW, -1, 0, MCDMA_FABRIC_PROGRESS_THREAD, 1, &r[rank].f));
        else CHECK(!mcdma_fabric_open(devices, -1, 4096, r[rank].w, WINDOW, -1, 0, MCDMA_FABRIC_PROGRESS_THREAD, &r[rank].f));
    }
    pthread_t th; CHECK(!pthread_create(&th, NULL, connect_peer, &r[1]));
    connect_peer(&r[0]); pthread_join(th, NULL);
    if (getenv("NBOND_MISMATCH")) {
        CHECK(r[0].status && r[1].status && !r[0].p && !r[1].p);
        for (unsigned rank = 0; rank < 2; ++rank) {
            mcdma_fabric_close(&r[rank].f); munmap(r[rank].w, WINDOW);
            for (unsigned d = 0; d < n; ++d) {
                char name[12]; snprintf(name, sizeof(name), "tb%u", 2 * d + rank);
                CHECK(!stub_tb_live_qps(name) && !stub_tb_live_mrs(name));
            }
        }
        puts("mismatched QP widths refused and all resources released"); return 0;
    }
    CHECK(!r[0].status && !r[1].status);
    if (getenv("NBOND_CLOSE_ONLY")) {
        for (unsigned rank = 0; rank < 2; ++rank) { mcdma_fabric_close(&r[rank].f); munmap(r[rank].w, WINDOW); }
        for (unsigned rank = 0; rank < 2; ++rank) for (unsigned d = 0; d < n; ++d) {
            char name[12]; snprintf(name, sizeof(name), "tb%u", 2 * d + rank);
            CHECK(!stub_tb_live_contexts(name) && !stub_tb_live_qps(name) && !stub_tb_live_mrs(name));
        }
        puts("exclusive connection mappings unmapped once and all contexts closed"); return 0;
    }
    CHECK(mcdma_fabric_link_count(r[0].p) == lanes);
    CHECK(mcdma_fabric_device_count(r[0].p) == n && mcdma_fabric_qps_per_device(r[0].p) == qps);
    if (qps > 1) {
        uint16_t overflow[8]; memcpy(overflow, r[0].ports, sizeof(overflow)); overflow[0] = 65535;
        struct mcdma_fabric_peer *extra = NULL;
        CHECK(mcdma_fabric_connect_links(r[0].f, "lo0/127.0.0.1", overflow, overflow, n,
                                          "overflow", NS, &extra) == MCDMA_FABRIC_INVALID && !extra);
    }
    for (unsigned rank = 0; rank < 2; ++rank) for (unsigned d = 0; d < n; ++d) {
        char name[12]; snprintf(name, sizeof(name), "tb%u", 2 * d + rank);
        CHECK(stub_tb_live_qps(name) == qps);
        CHECK(stub_tb_live_mrs(name) == 3 * qps); /* admission retired; each connection has two window MRs and one ring */
        CHECK(stub_tb_live_contexts(name) == qps + 1); /* QP-free admission context plus exclusive connections */
    }
    if (qps > 1) {
        struct mcdma_fabric_peer *extra = NULL;
        CHECK(mcdma_fabric_connect_links(r[0].f, "lo0/127.0.0.1", r[0].ports, r[0].peers, n,
                                          "over-budget", NS, &extra) == MCDMA_FABRIC_DEVICE && !extra);
    }
    const size_t sizes[] = {64, 16384, 32769, 40959, 40960, 40961, 163840, 1 << 20};
    unsigned tag = flag_order(r, n, lanes);
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        size_t len = sizes[i], src = WINDOW / 2 + 64;
        struct mcdma_fabric_link_stats before[24], after[24];
        for (unsigned k = 0; k < lanes; ++k) CHECK(!mcdma_fabric_link_stats(r[0].p, k, &before[k]));
        fill(r[0].w + src, len, ++tag);
        CHECK(!mcdma_fabric_write_signal(r[0].p, src, 0, len, FLAG, tag));
        CHECK(!mcdma_fabric_wait(r[1].f, FLAG, tag, 5 * NS));
        CHECK(!memcmp(r[0].w + src, r[1].w, len));
        uint64_t total = 0; unsigned active = 0;
        for (unsigned k = 0; k < lanes; ++k) {
            CHECK(!mcdma_fabric_link_stats(r[0].p, k, &after[k]));
            uint64_t bytes = after[k].posted_bytes - before[k].posted_bytes;
            active += bytes > 0;
            if (len >= 40960) CHECK(bytes > 0);
            total += bytes;
        }
        CHECK(total == len);
        if (len < 40960) CHECK(active == 1); /* no split overhead below the chosen threshold */
        CHECK(!mcdma_fabric_flush(r[0].p, 5 * NS));
    }
    /* One held member prevents publication, and a later overlapping write posts without a sender-side fence. */
    size_t src = WINDOW / 2 + 64, len = 65536, second = src + len + MCDMA_FABRIC_WS_ROOM;
    char last[12]; snprintf(last, sizeof(last), "tb%u", 2 * (n - 1) + 1);
    stub_tb_hold_qp_completions(last, qps - 1, 1);
    fill(r[0].w + src, len, 90); fill(r[0].w + second, len, 91);
    CHECK(!mcdma_fabric_write_signal(r[0].p, src, 0, len, FLAG, 90));
    CHECK(!mcdma_fabric_write_signal(r[0].p, second, 0, len, FLAG, 91));
    usleep(1000);
    CHECK(__atomic_load_n((uint64_t *)(void *)(r[1].w + FLAG), __ATOMIC_ACQUIRE) == tag);
    stub_tb_hold_qp_completions(last, qps - 1, 0);
    CHECK(!mcdma_fabric_wait(r[1].f, FLAG, 91, 5 * NS));
    CHECK(!memcmp(r[0].w + second, r[1].w, len));
    CHECK(!mcdma_fabric_flush(r[0].p, 5 * NS));
    if (getenv("NBOND_TRACE_REPORT")) {
        CHECK(mcdma_fabric_tracing(r[0].p) && mcdma_fabric_tracing(r[1].p));
        CHECK(!mcdma_fabric_trace_report(r[0].p, stdout));
        CHECK(!mcdma_fabric_trace_report(r[1].p, stdout));
    } else if (!getenv("MCDMA_TRACE")) {
        FILE *quiet = tmpfile(); CHECK(quiet);
        CHECK(!mcdma_fabric_tracing(r[0].p));
        CHECK(!mcdma_fabric_trace_report(r[0].p, quiet) && ftell(quiet) == 0);
        fclose(quiet);
    }
    /* One failed member poisons the whole peer; no automatic change to its negotiated lane count. */
    stub_tb_fail_qp(last, qps - 1);
    int status = 0;
    for (unsigned i = 0; !status && i < 2000; ++i) {
        status = mcdma_fabric_peer_status(r[1].p, NULL, 0); usleep(1000);
    }
    CHECK(status == MCDMA_FABRIC_PEER);
    CHECK(mcdma_fabric_write(r[1].p, src, 0, 64) == MCDMA_FABRIC_PEER);
    for (unsigned rank = 0; rank < 2; ++rank) { mcdma_fabric_close(&r[rank].f); munmap(r[rank].w, WINDOW); }
    for (unsigned rank = 0; rank < 2; ++rank) for (unsigned d = 0; d < n; ++d) {
        char name[12]; snprintf(name, sizeof(name), "tb%u", 2 * d + rank);
        CHECK(stub_tb_live_qps(name) == 0 && stub_tb_live_mrs(name) == 0);
        CHECK(stub_tb_live_contexts(name) == 0);
    }
    printf("N-bond %u x %u: split, payloads, delayed reassembly, overlap, ports, auto GIDs and fail-closed passed\n", n, qps);
    return 0;
}

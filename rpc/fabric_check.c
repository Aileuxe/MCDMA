/* fabric-check: qualify a libmcdma-fabric link. Two ranks ping-pong writes and signals at each size, each checking
 * every byte the moment the signal arrives, then rank 0 streams to rank 1 for a bandwidth figure. Exit 0 only when
 * every byte checked out.
 *   fabric-check DEVICE GID_INDEX VIA PORT NAME RANK
 *                [ROUNDS [SIZES [SECONDS [PEER_PORT [MODE [PROGRESS [WAIT [PATTERN [GAP_US]]]]]]]]]
 * Both ranks give the same NAME, ROUNDS and SIZES; RANK is 0 or 1. SIZES are bytes, comma-separated. WAIT is library
 * (mcdma_fabric_wait) or spin: spin on the window word as an engine does, leaving placement to progress threads.
 * PATTERN is pingpong, or swap: both ranks send at once and wait for each other's, after GAP_US of work, or with
 * GAP_US as A,B after A microseconds of work at rank 0 and B at rank 1, as when one side's work runs longer. */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#else
#define _DEFAULT_SOURCE 1
#endif
#include "mcdma_fabric.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define SLOT (16ull << 20)
#define FLAGS (4 * SLOT + 16384)    /* source headers have their own space before the flag page */
#define WINDOW (FLAGS + 16384)
#define PING FLAGS                  /* the peer's round lands here */
#define DONE (FLAGS + 8)            /* the bytes rank 0 streamed */
#define PIECE (4ull << 20)
#define STREAM_SLOTS (2 * SLOT / PIECE)
#define READY (FLAGS + 64)
#define ACK (FLAGS + 128)
#define START (FLAGS + 192)
#define CHECKED (FLAGS + 200)
#define FINISHED (FLAGS + 208)
#define SECOND 1000000000ull
#ifndef MCDMA_BUILD_ID
#define MCDMA_BUILD_ID "unknown"    /* the Makefile passes a checksum of the sources, so both ranks can compare */
#endif

static uint64_t clock_ns(void) {
#ifdef __APPLE__
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return (uint64_t)t.tv_sec * SECOND + (uint64_t)t.tv_nsec;
#endif
}

static uint64_t expected(uint64_t round, int rank, uint64_t i) {
    return (round << 32 | (uint64_t)rank << 31) ^ (i * 0x9e3779b97f4a7c15ull);
}

static void fill(unsigned char *p, uint64_t len, uint64_t round, int rank) {
    for (uint64_t i = 0; i * 8 < len; ++i) {
        uint64_t w = expected(round, rank, i);
        memcpy(p + i * 8, &w, len - i * 8 < 8 ? len - i * 8 : 8);
    }
}

static uint64_t wrong(const unsigned char *p, uint64_t len, uint64_t round, int rank) {
    uint64_t bad = 0;
    for (uint64_t i = 0; i * 8 < len; ++i) {
        uint64_t w = expected(round, rank, i);
        bad += memcmp(p + i * 8, &w, len - i * 8 < 8 ? len - i * 8 : 8) != 0;
    }
    return bad;
}

/* Poison before releasing a landing slot: an omitted byte cannot pass by retaining an earlier identical write. */
static uint64_t check_and_poison(unsigned char *p, uint64_t marker, int check) {
#if defined(__GNUC__)
    /* Walk two words at once without a vector 64-bit multiply; identical modulo-2^64 pattern arithmetic. */
    typedef uint64_t words2 __attribute__((vector_size(16)));
    const uint64_t stride = 0x9e3779b97f4a7c15ull;
    words2 seq = {0, stride}, step = {2 * stride, 2 * stride};
    words2 tag = {marker << 32, marker << 32}, count = {0, 0}, one = {1, 1};
    for (uint64_t i = 0; i < PIECE; i += 16) {
        words2 want = seq ^ tag, have;
        memcpy(&have, p + i, sizeof(have));
        if (check) count += (words2)(have != want) & one;
        want = ~want;
        memcpy(p + i, &want, sizeof(want));
        seq += step;
    }
    return count[0] + count[1];
#else
    uint64_t bad = 0;
    for (uint64_t i = 0; i < PIECE / 8; ++i) {
        uint64_t want = expected(marker, 0, i), have;
        memcpy(&have, p + i * 8, 8);
        bad += check && have != want;
        want = ~want;
        memcpy(p + i * 8, &want, 8);
    }
    return bad;
#endif
}

static int report(int status, const char *what);

/* Wait for the word at `off` to reach `value`, in the library or, with `spin`, by reading only the window as an
 * engine does; a spinning wait asks the library about the peer every few thousand reads, so a peer that went down
 * fails it at once. */
static int await(struct mcdma_fabric *f, struct mcdma_fabric_peer *p, const unsigned char *w, uint64_t off,
                 uint64_t value, uint64_t timeout_ns, int spin) {
    if (!spin) return mcdma_fabric_wait(f, off, value, timeout_ns);
    const uint64_t *word = (const uint64_t *)(const void *)(w + off);
    uint64_t deadline = clock_ns() + timeout_ns;
    for (unsigned spins = 1; __atomic_load_n(word, __ATOMIC_ACQUIRE) < value; ++spins) {
        if (spins % 4096) continue;
        char why[96];
        if (mcdma_fabric_peer_status(p, why, sizeof(why))) {
            fprintf(stderr, "fabric-check: the peer is down: %s\n", why);
            return MCDMA_FABRIC_PEER;
        }
        if (clock_ns() > deadline) return MCDMA_FABRIC_TIMEOUT;
    }
    return MCDMA_FABRIC_OK;
}

static int send_payload(struct mcdma_fabric_peer *p, uint64_t src, uint64_t dst, uint64_t len,
                        uint64_t flag, uint64_t value, int combined) {
    if (combined) return report(mcdma_fabric_write_signal(p, src, dst, len, flag, value), "write_signal");
    return report(mcdma_fabric_write(p, src, dst, len), "write") ||
           report(mcdma_fabric_signal(p, flag, value), "signal");
}

static int by_value(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static int report(int status, const char *what) {
    if (status) fprintf(stderr, "fabric-check: %s failed with status %d\n", what, status);
    return status;
}

static uint64_t ping_source(unsigned parity) { return (2 + parity) * SLOT + MCDMA_FABRIC_WS_ROOM; }

#ifdef FABRIC_CHECK_NO_MAIN
uint64_t fabric_check_test_source(unsigned parity) { return ping_source(parity); }
#endif

/* Swap `rounds` rounds of `len` bytes (at most SWAP_MAX), as a tensor-parallel engine exchanges partials: each rank
 * spins `gap_ns`, its stand-in for the GPU work between exchanges, then write-and-signals its bytes and waits for the
 * other's, which posted at the same moment. Each rank times its own rounds from its send to the other's flag. As in an
 * engine, no CPU work sits between the flag and the next send: three slots a side let each rank fill its next bytes
 * and check every byte of the other's previous round while this round is in flight (the other writes a slot again
 * only three rounds on, after this rank's next send). */
#define SWAP_MAX (2ull << 20)
#define SWAP_STRIDE (8ull << 20)    /* three slots in each half: sources keep their heads in one registration */
static uint64_t swap_dst(uint64_t n) { return (n % 3) * SWAP_STRIDE; }
static uint64_t swap_src(uint64_t n) { return 2 * SLOT + (n % 3) * SWAP_STRIDE + MCDMA_FABRIC_WS_ROOM; }

static uint64_t swap(struct mcdma_fabric *f, struct mcdma_fabric_peer *p, unsigned char *w, int rank, uint64_t len,
                     uint32_t rounds, uint64_t *round, uint64_t *times, int combined, int spin, uint64_t gap_ns) {
    uint64_t bad = 0, first = *round + 1;
    if (rounds) fill(w + swap_src(first), len, first, rank);
    for (uint32_t r = 0; r < rounds; ++r) {
        uint64_t n = ++*round;
        for (uint64_t until = clock_ns() + gap_ns; gap_ns && clock_ns() < until;) {
        }
        uint64_t began = clock_ns();
        if (send_payload(p, swap_src(n), swap_dst(n), len, PING, n, combined)) return ~0ull;
        if (r + 1 < rounds) fill(w + swap_src(n + 1), len, n + 1, rank);
        if (r) bad += wrong(w + swap_dst(n - 1), len, n - 1, !rank);
        if (report(await(f, p, w, PING, n, 10 * SECOND, spin), "waiting for the other rank")) return ~0ull;
        times[r] = clock_ns() - began;
    }
    if (rounds) bad += wrong(w + swap_dst(*round), len, *round, !rank);
    return bad;
}

#ifdef FABRIC_CHECK_NO_MAIN
uint64_t fabric_check_test_swap(uint64_t n, int src) { return src ? swap_src(n) : swap_dst(n); }
#endif

/* Ping-pong `rounds` rounds of `len` bytes; rank 0 times each round trip. Returns the words that landed wrong. The
 * round trip times the transport, as an engine's exchange runs: each rank fills its bytes before its round, rank 1
 * answers the moment the flag lands and checks every byte after answering (rank 0 writes that slot again only after
 * the next answer), and rank 0 checks every byte the moment the answer's flag lands, after stopping its clock. The
 * qualifier swaps the ranks, so each end's placement is checked at its flag in one direction. */
static uint64_t pingpong(struct mcdma_fabric *f, struct mcdma_fabric_peer *p, unsigned char *w, int rank, uint64_t len,
                         uint32_t rounds, uint64_t *round, uint64_t *times, int combined, int spin) {
    uint64_t bad = 0;
    for (uint32_t r = 0; r < rounds; ++r) {
        uint64_t n = ++*round, parity = n % 2, src = ping_source((unsigned)parity), began = 0;
        fill(w + src, len, n, rank);
        if (rank == 1) {
            if (report(await(f, p, w, PING, n, 10 * SECOND, spin), "waiting for rank 0") ||
                send_payload(p, src, parity * SLOT, len, PING, n, combined)) return ~0ull;
            bad += wrong(w + parity * SLOT, len, n, 0);
            continue;
        }
        began = clock_ns();
        if (send_payload(p, src, parity * SLOT, len, PING, n, combined) ||
            report(await(f, p, w, PING, n, 10 * SECOND, spin), "waiting for rank 1")) return ~0ull;
        times[r] = clock_ns() - began;
        bad += wrong(w + parity * SLOT, len, n, 1);
    }
    return bad;
}

/* Eight credited landing slots. Rank 1 checks and poisons each complete piece before acknowledging its reuse. */
static uint64_t stream(struct mcdma_fabric *f, struct mcdma_fabric_peer *p, unsigned char *w, int rank,
                       double seconds, int combined, int spin) {
    const uint64_t marker = 0xfeed;
    if (seconds == 0) return 0;
    if (rank == 1) {
        uint64_t checked = 0, bad = 0;
        for (uint64_t s = 0; s < STREAM_SLOTS; ++s) check_and_poison(w + s * PIECE, marker + s, 0);
        if (report(mcdma_fabric_signal(p, START, 1), "stream ready")) return ~0ull;
        for (uint64_t k = 0;; ++k) {
            uint64_t slot = k % STREAM_SLOTS, seq = k / STREAM_SLOTS + 1;
            if (report(await(f, p, w, READY + slot * 8, seq, 60 * SECOND, spin), "stream piece")) return ~0ull;
            uint64_t total = __atomic_load_n((uint64_t *)(void *)(w + DONE), __ATOMIC_ACQUIRE);
            if (total && checked == total) break;
            bad += check_and_poison(w + slot * PIECE, marker + slot, 1);
            checked += PIECE;
            if (report(mcdma_fabric_signal(p, ACK + slot * 8, seq), "stream acknowledgement")) return ~0ull;
        }
        printf("fabric-check: stream checked_bytes=%llu wrong_words=%llu\n", (unsigned long long)checked,
               (unsigned long long)bad);
        if (report(mcdma_fabric_signal(p, CHECKED, bad + 1), "stream result") ||
            report(mcdma_fabric_flush(p, 60 * SECOND), "stream result flush") ||
            report(mcdma_fabric_signal(p, FINISHED, 1), "stream result flushed")) return ~0ull;
        return bad;
    }
    if (report(mcdma_fabric_flush(p, 60 * SECOND), "before stream flush")) return ~0ull;
    unsigned links = mcdma_fabric_link_count(p);
    struct mcdma_fabric_link_stats *before = calloc(links ? links : 1, sizeof(*before));
    if (!before) return ~0ull;
    for (unsigned i = 0; i < links; ++i)
        if (report(mcdma_fabric_link_stats(p, i, &before[i]), "initial link stats")) { free(before); return ~0ull; }
    for (uint64_t s = 0; s < STREAM_SLOTS; ++s)
        fill(w + 2 * SLOT + s * (PIECE + MCDMA_FABRIC_WS_ROOM) + MCDMA_FABRIC_WS_ROOM, PIECE, marker + s, 0);
    if (report(await(f, p, w, START, 1, 60 * SECOND, spin), "stream start")) { free(before); return ~0ull; }
    uint64_t total = 0, began = clock_ns(), end = began + (uint64_t)(seconds * SECOND);
    for (uint64_t k = 0; clock_ns() < end || total < 4 * SLOT; ++k, total += PIECE) {
        uint64_t slot = k % STREAM_SLOTS, seq = k / STREAM_SLOTS + 1;
        if ((seq > 1 && report(await(f, p, w, ACK + slot * 8, seq - 1, 60 * SECOND, spin), "stream credit")) ||
            send_payload(p, 2 * SLOT + slot * (PIECE + MCDMA_FABRIC_WS_ROOM) + MCDMA_FABRIC_WS_ROOM,
                         slot * PIECE, PIECE, READY + slot * 8, seq, combined)) { free(before); return ~0ull; }
    }
    uint64_t chunks = total / PIECE;
    for (uint64_t s = 0; s < STREAM_SLOTS; ++s) {
        uint64_t seq = (chunks - 1 - s) / STREAM_SLOTS + 1;
        if (report(await(f, p, w, ACK + s * 8, seq, 60 * SECOND, spin), "final stream credit")) {
            free(before); return ~0ull;
        }
    }
    if (report(mcdma_fabric_flush(p, 60 * SECOND), "stream flush")) { free(before); return ~0ull; }
    double elapsed = (double)(clock_ns() - began) / SECOND;
    printf("fabric-check: stream bytes=%llu seconds=%.3f gbit_s=%.2f checked_bytes=%llu gb_s=%.3f\n",
           (unsigned long long)total, elapsed, (double)total * 8 / elapsed / 1e9, (unsigned long long)total,
           (double)total / elapsed / 1e9);
    for (unsigned i = 0; i < links; ++i) {
        struct mcdma_fabric_link_stats after;
        if (report(mcdma_fabric_link_stats(p, i, &after), "final link stats")) { free(before); return ~0ull; }
        printf("fabric-check: stream link=%u posted_bytes=%llu completed_bytes=%llu\n", i,
               (unsigned long long)(after.posted_bytes - before[i].posted_bytes),
               (unsigned long long)(after.completed_bytes - before[i].completed_bytes));
    }
    free(before);
    if (report(mcdma_fabric_signal(p, DONE, total), "stream signal") ||
        report(mcdma_fabric_signal(p, READY + (chunks % STREAM_SLOTS) * 8, UINT64_MAX), "stream end") ||
        report(mcdma_fabric_flush(p, 60 * SECOND), "final flush") ||
        report(await(f, p, w, CHECKED, 1, 60 * SECOND, spin), "stream check result") ||
        report(await(f, p, w, FINISHED, 1, 60 * SECOND, spin), "receiver result flush")) return ~0ull;
    return __atomic_load_n((uint64_t *)(void *)(w + CHECKED), __ATOMIC_ACQUIRE) - 1;
}

static int number(const char *s, uint64_t max, uint64_t *out) {
    char *end;
    if (!s || !*s || *s == '-') return 0;
    errno = 0;
    unsigned long long n = strtoull(s, &end, 10);
    if (errno || *end || n > max) return 0;
    *out = n;
    return 1;
}

static int valid_sizes(const char *s) {
    if (!s || !*s) return 0;
    for (;;) {
        char *end;
        if (*s < '0' || *s > '9') return 0;
        errno = 0;
        (void)strtoull(s, &end, 10);
        if (errno || (*end && *end != ',')) return 0;
        if (!*end) return 1;
        s = end + 1;
    }
}

int fabric_check(int argc, char **argv) {
    if (argc < 7 || argc > 16) {
        fprintf(stderr, "usage: fabric-check DEVICE GID_INDEX VIA PORT NAME RANK "
                        "[ROUNDS [SIZES [SECONDS [PEER_PORT [MODE [PROGRESS [WAIT [PATTERN [GAP_US]]]]]]]]]\n"
                        "MODE is split or combined; PROGRESS is 0 or 1; SECONDS defaults to 60, 0 skips streaming.\n"
                        "WAIT is library or spin; spin needs PROGRESS 1, which a bond always has.\n"
                        "PATTERN is pingpong or swap; GAP_US is the work each rank does before each swap.\n");
        return 2;
    }
    uint64_t rank_arg, gid, port, peer_port = 0, rounds_arg = 10000, progress = 0, gap_us = 0, gaps[2] = {0, 0};
    if (!number(argv[6], 1, &rank_arg) || !number(argv[2], INT_MAX, &gid) ||
        !number(argv[4], 65535, &port) || !port ||
        (argc > 7 && !number(argv[7], UINT32_MAX, &rounds_arg)) ||
        (argc > 10 && !number(argv[10], 65535, &peer_port)) ||
        (argc > 12 && !number(argv[12], 1, &progress))) return 2;
    int rank = (int)rank_arg, combined = argc > 11 && !strcmp(argv[11], "combined");
    if (argc > 11 && !combined && strcmp(argv[11], "split")) return 2;
    int spin = argc > 13 && !strcmp(argv[13], "spin");
    if (argc > 13 && ((!spin && strcmp(argv[13], "library")) || (spin && !progress))) return 2;
    int swapping = argc > 14 && !strcmp(argv[14], "swap");
    if (argc > 14 && !swapping && strcmp(argv[14], "pingpong")) return 2;
    if (argc > 15) {
        char first[24], *comma = strchr(argv[15], ',');
        size_t n = comma ? (size_t)(comma - argv[15]) : strlen(argv[15]);
        if (n >= sizeof(first)) return 2;
        memcpy(first, argv[15], n), first[n] = 0;
        if (!number(first, 1000000, &gaps[0]) || !number(comma ? comma + 1 : first, 1000000, &gaps[1])) return 2;
        gap_us = gaps[rank_arg];
    }
    uint32_t rounds = (uint32_t)rounds_arg;
    const char *sizes = argc > 8 ? argv[8] : "64,4096,14336,1048576";
    char *end = NULL;
    double seconds = argc > 9 ? strtod(argv[9], &end) : 60;
    if (!valid_sizes(sizes) || !isfinite(seconds) || seconds < 0 || seconds > 86400 ||
        (argc > 9 && (!*argv[9] || !end || *end))) return 2;
    unsigned char *w = mmap(NULL, WINDOW, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    uint64_t *times = calloc(rounds ? rounds : 1, sizeof(uint64_t)), round = 0, bad = 0;
    struct mcdma_fabric *f = NULL;
    struct mcdma_fabric_peer *p = NULL;
    if (w == MAP_FAILED || !times) {
        if (w != MAP_FAILED) munmap(w, WINDOW);
        free(times);
        return 2;
    }
    if (report(mcdma_fabric_open(argv[1], (int)gid, 4096, w, WINDOW, -1, 0,
                                 progress ? MCDMA_FABRIC_PROGRESS_THREAD : 0, &f), "open") ||
        report(mcdma_fabric_connect(f, argv[3], (int)port, (int)peer_port, argv[5], 120 * SECOND, &p), "connect")) {
        bad = ~0ull;
        goto finished;
    }
    printf("fabric-check: rank %d connected over %s\n", rank,
           mcdma_fabric_link(p) == MCDMA_FABRIC_THUNDERBOLT ? "thunderbolt" : "roce");
    printf("fabric-check: metadata abi=%u device=%s via=%s gid_index=%llu links=%u mode=%s progress=%llu "
           "qos=%s wait_poll=%s wait=%s pattern=%s gap_us=%llu stream_seconds=%.3f verification=every_byte build=%s\n",
           mcdma_fabric_abi(), argv[1], argv[3], (unsigned long long)gid, mcdma_fabric_link_count(p),
           combined ? "combined" : "split", (unsigned long long)progress,
           getenv("MCDMA_FABRIC_QOS") ? getenv("MCDMA_FABRIC_QOS") : "0",
           getenv("MCDMA_FABRIC_WAIT_POLL") ? getenv("MCDMA_FABRIC_WAIT_POLL") : "1", spin ? "spin" : "library",
           swapping ? "swap" : "pingpong", (unsigned long long)gap_us, seconds, MCDMA_BUILD_ID);
    for (const char *s = sizes; s && *s && bad != ~0ull; s = strchr(s, ',') ? strchr(s, ',') + 1 : NULL) {
        uint64_t len = strtoull(s, NULL, 10), wrong_words = 0, most = swapping ? SWAP_MAX : SLOT;
        len = len < 1 ? 1 : len > most ? most : len;
        wrong_words = swapping ? swap(f, p, w, rank, len, rounds, &round, times, combined, spin, gap_us * 1000)
                               : pingpong(f, p, w, rank, len, rounds, &round, times, combined, spin);
        if (wrong_words == ~0ull) {
            bad = ~0ull;
            break;
        }
        bad += wrong_words;
        qsort(times, rounds, sizeof(uint64_t), by_value);
        /* gb_s: twice the size over the median round, the rate one direction runs at in a ping-pong and both
         * directions together in a swap; both ranks time a swap */
        if ((rank == 0 || swapping) && rounds)
            printf("fabric-check: size=%llu rounds=%u wrong_words=%llu rtt_median_us=%.2f rtt_p99_us=%.2f "
                   "rtt_max_us=%.2f rtt_p50_us=%.2f gb_s=%.3f\n",
                   (unsigned long long)len, rounds, (unsigned long long)wrong_words, times[rounds / 2] / 1e3,
                   times[(uint64_t)rounds * 99 / 100] / 1e3, times[rounds - 1] / 1e3, times[rounds / 2] / 1e3,
                   2.0 * (double)len / (double)(times[rounds / 2] ? times[rounds / 2] : 1));
        else
            printf("fabric-check: size=%llu rounds=%u wrong_words=%llu\n", (unsigned long long)len, rounds,
                   (unsigned long long)wrong_words);
    }
    if (bad != ~0ull) {
        uint64_t streamed = stream(f, p, w, rank, seconds, combined, spin);
        bad = streamed == ~0ull ? ~0ull : bad + streamed;
    }
    /* rank 1 lingers so rank 0's last flush is answered before the link goes away */
    if (rank == 1) mcdma_fabric_wait(f, DONE + 8, 1, SECOND);
finished:
    mcdma_fabric_disconnect(&p);
    mcdma_fabric_close(&f);
    munmap(w, WINDOW);
    free(times);
    printf("fabric-check: rank %d %s\n", rank, bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}

#ifndef FABRIC_CHECK_NO_MAIN
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    return fabric_check(argc, argv);
}
#endif

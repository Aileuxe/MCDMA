/* fabric-check: qualify a libmcdma-fabric link. Two ranks ping-pong writes and signals at each size, each checking
 * every byte the moment the signal arrives, then rank 0 streams to rank 1 for a bandwidth figure. Exit 0 only when
 * every byte checked out.
 *   fabric-check DEVICE GID_INDEX VIA PORT NAME RANK [ROUNDS [SIZES [SECONDS [PEER_PORT]]]]
 * Both ranks give the same NAME, ROUNDS and SIZES; RANK is 0 or 1. SIZES are bytes, comma-separated. */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include "mcdma_fabric.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define SLOT (16ull << 20)
#define FLAGS (4 * SLOT)            /* two landing slots, two source slots, then the flag page */
#define WINDOW (FLAGS + 16384)
#define PING FLAGS                  /* the peer's round lands here */
#define DONE (FLAGS + 8)            /* the bytes rank 0 streamed */
#define PIECE (4ull << 20)
#define SECOND 1000000000ull

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

static int by_value(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static int report(int status, const char *what) {
    if (status) fprintf(stderr, "fabric-check: %s failed with status %d\n", what, status);
    return status;
}

/* Ping-pong `rounds` rounds of `len` bytes; rank 0 times each round trip. Returns the words that landed wrong. */
static uint64_t pingpong(struct mcdma_fabric *f, struct mcdma_fabric_peer *p, unsigned char *w, int rank, uint64_t len,
                         uint32_t rounds, uint64_t *round, uint64_t *times) {
    uint64_t bad = 0;
    for (uint32_t r = 0; r < rounds; ++r) {
        uint64_t n = ++*round, parity = n % 2, src = (2 + parity) * SLOT, began = 0;
        if (rank == 1 && report(mcdma_fabric_wait(f, PING, n, 10 * SECOND), "waiting for rank 0")) return ~0ull;
        if (rank == 1) bad += wrong(w + parity * SLOT, len, n, 0);
        fill(w + src, len, n, rank);
        began = clock_ns();
        if (report(mcdma_fabric_write(p, src, parity * SLOT, len), "write") ||
            report(mcdma_fabric_signal(p, PING, n), "signal"))
            return ~0ull;
        if (rank == 1) continue;
        if (report(mcdma_fabric_wait(f, PING, n, 10 * SECOND), "waiting for rank 1")) return ~0ull;
        times[r] = clock_ns() - began;
        bad += wrong(w + parity * SLOT, len, n, 1);
    }
    return bad;
}

/* Rank 0 writes 4 MiB pieces round the peer's two landing slots for `seconds`, then signals the total. */
static uint64_t stream(struct mcdma_fabric *f, struct mcdma_fabric_peer *p, unsigned char *w, int rank,
                       double seconds) {
    const uint64_t marker = 0xfeed;
    if (rank == 1) {
        if (report(mcdma_fabric_wait(f, DONE, 1, (uint64_t)(seconds * SECOND) + 60 * SECOND), "waiting for the stream"))
            return ~0ull;
        uint64_t total = __atomic_load_n((uint64_t *)(void *)(w + DONE), __ATOMIC_ACQUIRE);
        return total >= 2 * SLOT ? wrong(w, 2 * SLOT, marker, 0) : 0;
    }
    fill(w + 2 * SLOT, 2 * SLOT, marker, 0);
    uint64_t total = 0, began = clock_ns(), end = began + (uint64_t)(seconds * SECOND);
    for (uint64_t k = 0; clock_ns() < end || total < 2 * SLOT; ++k, total += PIECE) {
        uint64_t at = (k % (2 * SLOT / PIECE)) * PIECE;
        if (report(mcdma_fabric_write(p, 2 * SLOT + at, at, PIECE), "stream write")) return ~0ull;
    }
    if (report(mcdma_fabric_flush(p, 60 * SECOND), "stream flush")) return ~0ull;
    double elapsed = (double)(clock_ns() - began) / SECOND;
    printf("fabric-check: stream bytes=%llu seconds=%.3f gbit_s=%.2f\n", (unsigned long long)total, elapsed,
           (double)total * 8 / elapsed / 1e9);
    /* flushed, so the signal has landed before this rank tears its queue pairs down */
    return report(mcdma_fabric_signal(p, DONE, total), "stream signal") ||
                   report(mcdma_fabric_flush(p, 60 * SECOND), "final flush")
               ? ~0ull
               : 0;
}

int fabric_check(int argc, char **argv) {
    if (argc < 7) {
        fprintf(stderr, "usage: fabric-check DEVICE GID_INDEX VIA PORT NAME RANK [ROUNDS [SIZES [SECONDS [PEER_PORT]]]]\n");
        return 2;
    }
    int rank = atoi(argv[6]);
    uint32_t rounds = argc > 7 ? (uint32_t)strtoul(argv[7], NULL, 10) : 10000;
    const char *sizes = argc > 8 ? argv[8] : "64,4096,14336,1048576";
    double seconds = argc > 9 ? atof(argv[9]) : 5;
    unsigned char *w = mmap(NULL, WINDOW, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    uint64_t *times = calloc(rounds ? rounds : 1, sizeof(uint64_t)), round = 0, bad = 0;
    struct mcdma_fabric *f = NULL;
    struct mcdma_fabric_peer *p = NULL;
    if (w == MAP_FAILED || !times || (rank != 0 && rank != 1)) return 2;
    if (report(mcdma_fabric_open(argv[1], atoi(argv[2]), 4096, w, WINDOW, -1, 0, 0, &f), "open") ||
        report(mcdma_fabric_connect(f, argv[3], atoi(argv[4]), argc > 10 ? atoi(argv[10]) : 0, argv[5], 120 * SECOND,
                                    &p),
               "connect"))
        return 1;
    printf("fabric-check: rank %d connected over %s\n", rank,
           mcdma_fabric_link(p) == MCDMA_FABRIC_THUNDERBOLT ? "thunderbolt" : "roce");
    for (const char *s = sizes; s && *s && bad != ~0ull; s = strchr(s, ',') ? strchr(s, ',') + 1 : NULL) {
        uint64_t len = strtoull(s, NULL, 10), wrong_words = 0;
        len = len < 1 ? 1 : len > SLOT ? SLOT : len;
        if ((wrong_words = pingpong(f, p, w, rank, len, rounds, &round, times)) == ~0ull) {
            bad = ~0ull;
            break;
        }
        bad += wrong_words;
        qsort(times, rounds, sizeof(uint64_t), by_value);
        if (rank == 0 && rounds)
            printf("fabric-check: size=%llu rounds=%u wrong_words=%llu rtt_median_us=%.2f rtt_p99_us=%.2f "
                   "rtt_max_us=%.2f\n",
                   (unsigned long long)len, rounds, (unsigned long long)wrong_words, times[rounds / 2] / 1e3,
                   times[rounds * 99 / 100] / 1e3, times[rounds - 1] / 1e3);
        else
            printf("fabric-check: size=%llu rounds=%u wrong_words=%llu\n", (unsigned long long)len, rounds,
                   (unsigned long long)wrong_words);
    }
    if (bad != ~0ull) {
        uint64_t streamed = stream(f, p, w, rank, seconds);
        bad = streamed == ~0ull ? ~0ull : bad + streamed;
    }
    /* rank 1 lingers so rank 0's last flush is answered before the link goes away */
    if (rank == 1) mcdma_fabric_wait(f, DONE + 8, 1, SECOND);
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

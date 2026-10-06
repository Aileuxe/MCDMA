/* Offline run of fabric-check: both ranks in one process over the stub's Thunderbolt or RoCE devices and the
 * loopback's link-local address. Exit 0 means both ranks passed, 77 that loopback has no link-local address. */
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "../rpc/link.h"
#include "../rpc/mcdma_fabric.h"

int fabric_check(int argc, char **argv);
uint64_t fabric_check_test_source(unsigned parity);
uint64_t fabric_check_test_swap(uint64_t n, int src);

struct rank {
    char *argv[16];
    char port[8], peer[8], name[8];
    int status;
};

static void *run(void *arg) {
    struct rank *r = arg;
    r->status = fabric_check(16, r->argv);
    return NULL;
}

static int free_ports(int count) {
    int fd = socket(AF_INET6, SOCK_DGRAM, 0), second = -1;
    struct sockaddr_in6 a;
    socklen_t len = sizeof(a);
    memset(&a, 0, sizeof(a));
    a.sin6_family = AF_INET6;
    if (fd < 0) return -1;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) || getsockname(fd, (struct sockaddr *)&a, &len)) {
        close(fd);
        return -1;
    }
    int port = ntohs(a.sin6_port);
    if (count == 2) {
        if (port == 65535 || (second = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) { close(fd); return -1; }
        a.sin6_port = htons((uint16_t)(port + 1));
        if (bind(second, (struct sockaddr *)&a, sizeof(a))) { close(second); close(fd); return -1; }
        close(second);
    }
    close(fd);
    return port;
}

static int invalid_arguments(void) {
    char *a[] = {"fabric-check", "unused", "0", "unused", "20000", "check", "0", "1", "64", "0",
                 "0", "split", "0"};
    const struct { unsigned index; const char *value; } bad[] = {
        {2, "-1"}, {4, "0"}, {4, "65536"}, {6, "2"}, {7, "4294967296"}, {7, "-1"},
        {8, "64,"}, {8, "64,nope"}, {9, "nan"}, {9, "-1"}, {9, "1junk"}, {9, "86401"},
        {10, "65536"}, {11, "unknown"}, {12, "2"}
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
        char *was = a[bad[i].index];
        a[bad[i].index] = (char *)bad[i].value;
        int result = fabric_check(13, a);
        a[bad[i].index] = was;
        if (result != 2) return 1;
    }
    /* WAIT is library or spin, and spinning leaves placement to progress threads, so it needs PROGRESS 1 */
    char *w[] = {"fabric-check", "unused", "0", "unused", "20000", "check", "0", "1", "64", "0", "0", "split", "0",
                 "spin", "swap", "5"};
    if (fabric_check(14, w) != 2) return 1;
    w[12] = "1", w[13] = "nope";
    if (fabric_check(14, w) != 2) return 1;
    /* PATTERN is pingpong or swap, and GAP_US a number of microseconds up to a second */
    w[13] = "spin", w[14] = "trade";
    if (fabric_check(15, w) != 2) return 1;
    w[14] = "swap", w[15] = "1000001";
    if (fabric_check(16, w) != 2) return 1;
    const char *gaps[] = {"-5", "5,", ",5", "5,6,7", "5,x", "1,1000001"};
    for (unsigned i = 0; i < sizeof(gaps) / sizeof(*gaps); ++i) {
        w[15] = (char *)gaps[i];
        if (fabric_check(16, w) != 2) return 1;
    }
    return 0;
}

static int source_preflight(void) {
    const uint64_t slot = 16ull << 20, sizes[] = {64, 4096};
    for (unsigned parity = 0; parity < 2; ++parity) {
        uint64_t off = fabric_check_test_source(parity);
        if (off < 2 * slot + MCDMA_FABRIC_WS_ROOM || off + slot > 4 * slot + 16384) return 1;
        for (unsigned i = 0; i < sizeof(sizes) / sizeof(*sizes); ++i)
            if ((off - MCDMA_FABRIC_WS_ROOM) / TB_SEG != (off + sizes[i] - 1) / TB_SEG) return 1;
    }
    /* The former odd-parity address crosses this registration boundary with its source header. */
    uint64_t old = 3 * slot;
    if ((old - MCDMA_FABRIC_WS_ROOM) / TB_SEG == (old + sizes[1] - 1) / TB_SEG) return 1;
    /* a swap's three sources keep their heads and up to 2 MiB in one registration; its three landing slots hold 2 MiB
     * each below the sources, and nothing reaches the flags */
    for (uint64_t n = 0; n < 3; ++n) {
        uint64_t src = fabric_check_test_swap(n, 1), dst = fabric_check_test_swap(n, 0);
        if ((src - MCDMA_FABRIC_WS_ROOM) / TB_SEG != (src + (2ull << 20) - 1) / TB_SEG) return 1;
        if (dst + (2ull << 20) > 2 * slot || src + (2ull << 20) > 4 * slot) return 1;
        for (uint64_t m = 0; m < n; ++m)
            if (fabric_check_test_swap(m, 1) + (2ull << 20) > src || fabric_check_test_swap(m, 0) + (2ull << 20) > dst)
                return 1;
    }
    return 0;
}

static int pair(const char *lo, int tb, int bonded, int combined, int reverse, int spin, int swapping) {
    int lanes = bonded ? 2 : 1, ports[2];
    do { ports[0] = free_ports(lanes); } while (ports[0] < 0);
    do { ports[1] = free_ports(lanes); } while (ports[1] < 0 || abs(ports[0] - ports[1]) < lanes);
    struct rank r[2] = {0};
    pthread_t t[2];
    char via[128];
    if (bonded) snprintf(via, sizeof(via), "%s+%s", lo, lo);
    else snprintf(via, sizeof(via), "%s", lo);
    for (int i = 0; i < 2; ++i) {
        int device_rank = i ^ reverse;
        snprintf(r[i].port, sizeof(r[i].port), "%d", ports[i]);
        snprintf(r[i].peer, sizeof(r[i].peer), "%d", ports[!i]);
        snprintf(r[i].name, sizeof(r[i].name), "%d", i);
        char *device = bonded ? (device_rank ? "tb1+tb3" : "tb0+tb2") :
                       tb ? (device_rank ? "tb1" : "tb0") : (device_rank ? "roce1" : "roce0");
        char *args[16] = {"fabric-check", device, "0", via, r[i].port, "check", r[i].name, "8",
                          "1,64,4096,10240,14336,40960,163840,1048576,13631491,16777216", "0.2", r[i].peer,
                          tb && combined ? "combined" : "split", combined || spin ? "1" : "0",
                          spin ? "spin" : "library", swapping ? "swap" : "pingpong", swapping ? "3,9" : "0"};
        memcpy(r[i].argv, args, sizeof(args));
        if (pthread_create(&t[i], NULL, run, &r[i])) return 1;
    }
    for (int i = 0; i < 2; ++i) pthread_join(t[i], NULL);
    return r[0].status || r[1].status;
}

int main(int argc, char **argv) {
    static char lo[16];
    struct ifaddrs *all = NULL;
    if (argc != 2 || (strcmp(argv[1], "tb") && strcmp(argv[1], "roce"))) return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (invalid_arguments() || source_preflight()) return 1;
    if (!getifaddrs(&all)) {
        for (struct ifaddrs *a = all; a && !lo[0]; a = a->ifa_next)
            if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET6 && (a->ifa_flags & IFF_LOOPBACK) &&
                IN6_IS_ADDR_LINKLOCAL(&((struct sockaddr_in6 *)(void *)a->ifa_addr)->sin6_addr))
                snprintf(lo, sizeof(lo), "%s", a->ifa_name);
        freeifaddrs(all);
    }
    if (!lo[0]) return 77;
    int tb = !strcmp(argv[1], "tb");
    if (pair(lo, tb, 0, 0, 0, 0, 0) || pair(lo, tb, 0, 1, 1, 0, 0) || pair(lo, tb, 0, 1, 0, 1, 0) ||
        pair(lo, tb, 0, 1, 1, 1, 1) || pair(lo, tb, 0, 0, 0, 0, 1)) return 1;
    return tb && (pair(lo, tb, 1, 0, 0, 0, 0) || pair(lo, tb, 1, 1, 1, 0, 0) || pair(lo, tb, 1, 1, 0, 1, 0) ||
                  pair(lo, tb, 1, 1, 1, 1, 0) || pair(lo, tb, 1, 1, 0, 1, 1) || pair(lo, tb, 1, 0, 1, 0, 1));
}

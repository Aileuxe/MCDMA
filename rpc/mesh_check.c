/* mesh-check: qualify a four-node full mesh with libmcdma-fabric. Each node drives its three links at once: one-shot
 * all-reduces of bf16 rows, summed in fp32 in fixed node order 0, 1, 2, 3 and rounded once, every byte and every sum
 * checked against partials each node can compute for itself; then every link streams at once; then a long run.
 *   mesh-check NODE GID_INDEX BASE_PORT SECONDS ROWS LINK LINK LINK
 * NODE is 0-3. ROWS is a comma-separated list of row counts (14,336-byte Kimi K3 rows). A LINK is
 * DEVICE:IFACE:PEER, the port's RDMA device and Thunderbolt interface and the node at its other end, optionally
 * :LOCAL_PORT:PEER_PORT; by default the link between nodes a < b uses UDP BASE_PORT + 4a + b on both ends. */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#else
#define _GNU_SOURCE 1
#endif
#include "mcdma_fabric.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define NODES 4
#define LINKS 3
#define COLS 7168                   /* bf16 values in a K3 hidden row */
#define ROW (COLS * 2)
#define MAX_ROWS 300
#define SECOND 1000000000ull
#define STALL (10 * SECOND)
#define STREAM (4ull << 20)

struct link {
    char device[64], iface[96];
    int peer, port, peer_port, status;
    struct mcdma_fabric *f;
    struct mcdma_fabric_peer *p;
};

struct mesh {
    int node, gid;
    uint64_t slot, flags, window, step, wrong, stalls, hash;
    unsigned char *w;
    uint16_t *sum, *expect;
    struct link links[LINKS];
};

static uint64_t clock_ns(void) {
#ifdef __APPLE__
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return (uint64_t)t.tv_sec * SECOND + (uint64_t)t.tv_nsec;
#endif
}

/* A finite bf16 value that depends only on (node, step, row, column), so every node can compute every partial. */
static uint16_t value(int node, uint64_t step, uint64_t row, uint64_t col) {
    uint64_t h = (step * 0x9e3779b97f4a7c15ull) ^ ((uint64_t)node << 56) ^ (row << 24) ^ col;
    h ^= h >> 33, h *= 0xff51afd7ed558ccdull, h ^= h >> 33;
    return (uint16_t)((h & 1) << 15 | (120 + (h >> 1) % 16) << 7 | ((h >> 8) & 0x7f));
}

static float widen(uint16_t b) {
    uint32_t bits = (uint32_t)b << 16;
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

/* Round to nearest even; no NaN can arise from these partials. */
static uint16_t narrow(float f) {
    uint32_t bits;
    memcpy(&bits, &f, 4);
    return (uint16_t)((bits + 0x7fff + ((bits >> 16) & 1)) >> 16);
}

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const unsigned char *b = p;
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

static unsigned char *slot(struct mesh *m, uint64_t step, int node) { return m->w + ((step % 2) * NODES + node) * m->slot; }

static uint64_t *flag(struct mesh *m, int node) { return (uint64_t *)(void *)(m->w + m->flags + 8 * node); }

static int peer_link(const struct mesh *m, int node) {
    for (int k = 0; k < LINKS; ++k)
        if (m->links[k].peer == node) return k;
    return -1;
}

/* Wait until each peer's flag reaches step << 1; the progress threads place what arrives. 0, or -1 on a stall. */
static int wait_all(struct mesh *m, uint64_t step) {
    uint64_t began = clock_ns();
    for (int k = 0; k < LINKS;) {
        if (__atomic_load_n(flag(m, m->links[k].peer), __ATOMIC_ACQUIRE) >= step << 1) {
            k++;
            continue;
        }
        if (clock_ns() - began > STALL) {
            fprintf(stderr, "mesh-check: node %d waited 10 s for node %d at step %llu\n", m->node, m->links[k].peer,
                    (unsigned long long)step);
            m->stalls++;
            return -1;
        }
    }
    return 0;
}

/* Whether any node asked to stop at `step`: a flag word is step << 1 | stop, and a peer already past `step` did not. */
static int stopping(struct mesh *m, uint64_t step, int mine) {
    for (int k = 0; k < LINKS; ++k) {
        uint64_t w = __atomic_load_n(flag(m, m->links[k].peer), __ATOMIC_ACQUIRE);
        mine |= (w >> 1) == step && (w & 1);
    }
    return mine;
}

/* One all-reduce of `rows` rows: comm and sum times in ns, or -1 when a link failed or stalled. */
static int reduce(struct mesh *m, uint64_t rows, int stop, uint64_t *comm_ns, uint64_t *sum_ns) {
    uint64_t step = ++m->step, bytes = rows * ROW;
    uint16_t *mine = (uint16_t *)(void *)slot(m, step, m->node);
    /* the peers' flags for step - 1 prove our step - 2 bytes have arrived, so this source is free to refill */
    for (uint64_t r = 0; r < rows; ++r)
        for (uint64_t c = 0; c < COLS; ++c) mine[r * COLS + c] = value(m->node, step, r, c);
    uint64_t began = clock_ns(), off = (uint64_t)((unsigned char *)mine - m->w);
    for (int k = 0; k < LINKS; ++k)
        if (mcdma_fabric_write(m->links[k].p, off, off, bytes)) return -1;
    for (int k = 0; k < LINKS; ++k)
        if (mcdma_fabric_signal(m->links[k].p, m->flags + 8 * m->node, step << 1 | (uint64_t)stop)) return -1;
    if (wait_all(m, step)) return -1;
    uint64_t landed = clock_ns();
    for (uint64_t i = 0; i < rows * COLS; ++i) {
        float acc = 0;
        for (int n = 0; n < NODES; ++n) acc += widen(((uint16_t *)(void *)slot(m, step, n))[i]);
        m->sum[i] = narrow(acc);
    }
    uint64_t done = clock_ns();
    *comm_ns = landed - began, *sum_ns = done - landed;
    /* check every partial that crossed a link, then the sum against a reference built from local partials */
    for (int n = 0; n < NODES; ++n) {
        if (n == m->node) continue;
        const uint16_t *got = (const uint16_t *)(void *)slot(m, step, n);
        for (uint64_t r = 0; r < rows; ++r)
            for (uint64_t c = 0; c < COLS; ++c) m->wrong += got[r * COLS + c] != value(n, step, r, c);
    }
    for (uint64_t r = 0; r < rows; ++r)
        for (uint64_t c = 0; c < COLS; ++c) {
            float acc = 0;
            for (int n = 0; n < NODES; ++n) acc += widen(value(n, step, r, c));
            m->expect[r * COLS + c] = narrow(acc);
        }
    m->wrong += memcmp(m->sum, m->expect, bytes) != 0;
    m->hash = fnv(m->hash, m->sum, bytes);
    return 0;
}

static int by_value(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void report(const struct mesh *m, const char *what, uint64_t rows, uint64_t *comm, uint64_t *sum, uint64_t n) {
    qsort(comm, n, sizeof(uint64_t), by_value);
    qsort(sum, n, sizeof(uint64_t), by_value);
    printf("mesh-check: node=%d %s rows=%llu steps=%llu comm_p50_us=%.1f comm_p99_us=%.1f comm_max_us=%.1f "
           "sum_p50_us=%.1f wrong=%llu stalls=%llu hash=%016llx\n",
           m->node, what, (unsigned long long)rows, (unsigned long long)n, n ? comm[n / 2] / 1e3 : 0.0,
           n ? comm[n * 99 / 100] / 1e3 : 0.0, n ? comm[n - 1] / 1e3 : 0.0, n ? sum[n / 2] / 1e3 : 0.0,
           (unsigned long long)m->wrong, (unsigned long long)m->stalls, (unsigned long long)m->hash);
}

/* Every link streams at once; a node writes only its own two slots at each peer, so no later step is overwritten. */
static int stream(struct mesh *m, double seconds) {
    uint64_t total = 0, began = clock_ns(), end = began + (uint64_t)(seconds * SECOND), base = ++m->step;
    uint64_t piece = m->slot < STREAM ? m->slot : STREAM, pieces = m->slot / piece;
    for (uint64_t i = 0; clock_ns() < end || !total; ++i) {
        uint64_t off = ((i / pieces % 2) * NODES + (uint64_t)m->node) * m->slot + (i % pieces) * piece;
        for (int k = 0; k < LINKS; ++k)
            if (mcdma_fabric_write(m->links[k].p, off, off, piece)) return -1;
        total += piece;
    }
    for (int k = 0; k < LINKS; ++k)
        if (mcdma_fabric_flush(m->links[k].p, 60 * SECOND) ||
            mcdma_fabric_signal(m->links[k].p, m->flags + 8 * m->node, base << 1))
            return -1;
    if (wait_all(m, base)) return -1;
    double elapsed = (double)(clock_ns() - began) / SECOND;
    printf("mesh-check: node=%d stream links=3 bytes_per_link=%llu seconds=%.3f gbit_s_per_link=%.2f "
           "gbit_s_out=%.2f\n",
           m->node, (unsigned long long)total, elapsed, (double)total * 8 / elapsed / 1e9,
           (double)total * 3 * 8 / elapsed / 1e9);
    return 0;
}

static void *connect_link(void *arg) {
    struct link *l = arg;
    char name[16];
    snprintf(name, sizeof(name), "mesh%d", l->status);
    l->status = mcdma_fabric_connect(l->f, l->iface, l->port, l->peer_port, name, 300 * SECOND, &l->p);
    return NULL;
}

static int parse_link(const char *spec, int node, int base, struct link *l) {
    char rest[200];
    memset(l, 0, sizeof(*l));
    int n = sscanf(spec, "%63[^:]:%95[^:]:%d%199s", l->device, l->iface, &l->peer, rest);
    if (n < 3 || l->peer < 0 || l->peer >= NODES || l->peer == node) return -1;
    int lo = node < l->peer ? node : l->peer, hi = node < l->peer ? l->peer : node;
    l->port = l->peer_port = base + 4 * lo + hi;
    if (n == 4 && sscanf(rest, ":%d:%d", &l->port, &l->peer_port) != 2) return -1;
    l->status = 10 * lo + hi;   /* the link's name until connect replaces this with its status */
    return 0;
}

int mesh_check(int argc, char **argv) {
    if (argc != 9) {
        fprintf(stderr, "usage: mesh-check NODE GID_INDEX BASE_PORT SECONDS ROWS LINK LINK LINK\n"
                        "       LINK = DEVICE:IFACE:PEER[:LOCAL_PORT:PEER_PORT]\n");
        return 2;
    }
    struct mesh *m = calloc(1, sizeof(*m));
    uint64_t sizes[16], nsizes = 0, max_rows = 1;
    double seconds = atof(argv[4]);
    for (const char *s = argv[5]; s && *s && nsizes < 16; s = strchr(s, ',') ? strchr(s, ',') + 1 : NULL) {
        uint64_t rows = strtoull(s, NULL, 10);
        sizes[nsizes++] = rows < 1 ? 1 : rows > MAX_ROWS ? MAX_ROWS : rows;
        max_rows = sizes[nsizes - 1] > max_rows ? sizes[nsizes - 1] : max_rows;
    }
    m->node = atoi(argv[1]), m->gid = atoi(argv[2]), m->hash = 0xcbf29ce484222325ull;
    m->slot = (max_rows * ROW + 16383) / 16384 * 16384;
    m->flags = 2 * NODES * m->slot, m->window = m->flags + 16384;
    m->w = mmap(NULL, m->window, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    m->sum = malloc(max_rows * ROW), m->expect = malloc(max_rows * ROW);
    if (!m || m->w == MAP_FAILED || !m->sum || !m->expect || m->node < 0 || m->node >= NODES || !nsizes) return 2;
    for (int k = 0; k < LINKS; ++k)
        if (parse_link(argv[6 + k], m->node, atoi(argv[3]), &m->links[k]) ||
            (k && peer_link(m, m->links[k].peer) != k)) {
            fprintf(stderr, "mesh-check: bad link %s\n", argv[6 + k]);
            return 2;
        }
    /* one fabric a port over the same window, and all three links met at once */
    pthread_t t[LINKS];
    for (int k = 0; k < LINKS; ++k) {
        struct link *l = &m->links[k];
        /* a progress thread a port: a node sending on one link must keep taking what the other two bring */
        if (mcdma_fabric_open(l->device, m->gid, 4096, m->w, m->window, -1, 0, MCDMA_FABRIC_PROGRESS_THREAD, &l->f)) {
            fprintf(stderr, "mesh-check: cannot open %s\n", l->device);
            return 1;
        }
        pthread_create(&t[k], NULL, connect_link, l);
    }
    int bad = 0;
    for (int k = 0; k < LINKS; ++k) {
        pthread_join(t[k], NULL);
        if (m->links[k].status)
            fprintf(stderr, "mesh-check: link to node %d failed (%d)\n", m->links[k].peer, m->links[k].status);
        bad |= m->links[k].status != 0;
    }
    uint64_t *comm = calloc(1u << 20, sizeof(uint64_t)), *sum = calloc(1u << 20, sizeof(uint64_t)), c, s;
    if (bad || !comm || !sum) return 1;
    printf("mesh-check: node=%d connected to nodes %d %d %d\n", m->node, m->links[0].peer, m->links[1].peer,
           m->links[2].peer);
    /* sizes: a fixed number of steps a size, fewer for large ones */
    for (uint64_t z = 0; z < nsizes && !bad; ++z) {
        uint64_t steps = sizes[z] >= 64 ? 200 : 2000, n = 0;
        if (seconds <= 1) steps = 20;
        for (; n < steps && !(bad = reduce(m, sizes[z], 0, &c, &s)); ++n) comm[n] = c, sum[n] = s;
        report(m, "allreduce", sizes[z], comm, sum, n);
    }
    if (!bad) bad = stream(m, seconds < 5 ? 0.2 : 5);
    /* the long run: the same random sizes on every node, ending together at the first step any clock stops */
    uint64_t end = clock_ns() + (uint64_t)(seconds * SECOND), n = 0, seed = 0x2545f4914f6cdd1dull;
    uint64_t next = clock_ns() + 60 * SECOND;
    for (int done = 0; !bad && !done;) {
        seed ^= seed << 13, seed ^= seed >> 7, seed ^= seed << 17;
        int stop = clock_ns() >= end;
        if (!(bad = reduce(m, sizes[seed % nsizes], stop, &c, &s)) && n < (1u << 20)) comm[n] = c, sum[n] = s, n++;
        done = bad || stopping(m, m->step, stop);
        if (clock_ns() >= next || done) {
            report(m, "longrun", 0, comm, sum, n);
            n = 0, next += 60 * SECOND;
        }
    }
    for (int k = 0; k < LINKS; ++k) {
        mcdma_fabric_disconnect(&m->links[k].p);
        mcdma_fabric_close(&m->links[k].f);
    }
    printf("mesh-check: node=%d %s wrong=%llu stalls=%llu hash=%016llx\n", m->node,
           bad || m->wrong || m->stalls ? "FAIL" : "PASS", (unsigned long long)m->wrong,
           (unsigned long long)m->stalls, (unsigned long long)m->hash);
    return bad || m->wrong || m->stalls ? 1 : 0;
}

#ifndef MESH_CHECK_NO_MAIN
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    return mesh_check(argc, argv);
}
#endif

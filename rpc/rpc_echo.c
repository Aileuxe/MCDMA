/* rpc-echo: qualify an mcdma-rpcd link. The service, beside the listen daemon, answers every request with its bytes
 * transformed; the client, beside the connect daemon, checks every reply and times each call. Exit 0 only when every
 * byte came back right.
 *   rpc-echo service NAME
 *   rpc-echo client NAME [CALLS [SIZES]]     SIZES: comma-separated request bytes, default 0,1,4096,65536,1048576 */
#include "rpcd.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define CALL_NS 10000000000ull

static uint32_t reply_len(uint32_t len, uint64_t rep) { return (uint32_t)((len * 7ull + 13) % (rep - CTRL)); }
static unsigned char reply_byte(const unsigned char *req, uint32_t len, uint64_t i) {
    return (unsigned char)((len ? req[i % len] : 0) ^ 0x5a ^ (i * 131));
}

/* Map a mailbox, taking R and P from the words the daemon wrote at request +256. */
static unsigned char *map_box(const char *path, int shm, uint64_t *req, uint64_t *rep) {
    int fd = -1;
    struct stat st;
    /* the daemon may still be starting: give it ten seconds to create the mailbox */
    for (int i = 0; i < 100 && fd < 0; ++i)
        if ((fd = shm ? shm_open(path, O_RDWR, 0) : open(path, O_RDWR)) < 0 && errno == ENOENT) usleep(100000);
    if (fd < 0 || fstat(fd, &st) || st.st_size < (off_t)(2 * CTRL)) {
        fprintf(stderr, "rpc-echo: cannot open mailbox %s (errno %d); is the daemon running?\n", path, errno);
        if (fd >= 0) close(fd);
        return NULL;
    }
    unsigned char *b = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (b == MAP_FAILED) return NULL;
    *req = load_word((volatile uint64_t *)(b + 256));
    *rep = load_word((volatile uint64_t *)(b + 264));
    if (*req + *rep > (uint64_t)st.st_size || *req < 2 * CTRL || *rep < 2 * CTRL) return NULL;
    return b;
}

int echo_service(const char *name) {
    char sock[160], box[160], answer[16] = "";
    snprintf(sock, sizeof(sock), "%s/mcdma-rpcd.%s.sock", MCDMA_RPC_SOCK_DIR, name);
#ifdef MCDMA_RPC_BOX_DIR
    snprintf(box, sizeof(box), "%s/mcdma-rpc.%s", MCDMA_RPC_BOX_DIR, name);
    int shm = 0;
#else
    snprintf(box, sizeof(box), "/mcdma-rpc.%s", name);
    int shm = 1;
#endif
    const char *env = getenv("MCDMA_RPCD_SOCKET");
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", env && *env ? env : sock);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    uint64_t req = 0, rep = 0;
    unsigned char *b = map_box(box, shm, &req, &rep);
    if (!b || fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) || send(fd, "MODE poll\n", 10, 0) != 10 ||
        recv(fd, answer, sizeof(answer) - 1, 0) < 2 || strncmp(answer, "OK", 2)) {
        fprintf(stderr, "rpc-echo: the listen daemon at %s did not take the service (%s)\n", a.sun_path, answer);
        return 2;
    }
    fprintf(stderr, "rpc-echo: serving %s (%llu + %llu MiB)\n", name, (unsigned long long)(req >> 20),
            (unsigned long long)(rep >> 20));
    volatile uint64_t *word = (volatile uint64_t *)b, *staged = (volatile uint64_t *)(b + req + 128);
    uint32_t last = WORD_SEQ(load_word(word));
    uint64_t served = 0;
    for (unsigned idle = 0;;) {
        uint64_t w = load_word(word);
        if (!WORD_SEQ(w) || WORD_SEQ(w) == last) {
            char bye[8];
            /* the registration ends with BYE or a closed socket when the link drops or the daemon stops */
            if (++idle % 4096 == 0) {
                ssize_t got = recv(fd, bye, sizeof(bye), MSG_DONTWAIT);
                if (got >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) break;
            }
            cpu_relax();
            continue;
        }
        last = WORD_SEQ(w);
        uint32_t len = WORD_LEN(w), out = reply_len(len, rep);
        for (uint64_t i = 0; i < out; ++i) b[req + CTRL + i] = reply_byte(b + CTRL, len, i);
        store_word(staged, WORD(last, out));
        served++;
    }
    fprintf(stderr, "rpc-echo: the daemon ended the registration after %llu calls\n", (unsigned long long)served);
    close(fd);
    return 0;
}

static int compare(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

int echo_client(const char *name, uint32_t calls, const char *sizes) {
    char box[64];
    snprintf(box, sizeof(box), "/mcdma-rpc.%s", name);
    uint64_t req = 0, rep = 0, list[32];
    int nsizes = 0;
    unsigned char *b = map_box(box, 1, &req, &rep);
    for (const char *s = sizes; s && *s && nsizes < 32; s = strchr(s, ',') ? strchr(s, ',') + 1 : NULL)
        list[nsizes++] = strtoull(s, NULL, 10);
    uint64_t *times = calloc(calls ? calls : 1, sizeof(uint64_t));
    unsigned char *expect = malloc(rep);
    if (!b || !nsizes || !times || !expect) return 2;
    volatile uint64_t *word = (volatile uint64_t *)b, *done = (volatile uint64_t *)(b + req + 64);
    uint64_t deadline = now_ns() + CALL_NS, errors = 0, generation;
    while (load_word((volatile uint64_t *)(b + 64)) != 1)
        if (now_ns() > deadline) {
            fprintf(stderr, "rpc-echo: the link to %s never came up\n", name);
            return 1;
        }
    generation = load_word((volatile uint64_t *)(b + 72));
    uint32_t seq = WORD_SEQ(load_word(word));
    for (uint32_t call = 0; call < calls; ++call) {
        uint32_t len = (uint32_t)(list[call % nsizes] < req - CTRL ? list[call % nsizes] : req - CTRL);
        for (uint32_t i = 0; i < len; ++i) b[CTRL + i] = (unsigned char)(i * 31 + call);
        uint32_t out = reply_len(len, rep);
        for (uint64_t i = 0; i < out; ++i) expect[i] = reply_byte(b + CTRL, len, i);
        if (!++seq) seq = 1;
        uint64_t began = now_ns();
        store_word(word, WORD(seq, len));
        while (WORD_SEQ(load_word(done)) != seq) {
            /* a reconnect loses the request in flight: fail the call rather than wait on */
            if (now_ns() - began > CALL_NS || load_word((volatile uint64_t *)(b + 72)) != generation) break;
            cpu_relax();
        }
        times[call] = now_ns() - began;
        uint64_t w = load_word(done);
        if (WORD_SEQ(w) != seq || WORD_LEN(w) != out || memcmp(b + req + CTRL, expect, out)) {
            if (!errors) fprintf(stderr, "rpc-echo: call %u of %u bytes came back wrong or late\n", call, len);
            errors++;
            generation = load_word((volatile uint64_t *)(b + 72));
        }
    }
    qsort(times, calls, sizeof(uint64_t), compare);
    printf("rpc-echo: calls=%u errors=%llu median_us=%.1f p99_us=%.1f max_us=%.1f\n", calls,
           (unsigned long long)errors, calls ? times[calls / 2] / 1e3 : 0.0, calls ? times[calls * 99 / 100] / 1e3 : 0.0,
           calls ? times[calls - 1] / 1e3 : 0.0);
    free(times), free(expect);
    return errors ? 1 : 0;
}

#ifndef RPC_ECHO_NO_MAIN
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    if (argc == 3 && !strcmp(argv[1], "service") && valid_name(argv[2])) return echo_service(argv[2]);
    if (argc >= 3 && argc <= 5 && !strcmp(argv[1], "client") && valid_name(argv[2]))
        return echo_client(argv[2], argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 10) : 1000,
                           argc > 4 ? argv[4] : "0,1,4096,65536,1048576");
    fprintf(stderr, "usage: rpc-echo service NAME\n       rpc-echo client NAME [CALLS [SIZES]]\n");
    return 2;
}
#endif

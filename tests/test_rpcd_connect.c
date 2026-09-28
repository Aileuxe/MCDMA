/* Offline regression test of mcdma-rpcd connect mode against the stub verbs library, one scenario per run:
 *   race       a client that stages the moment it sees the link up is still served
 *   hang       SHUTDOWN completes even when the peer accepts the connection and never answers HELLO
 *   pullcrash  a pull-mode ready word longer than the reply half drops the link instead of crashing the daemon
 *   sigterm    SIGTERM during a pending call stops the daemon the orderly way, with no verbs object misused
 * A fake listen end runs in this process, so the stub's WRITE and READ land in its buffer. Exit 0 means pass. */
#include "../rpc/rpcd.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static const char *g_mode;
static char g_name[32], g_sock[104];
static unsigned char *g_peerbuf;
static volatile uint64_t g_mac_reply;
static volatile int g_finished, g_passed;
static int g_buffer_fd = -1;

static int race_mode(void) { return !strcmp(g_mode, "race") || !strcmp(g_mode, "dmabuf"); }

static int read_line(int fd, char *out, size_t n) {
    size_t at = 0;
    while (at + 1 < n) {
        char c;
        if (recv(fd, &c, 1, 0) <= 0) return -1;
        if (c == '\n') break;
        out[at++] = c;
    }
    out[at] = 0;
    return (int)at;
}

/* The listen end: answers HELLO with a buffer in this process, then serves requests the way the scenario needs. */
static void *fake_listen(void *arg) {
    int ls = *(int *)arg;
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) return NULL;
        char line[LINE];
        if (read_line(fd, line, sizeof(line)) < 0) {
            close(fd);
            continue;
        }
        char *save = NULL, *tok = strtok_r(line, " ", &save);
        for (int i = 0; i < 9 && tok; ++i) tok = strtok_r(NULL, " ", &save);
        tok = strtok_r(NULL, " ", &save);
        if (tok) g_mac_reply = strtoull(tok, NULL, 10);
        if (!strcmp(g_mode, "hang")) {
            while (!g_finished) usleep(10000);
            close(fd);
            continue;
        }
        char reply[256];
        snprintf(reply, sizeof(reply), "HELLO 777 %u fe80::1 4242 %llu\n", PSN, (unsigned long long)(uintptr_t)g_peerbuf);
        send(fd, reply, strlen(reply), 0);
        read_line(fd, line, sizeof(line));
        volatile uint64_t *req = (volatile uint64_t *)g_peerbuf;
        uint32_t last = 0;
        while (!g_finished) {
            uint64_t w = load_word(req);
            if (WORD_SEQ(w) && WORD_SEQ(w) != last) {
                last = WORD_SEQ(w);
                unsigned char *mac = (unsigned char *)(uintptr_t)g_mac_reply;
                if (race_mode()) {
                    memcpy(mac + CTRL, "pong", 4);
                    store_word((volatile uint64_t *)(mac + 64), WORD(last, 4));
                } else if (!strcmp(g_mode, "pullcrash")) {
                    store_word((volatile uint64_t *)mac, WORD(last, 8u << 20));
                }
            }
            usleep(100);
        }
        close(fd);
    }
}

static unsigned char *map_mailbox(void) {
    if (g_buffer_fd >= 0) {
        void *memory = mmap(NULL, 8u << 20, PROT_READ | PROT_WRITE, MAP_SHARED, g_buffer_fd, 0);
        return memory == MAP_FAILED ? NULL : memory;
    }
    char shm[64];
    snprintf(shm, sizeof(shm), "/mcdma-rpc.%s", g_name);
    int f = -1;
    for (int i = 0; i < 5000 && f < 0; ++i) {
        f = shm_open(shm, O_RDWR, 0);
        if (f < 0) usleep(1000);
    }
    if (f < 0) return NULL;
    unsigned char *b = mmap(NULL, 8u << 20, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
    close(f);
    return b == MAP_FAILED ? NULL : b;
}

static int wait_word(volatile uint64_t *word, uint64_t want, int mask_seq, uint64_t timeout_ms) {
    for (uint64_t waited = 0; waited < timeout_ms * 10; ++waited) {
        uint64_t w = load_word(word);
        if (mask_seq ? WORD_SEQ(w) == want : w == want) return 1;
        usleep(100);
    }
    return 0;
}

static void *client(void *arg) {
    (void)arg;
    unsigned char *b = map_mailbox();
    if (!b) return NULL;
    volatile uint64_t *up = (volatile uint64_t *)(b + 64), *req = (volatile uint64_t *)b;
    volatile uint64_t *done = (volatile uint64_t *)(b + (4u << 20) + 64);
    /* spin rather than sleep: the race is a client staging within microseconds of seeing the link up */
    uint64_t deadline = now_ns() + 5000000000ull;
    while (load_word(up) != 1)
        if (now_ns() > deadline) return NULL;
    if (g_buffer_fd >= 0) {
        /* The imported allocation was filled before daemon startup; no payload
         * byte, including the old prefault loop's page starts, may be touched. */
        for (size_t i = CTRL; i < (8u << 20); ++i) {
            if (i >= (4u << 20) && i < (4u << 20) + CTRL) continue;
            if (b[i] != 0xa7) { fprintf(stderr, "dmabuf: daemon modified a payload byte\n"); return NULL; }
        }
    }
    memcpy(b + CTRL, "hello", 5);
    store_word(req, WORD(1, 5));
    if (race_mode()) {
        g_passed = wait_word(done, 1, 1, 2000);
        if (g_passed && memcmp(b + (4u << 20) + CTRL, "pong", 4)) g_passed = 0;
        if (!g_passed) fprintf(stderr, "race: a request staged right after link-up was lost\n");
    } else if (!strcmp(g_mode, "pullcrash")) {
        g_passed = wait_word(up, 0, 0, 5000);
        if (!g_passed) fprintf(stderr, "pullcrash: an oversized reply did not drop the link\n");
    } else {
        g_passed = 1;
    }
    g_finished = 1;
    return NULL;
}

static void command(const char *text) {
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", g_sock);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (!connect(fd, (struct sockaddr *)&a, sizeof(a))) {
        char line[64];
        snprintf(line, sizeof(line), "%s\n", text);
        send(fd, line, strlen(line), 0);
        read_line(fd, line, sizeof(line));
    }
    close(fd);
}

static void *controller(void *arg) {
    (void)arg;
    if (!strcmp(g_mode, "hang") || !strcmp(g_mode, "sigterm")) {
        sleep(1);
        g_passed = 1;
        if (!strcmp(g_mode, "hang")) command("SHUTDOWN");
        else kill(getpid(), SIGTERM);
    } else {
        while (!g_finished) usleep(1000);
        command("SHUTDOWN");
    }
    /* the daemon must be gone well within its bounded waits; anything longer is the hang this test exists for */
    sleep(12);
    fprintf(stderr, "%s: the daemon did not stop within 12 s\n", g_mode);
    _exit(3);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    g_mode = argv[1];
    setvbuf(stderr, NULL, _IOLBF, 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    snprintf(g_name, sizeof(g_name), "t%dc", (int)getpid());
    snprintf(g_sock, sizeof(g_sock), "/tmp/rpcd-test-%d.sock", (int)getpid());
    setenv("MCDMA_RPCD_SOCKET", g_sock, 1);
    if (!strcmp(g_mode, "dmabuf")) {
        char filename[] = "/tmp/rpcd-buffer-test-XXXXXX";
        g_buffer_fd = mkstemp(filename);
        if (g_buffer_fd < 0 || unlink(filename) || ftruncate(g_buffer_fd, 8u << 20)) return 2;
        unsigned char *initial = mmap(NULL, 8u << 20, PROT_READ | PROT_WRITE, MAP_SHARED, g_buffer_fd, 0);
        if (initial == MAP_FAILED) return 2;
        memset(initial, 0xa7, 8u << 20);
        munmap(initial, 8u << 20);
    }
    g_peerbuf = calloc(1, 8u << 20);
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(a);
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) || listen(ls, 4) || getsockname(ls, (struct sockaddr *)&a, &len))
        return 2;
    pthread_t listener, user, ctl;
    pthread_create(&listener, NULL, fake_listen, &ls);
    if (strcmp(g_mode, "hang")) pthread_create(&user, NULL, client, NULL);
    pthread_create(&ctl, NULL, controller, NULL);
    char spec[160];
    snprintf(spec, sizeof(spec), "%s,127.0.0.1,%d,stub0,0,4096", g_name, ntohs(a.sin_port));
    char *specs[1] = {spec};
    int direct = race_mode();
    int rc = g_buffer_fd >= 0 ? run_connect_with_buffer(1, specs, direct, g_buffer_fd) : run_connect(1, specs, direct);
    if (g_buffer_fd >= 0) {
        if (fcntl(g_buffer_fd, F_GETFD) < 0) { fprintf(stderr, "dmabuf: borrowed descriptor was closed\n"); return 1; }
        close(g_buffer_fd);
    }
    g_finished = 1;
#ifdef MCDMA_RPC_LOCK_DIR
    char lock[160];
    snprintf(lock, sizeof(lock), "%s/mcdma-rpc.%s.lock", MCDMA_RPC_LOCK_DIR, g_name);
    unlink(lock);
#endif
    if (rc != 0 || !g_passed) {
        fprintf(stderr, "%s: failed (run_connect %d)\n", g_mode, rc);
        return 1;
    }
    printf("test_rpcd_connect %s: ok\n", g_mode);
    return 0;
}

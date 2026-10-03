/* Offline regression test of mcdma-rpcd connect mode against the stub verbs library, one scenario per run:
 *   race       a client that stages the moment it sees the link up is still served
 *   hang       SHUTDOWN completes even when the listen end never answers the offer
 *   pullcrash  a pull-mode ready word longer than the reply half drops the link instead of crashing the daemon
 *   sigterm    SIGTERM during a pending call stops the daemon the orderly way, with no verbs object misused
 *   bye        a goodbye from the listen end drops the link at once
 *   silent     a listen end that stops answering pings is declared down
 * A fake listen end runs in this process and speaks the exchange over the loopback interface's link-local address,
 * so the stub's WRITE and READ land in its buffer. Exit 0 means pass, 77 that loopback has no link-local address. */
#include "../rpc/rpcd.h"

#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static const char *g_mode;
static char g_name[32], g_sock[104], g_lo[16];
static unsigned char *g_peerbuf;
static volatile uint64_t g_mac_reply;
static volatile int g_finished, g_passed;
static struct xchg g_fake;

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

static int mode_is(const char *m) { return !strcmp(g_mode, m); }

/* Answer one session's offer from RTS, as a listen end does, and wait for the connect end's confirmation. */
static int fake_session(struct xchg *x, const struct xmsg *offer) {
    struct xmsg a, m;
    g_mac_reply = offer->info.table.base + offer->info.req;
    memcpy(x->peer_session, offer->from, 16);
    xchg_prepare(x, &a, X_OFFER, ROLE_LISTEN, X_HAVE);
    a.info.transport = LINK_ROCE, a.info.mode = offer->info.mode, a.info.qpn = 777, a.info.psn = 1;
    a.info.gid[0] = 0xfe, a.info.gid[1] = 0x80, a.info.gid[15] = 1;
    a.info.req = offer->info.req, a.info.rep = offer->info.rep;
    a.info.table.base = (uint64_t)(uintptr_t)g_peerbuf, a.info.table.length = offer->info.req + offer->info.rep;
    a.info.table.seg = a.info.table.length, a.info.table.n = 1, a.info.table.rkey[0] = 4242;
    for (uint64_t deadline = now_ns() + 5000000000ull; now_ns() < deadline && !g_finished;) {
        (void)xchg_send(x, &a);
        if (xchg_recv(x, &m, 100) == 1 && m.kind == X_OFFER && (m.flags & X_HAVE) && !memcmp(m.from, offer->from, 16))
            return 1;
    }
    return 0;
}

/* The listen end: answers offers with a buffer in this process, then serves requests the way the scenario needs. */
static void *fake_listen(void *arg) {
    (void)arg;
    struct xmsg m;
    while (!g_finished) {
        if (xchg_recv(&g_fake, &m, 50) != 1 || m.kind != X_OFFER || m.role != ROLE_CONNECT || (m.flags & X_HAVE)) continue;
        if (mode_is("hang") || !fake_session(&g_fake, &m)) continue;
        volatile uint64_t *req = (volatile uint64_t *)g_peerbuf;
        uint32_t last = 0;
        int served = 0;
        while (!g_finished) {
            uint64_t w = load_word(req);
            if (WORD_SEQ(w) && WORD_SEQ(w) != last) {
                last = WORD_SEQ(w);
                unsigned char *mac = (unsigned char *)(uintptr_t)g_mac_reply;
                if (mode_is("race")) {
                    memcpy(mac + CTRL, "pong", 4);
                    store_word((volatile uint64_t *)(mac + 64), WORD(last, 4));
                } else if (mode_is("pullcrash")) {
                    store_word((volatile uint64_t *)mac, WORD(last, 8u << 20));
                }
                served++;
            }
            struct xmsg in;
            if (xchg_recv(&g_fake, &in, 1) == 1 && in.kind == X_PING && !mode_is("silent"))
                xchg_answer(&g_fake, in.from, X_PONG, ROLE_LISTEN, NULL);
            if (mode_is("bye") && served) {
                xchg_answer(&g_fake, g_fake.peer_session, X_BYE, ROLE_LISTEN, NULL);
                while (!g_finished) usleep(1000);
            }
        }
    }
    return NULL;
}

static unsigned char *map_mailbox(void) {
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
    memcpy(b + CTRL, "hello", 5);
    store_word(req, WORD(1, 5));
    if (mode_is("race")) {
        g_passed = wait_word(done, 1, 1, 2000) && !memcmp(b + (4u << 20) + CTRL, "pong", 4);
        if (!g_passed) fprintf(stderr, "race: a request staged right after link-up was lost\n");
    } else if (mode_is("pullcrash") || mode_is("bye")) {
        g_passed = wait_word(up, 0, 0, 1000);
        if (!g_passed) fprintf(stderr, "%s: the link did not drop at once\n", g_mode);
    } else if (mode_is("silent")) {
        g_passed = wait_word(up, 0, 0, 4000);
        if (!g_passed) fprintf(stderr, "silent: a listen end that stopped answering was not declared down\n");
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
    if (mode_is("hang") || mode_is("sigterm")) {
        sleep(1);
        g_passed = 1;
        if (mode_is("hang")) command("SHUTDOWN");
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

static int find_loopback(void) {
    struct ifaddrs *all = NULL;
    if (getifaddrs(&all)) return 0;
    for (struct ifaddrs *a = all; a && !g_lo[0]; a = a->ifa_next)
        if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET6 && (a->ifa_flags & IFF_LOOPBACK) &&
            IN6_IS_ADDR_LINKLOCAL(&((struct sockaddr_in6 *)(void *)a->ifa_addr)->sin6_addr))
            snprintf(g_lo, sizeof(g_lo), "%s", a->ifa_name);
    freeifaddrs(all);
    return g_lo[0] != 0;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    g_mode = argv[1];
    setvbuf(stderr, NULL, _IOLBF, 0);
    if (!find_loopback()) return 77;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    snprintf(g_name, sizeof(g_name), "t%dc", (int)getpid());
    snprintf(g_sock, sizeof(g_sock), "/tmp/rpcd-test-%d.sock", (int)getpid());
    setenv("MCDMA_RPCD_SOCKET", g_sock, 1);
    g_peerbuf = calloc(1, 8u << 20);
    if (xchg_open(&g_fake, g_lo, 0, 0, g_name)) return 2;
    pthread_t listener, user, ctl;
    pthread_create(&listener, NULL, fake_listen, NULL);
    if (!mode_is("hang")) pthread_create(&user, NULL, client, NULL);
    pthread_create(&ctl, NULL, controller, NULL);
    char spec[160];
    snprintf(spec, sizeof(spec), "%s,%s,%d,stub0,0,4096", g_name, g_lo, g_fake.port);
    char *specs[1] = {spec};
    /* pull mode keeps a call pending inside the daemon, which is what hang, sigterm and pullcrash need */
    int direct = mode_is("race") || mode_is("bye") || mode_is("silent");
    int rc = run_connect(1, specs, direct);
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

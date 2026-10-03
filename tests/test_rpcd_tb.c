/* Offline end-to-end test of mcdma-rpcd over Thunderbolt: both daemons and rpc-echo's service and client in one
 * process, the stub's TN3205 devices tb0 and tb1 underneath, and the setup exchange on the loopback's link-local
 * address. Calls from 0 bytes to a whole request half, across the 12 MiB registration boundary, must come back byte
 * for byte. Exit 0 means pass, 77 that loopback has no link-local address. */
#include "../rpc/rpcd.h"

#include <ifaddrs.h>
#include <net/if.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define HALF (16ull << 20)
#define CALLS 120

int echo_service(const char *name);
int echo_client(const char *name, uint32_t calls, const char *sizes);

static char g_lo[16], g_name[32];
static int g_port;
static volatile int g_failed, g_done;

static void fail(const char *what) {
    fprintf(stderr, "test_rpcd_tb: %s\n", what);
    g_failed = 1;
}

static void *listen_end(void *arg) {
    static struct owner nobody;
    (void)arg;
    if (run_listen(g_name, "tb1", 0, 4096, g_lo, g_port, HALF, HALF, &nobody)) fail("the listen daemon failed");
    return NULL;
}

static void *service(void *arg) {
    char sock[160];
    struct stat st;
    (void)arg;
    snprintf(sock, sizeof(sock), "%s/mcdma-rpcd.%s.sock", MCDMA_RPC_SOCK_DIR, g_name);
    for (int i = 0; i < 1000 && stat(sock, &st); ++i) usleep(10000);
    if (echo_service(g_name)) fail("the echo service could not attach");
    return NULL;
}

static void *client(void *arg) {
    (void)arg;
    /* fixed edge sizes first, then sizes that wander across the whole half */
    if (echo_client(g_name, CALLS, "0,1,4064,4065,8191,16773120,13631493,9437187,3,12582912,777777"))
        fail("the echo client saw a wrong or missing reply");
    g_done = 1;
    return NULL;
}

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

/* Once the client is done, SHUTDOWN on the connect socket stops both daemons, which share this process's flag. */
static void *watcher(void *arg) {
    uint64_t deadline = now_ns() + 120000000000ull;
    while (!g_done && now_ns() < deadline) usleep(10000);
    if (!g_done) fail("the test ran past two minutes");
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", (const char *)arg);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) || send(fd, "SHUTDOWN\n", 9, 0) != 9) g_stop = 1;
    if (fd >= 0) close(fd);
    return NULL;
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

int main(void) {
    setvbuf(stderr, NULL, _IOLBF, 0);
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!find_loopback()) return 77;
    /* as the daemon's own main does: a peer that closed its socket must not kill the process */
    signal(SIGPIPE, SIG_IGN);
    unsetenv("MCDMA_RPCD_SOCKET");
    snprintf(g_name, sizeof(g_name), "tb%d", (int)getpid());
    g_port = free_port();
    char spec[160], sock[160];
    snprintf(spec, sizeof(spec), "%s,%s,%d,tb0,0,4096,%llu,%llu", g_name, g_lo, g_port, HALF >> 20, HALF >> 20);
    snprintf(sock, sizeof(sock), "%s/mcdma-rpcd.sock", MCDMA_RPC_SOCK_DIR);
    pthread_t lt, st, ct, wt;
    pthread_create(&lt, NULL, listen_end, NULL);
    pthread_create(&st, NULL, service, NULL);
    pthread_create(&ct, NULL, client, NULL);
    pthread_create(&wt, NULL, watcher, sock);
    char *specs[1] = {spec};
    int rc = run_connect(1, specs, 1);
    g_done = 1;
    pthread_join(ct, NULL);
    pthread_join(st, NULL);
    pthread_join(lt, NULL);
    pthread_join(wt, NULL);
    if (rc || g_failed) return 1;
    printf("test_rpcd_tb: %d calls over thunderbolt ok\n", CALLS);
    return 0;
}

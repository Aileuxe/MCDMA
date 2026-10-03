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

int fabric_check(int argc, char **argv);

struct rank {
    char *argv[11];
    char port[8], peer[8], name[8];
    int status;
};

static void *run(void *arg) {
    struct rank *r = arg;
    r->status = fabric_check(11, r->argv);
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

int main(int argc, char **argv) {
    static char lo[16];
    struct ifaddrs *all = NULL;
    if (argc != 2 || (strcmp(argv[1], "tb") && strcmp(argv[1], "roce"))) return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!getifaddrs(&all)) {
        for (struct ifaddrs *a = all; a && !lo[0]; a = a->ifa_next)
            if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET6 && (a->ifa_flags & IFF_LOOPBACK) &&
                IN6_IS_ADDR_LINKLOCAL(&((struct sockaddr_in6 *)(void *)a->ifa_addr)->sin6_addr))
                snprintf(lo, sizeof(lo), "%s", a->ifa_name);
        freeifaddrs(all);
    }
    if (!lo[0]) return 77;
    int tb = !strcmp(argv[1], "tb"), ports[2] = {free_port(), free_port()};
    static struct rank r[2];
    pthread_t t[2];
    for (int i = 0; i < 2; ++i) {
        snprintf(r[i].port, sizeof(r[i].port), "%d", ports[i]);
        snprintf(r[i].peer, sizeof(r[i].peer), "%d", ports[!i]);
        snprintf(r[i].name, sizeof(r[i].name), "%d", i);
        char *args[11] = {"fabric-check", tb ? (i ? "tb1" : "tb0") : (i ? "roce1" : "roce0"), "0", lo, r[i].port, "check",
                          r[i].name, "40", "64,4096,14336,1048576,13631491", "0.2", r[i].peer};
        memcpy(r[i].argv, args, sizeof(args));
        pthread_create(&t[i], NULL, run, &r[i]);
    }
    for (int i = 0; i < 2; ++i) pthread_join(t[i], NULL);
    return r[0].status || r[1].status;
}

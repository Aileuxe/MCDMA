/* Offline run of mesh-check: four nodes in one process, each on three of the stub's Thunderbolt devices, a full mesh
 * of six links met over the loopback's link-local address. Exit 0 means every node passed with the same hash, 77 that
 * loopback has no link-local address. */
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int mesh_check(int argc, char **argv);

struct node {
    char args[9][96];
    char *argv[9];
    int status;
};

static void *run(void *arg) {
    struct node *n = arg;
    n->status = mesh_check(9, n->argv);
    return NULL;
}

int main(int argc, char **argv) {
    static char lo[16];
    static struct node nodes[4];
    struct ifaddrs *all = NULL;
    const char *rows = argc > 1 ? argv[1] : "1,4,16";
    const char *seconds = argc > 2 ? argv[2] : "1";
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!getifaddrs(&all)) {
        for (struct ifaddrs *a = all; a && !lo[0]; a = a->ifa_next)
            if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET6 && (a->ifa_flags & IFF_LOOPBACK) &&
                IN6_IS_ADDR_LINKLOCAL(&((struct sockaddr_in6 *)(void *)a->ifa_addr)->sin6_addr))
                snprintf(lo, sizeof(lo), "%s", a->ifa_name);
        freeifaddrs(all);
    }
    if (!lo[0]) return 77;
    /* both ends of a link share this process, so each end binds its own port: node a's end of a-b is base + 16a + b */
    int base = 20000 + (int)(getpid() % 20000);
    for (int a = 0; a < 4; ++a) {
        struct node *n = &nodes[a];
        snprintf(n->args[0], 96, "mesh-check");
        snprintf(n->args[1], 96, "%d", a);
        snprintf(n->args[2], 96, "0");
        snprintf(n->args[3], 96, "%d", base);
        snprintf(n->args[4], 96, "%s", seconds);
        snprintf(n->args[5], 96, "%s", rows);
        for (int k = 0, b = 0; b < 4; ++b) {
            if (b == a) continue;
            /* node a's three ports are stub devices tb(3a) to tb(3a + 2) */
            snprintf(n->args[6 + k], 96, "tb%d:%s:%d:%d:%d", 3 * a + k, lo, b, base + 16 * a + b, base + 16 * b + a);
            k++;
        }
        for (int i = 0; i < 9; ++i) n->argv[i] = n->args[i];
    }
    pthread_t t[4];
    for (int a = 0; a < 4; ++a) pthread_create(&t[a], NULL, run, &nodes[a]);
    int failed = 0;
    for (int a = 0; a < 4; ++a) {
        pthread_join(t[a], NULL);
        failed |= nodes[a].status;
    }
    return failed ? 1 : 0;
}

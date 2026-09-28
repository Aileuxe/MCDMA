/* mcdma-rpcd: request/reply transport between applications on a Mac and a Linux peer over MCDMA RDMA.
 *
 * Applications never open a verbs context. Each talks to its local daemon through a shared-memory mailbox, and
 * only the daemons hold queue pairs, so an application that crashes or is killed cannot leave a QP behind. One
 * source builds on both ends: Linux libibverbs on the peer, Apple librdma plus the MCDMA provider on the Mac.
 *
 *   mcdma-rpcd listen [--owner USER] NAME DEVICE GID_INDEX PATH_MTU [ADDR:]PORT [REQ_MIB REP_MIB]
 *       The Linux end of one link. Mailbox /dev/shm/mcdma-rpc.NAME; TCP PORT takes one control connection from the
 *       connect end at a time, on every address unless ADDR names one; the Unix socket /tmp/mcdma-rpcd.NAME.sock
 *       takes one service ("MODE poll") and also answers STATUS and SHUTDOWN. Whoever reaches the control port can
 *       write into the mailbox, so bind it to the address the Mac connects to and let only the Mac through the
 *       firewall. Started as root, --owner gives the mailbox and socket to USER, whose services then attach without
 *       root.
 *   mcdma-rpcd connect [--buffer-fd FD [--parent-fd FD]] PEER...
 *       The Mac end. PEER = name,host,port,device,gid_index,path_mtu[,req_mib,rep_mib]
 *       e.g. worker-a,192.0.2.21,18620,rdma_mcrdma0,0,4096,4,64
 *       One POSIX shared memory mailbox /mcdma-rpc.NAME and one thread per peer. The Unix socket
 *       /tmp/mcdma-rpcd.sock answers STATUS and SHUTDOWN (tears every verbs object down, then exits).
 *       Linux-only --buffer-fd imports one caller-exported DMA-BUF for one peer,
 *       without creating a POSIX mailbox or touching payload bytes. The caller
 *       must supply coherent CPU/GPU memory and finish GPU work before publishing
 *       requests. Only control pages are initialized; no GPU driver is loaded.
 *       --parent-fd is a distinct read-only FIFO lease; parent EOF stops the
 *       daemon normally, and unexpected data/errors stop it with a failure.
 *       FD mode accepts one Unix control command per connection, with an
 *       absolute five-second deadline and parent-lease polling throughout.
 *   mcdma-rpcd version
 *
 * MCDMA_RPCD_SOCKET replaces the Unix socket path. Mailboxes and sockets are owner-only: run the daemon as the user
 * whose applications use it. NAME is 1-20 characters of [A-Za-z0-9_-].
 *
 * Mailbox (protocol 1): request half [0, R), reply half [R, R + P); R and P are whole multiples of 4 MiB (default
 * 4 MiB each) and must match on both ends. Each half starts with a 4 KiB control page. A word is (seq << 32 | len);
 * seq 0 means empty. Payloads are written before their word.
 *   request half +0    connect end: client -> daemon, a request is staged. listen end: the request has landed
 *                +64   connect end: daemon -> client, 1 while the link to the peer is up
 *                +72   connect end: link generation, bumped each time the link comes up; a request staged before a
 *                      reconnect is lost, and a changed generation tells the waiting client so
 *                +256  R and P (u64 each), so applications know the layout
 *   reply half   +0    connect end: the peer's ready word (pull mode). listen end: the word it sends
 *                +64   connect end: the client's done word (written by the peer in direct mode)
 *                +128  listen end: service -> daemon, a reply is staged
 * The Mac registers each 4 MiB of its mailbox as its own MR (the largest registration the MCDMA provider has been
 * validated with) and hands the peer the table of its reply-half segments; every transfer is cut at those
 * boundaries.
 *
 * One call: the client stages the request and sets its word; the connect daemon WRITEs payload, then word, into the
 * peer's request half; the peer's service sees the word, computes and stages its reply; in direct mode (the
 * default) the listen daemon WRITEs the reply and then the client's done word straight into the Mac's reply half.
 * That relies on the Mac's NIC not reordering its PCIe writes (MCDMARelaxedOrdering = No, the default);
 * MCDMA_RPC_PULL=1 makes the peer send only a ready word and the connect daemon READ the payload instead.
 *
 * Services: a connection to the listen socket sends one line within five seconds. "MODE poll" registers the service,
 * answered "OK", or "ERR busy" while another service is registered; after "OK" the daemon sends nothing until the
 * registration ends, then "BYE" or a closed socket. The registration ends when the link drops, because requests in
 * flight are lost with it. "STATUS" and "SHUTDOWN" work on both sockets.
 *
 * STATUS answers "VERSION mcdma-rpcd 1 RELEASE", one line per peer and "END". The connect end reports
 *   PEER NAME up|down calls N failures N MiB N host=HOST port=PORT device=DEVICE req_mib=R rep_mib=P since=EPOCH
 * and the listen end reports its one link with device=, req_mib=, rep_mib=, since= and service=attached|none.
 *
 * Safety. The connect end takes its QP past INIT only after the listen end has answered HELLO from RTS, posts
 * nothing before its own RTS, gives up on a peer by destroying the QP inside a live process (never by exiting),
 * waits out every posted work request before destroying anything, and tears every object down on every exit path.
 * Every socket wait is bounded, so shutdown never hangs on a silent peer. One daemon serves each link name: a lock
 * file in /tmp is taken before any device or mailbox is touched, and a second daemon refuses to start. SHUTDOWN,
 * SIGINT and SIGTERM all stop it the same orderly way; SIGHUP is ignored, and SIGKILL, which skips the teardown,
 * must never be used. Stop the connect end before restarting any listen end.
 */
#include "rpcd.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PORT or ADDR:PORT for the listen end's control socket; ADDR must be a dotted IPv4 address. */
static int parse_bind(const char *text, struct in_addr *addr, int *port) {
    const char *colon = strrchr(text, ':');
    addr->s_addr = htonl(INADDR_ANY);
    if (colon) {
        char host[INET_ADDRSTRLEN];
        size_t n = (size_t)(colon - text);
        if (n == 0 || n >= sizeof(host)) return -1;
        memcpy(host, text, n);
        host[n] = 0;
        if (inet_pton(AF_INET, host, addr) != 1) return -1;
        text = colon + 1;
    }
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (!*text || *end || value <= 0 || value > 65535) return -1;
    *port = (int)value;
    return 0;
}

static int usage(void) {
    fprintf(stderr, "usage: mcdma-rpcd listen [--owner USER] NAME DEVICE GID_INDEX PATH_MTU [ADDR:]PORT [REQ_MIB REP_MIB]\n"
                    "       mcdma-rpcd connect [--buffer-fd FD [--parent-fd FD]] name,host,port,device,gid_index,path_mtu[,req_mib,rep_mib] ...\n"
                    "       mcdma-rpcd version\n");
    return 2;
}

int main(int argc, char **argv) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    /* a closed terminal or SSH session must not stop a daemon that holds queue pairs */
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stderr, NULL, _IOLBF, 0);
    if (argc == 2 && !strcmp(argv[1], "version")) {
        printf("mcdma-rpcd %s protocol %d\n", RELEASE, PROTOCOL);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "listen")) {
        struct owner owner = {0};
        char **arg = argv + 2;
        int left = argc - 2;
        if (left >= 2 && !strcmp(arg[0], "--owner")) {
            struct passwd *user = getpwnam(arg[1]);
            if (!user) {
                fprintf(stderr, "unknown user %s\n", arg[1]);
                return 2;
            }
            owner = (struct owner){1, user->pw_uid, user->pw_gid};
            arg += 2;
            left -= 2;
        }
        if (left != 5 && left != 7) return usage();
        uint64_t req = 4 << 20, rep = 4 << 20;
        if (!valid_name(arg[0])) {
            fprintf(stderr, "NAME must be 1-20 characters of [A-Za-z0-9_-]\n");
            return 2;
        }
        if (left == 7 && (parse_mib((unsigned)strtoul(arg[5], NULL, 10), &req) ||
                          parse_mib((unsigned)strtoul(arg[6], NULL, 10), &rep))) {
            fprintf(stderr, "mailbox halves must be multiples of 4 MiB up to %d MiB\n", 4 * MAX_SEGS);
            return 2;
        }
        struct in_addr bind_addr;
        int port = 0;
        if (parse_bind(arg[4], &bind_addr, &port)) return usage();
        return run_listen(arg[0], arg[1], atoi(arg[2]), atoi(arg[3]), bind_addr, port, req, rep, &owner);
    }
    if (argc >= 3 && !strcmp(argv[1], "connect")) {
        const char *pull = getenv("MCDMA_RPC_PULL");
        char **arg = argv + 2;
        int left = argc - 2, buffer_fd = -1, parent_fd = -1;
        while (left >= 2 && (!strcmp(arg[0], "--buffer-fd") || !strcmp(arg[0], "--parent-fd"))) {
            int *destination = !strcmp(arg[0], "--buffer-fd") ? &buffer_fd : &parent_fd;
            if (*destination >= 0) return usage();
            char *end = NULL;
            errno = 0;
            long fd = strtol(arg[1], &end, 10);
            if (errno || !*arg[1] || *end || fd < 3 || fd > INT_MAX) return usage();
            *destination = (int)fd;
            arg += 2; left -= 2;
        }
        if (left < 1 || (parent_fd >= 0 && buffer_fd < 0) || (buffer_fd >= 0 && left != 1)) return usage();
        return run_connect_with_fds(left, arg, !(pull && !strcmp(pull, "1")), buffer_fd, parent_fd);
    }
    return usage();
}

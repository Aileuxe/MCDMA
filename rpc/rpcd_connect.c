/* mcdma-rpcd connect: one mailbox and one thread per peer link. */
#include "rpcd.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef MCDMA_RPC_CONTROL_SESSION_S
#define MCDMA_RPC_CONTROL_SESSION_S IO_TIMEOUT_S
#endif

struct peer {
    char name[32], host[128], device[64];
    int port, gid_index, mtu;
    struct ep e;
    struct box b;
    int fd;
    int external_buffer;             /* caller-exported Linux DMA-BUF; no POSIX shm name exists */
    int buffer_fd;                   /* owned duplicate, kept past every MR deregistration */
    struct reader rd;
    int up;                        /* read by the control thread through __atomic loads */
    long long since;
    uint32_t remote_rkey;
    uint64_t remote_base;
    uint64_t calls, failures, bytes;
    uint64_t generation;           /* times the link has come up, published at request half +72 */
    struct piece pieces[4 * MAX_SEGS + 4];
    pthread_t thread;
};

static struct peer g_peers[MAX_PEERS];
static int g_npeers;
/* Direct replies need the Mac NIC to place a WRITE's payload before the next WRITE's word, i.e. relaxed ordering
   off (MCDMARelaxedOrdering = No); MCDMA_RPC_PULL=1 falls back to READing the payload. */
static int g_direct = 1;

static void box_shm_name(const struct peer *p, char *out, size_t n) { snprintf(out, n, "/mcdma-rpc.%.*s", (int)sizeof(p->name), p->name); }

static int box_create(struct peer *p) {
    uint64_t total = p->b.req + p->b.rep;
    if (p->external_buffer) {
#ifdef MCDMA_RPC_HAS_DMABUF
        off_t size = lseek(p->buffer_fd, 0, SEEK_END);
        if (size < 0 || (uint64_t)size < total) {
            logf_("%s: DMA-BUF is not seekable or is smaller than the mailbox; no fallback", p->name);
            return -1;
        }
        p->b.base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, p->buffer_fd, 0);
        if (p->b.base == MAP_FAILED) {
            p->b.base = NULL;
            logf_("%s: DMA-BUF shared mapping failed (%d); no fallback", p->name, errno);
            return -1;
        }
        for (uint64_t off = 0; off < total; off += SEG) {
            int access = off < p->b.req ? IBV_ACCESS_LOCAL_WRITE : IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
            struct ibv_mr *mr = ep_reg_dmabuf(&p->e, p->buffer_fd, off, p->b.base + off, SEG, access);
            if (!mr) {
                logf_("%s: DMA-BUF segment %" PRIu64 " registration failed (%d); no fallback", p->name, (uint64_t)(off / SEG), errno);
                return -1;
            }
            p->b.seg[p->b.nseg++] = mr;
        }
        /* Register first: the kernel must accept a genuine DMA-BUF before we
         * write anything into a caller-supplied descriptor. Only control pages
         * belong to the daemon; the application owns every payload byte. */
        memset(p->b.base, 0, CTRL);
        memset(p->b.base + p->b.req, 0, CTRL);
        write_sizes(&p->b);
        logf_("%s: DMA_BUF_MAILBOX bytes=%" PRIu64 " segments=%d control_bytes=%u payload_initialization=0",
              p->name, total, p->b.nseg, (unsigned)(2 * CTRL));
        return 0;
#else
        logf_("DMA-BUF mailboxes require Linux; no fallback");
        return -1;
#endif
    }
    char name[64];
    box_shm_name(p, name, sizeof(name));
    shm_unlink(name);
    int f = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (f < 0 || ftruncate(f, (off_t)total)) {
        logf_("shm %s errno=%d", name, errno);
        if (f >= 0) close(f);
        return -1;
    }
    p->b.base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
    close(f);
    if (p->b.base == MAP_FAILED) {
        p->b.base = NULL;
        return -1;
    }
    long page = sysconf(_SC_PAGESIZE);
    for (uint64_t off = 0; off < total; off += (uint64_t)page) p->b.base[off] = 0;
    for (uint64_t off = 0; off < total; off += SEG) {
        int access = off < p->b.req ? IBV_ACCESS_LOCAL_WRITE : IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
        if (!(p->b.seg[p->b.nseg++] = ep_reg(&p->e, p->b.base + off, SEG, access))) {
            logf_("%s: registering mailbox segment %" PRIu64 " failed", p->name, (uint64_t)(off / SEG));
            return -1;
        }
    }
    write_sizes(&p->b);
    return 0;
}

static void peer_down(struct peer *p, const char *why) {
    if (__atomic_load_n(&p->up, __ATOMIC_ACQUIRE)) logf_("%s: down (%s)", p->name, why);
    __atomic_store_n(&p->up, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&p->since, 0, __ATOMIC_RELAXED);
    if (p->b.base) store_word((volatile uint64_t *)(p->b.base + 64), 0);
    ep_destroy_qp(&p->e);                   /* waits out anything still posted first */
    if (p->fd >= 0) {
        close(p->fd);
        p->fd = -1;
    }
}

/* Bring the link up; `last` receives the request word as it stood before clients could see the link up. */
static int peer_connect(struct peer *p, uint32_t *last) {
    /* every step is bounded, so SHUTDOWN is never stuck behind a peer that accepts and then stays silent */
    int fd = tcp_connect(p->host, p->port, IO_TIMEOUT_S);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    set_io_timeout(fd, IO_TIMEOUT_S);
    set_keepalive(fd);
    p->fd = fd;
    memset(&p->rd, 0, sizeof(p->rd));
    p->rd.fd = fd;
    if (ep_create_qp(&p->e)) {
        peer_down(p, "qp");
        return -1;
    }
    char mine[80], gid[80];
    gid_string(&p->e.gid, mine, sizeof(mine));
    char *line = malloc(LINE);
    if (!line) {
        peer_down(p, "memory");
        return -1;
    }
    int at = snprintf(line, LINE, "HELLO %d %u %u %s %s %" PRIu64 " %" PRIu64 " %d", PROTOCOL, p->e.qp->qp_num, PSN,
                      mine, g_direct ? "DIRECT" : "PULL", p->b.req, p->b.rep, (int)(p->b.rep / SEG));
    int first_rep = (int)(p->b.req / SEG);
    for (int i = first_rep; i < p->b.nseg && at < LINE - 64; ++i)
        at += snprintf(line + at, (size_t)(LINE - at), " %u %llu", p->b.seg[i]->rkey,
                       (unsigned long long)(uintptr_t)(p->b.base + (uint64_t)i * SEG));
    unsigned qpn, psn, rkey;
    unsigned long long raddr;
    int handshake = send_line(fd, line) == 0 && take_line(&p->rd, line, LINE, 1) == 1 &&
                    sscanf(line, "HELLO %u %u %79s %u %llu", &qpn, &psn, gid, &rkey, &raddr) == 5;
    if (!handshake) {
        /* the peer refused or failed: our QP never left INIT, nothing was posted */
        logf_("%s: handshake refused (%.200s)", p->name, line);
        free(line);
        peer_down(p, "handshake");
        return -1;
    }
    free(line);
    if (ep_connect(&p->e, qpn, psn, gid)) {
        peer_down(p, "rtr/rts");
        return -1;
    }
    p->remote_rkey = rkey;
    p->remote_base = raddr;
    store_word((volatile uint64_t *)p->b.base, 0);
    store_word((volatile uint64_t *)(p->b.base + p->b.req), 0);
    store_word((volatile uint64_t *)(p->b.base + p->b.req + 64), 0);
    send_line(fd, "READY");
    /* taken before clients can see the link up, so a request staged the moment they do is still sent */
    *last = WORD_SEQ(load_word((volatile uint64_t *)p->b.base));
    __atomic_store_n(&p->since, (long long)time(NULL), __ATOMIC_RELAXED);
    __atomic_store_n(&p->up, 1, __ATOMIC_RELEASE);
    /* a new generation tells a waiting client that a request staged before the reconnect was lost */
    store_word((volatile uint64_t *)(p->b.base + 72), ++p->generation);
    store_word((volatile uint64_t *)(p->b.base + 64), 1);
    logf_("%s: connected (%s qpn %u <-> %u, mailbox %" PRIu64 " + %" PRIu64 " MiB, %s replies)", p->name, p->device,
          p->e.qp->qp_num, qpn, p->b.req >> 20, p->b.rep >> 20, g_direct ? "direct" : "pull");
    return 0;
}

static void *peer_thread(void *arg) {
    struct peer *p = arg;
    volatile uint64_t *req = (volatile uint64_t *)p->b.base;
    volatile uint64_t *ready = (volatile uint64_t *)(p->b.base + p->b.req);
    volatile uint64_t *done = (volatile uint64_t *)(p->b.base + p->b.req + 64);
    uint32_t last = 0;
    uint64_t active = now_ns();
    unsigned idle = 0;
    char *line = malloc(LINE);
    while (!g_stop && line) {
        if (!__atomic_load_n(&p->up, __ATOMIC_ACQUIRE)) {
            if (peer_connect(p, &last)) {
                for (int i = 0; i < 20 && !g_stop; ++i) usleep(100000);
                continue;
            }
        }
        uint64_t w = load_word(req);
        uint32_t seq = WORD_SEQ(w), len = WORD_LEN(w);
        if (!seq || seq == last) {
            if (now_ns() - active > 50000000ull)
                usleep(20);
            else
                cpu_relax();
            if (p->fd >= 0 && ++idle >= 4096) {   /* notice a peer that went away while idle */
                idle = 0;
                if (take_line(&p->rd, line, LINE, 0) < 0) peer_down(p, "control closed");
            }
            continue;
        }
        last = seq;
        active = now_ns();
        if (len > p->b.req - CTRL) {
            store_word(done, WORD(seq, 0));
            continue;
        }
        /* payload first, then the word: RC places the WRITEs in order on the peer */
        int n = cut(&p->b, CTRL, len, p->remote_base, p->remote_rkey, NULL, NULL, 0, SEG, p->pieces, 4 * MAX_SEGS);
        if (n >= 0) {
            p->pieces[n] = (struct piece){(void *)req, box_lkey(&p->b, 0), p->remote_base, p->remote_rkey, 8};
            n++;
        }
        uint64_t began = now_ns();
        if (n < 0 || post_pieces(&p->e, IBV_WR_RDMA_WRITE, p->pieces, n, WINDOW_MAC, 10000000000ull)) {
            __atomic_fetch_add(&p->failures, 1, __ATOMIC_RELAXED);
            peer_down(p, "request write");
            continue;
        }
        if (len >= BULK) log_rate(p->name, "request", len, now_ns() - began);
        __atomic_fetch_add(&p->bytes, len, __ATOMIC_RELAXED);
        if (g_direct) {   /* the peer writes the reply and the client's word itself */
            __atomic_fetch_add(&p->calls, 1, __ATOMIC_RELAXED);
            continue;
        }
        uint64_t start = now_ns(), r = 0;
        unsigned spins = 0;
        int closed = 0;
        while (!g_stop && WORD_SEQ(r = load_word(ready)) != seq) {
            cpu_relax();
            if (++spins == 4096) {
                spins = 0;
                if (now_ns() - start > 30000000000ull) break;
                if (take_line(&p->rd, line, LINE, 0) < 0) {
                    closed = 1;
                    break;
                }
            }
        }
        if (WORD_SEQ(r) != seq) {
            __atomic_fetch_add(&p->failures, 1, __ATOMIC_RELAXED);
            peer_down(p, closed ? "control closed during a call" : g_stop ? "shutdown during a call" : "no reply in 30 s");
            continue;
        }
        uint32_t rlen = WORD_LEN(r);
        /* the length comes from the peer: never let it reach past the reply half */
        if (rlen > p->b.rep - CTRL) {
            __atomic_fetch_add(&p->failures, 1, __ATOMIC_RELAXED);
            peer_down(p, "reply longer than the reply half");
            continue;
        }
        /* READ the reply one piece at a time (the Mac provider allows one outstanding READ per QP) */
        n = cut(&p->b, p->b.req + CTRL, rlen, p->remote_base, p->remote_rkey, NULL, NULL, 0, READ_CHUNK, p->pieces,
                4 * MAX_SEGS);
        if (n < 0 || post_pieces(&p->e, IBV_WR_RDMA_READ, p->pieces, n, 1, 10000000000ull)) {
            __atomic_fetch_add(&p->failures, 1, __ATOMIC_RELAXED);
            peer_down(p, "reply read");
            continue;
        }
        store_word(done, WORD(seq, rlen));
        __atomic_fetch_add(&p->calls, 1, __ATOMIC_RELAXED);
    }
    free(line);
    return NULL;
}

static int parse_peer(const char *spec, struct peer *p) {
    memset(p, 0, sizeof(*p));
    p->fd = -1;
    p->buffer_fd = -1;
    unsigned req_mib = 4, rep_mib = 4;
    int n = sscanf(spec, "%31[^,],%127[^,],%d,%63[^,],%d,%d,%u,%u", p->name, p->host, &p->port, p->device,
                   &p->gid_index, &p->mtu, &req_mib, &rep_mib);
    if ((n != 6 && n != 8) || !valid_name(p->name) || p->port <= 0 || p->port > 65535) return -1;
    return parse_mib(req_mib, &p->b.req) || parse_mib(rep_mib, &p->b.rep) ? -1 : 0;
}

static void connect_status(int fd) {
    char out[512];
    snprintf(out, sizeof(out), "VERSION mcdma-rpcd %d %s", PROTOCOL, RELEASE);
    send_line(fd, out);
    for (int i = 0; i < g_npeers; ++i) {
        struct peer *p = &g_peers[i];
        int up = __atomic_load_n(&p->up, __ATOMIC_ACQUIRE);
        snprintf(out, sizeof(out),
                 "PEER %.*s %s calls %" PRIu64 " failures %" PRIu64 " MiB %" PRIu64
                 " host=%.*s port=%d device=%.*s req_mib=%" PRIu64 " rep_mib=%" PRIu64 " since=%lld%s",
                 (int)sizeof(p->name), p->name, up ? "up" : "down", __atomic_load_n(&p->calls, __ATOMIC_RELAXED),
                 __atomic_load_n(&p->failures, __ATOMIC_RELAXED), __atomic_load_n(&p->bytes, __ATOMIC_RELAXED) >> 20,
                 (int)sizeof(p->host), p->host, p->port, (int)sizeof(p->device), p->device, p->b.req >> 20, p->b.rep >> 20,
                 up ? __atomic_load_n(&p->since, __ATOMIC_RELAXED) : 0,
                 p->external_buffer ? " memory=dmabuf" : "");
        send_line(fd, out);
    }
    send_line(fd, "END");
}

/* Remove the mailboxes of the first `count` peers after a failed start or at exit. */
static void unlink_boxes(int count) {
    for (int i = 0; i < count; ++i) {
        if (g_peers[i].external_buffer) continue;
        char name[64];
        box_shm_name(&g_peers[i], name, sizeof(name));
        shm_unlink(name);
    }
}

static int release_boxes(int count) {
    int failed = 0;
    for (int i = 0; i < count; ++i) {
        struct peer *p = &g_peers[i];
        if (p->b.base) {
            if (munmap(p->b.base, p->b.req + p->b.rep)) failed = 1;
            else p->b.base = NULL;
        }
        if (p->buffer_fd >= 0) {
            if (close(p->buffer_fd)) failed = 1;
            p->buffer_fd = -1;
        }
    }
    if (failed) logf_("mailbox mapping/descriptor cleanup failed");
    return failed;
}

/* Readiness is the complete lease signal. Never read a borrowed FIFO: another
 * reader could consume a ready byte before read(), turning shutdown into a
 * blocking wait. Only bare HUP denotes an ordinary parent exit. */
static int parent_lease_event(int fd, short events, int *failed) {
#if defined(MCDMA_RPC_TEST_DMABUF) && defined(__APPLE__)
    /* The optional transport is Linux-only; the Mac test double translates
     * Darwin's EOF POLLIN|POLLHUP into Linux's bare POLLHUP without reading. */
    extern short mcdma_rpc_test_parent_events(int fd, short events);
    events = mcdma_rpc_test_parent_events(fd, events);
#else
    (void)fd;
#endif
    if (!events) return 0;
    *failed = events != POLLHUP;
    logf_(*failed ? "parent lease received unexpected data or failed; shutting down"
                 : "parent lease closed; shutting down");
    g_stop = 1;
    return 1;
}

/* FD-backed sessions accept one command, with an absolute deadline independent
 * of partial input. The accepted socket is ours, so making it nonblocking does
 * not alter either caller-owned FD's shared file flags. Responses are also
 * nonblocking; a client that will not read its answer is simply disconnected. */
static int take_fd_control_line(struct reader *reader, char *line, size_t n, int lease_fd, int *lease_failed) {
    int flags = fcntl(reader->fd, F_GETFL);
    if (flags < 0 || fcntl(reader->fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    uint64_t deadline = now_ns() + (uint64_t)MCDMA_RPC_CONTROL_SESSION_S * 1000000000ull;
    while (!g_stop) {
        uint64_t now = now_ns();
        if (now >= deadline) {
            logf_("FD-mode control session deadline expired");
            return -1;
        }
        uint64_t remaining_ms = (deadline - now + 999999) / 1000000;
        int timeout_ms = remaining_ms < 500 ? (int)remaining_ms : 500;
        struct pollfd wait[2] = {{.fd = reader->fd, .events = POLLIN}, {.fd = lease_fd, .events = POLLIN}};
        int polled = poll(wait, lease_fd >= 0 ? 2 : 1, timeout_ms);
        if (polled < 0) {
            if (errno == EINTR) continue;
            *lease_failed = 1;
            g_stop = 1;
            logf_("FD-mode control/parent polling failed (%d)", errno);
            return -1;
        }
        if (lease_fd >= 0 && parent_lease_event(lease_fd, wait[1].revents, lease_failed)) return -1;
        if (wait[0].revents & (POLLERR | POLLNVAL)) return -1;
        if (wait[0].revents & (POLLIN | POLLHUP)) {
            int got = take_line(reader, line, n, 0);
            if (got != 0) return got;
        }
    }
    return -1;
}

int run_connect_with_fds(int npeers, char **specs, int direct, int buffer_fd, int parent_fd) {
    if (npeers < 1 || npeers > MAX_PEERS) {
        logf_("at most %d peers", MAX_PEERS);
        return 2;
    }
    if (buffer_fd >= 0) {
#ifndef MCDMA_RPC_HAS_DMABUF
        logf_("--buffer-fd requires Linux DMA-BUF support; no fallback");
        return 2;
#else
        if (npeers != 1 || buffer_fd < 3) {
            logf_("--buffer-fd requires one peer and a descriptor of at least 3");
            return 2;
        }
        if (fcntl(buffer_fd, F_GETFD) < 0) {
            logf_("--buffer-fd is not an open descriptor");
            return 2;
        }
#endif
    }
    if (parent_fd >= 0) {
        struct stat info;
        int flags = fcntl(parent_fd, F_GETFL);
        if (buffer_fd < 0 || npeers != 1 || parent_fd < 3 || parent_fd == buffer_fd ||
            flags < 0 || (flags & O_ACCMODE) != O_RDONLY || fstat(parent_fd, &info) || !S_ISFIFO(info.st_mode)) {
            logf_("--parent-fd requires a distinct read-only FIFO and single-peer --buffer-fd mode");
            return 2;
        }
    }
    g_direct = direct;
    for (int i = 0; i < npeers; ++i) {
        if (parse_peer(specs[i], &g_peers[i])) {
            logf_("bad peer %s", specs[i]);
            return 2;
        }
        for (int j = 0; j < i; ++j)
            if (!strcmp(g_peers[j].name, g_peers[i].name)) {
                logf_("peer name %s is used twice", g_peers[i].name);
                return 2;
            }
    }
    if (buffer_fd >= 0) g_peers[0].external_buffer = 1;
    if (buffer_fd >= 0) {
        off_t size = lseek(buffer_fd, 0, SEEK_END);
        if (size < 0 || (uint64_t)size < g_peers[0].b.req + g_peers[0].b.rep) {
            logf_("--buffer-fd is not seekable or is smaller than the mailbox");
            return 2;
        }
    }
    /* locks and the socket come before any device or mailbox, so a running daemon is never orphaned */
    for (int i = 0; i < npeers; ++i)
        if (hold_link_lock(g_peers[i].name) < 0) return 2;
    const char *sock_path = socket_path("/tmp/mcdma-rpcd.sock");
    if (socket_in_use(sock_path)) {
        logf_("another mcdma-rpcd is serving %s; stop it with SHUTDOWN first", sock_path);
        return 2;
    }
    int us = unix_listen(sock_path);
    if (us < 0) return 2;
    int lease_fd = -1;
    if (parent_fd >= 0) {
        lease_fd = fcntl(parent_fd, F_DUPFD_CLOEXEC, 3);
        if (lease_fd < 0) {
            logf_("--parent-fd descriptor duplication failed (%d)", errno);
            close(us);
            unlink(sock_path);
            return 2;
        }
    }
    if (buffer_fd >= 0) {
        g_peers[0].buffer_fd = fcntl(buffer_fd, F_DUPFD_CLOEXEC, 3);
        if (g_peers[0].buffer_fd < 0) {
            logf_("--buffer-fd descriptor duplication failed (%d)", errno);
            close(us);
            unlink(sock_path);
            if (lease_fd >= 0) close(lease_fd);
            return 2;
        }
    }
    for (int i = 0; i < npeers; ++i) {
        struct peer *p = &g_peers[i];
        g_npeers = i + 1;
        if (ep_open(&p->e, p->device, p->gid_index, p->mtu) || box_create(p)) {
            unlink_boxes(g_npeers);
            close(us);
            unlink(sock_path);
            teardown_all();
            release_boxes(g_npeers);
            if (lease_fd >= 0) close(lease_fd);
            return 2;
        }
    }
    int started = 0;
    while (started < g_npeers && !pthread_create(&g_peers[started].thread, NULL, peer_thread, &g_peers[started]))
        started++;
    if (started < g_npeers) {
        logf_("could not start the thread for %s", g_peers[started].name);
        g_stop = 1;
    } else {
        logf_("connect: %d peers, control socket %s", g_npeers, sock_path);
    }
    char *line = malloc(LINE);
    static struct reader rd;
    int lease_failed = 0;
    while (!g_stop && line) {
        /* a short tick, so a signal that lands on any thread still ends the loop promptly */
        struct pollfd wait[2] = {{.fd = us, .events = POLLIN}, {.fd = lease_fd, .events = POLLIN}};
        int polled = poll(wait, lease_fd >= 0 ? 2 : 1, 500);
        if (polled < 0 && errno != EINTR) {
            lease_failed = 1;
            logf_("connect control/parent polling failed (%d)", errno);
            break;
        }
        if (polled <= 0) continue;
        if (lease_fd >= 0 && parent_lease_event(lease_fd, wait[1].revents, &lease_failed)) break;
        if (!(wait[0].revents & POLLIN)) continue;
        int fd = accept(us, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }
        /* a silent client is dropped after a few seconds, so it can never hold SHUTDOWN off */
        set_io_timeout(fd, IO_TIMEOUT_S);
        memset(&rd, 0, sizeof(rd));
        rd.fd = fd;
        while (!g_stop) {
            int got = buffer_fd >= 0 ? take_fd_control_line(&rd, line, LINE, lease_fd, &lease_failed)
                                     : take_line(&rd, line, LINE, 1);
            if (got != 1) break;
            if (!strcmp(line, "STATUS")) {
                connect_status(fd);
            } else if (!strcmp(line, "SHUTDOWN")) {
                g_stop = 1;
                send_line(fd, "BYE");
                break;
            } else {
                send_line(fd, "ERR unknown command");
            }
            if (buffer_fd >= 0) break; /* one bounded command per FD-mode session */
        }
        close(fd);
    }
    g_stop = 1;
    for (int i = 0; i < started; ++i) pthread_join(g_peers[i].thread, NULL);
    for (int i = 0; i < g_npeers; ++i) peer_down(&g_peers[i], "shutdown");
    unlink_boxes(g_npeers);
    free(line);
    close(us);
    unlink(sock_path);
    teardown_all();
    int cleanup = release_boxes(g_npeers);
    if (lease_fd >= 0 && close(lease_fd)) cleanup = 1;
    if (cleanup || lease_failed) return 2;
    logf_("connect: every verbs object destroyed, exiting");
    return started < g_npeers ? 2 : 0;
}

int run_connect_with_buffer(int npeers, char **specs, int direct, int buffer_fd) {
    return run_connect_with_fds(npeers, specs, direct, buffer_fd, -1);
}

int run_connect(int npeers, char **specs, int direct) {
    return run_connect_with_buffer(npeers, specs, direct, -1);
}

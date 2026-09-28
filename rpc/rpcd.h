/* Shared parts of mcdma-rpcd: constants, the mailbox and verbs structures, and the helpers every module uses. */
#ifndef MCDMA_RPCD_H
#define MCDMA_RPCD_H

#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#else
#define _GNU_SOURCE 1
#endif
#include <infiniband/verbs.h>
#include <netinet/in.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* The test define exercises FD ownership with a fake verbs implementation on
 * any build host; it is never enabled by the production build. */
#if defined(__linux__) || defined(MCDMA_RPC_TEST_DMABUF)
#define MCDMA_RPC_HAS_DMABUF 1
#endif

#define PROTOCOL 1
#define RELEASE "1.0.0"
#ifndef MCDMA_RPC_BOX_DIR
#define MCDMA_RPC_BOX_DIR "/dev/shm"    /* listen-end mailboxes; offline tests move them */
#endif
#ifndef MCDMA_RPC_LOCK_DIR
#define MCDMA_RPC_LOCK_DIR "/tmp"       /* one lock file per link name */
#endif
#ifndef MCDMA_RPC_HANDSHAKE_S
#define MCDMA_RPC_HANDSHAKE_S 10        /* a control connection must finish HELLO and READY within this */
#endif
#define IO_TIMEOUT_S 5                  /* bound on any single blocking socket call */
#define PENDING_S 5                     /* a socket client must send its command within this */
#define PSN 0x3a41c5u
#define SEG (4ull << 20)            /* Mac MR size and mailbox unit */
#define CTRL 4096ull
#define READ_CHUNK (2ull << 20)     /* the Mac provider's proven READ size, one outstanding at a time */
#define MAX_SEGS 64                 /* per half: 256 MiB */
#define MAX_MRS (2 * MAX_SEGS + 2)
#define MAX_PEERS 6
#define MAX_PENDING 4
#define LINE 16384
#define WINDOW_MAC 4
#define WINDOW_PEER 16
#define BULK (8u << 20)             /* transfers at least this big are logged with their wire time */
#define WORD(seq, len) (((uint64_t)(uint32_t)(seq) << 32) | (uint32_t)(len))
#define WORD_SEQ(w) ((uint32_t)((w) >> 32))
#define WORD_LEN(w) ((uint32_t)(w))

static inline void cpu_relax(void) {
#if defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__)
    __asm__ __volatile__("pause" ::: "memory");
#endif
}

static inline uint64_t load_word(const volatile uint64_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline void store_word(volatile uint64_t *p, uint64_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

struct ep {
    char device[64];
    int gid_index;
    enum ibv_mtu mtu;
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr[MAX_MRS];
    int nmr;
    union ibv_gid gid;
    int outstanding;               /* posted, not yet completed; nothing is destroyed while this is > 0 */
    int cleanup_failed;             /* retained handles must not be replaced or released out of order */
};

struct piece {
    void *local;
    uint32_t lkey;
    uint64_t remote;
    uint32_t rkey;
    uint32_t len;
};

/* The mailbox as a daemon sees it. On the Mac every 4 MiB has its own MR; on the peer one MR covers it all. */
struct box {
    unsigned char *base;
    uint64_t req, rep;                /* half sizes */
    struct ibv_mr *seg[2 * MAX_SEGS]; /* Mac: request segments then reply segments */
    int nseg;
    struct ibv_mr *all;               /* peer */
};

struct reader {
    int fd;
    char buf[LINE * 2];
    size_t len;
};

/* The user given the listen end's mailbox and socket when root runs the daemon for another user; set = 0 keeps the
   daemon's own user. */
struct owner {
    int set;
    uid_t uid;
    gid_t gid;
};

/* Set by SHUTDOWN, SIGINT or SIGTERM; every loop checks it and the normal teardown runs. */
extern volatile sig_atomic_t g_stop;

/* rpcd_common.c */
uint64_t now_ns(void);
int hold_link_lock(const char *name);
int tcp_connect(const char *host, int port, int seconds);
void set_io_timeout(int fd, int seconds);
void set_keepalive(int fd);
void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_rate(const char *name, const char *what, uint64_t bytes, uint64_t ns);
int valid_name(const char *name);
int parse_mib(unsigned mib, uint64_t *out);
int send_line(int fd, const char *text);
int take_line(struct reader *r, char *out, size_t n, int block);
int socket_in_use(const char *path);
int unix_listen(const char *path);
const char *socket_path(const char *fallback);

/* rpcd_verbs.c */
struct ibv_mr *ep_reg(struct ep *e, void *addr, size_t len, int access);
struct ibv_mr *ep_reg_dmabuf(struct ep *e, int fd, uint64_t offset, void *addr, size_t len, int access);
void ep_destroy_qp(struct ep *e);
void teardown_all(void);
/* teardown_all exits 2 before returning to callers that would unmap memory if
 * any destroy/deregister/deallocate operation failed. */
void on_signal(int sig);
int ep_open(struct ep *e, const char *device, int gid_index, int mtu);
int ep_create_qp(struct ep *e);
int ep_connect(struct ep *e, unsigned qpn, unsigned psn, const char *gid);
void gid_string(const union ibv_gid *g, char *out, size_t n);
int post_pieces(struct ep *e, enum ibv_wr_opcode op, const struct piece *p, int n, int window, uint64_t timeout_ns);
uint32_t box_lkey(const struct box *b, uint64_t off);
int cut(const struct box *b, uint64_t off, uint64_t len, uint64_t remote_base, uint32_t rkey, const uint32_t *rseg_rkey,
        const uint64_t *rseg_addr, uint64_t roff, uint64_t max, struct piece *out, int cap);
void write_sizes(struct box *b);

/* rpcd_listen.c and rpcd_connect.c */
int run_listen(const char *name, const char *device, int gid_index, int mtu, struct in_addr bind_addr, int port,
               uint64_t req_bytes, uint64_t rep_bytes, const struct owner *owner);
int run_connect(int npeers, char **specs, int direct);
/* fd is borrowed; the single-peer Linux path retains its own CLOEXEC duplicate. */
int run_connect_with_buffer(int npeers, char **specs, int direct, int fd);
/* Optional read-only FIFO lease: EOF requests orderly shutdown. */
int run_connect_with_fds(int npeers, char **specs, int direct, int buffer_fd, int parent_fd);

#endif

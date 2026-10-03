/* libmcdma-fabric, ABI 1: one-sided writes between registered windows over MCDMA links, for engines that run their
 * own collectives. RoCE links (MCDMA's CX5 provider, Linux mlx5) use RDMA WRITE and READ. Apple Thunderbolt RDMA has
 * only SEND and RECV, so the receiving library places incoming writes itself: call mcdma_fabric_progress or
 * mcdma_fabric_wait, or open with MCDMA_FABRIC_PROGRESS_THREAD. Peers meet through the same link-local exchange as
 * mcdma-rpcd, never TCP. docs/fabric.md describes the guarantees. */
#ifndef MCDMA_FABRIC_H
#define MCDMA_FABRIC_H

#include <stddef.h>
#include <stdint.h>

#define MCDMA_FABRIC_ABI 1u
#if defined(__GNUC__)
#define MCDMA_FABRIC_API __attribute__((visibility("default")))
#else
#define MCDMA_FABRIC_API
#endif

enum mcdma_fabric_status {
    MCDMA_FABRIC_OK = 0,
    MCDMA_FABRIC_INVALID = 1,       /* a bad argument or handle */
    MCDMA_FABRIC_UNSUPPORTED = 2,   /* the link cannot do this: READ on Thunderbolt, atomics on any link */
    MCDMA_FABRIC_NOMEM = 3,
    MCDMA_FABRIC_DEVICE = 4,        /* the device, a registration or a queue pair failed */
    MCDMA_FABRIC_PEER = 5,          /* the peer refused, went away or broke the protocol; disconnect it */
    MCDMA_FABRIC_TIMEOUT = 6,
    MCDMA_FABRIC_BOUNDS = 7,        /* outside a window, or a misaligned signal */
};

enum mcdma_fabric_link { MCDMA_FABRIC_ROCE = 1, MCDMA_FABRIC_THUNDERBOLT = 2 };

#define MCDMA_FABRIC_PROGRESS_THREAD 1u   /* a library thread places incoming Thunderbolt writes */

struct mcdma_fabric;
struct mcdma_fabric_peer;

#ifdef __cplusplus
extern "C" {
#endif

/* The ABI this library implements; callers refuse any other value. */
MCDMA_FABRIC_API uint32_t mcdma_fabric_abi(void);

/* Open `device` and register `window`, page aligned and a whole number of pages, for it. On Linux, dmabuf_fd >= 0
 * registers that DMA-BUF from dmabuf_offset instead, `window` being its CPU mapping; elsewhere it is UNSUPPORTED. */
MCDMA_FABRIC_API int mcdma_fabric_open(const char *device, int gid_index, int path_mtu, void *window, size_t length,
                                       int dmabuf_fd, uint64_t dmabuf_offset, uint32_t flags, struct mcdma_fabric **out);

/* Meet the peer at the other end of the Thunderbolt IP interface `via` (IFACE, or IFACE/fe80::ADDR to admit only
 * that address). Both sides call this with the same `name`; each binds UDP `port` and sends to `peer_port` (0 means
 * the same port). Returns once both queue pairs are at RTS and each side holds the other's window. */
MCDMA_FABRIC_API int mcdma_fabric_connect(struct mcdma_fabric *f, const char *via, int port, int peer_port,
                                          const char *name, uint64_t timeout_ns, struct mcdma_fabric_peer **out);

/* MCDMA_FABRIC_ROCE or MCDMA_FABRIC_THUNDERBOLT. */
MCDMA_FABRIC_API int mcdma_fabric_link(const struct mcdma_fabric_peer *p);

/* The length of the peer's window. */
MCDMA_FABRIC_API uint64_t mcdma_fabric_peer_length(const struct mcdma_fabric_peer *p);

/* Copy [local_offset, local_offset + length) of this window to the peer's window at remote_offset. Returns once
 * posted: keep the source unchanged until mcdma_fabric_flush returns. Writes to one peer land in posting order. */
MCDMA_FABRIC_API int mcdma_fabric_write(struct mcdma_fabric_peer *p, uint64_t local_offset, uint64_t remote_offset,
                                        uint64_t length);

/* Store `value` at the peer's 8-byte-aligned remote_offset, after every earlier write to that peer has landed. */
MCDMA_FABRIC_API int mcdma_fabric_signal(struct mcdma_fabric_peer *p, uint64_t remote_offset, uint64_t value);

/* Copy the peer's window at remote_offset into this window; RoCE only, returns once the bytes are here. */
MCDMA_FABRIC_API int mcdma_fabric_read(struct mcdma_fabric_peer *p, uint64_t local_offset, uint64_t remote_offset,
                                       uint64_t length);

/* No MCDMA link has atomics: always MCDMA_FABRIC_UNSUPPORTED; build barriers from signals. */
MCDMA_FABRIC_API int mcdma_fabric_fetch_add(struct mcdma_fabric_peer *p, uint64_t remote_offset, uint64_t add,
                                            uint64_t *old);

/* Return once every write and signal posted to the peer has landed in its window. On Thunderbolt this is a round
 * trip the peer answers, so the peer must be progressing too. */
MCDMA_FABRIC_API int mcdma_fabric_flush(struct mcdma_fabric_peer *p, uint64_t timeout_ns);

/* Place incoming Thunderbolt writes, reap completions and answer the peers' exchange messages. */
MCDMA_FABRIC_API int mcdma_fabric_progress(struct mcdma_fabric *f);

/* Wait until the 8-byte word at `offset` of this window reaches `value` (unsigned), making progress meanwhile:
 * monotonic flags such as step << 32 | bytes are never missed when a peer runs a step ahead. */
MCDMA_FABRIC_API int mcdma_fabric_wait(struct mcdma_fabric *f, uint64_t offset, uint64_t value, uint64_t timeout_ns);

/* Tell the peer goodbye and destroy its queue pairs; *p becomes NULL. */
MCDMA_FABRIC_API void mcdma_fabric_disconnect(struct mcdma_fabric_peer **p);

/* Disconnect every peer, stop the progress thread, deregister the window and close the device; *f becomes NULL.
 * The window itself stays the caller's. */
MCDMA_FABRIC_API void mcdma_fabric_close(struct mcdma_fabric **f);

#ifdef __cplusplus
}
#endif

#endif

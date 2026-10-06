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

/* Open `device` and register `window`, page aligned and a whole number of pages, for it. Exactly one '+' bonds two
 * Thunderbolt devices, e.g. "rdma_en3+rdma_en4", and registers the same window on both. A bond always starts one
 * progress thread per link. Single-link strings and ABI 1 are unchanged. On Linux, dmabuf_fd >= 0 registers that
 * DMA-BUF from dmabuf_offset instead, `window` being its CPU mapping; elsewhere it is UNSUPPORTED. */
MCDMA_FABRIC_API int mcdma_fabric_open(const char *device, int gid_index, int path_mtu, void *window, size_t length,
                                       int dmabuf_fd, uint64_t dmabuf_offset, uint32_t flags, struct mcdma_fabric **out);

/* Meet over `via`: IFACE, IFACE/fe80::ADDR or IFACE/IPv4 to admit only that address. A bond requires two entries
 * joined by '+', in device order, e.g. "en3/192.0.2.10+en4/198.51.100.10". Both ends must bond the corresponding links
 * and call with the same name. Lane k binds UDP port+k and sends to peer_port+k (0 means port); a bond therefore
 * needs two consecutive ports. Returns once every queue pair is at RTS and both links name the same remote peer. */
MCDMA_FABRIC_API int mcdma_fabric_connect(struct mcdma_fabric *f, const char *via, int port, int peer_port,
                                          const char *name, uint64_t timeout_ns, struct mcdma_fabric_peer **out);

/* MCDMA_FABRIC_ROCE or MCDMA_FABRIC_THUNDERBOLT. */
MCDMA_FABRIC_API int mcdma_fabric_link(const struct mcdma_fabric_peer *p);

/* The length of the peer's window. */
MCDMA_FABRIC_API uint64_t mcdma_fabric_peer_length(const struct mcdma_fabric_peer *p);

/* Additive ABI 1: MCDMA_FABRIC_OK while the peer is up, MCDMA_FABRIC_PEER once it is down for good, with the first
 * reason copied into `why` (n bytes; NULL for none). A peer goes down when either end fails: the end that fails logs
 * one line and tells the other, whose links report it within about a millisecond. An atomic load while the peer is
 * up, so a caller spinning on a window word can poll it. */
MCDMA_FABRIC_API int mcdma_fabric_peer_status(const struct mcdma_fabric_peer *p, char *why, size_t n);

/* Additive ABI 1 diagnostics: one link for an ordinary peer, two for a bond; NULL has zero. Counts are write payload
 * bytes, excluding headers, signals and fence traffic. Local SEND completion is not proof of remote placement;
 * the API requires flush before source reuse. RoCE completed_bytes is published at successful flush. Index order
 * is the device/via string order. */
struct mcdma_fabric_link_stats { uint64_t posted_bytes, completed_bytes; };
MCDMA_FABRIC_API unsigned mcdma_fabric_link_count(const struct mcdma_fabric_peer *p);
MCDMA_FABRIC_API int mcdma_fabric_link_stats(const struct mcdma_fabric_peer *p, unsigned index,
                                            struct mcdma_fabric_link_stats *out);

/* Copy [local_offset, local_offset + length) of this window to the peer's window at remote_offset. Returns once
 * posted: keep the source unchanged until mcdma_fabric_flush succeeds. Single-link writes retain posting order.
 * A bond spreads large disjoint chunks by
 * outstanding bytes, and keeps small writes whole on one link. Disjoint bonded chunks may land out of order;
 * overlapping writes retain later-write-wins order, and every signal covers all earlier writes on both links. */
MCDMA_FABRIC_API int mcdma_fabric_write(struct mcdma_fabric_peer *p, uint64_t local_offset, uint64_t remote_offset,
                                        uint64_t length);

/* Store `value` at the peer's 8-byte-aligned remote_offset, after every earlier write to that peer has landed. */
MCDMA_FABRIC_API int mcdma_fabric_signal(struct mcdma_fabric_peer *p, uint64_t remote_offset, uint64_t value);

/* mcdma_fabric_write, then mcdma_fabric_signal, fused when representable on one Thunderbolt link. A large bonded
 * write spreads across links before its ordered signal. The library may overwrite the MCDMA_FABRIC_WS_ROOM bytes
 * before local_offset; both ends need a library that has this call. A length of 0 is mcdma_fabric_signal, on any
 * link and with no room needed before local_offset. */
#define MCDMA_FABRIC_WS_ROOM 64u
MCDMA_FABRIC_API int mcdma_fabric_write_signal(struct mcdma_fabric_peer *p, uint64_t local_offset, uint64_t remote_offset,
                                               uint64_t length, uint64_t signal_offset, uint64_t value);

/* Copy the peer's window at remote_offset into this window; RoCE only, returns once the bytes are here. */
MCDMA_FABRIC_API int mcdma_fabric_read(struct mcdma_fabric_peer *p, uint64_t local_offset, uint64_t remote_offset,
                                       uint64_t length);

/* No MCDMA link has atomics: always MCDMA_FABRIC_UNSUPPORTED; build barriers from signals. */
MCDMA_FABRIC_API int mcdma_fabric_fetch_add(struct mcdma_fabric_peer *p, uint64_t remote_offset, uint64_t add,
                                            uint64_t *old);

/* Return once every write and signal posted to the peer has landed in its window. On Thunderbolt this is a round
 * trip the peer answers, so the peer must be progressing too. A bond fences both links. A failure poisons the whole
 * peer; disconnect it before reusing storage whose completion could not be established. */
MCDMA_FABRIC_API int mcdma_fabric_flush(struct mcdma_fabric_peer *p, uint64_t timeout_ns);

/* Place incoming Thunderbolt writes, reap completions and answer the peers' exchange messages. */
MCDMA_FABRIC_API int mcdma_fabric_progress(struct mcdma_fabric *f);

/* Wait until the 8-byte word at `offset` of this window reaches `value` (unsigned), making progress meanwhile:
 * monotonic flags such as step << 32 | bytes are never missed when a peer runs a step ahead. MCDMA_FABRIC_PEER once
 * every peer of this fabric is down. */
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

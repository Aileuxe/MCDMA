# One-sided writes for engine collectives

`libmcdma-fabric` lets an engine write straight into a registered window on another machine and then signal it. That
is the base for collectives an engine runs itself: barriers, plan broadcasts, all-reduce, all-to-all, pipeline hand-offs
and bulk copies. Unlike `mcdma-rpcd`, the library opens the verbs device inside the application, so an engine posts
writes with no daemon hop. Peers meet through the same Thunderbolt-only exchange as the daemons; there is no TCP.

Status: ABI 1, compiled and offline-tested against a stub verbs library that enforces RoCE keys and Apple's TN3205
rules for Thunderbolt RDMA. It has not run on hardware yet; `fabric-check` below is the qualification tool.

## The calls

| Call | What it does |
| --- | --- |
| `mcdma_fabric_open` | Opens a device and registers one window for it, optionally starting a progress thread |
| `mcdma_fabric_connect` | Meets one peer over a Thunderbolt IP interface; both sides call it with the same name |
| `mcdma_fabric_write` | Copies a range of this window to an offset in the peer's window |
| `mcdma_fabric_signal` | Stores an 8-byte word in the peer's window after every earlier write to that peer |
| `mcdma_fabric_flush` | Returns once everything posted to the peer has landed |
| `mcdma_fabric_read` | Copies from the peer's window into this one; RoCE only |
| `mcdma_fabric_fetch_add` | Always `MCDMA_FABRIC_UNSUPPORTED`: no MCDMA link has atomics |
| `mcdma_fabric_progress` | Places incoming Thunderbolt writes and answers the peers' exchange messages |
| `mcdma_fabric_wait` | Waits until a word of this window reaches a value, making progress meanwhile |
| `mcdma_fabric_disconnect`, `mcdma_fabric_close` | Say goodbye and tear down; the window stays the caller's |

`rpc/mcdma_fabric.h` has the exact signatures and statuses. The library exports nothing else.

## Guarantees

- Writes to one peer land in posting order, and a later write to the same bytes wins. Writes to different peers are
  unordered.
- A signal lands after every write posted to that peer before it. A barrier is one signal per sender into a flag word
  the receiver owns; that is how collectives work without atomics.
- `write` and `signal` return once posted. Keep the source bytes unchanged until `flush` returns, as with any RDMA
  write.
- `wait` returns when the word reaches the value as an unsigned number, so a monotonic flag such as
  `step << 32 | bytes` is never missed when a peer is already a step ahead.
- A write or signal outside either window, or a signal that is not 8-byte aligned, returns `MCDMA_FABRIC_BOUNDS` and
  posts nothing.
- Any failed operation, refused exchange or goodbye from the peer leaves that peer down: every later call on it
  returns `MCDMA_FABRIC_PEER`. Disconnect it and connect again.
- Calls on one fabric are serialized by a lock, so several threads may share it.

## Windows

A window is page aligned and a whole number of pages: 16 KiB pages on Apple silicon. `mcdma_fabric_open` registers it
once per device, and every peer on that device shares the registration. Engines that talk over several ports open one
fabric per port on the same memory.

| Link | Registration |
| --- | --- |
| Thunderbolt | 12 MiB pieces, local access only; Apple allows at most 0xfa0000 bytes a registration and 100 a device, so a port takes windows of about 1.1 GiB less what other users of that port hold |
| RoCE on a Mac | 4 MiB pieces, the largest MCDMA's CX5 provider has run |
| RoCE on Linux | One registration over the whole window |

GPU memory works through the same window:

- Metal: `mcdma_rpc_metal_wrap(window, length)` from `libmcdma-rpc` makes an `MTLBuffer` over the window with no copy.
- CUDA: `mcdma_rpc_cuda_wrap(window, length, device)` from [rpc/gpu](../rpc/gpu/README.md) maps the window into the
  current CUDA context. This is mapped host memory: registering `cudaMalloc` memory still fails on the tested Spark.
- Vulkan: allocate with `mcdma_rpc_vulkan_allocate_export`, then pass its `host_pointer` as the window and its exported
  DMA-BUF descriptor as `dmabuf_fd`. `mcdma_fabric_open` registers the DMA-BUF through libibverbs'
  `ibv_reg_dmabuf_mr`, on Linux only, with no fallback to a copy.

## Connecting

`mcdma_fabric_connect(f, via, port, peer_port, name, timeout_ns, &peer)` runs the [link daemon's exchange](link-daemon.md#the-setup-exchange)
in its symmetric form: both sides offer, each takes its queue pairs to RTS when it holds the other's offer, and each
returns once it has heard that the other holds its own. Each side binds UDP `port` on `via` and sends to `peer_port`,
which is the same port unless two ranks share one host. The same admission rules apply as for the daemons, and on a
Mac `via` must be the Thunderbolt port of a Thunderbolt device, or a Thunderbolt port at all for a CX5 device.

## Thunderbolt links

TN3205 sets what Apple's RDMA over Thunderbolt can do, and the library is built around it:

| Thunderbolt RDMA | What the library does |
| --- | --- |
| SEND and RECV only, `IBV_WR_SEND` | A write becomes a send that the receiver places; there is no READ |
| UC queue pairs, at most 10 a device | Two per peer |
| A receive must match its send's frame count, in 4 KiB frames | The receiver posts a receive of exactly the announced length |
| Credit flow: a send waits for a matching receive | The receiving side must make progress |
| No hardware acknowledgement: a completed send was sent, not received | `flush` is a fence that the peer answers |
| Queue depth up to 4,095 frames, possibly less as granted | Messages shrink to fit both sides' granted depth |

How a write lands:

1. Each peer pair has a control ring of 64 one-frame messages and a data queue. Control messages always fill a whole
   4 KiB frame, so every receive matches its send byte for byte.
2. A write of up to 4,064 bytes travels inside one control message and is copied into place.
3. A larger write is cut into pieces of at most 4 MiB that never cross a registration on either side. Each piece
   sends a control message with its offset and length, then its bytes on the data queue. The receiver posts a receive
   of exactly that length at that offset, so the bytes land in place without a copy.
4. Signals and fences are control messages. The receiver applies control messages in arrival order, each only after
   every earlier data receive has completed, and posts no data receive past an inline write or signal still waiting on
   the same bytes. So signals follow their writes, and later writes to the same bytes win, as RC ordering gives on
   RoCE.
5. Every control message carries a sequence number. A lost or reordered control message, a data message whose length
   differs from its announcement, a write outside the window or any failed completion fails the link instead of
   landing anywhere else.

Because Thunderbolt has no hardware acknowledgement, a data message lost whole with the next one exactly the same
length would land one slot early unnoticed. TN3205 gives no rate for that, and `fabric-check` checks every byte on
hardware to measure it.

Incoming writes land only when the receiving library posts receives for them. Open with
`MCDMA_FABRIC_PROGRESS_THREAD`, or call `mcdma_fabric_progress` or `mcdma_fabric_wait` while waiting. A rank that has
finished its own work keeps progressing until its peers have flushed, because their flushes need its answer.

## Mapping an engine's one-sided interface

An engine whose collectives are written against write, signal, fetch-and-add, read and flush maps onto the library like
this:

| Engine operation | `libmcdma-fabric` |
| --- | --- |
| Write bytes to a peer's offset | `mcdma_fabric_write` with the bytes' offset in the window; bytes outside it are staged there first |
| Signal a word after earlier writes | `mcdma_fabric_signal` |
| Fetch-and-add | `MCDMA_FABRIC_UNSUPPORTED` on every link; use per-sender flag words |
| Read | `mcdma_fabric_read` on RoCE, `MCDMA_FABRIC_UNSUPPORTED` on Thunderbolt |
| Flush | `mcdma_fabric_flush` for each peer |
| Link kind, for a cost model | `mcdma_fabric_link` |
| Wait for a flag | An acquire load of the window, or `mcdma_fabric_wait` when the CPU waits |

A Mac listen end of `mcdma-rpcd` keeps its mailbox in POSIX shared memory `/mcdma-rpc.NAME`, not `/dev/shm`, so a
mailbox client opens it with `shm_open` on macOS.

## Qualifying on hardware

`build/rpc/fabric-check` runs on both machines of one link:

```bash
fabric-check DEVICE GID_INDEX VIA PORT NAME RANK [ROUNDS [SIZES [SECONDS [PEER_PORT]]]]
```

Both ranks give the same name, rounds and sizes (bytes, comma-separated; default `64,4096,14336,1048576`). At each size
the ranks ping-pong a write and a signal `ROUNDS` times (default 10,000), and the receiver checks every word of the
write the moment the signal lands, which tests that data lands before its flag under load. Rank 0 prints the round-trip
median, p99 and maximum. Then rank 0 streams 4 MiB writes for `SECONDS` (default 5) and prints the rate. Each rank ends
with `PASS` and exit 0 only if no word was wrong:

```text
fabric-check: rank 0 connected over thunderbolt
fabric-check: size=4096 rounds=10000 wrong_words=0 rtt_median_us=... rtt_p99_us=... rtt_max_us=...
fabric-check: stream bytes=... seconds=5.0xx gbit_s=...
fabric-check: rank 0 PASS
```

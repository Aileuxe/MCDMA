# One-sided writes for engine collectives

`libmcdma-fabric` lets an engine write straight into a registered window on another machine and then signal it. That
is the base for collectives an engine runs itself: barriers, plan broadcasts, all-reduce, all-to-all, pipeline hand-offs
and bulk copies. Unlike `mcdma-rpcd`, the library opens the verbs device inside the application, so an engine posts
writes with no daemon hop. Peers meet through the same Thunderbolt-only exchange as the daemons; there is no TCP.

Status: ABI 1, compiled and offline-tested against a stub verbs library that enforces RoCE keys and models Thunderbolt
RDMA as two Studios on macOS 27.0 measured it. It has not run on hardware yet; `fabric-check`, `mesh-check` and
`metal-poll` below are the qualification tools.

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

Apple's TN3205 describes RDMA over Thunderbolt as SEND and RECV on UC queue pairs, with receives that match their
sends' frame counts. Two M3 Ultra Studios on macOS 27.0 (build 26A428) behaved differently in a qualification on
3 October 2026, and the library follows what they did:

| Measured on macOS 27.0 | What the library does |
| --- | --- |
| UC queue pairs only, three a device; RC creation fails | One queue pair a link, so a port holds a fabric link and a daemon link with one spare |
| Every request is a SEND; WRITE bytes land in the peer's next posted receive | A write is a SEND that the receiver places; there is no READ and no atomic |
| A SEND crosses as 4 KiB packets, each filling the next posted receive in order | Every receive is one 4 KiB packet, kept posted in a ring |
| Every packet but a message's last completes with `IBV_WC_LOC_LEN_ERR` and 4,096 bytes; a receive over 4,096 bytes fails | The ring reads a message's end from the one clean completion |
| A packet with no receive posted waits; a zero-length SEND is lost and never completes | Senders wait for the ring; no message is ever empty |
| The send queue counts packets, at most 4,095 in flight | Packets in flight are counted against the granted depth |
| A completed send has left the host, not reached the peer; `ibv_query_qp_data_in_order` is 0 | `flush` is a fence the peer answers; nothing relies on byte order inside a packet |

How a write lands:

1. Each link keeps a ring of 2,048 one-packet receives (8 MiB) posted, so a message lands at once whatever the
   receiver is doing.
2. A write of up to 4,064 bytes travels inside one header message, a single packet.
3. A larger write is a header message with its offset and length, then its bytes as a message of their own, sent
   straight from the window with no copy, in pieces of at most 4 MiB that never cross a registration.
4. The receiver takes whole messages from the ring in arrival order and copies each write's bytes into place. A signal
   is applied when its header is taken, so it lands after the writes before it, and later writes to the same bytes
   win, as RC ordering gives on RoCE.
5. Every header carries a sequence number. A lost or reordered message, bytes whose length differs from their header,
   a write outside the window or any unexpected completion fails the link instead of landing anywhere else.

Only one copy is paid, on the receiving CPU. An engine that reads partials where they land could skip it; that needs
the ring exposed, which this ABI does not do yet.

Incoming writes land only when the receiving library takes them from the ring. Open with
`MCDMA_FABRIC_PROGRESS_THREAD`, or call `mcdma_fabric_progress` or `mcdma_fabric_wait` while waiting. A node with
several links needs a progress thread on each: a node blocked sending on one link must keep taking what the others
bring, or three nodes waiting on each other in a ring deadlock. A rank that has finished its own work keeps
progressing until its peers have flushed, because their flushes need its answer.

## Writes from several peers

Nothing orders writes that arrive over different links. If two peers write the same bytes, a late write from one can
land after a newer write from the other, even past a flag. Give each sender its own region at every receiver, as the
all-reduce slots below do, and let each step's flag cover only that sender's region.

## GPU hand-off

A collective step pays a hand-off between the GPU and the host on each side. Waking the host from a Metal event costs
about 100 µs; a host thread spinning on a word the GPU writes, and a resident kernel spinning on a word the host or the
NIC writes, should cost a few microseconds. Whether a resident Metal kernel sees those words while it runs is not
documented, so `metal-poll` measures it:

- `metal-poll cpu ROUNDS` runs on any Apple silicon Mac. A host thread writes a 4 KiB packet whose every 16-byte line
  is three data words and a round tag, and a resident kernel polls the tags, checks the data and answers through a
  word the host spins on. It prints the round trip and counts lines that never appeared or appeared torn.
- `metal-poll nic-gpu` and `metal-poll nic-send`, on two Macs cabled port to port, send the same packets by Thunderbolt
  RDMA into a ring that the GPU polls directly, with no host copy and no completion read first. The sender times each
  round trip. Comparing it with `fabric-check`'s host-only round trip at 4 KiB gives the GPU's share.

Every line carries its own tag because the provider reports no byte order inside a packet. Spins are capped and a
late round stops the kernel, so no command buffer runs long.

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

`build/rpc/mesh-check` qualifies four nodes cabled as a full mesh, one Thunderbolt port to each peer:

```bash
mesh-check NODE GID_INDEX BASE_PORT SECONDS ROWS LINK LINK LINK
```

`NODE` is 0 to 3 and fixes the reduction order. Each `LINK` is `DEVICE:IFACE:PEER`, for example `rdma_en3:en3:3`.
Every node opens one fabric a port over the same window and meets its three peers at once. Then:

1. One-shot all-reduces at each row count in `ROWS`, of 14,336-byte bf16 rows. Each node writes its partial to its
   slot at all three peers at once, signals, and sums the four partials in fp32 in node order 0, 1, 2, 3, rounding
   once to bf16. Every partial that crossed a link and every sum is checked against partials the node computes
   itself. Each size prints the communication and sum times and a running hash of every sum.
2. All three links stream at once and each node prints its rate.
3. A long run of random sizes for `SECONDS`, reporting each minute; the nodes stop together on the first step any of
   their clocks asks to stop.

Every node must print `wrong=0`, `stalls=0` and the same hash; each ends with `PASS` and exit 0 only then.


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

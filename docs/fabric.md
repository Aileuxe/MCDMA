# One-sided writes for engine collectives

`libmcdma-fabric` lets an engine write straight into a registered window on another machine and then signal it. That
is the base for collectives an engine runs itself: barriers, plan broadcasts, all-reduce, all-to-all, pipeline hand-offs
and bulk copies. Unlike `mcdma-rpcd`, the library opens the verbs device inside the application, so an engine posts
writes with no daemon hop. Peers meet through the same Thunderbolt-only exchange as the daemons; there is no TCP.

Status: ABI 1. The single-port Thunderbolt path has hardware measurements; the bonded path needs the owner-run
qualification below before its throughput or latency is established. Offline tests use a stub verbs library that
enforces RoCE keys and models Thunderbolt RDMA as two Studios on macOS 27.0 measured it.

## The calls

| Call | What it does |
| --- | --- |
| `mcdma_fabric_open_qps` | Explicit QPs per device, or zero for the supported default |
| `mcdma_fabric_open` | Opens one device, or N Thunderbolt devices joined by `+`, and registers the same window on each |
| `mcdma_fabric_connect_links`, `mcdma_fabric_max_links` | Explicit N UDP ports and supported member limit |
| `mcdma_fabric_connect` | Meets one peer over a Thunderbolt IP interface; both sides call it with the same name |
| `mcdma_fabric_write` | Copies a range of this window to an offset in the peer's window |
| `mcdma_fabric_signal` | Stores an 8-byte word in the peer's window after every earlier write to that peer |
| `mcdma_fabric_write_signal` | A write and then a signal as one message on Thunderbolt; both ends need a library with it. Length 0 is a signal |
| `mcdma_fabric_flush` | Returns once everything posted to the peer has landed |
| `mcdma_fabric_read` | Copies from the peer's window into this one; RoCE only |
| `mcdma_fabric_fetch_add` | Always `MCDMA_FABRIC_UNSUPPORTED`: no MCDMA link has atomics |
| `mcdma_fabric_progress` | Places incoming Thunderbolt writes and answers the peers' exchange messages |
| `mcdma_fabric_wait` | Waits until a word of this window reaches a value, making progress meanwhile |
| `mcdma_fabric_link_count`, `mcdma_fabric_link_stats` | Report each physical link's posted and completed write-payload bytes |
| `mcdma_fabric_peer_status` | Whether the peer is still up, and if not the first reason it went down |
| `mcdma_fabric_disconnect`, `mcdma_fabric_close` | Say goodbye and tear down; the window stays the caller's |

`rpc/mcdma_fabric.h` has the exact signatures and statuses. These additions retain ABI 1 and the existing signatures.

## Guarantees

- Overlapping writes to one peer retain posting order, and a later write to the same bytes wins. Disjoint bonded
  chunks can land in different orders across the physical links. Writes to different peers are unordered.
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
- A peer that goes down says why, once. The end that fails logs one `mcdma-fabric:` line with the reason and sends it
  to the other end over the exchange socket, if that still works. The other end reads its exchange socket every
  millisecond while it progresses: in its progress threads, in `mcdma_fabric_progress` and `mcdma_fabric_wait`, and
  in any flush or send waiting inside the library, which then gives up instead of running out its timeout. It logs
  one line of its own and every later call on the peer returns `MCDMA_FABRIC_PEER`; `mcdma_fabric_wait` returns
  `MCDMA_FABRIC_PEER` once every peer of its fabric is down. `mcdma_fabric_peer_status` returns the first reason at
  either end. It is an atomic load while the peer is up, so a caller spinning on a window word can poll it and stop
  waiting for a peer that has gone.
- A zero-length `mcdma_fabric_write_signal` is `mcdma_fabric_signal`, on every link kind, and needs no room before its
  local offset. Thunderbolt loses a zero-length SEND, so the library never sends one.
- Calls on a single-link fabric are serialized by a lock, so several threads may share it. Callers spin for it rather
  than sleep. A bond has an independent progress thread and placement lock for each physical link, so receiving one
  link's payload does not hold the other link's placement lock.

## Windows

A window is page aligned and a whole number of pages: 16 KiB pages on Apple silicon. `mcdma_fabric_open` registers it
once per device, and every peer on that device shares the registration. A bonded fabric registers that same memory
on all devices. Separate fabrics on the same memory still represent separate peers and have no cross-peer ordering.

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

## N Thunderbolt links as one peer

Join two through eight distinct Thunderbolt devices with `+` in `mcdma_fabric_open`. Register the same window
on every device. `gid_index = -1` scans each data device's GID table, skips zero entries and prefers IPv6 link-local.
`fabric-check --gids DEVICE` prints its port state, limits and GID table. A data interface needs a usable GID and an
ACTIVE port, but its QP does not require IPv4. Meeting traffic can use another existing member's interface and IP.

Connect with one `via` shared by all members, or N entries in device order. For example, four devices
`rdma_en4+rdma_en3+rdma_en2+rdma_en13` can meet entirely over `en4/192.0.2.2`. The address is an example.
The meeting interface must expose an ACTIVE Thunderbolt RDMA device, which can be outside the data list.
An external meeting device is opened briefly for validation, without creating a QP or registering a window. Each physical cable must connect
matching positions in the two device lists. The old connect signature reserves N consecutive UDP ports starting
at `port` and `peer_port`, with zero peer_port meaning the local base. The additive ABI 1
`mcdma_fabric_connect_links` accepts N explicit nonzero, distinct local ports and N corresponding remote ports.
`mcdma_fabric_max_links` reports the supported maximum. Old single-link callers retain their ABI and wire protocol.
Bonds negotiate a new mode and N plus lane position; both ends must use this build. Mixed old/new bonds refuse.

Each QP lane owns a progress thread. `write_signal` of at least 40 KiB divides the source into N*Q contiguous parts,
normally at least 2 KiB per part, reduced near threshold for many lanes, with 64-byte cuts. Recent rates influence the cut, clamped to 3:1. All workers prepare
before one release lets them post concurrently. Each part carries an ordered control header and ordinary payload,
split further at registration or message boundaries. A final signal names all N placement watermarks. This costs
more headers than the old two-link tail encoding, but avoids serializing the parts of one mid-size exchange.

If a worker has not prepared within 100 us, the caller claims it and posts it after releasing the prepared workers.
That fallback preserves correctness but may serialize some parts. A peer logs its first fallback so qualification
can distinguish a warm concurrent path from scheduler delay. Smaller messages stay on one member; N greater than
two uses a staged single-packet write and signal where the payload plus its 48 + 8*N bytes of metadata fits.
The two-link small-message path keeps its joined encoding. The API still reserves only WS_ROOM bytes before the source.

A signal publishes after all N prefixes cover its watermarks, in signal sequence order. Placement counts advance
after payload copies. Overlapping write_signal calls carry the prior prefixes they must wait for at the receiver,
and every payload range covered by a signal waits for that signal
to publish before a later overlapping copy begins. Standalone signals cover all earlier payload ranges too. Plain overlapping writes retain their ordered
member or fence every member first. `flush` fences remote placement on all members; local SEND completion is
insufficient. Source bytes must remain unchanged through flush or the application's equivalent peer acknowledgement.

Plain writes use bounded chunks and the least queued eligible member. Small operations retain one member's affinity.
`link_count` returns N*Q; `link_stats` reports posted and completed payload bytes in QP-major order, excluding headers.
A missing or inactive device refuses open and unwinds already opened members. A failed member poisons the entire
peer and sends its reason. There is no silent reduced-width mode because both ends must agree on every watermark.
Disconnect and explicitly reconnect with the same smaller list on both ends if reduced width is wanted.

`MCDMA_FABRIC_QOS=1` gives progress threads user-interactive QoS on macOS. Cooperative wait polling remains available
through `MCDMA_FABRIC_WAIT_POLL`; a caller spinning directly on its word relies on the progress threads.

## Several QPs per cable

Unset `MCDMA_FABRIC_QPS` selects two per Thunderbolt device if all advertise support, otherwise a uniform one with
its choice logged. Set 1, 2 or 3 for an exact width, or use `mcdma_fabric_open_qps` with Q=0 for default and Q=1..3
for explicit selection independent of the environment. RoCE retains one QP. Unsupported widths refuse.

Every Thunderbolt connection owns its context, PD, window registrations, QP, CQ, ring and placement lock. The
QP-free admission registration is released on first connect so it is not charged alongside active connections.
This avoids sharing the provider's queue mappings between QPs; window bytes are still the same caller allocation.
Multiple connections consume more of the device's MR budget. With Q=2 each window's registrations count twice,
plus the rings, so a configuration that exhausts that budget refuses rather than silently dropping QPs.
Local simultaneous peer reservations count Q per connection against the reported cap, capped at three;
provider allocation also enforces other contexts/processes. A partial connect unwinds all allocations and its
reservation. Q=2 or 3 permits only one peer per physical device in a fabric. Any failed QP fails the peer.

Devices and vias still describe N physical members. Base connect reserves N*Q consecutive UDP ports. Explicit
N-port arrays reserve further QPs in disjoint blocks, stride max(base)-min(base)+1; N*Q arrays name every lane.
For four bases 7530..7533, Q=2 also reserves 7534..7537, Q=3 through 7541. Duplicates and overflow refuse before
peer creation. Multi-QP handshake checks N, Q, lane, window and session and rejects a differently configured peer.

`device_count` and `qps_per_device` report physical geometry. `link_stats` index q*N+d belongs to device d, QP q;
sum all q for a cable's traffic. Completion, signal watermarks and overlap dependencies cover every lane.
Four cables with default Q=2 cost eight progress threads per rank, four more than Q=1; Q=3 costs twelve.
Idle backoff remains, but CPU scheduling and provider/context contention can limit scaling. Host tests establish
correctness, not new throughput.

The flag orders publication; it does not hold a permanent snapshot for a reader. After publication a later write
may proceed. A consumer that must retain those bytes must acknowledge consumption before the producer reuses
that region, or use disjoint slots with the equivalent phase barrier. Source bytes also remain unchanged until
remote flush or an acknowledgement proving their use has ended.

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
   receiver is doing. Posting a receive on a live Thunderbolt queue pair costs 0.25-0.5 µs (0.01-0.03 µs on a fresh
   one), and a send posted meanwhile waits for the link's lock. So a progress thread puts a landed message's receives
   back 5 µs later, when a reply has usually posted, two between polls, and stops while a caller waits for the lock:
   a send that comes while they go back, as when two peers swap and one sends a little after the other's message
   lands, waits behind two receives (about a microsecond). A ring more than a quarter used refills at once, as a
   stream needs.
2. A write of up to 4,064 bytes travels inside one header message, a single packet.
3. A larger write is a header message with its offset and length, then its bytes as a message of their own, sent
   straight from the window with no copy, in pieces of at most 4 MiB that never cross a registration.
4. On one physical link, `mcdma_fabric_write_signal` sends the head, the signal's offset and the bytes as one message
   from the window, its head written into the 64 bytes before the source, so a write and its flag cost one send
   instead of three. One that would span two registrations or outgrow the peer's ring goes as a write, then a signal.
   A bond splits writes of 40 KiB or more across N members with headers and a final watermarked signal.
5. Each link's receiver takes whole messages from the ring in arrival order and copies each write's bytes into place.
   A single-link signal is applied when its header is taken; a bonded signal waits for placement counts from all
   members. Overlapping writes retain later-write-wins order, as
   RC ordering gives on RoCE.
6. Every header carries a sequence number. A lost or reordered message, bytes whose length differs from their header,
   a write outside the window or any unexpected completion fails the link instead of landing anywhere else.

Only one copy is paid, on the receiving CPU. An engine that reads partials where they land could skip it; that needs
the ring exposed, which this ABI does not do yet.

Incoming writes land only when the receiving library takes them from the ring. Open with
`MCDMA_FABRIC_PROGRESS_THREAD`, or call `mcdma_fabric_progress` or `mcdma_fabric_wait` while waiting. A node with
several links needs a progress thread on each: a node blocked sending on one link must keep taking what the others
bring, or three nodes waiting on each other in a ring deadlock. A rank that has finished its own work keeps
progressing until its peers have flushed, because their flushes need its answer.

## Writes from several peers

Nothing orders writes from different peers. If two peers write the same bytes, a late write from one can
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
fabric-check DEVICE GID_INDEX VIA PORT NAME RANK
             [ROUNDS [SIZES [SECONDS [PEER_PORT [MODE [PROGRESS [WAIT [PATTERN [GAP_US]]]]]]]]]
```

Both ranks give the same name, rounds and sizes (bytes, comma-separated; default `64,4096,14336,1048576`). At each size
the ranks ping-pong a write and a signal `ROUNDS` times (default 10,000). The round trip times the transport, as an
engine's exchange runs: each rank fills its bytes before its round, rank 1 answers the moment the flag lands and then
checks every word, and rank 0 checks every word the moment the answer's flag lands, after stopping its clock, which
tests that data lands before its flag under load. Rank 0 prints the round-trip
median, p99 and maximum; `rtt_p50_us` is an additive alias for `rtt_median_us`, and `gb_s` is twice the size over the
median round trip, the rate one direction runs at while it sends. `MODE` is `split`, the existing write then signal
calls, or `combined`, the Thunderbolt-only `write_signal` call. `PROGRESS` is `0` or `1`, selecting the single-link
progress thread; its default remains `0`, and a bond always has one thread per physical link. `WAIT` is `library`, the
default, which waits in `mcdma_fabric_wait`, or `spin`, which spins on the window word as an engine does and leaves
placement to the progress threads, so it needs `PROGRESS` 1; a spinning wait asks `mcdma_fabric_peer_status` every
few thousand reads and fails at once if the peer is down. `PATTERN` is `pingpong`, the default, or `swap`, a
tensor-parallel engine's exchange: both ranks write-and-signal their bytes at once and wait for each other's, and
each rank times its own rounds from its send to the other's flag. Before each round each rank spins `GAP_US`
microseconds, a stand-in for the GPU work between exchanges, or with `GAP_US` as `A,B` rank 0 spins A and rank 1
spins B, as when one side's work runs longer. Nothing else sits between a flag and the next send: three slots a side
let each rank fill its next bytes and check every byte of the other's previous round while a round is in flight, so
swap sizes are capped at 2 MiB. Metadata records ABI, device, interface, link count, mode, progress flag, QoS,
cooperative polling, wait, pattern, gap, requested stream duration and `build`, a checksum of the sources that `make`
passes in (`unknown` otherwise), so two ranks can be checked for the same build.
Both ping-pong source addresses leave the 64-byte header inside their registration, including the source near the
48 MiB boundary of the Thunderbolt 12 MiB pieces, so small combined pings can use the fused send on both parities.

Then rank 0 streams 4 MiB pieces for `SECONDS`, default 60; zero disables the stream for latency comparisons.
Eight landing slots hold up to 32 MiB in flight. The receiver checks every byte of each piece before acknowledging
its slot, and replaces the payload with its complement before reuse, so a missing repeated write cannot pass by
leaving the preceding iteration's bytes in place. The sender never reuses a slot until its acknowledgement arrives,
and a short positive run sends at least two complete slot cycles. Each source slot has separate header space for
`write_signal`. Timing includes placement, byte checking, slot-credit waits and the final placement fence, rather
than measuring unchecked repeated overwrites. This changes what the old stream measured.

The sender prints total bytes, elapsed seconds, throughput and checked bytes, then posted/completed payload bytes
for each physical link. The receiver independently prints checked bytes and wrong words. Each rank ends with
`PASS` and exit 0 only if its checks and fabric calls succeeded:

```text
fabric-check: rank 0 connected over thunderbolt
fabric-check: size=4096 rounds=10000 wrong_words=0 rtt_median_us=... rtt_p99_us=... rtt_max_us=... rtt_p50_us=... gb_s=...
fabric-check: stream bytes=... seconds=60.0xx gbit_s=... checked_bytes=... gb_s=...
fabric-check: stream link=0 posted_bytes=... completed_bytes=...
fabric-check: rank 0 PASS
```

`tests/dual_pipe_qualify.py` prepares one two-host hardware qualification plan. It is review-only by default and
does not invoke SSH until `--execute` is supplied. Give `--host-a` and `--host-b`, each host's checker as built by
`make -C rpc all` from the same commit as `--program-a` and `--program-b`, and two values per host for
`--devices-a/b`, `--interfaces-a/b` and `--addresses-a/b`. Addresses are the local addresses on the two cables; the
script uses the other endpoint's address in each `via` string. The argument order identifies link one and link two
at both hosts. `--ssh-option-a` and `--ssh-option-b` pass `ssh -o` options, such as `ProxyJump=HOST` for a host
reached through the other, and may be repeated.

Choose `--condition healthy` or `--condition degraded`, `--gid-index`, and a free `--base-port` and its successor.
The script measures what a tensor-parallel engine does: each cable alone and then the bond, with each host as rank 0
in turn, ping-pong `write_signal` at 64 bytes, 10, 40 and 160 KB and 1 MiB with both ranks spinning on the window
word (`WAIT` spin), progress threads at `MCDMA_FABRIC_QOS=1`, every byte checked, then a byte-checked stream of at
least 60 seconds. Before it starts it refuses to run while a `fabric-check` is still running on either host. Both
ranks must report the requested configuration, round count and the same make-built `build`, and every case must
come from that one build. Transferred and checked byte counts must agree, completed per-link payload bytes must sum
to the stream, and both bond links must carry payload. The bond passes, in each direction, when:

| Check | Healthy cables | One cable degraded |
| --- | --- | --- |
| Ping-pong p50 at 64 B and 10 KB | at most the better cable's plus 1 µs | the same |
| Ping-pong p50 at 40 KB, 160 KB and 1 MiB | below the better cable's | the same |
| Ping-pong p99 at every size | at most 1.25 times the better cable's plus 2 µs | the same |
| Ping-pong `gb_s` at 1 MiB | at least 1.6 times the faster cable's | at least 0.85 times the two cables' added |
| Stream | at least 1.8 times the faster cable's | at least 0.9 times the two cables' added |

It prints p50, p99 and GB/s for every size and link, the stream rates and each check, and exits unsuccessfully if
any check fails. The script derives stream throughput from transferred bytes and elapsed seconds, and checks that
the separately printed rate agrees within its two-decimal rate and three-decimal time rounding. A verified stream
must contain whole 4 MiB pieces and at least 64 MiB, the checker's minimum two slot cycles.

`python3 -B tests/dual_pipe_qualify.py --self-test` checks parsing and acceptance thresholds offline, with no SSH.
An executed plan stores its runtime configuration and raw logs under ignored `results/` by default. Those logs
contain private host and interface details; keep them out of publication. The script makes no RDMA enablement,
installation, network configuration or GPU changes. Its hardware results are still required before claiming the
bond achieves the throughput or latency targets.

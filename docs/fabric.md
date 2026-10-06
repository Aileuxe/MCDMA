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
| `mcdma_fabric_open` | Opens one device, or two Thunderbolt devices joined by `+`, and registers the same window on each |
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
on both devices. Separate fabrics on the same memory still represent separate peers and have no cross-peer ordering.

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

## Two Thunderbolt links as one peer

Join two devices with `+` in `mcdma_fabric_open`, for example `rdma_en2+rdma_en3`, and join their corresponding
interfaces in the same order in `mcdma_fabric_connect`, for example `en2/192.0.2.10+en3/198.51.100.10`.
Those addresses are documentation examples. Supply the peer's actual IPv4 address on each cable, or its admitted
IPv6 link-local address. A single device and interface keep their existing meaning; `+` is an explicit opt-in.
Both endpoints must open two Thunderbolt devices and give two matching interface entries. A bond needs two
consecutive UDP ports: physical link zero uses `port`/`peer_port`, and physical link one uses each port plus one.
A zero `peer_port` still means the local base port. Reserve both ports on both endpoints.

One bonded peer owns both physical links, and each link has its own receive-placement thread, including when the
caller did not request a progress thread. A caller can still explicitly request progress threads for a single-link
fabric. Both ends need a library with tails, described next; an end without them refuses the bond at connect.

`mcdma_fabric_write_signal` of 16 KiB or more is cut into one message on each link, so a write and its flag still cost
one send per link. The library cuts the source so both links finish together, from what each has queued and the
rate each measured on its own recent sends of 64 KiB and up, counted no further apart than 3:1 so a noisy estimate
cannot push a write onto one link: equal links each carry about half, and a link running at half the other's rate
carries about a third. Link one's part goes as a joined message, its 64-byte head written
into the room before the source as on one link. Link two's part, the bytes after it, goes as a tail: a message with
no head, sent straight from the window. The sender reads each link's queue and rate without the links' locks. With
both links locked it checks room for both parts and sets the watermarks; then it hands the tail to its link's progress
thread and posts the joined part itself, so the two sends post at once: on macOS 27 the first memory barrier after a
Thunderbolt send waits 0.4-1.2 µs for the device, and from one thread the second send would wait behind the first. A
tail the thread has not taken within 3 µs, as when it sleeps after a long idle, the sender takes back and posts. At
the receiver the joined part announces the tail to the other link's
placement thread, both copy at once, and the signal publishes when both parts are in place. The joined part names
the tail's write count on its link, so a lost or reordered tail fails the link. The receiver tells a tail from a
head by its first four bytes, so the sender moves the cut one byte when the bytes there begin with a head's magic.
The head's message is a whole number of 4 KiB packets where it can be, the links take turns carrying it, each part
stays inside one source registration (a source that crosses one is cut at the boundary), and each fits the peer's
ring. A write either link would carry almost all of goes whole on the link that finishes it first; one too large
for one message a link, at most about 8 MiB, goes as writes and then a signal.

A write_signal under 16 KiB goes whole on the link that would finish it first. Links whose finish times are within a
quarter of each other take turns, so traffic that leaves both idle between messages, such as a ping-pong, uses both;
a link measured at half the other's rate gets small messages only when the other has a queue. A link that has not
measured itself yet is taken to run like the other. Signals pick their link the same way.

Plain writes are split into chunks of up to 256 KiB, assigned by each link's posted but not yet completed wire bytes;
equal backlogs alternate. Small writes stay whole on one link. Each link's bulk backlog is bounded to roughly 1 MiB,
so posting a large burst cannot leave half its bytes queued behind a degraded link.

A signal covers earlier writes on both links, not only the link carrying the signal. The receiver waits for the
signal's per-link placement counts before publishing its word, and publishes later signals in order. A write with
its signal has the same rule. Placement counters publish only after the payload copy; acquire/release word
publication makes both links' earlier bytes visible to the waiting caller. The bounded signal reorder queue also
has sender credit: after 4,096 signals without a fence, the sender fences both links before posting another, so a
slow link cannot overflow that queue.

Overlapping writes retain posting order across the bond. The sender records the remote ranges of writes and
signalled words the receiver may not have placed yet, with the latest write on each link into each range;
overlapping ranges merge, and a full table folds into one range that every later call treats as overlapping until a
flush empties it. A joined write_signal that overlaps earlier writes on the other link carries that link's write
count, and its receiver places it only once that many are placed; one that covers a word an earlier signal stores
also waits for every earlier signal to publish. That wait happens at the receiver, beside data already arriving, so
a tensor-parallel exchange that rewrites the same slots every other step pays no fence. A plain write cannot wait that
way: one over bytes only one link carried stays on that link's ordered queue, and one over bytes both links carried,
or over a signalled word, first fences both links. `flush` completes both links and waits for the receiving side's
placement fence; send completion alone is not proof of remote placement on Thunderbolt. A bonded message that waits
10 s on the other link fails its link, since only a lost message keeps it waiting that long. A failed physical link
fails the entire peer, so later writes, signals and flushes report failure rather than silently using only the
surviving link. Disconnect and reconnect the peer after resolving the failed link.

`mcdma_fabric_link_count(peer)` returns one or two. `mcdma_fabric_link_stats(peer, index, &stats)` reports payload
bytes posted and completed on that physical link, excluding protocol headers and control signals. On RoCE the
completed counter is published at successful flush. Snapshot before
and after a workload to get its per-link bytes. Flush before taking the final snapshot. Completion counters measure
send completion; the flush fence supplies the separate remote-placement guarantee.

On macOS, `MCDMA_FABRIC_QOS=1` gives progress threads user-interactive QoS; `0` leaves the default QoS.
`MCDMA_FABRIC_WAIT_POLL=1`, the default, lets a caller waiting in `mcdma_fabric_wait` poll completions cooperatively
instead of waiting only for the progress thread's handoff; `0` selects the thread-only comparison when a progress
thread exists. A caller that spins on its window word, as the qualification does, relies on the progress threads.

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
   back 5 µs later, when a reply has usually posted, in batches of eight between polls, and stops while a caller
   waits for the lock; a ring more than a quarter used refills at once, as a stream needs.
2. A write of up to 4,064 bytes travels inside one header message, a single packet.
3. A larger write is a header message with its offset and length, then its bytes as a message of their own, sent
   straight from the window with no copy, in pieces of at most 4 MiB that never cross a registration.
4. On one physical link, `mcdma_fabric_write_signal` sends the head, the signal's offset and the bytes as one message
   from the window, its head written into the 64 bytes before the source, so a write and its flag cost one send
   instead of three. One that would span two registrations or outgrow the peer's ring goes as a write, then a signal.
   A bond cuts one of 16 KiB or more into such a message on one link and a tail with no head on the other.
5. Each link's receiver takes whole messages from the ring in arrival order and copies each write's bytes into place.
   A single-link signal is applied when its header is taken; a bonded signal waits for placement counts from both
   links, and a tail for its announcement from the other link. Overlapping writes retain later-write-wins order, as
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
fabric-check DEVICE GID_INDEX VIA PORT NAME RANK [ROUNDS [SIZES [SECONDS [PEER_PORT [MODE [PROGRESS [WAIT]]]]]]]
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
few thousand reads and fails at once if the peer is down. Metadata records ABI, device, interface, link count, mode,
progress flag, QoS, cooperative polling, wait, requested stream duration and `build`, a checksum of the sources that
`make` passes in (`unknown` otherwise), so two ranks can be checked for the same build.
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

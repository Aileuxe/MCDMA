# Link daemon for applications

`mcdma-rpcd` moves request and reply bytes between an application on the Mac and a service on a Linux peer over
MCDMA RDMA. Neither application opens a verbs context. Each one talks to its local daemon through a shared-memory
mailbox, and only the daemons hold queue pairs, so an application that crashes or is killed cannot leave a queue pair
behind. `libmcdma-rpc` gives applications in any language the ordered word loads and stores the mailbox needs.

Status: protocol 1 now has Linux CUDA/Vulkan application and model-adapter hardware checks in the
[28 September report](validation-2026-09-28-cuda-vulkan.md), plus offline lifetime tests. This does not establish
a fresh Mac installation or every inference-engine integration on the same revision.

## Build and install

On the Mac, `python3 tools/build.py native` builds `build/mcdma-rpcd` and `build/libmcdma-rpc.dylib` with the rest of
the native tools. Both ends can also build with the Makefile, which writes to `build/rpc`:

```bash
make -C rpc
make -C rpc test
sudo make -C rpc install
```

A Linux peer needs `build-essential` and `libibverbs-dev`. `install` copies `mcdma-rpcd` to `/usr/local/bin`,
`libmcdma-rpc` to `/usr/local/lib` and `mcdma_rpc.h` to `/usr/local/include`; set `PREFIX` to install elsewhere.

## Run a link

Run one listen daemon per link on the Linux peer, then one connect daemon on the Mac with a peer entry for each link.
The examples use documentation addresses and example device names; use your own.

```bash
# Linux peer: link "worker-a" on the port wired to the Mac, control port bound to the address the Mac connects to
mcdma-rpcd listen worker-a rocep1s0f1 3 4096 192.0.2.21:18620 4 64

# Mac: one entry per link, name,host,port,device,gid_index,path_mtu[,req_mib,rep_mib]
mcdma-rpcd connect worker-a,192.0.2.21,18620,rdma_mcrdma0,0,4096,4,64
```

Mailbox halves are whole multiples of 4 MiB up to 256 MiB, and both ends of a link must use the same sizes. Path MTU
must not exceed the port's active MTU.

Mailboxes and sockets are owner-only, so each daemon must belong to the user whose applications use it. On Linux the
mailbox is registered memory: either run the listen daemon as that user with a locked-memory limit (`ulimit -l`) that
covers both halves, or start it as root with `--owner USER` so the mailbox and socket belong to that user:

```bash
sudo mcdma-rpcd listen --owner worker worker-a rocep1s0f1 3 4096 192.0.2.21:18620 4 64
```

Anyone who can reach the listen daemon's control port can write into its mailbox. Bind it to the address the Mac
connects to, as above, and let only the Mac through the firewall. A control connection that has not finished its
handshake within ten seconds is dropped, and TCP keepalive clears a connection left by a Mac that crashed or
restarted, so a stray connection cannot lock the real Mac out.

## Metal buffers on macOS

On macOS, `libmcdma-rpc` also offers `mcdma_rpc_metal_wrap`, `mcdma_rpc_metal_contents` and `mcdma_rpc_metal_release`.
They make a Metal buffer over mailbox memory without a copy, so a GPU reads what the NIC wrote: oMLX imports the reply
half into MLX through DLPack and copies large frames into its own buffers on the GPU. The buffer is refused unless it is
the caller's memory. Helpers built before these functions simply lack them, and callers fall back to a CPU copy.

## Status and shutdown

On Linux, `connect --buffer-fd FD [--parent-fd PIPE_FD] PEER` can use one
application-exported DMA-BUF instead of creating a POSIX mailbox. This optional
single-peer mode registers the FD's existing allocation directly and never falls
back to host registration or copies payload into a replacement buffer. The
parent read-pipe lease requests orderly shutdown when its writer closes; the
GPU owner must wait for successful daemon teardown before releasing storage.
See the [CUDA/Vulkan guide](cuda-vulkan.md) for the allocation and launch order,
and use a fresh dedicated listener for the bounded GPU application example.
The default shared-memory route and protocol 1 remain available.

```bash
printf 'STATUS\n' | nc -U /tmp/mcdma-rpcd.sock
printf 'SHUTDOWN\n' | nc -U /tmp/mcdma-rpcd.sock
```

`STATUS` prints a `VERSION` line, one `PEER` line per link and `END`. The listen daemon answers the same commands on
`/tmp/mcdma-rpcd.NAME.sock`; its `PEER` line has no `host=` or `port=` and adds `service=attached|none`.
`MCDMA_RPCD_SOCKET` replaces either socket path.

One daemon serves each link name. A daemon takes the lock file `/tmp/mcdma-rpc.NAME.lock` for every link before it
touches a device or mailbox, and refuses to start while another daemon holds it or answers on its socket, so a running
daemon is never orphaned. The lock files stay behind after a daemon stops; they are harmless.

`SHUTDOWN`, `SIGINT` and `SIGTERM` all stop a daemon the same orderly way: every queue pair is drained and destroyed
before it exits, and every socket wait is bounded, so it cannot hang on a silent peer. `SIGHUP` is ignored, so a closed
terminal or SSH session leaves the daemon running. Never use `SIGKILL`, which skips the teardown. Stop the Mac's
connect daemon before restarting any listen daemon.

## Protocol 1

Each link has one mailbox: a request half of `R` bytes followed by a reply half of `P` bytes, each starting with a
4 KiB control page. A word is `seq << 32 | length`; sequence 0 means empty.

| Offset | End | Meaning |
| --- | --- | --- |
| request +0 | both | Request word: staged by the client, landed at the service |
| request +64 | connect | 1 while the daemon's link to the peer is up |
| request +72 | connect | Link generation, bumped each time the link comes up |
| request +256 | both | `R` and `P` as two little-endian u64 values |
| reply +0 | both | Pull mode: the peer's ready word |
| reply +64 | connect | Done word: the service's reply has landed |
| reply +128 | listen | Staged word: a reply is ready for the daemon to send |

The connect end's mailbox is the POSIX shared memory object `/mcdma-rpc.NAME`; the listen end's is the file
`/dev/shm/mcdma-rpc.NAME`. Write a payload before its word, store words with `mcdma_rpc_store_word` and wait on them
with `mcdma_rpc_wait_word`.

A client call:

1. Copy the request after the request half's control page and store its word with a new sequence.
2. Wait for the done word (reply +64) to carry that sequence, then read the reply after the reply half's control page.
   A request staged before a reconnect is lost: if the link generation (request +72) changes while you wait, fail
   the call instead of waiting on.

A service:

1. Connect to the listen daemon's socket and send `MODE poll`. The daemon answers `OK`, or `ERR busy` while another
   service holds the link. After `OK` it sends nothing until the registration ends with `BYE` or a closed socket,
   which happens when the link drops: requests in flight are lost with it, so fail the work rather than wait.
2. Wait for the request word to carry a sequence other than the last one served, and read the request.
3. Copy the reply after the reply half's control page and store the staged word (reply +128) with the request's
   sequence.

By default the listen daemon writes each reply and then the client's done word straight into the Mac's reply half.
That relies on the Mac's NIC not reordering its PCIe writes (`MCDMARelaxedOrdering = No`, the default).
`MCDMA_RPC_PULL=1` on the connect daemon makes the peer send only its ready word and the Mac read the payload instead.

The connect daemon reaches the listen daemon's control port over TCP. It sends
`HELLO 1 QPN PSN GID MODE R P N` followed by the key and address of each reply-half segment while its own queue pair
is still in INIT. The listen daemon takes its queue pair to RTS before it answers with its own `HELLO`, and only then
does the Mac move to RTR and RTS and send `READY`.

`STATUS` on the connect daemon's socket answers:

```text
VERSION mcdma-rpcd 1 RELEASE
PEER NAME up|down calls N failures N MiB N host=HOST port=PORT device=DEVICE req_mib=R rep_mib=P since=EPOCH
END
```

`since` is the time the link came up, or 0 while it is down.

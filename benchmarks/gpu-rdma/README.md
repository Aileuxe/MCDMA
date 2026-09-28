# CUDA and Vulkan RDMA correctness

This optional Linux check transfers GPU-generated payloads through RDMA and
verifies them on both GPUs, using the same shared allocation for GPU access and
NIC registration at each endpoint. CUDA imports caller-owned page-aligned storage through the public
`mcdma_rpc_cuda_wrap` API, which uses `cudaHostRegisterMapped`; Vulkan imports
that endpoint's caller-owned storage through `mcdma_rpc_vulkan_wrap`, using
`VK_EXT_external_memory_host` and requiring a
compatible `HOST_VISIBLE | HOST_COHERENT` memory type. There is no CPU payload
fill, comparison or staging copy. The CPU runs the control protocol, posts verbs
work, waits for GPU work and completions, and reads a four-byte mismatch count.

A passing result establishes four serialized RDMA operations over GPU-accessible
shared memory: initiator WRITE and READ, followed by responder WRITE and READ.
It does not establish GPUDirect registration of existing `cudaMalloc` memory,
Vulkan device-local memory registration, zero CPU activity, concurrent GPU/NIC
access, inference-engine integration, latency or throughput. A disabled CUDA
GPUDirect device-memory capability does not prevent this mapped-host path.
HIP is an optional build of the CUDA-shaped check and requires separate hardware
validation before making a HIP support claim.

The verbs control protocol is adapted from `peer/verbs_peer.c` at commit
`719219272c9ce6fc091b4eab6214eac510b5e387`, under the repository's Apache-2.0
license. These programs exercise the optional public GPU import/release APIs in
`rpc/gpu/` and Linux verbs directly; they do not load the macOS driver or an
inference transport. Both wrappers retain the caller allocation until GPU and
NIC accesses have completed, verbs has deregistered it, and wrapper release
succeeds.

## Build on each Linux endpoint

Install a C++17 compiler, RDMA headers and `libibverbs`, plus the corresponding
GPU development tools. CUDA needs `nvcc`, the CUDA runtime and driver libraries.
Vulkan needs its headers/loader and `glslangValidator`. HIP needs `hipcc` and the
HIP runtime. Python 3.10 or later runs the controller on any SSH-capable host;
each Linux endpoint needs GNU `timeout` and permission to register locked memory.

```sh
# On the CUDA endpoint, from the repository root:
make -C benchmarks/gpu-rdma cuda

# On the Vulkan endpoint:
make -C benchmarks/gpu-rdma vulkan

# Optional, on a supported HIP endpoint:
make -C benchmarks/gpu-rdma hip
```

Plain `make -C benchmarks/gpu-rdma` prints help and needs no GPU compiler.
Override `NVCC`, `NVCCFLAGS`, `CXX`, `CXXFLAGS`, `HIPCC`, `HIPFLAGS`, `GLSLANG` or
`BUILD` when tools or architecture flags differ. The CUDA and Vulkan targets also build their optional `rpc/gpu/` shared
library under `build/rpc/gpu/` and copy it beside the test binary, which uses an
`$ORIGIN` runtime search path. Keep that shared library beside its binary when
copying it to another directory. `CUDA_HOME`/`CUDA_LIBDIR` select the CUDA SDK
for the optional library build; the CUDA runtime must also be discoverable by
the endpoint's dynamic loader, for example through its installed loader config
or `LD_LIBRARY_PATH`. The default CUDA flags select the shared runtime so the
program and wrapper use the same runtime library. Keep `gpu_verbs_shader.spv` beside
`gpu-verbs-vulkan`, or supply its absolute path with `--b-shader`.
Both Vulkan NIC-sharing checks require `VK_EXT_queue_family_foreign`. The
Vulkan check rejects CPU/software devices and fails if host import or
coherent storage-buffer memory is unavailable; it never switches to staging.

## Configure the link

Use `rdma link`, `ibv_devinfo -d "$RDMA_DEVICE"`, `ip -br link`, and the selected
port's `/sys/class/infiniband/$RDMA_DEVICE/ports/$RDMA_PORT/gid_attrs/` entries to
identify the active Ethernet port and the RoCE v2 GID index for the intended
NIC/network interface. Verify both ports' active MTUs and configure reciprocal
IP/neighbor reachability for that RDMA network using your host's network tools.
The SSH management connection can use a different interface or a jump host.
The controller does not change interfaces, routes, neighbors, drivers or firmware.

Set the following shell variables to your own SSH destinations, absolute binary
paths, selected devices, GID indices and a new private output directory whose
parent already exists. SSH aliases can supply credentials and jump-host setup
from your local SSH config. Use `--ssh-config "$PRIVATE_SSH_CONFIG"` to pass a
separate existing readable config with `ssh -F`; it can define endpoint aliases,
`IdentityFile` and `ProxyJump` without changing your normal SSH configuration. The example deliberately has no machine addresses,
private keys or assumed device/GID defaults.

```sh
python3 benchmarks/gpu-rdma/run.py \
  --a-host "$CUDA_SSH" --a-exe "$CUDA_BINARY" --a-backend cuda \
  --a-device "$CUDA_RDMA_DEVICE" --a-gid "$CUDA_GID_INDEX" \
  --b-host "$VULKAN_SSH" --b-exe "$VULKAN_BINARY" --b-backend vulkan \
  --b-device "$VULKAN_RDMA_DEVICE" --b-gid "$VULKAN_GID_INDEX" \
  --payload 4096 --seed 17 --mtu 1024 --results "$PRIVATE_NEW_RESULT_DIRECTORY"
```

Use `--a-port`/`--b-port` when a NIC port is not 1 and `--a-gpu`/`--b-gpu` when a
GPU index is not 0. Each endpoint also accepts `--a-key`/`--b-key` and
`--a-jump`/`--b-jump`; paths and values are passed as arguments, not interpolated
into an unquoted shell command. A jump host's authentication comes from SSH
config, including a config selected with `--ssh-config`; OpenSSH does not forward
the endpoint's `-i` option to a `-J` jump host. `--swap` makes endpoint B initiate first. Repeat with 1024 and 4096 byte
payloads, different seeds and both initiators, using a fresh result directory for
every run. Only use `--mtu 4096` when both selected ports support that path MTU.

For a negative control, repeat with `--seed 17 --peer-seed 18`. The responder
must detect a mismatch in `received-forward`, and the runner must exit nonzero;
its stderr log should report `errors` equal to the payload length. A negative
run is recorded as a failed validation, never a passing transfer. Seeds whose
low eight bits are equal produce identical payloads and are unsuitable for this
control. The normal successful-cleanup requirement still applies to positive
runs; the negative control may end a peer before explicit successful cleanup.

## Read the result

The controller creates the result directory with mode 0700 and logs/manifest
with mode 0600, refuses to overwrite an existing directory, and prints only a
pass/fail marker. It redacts endpoint descriptors and named memory-region keys
and addresses from saved logs. These files can still contain hardware metadata
and error text, so keep them outside version control and review them separately
before sharing. The SSH channel carries descriptors and commands, never payload.

Exit 0 requires exact WRITE/READ markers, the requested backend, GPU index,
RDMA device/port/GID index and configuration,
all three GPU verification phases on each endpoint, public wrapper import and
release markers, and both explicit successful cleanup markers. Each verification covers all 24,576 registered bytes, including
leading/trailing guards, unused slots and 1024-byte payload tails. The wrapper imports exactly that span; a host requiring incompatible page or
Vulkan import alignment fails without expanding it or allocating a fallback.
GPU synchronization precedes verbs access; receiver verification follows sender
completion. The Vulkan verifier initializes its full 24,576-byte registered span,
then releases that exact range to `VK_QUEUE_FAMILY_FOREIGN_EXT` after each GPU
dispatch and reacquires it before subsequent GPU use. Release completes at the
fence before verbs work is posted; the separate scalar error buffer retains its
HOST synchronization. A completion by itself is insufficient evidence of GPU correctness.

Any missing marker, mismatch, nonzero peer exit or timeout makes the controller
exit nonzero and preserve a failed manifest. `--timeout` bounds each control
step; SSH has connection/liveness limits, and a remote GNU `timeout` watchdog
bounds a disconnected peer to at most eight step timeouts plus ten seconds,
with a minimum of sixty seconds and a five-second kill grace. Local failure
closes peer input, terminates the owned SSH process group including jump-host
children, and cancels nonblocking log readers even if another process keeps a
pipe open; successful teardown is reported only when
both peers actually emit the cleanup marker.

`mapped-host probe` can check local allocation/registration without a transfer.
CUDA also offers `device-probe probe`, which tries genuine device allocation
registration and exits 3 if it fails without falling back. Vulkan returns 3
without attempting that probe because an opaque `VkDeviceMemory` is not a verbs
address. Probe success never counts as a network test.

```sh
python3 -m unittest discover -s tests -p 'test_gpu_rdma_runner.py'
```

The offline suite uses simulated subprocess peers to check protocol validation,
timeouts, failure reporting, cleanup and redaction; it does not validate GPU or
NIC hardware.

## GPU applications over daemon mailboxes

`mailbox-cuda` and `mailbox-vulkan` build separate GPU applications that use the
public wrapper APIs and `mcdma-rpcd`. The CUDA service imports the existing
listen daemon's POSIX shared-memory payload spans. The Vulkan client can create
one coherent, host-visible, exportable 8 MiB GPU allocation and pass its DMA-BUF
to a child connect daemon, which maps and registers that same allocation. GPU
descriptors select the request/reply payload spans within the single Vulkan
buffer and exclude both 4 KiB control pages. The application opens no verbs
context or queue pair, and neither process stages payloads in another allocation.

```sh
# Build on the respective Linux hosts, with the compiler overrides above:
make -C benchmarks/gpu-rdma mailbox-cuda
make -C benchmarks/gpu-rdma mailbox-vulkan

# On a freshly started dedicated listen link, as its mailbox owner:
benchmarks/gpu-rdma/build/gpu-mailbox-cuda service --link "$LISTEN_LINK" --gpu 0

# After the service prints GPU_MAILBOX_READY, on the Vulkan host:
CONNECT_PEER="${CONNECT_LINK},${SERVICE_HOST},${SERVICE_PORT},${VULKAN_RDMA_DEVICE},${VULKAN_GID},${MTU},4,4"
benchmarks/gpu-rdma/build/gpu-mailbox-vulkan client \
  --link "$CONNECT_LINK" --gpu 0 \
  --daemon "$MCDMA_RPCD_BINARY" --connect "$CONNECT_PEER"
```

Start a fresh dedicated CUDA listen daemon and mailbox for every owned-client
run, using [the link-daemon guide](../../docs/link-daemon.md) with 4 MiB request
and reply halves. An idle listener reused after a previous owned-client shutdown
is insufficient: a new connection resets the daemon sequence after the service
has captured the previous one. The Vulkan command requires the daemon build
that supports `connect --buffer-fd FD --parent-fd FD PEER`, and requires a new or
idle link with no existing connect daemon holding its name. The application
spawns exactly one child through `posix_spawn`, without a shell or GPU calls in
a forked child. It sets that child's `MCDMA_RPCD_SOCKET` to the selected per-link
socket and leaves existing daemons alone. The separately started CUDA listen
daemon remains running after the test; stop it cleanly before starting a fresh
listener for another owned-client trial.

`--daemon` and `--connect` select explicit Vulkan-owned DMA-BUF allocation;
there is no fallback from a failed host import. This mode enables
`VK_KHR_external_memory_fd`, `VK_EXT_external_memory_dma_buf` and
`VK_EXT_queue_family_foreign`; missing support is an explicit failure. It requires
coherent host-visible memory, and excludes unsupported memory-property features
through the public allocator. Omitting both options retains the original
external-host-import diagnostic. That route can fail on file-backed POSIX shared
memory even when anonymous aligned host imports work, and a failure must remain
explicit. Do not substitute a private mapping or copy to make it pass.

Both modes require an idle link and completion of the previous client. This
client and the Python llama.cpp proxy share the same client lock; the listen
socket grants exclusive service registration. Other applications must follow
that ownership rule too. `--socket` overrides the service or child daemon socket.
`--mailbox` overrides an existing file mapping only and cannot be combined with
owned DMA-BUF mode. `--shader` selects the Vulkan SPIR-V file if it is not beside
the binary. The linked wrapper and mailbox helper libraries must remain beside
the binary. The application does not change network setup, drivers or firmware.

There are twelve payload calls, cycling through 64 B, 4 KiB and 1 MiB four times.
The client GPU writes a sequence-dependent request pattern, the service GPU
checks it and writes a different reply pattern, and the client GPU checks the
reply. Each check examines the entire payload span, including unused tail guards.
Only GPU kernels write or compare the tested payload bytes; CPUs initialize
new control pages, operate control words, wait for GPU work and read scalar
mismatch counts. The runtime and daemon still perform CPU work. This checks GPU
production and consumption in separate applications over RDMA mailboxes; it does
not establish direct access to arbitrary existing GPU tensors, device-local CUDA
GPUDirect, inference integration or a throughput result.

The synchronization contract is serialized. A GPU dispatch must finish before
its application publishes a request or staged-reply word with release ordering.
The receiver acquires the daemon's completion word before submitting a GPU
verification dispatch. For both Vulkan mailbox allocation modes, the GPU first initializes each
entire descriptor span on the compute queue, then releases that exact buffer
range to `VK_QUEUE_FAMILY_FOREIGN_EXT` after every dispatch. Subsequent GPU use
acquires the identical range back to the compute queue before shader access.
The application records foreign ownership only after the dispatch's fence has
completed, before publishing a mailbox word. The acquire depends on the existing
NIC-completion/control protocol; a queue-family barrier does not replace that
external execution ordering. Both control pages remain outside these transfers.

The Vulkan HOST memory barriers remain separate for CPU access and the scalar
error counter. CUDA synchronizes completed kernel work before each word
handoff. The service resets request-tail guards before publishing its reply, so
the next client request cannot race that reset. This requires the daemon's
payload-before-completion ordering, including its documented PCIe ordering
requirement in direct-reply mode; it also supports the daemon's pull-reply mode.
No concurrent GPU/NIC access to a payload span is intended or tested. The owned
mode follows the Khronos requirements for
[external queue-family ownership transfers](https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-queue-transfers)
and [buffer memory barriers](https://docs.vulkan.org/refpages/latest/refpages/source/VkBufferMemoryBarrier.html).
Both Vulkan demos share payloads with a NIC, so they require foreign queue-family
handoffs for their host-import and owned-allocation paths. Ordinary CPU access
to imported host memory alone does not add that requirement to the generic
wrapper API. Allocation/import success supplies neither NIC completion ordering
nor queue-family ownership transfers; callers must provide both for their
actual external device accesses. These barriers do not make unsupported
file-backed host imports succeed.

After the twelfth payload reply has been GPU-verified, the client sends one
additional empty control acknowledgment. The service waits for it before
releasing payload imports and observes the daemon's final empty-reply ready word
before closing registration. Only that final empty-reply check accepts an already
acquired ready word when the client has disconnected; all earlier health checks
remain strict. Both application success markers are required.

In owned mode, the GPU parent keeps the exported descriptor and the only write
end of a lifetime pipe. The daemon inherits the DMA-BUF as FD 3 and the pipe's
read end as FD 4. Closing the parent end requests orderly daemon shutdown; the
application requires child exit 0 before releasing the whole Vulkan allocation
or exported descriptor. A parent crash also closes the lease automatically.
The application never sends SIGKILL to the daemon. On failure it closes the lease
and waits a bounded interval, reports unverified cleanup, and makes no explicit
GPU allocation release; any remaining child DMA-BUF/MR references preserve its
backing storage until daemon teardown. A failed run cannot count as success.

Link-generation changes, service loss, invalid lengths/sequences, mismatches and
timeouts fail the application. `--timeout` bounds each mailbox/readiness wait;
child shutdown allows an additional fifteen seconds for teardown, and a process
watchdog bounds the whole application, including GPU calls. Follow the daemon's
stop/restart procedure before retrying an interrupted link with an incomplete
previous request.

A hardware pass requires both processes to exit 0, the requested physical GPU
backend/index, twelve `GPU_MAILBOX_CALL` records per side, zero mismatches for
every GPU verification, and both `GPU_MAILBOX_COMPLETE` markers with
`calls=12 final_control_ack=1 cleanup=ok`. The CUDA service must show two import
markers. The owned Vulkan client must show one `GPU_MAILBOX_OWNED` allocation,
two `GPU_MAILBOX_VIEW` payload views, the `GPU_MAILBOX_OWNERSHIP` foreign-family
marker, a clean `GPU_MAILBOX_DAEMON_EXIT`, and
`GPU_MAILBOX_OWNED_RELEASE` after that exit. Keep complete logs private and record
the daemon, wrapper, binary and shader build identities alongside them.

```sh
python3 -m unittest discover -s benchmarks/gpu-rdma -p 'test_mailbox_app.py'
```

The offline suite builds CPU simulations of the GPU callbacks and daemon. It
checks guards, control completion, link/service failures, client ownership,
inherited descriptors, child exit before allocation release, the final-reply
shutdown race, and lease closure on parent crash. These tests do not validate a
GPU, DMA-BUF registration or RDMA hardware.

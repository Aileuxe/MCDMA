# CUDA and Vulkan over RDMA

MCDMA can connect Linux CUDA and Vulkan applications through registered shared
memory. The CUDA kernel or Vulkan shader reads and writes the allocation that
the NIC transfers, without a separate payload staging allocation. The CPU still
submits work, synchronizes GPU execution, and handles RDMA completions.

Linux uses the ConnectX card's normal `mlx5_core`, `mlx5_ib`, and `libibverbs`
stack. Do not install the macOS kernel extension on either Linux host. The
optional [GPU import libraries](../rpc/gpu/README.md) extend the existing
[link daemon](link-daemon.md), alongside the existing macOS Metal wrapper.

## Tested allocation path

The tested pair is an NVIDIA GB10 with its ConnectX-7 and an AMD Ryzen AI Max+
395 with Radeon 8060S graphics, using a ConnectX-5 Ex in a Thunderbolt enclosure.
The network port negotiates 100 Gb/s; that port rate is not a measured transfer
rate or the enclosure's PCIe throughput. See the dated validation report for
versions and the scope of each check.

On the GB10, the CUDA device reports GPUDirect RDMA and device-memory DMA-BUF
support as zero, and ordinary verbs registration of `cudaMalloc` storage returns
`EFAULT`. [NVIDIA documents the GB10 coherency restriction](https://nvidia.custhelp.com/app/answers/detail/a_id/5780/kw/hdr/related/1)
and recommends host-allocated communication memory. Changing a capability flag
or loading `nvidia-peermem` does not establish support for a different memory path.

CUDA can import the daemon's shared-memory span with `mcdma_rpc_cuda_wrap`.
Vulkan can import supported anonymous host allocations with
`mcdma_rpc_vulkan_wrap`, as the direct verbs verifier does. The tested RADV driver
rejects file-backed POSIX shared-memory imports: its AMDGPU USERPTR call returns
`EPERM`, including for small mappings, while equally sized anonymous allocations
succeed. Raising the locked-memory limit does not remove that backing-type rule.

For a separate Vulkan application and daemon, allocate an exportable coherent
Vulkan buffer with `mcdma_rpc_vulkan_allocate_export` and pass its DMA-BUF FD to
the Linux daemon. The GPU, CPU control mapping and NIC then reference the same
allocation. This reverses allocation ownership without a payload staging copy;
it does not change the driver or retry a refused import through another path.
Existing device allocations and framework tensors are not transparently
converted by these APIs.

## Prepare both Linux hosts

Use a management interface independent of the intended RDMA cable. Inspect the
actual PCI devices, network interfaces and GPU tools before changing settings:

```sh
lspci -nn
rdma link
ip -br link
ip -br addr
ibv_devinfo
ulimit -l
```

The hosts need a C/C++ compiler, `libibverbs-dev`, `ibverbs-utils`, `rdma-core`,
and `iproute2`. The CUDA application needs the matching CUDA toolkit/runtime;
the Vulkan application needs the loader, headers, a working hardware driver and
`glslangValidator`. `vulkaninfo --summary` must show the intended hardware GPU.
The validation program rejects CPU/software Vulkan devices.

Identify each selected RDMA device's associated network interface and RoCE v2
GID index from `ibv_devinfo` and its `ports/PORT/gid_attrs/` sysfs entries. Do not
copy an index from another machine. An IPv4-addressed Linux pair can use its
IPv4-mapped RoCE v2 GIDs; both selected interfaces must have matching IP/neighbor
reachability. Keep unrelated ports and management addresses intact.

Require active Ethernet ports and a path MTU no larger than either port's
active MTU. The tested pair used Ethernet MTU 9000 and RC path MTU 4096. The
macOS restore helper is specific to `mcrdma` interfaces and is not a Linux setup
command.

Check out the same reviewed MCDMA revision on both hosts, then build:

```sh
make -C rpc
make -C rpc test

# CUDA host:
make -C rpc/gpu cuda CUDA_HOME=/usr/local/cuda
make -C benchmarks/gpu-rdma cuda NVCC=/usr/local/cuda/bin/nvcc \
  NVCCFLAGS='-O2 -std=c++17 --cudart shared -arch=sm_121'

# Vulkan host:
make -C rpc/gpu vulkan
make -C rpc/gpu test-vulkan
make -C benchmarks/gpu-rdma vulkan
```

`sm_121` is the tested GB10 target; select the correct architecture for other
CUDA hardware. These optional targets leave the ordinary RPC library and the
macOS build independent of CUDA and Vulkan SDKs. The Linux GCC formatting bounds
include the fix proposed in [MCDMA PR #5](https://github.com/ashhart/MCDMA/pull/5).

## Verify actual GPU transfers

Follow the [GPU RDMA verifier](../benchmarks/gpu-rdma/README.md) with the observed
devices, ports and GIDs. It exchanges connection descriptors over SSH and sends
the payload through RDMA. A pass requires GPU-generated data, GPU verification
of every registered byte and guard, READ and WRITE initiated by both machines,
and successful GPU/RDMA cleanup on both endpoints.

Use new private result directories, repeat at 1024 and 4096 bytes with both
initiators, and run the mismatched-seed negative control. The same workflow tests
the public GPU import/release APIs. A port being up, successful local memory
registration, or a completed work request alone is not a passing GPU transfer.

## Ordinary shared-memory daemon link

Start a dedicated listen daemon on the CUDA host and a connect daemon on the
Vulkan host, substituting observed values and a private link name:

```sh
# CUDA host:
build/rpc/mcdma-rpcd listen "$LINK" "$CUDA_RDMA_DEVICE" "$CUDA_GID_INDEX" \
  "$PATH_MTU" "$CUDA_DATA_IP:$CONTROL_PORT" 4 4

# Vulkan host:
build/rpc/mcdma-rpcd connect \
  "$LINK,$CUDA_DATA_IP,$CONTROL_PORT,$VULKAN_RDMA_DEVICE,$VULKAN_GID_INDEX,$PATH_MTU,4,4"
```

Bind the control listener to the intended data-link address and restrict access
to its intended peer. Choose a separate link name and socket if another MCDMA
application already owns a mailbox. Run applications under the daemon's owner;
do not take over another user's existing registration.

The locked-memory limit must cover both mailbox halves and the verbs resources,
not just the payload sizes. An 8 MiB limit proved too small for a 4+4 MiB mailbox
because CQ allocation also needed locked memory. Configure an adequate limit
for the daemon process using the host's normal administration policy; no broad
passwordless-sudo rule is needed. The tested connect process used a 128 MiB limit.

This ordinary link supports CUDA host registration and the CPU-staged inference
adapters. Do not assume the Vulkan driver can import its POSIX shared-memory
mapping; use the exported-buffer route below on the tested RADV stack.

## Vulkan-owned daemon memory

The optional Linux connect mode accepts one peer and one inherited exported
buffer: `mcdma-rpcd connect --buffer-fd FD --parent-fd PIPE_FD PEER`. The buffer
must be large enough for both mailbox halves. The daemon keeps its own FD
reference, maps the same storage for control access, and registers 4 MiB segments
with `ibv_reg_dmabuf_mr`. It does not create a POSIX mailbox file or fall back to
ordinary host registration. The parent pipe lets the daemon shut down when its
owning application exits.

Use the [GPU mailbox application example](../benchmarks/gpu-rdma/README.md) to
create the Vulkan buffer and launch that daemon with explicit ownership. Keep
the exported allocation, device, descriptor and parent-pipe writer alive until
all GPU work has completed and the child daemon confirms clean RDMA teardown.
An FD registration probe alone does not establish a successful GPU transfer.

Descriptors must cover only application payload, excluding the control pages.
Finish a GPU producer before publishing its request or reply word, and wait for
the protocol's RDMA completion before launching a GPU consumer. Vulkan needs
the documented host/compute memory dependencies even with coherent memory.
Follow the [API ownership rules](../rpc/gpu/README.md) before teardown.

## Inference integrations

The [native llama.cpp overlay](../integrations/llamacpp/native/README.md) now
allocates compute tensors in CUDA mapped pinned memory and Vulkan coherent
exported DMA-BUF storage. The NIC transfers those same allocations, with
registrations retained through their lifetime. Prepared Vulkan barriers run
with producer and consumer graphs; the tensor-copy path adds no registration
or per-copy RPC handshake. Graph/control messages also use RDMA after bootstrap.
The separate native KV runner sends metadata over TCP and moves its GPU KV
payload directly with RDMA.

The [native validation report](validation-2026-09-28-native-inference.md) records
engine tensor-copy verification, both-initiator NIC latency, Qwen3-8B TP=2 and
direct KV handoff. This is a pinned experimental runtime patch with backend
ABI 3, so follow its apply/verify and matching-build instructions on both hosts.
It does not make arbitrary existing GPU allocations RDMA-capable or remove CPU
scheduling, metadata and sampling work.

The older [Python mailbox adapter](../integrations/llamacpp/README.md) remains
available and explicitly host-staged: it serializes RPC frames through daemon
mailboxes and disables native RPC transport upgrades. The file-based KV tool
is also host-staged. Their measurements remain in the
[earlier report](validation-2026-09-28-cuda-vulkan.md).

Use identical model hashes and the exact pinned runtime on both hosts.
`--split-mode tensor` selects tensor parallelism; splitting layers or setting
split proportions alone does not. Keep NIC completion, actual tensor-copy
calls and model request clocks separate. Native model results must be compared
against matching allocator, prompt, batch and sampling configurations before
attributing a speedup to the transport alone.

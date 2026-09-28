# GPU tensor RDMA integration


The overlay applies to llama.cpp commit
`4da6337767f973e2b4d0797e5b323d77d8565e4a` and changes its private backend ABI to3.
It keeps CUDA and Vulkan compute tensors in storage registered with the NIC.
CUDA uses mapped pinned allocations; Vulkan uses coherent exported DMA_BUF
allocations. Registration and peer memory descriptors are established during
allocation/setup. Cross-host GPU tensor copies post RDMA READ or WRITE directly
against those retained registrations, without a per-copy RPC exchange or host
payload staging. The native RPC engine runs on the Vulkan host with a CUDA RPC
server; READ and WRITE support both payload directions. The separate verbs
benchmark tests both hosts as initiators.

The Vulkan scheduler plans outgoing partials and incoming reduction scratch
before producer graphs. Release barriers are included in the producer's final
GPU submission, and acquire barriers precede its consumer graph. Normal GPU
completion establishes readiness; synchronization is required for correctness,
but the prepared copy hooks make no additional GPU submission or CPU fence.
Control graph commands also use RDMA, with TCP restricted to bootstrap and peer
liveness. Legacy serialized SET/GET tensor payloads and TCP transport fallback
are rejected in native mode.

Use fresh builds on both hosts; ABI3 is incompatible with an unpatched backend
library. The patch is distributed as an MCDMA integration under the original
[llama.cpp MIT notice](LLAMA_LICENSE_MIT.txt); it is not an upstream release.

```sh
python3 integrations/llamacpp/native/apply_native.py apply --source "$LLAMA_SOURCE_DIR"
python3 integrations/llamacpp/native/apply_native.py verify --source "$LLAMA_SOURCE_DIR"
cmake -S "$LLAMA_SOURCE_DIR" -B "$LLAMA_BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release -DGGML_RPC=ON -DGGML_RPC_RDMA=ON \
  -DGGML_MCDMA=ON -DGGML_VULKAN=ON
# On the CUDA host, use -DGGML_VULKAN=OFF -DGGML_CUDA=ON and specify
# -DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc -DCMAKE_CUDA_ARCHITECTURES=121
cmake --build "$LLAMA_BUILD_DIR" --target llama-server llama-bench ggml-rpc-server -j
```

The overlay checks the exact base commit, patch hash and all affected source
hashes before applying, and refuses to overwrite a changed affected file.
Set `GGML_MCDMA=1`, `MCDMA_RDMA_DEV`, `MCDMA_RDMA_GID`, `GGML_RDMA_DEV` and
`GGML_RDMA_GID` to the observed ports on each host. The two sets of device/GID
variables select the tensor channel and metadata channel respectively.
Set `GGML_BACKEND_DL_PATH` to the matching build's `bin` directory and use a
process locked-memory limit large enough for all retained allocations.

```sh
# CUDA host, isolated trusted data link:
"$LLAMA_BUILD_DIR/bin/ggml-rpc-server" --host "$CUDA_DATA_IP" \
  --port "$RPC_PORT" --device CUDA0

# Vulkan host; --rpc must precede --device in this pinned parser:
"$LLAMA_BUILD_DIR/bin/llama-server" --rpc "$CUDA_DATA_IP:$RPC_PORT" \
  -m "$MODEL" --device Vulkan0,RPC0 --split-mode tensor --tensor-split 1,1 \
  -ngl 99 -fa on -ctk f16 -ctv f16 -c 16384 -np 1 --fit off \
  --host 127.0.0.1 --port "$HTTP_PORT"
```

## GPU KV handoff

`mcdma-native-kv` connects the patched inference allocator to direct verbs for a
bounded Qwen3 handoff. It uses the pinned runtime's private memory I/O interface,
so it must be built against the same patched source and libraries on both hosts.
It is a standalone experiment, not an upstream llama.cpp API or server endpoint.

The CUDA sender evaluates the supplied token IDs on its GPU and describes spans
of the actual KV cache allocation. The Vulkan receiver reads those spans with
RDMA directly into its GPU's KV cache allocation. CPU TCP messages carry cache
metadata and MR descriptors, not KV payload. No intermediate state file, tensor
download/upload, snapshot allocation or payload staging fallback exists in this
runner. The allocator hooks must themselves return the original GPU allocation.

Protocol version 2 connects the peers before loading either model, then uses a
buffer-allocation observer to register each complete exported GPU allocation
once. Buffer-owned cache entries retain the channel until each MR is deregistered
before its GPU allocation is freed. The sender's normal prefill synchronization
is the readiness boundary; the handoff does not synchronize or register each KV
span. It sends one bounded metadata/descriptor transcript and waits for one final
acknowledgement. The receiver validates and queues the tensor descriptors, groups
them by actual destination allocation, then acquires each complete KV buffer
once, performs its direct offset reads through the cached MR, and releases it
before moving to the next buffer. The next GPU graph acquires the received
ranges before using them. Any callback or grouped-transfer failure stops the QP
before ownership release and cache-cleanup GPU work.

The first implementation accepts Qwen3, one fresh sequence, F16 K/V and enabled
FlashAttention, and requires identical context size, model identity, token IDs
and tensor-span lengths. Multiple native GPU allocations are supported with
sequential leases, respecting the Vulkan backend's single-active-lease rule.
Fragmented or incompatible tensor-span layouts are rejected.
The model identity argument is the SHA-256 independently verified before the
run; this program does not hash a multi-gigabyte model inside the timing window.

Build after building the patched runtime with its native MCDMA support:

```sh
cmake -S integrations/llamacpp/native -B build/native-kv \
  -DLLAMA_SOURCE_DIR="$LLAMA_SOURCE_DIR" \
  -DLLAMA_BUILD_DIR="$LLAMA_BUILD_DIR"
cmake --build build/native-kv -j
```

Create a token file containing whitespace-separated integer token IDs from the
same stored prompt array on both hosts. Set `GGML_MCDMA=1`, the observed
`MCDMA_RDMA_DEV` and `MCDMA_RDMA_GID`, and a locked-memory limit sufficient for
the selected native allocator. Use a private data-link address and a new output
path for each run. The control port is an experimental peer interface with no
authentication; do not expose it to untrusted clients.

Start the receiver and wait for `MCDMA_KV_READY`:

```sh
build/native-kv/mcdma-native-kv --mode receive --device Vulkan0 \
  --model "$MODEL" --model-id "$MODEL_SHA256" --tokens "$TOKEN_IDS" \
  --address "$VULKAN_DATA_IP" --port "$CONTROL_PORT" \
  --ctx 16384 --predict 128 --output "$NEW_RECEIVER_JSON"
```

Run the sender with matching settings:

```sh
build/native-kv/mcdma-native-kv --mode send --device CUDA0 \
  --model "$MODEL" --model-id "$MODEL_SHA256" --tokens "$TOKEN_IDS" \
  --address "$VULKAN_DATA_IP" --port "$CONTROL_PORT" \
  --ctx 16384 --predict 128 --output "$NEW_SENDER_JSON"
```

Both processes also generate the requested output length with greedy sampling
and EOS ignored. The receiver verifies the restored cache position range,
removes and re-evaluates the final prompt token to reconstruct logits, then
decodes; the sender continues from its existing logits after the handoff.
`--mode baseline` runs a local prefill/decode using the same allocator and
settings, with no address or port required and no NIC channel or allocation
observer.

Success requires exit zero and `MCDMA_NATIVE_KV_PASS` on both endpoints, matching
nonzero KV byte/span counters, successful RDMA/GPU resource release and sensible
continuations against local CUDA and Vulkan references. A missing DMA hook,
failed registration, failed completion or incompatible span fails the run.
Run a mismatched model-ID negative control before accepting a new setup; it
must fail before any KV payload transfer. Model identities, backend identities,
library hashes, private logs and result JSONs belong in local evidence.

The output separates cold setup, prefill, handoff, last-token reconstruction and
decode times. Cold setup includes connection waiting, model/context allocation
and registration. A readiness exchange precedes the handoff timer, so the
receiver's handoff measurement does not include waiting for donor prefill.
Handoff time includes metadata processing, an ownership exchange per allocation,
RDMA completion and QP teardown. The runner asserts that no registrations occur
inside handoff, verifies that completed RDMA bytes/spans match the declared
state, and reports the actual whole-buffer lease count and zero per-span ACKs.
It is not integrated HTTP latency or wire bandwidth. These are cold
standalone runs unless an external campaign explicitly introduces warmup;
compare them only with matching baseline runs. `kv_payload_staging_bytes=0`
describes the runner's strict tensor-transfer path, not all CPU activity in
inference: prompt input, metadata, sampling and logits can still use CPU memory.

## Tensor-copy correctness and full copy timing

With the patched CUDA RPC server running in strict native mode, run
`mcdma-native-tensor-probe "$CUDA_RPC_ENDPOINT"` on the Vulkan host with the same
native allocator/RDMA environment. It runs two seeds at 1 KiB, 4 KiB and 1 MiB,
including nonzero tensor-view offsets and 704 bytes of guards. Vulkan produces
the outgoing values, CUDA transforms them, and both GPUs reduce payload/guard
differences to scalars. CPU access initializes seeds and expected values and
reads the final verification scalars; the inter-GPU payload copies use the
inference engine's strict native tensor-copy path. `--negative` changes an
expected value and must exit nonzero.

The printed `tensor_copy_forward_us` and `tensor_copy_reverse_us` values include
the synchronized tensor-copy API call, including ready-state checks and NIC completion. Persistent
registrations are retained, and prepared ownership hooks do not add GPU work or
RPC exchanges. Producer graph execution and the consumer graph are outside these
copy-call intervals. There are only two samples per size, so these output
lines are a correctness diagnostic rather than a latency distribution.

## Persistent-MR NIC completion timing

`mcdma-native-latency` is a separate two-process benchmark using the patched
GPU backend allocations and direct verbs channel without a llama RPC server.
Start `--mode server --device CUDA0` on the CUDA host and
`--mode client --device Vulkan0` on the Vulkan host, giving both the server's
numeric data-link `--address`, selected `--port`, and a distinct new `--output`
JSON file. Both sides accept `--warmup 1000 --iterations 10000`; reverse the
roles and device names to measure the other initiator. The server announces
`MCDMA_LATENCY_READY` before waiting for its peer.

For each of 64 B, 1 KiB, 4 KiB and 64 KiB, GPU graphs generate payloads and guards,
the backend releases ownership once, and three memory regions stay registered
through the warmup and timed READ/WRITE loops. CPUs never initialize or inspect
payload bytes. Both GPUs check payloads, untouched source and guards after all
NIC operations finish and ownership returns; CPUs read only reduced scalars.
Private JSON retains every timing sample and nearest-rank median/p95/p99,
along with the batch's acquisition, registration and release costs.

These samples measure the host call that posts one work request and waits for
its NIC completion with persistent registrations. They exclude registration,
TCP descriptor exchange and GPU ownership transitions; no GPU is consuming each
individual transfer inside the timed loops. A result below 10 microseconds here
does not establish a below-10-microsecond GPU-producer-to-GPU-consumer transfer
or an inference collective with that latency.

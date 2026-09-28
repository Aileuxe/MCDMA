# Native CUDA/Vulkan inference validation, 28 September 2026

The pinned llama.cpp integration now transfers the allocations used by CUDA and
Vulkan computation directly through RDMA. Qwen3-8B tensor parallelism and a
separate direct KV handoff passed with no interhost GPU-payload staging copy.
This closes the allocator gap in the earlier
[host-staged inference report](validation-2026-09-28-cuda-vulkan.md).

The CPU still submits GPU work, posts and completes RDMA operations, handles
graph/cache metadata and samples tokens. These results do not establish
CPU-free inference or GPU-initiated networking. CUDA uses mapped pinned memory;
the GB10 still reports ordinary device-memory GPUDirect RDMA as unsupported.
The NIC accesses the same storage as the GPU kernels, but existing `cudaMalloc`
allocations are not made RDMA-capable by this integration.
This follows NVIDIA's documented
[Spark communication-buffer allocation path](https://docs.nvidia.com/dgx/dgx-spark-porting-guide/porting/cuda.html).

## Implementation and measurement scope

The [native overlay](../integrations/llamacpp/native/README.md) applies to
llama.cpp `4da6337767f973e2b4d0797e5b323d77d8565e4a` and uses private backend ABI 3.
Its [manifest](../integrations/llamacpp/native/overlay.json) records the patch
and affected source hashes. CUDA tensor storage uses `cudaHostAllocMapped`;
Vulkan uses coherent exportable DMA-BUF allocations. Whole allocations are
registered during setup, and registrations outlive all operations against them.
The selected implementation has no per-copy registration or tensor-copy RPC
handshake. Prepared Vulkan ownership barriers run with producer and consumer
graphs; graph completion establishes the readiness boundary.

The hardware is the same NVIDIA GB10/ConnectX-7 and Radeon 8060S/ConnectX-5 Ex
pair described in the [CUDA/Vulkan setup guide](cuda-vulkan.md). The Ethernet
link negotiates 100 Gb/s, while the enclosure's observed USB4 host link is
40 Gb/s. Those rates are not measured GPU transfer bandwidth. Ethernet MTU was
9000 and the RC path MTU was 4096.

The [curated results](validation-2026-09-28-native-inference-results.json)
preserve timings, counts and comparison definitions without endpoint addresses,
memory-region keys, prompts or generated token sequences. Raw samples, build
artifacts and model files are retained privately. The three clocks below
measure different operations and must not be substituted for one another.

## NIC completion latency

The direct-verbs benchmark allocated tensors through the patched GPU backends,
generated payloads and guards on the GPUs, then retained three registered
regions through each READ/WRITE batch. Each operation has 1,000 warmups and
10,000 timed samples. Both GPUs verified payloads and guards after the batch;
the CPU read only verification scalars.

Times are microseconds from posting one work request through its successful NIC
completion. Registration, descriptor exchange, GPU production/consumption and
GPU ownership transitions are outside these intervals. No GPU consumes each
individual transfer during the timed loop.

| Initiator | Bytes | READ median | READ p99 | WRITE median | WRITE p99 |
|---|---:|---:|---:|---:|---:|
| Vulkan | 64 | 3.630 | 3.750 | 4.350 | 4.430 |
| Vulkan | 1,024 | 4.109 | 4.249 | 4.740 | 4.780 |
| Vulkan | 4,096 | 5.390 | 5.470 | 5.740 | 5.790 |
| Vulkan | 65,536 | 21.970 | 22.289 | 22.959 | 23.069 |
| CUDA | 64 | 3.728 | 3.824 | 2.352 | 2.480 |
| CUDA | 1,024 | 4.176 | 4.272 | 2.560 | 2.672 |
| CUDA | 4,096 | 5.375 | 5.456 | 3.536 | 3.648 |
| CUDA | 65,536 | 22.768 | 22.880 | 17.152 | 17.408 |

All medians and p99 values through 4 KiB were below 6 microseconds. This is not
a hard latency bound: for example, one CUDA-initiated 4 KiB READ took
77.104 microseconds. It also does not establish a below-10-microsecond complete
GPU producer/consumer exchange or inference collective.

## Inference tensor-copy correctness and timing

The engine-level probe passed two seeds at 1 KiB, 4 KiB and 1 MiB, with nonzero
tensor-view offsets and 704 guard bytes per case. Vulkan generated the outgoing
values, CUDA transformed them, and both GPUs reduced payload/guard differences
to zero. A deliberately incorrect expected value failed verification.
The successful run recorded zero unplanned Vulkan ownership acquisitions.

The following are the individual synchronized tensor-copy API durations in
microseconds, including readiness checks and NIC completion. Producer and
consumer graph execution are outside the interval. There are only two cases
per size, so this is a correctness diagnostic rather than a latency distribution.

| Bytes | Vulkan-to-CUDA calls | CUDA-to-Vulkan calls |
|---:|---:|---:|
| 1,024 | 18.229, 7.610 | 8.080, 5.950 |
| 4,096 | 8.170, 8.010 | 7.120, 6.970 |
| 1,048,576 | 295.420, 294.920 | 284.110, 284.370 |

The first 1 KiB forward call exceeded 10 microseconds; the remaining 1 KiB and
4 KiB calls were 5.950 to 8.170 microseconds. These measurements include the actual
inference tensor-copy path, while the preceding table isolates NIC completion.

## Qwen3-8B tensor parallelism

The model was official `Qwen/Qwen3-8B-GGUF`, revision
`7c41481f57cb95916b40956ab2f0b139b296d974`, using `Qwen3-8B-Q4_K_M.gguf`
with SHA-256
`d98cdcbd03e17ce47681435b5150e34c1417f50b5c0019dd560e4882c5745785`.
Its 5,027,783,488-byte checkpoint was verified on both hosts.

The Vulkan client selected `Vulkan0,RPC0`, `--split-mode tensor` and
`--tensor-split 1,1`; the remote backend was CUDA. Both GPUs executed their
tensor-parallel shards. Context size was 16,384, with one sequence, full GPU
offload, FlashAttention, F16 K/V, eight CPU threads, logical batch 2,048 and
microbatch 512. The same stored prompts contained exactly 512, 2,048 or 8,192
tokens. Requests used greedy sampling, seed 42, EOS ignored and prompt caching
disabled. A 512-token/32-output warmup preceded three measured runs at each size.

Every measured run evaluated the full prompt with zero cached prompt tokens
and generated 128 tokens. First-token and full-reply values below are medians
of requesting-host HTTP wall times; decode rates come from server timings.
Model loading is excluded.

| Prompt tokens | Native first token | Native full reply | Native decode tokens/s | Earlier staged full reply |
|---:|---:|---:|---:|---:|
| 512 | 0.583 s | 7.364 s | 18.76 | 11.332 s |
| 2,048 | 2.425 s | 9.432 s | 18.13 | 16.863 s |
| 8,192 | 10.796 s | 18.783 s | 15.91 | 39.653 s |

The 8,192-token native result is 2.11 times faster than the earlier staged TP
measurement. This is a cross-campaign observation, not an isolated transport
speedup: the earlier campaign used logical batch 512, and the integration and
launch settings changed. Earlier unmodified single-GPU runs completed that
request in 6.312 s on CUDA and 14.998 s on Vulkan. Fresh matched single-GPU
baselines with the native allocators are not included here, and these results
do not demonstrate that TP=2 beats either GPU alone.

All nine native runs matched the first generated token of the corresponding
earlier CUDA and Vulkan references. Matching greedy prefixes ranged from 56
to 128 tokens. This establishes functional continuation on this prompt set,
not bit-identical output or a general model-quality result.

A `1,3` Vulkan/CUDA split shortened the 8,192-token first-token median to
9.857 s but increased the full reply to 19.644 s and reduced decode to
12.97 tokens/s. Its 512-token full reply was also slower, at 8.607 s.
The equal split remains the measured choice for this workload. A separate
graph-cache experiment passed correctness checks but increased the 8,192-token
full-reply median to 19.957 s; that experiment was excluded from the selected
overlay. Removing payload copies does not remove graph scheduling, GPU compute
or collective dependencies.

## Direct GPU KV handoff

The standalone native KV runner transferred the CUDA sender's actual GPU KV
allocations directly into the Vulkan receiver's KV allocations. There was no
intermediate state file or tensor download/upload. One 512-token run and three
8,192-token runs passed, each generating 128 continuation tokens on both hosts.

| Prompt tokens | Cold runs | RDMA KV bytes per run | KV spans | Receiver handoff |
|---:|---:|---:|---:|---:|
| 512 | 1 | 75,497,472 | 72 | 0.027884 s |
| 8,192 | 3 | 1,207,959,552 | 72 | 0.338687 s median |

All registrations were established before handoff. Every endpoint reported
zero registrations during handoff, zero KV payload staging bytes and zero
per-span control acknowledgements. Each receiver used three allocation-level
ownership exchanges and re-evaluated only the final prompt token to reconstruct
logits, rather than repeating prefill.

| Stage | 512-token run | 8,192-token median |
|---|---:|---:|
| CUDA cold setup | 3.468 s | 3.576 s |
| Vulkan cold setup | 3.124 s | 3.097 s |
| CUDA prefill | 0.273 s | 4.972 s |
| Handoff, sender clock | 0.026670 s | 0.339536 s |
| Handoff, receiver clock | 0.027884 s | 0.338687 s |
| Vulkan final-token reconstruction | 0.026 s | 0.032 s |
| Vulkan decode | 2.839 s | 3.479 s |

Every donor/receiver pair and every comparison with the earlier local references
matched the first generated token. The 512-token donor and receiver shared
118 leading tokens. The 8,192-token pairs shared 58 leading tokens.
Against all three historical references at that length, the CUDA donors matched
128 tokens of the CUDA references and Vulkan receivers matched
58 to 128 tokens of the Vulkan references. These are functional continuation
checks with backend-dependent numerical differences, not a claim of universally
identical output or general model-quality equivalence.

Readiness precedes the handoff timer. Handoff includes metadata processing,
allocation-level ownership transitions, RDMA completion and QP teardown;
model loading, donor prefill and decoder generation are outside it. The
sender/receiver handoff times overlap and must not be added together. These
are cold standalone measurements, not wire bandwidth or integrated HTTP
request latency. Cold setup includes connection waiting, model/context
allocation and registration. Every stage is retained separately in the JSON;
the earlier unmodified single-GPU HTTP results are not matched baselines for
this runner.

The final 8,192-token pair additionally captured exit code zero from both
processes after resource teardown. A mismatched model-ID control failed before
model loading on both endpoints, with exit code one and zero RDMA transfers or
payload bytes. The receiver reported an identity mismatch and the sender
reported the resulting control disconnection.

## Reproduce and interpret

Follow the [native setup and runners](../integrations/llamacpp/native/README.md)
with the exact pinned source, model hash and matching ABI 3 libraries on both
hosts. Use the tensor probe and its negative control before model measurements.
Keep the selected interfaces, GPU identities, source/binary hashes and raw
results privately, and compare matching clocks and configurations.

The Python mailbox adapter and file-based KV tools remain explicitly
host-staged experiments. Installing the macOS driver alone does not enable
this Linux integration, and no fresh macOS installation is claimed here.

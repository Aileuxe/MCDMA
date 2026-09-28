# CUDA, Vulkan and model validation, 28 September 2026

CUDA kernels and Vulkan shaders exchanged and verified payloads through real
ConnectX RDMA without CPU payload staging. The application/daemon test required
a Vulkan-owned DMA-BUF: RADV rejected importing the daemon's POSIX shared-memory
file. Separate Qwen3-8B runs completed CUDA-prefill/Vulkan-decode handoffs and
genuine two-device tensor parallelism through the explicitly host-staged
llama.cpp adapter.

The small-model results favor one CUDA host for speed. The handoff reduces the
measured stage sum relative to Vulkan alone, while this first TP=2 adapter is
slower than either host alone. These results do not establish an inference
speedup from the no-staging GPU allocator path; the model adapter does not use
that allocator.

## Hardware and software

| Component | CUDA host | Vulkan host |
|---|---|---|
| Processor/GPU | NVIDIA GB10 in ASUS GX10 | AMD Ryzen AI Max+ 395, Radeon 8060S |
| GPU stack | NVIDIA driver 580.178.04, CUDA 13.0 | Mesa 26.0.8 RADV, Vulkan loader 1.4.341 |
| OS/kernel | Ubuntu 24.04.5, 7.0.0-1019-nvidia, aarch64 | Ubuntu 26.04.1, 7.0.0-34-generic, x86-64 |
| RDMA card | ConnectX-7, firmware 28.45.4028 | ConnectX-5 Ex, firmware 16.35.8002 |
| Host attachment | Built-in ConnectX-7 | Thunderbolt enclosure on the Strix USB4 host |

The link negotiated 100 Gb/s Ethernet, both selected interfaces had MTU 9000,
and tests used IPv4-mapped RoCE v2 GIDs with RC path MTU 4096. The enclosure link
reported two 20 Gb/s lanes in each direction, a 40 Gb/s host link; this does not
turn the NIC's 100 Gb/s port rate into a measured application bandwidth result.
No driver, firmware, kernel or host-network setting was replaced during this
validation. Vulkan validation layers were installed for the final GPU checks.

## GPU memory and transport checks

The GB10 reports `gpu_direct_rdma=0` and `dma_buf=0` for CUDA device-memory
capabilities. Ordinary registration of a `cudaMalloc` allocation returned
`EFAULT`; mapped host allocation/registration worked. This agrees with
[NVIDIA's documented GB10 restriction](https://nvidia.custhelp.com/app/answers/detail/a_id/5780/kw/hdr/related/1).
An exploratory HIP device-allocation registration also returned `EFAULT`.

The public direct-verbs verifier passed with CUDA first and Vulkan first, at
1024 and 4096 bytes, using different seeds. Each run performed WRITE and READ
from both endpoints, and both GPUs checked all 24,576 registered bytes, including
guards and unused slot tails. A mismatched-seed run failed with exactly 4096 GPU
mismatches. The final Vulkan runs used foreign queue-family ownership transfers
and `VK_LAYER_KHRONOS_validation`, with no validation errors.

The application/daemon check separately passed in both direct-reply and
pull-reply modes. In each mode, twelve calls cycled through 64 B, 4 KiB and
1 MiB four times. The Vulkan client GPU generated requests; the CUDA service GPU
checked them and generated different replies; the Vulkan GPU checked the replies
and its unchanged requests. Every check covered the entire 4 MiB-minus-control-page
payload span. Both applications exited successfully, and the connect daemon
confirmed MR/QP teardown before the Vulkan allocation was released.

That check uses one 8 MiB Vulkan-owned coherent allocation, exported as DMA-BUF
to a separate daemon process, with two 4 MiB MRs. The daemon initializes only
the control pages. CUDA imports the listen daemon's existing shared-memory
payload spans. There is no application CPU payload fill, comparison or staging
copy; CPUs still drive control words, synchronization and completions.
Direct replies exercise RDMA WRITE into the Vulkan allocation; pull replies
exercise RDMA READ into it. Both final runs completed without Vulkan validation
errors.

The backing-type investigation matters for reproduction. Anonymous 24,576-byte
and 4 MiB-minus-page allocations imported successfully into RADV, while small
and large file-backed mappings, including a private mapping after faulting its
pages, failed. `DRM_IOCTL_AMDGPU_GEM_USERPTR` returned `EPERM`; foreign-host-pointer
import was not advertised. The accepted route allocates through Vulkan first,
exports a standard coherent host-visible memory type, and registers that FD
with the NIC. It neither changes RADV's import restrictions nor enables extra
AMD coherent-memory features.

These are bounded correctness checks. They do not establish device-local VRAM
registration, zero CPU activity, GPU/NIC simultaneous access, hot removal,
long-duration fault recovery or sustained GPU-buffer bandwidth.

## Model method

Both hosts used the official [Qwen3-8B Q4_K_M GGUF](https://huggingface.co/Qwen/Qwen3-8B-GGUF/tree/7c41481f57cb95916b40956ab2f0b139b296d974),
5,027,783,488 bytes, SHA-256
`d98cdcbd03e17ce47681435b5150e34c1417f50b5c0019dd560e4882c5745785`.
The hash was verified on both hosts and on the local archival copy.
llama.cpp was unmodified at
`4da6337767f973e2b4d0797e5b323d77d8565e4a`, built separately for CUDA and Vulkan
with RPC support.

The HTTP runs used one slot, context 16,384, batch/microbatch 512, eight CPU
threads, full GPU layer offload, FlashAttention, and F16 K/V caches. Inputs were
the exact same stored 512/2048/8192-token arrays, produced by the science prompt
template in `measure_inference.py`; requests generated 128 tokens with greedy
sampling, seed 42 and EOS ignored. Each single-host and TP configuration had a
32-token warmup and three measured runs per length. A successful 512-token
handoff exercised that path before its separate nine-run campaign. Baselines and TP disabled prefix reuse and
reported all prompt tokens evaluated with zero cached tokens. These are a
bounded repeated-prompt experiment, not a broad quality or serving benchmark.

For handoff, CUDA evaluated the prompt and saved the slot state. The pinned
server samples once even when `n_predict=0`; the saved state still contains
exactly the input-token count. `kv_file.py` transferred the state through MCDMA
with SHA-256 checked at both ends, then the Vulkan server restored it and
decoded. All nine handoffs restored the requested token count and evaluated
only one prompt token, with 511/2047/8191 tokens reused from the imported cache.
That last-token evaluation reconstructs logits rather than repeating prefill.

The KV file path and RPC tensor path both serialize through host memory. The
GPU application test above is separate. Model-loading traffic, compilation and
shader warmup are not included in the reported request timings. The configurations
ran sequentially, not in a randomized long-duration campaign.

## Prefill/decode results

Values are medians of three runs, in seconds. Local and TP timings are measured
HTTP request wall times. Handoff values are sums of serial stage timings:
prefill plus save, export preparation/hash, verified file transfer, restore,
and decoder time. They exclude SSH orchestration and process launch, so they
are not the latency of an installed integrated serving API.

| Prompt tokens | CUDA first token | Vulkan first token | Handoff first-token stage sum | CUDA full reply | Vulkan full reply | Handoff full-reply stage sum |
|---:|---:|---:|---:|---:|---:|---:|
| 512 | 0.162 | 0.488 | 0.418 | 3.130 | 3.439 | 3.298 |
| 2048 | 0.636 | 2.093 | 1.578 | 3.731 | 5.172 | 4.586 |
| 8192 | 2.735 | 11.422 | 5.792 | 6.312 | 14.998 | 9.291 |

| Prompt tokens | State-file bytes | Verified file-transfer median |
|---:|---:|---:|
| 512 | 75,506,572 | 0.132 s |
| 2048 | 302,023,564 | 0.594 s |
| 8192 | 1,208,091,532 | 1.899 s |

File-transfer wall time includes CPU copies, file writes, hash verification and
fsync, and is not a wire-bandwidth measurement. At 8192 tokens the handoff's
full-reply stage sum was about 38% below Vulkan alone, while CUDA alone remained
faster than the split. Decode speed alone was similar on the two single hosts,
around 43 tokens/s at 512 tokens of context and 35.5 tokens/s at 8192.

## Tensor parallelism across both GPUs

The two-device run explicitly selected `Vulkan0,RPC0`, `--split-mode tensor`
and a 1:1 tensor split, with the same model, cache types, batch settings and
HTTP requests. This is the runtime's tensor-parallel mode, not a layer split.
The remote RPC server selected CUDA0, executed GPU graphs, and the local Vulkan
backend executed its shards. The MCDMA proxy carried the interhost RPC frames;
both native RPC capability handshakes were cleared so the loopback sockets could
not upgrade to a separate native RDMA transport.

| Prompt tokens | TP=2 first token | TP=2 full reply | TP=2 decode tokens/s |
|---:|---:|---:|---:|
| 512 | 1.818 s | 11.332 s | 13.35 |
| 2048 | 7.261 s | 16.863 s | 13.20 |
| 8192 | 29.787 s | 39.653 s | 12.87 |

TP=2 worked but was slower than either single host for this model. The current
adapter uses host serialization and several acknowledged mailbox exchanges per
RPC; this is not a GPU-native collective implementation. The aggregate proxy
record, which also includes loading, warmups and a preliminary smoke run,
contained 198,687 graph-compute commands and nonzero MCDMA byte counters in both
directions, with no fallback or proxy error. Those aggregate counters are not
an inference-only throughput result.

All handoff and TP outputs contained 128 tokens, and their first token matched
both local references for all three prompt lengths. Longer greedy outputs were
not universally identical: matching prefixes varied from 56 to 128 tokens in
these comparisons. The two local GPU backends also differed on longer outputs.
This is evidence of working cache/transport execution on this prompt set, not
proof of model-quality equivalence or general numerical accuracy.

## Reproduction and evidence boundaries

The [machine-readable result summary](validation-2026-09-28-cuda-vulkan-results.json)
contains the medians, clock definitions and token-prefix comparisons without
private endpoint identifiers or raw memory-region descriptors.

Use the [CUDA/Vulkan setup guide](cuda-vulkan.md),
[GPU correctness tools](../benchmarks/gpu-rdma/README.md), and
[pinned model adapter and measurement tools](../integrations/llamacpp/README.md).
Preserve the selected devices/GIDs, binary and shader hashes, raw samples and
GPU/backend identities privately. The source of record, model checkpoint,
remote build artifacts and test evidence were retained locally; raw logs are
not published because they can contain machine details and live MR capabilities.

The model measurements used the protocol-1 daemon with the Linux GCC formatting
fix and the proxy's TCP_NODELAY correction. Later FD ownership and teardown
hardening did not change that ordinary mailbox data path. Final GPU correctness
and daemon lifecycle checks exercised the hardened source. This report does not
claim a new macOS driver installation or validation of a different OS/GPU stack.

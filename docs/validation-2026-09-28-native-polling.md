# Native Vulkan completion polling — 28 September 2026

The native backend now checks GPU completion more frequently: one group of ten
CPU pause instructions between fence queries instead of 100 groups. A matched
same-build control confirms a modest improvement in median GPU-to-GPU latency,
but the full producer-to-consumer interval still exceeds 10 microseconds and
these tests show no TP=2 inference speedup.

This changes only host fence observation. GPU completion checks, error handling,
FOREIGN ownership barriers, retained NIC registrations and direct tensor memory
access remain intact. CPU submission and polling remain; there is no CPU copy of
the interhost tensor payload. More frequent polling may use more CPU and power.
The ordinary non-MCDMA path retains its previous backoff.

## Attribution and selected change

A private, separately built timing probe classified 2,200 tiny producer graphs
and 2,200 tiny consumer graphs, each containing two nodes. Mean producer sync
was 69.561 µs, including 68.554 µs in the final-fence polling loop, 0.389 µs in
empty fence submission and 0.116 µs in graph cleanup. Consumer sync averaged
28.846 µs, including 27.899 µs in polling, 0.344 µs in empty submission and
0.110 µs in cleanup. Neither tiny class used the blocking almost-ready wait.

Each old backoff averaged 12.810 µs. These nested instrumentation timings must
not be added together and include measurement overhead. They identify coarse
completion observation as avoidable; they do not distinguish GPU execution,
queue scheduling, power transitions or ownership/cache maintenance within the
remaining wait. Instrumentation is absent from the selected runtime.

The overlay retains llama.cpp base `4da6337767f973e2b4d0797e5b323d77d8565e4a`
and private ABI 3. Its new patch SHA-256 is
`baadaa61bee089bc1bbb94c7975a67fb16999e2d368f16c2b7e85de3e3ebd633`;
selected `ggml-vulkan.cpp` SHA-256 is
`c47df744bfa22c93c275d1313c2ad70e042185fd57bcfe0f9a7bb7da417ccd23`.
All other overlay source hashes are unchanged. Fresh application and complete
manifest verification passed, and the selected Vulkan library built on Strix.

## Matched GPU-to-GPU measurement

The existing end-to-end harness and its timing boundary are unchanged: one
Vulkan-host steady clock spans producer submission, GPU completion, native RDMA
copy and peer consumer GPU completion, with an already-ready receiver. GPU
verification/rearming occurs between samples and outside that primary clock.
The hardware, 100 Gb/s NIC link, 4 KiB path MTU and software stack are those in
the [original report](validation-2026-09-28-gpu-e2e.md); the AMD GPU power policy
remained `auto` during these comparisons.

Two native-default runs bracketed a same-library control using
`MCDMA_VK_FENCE_PAUSE_GROUPS=100`. Each case had 100 warmups and 1,000 measured
iterations. All values below are per-run medians in microseconds.

| Payload direction | Bytes | Old backoff control | New default, run 1 | New default, run 2 |
|---|---:|---:|---:|---:|
| Vulkan → CUDA | 1,024 | 95.296 | 89.977 | 89.877 |
| CUDA → Vulkan | 1,024 | 55.418 | 53.019 | 52.288 |
| Vulkan → CUDA | 4,096 | 96.967 | 92.762 | 90.937 |
| CUDA → Vulkan | 4,096 | 57.778 | 56.128 | 53.698 |

The observed median reduction is 1.650–6.030 µs across these comparisons.
Tails do not consistently improve: default-run p99 values span 72.748–166.605
µs and observed maxima reach 256.701 µs. Every positive run still fails the
maximum-below-10-µs gate with exit 2, while GPU correctness and local cleanup
pass. No NIC-only figure is substituted for this interval.

The two default runs plus a Vulkan-validation run cover 8,400 measured positive
exchanges and 840 warmups, with changing GPU-generated payloads, offset views
and 704 guard bytes per endpoint. The control adds 4,000 measured exchanges
and 400 warmups. Planned ownership acquisitions matched the exchange counts;
there were zero cold acquisitions and zero payload-staging bytes. The negative
wrong-reference control fails with exit 3 and clean local cleanup. No Vulkan
validation error was logged. The harness does not acknowledge remote teardown,
so local cleanup is not presented as a remote exit-zero acknowledgement.

## Model checks and rejected settings

Qwen3-8B Q4_K_M used the same model identity, stored prompts, equal Vulkan/CUDA
split, batch sizes and request settings as the
[native inference report](validation-2026-09-28-native-inference.md).
Each campaign used a 512-token/32-output warmup, then three uncached requests
at 512 and 8,192 input tokens, generating 128 tokens each.

| Configuration | 512-token full reply | 8,192-token full reply |
|---|---:|---:|
| Same new overlay, old 100-group backoff | 7.361 s | 18.857 s |
| New overlay, default 1 group | 7.408 s | 18.864 s |

These are medians of full HTTP request wall time, excluding model loading.
All requests completed with the expected token counts and functional
continuations; greedy output is not generally bit-identical across repeats.
The new default was 0.6% slower at 512 tokens and within 0.1% at 8k in this
small comparison; the communication microbenchmark improvement must not be
called an inference speedup.

Temporary peak GPU power policy improved the tiny forward communication case
but increased the new-default 8k model median to 19.970 s; the original `auto`
policy was restored after each bounded test. A 1:3 Vulkan/CUDA split took
19.559 s at 8k and 8.572 s at 512, so the equal split remains selected.
Changing the Vulkan allocator to ordinary coherent device-local memory passed
GPU/RDMA correctness but did not consistently improve latency and was rejected.
Neither experimental allocator nor a power-policy change is included.

Set `MCDMA_VK_FENCE_PAUSE_GROUPS=100` before process startup to restore the old
backoff. Both this setting and `GGML_MCDMA` are startup-only; the override accepts
decimal 0 through 100 and is ignored outside native mode. The source change is
limited to observation granularity, and resolving the remaining end-to-end
latency requires further work on GPU submission/completion, not a faster claim
for the already measured RDMA link.

[Curated results](validation-2026-09-28-native-polling.json) retain all reported
case statistics and model wall times; raw logs, private instrumentation and
rejected experiments remain outside publication.

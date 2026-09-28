# CUDA/Vulkan GPU producer-to-consumer validation

GPU-produced data was transferred through RDMA and consumed by the other GPU
in both directions, without CPU payload staging. The selected native runtime
passed 4,000 measured exchanges and 400 warmups at 1 KiB and 4 KiB, with changing
payloads and GPU verification after every exchange. This establishes the full
GPU producer, RDMA transfer and GPU consumer path on the tested pair.

The measured producer-to-consumer medians were 58 to 98 microseconds with an
already-ready receiver. The requested 10-microsecond gate failed. Functional
GPU-buffer RDMA is verified; that latency target is not.

## Path and clock

The test used the NVIDIA GB10/ConnectX-7 and Radeon 8060S/ConnectX-5 Ex pair from
the [native inference report](validation-2026-09-28-native-inference.md), with
the selected pinned llama.cpp overlay and private backend ABI 3. CUDA compute
uses mapped pinned allocations and Vulkan uses coherent exported DMA-BUF
allocations. The NIC transfers those same registered tensor allocations;
payload allocations and their registrations persist throughout each case.

The Vulkan host coordinates both directions with one `steady_clock`.
`producer_to_consumer_ready_receiver_us` starts immediately before producer
graph submission and stops after the receiving GPU's consumer graph has
completed. It includes producer submission/completion, the completed native
tensor copy, consumer submission/completion, required GPU ownership barriers
and RPC/control overhead. No clocks from different machines are subtracted,
and no round-trip time is divided by two.

The receiver is prepared before the producer starts. GPU verification,
reduced-scalar readback and rearming for the next receive occur outside that
primary interval and are timed separately. `full_iteration_cycle_us` includes
producer planning, the primary interval and verification/rearming. Fixture
setup, cold registration and final cleanup are outside both per-iteration
clocks. These are small GPU graph operations, not a model collective or a
measurement of model throughput.

## Measured baseline

Each size/direction case had 100 warmups followed by 1,000 samples. Values are
microseconds; p95/p99 use nearest rank and the median averages the middle pair.

| Producer to consumer | Bytes | Median | p95 | p99 | Maximum |
|---|---:|---:|---:|---:|---:|
| Vulkan to CUDA | 1,024 | 97.277 | 123.956 | 136.385 | 239.121 |
| CUDA to Vulkan | 1,024 | 57.758 | 67.357 | 83.997 | 101.557 |
| Vulkan to CUDA | 4,096 | 98.307 | 124.345 | 137.345 | 149.825 |
| CUDA to Vulkan | 4,096 | 59.528 | 71.748 | 88.707 | 127.246 |

The stage and complete-cycle medians show the clock boundaries:

| Direction | Bytes | Producer submit and complete | Tensor copy | Consumer submit and complete | Verification and rearm | Full iteration |
|---|---:|---:|---:|---:|---:|---:|
| Vulkan to CUDA | 1,024 | 73.498 | 9.539 | 14.190 | 270.176 | 368.107 |
| CUDA to Vulkan | 1,024 | 19.060 | 4.260 | 34.399 | 284.941 | 343.018 |
| Vulkan to CUDA | 4,096 | 73.397 | 10.639 | 14.320 | 266.936 | 363.872 |
| CUDA to Vulkan | 4,096 | 19.309 | 5.660 | 34.509 | 288.645 | 348.373 |

Producer and consumer totals were combined per sample before calculating their
medians. Medians of separate stages need not sum to the median total. These
host-observed stages include API, scheduling and completion-reporting work;
they are not GPU-only kernel durations.

The gate required every case's observed maximum to be strictly below
10 microseconds. It returned exit code 2 with `correctness_passed: true` and
`latency_gate_passed: false`. The observed distributions are neither a hard
worst-case guarantee nor a hardware lower bound. Scheduling and dispatch
experiments did not establish a material improvement and were not selected;
the baseline runtime is retained.

## Correctness and validation controls

The GPU producer increments a changing F32 payload each iteration. The remote
GPU applies a scale and bias, then independent GPU references and reductions
check the payload and guards. Transfers use a 320-byte offset and 704 guard
bytes per endpoint. The CPU reads only four-byte error reductions after the
timed consumer completes. The baseline also asserts that those scalar ranges
are disjoint from the transferred payload.

All positive payload and guard checks passed. Vulkan recorded 4,400 planned
acquisitions, zero unplanned acquisitions and 4,400 consumer barriers. Its two
host barriers drained the final unused reverse receive preparations during
cleanup. The client reported successful local teardown.

A wrong-reference control failed on the first 1 KiB warmup with consumer
reductions `[256, 0, 0]`, while all producer reductions stayed zero. It returned
exit code 3, completed local cleanup and left the latency gate unevaluated.
This distinguishes an actual GPU data mismatch from a latency failure.

An earlier supplementary run passed 100 samples per case after 20 warmups;
it did not report the later scalar-layout assertion field, so that assertion
is not inferred from its output. A final run of the shipped harness and
restored selected runtime, with Vulkan validation enabled, passed another
1,000 samples per case after 100 warmups, including the scalar-layout checks.
It reported no Vulkan validation errors and again returned exit code 2 solely
for the latency target. Validation-run timings are retained in the
[curated JSON](validation-2026-09-28-gpu-e2e-results.json) and are not pooled
with the unlayered baseline above.

## Provenance, limits and reproduction

The baseline harness source hash is
`6f2775f6becf1cf0f59e3fde36da8e6e7924d76375bb75e6e81ce682e0ed7319`.
The shipped harness hash is
`d6768b229e135a9baa34c38fc8ac2692f22c3420e4030a52c8d41f1882267001`.
Their only difference is JSON metadata: persistent client graph objects are
distinguished from persistent payload allocations/MRs, and graph UID zero and
disabled RPC graph reuse are stated explicitly. Timing and correctness logic
are unchanged. Verification graphs run between samples and can change backend
graph-cache state.

Follow the [end-to-end runner instructions](../integrations/llamacpp/native/README.md#gpu-producer-to-consumer-timing)
with matching source, libraries, GPU placement and RDMA configuration. The
curated JSON contains recomputed statistics and verdicts; raw samples, endpoint
logs, exit records and binaries remain private. The RPC protocol has no remote
teardown acknowledgement, so local cleanup does not substitute for retained
server cleanup evidence.

This verifies native GPU-buffer RDMA on compatible allocations. CPU scheduling,
control metadata, NIC submission and completion handling remain. It does not
enable GPUDirect access to arbitrary existing `cudaMalloc` storage or establish
CPU-free networking. The [NIC-only latency results](validation-2026-09-28-native-inference.md#nic-completion-latency)
retain their narrower clock and must not be presented as this full GPU exchange
or as an inference collective's latency.

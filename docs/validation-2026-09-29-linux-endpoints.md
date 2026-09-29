# Linux endpoint validation, 29 September 2026

The CLI 1.2 candidate built and installed its native stock-verbs peer on Linux x86-64 Strix Halo and Linux ARM64 NVIDIA GB10, then discovered the live ports/GIDs, resolved exact reciprocal neighbours using normal kernel NDP, and passed WRITE and READ from both endpoints. No GPU/NIC-register access, private driver, root authentication, network service installation or security-policy change was required.

The final source was checked with 43 Node CLI tests, nine tests compiling the actual peer source against simulated verbs, four packaging checks, and a real macOS build of the updated peer. The existing Mac/native device guard and provider-mode validation remain intact. A real Strix/Mac pairing still requires physical validation; the Mac route reuses existing MCDMA discovery, mapping, configuration and transfer tests.

## Measured NIC completion latency

These are medians in microseconds for CPU posting through application-observed NIC completion on the initiating host, at queue depth one and RDMA path MTU 1024. Each cell has 1,000 measured samples after 100 warmups, with real payload verification; all 8,000 measured samples are retained privately. GPU production/consumption, allocation/registration and SSH bootstrap are excluded. GPU activity was uncontrolled.

| Initiator | Payload | WRITE median | READ median |
|---|---:|---:|---:|
| Strix | 1 KiB | 4.510 | 3.980 |
| Spark | 1 KiB | 2.704 | 4.640 |
| Strix | 4 KiB | 5.710 | 5.220 |
| Spark | 4 KiB | 3.168 | 5.024 |

The [curated result](validation-2026-09-29-linux-endpoints-results.json) contains per-operation min, median, p95, p99, maximum and mean, including tails. These are neither one-way network measurements nor GPU-to-GPU or inference latencies. The separate resident CUDA/Vulkan experiment measured about 10.4 microseconds for a verified 1 KiB GPU-to-GPU round trip; it is not an installed inference guarantee of this CLI feature.

## Packaged behaviour and limits

`mcdma linux add`, `install`, `link`, `status` and `test` expose the new workflow; [the Linux endpoint guide](linux-endpoints.md) has commands. Native build and ABI markers avoid copying an ARM64 binary to Strix. Discovery selects an observed link-local RoCE v2 GID on the actual RDMA port/network device. Tests validate both stock hardware roles, exact descriptors, four byte-verifying operations, every latency trace, binary hashes and cleanup exits. Changed boot, GID, peer tool or requested test parameters invalidate cached verification.

The initial hardware attempt exposed the distinction between the stock verbs API's Mellanox IEEE vendor ID and the Mac provider's PCI vendor ID; the Linux-specific guard now accepts the observed Mellanox IEEE value while leaving the Mac guard unchanged. The pre-release checks cover missing or ambiguous GIDs, neighbour mismatches, wrong hardware-role evidence, stale tools/settings, missing samples and teardown failures. Failed tests cannot be reported as accepted hardware passes.

The CLI also bundles canonical peer/guard source for an installed package to compile on the destination. It validates source checksums and the stock protocol marker, preserves the previous tool before replacement, and installs only in that user's libexec directory. Optional permanent configuration merges existing neighbours instead of deleting Studio/Spark entries.

This is a locally implemented and hardware-verified Linux transport candidate, not a published release or an automatic inference integration. Linux GPU-buffer wrappers and the pinned native CUDA/Vulkan inference overlay retain their separate allocation, ownership and validation requirements. No Strix/Mac performance number, CPU-free networking claim or new inference speedup is established.

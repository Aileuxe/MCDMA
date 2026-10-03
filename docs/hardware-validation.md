# Hardware validation

The default build does not enable PCI ownership. The [developer installation guide](install.md) covers setup and recovery; its portable procedure still needs a fresh-machine end-to-end check, and the original machine-specific installers are outside this tree.

For an already approved lab installation, record the exact kernel UUID, OS build, SDK, provider/client hashes, NIC identity, firmware, physical link, Ethernet MTU and RC path MTU. Resolve each interface from its actual hardware identity after every restart rather than assuming its previous enumeration.

Require native discovery with a real RoCE v2 GID, mapping-protection checks, and four-way byte verification before any timing. The `client/` programs exercise those checks; the cross-host runner requires explicit hosts, executable paths, interfaces and devices. Its `--help` output documents the arguments without supplying private lab defaults.

Begin with kernel posting and ordinary direct posting, then test each BlueFlame arm independently. Missing mode markers or a failed transfer invalidate the arm. Time only accepted configurations with identical clients, payloads, queue depth, warmups and completion boundaries, alternating order and retaining every sample.

Raw runner output contains hostnames, paths, addresses, keys used for the test's registered regions and device details. Store it in ignored `results/` or outside this repository, and review it before sharing. Historical benchmark archives are retained outside this driver repository.

A timing improvement alone does not prove the final pmap cache attribute, one-way wire latency, GPU memory access or a hardware latency floor.

The [17 September 0.1.18 report](validation-2026-09-17.md) records sustained bandwidth, two concurrent ports and client termination during measured traffic, with the exact build identity for each check. These bounded checks do not validate mapped hot removal, foreign-QPN isolation or the driver's fallback orphan reclaim on hardware.

## Additional card and peer support

The source also accepts ConnectX-4 Lx PF `15b3:1015`, contributed in
[PR #3](https://github.com/ashhart/MCDMA/pull/3). The contributor reports
bidirectional RDMA and bandwidth tests; the maintainer has not reproduced
those hardware tests. ConnectX-5 Ex `15b3:1019` remains the card used for the
published results. Other IDs are rejected and `ibv_query_device` reports
the attached card's actual identity. Offline tests cover both identities,
the checker predicate and CLI classification, not hardware equivalence.

Apple Thunderbolt RDMA peer selection from PR #3 is deferred pending
separate end-to-end validation; the CX5 setup still requires an Ethernet link.

## Thunderbolt links between Macs

`mcdma-rpcd` and `libmcdma-fabric` also run over Apple's RDMA over Thunderbolt
between two Macs, without the MCDMA kernel extension. That path is offline-tested
only, against a stub that enforces TN3205's rules: SEND and RECV on UC queue
pairs, frame-matched receives and Apple's registration limits. Qualify it on two
Macs with `rdma_ctl enable` and the Thunderbolt Bridge inactive: `rpc-echo`
through a daemon link ([link daemon](link-daemon.md#qualifying-a-link-on-hardware))
and `fabric-check` on the one-sided path ([fabric](fabric.md#qualifying-on-hardware)),
both of which check every byte. Until then, no Thunderbolt latency, bandwidth or
ordering result is claimed.

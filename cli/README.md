# mcdma

A command-line tool that takes a Mac with a Mellanox ConnectX card from "just
plugged in" to a verified RDMA fabric with DGX Sparks, and keeps it that way.
CLI 1.2 also registers stock Linux RDMA endpoints, including Strix Halo, builds
their peer tool natively and verifies Linux/Linux links without the Mac driver.
See [Linux endpoints](../docs/linux-endpoints.md) for Strix/Spark and Strix/Mac setup.
It sits on top of the driver in this repository: the documented,
hardware-validated installation route remains `docs/install.md` with
`tools/install-native.sh` and `tools/restore-rdma.py`; `mcdma` automates the
same steps and adds discovery, wiring detection, persistence and tests. Its
driver-install step has not yet had a fresh-machine hardware check, so treat
that part as beta.

No dependencies beyond Node 20 or later and the system tools it drives
(`system_profiler`, `ioreg`, `kmutil`, `ibv_devinfo`, `ifconfig`, `ndp`,
`ssh`, `sudo`).

```sh
cd cli
node bin/mcdma.js status     # or: npm install -g .   (then just: mcdma status)
```

## Commands

```
mcdma status              the seven checks, hardware, driver, Sparks, topology, links
mcdma enable              do whatever is still missing, in order; exit 3 = waiting for you
mcdma detect              which Spark port each Mac port is cabled to (real RDMA probes)
mcdma configure           addresses and neighbours on the Mac (sudo) and the Sparks (ssh), persisted
mcdma test [LINK]         RDMA transfer test with latency; --quick for a bare check
mcdma driver install|load
mcdma sparks list|add HOST [--name N]|remove ID
mcdma macs list|add HOST [--name N]|remove ID     another Mac with a card, over ssh
mcdma linux list|add HOST [--name N]|remove ID   register Linux peers, including Strix
mcdma linux install NODE                       build a native peer for x86-64/ARM64
mcdma linux link NODE/IFACE NODE/IFACE --name ID save a Linux connection
mcdma linux status|configure|test [LINK]        inspect, persist or verify Linux links
mcdma map MAC-PORT SPARK/IFACE|none               e.g. map local:mcrdma1 spark1/enp1s0f0np0
mcdma keepalive run       hold the Mac's fast platform state until Ctrl-C
mcdma monitor [--seconds N]   per-link throughput and Spark inference state
mcdma package             rebuild the driver package from a Mac with the driver installed
mcdma install-cli         put mcdma on the PATH (/usr/local/bin)
mcdma settings [get|set KEY VALUE]
```

Options: `--json` (one JSON document on stdout, progress on stderr), `-y`
(no confirmations), `-q`, `--quick`, `--studio-host HOST` (manage a remote Mac
over ssh for checks, wiring and Spark setup; its driver and its own addresses
must be handled on that Mac), `--demo` (fictional data, never touches
settings), `--no-color`.
`--ssh-config FILE` selects an existing private OpenSSH configuration for all
remote peers; no credentials are stored by that option.

Exit codes: 0 done, 1 failed, 2 usage, 3 waiting for you (driver approval in
System Settings or a restart; run `mcdma enable` again afterwards).

## What the checks cover

1. **macOS & security** — version against the driver build, Apple silicon,
   kernel-extension policy (SIP / Reduced Security), user consent.
2. **ConnectX hardware** — Mellanox PCI devices, card name, ports, PCIe link,
   the Thunderbolt enclosure the card actually sits in (matched through the
   tunnel endpoint) and the Mac port it is on, QSFP link state.
3. **MCDMA driver** — installed bundle, loaded version/UUID, approval state,
   libibverbs provider, per-port driver registry, RDMA devices.
4. **Sparks** — ssh probe of each Spark: ports, MACs, speeds, cable EEPROM
   identity, RDMA devices, GID table, neighbours, GPU, tools.
5. **Topology** — which Spark ports face a Mac (Spark-to-Spark cables are
   recognised by the cable serial both ends report), the mapping, and
   `mcdma detect`: a short real RDMA transfer on every Mac-port/Spark-port
   pair. Handles one Mac with one or two Sparks, dual-port, ring, and several
   Macs; a Spark port nobody claims is flagged as "another host?".
6. **Addresses & neighbours** — link-local addresses and static IPv6
   neighbours on both ends, persisted: a LaunchDaemon on the Mac, a systemd
   service + 30 s timer + NetworkManager hook on each Spark (the Spark entry is
   otherwise lost on every link flap).
7. **Transfer test** — 4 KiB WRITE/READ in both directions with byte
   verification and latency (median/p95) using `native-verbs-peer` on the Mac
   and `verbs-peer` on the Spark.

Everything it reads is read-only. Anything it changes is behind a command
and, on the Mac, behind `sudo`.

## JSON

`--json` prints `checks` (steps with status/items/actions), `topology`
(links with `status.configured`, `status.wiringVerified`, `status.lastTest`,
Spark-to-Spark links, orphans), `macs`, `sparks`, `pkg`, `enable` and
`settings`. `mcdma monitor --json` prints one JSON line per tick.

## Driver package

`driver/manifest.json` plus `driver/mcdma-driver-<version>.tar.gz` (kernel
extension, `libmcdma-rdmav34.so`, `mcdma.driver`, the Mac tools and the Linux
`verbs-peer`). Neither is committed; build them from a Mac that has the driver
installed:

```sh
MAC=<ssh host> BUILD_DIR=<dir of the built tools on that Mac> SPARK=<ssh host> npm run package
```

## Layout

- `bin/mcdma.js` — the command.
- `lib/engine.js` — discovery, checklist, actions, tests, the Enable flow,
  monitor loop; an EventEmitter another program can embed.
- `lib/macinfo.js`, `lib/sparks.js` — the Mac and Spark probes.
- `lib/topology.js`, `lib/checks.js` — model and checklist (pure functions).
- `lib/actions.js` — install/load/restart, boot policy, neighbour
  configuration and persistence, wiring detection.
- `lib/testrun.js` — transfer test driver, Spark tool installer, keep-alive.
- `lib/collect.js` — monitor collector; `lib/exec.js`, `lib/store.js`,
  `lib/driverpkg.js`, `lib/demo.js`, `lib/parse.js`.
- `tools/make-driver-package.sh`.
- `native-peer.json` and `tools/bundle-linux-peer.js` retain the canonical stock
  Linux peer sources for native destination builds; run `npm run bundle-linux-peer`
  after changing the peer or device guard.

Settings live in `~/Library/Application Support/MCDMA/settings.json`.

## 0.1.18 compatibility and verification

CLI 1.1.0 targets driver 0.1.18 and refuses an older package over a newer installed driver, including repair requests. A package is generated locally from the installed 0.1.18 build; no ad-hoc-signed driver archive is shipped in Git. The installer verifies every package member before replacing files and remains a supervised developer route, not a fresh-machine-validated consumer installer.

`mcdma test --quick` checks four 4 KiB transfers per link without timing. Normal `mcdma test --json` also retains complete CSV traces and their SHA-256 hashes under each result's `traces` field. Latency peers currently use exactly 1,000 samples per operation after 100 warmups; any other `test.iterations` setting is rejected. GPU activity is uncontrolled unless the CLI's managed keepalive is active, so record that condition when comparing results. Old saved tests without a driver/provider fingerprint are treated as untested.

For sustained bandwidth, build `benchmarks/mcdma_bw.c` on each host, set `tools.macBw`, `tools.sparkBw` and `tools.macChecker` to their absolute paths, and run from this source checkout:

```sh
mcdma bandwidth mcrdma1 --studio-host "$MAC_SSH" --output ../results/bandwidth-run
```

The runner defaults to WRITE/READ in both directions, 64 KiB and 1 MiB requests, queue depths 1 and 16, three repeats and one warmup. Override `--ops`, `--sizes`, `--depths`, `--qps`, `--total`, `--repeats`, `--warmup` and `--verify-bytes` as needed. It saves CSVs, binary hashes and raw logs to a new directory and stops on the first failed pair. A local Mac must also have a working key-authenticated SSH alias for this runner. The tool exists; no sustained-performance headline is claimed by this release.

Use `--settings-dir DIR` for isolated test settings, and `npm test` for offline CLI regression tests. Raw status/test output includes private hardware details and memory keys; keep it outside publications.

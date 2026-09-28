# MCDMA agent instructions

Use this file when a user asks you to prepare, install, inspect or test MCDMA. The repository is https://github.com/ashhart/mcdma. Read `README.md`, `docs/install.md` and `docs/hardware-validation.md` before changing the target Mac. Read `docs/architecture.md` before editing driver code.

For Linux CUDA/Vulkan work, read `docs/cuda-vulkan.md`, `rpc/gpu/README.md` and `docs/validation-2026-09-28-cuda-vulkan.md`. Linux uses stock mlx5/rdma-core, not the macOS kernel extension. Distinguish GPU-generated/verified communication buffers with no payload staging from the explicitly host-staged llama.cpp inference adapters. The tested RADV stack cannot import a POSIX-shm mailbox through HOST_ALLOCATION; the separate-process GPU route allocates a Vulkan DMA-BUF first and passes it to the daemon with a parent lifetime lease. Never remove a driver guard, fake a capability or silently fall back to a payload copy.

## What this project does

MCDMA supplies a native macOS kernel extension and userspace verbs provider for a ConnectX-5 Ex in a Thunderbolt PCIe enclosure. It provides RDMA between registered host memory on an Apple Silicon Mac and a Linux RDMA peer. Separate functional tests verified Metal shared buffers and CUDA mapped host allocations with GPU-generated and GPU-checked payloads, using the same storage registered with RDMA and no payload staging copy. The README latency figures still describe ordinary host buffers. Direct registration of existing CUDA device allocations, direct access to Metal private buffers and zero CPU involvement are not established. The Linux model adapter has host-staged inference checks in the 28 September report; integration of no-staging GPU communication buffers into an inference allocator remains separate. The first oMLX stage-link integration, https://github.com/jundot/omlx/pull/3869, merged on 2026-09-23; check current upstream state before describing later follow-ups. One Qwen3-4B prefill/decode hand-off is documented in `docs/disaggregated-inference.md`; it used scripts outside this repository and is not an installed integration.

The current lab recipe uses one M3 Ultra Mac Studio with 256 GB, macOS 27 build `26A428`, an OWC Mercury Helios 5S, a ConnectX-5 Ex MCX516A-CDAT and one DGX Spark connected by a Mellanox MCP1600-C001E30N QSFP28 DAC negotiating 100GBASE-CR4 with RS-FEC, validated 2026-09-15. The pooled headline latency table was measured earlier at 40 Gb/s. The Thunderbolt 5 PCIe tunnel, not the 100 Gb/s port, bounds throughput, with sustained 0.1.18 measurements of about 50.6 Gbit/s into Studio memory and 29.4 Gbit/s out for Studio-initiated READ and WRITE. Individual CLI transfers and bounded concurrent transfers on both links of a two-Spark/one-Studio setup passed; see `docs/validation-2026-09-17.md` for exact build identities and limits. These are host-buffer measurements, not GPU-buffer or inference throughput.

## Prepare everything possible before asking the owner to act

1. Work from the user's chosen checkout or clone the repository into a new directory. Inspect the existing directory before cloning; do not overwrite someone else's checkout or reset uncommitted work. Record the commit and working-tree state. Read downloaded scripts before executing them.
2. Identify the actual target Mac and Linux peer. The agent's own execution host may be a different MacBook. Use already authorized connections; ask only for missing host selection or access details. Never copy SSH private keys or credentials into this repository.
3. Inspect the target Mac with read-only commands: `sw_vers -buildVersion`, `uname -m`, `xcode-select -p`, `xcrun --sdk macosx --show-sdk-version`, `rdma_ctl status`, `csrutil status`, `csrutil authenticated-root status`, `kmutil showloaded --list-only --variant-suffix release`, `ioreg -r -c MCDMACX5Native -l`, and `ifconfig -a`. Inspect the physical enclosure/NIC in System Information if attachment is uncertain. Inspect `systemextensionsctl list` for an older MCDMA DriverKit installation before attempting PCI ownership.
4. Confirm build `26A428`, the macOS 27 SDK, Python 3 and Apple RDMA headers/library for this recipe. The source's earlier beta allowance is not evidence that a fresh beta install follows the same recipe. Do not weaken the build allowlist or replace Apple's RDMA library to make a check pass. If an SDK or matching KDK is missing, identify the exact download and sign-in action required; do not redistribute Apple SDKs or kernel collections.
5. Run `python3 tools/build.py test`, then `python3 tools/build.py native` on a suitable development Mac. Preserve command output privately. Inspect generated version, architecture and dependencies; an offline test is not hardware acceptance.
6. Prepare the enabled copy in `local/install/` using the installation guide, set the three documented personality flags, ad-hoc sign the copy and verify its signature. Do not alter the default-disabled build as a shortcut. Preserve any pre-existing candidate before making a new one. Record its UUID and provider SHA-256, then run `bash tools/install-native.sh --check` on the target Mac. Inspect the installer before invoking it.
7. On the peer, identify the CX7 port wired to the Mac using `rdma link`, `ip -br link`, `ibv_devinfo` and its GID attributes. Leave the separate inter-Spark link alone. Build `peer/verbs_peer.c` with the documented Linux dependencies. Determine the actual MAC-derived link-local GID and RoCE v2 index rather than assuming the lab's index or device names.
8. Assemble the exact commands for this owner's observed interfaces and paths in an ignored `local/` file. Use `tools/restore-rdma.py --dry-run` to validate the address plan without changing the host. The Mac and Linux peer both need static neighbor entries. The Mac's `mcrdmaN` interface cannot carry ordinary management traffic.

Run independent read-only checks and preparation while waiting for owner input. Do not claim that the driver is installed, loaded, active or measured when only a candidate exists. If hardware or SDK access is missing, finish the independent preparation and state precisely which checks remain unavailable.

## Owner actions and installation

Respect the user's authorization already given in the current session. Preparing source, building, inspecting and making a local candidate need no repeated permission request. Recovery changes, extension approval and administrator authentication may require the owner to act physically or in macOS UI; never ask them to send a password in chat.

Before installation, provide a short, specific readiness report:

- What you found: target OS build, enclosure/NIC attachment and whether the expected SDK and driver are present.
- What you prepared: checkout revision, passing checks, signed local candidate, recorded UUID/provider hash and selected peer port.
- Exactly what the owner must do next, omitting steps they have already completed and you have verified.
- How to reverse the installation, using the guide and the backup location printed by the installer.

For this development recipe, Recovery actions are to select Reduced Security, allow user-managed kernel extensions, run `rdma_ctl enable`, and apply the documented separate `csrutil disable` development exception. Leave authenticated-root protection enabled. Explain the reduced protection and restoration procedure before asking for the security change. Do not claim those exceptions are requirements for Apple's built-in RDMA or a properly signed production driver.

Once installation is requested and prerequisites are satisfied, use `sudo /bin/bash tools/install-native.sh --install` through the user's authorized terminal/session. The script preserves the previous MCDMA installation. Let the owner enter any administrator password locally. Follow macOS's actual approval/restart request; do not repeatedly reinstall while approval is pending. Never create broad passwordless-sudo rules to make setup easier.

Stop RDMA jobs and follow the guide's shutdown/disconnection steps before changing a driver or attachment. Do not unload a live experimental driver or remove a card with mapped pages as part of routine setup. Do not modify vendor firmware or Apple's built-in Ethernet driver. Do not reboot a remotely used machine without the owner's authorization.

## Resume after the owner returns

1. Recheck the loaded native version and UUID against the candidate you prepared. A zero installer exit or a finished reboot does not prove the intended driver is running.
2. Resolve `mcrdmaN` again by its actual MAC; names can change across restarts. Restore the temporary Mac address/static neighbor and the reciprocal Linux static neighbor using the guide. Run the native checker with `--require-gid`.
3. Inspect the wired CX5 port for an active RoCE v2 GID. Unused Thunderbolt `rdma_enN` devices and an unplugged second CX5 port may remain down. Do not report failure or success based on an unrelated port.
4. Run four-way byte-verifying tests in kernel mode, then direct mode, then BlueFlame-64 mode, using the explicit arguments in `docs/install.md`. Require the runner's success result and requested mode markers. Stop and preserve the error if transfer, completion, cleanup or identity checks fail.
5. Only after correctness passes, and if the user requested performance testing, use the README's latency command for both initiators, with fresh output filenames, 1 KiB and 4 KiB payloads and repeated runs. Keep all samples and report median and tails. This test does not measure sustained link bandwidth; do not invent a throughput figure from latency, the 100 Gb/s port rate or TCP `iperf3` results. Report the negotiated link rate you observe rather than assuming it.

## Optional GPU keepalive

Read `docs/gpu-keepalive.md` before trying to reproduce the GPU-active headline figures. Those measurements used lab driver 0.1.17; the current source and installation recipe target 0.1.18. Do not bump versions, install a different driver or claim equivalent measured performance merely to match the banner.

Build `client/fabric_keepalive.swift` with the documented Swift command on the target Studio. The helper is an ordinary Metal process and requires no root, Recovery change, driver knob or reboot. Prefer a bounded duration for a smoke check; run continuously only for the requested measurement and stop it afterward. Do not install an autostart service without a request. Record GPU-active versus idle conditions, keep the RDMA path MTU at 1024 for the headline reproduction, and compare matching off/on runs. GPU activity consumes resources and does not prove zero CPU use or an inference speedup.

## Completion report

Say whether the outcome is preparation only, installed, loaded, port active, byte-verified RDMA or measured latency. Give the exact remaining owner action if blocked. Distinguish physical attachment problems, missing GIDs, OS approval, unsupported builds and transfer failures rather than calling all of them a driver problem.

Record private setup details and raw logs under ignored `local/` or `results/`, never in tracked documentation. Raw test output can contain hostnames, memory-region addresses and access keys. Do not publish it without a separate review.

## The mcdma command

`cli/` contains `mcdma`, a Node command-line tool over one engine (`cli/lib/engine.js`). `mcdma status --json` reports the seven setup checks, discovered hardware, driver state, Sparks, topology and link state; `mcdma enable` runs the remaining steps and exits 3 when the owner must approve the driver or restart. It reads the same registry, `kmutil`, `ibv_devinfo`, `ifconfig` and `ndp` sources listed above and probes Sparks over ssh. Its driver installation route has not had a fresh-machine hardware check; prefer `tools/install-native.sh` for the validated route and treat `mcdma driver install` as beta. `mcdma --demo` uses fictional identities and never writes the settings file.

## Changes and publication

Keep the repository focused on the driver, build/setup tools and validation. Retain the original source provenance and Apache-2.0 license. Do not incorporate MelonDMA code or erase third-party attribution. Preserve tests for resource lifetime, DMA isolation, completion correctness and timeout behavior when optimizing.

Run the relevant offline tests for code changes, and distinguish those results from hardware checks. A change to the installer or restore helper requires a documented fresh-machine hardware check before claiming that installation route is validated.

Do not push or create a release merely because setup succeeded. When publication is requested, inspect the entire candidate tree, hidden files, images and metadata, history, commit identities and destination for private information, secrets, generated junk and licensing issues. Owner credit “Ash Hart” and this repository URL are intentionally public; private paths, account credentials and hardware addresses are not. Resolve findings before uploading.

## First-time user setup

For a new Studio/Spark installation, follow [the user guide](docs/user-guide.md) and [agent setup runbook](docs/agent-setup.md), using [install.md](docs/install.md) as the canonical command recipe. Keep the completed [setup record](docs/examples/setup-record.md) in ignored `local/`. Distinguish verified RDMA from standalone inference and the still-missing installed engine connector.

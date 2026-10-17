# AGENTS.md: rules for AI agents (and humans) working on openDECTcell

This file keeps work consistent across sessions and agents. Read it completely before changing anything. For the current project state and next steps, see the maintainers' notes. When a rule here conflicts with a habit, the rule wins. If a rule is unclear or wrong, propose a change to this file in its own commit; don't silently deviate.

## 1. Project in one paragraph

openDECTcell turns used AVM FRITZ!Boxes into open DECT base stations under OpenWrt.
- **On the chip:** the Dialog SC144xx DECT chip runs AVM's unchanged "STUB" firmware, which handles the radio, the MAC and codec conversion. The STUB is uploaded at runtime.
- **On the box, our code:** the firmware loader, a kernel module for the PCM/TDM link (on the 7412: the VR9 SSC in SPI-slave mode), the control-channel daemon, and the DECT protocol library (DLC/NWK, in C).
- **On a server:** the separate repository **opendectcell-controller** turns DECT into SIP. It is not a PBX.

First target: FRITZ!Box 7412. See `README.md` and `ROADMAP.md`.

## 2. Clean-room and blob rules (non-negotiable)

1. **Never commit AVM material:** no binaries, `.ko` modules, `.hex` firmware, images, device-tree source, calibration data or analysed code. All of that lives in `work/`, which is git-ignored, and stays local.
2. **Never paste analysed or analysed AVM code** into docs, issues, commit messages, PRs or chat transcripts. Write down **facts in your own words**: register values, bit meanings, frame layouts, state-machine behaviour, primitive names and IDs, timing.
3. **Every hardware or protocol fact names its source:** box model, FRITZ!OS version, and the component (module or function name is fine). Mark it *derived statically*, *observed on hardware* or *assumed*.
4. **New code is written from our docs**, not from analyser output. If the docs are not sufficient, extend the docs first, in a separate commit.
5. **Blobs are never shipped.** STUB firmware and per-device DECT calibration are extracted at runtime (from an AVM image the user supplies, or from the box's factory partitions). Reference AVM files by SHA-256 only.
6. Legal basis: EU Directive 2009/24/EC Art. 5(3) and Art. 6 (§69d/§69e UrhG). Interoperability is the only purpose.

## 3. Commit and push discipline

- **Commit early and often:** one logical change per commit. A doc finding, a function plus its test, a refactor: each is its own commit. Never mix unrelated changes.
- **Push after every commit** (or at least every few small commits). The remote is the source of truth, so changes stay traceable and easy to revert.
- **Never rewrite published history:** no force-push and no rebasing of pushed commits. Fix mistakes with a follow-up commit or `git revert`.
- **Commit message format:** an imperative summary of 72 characters or less, a blank line, then the *why*. Reference issues (`Refs #12`, `Fixes #12`). End with the attribution line your tool requires.
- **Before committing:** `git status` and `git diff --staged`. Check that nothing from `work/` and no AVM material is staged.
- **Author identity** for this repo: `ctandi <ctandi@users.noreply.github.com>` (set repo-locally).

## 4. Repository layout

| Path | What belongs there |
|---|---|
| `docs/models/` | Per-model analyses (image format, components, GPIOs, boot) |
| `docs/` | Protocol and interface notes (STUB API, control channel, SPI-TDM, …) |
| `tools/` | Analysis tools (Python stdlib preferred; `the analysis tooling` for headless scripts) |
| `kernel/` | Kernel modules, one per OpenWrt target (`kmod-opendectcell-lantiq`, …) with the transport drivers and the board table (GPL-2.0) |
| `boards/` | Board profiles, one per model, named after OpenWrt's `board_name` (e.g. `avm,fritz7412`); see `docs/hardware-abstraction.md` |
| `src/` | Userspace C: `libopendectcell` (DECT protocol library), `dect-fwload`, `dect-stubd`, `dect-probe` |
| `tests/` | Unit tests, chip/STUB simulator, golden traces |
| `openwrt/` | OpenWrt feed: package Makefiles, init scripts, UCI defaults |
| `work/` | **git-ignored**: AVM files, the analysis tooling projects, analyser output, captures |

## 5. Code conventions

- **Language:** C (C11) for all userspace components and the protocol library. It must build for big-endian MIPS (7412), little-endian ARM and x86-64. The kernel module follows Linux kernel coding style.
- **Endianness:** STUB fields are little-endian. Always use explicit conversion helpers; never cast structs onto wire buffers.
- **Licence headers:** kernel code (`kernel/`) uses `SPDX-License-Identifier: GPL-2.0-only`, as usual for the kernel. **Everything else** (userspace, `libopendectcell`, tools, tests) uses `SPDX-License-Identifier: GPL-2.0-or-later`, so that the AGPL-3.0 controller (opendectcell-controller) can link `libopendectcell`. Never add GPL-2.0-only code outside `kernel/`.
- **No per-model code in userspace.** Model differences go into the kernel board table (pins, PCM interface) or the board profile in `boards/` (everything else). Userspace never handles GPIO numbers or register addresses. See `docs/hardware-abstraction.md`.
- **`libopendectcell` is shared** by `dect-probe` on the box and by the controller. Keep it free of platform I/O: transport and timers are injected.
- **Terminology** (use consistently): *STUB* (AVM chip firmware), *mail* (host↔STUB message), *control slot* (PCM slot carrying mails), *cell* (one box with radio), *controller* (central DECT↔SIP gateway), *FP*/*PP* (fixed and portable part), RFPI, IPUI, TPUI, PARK.

## 6. Testing and the hardware gates

- Every component gets unit tests that run on the PC (`tests/`). Protocol code is also tested against the STUB simulator and recorded golden traces.
- **The hardware gates G1–G7 in `ROADMAP.md` are go/no-go points.** Do not start work that depends on a gate before that gate has passed on real hardware. In particular: **no controller work beyond design before G7.**
- **Hardware tests are run by the user.** For each test, write a short **test card** in the issue:
  - the exact commands
  - the expected result
  - which logs to send back

  Record the outcome in the issue and, if relevant, in the docs ("observed on hardware").

## 7. Documentation workflow

- New findings go into the matching doc right away, with the source tag (rule 2.3) and an "Open questions" list per doc.
- Keep `ROADMAP.md` checkboxes and GitHub issues in sync. Close issues with a reference to the commit or doc.
- Write in English, concisely. Prefer tables for register, pin and field layouts.

## 8. Tools available locally (maintainer machine)

- the analysis tooling 12.1.4 at `~/opt/the analysis tooling`, run with `JAVA_HOME=~/opt/jdk-21.0.12.1+1`. Use `the analysis tooling headless analysis script` and keep the output in `work/decomp/`.
- `tools/avm_nand_sqfs.py`: reads AVM's Lantiq NAND `filesystem.image` (7362 SL, 7412).
- GitHub CLI at `~/opt/gh_2.102.0_linux_amd64/bin/gh`.

# AGENTS.md: rules for AI agents (and humans) working on openDECTcell

This file keeps work consistent across sessions and agents. Read it completely before changing anything. For the current project state see `docs/status.md`, `ROADMAP.md` and the open issues; who works on what, and every hand-over, is in the issues (CONTRIBUTING.md, "Working on an issue"). the maintainers' notes, where your checkout has it, is the maintainers' private note on their lab and is not part of the public repository. When a rule here conflicts with a habit, the rule wins. If a rule is unclear or wrong, propose a change to this file in its own commit; don't silently deviate.

## 1. Project in one paragraph

openDECTcell turns used AVM FRITZ!Boxes into open DECT base stations under OpenWrt.
- **On the chip:** the Dialog SC144xx DECT chip runs AVM's unchanged "STUB" firmware, which handles the radio, the MAC and codec conversion. The STUB is uploaded at runtime.
- **On the box, our code:** the firmware loader, a kernel module for the PCM/TDM link (on the 7520: the IPQ4019 ADSS TDM block; on the 7412: the VR9 SSC in SPI-slave mode), the control-channel daemon, and the DECT protocol library (DLC/NWK, in C).
- **SIP on the box:** the `opendectcell-baresip` package connects the handsets' calls to SIP accounts through baresip (since 2026-10-28).
- **On a server, later:** the separate repository **opendectcell-controller** joins several boxes into one DECT system and turns DECT into SIP centrally. It is not a PBX.

First target: FRITZ!Box 7520 (since 2026-10-17; previously the 7412). See `README.md` and `ROADMAP.md`.

## 1a. Scope: the standard is the product, the chip is the unknown

**We are building a standards-compliant DECT base. GAP (EN 300 444) and
CAT-iq (TS 102 527) are the goal; AVM's behaviour is not.** The FRITZ!Fon C4
speaks GAP and CAT-iq, and it works against base stations from other
manufacturers, so a correct implementation of the standard is sufficient to
serve it. Anything beyond the standard is a bonus.

This sets what we reverse engineer and what we read:

- **Reverse engineer the chip, not the protocol.** The split (`[CR A-0057]`):
  MAC and DLC run in the chip; **all of NWK -- LCE, CC, MM -- runs in our
  host code**. The chip's mail interface is undocumented and is the only
  thing that genuinely requires reverse engineering. Everything that goes
  over the air is specified, and we have the specifications.
- **Spec first, always, for anything on the air.** Message types, information
  elements, procedures, timers, state machines, error handling: look it up in
  `docs/specs/` before asking the clean room. We have EN 300 175 parts 3/4/5,
  EN 300 444 and TS 102 527. Asking the room an envelope question wastes a
  round trip, and a traced answer from one code path can be narrower or
  simply wrong where the standard is clear -- that has happened
  (`A-0055` -> `A-0060`: a close built from a stock handler that the standard
  explicitly excludes).
- **Ask the clean room for:** which chip mail performs a given operation, its
  parameters, the chip's state machines and events, memory layouts, and
  **where AVM deviates from the standard** -- the last one matters because a
  deviation tells us what handsets in the wild may expect.
- **Do not implement AVM-proprietary protocol in `libopendectcell`.** Until the
  standard works end to end, proprietary messages get the **standard
  decline** and nothing more: an unknown CC transaction is cleared with
  `{CC-RELEASE-COM}` and the reason the standard names (EN 300 175-5
  §17.3.2.1). We do not invent replies to proprietary content, and we do not
  chase a vendor feature because a handset asked for it.
- **Manufacturer behaviour belongs in a device profile**, not in the general
  library -- a later, separate layer keyed on the handset, carrying quirks
  and vendor extensions. The library stays the standard.

**The practical test for any new question:** can it be answered from a
specification we already hold? Then read it. Does it concern a mail number, a
chip register, a chip event, or a divergence from the standard? Then ask the
room.

## 1b. Work on one model must not change the others (maintainer, 2026-11-01)

The 7412 and 7520 are bench boxes of another maintainer, used with the Go
controller. An agent working on another model (the 7362 SL first) cannot
test on them, and neither can anyone reading its commits, so a regression
there lands unnoticed. **A change to anything those boxes share must not
alter their behaviour.**

1. **Packaging.** Several models share one OpenWrt target (lantiq/xrx200
   is the 7412 and the 7362 SL). A model's own kernel modules come in a
   per-board meta package (`opendectcell-fritz7362sl`), never as target-wide
   `DEPENDS` of `opendectcell`: a 7412 must not install the 7362 SL's modules.
2. **Run-time differences belong in `boards/<model>`.** The init script
   reads board keys (`transport`, `pcm_kmod`, `pcm_dev`, `uart`,
   `uart_kmod`, `fw_*`, `quiet_console`, …). Key new behaviour on a
   board setting of its own, not on one other boards share (the
   console quieting first keyed on `pcm_kmod` would have run on the
   7412).
3. **Do not change `libopendectcell`.** `dect-stubd` links it on every box,
   and the Go controller vendors a copy (`third_party/opendectcell`, through
   cgo) with real-library traces, golden files and the API versions of
   `docs/api-versions.md`. Its behaviour comes from the specifications
   and clean-room answers and changes only with that source, by its
   owner. A model's transport belongs in the kernel module and
   `dect-stubd`. If a chip answers differently at the mail level, open an
   issue in openDECTcell/opendectcell with the evidence; the maintainer or
   their agent decides.
4. **Run-time switch over build flag.** One lantiq binary serves the 7412
   and the 7362 SL, so `-DBOARD_…` cannot separate them; pass a board key
   to `dect-stubd` as an option. Where a compile-time switch is truly
   needed: only in `dect-stubd` or the kernel module, never in
   `libopendectcell` or its headers; off by default, so a build without it
   behaves exactly as the 7412 and 7520 run today; `make check` and
   `make cross` green with it on and off.
5. **Record every change to shared code** (`dect-stubd`, the init script,
   packaging, keep lists) in the issue and the pull request: what it does on
   the other boxes and how to bench it there, so their owner can check it.

## 2. Clean-room and blob rules (non-negotiable)

1. **Never commit AVM material:** no binaries, `.ko` modules, `.hex` firmware, images, device-tree source, calibration data or analysed code. All of that lives in `work/`, which is git-ignored, and stays local.
2. **Never paste analysed or analysed AVM code** into docs, issues, commit messages, PRs or chat transcripts. Write down **facts in your own words**: register values, bit meanings, frame layouts, state-machine behaviour, primitive names and IDs, timing.
3. **Every hardware or protocol fact names its source:** box model, FRITZ!OS version, and the component (a role phrase such as "the vendor's DECT host driver"; a file of the vendor's published GPL source may be named by path). Mark it *analysed*, *observed on hardware* or *assumed*.
4. **New code is written from our docs**, not from analyser output. If the docs are not sufficient, extend the docs first, in a separate commit.
5. **Blobs are never shipped.** STUB firmware and per-device DECT calibration are extracted at runtime (from an AVM image the user supplies, or from the box's factory partitions). Reference AVM files by SHA-256 only.
6. **AVM's published GPL source code is not "AVM material" in the sense of rule 1.** AVM releases the GPL parts of FRITZ!OS (kernel, drivers) as source at `osp.avm.de`. That code may be read and used under its licence (GPL-2.0+ files and the Qualcomm/ISC files AVM ships are compatible with our GPL-2.0-only kernel module): keep the original copyright notices in derived code and name the file it came from. Do not commit AVM's source tarball or copies of its files; keep them in `work/`. Rules 1-5 still apply to everything proprietary (binaries, blobs, `.ko` modules without source, analysed code).
7. **Clean-room process (since 2026-10-17).** AVM material lives only in `../../the analysis repository/` (the analysis side), worked on by a separate agent. Agents working in this repository **never open that folder**, never read AVM binaries, AVM source (including the GPL source) or analysis. Facts come in through the shared record `../../the clean-room record/` (numbered question and answer files; protocol in its README) and are cited as `[CR A-NNNN]`. The STUB blobs in `the clean-room record/blobs/` are only copied to the test box, never inspected.
8. Legal basis: EU Directive 2009/24/EC Art. 5(3) and Art. 6 (§69d/§69e UrhG). Interoperability is the only purpose.
9. **Other vendors' firmware (maintainer, 2026-10-28).** When a handset does not work with our base and the standards plus our own on-air observations cannot explain why, the firmware of that handset's own base station may be analysed for interoperability. The same process as for AVM applies: the material lives in a separate room worked on by a separate agent, facts come in through the shared record, rules 1-5 apply to it, and nothing from it goes into `libopendectcell` except as a device profile (§1a).

## 3. Commit and push discipline

- **Commit early and often:** one logical change per commit. A doc finding, a function plus its test, a refactor: each is its own commit. Never mix unrelated changes.
- **Push after every commit.** Always `git push` immediately after committing — never leave a commit living only on the maintainer's machine, where it can be lost. The remote is the source of truth, so changes stay traceable and easy to revert.
- **Never rewrite published history:** no force-push and no rebasing of pushed commits. Fix mistakes with a follow-up commit or `git revert`.
- **Commit message format:** an imperative summary of 72 characters or less, a blank line, then the *why*. Reference issues (`Refs #12`, `Fixes #12`). End with the attribution line your tool requires.
- **Before committing:** `git status` and `git diff --staged`. Check that nothing from `work/` and no AVM material is staged.
- **Author identity** for this repo: `ctandi <ctandi@users.noreply.github.com>` (set repo-locally), or the git user configured in the repository's local config (`git config --local user.name`/`user.email`). Don't change the git config to switch between them.

## 4. Repository layout

| Path | What belongs there |
|---|---|
| `docs/models/` | Per-model analyses (image format, components, GPIOs, boot) |
| `docs/journals/` | Dated logs: bench cards and their results, task lists, investigation logs. History, not reference |
| `docs/` | Protocol and interface notes (STUB API, control channel, SPI-TDM, …); `docs/README.md` is the index, `docs/glossary.md` the glossary, `docs/developing.md` the developer guide |
| `ARCHITECTURE.md` | The map of the code: pieces, data flow, the rules that keep them apart. Each source directory also has a README listing its files; keep both in step when files are added, renamed or removed |
| `tools/` | Analysis and release tools (Python standard library preferred) |
| `kernel/` | Kernel modules, one per OpenWrt target (`kmod-opendectcell-lantiq`, …) with the transport drivers and the board table (GPL-2.0) |
| `boards/` | Board profiles, one per model, named after OpenWrt's `board_name` (e.g. `avm,fritz7412`); see `docs/hardware-abstraction.md` |
| `src/` | Userspace C: `libopendectcell` (DECT protocol library), `dect-fwload`, `dect-stubd`, the baresip module, development tools |
| `profiles/` | Handset profiles (`docs/handset-profiles.md`) |
| `tests/` | Unit tests, chip/STUB simulator, golden traces |
| `openwrt/` | OpenWrt feed: package Makefiles, init scripts, UCI defaults |
| `work/` | **git-ignored**: AVM files, analysis projects and output, captures |

## 5. Code conventions

- **Language:** C (C11) for all userspace components and the protocol library. It must build for big-endian MIPS (7412), little-endian ARM and x86-64. The kernel module follows Linux kernel coding style.
- **Endianness:** STUB fields are little-endian. Always use explicit conversion helpers; never cast structs onto wire buffers.
- **Licence headers:** kernel code (`kernel/`) uses `SPDX-License-Identifier: GPL-2.0-only`, as usual for the kernel. **Everything else** (userspace, `libopendectcell`, tools, tests) uses `SPDX-License-Identifier: GPL-2.0-or-later`, so that the AGPL-3.0 controller (opendectcell-controller) can link `libopendectcell`. Never add GPL-2.0-only code outside `kernel/`.
- **No per-model code in userspace.** Model differences go into the kernel board table (pins, PCM interface) or the board profile in `boards/` (everything else). Userspace never handles GPIO numbers or register addresses. See `docs/hardware-abstraction.md`.
  The bring-up exception of 2026-10-17 (GPIO numbers on the `dect-fwload` command line) ended when the kernel module `/dev/dect0` was verified on the same day; `dect-fwload` no longer handles any GPIO or pinctrl detail.
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
- **Always leave the radio parked when not actively testing.** The DECT radio must never transmit unattended. Turn it on only for an explicit, agreed test, and park it (close registration `0x022C 01`, then radio off `0x015B 00`) as soon as the test ends — including after an interrupted or crashed run. If a run leaves the chip beaconing (it keeps transmitting autonomously after the daemon exits), park it explicitly, e.g. `dect-stubd -M "0x22:0x02:0x022c:01" -M "0x22:0x02:0x015b:00" -t 6`. Never start transmitting unless the user has explicitly agreed to that test.

## 7. Documentation workflow

- New findings go into the matching doc right away, with the source tag (rule 2.3) and an "Open questions" list per doc.
- Keep `ROADMAP.md` checkboxes and GitHub issues in sync. Close issues with a reference to the commit or doc.
- Write in English, concisely. Prefer tables for register, pin and field layouts.

## 8. Tools

- `tools/` holds the PC-side tools (see [tools/README.md](tools/README.md));
  all analysis output stays in the git-ignored `work/`, never committed.
- `tools/avm_nand_sqfs.py` reads AVM's Lantiq NAND `filesystem.image`
  (7362 SL, 7412).

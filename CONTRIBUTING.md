# Contributing to openDECTcell

Thank you for helping. The most useful contributions right now are reports
from real hardware: which handsets and boxes work, and which don't.

## Reporting a handset or a box

Open an issue with:

- the **box** (model, OpenWrt version) and the **openDECTcell version**;
- the **handset** (maker, model, firmware version if the handset shows it);
- what you tried: registering, calls in and out, HD voice, caller ID,
  calls between handsets;
- what happened, and the **log** of that moment: Status > DECT, tab *Log*
  (or `logread -e dect` on the box).

Before you post a log, **remove your personal data**: phone numbers, SIP
user names and server addresses, and the handset's IPEI if you prefer to keep
it private.

A handset that needs special treatment can get a profile file; see
[docs/handset-profiles.md](docs/handset-profiles.md). A tested profile is a
welcome pull request.

**Never attach firmware images, configuration backups or captures that contain real identities** (IPEI, IPUI, keys, PINs,
phone numbers, SIP credentials) to an issue; mask them, or describe what you saw.

## Reporting a bug

The same information as above, plus the steps that lead to the problem. For
the web interface, the browser's console output helps.

## Working on the code

### Layout

[ARCHITECTURE.md](ARCHITECTURE.md) explains how the pieces fit together, [docs/developing.md](docs/developing.md)
is the working guide, and each directory below has a README that says what each of its files does.

| Path | What is there |
|---|---|
| `src/libopendectcell/` | The DECT protocol library: framing, link layer, registration and authentication, call control, G.722, diagnostics, profiles ([README](src/libopendectcell/README.md)) |
| `src/stubd/` | `dect-stubd`, the daemon that runs the base ([README](src/stubd/README.md)) |
| `src/cell/` | `dect-cell`, the box's link to a controller ([README](src/cell/README.md)) |
| `src/fwload/` | `dect-fwload`, the chip firmware loader ([README](src/fwload/README.md)) |
| `src/baresip/` | The baresip module that connects calls to SIP ([README](src/baresip/README.md)) |
| `src/tools/` | Development tools for captures (`dect-pcmcap`, `dect-maildump`, …) |
| `kernel/` | The kernel drivers, one per SoC family (GPL-2.0-only) ([README](kernel/README.md)) |
| `openwrt/` | The OpenWrt feed, laid out as upstream: `net/opendectcell` (packages, init script, configuration), `applications/luci-app-opendectcell`; see [openwrt/README.md](openwrt/README.md) |
| `boards/` | Board profiles, one per FRITZ!Box model ([README](boards/README.md)) |
| `profiles/` | Handset profiles ([README](profiles/README.md)) |
| `tests/` | Unit tests, run on the PC ([README](tests/README.md)) |
| `tools/` | PC tools: firmware extraction, release build, analysis helpers ([tools/README.md](tools/README.md)) |
| `docs/` | Documentation ([docs/README.md](docs/README.md)); dated logs of bench sessions and investigations are in `docs/journals/` |

### Build and test on the PC

```sh
make check     # build and run every unit test (needs a C compiler and Python 3)
make           # also build the programs for the PC
```

The unit tests need no hardware. They must pass before every commit.

### Build for the box

With the [OpenWrt SDK](https://downloads.openwrt.org/releases/25.12.6/targets/ipq40xx/generic/)
for the target unpacked somewhere:

```sh
make cross SDK=/path/to/openwrt-sdk-25.12.6-ipq40xx-generic_…
```

This puts ARM builds of `dect-stubd`, `dect-fwload` and the capture tools
in `build/*.arm` (7520/7530 only). Or build the packages as the release does:

```sh
tools/release/build.sh ipq40xx/generic out
```

Packages built without the project's signing key install only with
`apk add --allow-untrusted`.

### Style

- C11 for userspace, Linux kernel style for `kernel/`. The code must build
  without warnings (`-Werror`) for little-endian ARM, big-endian MIPS and
  x86-64.
- Write code that reads like the code around it.
- New files carry an SPDX header: `GPL-2.0-or-later`, except under `kernel/`
  (`GPL-2.0-only`).
- One logical change per commit, with a message that says why.

## Licence, sign-off and AI assistance

- **Licence.** By contributing you license your change under the licence of the file you change: GPL-2.0-or-later for the
  programs, libraries and tools, GPL-2.0-only for `kernel/` ([LICENSES/](LICENSES/)). Every new source file starts with an
  `SPDX-License-Identifier` line.
- **Sign-off.** Every commit carries a `Signed-off-by: Your Name <you@example.org>` line (`git commit -s`): your statement
  that you wrote the change or have the right to submit it under that licence (the
  [Developer Certificate of Origin](https://developercertificate.org/)).
- **AI assistance.** This project is largely written with AI coding agents under the rules in [AGENTS.md](AGENTS.md). If
  you used an AI tool for a contribution, say so in the pull request, and check the output yourself: you remain responsible
  for it, including for any text it may have copied from elsewhere.

## The clean-room rules

openDECTcell must stay free of AVM's code. These rules are not negotiable:

- **Never contribute AVM code, analysed or analysed code, AVM firmware,
  or anything copied from AVM files.** That includes listings in issues and
  pull requests.
- **Facts in your own words, with their source:** register values, message
  layouts, timing. Say how you know (seen on hardware, read in a standard, …).
- **Anything on the air comes from the ETSI standards**, which are free
  ([docs/specs/README.md](docs/specs/README.md)).

The full rules, which also bind the AI agents working on the project, are in
[AGENTS.md](AGENTS.md), section 2.

## Safety on the air

A DECT base transmits. Switch the radio off (stop the service:
`/etc/init.d/opendectcell stop`) when you are not testing, and never leave an
experimental build transmitting unattended.

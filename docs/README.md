# Documentation

Where to start depends on what you want:

- **Use openDECTcell on your box:** [status.md](status.md), then [install.md](install.md).
- **Understand how it works:** the "How it works" section of the
  [main README](../README.md#how-it-works), then the design documents below.
- **Work on the code:** [CONTRIBUTING.md](../CONTRIBUTING.md),
  [AGENTS.md](../AGENTS.md) (the project's rules), then the reference
  documents.
- **Port it to another FRITZ!Box:** [hardware-abstraction.md](hardware-abstraction.md)
  and the model analyses.

## For users

| Document | What it covers |
|---|---|
| [status.md](status.md) | What works, what doesn't yet, tested boxes and handsets |
| [install.md](install.md) | Installing a release, the chip firmware, registering handsets, phone lines |
| [luci-manual.md](luci-manual.md) | The web pages (Network > DECT, Status > DECT), field by field, with troubleshooting |
| [firmware-sourcing.md](firmware-sourcing.md) | Why you supply the DECT chip firmware yourself, and which FRITZ!OS builds are known |
| [handset-profiles.md](handset-profiles.md) | Handset profiles: per-model settings in `/etc/opendectcell/profiles/`, and how to write one |
| [handset-compatibility.md](handset-compatibility.md) | What each tested handset did against openDECTcell, in detail |

## How it works (design)

| Document | What it covers |
|---|---|
| [hardware-abstraction.md](hardware-abstraction.md) | How several FRITZ!Box models are supported: board table, board profiles, packages |
| [bridge-interface.md](bridge-interface.md) | The control and audio sockets between `dect-stubd` and the SIP side (baresip) |
| [lines-and-calls.md](lines-and-calls.md) | Lines, call ids, ringing several handsets, call waiting (partly built) |
| [two-calls.md](two-calls.md) | Two calls at the same time on one chip |
| [phonebook.md](phonebook.md) | The handsets' phonebook from a CardDAV address book, both ways |
| [diagnostics.md](diagnostics.md) | What the DECT Monitor shows, and where each value comes from |
| [capacity.md](capacity.md) | How many handsets and calls one box carries, and how to go beyond |
| [more-than-six.md](more-than-six.md) | Design and test plan for more than six stand-by handsets in one base (chip slots as a cache), 2026-10-31 |
| [bench-2026-11-01.md](bench-2026-11-01.md) | Run sheet for the bench day 2026-11-01: the open tests in 14 blocks, box state, restore commands |
| [multicell-experiments.md](multicell-experiments.md) | Experiments towards several boxes as one DECT system |
| [notes-for-the-controller.md](notes-for-the-controller.md) | Interface notes for the separate multi-cell controller project |
| [hosting-dect-base.md](hosting-dect-base.md) | For a second host of the stack (the controller): the chip mails, the callbacks, the per-cell chip settings, testing against the golden traces |
| [stack-in-library.md](stack-in-library.md) | How the DECT stack moved from `dect-stubd` into `libopendectcell` (`struct dect_base`), stage by stage, so the controller can host it |
| [api-versions.md](api-versions.md) | What changed in `libopendectcell`'s public interface at each API version |
| [multicell-identities.md](multicell-identities.md) | Identities (RFPI, IPUI) for many cells under one controller |

## Reference: the DECT chip interface

These documents describe, in our own words, how the router talks to the DECT
chip. They are the basis for the code.

| Document | What it covers |
|---|---|
| [stub-api.md](stub-api.md) | Catalogue of the messages ("mails") between the router and the chip firmware |
| [stub-pcm-control-channel.md](stub-pcm-control-channel.md) | How those mails travel on the PCM bus: framing, checksums, audio paths |
| [stub-loader-protocol.md](stub-loader-protocol.md) | Uploading the chip firmware over the UART |
| [stub-image-format.md](stub-image-format.md) | The firmware upload image: format and patching |
| [mac-bringup.md](mac-bringup.md) | Switching the base on: the radio, the beacon, registration mode |
| [pcm-tdm-ipq4019.md](pcm-tdm-ipq4019.md) | The IPQ4019's PCM/TDM hardware (FRITZ!Box 7520/7530) |
| [kmod-spec.md](kmod-spec.md) | Specification of the kernel driver for the 7520/7530 |
| [7412-spi-tdm-and-loader.md](7412-spi-tdm-and-loader.md) | The FRITZ!Box 7412's bus and loader (running since 2026-10-29) |
| [models/7520-DECT-analysis.md](models/7520-DECT-analysis.md) | FRITZ!Box 7520: DECT hardware and components |
| [models/7412-DECT-analysis.md](models/7412-DECT-analysis.md) | FRITZ!Box 7412: DECT hardware and components |
| [models/7362SL-DECT-analysis.md](models/7362SL-DECT-analysis.md) | FRITZ!Box 7362 SL: DECT hardware and components |

## Reference: the DECT standards

| Document | What it covers |
|---|---|
| [specs/README.md](specs/README.md) | The ETSI standards used, and how to download them (free) |
| [spec-conformance.md](spec-conformance.md) | Our protocol stack checked against EN 300 175 |
| [gap-conformance.md](gap-conformance.md) | What GAP (EN 300 444) requires of a base, and where we stand |
| [spec-sweep-2026-10-28.md](spec-sweep-2026-10-28.md) | A full review against GAP, CAT-iq and the procedures, with its three reports in [spec-sweep-2026-10-28/](spec-sweep-2026-10-28/) |
| [verification-gap.md](verification-gap.md) | Where we tell the chip something and should check that it took effect |

## Development journal

How the project got here: bench sessions, experiments, dead ends and task
lists, kept as they were written. They are dated and often superseded by
later findings; the documents above are the current state. Each journal page
says at the top what it was for.

| Document | What it covers |
|---|---|
| [registration-g6.md](registration-g6.md) | Getting the first handset to register (gate G6) |
| [call-g7.md](call-g7.md) | Getting a handset to ring and talk (gate G7): the long log of every run |
| [why-the-handsets-never-rang.md](why-the-handsets-never-rang.md) | The search for why the Gigasets would not ring, and the answer |
| [tasks-2026-10-28.md](tasks-2026-10-28.md), [tasks-2026-10-30.md](tasks-2026-10-30.md) | Task lists, still current for some open details |
| [bench-later.md](bench-later.md) | Every open bench test on one card, with the outcomes written before the run, and the unattended runs' results |
| [bench-2026-10-25.md](bench-2026-10-25.md), [bench-next.md](bench-next.md), [bench-7412.md](bench-7412.md) | Older bench test cards and their results; `bench-7412.md` the first handset on a 7412 |

The project-wide history and hand-over notes are in
the maintainers' notes; the plan is in [ROADMAP.md](../ROADMAP.md).

## Labels used in the documents

Many statements carry a label saying how we know them:

| Label | Meaning |
|---|---|
| `[observed]`, `[observed on hardware]` | Seen on a real box or handset |
| `[derived statically]` | Worked out from analysing files, not yet seen running |
| `[CR A-NNNN]` | A fact from the clean-room process: answer NNNN, given by a separate analysis that is not part of this repository |
| *assumed*, `[inferred]` | Our reasoning, not yet checked |

# Documentation

Where to start depends on what you want:

- **Use openDECTcell on your box:** [status.md](status.md), then [install.md](install.md).
- **Understand how it works:** [ARCHITECTURE.md](../ARCHITECTURE.md) (the map of the code and the data flow), the
  [glossary](glossary.md), then the design documents below.
- **Work on the code:** [developing.md](developing.md) (build, test, where a change goes), the README of the
  directory you are in ([src/libopendectcell](../src/libopendectcell/README.md), [src/stubd](../src/stubd/README.md),
  [kernel](../kernel/README.md), [tests](../tests/README.md) and the others), [CONTRIBUTING.md](../CONTRIBUTING.md),
  [AGENTS.md](../AGENTS.md) (the project's rules), then the reference documents.
- **Port it to another FRITZ!Box:** [hardware-abstraction.md](hardware-abstraction.md)
  and the model analyses.

## For developers

| Document | What it covers |
|---|---|
| [../ARCHITECTURE.md](../ARCHITECTURE.md) | The pieces, how they talk, the data flow of a registration and a call, the rules that keep them apart |
| [developing.md](developing.md) | Build and test, where a change goes, how the tests work, debugging on a box, commit rules |
| [glossary.md](glossary.md) | DECT terms and the project's own words |
| [licensing.md](licensing.md) | The licence of each component, the kernel/userspace boundary, third-party parts, provenance (not legal advice) |
| [clean-room-process.md](clean-room-process.md) | How the analysis and implementation sides work, and what the `[CR A-nnnn]` citations mean |
| [releasing.md](releasing.md) | How a release is built, signed and published; the one OpenWrt version a release is built for |
| [api-versions.md](api-versions.md) | What changed in `libopendectcell`'s public interface at each API version |

## For users

| Document | What it covers |
|---|---|
| [status.md](status.md) | What works, what doesn't yet, tested boxes and handsets |
| [install.md](install.md) | Installing a release, the chip firmware, registering handsets, phone lines |
| [luci-manual.md](luci-manual.md) | The web pages (Network > DECT, Status > DECT), field by field, with troubleshooting |
| [privacy-and-data.md](privacy-and-data.md) | What personal and secret data the box stores, where, for how long, and how to remove it |
| [firmware-sourcing.md](firmware-sourcing.md) | Why you supply the DECT chip firmware yourself, and which FRITZ!OS builds are known |
| [handset-profiles.md](handset-profiles.md) | Handset profiles: per-model settings in `/etc/opendectcell/profiles/`, and how to write one |
| [handset-compatibility.md](handset-compatibility.md) | What each tested handset does against openDECTcell, and what is special about it (the October investigation is in [journals/](journals/handset-compatibility-2026-10.md)) |

## How it works (design)

| Document | What it covers |
|---|---|
| [hardware-abstraction.md](hardware-abstraction.md) | How several FRITZ!Box models are supported: board table, board profiles, packages |
| [bridge-interface.md](bridge-interface.md) | The control and audio sockets between `dect-stubd` and the SIP side (baresip) |
| [lines-and-calls.md](lines-and-calls.md) | Lines, call ids, ringing several handsets, call waiting (partly built) |
| [two-calls.md](two-calls.md) | Two calls at the same time on one chip |
| [phonebook.md](phonebook.md) | The handsets' phonebook from a CardDAV address book, both ways |
| [diagnostics.md](diagnostics.md) | What the DECT Monitor shows, and where each value comes from |
| [7412-stop-hang.md](7412-stop-hang.md) | The 7412's rare network death at a service stop: the evidence, the hypotheses, what was ruled out |
| [vendor-specific-support.md](vendor-specific-support.md) | Plan for handsets that need more than GAP and CAT-iq (manufacturer-specific commands); the O201 case and the DECT Command Finder |
| [handset-variation-catalog.md](handset-variation-catalog.md) | Every variation of the basic DECT functions the standards permit (registration, call control, audio, display and features, old procedures too), with what we can send today and how to tell the result |
| [images-plan.md](images-plan.md) | Ready-made OpenWrt images with the packages inside |
| [security-review-2026-11-02.md](security-review-2026-11-02.md) | The security review of 2026-11-02: what was fixed and what is open |
| [capacity.md](capacity.md) | How many handsets and calls one box carries, and how to go beyond |
| [more-than-six.md](more-than-six.md) | Design and test plan for more than six stand-by handsets in one base (chip slots as a cache), 2026-10-31 |
| [notes-for-the-controller.md](notes-for-the-controller.md) | Interface notes for the separate multi-cell controller project |
| [hosting-dect-base.md](hosting-dect-base.md) | For a second host of the stack (the controller): the chip mails, the callbacks, the per-cell chip settings, testing against the golden traces |
| [stack-in-library.md](stack-in-library.md) | How the DECT stack moved from `dect-stubd` into `libopendectcell` (`struct dect_base`), stage by stage, so the controller can host it |
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
| [spec-sweep-2026-10-28.md](journals/spec-sweep-2026-10-28.md) | A full review against GAP, CAT-iq and the procedures, with its three reports in [spec-sweep-2026-10-28/](journals/spec-sweep-2026-10-28/) |
| [verification-gap.md](verification-gap.md) | Where we tell the chip something and should check that it took effect |

## Development journal

How the project got here (bench sessions, experiments, dead ends, task lists) is in [journals/](journals/README.md),
kept as it was written. The documents above are the current state; the journals are dated and often superseded.

The project-wide history and hand-over notes are in
the maintainers' notes; the plan is in [ROADMAP.md](../ROADMAP.md).

## Labels used in the documents

Many statements carry a label saying how we know them:

| Label | Meaning |
|---|---|
| `[observed]`, `[observed on hardware]` | Seen on a real box or handset |
| `[analysed]` | Worked out by interoperability analysis of the hardware, not yet seen running |
| `[CR A-NNNN]` | A fact from the clean-room process: answer NNNN, given by a separate analysis that is not part of this repository |
| *assumed*, `[inferred]` | Our reasoning, not yet checked |

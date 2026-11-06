# Developing openDECTcell

For someone who wants to change the code. Start with [../ARCHITECTURE.md](../ARCHITECTURE.md) (what is where),
keep the [glossary](glossary.md) open, and read [../CONTRIBUTING.md](../CONTRIBUTING.md) for reports and
the pull request rules. This page is the working guide in between.

## What you need

| For | You need |
|---|---|
| The library, the daemon, the tests | A C compiler (C11) and Python 3. No hardware. `make check` runs everything. |
| The ARM or MIPS programs | The matching OpenWrt SDK (`make cross`, or the release scripts below). |
| Anything that touches the radio | A FRITZ!Box with OpenWrt, the chip firmware from your own FRITZ!OS image ([firmware-sourcing.md](firmware-sourcing.md)), and a handset. |

The chip firmware and any AVM file never go into the repository, a pull request, an issue or a log
([../AGENTS.md](../AGENTS.md) §2 has the rules in full; the short form is in the README's "Clean room").

## Build and test

```sh
make check          # build and run every unit test, with the sanitizers
make                # also the programs for the PC
make cross SDK=...  # ARM builds of dect-stubd, dect-fwload and the capture tools (7520/7530)
make check-cell     # the cell link's tests (needs mbedTLS)
```

The unit tests must pass before every commit; CI runs the same
(`.github/workflows/check.yml`). To build the packages as a release does:
`tools/release/build.sh ipq40xx/generic OUT` (7520/7530) or `lantiq/xrx200` (7412); the images:
`tools/release/images.sh`. See [../openwrt/README.md](../openwrt/README.md) and [../tools/README.md](../tools/README.md).

## Finding your way

1. [../ARCHITECTURE.md](../ARCHITECTURE.md): the map, the layers and the data flow.
2. The README of the directory you are in: [../src/libopendectcell/](../src/libopendectcell/README.md),
   [../src/stubd/](../src/stubd/README.md), [../kernel/](../kernel/README.md), [../tests/](../tests/README.md),
   [../boards/](../boards/README.md), [../profiles/](../profiles/README.md).
3. The header comment of the file: it says what the file implements and which clause of which standard it follows.
4. The reference documents in [README.md](README.md): the chip's mail interface ([stub-api.md](stub-api.md)),
   how mails travel on the bus ([stub-pcm-control-channel.md](stub-pcm-control-channel.md)), bringing the radio
   up ([mac-bringup.md](mac-bringup.md)).
5. [journals/](journals/): how things were found out, run by run. Not needed to use the code; useful when you
   ask "why does it do that?".

## How a change goes

| You want to | Where | What else to do |
|---|---|---|
| Fix or extend the DECT protocol stack | `src/libopendectcell/` | Add or change a test in `tests/`; if the interface changes, bump the API version in `dect_proto.h` and describe it in [api-versions.md](api-versions.md). The controller hosts the same library, so a change must not alter what an existing host sees. |
| Add a handset's special needs | `profiles/` | Write the profile ([handset-profiles.md](handset-profiles.md)), check it with `dect-stubd --check-profile FILE`, say which handset and firmware you tested with. |
| Add a FRITZ!Box model | `boards/`, `kernel/`, `openwrt/` | [hardware-abstraction.md](hardware-abstraction.md): a board profile, the transport driver, the packaging. Behaviour that only one model needs is keyed on that board's own setting, never on a setting other boards share ([../AGENTS.md](../AGENTS.md) §1b). |
| Change what the daemon does on a box | `src/stubd/` | The map of `dect-stubd.c` is in [../src/stubd/README.md](../src/stubd/README.md). Replay tests live in `tests/stubd-traces/`. |
| Change the web interface | `openwrt/applications/luci-app-opendectcell/` | Field names and help texts are described in [luci-manual.md](luci-manual.md); keep both in step. |
| Change the SIP side | `src/baresip/` | [bridge-interface.md](bridge-interface.md) is the contract between it and the daemon. |
| Document something | `docs/` | One topic per file; label each statement ([README.md](README.md#labels-used-in-the-documents)); add the file to the index in [README.md](README.md). Dated logs go to [journals/](journals/). |

## Tests

- **Unit tests** (`tests/test_*.c`) check one module each against the standard's examples or hand-made frames.
- **Golden traces** (`tests/stubd-traces/`, `tests/traces/`) are recorded exchanges of mails between a host and a
  (simulated) chip. A test replays one against the library and compares every line the library sends. They are the
  safety net for refactoring: a change in behaviour shows up as a diff in a trace. Traces contain made-up
  identities only.
- Tests are written in plain C with `tests/check.h`; Python tests cover the scripts. The list and what each covers
  is in [../tests/README.md](../tests/README.md).

If you change behaviour on purpose, change the trace in the same commit and say why in the message.

## Debugging on a box

| Question | Where to look |
|---|---|
| Is it running, how are the handsets doing? | Status > DECT in LuCI, or `cat /var/run/opendectcell/state.json` |
| What did the daemon do? | `logread -e dect`; start `dect-stubd` by hand with `-v` for the mails and the protocol layers ([../src/stubd/README.md](../src/stubd/README.md) has the command line) |
| What does the chip say? | `dect-maildump` and `dect-pcmcap` (`src/tools/`) decode captures; [diagnostics.md](diagnostics.md) explains each value in the monitor |
| Did the radio park at the end? | `cat /var/run/opendectcell/parked`: `verified` means the chip confirmed it |
| A handset behaves oddly | Compare with [handset-compatibility.md](handset-compatibility.md); try a profile setting ([handset-profiles.md](handset-profiles.md)) |

Take care with the radio: the daemon parks it on a clean stop, and the init script holds the chip in reset when
the park could not be confirmed. Do not kill the daemon with SIGKILL on a box you care about, and do not leave an
experiment's radio on after you are done.

## Commits and pull requests

- One logical change per commit; an imperative summary of at most 72 characters, a blank line, then why.
- Every source file starts with an SPDX licence line (GPL-2.0-or-later; the kernel modules GPL-2.0-only).
- `make check` and, for anything touching the box, `make cross` must pass.
- Say what you tested on which box and handset, and what you could not test.
- Do not paste analysed or copied vendor code, firmware or keys; describe facts in your own words and name how
  you know them (`[observed]`, `[analysed]`, *assumed*).
- The conventions for maintainers and their agents (push discipline, work log) are in [../AGENTS.md](../AGENTS.md).

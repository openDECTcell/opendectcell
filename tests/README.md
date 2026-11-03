# tests/: unit tests, golden traces, script tests

Everything here runs on a PC, with no DECT hardware, no radio and no AVM
files. C tests are plain C11 programs built with AddressSanitizer and
UBSan; there is no test framework. They test the protocol library
(`src/libopendectcell/`), the daemon above its chip transport (`src/stubd/`), the
firmware-image code (`src/fwload/`) and a few packaging scripts.

## Running

| Command | What it does |
|---|---|
| `make check` | Builds every C test into `build/` and runs it, then runs the four Python tests. Stops at the first failing test. This is what CI runs |
| `make build/test_cc && build/test_cc` | One test only (any name from the table below) |
| `make check-cell MBEDTLS=<prefix>` | The cell link: `test_srtp` and `test_cell_link.py`. Needs mbedTLS 3.6 headers and libraries and Python's `cryptography`; not part of `make check` or CI |
| `make cross` | Does not run tests. Builds the ARM binaries `dect-fwload`, `dect-pcmcap` and `dect-stubd` for the 7520 into `build/*.arm` with the OpenWrt SDK (`-Werror`, so it is also a compile check against the target toolchain). Looks for an `openwrt-sdk-25.12.5-ipq40xx-*` directory; set `SDK=` otherwise |

Run `make` from the repository root: `test_trace` and `test_stubd` open
`tests/traces/` and `tests/stubd-traces/` by relative path, and the daemon
trace expects its files under the default `build/` directory (a trace line
names `build/stubd-test-run/audio.sock`), so do not change `BUILD`. The daemon's
own log goes to `build/test_stubd.log`. Run `make check` before a commit; AGENTS.md section 1b
also asks for `make cross` to stay green ([../AGENTS.md](../AGENTS.md)).

The Makefile's compile flags (`-Wall -Wextra -Werror -Wshadow -Wconversion`)
apply to the tests, so a warning fails the build.

## Test files

All `test_*.c` files include [check.h](check.h) and are listed in `TESTS` in the
[top-level Makefile](../Makefile) (except `test_srtp.c`).

| File | Covers |
|---|---|
| `test_fwimage.c` | `src/fwload/fwimage.c`: the firmware upload image, [../docs/stub-image-format.md](../docs/stub-image-format.md). Synthetic HEX always; real AVM files only if `OPENDECTCELL_STUB_HEX` and `OPENDECTCELL_FL_HEX` point to them (never in git; the test prints `skip` otherwise) |
| `test_mail.c` | `dect_mail.c`: mail framing and checksums, [../docs/stub-pcm-control-channel.md](../docs/stub-pcm-control-channel.md) |
| `test_session.c` | `dect_session.c`: contact handshake and liveness watchdog, fake clock |
| `test_audio.c` | `dect_audio.c`: A-law round trip, tone analysis |
| `test_g722.c` | `dect_g722.c`: G.722 encoder and decoder (conformance against ffmpeg is the separate `tools/g722-crosscheck.sh`) |
| `test_cs.c` | `dect_cs.c`: C-plane transport mails (0x0203, 0x0244, 0x0202) |
| `test_lc.c` | `dect_lc.c`: Cs checksum (EN 300 175-4 Annex B vectors), reassembly and segmentation |
| `test_lapc.c` | `dect_lapc.c`: LAPC frame codec |
| `test_nwk.c` | `dect_nwk.c`: S-format header and information-element codec |
| `test_dsaa.c` | `dect_dsaa.c`: authentication algorithm against published vectors |
| `test_mm.c` | `dect_mm.c`: registration (key allocation) state machine against a simulated handset |
| `test_cc.c` | `dect_cc.c`: call control state machine |
| `test_clip.c` | `dect_clip.c`: calling party number and name |
| `test_callinfo.c` | `dect_callinfo.c`: CALL-INFORMATION element |
| `test_time.c` | `dect_time.c`: time and date to the handset |
| `test_iwu.c` | `dect_iwu.c`: CAT-iq list access responder |
| `test_contacts.c` | `dect_contacts.c`: contact store, its file formats, the Contact List |
| `test_calls.c` | `dect_calls.c`: call store and call lists |
| `test_link.c` | `dect_link.c`: the per-connection C-plane stack |
| `test_diag.c` | `dect_diag.c`: the 0xF625 decode and the monitor JSON |
| `test_profile.c` | `dect_profile.c`: handset profile parser, layers, display expansion |
| `test_ctl.c` | `dect_ctl.c`: control-socket line format, [../docs/bridge-interface.md](../docs/bridge-interface.md) |
| `test_cell.c` | `dect_cell.c`: cell to controller frames |
| `test_base.c` | `dect_base.c`: the handset table and the `struct dect_base` API |
| `test_ssc_align.c` | `src/stubd/ssc_align.c`: the 7412's SSC lane alignment |
| `test_trace.c` | Golden traces of the library stack (`tests/traces/`), below |
| `test_stubd.c` | Golden traces of the whole daemon (`tests/stubd-traces/`), below |
| `test_stubd_cell.c` | The daemon as a cell (`dect-stubd -K`): media on the socket pair, driven by code, no trace files |
| `test_srtp.c` | `src/cell/dect_srtp.c` against RFC 3711 and RFC 7714 vectors; `make check-cell` only |
| `test_fw_extract.py` | `tools/dect-fw-extract.py` on synthetic images; skipped without `mksquashfs` |
| `test_luci_js.py` | The LuCI views against the minifier's regex-after-`return` trap; also `node --check` if node is installed |
| `test_firmware_sh.py` | `openwrt/net/opendectcell/files/firmware.sh` with a fake board profile and fake `uci` |
| `test_package_keep.py` | The sysupgrade keep lists cover what the programs write to flash |
| `test_cell_link.py` | `dect-cell` against a scripted controller; `make check-cell` only |

## Golden traces

A golden trace is a text file: one action per line, `#` for comments. A test
replays the actions and compares what the code sends, byte for byte, with the
expected lines stored in the file. A change to anything that goes on the air or
to the chip then shows up here even where no unit test asserts it.

| Directory | Replayed by | Level | Actions | Expected output lines |
|---|---|---|---|---|
| `tests/traces/` (4 files) | `test_trace.c` | `dect_link`: Cs, Lc, LAPC, MM, CC | `hold`, `callstatus`, `pp`, `setup`, `release`, `tick` | `fp HEX`: each NWK message the base sends |
| `tests/stubd-traces/` (16 files) | `test_stubd.c` | The whole of `dect-stubd` above the chip transport; `dect-stubd.c` is compiled in with `STUBD_TEST` | `sub`, `store`, `args`, `start`, `up`, `on`, `pp`, `down`, `chip`, `tick`, `ctl` | `fp HEX`, `mail PRIM HEX`, `ev LINE` |

The syntax of each is documented in the comment at the top of `test_trace.c`
and `test_stubd.c`. In both, the handset side is played properly (LAPC
sequence numbers modulo 2, each I-frame acknowledged at once), and the daemon's
clock is fake, so runs are repeatable. An expected line belongs to the
action above it: it is what that action makes the code send, in order.

A short example, `tests/traces/locate.trace` (a held handset locates):

```
hold 80 a8 03 11 22 33 44
pp 05 54 05 07 80 a8 03 11 22 33 44 06 07 a0 a5 00 01 23 45 60
fp 85 55 05 05 a0 94 00 11 01 07 01 64 7c 07 90 03 00 01 02 00 84
```

**Adding a trace.** Write the file with only the actions and your comments (say
in a comment what scenario it covers and which clause of the standard it
follows), then run it with `GOLDEN_UPDATE=1`, which rewrites the expected lines
from what the code sends now:

```
make build/test_trace && GOLDEN_UPDATE=1 build/test_trace tests/traces/my-case.trace
make build/test_stubd && GOLDEN_UPDATE=1 build/test_stubd tests/stubd-traces/my-case.trace
```

Then read the recorded lines against the standard (a recording is not
evidence that the output is right), and run the file again without
`GOLDEN_UPDATE`. The test picks up every `*.trace` in its directory, so
nothing else needs registering. The same switch re-records after a deliberate
change of behaviour: review the diff before committing it, and check that an
unchanged old code base still replays the old file. The daemon traces also
serve as the reference for any second host of the stack
([../docs/hosting-dect-base.md](../docs/hosting-dect-base.md) section 8).

**Made-up identities.** Traces are public. They hold only invented values:
identities (`80 a8 03 11 22 33 44`, the base's `a0 a5 00 01 23 45 60`), keys,
PINs, numbers (such as `0301234567`) and names (`Test`). Never put a real
handset's identity, a real key, a PIN or a phone number into a trace or a
test, and never an AVM byte. The same rule applies to the fixtures in the C
tests, which state "no hardware" and "made up" where it matters.

## check.h

[check.h](check.h) is the whole test framework, three macros and two
counters:

| Macro | Use |
|---|---|
| `CHECK(cond)` | Counts a check; on failure prints `file:line: CHECK failed: cond` to stderr |
| `CHECK_EQ(a, b)` | Compares as `long long` and prints both values, decimal and hex, on failure |
| `CHECK_DONE()` | The last statement of `main`: prints `file: N checks, M failed` and evaluates to 1 if any failed, else 0 |

A test is a `main` that calls `CHECK`/`CHECK_EQ` and ends with
`return CHECK_DONE();`. A new C test also needs two Makefile edits: a rule
`$(BUILD)/test_x` (sources, `tests/check.h`, `$(SAN)`) and an entry in `TESTS`.

## Python tests

Run with `python3 -I`, use only the standard library (`test_cell_link.py` also
needs `cryptography`), use synthetic bytes only and exit non-zero on failure.
They are listed explicitly at the end of the `check` rule in the Makefile.

## CI

[../.github/workflows/check.yml](../.github/workflows/check.yml) runs `make
check` on Ubuntu 24.04 for every push and pull request, with read-only
repository permissions. It does not run `make check-cell` or `make cross`. The
packages are built by a separate, manually started workflow
(`.github/workflows/packages.yml`, using `tools/release/build.sh`).

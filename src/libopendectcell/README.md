# libopendectcell

The DECT base-station protocol stack of openDECTcell, as a plain C library. It
turns the mails a DECT chip sends into handset events, calls, lists and
registrations, and turns what the SIP side asks for into mails for the chip.

This page is a map of the directory: which file does what, in which order the
files stack, and where to look for a given behaviour. The documents it links
to hold the details; where this page and a header differ, the header wins.

Contents: [What it is](#what-it-is-and-what-it-is-not) ·
[Layers](#layers) · [Files](#the-files) · [Where do I look for...](#where-do-i-look-for)
· [Conventions](#conventions-for-contributors) · [Tests](#building-and-testing)

## What it is, and what it is not

The library is the whole of the DECT stack above the chip: framing of the
chip's mails, the C-plane (Cs, Lc, LAPC), the network layer (NWK, mobility
management, call control, list access), and on top of that `struct dect_base`,
one base station's worth of state: handset records, links, calls, lists,
notices, timers.

It is **not** a driver and **not** a program. It never touches hardware:

- no file, socket, device or serial I/O;
- no threads, no allocation, no global state (a store such as the contact list
  is one static object the owner provides);
- no clock: every time-dependent call takes a monotonic millisecond `now`, and
  wall-clock time comes from a callback;
- no entropy source: random numbers come from a callback.

Everything outside the stack is the **host's** job. There are two hosts, and
they run the same code:

| Host | What it provides |
|---|---|
| `dect-stubd` ([`src/stubd/`](../stubd/)), the daemon on a box | The chip transport (PCM/SSC or the kernel module), the chip's session, the control and audio sockets towards baresip, store files, the LED |
| The [Go controller](https://github.com/openDECTcell/opendectcell-controller), through cgo | One `dect_base` per cell over the mail tunnel; the cell side of that tunnel is `dect-cell` ([`src/cell/`](../cell/)) and `dect_cell.[ch]` |

### What the host supplies

A host fills in `struct dect_base_ops` (in [`dect_base.h`](dect_base.h)) and
sets `base.ops` and `base.ctx`; `ctx` is passed back to every callback. Any
callback may be NULL, and the library then does without. The callbacks, by job:

| Job | Callbacks |
|---|---|
| The chip | `send` (hand a mail to the chip), `pump` (C-plane frames are queued on a link: take what the transport can hold), `install` (write the served handsets into the chip's slots), `mask` (the chip's locked-in mask changed) |
| Basics | `random`, `log`, `clock` (date and time when known to be right), `wall` (date and time for a call-list entry) |
| Handset records | `save` (a record changed), `forget` (a record was deregistered), `named` (a handset renamed itself), `cap_learned` (its capabilities were proven), `switched` (the current record changed), `registered` (an enrolment finished), `registration_rejected` (one failed: for a per-cell attempt limit), `name_visible` (may handset A see handset B in its names list) |
| The SIP side | `event` (a line for the bridge, see [bridge-interface.md](../../docs/bridge-interface.md)); the verbs come back through `dect_base_ctl_call()` and `dect_base_ctl_admin()` |
| Audio | `call_up`, `call_down`, `side_call`, `side_down`, `lanes`, `link_up`, `swapped` (the host binds and releases voice paths; the library only says when) |

The host calls the library from outside in six places: `dect_base_init()`,
`dect_base_contact()`, `dect_base_conn_up()` / `dect_base_conn_down()`,
`dect_base_segment()` (every C-plane mail), `dect_base_tick()`, and the control
verbs. [hosting-dect-base.md](../../docs/hosting-dect-base.md) is the guide for
writing a host: set-up, the mail-to-call table, every callback with what
`dect-stubd` and the controller do in it, and testing a host against the golden
traces.

## Layers

Arrows point from the user to what it uses, as the `#include` lines and calls
show. A mail comes in at the bottom of the left column and leaves the base as a
call event at the top; outgoing frames take the same way back down.

```
   host: dect-stubd (box)                      host: controller (cgo)
        |                                              |
        |        struct dect_base_ops callbacks        |
        v                                              v
 +-----------------------------------------------------------------------+
 | dect_base   one base: records, links, calls, ring, lists, notices,    |
 |             registration window, cipher, deregistration, time round   |
 |   uses: dect_profile (handset quirks as data), dect_ctl (bridge lines)|
 +-----------------------------------------------------------------------+
        |                                    |
        v                                    v
 +--------------------------------+   +------------------------------+
 | dect_link  one connection:     |   | lists and notices            |
 | glue, MM drained before CC,    |   |   dect_contacts (05H)        |
 | one NWK message per LAPC window|   |   dect_calls (01H..04H)      |
 +--------------------------------+   |   served by dect_iwu (LiA)   |
        |                             +------------------------------+
        v
 +--------------------+  +------------------------------------------+
 | dect_mm            |  | dect_cc   call control                   |
 |   registration,    |  |   dect_clip      calling party identity  |
 |   locate, cipher   |  |   dect_callinfo  line id, call status    |
 |   dect_dsaa        |  |   dect_iwu       list access dialogue    |
 |   (A11/A12/A21/A22)|  |   dect_time      date and time (CISS)    |
 +--------------------+  +------------------------------------------+
        \                          /
         v                        v
 +-----------------------------------------+
 | dect_nwk   S-format header, IE codec    |
 +-----------------------------------------+
                      |
 +-----------------------------------------+
 | dect_lapc  class A acknowledged link    |   "C-plane": what the chip's
 | dect_lc    5-octet fragments, checksum  |   Cs channel carries, one
 | dect_cs    0x0203 in, 0x0244 out        |   connection at a time
 +-----------------------------------------+
                      |
 +-----------------------------------------+
 | dect_mail     framing of the mails      |   "chip interface"
 | dect_session  contact, liveness         |
 | dect_proto    task and primitive ids,   |
 |               the API version           |
 +-----------------------------------------+
                      |
                 the chip (STUB)
```

Not in the stack, but in the directory:

- **Beside it:** `dect_diag` (reads a `struct dect_link` and decodes the chip's
  connection status into data), `dect_cell` (the cell-to-controller wire
  format, independent of everything above).
- **Voice, used by hosts and tools, not by the stack:** `dect_audio` (G.711
  A-law, resampler, tone analysis) and `dect_g722` (the wideband codec).

Dependencies stay one way: no lower file includes a higher one.
`dect_cc.h` pulls in `dect_iwu.h` (list access runs inside a call of class 2),
which pulls in `dect_calls.h` and `dect_contacts.h`.

## The files

One row per `.c`/`.h` pair, lowest layer first. "Follows" is taken from the
file's own header comment. Every test is in [`tests/`](../../tests/); "golden"
means the trace tests of the last section.

| File | What it implements | Follows | Main functions | Tests |
|---|---|---|---|---|
| `dect_proto.h` (header only) | `OPENDECTCELL_API_VERSION`, the task and primitive ids the library uses, the C-plane append buffer size | [stub-api.md](../../docs/stub-api.md) | `OPENDECTCELL_API_VERSION`, `DECT_PRIM_SIGNALLING`, `DECT_PRIM_APPEND_DATA` | used by most tests |
| `opendectcell_version.h` (header only) | The release string for `--version`, set by the build | n/a | `OPENDECTCELL_VERSION` | none |
| `dect_mail` | Framing of mails on the PCM control slot: encode, and a byte-stream receiver with checksum and resync | [stub-pcm-control-channel.md](../../docs/stub-pcm-control-channel.md) | `dect_mail_encode()`, `dect_rx_push()`, `dect_rx_pull()`, `dect_mail_set_swap()` | `test_mail.c` |
| `dect_session` | The contact handshake (0xF600) and the liveness watchdog, a pure state machine with an injected clock | [stub-pcm-control-channel.md](../../docs/stub-pcm-control-channel.md) | `dect_session_init()`, `dect_session_tick()`, `dect_session_rx()`, `dect_session_contact_mail()` | `test_session.c` |
| `dect_cs` | The glue between the chip's connection mails and the DLC: decode 0x0203 segments, build 0x0244 appends | COMMAND_SET notes cited in the header | `dect_cs_decode_segment()`, `dect_cs_append_mail()` | `test_cs.c` |
| `dect_lc` | DLC lower layer on the Cs channel: reassemble 5-octet fragments into frames, build frames, the ISO 8073 checksum with the link signature | ETSI EN 300 175-4 V1.5.1, clauses 6.1 and 7.6-7.10, Annex B | `dect_lc_rx_init()`, `dect_lc_rx_fragment()`, `dect_lc_build()`, `dect_lc_checksum()` | `test_lc.c` |
| `dect_lapc` | LAPC frame codec and the class A acknowledged link (we are the fixed part) | ETSI EN 300 175-4 V1.5.1, clauses 6 and 7 | `dect_lapc_parse()`, `dect_lapc_send()`, `dect_lapc_rx()`, `dect_lapc_poll_tx()` | `test_lapc.c` |
| `dect_nwk` | NWK message codec: the S-format header and information elements | ETSI EN 300 175-5, clauses 6 and 7 | `dect_nwk_parse()`, `dect_nwk_build_hdr()`, `dect_nwk_put_ie()`, `dect_nwk_find_ie()` | `test_nwk.c` |
| `dect_dsaa` | The DECT Standard Authentication Algorithm and the A11/A12/A21/A22 processes | ETSI EN 300 175-7 (Annex K vectors), published DSAA descriptions | `dect_dsaa_pin_to_ac()`, `dect_dsaa_a11()`, `dect_dsaa_a12()`, `dect_dsaa_pin_check()` | `test_dsaa.c` |
| `dect_mm` | Mobility management, fixed side: obtaining access rights (registration), locate, temporary identities, deregistration, detach counting, ciphering of a connection | ETSI EN 300 175-5 clauses 13.2 and 13.8, EN 300 444 (GAP) | `dect_mm_init()`, `dect_mm_rx()`, `dect_mm_poll()`, `dect_mm_start_cipher()` | `test_mm.c`, golden |
| `dect_clip` | CALLING-PARTY-NUMBER and CALLING-PARTY-NAME, with explicit presentation (never leaks a withheld number) | ETSI EN 300 175-5 7.7.9 and 7.7.53 | `dect_clip_init()`, `dect_clip_set_number()`, `dect_clip_put_number()`, `dect_clip_put_name()` | `test_clip.c` |
| `dect_callinfo` | CALL-INFORMATION: line identifier, line type, call identifier, call status | ETSI EN 300 175-5 7.7.56, TS 102 527-3 7.4.5-7.4.6 | `dect_callinfo_init()`, `dect_callinfo_put()`, `dect_callinfo_parse()`, `dect_callinfo_valid()` | `test_callinfo.c` |
| `dect_time` | The TIME-DATE element and the CISS {FACILITY} that carries it to a handset | ETSI EN 300 175-5 7.7.50, TS 102 527-3 7.4.2 and 7.4.20 | `dect_time_put()`, `dect_time_get()`, `dect_time_build_facility()` | `test_time.c` |
| `dect_calls` | The call lists in RAM: Missed (01H), Outgoing (02H), Incoming Accepted (03H) and the merged All Calls (04H), per handset line | TS 102 527-3 V1.7.1, 7.4.10.5.3-6 | `dect_calls_add()`, `dect_calls_at()`, `dect_calls_save()`, `dect_calls_load()` | `test_calls.c` |
| `dect_contacts` | The Contact List store, sorted, with its text file and change journal | TS 102 527-3 V1.7.1, 7.4.10.5.7 | `dect_contacts_load()`, `dect_contacts_put()`, `dect_contacts_take_change()`, `dect_contacts_find_number()` | `test_contacts.c` |
| `dect_iwu` | The CAT-iq list access responder: sessions, reading, editing and deleting entries over {CC-IWU-INFO}; serves the supported lists, internal names, system and line settings, contacts and call lists | TS 102 527-3 V1.7.1, 7.4.10 (and 7.4.11) | `dect_iwu_init()`, `dect_iwu_handle()`, `dect_iwu_add_handset()`, `dect_iwu_set_pin()` | `test_iwu.c`, `test_contacts.c`, `test_calls.c` |
| `dect_cc` | Call control, fixed side, both directions: handset-placed and base-placed calls, codec negotiation, call status, call waiting, parallel calls, list-access calls (class 2) | ETSI EN 300 175-5 clause 9, EN 300 444, TS 102 527-1 and -3 | `dect_cc_init()`, `dect_cc_rx()`, `dect_cc_setup_id()`, `dect_cc_poll()` | `test_cc.c`, golden |
| `dect_link` | One connection's C-plane stack: wires Cs, Lc, LAPC, NWK, MM and CC together, with their ordering rules | the layers above | `dect_link_init()`, `dect_link_rx_segment()`, `dect_link_tick()`, `dect_link_poll_append_mail()` | `test_link.c`, `test_trace.c` |
| `dect_profile` | Handset profiles: the parser, key table and layered resolution of per-model quirks, as data | [handset-profiles.md](../../docs/handset-profiles.md) | `dect_profile_defaults()`, `dect_profile_parse()`, `dect_profile_resolve()`, `dect_profile_describe()` | `test_profile.c` |
| `dect_ctl` | The line format of the bridge's control socket: parse a line, build a line, escaping | [bridge-interface.md](../../docs/bridge-interface.md) section 2 | `dect_ctl_parse()`, `dect_ctl_get()`, `dect_ctl_begin()`, `dect_ctl_add()` | `test_ctl.c` |
| `dect_diag` | Per-handset diagnostics as data: decodes the chip's 0xF625 connection status and fills the protocol half from a link; JSON out | [diagnostics.md](../../docs/diagnostics.md) | `dect_diag_decode_status()`, `dect_diag_from_link()`, `dect_diag_frames_update()`, `dect_diag_json()` | `test_diag.c` |
| `dect_base` | The base station above the links (about 5,900 lines): records, slots in the chip, main and side links, registration window, locate, ring and page, calls in and out, internal calls, parallel calls and waiting, ciphering, deregistration, lists, notices (MWI, missed calls), time round, the bridge's verbs | EN 300 175-5 and TS 102 527-3 per function; the design is [stack-in-library.md](../../docs/stack-in-library.md) | `dect_base_init()`, `dect_base_segment()`, `dect_base_tick()`, `dect_base_ctl_call()` | `test_base.c` (the record table), golden (`test_stubd.c`, `test_stubd_cell.c`) |
| `dect_cell` | The cell-to-controller wire protocol v1: frames, TLV bodies, the mail tunnel's fixed layouts. Parse and build only | the controller's cell protocol document (named in the header) | `dect_cell_parse()`, `dect_cell_put()`, `dect_cell_mail_up()`, `dect_cell_mail_down()` | `test_cell.c`, `test_stubd_cell.c` |
| `dect_audio` | G.711 A-law, a tone analysis for the test tone, the 8/16 kHz resampler of the audio bridge | the STUB's voice slot format, observed | `dect_alaw_encode()`, `dect_alaw_decode()`, `dect_rs2_up()`, `dect_rs2_down()` | `test_audio.c` |
| `dect_g722` | ITU-T G.722 sub-band ADPCM at 64 kbit/s, the wideband codec of the TDM voice lane | ITU-T G.722 clauses 3 and 4 | `dect_g722_init()`, `dect_g722_encode()`, `dect_g722_decode()` | `test_g722.c` |

Two notes on the table. `dect_audio` and `dect_g722` are compiled into
`dect-stubd` and `dect-pcmcap` but included by no other library file. Where a
row names "golden", the module is also exercised end to end by
`tests/test_trace.c` (through `dect_link`) and the daemon traces.

## Where do I look for...

| Question | Where |
|---|---|
| How a received mail becomes a handset event | `dect_base_segment()` in `dect_base.c` takes the 0x0203 mail. It finds the link and calls `dect_link_rx_segment()` (`dect_link.c`), which runs `dect_cs_decode_segment()`, `dect_lc_rx_fragment()`, `dect_lapc_rx()` and `dect_nwk_parse()`, then hands the NWK message to `dect_mm_rx()` or `dect_cc_rx()`. What the link saw comes back in `struct dect_link_ind`; `dect_base` turns it into records, calls and `event()` lines (`dect_base_call_event()`) |
| How the stream of chip bytes becomes mails | `dect_rx_push()` then `dect_rx_pull()` in `dect_mail.c`; the host does this, the library only provides it |
| How connections come and go | `dect_base_conn_up()` and `dect_base_conn_down()`: a fresh main link, a side link while the main link is busy, or a refusal |
| Registration (access rights) | The window: `dect_base_reg_window()`. The handshake: `dect_mm_rx()` in `dect_mm.c` (ACCESS-RIGHTS-REQUEST, authentication, ACCEPT), the maths in `dect_dsaa.c`. Afterwards: `dect_base_main_enrolment()` in `dect_base.c` stores the record, and the host hears `registered()` and `save()`. A failed attempt gives `registration_rejected()` |
| Locate (a known handset comes back) | `dect_mm.c` (LOCATE-REQUEST), then `dect_base_main_located()` in `dect_base.c` |
| An outgoing call (a handset dials) | The handset's SETUP and digits run in `dect_cc.c`. `dect_base_main_dial_tick()` and `dect_base_call_out()` decide: internal call, or a `call-out` event to the bridge. The far end's answer comes back as the `remote-answered` verb through `dect_base_ctl_call()` |
| An incoming call (the SIP side rings a handset) | The `call-in` verb goes to `dect_base_ctl_call()`, then to `dect_base_ring_setup()`, `dect_base_ring_page()` and `dect_base_ring_tick()` (page, wait for the page response, then the base's SETUP through `dect_cc_setup_id()`). While the main link is busy: the side-ring functions (`dect_base_side_ring_start()`, `dect_base_side_ring_setup()`). Call waiting: `dect_base_call_waiting()` |
| Internal calls between handsets | `dect_base_ic_start()`, `dect_base_ic_setup()`, `dect_base_ic_tick()` |
| Caller identity shown on the handset | `dect_clip.c` builds the elements; `dect_base` fills `b->clip` for the next SETUP; the line and call status elements are `dect_callinfo.c` |
| How ciphering is started | `dect_base_main_cipher_start()` and `dect_base_main_cipher_tick()` (main link), `dect_base_side_cipher_tick()` (side links), `dect_base_cipher_start()` and then `dect_mm_start_cipher()` for the challenge; the chip's "cipher on" mail arrives at `dect_base_cipher_on()`. `cfg.no_cipher` keeps calls in the clear. The chip's slot table and key read-back: `dect_base_slot_install()`, `dect_base_uak_verify()` |
| The INT directory (internal names list) | Served by `dect_iwu.c` (list 06H); names live in `b->name[]`, shown names are decided by `name_visible()`; after a change call `dect_base_names_refresh()`; remote handsets of other bases: `dect_base_remote_set()` |
| Contacts (phonebook) | Store and file formats: `dect_contacts.c`. Handsets read and edit it through `dect_iwu.c` (list 05H). A base holds one list (`b->contacts`); a record can have its own (`dect_base_record_contacts()`). What handsets changed: `dect_contacts_take_change()` |
| Call lists (missed, outgoing, accepted, all) | Store: `dect_calls.c` (`b->call_lists`). Served by `dect_iwu.c` (lists 01H to 04H). Entries are added from the `calllog` verb (`dect_base_ctl_call()`). Missed-call counts and notices: `dect_base_missed_due()`, `dect_base_missed_send()`; message-waiting: `dect_base_mwi_send()` |
| Date and time on the handset | `dect_time.c` encodes; `dect_base_time_tick()` and `dect_link_send_time()` send it after a locate; `clock()` and `wall()` supply the time |
| Handset presence and detach | `b->last_seen_ms[i]` is stamped on every segment from record `i`. `{DETACH}` is counted by `dect_mm.c` (`stats.detached`); `dect_base_segment()` copies a new count into `b->detached_ms[i]`. A handset is detached while `detached_ms[i] >= last_seen_ms[i]`. The message is not authenticated: use it for display, never to refuse a call |
| Deregistration | `dect_base_dereg_begin()` and `dect_base_dereg_tick()`; `forget()` tells the host; the `deregister` verb comes in through `dect_base_ctl_admin()` |
| Handset quirks (per model) | Data, not code: `dect_profile.c` and [handset-profiles.md](../../docs/handset-profiles.md); the resolved profile is `b->prof[i]` |
| Radio figures per handset | `dect_diag_decode_status()` and `dect_diag_from_link()` |
| The bridge's text lines | `dect_ctl.c` (format); the verbs and events are handled in `dect_base.c` (`dect_base_ctl_call()`, `dect_base_ctl_admin()`, `event()`) |
| The cell-controller tunnel format | `dect_cell.c` |
| Audio paths | Not here: the host binds them in `call_up()`; the library offers the mails (`dect_base_audio_assign()`, `dect_base_audio_path()`, `dect_base_audio_release()`). Codecs: `dect_g722.c`, `dect_audio.c` |
| Where the API version is bumped | `OPENDECTCELL_API_VERSION` in [`dect_proto.h`](dect_proto.h), and its record in [api-versions.md](../../docs/api-versions.md). Bump both in the same commit |
| A message's IDs (task, primitive) | `dect_proto.h` for those the library uses, [stub-api.md](../../docs/stub-api.md) for the catalogue |

## Conventions for contributors

The full text is [AGENTS.md](../../AGENTS.md), sections 1b, 2 and 5;
[CONTRIBUTING.md](../../CONTRIBUTING.md) covers the workflow. In short:

- **The library has an owner.** Several boxes and the controller share it, and
  the controller carries its own copy with its own traces and API pins. Its
  behaviour comes from the specifications and from documented chip facts and
  changes only with that source, through its owner. If you work on another
  model and a chip answers differently at the mail level, open an issue with
  the evidence instead of patching here (AGENTS.md 1b, rule 3).
- **No per-model code, no compile-time board switches.** Model differences
  belong in the kernel board table, `boards/<model>` and `dect-stubd` options,
  never in this directory or its headers (AGENTS.md 1b rule 4, section 5).
- **No vendor material.** Nothing proprietary goes into the repository: no
  binaries, firmware, listings or analysed code, and no pasted vendor text.
  Facts go in docs in our own words, each with its source and whether it was
  derived, observed on hardware or assumed; the code is written from those docs
  and from the ETSI texts (AGENTS.md section 2). Comment tags like
  `[CR A-NNNN]` name the source record of a chip fact.
- **No platform I/O.** Transport, timers, randomness and the clock are injected
  (section 5). Licence header: `GPL-2.0-or-later` for everything outside
  `kernel/`, so that the AGPL controller can link it.
- **Explicit endianness.** The chip's fields are little-endian, the air
  interface and several wire formats big-endian. Always read and write through
  explicit helpers; never cast a struct onto a wire buffer. The code must build
  for big-endian MIPS, little-endian ARM and x86-64.
- **API version.** Any change to a public function signature or a public struct
  layout bumps `OPENDECTCELL_API_VERSION` and adds an entry to
  [api-versions.md](../../docs/api-versions.md). Behaviour changes and comments
  do not bump it; a behaviour change that matters is still named in the entry.
  The number is a build-time guard for consumers, nothing more.
- **Golden traces.** Anything the base puts on the air is compared byte for
  byte with the traces in `tests/traces/` and `tests/stubd-traces/`. A trace
  changes only on purpose (`GOLDEN_UPDATE=1`), and you review the diff.
- **Tests are not optional.** Each module has a PC test; `make check` and
  `make cross` stay green.
- **Terms:** STUB (the chip firmware), mail, control slot, cell, controller,
  FP and PP, RFPI, IPUI, TPUI, PARK: use them as in AGENTS.md.

## Building and testing

The library has no build of its own; the top-level [Makefile](../../Makefile)
compiles it into the tests and the daemon.

```
make check        # all host tests, built with ASan and UBSan, run in order; exit status is the verdict
make cross        # the ARM binaries (needs the OpenWrt SDK: make cross SDK=/path/to/sdk)
make build/test_link && build/test_link        # one test
GOLDEN_UPDATE=1 build/test_trace               # rewrite a golden trace on purpose, then review the diff
make check-cell MBEDTLS=/path                  # the cell side (needs mbedTLS and Python's cryptography)
```

`make check` is the gate for a commit, and so is `make cross`: the plain `make`
does not build the ARM binary. The compiler flags include `-Werror -Wconversion
-Wshadow`, so a warning fails the build.

### What each test covers

| Test | Covers |
|---|---|
| `test_mail.c`, `test_session.c`, `test_cs.c`, `test_lc.c`, `test_lapc.c`, `test_nwk.c`, `test_dsaa.c`, `test_mm.c`, `test_cc.c`, `test_clip.c`, `test_callinfo.c`, `test_time.c`, `test_iwu.c`, `test_calls.c`, `test_contacts.c`, `test_link.c`, `test_profile.c`, `test_ctl.c`, `test_diag.c`, `test_cell.c`, `test_audio.c`, `test_g722.c` | The module of the same name (see the table above); `test_calls.c` and `test_contacts.c` also test the lists inside `dect_iwu.c`. `check.h` is the tiny shared assertion header |
| `test_base.c` | The handset table and its working copy in `dect_base.c` |
| `test_trace.c` with `tests/traces/*.trace` | Golden traces for `dect_link`: scripted handset NWK messages in (`pp`, `hold`, `setup`, `tick`), every NWK message the base sends compared byte for byte (`fp`). Covers location registration, a handset-placed call with and without call status, and a base-placed call |
| `test_stubd.c` with `tests/stubd-traces/*.trace` | Golden traces for the whole base stack as `dect-stubd` runs it, with the chip and handsets scripted: `register`, `register-ipuio`, `deregister`, `locate`, `locate-second`, `call-in`, `call-out`, `call-waiting`, `dial-local`, `internal-call`, `side-ring`, `list-access`, `list-access-unauthenticated`, `notices`, `promote`, `time-round`. Compared: every mail to the chip and every event line. This is where `dect_base.c` is tested end to end; a host can replay the same traces against its own glue |
| `test_stubd_cell.c` | The cell side of media in `dect-stubd` with `dect_cell` frames |

Tests for other parts of the repository (`test_fwimage.c`, `test_ssc_align.c`,
`test_srtp.c` and the Python tests) are not about this library.

## More

[docs/README.md](../../docs/README.md) is the index of all documents. For the
library in particular:
[hosting-dect-base.md](../../docs/hosting-dect-base.md) (writing a host),
[stack-in-library.md](../../docs/stack-in-library.md) (how the stack moved here,
stage by stage), [api-versions.md](../../docs/api-versions.md) (every public
change), [bridge-interface.md](../../docs/bridge-interface.md) (the verbs),
[spec-conformance.md](../../docs/spec-conformance.md) and
[gap-conformance.md](../../docs/gap-conformance.md) (what is implemented from
the standards).

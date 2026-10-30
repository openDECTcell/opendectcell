# The base stack moves into libopendectcell

Decided 2026-10-30 by the maintainer (opendectcell-controller ADR 0002, option B):
the base-station stack that `dect-stubd` holds today moves into `libopendectcell`,
so the box and the controller run one implementation. This file is the plan
and the record of each stage.

## Where things are

`dect-stubd.c` is 13,400 lines, the library's C files 10,800 (2026-10-30).

| Part | Where |
|---|---|
| Layers: Cs, Lc, LAPC, NWK, MM, CC, DSAA, IWU, CLIP, call info, time, lists | `src/libopendectcell/` |
| Glue per link: `dect_link` (Cs + Lc + LAPC + MM + CC) | `dect_link.[ch]` |
| Chip transport: PCM/SSC framing, control-slot search, bring-up, park | `dect-stubd.c` |
| The stack above the links: registration window and slot writes, the subscription store, locate, side links, paging, calls in and out, internal calls, parallel calls, cipher, lists, MWI, date and time, CLIP, the missed call notification | `dect-stubd.c`, most of it |
| Control socket, cell mode, profiles, LED, state file | `dect-stubd.c` |

Mails enter the stack at `handle_reg_mail()` and leave through
`send_mail()`. Everything between them is what moves.

## The target

A `struct dect_base` in the library: one chip's worth of stack state
(links, handset records, calls, lists), driven by

- `dect_base_mail(base, mail, now)`: a mail from the chip;
- `dect_base_tick(base, now)`: time;
- `dect_base_ctl(base, message, now)`: the bridge's verbs
  (`docs/bridge-interface.md`);

and talking back through callbacks the host sets: send a mail, emit a
bridge event, store a handset record, randomness, log. No I/O, threads or
allocation inside, as for the rest of the library. `dect-stubd` keeps the
chip transport, the sockets, files and the LED, and becomes a host of
`dect_base`; the controller is the other host, over the mail tunnel.

## Stages

Each stage keeps `make check`, `make cross` and the golden traces green,
and changes no byte on the air unless it says so.

| Stage | What | Proof |
|---|---|---|
| **S0** | Daemon-level golden traces: a test build of `dect-stubd.c` with the chip replaced (mails in at `handle_reg_mail()`, out at `send_mail()`), scripted from a handset's NWK messages as `tests/test_trace.c` does for `dect_link` | Registration, locate, an incoming and an outgoing call, list access: the mails the daemon sends today, byte for byte |
| S1 | The handset records and the subscription store behind callbacks (`dect_base` holds them; the files stay in `dect-stubd`) | S0 traces unchanged |
| S2 | Registration and locate into `dect_base` | S0 traces; bench: register and relocate one handset |
| S3 | Calls: in, out, internal, parallel, side links, paging | S0 traces; bench: card A calls |
| S4 | Lists, MWI, date and time, CLIP, missed call notification | S0 traces; bench: card A lists |
| S5 | The control socket verbs as `dect_base_ctl()` | `test_ctl` and S0 |
| S6 | The controller's cgo host (opendectcell-controller C3) over the mail tunnel | a handset registers through the controller |

Bench time is needed from S2 on: each stage is one bench session on the
7412, with the card's outcomes written before the run.

## Record

**S0, 2026-10-30.** `tests/test_stubd.c` compiles `dect-stubd.c` with
`STUBD_TEST`: `main()` hands its set-up state to the harness where it would
open the chip, `send_mail()` hands over each mail, the monotonic and wall
clocks are the harness's, and the run directory and audio socket are
under `build/`. Nothing else in the daemon changed for it, apart from the
pure move of `loop_tick()` out of `run_loop()` (`8d2a09d`). The harness
plays the chip (connection up and down, any mail) and the handsets
(Lc/LAPC framing, acknowledgements) and is a real client on the control
socket. Traces in `tests/stubd-traces/`:

| Trace | Covers |
|---|---|
| `locate.trace` | start-up with one stored handset (bring-up mails, slot clears, subscription write), a location registration with authentication, the date and time |
| `call-out.trace` | an en-bloc CC-SETUP, call proceeding, ciphering, `call-out`, `remote-ringing` and `remote-answered` (ALERTING and CONNECT with the call status IE), the audio bind, the release from the handset |
| `register.trace` | the registration window from the control socket, access rights with key allocation from the PIN, ACCESS-RIGHTS-ACCEPT, the first location registration with the new UAK, the subscription written to the chip, the window closed |
| `list-access.trace` | a list access call (call class 2) with the A540's own commands: start session on list 00, read entries, end session, release; the authentication the base asks for is left unanswered and the list is served anyway |
| `internal-call.trace` | two stored handsets with `-a twocalls`: DECT 1 dials "2" (call class 9), DECT 2 is paged and answers on a side link, both are authenticated and ciphered, ringback, both audio channels bound, the caller hangs up and both links are released |
| `call-in.trace` | the bridge's `call-in`, page, page response, CC-SETUP with CLIP/CNIP, authentication and ciphering (0x0208, 0x0209), alerting, answer, the audio bind (0x0251, 0x0230, 0x022D), `remote-released` and the release (0x0233, 0x0202) |

S0 is complete with these six (2026-10-30). Further traces come with the
stage that needs them (parallel calls and call waiting with S3, the other
lists with S4).

Seen while recording: after a new handset's first location registration
the subscription install (slot clears, `0x020A`, `0x017C`) went to the
chip twice in a row, once from the registration and once from the
start-up restore, which then found a UAK (`register.trace`). Fixed as
task T2 of `docs/tasks-2026-10-30.md`: the registration's install counts
as the restore; the trace lost the 15 repeated mails.

The harness sends the chip's contact answer `0xF601` at start with a
made-up RFPI, `00 01 23 45 60`, the PARK the traces' handsets name.
Authentication answers are computed once from the made-up UAK with the
library's DSAA and written into the trace as plain data.

**S1a, 2026-10-30.** `dect_base.h`: `struct dect_hs_rec`, a handset's
record. dect-stubd's table (`struct handset`) and its working copy of the
current handset (the loose `uak`, `sub_port_id`, `cc_*` and `sub_slot`
fields of `struct stubd`) are now both this type, so `hs_load()` and
`hs_stash()` are struct copies. A rename by pattern, checked by the
compiler; the six traces unchanged.

**S1b, 2026-10-30.** `struct dect_base` (`dect_base.[ch]`): the table of
records, how many are in use, which is current and its working copy;
`dect_base_switch()` stashes and loads. dect-stubd embeds one as
`s->base`; its `struct handset` keeps only what is the daemon's (the
store's path, names, profile, MWI, capabilities), at the same index.
`tests/test_base.c` tests the swap; the six traces unchanged.

**S1c, 2026-10-30.** The save: `struct dect_base_ops` with `save()`,
called by `dect_base_changed()`. dect-stubd's three places that decided
the current record changed now say so through it, and its `sub_save()`
is the callback. Who decides stays where it is until S2 moves
registration. S1 is complete; the six traces unchanged throughout.

## S2, the order (written 2026-10-30, before starting)

S2 is the first stage that moves behaviour. Registration and locate are
spread over the main link's state (`struct stubd`'s `reg`), the mail
dispatch (`handle_reg_mail()` for 0x0200, 0x0201, 0x0203), `reg_start()`,
the identity gate and adoption in the segment path, the chip's slot table
(`install_subscription()`, `send_subscription()`, the slot audit and the
UAK read-back), the enrolment routing (`enrol_route()`) and the
randomness (`gen_rand()`). In this order, each step with the six traces
unchanged:

1. **Host callbacks** in `struct dect_base_ops`: send a mail, draw random
   bytes, write a log line (the library does no I/O). dect-stubd's
   `send_mail()`, `gen_rand()` and `printf` become them.
2. **The chip's slot table**: the writers of 0x0247, 0x020A and 0x017C
   and the audit build their mails from `dect_base`'s records, so they
   move without the links. The build-specific RAM addresses (06.88,
   08.25) come with them as data the host sets.
3. **The main link**: `reg` (its `dect_link`, handle, flags) into
   `dect_base`, and `dect_base_mail()` for connection up, down and the
   C-plane segments on it; `reg_start()`, the identity gate and adoption
   move with it.
4. **Side links** the same way.

Bench after step 3: register a handset and relocate it on the 7412
(card written before the run).

**S2.1, 2026-10-30.** The host callbacks `send()`, `random()` and `log()`
in `dect_base_ops`, with `dect_base_send()`, `dect_base_random()` and
`dect_base_log()`. dect-stubd's `chip_mail_to()` and its eight random draws
go through them already; the traces unchanged.

**S2.2, 2026-10-30.** The chip's slot writers: `dect_base_slot_clear()`,
`_slot_install()` and `_slot_mask()` build 0x0247, 0x020A and 0x017C from
`struct dect_base_chip` (the `attflag=`, `attcfg=` and `noslotcfg` options).
Which handsets go into the chip stays the host's (`send_subscription()`:
roamers, held-back handsets, loaded stores), since a controller decides
that per cell. dect-stubd's `-v` mail log moved into its `send()` callback,
so mails the library sends are logged too. Left for S2.2b: the UAK
read-back and the slot audit (chip memory reads at build-specific
addresses, with their own timers). Traces unchanged.


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
| `locate-second.trace` | two stored handsets, the second locates on the main link: MM adopts its record, it becomes current, its TPUI in the LOCATE-ACCEPT, the first is current again after the release |
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

**S2.3a, 2026-10-30.** `struct dect_base_link` and `base.main_link`: the
main link's handle, its `dect_link`, whether it is up, and the
registration's progress on it (subscription installed, MM state logged,
enrolment routed). dect-stubd's `reg` keeps the call and audio state
until S3. Data only; the traces unchanged.

**S2.3b, 2026-10-30.** `dect_base_link_open()`: the link's part of
`reg_start()` (the flags, `dect_link_init()`, the current identity, the
other records held on the link; which ones the host says, as a mask).
What stays in dect-stubd: the arbitration of a connection-up between the
main link, side links and a call in progress (0x0200 and 0x0201 in
`handle_reg_mail()`), which needs calls (S3) and side links (S2.4) in the
library first, and the ring target's switch. Traces unchanged.

**S2.3c, 2026-10-30.** `dect_base_main_segment()`: the entry of
`reg_segment()` (the challenges from the host's random(), the segment
through the link, the record MM adopted made current). The rest of
`reg_segment()` is dect-stubd's reaction to the indication: logging,
ringing, dialling, calls and the registration's completion; it moves with
the stages it belongs to. A seventh trace, `locate-second.trace` (the
second of two stored handsets locates on the main link and becomes
current), passed against the code before this step as well, with the
daemon's log identical.

**S2.3d, 2026-10-30.** `dect_base_main_registered()`: the registration's
completion on the main link (the new key and identity into the current
record, the host's `install()` and save, the link marked persisted). The
registration window's close and the handset profile stay with the host.
Traces unchanged, unit test added.

S2.3 is done as far as it goes without calls and side links: the
remaining main-link code is the arbitration on connection-up and the
reactions to calls, lists and the clock, which move with S2.4, S3 and S4.
**Bench after S2.3** (`docs/bench-later.md`, N2): register and relocate.

**S2.4, 2026-10-30.** Side links: dect-stubd's `struct side_link` embeds a
`struct dect_base_link`; `dect_base_side_open()` sets one up (every record
the host names held in table order, nobody expected), and
`dect_base_link_segment()` is the segment entry for any link. The chip
slot rule (`subslot=` for record 0, the fallback slot) is
`dect_base_slot()`. The side link's arbitration and its call handling stay
with dect-stubd until S3. Traces unchanged, unit tests added.

**S2 is done** (registration and locate, as far as they go without
calls): the library now owns the handset records, the chip's slot writes,
the links' set-up, their segment entry, adoption and the registration's
completion. **Next is S3, calls**, which carries most of what remains in
`reg_segment()` and the main link's `reg` state.

## S3, calls (started 2026-10-30)

**S3a.** A call's ciphering: `dect_base_uak()`, `dect_base_cipher_start()`
(the challenges and MM's cipher authentication) and
`dect_base_chip_key()` (0x0208, the call key in the chip's layout), for
the main link and side links alike. When to start and what to do on a
failure (release under `authfirst`) stays with the host for now.
dect-stubd's mail log masks 0x0208's key octets as well (it never logged
the mail before; through the library it would have under `-v -p`).
Traces unchanged.

**S3b.** The call state both kinds of link keep (dialling, the internal
call check, the cipher's outcome) is `dect_base_link`'s. Data only; the
main link and side links still reset it at their own points. Traces
unchanged.

**S3c.** The bridge's call table is `dect_base`'s: `struct
dect_base_call`, `calls[3]`, one per audio channel (was dect-stubd's
`struct bcall`; its constants are aliases now). Data only; traces
unchanged.

**S3d.** The call table's lookups (by id, by link, by the handset's call
id, the active one, a free entry) are the library's; dect-stubd's
`bc_*` helpers wrap them. Freeing a call (`bc_clear()`) stays with
dect-stubd while it also stops the main link's ring. Unit tests; traces
unchanged.

**S3e.** `dect_base` holds the side links' `dect_base_link`s
(`side[3]`, with the record each adopted); dect-stubd's `struct
side_link` points to its one and keeps the rest (lane, status, call
staging). The events to the bridge go through the host's `event()`:
`dect_base_call_event()` builds `verb call=ID hs=SLOT ...` with the
handset a link serves (`dect_base_party_slot()`). Unit tests; traces
unchanged.

**S3f.** The ring sequence's state is `dect_base.ring` (state, timers,
pages, the record rung); dect-stubd's `RING_*` names are aliases. Data
only; traces unchanged.

**S3g.** The page: `dect_base_tpui()` (a record's TPUI by MM's rule for
its slot) and `dect_base_page()` (0x0300 with the page information byte
the host chose). All five pages dect-stubd sends (a ring, a side ring, an
internal call, two in deregistration) go through it. Unit test; traces
unchanged.

Next in S3: `ring_tick()` (273 lines) and `ring_queue_setup()`, the main
link's connect staging, and the internal call. These carry most of the
host's options (ring timing, profiles, the line names): moving them means
first giving `dect_base` the call configuration the host now keeps in its
`audio_cfg`.

**S3h.** Each record's handset profile is `dect_base.prof[]`: it steers
the stack (call status, signal, the name's method, the release after a
call), so it sits beside the record. The host still resolves it from the
model and the profile files. Data only; traces unchanged.

**S3i.** How the base runs calls is `dect_base.cfg`: twenty options
dect-stubd parses (`-a` keys of the same names) are copied in once at
start, after main has resolved their defaults, and every read at run
time is the library's copy. Nothing before the copy reads them. Traces
unchanged.

Next in S3: `ring_tick()`, `ring_queue_setup()`, the connect staging and
the internal call can now move on `dect_base` alone, except for the CLIP
and call information the bridge supplies per call, and the audio binds,
which are chip-specific mails like the slot writers.

**S3j.** The audio binds are library writers: 0x0251, 0x0230, 0x022D,
0x0233, with the host passing its bus's values (selector, gain, the TDM
mask, mode and codec bytes). In cell mode the controller sends exactly
these through the tunnel. Every audio mail dect-stubd sends goes through
them. Unit test; traces unchanged.

**S3k.** Each record's terminal capability (seen, Part 3, NG-DECT) is
`dect_base.cap[]`, and the fixed call information option is
`cfg.callinfo`. Data only, with what the ring's SETUP needs now in the
library; traces unchanged.

**S3l.** The ring SETUP's helpers are the library's: a free call id, the
full call information for a Part 3 handset, the basic service by
profile and capability, the page byte. dect-stubd's names wrap them.
Unit tests; traces unchanged. What is left of the ring is its sequencing
(`ring_tick()`, `ring_queue_setup()`), which reads the host's ring timing
and the CLIP the bridge sets per call.

**S3m.** The ring's SETUP is `dect_base_ring_setup()`: the call id, the
call information, the profile's signal and name method, the record's
identities, the retries of a refused SETUP. The caller's identity for it
is `dect_base.clip` (set by the bridge's `call-in` or the `clip=`
options). One log line now names the record instead of the store's
file. Traces unchanged. Left of the ring: `ring_tick()`'s sequencing.

**S3n.** The ring's sequencing is the library's: `dect_base_ring_tick()`
(the release before the page, the in-use wait, the pages, the
page-response deadline, the fallback SETUP on a held link) and
`dect_base_ring_watch()` (a ring lost to a crossing call starts again).
The host gives the stack its start time and the handset's first contact
(`start_ms`, `first_contact_ms`) for the test timings, and a new callback,
`pump()`, sends the frames a link has queued (the LAPC release before the
page). dect-stubd's `ring_tick()` only checks that the chip is up. Unit
tests; traces unchanged.

**S3o.** A call out is the library's: `dect_base_call_out()` takes the
number a handset dialled to the bridge (`call-out`, a call table entry,
the call proceeding), with the call ids (`next_call`) and each record's
Line 2 (`line2[]`, set by the host from its accounts). The host says
whether a bridge client is connected (`bridge`). `dect_base_drain()`
sends what a link's stack queued, main or side. Unit tests; traces
unchanged.

**S3p.** A handset's dialling is the library's: when a number is complete
(`dect_base_dial_tick()`, for the main link and side links alike, with
the authenticate-first rule `dect_base_call_auth_ok()`), the base's own
connect when no one else takes the call (`dect_base_dial_connect()`, the
alertms/bindfirst steps and `dect_base_connect_tick()`), and the Part 3
"CS call connect" after an answer. The main link's call state (audio
bound and connected, the staged CONNECT) is `dect_base_link`'s. Routing
stays with the host for now: the internal call, then the bridge
(`dect_base_call_out()`), then the base's connect. New trace
`dial-local.trace` (overlap digits, no bridge, alertms and bindfirst),
recorded on S3p and replayed unchanged on the commit before it; the
other eight unchanged.

**S3q.** The internal call is the library's: `dect_base_ic_start()`
(refusals, the target found by slot, the page), `dect_base_ic_setup()`
(the SETUP with the caller's number and name, the internal signal, the
shared call id) and `dect_base_ic_tick()` (pages, ringback, the Part 3
name in {CC-INFO}, connect, the ends). Which records hold a subscription
and their names are `dect_base`'s (`loaded[]`, `name[][]`); the host
says how many audio channels a new call can count on (`lanes()`), and
keeps the audio bridge between the two parties and its codec warning.
The page response is still matched to the target by the host
(`page_adopt()`, with the side links' adoption). Unit tests; the nine
traces unchanged.

**S3r.** Calls in are the library's up to the bridge verbs: a call's end
(`dect_base_call_clear()`, which stops the main link's ring), the main
link's ring for a call (`dect_base_ring_main()`), and the side ring, a
call-in for a handset while the main link is busy: its page, its SETUP on
the side link the handset comes up on, its display text and CS call
connect (`dect_base_side_ring_*()`). The profile's display text on
ALERTING and the infoie= lab option are `dect_base_name_info()` and
`dect_base_info_tick()`. New trace `side-ring.trace` (DECT 1 in a call
out, a call-in for DECT 2 rung on a side link), replayed unchanged on
the commit before; the other nine unchanged.

**S3s.** Parallel calls on one link (TS 102 527-3 §7.4.3) are the
library's, moved whole: call waiting, the handset's control codes
(accept, reject, hold, resume, toggle, release, a second call out),
the call states in {CC-INFO}, the DTMF of the active call. New trace
`call-waiting.trace` (a second call to DECT 1 in a call: accept with
hold, toggle, release), replayed unchanged on the commit before; the
other ten unchanged.

**S3t.** The bridged calls' progress is the library's
(`dect_base_bridge_tick()`): parallel mode per link, side rings, the
events ringing, answered and released, and the end of a call-in no
handset took. Unit test; the eleven traces unchanged.

**S3u.** The release after a call (nlr) is the library's
(`dect_base_nlr_tick()`), with its state on the main link's
`dect_base_link`. Seven trace runs take it (six after a call, one after a
list session with the DISC first); the fourteen traces unchanged.

## S5, the control verbs (started 2026-10-30)

**S5a.** The bridge's call verbs are the library's:
`dect_base_ctl_call()` takes call-in (busy, call waiting, the main
link's ring or a side ring, the caller's name from the Contact List) and
remote-ringing, remote-answered and remote-released, and says what the
host answers its client (`struct dect_base_reply`). The host still owns
the sockets and decides which client is the bridge. Which records are
listed (`dect_base_listed()`, with `absent[]`) and the Contact List the
host keeps (`contacts`) are `dect_base`'s. Unit tests; the eleven traces
unchanged. Still the host's: `bridge`, `mwi` (S4) and `calllog` (S4).

## S4, lists and notices (started 2026-10-30)

**S4a.** The notifications are the library's: the list change notices
after a location registration, voice messages waiting, missed calls, the
Internal Names List's change, and who may hear them (Part 3 by what the
handset reported once proven, or its profile). Each record's notice
state (`note[]`), the host's call lists (`call_lists`) and the records to
page for a notice (`page_want`, read by the host's paging round) are
`dect_base`'s. New trace `notices.trace` (a Part 3 handset locates and
authenticates; the bridge reports two messages and a missed call),
replayed unchanged on the commit before; the other eleven unchanged.

Seen while recording: the notices after a location registration go to a
handset whose link reported Part 3 even before it authenticated, while
those that wait for a link (`dect_base_missed_due()`) count only a
capability learned after authentication (audit A3). Harmless (a notice
carries counts, no data), but not one rule; left as it is.

**S4b.** Deregistration is the library's: the request on the handset's
link or a C-plane page first, the queue, the erasure (chip slot, record
state, the lists) with or without the handset's answer, and a handset
that deregistered itself. The host keeps the store files (`forget()`
moves them aside and tells the clients) and the locked-in mask policy
(`mask()`), and SIGUSR2's request file. One order changed: a SIGUSR2
request is now taken before a self-deregistration seen in the same tick
(was after); both are erased either way. New trace `deregister.trace`
(DECT 2 paged, the request, its accept, the erasure), replayed unchanged
on the commit before; the other twelve unchanged.

**S4c.** The periodic date and time is the library's
(`dect_base_time_tick()`): the round, the C-plane page of a handset with
no link, the {FACILITY}, the release of the link it brought up, and the
notices for handsets with no link riding the same round. The host says
whether its clock is right (`clock()`) and calls the tick while the radio
is on. New trace `time-round.trace` (timeupdate=1: DECT 1 paged, the
date and time, the release), replayed unchanged on the commit before;
the other thirteen unchanged.

**S4d.** The lists' hooks per link are the library's
(`dect_base_lists_tick()`): a name a handset saved in the Internal Names
List (kept in `name[]`, spread to every link, the change notice; the host
saves the file and tells its clients through `named()`), the UAK for the
handset's authentication of the base, the base PIN and the hour's lock
after ten wrong ones, the cipher state, who is in a call, and a deletion
from a handset's list as a deregistration. Unit tests (no trace edits a
list yet); the fourteen traces unchanged.

**S5b.** The bridge's `mwi` and `calllog` are the library's too
(`dect_base_ctl_call()`): the messages waiting per line, to the handset
on its link or at the next paging round, and a finished call into the
call lists with its missed-call notices. The host gives the wall time
for an entry (`wall()`). Left with the host: `bridge` itself (which
client carries the calls). `notices.trace` covers both; the fourteen
traces unchanged.

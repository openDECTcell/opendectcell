# The cell's call interface: dect-stubd ↔ SIP bridge (design, 2026-10-28)

ROADMAP M9b, decided 2026-10-28: a **direct bridge** between `dect-stubd` and
baresip, with an option to use a controller instead that stays a dummy until
the cell protocol exists. The bridge carries the events the cell protocol will
carry, so the controller can replace it without touching `dect-stubd`. This
page is that interface: the messages, who sends them, which DECT procedure
each one drives (all built and unit-tested, see `lines-and-calls.md`), and
who may send what. Built: dect-stubd's side and the baresip module (§5).

## 1. Pieces

```
handsets ~~ air ~~ chip ── dect-stubd ──(control socket)── baresip + module opendectcell ── SIP
                                 └──────(audio socket)─────────┘
```

- **dect-stubd** stays the DECT side: it owns the handsets, the line pool
  (`dect_iwu` lines), call ids, the CC state per handset.
- **The bridge** is the baresip module `opendectcell` (`src/baresip/opendectcell.c`,
  package `opendectcell-baresip`), loaded into baresip itself: it is the control
  socket's client, translates the messages below to and from baresip's
  calls, and is baresip's audio source and player on the audio socket.
  One baresip account per VoIP profile that is a handset's line; the init
  script writes the accounts and the line map `/var/run/opendectcell/lines`
  (`<hs> <line> <aor>`) at every start (§5, step 4).
- **baresip** (OpenWrt telephony feed, BSD) does SIP, RTP and the codecs on
  the network side.
- Later the **controller** speaks the same messages over the network instead
  of the module; nothing of it is built yet.

## 2. The control socket

- A Unix stream socket, `/var/run/opendectcell.sock`, mode 0600, owned by root.
  One message per line, UTF-8: a verb, then `key=value` pairs; values with
  spaces in double quotes. Unknown verbs and keys are logged and ignored, so
  either side can be newer.
- `dect-stubd` listens; one client at a time (the bridge or, later, the
  controller's cell agent).
- Ids: `hs` is the handset's chip slot / terminal number (the Internal Names
  "Number"), `line` the pool line id (0..126), `call` the system-wide call id
  dect-stubd allocates (1..127, TS 102 527-3 §7.4.6.1). The bridge keeps its
  own SIP call handle per `call`.

### 2.1 dect-stubd → bridge (events)

| message | when | DECT side (already built) |
|---|---|---|
| `hello api=79 lines=1 hs=3` | client connects (then an `hs ... state=registered` line per handset) | — |
| `hs hs=2 state=registered name="Kueche" model=gigaset-c430a part3=0 wideband=1` | on connect, and when a handset is renamed (`model` with a profile, `part3` once its capabilities are known; `wideband=0` when we call it narrowband: a GAP handset, its profile, or wideband off); `state=attached` on location registration comes with step 2 | `names_take()`, profiles, `target_svc()` |
| `hs hs=2 state=gone` | deregistered | `dereg_erase()` |
| `call-out call=5 hs=1 line=1 codec=wb number=0301234567` | a handset's number is complete (en bloc or <<SENDING-COMPLETE>>, `#`, the dial pause, `1C 15`), on the main link or a side link; `line` is the one the handset chose (TS 102 527-3 §7.4.5.2) if it has that line, else 1; `codec=nb` when the handset's call is narrowband (the INVITE then offers G.711 only) | `dial_route()`, `side_route()`, `dect_cc_line()`, `dect_cc_codec()` |
| `call-out call=5 hs=1 line=1 codec=wb class=int number=201` | with `cfg.bridge_internal` (API 153): the handset's call is internal (call class 9) and `number` is what it dialled after INT or the entry it chose; the host routes it. Without `bridge_internal` an internal call stays in the base | `dect_base_call_out()` |
| `digit call=5 d=7` | a key during the connected call (DTMF, TS 102 527-3 §7.4.14): the bridge sends it as the account's `dtmfmode` says | `par_tick()`: the keypad once the call connected |
| `hold call=5` / `resume call=5` | a Part 3 handset put the call on hold (1CH 41H, or by accepting a waiting call or toggling) or took it back (1CH 42H, 1CH 31H) | `par_control()` |
| `ringing call=7 hs=2` | a handset alerts on our call | CC RINGING |
| `answered call=7 hs=2` | a handset answers (the others are released, §7.4.3.5.4) | CC CONNECT, CS call connect |
| `released call=5 by=hs reason=normal` | a handset hangs up or the call failed | CC release, `dect_cc_release_reason()` |
| `busy sip=abc why=channels\|in-call` | a call-in cannot be offered: the handset is in a call or being rung (`in-call`, also when three calls are up), or no audio channel is free (`channels`) | `hs_in_call()`, `lanes_available()` (A-0124) |

### 2.2 bridge → dect-stubd (commands)

| message | from SIP | DECT side |
|---|---|---|
| `lines line=1 name="Line 1" hs=1,2 line=2 name="Office" hs=2,3` | the configured pool | `dect_iwu_clear_lines()`, `dect_iwu_add_line()`, list change notices |
| `call-in line=1 hs=2 number=0301234567 name="Wednesday" codec=nb sip=abc` | INVITE on a line | allocate `call`, page and ring handset `hs` (default: the first) with the full call information to a Part 3 handset, naming `line`; on the main link if it is free, else on a side link (a side ring); `codec=nb` (the caller offered no G.722): page and SETUP for basic speech, G.726. Answer `call-in-ok call=7 sip=abc` or `busy`. An account that is the line of several handsets gets one call-in per handset from the bridge |
| `remote-ringing call=5` | 180/183 | {CC-ALERTING} to the handset (ring-back tone to a Part 3 handset, <<SIGNAL>> 01H) |
| `remote-answered call=5 codec=nb` | 200 OK | {CC-CONNECT} with CS call connect (TS 102 527-3 §7.4.6.4.3: only when end to end connected); `codec=nb` (the far end answered G.711): the CONNECT names G.726 if the handset offered it (`dect_cc_prefer_narrowband()`) |
| `remote-released call=5 reason=busy\|normal\|unreachable\|rejected` | BYE, 486, 404, 603 | busy: the busy tone and CS disconnecting to Part 3 (§7.4.8.3), else {CC-RELEASE} with the reason (14H user busy, 00H normal, 12H user unknown, 15H user rejection) |
| `call-in hs=2 int=1 number=201 name="Buero" codec=nb sip=abc` | (API 153) an internal call: rings handset `hs` with the internal pattern, no line, the caller's internal number and name; busy (`in-call`), never call waiting, if the handset is in a call | `call_in()`, `ring_svc_for()` |
| `mwi hs=2 line=1 count=3` | NOTIFY message-summary (RFC 3842, baresip's mwi module): one per handset line the account is | {FACILITY} with <<EVENTS-NOTIFICATION>> message waiting, voice, the count, the line (§7.4.1.2): at once on the handset's link, else in the date-and-time round's next visit, and again after each location registration (`mwi_send()`) |
| `calllog kind=missed number=0301234 name=Anna legs=1:1,2:2` | a SIP call to or from an account ended (incoming) or was placed (outgoing); kind `missed`, `accepted` or `outgoing`; `legs`, every handset that has the account as a line and which of its lines | an entry in the call lists (TS 102 527-3 §7.4.10.5.3-5, `dect_calls.h`), the Contact List's name before the network's; for a missed call the missed call notification (§7.4.1.3) to those handsets: at once on a link, else by a page |
| `deregister hs=3` | the LuCI page | `dereg_begin()` (replaces SIGUSR2 + file) |
| `register-window seconds=120 profile=gigaset-a540` | the LuCI page | `reg_window()` (replaces SIGUSR1); `profile=`, optional, is the model picked before registering: the handset that enrols in this window gets it as `<store>.profile` (`reg_profile_apply()`) |

Up to three calls (the chip's audio channels) run at once, each with
its own `call` id. A Part 3 handset (call status) also runs parallel
calls on its one link (TS 102 527-3 §7.4.3): a `call-in` for it while it
is in a bridged call becomes call waiting instead of `busy`; accepting
it holds the active call (`hold`, `answered`), 1CH 38H releases it
instead (`released`), rejecting it is `released by=hs`; 1CH 15H and a
number places a second call (`call-out`, the first goes on hold); toggle,
hold, resume and releasing one call map to `hold`, `resume` and
`released`. The far end releasing one of them sends that call's CS idle,
not a release of the link. Internal parallel calls (17H), transfer and
conference are refused with "control code not supported".

### 2.3 Answers and the client

Every command is answered on the same connection: `ok verb=deregister hs=3`,
`ok verb=register-window seconds=120`, or `error verb=X reason=R` with R one
of `unknown`, `malformed`, `bad-hs`, `no-handset`, `bad-seconds`,
`bad-profile` (no such file in the profile directory), `not-ready`. A second client gets `error reason=busy` and is closed.
`register-window` without `seconds` uses `regwindow=` (120 s).

`dect-stubd --ctl LINE...` is a client for scripts and the bench (the box
has no socat): it prints `hello`, the `hs` lines and the answers.
`-a ctl=PATH` moves the socket, `-a noctl` leaves it out; it exists only
while the daemon runs (`-g`) and is removed on exit.

## 3. Audio

The rule: the phones code the speech; the box converts only when the two
ends share no codec. Codecs are matched through signalling (§2: `codec=`
on `call-out`, `call-in`, `remote-answered`, and `wideband=` on `hs`), and
the audio socket runs at each call's SIP rate.

- One listening `SOCK_SEQPACKET` socket, `/var/run/opendectcell-audio.sock`
  (mode 0600), and one connection per call: once a call's speech is
  bound, dect-stubd announces it as `audio call=5 path=...`; the bridge
  connects and sends its first message: `call=5 codec=g722` when the
  call's SIP codec is G.722, else `call=5 rate=8000` (or `16000`), and
  the call's lane takes the connection. A connection that names no
  waiting call within 2 s is closed. When a re-INVITE changes the SIP
  codec, the bridge connects again with the new format, and the new
  connection replaces the old one.
- Frames of 20 ms both directions on the same connection: 160 G.722
  octets, or linear 16-bit PCM at the rate (320 or 640 octets, host byte
  order: both ends run on the box). A message of one octet from the
  bridge says it has nothing for this frame (dect-stubd fills with
  silence). What dect-stubd does with them:

  | DECT lane | socket | in dect-stubd | when |
  |---|---|---|---|
  | wideband (G.722 octets) | G.722 | nothing: the octets pass | G.722 at both ends: the matched case |
  | narrowband (linear, 8 kHz) | 8 kHz | nothing: the samples pass | G.726 handset, G.711 far end: the matched case |
  | narrowband | G.722 | resampling and G.722 coding | a far end with G.722 only, a narrowband handset |
  | wideband | 8 kHz | G.722 decoding and resampling | a handset that offered G.722 only, a far end with G.711 only |
  | either | 16 kHz | G.722 coding or resampling | a 16 kHz codec other than G.722 |

- The bridge feeds them to its baresip audio module (`ausrc`/`auplay`
  named `opendectcell`, the device named per call), which runs at the call's
  codec rate (the config sets no `ausrc_srate`). The module is baresip's
  "G722" codec as well: it passes the octets through, each carried as two
  equal samples in baresip's pipeline, so a wideband call is coded by the
  two phones alone (baresip's g722 module and spandsp are not used).
  G.711 is baresip's, a table lookup. `auresamp` adapts baresip's own
  rates where they differ from the codec's.
- The bridge picks the SIP codecs to match the handsets: a narrowband
  handset's INVITE offers G.711 only, an account whose handsets are all
  narrowband answers with G.711 only (`account_set_audio_codecs()` around
  the call's creation), and a caller who offers no G.722 rings the
  handsets narrowband.
- Internal calls stay inside dect-stubd (the host bridge of A-0124); the
  bridge never sees them.

## 4. Who may send what (task 37)

- The control socket is root-only, the same trust as the signals it replaces.
  `deregister`, `register-window` and `lines` are configuration: only from
  this socket, never derived from anything a SIP peer sends.
- Nothing from the SIP side reaches MM: an INVITE can ring a handset, never
  register, deregister, rename or re-profile one.
- The controller, later, gets the same messages over an authenticated
  channel (mutual TLS is the working assumption); the cell accepts
  configuration commands only from it. That belongs in the controller's own
  design, which this page feeds.
- On the air the protections stay as built: a handset's own deregistration
  needs its key (API 74), deleting another handset needs the base PIN on a
  ciphered link (API 76), a wrong RES ends a call (API 75), and `locauth`
  (API 77), on by default since the bench cleared it (test 19).

## 5. Order of work

1. **Done** (API 79): the control socket in dect-stubd, `hello`, `hs`,
   `deregister`, `register-window`; checked on the box with `--ctl`.
2. **Built** (2026-10-28): `bridge`, `call-out`, `ringing`, `answered`,
   `released`, `call-in` (rings the first handset), `remote-ringing`,
   `remote-answered`, `remote-released`; one bridged call at a time on the
   main link. Without a client that sent `bridge`, a dialled call is
   connected locally as before.
3. **Built** (2026-10-28), wideband first: `audio call=N
   path=/var/run/opendectcell-audio.sock rate=16000 frame=640` once the call's
   audio is bound; the bridge connects (SOCK_SEQPACKET) and both sides
   exchange 640-octet frames of 16 kHz linear PCM. dect-stubd codes them
   to and from the call's G.722 lane (40 ms of priming, silence on an
   underrun, each of the handset's coded words taken once). A narrowband
   call got no audio socket yet (it does since step 5). `dect-stubd --bridge-echo [ring
   [NUMBER]]` is a test bridge: it answers any dialled number and plays
   the caller's voice back 0.5 s late; `ring` also places a call-in.
4. **Built** (2026-10-28) as a baresip module, not a separate daemon:
   `src/baresip/opendectcell.c` (package `opendectcell-baresip`, `make baresip-mod`).
   It is the control client (it sends `bridge`), the audio source and
   player `opendectcell` on the audio socket, and maps accounts to handsets via
   `/var/run/opendectcell/lines`. The init script writes baresip's `config` and
   `accounts` from each handset's Line 1/Line 2 and the VoIP tab, and runs
   baresip when a line is on. A call to an account rings its handset
   (`call-in hs=N`); a handset's calls go out on its Line 1. baresip 3
   leaves accepting a new session to the application: the module does it
   for the handsets' accounts (404 otherwise, 503 while dect-stubd is
   away). Unanswered call-ins end with 480 (nobody answered the page) or
   486 (the handset declined). Checked on the box with an INVITE from a
   test peer to an invented handset: 180 at once, the base paged it,
   480 after 60 s.
   Hardened 2026-10-29 (built, not yet tested on SIP): the caller's name
   goes out cut to the CLIP's 64 octets (at a UTF-8 boundary) and the
   number to its 32, so a call-in always fits the line format. A call-in
   that dect-stubd answers with `error verb=call-in` (or `error verb=?`)
   is the oldest one still waiting, since answers come in command order:
   its SIP call gets 480 (`no-handset`, `bad-hs`) or 503, and its slot is
   freed; a call-in that cannot be sent gets 503 at once. A caller who
   hangs up before `call-in-ok` keeps the slot until it comes, and the
   bridge then sends `remote-released` for that call (as for any
   `call-in-ok` it has no caller for). Each call-in has a 90 s limit in
   the bridge (480, `remote-released`), above dect-stubd's 75 s. An
   INVITE to an account that registers is taken only from the address
   its last REGISTER 2xx came from (baresip's `ua_req_check_origin()`),
   else 403; `opendectcell_any_source yes` in baresip's config turns that off.
5. **Built** (2026-10-29): up to three bridged calls at once, each with
   its own audio connection; a handset on a side link dials out, and a
   call-in for a handset while the main link is in a call rings it on a
   side link. Narrowband calls are resampled to the socket's 16 kHz.
   Numbers sent complete (redial, the phonebook) go to the bridge too.
   A VoIP profile may be the line of several handsets: the bridge sends
   one call-in per handset, the first `answered` takes the SIP call and
   the other legs get `remote-released`. The line a handset picks goes
   out with `call-out`, the called one comes in with `call-in`. Not yet
   tested on air.
6. `lines` and the pool in dect-stubd (one line list for the cell rather
   than per handset files), when the controller needs it.

## Open points

- Decided: a baresip module (fewer processes); see step 4.
- Whether the audio socket should carry G.722 untouched for a G.722 SIP
  call and save the decode and encode (wideband to wideband).

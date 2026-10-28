# The cell's call interface: dect-stubd ↔ SIP bridge (design, 2026-10-28)

ROADMAP M9b, decided 2026-10-28: a **direct bridge** between `dect-stubd` and
baresip, with an option to use a controller instead that stays a dummy until
the cell protocol exists. The bridge carries the events the cell protocol will
carry, so the controller can replace it without touching `dect-stubd`. This
page is that interface: the messages, who sends them, which DECT procedure
each one drives (all built and unit-tested, see `lines-and-calls.md`), and
who may send what. Not built yet; step 7 of `lines-and-calls.md`.

## 1. Pieces

```
handsets ~~ air ~~ chip ── dect-stubd ──(control socket)── opendectcell-sipd ──(ctrl_tcp)── baresip ── SIP
                                 └──────(audio socket)──────────┘
```

- **dect-stubd** stays the DECT side: it owns the handsets, the line pool
  (`dect_iwu` lines), call ids, the CC state per handset.
- **opendectcell-sipd**, a small new daemon, is the bridge: it translates the
  messages below to baresip's `ctrl_tcp` commands and events (JSON in
  netstrings), and moves audio between the audio socket and a baresip audio
  module. One SIP account per pool line.
- **baresip** (OpenWrt telephony feed, BSD) does SIP, RTP and the codecs on
  the network side.
- Later the **controller** speaks the same messages over the network instead of
  opendectcell-sipd; `-a controller=ADDR` is accepted today and logs "not
  implemented, running standalone".

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
| `hello api=77 lines=1 hs=3` | client connects | — |
| `hs hs=2 state=attached name="Kueche" model=gigaset-c430a part3=0` | a handset locates, is renamed, changes profile | location registration, `names_take()`, profiles |
| `hs hs=2 state=gone` | deregistered | `dereg_erase()` |
| `call-out call=5 hs=1 line=1 number=0301234567` | a handset's number is complete (en bloc, `#`, the dial pause, `1C 15`) | `dial_route()`, the line choice (`dect_cc_line()`) |
| `digit call=5 d=7` | keypad during a connected call (DTMF, TS 102 527-3 §7.4.14) | `learn_keypad()` in ACTIVE |
| `ringing call=7 hs=2` | a handset alerts on our call | CC RINGING |
| `answered call=7 hs=2` | a handset answers (the others are released, §7.4.3.5.4) | CC CONNECT, CS call connect |
| `released call=5 by=hs reason=normal` | a handset hangs up or the call failed | CC release, `dect_cc_release_reason()` |
| `busy call=7 why=channels\|in-call` | an incoming call cannot be offered | `link_in_use()`, the channel count (A-0124) |

### 2.2 bridge → dect-stubd (commands)

| message | from SIP | DECT side |
|---|---|---|
| `lines line=1 name="Line 1" hs=1,2 line=2 name="Office" hs=2,3` | the configured pool | `dect_iwu_clear_lines()`, `dect_iwu_add_line()`, list change notices |
| `call-in line=1 number=0301234567 name="Wednesday" sip=abc` | INVITE on a line | allocate `call`, page and ring every idle attached handset with the full call information to Part 3 handsets; answer `call-in-ok call=7 sip=abc` or `busy` |
| `remote-ringing call=5` | 180/183 | {CC-ALERTING} to the handset (ring-back tone to a Part 3 handset, <<SIGNAL>> 01H) |
| `remote-answered call=5` | 200 OK | {CC-CONNECT} with CS call connect (TS 102 527-3 §7.4.6.4.3: only when end to end connected) |
| `remote-released call=5 reason=busy\|normal\|unreachable\|rejected` | BYE, 486, 404, 603 | busy: the busy tone and CS disconnecting to Part 3 (§7.4.8.3), else {CC-RELEASE} with the reason (14H user busy, 00H normal, 12H user unknown, 15H user rejection) |
| `mwi line=1 count=3` | NOTIFY message-summary | events notification type 0 (§7.4.1.2), later |
| `deregister hs=3` | the LuCI page | `dereg_begin()` (replaces SIGUSR2 + file) |
| `register-window seconds=120` | the LuCI page | `reg_window()` (replaces SIGUSR1) |

Call waiting, hold and toggle (lines-and-calls.md steps 5-6) add
`call-in` for a busy Part 3 handset (call waiting instead of `busy`),
`hold`/`resume`/`toggle call=` from the handset's `1C` codes, and their
answers; they come with those steps.

## 3. Audio

- A second Unix socket per call, `SOCK_SEQPACKET`, opened by dect-stubd when
  a call's audio is bound and announced as `audio call=5 path=... rate=16000`.
- Frames of 20 ms of **linear 16-bit PCM**, little-endian: 640 octets at
  16 kHz (wideband, the handset's G.722 decoded by dect-stubd's `dect_g722`),
  320 at 8 kHz (narrowband, from the chip's G.726 path as linear PCM, the
  mode-2 bind). Both directions on the same socket.
- The bridge feeds them to a small baresip audio module (`ausrc`/`auplay`
  named `opendectcell`); baresip encodes for the SIP side (G.722, G.711, Opus).
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
  (API 77) once the bench has cleared it.

## 5. Order of work

1. The control socket in dect-stubd, `hello`, `hs`, `deregister`,
   `register-window`: no SIP needed, testable with `socat`.
2. `call-out`, `released`, `remote-*` against a scripted client (socat or a
   test program): the DECT side of a SIP call without SIP.
3. The audio socket for one call, narrowband first (the chip's linear PCM),
   checked with a loop-back client.
4. opendectcell-sipd with baresip on the box: one line, calls out, then in.
5. `lines` and the pool: two lines, ringing all attached handsets
   (lines-and-calls.md step 4).
6. Wideband audio over the socket (the G.722 path).

## Open points

- Whether opendectcell-sipd is C (one more binary) or a baresip module inside
  baresip (fewer processes, but our code in baresip's process).
- The narrowband linear PCM path: which bind gives clean linear PCM both ways
  today (`call-g7.md`), and whether the audio socket should carry G.722
  untouched instead and let baresip decode it.

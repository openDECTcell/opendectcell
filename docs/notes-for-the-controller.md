# Notes for the controller

Things the **cell side** (`openDECTcell/opendectcell`) needs the **controller side**
(`openDECTcell/opendectcell-controller`) to know: API and ABI changes, behaviour that
would otherwise be surprising, and results that unblock or redirect controller
work.

This file exists so neither side has to relay findings through the maintainer
by hand. **It is append-only and dated, newest last.** After a `git pull` of
the submodule, read any section dated later than your last read.

Nothing here is a task assignment. Issues remain the place for that; this is
context.

---

## 2026-10-22

### 1. `OPENDECTCELL_API_VERSION` is now 3 — rebuild, do not relink

`struct dect_cc` gained an embedded `struct dect_iwu` (see below), so **its
layout changed**. Anything that allocates, copies or embeds a `struct dect_cc`
must be **recompiled**, not just relinked. A stale object file linked against
the new library will not fail loudly — it will corrupt memory at an offset that
has nothing to do with the field you changed.

Also noted on opendectcell-controller issue `#47`.

### 2. A `{CC-CONNECT}` is NOT a voice call — gate audio on the call class

The most consequential item here. **Read this before bridging any audio.**

`<<BASIC-SERVICE>>` (`0xE0`) carries a value octet whose **high nibble is the
call class** and low nibble the service (EN 300 175-5 V2.9.1 §7.6.4):

| class | meaning | class | meaning |
|---|---|---|---|
| `0x2` | **LiA service setup** (CAT-iq list access) | `0x9` | internal call |
| `0x3` | ULE service | `0xA` | emergency |
| `0x4` | message call | `0xB` | service call |
| `0x7` | DECT/ISDN IIP | `0xC` | external handover |
| `0x8` | **normal call setup** | `0xD` | supplementary service |
|  |  | `0xE` | OA&M |

Services: `0x0` basic speech defaults, `0x8` wideband speech defaults,
`0x9`/`0xA` SUOTA, `0xB` DTAM wideband, `0xF` other.

So `e0 88` = normal call, wideband speech — a real voice call. `e0 28` = **list
access**, which is a handset reading your phonebook or call list. It completes
a full `{CC-SETUP}` / `{CC-CALL-PROC}` / `{CC-CONNECT}` / `{CC-RELEASE}`
sequence that is indistinguishable from a voice call unless you read the call
class.

**This is not a corner case.** A Gigaset A540 opens these constantly. In a
four-minute bench run on 2026-10-22:

```
7 × e0 28   list access
1 × e0 88   the actual voice call the maintainer placed
```

Seven to one. Our own `dect-stubd` currently binds the audio channel on *any*
`{CC-CONNECT}`, which is a bug on our side — on hardware that evening it played
a file into every list-access session, with the handset showing no call. We are
fixing it. **Do not reproduce it:** a controller that bridges on `{CC-CONNECT}`
alone will connect phonebook lookups to SIP.

Gate on call class `0x8` (and `0x9` for internal calls, once those exist).

### 3. `dect_iwu.c` / `dect_iwu.h` — the list-access transport, verified on hardware

New in `libopendectcell`. It answers the `<<IWU-TO-IWU>>` (`0x77`) session dialog
carried inside `{CC-IWU-INFO}`, which is what CAT-iq list access actually rides
on. Records begin with the discriminator `0xC3`; the handset opens the session
and **we echo its session id** rather than generating one.

Measured on a real A540, 2026-10-22, in one 240 s run:

| received | n | we answered | n |
|---|---|---|---|
| `c3 00` open | 22 | `c3 01` open response | **22** |
| `c3 02` close | 19 | `c3 03` close ack | **19** |
| `c3 06` request | 17 | `c3 07` response | **17** |
| `c3 04` *(unknown)* | 5 | — | **0** |

Before this existed the handset opened these sessions and got silence. It now
carries the dialog through to the close.

Two things to know if you want list access:

- **`c3 04` is unhandled.** It had never been seen before, because the dialog
  never previously got far enough to produce it. Five occurrences in that run.
  That is the next piece of work for list access, and it needs bench
  observation, not guessing.
- The payload field *semantics* (`0x82`/`0x83`/`0x84`, `ff 01`, `81 03`) are
  deliberately not modelled. `dect_iwu_set_open_params()` exists so the bench
  can sweep them. Do not invent meanings for them.

### 4. The downlink-noise blocker did not reproduce

Controller audio work has been carrying a known cell-side defect: file and
codec downlink playback was loud noise while loopback was clean. **It did not
reproduce on 2026-10-22**, in two consecutive runs on a Gigaset A540:

```
downlink starvation: 0 of 8000 frames idle-filled (0.00%)   208/209 s, then 224/225 s
play cadence:        8000 frames, 4000 advances -> 8000 octets/s (correct for G.722)
```

with intelligible recorded speech at the handset. The second run had the raw
TDM capture and the 20 ms memory sweeps enabled and was indistinguishable from
the first, which kills the theory that the noise was our own instrumentation
load.

**This is "did not reproduce", not "fixed".** The largest uncontrolled
difference from the noisy runs of 2026-10-20 is the handset: those were a
FRITZ!Fon C4, these an A540. Treat the downlink as *probably* usable and tell
us immediately if you see otherwise.

### 5. CI can read this repo again

The submodule checkout now uses a **read-only SSH deploy key** on `openDECTcell/opendectcell`
(secret `OPENDECTCELL_READ_SSH_KEY`), not a PAT. A deploy key is scoped to one
repository, so it grants CI read on `opendectcell` and nothing else. Verified:
`internal/dect/cdect` builds and tests in CI. If the secret is absent the job
warns and skips that package rather than failing red.

### 6. Correction to §4, same evening — the downlink DOES fail, in the direction you care about

§4 above said the downlink-noise blocker "did not reproduce" and that the
downlink was probably usable. **That was written after four
handset-originated calls.** Two base-originated calls later the same evening
reproduced it immediately, on a FRITZ!Fon C4, with the same binary and the same
audio parameters:

| call direction | audio |
|---|---|
| handset-originated (A540 ×2, C4 ×1) | clean |
| **base-originated** (C4 ×2) | **wind noise** |

**This is the direction the controller cares most about** — an inbound SIP call
ringing a handset is a base-originated call. Treat downlink audio on
base-originated calls as **broken**, not usable, until this file says otherwise.
Handset-originated calls (a handset dialling out through SIP) are clean in every
run we have.

The cause is not the clock crossing §4 implied and not feeder starvation: the
transmit queue never underruns in either case. On a base-originated bearer the
chip holds lane words about twenty times as often (152/s against 7/s), so the
lane is not consuming at the rate we write. Details, measurements and the
remaining confounds are in `docs/call-g7.md`, "Bench session, 2026-10-22
evening".

Nothing here changes the controller's interface. It changes what you should
expect to hear while testing, and it means an end-to-end inbound-call demo will
sound broken for reasons that are ours, not yours.

## 2026-10-23

Three things here change what you build, and one of them contradicts a fact in
your own `docs/voip-requirements.md`. Read §3 before planning any media work.

### 1. Downlink audio on base-originated calls is SOLVED

The 2026-10-22 note above told you to treat it as broken. **It works**, and it
is reproducible for a whole call.

It needed two things we never had together: a second `0x0230` with flag bit 6
clear after the audio connect, which puts the chip in its narrowband mode; and
the right lane format. Proved by a controlled test — eight (chip mode × lane
format) windows in one call, each with its own chip-state reads. Three windows
had **identical** route words and differed only in lane format: one gave
silence, one gave noise, one gave speech with an envelope correlation of
**+0.922** against the source. Reproduced for a full 73-second call.

**So an inbound SIP call ringing a handset can carry audio.** The earlier
"expect it to sound broken" warning is withdrawn.

### 2. The lane format, and a correction you need to make

`docs/voip-requirements.md` §5 says, of the cell link: *"carries G.722 for
wideband calls and A-law for narrowband ones"*. **The narrowband half is
wrong.**

| direction | what the lane actually carries |
|---|---|
| **downlink (controller → handset)** | **signed 16-bit linear PCM, one sample per frame, 8000 Sa/s** |
| uplink (handset → controller) | **not yet measured in the working mode** — see below |

Narrowband calls are **G.726 on the air**, and the chip does that encoding
itself from the linear PCM we feed it. A-law was one of four hypotheses we
tested; it produced silence in one byte half and noise in the other.

The uplink in narrowband mode is genuinely open. Everything we have measured
about the uplink (G.722 code octets, two per 16-bit word, each word held two
frames) was taken in a **different chip mode on a wideband bearer**, so it
describes a path the narrowband call does not use. The leading candidate is
signed 16-bit linear, symmetric with the downlink; the fallback is A-law in the
low byte. We will measure it and update this file. **Do not hard-code either.**

### 3. Wideband may not be reachable at all on this chip — plan for it

This is the one that should change your design, so it is stated bluntly.

`voip-requirements.md` §5 says *"The cell delivers the coded G.722 stream
itself, so a G.722 SIP call needs no transcoding"*. **That assumption is now at
risk.** The clean room enumerated the chip's complete channel-configurator
branch table: six modes are reachable by a host mail and **the two true
two-slot wideband classes are reachable by no mail at all**. Wideband, if we
get it, has to come through one of two coded-block modes whose lane packing is
unproven, and the chip has **no G.722 encoder**, so the host must produce the
octets. We have a test rig built and not yet run. A clean negative is a real
possible outcome, and the clean room has committed to confirming it if so.

**What to do about it:** do not build the media engine on the assumption that
wideband is a passthrough. Treat the cell link's wideband capability as a
**runtime-negotiated property of the cell**, not a constant — so that a cell
that reports narrowband-only still works, and a G.722 SIP leg transcodes to
narrowband rather than failing. If wideband does land later, passthrough
becomes an optimisation you switch on, not an assumption you unwind.

### 4. A scope rule we have adopted, which you may want to mirror

We have written into `AGENTS.md` §1a: **the standard is the product, the chip
is the unknown.** GAP (EN 300 444) and CAT-iq (TS 102 527) are the goal; AVM's
proprietary behaviour is not. We reverse engineer the chip's mail interface
only, and read the specifications for everything that goes over the air.
Proprietary messages get the standard decline and nothing more, and
manufacturer-specific behaviour goes in a device-profile layer keyed on the
handset, never in the general library.

The equivalent for you: **where SIP and RTP are concerned, the RFCs are the
product.** Nothing on your side should depend on AVM behaviour, and
`docs/dect-features.md`'s per-model feature framework is the right home for
anything that does.

### 5. Smaller things worth knowing

- **Narrowband is correct, not a bug.** A narrowband call shows no HD symbol on
  the handset. That is the G.726 path working as designed, not a defect to
  report.
- **Codec list on the wire.** The narrowband codec list we advertise is
  `7c 04 90 02 00 84` — one codec, G.726 32 kbit/s, full slot. That matches
  stock byte for byte. The wideband list is longer and is the one in our
  registration path today.
- **An FP-originated call needs the handset to be pageable.** A handset holding
  a link does not answer a page, and the page is what makes the chip allocate a
  traffic bearer. We handle this on our side; it may show up as added latency
  before a handset rings on an inbound call, in the order of a second.

## 2026-10-23 (night)

### openDECTcell#67 is done. The integration freeze can lift.

`libopendectcell` now owns the C-plane stack glue. **API version 4.**

```c
#include "dect_link.h"

struct dect_link l;
dect_link_init(&l, handle, &mmcfg);

/* one received 0x0203 mail */
struct dect_link_ind ind;
dect_link_rx_segment(&l, &mail, now, rand_f, rs, &ind);

/* once per poll loop */
dect_link_tick(&l, now);

/* take everything queued and put it on your transport */
struct dect_mail out;
while (dect_link_poll_append_mail(&l, &out))
        send(&out);
```

**It is per connection, with no globals and no I/O.** `struct dect_link`
holds Lc, LAPC, MM and CC for one connection; make one per handset and they do
not interfere. That is the part you asked for: the single global connection in
`dect-stubd.c` was the reason the glue could not be shared.

**The change is purely additive.** No existing signature or struct layout
moved, so API 3 code still compiles — **but your C copy in
`internal/dect/cdect` should now go**, which is the point of the issue.
Replace it with calls to the above; the behaviour you documented in
`libopendectcell-integration.md` §1.1 is exactly what the library now does, and we
would rather you deleted your copy than kept it in step by hand.

**Two things stay yours, deliberately:**

- **Randomness.** `rx_segment()` takes RAND_F (8 bytes) and RS (16 bytes) as
  arguments rather than calling a generator. The library has no opinion about
  entropy and stays testable with fixed vectors. Pass NULL when you know no
  authentication is due.
- **Policy.** Everything observed comes back in `struct dect_link_ind` —
  `page_response`, `cc_message`, `mm_message`, the reassembled NWK message,
  the LAPC frame, the Lc return code. The link never decides what any of it
  *means*. Ringing, subscription storage and audio are yours.

`ind.nwk` is the reassembled NWK message and **may carry identities** — do not
log it anywhere that leaves the device. Our daemon prints it under `-v` only.

**The audio bind is still out of scope**, as agreed on the issue: it is chip
specific and currently wrong, and moving it would standardise the defect
across both consumers. `0x0233` (clean audio teardown) landed on our side
today and is not in the library either.

Six tests ship with it (`tests/test_link.c`), including one asserting two
links are independent and one asserting an unusable segment still lets your
own queue drain — the bug that once left a queued `{CC-SETUP}` sitting for
ever.

## API 5, and one thing you have to act on `[2026-10-23 night]`

`OPENDECTCELL_API_VERSION` is **5**. The bump is a struct layout change:
`struct dect_cc` grows `peer_basic_service`. Two functions arrive with it,
`dect_cc_peer_is_speech()` and `dect_nwk_find_fixed_ie()`.

### Not every call a handset places is a speech call

This is the part that needs your attention, and it is a defect we shipped and
have now fixed on our side.

A CAT-iq handset opens **list-access calls** to read the phonebook and the
call list: `{CC-SETUP}` with `<<BASIC-SERVICE>>` call class `0010`, answered
over `{CC-IWU-INFO}` carrying `<<IWU-TO-IWU>>`, with **no U-plane anywhere in
it**. In our capture corpus one handset placed **145 of those against 2 voice
calls**.

Our daemon bound the audio path to every one, because the bind triggered on CC
reaching connect without looking at what kind of call it was. If your media
engine arms a path, allocates an RTP or PCM resource, or reports "call
connected" on an incoming `{CC-SETUP}`, it has the same bug.

```c
if (cc->incoming || dect_cc_peer_is_speech(cc))
        /* ... arm audio ... */
```

`incoming` means *we* originated towards the PP; there is no peer
`<<BASIC-SERVICE>>` to read on those.

**Do not test the service nibble yourself.** That is the trap: a list-access
call carries call class `0010` with basic service `1000` "wideband speech", so
reading the low nibble alone calls a phonebook read a wideband voice call.
`dect_cc_peer_is_speech()` requires **both** nibbles — speech classes are
normal (`1000`), internal (`1001`), emergency (`1010`) and external handover
(`1100`); everything else is declined whatever its service nibble says.
EN 300 175-5 §7.7.5, figure 18.

The call itself is still answered either way, so **list access keeps working**
— you only decline to arm media.

### While you are in there: `<<BASIC-SERVICE>>` values

If you build a `{CC-SETUP}` anywhere, check the byte. We were sending `0x40`,
which that table makes **"message call setup"**. A normal narrowband voice
call is `0x80`; wideband is `0x88`. External handover is `0xC0`, not `0x80` —
an earlier clean-room answer had the class numbering wrong and we propagated
it for an afternoon.

### Nine tests ship with this

`tests/test_cc.c` covers the class/service matrix, a `{CC-SETUP}` with no
`<<BASIC-SERVICE>>` at all, and a planted `0xE0` inside a portable identity —
the last one because the obvious implementation scans for the byte, and
`dect_nwk_find_fixed_ie()` walks the element list instead.

## Diagnostics are now on both roadmaps `[2026-10-23]`

The maintainer asked for diagnostic features in the library, the LuCI app and
the controller. Design and field list: **`docs/diagnostics.md`** in this repo.
Your side is in your `ROADMAP.md` under C2 (transport), C5 (joining air
quality to RTP quality) and C7 (the cross-cell view).

**Why it became a feature rather than a debugging aid.** AVM's stock firmware
ships a **DECT Monitor** showing, per handset: identity, in range / connected,
model, field strength, signal quality, **long slot vs full slot**, the time
slot in use, the carrier, codecs offered and in use, and encryption state. We
spent a week inferring several of those through hypotheses that were withdrawn
twice. A shipping product prints them.

The user-facing argument is the same one: *"why does this handset drop in the
kitchen"* is the same question as *"why will the A540 not attach"*, and the
answer comes from the same place.

**The one that affects C5 directly.** Your RTCP statistics cover
controller↔PBX. They say nothing about the air. **A call that sounded bad
because the handset was at the edge of range is indistinguishable in RTCP from
one that lost packets** — and those have opposite fixes. Joining the two needs
the cell to send its DECT-side quality, which is the C2 transport item.

**Do not design the field semantics yourself.** `libopendectcell` will carry a
`struct dect_diag`, and the LuCI page and your admin view should mean the same
thing by "quality". The struct is not written yet; its protocol half needs no
new facts and its radio half is blocked on a clean-room answer (`Q-0054`),
so treat the field list in `docs/diagnostics.md` as the contract and the
struct as pending.

**Already real, and feeding it when it exists:** the chip subscription slot
audit (`sub: chip slot audit: 1=OURS 2=- ...`) and the UAK read-back, both
landed 2026-10-23. "Does the chip actually hold this handset's subscription"
is answerable today, per cell, and is exactly the sort of thing your alerting
item wants.

## API 6: list access is real, the IWU responder's interface changed `[2026-10-24 evening]`

**The ringer defect has a cause you should know about, because it shapes
how you will present incoming calls.** Every refusal of an FP-originated
call by a Gigaset in our corpus (60 of 60) coincided with the handset's own
CAT-iq list-access call, and the handset never finished its list sync
against us because our responder confirmed reads and never sent a data
packet. Full account in `call-g7.md` ("THE REFUSAL IS A {CC-SETUP}
COLLISION") and `spec-conformance.md` (the corpus table and the TS 102 527-3
audit).

**What changed in `libopendectcell`:**

- `dect_iwu` is now a TS 102 527-3 §7.4.10 list-access responder, not a
  byte-echo. `dect_iwu_reply()`, `dect_iwu_set_open_params()` and the
  `DECT_IWU_TAG`/`REQ_06`/`RESP_07`/`REQ_10`/`RESP_12`/`PUSH_14` names are
  **gone**. In their place: `dect_iwu_handle(iwu, in, len, &out)` fills a
  `struct dect_iwu_out` with 0..12 reply contents, each to go in its own
  `{CC-IWU-INFO}`; `dect_iwu_add_handset()`, `dect_iwu_set_line()`,
  `dect_iwu_set_own()`, `dect_iwu_reset_sessions()` carry the data. Lists
  served: 00 (supported lists), 06 (internal names), 08 (line settings).
  **The data is yours to supply:** the handset table and the line are
  exactly what the controller knows and the cell does not. For now the
  daemon installs its one subscription slot as terminal id `n`, name
  `DECT n`, and a line `1` named `Line 1`.
- `dect_cc`: a list-access service call (call class `0010`) is answered
  with `{CC-CALL-PROC}` only and never connected; it parks in
  `DECT_CC_ST_PROCEEDING` until the handset releases it. `dect_cc_busy()`
  is true meanwhile, `dect_cc_peer_is_speech()` is false.
- `dect_cc`: EN 300 175-5 §9.5.2.3 / §17.3.3. A peer `{CC-SETUP}` on another
  transaction while a call is in progress gets `{CC-RELEASE-COM}`
  "insufficient resources" (`DECT_CC_REASON_INSUFFICIENT_RESOURCES`); a
  0x32 release of our unanswered incoming call re-presents the identical
  SETUP at once, at most `DECT_CC_REPRESENT_MAX` (2) times. New stats:
  `collisions`, `represented`. `DECT_CC_TXQ` is 16 (a list read queues a
  confirm plus data packets).
- `struct dect_cc` grew (`setup_msg`, 256 bytes); anything embedding it by
  value is recompiled, nothing else.

**For your design, two consequences.** (1) The Internal Names List and the
Line Settings List are what a CAT-iq handset shows as "INT n" and the line
name; the controller owns both and will eventually feed them per cell —
plan a small table (terminal id, name) and (line id, name, attached
handsets) in the cell API. (2) TS 102 527-3 §7.4.10.6.3 says an incoming
voice call that arrives while the handset has a list-access session open is
presented *inside* that call as a pseudo call waiting (`{CC-CONNECT}` first,
then the call-waiting indication), not with a fresh `{CC-SETUP}`. We do not
do that yet; it only matters while a sync is running, which with real data
is seconds after attach rather than for ever.

## Incoming calls ring on a Gigaset: transaction value 0 `[2026-10-24 night]`

The Gigaset A540 now rings on an FP-originated call (`docs/call-g7.md`,
runs 14-16). The cause was ours: the daemon rotated the CC transaction value
per call, and EN 300 444 §6.9.2 requires the lowest free value. The A540
refuses value 1 with `0x32` and alerts on 0; the C4 accepts both, which is
why it never showed. No ABI change.

**What to act on when you drive `dect_cc` yourself:**

- Leave the transaction value alone. `dect_cc_init()` sets 0, and
  `dect_cc_set_next_ti()` is for tests only. One `struct dect_cc` runs one
  call, so 0 is always the lowest free value for a call we originate. A
  transaction left open by a lost call is cleared by releasing it, not by
  choosing another value.
- Send `<<SIGNAL>>` in every incoming `{CC-SETUP}` (`dect_cc_set_signal()`,
  `0x41` external, `0x40` internal). EN 300 444 §8.14 requires the FT to
  start alerting by one of two methods, and the library sends nothing by
  default yet.
- For a CAT-iq external incoming call, set `<<CALL-INFORMATION>>` with the
  line identifier, line type, a system-wide call identifier and call status
  "CS call setup" (`dect_cc_set_callinfo()`; TS 102 527-3 §7.4.5.3.2,
  §7.4.6.3, figure 50). The A540 rings without it, but it is mandatory, and
  the line and call ids are yours to own, like the list data.
- Expect the handset's chosen codec in `{CC-ALERTING}`: the A540 sent a
  one-entry `<<CODEC-LIST>>` with G.722 there.

## API 10: the review fixes, and three things you have to act on `[2026-10-24 night]`

`OPENDECTCELL_API_VERSION` is 10 (`dect_proto.h` has the full entry). Struct
layouts changed: `struct dect_mm`, `struct dect_lapc`, `struct
dect_link_ind`, `dect_mm_stats`, `dect_lapc_stats`. Recompile; nothing
else changes for code that does not touch the items below.

**Act on these if you drive the stack yourself:**

1. **Check who answered the page.** `ind.page_response` is now set only for
   an `{LCE-PAGE-RESPONSE}` that carries its identity, and the identity is
   in `ind.page_id`. Compare it with the IPUI you paged before sending the
   `{CC-SETUP}` (TS 102 527-1 §7.3.8.2.1); `dect-stubd` ignores a mismatch.
   The standard's reply to a mismatch, `{LCE-PAGE-REJECT}`, is not built yet.
2. **Arm `mm.port_id` on every link** from your subscription store, as
   `dect-stubd` does after `dect_link_init()`. A `{LOCATE-REQUEST}` is now
   rejected when its identity is missing ("information element error") or
   does not match ("IPUI unknown"), and with nothing armed every handset is
   unknown. During registration the identity is adopted only after the PIN
   is proven.
3. **`<<SIGNAL>>` is on by default** (0x41, external call). Call
   `dect_cc_set_signal(cc, 1, DECT_SIGNAL_ALERT_PATTERN0)` for an internal
   call. Without the element the A540 rings silently (run 18).

**Behaviour you get for free:** CC messages are matched to their
transaction (a release of the handset's list-access call no longer kills
your ringing call; unhandled messages on your call are ignored, not
closed); LAPC drops frames for SAPIs other than 0 and cuts an over-long
inbound message once; a TPUI acknowledgement with no assignment is ignored;
malformed 0x0203 segments are rejected. Counters: `mm.stats.tpui_unsolicited`,
`lapc.stats.rx_bad_sapi`, `lapc.stats.rx_truncated`.

**One daemon-side fix you need to copy** if you replace `dect-stubd`: a
chip release (0x0201) names a handle, and only the release of *your*
connection ends your link. The chip reports old connections' releases just
after a new connection comes up (102 times in the corpus).

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

## API 11-15: a base with several handsets `[2026-10-25 night]`

`OPENDECTCELL_API_VERSION` is 15. Five steps in one night, each with a full entry
in `dect_proto.h`; struct layouts changed in `dect_cc`, `dect_mm`,
`dect_link`, `dect_link_ind` and their stats. Recompile. All verified on the
three Gigasets unless marked.

**Act on these:**

1. **CC is gated like MM (API 11).** `dect_link` admits a handset's
   `{CC-SETUP}` only if its IPUI is `mm.port_id` (or a held handset, below);
   everyone else gets `{CC-RELEASE-COM}` "unknown identity" `0x0A`, and a
   link with no identity armed takes no call at all. If you were relying on
   un-armed links accepting calls, they no longer do.
2. **Hold every handset you serve (API 12).** `dect_mm_hold(mm, ipui, len,
   slot, tpui, tag)` after every `dect_link_init()`, once per subscribed
   handset. `{LOCATE-REQUEST}` and `{CC-SETUP}` from any of them are
   accepted, and the link adopts that handset: `mm.port_id`, the slot and
   the TPUI become its own, and `mm.adopted` is your tag. Read
   `mm.adopted` to know whose link it is; serve its Internal Names entry
   with the own-flag accordingly (`dect_iwu_set_own()`). Without this a
   base with three handsets turned two of them away twenty times a minute.
3. **Page with the target's TPUI, not the link's.** A link's MM holds
   whichever handset last used that link. `dect-stubd` paged the wrong
   handset for exactly that reason (runs 32-33); build the page TPUI from
   the target's slot (or pool TPUI) with `dect_mm_tpui()` on a fresh MM.
4. **Answer a foreign page response (API 13).** `dect_link_page_reject(l,
   ind.page_id, ind.page_id_len, ind.page_ti)`, then release the link
   (TS 102 527-1 §7.3.8.3.1). Seen once on hardware.
5. **Links without audio: `dect_cc_set_busy()` (API 15)** refuses every
   handset-placed call except list access with "insufficient resources".
   `dect-stubd` uses it for side links (a connection that arrives while
   the main link carries a call); a controller with one audio path per
   call wants the same for every link but the one it bound.

**Behaviour you get for free (API 14):** a handset-placed call whose SETUP
negotiates gets G.726 named in our `{CC-CONNECT}` (TS 102 527-1 §7.3.3);
read `cc.chosen_codec` before binding audio. Not yet seen on hardware.

**Defaults that changed in `dect-stubd`, worth mirroring:** no
<<CODEC-LIST>> in our `{CC-SETUP}` (basic speech means G.726; a list must
hold G.722 and G.726 if present); the first `0x0230` with flag bit 6 clear,
and no 500 ms re-assign, which removes the half second of noise at the start
of every call; every UAK in its own chip slot, clear only unused slots.

## API 16-17: diagnostics as data, and who may enrol `[2026-10-25 night]`

**API 16, `dect_diag.[ch]`.** One `struct dect_diag_link` per link:
`dect_diag_from_link()` fills the protocol half, `dect_diag_decode_status()`
decodes the chip's `0xF625` row (carrier, time slot, ten RSSI values, eight
integrity counters, frames since establishment), `dect_diag_frames_update()`
unwraps the 16-bit frame count, `dect_diag_json()` emits one object (IPUI
only on request). MM keeps the handset's <<MODEL-IDENTIFIER>> and
<<CODEC-LIST>> from its LOCATE or ACCESS-RIGHTS request. Use it for C5/C7
rather than decoding `0xF625` again; `dect-stubd` writes the same objects to
`/var/run/opendectcell/state.json` for LuCI.

**API 17, `dect_mm_cfg.registration`.** `DECT_MM_REG_OPEN` (default, as
before), `CLOSED`, or `SAME` (only the held `port_id` may re-enrol).
Refusals answer "IPUI not accepted". **Act on it:** set `CLOSED` on every
link that cannot persist a subscription, and outside an enrolment window.
Without it a handset enrolling on such a link shows "registered" with a key
the base never stored -- an independent review found exactly that in
`dect-stubd`'s side links.

## API 18-20: roaming between cells of one controller `[2026-10-25 morning]`

Results and method: `docs/multicell-experiments.md`. On STUB 07.56 the
chip admits, pages and rings a handset whose key it does not hold (M1, M2');
so a cell can serve a roamer from host records alone. What the controller
has to do differently:

**API 18, deferred location registration.** `dect_mm_cfg.defer_unknown`:
a `{LOCATE-REQUEST}` from an identity the link does not hold is held, not
rejected. `dect_mm_locate_pending()` gives you the identity; fetch the
record, `dect_mm_hold()` it, then `dect_mm_locate_resume()` (accept; or
reject "IPUI unknown" if you held nothing). The PT waits `<MM_locate.1>`
= 20 s, retransmits once, then aborts. Measured on the A540: accepted at +2 s and
+15 s; at +30 s the request dies with its link and the next one is
accepted. **Your real budget is the PT's `<CC.03>`, 20 s**: a Gigaset
opens list access ~2 s after locating, holds that call 20 s, then
releases it with "timer expiry" and takes the link (and the held LOCATE)
with it. Keep a fetch under ~15 s. Serve a held SETUP the moment its
record arrives.

**API 19, a held `{CC-SETUP}`.** Same idea in `dect_link`:
`dect_link_setup_pending()` / `dect_link_setup_resume()`. **This is the
one you need most.** A handset moving between cells of one location area
does **not** locate again (EN 300 175-5 §13.4; seen on the A540), so the
first thing a new cell sees is a call or list-access `{CC-SETUP}` from an
unknown IPUI. Without the hold, the CC identity gate turns it away
(`0x0A`) and a Gigaset retries every ~2.4 s for minutes. The alternative
is to push every record to every cell of the area up front; on 07.56 that
costs host memory only, not chip slots.

**API 20, `dect_mm_cfg.locate_validity`.** Non-zero adds <<DURATION>> to
`{LOCATE-ACCEPT}`: the PT locates again after N x 40.96 s (§13.4.1). For
tests now; possibly useful for presence later (a handset that stops
renewing is gone). Whether Gigasets honour it is test M6.

## API 21-25: PINs, wideband, dialling, and a call in steps `[2026-10-25 midday]`

All measured at the bench the same day (`docs/bench-2026-10-25.md`).

**API 21, PINs.** `dect_dsaa_pin_to_ac()` returns -1 for a non-digit or more
than 8 digits; it used to write in front of the buffer at 9+. Validate a
user's PIN with it. 6- and 8-digit PINs register on the AS690H. A wrong PIN
now gets `{ACCESS-RIGHTS-REJECT}` "authentication failed" (`0x10`).

**API 22, wideband.** `dect_cc_set_wideband(cc, 1)` lets our `{CC-CONNECT}`
choose G.722 when the handset offers it; `dect_cc_codec()` tells you the
call's codec, including the one the handset names in its ALERTING/CONNECT
on a call you place (ask for wideband with basic service `0x88` in the
SETUP). **Act on it:** bind the audio per call from `dect_cc_codec()`. On
the 7520 (07.56 STUB): G.722 means `0x0230` flags `0x41`, codec byte 0, no
mode-2 re-assign, and a G.722 lane you feed phase-locked to the chip's
two-frame words; anything else the narrowband bind as before. Clean on all
three Gigasets.

**API 23, overlap receiving.** `dect_cc_set_overlap(cc, 1)`: a handset's
speech SETUP gets `{CC-SETUP-ACK}`, its digits arrive in `{CC-INFO}`
(`dect_cc_keypad()`, `ind.keypad`), and **you** decide when the number is
complete. F<CC.01> releases the call 20 s after the last digit if you never
do. `DECT_CC_ST_OVERLAP` is new; `switch` statements over the state need it.

**API 24, a roamer's held SETUP is answered at once.** The link sends
SETUP-ACK (or CALL-PROC for list access) when it holds the SETUP for your
fetch, holds the handset's follow-ups (digits, list access) and replays
them on `dect_link_setup_resume()`. **The 20 s budget from API 19 is gone
for calls:** the A540 waited 17.5 s showing its digits and connected.

**API 25, a call in steps.** `dect_cc_proceed()` (CALL-PROC: number
complete), `dect_cc_alert()` (ALERTING: the far end rings), `dect_cc_connect()`
(CONNECT: it answered). Our CONNECT now enters ACTIVE (EN 300 175-5
§9.3.1.8); no CONNECT-ACK comes for a handset-placed call. The codec is
known from the SETUP, so you can bind the audio before CONNECT. Ringback
from ALERTING was heard on the A540. **Open:** the A540 shows no call timer
on calls it places, whatever we sent (12e); the C430A/AS690H are next, and
the room is asked what stock sends (Q-0060).

## API 26-27: the call timer, radio numbers, and 8-digit PINs `[2026-10-25 evening]`

**API 26, call status and FRITZ!OS's link numbers.** Layout change:
`struct dect_cc` gains `call_status` and `status_call_id`;
`struct dect_diag_status` gains `has_long`, `connect_frames` and `uptime`.
`dect_cc_set_call_status(cc, 1, call_id)` puts
<<CALL-INFORMATION>> "CS call connect" (`7e 06 01 00 81 02 01 85`) into our
`{CC-CONNECT}` to a handset's call. TS 102 527-3 §7.4.6.4.3 requires it, and
stock sends it (A-0122). It is the best candidate for the missing call
timer, but it is **not yet tested on air** (bench test 15).
`dect_diag_rssi_dbm10()` and `dect_diag_quality()` turn the chip's link
numbers into dBm and a quality percentage (A-0121). **Renamed:** the eight
event counters are now `acrc`, `xcrc`, `zcrc`, `qbit1`, `qbit2`, `handover`,
`slideerr` and `syncerr` (A-0123), and the JSON keys follow. The JSON radio
object gains `"dbm"` and `"quality"`.

**API 27, registration PIN length.** No layout change, but behaviour
changed. **Act on it before the libopendectcell integration thaws:**

- **The library has no default PIN any more.** An empty `dect_mm_cfg.pin`
  used to mean "0000", so an owner that forgot to set a PIN opened
  registration to anybody nearby. A PIN that is not 4 to 8 decimal digits
  now refuses every `{ACCESS-RIGHTS-REQUEST}` with "IPUI not accepted"
  (`0x06`), before any KEY-ALLOCATE. Always set the window's PIN.
- **New:** `dect_dsaa_pin_check(pin, min_digits)` returns 0 for
  `min_digits` to 8 decimal digits. The constants `DECT_PIN_DIGITS_MIN` (4),
  `DECT_PIN_DIGITS_DEFAULT` (8) and `DECT_PIN_DIGITS_MAX` (8) go with it.
- **The maintainer's decision:** the controller hands out **8-digit PINs by
  default**, with 6 or 4 as a fallback for a handset that will not take
  8. Today `registration_pin_digits` defaults to 6 and allows 4 to 8
  (`openapi.yaml`, `threat-model.md` B1 and §"Tokens and PINs", plus the
  open question in `threat-model.md` and the maintainers' notes). Make the default 8,
  restrict the choices to 8, 6 and 4, and close the open question as
  decided.
- **Evidence:** 6- and 8-digit PINs registered the AS690H on 2026-10-25
  (bench test 8). The A540 and C430A have only been registered with 4
  digits, so we do not know yet whether they take 8 (bench test 20).
  That is what the fallback is for.

Generating the PIN stays with the controller: use uniform random digits
(rejection sampling, not a modulus), as with RAND_F and RS.

## The cell side exists: cell mode, dect-cell, and a revised protocol `[2026-10-29 night]`

The maintainer confirmed the v1 split (you run the DECT stack, the cell
tunnels mails) and asked for the cell side. **Pull both repos.**

**Read first:** `docs/cell-protocol.md` in your repo, revised by us
(`09625f5`, `626dc56`, `b00279b`). The facts that changed under you:

- §7.4: the chip's subscription table **survives** daemon restarts and
  parks; op 0 of `0x0247` **clears** a slot [CR A-0065]; the key table is
  **readable** at a build-specific address; on 07.56 the chip admits a
  handset whose key it does not hold. So: rewrite the slots you want and
  clear the others after every radio-on, instead of assuming they are
  empty.
- §7.1: 06.88 (the 7412) sends `0x0203` with 7 octets and no handset tag.
- §7.5: three voice channels; `max_calls` default 2.
- §8.1-8.2: wideband works both ways as G.722 octets on the lane (format
  6), narrowband as L16 8 kHz (format 1). Pass G.722 through; transcode
  only when the ends share no codec.
- §6.4: the grace-period release is `0x0233` then `0x0202 {link}`.
- §13.1 P1-P4: four proposals for you to accept or refuse.
- §14: how we implement the cell.

**What runs** (openDECTcell `5688ef4`, `d174295`, `b2e1a22`, `bfe2f6e`):

- `dect-stubd -K`: no stack, no handsets; every chip mail up as MAIL_UP,
  every MAIL_DOWN to the chip (MAIL_REJECT on failure), FLOW, STATUS; the
  radio parked once the chip answers and only you turn it on; §6.4's
  release and park without you.
- `dect-cell` (package `opendectcell-cell`, mbedTLS): enrolment, mutual TLS
  1.3 with the pin check after every handshake, HELLO, heartbeats,
  back-off; relays frames to `dect-stubd` on a local socket in your frame
  format. HELLO announces capabilities MAIL | FLOW, formats [6, 1] both
  ways, `stub_version` "06.88" style. No media, resume, renewal or STATS
  yet; MEDIA_OPEN is answered with status 4.
- libopendectcell API 84: `dect_cell.h`, the frame/TLV/mail codec, if you want
  test vectors (`tests/test_cell.c`).
- `option role 'cell'` / `/etc/init.d/opendectcell enroll '<string>'`: the box
  runs nothing of the base, no baresip.

**Tested only against a stand-in** (`tools/cell-testpeer.py`, a Python
peer with its own CA), not against your Go controller: Go is not
installed on this machine. On the 7412: enrolled, HELLO, 34 s of
heartbeats, a memory read down and its answer up, radio-off applied,
link loss released after 7 s, parks read back. **The first thing worth
doing on your side** is an interop run of `internal/cell` against a real
`dect-cell` (or against `tools/cell-testpeer.py` the other way round).

**Interop with the Go controller, 2026-10-30** (controller af4deec in
Docker, `docker compose` with `OPENDECTCELL_CELLS_PUBLIC_ADDRESS`; the 7412 on
the openDECTcell 424017b packages). Every pre-written outcome passed:

- enrolment from an auto-approving token made in the web UI (the `%3A` in
  `h=` is decoded);
- mTLS and HELLO; the cell page shows "openDECTcell 0.3.1 · STUB 06.88", round
  trip 1-2 ms;
- the link stayed up with no reconnects;
- 34 mails went up and the controller counted and dropped them (C3 not
  built); none came down, so the radio stayed parked;
- state disabled, then active, from the cell page: the cell parked on
  disabled;
- a controller restart: the cell reconnected by itself (4 attempts during
  the restart);
- teardown: credentials forgotten, park read back.

Not covered: the mail tunnel (C3 on your side) and media.

## 2026-10-30 (evening): STATS from the cell; the stack moving into the library

- **STATS** (§10): dect-cell now announces CAP_STATS, answers
  STATS_REQUEST and sends STATS every 60 s (`stats_interval_s` from
  CONFIG_SET is not applied yet). Counters from dect-stubd: 1, 2, 3, 13,
  15-19; from dect-cell: 6 (reconnects). Without dect-stubd the cell
  answers with 6 alone. Not yet run on a box: bench N6.
- **The base stack moves into libopendectcell** (maintainer's choice, your ADR
  0002 option B): `docs/stack-in-library.md`. S1-S2 are done (API 99):
  `struct dect_base` holds the handset records, the chip's slot writes,
  the links' set-up and segment entry, adoption and the registration's
  completion, with host callbacks (send, random, log, save, install).
  Calls (S3) are next; that is when C3 can host `dect_base` over the
  mail tunnel.
- `dect_mm_cfg.assign_id` (API 99): an identity to assign in
  {ACCESS-RIGHTS-ACCEPT}, the IPUI O a class B system needs.
- **Resume** (§6.4): dect-cell announces CAP_RESUME, keeps the session
  token and grace from HELLO_ACK (tags 4, 8), offers the token in HELLO
  (tag 13) while the grace lasts, and on `resumed = 1` sends
  STATE_SNAPSHOT from dect-stubd: tag 1 chip epoch, tag 2 radio (0/1),
  tag 3 per live connection {handle, tag[3]} (the tag all zero on 06.88),
  no tag 4 yet (no media). Your side accepts the snapshot and does not
  reconcile yet (C3). Bench N7.


## API 100-137: the base stack is in the library; C3 can host it `[2026-10-30 night]`

The move your ADR 0002 option B asked for is done. `dect-stubd` is now a
host of `struct dect_base` (`src/libopendectcell/dect_base.h`) and keeps only
the transport, its sockets and files, audio lanes and diagnostics.

- **Read first:** [hosting-dect-base.md](hosting-dect-base.md), written
  for you: what a second host provides, the chip mails and where each
  goes, the 23 callbacks with what `dect-stubd` does and what you would
  do, the per-cell chip settings, testing, limits.
- **What moved** (record: [stack-in-library.md](stack-in-library.md);
  every public change: [api-versions.md](api-versions.md) 100-137):
  calls (ring, call out, dialling, internal calls, side rings, parallel
  calls, call waiting), the bridge's verbs, lists and notices (names,
  lines, MWI, missed calls, date and time), deregistration, link set-up,
  the connection and segment mails, one tick (`dect_base_tick()`), and
  the key read-back and slot audit (API 137).
- **Act on:** `cdect.h` checks `OPENDECTCELL_API_VERSION != 99`; a submodule
  bump needs it raised. The `lc`/`lapc` functions cdect uses did not
  change shape.
- **Act on, per cell:** `chip.uak_table` from the cell's `stub_version`
  (06.88 `0x067F`, 08.25 `0x9518`, 07.56 none known: 0) and
  `chip.check_delay_ms` 1000 on 06.88. With 0 the library skips the
  read-back and the audit rather than read the wrong address.
- **Keys:** `save()` hands you the record with its UAK; your store
  encrypts it. `random()` must be a CSPRNG.
- **The behaviour is pinned by fifteen golden traces**
  (`tests/stubd-traces/`, made-up identities): a host that replays them
  and gets the same mails and bridge events is the same base. The
  cheapest first test of a cgo host.
- **On hardware since the move** (`docs/bench-later.md` §1a): the 7412
  (06.88) started, restored, read back and audited its slots, served
  STATS and resumed a link to your controller in Docker (U1-U6); the lab
  7520 (07.56) runs the new build with three Gigasets (V1, V3; V2, a
  handset re-locating, waits for the bench).
- **Open for C3, not decided here:** who sends the chip's audio mails
  (0x0251, 0x0230, 0x022D) over the tunnel: the controller through
  `call_up()`, or the cell on MEDIA_OPEN (cell-protocol.md §8). The
  library works either way.

## C3: go, and who binds the audio `[2026-10-30, late night]`

- **The maintainer gave C3 the go** on 2026-10-30.
- **The controller sends the chip's audio mails**, the open point of the
  entry above: 0x0251, 0x0230, then 0x022D on the long-form 0x0242, and
  0x0233 with the 0x022D disconnect at the end, as MAIL_DOWN from
  `call_up()`/`call_down()`. MEDIA_OPEN follows the bind and names the
  channel. The maintainer's condition: over the tunnel unless it proves
  too time-critical, then the cell. Nothing measured says it is (no air
  timer on the bind; one exchange since the 10-25 defaults). Recorded in
  the controller maintainers' notes and cell-protocol.md §8.1 (`09a0d07`).
- **For the cell side:** `dect-stubd -K` already relays every MAIL_DOWN,
  so nothing changes there. What the first call with audio still needs is
  the cell's media (MEDIA_OPEN answers status 4 today).

## The cell's link, after a security review `[2026-10-30, late night]`

An independent review of `dect-cell` and `dect-stubd -K` found nine
points; seven are fixed. What the controller sees differently:

- **MAIL_DOWN, MEDIA_OPEN and MEDIA_CLOSE while the cell is not active**
  get `ERROR NOT_ALLOWED` (§5.3), from dect-cell and, as a second lock,
  from dect-stubd. Before, they went to the chip in any state: a disabled
  cell's radio came back on with a `0x015B {01}`.
- **A malformed MAIL_DOWN** gets `ERROR MALFORMED` (§3.4), no longer
  MAIL_REJECT CHIP_REFUSED.
- **A MAIL_DOWN that dect-stubd cannot take** (its socket full) gets
  MAIL_REJECT QUEUE_FULL when it has a corr; it was dropped silently.
- **CELL_STATE goes to dect-stubd as the state alone.** The controller
  could put the local reason `link-lost` in tag 2, and dect-stubd then
  released the links and parked the radio with the link up.
- **A GOODBYE no longer crashes dect-cell.** Any GOODBYE, or a failed
  write, inside the frame loop led to a memmove of about 2^64 octets; the
  cell died and procd gave up after five respawns in an hour.
- **A TLS record bigger than the receive buffer** (8 kB) took the link
  down. A controller that coalesces many mails into one record now gets
  them all.
- **Connect and handshake together take at most 5 s**, and SIGTERM ends
  them at once. Before, an unanswered SYN blocked for two minutes and a
  peer that dribbled the handshake held it for ever.

Open, for later: the enrolment string reaches dect-cell on its command
line, so it is visible in `/proc` for up to a minute (needs another way
from LuCI to the init script). §9's P0 bound (close when a frame waits
over 500 ms) and `TCP_NOTSENT_LOWAT` are not built; writes block for up
to 5 s. `make check-cell MBEDTLS=...` runs dect-cell with the sanitizers
against a scripted controller (`tests/test_cell_link.py`).

## The cell's media is built `[2026-10-30, late night]`

MEDIA_OPEN no longer answers status 4. What the cell does (cell-protocol.md
§8, §14.2):

- **HELLO announces MEDIA and SRTP** (and PLAIN_RTP only where the admin
  set `opendectcell.main.cell_plain_rtp '1'`, which runs `dect-cell -P`). The
  cell takes MEDIA_OPEN only if HELLO_ACK tag 10 has MEDIA, and only while
  the cell is active.
- **The lane comes from the chip's answer to your bind.** dect-stubd keeps
  the last long `0x0242 {h, chan_flags, mask0, mask1}` per live handle.
  MEDIA_OPEN's `audio_channel` must equal that `chan_flags` low nibble
  (1, 2 or 4 for channels 1, 2, 3 as stock binds them); the mask bit
  gives the lane, flag bit 6 whether the lane carries G.722. So send
  MEDIA_OPEN after the long 0x0242, as `call_up()` does. No such 0x0242 on
  a live handle: status 1. A handle's 0x0201 (connection down) closes its
  session with MEDIA_CLOSED reason 1, unasked.
- **One format both ways for now**: tag 3 is 6 (G.722) or 1 (L16 8 kHz);
  tag 13, if present, must be the same, else status 2. The lane's own
  format passes as it is; the other is converted on the box (G.722 coded
  or decoded), so pick the lane's format when the far end allows it.
  `ptime` 10 or 20; `payload_type` 0-127; `media_id` any u32, 0 too.
- **SRTP**: profile 1 (AES_CM_128_HMAC_SHA1_80, 30-octet key || salt) and
  2 (AEAD_AES_128_GCM, 28 octets), RFC 3711 KDF at rate 0, no MKI; the
  receiver keeps a 64-packet replay window. Checked against the RFC 3711
  B.2/B.3 and RFC 7714 16.1.1 vectors and, end to end, against an SRTP
  written apart from it in Python (`make check-cell`). Profile 0 without
  the admin's setting: status 4.
- **The UDP socket is connected to `ctl_addr`**: the kernel drops packets
  from anywhere else; the cell also drops a wrong SSRC or payload type, a
  failed tag, a replay, and a payload not exactly one ptime long. Your
  packets: one ptime of audio each, in order (two 10 ms halves make one
  20 ms frame; a gap drops the first half).
- **Packets up** start at a random sequence number and timestamp, the
  marker set on the first; the timestamp counts 8000 per second for both
  formats (G.722 as RFC 3551 says). They flow while the lane carries live
  words, which it does through a call; a silent or idle lane sends none
  (§8.3's "every ptime, silence too" is not met there yet).
- **MEDIA_CLOSE** is answered by dect-stubd's MEDIA_CLOSED (reason 0),
  also for a session already gone. dect-stubd's restart closes every
  session with reason 2. A lost control link leaves the flows running
  (§6.4).
- Not on air yet: the first call with audio is C3's. Bench card M1 in
  `docs/bench-later.md`.

## The cell renews its certificate `[2026-10-30, late night]`

§5.4 is built in dect-cell:

- At two thirds of the certificate's life (the dates in the certificate,
  so day 60 of 90), on a live link in any state, the cell makes a new
  P-256 key and sends `RENEW_REQUEST {1: csr}` with corr `0x5244`. No
  answer within 60 s, a status other than 0, or a certificate that does
  not fit: again in an hour.
- **What it checks in `RENEW_RESPONSE`:** the certificate verifies against
  the CA it was enrolled with (`ca.der`; TRUST_UPDATE is not built), has
  clientAuth, the same `urn:opendectcell:cell:` URI as the old one, and
  certifies the new key. Otherwise the old pair stays.
- The new pair replaces the old on disk (the certificate first, then the
  key; a start after a cut short finishes it) and serves **from the next
  connection**. The current link carries on with the old certificate, so
  keep both fingerprints current until the cell connects with the new
  one, as §5.4 says.
- A clock before the certificate's start counts as unset (no RTC):
  nothing is renewed, and an expired-looking certificate is not judged.
  With a set clock past `not_after`, the cell stops before it dials
  (state `stopped: certificate expired, enrol again`).
- `cell-link.json` has `cert_expires` (Unix seconds); LuCI shows it.

## The protocol is complete; the handsets in the controller `[2026-10-31]`

- **cell-protocol.md is v1, complete for C3** (controller `15af9a2`). Its
  new §15 is the table of what the cell does today against the spec. The
  rows that are *not built* here, so you know what not to depend on:
  CONFIG_SET (answered status 1, nothing applied), TRUST_UPDATE (do not
  rotate the cell CA; re-enrol instead), STATUS tags 6 and 7, silence
  packets on an idle lane, and §9's P0 bound and `TCP_NOTSENT_LOWAT`. The
  controller accepted P1-P4; log shipping, update and debug are not in v1.
- **API 138: the Internal Names List per viewer** (list-access.md §1.1 in
  the controller, the maintainer's rule of 2026-10-30: in the controller a
  handset sees only the handsets in its user's account and cannot rename
  any; a standalone box is unchanged).
  - `struct dect_base_ops.name_visible(ctx, viewer, slot)`: 1 if the
    handset in slot `viewer` may see the entry of slot `slot`. NULL = all
    (today). A handset always sees itself. Set it from the user's account.
  - `cfg.names_readonly = 1`: the Name field's editable bit is clear and an
    edit or save is answered "procedure not allowed" (TS 102 527-3
    7.4.10.4). Names are then the controller's.
  - After an account change (a handset moves to another user) call
    `dect_base_names_refresh()`: every link's list gains or loses the
    entries and the handsets are told.
  - Built and unit-tested (`tests/test_base.c`, `tests/test_iwu.c`). **Not
    on air**: no Gigaset has shown a read-only Name field yet. If one
    mishandles it, the fallback is `names_readonly` off and the controller
    discarding a handset's rename (`named()` fires; ignore it and call
    `dect_base_names_refresh()`).
  - The internal call's dial list is this list, so a handset can only call
    handsets in its account. Assumed to be what the maintainer wants; not
    confirmed.
- **The enrolment string is no longer on a command line** when LuCI
  enrols a cell: `dect-cell enroll @FILE` and `enroll @-` read it from a
  root-only file or standard input, LuCI writes the file (mode 0600,
  removed by the init script afterwards) and passes only its name. The
  one-time token was readable in `/proc` for up to a minute before. An
  older init or a CLI user passing the string itself still works. Nothing
  changes on the wire.

## Several registration PINs at once, API 139 `[2026-10-31, evening]`

For the registration flow in the controller maintainers' notes (one PIN per
user's window, registration open on many cells at once).

- **What a handset does:** it connects to a base first, and only then asks
  its user for the PIN; the base checks the handset's reply (RES1) against
  the key derived from a PIN. So the PIN is not needed to *find* the base,
  and the base can try several PINs against one reply. Which one matched
  tells you the user.
- **The API (139):** `dect_base_cfg.reg_pins` (an array of decimal strings,
  4 to 8 digits, owned by you) and `n_reg_pins` (at most 32,
  `DECT_MM_REG_PINS_MAX`). A handset that proves one of them registers; the
  record `save()` sees has **`reg_pin1` = 1 + the index** of the PIN it
  proved (0 for a stored or loaded record). With `n_reg_pins` 0 the single
  `cfg.pin` is the only candidate, as before. `cfg.pin` stays the PIN of the
  list access service (deleting a handset from a handset's list); keep it
  apart from the registration PINs.
- **Cost:** a few DSAA computations per handset attempt (every candidate is
  computed so the time does not leak which matched); nothing on the air. A
  PIN list change takes effect for the next link, not one already in its
  registration.
- **Equal PINs:** the first wins; the controller must not issue two live
  PINs that are equal.
- **Correction of an earlier note:** `dect_base_cfg.tpui_offset` (openDECTcell
  `148daa7`, the TPUI test knob) changed a public struct and should have
  bumped the API; it is part of 139 now. The per-record TPUI for your pool
  will be API 140, after the maintainer's T-C test.
- **The pin of the submodule** stays where you want it; at API 139 your
  `cdect.h` check needs `139`.

## API 140: a registration is always saved; the lowest slot; the read-back asked twice `[2026-10-31, evening]`

Three fixes found at the bench on 2026-10-31, one struct change, hence 140
(openDECTcell `docs/api-versions.md`):

- **A handset that registers and does not locate on that link is now
  persisted anyway**, 2 s after the key allocation (`DECT_BASE_LOCATE_GRACE_MS`).
  Before, the `save()` callback fired only after the handset's location
  registration on the same link, so a handset re-registering over its own
  record (it counts itself located) kept its new key while the base kept the
  old one. The call is `dect_base_registration_tick(b, now)`, made by
  `dect_base_tick()`; a host that calls its own ticks (as `dect-stubd` does)
  must call it too. Your `host.c` copy of the glue needs the same call.
- **A new handset takes the free record with the lowest slot**
  (`dect_base_free_record()`), not the first in list order. If your stack
  chooses the record itself, nothing changes for you.
- **The key read-back is asked once more after 500 ms** when nothing answers,
  and `dect-stubd` waits 50 ms before the first read on every build. Your 50 ms
  (§0j) stays right; the library now does the repeat itself.

Bench results of the same day (T-B, T-C, registration during a call) are in
the maintainers' notes and §0m of your repo. **The pin of the submodule** needs
`140` in `cdect.h` when you take it.

## API 141: the TPUI per record `[2026-10-31, evening]`

`dect_hs_rec.tpui[3]` (openDECTcell `docs/api-versions.md` #141) is the pool's
hook. Set it on a record before the handset locates (a free record an
enrolment is going to take too) and the library uses it in the location
accept and in every page and link for that record; all zero keeps the old
slot-derived value. A registration fills it with what it assigned, so the
record `save()` hands you carries the TPUI the handset holds: **keep it and
give it back unchanged when you load the record again**. A handset takes a
new TPUI only at its next location registration, so never change one that is
in use (a page by a TPUI it does not hold is silent). Bench-tested on air
for the form `{00, 11, N}` only (N = slot + 0x40, 06.88, two handsets);
another high byte is untested. `cdect.h` needs `141` when you take it.

**From the chip room (A-0133, 08.25 STUB, static reading):** the chip has a
shortcut for TPUIs `11 nn` with nn from 1 to 6 (treated as slot nn); values
above 6 take the normal path. Keep pool values at N of 7 or more, as the
bench did, until it is shown otherwise. That is also why your pool must not
hand out `11 01`..`11 06`.

**Superseded 2026-11-02 (API 150): which TPUIs a 06.88 chip hears.** The 7412's chip finds
a handset's own access (a call, the answer to a page) by the low nibble of the TPUI's last
byte N and hears only 1 to 6; it needs no chip slot and compares no identity. `dect_tpui_hearable()`
says it. A pool for a controller with 7412 cells is therefore `{00, 11, N}` with N from 0x10 and a
low nibble of 1 to 6 (90 values; `11 01`..`11 06` stay the chip's own). The bench saw 11, 16, 25,
35, f1 answered and 3d, 0d, 17, 19 never. The 7520's chip hears any TPUI. The pool must be unique
among the handsets in one cell's table, not across the host: a handset that locates on a cell
where another holds its TPUI is given a new one in the location accept (it takes it at once). Two
handsets whose TPUIs share the low nibble (a slot class) are both heard but share one lane on
the 06.88 chip: only one can hold a link at a time, and a page response that nobody waits for is
rejected and its link released (API 150 build). The default TPUI of a record the host sets none
for is now hearable for every record number.

## API 142: a callback for a rejected registration attempt (your #83) `[2026-10-31, evening]`

`dect_base_ops.registration_rejected(ctx, pin, expected)` (optional; openDECTcell
`docs/api-versions.md` #142): once per rejected enrolment, `pin` = 1 when the
handset's reply matched no PIN, `expected` = 1 when its profile says it tries
PIN 0000 first. Count `pin && !expected` for the per-cell limit; the log line
can stay as your fallback and its wording is unchanged. On the read-back
repeat in the same issue: done in the library at API 140 (asked again once
after 500 ms; the daemon waits 50 ms on every build). `cdect.h` needs `142`
when you take it; 140 to 142 together are in the three notes above this one.


## API 143: the PARK of a system, and the cell's identity `[2026-11-01]`

**The PARK leaves the RPN out; class B has its own PLI and LAL** (openDECTcell
`docs/api-versions.md` #143). `dect_base_cfg.park_pli` (0: the class's whole ARI,
36 or 31) is what the controller sets to 19 for class B (the PLI setting: ARC and
EIC, so one subscription covers every site of the EIC). The RFPI the library
builds the PARK from is the one the chip reports (`0xF601`), so a cell given an
identity needs nothing more in the stack; every cell of one system then sends
identical PARK bytes whatever its RPN.

**The cell side of the identity** (no API change): the cell reports the RFPI its
chip broadcasts in HELLO (tag 17) and STATUS (tag 6); `CONFIG_SET` tag 6 sets one.
The chip has no runtime mail for it (room A-0131), so the cell stores it in
`/etc/opendectcell/cell/rfpi`, which the init script uploads the chip with, and restarts
the service once per value asked for; a chip that still reports another RFPI after
that reload is not reloaded again, and the CONFIG_ACK says status 1. Leaving cell
mode erases the file and reloads the chip with the factory RFPI. `cdect.h` needs
`143` when you take it.

## API 144: sixteen records per base `[2026-11-01]`

The library's record capacity is 16 (`DECT_HS_MAX`), no longer the chip's six slots
(openDECTcell `docs/api-versions.md` #144). A record's slot number is its record number;
only 1 to 6 are chip slots. For the controller: give a handset on every cell a record
numbered 7 or more and a TPUI of its own (the form `{00, 11, N}` with N from 7), and the
chip needs no key for it; `install()` (host.c) writes only records numbered 1 to 6 and
the dummy key keeps the table non-empty when there is none. Limits that remain: the
names list holds eight handsets and the call lists seven terminal ids, so a record
numbered 8 or more shows in neither. `cdect.h` needs `144` when you take it.

## API 145: thirty-two records per base `[2026-11-01]`

`DECT_HS_MAX` is 32 (openDECTcell `docs/api-versions.md` #145): record numbers 1 to 32, the
chip's slots still 1 to 6. A stack is 63 kB instead of 55 kB. Not a bit per record number
in an `unsigned`: 32 does not fit. `cdect.h` needs `145` when you take it.

## API 146: dropping a record, and the pool TPUI fix `[2026-11-01]`

`dect_base_record_drop()` takes a record out of a quiet base (openDECTcell `docs/api-versions.md`
#146); the controller uses it to expire roamers from a cell's table. If you kept per-record
TPUIs (API 141) and saw `{00, 11, slot}` in LOCATE-ACCEPT anyway, that was a bug here and is
fixed. `cdect.h` needs `146` when you take it.

## API 147: names and call lists for records 8 to 32 `[2026-11-01]`

The terminal identity number is the record number, so every record now has its
Internal Names entry (`DECT n`, and a name the handset saves) and a call list of its
own. `DECT_IWU_HANDSETS` 32, `DECT_CALLS_TERMINALS` 33. A host that kept records 8 and up
out of the lists as a workaround can stop. A read of the names list still delivers what
fits one reply, the entries asked for up to fifteen data packets, and the handset asks
for the rest. The call-list text (`dect_calls_save()`) writes 66 hex digits per entry for
the lines; the older 16 digits still load. Not tried on air: the names list with more
than eight entries on a real handset (the AS690H, A540 and C430A show it differently).

## API 148: recycling a chip-slot record `[2026-11-01]`

`dect_base_record_recycle()` (openDECTcell `docs/api-versions.md` #148): after a pooled handset enrolled
into a chip slot, the controller frees the slot in place instead of rebuilding the stack, so an
enrolment never ends a call. Your `save()` must set `loaded[cur] = 1` and `absent[cur] = 0` the way
`dect-stubd`'s does; `cdect.h` needs `148` when you take it. Not tried on air yet: the clear of a slot on a
running radio is the deregistration's own mail pair, the rest is host bookkeeping.

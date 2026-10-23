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

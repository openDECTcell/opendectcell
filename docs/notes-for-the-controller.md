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

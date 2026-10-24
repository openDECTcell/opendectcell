# Why the Gigasets never rang

Answered 2026-10-24 by diffing our own logs against the stock capture
(`freetz-ng/captures/2026-10-24-stockcap/`). This supersedes every earlier
theory in this repo about FP-originated calls.

## The short version

**We never configure the chip.** Stock's entire MAC bring-up is five mails.
We send one of them, and we send it with one parameter byte where stock sends
seven.

| mail | stock | ours |
| --- | --- | --- |
| `0x0153` ECO_CONFIG | `00` | **never sent** |
| `0x0169` FEATURE_FLAGS | `07` | **never sent** |
| `0x0157` CAPABILITY_PROFILE | `00` | **never sent** |
| `0xF600` contact | sent | sent |
| `0x015b` RADIO_ON | `01 00 00 00 00 00 00` | `01` — see correction below |

`0x0153`, `0x0157` and `0x0169` appear **zero times in our entire source
tree.** Stock sends them from `src = 0x14`; we use `src = 0x22`.

> **Correction to the first version of this page.** I led with the `0x015b`
> byte-count difference as a candidate. `[CR A-0108]` decoded the handler
> from the firmware: the radio-on path reads **only `param[0]`**, so our
> single `01` is equivalent to stock's `01 00 00 00 00 00 00` and the six
> extra bytes are reserved. **That difference is not the bug.** It was the
> most visually striking line in the diff and the least important.

## The mechanism, from the firmware `[CR A-0108]`

`0x0157` and `0x0169` write the chip's **beacon capability region**,
`0x228c`–`0x2295` — the FP capability field the chip broadcasts.

- **`0x0157` CAPABILITY_PROFILE** is *how stock tells the chip what to
  advertise*: it sets capability bits across `0x228c/228e/228f/2290/2292/2293`
  plus `config[0]`, `config[1]`, `config[0xa]` and `0x2451`.
- **`0x0169 = 07`**: bit 0 → `config[+0xa] |= 0x05`; bit 1 → `0x95ea` bit 7;
  **bit 2 → `0x2295` bit 0**, inside that same capability block.
- **`0x0153` ECO_CONFIG**: low 7 bits set the eco level at `0x9b89`, bit 7
  arms an eco timer. Our equivalent of `00` is "eco level, no timer".

**We never send either of the two that matter, so the chip beacons its
power-on defaults.** That is the defect in one sentence.

It also predicts the two values we measured: the `0x0243` byte is built from
the `0x2293`/`0x2295` region that `0x0157`+`0x0169` write, and `0x0157` also
pokes `config[0xa]`/`0x2451`, which gate table setup — hence the empty
`0x96ad`.

## The measured consequence

Two chip-reported values differ, persistently, in every run either side has:

```
0x0243 middle byte     stock: 2b     ours: 29
0x96ad                 stock: populated at idle     ours: all zeros
```

`COMMAND_SET §8.2` documents that `0x0243` byte as the constant `0x29`. It is
not constant. And nothing we have ever varied moved it — not the page info
byte, not `<<BASIC-SERVICE>>`, not the page header, not `0x015E`.

We also **never receive `0x0205` or `0x0209`**, not once, in any run. Stock's
log 23 has `0x0205` 114 times and `0x0209` three times in a single call.

## Why this explains what nothing else did

Three things had no explanation until now, and this accounts for all three:

1. **Nineteen per-call variables gave byte-identical results.** Because the
   cause is *one-time init*, not anything in a call.
2. **`0x96ad` read zero while paging demonstrably worked.** Because the cause
   is *upstream of paging*.
3. **A message satisfying EN 300 444 table 21 exactly was still refused.**
   Because the cause is *below the air protocol*.

## What was NOT the cause, with evidence

Recorded because each cost real time:

- **`0x015E` per call.** Stock never sends it at all. We never sent it either
  — the `0300 → 015e ×2 → 0300` in `mac-bringup.md` was a *reconstruction of
  stock* from A-0006, superseded by A-0036 and never implemented. A-0095/96's
  bearer-arm model was falsified on hardware before the capture existed.
- **Message content.** The exact GAP table 21 message, with and without
  `<<CODEC-LIST>>`, with `<<SIGNAL>>`, on narrowband and wideband
  connections, with the handset's own preferred codec list: all refused.
- **`0x0230` audio assign.** Stock's is `03 41 02 00 00 00` — byte-identical
  to our bind, including the `01`-then-`41` two-step we reverse-engineered
  independently for G7. Pickup-time, never the ring problem.
- **The handset.** A540, C430A and AS690H all refuse on a clean instrument.
- **Timing.** Setup-to-page-response gap swept 120 ms – 1 720 ms.

## The test

Replay the four config mails before the radio comes up, then ring:

```
-M 0x14:0x02:0x0153:00
-M 0x14:0x02:0x0169:07
-M 0x14:0x02:0x0157:00
```

**Two constraints from `[CR A-0108]`, both of which can invalidate the test:**

1. **The config must land before `0x015b` radio-on.** The radio-on path
   latches state; configuring afterwards may not re-beacon. Stock's order is
   `0153 → 0169 → 0157 → F600 → 015b`. Our daemon currently sends `0x015b`
   at `t=28 ms`, so `-M` mails arriving at ~100 ms per tick are **too late**
   — this needs a code change, not just `-M`.
2. **Use `src = 0x14`.** The handler does not check it, but dispatcher
   routing may only accept config from the FP-management task.

Because the beacon capability field is what a handset reads when deciding
whether to register, the config must also be in place **before
registration**, not just before the ring. All handsets were factory-reset for
the stock capture and the stores are gone, so re-registration is part of the
test either way.

Pre-committed outcomes:

| outcome | meaning |
| --- | --- |
| it rings | done |
| `0x0243` turns `2b` and/or `0x96ad` stops reading zero, still no ring | the bring-up gap is real and there is a second cause above it |
| neither value moves | the cause is lower still, and this is not it |

## Open

What the six missing `0x015b` parameter bytes mean, and what bits 0..2 of
`0x0169 = 07` enable. Asked of the clean room. Replaying the bytes is enough
to test the hypothesis; understanding them is what turns it into a stack.

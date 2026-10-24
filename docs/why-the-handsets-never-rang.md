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
| `0x015b` RADIO_ON | `01 00 00 00 00 00 00` | **`01`** — one byte |

`0x0153`, `0x0157` and `0x0169` appear **zero times in our entire source
tree.** Stock sends them from `src = 0x14`; we use `src = 0x22`.

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
-M 0x14:0x02:0x015b:01000000000000
```

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

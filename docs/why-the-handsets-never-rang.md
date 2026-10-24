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


## 2026-10-24, later: the beacon mechanism is withdrawn, the localisation is stronger

`[CR A-0108]` was decoded against **08.25** firmware while this box runs
**07.56**, where the structures are at entirely different addresses
(dispatcher `a stock routine` / config base `0x2287` versus `a stock routine` /
config base `0xc06`). Withdrawn: the beacon mechanism, the addresses, and the
`0x0243`/`0x2451` byte-change predictions. The contradiction that caught it
was ours: our QH=4 and QH=C already matched stock byte-for-byte, which a
"we never configure the beacon" story cannot survive.

**Two things were settled on the way out, and both are worth more than the
withdrawn mechanism.**

**1. `0x96ad` is built at BRING-UP.** Verified live: dumped with four handsets
registered, then one deregistered and dumped again — **byte-identical**.
Combined with A-0106 (unchanged by paging), it is a static table set up at
init. So our all-zero page table is a **bring-up gap, not a registration
gap**, and the fix belongs at bring-up. That is now evidenced from two
independent directions rather than inferred.

**2. The QH=7 question is closed, and there was never a difference.** On 07.56
the capability templates live at `0x14d`, and stock's read
`38 49 94 EE` / `C8 01 85 11` / `40 11 10 20 04` / **`73 07 00 00`** — our
values exactly, QH=7 included. The `70 53 41 47 48` both rooms attributed to
stock is added at beacon-TX, not held in the template. Days were spent
looking for a mail that writes QH=7; **no such mail was needed, because our
beacon was never wrong.**

### What is implemented

`dect-stubd` now replays stock's bring-up order:

```
0x14:0x02:0x0153:00     ECO_CONFIG
0x14:0x02:0x0169:07     FEATURE_FLAGS
0x14:0x02:0x0157:00     CAPABILITY_PROFILE
0x22:0x02:0x015b:01     radio on
0x22:0x02:0x022b:41     open registration
```

`-a nostockcfg` restores the old two-mail bring-up, so the control is one
flag.

**This is a faithful replay, not an understood mechanism.** What those three
mails do on 07.56 is re-derived but unproven: `0x0153` writes `0x16c`/`0xdd8`,
`0x0157` writes `0x989b` bit 4 and `0x1e6` bit 0, `0x0169` sets
`0x154 |= 0x05`. None of that is a beacon block.

### Judge the test by this, and nothing else

| outcome | meaning |
| --- | --- |
| it rings | that was it |
| `0x96ad` populates, no ring | the bring-up gap is real, with a second cause above it |
| `0x96ad` still zero | not it; next is the `0x96ad` writer and `a stock routine`'s gate, both of which survive the reflash |

**Do not judge it by `0x0243` turning `2b`.** That prediction came from the
withdrawn decode.

### Still unproven

Which of the three mails matters, and the chain from it to the no-ring. Five
mechanisms this week fitted the evidence and died to a control: the bearer
arm, the `{CC-SETUP}` collision, the DSP regions, the page table, and now the
beacon. The bring-up gap has survived every control so far — which is not the
same as being proven.

## 2026-10-24, after the OpenWrt reflash: two findings, one of which voids evidence

### The FP-originated {CC-SETUP} addressed the wrong handset

`[observed]` Every FP-originated `{CC-SETUP}` this project has ever sent carried
the **same** `<<PORTABLE-IDENTITY>>`, whichever handset was being rung:

| run | handset that answered the page | IPUI in our `{CC-SETUP}` | |
|---|---|---|---|
| `vRINGC430A-20261024-104558`  | `80 a8 03 de ad 00 03` | `80 a8 02 de ad 00 01` | mismatch |
| `vRINGAS690H-20261024-105045` | `80 a8 03 de ad ...`   | `80 a8 02 de ad 00 01` | mismatch |
| `vRINGA540-20261024-103949`   | `80 a8 02 de ad 00 01` | `80 a8 02 de ad 00 01` | correct |

Cause: `cc_id_learned` is set only when learning and is never set by
`sub_load()`, so **every run starts with the gate open** and the first CC
message from any handset on air was learned and persisted into whichever `-s`
store was open. One handset runs a CAT-iq list-access call every ~22 s
(`{CC-SETUP}` call class 2, an `<<IWU-TO-IWU>>` dialog, `{CC-RELEASE}`), so it
reliably spoke first and won that race. The C430A and AS690H stores were
poisoned at registration, on 2026-10-24, while the A540 was powered on.

**What this voids.** "All three handsets refuse identically, therefore it is
our base" does **not** hold. The C430A and AS690H were paged correctly,
answered the page correctly, and were then offered a call addressed to a
different handset; their `0x32` is explained by that and says nothing about our
CC content. Only the A540 runs tested what they were believed to test.

Fixed: `dect_cc_peer_is()` (`src/libopendectcell/dect_cc.c`, tested in
`tests/test_cc.c`, both mutations caught) now gates the learn, and the daemon
prints `ignoring call identities from a handset we are not subscribed to`.
`cc_id_learned` is deliberately NOT set on the reject, so the subscribed
handset may still be learned later in the same run.

### The {CC-SETUP} collision is dead for the A540 -- properly this time

`[observed]` In `vRINGA540-20261024-103949` our setup went out in a **gap**
between the handset's own service calls: the previous released at 12565, the
next began at 13724, ours went at 13506. Correctly addressed, handset idle,
still `0x32`. The collision reading of EN 300 175-5 §9.5.2.3 / §17.3.3 is
eliminated for this handset.

### We were running a different chip firmware than the reference capture

`[observed]` `fwload.sh` defaulted to the **08.25** second-stage STUB; the
stock capture and every address the the hardware-analysis step derived are **07.56**
(`[CR A-0109]`: different dispatcher, different config base, "entirely
different addresses"). So every stock-vs-ours RAM comparison made before this
was cross-build, including "`0x96ad` is all zeros on our base".

Tested rather than assumed: the box was loaded with the 07.56 STUB and the
ring re-run. **Identical outcome** -- page -> `0x0242`/`0x0200` -> page
response -> `{CC-SETUP}` -> `0x32`, no `0x0209`. The STUB build is **not** the
cause. It did expose one build-specific defect: `uak_readback` reads `MATCHES`
on 08.25 and `MISSING` on 07.56, so `0x9518 + (slot-1)*0x11` is an 08.25
address and our UAK write lands nowhere on 07.56.

### What survives: the 0x0209 -> 0x0208 exchange

`[CR A-0107]` gives stock's working ring as
page -> `RX 0x0242` -> `RX 0x0200` -> `RX 0x0209` -> `TX 0x0208` -> handset alerts.
Measured on our side, both builds:

| step | stock | us |
|---|---|---|
| `0x015e` per call | never | 0 -- already matching |
| `RX 0x0242` / `RX 0x0200` after the page | yes | **yes, both** |
| `RX 0x0209` -> `TX 0x0208` | rings right after | **0 and 0** |
| `0x96ad` | populated | all zeros (but cross-build until now) |

So A-0107's hypothesis #4 is weakened: we *do* get the chip's page-response
pair, i.e. the chip matches the page response even with `0x96ad` reading zeros.
The single surviving divergence is that the chip never asks us for the auth,
so we never answer, and that is exactly where stock's handset starts alerting.
What makes the chip emit `0x0209` is a chip-mail question and is with the room.

### Not the cause, recorded so it is not re-tested

The page info byte. Stock pages `2c`, we page `0c`, and that is only the
documented derivation `0x0C | (basic service == 8 ? 0x20 : 0)` -- stock's setup
was wideband, ours is narrowband. `[CR A-0076]` already established the chip
never reads that byte and both values produce byte-identical pages.

### Correction to the section above: the build switch retired two divergences

`[observed]` The 07.56 run's *outcome* is identical (`0x32`), but the chip-mail
counts are not. Same A540 ring, same script, both builds:

| | 08.25 | 07.56 |
|---|---|---|
| `0x0242` / `0x0200` | 8 / 5 | 6 / 6 |
| `0x0243` | 254, middle byte **29** | 263, middle byte **2b** |
| `0x0205` | **0** | **258** |
| `0x0209` / `0x0208` | 0 / 0 | 0 / 0 |

Two items that had been carried as unexplained divergences from stock were
artefacts of the build mismatch and are now retired:

- **`0x0205` was absent on 08.25 and fires 258 times on 07.56**, the first at
  +1 ms after the page-response `0x0242`, then at frame cadence. `[CR A-0111]`
  identifies the `0x0243`/`0x0205` loop as the alerting emission itself, so on
  the matched build we HAVE it. "The CC machine never runs for our connection"
  was true of 08.25 and is false of 07.56.
- **`0x0243`'s middle byte is `2b` on 07.56**, matching stock, where 08.25 gave
  `29`. `[CR A-0111]` reports it as a hardcoded constant in the 07.56 image, so
  the two builds simply emit different constants. This was never a defect.

So the remaining chip-level gap is `0x0209` **alone**, not the trio. And the
handset still answers `0x32` while the alerting loop is running, which makes
the refusal an NWK-level decision rather than a missing bearer.

**Open, and not answerable with our current instrument:** whether the ring
connection gets a MAC bearer / U-plane slot. `memdump` revisits each 32-byte
chunk about every 176 ms while the frame pump runs every 10 ms, so a rotating
per-frame structure is sampled roughly 1 frame in 17. The 2026-10-24 sweep of
`0x160` and `0x10000`-`0x100ff` found no structural allocation after the page,
but at that resolution the null is aliasing, not evidence. A real answer needs
either a chip-side trigger or a far tighter dump window.

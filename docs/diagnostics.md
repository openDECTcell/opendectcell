# Diagnostics: what a DECT base should be able to tell you

**Status: design, not built.** Written 2026-10-23 after the maintainer pointed
at AVM's **DECT Monitor**, a feature of stock FRITZ!OS. Roadmap items are in
`ROADMAP.md` M8/M9 and in the controller's C5/C7.

## Why this is a feature and not a debugging aid

Every hard question of the week before this was written is a field on AVM's
Monitor screen: long-slot vs full-slot, the time slot in use, the carrier,
field strength, signal quality, which handset holds which subscription. We
were inferring, across multiple withdrawn hypotheses and several bench runs,
things a shipping product simply prints.

Two consequences:

1. **The information is obtainable.** A displayed field means the host can
   read it from the chip.
2. **Users want it too.** "Why does this handset drop in the kitchen" is the
   same question as "why will the A540 not attach", and the answer comes from
   the same place.

So this belongs in the product, not in a scratch script.

## The data

Per registered handset:

| field | source | status |
|---|---|---|
| identity (IPUI) | our own MM records; per-slot chip identity is `Q-0054` §1 | partly available |
| subscription slot, and whether the chip really holds it | slot audit at `0x9518 + (slot−1) × 0x11` `[CR A-0085]` | **built** |
| connected / idle | `0x0200` CONN_UP/DOWN | available |
| manufacturer, model | NWK `<<MODEL-IDENTIFIER>>` (0x78) | available, not captured |
| codecs offered / in use | `<<CODEC-LIST>>` we send, `0x0230` codec byte | available |
| encryption on this call | we run with `-C`; chip side is `Q-0049` | not available |
| **slot type (long / full)** | `dir1+6` = `0x11718`, `0x28`/`0x50` `[CR A-0079]`; not in `0xF625` at all `[CR A-0089]` | **available** |
| **time slot in use** | `0xF625` `[2]` — moves in lockstep with the carrier | **decoded** |
| **carrier (Tx/Rx frequency)** | `0xF625` `[3]` | **decoded** |
| **field strength** | `0xF625` `[6..15]`, ten per-carrier levels, larger = stronger | **decoded** `[CR A-0088]` |
| **signal quality** | `0xF625` eight per-interval error counters `[CR A-0089]`; which is which is open | **decoded, unnamed** |
| **call duration** | `0xF625` `[36..37]`, frames since establishment at 100 Hz | **decoded** |

Per base: RFPI (have), radio parked/up (have), the dummy bearer's slot,
carrier, radio power, DECT Eco state.

Per call, for history rather than live view: duration, codec, release reason,
and the quality figures above sampled over the call.

## Where each piece lives

### `libopendectcell` — the data, no I/O

A `struct dect_diag` per link plus a base-wide one, filled from what the
library already sees (CC/MM state, codec, identities) and from chip values the
caller hands in. The library must not read the chip itself: it has no
transport and that stays true.

So: `dect_diag_from_link()` for the protocol half, and plain fields the daemon
fills for the radio half. One struct, one place, so the LuCI app and the
controller cannot drift apart in what they mean by "quality".

### `dect-stubd` — the chip half

Periodic reads of the addresses above into that struct, and a way out:
a JSON line on a UNIX socket is the obvious one, since both consumers are
out-of-process. The slot audit and the UAK read-back already exist and should
feed the same structure rather than only printing.

### LuCI — the user-facing Monitor

A status page: one row per registered handset with the fields above, a base
section, and a live-ish refresh. The point is that a non-technical user can
answer "is this handset in range and what is it doing" without reading a log.

Deliberately **not** a debugging console — raw mails stay in `dect-stubd -v`.

### Controller — across cells

The single-cell view is the LuCI one. The controller's job is what one cell
cannot see:

- **which cell a handset is on**, and its level there, which is the input to
  any future roaming decision;
- **history**: quality per call in `call_log`, so "it was bad last Tuesday" is
  answerable;
- **alerting**: a handset whose subscription audit fails, or that has not been
  seen for *n* hours, is a support call waiting to happen.

The cell→controller protocol needs to carry the struct; `docs/cell-protocol.md`
has no diagnostics message today.

## Order, and what is blocked

1. **Now, no new facts needed:** the struct, the protocol-side fields, the
   slot audit and UAK read-back feeding it instead of only printing, and
   `<<MODEL-IDENTIFIER>>` captured from the handset.
2. **After `Q-0054`:** slot, carrier, field strength, signal quality — the
   radio half, which is most of the user-visible value.
3. **After `Q-0049`:** encryption state, which we cannot show because we do
   not yet do it.

Step 1 is worth doing before step 2 lands: it is the half that does not depend
on an answer, and it gives the chip values somewhere to go when they arrive.

## `0xF625` decoded from our own corpus `[2026-10-23]`

`[CR A-0087]` says the Monitor does not infer anything: the host sends
**`0xF624` with the link number**, and the chip answers **`0xF625`** carrying
the whole per-handset row. **We have been sending that request since G7** —
`-a status=8000` polls it every 8 s during a call — and have been throwing the
answer away except for one byte.

554 answers are in the corpus. Two fields fall out without needing the room.

### `[3]` is the carrier in use, and `[6..15]` are the ten per-carrier levels

DECT has ten carriers; bytes 6..15 are ten values. If `[3]` names the carrier
in use, the level at index `[3]` should be the strongest of the ten. Across
every sample we hold:

| RSSI spread (max − min) | samples | `[3]` is the strongest |
|---|---|---|
| any | 554 | 74.2 % |
| ≥ 8 | 509 | 79.8 % |
| ≥ 12 | 438 | 88.1 % |
| ≥ 16 | 389 | 94.6 % |
| **≥ 20** | **325** | **97.2 %** |

**The correlation appears exactly as a peak appears.** Where the spectrum is
flat — mean spread 8.0, no carrier dominant — "which index is largest" is
arbitrary and the match drops to chance. Where there is a real peak, it sits
on `[3]` 97 times in 100. A field that correlates only when there is something
to correlate with is the signature of a true identification rather than a
coincidence.

Worked example, `vF418`:

```
t= 50.84  [3]=06   rssi = 1e 1e 20 20 1f 1d 2c 24 1d 1f    index 6 = 0x2c
t= 58.84  [3]=06   rssi = 1f 1f 1f 20 1f 1f 38 24 1e 1f    index 6 = 0x38
t= 66.84  [3]=01   rssi = 1e 2f 1f 20 1e 1e 1d 24 1e 1e    index 1 = 0x2f
t=256.73  [3]=07   rssi = 1f 20 1f 1f 1f 1f 24 3b 1f 1e    index 7 = 0x3b
```

The peak moves with `[3]`, call by call.

**Units, partly answered:** the in-use carrier reads ~`0x2c`–`0x3b` against a
floor of ~`0x1e`–`0x20`, so **larger is stronger** — a level, not an inverted
quality. The absolute scale is still unknown.

**A free observation:** `vF413` shows a persistent ~`0x24`–`0x27` on two
carriers that are never the one in use. That is almost certainly another base
in the house, which is what a spectrum view is for.

### Fields still wanted from the clean room

`[0]` echoes the link number we asked for. `[2]` takes only even values
(`00 02 04 06 08`) and changes between calls — a slot candidate, unproven.
`[5]` is the byte `A-0077` traced to record `+0x116` bit 1, which
`[CR A-0087]` now says is one bit of a multi-bit computed value.

So what we still need traced is **Mode (long vs full slot)** and **the time
slot** — not the carrier or the level, which we now have.

## `0xF625`, second pass: the counter block, and a negative on Mode `[2026-10-23]`

`[CR A-0088]` confirmed `[6..15]` from the chip side (ten values copied from a
base array, stride 2, low byte each) and gave the **semantic** key for Mode —
`(received bearer byte0 & 0x0E) == 0x0A` means Long Slot — deliberately
leaving *which* answer byte carries it to our data, since we hold 554 answers
and they do not.

### The Mode test cannot run on this corpus, and that is a fact about the corpus

The proposed cross-check was: find the answer byte whose `& 0x0E` is `0x0A`
exactly when `dir1+6` (`0x11718`) reads `0x50`, and not when it reads `0x28`.

**Every read of `0x11718` we have ever taken reads `0x28`.** 57 reads, across
nine runs (`cap_A/B/C`, `sv_D/E/F/G`, `h0x10000`, `vF435`), two different base
addresses covering the byte. Not one `0x50`.

So every link in the corpus is a full slot, the independent variable never
moves, and no amount of mining can separate the Mode byte from the 25 other
bytes that are also constant. **The test is not negative; it is unrunnable,**
and it stays unrunnable until a long slot exists on the bench — which is
exactly what `F4.37` is for.

What the corpus *can* say is a bound on where Mode is **not**:

> Across all 554 answers, **no byte except `[1]` ever satisfies
> `(b & 0x0E) == 0x0A`**, and `[1]` is `0x0a` in all 554 — constant, and the
> obvious reading of a constant ten beside ten RSSI entries is "number of
> entries", not a bearer attribute.

Which leaves two possibilities, and they are worth stating because they need
different next steps:

1. Mode is one of the bytes that is **constant at `0x00`** here (`[4]`, or one
   of the high halves of the counter fields below) — consistent, since
   `0x00 & 0x0E = 0x00`, i.e. "not Long". A long-slot call settles it in one
   poll.
2. Mode is **not copied into `0xF625` at all**, and the Monitor reads it from
   the per-handset record directly. Also settled by the same run — if nothing
   moves, it is not here.

Either way the next move is a long-slot call, not a trace. Recorded so that
the room does not spend a trace on a question our data cannot answer yet.

### What the corpus *did* give up: the answer is a counter block

Bytes `[16..37]` are **eleven 16-bit little-endian counters** — every odd
index from 17 to 35 is `0x00` in all 511 full-length samples, which is what a
16-bit field with small values looks like. `[38..44]` are always zero.

**`[36..37]` is a frame counter for the link.** Measured against wall clock
across 365 consecutive poll intervals:

| | frames per second |
|---|---|
| median | **100.00** |
| 10th percentile | 99.92 |
| 90th percentile | 100.05 |

The DECT TDMA frame is 10 ms. **Nothing else in this system ticks at 100 Hz**,
and the agreement holds to ~0.01 s over 88 s of call:

```
vC4T4d, link 1:
  t= 93.71  @36=  343
  t=101.71  @36= 1142   (+799 frames = 7.99 s, elapsed 8.00 s)
  ...
  t=181.71  @36= 9143   (+8800 frames = 88.00 s, elapsed 88.00 s)
```

It starts near zero when the link comes up, so it is **frames since
establishment — the call duration, free, with no timer of our own**. It is 16
bits, so it wraps at 65 536 frames ≈ **10 min 55 s**; a consumer must track
wraps for longer calls.

**`[26..27]` is the frame count for the polling interval**, mirrored at
`[34..35]` (equal in 505 of 511). `0x0320` = 800 = 8 s × 100 Hz is the modal
value, matching our `status=8000` poll. And the running sum closes:
`@36(n) = @36(n−1) + @26(n)` is **exact in 347 of 365 consecutive pairs**; the
18 misses are all new-call restarts that my session-splitting did not catch,
not drift.

The other eight — `[16] [18] [20] [22] [24] [28] [30] [32]` — are **per-interval
event counters**: zero in 94–99 % of samples, otherwise 1 or 2, and they go
both up and down between polls, so they are not cumulative. These are the
shape of a Monitor's error columns (lost frames, handovers, CRC failures). Our
corpus cannot name them individually — we have too few events, and the obvious
test against spectrum quality came back **confounded**, not supporting:
samples with a *strong* carrier show more events (19 %) than flat ones (0 %),
because flat-spectrum samples are mostly idle links where nothing happens at
all. Naming these is a reasonable thing to ask the room for, later.

### `[2]` is promoted: it is the other half of the physical channel

`[2]` takes only `00 02 04 06 08`. The decisive observation is not its values
but **when it moves**. Across consecutive polls within one call:

| | count |
|---|---|
| neither `[2]` nor carrier `[3]` changed | 328 |
| **both changed together** | **32** |
| `[2]` alone | 3 |
| `[3]` alone | 2 |

**86 % of all changes to either byte are simultaneous changes to both.** A
DECT bearer handover moves to a new *physical channel*, which is a (slot,
carrier) pair — both halves change at once, by construction. A byte that is
constant through a call and jumps in lockstep with the carrier is the other
half of that designation. That is much stronger than "even values and it
varies", which was all we had.

Still unexplained: why only five even values. Either the chip reports
`slot × 2`, or it allocates only even slots. Not resolved here, and not worth
a guess.

### Revised field map

| bytes | meaning | confidence |
|---|---|---|
| `[0]` | link number (echo of the `0xF624` request) | certain |
| `[1]` | `0x0a` always — count of carrier entries | strong |
| `[2]` | **time slot**, or slot × 2 | strong (moves with `[3]`) |
| `[3]` | **carrier in use** | certain |
| `[4]` | `0x00` always | — |
| `[5]` | `01`/`02`; the `+0x116`-derived byte of `A-0077` | unknown |
| `[6..15]` | **per-carrier RSSI, larger = stronger**; dBm = (raw − 20) × 80/37 − 110 as FRITZ!OS converts it on the 7520, raw 0/255 = no value; index = carrier 0..9 = 1897.344 − 1.728·c MHz | certain `[CR A-0088]`, scale `[CR A-0121]` |
| `[16..25]`, `[28..33]` | eight 16-bit LE per-interval event counters | strong |
| `[26..27]`, `[34..35]` | frames in this polling interval | certain |
| `[36..37]` | **frames since link establishment** (wraps at ~10m55s) | certain |
| `[38..44]` | zero | — |

**Offset note** `[CR A-0088]`: our `[N]` is the chip's `answer[N+2]`; we index
the payload after the 2-octet primitive.

## `0xF625` closed from both sides `[CR A-0089]` `[2026-10-23]`

`[CR A-0089]` answered the two traces asked for above, and confirmed the
corpus decode from the chip side. What follows is what changed.

### `[5]` is named, and it is not Mode

**`[5]` is written as `(+0x116 bit 1) ? 2 : 1`** — the exact bit `A-0077`
traced and `[CR A-0087]` then withdrew as a slot-type indicator.

That closes the loop on our own measurement. `[5]` takes `01` and `02` across
our corpus while `0x11718` reads `0x28` (full slot) in all 57 reads: the bit
is set in ordinary bearer flow regardless of slot width, so `[5]` tracks
something else entirely. The corpus and the firmware now say the same thing
from opposite directions.

**And the authoritative slot type is not in `0xF625` at all.** It lives in the
per-handset record at `+0x0C`, as `(received bearer byte0 & 0x0E) == 0x0A`
`[CR A-0088]`. So our "no byte satisfies `& 0x0E == 0x0A`" was not a sampling
limitation after all — the byte genuinely is not there.

Two consequences, and the first is the good one:

1. **Mode is not blocked on anything.** `0x11718` (`dir1+6`) is our instrument
   and always was; `0xF625` was never going to carry it. The diagnostics table
   above is updated accordingly.
2. The `0xF625` diff on `F4.37` changes from a search into a **falsification
   with a pre-committed expectation: nothing moves.** It costs nothing — the
   daemon already polls — and if a byte *does* move on a long slot, the clean
   room's record-offset reading is wrong and we want to know.

A note worth keeping: if AVM's Monitor does display Mode from `[5]`, it would
mis-label some full-slot links as long. Not a thing to copy.

### The eight event counters: per-slot receive-status, and each one's trigger

`[CR A-0089]` mapped each counter to the bit that drives it. Every one
increments when its bit is **clear**, and since the bit is set in 94–99 % of
intervals (our zeros), **each counter counts the rare failing frames** — the
error columns, confirmed in shape.

| our byte | increments when |
|---|---|
| `[16]` | receive status `[0x12]` bit 6 clear |
| `[18]` | receive status `[0x12]` bit 1 clear |
| `[20]` | receive status `[0x12]` bit 0 clear |
| `[22]` | a derived condition — resolved by `[CR A-0090]` to frame byte 0 bit 4 set |
| `[24]` | a derived condition — resolved by `[CR A-0090]` to frame byte 0 bit 0 clear |
| `[28]` | did not map cleanly |
| `[30]` | receive status `[0x13]` high nibble nonzero |
| `[32]` | receive status `[0x12]` bit 7 clear |

**What is deliberately absent is which bit means what** — A-CRC vs X-CRC vs
sync. The room declined to label them from a reading it had not taken to
ground, which after this week is the right call.

### Our corpus cannot name them either, and the reason matters

Before asking for anything further I ran the obvious check: do the counters
separate by *when* they fire, or by *which fire together*? Across 511
full-length samples:

| pair | joint | expected if independent | enrichment |
|---|---|---|---|
| `[24]`&`[28]` | 13 | 0.90 | **14.4×** |
| `[16]`&`[30]` | 7 | 0.47 | 15.0× |
| `[16]`&`[18]` | 8 | 0.74 | 10.8× |
| `[22]`&`[24]` | 12 | 1.56 | 7.7× |
| `[22]`&`[28]` | 12 | 1.64 | 7.3× |
| *(every other pair with n ≥ 3)* | | | 4.5×–29× |

**Every pair is enriched.** That is the tell: errors cluster in time, a bad
interval lights several counters at once, and the co-occurrence is dominated
by one common "this interval was bad" factor rather than by any relationship
between specific fields. The same confound as the spectrum-quality test
earlier on this page, and it has to be called the same way: **the result is
not evidence about which counter is which.**

The call-phase split is no better — the apparent setup-vs-steady-state
separation rests on counts of 1 to 8 out of 146 first polls, which is nothing.

So the corpus is exhausted on this question. Naming these needs either a
**directed** test — drive one known cause and watch one column — or a trace of
the receive-status byte format. Not more mining.

### The directed test, designed now so it discriminates

Written before any run, per the project's own rule. A counter is named only if
it moves **alone**, because the clustering above means "several went up" tells
us nothing.

| cause to drive | predicted | falsifies |
|---|---|---|
| handset carried to the range edge, call held | the signal/CRC columns climb steadily; sync columns stay near zero | if everything climbs together, the test cannot name anything and must be redesigned |
| microwave oven / 2.4 GHz interferer near the base | a CRC column climbs while RSSI on the in-use carrier stays high | if RSSI drops too, the cause is not separable from range |
| handset at rest, strong signal, long call | all eight stay at zero over ≥ 5 min | if any column ticks on a clean link, it is not an error counter |

The third is the control and is the one to run first, because it is free: it
needs no movement, and it is the only one of the three that can be run as part
of an existing bench session.

**Blocked on the maintainer**, not on the clean room: no DECT transmission
happens unattended in this project, and the first two need somebody walking
around with a handset.

## The counters: shape confirmed, bits left unnamed `[CR A-0090]` `[2026-10-23]`

`[CR A-0090]` took the receive-status bits to ground and **declined to name
them**, which is the right outcome and worth recording as one.

### What resolved

**The counters are per-slot receive field-integrity counters.** They gate on a
receive-slot validator that reads the slot buffer and runs field-length checks
(10-bit and 9-bit field masks) plus a CRC-style complement.

That **explains our confound rather than merely coexisting with it**: a bad
slot fails field-length *and* CRC *and* sync at once, so the columns co-fire on
a single "this slot was bad" factor. The 4.5×–29× enrichment of every pair is
the signature of integrity counters sharing a quality factor — not noise, and
not evidence of any relationship between specific fields. The thing that made
the corpus useless for naming them is the same thing that confirms what they
are.

**`[22]` and `[24]` are a different kind.** They do not gate on the status byte
at all but on the received frame's byte 0, the slot header:

| counter | increments when |
|---|---|
| `[22]` | frame byte 0 **bit 4 set** |
| `[24]` | frame byte 0 **bit 0 clear** |

Which header bits those are in DECT terms is not pinned, so "header-bit
condition" is the honest level.

### What did not resolve, and stays a gap

Bits 0, 1, 6, 7 of receive status `[0x12]` and the `[0x13]` high nibble are
**unnamed**. The room traced the status byte toward the validator and hit an
inconsistency it could not clear — the writer it can see produces a value
inconsistent with the counters responding to that byte's low bits, so the
byte's true source is not pinned. Labelling them "A-CRC / X-CRC / sync" from
there would be a guess dressed as a trace, and this page would then carry it
as a prediction. **`[28]` likewise stays explicitly unknown.**

Per-bit names are available as a dedicated deep pass if there is ever a reason.
There is not one now: the directed tests that would use them are blocked on the
maintainer anyway.

### Our corpus corroborates the two-kinds split

A-0090's split makes a prediction the corpus *can* test, unlike the naming
question: counters of the **same kind** should co-fire more than counters of
different kinds, because they fail together on one bad slot. Pooling joint
occurrences against their independence expectations across 511 samples:

| group | joint | expected | pooled enrichment |
|---|---|---|---|
| within integrity (`[16] [18] [20] [30]`) | 36 | 2.67 | **13.5×** |
| within header (`[22]`&`[24]`) | 12 | 1.56 | 7.7× |
| **across integrity × header** | 46 | 7.97 | **5.8×** |

**Same-kind counters co-fire about 2.3× more than different-kind counters.**
That is a behavioural check on a firmware claim, from the one direction that
does not depend on the trace being right, and it holds.

Two things fell out that the room may want to revisit — both **suggestive, not
established**, because the expectations are small:

- **`[32]` does not behave like the integrity counter it is gated as.** It is
  described as status `[0x12]` bit 7, but against the other integrity counters
  it pools to 1.4× (1 joint, 0.74 expected) and against the header pair to
  5.8× (4 joint, 0.69 expected). It fires 6 times in 511, so this is thin —
  but the *contrast* is the signal, not the zeros.
- **`[28]`, the one that did not map, behaves like a header counter:** 9.8×
  with `[22]`/`[24]` (25 joint, 2.54 expected) against 5.7× with the integrity
  group (17 joint, 2.97 expected). A hint about where it belongs, nothing more.

### The control prediction is now stronger

Every one of the eight is gated on a receive-integrity failure or a header-bit
condition that a clean slot does not produce. So the F4.37 control —
**clean link, strong signal, five minutes, all eight read zero** — is no longer
just a sanity check on our reading; it is the room's prediction too. A nonzero
column on a genuinely clean link means one of them is not an error counter.

## The six-connection filter: is a region worth diffing at all? `[CR A-0100]`

Before reading a defect into any region that differs between our stack and
stock, classify it with data we already produce. It needs no firmware access
and no stock capture.

**The test.** A run that pages the handset also catches several connections
the handset opens by itself. Those are TDM-routed exactly as our
page-response connection is, so they are a controlled sample:

- a region that **moves** across those handset connections is
  protocol / C-plane / MAC-driven. Both stacks drive it, so a difference
  there is a **real defect** — diff it;
- a region that is **static** across them is architecture-dependent. It will
  diff as "missing" against any stock reference for architectural reasons,
  because stock's chip DSP carries the U-plane and ours does not — the audio
  goes over the IPQ4019 TDM block and the chip DSP is never engaged. **Do not
  read a defect into it.**

`0x3D4` and `0xEF4` both fail this test, which is exactly why they told us
nothing: one change in 63 chunks, at `t=1063 ms` during start-up, static
through six working connections and ours.

**Run every candidate region through it before trusting a null.** This is the
general form of the mistake that cost 2026-10-23/24 — see
[[verify-the-variable-arrived]] in spirit: a clean null from an instrument
that cannot move is not evidence.

### Where the firmware can and cannot help

`[CR A-0100]`, stated as a limit rather than a result: **below ~`0x1Axx` the
firmware image and live data RAM diverge.** At `0xEF4` the image holds
executable code, so the RAM our `0xFFF1` read returns there is a different
bank from what the image maps, and a dump address in that range cannot be
given a firmware meaning at all.

Consequently A-0098's "`0xEF4` = DSP lane-descriptor table" was read from the
dumped bytes alone, not cross-referenced, and is **withdrawn as structure**.
Only the behaviour stands: static, architecture-dependent on a TDM-routed
stack.

**Above ~`0x1800` the clean room can cross-reference and the classification is
firm.** Confirmed protocol/MAC targets:

| region | note |
| --- | --- |
| `0x1AB9 + handle × 0x148` | connection record. `+0xFD` rx slot, `+0x104` tx slot, `+0x116` slot/bearer state word, `+0x137` saved audio/flags — the MAC bearer handler sets and clears bit 6 of `+0x137` directly, so it is a protocol flag, not a TDM artefact |
| `0x965d` | bearer arm table; bit 15 observed moving 72 ms after an `0x015E` mail |
| `0x96ad` | page table |
| `0x2269` | beacon — live but continuous, so not connection-diagnostic |

Below `0x1800`, **the six-connection test is the only authority.**

> `0x9518 + (slot−1) × 0x11` holds the UAK. It is key material: **never add
> it to a dump list**, and exclude it from any analysis that prints bytes.
>
> A-0100 originally listed it among the firm protocol/MAC targets. It is not
> wrong that it is protocol-driven — that is precisely what makes it
> sensitive, since the path that populates it is the authentication exchange.
> Raised, and `[CR A-0101]` withdrew it. The target list above is final and
> carries no key material.

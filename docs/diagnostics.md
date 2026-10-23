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
| **slot type (long / full)** | `dir1+6` = `0x11718`, `0x28`/`0x50` `[CR A-0079]`; semantics `[CR A-0088]`, byte not yet locatable | candidate |
| **time slot in use** | `0xF625` `[2]` — moves in lockstep with the carrier | **decoded** |
| **carrier (Tx/Rx frequency)** | `0xF625` `[3]` | **decoded** |
| **field strength** | `0xF625` `[6..15]`, ten per-carrier levels, larger = stronger | **decoded** `[CR A-0088]` |
| **signal quality** | `0xF625` eight per-interval event counters; which is which is open | **received** |
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
| `[6..15]` | **per-carrier RSSI, larger = stronger** | certain `[CR A-0088]` |
| `[16..25]`, `[28..33]` | eight 16-bit LE per-interval event counters | strong |
| `[26..27]`, `[34..35]` | frames in this polling interval | certain |
| `[36..37]` | **frames since link establishment** (wraps at ~10m55s) | certain |
| `[38..44]` | zero | — |

**Offset note** `[CR A-0088]`: our `[N]` is the chip's `answer[N+2]`; we index
the payload after the 2-octet primitive.

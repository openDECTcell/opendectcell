# Handset compatibility

One table, kept current, of what each handset has actually been observed to do
against **our** base. It exists to answer a question that keeps coming up --
"should `libopendectcell` grow per-handset support?" -- with evidence instead of
impressions.

**The finding so far is that it should not, yet.** Every difference below that
looked like a vendor quirk turned out to be **our** non-conformance, which the
FRITZ!Fon C4 tolerated and the Gigasets did not. Writing a per-device
abstraction now would freeze our own bugs as "quirks". The working assumption
is therefore: **implement the standard, keep this table, and revisit only when
two demonstrably *conformant* handsets need genuinely different FP behaviour.**

## The handsets

| | FRITZ!Fon C4 | Gigaset A540 CAT | Gigaset C430A |
|---|---|---|---|
| vendor | AVM | Gigaset | Gigaset |
| role here | the handset G1-G7 were developed against | current bench handset | arrived with the A540; used by the the hardware-analysis step for stock captures |
| registered to our base | yes (2026-10-18) | **yes, 2026-10-22** | no -- *re-attaches* on a FRITZ!OS-era subscription |
| local status | a handset, on the a second unit, **off-limits for experiments** | spare | spare |

## Observed behaviour

| behaviour | C4 | A540 CAT | C430A | reading |
|---|---|---|---|---|
| GAP registration against our MM stack | passes | **passes** | not attempted | our MM is not AVM-specific |
| post-LOCATE message | not recorded | bare `0x5D`, `nwk_len=2`, no IEs | bare `0x5D` **and** `0x64` + escape `0x59` | `0x5D` is common; `0x64`/`0x59` is C430A-specific |
| accepts our 3-IE CC-SETUP (no bearer) | **rings** | **rejects**, `0x32` "Insufficient resources" | not attempted | the C4 is *tolerant*; the A540 is *correct* (§9.3.2.3) |
| PP-originated call binds audio | yes | yes | yes | the handset builds the bearer, so this path always worked |
| opens a periodic CAT-iq service call unprompted | no | **yes** -- `03 05`, then `03 60 77 04 c3 00 NN 00` with an advancing counter, then `03 4d` normal release, ~13 s cycle | not measured | likely LiA list access; harmless, but it misled the collision investigation |
| re-sync time after our base leaves the air | not measured | **~30 s** | not measured | paging must outlast this (now 20 pages / 60 s) |
| display with call class 8 (`0x88`) | "Intern", no number | not reached | n/a | call class, not CLIP encoding, drives the C4 display |
| display with call class 3 (`0x38`) | shows the number, call dies ~4 s | not reached | n/a | the dying call is our `CALL_PRESENT` hole, not a handset trait |

## Differences that are genuinely per-vendor

Only one so far, and it is vendor-scoped **by definition** rather than by
quirk: the handset **name and clock push**, carried in `<<ESCAPE-TO-
PROPRIETARY>>` (`0x7B`) under **EMC `0x0002`**, sub-tag `0x54` (name) and
`0x3B` (BCD date/time). See `registration-g6.md`. `[observed]` EMC `0x0002` is
used in both directions and by a factory-reset third-party Gigaset, so emitting
it is interoperation rather than impersonation.

When this is implemented it should be a **feature flag**, not a device
abstraction: gate it on our own `LOCATE-ACCEPT`, which every handset reaches,
and not on `0x59`/`0x64`. There is now a positive reason for that and not just
a cautious one: `0x59` is the handset's **acknowledgement** of the base's
`0x5B` sequence counter `[CR A-StockTrace-4 §3]`, so it is a response, never a
request. The earlier reading of it as an identify that triggers the push was an
artefact of the interleaving (push N, ack N, push N+1, ack N+1).

The channel is **sequenced and acknowledged**: one monotonic byte per
subscription, `+1` on every push, shared across name (`0x54`), time (`0x3B`),
settings (`0x58`) and softkey labels (`0x26`). A value the handset has already
seen will be deduped, so a base that reuses one would push once and then
silently stop updating.

**The answer is capability detection, not a per-model allowlist.**
`[CR A-StockTrace-5]` makes the point that settles the design question at the
top of this file: **whether a handset implements the EMC `0x0002` channel is
observable at registration.** A handset that sends EMC `0x0002` escape records
during ACCESS-RIGHTS / LOCATE implements it; one that sends none does not. So
gate the push on "did this subscription use the `0x0002` channel while
registering", recorded per subscription at runtime -- never on a model string.

That is the clean form of per-handset support: not a table of devices, which
ages badly and encodes our own bugs as quirks, but a capability learned from
what the handset actually did. It also degrades correctly for handsets nobody
has ever seen.

`[observed]` the C430A uses the channel **proactively, not only to
acknowledge**: six handset-originated EMC `0x0002` escape IEs that are not
acks -- capability records `0x06`/`0x29` and `0xa1` during registration, and a
`0x58` settings record with its own counter at re-attach. Our A540 sends
**none of these, ever**.

**Open, and the first real candidate for per-handset behaviour:** `[observed]`
our **A540 CAT has never sent a single `0x7B` escape IE** -- not in
registration, locate, or any call, across every run we hold. The C430A sends
them. **This does not establish that the A540 cannot receive a push**: the
C430A's frames are acks, and our base has never pushed anything, so the A540
has had nothing to acknowledge. The test is cheap once the push is implemented
-- send a name and watch for a display change and a `0x59` carrying our
counter. If the A540 ignores it, this becomes the first genuine case of two
conformant handsets needing different FP behaviour -- but it is handled by the
capability detection above rather than by a device abstraction, so the
recommendation at the top of this file stands.

`[assessed, CR A-StockTrace-5]` the ack is integral to the channel, so a
handset that cannot emit `0x7B` almost certainly cannot be a receive-only
participant either: it could not acknowledge a push, which would stall the
stock flow. `[not established]` the receive direction for the A540 -- there is
no capture of one being pushed to, and our own test is still the definitive
check.

`[not established]` who owns EMC `0x0002`. It is used by the AVM base and by
the C430A but apparently not by the A540 -- **both Gigasets** -- so it reads as
a model/firmware-specific implementation of an AVM extension rather than a
universal Gigaset feature. Registry ownership remains unchecked.

## What would change the recommendation

Add a per-handset mechanism when, and only when, **two handsets that both
conform to EN 300 175 require different FP behaviour**. The one candidate is
the call-class/display interaction in the table above, and it is not proven:
the C4's class-3 call died from a defect of ours, so that row does not yet
describe handset behaviour at all.

Until then, every entry above is a to-do against the **standard**, not against
a device.

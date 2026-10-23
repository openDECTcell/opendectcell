# We tell the chip things and never check `[audit 2026-10-23]`

Prompted by the maintainer asking a question nobody had asked: *once we load a
handset ID into the chip, do we check that it actually worked? Do we check
commands were confirmed? Do we raise errors if not?*

The answer is **almost nowhere**, and it is systemic rather than an oversight
in one place.

## Every host→chip command, and what we know afterwards

| mail | what it does | do we verify? |
|---|---|---|
| `0x020A` | **writes the subscription record / UAK** | **no** |
| `0x0247` | subscription slot operations (incl. clearing) | **no** |
| `0x0208` | cipher enable | **no** |
| `0x016A` | channel config | **no** |
| `0x0251` | set channel parameters | **no** |
| `0x022D` | audio path on | **no** |
| `0x0233` | audio disconnect | **no** |
| `0x0300` | page a handset | **no** (the page *response* is a NWK event, not a confirmation that the page was built or sent) |
| `0xF626` | DSP route | **no** |
| `0x0230` | assign audio channel | **believed** confirmed by `0x0242` — and `[CR A-0075]` says `0x0242` is emitted on every path including the no-act early-out. It means "mail processed", not "it worked" |
| `0xF624` | status poll | **yes** — `0xF625` carries the answer |
| `0xFFF1` | memory read | **yes** — it is a read; the reply *is* the result |

**Two of twelve are verified, and both are reads.** Every command that
*changes* chip state is fire-and-forget.

## Why that is worse than it sounds

- **The chip silently drops unknown commands.** `[CR A-0070]`: "Anything not
  in that table is NOT a command — the dispatcher silently drops it." So a
  wrong primitive, a wrong task id or a malformed payload produces **no error
  and no reply**, which is indistinguishable from success.
- **The one reply we treated as confirmation was not one.** We read `0x0242`
  as "the channel is configured" for weeks. `vF418` has six byte-identical
  `0x0242` replies to sends that changed nothing, next to one that worked.
- **Chip state outlives us.** Subscription slots survive a daemon restart, so
  "it works" can mean "something an earlier session wrote is still there".
  We found that exactly once, by accident, when a handset we held no record
  for kept attaching.

## What we *do* check, and the distinction worth drawing

The daemon raises 22 errors. Every one is about **our own inputs**: a
malformed caller number, a `<<CALL-INFORMATION>>` missing its line id, a
subscription slot outside 1..6, a local send that failed. These are good and
they have caught real bugs.

**We validate what we send. We never verify what happened.** Those are
different problems and we have only solved the first.

## This may be the A540 question

The maintainer asked, separately, why the A540 sometimes does not appear.
**If its subscription record did not actually land in the chip, the symptom
would be exactly that** — a handset that is powered, in range and registered
as far as we know, which simply never attaches. We would have no way to tell,
because we write `0x020A` and move on.

That is not a claim that it *is* the cause. It is a claim that **we could not
currently distinguish it from any other cause**, which is the real defect.

## What would fix it

We already own the instrument: `0xFFF1` sub `0x07` reads chip RAM, and we use
it constantly — for research. Turning it into a routine post-write check needs
one thing we do not have: **the address of the subscription table**. We know
the per-connection records (`0x1AB9 + handle × 0x148`) but not where `0x020A`
puts its record. Asked as `Q-0052`.

With that, the shape is small and mechanical:

1. **Read back after writing.** `0x020A` writes a record; read those bytes and
   compare. Log loudly on mismatch rather than carrying on.
2. **Stop calling `0x0242` a confirmation.** Use the per-connection record's
   act/no-act gate (`+0x116` bit 5, `[CR A-0075]`) where one is needed.
3. **A health line at bring-up.** Which subscription slots the chip actually
   holds, read from the chip rather than from what we believe we wrote.
4. **Say when a check is impossible.** For commands with no readable effect,
   the honest log line is "sent, cannot verify" — not silence that reads as
   success.

Items 2 and 4 need no new facts and can be done now. Item 1 and 3 wait on
`Q-0052`.

## ANSWERED, and the conclusion is narrower than the audit `[2026-10-23]`

`[CR A-0085]` and `[CR A-0086]` came back, and between them they say: build
**one** check, not a system.

### The chip cannot say no, so a read-back is the only check that can exist

`[CR A-0086]` §5, the sentence we asked for: **none of the ~24 chip→host
indications is an error or a NAK.** All are state, event or data. The chip has
no way to report that a command was wrong. We were not missing a mechanism;
there is none, for anything.

### Stock does not verify either — and that is not negligence

`[CR A-0086]`: no read-back, no retry, no verify, anywhere. A product that has
run this chip in the field for years does not check. **But it periodically
re-asserts** most subscription state, so a silently lost write self-heals.
That is a better design than checking: it needs no error channel, and it
recovers rather than merely noticing.

**It also means our general verification would have been effort spent on a
problem that does not occur.** We dropped it.

### The UAK is the exception, and stock has the hole too

The UAK is written once at enrolment and **never re-asserted**. It is also the
credential a handset needs to attach. So it is the one write-once,
unprotected, silently-droppable command in the system — and stock has our
exact failure mode here with no way to notice or recover.

**So: one read-back, for the UAK.** `[CR A-0085]` gives the address —
`0x9518 + (slot − 1) × 0x11`, stride 17, six slots, the handler a direct
memcpy that returns immediately, and the chip processes mails in order, so a
read issued after the write sees the result.

**Implemented and confirmed on hardware the same evening:**

```
sub: UAK read-back slot 1 at 0x09518: MATCHES (16 bytes, flag 00)
```

The UAK is key material: we compare it and print the verdict, never the bytes,
not even under `-p`.

### A sharp edge we were already guarded against, by luck

`[CR A-0085]`: `0x020A` writes per-slot config; we clamp the slot to the valid range
`0x957E+`, which begins the per-handset config. We clamp to 1..6
(`DECT_MM_MAX_SLOT`), added earlier for a different reason entirely
(`A-SubscriberCapacity-7`). Two independent reasons for the same guard is a
good place to be; it being luck is worth writing down.

### What the run summary is for, now

The table stays, with its notes corrected to say what we now know. It is no
longer a to-do list — it is there so a quiet log never reads as a verified
one:

```
chip: 8 state-changing commands sent, 1 verified by read-back. 84 query/reads.
chip: the other 7 were sent and assumed -- the chip has no error indication
      to have missed [A-0086].
```

### Still open: the A540

`[CR A-0086]` §6: stock has **no reachability supervision** either. It reacts
to DECT protocol events, not to absence. So "powered, in range, registered,
intermittently will not attach" is a symptom stock could not diagnose on this
hardware either — we did not fail to build something it has.

What we now *can* do is settle one candidate: dump
`0x9518 + (slot − 1) × 0x11` after the A540 registers. If its UAK is not
there, that is the answer.


## The subscriptions themselves, and a correction `[2026-10-23, later]`

The maintainer pushed back on the conclusion above: *what about the handset
subscriptions, since they have been problematic?* Checking it found that the
reasoning behind "general verification is unneeded" **does not apply to us.**

### Stock re-asserts. We do not.

`[CR A-0086]`'s argument is that stock gets away without verifying because it
periodically re-asserts subscription state, so a silently lost write
self-heals. True of stock. **But `0x017C`, the re-assert, is on our own list
of mails we deliberately do not send** -- `send_subscription()` has said so in
a comment since G6.

So: **stock does not verify but does re-assert. We did neither.** The
subscription table is simultaneously the thing that has caused us the most
trouble and the place we were least like the design we were reassuring
ourselves with.

### What the chip has actually been holding

`vF426` is what this looks like when nobody checks: slot 1 ours, slots 2–6
untouched from earlier sessions, **51 connections, every one from a handset
our store did not contain**. We added a clear for foreign slots in response.
`0x0247` has no reply and no error path, so "cleared" has meant "asked" ever
since.

Now audited. After the clear, all six slots are read back from
`0x9518 + (slot − 1) × 0x11` and reported:

```
sub: chip slot audit: 1=OURS 2=- 3=- 4=- 5=- 6=-
sub: slot table is as we intend it
```

and, when it is not:

- `*** OUR OWN SLOT n IS EMPTY ***` — the subscription did not land, the
  handset will not attach.
- `*** n FOREIGN SLOT(S) STILL HELD ***` — the clear did not take, and a
  handset we hold no record for can attach.

Occupancy only: a slot holds key material or it does not. **We never print the
key.** `0x0247` moves from "not verified" to verified-by-read-back, and a
clean run now reads `8 state-changing commands sent, 7 verified by read-back`
where an hour ago it read `NONE`.

### Still missing: which handset

We can say a slot is occupied, not *whose* it is. The identity lives in the
per-handset config above `0x957E`, whose layout we do not have. `Q-0054` asks
for it. With that, the audit could name the intruder instead of counting it --
and the A540 question becomes answerable directly: is its record there, under
its own identity, after it registers?

## The diagnostic we did not know existed `[2026-10-23, later still]`

The maintainer pointed at AVM's public help page for the **DECT Monitor**, a
feature of stock FRITZ!OS. It displays, per registered handset: `IPUI`,
connected/disconnected, manufacturer and model, **field strength**,
**signal quality**, **`Mode` — "Long Slot" (HD, two slots) or "Full Slot"**,
**Tx/Rx frequency**, **the time slot in use**, supported and active codecs,
whether the call is encrypted, and DECT Eco support. For the base: RFPI, the
slot in use (including the **dummy bearer's** when no call is up), Tx
frequency and radio power.

**Every hard question of the last week is a field on that screen.**

| what we did | what the Monitor does |
|---|---|
| spent an evening inferring long-slot from `0xF625` byte 5, then record `+0x116` bit 1 (withdrawn), then `dir1+6` | prints `Mode` |
| declined the air-slot question for lack of a consumer | prints the slot in use |
| have a handset that will not attach, and no way to see why | prints field strength and signal quality |
| audit slot occupancy but cannot say whose slot it is | prints the IPUI per handset |

That the information is displayed means **the host can get it from the chip**.
We have been inferring what a shipping product reads directly.

### One thing we already receive and have never used

`0xF623` streams while the radio is on:

```
09 0a 1d 1e 1e 1f 1e 1d 25 21 1e 1e 37 00
      '--------- ten values, 0x1d..0x27 ---------'
```

**Ten values, and DECT has ten carriers.** Our guess is a per-carrier RSSI
scan — which would also be how the base picks its quietest carrier, something
`mac-bringup.md` has recorded as "the STUB picks the quietest carrier itself"
since G5 without anyone asking how it knows.

Asked as `Q-0054`, with the guess stated so it is cheap to correct.

### A position withdrawn

Earlier the same day I declined to have the air slot traced, on the grounds
that it had no consumer and the room had four open items that did. That was
right at the time and is wrong now: the Monitor is the consumer, and it shows
the slot, the carrier and the level together. Recorded because the reasoning
was sound and the conclusion still expired within hours.

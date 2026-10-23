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

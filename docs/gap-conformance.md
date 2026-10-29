# GAP conformance: what EN 300 444 actually requires of us

**Audited 2026-10-23 against EN 300 444 V2.5.1 (2017-10), Table 1 and
clause 7.** Before this date the project did not hold the GAP specification
at all — we had been implementing against EN 300 175-5 and *inferring* what
GAP wanted. The first audit against the real profile found a mandatory
feature we had never implemented.

## Which column applies to us

Table 1 splits the FT status into **R/B** (residential/business) and **P**
(public). We are a residential base, so **R/B is our column** — and it is
materially weaker than the public one. Two conditionals matter:

- **C101 / C501: IF feature N.35 (enhanced security) THEN M ELSE O.** We do
  not implement N.35, so everything gated on it is **optional for us**.
- **C502: IF N.36 (AES/DSAA2) THEN M ELSE I.** Not implemented, not required.

## The mandatory set, and where we stand

| feature | procedures (clause) | status |
|---|---|---|
| **N.1 Outgoing call** | 8.2 request, 8.6 connection, 8.10 sending keypad information | **procedures complete** 2026-10-23; the collected digits still need a consumer |
| **N.2 Off hook** | 8.2 outgoing call request, 8.15 incoming call connection | done |
| **N.3 On hook (full release)** | 8.7 normal call release, 8.8 abnormal call release | done |
| **N.4 Dialled digits (basic)** | 8.10 sending keypad information | **implemented 2026-10-23**, not yet seen on hardware |
| **N.8 Incoming call** | 8.12 request, 8.13 confirmation, 8.14 PT alerting, 8.15 connection | done; verified on the C4, and on the A540 since the transaction-value fix of 2026-10-24 (below). 8.14: <<SIGNAL>> 41H by default since `9c4414c`, 40H for internal calls since `5c25e3d` |
| **N.15 Alerting** | — | done |
| **N.18 Subscription registration on-air** | 8.30 obtaining access rights | done, verified on two vendors' handsets |
| **N.19 Link control** | 8.35 indirect FT-initiated establishment, 8.36 direct PT-initiated, 8.37 normal release, 8.38 abnormal release, 8.39 link maintenance | 8.35/8.36 done; 8.37-8.39 since 2026-10-28: the FT releases a link after its last call or list session (`0x0202`, `96b54e4`, `f93e86b`), no <LCE.01>/<LCE.02> yet. This row said "done" before 8.39 existed here (spec sweep A1) |

**Eight mandatory features. As of 2026-10-23 all eight are implemented**, and
seven of the eight are verified on hardware — N.4 is the exception and gets
its first live test on `F4.37`, where the maintainer dials out from the A540.

> *Audited and found missing on 2026-10-23; implemented the same day. The
> table below is kept as written so the gap and its closure are both legible.*

## The gap: N.4, dialled digits

EN 300 444 §8.10:

> "The PT shall be capable of sending keypad information which shall be
> included in the `<<MULTI-KEYPAD>>` information element in one or several
> `{CC-INFO}` messages. The PT and the FT are mandated to be able to perform
> this procedure in states T-02 and T-10."

We implement neither half:

- **`<<MULTI-KEYPAD>>` does not exist in the codebase.** No IE constant, no
  parser, no accessor.
- **`{CC-INFO}` (`0x7B`) is defined in `dect_nwk.h` but never dispatched.**
  `dect_cc_rx()` handles SETUP, ALERTING, CONNECT, CONNECT-ACK, RELEASE,
  IWU-INFO and RELEASE-COM. A `{CC-INFO}` arriving from a handset is
  silently ignored.

**What that means in practice: a handset dialling a number on our base sends
us the digits and we drop them.** It is the single most user-visible hole in
the GAP surface, and it is why N.1 "outgoing call" cannot be called done
either — 8.10 is listed as mandatory under N.1 as well as N.4.

It is also squarely on the critical path for the controller: SIP calls *out*
cannot be placed until the digits arrive.

**Fixed 2026-10-23** (API 6). `{CC-INFO}` is dispatched, `<<MULTI-KEYPAD>>`
is collected across messages and from an en-bloc `{CC-SETUP}`, bounded at 32
characters with an overflow flag, surfaced through `ind.keypad` and
`dect_cc_keypad()`, and printed by the daemon. Five tests, one of which
replays the exact nine bytes captured from a handset. No reply is sent:
§8.10's figure leaves the FT in the same state and LAPC has already
acknowledged the frame.

**The identifier came from our own captures, not from the spec's coding
table** — `pdftotext` renders that table column-wise and a first
reconstruction of it gave `0x32`, which failed its own calibration against
IE values we already knew. The corpus settled it:
`03 7b 2c 05 31 32 33 34 35`.

**What the original fix note said** (kept for the record): the
`<<MULTI-KEYPAD>>` IE codec, a `{CC-INFO}` dispatch case, digit accumulation
on the CC entity with the collected string exposed through `dect_link_ind`,
and the DECT 8-bit character codings from EN 300 175-5 annex D. Overlap
sending (8.3) is **optional** for R/B, so en-bloc collection satisfies the
profile; the mandatory part is only that the digits are received and
understood.

## What is optional for us that we had assumed mandatory

**Ciphering is not required of a residential GAP FP.** N.17 "Encryption
activation FT initiated" is **C101 → optional** in the R/B column, and the
DLC service D.6 is `C203: IF N.17 OR N.27 THEN M ELSE I` — irrelevant if we
do not claim either. `Q-0049`'s framing ("GAP expects the FP to support
ciphering, and a handset is entitled to insist on it") is **half right**: the
profile does not require it of us, though an individual handset may still
refuse to operate without it, which is a compatibility question rather than a
conformance one.

Also optional in our column, and implemented anyway: **N.9 authentication of
the PP** (C101), **N.11 location registration**, **N.30 CLIP**, **N.34
CNIP**. CAT-iq list access is not a GAP feature at all.

## What this does to the roadmap

M8 lists "outgoing calls" as an item. It is more pointed than that: **one
mandatory GAP feature is absent**, and it is small, well-specified and
hardware-free. It should come before the optional work in M8 — ciphering in
particular, which this audit shows is not required of us.

## Two things that look like GAP basics but are not

Asked 2026-10-23 alongside the dialled-digits work, and worth recording so
nobody implements them twice.

### Call duration is the handset's own timer

**There is nothing for us to implement.** GAP's feature list (N.1–N.36) does
not contain call duration, and `<<DURATION>>` in EN 300 175-5 is not about
call timing at all — it carries **identity and access-rights lifetime**, used
in `{TEMPORARY-IDENTITY-ASSIGN}` and in `{LOCATE-ACCEPT}`/`{LOCATE-REJECT}`
to say how long an assignment stays valid. (`DECT_IE_DURATION` is `0x72` and
was already in `dect_nwk.h`.)

The elapsed-time display during a call is **handset-local**: the PT starts its
own timer when the call connects. The only way we break it is by failing to
establish the call properly — if `{CC-CONNECT}` goes out and the call reaches
ACTIVE, the handset counts. That path is verified (G7).

Where duration *does* become ours is the **CAT-iq call list**: missed- and
received-call entries carry a duration and a timestamp. That is list access
(TS 102 527-3), which `dect_iwu.c` partly implements, and it is a later
feature rather than a GAP basic.

### The handset's clock: `<<TIME-DATE>>` (resolved)

Not a GAP requirement, but **visible to a user**: without it, any timestamp
a handset shows (call lists in particular) is whatever the handset itself
believes. CAT-iq's date and time synchronisation (TS 102 527-3 §7.4.2) now
runs in two places, both behind the same clock gate (no NTP-disciplined
clock, nothing sent; `-a timesync=`):

- **After every location registration** (§7.4.20.2), on that link.
- **Every `time_update` hours** (UCI, default 24; `-a timeupdate=MIN` on the
  daemon; 2026-10-29): one round over all handsets, one at a time. A
  handset with a link gets the `{FACILITY}` there; one without is paged
  first (§7.4.2.1: indirect link establishment, C-plane page), and the link
  is released once the `{FACILITY}` is acknowledged. A handset that does
  not answer five pages is skipped until the next round. The round waits
  while a call, a ring or a deregistration is under way.

A daily round also carries the change to and from summer time, which a
handset that stays attached would otherwise only learn at its next location
registration. `[assumed]` until bench card 36 has run.

## General requirements we broke, found 2026-10-24 night

Three clauses outside the feature table, found by the external review
(`the clean-room record/an external review-24102026.md`) and checked against the text:

| clause | requirement | status |
|---|---|---|
| **6.9.2** | "The transaction identifier value for a CC call shall always get assigned the lowest available free number." | **broken from 2026-10-20 to 2026-10-24 and the cause of every Gigaset refusal**; fixed in `dd5a630`, verified on the A540 (run 15 rang, control run 16 refused) |
| **8.14** | PT alerting by `<<SIGNAL>>` in `{CC-SETUP}` or in `{CC-INFO}`; "FT is required to support one of the methods" | **fixed** in `9c4414c`: 41H by default; 40H for internal calls since `5c25e3d` |
| **6.9.4** | an unexpected or unrecognised CC message "shall be ignored" (except release messages) | **fixed** in `0ac7e38`: ignored on our own call; on a transaction we do not hold, cleared "unknown transaction" (EN 300 175-5 §17.3.2.1), extended to every message type in `6400c26` |

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
| **N.1 Outgoing call** | 8.2 request, 8.6 connection, **8.10 sending keypad information** | **incomplete** — 8.10 missing |
| **N.2 Off hook** | 8.2 outgoing call request, 8.15 incoming call connection | done |
| **N.3 On hook (full release)** | 8.7 normal call release, 8.8 abnormal call release | done |
| **N.4 Dialled digits (basic)** | **8.10 sending keypad information** | **NOT IMPLEMENTED** |
| **N.8 Incoming call** | 8.12 request, 8.13 confirmation, 8.14 PT alerting, 8.15 connection | done, verified on the C4 |
| **N.15 Alerting** | — | done |
| **N.18 Subscription registration on-air** | 8.30 obtaining access rights | done, verified on two vendors' handsets |
| **N.19 Link control** | 8.35 indirect FT-initiated establishment, 8.36 direct PT-initiated, 8.37 normal release, 8.38 abnormal release | done |

**Eight mandatory features. Seven are implemented; one is missing, and it
breaks a second.**

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

**Scope of the fix** (hardware-free up to final verification): the
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

# Lines and calls: the line pool, call ids and call waiting (design, 2026-10-28)

The MVP decided on 2026-10-28 (ROADMAP M9b): **two SIP lines per handset from a
line pool**. This page is the design for the DECT side of it, from TS 102 527-3
V1.7.1 and the spec sweep (`spec-sweep-2026-10-28.md`, findings 1, 3-6, and the
CAT-iq tables in `spec-sweep-2026-10-28/A2-catiq.md` sections D, E, G, K,
MVP-1). It covers what the cell must do; the SIP side (baresip bridge) only
appears as the events it exchanges.

## The model

| object | what it is | where it lives |
|---|---|---|
| **line** | a SIP account offered to handsets: line id (0..126, unique), name, the set of attached handsets (a bitmap by terminal id), the multiple-calls mode (single / multiple) | the line pool: `dect_iwu` serves it as the Line Settings List (§7.4.11.4, table 72); the bridge configures it |
| **call** | one call in the system: a **call id** unique system-wide (§7.4.6.1), its line (external) or its two handsets (internal), its state per handset | the daemon's call table; one call has the same id on every handset that sees it |
| **handset context** | per handset: its link, its one CC transaction (§7.4.3.1: one link, one U-plane, one CC instance per PP), the calls it holds (at most two: active plus held or waiting) | the daemon |

Two facts shape everything else:
- **A second call on a busy handset costs no radio link and no audio
  channel.** It rides the existing CC transaction as {CC-INFO} messages
  carrying <<CALL-INFORMATION>> with the second call's id (§7.4.3.1,
  §7.4.3.5.2). The chip's three audio channels limit the number of handsets
  in calls, not the number of calls.
- **Only NG-DECT Part 3 handsets understand any of it.** "A Part 3 FP shall not
  send any call status to GAP or NG Part 1 devices" (§7.4.6.4.1). Today's
  bench: the A540 is Part 3; the C430A and AS690H are Part 1, so for them a
  second call is "busy" toward SIP (A2 K6), and line choice is FP-managed.

## What each part does

| situation | DECT side (Part 3 handset) | Part 1 / GAP handset | bridge event |
|---|---|---|---|
| incoming call on line L | page and SETUP **every idle handset attached to L** (§7.4.8.1.2, G7), each with line L, line type, the call id, CS call setup (done for one handset, `2c76ace`); on the first CONNECT release the others (§7.4.3.5.4) and send CS call connect (done) | SETUP without call information | `call-in(line, number, name)` → `answered(handset)` |
| incoming call for a handset already in a call | **call waiting**: {CC-INFO} on its link with the new call id, CS call setup, <<SIGNAL>> 07, number, name (§7.4.3.5.2, K2); it answers `1C35` accept, `1C36` reject, `1C38` release-and-accept with the call id (K3) | not offered; busy toward SIP | `waiting`, `accepted` / `rejected` |
| handset places a call | read its line choice (<<CALL-INFORMATION>> line id in SETUP or the first CC-INFO, or "None" 7FH → the FP picks); confirm line and type before CONNECT; refuse a line it is not attached to (§7.4.5.2, E2) | the FP picks its default line (or `#`+digit, optional) | `call-out(line, digits)` → `connected` |
| second outgoing call while in a call | `1C15`+number or `17`+number in {CC-INFO}: CS call hold for the active call, the new call id, CS call setup ack (§7.4.3.5.1, D4) | n/a | `call-out` with hold |
| toggle, hold, resume, release one | `1C31`, `1C41`/`1C42`, `1C33` with call id; CS hold / connect / idle in {CC-INFO} (D6-D9) | n/a | `hold`, `resume`, `hangup(call)` |
| no audio channel | incoming: not presented to idle handsets (K7, busy toward SIP); a busy handset may still get call waiting. Outgoing: CS call disconnecting "system busy" + <<SIGNAL>> 04, then release (§7.4.8.3, K8) | {CC-RELEASE-COM} "insufficient resources" (as today) | `busy` |
| a handset in a list session | pseudo call waiting: CONNECT (no ids), then the call-waiting {CC-INFO}s (§7.4.10.6.3, K5) | n/a | as call waiting |

Call ids: allocated by the daemon from 1..127, never two live calls with the
same id; an internal call keeps one id on both handsets (§7.4.6.1 note 2). For
an external incoming call the per-handset ids before the answer may differ
(§7.4.6.1 note 1); we use one id for all.

## Order of work, each step testable on the PC first

1. **Line table in `dect_iwu`** (this commit series): N lines, each with its id,
   name and attached-handset bitmap; the Line Settings List serves them all
   (fields 01-03 as today, per line). The single-line API stays as a
   shortcut. No behaviour change with one line.
2. **The handset's line choice:** parse <<CALL-INFORMATION>> from the handset's
   SETUP and first {CC-INFO} into `dect_cc` (line id or "None"); answers carry
   the chosen line. The daemon refuses an unattached line.
3. **Call ids:** a daemon allocator; `dect_cc` takes the id per call instead of
   per link, set by the `call_status_for` hook (API 35) at SETUP time, which
   already runs before CC answers.
4. **Ring all attached idle handsets** for an incoming call; release the others
   on the first answer.
5. **Call waiting** for a Part 3 handset in a call: the {CC-INFO} builder (have:
   `dect_cc_info`, API 31; `dect_callinfo` lacks the call-status *reason*
   subtype, D3), parsing `1C35`/`1C36`/`1C38` with their call id.
6. **Hold, toggle, release one, outgoing parallel call** (D4-D9).
7. The bridge's events, then baresip.

**Progress (2026-10-28 evening):** step 1 done (`e8b738a`, API 39); step 2
done for reading the choice and naming the selected line (`7f1b9d9`,
`8d128d1`, API 40; the A540 sends 'None', so FP-managed selection is its normal
case), the refusal of an unattached line still open; step 3 done (`f5d150a`,
`9a13f74`, API 41). Also: the full call information to Part 3 handsets on our
rings, and CS call connect after they answer (`2c76ace`). On-air checks:
`bench-next.md` tests 8-10.

**Progress (2026-10-29):** step 4 done in the bridge rather than in the
daemon: a VoIP profile may be the line of several handsets, and the
baresip module sends one call-in per handset; the first to answer takes
the call and the others are released (`07a57dc`). A call-in for a handset
while another is in a call is rung on a side link, up to the chip's three
calls. The line a handset picks for an outgoing call is used. Not yet on
air.

Steps 1-3 are desk work; 4 needs two handsets and a bench run; 5-6 need the
A540 (the only Part 3 handset on the bench) plus a second caller, which before
the bridge exists means an internal call from another handset.

## Open questions

- **Answered** `[CR A-0128]`: a34 (multiple lines) is 0 in our broadcast and
  in stock's, which offers two lines all the same; no host mail sets it
  directly (the chip reports the block in `0xF602`, stub-api.md §2.1).
  §7.4.7.1.0 makes a34 mandatory once the FP offers multiple lines (N.14):
  keep it at 0, as stock, while the daemon serves one line, and find the
  mail that changes byte 3 before the pool offers two. **Decided by the
  maintainer (2026-10-28):** keep it as stock for now; finding the mail
  means either one more room question (which chip code builds byte 3 of
  the Qh-12 message) or an experiment stepping the bring-up mails
  (`0x0169`, `0x0157`) with the radio on and reading `0xF602` back. Not
  before the second line exists.
- Which second-call behaviour the Part 1 Gigasets show in practice when a call
  arrives during a call: nothing (our plan: busy toward SIP), or an in-band
  tone on the active U-plane (A2 K6, a stock capture would tell).

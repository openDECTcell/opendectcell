# Spec sweep, 2026-10-28: what we miss, and what we do out of spec

Asked for by the maintainer after the Wednesday bench, because twice a bench
pass hid a real error (the transaction value of an incoming call, and the
ringing procedure). Two questions:

- **(A) Missing:** which features the standards require of a base (FP) that we
  do not have.
- **(B) Out of spec:** which things we do differently from the standard and
  keep only because a bench test passed.

## Method

Three independent read-only reviews ran in parallel, each against the code at
`1496d6e`, the specs in `docs/specs/` and our earlier audits
(`gap-conformance.md`, `spec-conformance.md`):

| review | standard | full tables |
|---|---|---|
| GAP | EN 300 444 V2.5.1: FP feature tables (N.1-N.36), §8 procedures, general rules | `spec-sweep-2026-10-28/A1-gap.md` |
| CAT-iq | TS 102 527-1 V1.5.1, TS 102 527-3 V1.7.1: FP features, §7 procedures, the MVP | `spec-sweep-2026-10-28/A2-catiq.md` |
| Procedures | EN 300 175-5 V2.9.1 (NWK) and the parts of EN 300 175-4 V2.9.1 our host runs (LAPC, Lc) | `spec-sweep-2026-10-28/B-procedures.md` |

The appendix tables are the reviewers' output, kept as written. **This page is
the checked summary**: every item below marked ✔ was checked by hand against
the code and the clause; items without the mark are the reviewers' reading,
not yet checked. Two findings were found independently by two reviews (C1/F-1
and M1/F-5); both were fixed first.

**Not covered:** the MAC and DLC parts the chip implements (marked N/A in the
tables); EN 300 175-6 (identities) and EN 300 175-7 (security), which we do not
hold, so the DCK storage meaning and the TPUI layout rest on -5 and GAP only;
list-access content beyond what the CAT-iq review read.

## Fixed the same day

| finding | clause | what was wrong | commit |
|---|---|---|---|
| ✔ C1 / F-1 | EN 300 175-5 §9.5.1 | after a release the call stayed in RELEASED, so the next call on a link that stayed up was ignored (same transaction value) or refused "insufficient resources" (another value); old digits were also kept | `ec2c96a` (API 29) |
| ✔ M1 / F-5 | EN 300 444 table 43; EN 300 175-5 figure 28 | <<AUTH-TYPE>> stored the call's DCK as cipher key 0 (`0x10`) while our CIPHER-REQUEST names key 8 (`0x98`); now `0x18`. A second candidate cause for bench test 17, beside the DCK never reaching the chip | `dc4cd39` |
| ✔ C4 / F-9 | EN 300 444 §8.3.2.3; EN 300 175-5 §9.3.1.5 | the overlap timer's expiry sent {CC-RELEASE-COM} (abnormal); now {CC-RELEASE} "timer expiry" and release pending. The test had pinned the deviation | `be1dd06` (API 30) |
| ✔ F-10 | EN 300 444 table 23 | an internal call's SETUP carried <<SIGNAL>> 41H (external); now 40H. Stock does the same (A-StockTrace-1). Untested on air | `5c25e3d` |
| ✔ | EN 300 444 §8.19; TS 102 527-3 §7.4.10.6.2 | the internal-call key 0x17 (the A540's way) | `6135cef` (API 28), bench test 19 |
| ✔ | TS 102 527-3 §7.4.11 | `lineid=` must name the line list's line | `c1e2a9c` |

## The answer to "all three channels busy"

| case | the standard | ours |
|---|---|---|
| A handset places a call and we have no channel | ✔ EN 300 444 §8.2.2.3, EN 300 175-5 §9.3.1.2 and §17.3.3: abnormal release, {CC-RELEASE-COM} "insufficient resources" (32H) | conformant (`on_setup`, `dect_cc_set_busy`) |
| A call arrives for a handset already in a call | TS 102 527-3 §7.4.3: **call waiting** on the existing link: {CC-INFO} with line, call id and "CS call setup", then <<SIGNAL>> 07H, caller number and name. No new SETUP, no extra audio channel. A GAP handset would refuse a SETUP with 32H ("user busy" is 14H) | missing. Worse, `ring_tick` releases a live link before paging, without a busy check: a ring during a call would end that call (F-2, LCE1) |
| A call arrives and no channel is free | TS 102 527-3: not offered to idle handsets; for the SIP side "busy" | missing (no SIP side yet) |

## Open findings, by priority

### 1. Blockers for the MVP (two SIP lines per handset, calls in and out, caller ID)

| # | finding | clause | action |
|---|---|---|---|
| 1 | ✔ **Call id is 1 on every link**, so two parallel calls (bench test 18) already share one | TS 102 527-3 §7.4.6 | allocate call ids per call, system-wide |
| 2 | ✔ **<<TERMINAL-CAPABILITY>> is never parsed**, so a GAP, Part 1 and Part 3 handset look alike. "A Part 3 FP shall not send any call status to GAP or NG Part 1 devices" (527-3 line 8579 of our text), and we send it to all. Prerequisite for line selection, UTF-8 names, the internal call class for GAP handsets. Do not switch call status off before this exists: the A540's timer needs it (test 15) | TS 102 527-1 §7.3.7; 527-3 §7.4.6.4.1, §7.4.9.1 | parse at access rights and location registration; store with the subscription; feed the handset profile |
| 3 | The handset's **line choice is never read**, and the line list holds one line | 527-3 §7.4.7 | line table (the pool), attached-handset bitmaps; read the PP's <<CALL-INFORMATION>> line id |
| 4 | **An incoming external call is not CAT-iq-shaped**: line id, line type, call id and "CS call setup" in the SETUP; "CS call connect" after CONNECT-ACK | 527-3 §7.4.5.3, §7.4.6 | build it; but the A540 rings silently with any line id (bench 21c: line id only, no call id or status) — run the pre-committed sweep below first |
| 5 | **Incoming calls ring one handset**; a line attached to several must ring all idle attached handsets and release the others on answer | 527-3 §7.4.3.5.4 | page all attached idle handsets, one transaction each |
| 6 | **Call waiting / parallel calls** (mandatory): one call per link today | 527-3 §7.4.3 | per-handset call table on one CC transaction, {CC-INFO} builder, `0x1C` codes |
| 7 | **Dialling from a list** (`1C 15` + number in the list-access call) hangs in PROCEEDING; only 0x17 is handled | 527-3 §7.4.10.6.2 | treat it as dialled digits, like 0x17 |
| 8 | No **pseudo call waiting** into an open list-access session: an incoming ring waits | 527-3 §7.4.10.6.3 | implement before serving call lists and contacts (more sessions) |
| 9 | ✔ **A ring releases a live link** before paging, without checking it (F-2, LCE1). Rests on A-0052/A-0053 and runs vF415/vF423: needed for audio, never shown harmless | EN 300 175-5 §14.2 | release only an NWK-idle link; for a busy handset, call waiting (6) |
| 10 | **CNIP shown on no handset**; we only use the SETUP, and drop a number or name that does not fit, although §8.41/8.42 require a {CC-INFO} then | EN 300 444 §8.41, §8.42; 527-3 §7.4.5 | send CLIP/CNIP in {CC-INFO} too; Q-0064 asks what stock sends |

### 2. Conformance fixes (small, testable on the PC)

| # | finding | clause | action |
|---|---|---|---|
| 11 | **Re-presentation on any 32H** in CALL PRESENT, not only after a detected collision; the rule came from the corpus the TI bug voided (C6) | EN 300 175-5 §9.5.2.3 | gate it on a crossing SETUP we released |
| 12 | **LAPC retransmits for ever**: no N250 (3) (L1) | EN 300 175-4 §9.2.3.6 | count, then report the link down |
| 13 | **The base never releases an idle link** and has no <LCE.01/.02> (F-4, LCE2) | EN 300 444 §8.37, §8.39; EN 300 175-5 §14.2 | NWK link release when CC and MM are idle; ask the room which mail does it |
| 14 | ✔ **LOCATE-ACCEPT echoes the handset's old location area**, and omits it when the handset sent none; mandatory (M7, F-8). Stock's bytes match ours (A-0117) only because every sample carried one | EN 300 175-5 table 54; EN 300 444 table 45 | always send our own LAL |
| 15 | Basic service 0x88 (wideband) in every incoming SETUP; GAP table 21 allows 0 only (F-6) | EN 300 444 table 21; 527-1 §7.1 | 0x80 for a handset that offered no G.722 (needs 2) |
| 16 | U-plane bound after CONNECT by default (`bindfirst` off) (F-7, C10) | EN 300 444 §8.6 | make `bindfirst` the default after one bench comparison |
| 17 | Key allocation failure answered with ACCESS-RIGHTS-REJECT, not AUTHENTICATION-REJECT; no <MM_key.1>; the UAK is committed with no "unconfirmed" state (M2-M4) | EN 300 444 §8.32 | fix to spec, re-run bench test 9 |
| 18 | After a failed TPUI assignment we keep paging with it (LCE8, M9) | EN 300 444 §8.28.2.3/4 | page with the default TPUI after a failure |
| 19 | A PAGE-RESPONSE without PORTABLE-IDENTITY is ignored; §17.6.5 wants PAGE-REJECT; plus small validation gaps (SETUP with F=1, TV 7, no BASIC-SERVICE, IWU-INFO TI, class B SABM, V(S) on NLF) | EN 300 175-5 §17; 175-4 §9.2.3.8 | a batch, one unit test each |
| 20 | No F<CC.02> for our own release; a release collision answered with RELEASE-COM | EN 300 175-5 §9.5.1, §9.5.3 | add the timer; follow §9.5.3 |

### 3. Features to plan (not defects)

Mandatory in 527-3 for a Part 3 base and missing: lists Missed calls, Incoming
accepted, All calls, Contacts, DECT system settings; Line settings fields 08 and
0B-0D; search, edit and save where mandatory; <<EVENTS-NOTIFICATION>> (missed
call, list change, voicemail); tones (ringback, busy, call waiting); codec change
({CC-SERVICE-CHANGE}); handset-initiated date and time; FP-initiated access
rights termination; internal-call CLIP/CNIP (optional for GAP, mandatory for
527-3). **A decision for the maintainer:** 527-3 makes user-defined contacts
editable, so a read-only vCard import does not meet it: write-back, a local
overlay store, or a recorded deviation.

## Out of spec but bench-passed

From the procedures review, ranked by risk. ✔ fixed today: M1, C4.

| rank | deviation | rests on | test that would discriminate |
|---|---|---|---|
| 1 | C6: re-present on any 32H (finding 11) | 2026-10-24 corpus, later explained by the TI bug | unit test: 32H without a crossing SETUP must end the call |
| 2 | LCE1: release a live link before paging (finding 9) | A-0052/A-0053, vF415/vF423 | ring while the A540's list-access call is open: is it torn down? |
| 3 | C10: CONNECT before the U-plane (finding 16) | bench 12e (timer only) | a tone at t=0 after CONNECT, with and without `bindfirst`, on a capture |
| 4 | M2: ACCESS-RIGHTS-REJECT after a failed key allocation (finding 17) | bench 9, test 20 | fix, re-run test 9 |
| 5 | M7: location area echoed or omitted (finding 14) | A-0117, 239 accepts that all carried one | unit test: no LA in, LA out |
| 6 | L10: LSIG learned from an unchecked first frame | clean frames only | unit test with a corrupted first frame |
| 7 | L2: RR/UA mirror the P bit on a class A link | the handset ignores it | fix to spec |

Spec-correct after all, only the comment cites wrongly: no CONNECT-ACK wait on a
handset's call (EN 300 175-5 A.1 note on <CC.05>).

## Next bench tests, outcomes written in advance

**S1, cipher (test 17 again, after `dc4cd39`):** the same ciphered call, key
number now 8, chip path unchanged.

| shows | meaning |
|---|---|
| valid LAPC frames after CIPHER-REQUEST, the call holds | the key number was the whole fault; the chip ciphers by itself |
| link released ~200 ms after CIPHER-REQUEST, as before | the key number was not it (or not alone); the chip still needs the DCK (Q-0065) |
| {CIPHER-REJECT} from the handset | the handset checks the key number and now finds no key 8: our AUTH-TYPE storage is still wrong; needs EN 300 175-7 |

**S2, the A540's silent ring with a line id (before finding 4):** one ring each,
<<SIGNAL>> 41H, the number in all: (a) line id only (21c again, control);
(b) line id + line type 01; (c) + call id; (d) + "CS call setup", the standard's
full shape (TS 102 527-3 figure 42).

| shows | meaning |
|---|---|
| silent in a-c, aloud in d | the A540 wants the complete call information; build finding 4 that way |
| silent in all | the A540 ignores CAT-iq call information for ringing; a device-profile quirk ("no line id") |
| aloud from b or c on | the missing element is that one |

**S3, caller name in {CC-INFO}:** CLIP and CNIP sent again in a {CC-INFO} after
ALERTING (EN 300 444 §8.41/8.42), on the C430A.

| shows | meaning |
|---|---|
| the name appears | placement: the name must follow in {CC-INFO} |
| still the number only | content: wait for Q-0064 |

**S4, internal ring pattern:** an internal call after `5c25e3d`; the target should
ring with its internal pattern (a different melody or rhythm from an external
call). If it rings as before, the handset ignores the signal value; nothing to fix.

**S5, a second call on one link (after `ec2c96a`):** call 123 from the A540, hang
up, and redial at once, before the link drops. Before the fix the redial was
ignored or refused; it should now connect.

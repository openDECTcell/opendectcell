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
| ✔ finding 7 | TS 102 527-3 table 17, §7.4.10.6.2 | a number dialled from a list (`1C 15` + number on the list-access call) hung in PROCEEDING; now an external call, routed at once | `a6959c2` (API 32) |
| ✔ finding 12 | EN 300 175-4 §9.2.3.6, A.3.1 | LAPC retransmitted for ever; now DISC after N250 = 3, logged loudly (no saved run counts retransmissions, so a false release must show) | `2636eae`, `bdb38a3` (API 33) |
| ✔ finding 2 | TS 102 527-3 §7.4.9.1, §7.4.6.4.1 | <<TERMINAL-CAPABILITY>> parsed and kept; call status only to NG-DECT Part 3 handsets, decided per IPUI before CC answers the SETUP. The bench logs decode as A540 Part 3 (PI_7 `0x16`), C430A and AS690H Part 1 (`0x12`), which is exactly whom stock sends call status (A-0125) | `6f8e112` (API 34), `44714e8` (API 35), `b9f8f8d` |
| ✔ | A-0126 | the call key to the chip (`0x0208`) before CIPHER-REQUEST, stock's layout | `0628a67` |
| ✔ | EN 300 444 §8.41/8.42 | `dect_cc_info()`, a {CC-INFO} with given IEs (API 31); `-a infoie=` for S3 | `b0b3a82`, `599af45` |
| ✔ finding 17 | EN 300 175-5 §13.6; EN 300 444 §8.32.2.4 | a wrong RES1 now gets {AUTHENTICATION-REJECT}, then {ACCESS-RIGHTS-REJECT} as before | `1c8469f` (API 36) |
| ✔ C9 (finding 19) | EN 300 175-5 §6.3.2.1, §17.6.1 | a {CC-SETUP} without <<BASIC-SERVICE>> gets {CC-RELEASE-COM} "mandatory IE missing" | `bd7ede5` (API 37) |
| ✔ finding 14 | EN 300 175-5 table 54; A-0127 | {LOCATE-ACCEPT} names our own area, `07 01 64` (class A, LAL 36), not the handset's echo | `0c6e98e` (API 38) |
| ✔ finding 13, part | EN 300 175-5 §14.2.7; A-0127 | release the link after its last call or list session: `0x0202`, a DISC first after list access; default since bench test 7 passed | `96b54e4`, `f93e86b` |
| ✔ finding 15 | EN 300 444 table 21; TS 102 527-1 §7.3.6 | basic speech `0x80` to a pure GAP handset on our ring and internal calls | `e750f44` |
| ✔ finding 20 | EN 300 175-5 §9.5.1, A.1 | <CC.02>: an unanswered {CC-RELEASE} completed by our {CC-RELEASE-COM} after 36 s | `faa6d77` (API 45) |
| ✔ E5 (finding 19) | EN 300 175-5 §17.6.5, table 65 | an identity-less page response gets {LCE-PAGE-REJECT} when the link knows the IPUI | `540ba3d` (API 46) |
| ✔ A2 K8 | TS 102 527-3 §7.4.8.3 | busy system notification for a Part 3 handset: busy tone, CS call disconnecting "system busy", then release | `9f3e5df` (API 43) |
| ✔ E2 rest | TS 102 527-3 §7.4.5.2.1 | a call on a line the handset is not attached to is released ("incompatible service", our choice) | `c1b8816` (API 44) |

**`[CR A-0127]` on call waiting and calls during list access:** neither is in
the stock captures (every captured call ended before the next began). The room
gives TS 102 527-3's behaviour, labelled spec-derived: for a Part 3 handset in a
call, {CC-INFO} with <<CALL-INFORMATION>> for a second call id and "CS call
setup", plus <<SIGNAL>>; Part 1 handsets (C430A, AS690H) have no DECT-signalled
second call. During a list session: keep the session, offer the call on the same
link in {CC-INFO} ("pseudo call waiting", §7.4.10.6.3). A capture can be made if
needed.

**Decided, not changed:**
- **Finding 11 (re-present on any 32H) stays.** EN 300 175-5 §9.5.2.3's FT rule is "If the FT receives a {CC-RELEASE-COM} releasing the incoming call, the FT may re-present", and the PT rule applies from CALL INITIATED, before its own SETUP is on the air: the handset can refuse our call for a call we never see, and then waits P<CC.06> for the re-presentation. Gating on a crossing SETUP we saw would break that case. Bounded at two re-presentations.
- **Finding 1 (call ids) waits for the parallel-call work.** Call ids must be unique system-wide (TS 102 527-3 §7.4.6.1) and the first one goes out in the early SETUP-ACK, before CC has the SETUP, so the allocator belongs with dect_link's SETUP hook (the one added for finding 2), the daemon's link table and the internal-call legs together. Today each handset sees one call, so a shared id 1 is a spec deviation without a visible effect until call waiting exists.

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

## Clean-room answers received the same afternoon

**`[CR A-0125]` caller ID and call information (observed, stock 07.56):**
- Stock's external SETUP to the **A540** carries `7e 0c 00 00 82 00 05 81 01 00 81 02 01 81`: line 2 (the incoming line), line type "significant delay" (VoIP), call id 1, **CS call setup**. Never a bare line identifier. The **AS690H and C430A get no <<CALL-INFORMATION>>** (consistent with finding 2: no call status to GAP handsets), the C4 a codec list instead. Internal SETUPs to the A540: `7e 06 01 00 81 02 01 81`.
- Call status sequence to the A540: CS call setup in SETUP; the A540 echoes call id 1 in CONNECT; CS call connect in a {CC-INFO} after CONNECT-ACK; CS disconnecting, then CS idle, before release.
- The caller name goes **twice**: <<CALLING-PARTY-NAME>> in the SETUP with octet 3 = `00` (allowed, DECT alphabet, not screened), and **as text in a {CC-INFO} <<MULTI-DISPLAY>> (0x28)** ~0.8-1.3 s after the SETUP, once the handset alerts. The text is framed by display control octets (vendor framing, kept in the answer, not here); the octet before the text is its length. With no name, the same framing carries the number.
- On the AS690H and C430A stock **draws the call timer itself**: a {CC-INFO} MULTI-DISPLAY with `hh:mm:ss` about once a second after CONNECT-ACK.
- A-StockTrace-1 rows 7-9 (single-octet DISPLAY, escape element) were a decoder overrun; stock's SETUP ends after 0x7E or 0x6D.

**`[CR A-0126]` ciphering:** see `stub-api.md` §2.1, "Stock call layout". Stock
loads the call key with `0x0208` before {CIPHER-REQUEST}; we never do by
default. That, not the key number, is the expected cause of test 17's drop.

## Next bench tests, outcomes written in advance

**S1, cipher (test 17 again, after the A-0126 `0x0208`):** the same ciphered
call, the chip now given the key in stock's layout before CIPHER-REQUEST, key
number 8 (`dc4cd39`). Two runs, the key bytes reversed (`[CR A-0028]`'s
reading, the default) and in plain order (`-k plain`).

| shows | meaning |
|---|---|
| `0x0209 <link>` from the chip, valid LAPC frames after CIPHER-REQUEST, the call holds | ciphering works; that byte order is the right one |
| `0x0209` but the link still drops ~200 ms later | the chip took the mail but the key is wrong: try the other byte order |
| no `0x0209`, drop as before | the chip rejected or ignored the mail: compare our mail with A-0126 byte by byte before anything else |
| {CIPHER-REJECT} from the handset | the handset finds no key 8: the AUTH-TYPE storage is still wrong; needs EN 300 175-7 |

**S2, the A540's silent ring with a line id:** two rings, <<SIGNAL>> 41H and the
number in both: (a) line id only (21c again, control), (b) stock's full form
(`lineid=1,linetype=1,callid=1,callstatus=1`, which encodes to
`7e 0c 00 00 81 00 05 81 01 00 81 02 01 81`, checked). A-0125 predicts (b) rings
aloud.

| shows | meaning |
|---|---|
| silent in a, aloud in b | the A540 wants the complete call information; build finding 4 that way, for Part 3 handsets only |
| silent in both | the prediction is wrong; a device-profile quirk ("no line id") until explained |
| aloud in both | 21c's silence had another cause: re-read that log |

**S3, caller name in {CC-INFO} <<MULTI-DISPLAY>>:** on the C430A and AS690H,
with the name in the SETUP (octet 3 `00`) as before, then a {CC-INFO} after
ALERTING carrying <<MULTI-DISPLAY>>: (a) the name as plain text; (b) the name in
stock's framing from A-0125 §5. Both through a lab option that takes the IE
bytes, so no vendor framing goes into the library.

| shows | meaning |
|---|---|
| the name in a | the Gigasets take the name from MULTI-DISPLAY; plain text is enough |
| the name in b only | the framing is needed: a Gigaset manufacturer-profile entry |
| the name in neither | not the display path either; keep the logs, ask the room what else differs |

**S4, internal ring pattern:** an internal call after `5c25e3d`; the target should
ring with its internal pattern (a different melody or rhythm from an external
call). If it rings as before, the handset ignores the signal value; nothing to fix.

**S5, a second call on one link (after `ec2c96a`):** call 123 from the A540, hang
up, and redial at once, before the link drops. Before the fix the redial was
ignored or refused; it should now connect.

## Bench card for S1-S5 (commands)

Box at `599af45` or later (API 31; `md5sum /tmp/dect-stubd` against
`build/dect-stubd.arm`), radio parked. `ringone.sh` runs from `work/7520/lab`
and always passes `-C`, so the cipher runs start the daemon directly. Wait for
each run to end and park before the next. Every log name is unique.

| test | command | handset | what to do |
|---|---|---|---|
| S1a | `ssh root@192.168.1.1 '/tmp/dect-stubd -g -P 0000 -s /etc/opendectcell-sub.a540 -s /etc/opendectcell-sub.c430a -s /etc/opendectcell-sub.as690h -p -t 120 -a mask=0x0002,codec=3,status=8000 > /tmp/vS1a-$(date +%H%M%S).log 2>&1'` | A540 | dial 123, hold 20 s; look for `cipher: call key to the chip` and `rx ... prim 0209` |
| S1b | the same with `-k plain`, log `vS1b-...` | A540 | as S1a |
| S2a | `EXTRA=clip=0301234567,lineid=1 HOLD=1 ./ringone.sh a540` | A540 | do not answer: aloud or silent? |
| S2b | `EXTRA=clip=0301234567,lineid=1,linetype=1,callid=1,callstatus=1 HOLD=1 ./ringone.sh a540` | A540 | as S2a |
| S3a | `EXTRA=clip=0301234567,cnip=Wednesday,cnipalpha=0,infoie=28095765646e6573646179 HOLD=1 ./ringone.sh c430a` | C430A | do not answer: is "Wednesday" shown? |
| S3b | `EXTRA=clip=0301234567,cnip=Wednesday,cnipalpha=0,infoie=2811020a1a910202095765646e65736461790d HOLD=1 ./ringone.sh c430a` | C430A | as S3a (stock's framing, A-0125 §5) |
| S4 | `ssh root@192.168.1.1 '/tmp/dect-stubd -g -P 0000 -s /etc/opendectcell-sub.a540 -s /etc/opendectcell-sub.c430a -s /etc/opendectcell-sub.as690h -p -C -t 180 -a mask=0x0002,codec=3,status=8000,twocalls > /tmp/vS4-$(date +%H%M%S).log 2>&1'` | A540 → C430A | INT, DECT 2: does the C430A ring with its internal pattern? |
| S5 | the S4 command, log `vS5-...` | A540 | dial 123, hang up, redial at once: does the second call connect? |

| S6 | `MAXSECS=90 HOLD=20 ATTACH=600000 EXTRA=playpcm=/tmp/wb16k.pcm ./ringone.sh c430a`, then the same for `a540` | C430A, A540 | dial 123: does each still show its call timer? |

**S6 outcomes (call status only to Part 3, `b9f8f8d`):** the log should show
`cap: /etc/opendectcell-sub.c430a reports NG-DECT Part 1 -- no call status to it` and
a CONNECT `83 07 7c 04 90 03 00 81` with no `7e`; for the A540 `NG-DECT Part 3`
and the CONNECT with `7e 06 01 00 81 02 01 85` as in test 15.

| shows | meaning |
|---|---|
| both timers as in test 15 | the C430A's timer is its own (as A-0125 suggests); the gate costs nothing |
| no timer on the C430A any more | it did key on our call status: keep the standard, and note it for the Gigaset profile (stock draws the timer as display text instead) |
| the A540 lost its timer | the gate misfired for a Part 3 handset: read the `cap:` line and the CONNECT bytes |

S3 repeats on the AS690H if the C430A shows the name. The `infoie` bytes are
`28 <len>` plus the text, or stock's framing around it; computed, not typed.

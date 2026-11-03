> **Reviewer output, kept as written** (2026-10-28, read-only review of `1496d6e`). Line numbers refer to that commit; several findings were fixed the same day. Which items were checked by hand, and the priorities, are in `../spec-sweep-2026-10-28.md`.

# B: NWK and C-plane procedures against EN 300 175-5 / -4 V2.9.1 (and EN 300 444 V2.5.1)

Read-only sweep, 2026-10-28. Tree: `.` (working copy at the time of reading).
Spec texts: the `pdftotext -layout` extracts in the scratchpad `specs/` directory. Clauses below are V2.9.1 for EN 300 175-4/-5 and V2.5.1 for EN 300 444 (GAP).

**How sure each row is** (in the "our code" column):
- **[V]** I read the code and confirmed it.
- **[P]** I also compiled the library sources into a scratch program outside the repo (`scratchpad/sweep/probe/p1.c`, `p2.c`) and watched the behaviour.
- **[A]** Assumed or inferred; not confirmed.

"Justified by" says what the code or its comment rests on: *spec*, *bench* (one of our runs), *capture* (a stock trace or `[CR A-…]`), or *nothing*.

**What the two missing parts would settle.** EN 300 175-7 (security) is not available. Without it I cannot check the DSAA key derivations, the meaning of "store DCK under number n", what the FT must do when authentication fails, or the details of cipher switching (§6.5.3). Rows that need it are marked "‑7 needed". EN 300 175-6 (identities) is not available either. That leaves the TPUI value layout, PMID-from-TPUI and the LSIG derivation unchecked; those rows are marked "‑6 needed".

**Prior audits.** I read `docs/spec-conformance.md` and `docs/gap-conformance.md` and did not redo what they settled. I re-checked the "fixed" claims against the current code in section 0.

---

## 0. Prior "fixed" claims, re-checked against the current code

| ID | claim (source) | current code | holds? | note |
|---|---|---|---|---|
| R1 | TI value is the lowest free one, 0 (GAP §6.9.2; `dd5a630`) | `dect-stubd.c:3907-3909`, `4795`, `5377`; `dect_cc.c:248-250` [V] | **yes** | The value is right. Its *lifetime* is the problem: see C1. |
| R2 | CC transaction matching (an external review #3, `0ac7e38`) | `dect_cc.c:914-919` (`our_transaction`), used by every case [V] | **yes, with a new side effect** | `our_transaction()` counts the RELEASED state as a live call. See C1. |
| R3 | `<<SIGNAL>>` 0x41 by default (GAP §8.14) | `dect_cc.c:26-27`; `dect-stubd.c:7773-7774` [V] | yes | Internal calls also use 0x41: see C17. |
| R4 | `F<CC.03>` 20 s, release "timer expiry" | `dect_cc.c:835-848` [V] | yes | |
| R5 | `<DL.04>` 2 s | `dect_lapc.c:136` [V] | yes | Nothing limits how often a frame is resent: see L1. |
| R6 | Only `{LCE-PAGE-RESPONSE}` with `<<PORTABLE-IDENTITY>>` counts (`02b9039`) | `dect_link.c:272-280` [V] | **only half** | A response *without* the IE is ignored. §17.6.5 says it must get `{LCE-PAGE-REJECT}`, and the code comment cites §17.6.5 for the opposite. See E5. |
| R7 | LOCATE-REQUEST identity check (`7e7e5f4`) | `dect_mm.c:499-502` [V] | yes | |
| R8 | Unsolicited TPUI ACK (`edaaeb3`) | `dect_mm.c:755-758` [V] | yes | |
| R9 | LAPC SAPI check (`2fd24ae`) | `dect_lapc.c:302-305` [V] | yes | |
| R10 | Over-long reassembly (`e5bc10b`) | `dect_lapc.c:368-384` [V] | yes | |
| R11 | Class B DISC on a class A link (an external review #4) | `dect_lapc.c:210-221`, still DISC with P=1 [V] | **still open**, as recorded | Re-confirmed against §9.2.3.7 (release of class A is not a DISC exchange) and §9.2.1.1 (P/F = 0). |
| R12 | §6.9.4 default branch had an inverted flag | `dect_cc.c:1115-1131` [V] | yes | |
| R13 | No CONNECT-ACK expected for a PT-originated call (bench 12b) | `dect_cc.c:717-722` [V] | yes, and **the spec agrees** | EN 300 175-5 A.1 under `<CC.05>`: "in the FP timer CC.05 only applies in case of external handover". The comment cites only the bench; it should cite A.1. |

---

## 1. LCE: link establishment, paging, page response, link release (§14.2, §8.2; GAP §8.35-8.38)

| ID | clause | spec requires | our code (file:line) | deviates? | justified by | risk and recommended action |
|---|---|---|---|---|---|---|
| LCE1 | 175-5 §14.2.3; GAP Table 66 | Page only when **no suitable link exists**. While a page is pending, queue the messages. | `dect-stubd.c:4032-4041` [V]: if a link is up, `dect_lapc_disconnect()` is called unconditionally, then we page | **yes** | capture `[CR A-0052/A-0053]` (stock releases before paging) + bench (7/7 split, vF415/vF423) | **Medium.** It is a chip constraint, not a protocol choice. Nothing checks that the PT is idle first, so an LiA/MM transaction in progress is cut (§17.9: the PT clears every call not ACTIVE). **Action:** release only when no CC/MM transaction is open on the link, and otherwise wait. Write this down as a device or chip deviation from §14.2.3, not as standard behaviour. |
| LCE2 | §14.2.1, §14.2.7.1, GAP §8.37 | When no higher entity uses the link (NLR), the FT LCE **shall release** it: `DL_RELEASE-req` "normal", then `<LCE.01>` 5 s, then "abnormal". | No NLR notion. After a call the FT never releases; only the PT, the chip, or the pre-page DISC does. [V] (`grep dect_lapc_disconnect`: only `dect-stubd.c:4037`, `5793`) | **yes** | nothing (`gap-conformance.md` lists N.19 as "done") | **Medium.** It works because the handsets release. **Action:** add an NLR step to CC and MM (normal release after the last transaction, partial release after a collision, §14.2.7.2) and an FT link-release path. It needs the class A release mail from the room (R11). |
| LCE3 | §14.2.2 `<LCE.05>` 5 s | If `DL_ESTABLISH-ind` arrives without an SDU, start `<LCE.05>`. On expiry, with no higher entity, release (abnormal). | not implemented [V] | yes | nothing | Low. It belongs with LCE2. |
| LCE4 | §14.2.7.2 `<LCE.02>` 3-10 s; §9.5.2.3 | After a collision release, NLR "partial release" keeps the link for a follow-on call. | none [V] | yes | nothing | Low on its own, but it is the case that hits C1. |
| LCE5 | §14.2.3, GAP §8.35.1.1 `<LCE.03>` 3 s, N300 (recommended 3) | Resubmit the page every 3 s, at most N300 times. | `dect-stubd.c:3727-3732`: 3 s × 20 [V] | no (N300 is "application specific") | spec (3 s, which the comment calls "the stock guard") + bench F1 (20 retries) | Low. Fix the comment: 3 s is `<LCE.03>`. |
| LCE6 | §14.2.3; GAP §8.35.2.1, Table 68 | On an identity mismatch: `{LCE-PAGE-REJECT}` with the same TV and the full IPUI, then release the link "normal". | `dect_link.c:131-148`, `dect-stubd.c:5781-5801` [V] | no (release is the open R11 DISC) | spec | Unit-tested only (as recorded). |
| LCE7 | §17.3.2.6 | A `{LCE-PAGE-RESPONSE}` with TI flag 1 shall be ignored. | `dect_link.c:274` checks only the message type [V] | yes | nothing | Low. Add `h.ti_flag == 0`. |
| LCE8 | §14.2.3 (a), GAP §8.28.2.4 | Page with the assigned TPUI **if available**. After `<MM_ident.1>` expires the assignment has **failed**, so page with the default individual TPUI. | `dect-stubd.c:3843-3852` still pages with the assigned TPUI and prints a WARNING [V]; `dect_mm.c:784-798` [V] | **yes** | nothing (the comment says "the caller decides") | **Medium-low.** This is the "silent page" failure the comment itself names. **Action:** when `tpui_timed_out` is set, page with the default TPUI. The default TPUI's layout needs EN 300 175-6. |
| LCE9 | §14.2.3 | After a page, send the queued messages **only once a matching page response has arrived**. | `dect-stubd.c:3867-3879` and `3962-3966` send the SETUP at the response deadline on whatever link is up, if it serves the ring target [V] | marginal | bench (E1b, 12d) | Low. Using an existing link for the right PT is allowed (§14.2.3 "no suitable link"). |
| LCE10 | §8.2.4.1; GAP §8.35, Table 66 | LCE header 0 = C-plane only, 4 = voice. Short format only. | chip mail `0x0300`; settled in spec-conformance Gap 5 [A] | settled | spec + capture | Not re-audited (prior audit). |

---

## 2. CC: FT state machine, both directions, timers, release (§9; GAP §8.1-8.19)

| ID | clause | spec requires | our code (file:line) | deviates? | justified by | risk and recommended action |
|---|---|---|---|---|---|---|
| **C1** | §9.5.1 / §9.5.2.2 (enter NULL); §17.3.2.1 "Repeated {CC-SETUP}"; §17.3.3; GAP Table 66 ("follow on call on the same link") | After RELEASE-COM is sent or received, the call is in **NULL**, and its TI no longer "relates to an active call or a call in progress". | `on_release()` `dect_cc.c:792-799`, RELEASE-COM `1112`, `dect_cc_release()` `1149` all end in `DECT_CC_ST_RELEASED`, and it never becomes NULL. `our_transaction()` `917` treats RELEASED as live. In `dect_cc_rx` the SETUP case `932` accepts only NULL. [P] probe p1: after a PT call has been released, a new PT `{CC-SETUP}` on TV 0 is **silently ignored**. On TV 1 it gets `{CC-RELEASE-COM}` 0x32. After our own incoming call, any PT SETUP gets 0x32. | **yes** | nothing. No test covers a second SETUP on one CC entity (`test_transaction_value_is_lowest_free` covers only FP→FP). The bench never ran it: `ringone.sh` ends the run "after the first call's end in any case" (`bench-2026-10-25.md:665`). | **High** whenever a PT keeps the link: partial release, an LiA call straight after a voice call, redialling within `<LCE.02>`. The handset would show "---" until `P<CC.03>`, or be refused with "insufficient resources". The only reset is `main_cc_setup()` on a new connection (`dect-stubd.c:4743`, `5579`). **Action:** enter NULL after release completion, and keep a separate RELEASE PENDING (F-19) only while our own `{CC-RELEASE}` is unanswered. Add tests: PT call, release, PT call on the same entity; FP call, release, PT call. |
| C2 | §9.5.1, A.1 `<CC.02>` 36 s; GAP §8.7.1.2 | Sending `{CC-RELEASE}` starts `F<CC.02>`. On expiry, send `{CC-RELEASE-COM}` and go to NULL. | `dect_cc_release()` `1144-1152` has no timer [V] | yes | nothing | Low on its own, but GAP §8.7.1.2 makes the timer part of the procedure. Fix it together with C1. |
| C3 | §9.5.3; GAP §8.7.2.1, Fig. 23 | `{CC-RELEASE}` received in RELEASE PENDING: do **not** answer; go to NULL. | `on_release()` `794` answers RELEASE-COM in any state, RELEASED included [P] (p1: "pp release crossing" → `835a`) | yes | nothing | Low. Do not reply when in F-19. |
| C4 | §9.3.1.5; **GAP §8.3.2.3** | `F<CC.01>` expiry: **normal** release, `{CC-RELEASE}` "timer expiry", then F-19. | `dect_cc.c:812-821` sends `{CC-RELEASE-COM}` 0x0D (abnormal) and goes to NULL. Its comment reads "GAP 8.3.2.3: the normal release" [P] (p2: `835ae20d`) | **yes** | the comment cites the clause, but the code does the other thing | Low-medium. A PT in T-02 that gets RELEASE-COM is not stuck, but this is a clean GAP fail. Use `dect_cc_release()` with reason 0x0D, after C2. |
| C5 | A.1 `<CC.01>` | 20 s from SETUP-ACK, restarted on every CC-INFO, stopped when F-02 is left. | `dect_cc.c:807-822`, `1064-1065`, `707` [V] | no | spec | |
| **C6** | §9.5.2.3 FT requirements; §17.3.3 | Re-present the incoming call **only after the FT has detected a {CC-SETUP} collision** and the PT released our call. | `dect_cc.c:1094-1111` re-presents on **any** 0x32 RELEASE-COM in CALL PRESENT, up to `DECT_CC_REPRESENT_MAX`=2 times. Nothing checks that a crossing SETUP was seen (`stats.collisions` is not consulted) [P] (p2: collisions=0, SETUP resent) | **yes** | corpus reading ("or was about to … before anything is on the air"; the 25 refusals "with nothing visible"). Those refusals were later explained by the TI-value defect (`spec-conformance.md` §"review of 2026-10-24") | **Medium.** A PT that refuses for a real reason gets the SETUP up to twice more. The premise came from a corpus the TI bug had poisoned. **Action:** re-present only if a crossing PT SETUP was released in this call (C7), or within `P<CC.06>`=1.5 s of one. Otherwise go to NULL. |
| C7 | §9.5.2.3, §17.3.3 | On collision: `{CC-RELEASE-COM}` (1, TV2) "insufficient resources" to the PT's call. Past CALL INITIATED: the same reason, and our call continues. | `dect_cc.c:954-983`, `queue_release_other` `176-197` [V] | no | spec | Correct, except while in the RELEASED state (C1). |
| C8 | §17.3.2.1 "Incorrect {CC-SETUP} TI flag" | A SETUP with F=1 shall be ignored. | `dect_cc.c:931-953` does not test `h.ti_flag` [P] (p2: SETUP with header `83` creates a call) | yes | nothing | Low. Add the check. |
| C9 | §9.3.1.2 call reject; §17.6.1 | A SETUP missing a mandatory IE (`<<PORTABLE-IDENTITY>>`, `<<FIXED-IDENTITY>>`, `<<BASIC-SERVICE>>`, Table 7) gets `{CC-RELEASE-COM}` "mandatory IE missing". | Only a missing PORTABLE-IDENTITY is caught, through the gate (`dect_cc.c:888-898`, always armed by `dect_link.c:332`). A SETUP without BASIC-SERVICE or FIXED-IDENTITY is **accepted and connected** [P] (p1 "empty setup" with the gate off: CALL-PROC and CONNECT) | yes | nothing | Low. Check all three in `on_setup`. |
| C10 | §9.3.1.8 | FT: **confirm the U-plane, then** send CONNECT and enter ACTIVE. | default: CONNECT at once (`dect_cc.c:717-733`), bind afterwards. `bindfirst` is opt-in (`dect-stubd.c:2729-2766`) [V] | **yes** (default) | bench 12e (falsified "bindfirst starts the timer"; the timer later came from A-0122) | Low-medium: possible first-syllable clipping, or a PT connecting its U-plane before the chip has one. **Action:** make bindfirst the default. 12e showed it costs nothing. |
| C11 | §9.3.1.7 | Send `{CC-ALERTING}` only once the U-plane resources are fully installed. | `dect_cc_alert()` `751-759`; the daemon calls it before the bind on the internal and alertms paths [A] | unclear | bench 12e | Low. Check the call sites in the daemon. |
| C12 | §9.3.2.7/9.3.2.8 | Incoming call: CONNECT received, stop `F<CC.03>`, confirm the U-plane, `{CC-CONNECT-ACK}`, ACTIVE. | `dect_cc.c:994-1008`: CONNECT-ACK at once [V] | marginal (U-plane not confirmed first) | bench (calls work) | Low. The PT may hold its U-plane until it gets CONNECT-ACK (§9.3.2.4 NOTE). |
| C13 | §9.3.2.7, A.1 `<CC.04>` | `F<CC.04>` 100 s in CALL RECEIVED is optional. | absent [V] | no (optional) | spec | Ringing never times out in CC; the daemon decides. |
| C14 | GAP §8.7 | The FT may start a normal release in any state except F-00, F-01, F-19. | Refusing an internal call after CALL-PROC (F-03) uses `{CC-RELEASE}` (`dect-stubd.c:5307-5324`) [V] | no | spec | |
| C15 | §9.3.1.2 call reject | Reject = `{CC-RELEASE-COM}`, then NULL. | busy: `dect_cc.c:668-673` 0x32. Stranger: 0x0A or 0x03 [V] | no | spec (§17.3.3 names 0x32) | |
| C16 | §9.3.2.1; Table 7 | Fixed IEs go in table order, variable IEs ascending. | `dect_cc.c:253-329`: 05, 06, E0, E4, 6C, 6D, 7C, 7E; invariant test in `tests/test_cc.c:1014` [V] | no | spec | `emit_before_basic_service` (opt-in, `dect-stubd.c:8085`) puts variable IEs ahead of a fixed IE that comes earlier in the table. §6.3.1 does not plainly forbid that. Unclear; leave it off by default. |
| C17 | GAP §8.14 Table 23 | `<<SIGNAL>>` 40H internal, 41H external. | Internal-call SETUP to the target goes out with `e4 41` (`bench-2026-10-25.md` test 19: "SETUP `e0 98 e4 41`"). The value is the link's default (`dect-stubd.c:4996`) [V] | yes | nothing | Low (wrong ring pattern). Send 0x40 when the basic service class is 9. |
| C18 | TS 102 527-3 §7.4.10.6.1; §9.3.1.6 | LiA call: CALL-PROC only, no CONNECT. | `dect_cc.c:662-667` [V] | no | spec | |
| C19 | GAP §8.19 / TS 102 527-3 §7.4.10.6.2 | INT key 0x17 in the keypad of an LiA call: the FP "shall maintain the call". | `dect_cc.c:541-554` re-classes the call to class 9 [V] | no | spec + bench 19 | It applies to class 2 and class 8 calls. |
| C20 | §9.3.1.5 / GAP §8.3 | Overlap sending: SETUP-ACK, CC-INFO, F-02. | `dect_cc.c:685-692`, `1064-1073` [V] | no | spec + bench 12b | The inter-digit "complete after 3000 ms" is IWU policy in the daemon, which is allowed. |
| C21 | §9.3.1.2 / held SETUP | No FT timer while the F-CC waits for the IWU. The PT runs `P<CC.03>`. | `dect_link.c:34-59` answers a held SETUP with CALL-PROC or SETUP-ACK before the record arrives [V] | no | bench 7/12c | Once CALL-PROC is out, the PT runs only the optional `P<CC.04>`. A fetch that never ends leaves the call in F-03 indefinitely: there is no FT bound. Low. |

---

## 3. MM: location registration, access rights, key allocation, authentication, ciphering, TPUI (§13; GAP §8.22-8.34)

| ID | clause | spec requires | our code (file:line) | deviates? | justified by | risk and recommended action |
|---|---|---|---|---|---|---|
| **M1** | GAP §8.27 Table 43; Table 39; §7.7.4 | To store the DCK, `<<AUTH-TYPE>>` octet 5 is UPC=1 with **cipher key number 8**, i.e. `0x18`. `{CIPHER-REQUEST}` then names DCK **8** (GAP Table 61). | `dect_mm.c:541`: `AUTH_TYPE_CIPHER {0x01, 0x18, 0x10}`. Octet 5 `0x10` means UPC=1 with **cipher key number 0** (related to the active IPUI). `CIPHER_INFO_DSC {0x81, 0x98}` (`542`) then asks for DCK number 8 (related to the IPUI/PARK pair) [V] | **yes** | capture `[CR A-0011 Q3]` (third octet "composed from flags/context … 0x10 in one branch", so the key-number bits were not recovered) + `[CR A-0027]`. The comment says "flags (TBD)". It treats octet 4's `0x18` (UAK #8) as if it were the cipher key number. | **High for ciphering.** We store the DCK as #0 and ask for #8. A conformant PT is entitled to refuse: GAP §8.33, "acceptable if … the cipher key is available". **Test 17 failed exactly at `{CIPHER-REQUEST}`**; the bench put it down to the chip not having the DCK. This is a second, independent candidate, and one run cannot tell them apart. **Action:** octet 5 = `0x18`. Pre-commit a run that changes only this byte. ‑7 needed to confirm the storage semantics. |
| M2 | GAP §8.32.2.4, §8.32.2.3; §17.6.4 | Key allocation: if PT authentication fails (RES1 ≠ XRES1), send **`{AUTHENTICATION-REJECT}`**. A missing IE in the PT's AUTH-REQUEST gets AUTHENTICATION-REJECT "information element error". | `dect_mm.c:675-678` and `685-688` send `{ACCESS-RIGHTS-REJECT}` (reasons 0x60 or 0x10) on the access-rights transaction. The FT-initiated key-allocation transaction is never closed [V] | **yes** | bench 9 (wrong PIN), test 20 (the A540's silent 0000 try was rejected and it retried) | Low-medium. Gigasets accept it, but a strict PT would wait out its `<MM_auth.1>`. **Action:** send AUTHENTICATION-REJECT (flag 0, key-allocation TI), then ACCESS-RIGHTS-REJECT if the access-rights request should also fail. |
| M3 | GAP §8.32.1.1 / A.5 `<MM_key.1>` 10 s; GAP §8.32.2.1 | Start `<MM_key.1>` when KEY-ALLOCATE is sent. On expiry the procedure has failed. | none. `state = DECT_MM_WAIT_AUTH` (`dect_mm.c:186`) lasts as long as the link, and a late AUTH-REQUEST is still accepted (`731`) [V] | yes | nothing | Low. |
| M4 | GAP §8.32 (after Table 58) | KS' is stored as UAK #8 marked **"unconfirmed"**. Keep the AC until the PT has authenticated with the UAK; if the PT replies "authentication key not available", delete the UAK. | `dect_mm.c:701-705`: UAK stored and `subscribed = 1` at once. No unconfirmed state [V] | yes | nothing | Low-medium. A PT whose FT authentication failed (RES2) keeps its AC, while we have overwritten the store. Re-registration recovers it. ‑7 needed. |
| M5 | Table 38; GAP Table 51; §17.6.4 | ACCESS-RIGHTS-ACCEPT: PORTABLE-IDENTITY (IPUI) is **M**. A request without its mandatory PORTABLE-IDENTITY gets ACCESS-RIGHTS-REJECT "information element error". | `dect_mm.c:650-656` runs key allocation without an identity. `304-307` then **omits** PORTABLE-IDENTITY from the accept [V] | yes | nothing | Low (no GAP PT does this). Reject with 0x60. |
| M6 | GAP Table 50 | If the PT says UAK (key type 1) and the FT has only an AC, do not accept. DSAA2 is only for N.36. | `<<AUTH-TYPE>>` in ACCESS-RIGHTS-REQUEST is not parsed. DSAA1 key allocation is always run, even when the PT proposes DSAA2 (`dect_mm.c:29-37`) [V] | unclear | bench (C4 and Gigasets register) | Low. Parse it and log the outcome. ‑7 for DSAA2. |
| **M7** | Table 54 (**M**); §13.4.1; GAP Table 45 ("LAL 0-39, even if default LAL") | LOCATE-ACCEPT **always** carries `<<LOCATION-AREA>>` with **the FT's** location area level. The PT's LOCATE-REQUEST carries its **old** LAL, and only "if the location area has changed" (§13.4.1). | `dect_mm.c:398-400`: LA only if the request had one, **echoing the PT's old LAL** (`530-531`) [V]. In the corpus all 239 LOCATE-ACCEPTs carry `07 01 64` (LAL 36), because every handset sent it | **yes** | capture `[CR A-0117]`: stock sends the same `07 01 64` | Low-medium. The value is right by coincidence. It is the PT's value, not ours, and omitting it is untested. It matters for the multi-cell/roamer work, where LAL decides when a PT re-locates. **Action:** always send our own LAL (a configured constant, 36 today). |
| M8 | §13.4.1, Table 54 note 1; GAP Table 45 | PORTABLE-IDENTITY: TPUI type, 20 bits, "only assigned individual TPUIs". | `dect_mm.c:357-421`: `a0 94 {0x0, 0x11, slot}`; prefix 0x11 "AVM fixed prefix" [V] | unclear | capture `[CR A-0023]` | Low. ‑6 needed to confirm the value is an assigned *individual* TPUI and is unique. |
| M9 | §13.2.2; GAP §8.28.1.2, §8.28.2.3/4 | `<MM_ident.1>` 10 s. Stop it on ACK **or on `{TEMPORARY-IDENTITY-ASSIGN-REJECT}`**. On expiry the assignment has failed: abort and release the transaction. | `dect_mm.c:769-799`: timer present; expiry only sets a flag. TEMPORARY-IDENTITY-ASSIGN-REJECT is not handled (falls into `default`, `764`) [V] | yes (partly) | nothing ("the caller decides") | Low-medium, because the daemon keeps paging with the failed TPUI (LCE8). Handle REJECT like a timeout. The comment cites "§A.2"; the MM timers are in **A.5**. |
| M10 | GAP §8.28 Table 46; §13.2.2 | The TPUI ACK closes the assignment. | `dect_mm.c:742-763` [V] | no | spec | |
| M11 | §13.3.1 / GAP §8.24 Table 39; A.5 `<MM_auth.1>` 10 s | FT authentication of the PT: AUTH-TYPE key type UAK, number 8. Run `<MM_auth.1>`. Failure handling is in ‑7. | `dect_mm.c:545-561`: no timer. A RES mismatch (`597-600`) sets `CIPHER_FAILED` and sends **nothing** [V] | yes (timer); unclear (reject) | nothing | Low. ‑7 needed for what the FT must send on a failed authentication. |
| M12 | GAP §8.33; A.5 `<MM_cipher.1>` 10 s; `DL_ENC_KEY-req` | `{CIPHER-REQUEST}`, then `<MM_cipher.1>` until `{CIPHER-REJECT}` or `DL_ENC_KEY-ind`. The NWK layer **shall** hand the key to the DLC. | `dect_mm.c:564-576`: no timer, no CIPHER-REJECT handling. The key is not loaded into the chip (the `0x0208` path is behind `-k`, as recorded) [V] | yes | bench 17 (fail) | High for ciphering, already known. Fix M1 first, so that test 17 tests only the chip question. |
| M13 | §17.3.1, §7.3 Table 68 | MM TV is "0" only. Any other value means ignore. The F flag places a message in a transaction. | `dect_mm_rx` `711-767` checks neither the TV nor the flag. `mm->ti_value` is overwritten by every ACCESS-RIGHTS or LOCATE request (`723`, `739`) [V] | yes | nothing | Low. Ignore TV≠0. Accept AUTH-REQUEST as the key-allocation answer only when F=1. |
| M14 | GAP §6.9.6 Table 9 | Key allocation only in F-00. | not checked [V] | marginal | nothing | Low. |
| M15 | §17.4.4 / §17.3.2.5 | Unknown or unexpected MM messages are ignored. | `dect_mm.c:764` [V] | no | spec | DETACH, IDENTITY-REPLY, CIPHER-SUGGEST and FT authentication (AUTH-REQUEST from the PT outside key allocation) are therefore ignored. GAP makes these optional for us (gap-conformance.md). A handset's FT authentication then runs to its timeout; noted. |
| M16 | §13.4.1 (MM_LOCATE-res) | No FT timer for an unanswered LOCATE-REQUEST. The PT gives up at `<MM_locate.1>` 20 s. | `dect_mm.c:505-523` defers an unknown identity while it is fetched [V] | no | bench 12c | A fetch longer than 20 s answers a transaction the PT has already abandoned. Cap the fetch below 20 s. |
| M17 | Table 53, GAP Table 56 | KEY-ALLOCATE: ALLOCATION-TYPE 1/8/8, RAND 8, RS 8. | `dect_mm.c:37-39`, `169-188` [V] | no | spec | `0x88` = UAK 8, AC 8, both related to the IPUI/PARK pair. |
| M18 | Table 45 note 2 | In key allocation with DSAA, the AUTH-REPLY RS "should not be included". | `dect_mm.c:191-202` omits it [V] | no | spec | |

---

## 4. CC-INFO and IWU-INFO

| ID | clause | spec requires | our code (file:line) | deviates? | justified by | risk and recommended action |
|---|---|---|---|---|---|---|
| I1 | GAP §8.10, §9.4 | Accept CC-INFO keypad in T-02 and T-10. No reply. | `dect_cc.c:1040-1075` [V] | no | spec | The comment says "F-02 (call present)". F-02 is **OVERLAP SENDING**; CALL PRESENT is F-06. Fix the comment. |
| I2 | §17.3.2.1 | CC-INFO on an unknown TI: RELEASE-COM "unknown TI" (a *should*). | silently ignored [P] (p1: "info unknown TI", nothing sent) | yes ("should") | nothing | Low. Inconsistent with the default branch, which does reply. |
| I3 | §7.3 NOTE 2 of §9.5.2.3 ("all CC messages of a call belong to the same transaction"); §17.3.2.1 | Answer `{CC-IWU-INFO}` on **its** transaction. On an unknown TI, release. | `dect_cc.c:1018-1039` has no `our_transaction()` check. Replies go out on `cc->ti_flag`/`ti_value` (`queue_iwu` `110-111`), even in NULL, i.e. on a stale or default transaction [V] | yes | nothing ("records ride whatever transaction is up") | Low-medium. A list-access session opened on a transaction other than the one CC holds would be answered on the wrong one. Answer with the received TV and the inverted flag, as `queue_release_other` does. |
| I4 | §17.7.2 | IWU-TO-IWU may be truncated and processed. | the whole IE goes to `dect_iwu_handle` [V] | no | spec | |

---

## 5. Message validation (§17) and the codec

| ID | clause | spec requires | our code (file:line) | deviates? | justified by | risk and recommended action |
|---|---|---|---|---|---|---|
| E1 | §17.1 | Unknown or unsupported PD: ignore. | `dect_link.c:344-347` sends every non-LCE, non-CC PD to `dect_mm_rx`, which drops anything that is not MM (`dect_mm.c:718`) [V] | no | spec | CISS, COMS and CLMS are ignored, which is allowed. |
| E2 | §17.2 | Too short for the message type: ignore. | `dect_nwk.c:206-207` [V] | no | spec | |
| E3 | §17.3.1, §7.3 | TV 7 = extension (octet 1a). If extended TVs are not supported, **ignore**. | `dect_nwk_parse` `203-216` takes TV 7 as a value, and octet 2 as the message type [V] | yes | nothing | Low. Return an error for TV 7. |
| E4 | §17.3.2.1 | Unknown TI, any message except SETUP, RELEASE or RELEASE-COM: RELEASE-COM 0x02. RELEASE or RELEASE-COM on an unknown TI: no action. | default branch `dect_cc.c:1115-1131` [V]. ALERTING, CONNECT, CONNECT-ACK, INFO and IWU-INFO on an unknown TI are ignored [P] | partly | spec ("should") | Low. |
| E5 | **§17.6.5** | `{LCE-PAGE-RESPONSE}` missing a mandatory IE or with invalid content: **clearing shall be initiated by `{LCE-PAGE-REJECT}`**. | `dect_link.c:262-264` comment: "a missing one means ignore, §17.6.5". The code ignores it [V] | **yes** | the comment misreads the clause | Low (GAP PTs always include it), but the comment is wrong. Send PAGE-REJECT and release the link. |
| E6 | §17.4.1, GAP §6.9.4 | Unexpected CC message: ignore. Unexpected RELEASE: normal release. Unexpected RELEASE-COM: abnormal release. | `dect_cc.c:1013-1017`, `1076-1114`, `1115-1131` [V] | no | spec | |
| E7 | §17.5.1 | IEs out of order may be processed anyway (NOTE). | `dect_nwk_find_ie` scans the whole message [V] | no | spec | |
| E8 | §17.5.2 | Repeated IEs: use the first. | `dect_nwk.c:281-311` returns the first match [V] | no | spec | |
| E9 | GAP §6.9.4 ("shall"), §17.6.1 | A message other than SETUP or RELEASE with a mandatory IE missing: normal release. | no check in CC [V] | yes | nothing | Low. |
| E10 | §17.8, §17.9 | On data-link failure, clear the calls that are not ACTIVE (ACTIVE ones too on a "normal" release). | handled implicitly: `reg_start()` re-initialises the link on the next `CONN_UP` (`dect-stubd.c:4848`) [A] | marginal | nothing | Low. A CC entity is left stale between `CONN_DOWN` and the next `CONN_UP`. |
| E11 | GAP §6.9.3 | Receive at least 63 octets. | `DECT_LAPC_MSG_MAX` 256 [V] | no | spec | |
| E12 | §6.3.1, §7.6 | Variable IEs ascending. | Checked for every message we send: CC SETUP (invariant test), CONNECT 7C<7E, ACCESS-RIGHTS-ACCEPT 05<06<7C, LOCATE-ACCEPT 05<07<72<7C, KEY-ALLOCATE 0B<0C<0E, AUTH-REQ 0A<0C<0E, PAGE-REJECT 05 [V] | no | spec | |

---

## 6. Transaction value and TI flag rules (§7.3, GAP §6.9.2)

| ID | clause | spec requires | our code (file:line) | deviates? | justified by | risk and recommended action |
|---|---|---|---|---|---|---|
| T1 | GAP §6.9.2 | Lowest free TV for a CC call. | 0 on every path (R1) [V] | no | spec + bench 15/16 | |
| T2 | §7.3 | TV 6 only for connectionless transactions. | CC: `dect_cc_set_next_ti(ti % 7)` allows 6 (tests, `-a ti=`). CISS date/time uses TV 6 (`dect_time.c`) [V] | no (test override only) | spec | |
| T3 | §7.3 Table 68 | MM and LCE: TV 0 only. | sent: echoes the PT's value (0 in practice). Received: not validated (M13, LCE7) [V] | partly | nothing | Low. |
| T4 | §9.5.2.3 NOTE 7 | TV1 = TV2 is allowed; F tells them apart. | `our_transaction()` value + inverse flag [V] | no | spec | |
| T5 | §7.3 | TI lifetime: the transaction ends at NULL. | see **C1** [P] | **yes** | nothing | High (C1). |

---

## 7. LAPC (EN 300 175-4 §9.2) and Lc (§6.1, §7.10, §10.3.1)

| ID | clause | spec requires | our code (file:line) | deviates? | justified by | risk and recommended action |
|---|---|---|---|---|---|---|
| **L1** | 175-4 §9.2.3.6; A.3.1 **N250 = 3** | On `<DL.04>` expiry, retransmit only while the count is below N250. At N250, release (`MAC_DIS-req`, `DL_RELEASE-ind` "abnormal"). | `dect_lapc_tick` `411-417` retransmits **forever** every 2 s. No counter [P] (p2: retransmits=6 after 12 s, still outstanding) | **yes** | nothing | **Medium.** A dead peer is never detected at DLC. The link stays "up" from NWK's view, and CC/MM timers are the only bound. Add N250 and report the release up. |
| L2 | §9.2.1.1; §9.2.4.2 | Class A: P/F = 0 in **all** transmitted frames. When refusing class B, the RR carries "F … set to the fixed class A value". | `queue_rr(l, f->nlf, f->pf)` `382`, `395` mirrors the peer's P. `queue_ua(l, f->pf)` mirrors it too [P] (p2: a class-B request with P=1 is answered `91 31 01`, F=1) | **yes** | capture `[CR AVM]` (class-A refusal) + bench (attach works) | Low. Send F=0. Refusing on LLN 1 is spec-correct (§9.2.4.3.1 b). |
| L3 | §9.2.4.1/§9.2.8; GAP Table 69/70 (class A only) | SABM is class B (re-)establishment. A class A-only FT should not accept it. | `dect_lapc.c:323-329` answers SABM with **UA** (LLN 1) and resets the variables [P] (p2: `11 73 01`) | yes | nothing | Low. UA accepts class B operation. Answer DM instead (or the class A refusal); ‑4 §9.2.8 for the exact frame. |
| L4 | §9.2.3.8 | Re-establishing class A discards all outstanding I-frames and resets **all** link variables. | On NLF, `dect_lapc.c:315-320` resets V(R) and the reassembly but **not** V(S), V(A) or `outstanding` [V]. Consequence assumed [A]: a PT re-establishing on the same handle can make our next I-frame look like a duplicate, and its RR N(R)=0 then "acks" it | yes | nothing | Low-medium, unclear in practice, because `reg_start()` usually re-initialises on a new handle. Reset V(S), V(A), `outstanding` and `txmsg` on NLF. |
| L5 | §9.2.3.5 (c) | An invalid N(R) means re-establish or release. | `handle_ack` `271-286` ignores it [V] | yes | nothing | Low. |
| L6 | §9.2.3.2 | Class A: window 1, modulus 2. | `dect_lapc.c:146`, `255-268`, `271-286` [V] | no | spec | |
| L7 | §9.2.3.1 | Answer the PT's establishment I-frame with RR, NLF set. | `dect_lapc.c:382` (NLF mirrored) [V] | no | spec | |
| L8 | §9.2.3.7 | Class A release is not a DISC exchange. | R11, open [V] | yes | (open) | As recorded. |
| L9 | §9.2.5.5 | RNR means peer busy: stop sending. | RNR is treated as an ACK only (`338-344`) [V] | yes (class B clause; unclear for class A) | nothing | Low. |
| **L10** | 175-4 §6.1.5 (a), §7.10, §10.3.1 | A frame with a checksum error is **invalid** and discarded. LSIG = the lower 16 bits of the PMID, **supplied by the LLME at establishment**. | `dect_lc.c:162-172`: if the first frame of a link fails with LSIG 0, the LSIG is **solved from that frame** and locked, so the first frame's checksum is never checked [V] | **yes** | bench (multi-fragment page response "made every incoming call fail"; comment `dect_lc.c:155-161`) | Low-medium. A corrupted first frame is accepted *and* locks a wrong LSIG, after which every later frame fails until the next connection. **Action:** compute the LSIG from the PMID (the assigned TPUI, ‑6 needed), or at least accept a derived LSIG only if it equals a PMID we know. |
| L11 | §6.1.4 / §7.8 | 5-octet fragments, fill `F0`, FLEN rounded up. | `dect_lc.c:81-106` [V] | no | spec | |
| L12 | GAP §6.9.3 | Segments past the first may be discarded. | R10 [V] | no | spec | |

---

## Out of spec but bench-passed

Deviations whose only backing is a bench result or a capture, or which were explained by a corpus the TI-value defect had since voided. Ranked by risk.

| rank | ID | deviation | the bench or capture it rests on | why the pass does not prove it right | recommended discriminating test |
|---|---|---|---|---|---|
| 1 | **M1** | AUTH-TYPE stores the DCK as #0; CIPHER-REQUEST asks for #8. | `[CR A-0011]` (byte 3 only partly recovered), `[CR A-0027]` | Ciphering never passed (test 17). The failure was given one cause (the chip DCK) while this second cause sat in the same message pair. | Pre-commit: change octet 5 to `0x18` only, chip path unchanged. "CIPHER-REJECT appears or disappears" or "the link is still released at 212 ms" separates the two causes. |
| 2 | **C6** | Re-present the SETUP on any 0x32, not only after a detected collision. | corpus reading of 2026-10-24 ("about to cross"). Those refusals were later explained by TV=1. | The re-presentation rang only because the refusal was a collision. A non-collision 0x32 has never been tested since the TI fix. | Unit test: 0x32 without a crossing SETUP must go to NULL. On the bench: ring a handset that is in a call with another base (it should refuse once and not be rung three times). |
| 3 | **LCE1** | Release any live link before paging, without checking the PT's open transactions. | `[CR A-0052/A-0053]`, bench vF415/vF423 (7/7) | The bench showed it is *needed for audio*. It did not show it is harmless while the PT has an LiA or MM transaction open. | Ring while the A540's 17-second LiA call is open; check whether the handset's LiA call is torn down mid-session. |
| 4 | **C10** | CONNECT before the U-plane is confirmed (default). | bench 12e (the timer was not affected) | 12e tested the call timer, not clipping or PT behaviour. | A short tone at t=0 after CONNECT, with and without `bindfirst`, compared on a capture. |
| 5 | **M2** | Key-allocation failure answered with ACCESS-RIGHTS-REJECT instead of AUTHENTICATION-REJECT. | bench 9, test 20 (the A540's silent 0000 try) | Gigasets restart access rights after any reject. A PT that waits for AUTHENTICATION-REJECT would sit out `<MM_auth.1>`. | Low priority. Fix it to the spec and re-run test 9. |
| 6 | **M7** | LOCATE-ACCEPT echoes the PT's old LAL and omits LA if the PT sent none. | `[CR A-0117]` (stock bytes `07 01 64` are identical), corpus of 239 accepts | Every sample had a PT-supplied LA. The omission path and the "PT's LAL ≠ ours" path were never exercised. | Unit test: a LOCATE-REQUEST without LA must still get LA. Multi-cell test: a roamer with a different stored LAL. |
| 7 | **L10** | LSIG learned from the first frame, whose checksum is never checked. | bench (a page response opening the link) | It passes on clean frames. A corrupted first frame has never occurred on the bench. | Unit test: corrupt one octet of a first frame. It must be rejected, not lock an LSIG. |
| 8 | **L2** | RR/UA mirror the P bit on a class A link. | `[CR AVM]` refusal pattern, attach works | The PT ignores P/F in class A (§9.2.1.1 "should ignore"), so the pass is the PT's tolerance. | none needed; fix it to spec |
| 9 | **LCE5** | N300 = 20 pages. | bench F1 (re-sync) | Allowed ("application specific"). Listed only because the value comes from one run. | none |
| 10 | R13 | No CONNECT-ACK wait on a PT call. | bench 12b | **Spec-correct after all** (A.1 `<CC.05>` note). Not a deviation; fix the comment's citation. | none |

---

## Top findings (10)

| # | ID | finding | clause | confidence | action |
|---|---|---|---|---|---|
| 1 | **C1** | A CC entity never returns to NULL after a release. On the same link, a second PT SETUP is silently ignored on the same TV, or refused with 0x32 on another TV, or after our own incoming call. No test and no bench run (the harness stops after the first call) has covered it. | 175-5 §9.5.1, §9.5.2.2, §17.3.2.1, §17.3.3; GAP Table 66 | **probe-verified** | NULL after release completion. A real F-19 only while our own RELEASE is pending. Two new tests. |
| 2 | **M1** | `<<AUTH-TYPE>>` octet 5 is `0x10` (store DCK as #0); GAP requires `0x18` (#8), matching the `0x98` CIPHER-INFO. A competing cause for the test 17 failure. | GAP §8.27 Table 43, Table 61; 175-5 §7.7.4, §7.7.10 | verified by reading; effect on the handset assumed | One-byte fix, then a pre-committed run. ‑7 to confirm. |
| 3 | **C6** | Re-presentation fires on any 0x32 in CALL PRESENT, with no detected collision. The premise came from a corpus later explained by the TV bug. | 175-5 §9.5.2.3 (FT requirements) | probe-verified | Gate it on a crossing SETUP having been released. |
| 4 | **L1** | No N250: LAPC retransmits forever, so a dead peer is never reported. | 175-4 §9.2.3.6, A.3.1 | probe-verified | Count to 3, then release up. |
| 5 | **LCE2 / LCE1** | The FT never releases a link when NWK goes idle (no NLR, no `<LCE.01/.02/.05>`). Yet it releases a *live* link before every page without checking for open PT transactions. | 175-5 §14.2.1, §14.2.3, §14.2.7; GAP §8.37 | verified by reading | NLR in CC and MM. Release before paging only when the link is NWK-idle. Record the chip constraint as such. |
| 6 | **C4** | `F<CC.01>` expiry sends RELEASE-COM (abnormal). GAP §8.3.2.3 requires normal release, and the code comment cites that very clause. | GAP §8.3.2.3; 175-5 §9.3.1.5 | probe-verified | `dect_cc_release(0x0D)`, plus `F<CC.02>` (C2). |
| 7 | **LCE8 / M9** | After `<MM_ident.1>` expires (or on an unhandled TEMPORARY-IDENTITY-ASSIGN-REJECT) the TPUI assignment has failed, but we keep paging with it and only print a warning. | GAP §8.28.2.3/4; 175-5 §14.2.3 (a) | verified by reading | Page with the default TPUI after a failure (‑6 for its layout). Handle REJECT. |
| 8 | **M7** | LOCATE-ACCEPT's mandatory `<<LOCATION-AREA>>` is the PT's own old LAL echoed back, and is omitted when the PT sent none. | 175-5 Table 54, §13.4.1; GAP Table 45 | verified by reading; corpus shows the echo always present | Always send our own LAL. |
| 9 | **M2 / M3 / M4** | Key allocation: failure gets ACCESS-RIGHTS-REJECT instead of AUTHENTICATION-REJECT; no `<MM_key.1>`; the UAK is committed with no "unconfirmed" state. | GAP §8.32, §8.32.1.1, §8.32.2.4 | verified by reading | Send AUTHENTICATION-REJECT on the key-allocation TI. Add the timer and an unconfirmed-UAK flag (‑7). |
| 10 | **E5 + small validation gaps** | A PAGE-RESPONSE without PORTABLE-IDENTITY is ignored, but §17.6.5 requires PAGE-REJECT (the comment misquotes the clause). Also not handled: a SETUP with F=1 (C8), TV 7 (E3), a SETUP without BASIC-SERVICE accepted and connected (C9), IWU-INFO answered on CC's own TI rather than the received one (I3), class B SABM answered UA (L3), V(S) not reset on NLF (L4). | 175-5 §17.6.5, §17.3.1, §17.3.2.1, §17.6.1; 175-4 §9.2.3.8 | probe- or reading-verified | A batch of small fixes, each with a unit test. |

**Where I am unsure.**
- C1's real-world exposure depends on PTs reusing a link after a release. In the 2026-10-25/28 logs I saw, the handsets released the link after every call. A first awk pass flagged 39 "second SETUPs"; they turned out to be log-ordering artefacts of a void run. So the defect is real in the code, but I found no logged occurrence.
- M1's effect on test 17 is a hypothesis. The bench saw no CIPHER-REJECT from the handset.
- L4's consequence depends on whether the chip ever delivers a re-establishment on an unchanged handle.
- I did not audit `dect_iwu.c` list-access content (TS 102 527-3), `dect_session.c`, or the side-link and internal-call routing in `dect-stubd.c` beyond the call sites cited.

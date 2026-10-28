> **Reviewer output, kept as written** (2026-10-28, read-only review of `1496d6e`). Line numbers refer to that commit; several findings were fixed the same day. Which items were checked by hand, and the priorities, are in `../spec-sweep-2026-10-28.md`.

# A2: CAT-iq audit of the openDECTcell FP (TS 102 527-1 V1.5.1, TS 102 527-3 V1.7.1)

Read-only review, 2026-10-28, repo at `cf4eea1`. FP column used throughout: **FT R/B** (residential/business).
Code claims are marked **[V]**, meaning I read the code, or **[A]**, meaning assumed or inferred and not read.
"N/A chip" means MAC/DLC/PHL that the SC144xx does. File paths are relative to `opendectcell/`.
I used `docs/gap-conformance.md` and `docs/spec-conformance.md` as the baseline and did not redo what they settled.
The "fixed" claims I re-checked hold in the code: CC-INFO dispatch, IE ascending order, lowest free TI, CALL-PROC-only answer to a LiA call, CS call connect in our CONNECT, re-classing on `0x17`, and line 1 in the line list.

**Correction to gap-conformance.md.** That file calls CLIP (N.30) and CNIP (N.34) "optional for us", which is right for EN 300 444 R/B only. Under CAT-iq the status is higher:

- TS 102 527-1 Table 3 makes **GAP.N.30 CLIP mandatory**.
- TS 102 527-3 Table 3 makes these mandatory for the FT R/B: **N.9, N.12, N.17, N.20, N.30, N.31, N.34 and N.35**. That covers authentication, key allocation, encryption FT initiated, terminate access rights FT initiated, CLIP, internal call, CNIP and enhanced security ("encryption of all calls", EN 300 444 §8.45.1).

---

## A. TS 102 527-1 feature and service tables (FP R/B)

| ID | clause | requirement | FP | ours | evidence | finding and recommended action |
|---|---|---|---|---|---|---|
| A1 | 527-1 T1 NG1.1, NG1.3; T7 NG1.SC.1/3 | G.726 and G.722 voice services and codecs | M | implemented: `dect_audio.c`, `dect_g722.c`. The default incoming basic service is 0x88 (`dect-stubd.c` RING_BASIC_SVC_WB:111, :7938) [V] | bench Tests 10/11: wideband both ways on all three Gigasets; `test_g722.c` | done |
| A2 | T1 NG1.2, NG1.4, NG1.7–10 | G.711, G.729.1, LC3plus | O / C | missing | none | not needed. Transcode SIP G.711 on the host |
| A3 | T3 NG1.N.1 | codec negotiation | M | partial, see B1–B4 | | |
| A4 | T3 NG1.N.2, T7 NG1.SC.9 | codec switching (§7.3.4 codec change, §7.3.5 slot type modification) | M | **missing**. No {CC-SERVICE-CHANGE}/ACCEPT/REJECT type in `dect_nwk.h`:69–80, so the message falls into the ignore branch `dect_cc.c:dect_cc_rx:1115` [V] | none | see B5 |
| A5 | T3 GAP.N.1–4, N.8, N.15, N.18, N.19 | GAP mandatory set | M | done per gap-conformance.md. Re-checked: `case DECT_CC_INFO` at `dect_cc.c:1040` [V] | bench G6/G7, Tests 15/19 | settled |
| A6 | T3 GAP.N.9, N.12, N.17 | authentication, key allocation, encryption FT initiated | C301 (O without N.35) | DSAA in `dect_dsaa.c`, `send_key_allocate` `dect_mm.c:169` [V]. Ciphering fails | Test 17 fail: the chip never gets the DCK | optional in Part 1, **mandatory in Part 3** (C21) |
| A7 | T3 GAP.N.30 | CLIP | **M** | encoder `dect_clip.c:dect_clip_put_number:174` [V], fed only by `-a clip=` | Test 21: the number shows on the C430A and AS690H, not on the A540 | see F1 |
| A8 | T3 GAP.N.34 | CNIP | O (M in 527-3) | `dect_clip.c:dect_clip_put_name:200` [V] | Test 21: no handset shows the name | see F2 |
| A9 | T3 GAP.N.31, T9 §7.3.6 | internal call | O (M in 527-3) | `dect-stubd.c:ic_start:~5291`, `ic_setup:~5367`, only behind `-a twocalls` [V] | Test 19 retry passes both directions | make it the default path; see C23, F5 |
| A10 | T4 GAP.D.1, D.2 | LAPC class A and Lc; CS fragmentation | M | host `dect_lapc.c`, `dect_lc.c` [V] | `test_lapc.c`, `test_lc.c` | done. Class A release is still open (spec-conformance) |
| A11 | T4 NG1.D.1/5, GAP.D.3–6 | LU1/FU1, Lb, handover, encryption | M / C | N/A chip | | |
| A12 | T5 all NG1.M.*, GAP.M.*; T6 PHL | advanced connections, long slot, broadcast, paging, Q-control | M | N/A chip | wideband long slot in use | NWK-initiated slot change is not reachable from the host (`[CR A-0069]`) |
| A13 | T7 NG1.SC.32 (C710: M if N.31) | FP audio type 6a, internal call | C→M | bridge exists. With mismatched codecs there is no transcoder and the parties hear silence (`dect-stubd.c` ic_tick ~:5466 log text) [V] | Test 19: same codec on both sides | add a G.722↔G.726 path or force wideband (§7.4.3.9) |
| A14 | T7 NG1.SC.26–29 | FP audio type (VoIP 3.1 kHz / wideband) | C706/C708, at least one | N/A to the radio. It is an echo and delay property of the SIP side | none | controller topic |
| A15 | T8 GAP.A.1 | AC to bitstring mapping | C801→M | `dect_mm.c:pin_ac:52` [V] | Tests 8, 20 | done |

## B. TS 102 527-1 §6.14 and §7.3 procedures

| ID | clause | requirement | FP | ours | evidence | finding and recommended action |
|---|---|---|---|---|---|---|
| B1 | §7.3.1 | <<CODEC-LIST>> in {ACCESS-RIGHTS-ACCEPT} and {LOCATE-ACCEPT}, at least G.722 and G.726, negotiation "possible" (001) | M | `dect_mm.c:put_codec_list:249`, called at :312 and :408 [V] | wideband bench | done. `narrowband_only` sends G.726 alone (:244), which breaks "at least G.722+G.726". Fine as a test knob, never as a default |
| B2 | §7.3.2 | basic service "wideband speech default" (0x88) | M | default 0x88 [V] | runs 31–33 | done |
| B3 | §7.3.3 | the receiver chooses one codec at the latest in CONNECT (or in CC-INFO with PROGRESS-IND). A SETUP list, if present, holds G.722+G.726 | M | PP-originated: `dect_cc.c:queue_connect:577` names one codec [V]. FP-originated: the handset's choice is learnt in ALERTING/CONNECT (`learn_codec_choice:619`) [V]. No SETUP list by default [V] | `test_connect_names_the_chosen_codec`, `test_wideband_link_chooses_g722` | done |
| B4 | §7.3.3, §7.3.5.1 | if slot modification fails: fall back with {IWU-INFO}+<<CODEC-LIST>> | M | missing. {CC-IWU-INFO} is sent only for LiA (`queue_iwu:102`) [V] | none | low impact. The chip follows the PP's bearer request |
| B5 | §7.3.4 | codec change: {CC-SERVICE-CHANGE}+codec list+service-change-info, accept or reject, IWU-INFO afterwards, mute rules | M | **missing** [V] | none | first step: answer a received SERVICE-CHANGE with {CC-SERVICE-REJECT}, which the standard allows ("shall either accept or reject"). Needed later for parallel calls (527-3 §7.4.3.3) |
| B6 | §7.3.6 | internal call: class 9 to NG PPs, **"external call" class to GAP PPs** | M | always 0x98/0x90 (`dect-stubd.c` IC_BASIC_SVC:5219, `ic_setup:5378`) [V] | Test 19 | needs the PP type (B7) |
| B7 | §7.3.7; 527-3 §7.4.9.1 | FP receives <<TERMINAL-CAPABILITY>> in ACCESS-RIGHTS-REQUEST and LOCATE-REQUEST; profile indicator_7 bits 2/3 = Part 1/Part 3 | M | **not parsed anywhere** (no TERMINAL_CAP in `src/`) [V] | none | parse and store per handset. Prerequisite for B6, E11, F2 and 7.4.5.2.4 (GAP PP: no CALL-INFORMATION) |
| B8 | §7.3.8 | indirect FT link establishment, IPUI mismatch → {LCE-PAGE-REJECT}, F-LCE.03 | M | `dect_link.c` page response :255–280 [V], `dect_link_page_reject` [V] | unit tests; A540 run 20 | settled |
| B9 | §7.3.9; 527-3 §7.4.9.2 | broadcast Qh=3 (a32, a33, a44 …), Qh=11 (a24=1, a29=1, a34 multiple lines, a30–a33) | M | N/A chip (0x0157/0x0169 capability region, `why-the-handsets-never-rang.md`) | our extended part-2 tail matches stock's `c8 01 85 11 34` (`mac-bringup.md:384`) | decode the tail against 527-3 table 37. **a34 must be set if we claim Multiple Lines.** Ask the room which mail sets it |
| B10 | §6.14.2 | lowest free TI | M | `ring_queue_setup` sets 0 (`dect-stubd.c` ~:3905), internal SETUP `set_next_ti(0)` :5377 [V] | runs 15/16 | done |
| B11 | §6.14.4 | ignore an unexpected or unrecognised CC message | M | `dect_cc.c:1115` ignores it on our own TI [V] | `test_unhandled_message_on_our_call_is_ignored` | done |
| B12 | §6.14.6 | {CC-NOTIFY}+<<TIMER-RESTART>> while MM delays CC | O (FT) | missing | none | useful for the held roamer SETUP; optional |

## C. TS 102 527-3 Table 3 and Table 8, FP R/B

| ID | clause | requirement | FP | ours | evidence | finding and recommended action |
|---|---|---|---|---|---|---|
| C1 | NG1.N.3, §7.4.1.3 | missed call notification | M | **missing**: no <<EVENTS-NOTIFICATION>> (0x7D) encoder [V] | none | see I3 |
| C2 | NG1.N.4, §7.4.1.2 | voice message waiting notification per line, and after location registration | M | **missing** [V] | none | SIP MWI → FACILITY (I2) |
| C3 | NG1.N.5, §7.4.2 | date/time sync FT-initiated **and** PT-initiated | M | FT side done; PT side missing (I4, I5) | `test_time.c` | |
| C4 | NG1.N.6 (+§7.4.8.3) | parallel calls | **M** | **missing**: one call per CC entity per link (`dect_cc_rx` SETUP branch rejects a second call, ~:961–975) [V] | `test_second_setup_during_an_active_call_is_rejected` | section D |
| C5 | NG1.N.7 | common parallel call procedures | **M** | **missing** | none | section D |
| C6 | NG1.N.8 | call transfer (announced, unannounced, re-injection, remote CLIP/CNIP) | **M** | **missing** | none | after D |
| C7 | NG1.N.9/10/11 | conference, intrusion, deflection | O, note 6 | missing | none | the **negative acknowledgement (§7.4.3.4) stays mandatory** |
| C8 | NG1.N.12 | line identification | M | partial (E) | | |
| C9 | NG1.N.13 | call identification | M | partial (E) | | |
| C10 | NG1.N.14 | multiple lines | **O** | missing (one line, `dect_iwu.h` struct) [V] | none | the MVP needs it (section G, MVP-1) |
| C11 | NG1.N.15 | multiple calls (≥2 simultaneous, call waiting) | **M** | partial: two calls on two handsets work [V] | Test 18 pass | no call waiting, no "multiple calls mode" field |
| C12 | NG1.N.16 | list access | M | partial (H) | | |
| C13 | NG1.N.17 | CLIR | O | missing; a restricted number is omitted (`dect_clip_put_number:182`) [V] | `test_clip.c` | fine |
| C14 | NG1.N.18, §7.4.13 | external call forwarding CFU/CFNA/CFB, line settings 0B–0D | **M** | **missing** | none | can map to SIP forwarding. The fields are required even read-only |
| C15 | NG1.N.19, §7.4.14.1.2 | uplink DTMF after CONNECT from <<MULTI-KEYPAD>> | M | digits gathered in ACTIVE (`dect_cc.c:1040–1063`) [V]. No per-digit event to the network [A] | none | expose a per-digit event (RFC 2833 / SIP INFO in the controller) |
| C16 | NG1.N.20, §7.4.15.2.1 | tones via <<SIGNAL>> to a Part 3 PP: ring-back, busy (04), call waiting (07), intercept, neg-ack (09), tones off (3F) | **M** | only alerting 0x40/0x41/0x4F are defined (`dect_nwk.h:151–153`) [V] | none | add the values and send them in CC messages |
| C17 | NG1.N.21 | headset management | M (FT R/B) | missing | none | matters only with headset PPs. Minimum: negative ack on 1C 50 |
| C18 | NG1.N.22 | DCIBS lines | O | N/A (SIP lines) | | send line type SCT=0, NDT=1 |
| C19 | GAP.N.9, N.12 | authentication, key allocation | **M** | done (A6) | Tests 8, 9, 20 | |
| C20 | GAP.N.17, N.35 (§8.45.1 "encryption of all calls"); GAP.D.6, GAP.M.7 | ciphering | **M** | fails: CIPHER-REQUEST goes out, the chip lacks the DCK | Test 17 fail | a conformance gap, not an MVP blocker. Calls work unciphered on the test handsets |
| C21 | GAP.N.20, EN 300 444 §8.31 | terminate access rights FT initiated | **M** | **missing** (no builder in `dect_mm.c`) [V] | none | needed for "delete entry" on the Internal Names List (§7.4.11.2) |
| C22 | GAP.N.31 + §8.43/§8.44 | internal call with internal CLIP (TON 3, NPI 9, terminal id) and CNIP (name, DECT alphabet, screening 01) | **M** | internal SETUP without identity (`ic_setup:5378` passes NULL) [V] | Test 19 | add both |
| C23 | GAP.N.34 + §7.4.17 | CNIP; UTF-8 to Part 3 PPs, IA5 to GAP/Part 1 PPs | **M** | both alphabets exist [V]; no gating by PP type | Test 21 | F2 |
| C24 | GAP.N.15 + §7.4.19 | <<SIGNAL>> 0x41 external, **0x40 internal** | M | external 0x41 default (`dect_cc.c:dect_cc_init:27`) [V]. **The internal SETUP carries 0x41**: side link signal from `s->audio.signal_value` (`dect-stubd.c` ~:4996) [V] | bench log, Test 19 retry: `e0 98 e4 41` | send 0x40 on class 9 calls |
| C25 | NG1.A.2, §7.10.1.2.2 | easy pairing: FP button, a44 cleared on success or timer, one more attempt after a failed 0000, one success per window | M | button / SIGUSR1, window closes after enrolment [V via bench] | Test 16 pass; Test 20 note (a 0000 try, then the PIN accepted) | done in substance |
| C26 | NG1.A.1, NG1.A.3 | easy PIN, handset locator | O | missing | | optional |

## D. §7.4.3 parallel calls (FP side; every procedure is M for the FP in Table 9)

| ID | clause | requirement | FP | ours | evidence | finding and recommended action |
|---|---|---|---|---|---|---|
| D1 | §7.4.3.1 | "only one link between FP and PP, with one U-Plane and one call control instance" for every parallel call | M | architecture: `struct dect_cc` holds one call on one TI [V] | | needs a call table per PP (call id → SIP leg, state) driven by CC-INFO. A second call on the same PP needs **no** extra radio link or audio channel |
| D2 | §7.4.3.2 table 17 | keypad control codes in CC-INFO: `17`+nr, `1C15`+nr, `1C31` toggle, `1C33` release, `1C34` transfer, `1C35`/`1C36` accept/reject, `1C38`, `1C41`/`1C42` hold/resume, `1C32`/`1C40` → neg ack | M | only `0x17` as the first octet re-classes the call (`dect_cc.c:learn_int_key:541`) [V]. `0x1C` codes are stored as dialled digits [V]. The PP's <<CALL-INFORMATION>> (call id) is never parsed [V] | `test_int_key_makes_an_internal_call` | parse the control code and the call id |
| D3 | §7.4.3.4, table 17a | negative ack: re-send the target call's status + reason ('control code not supported'/'failed') + <<SIGNAL>> 09 | M | missing. `dect_callinfo_put:50` cannot encode the **call status reason** subtype (2/2) [V] | `test_callinfo.c` | add subtype 0x02 (values 00 system busy, 01 line in use, 02 not supported, 03 failed, 10 user busy, 11 number n/a; EN 300 175-5 §7.7.56) |
| D4 | §7.4.3.5.1 | outgoing parallel call: CS call hold for the active call, new call id + CS call setup ack, line confirmation, statuses in CC-INFO | M | missing | none | |
| D5 | §7.4.3.5.2 | **call waiting indication** (detail in section K) | M | missing; no FP-side CC-INFO API in `dect_cc.h` [V] | none | add `dect_cc_info(...)` building CC-INFO with callinfo/signal/CLIP/CNIP |
| D6 | §7.4.3.5.3 | toggle: CS call hold (old), then CS call connect (target) | M | missing | | |
| D7 | §7.4.3.5.4 | release of one call (1C33) / FP releases one call with CS idle | M | missing | | |
| D8 | §7.4.3.5.6/7 | accept: CS call connect (new) + CS call hold (old). Reject: SIGNAL 3F + CS idle | M | missing | | |
| D9 | §7.4.3.5.8/9 | hold / resume | M | missing | | |
| D10 | §7.4.3.5.10/11 | CLIP/CNIP on call waiting, IE order SIGNAL → CPN → CPName; private NPI if internal | M | missing (encoders reusable) | | |
| D11 | §7.4.3.5.12 | active call release with replacement (1C38) | M (FP) | missing | | |
| D12 | §7.4.3.6 | transfer (1C34), re-injection, remote CLIP/CNIP | M | missing | | after D4–D9 |
| D13 | §7.4.3.9 | internal call codec priority: wideband if both PPs can | M | 0x98 by default [V]; no transcoder on a mismatch | Test 19 | A13 |
| D14 | §7.4.8.3 | busy system/line: after one non-INFO CC message, CC-INFO {call id, CS call disconnecting, reason} + SIGNAL 04, later CS idle + {CC-RELEASE} | M | busy CC answers {CC-RELEASE-COM} "insufficient resources" (`dect_cc.c:on_setup:667–672`) [V]. An internal call with no channel is released (`dect-stubd.c:5310–5315`) [V] | `test_busy_link_refuses_speech_and_takes_list_access` | Part 3 PPs: use 7.4.8.3. GAP PPs: keep the release |

## E. Line and call identification (§7.4.5, §7.4.6)

| ID | clause | requirement | FP | ours | evidence | finding and recommended action |
|---|---|---|---|---|---|---|
| E1 | §7.4.5.1 | line id on every external call even with one line, with line type information (NDT, SCT) | M | the encoder is right (`dect_callinfo.c:50`, coding checked against EN 300 175-5 §7.7.56) [V]. **Off by default**; only `-a lineid=,linetype=` (`dect-stubd.c` help :7658–7663) [V] | `test_setup_carries_line_identifier`; Test 21: **A540 rings silently with any line id** | see Top-5 |
| E2 | §7.4.5.2.1/2, §7.4.5.2.4 | PP picks the line in SETUP or in the first CC-INFO (by the first digit at the latest), or sends "None" 0x7F. FP confirms line + type in SETUP-ACK/CALL-PROC/CC-INFO before CONNECT. A line the PP is not attached to → disconnect | M | **missing**: the PP's 0x7E is never parsed [V]. Our answers carry a line only if `-a lineid` is set (`dect_cc_put_call_status:60–79` copies `cc->callinfo`) [V] | none | **MVP blocker** for two lines per handset |
| E3 | §7.4.5.2.3 | `#`+digit line selection (GAP/Part 1 PPs) | O | missing | | useful for a C4 or GAP PP with two lines |
| E4 | §7.4.5.2.4.1 | toward a GAP/Part 1 PP: FP-managed selection, **no** <<CALL-INFORMATION>> | M | not distinguishable (B7) | | |
| E5 | §7.4.5.3.2, fig. 42 | incoming external SETUP: line id + line type + call id + CS call setup | M | only via `-a` [V] | Test 21 c (line only) | default for Part 3 PPs |
| E6 | §7.4.6.1 | call ids unique system-wide; one id per call; the same id on both PPs of an internal call | M | **call id fixed to 1 on every link** (`dect-stubd.c:4790–4792`, `:5002–5004`) [V] | Test 18: two calls, both id 1 [A, by code] | allocate per call ("no re-use" 0..127) |
| E7 | §7.4.6.2 | call id in the first FP message of a PP-placed call | M | SETUP-ACK/CALL-PROC/ALERTING/CONNECT via `queue_with_status:82` [V]; none on LiA calls (correct, `on_setup:661–666`) [V] | `test_call_status_in_our_answers`; Test 15 `7e 06 01 00 81 02 01 85` | done |
| E8 | §7.4.6.3 | call id in **every** incoming SETUP, internal too | M | only `-a callid` for the main link. The internal SETUP has none (side links never call `dect_cc_set_callinfo`; only `:4793` does) [V] | none | default on |
| E9 | §7.4.6.4.3 | incoming: CS call setup in SETUP; **CS call connect in a CC-INFO after CONNECT-ACK** (never in CONNECT-ACK) | M | missing: on the PP's CONNECT we send only CONNECT-ACK (`dect_cc.c:1004`) [V] | none | add it. The likely reason incoming calls start no handset call timer (untested) |
| E10 | §7.4.6.4.3 | PP-placed call, non-early CONNECT: CS call connect in CONNECT, **only when end to end connected** | M | sent in CONNECT [V] | Test 15 pass | the controller must send CONNECT at SIP 200 OK, or use an early CONNECT followed by CC-INFO statuses |
| E11 | §7.4.6.4.1 | "A Part 3 FP shall not send any call status to GAP or NG Part 1 devices" | M | **call status on for every PP** (`!no_call_status`, `dect-stubd.c:4790`) [V] | | gate on B7 |
| E12 | §7.4.10.6.2 | LiA → first voice call: CONNECT carries **no** call id, call status or line id; then CC-INFO {new call id, CS call proc} … | M | our CONNECT on the re-classed LiA call carries CS call connect (`queue_connect`) [V] | Test 19 A540 passes anyway | conform once D/H24 exist |
| E13 | §7.4.5 | internal call: **no** line identifier | M | ok today (no line by default). Once lineid is on by default, `dect_cc_put_call_status` would add it to class 9 calls [V] | | gate by call class |

## F. CLIP / CNIP

| ID | clause | requirement | FP | ours | evidence | finding and recommended action |
|---|---|---|---|---|---|---|
| F1 | EN 300 444 §8.41 | CLIP in SETUP **or** CC-INFO (the FT supports one, the PT both) | M | SETUP method, `dect_cc_setup_id` `put_identity` [V] | Test 21 a: number shown on C430A/AS690H; **A540 nothing** in a, b, c | sweep the A540 with CLIP in a CC-INFO carrying the call id, and with the full fig. 42 callinfo |
| F2 | §8.42; 527-3 §7.4.17.1 | CNIP; UTF-8 to Part 3, IA5 to GAP/Part 1 | **M** (527-3) | encoder per §7.7.53 fig. 98b (octet 3 bit 8 = 0) [V] | Test 21: name shown on **none**, both alphabets | no further mandatory field exists. Pre-commit a sweep: (i) CNIP in CC-INFO with call id (§7.4.6.1 names that case), (ii) screening 01 or 11, (iii) with full callinfo. [A] Handsets may prefer their own phonebook |
| F3 | EN 300 175-5 §7.7.53 note 1 | if CLIP matches the FP Contact List, send that name with screening 01 | should | missing | | comes with the phonebook (MVP-3) |
| F4 | EN 300 175-5 §7.6 | ascending IE order | M | fixed; `test_setup_ie_order_is_ascending` [V] | | settled |
| F5 | §8.43/§8.44 | internal CLIP/CNIP | M | missing (C22) [V] | | |
| F6 | §7.4.3.5.10/11 | CLIP/CNIP on call waiting | M | missing | | D10 |

## G. Multiple lines and multiple calls (§7.4.7, §7.4.8)

| ID | clause | requirement | FP | ours | evidence | finding and recommended action |
|---|---|---|---|---|---|---|
| G1 | §7.4.7.1.0 | FP sets a34 | M if N.14 | N/A chip, unverified | | B9 |
| G2 | §7.4.7.1.2, §7.4.8.1.2 | at least 2 simultaneous calls and call contexts | M | 2 proven, 3 audio channels per chip (`[CR A-0124]`) | Test 18 | ok |
| G3 | §7.4.7.2, §7.4.11.2 | every PP attached to ≥1 line; FP-managed or PP-managed attachment via "Attached handsets" | M | one line, every handset attached (`dect_iwu.c:put_entry_field:317–345`) [V] | `test_attached_handsets_bitmap_spans_two_octets` | one bitmap per line from the pool |
| G4 | §7.4.7.2.3 | PP may update settings only of lines it is attached to | M | read-only | | |
| G5 | §7.4.7.3 table 33 | incoming call on line A for a PP busy on line B → **call waiting**; outgoing → parallel call initiation | M | missing | | mandatory consequence of two lines per handset |
| G6 | §7.4.7.4 | internal calls between any PPs | M | slot-based, no line check [V] | Test 19 | ok |
| G7 | §7.4.8.1.2 | **all idle PPs ring at once** on an incoming call | M | the daemon rings the first store's handset only (`ring_queue_setup:3881–3889`) [V] | | controller/cell: page every attached idle PP |
| G8 | §7.4.8.2.2 + note 5 | multiple-call mode: present to all attached PPs (idle: SETUP; busy: call waiting). When one answers, release the others (§7.4.3.5.4) | M | missing | | |
| G9 | §7.4.11.4.8 | "Multiple calls mode" line setting (30H/31H) | M | missing [V] | | |

## H. List access (§7.4.10, §7.4.11; Table 9 and Table 41)

| ID | clause | requirement | FP | ours | evidence | finding and recommended action |
|---|---|---|---|---|---|---|
| H1 | §7.4.10.1 table 38 | commands in <<IWU-TO-IWU>> PD 03, S/R=1, over {CC-IWU-INFO} | M | `dect_iwu.c:dect_iwu_handle:643`, dispatch `dect_cc.c:1018–1038` [V] | `test_iwu.c` (93 checks) | done |
| H2 | §7.4.10.1 "Sessions" | ≥2 sessions per PP; error code at the maximum; all end at release | M | DECT_IWU_SESSIONS 2; reset in `on_release:792` and on RELEASE-COM [V] | `test_two_sessions_and_end_session` | done |
| H3 | §7.4.10.1 "Guarantee of service", table 41 | a supported list processes **all** its M commands; no systematic NACK | M | **violated**: search → NACK 06 for every list (`dect_iwu.c:675–677`), edit/delete/save → NACK 07 (:678–712) [V] | `test_unsupported_commands_are_refused` encodes this | Internal Names: search/edit/save/delete-entry are **M**. Line Settings: edit/save M; search/delete-list I → 07 (not 06). Supported Lists: query is I → 07 (we confirm, `query_fields:466`) [V] |
| H4 | §7.4.10.1 "complete set of fields"; table 9 | every field of a supported list | M | Line Settings serves 01–03 only (`fields_line`, `dect_iwu.c:21`) [V] | | add 08 multiple calls mode and 0B/0C/0D call forwarding (0A only with CLIR) |
| H5 | §7.4.10.2.2 | list change notification for Line Settings changes, Internal Names add/remove/rename, and after location registration once per line | M | **missing** | | FACILITY + <<EVENTS-NOTIFICATION>> type 3 |
| H6 | §7.4.10.3 | default sort: call lists by date (newest = index 1), contacts by Name then First name, internal by number, lines by line id | M | internal/line ok (`default_sort_field:226`) [V] | | |
| H7 | §7.4.10.4.1 | start session confirm (count, sorting fields, reject reasons) | M | `start_session:358` [V] | settled in spec-conformance | done |
| H8 | §7.4.10.4.2 | query supported fields (editable first, instance counts) | M | 3 lists, nothing editable [V] | `test_query_supported_fields` | needs instance counts for Contact number |
| H9 | §7.4.10.4.3 | read entries + **mark entries request** (Read status) | M | read ok; mark ignored (`read_entries:520`) [V] | `test_read_*` | needed for Missed Calls |
| H10 | §7.4.10.4.4/5 | edit (locks the entry) / save (new entry id 0, modify, position index, truncation, list-full NACK) | M (contacts, internal names, system, line settings) | missing | | |
| H11 | §7.4.10.4.6/7 | delete entry / delete list | M (call lists, contacts); delete entry M on internal names | missing | | internal-names delete → C21 |
| H12 | §7.4.10.4.8 | search entries | M (call lists, contacts, internal names) | missing | | |
| H13 | §7.4.10.4.10 | data packets, 56 octets each | M | `read_entries:607–619` [V] | `test_long_lists_are_segmented_into_packets` | done |
| H14 | §7.4.10.5.2 | List of Supported Lists names every supported list | M | names 00, 06, 08 (`dect_iwu.c:12–16`) [V] | | FP R/B must support 01, 03, **04**, 05, 06, **07**, 08 (02, 09 O) |
| H15–H19 | §7.4.10.5.3–.7 | Missed (M), Outgoing (O), Incoming Accepted (M), **All Calls (M for FT R/B)**, Contact (M) | M/O | **missing** | | MVP-2, MVP-3 |
| H20 | §7.4.10.5.8, Annex H.7 | Internal Names: Number (internal, own props), Name (**editable YES**), Call interception | M | served; Name not editable (`put_entry_field:281–283`) [V] | `test_read_internal_names` | allow editing the name |
| H21 | §7.4.11.3 | DECT System Settings List: current PIN (reads FFFFFFFF), clock master, base reset, FW version, EEPROM version, new PIN | M (R/B) | **missing** | | |
| H22 | §7.4.11.4 | Line Settings List | M | partial (H3, H4) | | |
| H23 | §7.4.10.6.1 | LiA call: CALL-PROC only, no call id, F<CC.04> off; FP maintain-call timer ≥1 s | M | CALL-PROC only [V]; the FP never releases a LiA call, so the maintain-call timer holds by construction (`dect_cc_tick:810–849` has no LiA timer) [V] | `test_list_access_call_gets_call_proc_only` | done |
| H24 | §7.4.10.6.2 | LiA → first voice call: PP sends **`1C15`+number** (external) or `17`+number in one CC-INFO; the FP **immediately** sends CONNECT, then CC-INFO statuses | M | `17` handled by re-class [V]. **`1C 15`+number is not handled**: the call stays PROCEEDING and digit completion runs only in OVERLAP (`dect-stubd.c:2706–2712`) [V] | Test 19 (internal only) | **MVP blocker: dialling from the phonebook or a call list** |
| H25 | §7.4.10.6.3 | incoming voice call during a LiA session: CONNECT, then **pseudo call waiting** (§7.4.3.5.2); a normal SETUP to other idle PPs | M | missing: the ring waits for the CC entity (`ring_queue_setup` comment :3893–3901) [V] | open since an external review #7 | the A540 held a LiA call ~13 of every ~17 s (run F4, **before** the 2026-10-24 LiA fix; re-tabulate) |
| H26 | §7.4.10.6.0/.4 | during a voice call, restrict edits on system/line/internal lists ("temporarily not possible") | M | moot while there is no editing | | add together with H10 |
| H27 | §7.4.11.2 | registration adds an Internal Names entry "DECT n" + notification; mono-line auto-attach; delete → terminate access rights | M | entry added (`dect-stubd.c:4757–4768`, `:4990–4995`) [V]; notification and termination missing | | |
| H28 | §7.4.11.5 | virtual contact and call lists per line | C902 (O if N.14) | missing | | decide for the line pool (privacy between lines) |

## I. Notifications and date/time

| ID | clause | requirement | FP | ours | evidence | finding and recommended action |
|---|---|---|---|---|---|---|
| I1 | §7.4.1.1 | {FACILITY}, PD CISS, dummy TI 6, <<EVENTS-NOTIFICATION>> 0x7D (+ <<CALL-INFORMATION>> line) to the relevant PPs only | M | transport exists (`dect_time.c:dect_time_build_facility:88`, `dect_link_send_time`) [V]; events IE missing [V] | `test_facility_header_is_ciss_with_ti_six` | generalise the FACILITY builder |
| I2 | §7.4.1.2 table 14 | MWI: type 0 / subtype 1, count per line, line subtype 00. Also after every location registration, once per attached line | M | missing | | |
| I3 | §7.4.1.3 table 15 | missed call: type 1 sub 01 (new) or 02 (count changed) **plus** list change type 3 sub 01 in the same IE. Sent on a new missed call, on read/delete, and after location registration (sub 02) | M | missing | | MVP-2 |
| I4 | §7.4.2.1, §7.4.20.2 | FT-initiated time (coding 11, interpretation 0, TZ) after location registration, when the FP clock is valid | M | `dect-stubd.c:6034–6068` [V] | `test_time.c` (5 tests); bench log line "reg: sent the date and time" [A] | also re-send after an NTP step and to all PPs when a PP sets the clock |
| I5 | §7.4.2.2 | **PT-initiated**: the FP shall support it and check <<PORTABLE-IDENTITY>> | M | missing: `dect_link.c` decodes only LCE and CC (:255, :282) [V] | none | dispatch CISS FACILITY; obey "Clock master" (H21) |
| I6 | §7.4.20.3 | FT recovery within 5 min (NTP, or ask PPs) | O | NTP only [V] | | ok |

## K. Question 4: a call for a handset that is already busy, and no free audio channel

| ID | clause | requirement | FP | ours | evidence | finding and recommended action |
|---|---|---|---|---|---|---|
| K1 | §7.4.3.1, §7.4.3.5.2, fig. 6 | **PP busy in a voice call, a new call for it** (a second call on its line in multiple-call mode, or a call on its other line, table 33): the FP sends no SETUP. It reuses the existing link, U-plane and CC transaction and sends {CC-INFO} | M | missing | | |
| K2 | §7.4.3.5.2, §7.4.3.5.10/11 | **Content and order.** The 1st CC-INFO carries <<CALL-INFORMATION>> {line id (0/00), line type (0/05, NDT=1 VoIP, SCT=0), **new call id** (1/00), **CS call setup** (2/01 = 01)}. In that or later CC-INFOs, in this order: <<SIGNAL>> **07 "call waiting tone on"** (mandatory toward Part 3 PPs, §7.4.15.2.1; may be repeated), then <<CALLING-PARTY-NUMBER>>, then <<CALLING-PARTY-NAME>>. Every CC-INFO carries the waiting call's id. Internal waiting call: NPI private, no line id | M | missing | | build on `dect_callinfo`/`dect_clip` |
| K3 | §7.4.3.5.6/7/12, §7.4.3.5.4 | PP answers `1C35` (accept), `1C36` (reject) or `1C38` (release active and accept), each with the call id. FP: CS call hold (old) + CS call connect (new); or SIGNAL 3F + CS idle. Remote hangs up first → CS idle | M | missing | | |
| K4 | §7.4.3.5.2 note 2; §7.4.8.2.1 | single-call mode: only the PP in the call gets the waiting call; idle PPs do not ring. Multiple-call mode: busy PPs get call waiting, idle PPs a SETUP | M | missing | | |
| K5 | §7.4.10.6.3 | PP in a LiA session only: CONNECT (no ids), then the same pseudo call waiting | M | missing (H25) | | |
| K6 | §7.4.6.4.1 | GAP / Part 1 PP busy: no call status allowed, so CAT-iq call waiting is undefined | — | — | | [A] treat as busy toward SIP, or play an in-band tone on the active U-plane |
| K7 | §7.4.8.2.2 table 34 note 1 | **no free audio channel, incoming**: if the system is busy the call "is not presented" to idle PPs | M | the daemon defers or refuses the ring [V] | | reject toward SIP (code not in the spec; 486 or 503 [A]). Busy PPs may still get call waiting, because it costs **no** new channel (K1) |
| K8 | §7.4.8.3 fig. 53 | **no free channel, outgoing (first or parallel)**: first at least one non-INFO CC message (SETUP-ACK/CALL-PROC), then CC-INFO {call id, CS call disconnecting, reason 00 "system busy" (or 01 "line in use")} + <<SIGNAL>> 04 busy tone. After a timeout: SIGNAL 3F + CS idle, then {CC-RELEASE} (first call) or a §7.4.3.5.4 release (parallel). CS idle never carries a new call id | M | {CC-RELEASE-COM} "insufficient resources" (`dect_cc.c:667–672`) [V]; internal call: release (`dect-stubd.c:5313`) [V] | `test_busy_link_refuses_speech_and_takes_list_access` | implement for Part 3 PPs; keep the GAP release for GAP PPs |

---

## MVP-1: two SIP lines per handset from a line pool (a line attached to several handsets)

| ID | clause | requirement | FP | ours | evidence | finding and recommended action |
|---|---|---|---|---|---|---|
| M1.1 | §7.4.11.4 table 72 | Line Settings List: one entry per line, sorted by line id, unique line id. Fields 01 name (never empty), 02 line id (subtype 03 "relating to"), 03 attached handsets (count + bitmap, terminal 1 = bit 1), **08 multiple calls mode**, **0B/0C/0D call forwarding**. 0A only with CLIR | M | single entry, 01–03 [V] | `test_read_line_settings` | make `struct dect_iwu` hold N lines |
| M1.2 | table 41 + notes 3, 4; Annex H.9 | Line Settings commands: start/end, query, read, **edit, save** (modifying; creation may NACK 07), delete entry O (else 07), search/delete list I → 07 | M | NACK all [V] | | attached handsets and line name are MD-editable, so the pool can stay **FP-managed and non-editable**. A save of a non-editable field → NACK 07 |
| M1.3 | §7.4.10.2.1/.2 | list change notification on any line-settings change to the PPs attached to that line, and after each location registration once per attached line (event type 3 sub 08, <<CALL-INFORMATION>> 0/03/line) | M | missing | | needed when the pool reassigns lines |
| M1.4 | §7.4.7.2.1, §7.4.11.2 | attach every PP to ≥1 line at registration (FP-managed allowed) | M | all to line 1 [V] | | pool policy |
| M1.5 | §7.4.5.3.2 | incoming: SETUP carries the line id of the SIP account + line type (NDT=1, SCT=0) + call id + CS call setup | M | `-a` only | Test 21 | default for Part 3 PPs |
| M1.6 | §7.4.8.1.2, §7.4.8.2.2 | incoming: ring **all idle PPs attached**; per-PP call ids allowed until answer; on answer release the others and free their ids | M | first handset only [V] | | |
| M1.7 | §7.4.5.2.1/.2/.4 | outgoing: read the PP's line id (SETUP or first CC-INFO) or "None" → the FP picks a line. Confirm line + type before CONNECT. Unattached line → disconnect | M | missing [V] | | **blocker** |
| M1.8 | §7.4.5.2.3, §7.4.7.5.1 | GAP / Part 1 / non-multiline PP: `#`+digit, or FP-managed. No CALL-INFORMATION toward GAP/Part 1 | O / M | missing; no PP type (B7) | | |
| M1.9 | §7.4.7.3 table 33 | a call on line B for a PP busy on line A → call waiting; an outgoing call on B while on A → outgoing parallel call | M | missing | | needs section D |
| M1.10 | §7.4.6.1 | system-wide unique call ids | M | fixed 1 [V] | | |
| M1.11 | §7.4.7.1.0 | broadcast a34 | M if N.14 | unverified (chip) | | ask the room |
| M1.12 | §7.4.10.5.3–.6 | every call-list entry carries line id (0/00) + line name | M | n/a yet | | MVP-2 |

## MVP-2: call lists (missed / outgoing / incoming)

| ID | clause | requirement | FP | ours | evidence | finding and recommended action |
|---|---|---|---|---|---|---|
| M2.1 | table 9, §7.4.10.5.2 | serve **Missed (01) M, Incoming Accepted (03) M, All Calls (04) M**, Outgoing (02) O, All Incoming (09) O, and name them in list 00 | M | none [V] | | All Calls is mandatory for the FT R/B, so build it with the others |
| M2.2 | table 65 | Missed: 01 Number (length 1 if no CLIP), 02 Name (length 1 if none; contact match recommended), 03 Date-time (<<TIME-DATE>> octets, interpretation 0, ≥5), 04 Read status (bit 6 unread; set on a new entry), 05 Line name, 06 Line id (subtype 00), 07 Number of calls (merging strategy 1, 2 or 3) | M | — | | reuse `dect_time_put` for the field |
| M2.3 | tables 66–68 | Outgoing: Number (≥2, digits up to CS call connect), Name, Date-time, Line name, Line id. Incoming accepted: same, Number may be empty. All Calls: 01 Call type (missed/accepted/outgoing bits) + the same | M/O | — | | |
| M2.4 | table 41, Annex H.2–H.5 | commands: start/end, query, read (+ mark read status), edit/save **only for Read status** (else 07), **delete entry M, delete list M, search M** | M | — | | |
| M2.5 | §7.4.10.1 "FP side consistent update" | a call is added to (and deleted from) all implemented lists consistently, always including All Calls | M | — | | one call record, several views |
| M2.6 | §7.4.10.3, §7.4.10.4.1 | sorted by date-time, newest = index 1 | M | — | | |
| M2.7 | §7.4.1.3 | missed call notification + list change in one IE: new call (sub 01), read or delete (sub 02), after every location registration per attached line (sub 02). Multiplicity = unread entries per line | M | — | | needs I1 |
| M2.8 | §7.4.2, §7.4.20.2 | a valid FP clock, and the PP clock set | M | NTP-gated FT sync [V] | | ok; add PT-initiated (I5) |
| M2.9 | §7.4.10.6.2 | redial or call back from a list arrives as `1C15`+number in CC-INFO on the LiA call | M | **not handled** [V] | | H24 |

## MVP-3: phonebook served through list access (vCard source)

| ID | clause | requirement | FP | ours | evidence | finding and recommended action |
|---|---|---|---|---|---|---|
| M3.1 | table 69 | Contact List (05) fields: 01 Name (last), 02 First name, 03 Contact number (**multi-instance, ≥2 per entry**; props own/fixed/mobile/work), 04 Associated melody (optional; if absent, absent from every entry), 05 Line id (subtype 03 relating-to, or 04 "all lines") | M | none | | vCard N → 01/02, TEL;TYPE= → 03 props, a per-line address book → 05 |
| M3.2 | table 41 | **all** commands M: start/end, query (with instance counts), read, edit, save (create with id 0 + modify), delete entry, delete list, **search** | M | none | | |
| M3.3 | Annex H.6 note | user-defined contacts are **editable** in every field (only manufacturer entries may be read-only) | M | — | | a read-only vCard import does not meet this. Choose write-back (CardDAV) or a local overlay store. Record it as a deliberate deviation if read-only |
| M3.4 | §7.4.10.3, §7.4.10.4.1 | sort by Name, then First name, case-insensitive (UTF-8, locale-dependent) | M | — | | |
| M3.5 | §7.4.10.1 | UTF-8 storage; truncate over-long strings; dedicated NACK when the list is full; keep instance order | M | — | | |
| M3.6 | §7.4.10.6.2 | dial from the phonebook = `1C15`+number in CC-INFO on the LiA call | M | **not handled** [V] | | H24, **blocker** |
| M3.7 | EN 300 175-5 §7.7.53 n.1; §7.4.10.5.1.3 n.1 | CLIP matching a contact → that name as CNIP (screening 01) and in call lists | should | — | | cheap, once the store exists |
| M3.8 | §7.4.10.2.1 | list change notification for contacts | O (free) | — | | lets handsets refresh their caches |

---

## Top findings (ranked by impact on the MVP: two SIP lines per handset, calls in and out, caller ID)

| # | finding | ref | why it ranks here | action |
|---|---|---|---|---|
| 1 | The PP's line choice is never read (<<CALL-INFORMATION>> line id / "None" in SETUP or CC-INFO). Answers carry no line id by default. `dect_iwu` models a single line | E2, M1.1, M1.7 | without it no outgoing call can be routed to line A or B | parse the PP's 0x7E; keep a line table in `dect_iwu`; confirm line + type before CONNECT |
| 2 | Dialling from a list in a LiA call (`1C 15`+number in CC-INFO) is not handled; only `0x17` is | H24, M2.9, M3.6 | every call from the phonebook or a call list hangs in PROCEEDING | treat `1C15`+digits like dialled digits: CONNECT at once, then call id + CS call proc in CC-INFO |
| 3 | No pseudo call waiting into an open LiA session; the incoming ring waits for the CC entity | H25, K5 | handsets that sync lists block incoming calls, and serving call lists and contacts increases LiA traffic | implement §7.4.10.6.3; re-tabulate how often LiA is open after the 10-24 fix |
| 4 | No parallel call machinery (call waiting, accept/reject, hold, toggle, release by call id, negative ack) | C4, C5, D1–D11, K1–K3, G5 | mandatory (NG1.N.6/7/15). With two lines per handset, a call on line B during a call on line A **must** become call waiting | per-PP call table on one CC transaction; CC-INFO builder; parse `0x1C` codes |
| 5 | An incoming external call is not CAT-iq-shaped by default (line id + type + call id + CS call setup in SETUP; CS call connect after CONNECT-ACK). The A540 rings silently whenever a line id is present | E1, E5, E8, E9 | the user cannot see which line rang. Silent ringing blocks the default | pre-commit an A540 sweep: (a) line + linetype, (b) + callid, (c) + CS call setup (full fig. 42), each with <<SIGNAL>> 0x41. Only (c) is the standard's shape |
| 6 | CNIP shown on no handset; the A540 shows no CLIP. CLIP is M in 527-1/-3, CNIP M in 527-3 | F1, F2 | caller ID is MVP scope | sweep CLIP/CNIP in CC-INFO with call id and full callinfo; screening 01 or 11 |
| 7 | <<TERMINAL-CAPABILITY>> not parsed, so GAP, Part 1 and Part 3 PPs look the same; call status goes to every PP | B7, E11, B6, C23 | standard violation (§7.4.6.4.1) and prerequisite for UTF-8 CNIP, `#` line selection and internal call class | parse at access rights and locate; store with the subscription |
| 8 | Call id fixed to 1 on every link | E6, M1.10 | two simultaneous calls (Test 18) already share an id; with call waiting this breaks outright | allocate per call ("no re-use" method) |
| 9 | Mandatory lists and notifications missing: Missed, Incoming accepted, **All Calls**, Contacts, **DECT System Settings**; Line Settings missing 08/0B–0D; search/edit/save refused where M; list 00 incomplete; no <<EVENTS-NOTIFICATION>> (missed call, list change, MWI) | H3–H5, H14–H22, I1–I3 | MVP-2/3 scope; NACK-everything breaks §7.4.10.1 "guarantee of service" | build the list engine generically (records, fields, sort, search, edit lock), then the notifications |
| 10 | Incoming calls ring one handset; mandatory: all idle attached PPs, release the others on answer | G7, G8, M1.6 | a pool line attached to several handsets needs this | page all attached idle PPs, per-PP transaction, release losers via §7.4.3.5.4 / {CC-RELEASE} |

Just outside the top 10: ciphering fails (GAP.N.17/N.35 are M in 527-3; Test 17); tones (ring-back/busy/call waiting <<SIGNAL>> M for Part 3 PPs, C16); an internal SETUP carries 0x41 instead of 0x40 plus no internal CLIP/CNIP or call id (C22, C24, E8); codec change via {CC-SERVICE-CHANGE} missing (B5); PT-initiated date/time missing (I5); FT-initiated access rights termination missing (C21).

## MVP checklist (spec-derived, in build order)

1. Parse and store <<TERMINAL-CAPABILITY>> per handset (B7). Gate every CAT-iq element on "Part 3 PP".
2. Allocate call ids system-wide (E6). Add the call status **reason** subtype to `dect_callinfo` (D3).
3. Line table (N lines, attached-handset bitmaps from the pool) in `dect_iwu`. Line Settings fields 01, 02, 03, 08, 0B–0D. Edit/save → NACK 07 on non-editable fields (M1.1, M1.2).
4. Incoming call: page all idle attached PPs. SETUP with basic service 0x88, <<SIGNAL>> 0x41, <<CALLING-PARTY-NUMBER>>, <<CALLING-PARTY-NAME>> (UTF-8 to Part 3), and <<CALL-INFORMATION>> {line, line type 0x01, call id, CS call setup}. After CONNECT: CONNECT-ACK, then CC-INFO {call id, CS call connect}. Release the other PPs (E5, E9, G7, G8).
5. Outgoing call: read the PP's line id or "None". Answer SETUP-ACK/CALL-PROC with line + type + call id + status. CONNECT with codec + CS call connect **at SIP 200 OK** (E2, E10). On system busy use §7.4.8.3 (K8).
6. LiA voice-call transitions: `1C15`/`17`+number → immediate CONNECT, then CC-INFO statuses (H24). Incoming call during LiA → CONNECT + pseudo call waiting (H25).
7. Parallel calls on one CC transaction: CC-INFO builder; parse `0x1C` codes + call id; call waiting (K2), accept/reject/release/hold/toggle, negative ack with <<SIGNAL>> 09 (D2–D11).
8. A540 pre-committed sweep for the silent ring with a line id, and a CLIP/CNIP placement sweep (Top 5, Top 6).
9. List engine: Missed, Incoming accepted, All Calls (+ Outgoing), Contacts (vCard; editability decision M3.3), DECT System Settings. Full command set including search, mark read, delete (H3, M2.x, M3.x).
10. FACILITY/<<EVENTS-NOTIFICATION>>: missed call + list change, MWI, line-settings and internal-names list change, all re-sent after location registration per attached line (I1–I3, H5).
11. Room question: which STUB mail sets broadcast bits a34 (multiple lines) and checks a24/a29 (B9, M1.11).

**Uncertainties.** Bench claims come from `docs/bench-2026-10-25.md`; I opened no logs. Whether the 2026-10-28 A540 `lineid=1` run also carried callid/callstatus is not recorded. The LiA-collision rate comes from a code comment written before the 10-24 fix. The Gigaset behaviour hypotheses in F2 and Top 5 are not spec facts. Line numbers marked `~` are within a few lines.

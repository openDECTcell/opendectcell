> **Reviewer output, kept as written** (2026-10-28, read-only review of `1496d6e`). Line numbers refer to that commit; several findings were fixed the same day. Which items were checked by hand, and the priorities, are in `../spec-sweep-2026-10-28.md`.

# A1: openDECTcell FP against EN 300 444 V2.5.1 (GAP), audit of 2026-10-28

Scope: the host side (`src/libopendectcell`, `src/stubd/dect-stubd.c`) against the **FT R/B column** of EN 300 444
V2.5.1 (residential base, the column `docs/gap-conformance.md` already chose), with EN 300 175-5 V2.9.1 where GAP
points to it. This was a read-only review. One probe was compiled and run in the scratchpad against
`dect_cc.c`/`dect_nwk.c` (no repo files touched, no hardware).

Legend for code claims: **V** = verified by reading code, **P** = verified by a scratchpad probe that ran the library
code, **A** = assumed (not traced to the end). "chip" = MAC/DLC/PHL in the SC144xx, out of scope.

Status convention (EN 300 444 §6.1, §7): an FT feature marked O that we implement anyway becomes **process
mandatory**. Its associated procedures (timers) and exceptional cases are then mandatory too. Several findings below
come only from that rule.

---

## 1. NWK features, Table 1 / Table 5 (FT R/B column)

| ID | clause | requirement | FP M/O/C | ours | evidence | finding and recommended action |
|---|---|---|---|---|---|---|
| N.1 | 4.1, T5: 8.2 M, 8.3 O, 8.4 O, 8.5 O, 8.6 M, 8.10 M | Outgoing call: SETUP accepted, connection, keypad | M | implemented: `dect_cc.c:on_setup:649`, `accept_now:729`, `dect_cc_rx:921` (V) | test_cc `test_overlap_receiving`, `test_handset_call_in_steps`, `test_collects_the_dialled_digits_from_a_real_capture`. Bench: G7 calls, tests 15/18 2026-10-28 | Conformant in the normal case. See §2 for the 8.3/8.6 deviations and for the follow-on-call defect (F-1) |
| N.2 | 8.2, 8.15 | Off hook | M | implemented (V) | as N.1/N.8 | none |
| N.3 | 8.7, 8.8 | On hook, full release | M | partial: `on_release:792`, `dect_cc_release:1144`, RELEASE-COM case `:1076` (V) | `test_release_from_pp`, `test_release_from_fp` | No F-<CC.02> (8.7.1.2, 36 s). After release the CC entity stays in RELEASED, which blocks any further call on the same link (F-1) |
| N.4 | 8.10 | Dialled digits 0-9 * # in `<<MULTI-KEYPAD>>`, F-02 and F-10 | M | implemented: `learn_keypad:512`, INFO case `:1040` (V) | 5 unit tests; bench dialling 2026-10-25/28 | Done. Digits reach `dect_cc_keypad()`. There is no consumer yet (no SIP) |
| N.5 | 8.10, keypad 15H | Register recall | O | not interpreted. The byte is collected as a digit (V) | none | Optional. NOTE 5 says the FT need not understand it. The controller must not dial 0x15 as a digit |
| N.6 | 8.10, keypad 14H | Go to DTMF | O | not interpreted (V) | none | Optional for R/B |
| N.7 | 8.10, keypad 05H | Dialling pause | O | not interpreted (V) | none | Optional. Strip control codes before SIP |
| N.8 | 8.12, 8.13, 8.14, 8.15 | Incoming call | M | implemented: `dect_cc_setup_id:227`, ALERTING `:985`, CONNECT/CONNECT-ACK `:994` (V) | `test_outgoing_call_to_handset`, `test_setup_timer_*`; bench runs 15-24, all three Gigasets ring | Conformant. Basic service is wideband for every handset (F-6). The only trigger is the lab `ring=` timer, which has no busy guard (F-2) |
| N.9 | 8.24 | Authentication of PP (DSAA) | C101 → O | partial: `send_auth_request_fp:545`, `on_auth_reply_cipher:583` (V); used only as the first step of ciphering | `test_cipher` | No F-<MM_auth.1> (10 s). On a RES mismatch the call is **not** interrupted, although 8.24.2.3 (numbered "8.23.2.3" in the spec) says it shall be. `<<AUTH-TYPE>>` octet 5 is wrong for storing the DCK (F-5) |
| N.10 | 8.25 | User authentication (UPI) | O | missing | none | Optional, fine |
| N.11 | 8.28 (M), 8.29 (O), 8.17 (O) | Location registration | O | implemented: `on_locate_request:481`, `send_locate_accept:383`, `dect_mm_tick:772` (V) | test_mm `test_locate*`, `test_tpui_*`; every bench run | Process mandatory. `<<LOCATION-AREA>>` is sent only if the PT sent one, and then only as an echo of the PT's *old* LAL (F-8). TEMPORARY-IDENTITY-ASSIGN-REJECT is ignored (the timer covers it). 8.29 location update is not implemented, which is allowed |
| N.12 | 8.32 | On-air key allocation | C101 → O | implemented: `send_key_allocate:169`, `on_auth_request:664` (V) | `test_register_ok`, `_bad_pin`, `_8_digit_pin`; bench test 20 | Table 56 values are correct (`<<ALLOCATION-TYPE>>` 01 88, RAND 8 octets, RS 8 octets). Missing: F-<MM_key.1> (10 s, 8.32.1.1) and handling of a PT `{AUTHENTICATION-REJECT}` (8.32.2.3/4). Both are ignored and MM stays in WAIT_AUTH until the next request. Low |
| N.13 | 8.22 | Identification of PP | O | missing | none | Optional |
| N.14 | 8.30, 8.24 | Service class indication/assignment | O | missing (no `<<SERVICE-CLASS>>`) (V) | none | Optional |
| N.15 | 8.14 | Alerting | M | implemented: `<<SIGNAL>>` 0x41 in SETUP, `dect_cc_init:26`, `dect_cc_setup_id:277` (V) | bench run 18 (silent without), runs 19+ ring | Fixed. `gap-conformance.md` row 8.14 still says "open" and is stale. Internal calls also get 0x41, but Table 23 gives 40H for internal (F-10) |
| N.16 | 8.26, 8.30, 8.23 | ZAP | O | missing | none | Optional |
| N.17 | 8.33, 8.27 | Encryption activation, FT initiated | C101 → O | partial: `send_cipher_request:564` (`19 02 81 98`, Table 61 correct) (V). Off by default in the service (`opendectcell.init:162`) | bench test 17 fail: link drops 212 ms after CIPHER-REQUEST | Known: the chip never gets the DCK. Two further deviations: the DCK is stored under the wrong key number (F-5), and there is no F-<MM_cipher.1>. 8.33.2.2: "should not release the call" on expiry; we do not release, which is fine |
| N.18 | 8.30, 8.17 | Subscription registration on air | M | implemented: `on_access_rights_request:625`, `send_access_accept:294` (V) | bench tests 16/20, G6 | Table 51: `<<PORTABLE-IDENTITY>>` is sent only `if (mm->port_id_len)` (`:304`). A request without the IE would get an ACCEPT without it, but such a request cannot reach `on_auth_request` usefully. Table 50: `<<AUTH-TYPE>>` "UAK while FT only has AC → shall not accept" is never examined. Low |
| N.19 | 8.35, 8.36, 8.37, 8.38, 8.39 all M | Link control | M | partial (see §2 rows 8.35-8.39) | `test_page_reject_is_built_from_the_response`; bench | 8.37/8.39 are missing: no FT-side release after the last higher entity, no <LCE.01>, no <LCE.02>, and DISC discards outstanding data. **8.39 "maintain" is M and was not listed in `gap-conformance.md`** (F-4) |
| N.20 | 8.31 | Terminate access rights FT initiated | O | missing | none | Optional. Handsets are removed locally only (assumed) |
| N.21 | 8.9 | Partial release | O | missing: we send no reason 0EH and keep no <LCE.02> (V) | none | Optional. A PT partial release is treated as a normal release |
| N.22, N.23 | 8.10 | DTMF infinite, pulse | O | not interpreted | none | Optional |
| N.24, N.25 | 8.16, 8.17 | Display characters, control | O | missing (no `<<MULTI-DISPLAY>>`) | none | Optional |
| N.26 | 8.23 | Authentication of FT | O | only inside key allocation. A stand-alone PT `{AUTHENTICATION-REQUEST}` is ignored (`dect_mm_rx:730` requires WAIT_AUTH) (V) | none | Allowed by 6.9.4 para 1 (ignore an MM procedure we do not support) |
| N.27-N.29 | 8.33, 8.34 | Encryption PT initiated / deactivation | O | missing | none | Optional |
| N.30 | 8.12, 8.41 | CLIP | O | implemented in SETUP: `dect_clip_put_number`, `put_identity:200` (V) | test_clip; bench test 21: number shown on C430A/AS690H, **not on the A540** | Known. 8.41 lets the FT use SETUP or CC-INFO, and the PT must support both. Next discriminating test: the same IE in a `{CC-INFO}` after ALERTING |
| N.31 | 8.18 (M), 8.19 (O), 8.43/8.44 (O) | Internal call | O | implemented: class 9 and the 0x17 key (`learn_int_key:541`, API 28), routing `dect-stubd.c:ic_start:5288` (V) | bench test 19 retry pass, both directions | Process mandatory. No internal CLIP/CNIP to the target (`ic_setup` passes `NULL` identity). The SIGNAL is 0x41, not 40H. The target always gets class 9 / 0x98 even if it is a GAP PP (TS 102 527-1 §7.1 NOTE: use "external call" for a GAP PP) (F-10) |
| N.32 | 8.20 | Service call (class 11) | O | not recognised: class B falls through `on_setup` and is **connected** as a call without speech (V) | none | Minor. Reject with RELEASE-COM 05H "incompatible service" or 06H "service not implemented" instead |
| N.33 | 8.40 | Enhanced U-plane connection (PROGRESS-INDICATOR 8H) | O | missing (V) | none | Optional, but it is the GAP way to give SIP early media/ringback (Annex B). Needed for C5 |
| N.34 | 8.42 | CNIP | O | implemented in SETUP: `dect_clip_put_name` (V) | bench test 21: **name shown on no handset**, with either alphabet | Known. Default alphabet is UTF-8 (001, `dect_clip_init`), which a GAP PP may not know. The DECT alphabet (000) was also tried and also failed, so the alphabet is not the cause. Try `{CC-INFO}` per 8.42 |
| N.35, N.36 | 8.45 | Enhanced security, DSAA2 | O / C102 → O | missing | none | Optional. Do not broadcast extended capabilities part 2 |

---

## 2. §8 procedures the FT takes part in (IEs and values)

| ID | clause | requirement | FP M/O/C | ours | evidence | finding and recommended action |
|---|---|---|---|---|---|---|
| P-8.2 | 8.2, T10 | Accept PT SETUP: IPUI type 0, PARK type 32, basic service class 8/9/11, service 0 | M | `dect_cc_rx:931-953` learns identity, basic service, codec, keypad (V) | `test_learns_identities_from_setup` | A SETUP with mandatory IEs missing is not rejected (175-5 §17.6.1 says "should" return RELEASE-COM 03H). It is rejected only when the gate is armed and the identity is absent (`setup_from_stranger:881`). Low |
| P-8.2.2.3 | 8.2.2.3, T17 | FT rejects a PT call: **abnormal** release, `{CC-RELEASE-COM}` from F-01. The reason is "all optional" | M | `on_setup:668-673`: RELEASE-COM 0x32 when `cc->busy` (V) | `test_busy_link_refuses_speech_and_takes_list_access`; bench 18 (side link busy without `twocalls`) | Conformant. See §4(a) |
| P-8.3 | 8.3, T11; 175-5 §9.3.1.5 | Overlap sending: SETUP-ACK (PROGRESS-IND 8H optional), restart F-<CC.01> on each INFO | O (we implement) | `on_setup:685-691`, INFO case `:1064-1073`, `dect_cc_tick:812` (V) | `test_overlap_timer`, `test_overlap_receiving` | Timer correct (20 s, restarted on INFO) |
| P-8.3.2.3 | 8.3.2.3 Fig 12; 175-5 §9.3.1.5 last para | On F-<CC.01> expiry the FT uses **normal** release: `{CC-RELEASE}` "timer expiry" | process M | **deviates**: `dect_cc_tick:816-821` sends `{CC-RELEASE-COM}` 0x0D and goes to NULL. The code comment says "normal release". `test_overlap_timer` asserts RELEASE-COM (V) | test encodes the deviation | Send `{CC-RELEASE}` 0x0D and wait for RELEASE-COM (F-<CC.02>). Fix the test. Low impact: the PT clears either way |
| P-8.4/8.5 | 8.4, 8.5, T12/T13 | CALL-PROC / ALERTING, PROGRESS-IND optional | O | `proceed:700`, `dect_cc_alert:751` (V) | `test_handset_call_in_steps` | 175-5 §9.3.1.7: ALERTING "shall only be sent if the U-plane resources are fully installed". The `alertms=` path sends it before the bind. Low |
| P-8.6 | 8.6, T14; 175-5 §9.3.1.8 | "Before sending the {CC-CONNECT} message the FT shall connect the U-plane" | M | **deviates by default**: the bind triggers on ACTIVE, i.e. after CONNECT (`dect-stubd.c` ≈6123-6127). Only `-a bindfirst` binds first (`connect_tick:2751`) (V) | bench: audio works, but the start is not measured | Make bind-before-CONNECT the default (or justify the deviation). Also: with `<<SENDING-COMPLETE>>` or `nooverlap`, `on_setup` connects at once (`accept_now`). An IWU (SIP) has no chance to defer the CONNECT until the far end answers (F-7) |
| P-8.7 | 8.7, T15/T16 | Normal release both ways. FT not in F-00/F-01/F-19. F-<CC.02> 36 s | M | `dect_cc_release:1144` sends RELEASE without a reason, enters RELEASED. `on_release:792` answers RELEASE-COM (V) | `test_release_from_*` | Missing F-<CC.02> (on expiry send RELEASE-COM). Release collision (8.7.2.1, 175-5 §9.5.3): on a PT RELEASE in RELEASE PENDING we still answer RELEASE-COM (P: probe printed `83 5a`), where the standard says go to NULL without answering. Harmless but non-conformant |
| P-8.8 | 8.8, T17 | Abnormal release. Unknown PT: recommend reason 0AH | M | RELEASE-COM case `:1076-1114`; `setup_from_stranger` uses 0x0A (V) | `test_gate_turns_away_another_handset` | Conformant |
| P-8.10 | 8.10, T20 | Keypad in F-02 and F-10, en-bloc in one MULTI-KEYPAD | M | INFO accepted in CALL_PRESENT, CONNECT_PENDING, ACTIVE, OVERLAP, PROCEEDING (`:1052-1057`) (V) | 5 tests + capture replay | Conformant. The code comment calls CALL_PRESENT "F-02", which is wrong (F-02 = overlap sending, which is also covered). Cosmetic |
| P-8.12 | 8.12, T21 | FT SETUP: IPUI, PARK type 32 + whole PARK, basic service class 8/service 0, SIGNAL 40-48/4F, CLIP if N.30 | M | `dect_cc_setup_id:227`; identity bytes `dect_mm_fixed_identity:283` (type 0xA0, length 37 = 36+1) (V) | `test_setup_ie_order_is_ascending`; bench | Basic service defaults to **0x88** (wideband) for every PT (`dect-stubd.c:7938`). Table 21 allows only service 0, and TS 102 527-1 §7.1.1 requires GAP interop (F-6). PARK length "PLI+1": we hard-code 37. This is correct only if PLI = 36 for our class-A PARK. **A**: not checked against the broadcast PLI |
| P-8.12.1 | 8.12.1.1, 8.12.2.3 | F-<CC.03> 20 s, on expiry abnormal release (RELEASE-COM, "timer expiry") | M | `dect_cc_tick:835-848` (V) | 3 tests (`test_setup_timer_*`) | Conformant |
| P-8.12.3 | 8.12.3 | Collective/group ringing | O | missing | none | Optional |
| P-8.13 | 8.13, T22 | Accept ALERTING | M | `:985-993` (V) | bench | Conformant |
| P-8.14 | 8.14, T23 | SIGNAL in SETUP or in CC-INFO at F-07, FT one method | M | SETUP method, 0x41 (V) | runs 18-24 | Conformant for external calls. Internal: 40H expected (F-10) |
| P-8.15 | 8.15, T25/T26 | Accept CONNECT, send CONNECT-ACK | M | `:994-1008` (V) | bench | Conformant |
| P-8.17 | 8.17, T28/29 | FP "able to receive" `<<TERMINAL-CAPABILITY>>` in ACCESS-RIGHTS-REQUEST and LOCATE-REQUEST | M (within N.18) | received and ignored, no parse error (V) | bench | Conformant as far as "receive" goes. Not used to tell a GAP PP from a CAT-iq PP, which F-6 needs (TS 102 527-1 §7.1 NOTE 2/3) |
| P-8.18/8.19 | 8.18 T30, 8.19 T31 | Internal call class 9; keypad 17H | O (we implement) | `on_setup:680`, `learn_int_key:541` (V) | `test_int_key_makes_an_internal_call`, `test_internal_call_waits_for_the_owner`; bench 19 | Conformant (already known, API 28). Refusals in `ic_start` use RELEASE **without reason** from F-03. Add 14H "user busy" or 12H "user unknown" so the handset can say why |
| P-8.24 | 8.24, T39/T40, 8.27 T43 | AUTH-REQUEST: alg 1, key type 1 (UAK), key no 8, UPC=1 and **cipher key number 8** when storing the DCK, RAND 8, RS 8 | O→process M | `AUTH_TYPE_CIPHER {01 18 10}` (`dect_mm.c:541`): octet 5 = 0x10 = UPC 1, cipher key number **0** (V, against 175-5 §7.7.4 fig 28) | `test_cipher` (does not check octet 5) | **Deviation (F-5)**: Table 43 requires cipher key number 8 (octet 5 = 0x18). The CIPHER-REQUEST then asks for key 8 (`98`). The PT is told to store the DCK as key 0 and to cipher with key 8. Already marked "flags (TBD)" in the code |
| P-8.28 | 8.28, T45 | LOCATE-ACCEPT: TPUI type 32 length 20 (or zero length), **`<<LOCATION-AREA>>` LI-type 1, LAL 0-39 "even if default"** | process M | TPUI `a0 94 …` correct. LA only `if (la && la_len)` (`dect_mm.c:398`), copied from the PT's request (V) | `test_locate`; every bench run | **Deviation (F-8)**: 175-5 Table 54 makes Location area M. Table 44 says the PT sends its *old* LAL. Always send our own LAL (the value the chip broadcasts) |
| P-8.28.1.2 | 8.28.1.2, 8.28.2.4 | F-<MM_ident.1> 10 s | process M | `dect_mm_tick:772` (V) | `test_tpui_assignment_times_out_at_ten_seconds` | Conformant. TI-ASSIGN-REJECT should stop it (not handled). Low |
| P-8.28.2.1 | 8.28.2.1 | Reject unknown PT, recommended reason 02H | process M | `on_locate_request`: 02H unknown, 60H IE missing (V) | `test_locate_request_*` | Conformant |
| P-8.30 | 8.30, T51/T52 | ACCESS-RIGHTS-ACCEPT: IPUI, PARK type 32 whole PARK. Reject "all optional" | M | `send_access_accept:294`, `send_access_reject_reason:318` (V) | bench 16/20 | Conformant in practice; see N.18 |
| P-8.32 | 8.32, T56-T58 | KEY-ALLOCATE, AUTH-REQUEST from PT, AUTH-REPLY RES2, UAK = KS' | process M | as N.12 (V) | test_mm | Missing MM_key.1 and AUTH-REJECT handling (low) |
| P-8.33 | 8.33, T61 | CIPHER-REQUEST Y/N 1, alg 1, key type 9, key no 8. Should not release on reject or timeout | process M | `send_cipher_request:564` `81 98` (V). CIPHER-REJECT ignored, call kept (V) | bench 17 | IE correct. Chip DCK load missing (known), plus F-5 |
| P-8.35 | 8.35, T66-T68; 175-5 §14.2.3 | Short page only, LCE header 0 or 4. <LCE.03> 3 s, up to N300 resubmissions. On IPUI mismatch: LCE-PAGE-REJECT with full IPUI, same TV, then normal link release | M | page: chip mail 0x0300 (header = chip). Resubmission every 3000 ms, up to 20 times (`RING_PAGE_INTERVAL/RETRIES`, `dect-stubd.c:3728-3729`). Reject: `dect_link_page_reject:131` + `dect_lapc_disconnect` (V) | `test_page_reject_is_built_from_the_response`; reject not seen on air | N300 = 20 is allowed (application specific; 3 is recommended). The LCE header value is chip territory (Gap 5, settled). The reject's link release is a DISC, not a "normal" release (see 8.37) |
| P-8.36 | 8.36 | Direct PT-initiated establishment; on failure inform higher entities | M | chip + `dect_link_rx_segment:192`. CONN_DOWN clears `reg.active`, the link is re-initialised per connection (`dect-stubd.c` ≈4848) (A for the full path) | every bench run | Conformant (A) |
| P-8.37 | 8.37, T72-T77 | Link release "normal": DLC completes outstanding data. <LCE.01> 5 s, then abnormal | M | `dect_lapc_disconnect:210` drops outstanding data and queues DISC (P/F set). No <LCE.01> (V) | 2026-10-23 bench: DISC failed (spec-conformance open item) | **Missing (F-4)**. Already open as "Class B DISC on a Class A link" in `spec-conformance.md`. Class A release = MAC connection release, which is a chip mail question for the room |
| P-8.38 | 8.38 | Link release "abnormal" | M | DISC is effectively this (V) | as above | Same open item |
| P-8.39 | 8.39; 175-5 §14.2.7 | Every higher entity notifies the LCE when done. If nobody needs the link and no LCE timer runs, the LCE **shall release** it. Partial release → <LCE.02> 3-10 s, then abnormal release | M | **missing**: the FP never releases a link after CC/MM finish. The only releases are before paging (`ring_tick`, `dect-stubd.c:4033`) and after a page reject (V) | spec-conformance [CR A-0053]: "a linked handset does not answer pages" | **Gap in a mandatory feature (F-4)**. It is also why the ring path has to DISC the link before every page (that DISC kills a live call, F-2) |
| P-8.41/8.42 | 8.41 T79, 8.42 T80/T81 | CLIP/CNIP in SETUP or CC-INFO. If it does not fit, an extra CC-INFO "shall be used" | O→process M | SETUP only. An IE that does not fit is silently dropped (`put_identity` "never fatal") (V) | test_clip; bench 21 | 8.41/8.42: "an additional {CC-INFO} message shall be used" when it does not fit. We drop it instead. Implement the CC-INFO path. That is also the next CNIP test |
| P-8.43/8.44 | 8.43 T82, 8.44 T83 | Internal call CLIP (terminal id) and CNIP (stored name, else "DECT n"), alphabet 000 | O (we do N.31) | missing (`ic_setup` → `dect_cc_setup_id(..., NULL)`) (V) | none | Cheap. We already hold "DECT n" names in the Internal Names List |

---

## 3. General requirements (6.9, 7, 8.0) and the EN 300 175-5 rules they import

| ID | clause | requirement | FP M/O/C | ours | evidence | finding and recommended action |
|---|---|---|---|---|---|---|
| G-1 | 6.1, 7 | Implemented optional elements are process mandatory, including associated timers and exceptional cases | M | see the timer rows below | — | Drives the missing-timer findings |
| G-2 | 6.9.1 | Reserved bits set to 0 | M | `dect_nwk_build_hdr`; CLIP/CNIP octet-3 bit 8 = 0 (V) | test_clip vectors | Conformant as far as read |
| G-3 | 6.9.2 | CC transaction value = lowest free | M | FT side always TV 0 per CC entity (`dect-stubd.c` ≈3909, `ic_setup` ≈5377) (V) | `test_transaction_value_is_lowest_free`; runs 15/16 | Conformant. `test_transaction_value_rotates` still exists and should be checked to confirm it does not test the old behaviour (A) |
| G-4 | 6.9.3 | Receive at least 63-octet messages; extra segments may be discarded | M | LAPC reassembly, bounded (`e5bc10b`) (A, per spec-conformance) | unit-tested | Conformant |
| G-5 | 6.9.4 para 1 | Ignore an MM message initiating an unsupported procedure | M | `dect_mm_rx` default `break` (V) | — | Conformant |
| G-6 | 6.9.4 para 2; 175-5 §17.4.1 | Unexpected or unrecognised CC message (except RELEASE/RELEASE-COM) **shall be ignored** | M | own transaction: default branch ignores (`:1115-1130`). Unknown transaction: RELEASE-COM 02H (175-5 §17.3.2.1 "should") (V) | `test_unhandled_message_on_our_call_is_ignored`, `test_closes_a_facility_transaction` | Fixed (`0ac7e38`). **`gap-conformance.md` row 6.9.4 still says "open" and is stale.** Gap: a SETUP arriving while the entity is RELEASED is neither ignored correctly nor accepted (F-1) |
| G-7 | 6.9.4 para 3; 175-5 §17.6.1/2 | Non-SETUP/RELEASE message with a mandatory IE missing or invalid → normal release (or keep the call, "alternatively") | M | not checked. Messages are acted on with defaults (V) | none | Low. `{CC-INFO}` has no mandatory IE, CONNECT/ALERTING have none in GAP. Nothing practical breaks |
| G-8 | 6.9.4 para 5 | A reserved value is not an error by itself; process it or ignore it | M | `dect_cc_peer_is_speech` treats unknown classes as non-speech, call proceeds (V) | `test_peer_basic_service_decides_speech` | Conformant. But an unknown call class gets CONNECTed (see N.32) |
| G-9 | 6.9.5 | IWU-ATTRIBUTES/CALL-ATTRIBUTES need not be understood; Annex E defaults | M | not parsed, not sent (V) | — | Conformant |
| G-10 | 6.9.6, Table 9 | MM procedures in all CC states (identification, auth, locate, cipher); access rights only in F-00 | M | MM is independent of CC (V) | bench (locate during calls) | Conformant |
| G-11 | 6.9.7 | First octet of each octet group present; no truncation by length | M | encoders emit full groups (V for CLIP/CNIP, CODEC-LIST, AUTH-TYPE) | tests | Conformant |
| G-12 | 8.0; 175-5 §17 | Ignore an unsupported or unrecognised NWK message or IE | M | `dect_nwk_find_ie` skips unknown IEs (V) | — | Conformant |
| G-13 | 175-5 §17.3.1 | TV 7 (extension) → ignore the message | M (imported) | not filtered; TV 7 is treated as a value (V) | none | Low |
| G-14 | 175-5 §17.3.2.1 | SETUP with TI flag = 1 → ignore | M (imported) | accepted as a call (P: probe connected it) | none | Low. Add a flag check in the SETUP case |
| G-15 | 175-5 §17.3.2.1 | Repeated SETUP on a live transaction → ignore | M | `our_transaction` → no action (V) | — | Conformant |
| G-16 | 175-5 §17.3.3 | SETUP for a new transaction while a call is in progress/active → RELEASE-COM "insufficient resources" (32H), same TV; call continues | M | `:954-983` (V) | `test_second_setup_during_an_active_call_is_rejected` | Conformant **while a call is in progress**. **Also fires after the call has ended (state RELEASED)** and rejects a legitimate follow-on call (P, F-1) |
| G-17 | 175-5 §9.5.2.3 | Setup collision: FT releases the PT call with 32H, re-presents its own at once | M | `:980`, re-presentation `:1094-1111` (V) | `test_setup_collision_*`, `test_insufficient_resources_represents_the_setup` | Conformant |
| G-18 | 175-5 §17.9 | Data link failure: clear non-ACTIVE calls, clear ACTIVE on a normal release | M | link re-initialised per connection; CONN_DOWN checked against its handle (`12cb980`) (A) | bench | Conformant (A) |
| G-19 | 175-5 A.1/A.5/A.7 | FT timers: CC.01 20 s ✓, CC.02 36 s ✗, CC.03 20 s ✓, CC.04 opt, MM_ident.1 10 s ✓, MM_key.1 10 s ✗, MM_auth.1 10 s ✗, MM_cipher.1 10 s ✗, LCE.01 5 s ✗, LCE.02 3-10 s ✗, LCE.03 3 s ✓ | M for implemented procedures | as listed (V) | timer tests for CC.01/CC.03/MM_ident.1 | Implement CC.02, LCE.01/02 first (they affect link and call state). The MM timers only matter for stuck procedures |

---

## 4. DLC, MAC, PHL, application, speech (Tables 2-4a) and clauses 13-14

| ID | clause | requirement | FP M/O/C | ours | evidence | finding |
|---|---|---|---|---|---|---|
| D.1 | 9.1-9.3 | LAPC class A + Lc | M | **host**: `dect_lapc.c`, `dect_lc.c`; DL.04 = 2 s (`dect_lapc.c:136`) (V) | test_lapc, test_lc | Covered by `spec-conformance.md`; class A release open (P-8.37) |
| D.2 | 9.5 | CS fragmentation/recombination | M | split host (`dect_cs.c`) / chip (A) | test_cs | none new |
| D.3-D.5, D.7, D.8 | 9.6-9.10 | Lb broadcast, handover, LU1, FU1 | M/C201/O | N/A (chip) | — | — |
| D.6, D.9 | 9.8 | Encryption activation/deactivation | C203/C202 → I | N/A (chip); DCK hand-off is the open defect | bench 17 | — |
| M.1-M.17 | 10 | MAC services | various | N/A (chip) | — | — |
| PHL | 11, Annex C | PHL, FP sync | M | N/A (chip) | — | — |
| A.1 | 14.2 | AC to bitstring: BCD, leading F padding, 32 bits | C401 → **M** (we do N.9/N.12) | `dect_dsaa_pin_to_ac:210` (V) | `test_register_8_digit_pin`; bench 20 (RES1 reproduced for 0000 and 31415926) | Conformant |
| A.4 | 14.4, T130 | Terminal identity number = low 8 bits of the assigned TPUI | O | TPUI `{0, 0x11, slot}` (`dect_mm_tpui:370`); "DECT n" names (V) | `test_sub_slot_drives_the_tpui` | Conformant |
| SC.1 | 6.7 | G.726 | M | chip/DSP; codec choice in `choose_codec:395` (V) | bench audio | — |
| SC.8-SC.13 | 6.7 | ≥1 FP audio type (1b/3 recommended for VoIP) | C405 | not characterised (A) | none | Echo/loss plan for SIP (type 3, EN 300 176-2) is untested. Note for C5 |
| SC.14 | 6.7 | FP audio type 6a (internal call) | C407 → M (we do N.31) | bridged internal call (`connected, bridging`) (A) | bench 19 audio both ways | No measurement against EN 300 176-2 |
| 13.6 | 13.6.1 | Broadcast a32/a33/a36/a37/a38/a44 | M | chip, partly host-configured (A) | — | Out of scope |
| 13.5 | 13.5 | FT initiates and stores the DCK | M for N.17 | as N.17 | — | — |

---

## 5. The two specific questions

### (a) The FP cannot accept a handset-placed call for lack of resources

| source | what it says | value |
|---|---|---|
| EN 300 444 **§8.2.2.3** "FT rejects the outgoing call request" | "The abnormal call release procedure shall be used (see clause 8.8)": the FT answers the {CC-SETUP} with **{CC-RELEASE-COM}**, F-01 → F-00. "Either F-CC or the F-IWU may reject the call." | Table 17: `<<RELEASE-REASON>>` "All optional" |
| EN 300 444 **§8.7.0** | "The FT is allowed to initiate this procedure [normal release] in any state except F-00, **F-01** and F-19." So a {CC-RELEASE} is *not* allowed in answer to the SETUP | — |
| EN 300 444 §8.3.2.2 / §8.4.1.2 | Rejection in overlap sending (F-02): RELEASE-COM again. After CALL-PROC/ALERTING (F-03/F-04): normal release ({CC-RELEASE}) | — |
| EN 300 175-5 **§9.3.1.2** "Call reject" | "If the F-CC cannot meet any of the setup requests … the FT shall reject the call setup by sending a {CC-RELEASE-COM} message, and shall enter the 'NULL' state. The MNCC_REJECT-req may include a release reason … and this should be included." | the reason from the F-IWU |
| EN 300 175-5 **§17.3.3** "Call Resource Contention" | A {CC-SETUP} for a new transaction while a call is beyond CALL INITIATED or active: "the call shall continue, and the request shall be rejected by sending a {CC-RELEASE-COM} message which should contain the release reason **'insufficient resources'**, using the same (unknown) transaction value" | **32H** (§7.7.18 temporary-overload block; neighbours 31H Overload, 33H Insufficient bearers available, 34H IWU congestion) |
| ours | `on_setup:668-673`: `cc->busy` → RELEASE-COM 32H, NULL. `:954-983`: §17.3.3 path, 32H. Side links are busy by default (`dect-stubd.c:4999`). List-access calls are still taken (`:662`) (V) | 32H. **Conformant.** Only flaw: the same 32H also rejects follow-on calls after a finished call on the same link (F-1) |

### (b) The FP must place a call to a handset that is already in a call

| source | what it says | value |
|---|---|---|
| EN 300 444 | **No clause covers it**: the word "busy" does not occur in the profile. The FT procedures (8.12) assume a PT in T-00. §8.12.3 (collective ringing): "A PP that has already established a call, should interpret a collective or group ringing request as an unexpected message and shall not initiate ringing." | — |
| EN 300 444 §6.9.2 + EN 300 175-5 §7.3 NOTE 4 | A second FT-initiated CC transaction would take the lowest free TV. The PT's call uses the other flag, so TV 0 is free on the FT side unless *we* placed the first call | TV 0 or 1 |
| EN 300 175-5 **§17.3.3** (PT side, same text as above) | The busy PT keeps its call and rejects our SETUP with {CC-RELEASE-COM}, release reason **"insufficient resources"** | **32H** from the PT |
| EN 300 175-5 §7.7.18 | Value for the condition itself, should the F-IWU report it, or for an FT release: **"User busy"** | **14H** (13H "User already active") |
| Practical reading | The F-IWU knows the PT is in a call, so the right FT behaviour is **not to send a SETUP** and to return busy to the network (SIP 486). If it does send one, it must accept the PT's RELEASE-COM 32H as "busy". Call waiting is CAT-iq (TS 102 527-1), not GAP | — |
| ours | `dect_cc_setup_id:236` refuses while the CC entity is not idle, so no SETUP goes out on a busy entity. But the only incoming-call trigger, `ring_tick:3933`, has **no busy check**: in RING_IDLE it sends a DISC on any live main link (`dect-stubd.c:4033-4040`) to page from idle. **A ring that fires during a call tears the call down** (V). No busy result is reported anywhere, and there is no SIP side yet (known) | F-2 |

---

## 6. Re-check of earlier "fixed/done" claims

| claim (doc) | current code | verdict |
|---|---|---|
| N.4 implemented, API 6 (gap-conformance) | `learn_keypad:512`, INFO case `:1040` | holds |
| 8.14 "open, default sends no alerting" (gap-conformance, last table) | `dect_cc_init:26` signal on, 0x41; daemon default `signal_on = 1` | **doc stale**: fixed in `9c4414c` |
| 6.9.4 "open, default branch inverted" (gap-conformance) | `our_transaction:914`, default branch `:1115` | **doc stale**: fixed in `0ac7e38` |
| F<CC.03> implemented (spec-conformance) | `dect_cc_tick:835` | holds |
| DL.04 = 2 s (spec-conformance) | `dect_lapc.c:136` | holds |
| MM_ident.1 + TPUI ACK (spec-conformance) | `dect_mm_tick:772`, ACK case `:742` | holds |
| LOCATE-REQUEST without identity rejected 60H (an external review #1) | `on_locate_request:499-501` | holds |
| LCE-PAGE-REJECT (an external review #10) | `dect_link_page_reject:131` | holds (built from the full IPUI, same TV, as Table 68 requires) |
| 0x17 internal-call key (API 28) | `learn_int_key:541` | holds |
| "Eight mandatory features, all implemented" (gap-conformance) | N.19 omits 8.37/8.39; N.3 lacks CC.02; N.1 has the follow-on defect | **overstated**: N.19 is partial |

---

## Top findings (ranked by impact on a GAP/CAT-iq user making and receiving SIP calls)

| rank | ID | finding | impact | action |
|---|---|---|---|---|
| 1 | **F-1** | **A second call on the same link never works.** After any call ends (normal or abnormal release, list access included) the CC entity stays in `DECT_CC_ST_RELEASED`, and `RELEASED` is never reset to `NULL` except by re-initialising the link. A new PT SETUP with TV 0, which 6.9.2 makes the normal case, is silently ignored (it matches `our_transaction`). With any other TV it is rejected with RELEASE-COM 32H. **P**: probe output `call2(tv0)`: nothing; `call2(tv1)`: `93 5a e2 32`. Code: `dect_cc.c:932` tests `== NULL` only; `on_release:795`, `:1112`, `dect_cc_release:1149` set RELEASED | Hang up and redial before the PT drops its link, or a voice call right after a CAT-iq list-access call on the same link (the A540 opens one every ~17 s), and the call fails: silence for 20 s, or an immediate "insufficient resources". The bench has not caught it because links are usually re-created per call (assumed) | Treat RELEASED as NULL for an incoming SETUP (or go to NULL once RELEASE-COM is sent or received). Add a unit test "release, then SETUP TV 0, then SETUP TV 1" |
| 2 | **F-2** | **Calls to a busy handset: no handling, and the ring path tears down the live call.** `ring_tick` in RING_IDLE DISCs any live main link before paging, with no `dect_cc_busy` check (`dect-stubd.c:4033-4040`). GAP itself is silent; 175-5 §17.3.3 has the PT reject with 32H | When the SIP side is added on top of today's ring logic, an incoming SIP call during a conversation drops the conversation | Before paging: if the target's CC is busy, do not page; report "user busy" (14H / SIP 486). Map a PT RELEASE-COM 32H to the SIP busy response. Call waiting later via CAT-iq |
| 3 | **F-3** | CNIP never shown and CLIP not shown on the A540 (known). We use only the SETUP method, and drop the IE when it does not fit, although 8.41/8.42 say an additional {CC-INFO} "shall be used" | Every incoming SIP call shows no name (and on the A540 no number) | Implement CLIP/CNIP in `{CC-INFO}` (after ALERTING, F-07). That is both the conformance fix and the next discriminating bench test (pre-commit: name appears in INFO only → placement; still nothing → content) |
| 4 | **F-4** | **N.19 link control is partial (mandatory).** No FT-side link release when CC/MM are done (8.39), no <LCE.02>, no <LCE.01>, and DISC drops outstanding data instead of a "normal" release (8.37). Class A release is really a MAC connection release (open room question) | Links linger. Pages go unanswered ([CR A-0053]), which forced the DISC-before-page workaround behind F-2. A final RELEASE-COM can be lost when we DISC | Implement LCE 8.39 (release after the last higher entity, plus <LCE.02> for partial release) on the chip's connection-release mail; then drop the DISC-before-page. Ask the room which mail does a class A release |
| 5 | **F-5** | Storing the DCK: `<<AUTH-TYPE>>` octet 5 = `0x10` (UPC 1, cipher key number 0), but Table 43 needs cipher key number **8** (`0x18`), and our CIPHER-REQUEST asks for key 8 (`dect_mm.c:541`). Auth failure does not interrupt the call (8.24.2.3) | Ciphering (needed by some handsets and by the CAT-iq security expectations) may fail on this even after the chip gets the DCK. One variable less to chase in test 17 | Set octet 5 to 0x18 and assert it in `test_cipher`. Release the call on a RES mismatch. Add MM_auth.1/MM_cipher.1 |
| 6 | **F-6** | Incoming SETUP always uses basic service **0x88 (wideband)** (`dect-stubd.c:7938`), internal calls 0x98, for every handset. GAP Table 21 allows service 0 only, and TS 102 527-1 §7.1.1 and the §7.1 NOTE require GAP interop and the external call class for a GAP PP | A pure GAP handset (no G.722 in its location-registration codec list) may refuse every incoming call ("incompatible service"). All bench handsets are CAT-iq, so this is untested | Choose per handset: 0x88 only if the PP's stored `<<CODEC-LIST>>` (`mm->peer_codecs`) contains G.722, else 0x80. Internal: 0x90/0x80 for a GAP target |
| 7 | **F-7** | Outgoing call connection is not IWU-driven. With `<<SENDING-COMPLETE>>` (en-bloc, e.g. phonebook dial) or `nooverlap`, `on_setup` connects at once (`accept_now`), and overlap calls connect after a 3 s digit pause (`dial_tick:2690`). The U-plane is bound **after** CONNECT by default, against 8.6 ("before sending the {CC-CONNECT} … shall connect the U-plane"); `bindfirst` exists but is off | For SIP the handset shows "connected" (and starts its call timer) before the far end answers. There is no ringback and no early media (no PROGRESS-INDICATOR 8H, N.33). The first audio may be clipped | Add a mode where `on_setup` only sends CALL-PROC/SETUP-ACK and the controller drives ALERTING/CONNECT (the API `dect_cc_proceed/alert/connect` already exists). Make `bindfirst` the default. Send PROGRESS-INDICATOR 8H with in-band ringback for SIP 180/183 |
| 8 | **F-8** | LOCATE-ACCEPT omits `<<LOCATION-AREA>>` when the PT sent none, and otherwise echoes the PT's *old* LAL (`dect_mm.c:398`). 175-5 Table 54: M; GAP Table 45: "even if default LAL" | A strict handset can refuse the location registration or keep a wrong LAL, so it re-locates repeatedly or is not reachable for incoming calls. Bench handsets tolerate it (they send an LA) | Always send our own LI-type 1 + LAL, from the value the chip broadcasts |
| 9 | **F-9** | Missing CC timers and exception paths: F-<CC.02> (8.7.1.2); F-<CC.01> expiry sends RELEASE-COM instead of the normal release that 8.3.2.3 / 175-5 §9.3.1.5 require (`dect_cc_tick:816-821`, and `test_overlap_timer` asserts the deviation); a release collision is answered with RELEASE-COM (P); a SETUP with TI flag 1 is accepted (P); service calls (class 11) are connected instead of rejected | Mostly invisible to users. Occasionally a stuck "released" state or an odd handset reaction | Fix together with F-1 (one state-machine pass), with tests |
| 10 | **F-10** | Internal calls (N.31, process mandatory once implemented): the target gets SIGNAL 0x41 (external) instead of 40H (Table 23), and no internal CLIP/CNIP (8.43/8.44; terminal id + "DECT n"). Refusals in `ic_start` send RELEASE without a reason | The handset cannot tell an internal call from an external one by its ring, and shows no caller. Low for SIP, visible in the house | `ic_setup`: `dect_cc_set_signal(..., 0x40)`, pass a `dect_clip` with number = terminal id (Table 82: type 3, plan 9) and the stored name (alphabet 000). Release with 14H/12H |

Uncertainties, stated plainly. F-1's real-world frequency depends on how often handsets reuse a link for a second
call; that is assumed and not measured, though the code path is proven by the probe. F-6 is untested for lack of a
GAP-only handset. The PARK length "37" (P-8.12) was not checked against the broadcast PLI. Everything marked N/A
(chip) was not examined.

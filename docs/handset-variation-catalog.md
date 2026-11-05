# Handset variation catalog

Written 2026-11-05. For the maintainers and for anyone who has to make a handset work that is not plain GAP or CAT-iq
(the first case is the Motorola O201, [vendor-specific-support.md](vendor-specific-support.md) §7). It lists **every
variation of the basic DECT functions that the standards permit**, including the unusual and the old ones, so that a
handset that misbehaves can be fitted by trying the variations one at a time instead of guessing. It also says, row by
row, whether we can send the variation today and how the result is seen.

**What it is not.** It is not a list of what any vendor does: a row says what the standard allows, and where a handset of
ours has been seen to need it, the row says so and cites the note. It was written from the standards and from our code and
notes, **not from a bench**: rows that rest on reading our code are marked `[code]` and have not been seen on air. The standards
are paraphrased with clause numbers, never quoted; they are not in the repository (`docs/specs/` is ignored by git, see its
README).

## How it was made

Four readers worked in parallel, one per area, each from the standards on disk (EN 300 175-3, -4, -5, -6 V2.9.1; EN 300 444
V2.5.1; TS 102 527-1 V1.5.1 and -3 V1.7.1), each clause number checked in the text, and each starting from our own notes and
code so that nothing already known is repeated. Their four parts follow unchanged after this introduction (their own preambles
have the sources and the reading notes). **Not on disk, so rows that depend on them say "unverifiable" or "not read":** EN 300 175-2 (the
radio layer), -7 (security), -8 (speech coding) and TS 102 527-5.

## How to read a row

`| ID | Variation | Spec | Why a strict or odd handset might care | Ours today | Check | Rank |`

- **Why** starts with the evidence: *spec-permitted* (the standard allows it; nobody has seen a handset need it), *observed*
  (a bench note of ours, cited) or *code* (found by reading our source). A row never claims what a vendor does without a source.
- **Ours today:** `key:` a profile key that acts, `lab:` a `dect-stubd -a` option, `default` (we send exactly this and have no switch; the file and line are given),
  `code` (a sender or handler would have to be written), `chip` (the chip decides), `n/a`.
- **Check:** how the result is seen: `frames` (the decoded message trace shows accept, reject, release or timeout), `ear`, `eye`
  (the handset's display), `hand` (a person presses something), `log`.
- **Rank:** **A** cheap to send and a plausible cause for a strict or odd handset; **B** plausible but costly or less likely;
  **C** exotic or old, kept for completeness.

## Size

**507 rows** in four areas. By rank:

| Area | A | B | C | Total |
|---|---|---|---|---|
| A: identity, registration, security, paging and link | 24 | 40 | 56 | 120 |
| B: call control | 19 | 42 | 55 | 116 |
| C: audio, codecs and bearers | 12 | 39 | 63 | 114 |
| D: display, names and features | 34 | 50 | 73 | 157 |
| **All** | **89** | **171** | **247** | **507** |

By what we can do today:

| Area | key | lab | default | code | chip | n/a | Total |
|---|---|---|---|---|---|---|---|
| A: identity, registration, security, paging and link | 3 | 9 | 55 | 33 | 15 | 5 | 120 |
| B: call control | 9 | 6 | 51 | 44 | 2 | 4 | 116 |
| C: audio, codecs and bearers | 4 | 13 | 31 | 34 | 19 | 13 | 114 |
| D: display, names and features | 20 | 4 | 65 | 56 | 2 | 10 | 157 |
| **All** | **36** | **32** | **202** | **167** | **38** | **32** | **507** |

Read it this way: only the `key` and `lab` rows (about an eighth of all) can be varied without writing code, and `default` rows
are what we send now with no way to change it. The `code` rows are the work. About two thirds of the rows are rank B or C:
a handset is unlikely to depend on them, and they are in the catalogue so that "we tried everything the standard allows" is a
claim that can be checked.

## What the cataloguing turned up

Found while reading, **not seen on air**; each is a row to test, not a conclusion. They are listed here because several change
what we believed.

1. **Our progress indicator goes out in the wrong place.** `queue_with_status()` in `dect_cc.c` appends <<PROGRESS-INDICATOR>>
   (0x1E) *after* <<CALL-INFORMATION>> (0x7E), but variable-length elements go out in ascending identifier order
   (EN 300 175-5 §7.5.1), and a receiver may stop scanning once it is past the one it wants. The O201 gets no call status, so
   it is not affected; a Part 3 handset given `cc.progress_in_band` would be (B-OUT-08, C-UP-07). The reference flows in TS 102 527-3
   put the indicator and the chosen codec list in the same {CC-SETUP-ACK}; we send the indicator only (B-OUT-09, C-CODEC-09).
   GAP also says a handset **shall** connect its audio when the indicator is present (EN 300 444 tables 11 to 13), so the O201's fix is
   the standard's own method.
2. **The O201's reason 0x0E has a defined meaning that we ignore.** "Partial release" is feature N.21 in GAP: the base should
   answer {CC-RELEASE-COM} with the same reason and keep the link for a few seconds (`<LCE.02>`); we send no reason and release the link
   at once (B-REL-01, B-REL-02, A-LNK-01). The reject may also carry an alternative service that we throw away (C-ATTR-05). This is a
   candidate reason for the O201's habit of ending every call that way, and nothing more than a candidate.
3. **`bindfirst` and `alertms` act on nothing for a bridged call** (the path through `dect_base_call_out()` never reaches
   `dect_base_dial_connect()`), so the line in vendor-specific-support.md §7.7, "bindfirst: no change, order of bind and CONNECT
   ruled out", may be **void**: the option was probably never applied in that run (C-BIND-01). Re-run it before relying on it.
4. **Profile keys that the stack does not act on.** The stack itself warns at start (`dect-stubd.c`, "some of its keys are not acted on in this
   build") for `cc.ring_callinfo`, `cc.connect_status_info`, `cc.bind_before_connect`, `cc.name.alphabet`, `cc.timer`, `cc.number.method`,
   `lists.serve`, `line.selection`, `cc.signal.internal` and the `feature.*` switches, but handset-profiles.md lists most of them with a
   lab option in its "today" column as if they worked. The table needs a note; the rows say "accepted, not applied".
5. **The capability default is the other way round.** For a handset whose <<TERMINAL-CAPABILITY>> is unknown the standard assumes full
   slot only (EN 300 444 §8.17 table 29); we offer wideband (0x88) and read the element only in MM, never in a {CC-SETUP} or a first
   response (C-TCAP-01, C-TCAP-02). The O201 is the case where this matters.
6. **No tone for GAP and Part 1 handsets.** The standard has the base generate ring-back and busy as in-band audio for them and not send
   <<SIGNAL>> (TS 102 527-3 §7.4.15.2.2.0); we send neither (C-TONE-03).
7. **Names and keypad.** `dect_profile_display` puts raw UTF-8 into <<MULTI-DISPLAY>>, which allows DECT characters only (part D, display
   text); <<SINGLE-KEYPAD>> and <<SINGLE-DISPLAY>> are not handled; a dialled string holding a pause, DTMF or register-recall code is
   refused (D-KEY-04 to D-KEY-06). Old handsets use these forms.
8. **The A540's own-menu deregistration** (open since October) may trace to our answering only one key type with one algorithm and
   rejecting the rest (A-AUT-08, A-REG-14). A hypothesis, with a result that would tell it apart from the alternatives in the rows.
9. **Our own notes disagree.** The comment above `terminate_answer` (`dect_mm.c`) says a handset's own deregistration is accepted without
   authentication, but the code authenticates first (part A, ambiguity 10); vendor-specific-support.md says in §7 that the O201 reports no
   terminal capability and in §7.7 that it reports "GAP only" (part A, ambiguity 11). `handset-profiles.md` cited the wrong clause for
   <<MODEL-IDENTIFIER>> (§7.7.30; it is §7.7.46), corrected with this catalogue.

## From the catalogue to a battery

The guided fitter of [vendor-specific-support.md](vendor-specific-support.md) §7.8 is derived from this catalogue by these rules:

1. **One variation at a time, then cumulative.** Start from our default and change one rank-A row. When a variation helps, keep it and
   go on from there (as §7.4 does); never combine variations that have not each been seen to matter.
2. **Order inside an area by rank, then by how often the standard's own flows use it,** and skip a whole area once the symptom is gone
   (the symptom is a row's *Check*: a release reason, an unanswered setup, no audio, a blank display).
3. **`ear` and `eye` rows are batched into one list of questions per run** ("did it ring? did you hear beeps? what does the display
   say?"), as in the 7412 runs; a person is the slow part, not the box.
4. **Rows marked `code`** are written only when the symptom points at them and the row has a ranked, falsifiable prediction. Writing
   them all first would be a large amount of code for variations nobody has needed.
5. **Registration-time rows** (codec list, PARK form, TPUI assignment) cannot depend on a profile for a handset that is not yet known: the
   Finder's session decides them (part A, reading note).
6. **The decoder comes first:** most rows are settled by reading the frames, not by asking a person (`frames` in *Check*). That is tier 1
   of §7.8.

## Index of the rank-A rows

Rank A is where a battery starts. The variation text is shortened; the full row is in the part named by its ID.

#### Rank A that can be sent today (a profile key or a lab option exists) (36)

| ID | Variation | Ours today | Check |
|---|---|---|---|
| A-PUI-02 | IPUI O assigned (type O, 4 + 36 bits as built) in place of the handset's IPUI N | lab:ipuio=N (dect_base.c:5127) | frames |
| A-REG-02 | accept the first try with 0000, reject, take the PIN typed second (easy pairing) | key:reg.tries_0000_first (log text only) | frames |
| A-REG-09 | CODEC-LIST in the accept and in {LOCATE-ACCEPT}: wideband list G.722 + G.726 (ours), full-slot list only, or none | lab:narrowband (all handsets) via ... | frames |
| A-AUT-01 | when the base challenges the handset on a call: before anything goes anywhere (authfirst, ours), in parallel with the ... | lab:authfirst/noauthfirst (dect_base.c:4165), ... | frames |
| A-AUT-03 | authentication nested in {LOCATE-REQUEST} before {LOCATE-ACCEPT} (TIF as annex H.2 example 2) vs none vs after | lab:locauth (default), nolocauth | frames |
| A-CIP-01 | cipher timing within the call: after the first NWK message, early (before the first dialled digits), or never | lab:-C, -k off; default (dect_base.c:4135) | frames |
| A-LNK-01 | after a CC release with reason "partial release" (0x0E) keep the link for <LCE.02> (3 to 10 s) in case the handset ... | key:lce.release_after_call (yes/no only); ... | frames |
| A-CAP-01 | TERMINAL-CAPABILITY absent, wrong or CAP-only (profile indicator 1 bit 1 but not bit 2 "GAP"): which defaults we ... | key:trust.terminal_capability, cap.ngdect; ... | log |
| B-INC-01 | BASIC-SERVICE octet: 0x88 (normal call, wideband default) versus 0x80 (normal call, basic speech default) for this ... | key:cc.basic_service (applied), lab:basicsvc, ... | frames |
| B-INC-02 | Keep 0x88 but put <<CODEC-LIST>> in the SETUP with G.726 first, the standard's "preferred method" for a ... | lab:codeclist=HEX (octets by hand) | frames |
| B-INC-04 | <<SIGNAL>> in the SETUP: alerting pattern 41H (ours), 40H, 42H-47H, 48H continuous, or none | key:cc.signal.external (applied), ... | ear |
| B-INC-10 | Calling party number in the SETUP versus absent versus repeated as display text | key:cc.number.method (accepted, not applied), ... | eye |
| B-INC-11 | Calling party name: <<CALLING-PARTY-NAME>> in the SETUP versus display text in a later {CC-INFO} versus both versus ... | key:cc.name.method, cc.name.display ... | eye |
| B-INC-14 | <<CALL-INFORMATION>> in the SETUP: line id, line type, call id, call status "CS call setup" (full), a bare line id, ... | key:cc.ring_callinfo (accepted, not applied; ... | ear |
| B-ANS-04 | "CS call connect" for Part 3 handsets: our {CC-INFO} after {CC-CONNECT-ACK} (never inside the ACK) | key:cc.connect_status_info (accepted, not ... | eye |
| B-OUT-01 | First answer: {CC-SETUP-ACK} (overlap, ours) versus {CC-CALL-PROC} versus {CC-ALERTING} versus {CC-CONNECT} at once | lab:nooverlap (CALL-PROC then CONNECT at ... | frames, eye |
| B-OUT-03 | <<PROGRESS-INDICATOR>> cause 8 ("in-band information now available") in the first answer ({CC-SETUP-ACK}, else ... | key:cc.progress_in_band (applied; dect_cc.c ... | ear |
| B-DIAL-01 | End of dialling: inter-digit timeout (ours dialwait 3000 ms) versus <<SENDING-COMPLETE>> in the SETUP or an INFO ... | lab:dialwait=MS (not a profile key; the ... | eye, hand |
| C-SVC-02 | Incoming SETUP carries 0x80 (normal call, basic speech default) | key:cc.basic_service = basic; lab:-a ... | ear |
| C-UP-01 | Indicator (location 1, description 8: 1e 02 81 88) in the first answer to the handset's call, {CC-SETUP-ACK} or else ... | key:cc.progress_in_band (dect_cc.c:125-135, ... | ear |
| C-OSEQ-04 | Bind the audio before {CC-CONNECT} (the FT shall connect its U-plane first) | lab:-a bindfirst (unbridged only) | ear |
| C-BIND-01 | Bind before our CONNECT (the FT connects its U-plane first, GAP §8.6) | lab:-a bindfirst (unbridged only); ... | log, ear |
| C-OLD-01 | GAP-only handset: full slot, G.726 32 kbit/s, FU1, no codec list | key:cc.basic_service = basic | ear |
| D-NAME-02 | Name in DECT alphabet (IA5) with transliteration of non-IA5 characters, for Part 1 and GAP handsets | key:cc.name.alphabet (parsed, not acted); ... | frames, eye |
| D-DISP-01 | One <<MULTI-DISPLAY>> with the bare text in a {CC-INFO} after the handset alerts (the present default framing ... | key:cc.name.method (display, both) and ... | eye |
| D-DISP-02 | A framing with cursor control codes before the text: clear display (0CH), return home (02H), clear to end of line ... | key:cc.name.display (data) | eye |
| D-DISP-03 | Text preceded only by 0CH (clear display) | key:cc.name.display | eye |
| D-DISP-07 | Timing: before the handset alerts (straight after the SETUP), after ALERTING (today), after the answer | lab:-a infoie=HEX (once on ALERTING); code ... | eye |
| D-CAPS-07 | No <<TERMINAL-CAPABILITY>> at all: assume the defaults, and treat the handset as GAP | key:cap.ngdect, trust.terminal_capability; ... | frames |
| D-CSTAT-01 | Call status in SETUP-ACK, CALL-PROC, ALERTING and CONNECT of a handset-placed call (Part 3) | key:cc.call_status (auto = Part 3 only); ... | eye |
| D-EVT-06 | List-change indication after location registration for the Line Settings List and List of Supported Lists | key:notify | eye, log |
| D-LIST-01 | Refuse every list ("list not supported") | key:lists.serve (parsed, not acted; always ... | frames |
| D-KEY-01 | Dial pause after the last digit when no end marker is sent | lab:-a dialwait=MS; uci opendectcell.main.dialwait | ear, hand |
| D-KEY-10 | Dial tone while overlap digits are collected: SETUP-ACK with in-band information, then the FP plays a dial tone | key:cc.progress_in_band | ear |
| D-TONE-01 | External call: pattern 41H in the SETUP (default); other 40H-48H | key:cc.signal.external (acts); lab:-a ... | ear |
| D-TONE-09 | In-band availability signalled by <<PROGRESS-INDICATOR>> (in SETUP-ACK, CALL-PROC or ALERTING) or by the CONNECT; ... | key:cc.progress_in_band | ear |

#### Rank A that needs code first (17)

| ID | Variation | Ours today | Check |
|---|---|---|---|
| A-LOC-02 | zero-length PORTABLE-IDENTITY in {LOCATE-ACCEPT}: no TPUI assigned, the PT keeps its TPUI or uses the default one, ... | code (dect_mm.c:756 always assigns) | frames |
| A-LOC-07 | FT-initiated location update: two consecutive {MM-INFO-SUGGEST} "locate suggest", first with ext 0, then with ext 1 | code | frames |
| B-INC-05 | Start alerting from a {CC-INFO} <<SIGNAL>> after the handset's {CC-ALERTING} (GAP figure 31) instead of inside the ... | code (the IE and dect_cc_info() exist, ... | ear |
| B-OUT-02 | "Early {CC-CONNECT}": CONNECT right after the SETUP, later call status in {CC-INFO}; also the standard way to open ... | code (nooverlap sends CALL-PROC then CONNECT, ... | ear |
| B-OUT-08 | PI IE order: ours appends it after <<CALL-INFORMATION>> (7EH) when call status is on, so 1EH follows 7EH | code defect in queue_with_status (the PI is ... | frames |
| B-OUT-09 | {CC-SETUP-ACK} carrying <<CODEC-LIST>> with the chosen codec together with the PI (the reference flow); ours names ... | code (queue_connect only; peer_negotiates) ... | frames, ear |
| C-TCAP-01 | Learn the capability from the handset's CC-SETUP and from its first response to ours, not only from MM | code | frames |
| D-NAME-03 | Name in a {CC-INFO} as <<CALLING-PARTY-NAME>> after the SETUP (for external calls) | code (INFO path only for internal calls) | frames, eye |
| D-DISP-08 | German and other accents through an ISO 2022 escape to Latin-1 (ISO 8859-1), -15, -9 or Greek, only to a handset ... | code | frames, eye |
| D-DISP-09 | Transliterate to IA5 for display text (ä to ae or a) | code | eye |
| D-CAPS-01 | Read the display capability (none, numeric, numeric-plus, alphanumeric, full) and decide what we send: whether to ... | code (stored in struct dect_term_cap.display, ... | frames, eye |
| D-CAPS-03 | Read the control-code level (octet 5) and use only the codes it allows | code | frames |
| D-CAPS-04 | Read the 8-bit sets (octet 5a) | code | frames |
| D-KEY-05 | Control codes inside a dialled string (05H pause, 12H, 14H, 16H) | code | frames, hand |
| D-KEY-06 | Register recall (15H alone) mapped to hold or flash toward SIP | code | frames, hand |
| D-TONE-06 | Ring-back tone on SIGNAL 01H to a Part 3 handset; in-band ring-back to a Part 1 or GAP handset | code (SIGNAL 01H never sent) | ear |
| D-ESC-03 | A decoder that prints every escape, proprietary event and unknown IE of a handset with its EMC, for the Command ... | code | frames |

#### Rank A that we send one way only (a switch would have to be written; see "Ours today" for where) (35)

| ID | Variation | Ours today | Check |
|---|---|---|---|
| A-BCN-01 | a44 "access rights requests supported" set only while the registration window is open and cleared after the first ... | default: 0x022B 'A' / 0x022C 01 in ... | frames |
| A-BCN-02 | order of the two human steps: window first, the handset's search second; a PT that read a44 = 0 may put the RFPI on a ... | default (the order is the user's; LuCI or ... | hand |
| A-ARI-01 | class A ARI (EMC 16 + FPN 17 = 36 bits), RPN 0, PARK PLI 36 (the box's own identity) | default (rfpi, dect_mm_fixed_identity ... | frames |
| A-ARI-02 | class B ARI (EIC 16 + FPN 8 + FPS 4 = 31 bits), RPN 0, one ARI per cell, PARK PLI 19 (ARC + EIC) so one subscription ... | default (controller sets rfpi and park_pli, ... | frames |
| A-PUI-01 | IPUI N (the handset's IPEI) echoed in the accept | default (dect_mm.c:1233, port_id = pend_id) | frames |
| A-REG-01 | the PIN (authentication code): 4 to 8 digits as we require; the standard lets a PT take 0 to 8 digits, the AC is 32 ... | default: -P PIN, 4 to 8 digits ... | hand |
| A-REG-14 | PT-initiated deregistration (the handset's own menu): ours first challenges the PT with its UAK, then accepts; ... | default (dect_mm.c:360, 474) | hand |
| A-AUT-08 | the handset's authentication of the base: we answer only DSAA with key type UAK; AC or UPI key types, other ... | default (dect_mm.c:474, at[0] == 0x01, key ... | frames |
| A-LOC-01 | LOCATION-AREA level in {LOCATE-ACCEPT}: the whole ARI (36 class A, 31 class B, ours) vs a shorter level (for example ... | default (dect_mm.c:787, 0x40 \| ari_len) | frames |
| A-LOC-11 | link after a {LOCATE-ACCEPT} with TPUI assignment: released at once (the standard excepts this case from "partial ... | default for the ring; code for "release right ... | frames |
| A-TPU-05 | {TEMPORARY-IDENTITY-ASSIGN-REJECT} from the PT (storage or limits): then page by the default TPUI | default (dect_mm.c:1389) | frames |
| A-PAG-03 | page identity: the assigned TPUI (ours, slot-derived) vs the default individual TPUI (type E, lowest 16 bits of the ... | default (dect_base.c:1231 pages by the ... | frames |
| A-LNK-02 | link release after a call or list session: DISC first and then the chip release (as stock for list access, ours) vs ... | default (dect_base.c:4117, DISC first only ... | frames |
| B-REL-01 | Answer to the handset's {CC-RELEASE}: {CC-RELEASE-COM} with no reason (ours) versus with the same reason, 0EH when it ... | default (on_release) | frames |
| B-REL-02 | Keep the link a few seconds after a partial release (timer <LCE.02>) before releasing it | default (dect_base_nlr_tick); ... | frames |
| B-COL-01 | After the PT released our SETUP with 32H (its own SETUP crossed ours): re-present at once (ours, up to 2 times) ... | default (DECT_CC_REPRESENT_MAX, ring requeue ... | frames |
| B-INT-01 | Class of the SETUP to the called handset: 9 for NG-DECT handsets, 8 ("external") for GAP and Part 1 handsets | default (ring_svc_for, dect_base_basic_svc); ... | eye, hand |
| B-LNK-01 | Page for every call after releasing the link (ours, as stock does) versus a SETUP on an already linked handset versus ... | default (dect_base_ring_tick) | frames |
| C-SVC-01 | Incoming SETUP carries 0x88 (normal call, wideband speech default) | default (dect-stubd.c:8780 sets 0x88; ... | frames |
| C-ATTR-05 | Read, not send: a refusal that carries the alternative service. A PT that cannot accept the SETUP may reply ... | default (reason logged, rest discarded: ... | frames (the 2026-11-05 O201 capture) |
| C-TCAP-02 | While the capability is unknown, assume the standard's default: full slot only (GAP) or full plus long j=640 (NG), ... | default (dect_base_basic_svc) | frames |
| C-UP-07 | Indicator after CALL-INFORMATION in the same message (our current order when a Part 3 handset has call status on) ... | default (dect_cc.c:125-135: status first, ... | frames |
| C-TONE-01 | No dial tone at all (we send no tone and no indicator) | default | ear |
| C-TONE-03 | Ring-back: {CC-ALERTING} only (ours) versus plus <<SIGNAL>> 01H versus in-band ring-back for GAP/Part 1 handsets | default | ear |
| D-CLIP-01 | Number in {CC-SETUP} (the present form) | default (dect_cc.c:248 put_identity, ... | frames, eye |
| D-CLIP-04 | Number type and plan: unknown/unknown (SIP numbers, today), international + E.164, national + E.164, private plan, ... | default (dect_base.c:2917); lab:-a clip= | frames, eye |
| D-CLIP-05 | A leading "+" kept as a character, or stripped with type "international", or turned into "00" | default (the string goes out as given, ... | eye |
| D-NAME-01 | Name in {CC-SETUP}, UTF-8 (the present form for external calls) | default (dect_base.c:2934; ... | eye |
| D-CINFO-01 | Full information in an incoming SETUP: line, line type, call id, "CS call setup" (Part 3 handsets) | default for Part 3 (dect_base.c:1039); lab:-a ... | ear, frames |
| D-CSTAT-02 | "CS call connect" sent in a {CC-INFO} after the handset answers an incoming call | default (dect_base_side_ring_call_tick, ... | eye |
| D-TIME-01 | {FACILITY} with TIME-DATE after each location registration | default (dect_time.c; lab:-a ... | eye |
| D-EVT-01 | MWI voice count per line, sent when the count changes on a link that is up, and after location registration | default (dect_base.c:3271, control verb mwi) | eye |
| D-EVT-05 | Missed-call notification: multiplicity = unread, subtype 01H "a new one just arrived" vs 02H "count only", sent with ... | default (dect_base.c:3324) | eye |
| D-INT-01 | Basic service of an internal SETUP to a GAP or Part 1 handset: class 9 (GAP §8.18) or class 8 "external" (Part 1 ... | default (class 8 for a known non-NG handset, ... | ear, eye |
| D-INT-02 | Internal caller shown: number and name both in the SETUP (today, Part 1 targets), or number in the SETUP and name in ... | default (dect_base.c:1896-1910) | eye |

#### Rank A that is the chip's, or not ours to vary (1)

| ID | Variation | Ours today | Check |
|---|---|---|---|
| A-PAG-01 | LCE header of the short page: 4 "voice, MT basic, full slot, IN_min_delay" vs 0 "no U-plane" (C-plane only) vs 3 ... | chip (page info byte 0x0C/0x2C is not ... | frames |

## What to do next

Nothing here is built. In order: (1) the tier-1 decoder and support bundle of §7.8, so that a trace is readable; (2) test the cheap ones among
the findings above (the ordering of the progress indicator, the echo of reason 0x0E and the link time, the capability
default, `bindfirst` re-run, the documentation of the unapplied keys); (3) only then a guided fitter, when a second handset needs it.

---

---

## Part A: identity, registration, security, paging and link set-up

Scope: everything a handset does before and around a call, up to the point where a CC message is carried: what the
base broadcasts, how it is identified, how a handset subscribes (access rights), locates, is authenticated and
ciphered, how it is paged and how the signalling link (LCE, DLC/LAPC) comes up and goes down.

Sources read for this part (all from `docs/specs/`, text extracted with `pdftotext -layout`): EN 300 175-5 V2.9.1 (NWK:
clauses 6.3, 7.6/7.7, 8.2, 13, 14, 15.5, 17, annexes A, F, H), EN 300 175-6 V2.9.1 (identities), EN 300 175-4 V2.9.1
(DLC: 5, 7, 9, 10.3, 10.6, annex A), EN 300 175-3 V2.9.1 (MAC: 7.2.3, 7.2.4, 11.3.3, read for what the chip
broadcasts and pages), EN 300 444 V2.5.1 (GAP: 6.9, 8.17, 8.22 to 8.45, 13, 14, annex A), TS 102 527-1 V1.5.1 and
TS 102 527-3 V1.7.1 (7.10 registration, annex A.3). EN 300 175-7 (security) is **not** in the local set.
Our own notes were read first and are linked, not repeated: [spec-conformance.md](spec-conformance.md),
[gap-conformance.md](gap-conformance.md), [handset-compatibility.md](handset-compatibility.md),
[handset-profiles.md](handset-profiles.md), [vendor-specific-support.md](vendor-specific-support.md) (§1 to §7.7),
[multicell-identities.md](multicell-identities.md), [mac-bringup.md](mac-bringup.md), [more-than-six.md](more-than-six.md).

**Evidence marks** (first word of the "Why" column): **S** the standard allows or requires it, nothing else known;
**O** observed in our own bench notes or code comments (cited); **H** hypothesis, no source.
**Ours today:** `key:` an existing profile key, `lab:` a `dect-stubd -a` option, `default` we send exactly this
(file:line given), `code` a sender or handler would have to be written, `chip` the chip decides, `n/a`.
**Check:** `frames` the decoded NWK/LAPC trace shows accept, reject, release or timeout; `ear`; `eye` (display);
`hand` (the user presses or enters something); `log`.
**Rank:** A cheap to send and a plausible cause for a strict or odd handset; B plausible but costly or less likely;
C exotic or old, listed for completeness.

Reading note for the Finder: the *profile is chosen after registration* (by model or by the user), so a registration-time
variant (codec list, optional IEs, PARK form, TPUI assignment) cannot depend on a profile for a handset that is not
yet known. The Finder's own session decides these.

---

#### A-BCN: what the handset reads before it speaks (N/Q-channel broadcast)

The base broadcasts its identity (RFPI, N channel) and capability bits (Q channel, QH = 3, 4, 5, 6, 7, B, C, D, E). A
handset that is looking for a base reads them while locking, and a handset in subscription mode reads bit a44. The
chip sends these; our only handle is mail `0x022B`/`0x022C`, which reaches a44 and nothing else
([mac-bringup.md](mac-bringup.md)). From the dumped block `38 49 94 ee` (decoded **by us** with a44 as the
calibration, not on air) the base declares: extended FP info, full slot, C/O setup on dummy, basic and advanced
A-field setup, ADPCM voice, GAP basic speech, DSAA required, DSC supported, location registration supported.

| ID | Variation (what the base could send or do differently) | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| A-BCN-01 | a44 "access rights requests supported" set only while the registration window is open and cleared after the first success (what we do) | GAP §8.45.4, §13.6.1 table 126, annex A; TS 102 527-3 §7.10.1.2.2, A.3 (F<AP.01> 120 s) | S: a PT in subscription mode only tries a base with a44 = 1; Part 3 PTs expect it to stay set after a failed easy-pairing try (the FT must allow at least one more attempt) | default: `0x022B 'A'` / `0x022C 01` in `dect_base_reg_window`, closes on enrolment (dect-stubd.c:5970); lab:`regopen[=SECS]`, `regwindow=SECS` (120) | frames | A |
| A-BCN-02 | order of the two human steps: window first, the handset's search second; a PT that read a44 = 0 may put the RFPI on a barred list | TS 102 527-3 §7.10.1.1.1 note 1 step 4b; GAP annex A steps 3 to 5 | H: a handset started before the window opens may keep skipping the base; S: its search mode runs P<AP.02> 120 s | default (the order is the user's; LuCI or `SIGUSR1` opens the window) | hand | A |
| A-BCN-03 | a38 "location registration supported" = 0 (ours decodes to 1): the PT then locates only on a "locate suggest" | GAP §13.2, §13.6.1 table 126; EN 300 175-5 annex F.1 | S: no location registration, so no TPUI assignment and paging by default TPUI | chip (`0x022B` reaches only a44, [mac-bringup.md](mac-bringup.md)) | frames | C |
| A-BCN-04 | a36 "DSAA required" / a37 "DSC supported" cleared (ours decodes to 1 / 1) | EN 300 175-5 annex F.1; GAP §8.45.5.2 | S: a PT watches a37 for its cipher observation; a37 = 0 tells it not to expect ciphering (a handset that cannot cipher) | chip | frames | C |
| A-BCN-05 | the Part 3 marker (extended capabilities part 2, a29 "extended wideband voice") on or off | EN 300 175-5 annex F.3; TS 102 527-3 §7.10.1.2.3 (backward-compat options 1 to 3) | S: a PT may skip the 0000 easy-pairing first try when the bit is absent; O: the A540 tries 0000 regardless (profile `reg.tries_0000_first`) | chip | frames | B |
| A-BCN-06 | extended higher-layer capability "location registration with TPUI allowed" (a39 of the QH = 4 block) | EN 300 175-5 §13.4.1 note 2, annex F.2 | S: the PT may name an assigned TPUI instead of the IPUI in {LOCATE-REQUEST} only if set; our value not decoded | chip | frames | C |
| A-BCN-07 | page repetition bit a19 (low duty-cycle idle-locked allowed); ours decodes to 0 | EN 300 175-3 §7.2.3.4.2 table 7.14 note 1, §11.3.3.1 | S: with a19 = 1 a PT may listen in one of four multiframes only, so pages are answered later; with 0 it must not | chip | ear | C |
| A-BCN-08 | manufacturer-specific escape tail (QH = 7) other than ours; no mail known that writes it | EN 300 175-3 §7.2.3.8; [mac-bringup.md](mac-bringup.md) | H: a vendor-branded handset might look for its own vendor's escape (no source) | chip | frames | C |
| A-BCN-09 | multiframe number (QH = 6), transmit info (QH = B), extended static system info (QH = D) | EN 300 175-3 §7.2.3.7, §7.2.3.10, §7.2.3.12 | S: DURATION time limits and PT clocks count multiframes ("may be used" with the counter); a legacy PT may rely on QH = 6 | chip | log | C |

#### A-ARI: the identity of the base (ARI, RFPI, PARK, SARI/TARI)

The base is known by its ARI (class A: manufacturer code + number; class B: installer code + number + sub-number),
broadcast inside the RFPI with a radio part number (RPN). The handset stores the PARK (a prefix of the ARI of length
PLI) from registration and compares it with what it hears. See [multicell-identities.md](multicell-identities.md)
(§4, §7 and the bench result of 2026-11-01: RPN stays 0, one ARI per cell, PLI 19).

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| A-ARI-01 | class A ARI (EMC 16 + FPN 17 = 36 bits), RPN 0, PARK PLI 36 (the box's own identity) | EN 300 175-6 §5.1, §6.1.1 | O: the form every bench handset has locked to (docs/handset-compatibility.md); GAP §13.7 table 128 stores PLI, LAL, ARI, RPN | default (`rfpi`, `dect_mm_fixed_identity` dect_mm.c:605) | frames | A |
| A-ARI-02 | class B ARI (EIC 16 + FPN 8 + FPS 4 = 31 bits), RPN 0, one ARI per cell, PARK PLI 19 (ARC + EIC) so one subscription covers all cells; alternatives PLI 31 (one cell only) | EN 300 175-6 §5.2, §6.1.2, §8.1 | O: A420, A540, AS690H and O201 moved between a 7520 and a 7412 within seconds (2026-11-05, [status.md](status.md)); S: a short PLI covers every base with that prefix | default (controller sets `rfpi` and `park_pli`, dect_mm.c:605) | frames | A |
| A-ARI-03 | one ARI, a distinct RPN per cell (the textbook multi-cell identity; RPN also separates handover domains) | EN 300 175-6 §5 figures 5.2 and 5.3, §5.2 | O: refused by the 08.25 firmware (A540 never reached the chip under RPN 1 or 2, 10 of 10; the 7412 06.88 attaches under RPN 2), [multicell-identities.md](multicell-identities.md) §7 | chip | frames | C |
| A-ARI-04 | PARK bits beyond the PLI: zero (ours) vs the RFPI's own RPN/E bits copied in | EN 300 444 table 51 and §13.7 table 128; EN 300 175-6 §4.1 | S: GAP has the handset store the complete PARK including non-significant bits as given; copied RPN bits make the cells' PARKs differ | default (dect_mm.c:605, bits above the PLI zeroed) | frames | B |
| A-ARI-05 | more than one PARK in {ACCESS-RIGHTS-ACCEPT} via REPEAT-INDICATOR (at most five): a system PARK{19} plus the cell's own | EN 300 175-5 §6.3.6.1 table 38 note, §13.5.1 | S: the handset would keep access rights for cells added later without a second registration; some handsets may store only the first | code (dect_mm.c:627 sends one PARK) | frames | B |
| A-ARI-06 | a made-up ARI as the Finder's random identity: values must stay legal (EMC, EIC, FPN not 0; FPN + FPS not 0) | EN 300 175-6 §5.1, §5.2 | S: the zero values are reserved; H: a handset might validate ranges | code (Finder design, [vendor-specific-support.md](vendor-specific-support.md) §7.6; not built) | frames | B |
| A-ARI-07 | SARI list (QH = 5, E bit in the RFPI), TARI request/reply, black ARI | EN 300 175-6 §5.6, §8.2; EN 300 175-3 §7.2.3.6, §7.2.5.10 | S: a PP whose PARK matches only a SARI/TARI could use the base; we broadcast none | chip | frames | C |
| A-ARI-08 | class C, D or E ARI (public access, GSM-attached, PP-to-PP direct) | EN 300 175-6 §5.3 to §5.5 | S: allowed classes for other environments; class E is a keypad-entered TPUI | n/a | frames | C |
| A-ARI-09 | the PARK typed into the handset by hand (key sequence `!!LLP...PC#`: PARK length, octal digits, check digit) before the handset will subscribe | EN 300 444 §14.3 | S: old handsets that cannot pick a base from the broadcast; the page would have to print the PARK in that form | code | hand | C |

#### A-PUI: the identity of the handset (IPUI, IPEI)

The handset names itself by an IPUI; type N is its IPEI. {ACCESS-RIGHTS-ACCEPT} returns an IPUI (the handset stores the
one it is given) next to the PARK; an IPUI/PARK pair is one subscription.

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| A-PUI-01 | IPUI N (the handset's IPEI) echoed in the accept | EN 300 175-6 §6.2.1; EN 300 444 table 51 | S: class A default; GAP says non-class-A bases never send IPUI N; O: all handsets accepted the echo on class A | default (dect_mm.c:1233, `port_id = pend_id`) | frames | A |
| A-PUI-02 | IPUI O assigned (type O, 4 + 36 bits as built) in place of the handset's IPUI N | EN 300 175-5 §7.7.30 figure 65; EN 300 175-6 §6.2.3; EN 300 444 table 51 | S: required for class B; O: used by the controller's multi-cell run; not tried on the O201 alone | lab:`ipuio=N` (dect_base.c:5127) | frames | A |
| A-PUI-03 | IPUI O number length and range (up to 60 bits) | EN 300 175-5 §7.7.30 figure 65 | S: a PT may store only a short number | lab:`ipuio=N` (32-bit N only) | frames | B |
| A-PUI-04 | IPUI T (installer code + BCD number), S (BCD phone number), P/Q/R/U (public) | EN 300 175-6 §6.2.2 to §6.2.8; EN 300 175-5 §7.7.30 | S: allowed types for roaming and public use; a strict PT may store only N and O | code | frames | C |
| A-PUI-05 | a second subscription on the handset and overwriting of the active IPUI/PARK pair | EN 300 444 §14.1 | O: the O201 keeps a leftover entry per random identity ([vendor-specific-support.md](vendor-specific-support.md) §7.7); S: the PT must store at least two and accept a new subscription for the active pair | n/a (handset side; we can only keep our identity stable) | hand | B |

#### A-REG: subscription (obtaining access rights)

The handset in subscription mode sends {ACCESS-RIGHTS-REQUEST} with its IPUI; the base runs key allocation (see A-KEY)
and answers {ACCESS-RIGHTS-ACCEPT} (IPUI, PARK and options) or {ACCESS-RIGHTS-REJECT}. The window must be opened by a
person and ends at the first success (GAP §8.45.4).

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| A-REG-01 | the PIN (authentication code): 4 to 8 digits as we require; the standard lets a PT take 0 to 8 digits, the AC is 32 bits padded with leading 1111 | EN 300 444 §14.2, §8.45.4; TS 102 527-3 §7.4.16.1 (headset: always 0000) | S: a handset with a numeric prompt may accept only 4 digits; a headset can only send 0000 | default: `-P PIN`, 4 to 8 digits (dect_dsaa.h:40), several PINs at once (API 139) | hand | A |
| A-REG-02 | accept the first try with 0000, reject, take the PIN typed second (easy pairing) | TS 102 527-3 §7.10.1.2.3, figures 82 and 83 | O: the A540 tries 0000 first; the refused try is logged as expected | key:`reg.tries_0000_first` (log text only) | frames | A |
| A-REG-03 | {ACCESS-RIGHTS-REJECT} content: reason 0x10 after a failed PIN (ours), 0x06 when closed, 0x30 overload, none; with or without DURATION (hold-off MM_wait 5 min) | EN 300 175-5 §7.7.34, §13.5.1, A.5; EN 300 444 table 52 | O: AS690H shows "PIN ungültig" on a wrong PIN ([handset-compatibility.md](handset-compatibility.md)); S: reason and DURATION are optional | default (dect_mm.c:651, 683) | eye | B |
| A-REG-04 | on a wrong PIN: {AUTHENTICATION-REJECT} then {ACCESS-RIGHTS-REJECT} (ours) vs the ACCESS-RIGHTS-REJECT alone | EN 300 175-5 §13.6; EN 300 444 §8.30.2.1, §8.32.2.4 | S: both are in the standard; a PT may wait for one or the other to close its transaction | default (dect_mm.c:1206, `send_auth_reject` then `send_access_reject`) | frames | B |
| A-REG-05 | optional element LOCATION-AREA in the accept | EN 300 175-5 §6.3.6.1 table 38 | S: pre-GAP handsets took their location area from the accept; GAP takes it from {LOCATE-ACCEPT} | code (dect_mm.c:627) | frames | B |
| A-REG-06 | ZAP-FIELD (0 to 15) and SERVICE-CLASS in the accept | EN 300 175-5 table 38, §7.7.39, §7.7.44; EN 300 444 table 51 (features N.16, N.14) | S: handsets that count ZAP or show a call restriction; service class 1 to 6 limits what may be dialled | code | frames | C |
| A-REG-07 | AUTH-TYPE and CIPHER-INFO in the accept, telling the PT which algorithm and key to use | EN 300 175-5 §13.5.1 table 38 | S: a PT that supports several algorithms picks from it | code | frames | C |
| A-REG-08 | MODEL-IDENTIFIER (the base's maker and model) and ESCAPE-TO-PROPRIETARY in the accept and in {LOCATE-ACCEPT} | EN 300 175-5 tables 38 and 54, §7.7.45, §7.7.46 | H: a vendor handset may enable its extras only for a known base; O: the O201 puts its own vendor element `7b 02 80 04` in every {CC-SETUP}, unanswered so far ([vendor-specific-support.md](vendor-specific-support.md) §7.7) | code | frames | B |
| A-REG-09 | CODEC-LIST in the accept and in {LOCATE-ACCEPT}: wideband list G.722 + G.726 (ours), full-slot list only, or none | TS 102 527-1 §7.3.1; EN 300 175-5 §7.7.54 | O: leaving it out made wideband setups fail on a handset that sent its own list (comment dect_mm.c:531, 2026-10-25); the O201 registers and locates with the wideband list | lab:`narrowband` (all handsets) via `narrowband_only` (dect_mm.c:556); per handset `code` | frames | A |
| A-REG-10 | the base's name broadcast while the window is open ({CLMS-FIXED} "device name", at most 17 characters, first segment within F<AP.03> 160 ms of the capability message) | TS 102 527-3 §7.10.1.3.2, tables 77 to 79, A.3 | S: Part 3 handsets show it and must cope with its absence (note 2); a GAP-only handset ignores it | code | eye | B |
| A-REG-11 | who may register: anyone with the PIN (open), only the same identity again (same), nobody (closed) | EN 300 444 §14.1 | S: the PT must accept a new subscription for the active pair; O: closed refuses with "IPUI not accepted" | default (dect_mm.h:43 `registration`) | frames | B |
| A-REG-12 | the link after the accept: kept for the PT's {LOCATE-REQUEST} that follows at once (ours: the base leaves an MM-only link alone) vs the FT releasing it | EN 300 444 §8.39, §13.2; EN 300 175-5 §14.2.7.2 | S: MM, CISS and CLMS use "partial release" except after a TPUI assignment or the access rights procedure | code (`dect_base_nlr_tick` dect_base.c:4089 acts only after a CC or list session) | frames | B |
| A-REG-13 | FT-initiated deregistration: with the PARK (GAP) or without (erase the IPUI and all its data); the PT may authenticate the FT first (nested) | EN 300 175-5 §13.5.2; EN 300 444 §8.31, tables 53 to 55 | O: works on the AS690H, handset on and in range ([handset-compatibility.md](handset-compatibility.md)); S: the PT may reject | default (dect_mm.c:291, IPUI + PARK) | frames | B |
| A-REG-14 | PT-initiated deregistration (the handset's own menu): ours first challenges the PT with its UAK, then accepts; variants: accept without authentication, reject with a reason, or hold-off DURATION | EN 300 175-5 §13.5.2; EN 300 444 §8.31 | O: "does not work yet" on the A540 ([handset-compatibility.md](handset-compatibility.md)); H: the PT may authenticate us first with a key type we refuse (A-AUT-08); note the comment above `terminate_answer` (dect_mm.c:323) says "no authentication first" while the code authenticates | default (dect_mm.c:360, 474) | hand | A |
| A-REG-15 | the handset proposes key type UAK or DSAA2 in its {ACCESS-RIGHTS-REQUEST} AUTH-TYPE | EN 300 444 table 50; EN 300 175-5 §7.7.4 | O: the C4 proposes DSAA2; GAP: a PT sending UAK is claiming it has a key and the FT shall not accept if it holds only an AC; we log the element and run DSAA key allocation regardless | default (dect_mm.c:1098) | frames | C |

#### A-KEY: key allocation (the PIN becomes a key)

The base sends {KEY-ALLOCATE} (algorithm, key numbers, RAND_F, RS); the handset answers {AUTHENTICATION-REQUEST} with RES1
computed from the PIN and its own RAND_P; the base answers {AUTHENTICATION-REPLY} with RES2. One MM transaction, and the new
key is the handset's UAK (number 8, the pair).

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| A-KEY-01 | ALLOCATION-TYPE `01 88` (DSAA, UAK 8, AC 8, "IPUI/PARK pair") vs other key numbers or the "IPUI only" flag | EN 300 175-5 §7.7.2; EN 300 444 table 56 | S: GAP fixes 8 and 8; a PT that stores keys per IPUI only would use another number | default (dect_mm.c:38 and 39) | frames | C |
| A-KEY-02 | DSAA2 key allocation (algorithm 2, RS128, RAND_P taken into RES1, fresh RAND_F in the reply) | EN 300 175-5 §13.6; EN 300 444 §8.45.9 | O: the C4 proposes DSAA2 and accepts DSAA (comment dect_mm.c:30 to 36); S: a handset claiming DSAA2 in TERMINAL-CAPABILITY octet 5 may prefer it | code (dect_mm.c:38 "DSAA2 not yet implemented") | frames | B |
| A-KEY-03 | {AUTHENTICATION-REJECT} from the PT (allocation type unacceptable) | EN 300 444 §8.32.2.3 | S: the PT may refuse our type; ours fails the registration and rejects | default (dect_mm.c:1313, `key_allocation_failed`) | frames | B |
| A-KEY-04 | the new UAK marked "unconfirmed" and the AC kept until the first successful PT authentication; the UAK is deleted if that fails | EN 300 175-5 §13.6; EN 300 444 table 58 text | S: ours stores the key as final at once (`subscribed = 1`, dect_mm.c:1223) | default | log | C |
| A-KEY-05 | {KEY-ALLOCATE} sent at once on {ACCESS-RIGHTS-REQUEST} (ours) vs after a pause | EN 300 175-5 §13.5.1 (PT timer MM_access.1 60 s); TS 102 527-3 figure 81 (PIN is typed before the request) | H: an old handset that prompts for the PIN only after seeing {KEY-ALLOCATE} | default (dect_mm.c:1098 to `send_key_allocate`) | frames | C |

#### A-AUT: authentication of the handset (and of the base)

The base authenticates the handset with its UAK (RAND_F, RS challenge; the response also yields the cipher key DCK).
The handset may authenticate the base the same way, on its own transaction, at any time (priority 1). Procedures are
nested by transaction-identifier flags (annex H).

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| A-AUT-01 | when the base challenges the handset on a call: before anything goes anywhere (`authfirst`, ours), in parallel with the {CC-SETUP} answer, or never (a handset that cannot, or `-C`) | EN 300 175-5 §13.3.1; EN 300 444 table 9, §8.24 | O: with authentication running beside the SETUP the O201 still rang on basic service 0x80 ([vendor-specific-support.md](vendor-specific-support.md) §7.7), so it was not the cause; with `authfirst` a handset that does not answer in 10 s loses the call | lab:`authfirst`/`noauthfirst` (dect_base.c:4165), `-C` (dect_base.c:4140) | frames | A |
| A-AUT-02 | {CC-NOTIFY} with TIMER-RESTART while our authentication delays the answer to a {CC-SETUP} | EN 300 444 §6.9.6; EN 300 175-5 §15.5, §7.6.9 | S: the PT's CC timers keep running during MM; the PT must support it, the FT may send it | code | frames | B |
| A-AUT-03 | authentication nested in {LOCATE-REQUEST} before {LOCATE-ACCEPT} (TIF as annex H.2 example 2) vs none vs after | EN 300 175-5 §13.3.1, §13.4.1, annex H.2 | O: the A540 on a 7412 was rejected "authentication failed" on a fresh registration until the key from this link was used (2026-10-29, comment dect_mm.c:968) | lab:`locauth` (default), `nolocauth` | frames | A |
| A-AUT-04 | AUTH-TYPE octet 5 bits: ours `08` for locate/terminate and `18` for ciphering (UPC, key number 8); alternatives: other key numbers, INC (ZAP), DEF (default cipher key), TXC | EN 300 175-5 §7.7.4; EN 300 444 tables 42, 43 | S: a PT may refuse key numbers it does not store; the code comment says `0x10` would store the DCK as key 0 | default (dect_mm.c:1013, `AUTH_TYPE_CIPHER`) | frames | B |
| A-AUT-05 | ZAP increment: INC = 1 with a nested authentication of the FT, ZAP-FIELD echoed | EN 300 444 §8.26; EN 300 175-5 §13.3.1 | S: feature N.16, a counter the handset keeps per subscription | code | frames | C |
| A-AUT-06 | DSAA2 authentication (algorithm 2, RS128, RAND_P in the reply) when the PT sets DSAA2 in TERMINAL-CAPABILITY octet 5 | EN 300 175-5 §13.3.1, §7.7.41; EN 300 444 §8.45.7 | O: the C4 proposes it; we run DSAA only | code | frames | B |
| A-AUT-07 | authentication of the user (user PIN + UAK, key type UPI, timer MM_auth.2 100 s) | EN 300 175-5 §13.3.2, §7.7.4, A.5 | S: old business phones prompt for a user PIN | code | hand | C |
| A-AUT-08 | the handset's authentication of the base: we answer only DSAA with key type UAK; AC or UPI key types, other algorithms get {AUTHENTICATION-REJECT}; counted and capped | EN 300 175-5 §13.3.3; EN 300 444 §8.23 | H: a handset that authenticates us before accepting its own deregistration with another key type or number would fail on this (see A-REG-14) | default (dect_mm.c:474, `at[0] == 0x01`, key type 1) | frames | A |
| A-AUT-09 | after a wrong RES: {LOCATE-REJECT} 0x10 or call release (ours) vs {IDENTITY-REQUEST} to re-identify a TPUI user | EN 300 175-5 §13.3.1 last bullet, §13.2.1 | S: with a TPUI the failure may be a name clash, not an impostor | default (dect_mm.c:977, dect_base.c:4228) | frames | C |
| A-AUT-10 | {AUTHENTICATION-REJECT} from the base carrying an alternative AUTH-TYPE list | EN 300 175-5 §13.3.1, §13.3.3 | S: lets a PT retry with an algorithm or key the FT offers | code (ours sends the reason only, dect_mm.c:683) | frames | C |

#### A-LOC: location registration, detach, location update

After subscription and at power-up or area change the handset sends {LOCATE-REQUEST}; the base answers {LOCATE-ACCEPT}
with the location area and a TPUI to be paged by, or {LOCATE-REJECT}. The whole is one transaction, the TPUI part
being the temporary-identity procedure (A-TPU).

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| A-LOC-01 | LOCATION-AREA level in {LOCATE-ACCEPT}: the whole ARI (36 class A, 31 class B, ours) vs a shorter level (for example 19, so all cells of one EIC are one area) vs 0; also LI type 3 with extended location information | EN 300 175-5 §7.7.25, §13.4.1; EN 300 444 table 45 (0 to 39); EN 300 175-6 §5 | S: the level counts PARI+RPN bits whose change forces a new registration; with one ARI per cell each cell is its own area and the handset relocates on every cell change | default (dect_mm.c:787, `0x40 \| ari_len`) | frames | A |
| A-LOC-02 | zero-length PORTABLE-IDENTITY in {LOCATE-ACCEPT}: no TPUI assigned, the PT keeps its TPUI or uses the default one, and sends no ack | EN 300 175-5 §13.4.1; EN 300 444 table 45 and §8.28 | S: a handset with no TPUI storage would reject the assignment; O: the ack arrives on all tested handsets; on the 7412 the chip hears only TPUIs whose low nibble is 1 to 6 ([more-than-six.md](more-than-six.md)) | code (dect_mm.c:756 always assigns) | frames | A |
| A-LOC-03 | USE-TPUI element: the PT may then answer pages and set up calls with its TPUI | EN 300 175-5 §7.6.2, §13.4.1, §14.2.3 | S: absent, the PT must use the full IPUI in {LCE-PAGE-RESPONSE}; GAP table 45 does not list it | code | frames | B |
| A-LOC-04 | DURATION in {LOCATE-ACCEPT}: none (default), infinite, lock limits, time limit 1 (unit 40.96 s) or erase | EN 300 175-5 §7.7.13, §13.4.1; EN 300 175-6 §6.3 | O: `locvalid=1` made the handsets locate again every 41 s (journals/bench-2026-10-25.md) | lab:`locvalid=N` | frames | B |
| A-LOC-05 | {LOCATE-REJECT} reason: IPUI unknown 0x02 (ours); TPUI unknown 0x01 (the PT retries with its IPUI), invalid TPUI 0x23, authentication failed 0x10, location area not allowed 0x80; with DURATION hold-off | EN 300 175-5 §7.7.34, §13.4.1; EN 300 444 §8.28.2.1 | S: PT behaviour after a reject differs (retry, wait, keep or drop the subscription); a roamer without a record on this cell must not lose its subscription | default (dect_mm.c:823, 0x02) | frames | B |
| A-LOC-06 | a slow answer (the controller fetches the record): {MM-NOTIFY} TIMER-RESTART to stop the PT's 20 s timer MM_locate.1 | EN 300 175-5 §13.4.1 note 4, §7.6.9, A.5 | O: requests are held while the owner fetches the record (api-versions.md 19); no restart is sent | code | frames | B |
| A-LOC-07 | FT-initiated location update: two consecutive {MM-INFO-SUGGEST} "locate suggest", first with ext 0, then with ext 1 | EN 300 175-5 §13.4.3, annex H.3.1; EN 300 444 §8.29 table 49 | H: after a base restart or a moved cell a handset with a stale TPUI would re-attach without a reboot (the AS690H needed one before it could dial, 2026-11-05) | code | frames | A |
| A-LOC-08 | {LOCATE-REQUEST} naming an assigned TPUI instead of the IPUI | EN 300 175-5 §13.4.1 | S: allowed if a39 is set; we find records by IPUI only, so reject and the PT retries with the IPUI | default (dect_mm.c:159, `dect_mm_find`) | frames | C |
| A-LOC-09 | {DETACH} from a handset switching off in range: taken as a hint (ours) vs ignored | EN 300 175-5 §13.4.2 | O: Offline at once on the A540 and AS690H (2026-11-03, [handset-compatibility.md](handset-compatibility.md)) | default (dect_mm.c rx `DECT_MM_DETACH`) | log | C |
| A-LOC-10 | the PT's extra elements in {LOCATE-REQUEST}: old FIXED-IDENTITY and LOCATION-AREA, NWK-ASSIGNED-IDENTITY, CIPHER-INFO, SETUP-CAPABILITY | EN 300 175-5 §13.4.1 table 56 | S: the old area tells the base the handset came from elsewhere; we read TERMINAL-CAPABILITY, MODEL-IDENTIFIER and CODEC-LIST only | default (dect_mm.c:871) | log | C |
| A-LOC-11 | link after a {LOCATE-ACCEPT} with TPUI assignment: released at once (the standard excepts this case from "partial release") vs left to the handset | EN 300 444 §8.39, §13.4; EN 300 175-5 §14.2.7.2 | S: the PT keeps its old PMID/LSIG until the link is released and a page by the new TPUI needs an idle handset; O: a linked handset did not answer 16 pages, so we release before every ring ([spec-conformance.md](spec-conformance.md), dect_base.c:1430) | default for the ring; code for "release right after the ack" | frames | A |

#### A-TPU: temporary identity (TPUI) and its timer

A TPUI is the short identity the handset is paged by. It is assigned in {LOCATE-ACCEPT} or by a stand-alone
{TEMPORARY-IDENTITY-ASSIGN}; the handset answers with an ack or a reject; the base runs <MM_ident.1> (10 s). There is no
timer called "TPUI timer": validity is the DURATION element (A-LOC-04).

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| A-TPU-01 | value form: `0 11 slot` (assigned individual, ours) vs other assigned types (first and second digit 0 to B, for example a BCD extension number padded with B) and assignment type 0001 "number assigned" | EN 300 175-6 §6.3.1 table 6.1, §6.3.2 note 1; EN 300 175-5 §7.7.30 figure 72; EN 300 444 table 45 | O: the 7412 chip hears only low nibbles 1 to 6; `tpuioffset=N` assigns a non-slot value ([more-than-six.md](more-than-six.md) T-C) | default (dect_mm.c:743, `dect_mm_tpui`); lab:`tpuioffset=N`, `subslot=N` | frames | B |
| A-TPU-02 | stand-alone {TEMPORARY-IDENTITY-ASSIGN} at any time (re-assign without a location registration, for example when the controller moves a handset between pool slots) | EN 300 175-5 §13.2.2, §6.3.6.24 | S: the base may initiate it; the PT acks or rejects | code | frames | B |
| A-TPU-03 | DURATION "erase" to delete the TPUI in the handset | EN 300 175-5 §13.2.2, §7.7.13 | S: the ack means it is erased, also when there was none | code | frames | C |
| A-TPU-04 | first <MM_ident.1> expiry: retransmit (stand-alone path) vs abort (the {LOCATE-ACCEPT} path, ours: record `tpui_timed_out`, keep the registration) | EN 300 175-5 §13.2.2, §13.4.1, A.5 | O: a TPUI that was never acked is paged in vain and silently; `WARNING: TPUI never acknowledged` marks it ([spec-conformance.md](spec-conformance.md)) | default (dect_mm.c:1419, `dect_mm_tick`) | log | B |
| A-TPU-05 | {TEMPORARY-IDENTITY-ASSIGN-REJECT} from the PT (storage or limits): then page by the default TPUI | EN 300 444 §8.28.2.3, table 48; EN 300 175-6 §6.3.2 | S: the default individual TPUI (type E, lowest 16 bits of the IPUI) is always available; we record the reject but keep paging by the assigned one | default (dect_mm.c:1389) | frames | A |
| A-TPU-06 | NWK-ASSIGNED-IDENTITY (TMSI) in {LOCATE-ACCEPT} | EN 300 175-5 §7.7.28, §13.4.1 | S: for networks with a TMSI; the PT then sends it in later requests | code | frames | C |
| A-TPU-07 | emergency TPUI (`F1xxx`) and group TPUIs (call group `Dxxx`, connectionless group `Cxxx`, CBI `CFFF`) | EN 300 175-6 §6.3.1, §6.3.3 | S: allowed paging identities; group TPUIs ring several handsets at once | code | ear | C |

#### A-IDN: other MM procedures (identity request, info, notify, external protocol)

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| A-IDN-01 | {IDENTITY-REQUEST} for the IPEI or IPUI (stolen-equipment check, or after a failed authentication); an empty {IDENTITY-REPLY} is a reject | EN 300 175-5 §13.2.1, §7.7.19; EN 300 444 §8.22 | S: the base can ask the handset what it is; a handset must answer in every CC state (GAP table 9) | code | frames | C |
| A-IDN-02 | {MM-INFO-SUGGEST} "access rights modify suggest": the base makes the handset redo access rights without the user (for example to swap a PARK) | EN 300 175-5 §13.5.3 | S: the PT authenticates the FT first, then runs {ACCESS-RIGHTS-REQUEST}; useful to move handsets to a system PARK | code | frames | B |
| A-IDN-03 | {MM-INFO-REQUEST} from the PT (parameter retrieval) answered with {MM-INFO-ACCEPT} or {MM-INFO-REJECT} | EN 300 175-5 §13.7; timer MM_info.1 5 s | S: ours ignores it silently, so the PT times out and retransmits once | default (dect_mm.c:1244, unknown messages dropped) | frames | C |
| A-IDN-04 | {CIPHER-SUGGEST} from the PT (PT-initiated cipher switching): answer with {CIPHER-REQUEST} or {CIPHER-REJECT} | EN 300 175-5 §13.8, annex H.3.3; EN 300 444 §8.34 | S: GAP lists PT-initiated switching as supported in all CC states; ours ignores it, the PT times out after 10 s (MM_cipher.2) | default (dect_mm.c:1244) | frames | B |
| A-IDN-05 | {MM-IWU} and segmentation of an MM message beyond 58 octets | EN 300 175-5 §13.9 | S: external protocol information; ignored by us | default | frames | C |
| A-IDN-06 | an unexpected or unrecognised MM message in any state: ignore (ours); for IE errors in the listed requests return the reject with "information element error" or "invalid information element contents" | EN 300 175-5 §17.3.2.5, §17.4.4, §17.6.4; EN 300 444 §6.9.4 | S: a strict PT may depend on the reject; ours rejects for a missing PORTABLE-IDENTITY in {LOCATE-REQUEST} and {ACCESS-RIGHTS-REQUEST} | default (dect_mm.c:904, 1098) | frames | B |

#### A-CIP: ciphering (cipher switching, DSC, early encryption)

The base authenticates the handset (A-AUT-01), derives a cipher key and sends {CIPHER-REQUEST}; the real start is a MAC
operation always initiated by the handset (the chip). GAP requires FT-initiated encryption for every call and a release
with "encryption activation failed" otherwise; we do not claim that feature (GAP N.35).

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| A-CIP-01 | cipher timing within the call: after the first NWK message, early (before the first dialled digits), or never | EN 300 444 §8.45.1, §8.45.5.1; EN 300 175-5 §13.8 | S: the timer MM_encryption_check.1 (value in EN 300 175-7 I.1, not local); O: ciphered calls worked on the O201 with basic service; `-C` (no cipher) did not change its outgoing-audio problem ([vendor-specific-support.md](vendor-specific-support.md) §7.7) | lab:`-C`, `-k off`; default (dect_base.c:4135) | frames | A |
| A-CIP-02 | a per-handset switch "never cipher" for a handset that cannot (a security trade the user would decide) | EN 300 444 §8.45.5.1 (the FP shall release such a call; a deliberate deviation) | H: a legacy non-GAP handset without cipher support | code (only the global `-C` exists) | frames | B |
| A-CIP-03 | CIPHER-INFO content `81 98`: enable, DSC, derived key number 8; alternatives: key number 0 to 7, DSC2, static key type, disable (Y/N = 0) | EN 300 175-5 §7.7.10; EN 300 444 table 61 | S: GAP fixes DSC with key 8; a handset that stores the DCK under another number would reject | default (dect_mm.c:1033, `CIPHER_INFO_DSC`) | frames | B |
| A-CIP-04 | CALL-IDENTITY or CONNECTION-IDENTITY in {CIPHER-REQUEST} (ours: neither, so it applies to all calls) | EN 300 175-5 §13.8, §15.6 | S: with neither element the switch relates to all existing connections of the PT | default (dect_mm.c:1033) | frames | C |
| A-CIP-05 | {CIPHER-REJECT} from the PT: keep the call (ours, counted), release it with "encryption activation failed", or retry after a fresh authentication when the reason is "cipher key not supported" | EN 300 175-5 §13.8 table 83 and options; EN 300 444 §8.33.2.1, §8.45.5.1 | S: all three are allowed by NWK; GAP requires the release for FPs claiming N.35 | default (dect_mm.c:1331, counted only) | frames | B |
| A-CIP-06 | the cipher key delivered to the chip (mail `0x0208`) before the request; withheld | EN 300 175-4 §10.6.1.1 (DL_ENC_KEY) | S: without the key at MAC the PT's START is not granted | lab:`-k off`; chip | frames | C |
| A-CIP-07 | early encryption with a default cipher key (AUTH-TYPE DEF bit, index, algorithm octet 5c; capability bit a42 and TERMINAL-CAPABILITY "re-keying and early encryption") | EN 300 444 §8.45.3; EN 300 175-5 §7.7.4, annex F.3 | S: a PT that holds a default key encrypts at once after connection set-up and drops the link after 10 s if the FP does not accept; we assign none | code | frames | B |
| A-CIP-08 | re-keying during a call (periodic authentication + {CIPHER-REQUEST}, timer MM_re-keying.1) | EN 300 444 §8.45.2; EN 300 175-5 §13.8 | S: only for FPs that set a42; a PT that expects it releases with "re-keying failed" | code | frames | C |
| A-CIP-09 | DSC2 and DSAA2 (a43, a44 and TERMINAL-CAPABILITY octet 5) | EN 300 444 §8.45.10, §8.45.11, §13.6.3 | O: not needed so far (A540, C430A, AS690H cipher with DSC); S: a handset may offer both | code | frames | C |
| A-CIP-10 | the PT observing an unciphered call after earlier ciphered ones ("security attack assumed") | EN 300 444 §8.45.5.2 | S: if ciphering silently stops for a handset (key mismatch after a re-registration), a legacy-aware PT releases the call | n/a (handset side); log the release reason | frames | C |

#### A-PAG: paging (what the handset must recognise to answer)

The base pages with a short {LCE-REQUEST-PAGE} (our mail `0x0300`: page info byte, lowest 16 bits of the TPUI) and
waits for the handset to set up a link and send {LCE-PAGE-RESPONSE}; only then the {CC-SETUP} goes out. The chip builds
the page and compiles the LCE header in; GAP allows only the short format and header 0 or 4
([spec-conformance.md](spec-conformance.md) gaps 2 and 5, [CR A-0076]).

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| A-PAG-01 | LCE header of the short page: 4 "voice, MT basic, full slot, IN_min_delay" vs 0 "no U-plane" (C-plane only) vs 3 "general purpose" (PT negotiates attributes) | EN 300 175-5 §8.2.4.1, §8.2.4.3 table 80b; EN 300 444 table 66 | S: the header chooses the PT's default MAC set-up; O: the value the chip compiles in has never been read (`F4.36`, [spec-conformance.md](spec-conformance.md)) | chip (page info byte `0x0C`/`0x2C` is not referenced in the page path) | frames | A |
| A-PAG-02 | full page format with TPUI (slot type, set-up info) or with IPUI instead of the short form | EN 300 175-5 §8.2.2, §8.2.4.2; EN 300 444 §8.35 | S: GAP allows only the short format; the stub has no full-format builder | chip | frames | C |
| A-PAG-03 | page identity: the assigned TPUI (ours, slot-derived) vs the default individual TPUI (type E, lowest 16 bits of the IPUI) when no TPUI was acked or the handset has not located | EN 300 175-5 §14.2.3; EN 300 175-6 §6.3.2, §9.1.2 | S: a handset without an assigned TPUI listens only for the default one; O: a never-acked TPUI is paged in vain ([spec-conformance.md](spec-conformance.md)) | default (dect_base.c:1231 pages by the assigned one); code for the fallback | frames | A |
| A-PAG-04 | page resubmission interval and count: <LCE.03> 3 s (ours), up to N300 times (3 recommended for voice); we try 20 times (60 s) | EN 300 175-5 §14.2.3, A.6, A.7 | S: a handset in a low duty cycle or far away answers late; N300 = 3 gives up after ~9 s | default (dect_base.h:968 and 969) | log | B |
| A-PAG-05 | page a handset that holds a link: release first (ours) vs page it | EN 300 175-5 §14.2.3; [CR A-0053] | O: with the link up 16 pages went unanswered, without it the first was answered (vF423, [spec-conformance.md](spec-conformance.md)); the experiment is closed | default (dect_base.c:1430) | frames | C |
| A-PAG-06 | timing between the page response and our {CC-SETUP} (we wait for the response, then send; a deadline otherwise) | EN 300 175-5 §14.2.3; EN 300 444 §8.35 | O: a SETUP sent before the page response was lost and the retry drew `0x32` "insufficient resources" (comment dect_base.c:1108) | default (dect_base.c:1117 `dect_base_ring_setup`) | frames | B |
| A-PAG-07 | the PT's {LCE-PAGE-RESPONSE} with a TPUI (after USE-TPUI), a wrong or late identity: {LCE-PAGE-REJECT} with "invalid TPUI", "false LCE reply", "late LCE reply" or a test call-back reason | EN 300 175-5 §14.2.3, §7.7.34, §15.8; EN 300 444 §13.3 (the reject shall not delete a TPUI) | S: unwanted responses must be cleared; ours rejects only a response without a PORTABLE-IDENTITY | default (dect_link.c:232 `rx_lce`, 142 `dect_link_page_reject`) | frames | C |
| A-PAG-08 | fast paging / expedited page (only if the PT's SETUP-CAPABILITY says "fast paging"), fast set-up without a page | EN 300 175-5 §14.2.3, §7.7.40; EN 300 175-3 §11.3.3.1, §11.3.3.2 | S: a PT in high duty-cycle mode answers sooner; the chip's a20 bit allows set-up on the dummy bearer | chip | frames | C |
| A-PAG-09 | collective and group ringing pages (LCE header 001, ring pattern, CBI `CFFF`, stop-ring patterns) before the {CC-SETUP} | EN 300 175-5 §8.2.4.4, §14.4; EN 300 444 §8.12.3 | S: optional, old PABX style; the PT must still get a {CC-SETUP} after | chip (code for the stub) | ear | C |
| A-PAG-10 | a page for a C-plane-only reason (header 0): to deliver a time update, a notification or a location suggest to an idle handset | EN 300 444 table 66; EN 300 175-5 §14.3.3 | S: GAP requires the PT to follow on with a call on that link; ours: `timeupdate=MIN` pages handsets without a link | lab:`timeupdate=MIN` (header decided by the chip) | frames | C |

#### A-LNK: LCE link set-up and release (NWK side of the link)

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| A-LNK-01 | after a CC release with reason "partial release" (0x0E) keep the link for <LCE.02> (3 to 10 s) in case the handset follows with another procedure; we release as soon as the last frames are acknowledged | EN 300 175-5 §14.2.7.2, A.6; EN 300 444 §8.39, §8.9 | O: the O201 ends every call, a normal hang-up too, with reason 0x0E ([vendor-specific-support.md](vendor-specific-support.md) §7.7); S: partial release means "keep the link" | key:`lce.release_after_call` (yes/no only); default (dect_base.c:4089, no hold timer) | frames | A |
| A-LNK-02 | link release after a call or list session: DISC first and then the chip release (as stock for list access, ours) vs the chip release alone (calls) vs DISC always | EN 300 175-5 §14.2.7.1; EN 300 175-4 §9.2.3.7 | S: "normal" release lets outstanding data finish, "abnormal" is immediate; <LCE.01> 5 s | default (dect_base.c:4117, DISC first only for list sessions); lab:`nonlr` (no release) | frames | A |
| A-LNK-03 | <LCE.05>: the PT opens a link without an SDU and nothing follows; the FT releases after 5 s | EN 300 175-5 §14.2.2, A.6 | S: a PT that opens a link and waits (some handsets do, for MM) would be dropped | code (no LCE timer is implemented) | frames | C |
| A-LNK-04 | direct FT-initiated link set-up (a connection to the handset's last RFP without paging) | EN 300 175-5 §14.2.4; EN 300 175-3 §11.3.3.2 | O: stock raises a connection bearer before its SETUP ([mac-bringup.md](mac-bringup.md)); S: needs a PT in set-up detection mode, advertised in its SETUP-CAPABILITY | chip | frames | B |
| A-LNK-05 | link suspend and resume (class B only) | EN 300 175-5 §14.2.6; EN 300 175-4 §9.2.7 | S: not available on class A; a class A "suspend" is a link release | n/a | frames | C |

#### A-DLC: the data link (LAPC, Lc, frames)

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| A-DLC-01 | a class B request (I-frame or SABM on an assigned LLN): answer on class A (LLN 1), as stock does | EN 300 175-4 §9.2.2.2, §9.2.4.2, §9.2.4.3 | S: "the responding entity may refuse"; O: the handsets accept the refusal; class B would need window k = 3 | default (dect_lapc.c:304 to 311) | frames | C |
| A-DLC-02 | class A parameters: modulus 2, window 1; retransmission <DL.04> 2 s, N250 = 3, class A establish <DL.07> 2 s | EN 300 175-4 §7.5.2, §9.2.3.1 to §9.2.3.6, annex A.1 | O: ours was 1 s until 2026-10-22 ([spec-conformance.md](spec-conformance.md) gap 3); S: a slow handset needs the full 2 s | default (dect_lapc.c:135 and 136) | frames | C |
| A-DLC-03 | segment size of our I-frames: up to 63 information octets per frame (LI 6 bits; CS frames FLEN 5 to 70); a shorter segment size | EN 300 175-4 §7.7.3; EN 300 444 §6.9.3 | S: GAP only requires a PT to take the first 63 octets of an NWK message; an odd handset might not reassemble several segments (M bit) | default (dect_lapc.h:104, 256-octet messages, M-bit segments) | frames | C |
| A-DLC-04 | NLF flag: set on the first frame of each link and on class B establish/release only | EN 300 175-4 §9.2.2.1, §9.2.3.1 | S: a handset that mis-sets it makes us treat its frames as a new link; we discard receive state on NLF | default (dect_lapc.c:312) | frames | C |
| A-DLC-05 | link signature: derived from the handset's PMID (assigned TPUI, else arbitrary default) learned from its first frame | EN 300 175-4 §10.3.1; EN 300 175-6 §9.1.2 | S: the PMID is not updated while a link is open even if the TPUI changed (GAP §13.4), so a frame after a TPUI change on an open link carries the old signature | default (dect_lc.c, `derive_lsig`) | frames | B |
| A-DLC-06 | connectionless frames (SAPI 3, UI) and broadcast ({CLMS-FIXED}, {CLMS-VARIABLE}) | EN 300 175-4 §9.3, §9.4; EN 300 175-5 §8.3, §12 | S: old handsets and Part 3 name broadcast use them; ours counts and drops SAPI 3 | default (dect_lapc.c:300, `rx_bad_sapi`) | frames | C |
| A-DLC-07 | transaction identifiers and nesting: MM TI value always 0, flags per annex H (key allocation: base 0, handset 1, base 0; locate with nested authentication: handset 0, base 0, handset 1, base 1) | EN 300 175-5 §7.3 table 68, §15.5, annex H | O: the handset's answer in key allocation always carried F = 1 (17 of 17 in the logs, comment dect_mm.c:1279) | default (dect_mm.c:227, `hdr`) | frames | B |

#### A-CAP: what the handset says about itself

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| A-CAP-01 | TERMINAL-CAPABILITY absent, wrong or CAP-only (profile indicator 1 bit 1 but not bit 2 "GAP"): which defaults we assume | EN 300 175-5 §7.7.41 notes 8 and 12; EN 300 444 §8.17 table 29 | S: absent means no re-keying, no DSAA2/DSC2, full slot, minimum TCL; O: the O201 reports GAP only | key:`trust.terminal_capability`, `cap.ngdect`; default (dect_mm.c:843 `term_cap_parse`) | log | A |
| A-CAP-02 | SETUP-CAPABILITY from the handset (page capability: normal or fast; set-up capability: selective or complete fast set-up) used to choose the paging and set-up mode | EN 300 175-5 §7.7.40 | S: tells whether a fast page or a fast set-up is safe; we ignore it | default (not parsed) | log | C |
| A-CAP-03 | MODEL-IDENTIFIER from the handset used to pick a profile (`match.model`); the O201 sends none | EN 300 175-5 §7.7.46; [handset-profiles.md](handset-profiles.md) | O: the O201's model is unknown to us, so its profile is chosen by the user; S: the element is optional | default (dect_mm.c:871, kept for diagnostics) | log | B |
| A-CAP-04 | the handset's codec list in its registration messages (kept in `peer_codecs`); we may answer with the same or a smaller list | TS 102 527-1 §7.3.1; EN 300 175-5 §7.7.54 | S: the lists are the base of later codec negotiation; cross to part C | default (dect_mm.c:871) | log | B |
| A-CAP-05 | vendor ESCAPE-TO-PROPRIETARY from the handset in MM messages (the O201 puts `7b 02 80 04` in its {CC-SETUP}) | EN 300 175-5 §7.7.45 | O: ignored so far and not needed for audio ([vendor-specific-support.md](vendor-specific-support.md) §7.7); H: a vendor handshake that a vendor base answers | default (ignored) | log | C |

#### A-OLD: unusual and old procedures not placed above

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| A-OLD-01 | external handover support in {LOCATE-ACCEPT} ("external handover length indicator"), handover candidates and reference in {MM-INFO-SUGGEST} | EN 300 175-5 §7.7.51, §13.7, §15.7; EN 300 175-6 §5 (external handover length) | S: tells the PP which PARI bits must match for an external handover; with one ARI per cell the PP sees every cell as external | code | frames | C |
| A-OLD-02 | test call back: {LCE-PAGE-REJECT} with reason 0x40 to 0x43 makes the PT call back (test mode) | EN 300 175-5 §14.2.3, §15.8 | S: conformance-test machinery; a handset in a test mode would do it | n/a | frames | C |
| A-OLD-03 | emergency calls from a handset with no subscription (emergency TPUI, IPUI N) | EN 300 175-6 §6.3.1, §8.1 rule 6 | S: a PP may always try an emergency call; ours refuses a handset it does not hold | default (dect_link.c:304, `dect_mm_adopt`) | frames | C |
| A-OLD-04 | B-field set-up, DPRS and ULE pages and codes (LCE headers 110, 111, MAC resume page) | EN 300 175-5 §14.5, §8.2.4.3; EN 300 175-3 §7.2.4.1.4 | S: data services; a voice handset ignores them | chip | frames | C |
| A-OLD-05 | CAP-era procedures: handover, service class, billing and debiting INFO-TYPEs, authentication of the user | EN 300 175-5 §7.7.20, §7.7.39, §13.3.2 | S: kept in the standard for old public-access and PABX phones | code | hand | C |
| A-OLD-06 | headset registration: a headset (no keypad) subscribes only with 0000 and expects "Registration mode automatic access" | TS 102 527-3 §7.4.16.1, §7.10.1.3.1 | S: a Part 3 headset needs the PIN to be 0000 at least temporarily | default (PIN is the user's choice) | hand | C |

---

### Spec text I could not find or that is ambiguous

1. **LCE header 010.** EN 300 175-5 §8.2.4.1 names it "Escape"; table 80b (§8.2.4.3) lists "011, 010" together as "general purpose / unknown". Older notes in [spec-conformance.md](spec-conformance.md) treat 010 as general purpose. I did not find which reading is current.
2. **Release of the link after the access rights procedure.** GAP §8.39 excepts the access-rights and TPUI-assignment cases from the MM "partial release" rule, which implies a normal release; no clause orders the FT to release at once (A-REG-12, A-LOC-11).
3. **Security timers** (MM_encryption_check.1/.2, MM_re-keying.1/.2, MM_early_encryption.1, MM_registration.1) are in EN 300 175-7 annex I.1, which is not in the local set; GAP and EN 300 175-5 only point at it.
4. **"TPUI timer".** The only timers on a TPUI are <MM_ident.1> (A.5) and the DURATION limits (§7.7.13, EN 300 175-6 §6.3.1: T601 min, T603 s); I found no timer by that name. The values of T601, T602, T603 were not looked up.
5. **DURATION units.** `pdftotext` lost the exponent: "28 multiframes" and "216 multiframes" read as 2^8 and 2^16 (our API note says 40.96 s per unit, which fits 2^8 x 160 ms).
6. **MM message type numbers** were not re-read from the coding tables (the column-wise text of the tables is unreliable, as already noted in [spec-conformance.md](spec-conformance.md)); I used the message names only and our `dect_nwk.h` values as given.
7. **USE-TPUI under GAP.** EN 300 175-5 allows it, GAP table 45 lists no such element; whether a GAP PT honours it is not stated.
8. **QH = 4 and QH = C bit numbering.** I decoded only QH = 3 (calibrated by a44); the extended blocks' bit order against annex F.2/F.3 gave implausible results, so A-BCN-05 and A-BCN-06 carry no "ours" value.
9. **Frame sizes.** EN 300 175-4 §7.7.3 allows LI up to 63 on the CS channel; I found no statement of how many octets a PT must accept per I-frame beyond GAP §6.9.3's 63-octet message.
10. **Comment against code.** The comment above `terminate_answer` (dect_mm.c:323) says the base accepts a handset's own deregistration "no authentication first"; `on_terminate_request` (dect_mm.c:360) challenges the handset with its UAK first. One of them is stale.
11. **What the O201 sends in MM.** [vendor-specific-support.md](vendor-specific-support.md) §7 says it "reports no terminal capability or model" (first evidence); §7.7 says it "reports GAP only in its <<TERMINAL-CAPABILITY>>". Both cannot be about the same message; the decoded trace (slice S1) will settle it, and A-CAP-01 and A-CAP-03 depend on it.

### Cross-area dependencies

- **B (call control):** authentication and cipher timing relative to {CC-SETUP} (A-AUT-01, A-CIP-01) and {CC-NOTIFY} TIMER-RESTART (A-AUT-02); release reason 0x0E "partial release" and the link hold <LCE.02> (A-LNK-01); the page-to-SETUP order (A-PAG-06); "encryption activation failed" and "re-keying failed" release reasons (A-CIP-05, A-CIP-08); an NWK message over 63 octets only keeps its first segment at the PT (A-DLC-03).
- **C (audio, codec, U-plane):** CODEC-LIST in the registration messages (A-REG-09, A-CAP-04) sets what a later {CC-SETUP} may assume; the page's LCE header and slot type decide the MAC service and the long slot for G.722 (A-PAG-01, A-PAG-02); the cipher key to the chip and the cipher start precede audio (A-CIP-06); early encryption would change when the first U-plane frame is protected (A-CIP-07).
- **D (display, names, features, lists):** TERMINAL-CAPABILITY drives what may be shown (display, control codes, scrolling; A-CAP-01); MODEL-IDENTIFIER feeds `match.model` (A-CAP-03); the base-name {CLMS-FIXED} broadcast is a display feature of registration (A-REG-10); the link release after a list session (DISC first, A-LNK-02) and the `notify` key sit next to the A540's location registration (A-LOC-11); `timeupdate` pages (A-PAG-10) deliver the date and time.
- **Profile timing:** registration-time variants (A-REG-05 to A-REG-09, A-LOC-01 to A-LOC-04) run before a profile exists for a new handset; only a Finder session or a model match can choose them.


## Part B: call control (what a base can send or do differently around a call)

Written for the handset variation catalog; areas A, C and D are other parts. Sources read for this part (text conversions
in the scratchpad, versions as cited in the repo): EN 300 175-5 V2.9.1 (NWK), EN 300 444 V2.5.1 (GAP), TS 102 527-1 V1.5.1
(NG-DECT Part 1), TS 102 527-3 V1.7.1 (CAT-iq). Every clause number below was read in those texts. Our own notes are linked,
not repeated: `spec-conformance.md` (the CC audit, collision and transaction-value findings), `gap-conformance.md`,
`handset-compatibility.md`, `handset-profiles.md`, `vendor-specific-support.md` §7.7 (the O201 run), `lines-and-calls.md`.
Code read: `src/libopendectcell/dect_cc.c`, `dect_nwk.{h,c}`, `dect_link.c`, `dect_base.c` (ring, dial, nlr, par), `dect_profile.c`,
`src/stubd/dect-stubd.c` (`-a` options, profile "not acted on" list), `profiles/*.profile`.

How to read the columns.

- **Why** starts with `[spec]` (the standard permits or asks for it, nothing about a real handset), `[observed: ...]` (our own
  bench or log, with the note) or `[code]` (found by reading our source, not seen on air).
- **Ours today**: `key:` a profile key (a key that dect-stubd accepts but does not apply yet says "accepted, not applied"), `lab:` a
  `dect-stubd -a` option (per daemon start), `default` (we send exactly this and there is no switch), `code` (a sender or handler would
  have to be written), `chip`, `n/a`.
- **Check**: `frames` = visible in the decoded NWK trace (accept, reject, release, timeout), `ear`, `eye`, `hand` (the user presses
  something), `log`.
- **Rank**: A cheap to send and a plausible cause for a strict or odd handset; B plausible but costly or less likely; C exotic or old,
  for completeness.

Ground rules the standards give a base, which frame everything below (all `[spec]`):

- A handset may keep its receive U-plane closed until it sees PROGRESS-INDICATOR cause 8, and may delay it until CONNECT
  (outgoing) or CONNECT-ACK (incoming): EN 300 175-5 §9.3.1.4, §9.3.2.4. GAP makes the PT connect when the indicator is present
  (EN 300 444 tables 11-13).
- An unexpected CC message other than release is ignored, never answered with a release (EN 300 175-5 §17.4.1; GAP §6.9.4).
- Variable-length IEs go out in ascending identifier order; a receiver may ignore an out-of-sequence one (EN 300 175-5 §7.6, §17.5.1).
- A handset only needs the first 63 octets of a NWK message (GAP §6.9.3).

---

#### Incoming call: the FT-originated {CC-SETUP}

The base sends {CC-SETUP} to make a handset ring (EN 300 175-5 §9.3.2.1; GAP §8.12, table 21). GAP lists PORTABLE-IDENTITY,
FIXED-IDENTITY, BASIC-SERVICE and SIGNAL as the mandatory content; everything else in EN 300 175-5 table 7 is optional.

| ID | Variation (what the base could send or do differently) | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| B-INC-01 | BASIC-SERVICE octet: 0x88 (normal call, wideband default) versus 0x80 (normal call, basic speech default) for this handset | EN 300 175-5 §7.6.4, §9.3.2.1; EN 300 444 table 21; TS 102 527-1 §7.3.2 | A handset that knows only basic speech default attributes may refuse the wideband default. [observed: Motorola O201 answers 0x88 with {CC-RELEASE-COM} 0x0E after about 380 ms and rings on 0x80, `vendor-specific-support.md` §7.7] | key:cc.basic_service (applied), lab:basicsvc, lab:narrowband | frames | A |
| B-INC-02 | Keep 0x88 but put <<CODEC-LIST>> in the SETUP with G.726 first, the standard's "preferred method" for a narrowband-preferring peer (0x80 is its "fall back method (not notably recommended)") | TS 102 527-1 §7.3.3 | [spec] Discriminates INC-01: if the O201 rings on 0x88 plus a G.726-first list, its refusal was about the missing list; if it still releases with 0x0E, the wideband service nibble itself is refused | lab:codeclist=HEX (octets by hand) | frames | A |
| B-INC-03 | Call class of the SETUP: 8 (normal) versus 9 (internal); classes 2, A, B and C are "not applicable" for an incoming call | EN 300 175-5 §9.3.2.1, §7.6.4; EN 300 444 table 21, §8.18; TS 102 527-1 §7.3.6 | [observed: a 0x88 call made the FRITZ!Fon C4 label it "Intern" instead of showing the number, comment on `basicsvc=` in `dect-stubd.c`] | default: class 8 external (`dect_base_basic_svc`, GAP-only handsets get 0x80); class 9 only for internal calls, see B-INT-01 | eye | B |
| B-INC-04 | <<SIGNAL>> in the SETUP: alerting pattern 41H (ours), 40H, 42H-47H, 48H continuous, or none | EN 444 §8.14, tables 23-24; EN 300 175-5 §7.6.8 | [observed: Gigaset A540 shows the call but rings silently without a SIGNAL, run 18, `9c4414c`] [spec] a PT "shall respond to all alerting patterns" but the sound is its own | key:cc.signal.external (applied), lab:signal=, lab:nosignal; cc.signal.internal accepted, not applied | ear | A |
| B-INC-05 | Start alerting from a {CC-INFO} <<SIGNAL>> after the handset's {CC-ALERTING} (GAP figure 31) instead of inside the SETUP | EN 444 §8.11 fig 31, §8.14 (FT must support one method, PT both); EN 300 175-5 §9.3.2.7 | [spec] A handset that mishandles SIGNAL in the SETUP, or only plays its ring in state "call received" | code (the IE and `dect_cc_info()` exist, nothing sends it) | ear | A |
| B-INC-06 | Cadence following: alternate SIGNAL 48H (alerting on, continuous) and 4FH (alerting off) in {CC-INFO} while the handset rings | EN 300 175-5 §9.3.2.7 note, §7.6.8 note 1 | [spec] A handset that plays one flat tone per pattern; for a ring the FT wants to shape | code | ear | C |
| B-INC-07 | Stop a ring with SIGNAL 4FH (alerting off) in a {CC-INFO} before releasing (call answered elsewhere, caller hung up) instead of only {CC-RELEASE} | EN 300 175-5 §7.6.8 | [spec] A handset that keeps ringing until the release arrives late | code (`DECT_SIGNAL_ALERT_OFF` defined, never sent) | ear | C |
| B-INC-08 | Portable identity in the SETUP: the full IPUI (ours, copied from what the handset itself sent) versus the assigned TPUI when the PT answered the page with its TPUI | EN 300 175-5 §9.3.2.1; EN 444 table 21 | [spec] Only a handset that checks the form of the identity | default (`dect_cc_setup_id`, `peer_port_id`) | frames | C |
| B-INC-09 | Fixed identity: the whole PARK including non-significant bits (ours, copied from the handset's own SETUP) versus a rebuilt one | EN 444 table 21 | [spec] A handset comparing the PARK length indicator against the base it locked to | default (`peer_fixed_id`) | frames | C |
| B-INC-10 | Calling party number in the SETUP versus absent versus repeated as display text | EN 300 175-5 §7.7.9; EN 444 table 21 (N.30 optional) | [observed: shown on every handset we hold; the O201 shows its own "Externer Anruf" and the number, `vendor-specific-support.md` §7.7] cross D | key:cc.number.method (accepted, not applied), lab:clip=NUMBER | eye | A |
| B-INC-11 | Calling party name: <<CALLING-PARTY-NAME>> in the SETUP versus display text in a later {CC-INFO} versus both versus none | EN 300 175-5 §7.7.53; EN 444 §8.16 | [observed: Part 1 Gigasets ignore the IE and show display text only; the O201 shows no name at all, `handset-compatibility.md`] cross D | key:cc.name.method, cc.name.display (applied), cc.name.alphabet (accepted, not applied), lab:cnip= | eye | A |
| B-INC-12 | IE order inside our SETUP (identity before <<CODEC-LIST>>, ascending) | EN 300 175-5 §7.6, §17.5.1 | [observed: an out-of-order list put the caller identity behind the codec list for a day, fixed 2026-10-23, invariant in `tests/test_cc.c`] a conforming handset may stop scanning | default (regression row) | frames | B |
| B-INC-13 | SETUP length: a long identity plus name plus codec list plus <<CALL-INFORMATION>> passes the 63 octets every handset must handle (GAP) | EN 444 §6.9.3; EN 300 175-5 A.1 `<CC_segment.1>` | [code] A handset that drops anything after the first DLC segment would lose the name or call information; we cap at 256 octets (`DECT_LAPC_MSG_MAX`), not at 63 | default | frames, eye | B |
| B-INC-14 | <<CALL-INFORMATION>> in the SETUP: line id, line type, call id, call status "CS call setup" (full), a bare line id, or nothing | TS 102 527-3 §7.4.5.3.2, §7.4.6.3, §7.4.6.4.3 | [observed: A540 rings aloud only with the full set, `handset-compatibility.md`] cross D | key:cc.ring_callinfo (accepted, not applied; decided by capability), key:cc.call_status, lab:lineid=, linetype=, callid=, callstatus= | ear | A |
| B-INC-15 | What the SETUP assumes about a handset that reports no <<TERMINAL-CAPABILITY>> (the O201): GAP defaults versus NG-DECT defaults | EN 300 175-5 §9.3.2.9, §7.7.41; EN 444 §8.17 table 29; TS 102 527-1 §7.3.7 table 16 | [observed: the O201 reports GAP only] the defaults differ (full slot only versus full and long slot) | key:cap.ngdect, trust.terminal_capability (applied) | frames | B |
| B-INC-16 | <<ESCAPE-TO-PROPRIETARY>> (7BH, EMC discriminated) in our SETUP | EN 300 175-5 §6.3.2.1 table 7, §7.7.45 | [observed: the O201 puts `7b 02 80 04` (discriminator "unspecified", one content octet 04) in every SETUP it sends; not needed to make it work, §7.7] no evidence it expects one back | code (Step B/C of `vendor-specific-support.md` §3) | frames | B |
| B-INC-17 | BASIC-SERVICE "other" (low nibble F) with explicit <<IWU-ATTRIBUTES>> and <<CALL-ATTRIBUTES>> (annex E.1 values) instead of default attributes | EN 300 175-5 §9.3.2.1, annex E.1; EN 444 §6.9.5 | [spec] A pre-GAP or non-GAP handset that wants its attributes spelled out | code | frames | C |
| B-INC-18 | Prioritized list of attributes: REPEAT-INDICATOR plus up to three <<CALL-ATTRIBUTES>>, <<CONNECTION-ATTRIBUTES>> or <<IWU-ATTRIBUTES>> | EN 300 175-5 §15.2.2, table 7 notes 2-3 | [spec] Old service negotiation; the PT must confirm its pick in its first response | code | frames | C |
| B-INC-19 | Advanced connection: <<CONNECTION-IDENTITY>> and <<CONNECTION-ATTRIBUTES>> in the SETUP (ours omits both, which means "basic connection") | EN 300 175-5 §9.3.2.3 | [spec] A handset that wants the U-plane link association stated. History: the conformance audit misread this once (`spec-conformance.md` Gap 2, corrected) | code | frames | C |
| B-INC-20 | <<CIPHER-INFO>> in the SETUP (advanced ciphering announcement) | EN 300 175-5 table 7, §7.7.10, §15.6 | [spec] Early encryption handsets (N.35) | code | frames | C |
| B-INC-21 | <<PROGRESS-INDICATOR>> cause 8 in the FT's SETUP (allowed F to P) to open the PT's receive U-plane at once | EN 300 175-5 table 7, §9.3.2.4; TS 102 527-3 §7.4.15.2.2.1 | [spec] Without it a PT "may delay connection of the U-plane until receiving {CC-CONNECT-ACK}". [observed: O201 incoming audio opened about 2 s after the answer, §7.7] | code (the indicator is only sent for handset-placed calls, B-OUT-03) | ear | B |
| B-INC-22 | Transaction value of our SETUP: the lowest free (0) versus any other | EN 444 §6.9.2; EN 300 175-5 §7.3 | [observed: the A540 refused value 1 with 0x32 and rang on value 0, runs 15-16, `dd5a630`] | default (`dect_cc_set_next_ti(0)`; no `-a` switch left) | frames | B |
| B-INC-23 | Overlap receiving: SETUP, the PT answers {CC-SETUP-ACK}, the FT sends the number in {CC-INFO}, the PT {CC-CALL-PROC} (optional states T/F-22 and T/F-23) | EN 300 175-5 §9.2.3, §9.3.2.5, §6.3.2.3 note 1; annex B notes ("for further study") | [spec] Meant for DECT used as an intermediate network; a PT that implements it may expect the FT to start that way | code | frames | C |
| B-INC-24 | Ring by LCE page without any SETUP ("collective and group ringing"); the handset answers by going off hook, its SETUP is mapped to the waiting call and answered with {CC-CONNECT} | EN 300 175-5 §14.4, §8.2.1; EN 444 §8.12.3 | [spec] GAP FPs that do this must still send a SETUP afterwards. Covers handsets that ring from the page alone. cross A (paging) | code | ear, hand | C |

#### Incoming call: the handset's answer ({CC-ALERTING}, {CC-CONNECT}, {CC-CONNECT-ACK})

The handset answers {CC-ALERTING} when it rings and {CC-CONNECT} when the user accepts; the FT confirms with {CC-CONNECT-ACK}
and enters ACTIVE (EN 300 175-5 §9.3.2.7-8, EN 444 §8.13-8.15, figures 31-32).

| ID | Variation | Spec | Why | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| B-ANS-01 | A handset that sends {CC-CONNECT} straight from CALL PRESENT without {CC-ALERTING} | EN 300 175-5 annex B table B.2 (Q07 from F-06 and F-07) | [spec] Allowed by the state table; we must not time out or mis-order. Classification row, no change needed | default (CONNECT accepted in CALL PRESENT and RINGING) | frames | C |
| B-ANS-02 | When {CC-CONNECT-ACK} goes out: at once (ours) versus after the U-plane is confirmed and the audio bound | EN 300 175-5 §9.3.2.8, §9.3.2.4; A.1 `P<CC.05>` 10 s | [spec] The PT "may delay connection of the U-plane until receiving {CC-CONNECT-ACK}" and then needs it within 10 s. [observed: on outgoing calls an added CONNECT-ACK changed nothing for the O201, §7.7] | default (queued in `dect_cc_rx`); lab:bindfirst is for handset-placed calls only | ear, frames | B |
| B-ANS-03 | {CC-CONNECT-ACK} carrying display text or <<FEATURE-INDICATE>> or proprietary content | EN 300 175-5 table 13 | [spec] Optional content that some handsets show | code | eye | C |
| B-ANS-04 | "CS call connect" for Part 3 handsets: our {CC-INFO} after {CC-CONNECT-ACK} (never inside the ACK) | TS 102 527-3 §7.4.6.4.3 | [observed: the A540 starts its call timer only on it, `A-0122`, bench 2026-10-25 test 15; Part 1 Gigasets start theirs 3-5 s after the connect with nothing from us] cross D | key:cc.connect_status_info (accepted, not applied); default for Part 3 handsets (`dect_base_cs_connect_tick`) | eye | A |
| B-ANS-05 | What we do with the handset's first response content: its <<CODEC-LIST>> choice, <<TERMINAL-CAPABILITY>> | EN 300 175-5 §9.3.2.9; TS 102 527-1 §7.3.3 note 1 | [spec] The PT chooses the codec in ALERTING or CONNECT; a handset that names none, or G.726 when we offered G.722 | default (`learn_codec_choice`) cross C | frames | C |
| B-ANS-06 | F`<CC.04>` completion timer (100 s) while the handset rings | EN 300 175-5 §9.3.2.10, A.1 | [spec] Optional; ours has none, the ring is bounded by the caller and the bridge | n/a | log | C |
| B-ANS-07 | Cipher request timing relative to the answer: after CONNECT (ours) versus before the SETUP (early encryption) versus none (`-C`) | EN 300 175-5 §15.6; EN 444 N.17 (optional for a residential FT) | [observed: the O201 authenticated and ciphered at the answer; ciphering off changed nothing for its outgoing audio, §7.7] cross A | default (`dect_base_main_cipher_start`), lab:-C | frames | B |

#### Outgoing call: the handset's SETUP and our first answers

The handset sends {CC-SETUP} (call class 8 or 9, basic service); the FT answers with one of {CC-SETUP-ACK}, {CC-CALL-PROC},
{CC-ALERTING} or {CC-CONNECT} (EN 300 175-5 §9.3.1.2; annex B table B.2 Q05, Q09, Q10, Q11 from F-01). GAP figures 1-4 show four
sequences. Ours: SETUP-ACK, digits in {CC-INFO}, a pause (`dialwait`), then CALL-PROC and CONNECT together.

| ID | Variation | Spec | Why | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| B-OUT-01 | First answer: {CC-SETUP-ACK} (overlap, ours) versus {CC-CALL-PROC} versus {CC-ALERTING} versus {CC-CONNECT} at once | EN 444 §8.1 figs 1-4; EN 300 175-5 annex B Q05, Q09-Q11 | [observed: without overlap (CONNECT before the digits) the Gigasets show neither "connected" nor a call timer, bench 2026-10-25 test 2; with it the A540 shows "connected"] | lab:nooverlap (CALL-PROC then CONNECT at once); default overlap | frames, eye | A |
| B-OUT-02 | "Early {CC-CONNECT}": CONNECT right after the SETUP, later call status in {CC-INFO}; also the standard way to open the U-plane for in-band tones | TS 102 527-3 §7.4.6.4.6 fig 47, §7.4.15.2.2.1 fig 74; EN 444 fig 2 | [spec] Opens audio at the first answer without a PI. Rival to B-OUT-03 for the O201 | code (`nooverlap` sends CALL-PROC then CONNECT, not CONNECT alone with a later status sequence) | ear | A |
| B-OUT-03 | <<PROGRESS-INDICATOR>> cause 8 ("in-band information now available") in the first answer ({CC-SETUP-ACK}, else {CC-CALL-PROC}) | EN 300 175-5 §9.3.1.4; EN 444 tables 11-13 (PT shall connect the U-plane when present); TS 102 527-3 §7.4.15.2.2.1, fig 73, fig 66 | [observed: O201 audio at once with it, 3 of 3 calls; before, none or only after seconds on about ten; N.33 "enhanced U-plane connection" is the same feature] | key:cc.progress_in_band (applied; `dect_cc.c` `queue_with_status`) | ear | A |
| B-OUT-04 | PI on later answers too ({CC-CALL-PROC}, {CC-ALERTING}) | EN 444 tables 12-13 | [observed: SETUP-ACK alone 2/2, SETUP-ACK plus CALL-PROC plus ALERTING 3/3, ALERTING alone 0/3, §7.7] so the first answer carries it | default (only the first answer, `progress_sent`) | ear | B |
| B-OUT-05 | PI placed in a {CC-INFO} that connects the U-plane | TS 102 527-1 §7.3.3; TS 102 527-3 §7.4.15.2.2.1 note 2 (FP should avoid it: a PP need not support PI in CC-INFO) | [spec] A handset that only honours PI in a CC message of the call establishment | code | ear | C |
| B-OUT-06 | PI field contents: location 0001 "private network serving the local user" (ours: `1e 02 81 88`) versus 0000 "user", 1010 "beyond interworking point" or 1111 "not applicable" | EN 300 175-5 §7.7.31 fig 73 | [spec] A decoder that checks the location field, or the coding standard bits | code (constants in `queue_with_status`) | ear | B |
| B-OUT-07 | PI descriptions other than 8: 1 "not end-to-end ISDN, in-band may be available", 9 "in-band information not available" (end of an announcement), 5 "service change" | EN 300 175-5 §7.7.31, §10.6.2.1 | [spec] Meant for queue announcements; a handset that opens on 1 | code | ear | C |
| B-OUT-08 | PI IE order: ours appends it after <<CALL-INFORMATION>> (7EH) when call status is on, so 1EH follows 7EH | EN 300 175-5 §7.6, §17.5.1 | [code] A Part 3 handset that needs the PI AND gets call status would see an out-of-sequence IE it "may ignore". The O201 is GAP-only (no call status), so it is unaffected; not seen on air | code defect in `queue_with_status` (the PI is appended after `dect_cc_put_call_status`) | frames | A |
| B-OUT-09 | {CC-SETUP-ACK} carrying <<CODEC-LIST>> with the chosen codec together with the PI (the reference flow); ours names the codec in {CC-CONNECT} only | TS 102 527-1 §7.3.3 note 1; TS 102 527-3 fig 66 and fig 73 | [spec] The recommended message for outgoing calls is SETUP-ACK, CALL-PROC or CONNECT; a handset that configures its U-plane only when it knows the codec | code (`queue_connect` only; `peer_negotiates`) cross C | frames, ear | A |
| B-OUT-10 | Dial tone: SIGNAL 00H in {CC-SETUP-ACK} versus in-band dial tone versus none | EN 300 175-5 §7.6.8; TS 102 527-3 §7.4.15 table 73, §7.4.15.2.2 | [spec] For GAP and Part 1 handsets the FP "shall" provide ring-back and busy in band, dial tone is optional; a silent line after pickup is what some users read as "no connection" | code | ear | B |
| B-OUT-11 | Ring-back: SIGNAL 01H in {CC-ALERTING} versus in-band ring-back (early media) versus none | EN 300 175-5 §7.6.8; TS 102 527-3 §7.4.15 | [spec] Part 1 handsets get the in-band method and must NOT also get a SIGNAL (double tone, §7.4.15.2.2). Early media is not built (`Limits.md`) | code | ear | B |
| B-OUT-12 | Busy: SIGNAL 04H plus "CS call disconnecting" then CS idle then release (Part 3, ours for a busy system) versus a plain {CC-RELEASE} "user busy" | TS 102 527-3 §7.4.8.3, §7.4.6.4.7 | [code] Far-end busy goes out as a release with a reason only; Gigasets show what they show | default for system busy (`busy_notify`); far-end busy: default release reason 0x14 | ear, eye | B |
| B-OUT-13 | {CC-ALERTING} to the caller after CALL-PROC (far end rings), or skipped | EN 300 175-5 §9.3.1.7 (ALERTING only when the U-plane is fully installed); EN 444 fig 3 | [observed: `alertms` experiments on the A540's missing call timer; the timer was solved by call status instead, `bench-2026-10-25.md` tests 12e and 15] | lab:alertms=N (and routed calls send it when the far end rings) | eye | B |
| B-OUT-14 | CONNECT only after the U-plane is installed and confirmed (bind first) | EN 300 175-5 §9.3.1.8; EN 444 §8.6 | [observed: `bindfirst` changed nothing for the O201, §7.7] | lab:bindfirst; key:cc.bind_before_connect (accepted, not applied) | ear | B |
| B-OUT-15 | Content of our {CC-CONNECT}: chosen codec alone (ours when the SETUP negotiates), plus "CS call connect" for Part 3; or a bare CONNECT | EN 300 175-5 table 12; TS 102 527-1 §7.3.3; TS 102 527-3 §7.4.6.4.3 | [observed: all four handsets in `handset-compatibility.md`; `codec_unmatched` counts handsets that negotiate but offer no G.726] cross C | default (`queue_connect`); key:cc.call_status | frames | B |
| B-OUT-16 | {CC-CONNECT-ACK} after our {CC-CONNECT} on a handset-placed call (the standard leaves it out for this direction) | EN 300 175-5 A.1 note under `<CC.05>` | [observed: added in an experiment, three O201 calls, `83 0f` in the trace, no change; not committed, §7.7] | code (experiment patch outside the repo) | ear | C |
| B-OUT-17 | Called party number or name back to the handset in {CC-CALL-PROC}, {CC-ALERTING} or {CC-CONNECT} (resolved contact name) | EN 300 175-5 tables 10-12 (<<CALLED-PARTY-NUMBER>>, <<CALLED-PARTY-NAME>> F to P optional); EN 444 §8.17 (the FT must not echo keys) | [spec] A handset display that shows the dialled party; echo of typed digits is forbidden | code | eye | C |
| B-OUT-18 | When the PT's authentication must have succeeded relative to the call: the number waits for the proof (ours, `authfirst`) versus the call runs and the proof comes later | EN 444 N.9 (optional), EN 300 175-5 clause 13 | [observed: the O201 authenticated and ciphered before it rang, §7.7] cross A | lab:authfirst, noauthfirst (default per daemon) | frames | B |
| B-OUT-19 | Where the handset's call goes when the controller is slow: SETUP-ACK held back while the stack fetches a roamer's record versus answered at once (ours) | EN 300 175-5 §9.3.1.5 | [code] `answer_held_setup` answers SETUP-ACK or CALL-PROC immediately so the handset does not wait out its 20 s timer | default (`dect_link.c`) | frames | C |

#### Dialling and number reception

The number arrives as <<MULTI-KEYPAD>> in {CC-INFO} (GAP) or in other forms the standard allows; the FT decides when dialling is
complete (EN 300 175-5 §9.3.1.5; EN 444 §8.3, §8.10).

| ID | Variation | Spec | Why | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| B-DIAL-01 | End of dialling: inter-digit timeout (ours `dialwait` 3000 ms) versus <<SENDING-COMPLETE>> in the SETUP or an INFO versus a shorter timeout versus a dial plan | EN 300 175-5 §9.3.1.5, §7.6.2; EN 444 §8.3 | [observed: a 3 s pause after the last digit on every handset; the O201 sends digits in {CC-INFO} with no SENDING-COMPLETE, §7.7; the maintainer notices the pause] | lab:dialwait=MS (not a profile key; the controller does not expose it); SENDING-COMPLETE honoured by default | eye, hand | A |
| B-DIAL-02 | {CC-SETUP-ACK} with <<DELIMITER-REQUEST>>, asking the PT to send <<SENDING-COMPLETE>> when the number is done | EN 300 175-5 §6.3.2.3 note 6, §7.6.2 | [spec] A handset that honours it ends the dial pause at once | code | frames | B |
| B-DIAL-03 | En-bloc number in <<CALLED-PARTY-NUMBER>> (70H) in the SETUP or an INFO instead of keypad | EN 300 175-5 §9.3.1.5, §7.7.7, tables 7-8 | [spec] GAP forbids it ("MULTI-KEYPAD is used"), a non-GAP handset may use it and we would see no digits. [observed: the O201 uses keypad] | code (identifier 70H not parsed) | frames | B |
| B-DIAL-04 | One character per message in <<SINGLE-KEYPAD>> (E9H) | EN 300 175-5 §7.6.6, tables 7-8 | [spec] Older handsets; ours parses only <<MULTI-KEYPAD>> (2CH) | code | frames | B |
| B-DIAL-05 | Keypad control characters: pause 05H, go to pulse 12H, DTMF defined 14H, register recall 15H, DTMF infinite 16H, null 00H | EN 300 175-5 annex D table D.1; EN 444 table 20, N.5-N.7, N.22-N.23 | [code] Only 0-9, `*`, `#` reach the digit event; the rest is ignored, so a handset that sends 16H then a digit then 00H has the digit kept and the tone length lost | default (`dect_base_par_tick`) | frames | C |
| B-DIAL-06 | Line choice for GAP and Part 1 handsets: `#` plus a digit in a {CC-INFO} (backward compatible), the FP does not confirm | TS 102 527-3 §7.4.5.2.3, table 26 | [spec] A handset of an older generation selecting among our lines; cross D | key:line.selection (accepted, not applied) | hand | B |
| B-DIAL-07 | Digits in the SETUP itself (<<MULTI-KEYPAD>> in {CC-SETUP}) | EN 300 175-5 table 7 note 10 | [spec] A handset that dials en bloc in the SETUP | default (collected, `learn_keypad`) | frames | C |

#### Call release

Normal release is {CC-RELEASE} answered by {CC-RELEASE-COM}; abnormal release is a {CC-RELEASE-COM} alone (EN 300 175-5 §9.5.1-9.5.3;
EN 444 §8.7-8.9). "Partial release" (reason 0EH, feature N.21) means a follow-on call is expected and the link stays up a few seconds.

| ID | Variation | Spec | Why | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| B-REL-01 | Answer to the handset's {CC-RELEASE}: {CC-RELEASE-COM} with no reason (ours) versus with the same reason, 0EH when it asked for partial release | EN 444 §8.9 table 19 (always include 0EH when partial release was requested and N.21 is supported); EN 300 175-5 §9.5.1 | [observed: the O201 ends every call, a normal hang-up too, with reason 0x0E, §7.7] [spec] a handset that uses N.21 may expect the echo | default (`on_release`) | frames | A |
| B-REL-02 | Keep the link a few seconds after a partial release (timer `<LCE.02>`) before releasing it | EN 300 175-5 §9.5.1 note 3, §14.2.7.2; EN 444 §8.9 | [spec] The O201's follow-on activity (a list call, a second call) would find the link. [code] ours releases the link at once (0x0202, `nlr`) unless the profile keeps it for good | default (`dect_base_nlr_tick`); key:lce.release_after_call (yes releases at once, no never) | frames | A |
| B-REL-03 | Release reason in our own {CC-RELEASE} at a normal hang-up: none (read as normal) versus 00H versus 0EH | EN 300 175-5 §7.6.7, §9.5.1 | [spec] A handset that behaves differently on an explicit reason | default none (`dect_cc_release_reason` takes any) | frames | B |
| B-REL-04 | Reason when we refuse or end a call: 32H insufficient resources (collision, busy) versus 31H overload, 33H insufficient bearers, 34H IWU congestion, 14H user busy, 12H user unknown | EN 300 175-5 §7.6.7, §9.5.2.3, §17.3.3 | [observed: 32H was the A540's answer to a collision, `spec-conformance.md` 2026-10-24] A handset shows different text per reason | default (reason chosen per case in `dect_base.c`) | eye | B |
| B-REL-05 | A handset we do not know: {CC-RELEASE-COM} 0AH "unknown identity" versus 03H when no identity is present | EN 444 §8.8; EN 300 175-5 §17.6.1 | [spec] Recommended by the profile | default (`setup_from_stranger`) | frames | C |
| B-REL-06 | Ending a ringing call (caller gave up): {CC-RELEASE} (ours) versus {CC-RELEASE-COM} | EN 444 §8.12.2.1 fig 34; EN 300 175-5 §9.5.1 note 1 | [spec] GAP shows the normal release in F-06 | default | frames | C |
| B-REL-07 | Part 3: "CS call disconnecting" with a reason (user busy, number not available, system busy) and a tone, then CS idle, then the release, for a far-end failure | TS 102 527-3 §7.4.6.4.7, §7.4.6.4.8, §7.4.8.3 | [code] Only the system-busy case does it; a far-end busy goes out as a release with a reason | code (far-end cases) cross D | eye | B |
| B-REL-08 | Display text, progress indicator or {FACILITY} in the release (CRSS during release, e.g. advice of charge) | EN 300 175-5 tables 14-15, annex B note 2 | [spec] Optional extras | code | eye | C |
| B-REL-09 | Data link reset or failure during a call: maintain the ACTIVE call and ask the DLC to re-establish the link, or clear it | EN 300 175-5 §17.8, §17.9 (c: an abnormal release "may" keep an active call, `<LCE.04>` bounds the wait) | [code] `dect_base_conn_down` ends the link and the call with it; no re-establishment is requested (`Limits.md`: a call does not survive its cell) | default | frames, log | C |

#### Timers and {CC-NOTIFY}

Timers on the FT side: `<CC.01>` 20 s (overlap), `<CC.02>` 36 s (release), `<CC.03>` 20 s (setup), `<CC.04>` 100 s (optional, completion),
`<CC.05>` 10 s (CONNECT-ACK, PT side), `<CC_service>` 20 s, `P<CC.06>` 1.5 s (PT, setup crossing) (EN 300 175-5 A.1).

| ID | Variation | Spec | Why | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| B-TMR-01 | Send {CC-NOTIFY} with <<TIMER-RESTART>> while we make the handset wait (authentication, the dial pause, fetching a roamer's record) so its `P<CC.03>` and `<CC.04>` restart | EN 444 §6.9.6 (mandatory in a PT, optional in an FT), §8.2.1.1; EN 300 175-5 §6.3.2.13, §7.6.9, §9.3.1.2 | [spec] The handset's 20 s setup timer ticks during our delays, and a long wait looks like a dead base ("---" on the display, the O201 "takes a while") | code (`DECT_CC_NOTIFY` is defined, no sender) | frames | B |
| B-TMR-02 | Timer values and states we implement: `<CC.01>` 20 s with restart per {CC-INFO}, `<CC.02>` 36 s, `F<CC.03>` 20 s; not `<CC.04>` | EN 300 175-5 A.1 | [observed: `F<CC.03>` implemented 2026-10-22; every unanswered ring used to hang in CALL PRESENT] | default (`dect_cc_tick`) | log | C |

#### Collisions and concurrent calls

A {CC-SETUP} from each side at once, or a call arriving during a call (EN 300 175-5 §9.5.2.3, §17.3.3).

| ID | Variation | Spec | Why | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| B-COL-01 | After the PT released our SETUP with 32H (its own SETUP crossed ours): re-present at once (ours, up to 2 times) versus ring again from the start versus give up | EN 300 175-5 §9.5.2.3, A.1 `P<CC.06>` 1.5 s | [observed: the A540 opens a CAT-iq list call on every link and refuses ours with 32H, rings when re-presented; the same, `spec-conformance.md` 2026-10-24, bench 2026-10-28] | default (`DECT_CC_REPRESENT_MAX`, ring requeue `DECT_BASE_REQUEUE_MAX`) | frames | A |
| B-COL-02 | Our answer to the PT's SETUP that crosses ours: {CC-RELEASE-COM} 32H and keep our call (ours) versus accept its call and drop ours | EN 300 175-5 §9.5.2.3 (both sides abnormally release the other's call), §17.3.3 | [spec] The standard fixes this; the variation is only who re-presents | default | frames | B |
| B-COL-03 | Do not ring while the link carries a list session or an MM procedure (ours waits up to `DECT_BASE_IN_USE_WAIT_MS`) versus ring at once | EN 300 175-5 §14.2.3 (page only when no suitable link), §14.2.1 | [observed: a ring during a call cut the call, spec sweep F-2] avoids the 32H collision at the source | default (`ring_in_use`) cross A | frames | B |
| B-COL-04 | Release collision (both send {CC-RELEASE}): stop `<CC.02>` and go to NULL, no RELEASE-COM | EN 300 175-5 §9.5.3; EN 444 §8.7.2.1 | [spec] The FT must not answer with a RELEASE-COM it does not need to | default (`release_collisions`) | frames | C |

#### Call classes and service types the handset places

The BASIC-SERVICE call class says what the handset wants (EN 300 175-5 §7.6.4). We serve 2 (list access, answered with CALL-PROC and never
connected), 8, 9, A and C; we release 3, 4, 7, B, D, E with 06H "service not implemented" (`served_call_class`).

| ID | Variation | Spec | Why | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| B-CLS-01 | List access service call (class 2): {CC-CALL-PROC} only, no CONNECT; the list dialog runs on the transaction | TS 102 527-3 §7.4.10.6.1; EN 300 175-5 §7.6.4 note 1 | [observed: the A540 opens one on every link, which causes the collisions of B-COL-01] cross D | default (`on_setup`) | frames | B |
| B-CLS-02 | Service call (class B, keypad 18H, feature N.32): answered for service dialling versus rejected (ours: 06H) | EN 444 §8.20-8.21; EN 300 175-5 §7.6.4 | [observed: the AS690H places a service call (`e0 b8` plus a Gigaset proprietary escape) after registering; we release it] | default (`served_call_class`) | frames | B |
| B-CLS-03 | Emergency call (class A): FP must accept it regardless of identity contents (IPEI, empty FIXED-IDENTITY) and route it | EN 300 175-5 §9.8 | [code] Our identity gate turns strangers away with 0AH, so an emergency call from an unsubscribed handset is refused; a subscribed one is a speech call | default (`setup_from_stranger`) | frames | C |
| B-CLS-04 | Message call (4), ULE (3), DECT/ISDN IIP (7), supplementary service call (D), OA&M (E) | EN 300 175-5 §7.6.4 | [spec] Refused with 06H; OA&M and supplementary classes carry handset-vendor features | default | frames | C |
| B-CLS-05 | Basic services other than the two speech defaults: 4 and 6 (GSM or UMTS IWP, SMS), 9 and A (SUOTA via DPRS), B (DTAM wideband, treated as speech), F ("other" with IWU-ATTRIBUTES) | EN 300 175-5 §7.6.4, §9.3.1.1 | [spec] Data and update services; ours has no IWU for them | default (`dect_cc_peer_is_speech`) | frames | C |
| B-CLS-06 | External handover call (class C): accepted as speech | EN 300 175-5 §15.7 | [spec] Not an FP-side feature of ours | default | frames | C |

#### Internal calls

Handset to handset through the base (call class 9, keypad 17H, GAP N.31).

| ID | Variation | Spec | Why | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| B-INT-01 | Class of the SETUP to the called handset: 9 for NG-DECT handsets, 8 ("external") for GAP and Part 1 handsets | TS 102 527-1 §7.3.6 tables 13-14, notes 2-3 | [observed: the C430A shows "1", the AS690H rang 29 s and sent nothing until the caller hung up (its red key only silences the ringer); the A540 showed "via Intern DECT", `handset-compatibility.md`] | default (`ring_svc_for`, `dect_base_basic_svc`); `cc.signal.internal` accepted, not applied; 40H is the default pattern | eye, hand | A |
| B-INT-02 | The prefix 17H plus the number versus 17H plus `*` ("internal general call", all handsets) | EN 444 §8.19; TS 102 527-3 table 17 and note 1 | [spec] Optional GAP feature | default for 17H plus number; `*` code | hand | C |
| B-INT-03 | Ring-back and busy to the internal caller: SIGNAL 01H or 04H, or in band, with the busy case as "CS call disconnecting" | TS 102 527-3 §7.4.15, figs 75-76, §7.4.8.3 | [spec] Part 1 handsets get in-band tones | default busy (`dect_cc_busy_system`); ring-back code | ear | B |
| B-INT-04 | Both handsets in the highest common codec (G.722 where both offer it) | TS 102 527-3 §7.4.3.9 | [spec] Internal-call audio quality cross C | chip, code | ear | C |

#### Second calls, hold, transfer (Part 3 and legacy)

TS 102 527-3 §7.4.3 describes parallel calls on one link, one U-plane and one CC instance; the control codes are keypad characters
(table 17). EN 300 175-5 §10.4.1 has an older separate-message hold. Our parallel-call support is built and not yet seen on air
(`lines-and-calls.md`).

| ID | Variation | Spec | Why | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| B-PAR-01 | Call waiting indication to a Part 3 handset in a call: {CC-INFO} with SIGNAL 07H, the caller and "CS call setup" | TS 102 527-3 §7.4.3.5.2, table 17 | [spec] The handset builds its second-call menu from it | default (`dect_cc_call_waiting`, built, not seen on air) | ear, eye | B |
| B-PAR-02 | Call waiting for a GAP or Part 1 handset: an in-band call waiting tone (optional) versus busy toward the network (ours) | TS 102 527-3 §7.4.15 table 73, §7.4.6.4.1 | [observed: C430A and AS690H get busy for a second call, `handset-compatibility.md`] | default (busy) | ear | B |
| B-PAR-03 | Accept, reject, release-and-accept (1CH 35H, 36H, 38H); toggle, release one, hold, resume (1CH 31H, 33H, 41H, 42H) | TS 102 527-3 table 17, §7.4.3.5.3-7.4.3.5.12 | [spec] A handset menu expects each acknowledged by call status | default (`par_control`), built, not seen on air | eye | B |
| B-PAR-04 | A second outgoing call: 1CH 15H plus a number (external), 17H plus a number (internal) | TS 102 527-3 table 17, §7.4.3.5.1 | [spec] | default, built, not seen | hand | B |
| B-PAR-05 | Call transfer: 1CH 34H announced or unannounced, "CS call under transfer", call id update | TS 102 527-3 §7.4.3.6 | [code] Ours answers with the negative acknowledgement "control code not supported" (§7.4.3.4) | code | hand | C |
| B-PAR-06 | 3-party conference (1CH 32H, "CS conference connect") | TS 102 527-3 §7.4.3.7 | [code] Not supported, negative acknowledgement | code | hand | C |
| B-PAR-07 | Intrusion call, implicit and explicit (1CH 40H in the SETUP), call interception by a headset (1CH 50H), call deflection (1CH 39H) | TS 102 527-3 §7.4.3.8, §7.4.16.2, §7.4.4 | [code] Not supported | code | hand | C |
| B-PAR-08 | Remote status notification "CS call remote connect or hold" | TS 102 527-3 §7.4.3.5.13, table 30 | [spec] Optional on both sides | code | eye | C |
| B-PAR-09 | PSTN lines where a second call is signalled in band (DCIBS lines) | TS 102 527-3 §7.4.3.10 | [spec] Not applicable to a SIP-only base | n/a | n/a | C |
| B-PAR-10 | The older hold protocol: {HOLD} (24H), {HOLD-ACK} (28H), {HOLD-REJECT} (30H), {RETRIEVE} (31H), {RETRIEVE-ACK} (33H), {RETRIEVE-REJECT} (37H) | EN 300 175-5 §10.4.1, table 70, tables 22-27 | [code] A handset with a hold key that waits in its auxiliary state "hold request" gets no answer: our CC ignores the messages. A {HOLD-REJECT} costs little | code | hand | B |
| B-PAR-11 | Register recall (R key, 15H) | EN 444 N.5, table 1 note 5 | [spec] FT optional; today dropped | n/a | hand | C |

#### Supplementary services and old procedures

| ID | Variation | Spec | Why | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| B-SUP-01 | Keypad protocol: the PT sends network-dependent codes in <<KEYPAD>>, the FT answers with <<DISPLAY>> | EN 300 175-5 §10.2 | [spec] An FT that cannot support a service "shall ignore the request" | default (keys go to the bridge as digits) | eye | C |
| B-SUP-02 | Feature key management: <<FEATURE-ACTIVATE>> (38H) in the SETUP or an INFO, <<FEATURE-INDICATE>> (39H) back | EN 300 175-5 §10.3, tables 7-8 | [spec] Handsets with soft keys for services | code | hand | C |
| B-SUP-03 | Functional protocol: <<FACILITY>> in CC messages and the {FACILITY} message for call related supplementary services | EN 300 175-5 §10.4.2.1, table 21 | [observed: the FRITZ!Fon C4 sends a {CC-FACILITY} with proprietary content; unanswered it kept the link busy, so we close the transaction, `dect_cc.c` `queue_release_other`] | default (cleared with 02H on a transaction we do not hold) | frames | B |
| B-SUP-04 | Queue management for outgoing calls: PI cause 8 for the announcement, <<FEATURE-INDICATE>> for the position, {CC-NOTIFY} timer restart, PI cause 9 at the end | EN 300 175-5 §10.6.2.1 | [spec] Old PSTN-trunk behaviour | code | ear | C |
| B-SUP-05 | Indication of subscriber number, echo control, cost information | EN 300 175-5 §10.6.2.2-§10.6.2.4 | [spec] Old network services | code | eye | C |
| B-SUP-06 | <<TEST-HOOK-CONTROL>> (on or off hook) from the FT in a {CC-INFO} | EN 300 175-5 §7.6.10, table 8 | [spec] A test feature | code | n/a | C |
| B-SUP-07 | DTMF in a call: keypad digits to the bridge (ours); an in-band DTMF echo from the FP to a GAP PP is optional and not sent | TS 102 527-3 §7.4.14; EN 300 175-5 annex D | [spec] A PP "shall not generate any in band audio DTMF" | default | ear | C |
| B-SUP-08 | Other character sets: locking and non-locking shift, <<ESCAPE-FOR-EXTENSION>> (7FH) | EN 300 175-5 §7.5.2-§7.5.4, annex D.2.4 | [spec] Display and keypad content of handsets with extra sets | code | eye | C |
| B-SUP-09 | Test call back requested through {LCE-PAGE-REJECT} | EN 300 175-5 §14.2.3, §15.8 | [spec] Test feature cross A | code | frames | C |

#### Service change and codec change in a call

| ID | Variation | Spec | Why | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| B-SVC-01 | The handset's {CC-SERVICE-CHANGE} (codec change, IWU attribute change): rejected with {CC-SERVICE-REJECT}, no reason, no codec list (ours) | EN 300 175-5 §9.6.1; TS 102 527-1 §7.3.4 | [spec] The PT must be told promptly; a handset that wants the change keeps trying or drops to G.726 | default (`service_rejected`) cross C | frames | B |
| B-SVC-02 | Accept the codec change and send {IWU-INFO} <<CODEC-LIST>> from both sides, each side muting its receive path from the accept to the IWU-INFO | TS 102 527-1 §7.3.4, §7.3.5.1 | [spec] The mandatory NG-DECT procedure; needs a chip slot-type change cross C | code, chip | ear | B |
| B-SVC-03 | A service change started by the FP (for example G.722 to G.726 when the far end is narrowband) | TS 102 527-1 §7.3.4, §7.3.3 | [code] `dect_cc_prefer_narrowband` only changes the choice before CONNECT | code | ear | C |
| B-SVC-04 | Fall back with {IWU-INFO} <<CODEC-LIST>> after a failed slot-type change | TS 102 527-1 §7.3.3, §7.3.5.1 | [code] We do not send or answer it (`dect_iwu` handles <<IWU-TO-IWU>> only) | code cross C | ear | C |
| B-SVC-05 | Bandwidth change, connection reversal, rerouting, suspend and resume, modulation change; NWK layer suspend and resume of a call | EN 300 175-5 §9.6.2-§9.6.5, §9.7.4 | [spec] Advanced connections and packet mode only | n/a | n/a | C |
| B-SVC-06 | Change mode "Profile or basic service and IWU attributes change" (a call switching narrowband and wideband default attributes) and "voice and data change" | EN 300 175-5 §7.7.38 | [spec] Another way to switch codec | code | n/a | C |

#### Lower layer per call (link, paging, segmentation)

How the base brings a link up for each call and takes it down (EN 300 175-5 §14.2). The paging and DLC side is area A; the rows here are the
per-call decisions.

| ID | Variation | Spec | Why | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| B-LNK-01 | Page for every call after releasing the link (ours, as stock does) versus a SETUP on an already linked handset versus the existing-link fallback after 16 unanswered pages | EN 300 175-5 §14.2.1, §14.2.3 | [observed: the FRITZ!Fon C4 ignores pages while it holds a link, vF423; the fallback SETUP gave a ringing call with no audio] cross A | default (`dect_base_ring_tick`) | frames | A |
| B-LNK-02 | The short LCE confirm stock sends between the page response and the SETUP (`00 91 21 01 45`, from the room's notes); we send none | `spec-conformance.md` "Still to audit" (clause 14); EN 300 175-5 §14.2.3 | [observed in a capture, not found in the standard] A handset that expects it | code cross A | frames | B |
| B-LNK-03 | Delay between link-up and our SETUP: at once (ours, about 120 ms) versus 360 to 1600 ms | EN 300 175-5 §14.2.3 | [observed: no change on the A540 refusal in the 2026-10-24 timing sweep; the option is gone from the current daemon] | default | frames | C |
| B-LNK-04 | Direct FT-initiated link establishment instead of paging | EN 300 175-5 §14.2.4 | [spec] Needs a location table | chip | frames | C |
| B-LNK-05 | Keep the CC messages inside one DLC segment (63 octets), see B-INC-13 | EN 444 §6.9.3 | [spec] | default (no cap) | frames | B |

#### Message and transaction handling

| ID | Variation | Spec | Why | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| B-MSG-01 | A message on a transaction we do not hold: {CC-RELEASE-COM} 02H (ours) versus ignoring it; an unexpected message on our own call is ignored | EN 300 175-5 §17.3.2.1, §17.4.1; EN 444 §6.9.4 | [observed: the first reviewers' findings, fixed in `0ac7e38`, `6400c26`] | default | frames | B |
| B-MSG-02 | Extended transaction value (TV 7 plus an octet): ignored (ours) | EN 300 175-5 §7.3, §17.3.1 | [spec] Only for more than 7 parallel calls | default | frames | C |
| B-MSG-03 | Connectionless transaction value 6: dropped by CC | EN 300 175-5 §7.3, §10.4.2.3 | [spec] Handled by the {FACILITY} path elsewhere | default | frames | C |

---

### Spec text I could not find or that is ambiguous

- **EN 444 §8.16** (display in CC messages): the sentence "A <<DISPLAY>> information element may be included in any CC messages in the FT => PT direction except in" is cut off in the text conversion; EN 300 175-5 §10.2 says "except {CC-NOTIFY} and {IWU-INFO}", which I used.
- **EN 300 175-5 table 68 versus §7.3:** the table allows CC transaction values "0 to 6 + extend"; the text says 6 is for connectionless transactions only. I read it as 0 to 5 for calls and keep B-MSG-02/03 as C.
- **EN 300 175-5 §9.3.1.7 versus §9.3.2.3 on "first response message":** the FT-initiated list of first responses omits {CC-SETUP-ACK}, while table 9 notes its P to F use for "incoming call proceeding"; those optional states (annex B note 1: "for further study") are not specified further. Used only for B-INC-23.
- **EN 300 175-5 annex A.1 `<CC.04>`:** start and stop are "refer to clause 9.3"; the text defines no more. B-ANS-06 treats it as optional per §9.3.2.10.
- **EN 300 175-5 annex A.1 note on `<CC.05>`:** "in the direction FT to PT {CC-CONNECT-ACK} applies only for external handover" reads against §9.3.2.8 for a call the FT originates; I read it as covering a call the PT originates (the FT answers CONNECT with CONNECT, no ACK). B-OUT-16 follows that reading.
- **Which PD the HOLD and RETRIEVE messages use:** §7.2 note 2 says CC messages include the CRSS messages, table 69 lists no hold message, table 70 gives their types (24H, 28H, 30H, 31H, 33H, 37H); I assumed PD CC with those types (B-PAR-10). No handset of ours is known to send them.
- **TS 102 527-3 §7.4.15.2.2.1 note 2:** the sentence on PROGRESS-INDICATOR in {CC-INFO} "on PP side in ..." is cut off; I used only the part that is legible (the FP should avoid it).
- **PI "location" values:** §7.7.31 lists locations but no profile says which one a base must use; B-OUT-06 is a pure variation, not a requirement.
- **Whether a Motorola-style partial release (0EH) from the handset in a plain hang-up obliges the FT to keep the link:** EN 444 §8.9 says both CC entities "shall request a delayed link release" (when N.21 is supported by the requested side); the length "a few seconds" is not given, only the timer name `<LCE.02>`.

### Cross-area dependencies

- **A (identity, registration, security, paging, link):** B-INC-08/09 (identity forms), B-INC-24 and B-LNK-01 to 04 (paging, page response, LCE confirm, direct link), B-ANS-07 and B-OUT-18 (ciphering and authentication around the call), B-COL-03 (link in use by an MM or list session), B-REL-02 (when the link is released, `<LCE.02>`), B-INC-15 (what the last {LOCATE-REQUEST} said about the terminal).
- **C (audio, codec, U-plane, bearers):** B-INC-01/02 (service nibble and codec list), B-OUT-02/03/04/09 and B-ANS-02 (when the U-plane opens and what tells the handset), B-OUT-14 (bind before CONNECT), B-OUT-15 and B-SVC-01 to 04 (codec choice and change, slot type, {IWU-INFO} fallback), B-INT-04 (codec priority), B-SUP-07 and B-OUT-10/11 (tones and DTMF in band), early media.
- **D (display, names, features, lists):** B-INC-10/11/14 (CLIP, CNIP, call information), B-ANS-04 and B-REL-07 (call status, reasons), B-CLS-01 (list access calls and the collisions they cause), B-DIAL-06 (line selection), B-PAR rows (parallel-call features), B-SUP-01/08 (keypad and display protocol, other character sets), B-INT-01 (how the caller's name shows on an internal call).


## Part C: audio path, codecs, U-plane and bearers

Written 2026-11-05 for the handset variation catalog. Source: the specs in `docs/specs/` (EN 300 175-3, -4, -5, EN 300 444 V2.5.1,
TS 102 527-1 V1.5.1, TS 102 527-3 V1.7.1; every clause number below was read in the text, not recalled), our notes
([vendor-specific-support.md](vendor-specific-support.md) §7.7, [stub-api.md](stub-api.md) §3.5-3.8, [two-calls.md](two-calls.md),
[bridge-interface.md](bridge-interface.md), [handset-profiles.md](handset-profiles.md), `journals/call-g7.md`) and the code
(`src/libopendectcell/dect_cc.c`, `dect_mm.c`, `dect_base.c`, `src/stubd/dect-stubd.c`). EN 300 175-8 (speech coding, audio types)
and EN 300 175-2 (PHY) are **not on disk**; rows that depend on them say so.

How to read a row. *Variation* = what the base could send or do differently. *Spec* = where the standard permits or requires it.
*Why* starts with the evidence class: **spec-permitted** (the standard allows or asks for it, nobody has seen a handset need it) or
**observed** (a bench note of ours, cited). *Ours today*: `key:` a profile key, `lab:` a `dect-stubd -a` option, `default` (we send
exactly this, no alternative), `code` (a sender or handling would have to be written), `chip` (the chip's, not ours), `n/a`.
*Check*: `frames` (a decoded capture settles it), `ear`, `eye`, `hand` (a person does something), `log`. *Rank*: A cheap and plausible,
B plausible but costly or less likely, C exotic or old, kept for completeness.

### Notes for the editor (what the reading turned up)

1. **Our progress indicator goes out after CALL-INFORMATION.** `queue_with_status()` (`dect_cc.c:125-135`) appends
   <<PROGRESS-INDICATOR>> (0x1E) after the call status (0x7E). EN 300 175-5 §7.5.1 says variable-length elements go out in
   ascending identifier order so a receiver may stop scanning once past the one it wants. The O201 gets no call status, so it is not
   affected; a Part 3 handset given `cc.progress_in_band` would be. See C-UP-07.
2. **`bindfirst` and `alertms` do nothing on a bridged call.** `dect_base_call_out()` (`dect_base.c:1542-1571`) answers with CALL-PROC
   and returns, so `dect_base_dial_connect()` (`dect_base.c:1645`, the only user of `bind_first` and `alert_ms`) never runs while a bridge is
   connected. The §7.7 line "bindfirst: no change ... order of bind and CONNECT ruled out" may therefore be **void**; the run's log would
   not show "CONNECT now ... the U-plane is up". The profile key `cc.bind_before_connect` is parsed and acted on nowhere (`dect-stubd.c:4688`
   says so at start-up). See C-BIND-01.
3. **The standard's default for a missing <<TERMINAL-CAPABILITY>> is "full slot only"** (EN 300 444 §8.17 table 29), and we read the element
   only in MM (`dect_mm.c:871-891`), never in a CC-SETUP or first response (EN 300 175-5 §9.3.1.1, §9.3.2.9 allow both). Until a handset's
   capability is learned we offer 0x88, the opposite default. See C-TCAP-01, C-TCAP-02.
4. **TS 102 527-3 figures 66 and 73 show the same message pair the O201 needed:** {CC-SETUP-ACK} with the chosen <<CODEC-LIST>> **and**
   <<PROGRESS-INDICATOR>> "in-band" in one message, "U-plane connected" after it. Our key sends the progress indicator but never the codec
   list with it, which a negotiating (NG) handset would need. See C-CODEC-09.
5. **The refusal's reason 0x0E has a defined meaning**, EN 300 175-5 §15.2.3: the reject that carries "partial release" may also carry
   <<IWU-ATTRIBUTES>> "exchanged parameter negotiation" naming an alternative. We log the reason and throw the rest away. See C-ATTR-05.
6. **For GAP and NG-DECT Part 1 handsets the standard says the base generates ring-back and busy as in-band audio and must not send
   <<SIGNAL>>** (TS 102 527-3 §7.4.15.2.2.0). We send neither tone to those handsets, only {CC-ALERTING}. See C-TONE-03.

### 1. Basic service and call class

The <<BASIC-SERVICE>> element of our {CC-SETUP} (EN 300 175-5 §7.6.4, figure 18) names a call class (high nibble: 8 normal, 9 internal, A
emergency, C external handover, 2 list access, ...) and a service (low nibble: 0 basic speech, 8 wideband speech, B DTAM wideband, F
"other", 1/5/9/A data). "Default" services mean the attributes come from annex E and nothing else is sent (§9.3.2.1).

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| C-SVC-01 | Incoming SETUP carries 0x88 (normal call, wideband speech default) | §7.6.4; TS 102 527-1 §7.3.2 | Observed: the O201 answers 0x88 with {CC-RELEASE-COM} 0x0E after ~380 ms and never alerts ([vendor-specific-support.md](vendor-specific-support.md) §7.7 row 1). GAP's own {CC-SETUP} table lists 0x80 only (EN 300 444 table 21), so a GAP-only handset may refuse a service nibble it does not know; the standard does not say it must | `default` (`dect-stubd.c:8780` sets 0x88; `dect_base.c:1086`) | frames | A |
| C-SVC-02 | Incoming SETUP carries 0x80 (normal call, basic speech default) | §7.6.4, annex E.1; EN 300 444 table 21 | Observed: the O201 rings and carries narrowband audio both ways ([vendor-specific-support.md](vendor-specific-support.md) §7.7 rows 2-3). Spec-permitted for every handset (527-1 §7.1.2) | `key:cc.basic_service = basic`; `lab:-a basicsvc=0x80`, `-a narrowband`; picked automatically only once the handset's capability is known and not NG (`dect_base.c:1072-1082`) | ear | A |
| C-SVC-03 | 0x88 but with the codec list in the SETUP, G.726 first (the standard's "preferred method" for a G.726 call) | 527-1 §7.3.3 | Spec-permitted; for a handset that wants wideband service but G.726 audio. Not for a handset that refuses 0x88 outright (it does not read the list) | `lab:-a codeclist=HEX` (`dect-stubd.c:7633`) | frames | B |
| C-SVC-04 | Basic service 0xB "DTAM wideband speech" (answering-machine variant) | §7.6.4 note 3, annex E.2 | Spec-permitted; only a handset that is a DTAM would care | `code` | frames | C |
| C-SVC-05 | Internal call: class 9 for an NG handset, class 8 (external) for a GAP handset | 527-1 §7.3.6 | Spec-permitted and required for GAP PPs. Observed: the A540 once showed "via Intern DECT" for an internal call ([handset-compatibility.md](handset-compatibility.md)); not re-checked | `default` (`dect_base.c:1023-1029`: 0x98, narrow 0x90; non-NG known handsets drop to 0x80 through `dect_base_basic_svc`) | eye | B |
| C-SVC-06 | A handset's own SETUP names a service we do not carry (data: 0x5 LRMS, 0x9/0xA light data, 0xF other; classes 3/4/7/B/D/E) | §7.6.4 | Spec-permitted; we answer like a voice call ({CC-SETUP-ACK}/CALL-PROC/CONNECT) and bind no audio, which a data application would take as success | `default` (`dect_cc.c:932`, `dect_cc.c:504`, `dect-stubd.c:5997`) | frames | C |

### 2. Default attributes, "other" service and service negotiation

IWU-ATTRIBUTES (§7.7.21), CALL-ATTRIBUTES (§7.7.5) and CONNECTION-ATTRIBUTES (§7.7.11) describe a service explicitly; the "default"
services leave them out. Negotiation (§15.2) lets a call offer alternatives.

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| C-ATTR-01 | Nothing beyond BASIC-SERVICE for 0x80 and 0x88 (the baseline) | §9.3.2.1; annex E.1, E.2; 527-1 §7.3.3 (the three attribute elements "shall not be used" with wideband default) | Spec-required. A handset that validates attributes it was never sent would reject; none seen | `default` | frames | C |
| C-ATTR-02 | 0x80 plus an explicit IWU-ATTRIBUTES (speech, 32 kbit/s, G.726) and CALL-ATTRIBUTES (basic speech, LU1, class 0 min_delay, FU1), the annex E.1 values written out | §9.3.2.1 says they "shall not" accompany a default service; annex tables E.1, E.2 | Spec-contradicting, so only for a pre-GAP handset that insists on explicit attributes | `code` | frames | C |
| C-ATTR-03 | BASIC-SERVICE "other" (0xF) with IWU-ATTRIBUTES (speech, 3,1 kHz audio 10000b or 7,0 kHz audio 10001b, user protocol G.726 or G.711) and CALL-ATTRIBUTES | §9.3.2.1; §7.7.21 octets 3 to 6; §7.7.5 | Spec-permitted; how DECT/ISDN-era and non-GAP handsets are told what to do | `code` | frames | C |
| C-ATTR-04 | Prioritized list: REPEAT-INDICATOR plus up to three attribute sets in the SETUP (for example wideband first, basic speech second) | §15.2.2, §6.3.2.1 table 7 notes 2, 3 | Spec-permitted; the PT confirms its pick in the first response. A GAP PT ignores unknown elements (527-1 §7.3.0), so it may answer nothing | `code` | frames | C |
| C-ATTR-05 | Read, not send: a refusal that carries the alternative service. A PT that cannot accept the SETUP may reply {CC-RELEASE-COM} with reason partial release 0x0E **and** an IWU-ATTRIBUTES element "exchanged parameter negotiation" naming a service it supports | §15.2.3 (it ties "partial release" to this procedure) | Observed: the O201's refusal is {CC-RELEASE-COM} 0x0E ([vendor-specific-support.md](vendor-specific-support.md) §7.7), but it also ends **every** call with 0x0E, so the reason alone says nothing. What would discriminate: any element besides RELEASE-REASON in that refusal frame | `default` (reason logged, rest discarded: `dect_cc.c:1269-1285`) | frames (the 2026-11-05 O201 capture) | A |
| C-ATTR-06 | Peer attribute negotiation: the PT answers a SETUP with its own IWU-ATTRIBUTES in the first response | §15.2.5 | Spec-permitted; would show a handset that wants a different codec or rate without releasing | `code` (we ignore IWU-ATTRIBUTES in responses) | frames | C |
| C-ATTR-07 | CONNECTION-IDENTITY / CONNECTION-ATTRIBUTES (advanced connection, number of bearers, slot type, MAC service) in SETUP or first response | §9.3.1.3, §9.3.2.3, §7.7.11, §7.7.12, annex E.5 | Spec-permitted: "shall be omitted when using basic connections", which is what a G.726 call is. A wideband call on a long slot is an advanced connection, but E.2 says its attributes come from the codec list | `chip` (we send neither) | frames | C |
| C-ATTR-08 | Operating parameters: TRANSIT-DELAY, WINDOW-SIZE | §15.2.4, §7.7.42, §7.7.43 | Data services only | `n/a` | frames | C |

### 3. Codec lists: registration, setup, choice

NG-DECT wideband equipment exchanges <<CODEC-LIST>> (EN 300 175-5 §7.7.54, TS 102 527-1 §7.3.1) in the registration and location
messages and may add one to a {CC-SETUP}; the receiving side names **one** codec in a response, at the latest in {CC-CONNECT} (or a
{CC-INFO} that opens the U-plane with a progress indicator). Entries are three octets: codec id, direction plus MAC/DLC service,
C-plane routing plus slot size.

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| C-CODEC-01 | The list we send in {ACCESS-RIGHTS-ACCEPT} and {LOCATE-ACCEPT}: G.722 on long slot j=640 first, G.726 on a full slot last, negotiation "001" | 527-1 §7.3.1 ("at least G.722 and G.726", "negotiation possible" always); §7.7.54 | Observed: before 2026-10-22 we sent none and the A540 refused every call ([journals/call-g7.md](journals/call-g7.md), "ROOT CAUSE"). The baseline | `default` (`dect_mm.c:541-554`, sent at `:645`, `:801`) | frames | C |
| C-CODEC-02 | Same list, G.726 first | 527-1 §7.3.3 (preferred method for a G.726 call) | Spec-permitted; lets a handset that takes the first entry pick narrowband without a basic-service change | `code` (the list is two constants; no per-handset order) | frames | B |
| C-CODEC-03 | G.726 alone in the list | 527-1 §7.3.3 ("a list with only the preferred codec is not allowed"), §7.3.1 | The standard forbids it; we do it under `narrowband` | `lab:-a narrowband` (`dect_mm.c:551`) | frames | C |
| C-CODEC-04 | More codecs offered or accepted: G.711 A-law (id 4) and µ-law (5), G.729.1 (6), MPEG-4 ER AAC-LD 32/64 kbit/s (7, 8), LC3plus (11 to 34), user-specific ids 1, 9, 10 | §7.7.54; 527-1 §5.1, §7.8.2 ("may") | Spec-permitted. A handset that lists, say, AAC-LD first is answered with G.722 or G.726 only (`choose_codec`, `dect_cc.c:414`); the chip carries only those two (Part 8 not on disk, so no more can be said) | `chip` | frames | C |
| C-CODEC-05 | Direction bits in each entry: we and every handset seen send 00 | §7.7.54 (00 "reserved", x1 uplink, 1x downlink) | Ambiguous in the standard (the stock list and the A540's list both carry 00; see end of file). A strict decoder could read 00 as "no direction" | `default` (`dect_mm.c:542-544`) | frames | C |
| C-CODEC-06 | MAC and DLC service field other than 0 (LU1 INA): INB, IPM, IPQ, LU7 INB, LU12 | §7.7.54; 527-1 §7.6.1 ("shall" INA, "may" INB, IPQ) | Spec-permitted; a handset that wants error-detecting IPQ for G.722 is not served | `chip` | frames | C |
| C-CODEC-07 | Slot size of the G.722 entry: long slot j=672, double slot, or half slot instead of j=640 | §7.7.54; 527-1 §7.7.2 (j=640 "shall", j=672 and double "may") | Spec-permitted; the PT's own entry (slot-type capability, TC octet 3c) says which it can do. Observed: every handset so far sends j=640 | `default` (`dect_mm.c:543`; the CONNECT echoes the handset's own entry, `dect_cc.c:696`) | frames | B |
| C-CODEC-08 | C-plane routing other than "CS only" (000): CS preferred (001), CF preferred (010), CF only (100) | §7.7.54; §7.7.5 | Spec-permitted; a handset that needs the CF channel on a long slot | `chip` | frames | C |
| C-CODEC-09 | The message that carries a progress indicator also carries the chosen <<CODEC-LIST>> (one entry) | 527-3 figures 66, 73, 75 and note 1 of §7.4.15.2.2.1; 527-1 §7.3.3 ("at latest in a {CC-INFO} that connects the U-plane using PROGRESS-INDICATOR or with CC-CONNECT") | Spec-permitted and drawn in the standard: an NG handset that opens its U-plane at the progress indicator must already know the codec. `cc.progress_in_band` appends the indicator but never the list (`dect_cc.c:125-135`); harmless for the GAP-only O201, wrong for an NG handset needing the key | `code` | frames | B |
| C-CODEC-10 | Codec list in our incoming SETUP, call by call, instead of relying on the registration list | 527-1 §7.3.3 (optional) | Observed history: all three Gigasets ring without it since 2026-10-25 ([journals/bench-2026-10-25.md](journals/bench-2026-10-25.md)); with a stale registration list it would matter | `lab:-a codeclist=HEX` | frames | B |
| C-CODEC-11 | Where we read the handset's choice for our incoming call: {CC-ALERTING} or {CC-CONNECT} only | 527-1 §7.3.3 (also "a {CC-INFO} that connects the U-plane using PROGRESS-INDICATOR", note 1 recommends ALERTING or CONNECT) | Spec-permitted that a handset names its codec in a {CC-INFO}; we would not learn it and bind narrowband ("codec not named") | `default` (`learn_codec_choice` called at `dect_cc.c:1463`, `:1481`) | frames | B |
| C-CODEC-12 | Handset-placed call: where we name our choice. Ours only in {CC-CONNECT} (G.726 alone, or G.722 when the link is wideband and the handset offered it) | 527-1 §7.3.3 note 1 (recommended: {CC-SETUP-ACK}, {CC-CALL-PROC} or {CC-CONNECT}) | Spec-permitted earlier; matters for a handset that must know the codec when its U-plane opens early | `default` (`queue_connect`, `dect_cc.c:672-712`) | frames | B |
| C-CODEC-13 | Handset's list lacks G.726, or lacks both we know (non-compliant list) | 527-1 §7.3.1 | We name no codec (`codec_unmatched`), CONNECT without a list; the call binds narrowband. A handset that insists on an answer would stall | `default` (`dect_cc.c:684-687`) | frames | C |
| C-CODEC-14 | Force G.726 for one NG handset's calls (its wideband path is broken) without a basic-service change on incoming calls | 527-1 §7.3.3 specific case of G.726 | For outgoing handset-placed calls the choice is G.722 whenever the link is wideband; no per-handset override exists | `code` (only `lab:-a narrowband`, global) | ear | B |

### 4. Codec change, service change, slot-type modification

After the call is up, either side may switch codec with {CC-SERVICE-CHANGE} (change mode "Audio codec change", one-entry codec list);
both then send {IWU-INFO} with the new list; the initiator changes the slot type at the MAC when the codec needs a different one.

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| C-CHG-01 | Handset-initiated codec change: we reject every {CC-SERVICE-CHANGE} | 527-1 §7.3.4 ("shall either accept or reject"); EN 300 175-5 §9.6.1; 527-3 §7.4.3.3 (parallel calls with different codecs use this procedure) | Spec-permitted outcome, but a handset that needs the change (second call in another codec, a codec fall-back) gets a reject and may release or go silent. Not seen yet | `default` (`dect_cc.c:1583-1596`) | frames | B |
| C-CHG-02 | FP-initiated codec change (for example after a SIP re-INVITE changes the far end's codec) | 527-1 §7.3.4 | Spec-permitted; today the bridge converts at the host instead (`bridge-interface.md` §3) | `code` | ear | B |
| C-CHG-03 | Accepting a change: mute the receive path at {CC-SERVICE-ACCEPT}, unmute at {IWU-INFO}, both sides send {IWU-INFO} with the new list | 527-1 §7.3.4.0 | Spec-required if we ever accept; a handset mutes until it gets the {IWU-INFO} | `code` | ear | B |
| C-CHG-04 | Slot-type modification after codec choice (full to long j=640 and back), and its failure fallback ({IWU-INFO} with the mandatory codec) | 527-1 §7.3.5, §7.3.5.1; EN 300 175-3 §10.3.2.2.3 | Spec-permitted; the chip does the MAC part (`chip`); the failure path is ours | `chip` (MAC), `code` (NWK fallback) | frames | C |
| C-CHG-05 | Change mode "Profile/Basic service and IWU attributes change" (wideband to basic speech mid-call) | EN 300 175-5 §7.7.38, §9.6.1 note 1 | Spec-permitted; "not fully described" in 527-1 §7.3.3 for the 0x80 fall-back method | `code` | frames | C |
| C-CHG-06 | Suspend/resume, bandwidth change, re-routing, modulation change | §9.6.2 to §9.6.5 | Data and advanced-connection services | `n/a` | frames | C |
| C-CHG-07 | CC-NOTIFY with TIMER-RESTART while a long operation (MM, dial wait) delays our answer | EN 300 175-5 §9.3.1.2, 527-1 §6.14.6 | A handset with short <CC.03>/<CC.04>; we never send it (3 s dial wait, 20 s timers) | `code` | log | C |

### 5. What a handset says about itself: <<TERMINAL-CAPABILITY>>

EN 300 175-5 §7.7.41 carries tone, display, echo (TCLw), noise rejection, adaptive volume, **slot-type capability** (octet 3c), blind
slots (octets 6, 6a) and the profile indicators (GAP, NG-DECT Part 1/3, ...). A PT may send it in {CC-SETUP} (§9.3.1.1) and in its
first response (§9.3.2.9); if omitted, the last known values apply, else the defaults.

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| C-TCAP-01 | Learn the capability from the handset's CC-SETUP and from its first response to ours, not only from MM | §9.3.1.1, §9.3.2.9 | Observed: the O201 reports "GAP only" and still got 0x88 (it needed a profile); we read the element only in {LOCATE-REQUEST} and {ACCESS-RIGHTS-REQUEST} (`dect_mm.c:871-891`, called at `:950`, `:1105`) and nowhere in CC. Which message carries the O201's report is not recorded in our notes (frames settle it) | `code` | frames | A |
| C-TCAP-02 | While the capability is unknown, assume the standard's default: full slot only (GAP) or full plus long j=640 (NG), i.e. offer 0x80 | EN 300 444 §8.17 table 29 (slot capability 8 = full slot); 527-1 §7.3.7 table 16 | Spec-permitted default; ours is the opposite (0x88 until known: `dect_base.c:1072-1082`, a known GAP-only handset gets 0x80). Cost: an NG handset that sends no capability gets narrowband until it does | `default` (`dect_base_basic_svc`) | frames | A |
| C-TCAP-03 | Read the slot-type capability (octet 3c) and offer G.722 only if the PT has long j=640 (or pick j=672/double when that is all it has) | §7.7.41 octet 3c | Our parser keeps tone, display and profile indicators only (`struct dect_term_cap`, `dect_mm.h:262-270`); a PT without a long slot cannot carry G.722 | `code` | frames | B |
| C-TCAP-04 | Use the tone capability (octet 3: none, dial tone only, E.182 tones, complete) to decide between <<SIGNAL>> tones and in-band tones | §7.7.41 octet 3; 527-3 §7.4.15 | Parsed and stored, used nowhere (`peer_cap.tone`, grep) | `code` | frames | B |
| C-TCAP-05 | Echo parameter (octet 3b: 001 minimum TCLw, 010 > 46 dB, 011 > 55 dB), noise rejection, adaptive volume ("disable FP adaptive volume control") | §7.7.41 octet 3b and notes 9 to 11; Part 8 not on disk | Decides whether the FP must do echo control towards a VoIP network. Not read | `code` | ear | B |
| C-TCAP-06 | Blind-slot indication (octets 6, 6a; "PT with fast hopping radio", PI_6 bit 6) | §7.7.41 octets 6, 6a, PI_6, notes 21, 22 | Decides whether a second bearer or a bearer handover can be set up next to an active slot | `chip` | frames | C |
| C-TCAP-07 | Profile indicators other than GAP and NG: CAP (PI_1 bit 1), DECT/ISDN end system, "no GAP" | §7.7.41 PI_1 | A CAP-only handset, or one that does not set GAP, may not understand a GAP-profile call at all (CAP is EN 300 824, not on disk) | `n/a` | frames | C |

### 6. When the handset opens its audio: PROGRESS-INDICATOR and the connection points

The PT "is not required" to connect its receive U-plane unless it gets <<PROGRESS-INDICATOR>> cause 8, "in-band information or
appropriate pattern now available" (EN 300 175-5 §9.3.1.4, §9.3.2.4, §7.7.31); otherwise it may wait for {CC-CONNECT} (outgoing) or
{CC-CONNECT-ACK} (incoming). GAP tables 11 to 13 list the indicator as optional in {CC-SETUP-ACK}, {CC-CALL-PROC} and {CC-ALERTING}
and say that if present the PT connects the U-plane; the FT connects its own U-plane before {CC-CONNECT} (EN 300 444 §8.6).

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| C-UP-01 | Indicator (location 1, description 8: `1e 02 81 88`) in the first answer to the handset's call, {CC-SETUP-ACK} or else {CC-CALL-PROC} | EN 300 444 tables 11, 12; EN 300 175-5 §9.3.1.4; 527-3 figures 66, 73 | Observed: the O201's receive path opens at once, 3 of 3 calls with the profile alone ([vendor-specific-support.md](vendor-specific-support.md) §7.7) | `key:cc.progress_in_band` (`dect_cc.c:125-135`, once per call) | ear | A |
| C-UP-02 | Indicator in {CC-ALERTING} only | EN 300 444 table 13 | Observed: 0 of 3 (§7.7, lab patch, not in the repo). Kept as the control that failed | `code` (patch only) | ear | C |
| C-UP-03 | Indicator in all three early messages (SETUP-ACK, CALL-PROC, ALERTING) | EN 300 444 tables 11 to 13 | Observed: 3 of 3 (§7.7, lab patch); indistinguishable from C-UP-01 on the O201 | `code` (patch only) | ear | C |
| C-UP-04 | Indicator in {CC-CONNECT}, or in a {CC-INFO} | EN 300 175-5 §6.3.2.6 table 12 (allowed in CONNECT); 527-3 §7.4.15.2.2.1 note 2 (avoid it in {CC-INFO}, PT support not mandatory) | Redundant with CONNECT for the PT; only for a PT that waits for the indicator even at CONNECT | `code` | ear | C |
| C-UP-05 | Other progress descriptions: 1 ("not end-to-end ISDN, further call progress info may be available in-band"), 2, 3, 5 ("service change"), 9 ("in-band information not available") | §7.7.31 | Spec says only cause 8 connects the U-plane; an ISDN-minded handset may treat 1 the same | `code` | ear | C |
| C-UP-06 | Indicator location other than 1 ("private network serving the local user"): 0 user, 2 public network serving the local user, 15 not applicable; coding standard 00 | §7.7.31 | Spec-permitted; a PT that filters on location. Cheap, no sign of need | `default` (location 1) | ear | B |
| C-UP-07 | Indicator **after** CALL-INFORMATION in the same message (our current order when a Part 3 handset has call status on) versus in identifier order (0x1E, 0x7C, 0x7E) | EN 300 175-5 §7.5.1 ("ascending numerical order", so a receiver may stop scanning) | A receiver that stops at 0x7E never sees the indicator. The O201 has no call status, so it is not hit | `default` (`dect_cc.c:125-135`: status first, indicator appended) | frames | A |
| C-UP-08 | Do not send the indicator and let the PT connect at {CC-CONNECT}: the normal case for every Gigaset | EN 300 175-5 §9.3.1.4 note, §9.3.1.8 | Baseline. A handset that connects late "at an unpredictable time" is the O201's pre-fix symptom (observed, §7.7 "Open at the time", answered by C-UP-01) | `default` | ear | C |
| C-UP-09 | Incoming call: indicator to the PT in our SETUP (early in-band audio such as a base announcement) | EN 300 175-5 §6.3.2.1 table 7 (F to P allowed), §9.3.2.4 | Spec-permitted; nothing needs it today | `code` | ear | C |

### 7. The sequence of our answers to a handset-placed call

The standard sanctions four orders (EN 300 444 §8.1 figures 1 to 4): SETUP-ACK, digits, CALL-PROC, ALERTING, CONNECT; SETUP then
CONNECT at once; SETUP-ACK, digits, CONNECT; the same with CALL-PROC but no ALERTING. Ours with a bridge: SETUP-ACK, digits, wait
`dialwait` (3 s), CALL-PROC, ALERTING when the far end rings, CONNECT when it answers. Without a bridge: CALL-PROC then CONNECT.

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| C-OSEQ-01 | {CC-CONNECT} straight after {CC-SETUP}, digits in {CC-INFO} afterwards (figure 2) | EN 300 444 §8.1 figure 2, §8.6 | The U-plane opens at CONNECT, at once, with in-band dial tone possible. Observed cost: Gigasets then show neither "connected" nor a call timer (bench test 2, 2026-10-25) | `lab:-a nooverlap` | ear | B |
| C-OSEQ-02 | SETUP-ACK, digits, {CC-CONNECT} without CALL-PROC or ALERTING (figure 3) | EN 300 444 §8.1 figure 3 | Fewer states for a handset with a crude state machine | `code` (bridged path always sends CALL-PROC: `dect_base.c:1570`) | frames | C |
| C-OSEQ-03 | Dial wait 0 or longer than 3 s; digits with {SENDING-COMPLETE} | EN 300 175-5 §9.3.1.5; EN 300 444 §8.3 | Observed: every handset waits the same 3 s; the O201's digits come in {CC-INFO} without sending-complete (§7.7). The U-plane stays closed during it unless an indicator was sent | `lab:-a dialwait=MS`; not in the controller | ear | B |
| C-OSEQ-04 | Bind the audio before {CC-CONNECT} (the FT shall connect its U-plane first) | EN 300 444 §8.6; EN 300 175-5 §9.3.1.8 | Spec-required. Ours binds when CC reaches ACTIVE, after CONNECT is queued (`dect-stubd.c:6005-6013`). See C-BIND-01 | `lab:-a bindfirst` (unbridged only) | ear | A |
| C-OSEQ-05 | Send {CC-ALERTING} only with the U-plane installed | EN 300 175-5 §9.3.1.7 ("shall only be sent if the U-plane resources are fully installed") | Spec-required; ours sends ALERTING (and the indicator) before any bind. See C-BIND-02 | `default` (`dect_cc.c:1043-1051`) | ear | B |
| C-OSEQ-06 | TIMER-RESTART in {CC-NOTIFY} while the far end is slow | §9.3.1.2, §9.3.1.9 | A handset with <CC.04> running releases on expiry (optional timer) | `code` | log | C |

### 8. The incoming call: CONNECT, CONNECT-ACK and the handset's audio

The PT "may delay connection of the U-plane until receiving {CC-CONNECT-ACK}"; the FT is to request U-plane confirmation from its LLME
and only then return the ACK; the PT may mute its receive path meanwhile (EN 300 175-5 §9.3.2.4, §9.3.2.8 and its note).

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| C-ISEQ-01 | Send {CC-CONNECT-ACK} only after the audio is bound, not at once | §9.3.2.8 | A PT that connects its receive path at the ACK would then find the chip's U-plane ready. Ours: ACK queued on receipt of CONNECT (`dect_cc.c:1483`), bind at the next tick | `default` | ear | B |
| C-ISEQ-02 | Handset connects without {CC-ALERTING} | §9.3.2.7, §9.3.2.8 | Handled (CALL_PRESENT to ACTIVE, `dect_cc.c:1469-1487`) | `default` | log | C |

### 9. Tones: in-band or signalled, ring-back, early media

Two ways to give the user call-progress tones: the PT makes them from <<SIGNAL>> (EN 300 175-5 §7.6.8), or the FP puts them in the
audio and connects the U-plane (EN 300 444 annex B, TS 102 527-3 §7.4.15). For GAP and NG Part 1 handsets the system "shall provide"
ring-back and busy, in-band, and "shall not send any <<SIGNAL>>" for them (527-3 §7.4.15.2.2.0).

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| C-TONE-01 | No dial tone at all (we send no tone and no indicator) | EN 300 444 annex B.3 (the PT may play its own invitation to dial when it got no indicator) | Baseline. With `cc.progress_in_band` the PT connects its U-plane and may suppress its own tone (annex B.2 case B) while we send silence: the O201 user hears nothing during the 3 s wait (not asked; ear) | `default` | ear | A |
| C-TONE-02 | <<SIGNAL>> dial tone (00H) in {CC-SETUP-ACK} | EN 300 175-5 §6.3.2.3 table 9 note 2; §7.6.8 | For a PT with E.182 or full tone capability that plays tones from SIGNAL | `code` | ear | B |
| C-TONE-03 | Ring-back: {CC-ALERTING} only (ours) versus plus <<SIGNAL>> 01H versus in-band ring-back for GAP/Part 1 handsets | EN 300 175-5 §7.6.8 note 4; 527-3 §7.4.15.2.2.0 (in-band, no SIGNAL, for GAP and Part 1) and §7.4.15.2.1 (SIGNAL for Part 3); EN 300 444 annex B | Observed: ringback from ALERTING was heard on the A540 (the handset makes it; [notes-for-the-controller.md](notes-for-the-controller.md), API 25). A handset without its own tone gets silence. [bridge-interface.md](bridge-interface.md) §2.2 says SIGNAL 01H goes to Part 3 handsets; the code sends none (`dect_cc_alert`, `dect_cc.c:1043`) | `default` | ear | A |
| C-TONE-04 | The base plays ring-back and busy as audio in the active codec (U-plane connected early) | 527-3 §7.4.15.2.2, figures 73 to 77 | Spec-required for GAP and Part 1 handsets; needs an audio bind before CONNECT and a tone source | `code` | ear | B |
| C-TONE-05 | Early media: the far end's 183 audio and ring-back passed through, U-plane opened at {CC-ALERTING} or by an indicator | 527-3 §7.4.15.3; EN 300 444 annex B.4 | Not built in the controller either (the maintainers' notes, "early media"). A handset that expects in-band progress gets silence | `code` | ear | B |
| C-TONE-06 | Busy: SIGNAL 04H with call status for Part 3, {CC-RELEASE} reason "user busy" 0x14 for others | 527-3 §7.4.8.3; EN 300 175-5 §7.6.7 | For GAP handsets the standard wants an in-band busy tone (C-TONE-03). Cross B | `default` (`dect_cc.c:917-930`) | ear | B |
| C-TONE-07 | Confirm, answer, off-hook warning, intercept, congestion tones | §7.6.8 | Spec-permitted values; mostly Part 3 | `default` (negative-ack and call-waiting only: `dect_cc.c:909`, `:1705`) | ear | C |

### 10. DTMF and keypad

The PP sends dialled digits as <<MULTI-KEYPAD>> or <<KEYPAD>> and "shall not generate any in-band audio DTMF"; the FP converts
(TS 102 527-3 §7.4.14.1). GAP lists special keypad codes (EN 300 444 §8.10 table 20).

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| C-DTMF-01 | Keypad digits after CONNECT become `digit` events (0-9, *, # only) | 527-3 §7.4.14.1.2; EN 300 444 §8.10 | Baseline | `default` (`dect_base.c:2622-2634`) | hand | C |
| C-DTMF-02 | Keypad codes 14H and 16H ("go to DTMF", defined or infinite length), 12H (pulse), 05H (pause), 15H (register recall, flash) | EN 300 444 §8.10 table 20, features N.5, N.6, N.7, N.22, N.23 | We drop them in an active call (15H and 17H have meanings only in dialling); a handset that sends 14H then digits expects the digits as tones | `default` (`dect_base.c:2622-2634`) | hand | B |
| C-DTMF-03 | A handset that generates DTMF in the speech path instead of keypad | 527-3 §7.4.14.1 (forbidden for PPs) | An old or odd handset; the digits are lost, no in-band detector | `code` | hand | C |
| C-DTMF-04 | Downlink DTMF: far-end tones passed in band, or generated by us in the active codec from RFC 2833 events | 527-3 §7.4.14.2 | Not examined (baresip side, `src/baresip/opendectcell.c`); G.722 coded tones are the awkward case | `n/a` | ear | C |

### 11. Audio quality features the handset states

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| C-AUD-01 | FP-side echo control for a handset with minimum TCLw on a VoIP path (long delay) | §7.7.41 octet 3b notes 9 to 11; 527-1 §5.6 audio types; Part 8 not on disk | A handset that depends on it echoes the far end back. Open note: the maintainer once heard "loopback" (sidetone versus chip path; memory note g7-downlink-state), never settled | `code` | ear | B |
| C-AUD-02 | FP adaptive volume (handset reports A-VOL "PP uses it" or "disable FP adaptive volume control") | §7.7.41 octet 3b | Level jumps on a long call if both sides adapt | `n/a` | ear | C |
| C-AUD-03 | Downlink level: the chip's channel parameter (0x0251 selector value, default 0x2B) and the host's own gain | [stub-api.md](stub-api.md) §3.6; units not established | A handset whose playback is too quiet or clipped | `lab:-a sel=, gain=` | ear | C |
| C-AUD-04 | What is sent while the bridge has no frame: silence versus repeat; G.722 coded silence | memory note g7-downlink-state (idle fill of 0xFF or 0xFEFE beeped in G.722) | A decoder with packet-loss concealment reacts differently from one without (527-1 §5.6 NG1.SC.7) | `default` (silence coded) | ear | C |
| C-AUD-05 | Fax/modem tone detection and switch to G.711 | 527-1 §5.6 NG1.SC.8 | Fax over DECT; out of scope for G.722 passthrough | `n/a` | ear | C |

### 12. The host's audio bind: what we ask of the chip

The host installs the U-plane with chip mails (`0x0251` channel parameter, `0x0230` assign, `0x022D` path on, `0x0233` release; see
[stub-api.md](stub-api.md) §3.6 to §3.8). Narrowband: signed 16-bit linear PCM, mode 2; wideband: coded G.722 words held two frames.

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| C-BIND-01 | Bind **before** our CONNECT (the FT connects its U-plane first, GAP §8.6) | EN 300 444 §8.6; EN 300 175-5 §9.3.1.8 | Observed: `bindfirst` "no change" on the O201 ([vendor-specific-support.md](vendor-specific-support.md) §7.7), but by the code it acts only on the unbridged dial path (`dect_base.c:1542-1543`, `:1645-1662`; the bind test at `dect-stubd.c:6005-6008` needs `connect_staged`). If that run had a bridge (`--bridge-echo`), the variable never arrived: check the log for "CONNECT now ... the U-plane is up (0x022D sent)". The profile key `cc.bind_before_connect` is parsed and ignored | `lab:-a bindfirst` (unbridged only); `key:cc.bind_before_connect` accepted, not acted on | log, ear | A |
| C-BIND-02 | Bind before the indicator or ALERTING goes out (U-plane installed first) | EN 300 175-5 §9.3.1.7, §9.3.1.4 | Spec-required. Counter-evidence: the indicator in SETUP-ACK, ahead of any bind, worked 3 of 3 on the O201 | `code` | ear | B |
| C-BIND-03 | Chip mode and assign flags: bit 6 set (wideband, bit-6-clear first assign avoids the 500 ms noise burst) and the second assign `remode` | [stub-api.md](stub-api.md) §3.6, §3.7; memory note g7-downlink-state | Solved on all three Gigasets; kept as the knob if a handset's audio is noise or silence | `lab:-a flags=, mask=, mode=, codec=, remode=, noremode` | ear | C |
| C-BIND-04 | Path-on (`0x022D`) timing: on the chip's `0x0242` answer or after the 500 ms fallback; or not at all (a no-022D narrowband test "left unrun" in G run 26) | [stub-api.md](stub-api.md) §5 (superseded statements, the `022D` row) | Untested variation | `default` (`dect-stubd.c:3625-3660`) | ear | C |
| C-BIND-05 | Verify the slot type the chip actually set up (long j=640 for G.722) and fall back to G.726 if the bearer is a full slot | 527-1 §7.3.3 (slot-type modification can fail), [journals/call-g7.md](journals/call-g7.md) ("a G.722 call on a full-slot bearer would explain silence") | The F625 slot-type reading is disputed in our own notes (`dect-stubd.c:3893-3935`: byte 5 is not the slot type) | `lab:-a status=MS` reads it; no fallback | log | B |
| C-BIND-06 | Release order at call end: `0x0233` then `0x0202` | [stub-api.md](stub-api.md), A-0070 | A stale audio state on the next call (never seen since fixed) | `default` (`base_call_down`, `dect-stubd.c:6079`) | ear | C |
| C-BIND-07 | Bind parameters per handset (selector, gain, mask, extra byte) | [stub-api.md](stub-api.md) §3.6 | Only one working recipe is known per codec; no handset has needed another | `lab:-a sel=, gain=, mask=, mode=, codec=` | ear | C |

### 13. Ciphering and the audio

The handset ciphers the whole bearer, voice included, once it sees our cipher request; the host starts it after the bind
(`start_call_cipher`, `dect-stubd.c:5943`, called at `:6062`).

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| C-CIPH-01 | No ciphering (`-C`) | EN 300 175-5 §15.6, §7.7.10 | Observed: no change for the O201's outgoing audio ([vendor-specific-support.md](vendor-specific-support.md) §7.7); a handset may complain about an unciphered link | `lab:-C` | ear | C |
| C-CIPH-02 | Cipher before the bind, or before CONNECT, instead of after | §15.6 | Order of the key switch against the first audio frames; cross A | `code` | ear | B |
| C-CIPH-03 | Early encryption and "default cipher key" (TC PI_7 bit 5; FP broadcast a42) | EN 300 175-5 §7.7.41 PI_7; annex F.3 a42; EN 300 444 table 7 (M.15, M.16) | A handset that has the bit set may expect ciphering from the first frame; we do not read the bit | `code` | frames | C |
| C-CIPH-04 | DSC2/AES (TC octet 5 bit 6; broadcast a44) | §7.7.41 octet 5; annex F.3 | Handset capability only; cross A | `n/a` | frames | C |

### 14. Paging, bearer setup, slot types (mostly the chip's)

An incoming call starts with a page (`0x0300`); the LCE header says "voice", a full page can name the slot type and setup method
(EN 300 175-5 §8.2). The chip builds the page; we hand it one byte.

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| C-PAGE-01 | LCE header 100 (voice, former IN_min_delay): short page = basic connection on a full slot; "full page format is mandatory if the slot is different of a full slot" | §8.2.4.1, §8.2.4.3 table 80b | A long-slot (G.722) call needs a full page naming j=640 or an attribute exchange afterwards | `chip` (we send page-info 0x0C, 0x2C for wideband: `dect_base.c:1102-1106`; per [stub-api.md](stub-api.md) §3.5 the byte does not change the page) | frames | B |
| C-PAGE-02 | LCE header 011 (general purpose, forces attribute negotiation) or 101 (auxiliary, IN_normal_delay) | §8.2.4.1 | Handset that does not know header 100 | `chip` | frames | C |
| C-PAGE-03 | Full page with TPUI: slot-type field and setup-info field (basic or advanced MT, B-field) | §8.2.2, §8.2.4.2 | The page can set up the bearer in the codec's slot format directly (527-1 §7.3.3 "preferred codec selection") | `chip` | frames | B |
| C-PAGE-04 | Full page with IPUI, or collective/unknown-service ringing (LCE 001) | §8.2.2, §8.2.4.3 | Page shapes a handset must answer; mostly for roaming | `chip` | frames | C |
| C-PAGE-05 | Page repetition: 3000 ms interval, 20 tries | `DECT_BASE_PAGE_INTERVAL_MS`, `DECT_BASE_PAGE_RETRIES` (`dect_base.h:968-969`); "paging detection normal or fast" in §7.7.40 | A handset in a power-saving paging cycle needs the repeats (cross A) | `default` | log | C |
| C-PAGE-06 | Fast setup (FT-initiated setup without a page) and the PT's setup capability | §7.7.40; EN 300 175-3 §10.2.3 | Handset that supports "complete fast setup" | `chip` | frames | C |

### 15. MAC services, connection types, bearers (the chip's)

Speech uses one duplex bearer, MAC service IN_minimum_delay, DLC class 0/min_delay, frame unit FU1 (EN 300 175-4 §11.2; EN 300 175-3
§5.6.2.1). A basic connection is full slot only (§5.6.1.1); wideband needs an advanced connection with a long slot (§5.6.1.2; 527-1 §7.6.6).

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| C-MAC-01 | Basic connection (G.726, full slot) versus advanced connection with ECN (G.722, long slot) and the connection-type modification between them | EN 300 175-3 §5.6.1.1, §5.6.1.2, §10.3.3; 527-1 §7.6.6 | A handset whose firmware sets up advanced connections badly would fail only wideband calls | `chip` | frames | B |
| C-MAC-02 | MAC service IN_normal_delay or IPQ error detection instead of IN_minimum_delay | 527-1 §7.6.1; EN 300 175-3 §5.6.2.1 | Spec says min_delay shall be supported, the others may | `chip` | frames | C |
| C-MAC-03 | Slot types: long j=640 (64 kbit/s, FU1 frame of 80 octets), j=672, double slot (P80); full slot FU1 of 40 octets | 527-1 §7.5.1, §7.7.2; EN 300 175-3 table 5.1 | Spec-permitted alternatives for G.722 | `chip` | frames | C |
| C-MAC-04 | Multi-bearer and bandwidth changes, asymmetric connections, suspend/resume | EN 300 175-3 §10.3.1; EN 300 175-5 §7.7.11 | Data services, not voice | `n/a` | frames | C |
| C-MAC-05 | FP broadcast bits the handset reads before choosing a service: a32 ADPCM/G.726 voice, a33 GAP basic speech, a24 NG-DECT wideband, a29 extended wideband, a45 external handover, a46 connection handover | EN 300 175-5 annex F.1 (a32 to a46), F.3 (a24, a29) | A handset may refuse a service whose bit is 0. Known: a24 and a29 are 1, a34 is 0 ([stub-api.md](stub-api.md), "Capability bits a24, a29, a34", A-0128); the others are the chip's and not recorded | `chip` | frames (the 0xF602 broadcast body) | B |
| C-MAC-06 | Dummy bearer and connectionless broadcast shapes | EN 300 175-3 §5.5.1, §9.1 | Broadcast, not a call | `chip` | frames | C |

### 16. Handover and continuity of a call

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| C-HO-01 | Intra-cell bearer handover by the handset (interference): the FT answers with "bearer confirm" or "wait" | EN 300 444 table 7 (M.9); 527-1 §7.6.11; EN 300 175-3 §10.6.2 | Mandatory for the PT; a base that does not answer drops the call. The chip does it; our `0x016A` request has "no demonstrated effect" ([stub-api.md](stub-api.md)) | `chip` | log | B |
| C-HO-02 | Connection handover (new MAC connection, DLC class A/B handover) | EN 300 444 table 7 (M.11); 527-1 §7.6.12; EN 300 175-4 §9.2.7.3 | Same; "A call in progress does not follow a handset" between our cells ([Limits](../../opendectcell-controller/docs/wiki/Limits.md)) | `chip` (within one cell); `n/a` between cells | log | B |
| C-HO-03 | External handover: the PT places a SETUP of call class C with a handover reference, and the FP advertises it (broadcast a45) | EN 300 175-5 §15.7; annex F.1 a45; §7.6.4 class 1100 | The standard's own way to move a call between two fixed parts, relevant to the controller's multi-cell goal. We treat class C as speech and have no handover candidate or reference procedure | `code` | frames | C |
| C-HO-04 | Emergency call setup (class A) and the FP's "emergency call supported" bit (a40) | EN 300 175-5 §9.8; annex F.2 | A handset may look for a base that sets the bit | `default` (accepted as speech, `dect_cc.c:504`) | frames | C |

### 17. Old, unusual and out-of-profile procedures

Kept for completeness: what a pre-GAP, non-GAP or data-oriented handset could do. None has been seen on our bench unless marked.

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| C-OLD-01 | GAP-only handset: full slot, G.726 32 kbit/s, FU1, no codec list | 527-1 §7.1.2; EN 300 444 table 21, annex E.1 | Observed (O201: [vendor-specific-support.md](vendor-specific-support.md) §7.7) | `key:cc.basic_service = basic` | ear | A |
| C-OLD-02 | CAP (Cordless Access Profile) handset, no GAP bit | §7.7.41 PI_1 bit 1; CAP is EN 300 824, not on disk | Pre-GAP handsets use their own call setup | `n/a` | frames | C |
| C-OLD-03 | DECT/ISDN intermediate access profile (call class 0111, IWU "restricted digital", G.711, 64 kbit/s on LU7, double slot) | §7.6.4 class 0111; §7.7.21; EN 300 175-4 §11.9 (LU7: double slot, +80 ms delay) | ISDN-style handset or terminal adapter | `n/a` | frames | C |
| C-OLD-04 | CTM FP1 32 kbit/s, DMAP, MRAP, ODAP, LRMS (E-profile), GSM/UMTS interworking, DPRS, ULE, SUOTA, advanced audio profile | §7.7.21 profile list; §7.6.4 services 0x4 to 0xA; §7.7.41 PI_2 to PI_11 | Handsets that register but speak another service; out of scope (see C-SVC-06 for what a SETUP of one gets) | `n/a` | frames | C |
| C-OLD-05 | Proprietary 32 kbit/s and 64 kbit/s codecs (codec ids 1, 9, 10) and <<ESCAPE-TO-PROPRIETARY>> | §7.7.54; §7.7.45 | Vendor-specific; Observed: the O201 puts `7b 02 80 04` in every SETUP (§7.7); not needed, still ignored | `default` (ignored) | frames | C |
| C-OLD-06 | Repeater (WRS) paths: complementary connections, double duplex bearers | EN 300 175-3 §5.6.1.4, §10.5.1.6; PI_3, PI_10 | A handset locked to a repeater of ours; not applicable to the 7412 and 7520 | `n/a` | frames | C |
| C-OLD-07 | Hands-free, headset and wireless-microphone audio types, headset management | 527-1 §5.6 (audio types 1a to 2c); 527-3 §7.4.16; §7.7.41 PI_11 | Different audio specifications and call interception; headset PPs only | `n/a` | ear | C |
| C-OLD-08 | G.711 narrowband on a long slot (NG1.2): a "narrowband PCM" service for PSTN calls without transcoding | 527-1 §5.1, §7.8.2 ("may") | A G.711-first handset or fax | `chip` | ear | C |
| C-OLD-09 | A handset ending every call with partial release (0x0E), so the link is meant to stay for a few seconds (timer <LCE.02>) | EN 300 444 §8.9; EN 300 175-5 §14.2.7 | Observed: the O201 (§7.7). We release the link at once after the last release (`lce.release_after_call = yes`, `-a nlr` default, stock does the same); a handset that expects the link to linger may re-set it up, which can cost its next audio. Untested. Cross A | `key:lce.release_after_call` | log | B |

### 18. More than one call at a time

| ID | Variation | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| C-MULTI-01 | Second and third call on channels 2 and 3 (TDM word k XOR 1), one codec each | [two-calls.md](two-calls.md); [stub-api.md](stub-api.md) §3.8 | Built, not on air for all handsets; chip limit three voice channels | `lab:-a twocalls` | ear | B |
| C-MULTI-02 | Internal call codec priority: wideband between two wideband handsets, transcoding for a mixed pair | 527-3 §7.4.3.9; [two-calls.md](two-calls.md) ("no transcoding yet") | A G.722 handset calling a G.726 one needs a conversion; both calls have to share the codec today | `code` | ear | B |
| C-MULTI-03 | Parallel calls on one link in different codecs (call waiting with a codec change) | 527-3 §7.4.3.3; figure 3 | Needs C-CHG-01 and C-CHG-03; today the second call must match | `code` | ear | B |

### Spec text I could not find or that is ambiguous

- **EN 300 175-8 (speech coding, audio types, TCLw) and EN 300 175-2 (PHY) are not on disk.** Anything about G.726 bit order, G.722 framing on
  the air, FP echo requirements (C-AUD-01, C-TCAP-05) or physical packets is read only through TS 102 527-1 references and cannot be checked.
- **CODEC-LIST direction bits** (§7.7.54, octet 4a): the table calls 00 "reserved" and defines x1 and 1x; the A540's list and the stock list both
  carry 00 (observed). Which a strict receiver accepts is undefined (C-CODEC-05).
- **Half-slot j in CODEC-LIST:** §7.7.54 says "Half slot; j = 0", while §7.7.11 and §7.7.41 say "j = 80".
- **Annex E.2 table E.4** says the U-plane class comes "according to the <<CODEC-LIST>>", but the list has no field for it (only the MAC and DLC
  service): the mapping is implicit.
- **§9.3.1.4 and the GAP tables differ in wording.** EN 300 175-5: the PT "is not required" to connect unless it sees the indicator. GAP: "if it is
  present, PT shall connect". Neither says when a PT with no indicator opens its receive path in an outgoing call beyond "may delay until
  {CC-CONNECT}" (§9.3.1.4 note); a delay of seconds is within the text (the O201's pre-fix symptom).
- **Where a PT must send its <<TERMINAL-CAPABILITY>>.** §9.3.1.1 and §9.3.2.9 say "may"; EN 300 444 §8.17 and 527-1 §7.3.7 require it only in
  {ACCESS-RIGHTS-REQUEST} and {LOCATE-REQUEST}. A handset that sends it only in CC is not out of line (C-TCAP-01).
- **TS 102 527-3 §7.4.15.2.2.0** says the FP "shall provide ring-back and busy" to GAP and Part 1 PPs, in-band, and elsewhere
  (§7.4.15.2.1) lets Part 3 PPs generate them from <<SIGNAL>>. How a base knows a PP's tone capability for a GAP PP that sent none is not stated.
- **EN 300 175-3 and -4 were read for headings and the clauses cited**, not end to end; MAC bearer procedures (§10.5, §10.6) are the chip's and
  were not studied beyond what the rows need.
- **CAP (EN 300 824), DPRS (EN 301 649) and the DECT/ISDN, CTM and LRMS profiles** are not on disk; C-OLD rows rest on the lists in EN 300 175-5 only.

### Cross-area dependencies

- **A (identity, registration, security, paging, link):** the capability is learned in MM only (C-TCAP-01); the registration codec list decides every
  wideband call (C-CODEC-01); cipher start versus audio (C-CIPH-01 to 04) and DSC2/early-encryption bits; page shape and TPUI (C-PAGE-01 to 06,
  the A540 "deaf" notes: memory note a540-deaf-on-7412-needs-slot-tpui);
  link release after "partial release" (C-OLD-09, EN 300 444 §8.9, timer <LCE.02>).
- **B (call control):** the order and timing of SETUP-ACK, CALL-PROC, ALERTING, CONNECT, CONNECT-ACK and their timers (section 7 and 8 here);
  release reasons (0x0E at every call end, C-ATTR-05); busy handling and <<SIGNAL>> values (C-TONE-06); internal call class (C-SVC-05); service
  change and {IWU-INFO} (C-CHG).
- **D (display, names, features, lists):** <<CALL-INFORMATION>> rides in the same messages as the progress indicator, and its position matters
  for IE order (C-UP-07); call status "CS call connect" interacts with when the audio is connected; the O201 shows no caller name
  (not a codec matter); keypad codes with feature meaning (C-DTMF-02); the tone capability field shares an octet with display capability.


## Catalog part D: what the handset shows, and the features around a call

Written for the maintainer's variation catalog (docs/handset-variation-catalog.md). Area D covers everything a user sees or presses
around a call: caller number and name, display text, the terminal-capability report, call information and status, date and time,
the base's name, notifications (lamp, missed calls), lists and settings, keypad and DTMF, supplementary services, tones and ring
patterns, internal calls and the handset locator, vendor escapes, and the old procedures.

**Sources read for this part** (all local, converted with `pdftotext -layout`): EN 300 175-5 V2.9.1 (NWK, "NWK"), EN 300 444 V2.5.1
(GAP), TS 102 527-1 V1.5.1 (NG-DECT Part 1), TS 102 527-3 V1.7.1 (CAT-iq Part 3, "P3"). Our notes first: docs/handset-profiles.md,
handset-compatibility.md, vendor-specific-support.md, gap-conformance.md, spec-conformance.md, lines-and-calls.md and the code
(`dect_clip.c`, `dect_cc.c`, `dect_base.c`, `dect_iwu.c`, `dect_time.c`, `dect_mm.c`, `dect_profile.c`, `dect-stubd.c` help text, `profiles/`).
Every clause number below was read in the text, not recalled. Part 5 (TS 102 527-5) is not in our spec set; where Part 3 or the NWK
spec points to it, the row says so.

**Reading the columns.** *Why* starts with the evidence class: `[spec]` = the standard permits or requires it, nothing observed;
`[obs: note]` = seen on a handset and written down in the named note; `[hyp]` = a hypothesis, with what a result would tell apart.
*Ours today*: `key:` = a profile key that acts; `key:` followed by "(parsed, not acted)" = the key is accepted but does nothing in this
build (`dect-stubd.c:4686` prints that warning); `lab:` = a `dect-stubd -a` option; `default` = we send exactly this and have no
alternative; `code` = a sender or handler would have to be written; `chip` = the chip does it; `n/a`. *Check*: `frames` (decoded
messages), `ear`, `eye` (handset display), `hand` (a person presses something), `log` (base log).

**What the code shows about the keys** (checked, because the profile table says more than the build does): of the 13 `cc.*` keys
in handset-profiles.md only six are read by the stack (`cc.call_status`, `cc.basic_service`, `cc.signal.external`, `cc.progress_in_band`,
`cc.name.method`, `cc.name.display`); the link keys `lce.release_after_call`, `reg.tries_0000_first` and `notify` are read too.
`cc.ring_callinfo`, `cc.connect_status_info`, `cc.bind_before_connect`, `cc.signal.internal`, `cc.name.alphabet`, `cc.timer`,
`cc.number.method`, `lists.serve`, `line.selection` and the `feature.*` switches parse and then do nothing (`dect-stubd.c:4686-4698`).

---

#### Caller's number (CLIP)

The calling-party number reaches the handset either in the incoming {CC-SETUP} or in a {CC-INFO} sent afterwards (GAP §8.41: the FT must
support one method, the PT both; the PT's CLIP display itself is feature N.30, optional, GAP Table 1). The element is
<<CALLING-PARTY-NUMBER>> (NWK §7.7.9, id 6CH): a number-descriptor octet (type and plan), usually a second octet with presentation and
screening, then IA5 characters. We send it in the SETUP, with the second octet, presentation "allowed", and a type/plan of unknown for
SIP numbers (`dect_base.c:2917`).

| ID | Variation (what the base could send or do differently) | Spec | Why a strict/odd handset might care | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| D-CLIP-01 | Number in {CC-SETUP} (the present form) | GAP §8.41, §8.12 Table 21; NWK §7.7.9 | [obs: handset-compatibility.md] shown on the A540, C430A, AS690H; the O201 shows the number plus its own "Externer Anruf" text (vendor-specific-support.md §7.7). Control row | default (`dect_cc.c:248` put_identity, `dect_clip.c:178`) | frames, eye | A |
| D-CLIP-02 | Number in a {CC-INFO} after the SETUP, instead of or as well as in the SETUP | GAP §8.41 Table 79; NWK Table 8 (CC-INFO may carry it F to P) | [spec] the PT "is required to support both" but a handset that was only exercised with one path may read only that; we already use the INFO path for internal calls and call waiting, never for an external call | code (INFO path exists only in `ic_name_info`, `dect_base.c:1949`) | frames, eye | B |
| D-CLIP-03 | Leave out octet 3a (presentation and screening); set the extension bit in octet 3 | NWK §7.7.9 octets 3, 3a; GAP §8.41 note 1 (PT may show the IA5 characters "without consideration of octets 3 and 3a") | [hyp] a parser that assumes one descriptor octet would show our octet 3a (0x80) as a stray first character; if so the display shows junk before the digits, otherwise nothing changes | default (always two descriptors, `dect_clip.c:178`) | frames, eye | B |
| D-CLIP-04 | Number type and plan: unknown/unknown (SIP numbers, today), international + E.164, national + E.164, private plan, SIP "From:" plan 1010B | NWK §7.7.9 number-type and plan tables | [spec] a display may format or filter by type; the lab option `-a clip=` sends unknown + E.164 (`dect-stubd.c:8990`) while the SIP path sends unknown + unknown, so two codings are already in use and only one was bench-tested per handset | default (`dect_base.c:2917`); lab:`-a clip=` | frames, eye | A |
| D-CLIP-05 | A leading "+" kept as a character, or stripped with type "international", or turned into "00" | NWK §7.7.9 (IA5 characters; type 001 international); §7.7.41 note 12 (a "Numeric" display shows only space, 0-9, *, #) | [hyp] a numeric-only display (terminal capability display value 2) may drop "+", so "+4930…" shows as "4930…" or fails the phonebook match; tells apart "handset hides unknown character" from "handset uses type" | default (the string goes out as given, `dect_clip.c:112` accepts any printable IA5) | eye | A |
| D-CLIP-06 | Number length: we cap at 32 characters and send no IE when longer; a SIP user part can be longer or not numeric | NWK §7.7.9 (no stated limit); `dect_clip.h` comment (stock capped at 21 or 23 bytes, from the clean-room record) | [spec] no limit, so a handset may truncate or reject; [obs: dect_clip.h] stock was seen capping | default (`DECT_CLIP_NUMBER_MAX` 32; over it, no IE) | frames, eye | B |
| D-CLIP-07 | Presentation "restricted" or "not available" with the address left out or a default, instead of omitting the whole IE | NWK §7.7.9 octet 3a and note 4 | [spec] note 4 permits a default number or no address; a CLIP-capable handset can then say "withheld" or "unknown" instead of showing nothing, or it may fall back to its own text; we omit the IE for any presentation except "allowed" (comment in `dect_clip.h`: the stock value 0x65 is ambiguous) | default (`dect_clip.c:178` returns early) | eye | B |
| D-CLIP-08 | Screening indicator: not screened (SIP callers, today), verified and passed (internal callers, contact-list names), network provided | NWK §7.7.9 octet 3a | [spec] only presentation is described as shown; low value, kept so no value is missed | default (0 for SIP, 1 for internal) | frames | C |
| D-CLIP-09 | Place the number and name IEs before <<BASIC-SERVICE>> in the SETUP instead of after it | NWK §6.3.1 (fixed-length IEs in the message table's order, variable-length ones in ascending id order); `dect_clip.h` field `emit_before_basic_service` | [obs: dect_clip.h] a bench experiment for the FRITZ!Fon C4, "only their place relative to the fixed-length <<BASIC-SERVICE>> moves"; [spec] the standard order is the default; nothing sets the flag today | n/a (flag exists, no setter: `dect_cc.c:310`, `:335`) | frames, eye | C |
| D-CLIP-10 | The number also as display text (a second route to the same eye) | GAP §8.16 Table 27; NWK §7.7.26 | [obs: handset-compatibility.md] the C430A and AS690H show names only as base-drawn display text; the same may hold for numbers on an odd handset | key:`cc.number.method` (parsed, not acted) | eye | B |
| D-CLIP-11 | Internal-call number: terminal identity number as the number, type 3 (network specific), plan 9 (private), screening "verified and passed" | GAP §8.43 Table 82, §14.4 | [spec] the suggested coding; [obs: comment at `dect_base.c:1862`] bench test 5 (2026-10-28): "the C430A showed no caller"; whether this coding fixed that is not recorded in the comment | default (`dect_base.c:1875`) | eye | B |

#### Caller's name (CNIP)

The name travels in <<CALLING-PARTY-NAME>> (NWK §7.7.53, id 6DH): one octet with presentation, alphabet and screening, then the
characters. A Part 3 handset takes UTF-8; a GAP or Part 1 handset is owed DECT characters (IA5 plus the control codes), with the FP
translating what is not IA5 (P3 §7.4.17.1, "ö replaced with oe" as an example). The name may be in the SETUP or in a {CC-INFO}
(GAP §8.42). We send UTF-8 in the SETUP for every handset that has `cc.name.method = cnip`.

| ID | Variation | Spec | Why | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| D-NAME-01 | Name in {CC-SETUP}, UTF-8 (the present form for external calls) | GAP §8.42 Table 80; NWK §7.7.53; P3 §7.4.17.1 | [obs: handset-compatibility.md] works for the A540; the C430A and AS690H show no name from it (bench 2026-10-28 test 1) | default (`dect_base.c:2934`; `key:cc.name.method=cnip`) | eye | A |
| D-NAME-02 | Name in DECT alphabet (IA5) with transliteration of non-IA5 characters, for Part 1 and GAP handsets | P3 §7.4.17.1; NWK Annex D.2.3; Annex M note 2 | [spec] a Part 1 handset has no duty to understand UTF-8, and Part 3 says the FP sends it DECT characters; the C430A and AS690H are Part 1 and receive UTF-8 today, so umlauts may show as junk or hide the whole name. The key exists but the build ignores it | key:`cc.name.alphabet` (parsed, not acted); `dect_clip_set_name` takes DECT but no transliteration exists | frames, eye | A |
| D-NAME-03 | Name in a {CC-INFO} as <<CALLING-PARTY-NAME>> after the SETUP (for external calls) | GAP §8.42 Table 81; NWK Table 8 | [obs: comment at `dect_base.c:1899`, handset-compatibility.md] bench test 11 (2026-10-28): the A540 took a name sent inside the SETUP of an internal call for "via Intern DECT"; the name then went in a {CC-INFO} (not re-checked); [hyp] the same ordering may matter for external calls | code (INFO path only for internal calls) | frames, eye | A |
| D-NAME-04 | Name in both the SETUP and an INFO | GAP §8.42 Tables 80, 81; §8.44 "may receive two names" | [spec] allowed; a handset that keeps the first and one that keeps the last both end up right | code | frames, eye | B |
| D-NAME-05 | Presentation indicator: allowed (today), restricted, "name not available", "handset locator" | NWK §7.7.53 | [spec] note 3: "name not available" should not be used to mean "no CNIP"; we omit the IE unless "allowed". Handset locator is its own row (D-LOC-01) | default (`dect_clip.c:204`) | frames | C |
| D-NAME-06 | Screening: contact-list names "verified and passed", SIP names "not screened" | NWK §7.7.53 note 1; P3 §7.4.10.5.1.3 note 1 | [spec] a handset could show a marker for verified names; unknown if any does | default (`dect_base.c:2934`) | eye | C |
| D-NAME-07 | Name length: we truncate at a complete UTF-8 character, at most 64 octets; a short display shows fewer | `dect_clip.h` comment (stock budget of at most 58); NWK §7.7.41 octets 3d-3g (stored characters, lines, characters per line) | [spec] the handset's own display size is in its capability report, which we do not read; [hyp] a handset that rejects a name over its limit shows nothing | default (`DECT_CLIP_NAME_MAX` 64; `dect_clip_put_name(..., 0)`) | eye | B |
| D-NAME-08 | Alphabet "network specific" (111B) | NWK §7.7.53 | [spec] reserved for a network; no profile uses it | code (enum exists, no path) | frames | C |
| D-NAME-09 | Normalise names to NFC before sending (no combining marks) | NWK Annex M note 2 ("the FP should not use" combining forms) | [spec] a handset showing "o" then a loose diaeresis instead of "ö" tells apart "no normalisation" from "no glyph"; our UTF-8 check accepts well-formed text without normalising (`utf8_name_ok`, `dect_clip.c`) | code | eye | B |
| D-NAME-10 | <<CALLED-PARTY-NAME>> to the calling handset on an outgoing call (name of the dialled contact) | NWK §7.7.52, Table 8 (F to P allowed) | [spec] the IE says the service is defined in Part 3, but V1.7.1 has no procedure that uses it (see the ambiguity list); a handset that shows it would show the contact's name while dialling | code | eye | C |

#### Display text

Two display routes exist besides the name element. <<MULTI-DISPLAY>> (28H) carries a list of DECT characters and may ride in any CC
message from the FP except {CC-NOTIFY} and {IWU-INFO} (GAP §8.16; NWK Table 7 note 14 and Table 8 note 6: MULTI or SINGLE, not both). The characters are
IA5 above 20H plus control codes below it (NWK Annex D.2.2, Table D.1: 0CH clear display, 02H return home, 08H to 0BH cursor,
0DH return, 0EH/0FH flash off/on, 19H/1AH clear to end of display/line); codes from 80H up are "extended" and need an ISO 2022
escape (D.2.4). The handset reports which control codes and which 8-bit sets it understands in <<TERMINAL-CAPABILITY>> (octets 5, 5a).
We send one MULTI-DISPLAY in one {CC-INFO} when the handset's ALERTING arrives, with a framing taken from the profile.

| ID | Variation | Spec | Why | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| D-DISP-01 | One <<MULTI-DISPLAY>> with the bare text in a {CC-INFO} after the handset alerts (the present default framing `hex:{text}`) | GAP §8.16 Table 27; NWK §7.7.26 | [obs: handset-compatibility.md] the C430A shows it as "a separate screen, then the ringer screen with the number" | key:`cc.name.method` (display, both) and `cc.name.display` (`dect_base.c:2253`, `dect_profile.c:628`) | eye | A |
| D-DISP-02 | A framing with cursor control codes before the text: clear display (0CH), return home (02H), clear to end of line (1AH), return (0DH) | NWK Annex D.2.2 Table D.1; GAP Table 27 (08-0BH, 0DH only if the PT has feature N.25) | [obs: profiles/gigaset-c430a.profile] stock's framing `02 0a 1a 91 02 02 {len} {text} 0d` put the number line, not the name, on the C430A screen ("unconfirmed"); the sequence holds octets 91H 02H 02H that are not in Table D.1 (codes from 80H up are "extended", D.2.4), so their meaning is not established | key:`cc.name.display` (data) | eye | A |
| D-DISP-03 | Text preceded only by 0CH (clear display) | NWK Annex D.2.2 | [spec] the one control code "mandatory for all displays" (NWK §7.7.41 "Control codes"); a minimal framing that separates our text from the handset's own | key:`cc.name.display` | eye | A |
| D-DISP-04 | Several MULTI-DISPLAY elements, or several INFO messages, one line each | NWK Table 8; D.2.2 (0AH move down) | [hyp] a two-line display may show line 1 as the "name" and line 2 as the number; tells apart "one logical line" from "two" | key:`cc.name.display` (one element only) | eye | B |
| D-DISP-05 | <<SINGLE-DISPLAY>> (E8H, one character per element) | NWK §7.6.5, Table 75 | [spec] the old single-character form, allowed in the SETUP, SETUP-ACK and INFO tables; no handset we hold is known to need it | code | frames, eye | C |
| D-DISP-06 | Put the display text in another message than the one INFO: the SETUP, the CC-ALERTING, the CONNECT, the SETUP-ACK | GAP §8.16 ("any CC message in the F to P direction except {CC-NOTIFY} and {IWU-INFO}"); NWK Tables 7, 8, 9 (SETUP-ACK lists both display IEs) | [spec] a handset may draw FP text only in the SETUP screen or only after the answer; today it goes out once, in an INFO, on ALERTING (`dect_base_info_tick`, `dect_base.c:2276`) | code | frames, eye | B |
| D-DISP-07 | Timing: before the handset alerts (straight after the SETUP), after ALERTING (today), after the answer | GAP §8.16 | [obs: handset-compatibility.md] the C430A shows the text on "a separate screen, then the ringer screen with the number"; [hyp] sending it before ALERTING changes that order, tells apart "screen order follows our timing" from "the handset always does it" | lab:`-a infoie=HEX` (once on ALERTING); code for the others | eye | A |
| D-DISP-08 | German and other accents through an ISO 2022 escape to Latin-1 (ISO 8859-1), -15, -9 or Greek, only to a handset whose capability octet 5a says it understands it | NWK Annex D.2.4; §7.7.41 octet 5a | [spec] <<MULTI-DISPLAY>> may hold only DECT characters; our `dect_profile_display` copies the name's UTF-8 bytes straight in (`dect_profile.c:628`), so "ä" goes out as C3H A4H, two extended codes with no designated set, and a Part 1 handset may show two junk characters or drop the text | code | frames, eye | A |
| D-DISP-09 | Transliterate to IA5 for display text (ä to ae or a) | P3 §7.4.17.1 (as the example for CNIP); NWK Annex D.2.3 | [spec] the safe subset for every display ("Numeric", "Alphanumeric" and "Full display" all include IA5 letters, §7.7.41 notes 12-14) | code | eye | A |
| D-DISP-10 | Fit the text to the display: number of lines and characters per line and the scrolling type (1 or 2, Annex J) from the capability report | NWK §7.7.41 octets 3d-3h; Annex J | [hyp] a long name that does not scroll the way we assume is cut or wraps over the number line | code (the parser keeps tone, display and profile bits only, `dect_mm.c:843`) | eye | B |
| D-DISP-11 | Flash on/off (0FH/0EH) around the name, or "clear to end of display" (19H) | NWK Table D.1 notes 1-2; capability "Control codes" 3 or 4 | [spec] only if the handset reports control codes level 3 or 4; low value, listed so the set is complete | key:`cc.name.display` | eye | C |
| D-DISP-12 | A one-line "Line u is used" notice after the FP picks a line for a GAP or Part 1 handset | P3 §7.4.5.2.4.2, Figure 41 | [spec] "may notify"; the handset shows which line it is on | code | eye | C |
| D-DISP-13 | A display notice to an idle handset when a single-call line is busy, before it tries to dial | P3 §7.4.8.3 note 1 | [spec] optional; avoids a refused SETUP | code | eye | C |
| D-DISP-14 | Keypad-protocol answers: a display reply to a service code the handset dialled ("not supported") | NWK §10.2 ("may optionally inform the user with display messages") | [spec] the FT may ignore silently (what we do); a handset user who dials a network code and sees nothing may think the call is dead | default (ignored) | eye | C |

#### Terminal capability, model identity

The handset reports itself in <<TERMINAL-CAPABILITY>> (NWK §7.7.41, id 63H) in {ACCESS-RIGHTS-REQUEST} and {LOCATE-REQUEST} (GAP §8.17; P3
§7.4.9.1): tone capability, display capability, echo/noise/volume, slot types, stored display characters, lines, characters per
line, scrolling, eleven profile-indicator octets, control codes and algorithms (octet 5), 8-bit sets (5a), blind slots (6). When the IE is
missing, the spec assumes defaults (GAP Table 29, P3 Table 36). <<MODEL-IDENTIFIER>> (NWK §7.7.46, id 78H) gives manufacturer code and
model. We parse the tone and display nibbles and the profile indicators, and act on Part 1 and Part 3 only (`dect_mm.c:843`).

| ID | Variation | Spec | Why | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| D-CAPS-01 | Read the display capability (none, numeric, numeric-plus, alphanumeric, full) and decide what we send: whether to send a name at all, letters, "+", control codes | NWK §7.7.41 display coding and notes 12-14 | [hyp] a numeric display refuses letters; tells apart "capability decides" from "handset ignores it" | code (stored in `struct dect_term_cap.display`, never read) | frames, eye | A |
| D-CAPS-02 | Read the display geometry and scrolling (octets 3d-3h) | NWK §7.7.41 | [spec] feeds D-DISP-10 and D-NAME-07 | code | frames | B |
| D-CAPS-03 | Read the control-code level (octet 5) and use only the codes it allows | NWK §7.7.41 "Control codes"; GAP Table 27 | [spec] feeds D-DISP-02, -03 and -11 | code | frames | A |
| D-CAPS-04 | Read the 8-bit sets (octet 5a) | NWK §7.7.41; Annex D.2.4 | [spec] feeds D-DISP-08 | code | frames | A |
| D-CAPS-05 | Read the tone capability (none, dial tone only, E.182 tones, complete) and choose between SIGNAL tones and in-band tones | NWK §7.7.41 tone coding; P3 §7.4.15 Table 73 | [spec] a handset with no tone capability needs in-band tones even in Part 3 mode; stored in `tone`, never read | code | ear, frames | B |
| D-CAPS-06 | Use more profile-indicator bits: PI_7 bit 4 (headset), bit 6 (associated melody per contact), bit 7 (NG-DECT Part 5); PI_10 bits 1-3 (date and time recovery, extended list change, screening) | NWK §7.7.41 PI_7, PI_10 | [spec] each bit is a promise by the handset (e.g. bit 6 means it answers alerting patterns 41H-47H); we read bits 2 and 3 of PI_7 only | code | frames | B |
| D-CAPS-07 | No <<TERMINAL-CAPABILITY>> at all: assume the defaults, and treat the handset as GAP | GAP Table 29; P3 Table 36 | [obs: vendor-specific-support.md §7] the O201 reports no capability and no model; with an unknown capability we send the configured service (0x88), which it refused until its profile said `basic` | key:`cap.ngdect`, `trust.terminal_capability`; default in `dect_base_basic_svc` (`dect_base.c:1072`) | frames | A |
| D-CAPS-08 | Use <<MODEL-IDENTIFIER>> (manufacturer code + model value) to suggest a profile, or to refuse to guess one | NWK §7.7.46 (the model identifier is §7.7.46, not §7.7.30) | [obs: handset-profiles.md] `match.model` is the intended hook and cites §7.7.30, which in V2.9.1 is <<PORTABLE-IDENTITY>>; we parse and keep `peer_model` for diagnostics only (`dect_mm.c:882`) | key:`match.model` (suggestion only) | frames | B |
| D-CAPS-09 | An FP model identifier sent to the handset in return | NWK §7.7.46 ("identify the model of a PT to the FT or vice versa") | [hyp] a handset could enable features keyed to the base's maker; we send none; what the handset does with it is unsourced | code | frames | C |
| D-CAPS-10 | CALL-IDENTITY (0x1A) is not the display call id; it names a CC, MM or COMS call for ciphering and parameter retrieval | NWK §7.7.6; Tables 57, 58 (MM-INFO) | [spec] noted so nobody sends it for the Part 3 call id (that is a <<CALL-INFORMATION>> field); belongs to area A | n/a | frames | C |

#### Call information, line and call identity

<<CALL-INFORMATION>> (NWK §7.7.56, id 7EH) is a list of typed values: line identifier (subtypes external, source/target for internal,
"relating to", "all lines", line type), call identifier (and an updated one), call status (and a reason), service identifier. P3 makes the
line id mandatory in every external incoming SETUP (§7.4.5.3.2) and a call id and call status in every call message (§7.4.6.1,
§7.4.6.4). It is for Part 3 handsets only; "A Part 3 FP shall not send any call status to GAP or NG Part 1 devices" (§7.4.6.4.1).

| ID | Variation | Spec | Why | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| D-CINFO-01 | Full information in an incoming SETUP: line, line type, call id, "CS call setup" (Part 3 handsets) | P3 §7.4.5.3.2, §7.4.6.3, §7.4.6.4.3; figure 42 | [obs: handset-compatibility.md] the A540 rings aloud only with the full set; a bare line id gave a silent ring | default for Part 3 (`dect_base.c:1039`); lab:`-a lineid=,linetype=,callid=,callstatus=` | ear, frames | A |
| D-CINFO-02 | Line id only, or call id only | P3 §7.4.5.3.2 | [obs: handset-compatibility.md] A540 silent; kept as the known negative so a new handset is not run on it blindly | key:`cc.ring_callinfo` (parsed, not acted) | ear | C |
| D-CINFO-03 | No call information (Part 1, GAP, unknown handsets) | P3 §7.4.6.4.1 | [obs: the log text in `dect_base_ring_callinfo`] with an unknown capability we send none and an A540 would then ring silently; the O201 reports none, so it is in that case | default (`dect_base_ring_callinfo`) | ear, frames | B |
| D-CINFO-04 | Line type flags: network delay (VoIP) and second-call type (in-band PSTN) | NWK §7.7.56 figure 98g; P3 §7.4.3.10 | [spec] we send delay=VoIP and second-call=common procedures; a PSTN-style setting changes how a handset treats a second call | default (`dect_base.c:1060`, flags `DECT_CALLINFO_NDT_DELAY`) | frames | C |
| D-CINFO-05 | Line identification subtypes for internal calls: "source" and "target line identifier for internal call" as a shorthand for the group of handsets on a line | NWK §7.7.56 line subtypes 01H, 02H | [spec] we send no line in an internal SETUP; one is added to the status messages from CALL-PROC on because the A540 starts its timer only on a CS call connect carrying a line id and type | default (`dect_cc.c:93-103`) | frames | C |
| D-CINFO-06 | Line selection by the handset: confirmed in our SETUP-ACK (Part 3), by the FP on its own ("FP managed", id 7FH "None"), or via `#k` keypad (GAP, Part 1) | P3 §7.4.5.2.2, §7.4.5.2.3 Table 26, §7.4.5.2.4.2 | [spec] three forms for the same choice; a handset that sends "None" expects us to pick a line and say which | default (`dect_cc.c:78-90`); key:`line.selection` (parsed, not acted) | frames | B |
| D-CINFO-07 | "Updated call identifier" when the id of a call changes (transfer, re-injection) | NWK §7.7.56 call identifier subtype 01H; P3 §7.4.3.6 | [spec] needed only when a call changes identity; not built | code | frames | C |
| D-CINFO-08 | Our call id value width and uniqueness: one octet, system-wide, FP-defined, not per handset or line | NWK §7.7.56; P3 §7.4.6.1, note 8 of the IE | [spec] the same id on both ends of an internal call (done) | default (`dect_base_call_id`; internal calls: `dect_base_ic_setup`) | frames | C |

#### Call status and the call timer

Part 3 handsets build a state machine from the call status we send and show a timer when they see "CS call connect"
(P3 §7.4.6.4.1: the PP "shall always rely on 'CS call connect'"). The first call's statuses ride in the CC messages of the same name
(SETUP-ACK, CALL-PROC, ALERTING, CONNECT); a {CC-INFO} may not carry the first call's status before the CONNECT has been sent
(§7.4.6.4.3). GAP and Part 1 handsets draw their own timer; nothing in either spec tells the FP how.

| ID | Variation | Spec | Why | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| D-CSTAT-01 | Call status in SETUP-ACK, CALL-PROC, ALERTING and CONNECT of a handset-placed call (Part 3) | P3 §7.4.6.4.2, §7.4.6.4.3 | [obs: handset-compatibility.md] A540 timer starts at once; the C430A timer "appears by itself after a few seconds" | key:`cc.call_status` (auto = Part 3 only); lab:`-a nocallstatus`, `allcallstatus` (`dect_base.c:4912`) | eye | A |
| D-CSTAT-02 | "CS call connect" sent in a {CC-INFO} after the handset answers an incoming call | P3 §7.4.6.4.3 | [obs: dect_base.c side-ring tick] sent once the call is ACTIVE for Part 3 handsets; the timer needs it | default (`dect_base_side_ring_call_tick`, `dect_base.c:2235`, built in); key:`cc.connect_status_info` (parsed, not acted) | eye | A |
| D-CSTAT-03 | Call status to a Part 1 or GAP handset anyway | P3 §7.4.6.4.1 (forbids it) | [hyp] a Part 1 handset that ignores unknown IEs shows no change; one that parses it badly drops the call; a controlled lab-only row for the C430A timer delay | key:`cc.call_status=on` | eye, frames | B |
| D-CSTAT-04 | The base draws the call timer as display text once a second after the answer | `docs/handset-profiles.md` `cc.timer` ([CR A-0125]: stock does this for some Gigasets) | [obs: handset-compatibility.md] the C430A shows its own timer after a few seconds; whether base-drawn text would appear earlier is untested | key:`cc.timer` (parsed, not acted; "planned") | eye | B |
| D-CSTAT-05 | CONNECT early (U-plane open, call not yet answered at the far end) with "CS call connect" only when the far end answers | P3 §7.4.6.1 ("early CC-CONNECT"), §7.4.6.4.1 | [spec] the timer should start at end-to-end connect, not at CONNECT; today both go out together when the call is answered | default (`dect_cc.c:681`, `:686`) | eye | B |
| D-CSTAT-06 | Failure reasons with "CS call disconnecting": system busy (today), line in use, user busy (10H), number not available (11H) | NWK §7.7.56 call status reason; P3 §7.4.8.3, §7.4.6.4.7, §7.4.6.4.8 | [spec] the handset shows a matching message; only "system busy" has a sender | default for busy (`dect_cc.c:928`); code for 10H and 11H (`DECT_CSR_*` are defined and validated, never sent) | eye, frames | B |
| D-CSTAT-07 | "CS idle" for the last released call (optional) and for every call (required) | P3 §7.4.6.4.3 | [spec] a handset that waits for it before clearing its screen | default (`dect_cc.c:1123`) | eye | C |
| D-CSTAT-08 | "CS call remote hold" and "remote connect" statuses to a handset on an internal call | NWK §7.7.56 notes 13-14; P3 §7.4.6.4.3 | [spec] always available for internal calls | code | eye | C |

#### Date and time

Handsets get the clock by {FACILITY} (CISS, dummy transaction value 6) carrying <<TIME-DATE>> (NWK §7.7.50, id 23H; P3 §7.4.2.0). The FT
must send it after every successful location registration (P3 §7.4.20.2) and may refresh later (§7.4.2.1); the PP may also send its own
(§7.4.2.2), subject to "Clock master". Coding 01 gives time only, 10 date only, 11 both; the time zone is in quarter-hours.
Neither GAP nor Part 1 mentions the clock at all (checked: no hit in either text), so a Part 1 handset has no standard way to be
told the time. We send after location registration and, if `time_update` is set, in a periodic round that pages handsets without a link;
both are gated on a disciplined clock.

| ID | Variation | Spec | Why | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| D-TIME-01 | {FACILITY} with TIME-DATE after each location registration | P3 §7.4.20.2, §7.4.2.1 | [obs: handset-compatibility.md] the A540 gets the date after a reboot; the C430A and AS690H do not show one | default (`dect_time.c`; lab:`-a timesync=auto/force/off`) | eye | A |
| D-TIME-02 | Periodic round, paging a handset that has no link | P3 §7.4.2.1 (indirect link establishment) | [obs: gap-conformance.md] built 2026-10-29, "assumed" until bench card 36 | lab:`-a timeupdate=MIN`; uci `time_update` | log, eye | B |
| D-TIME-03 | Time zone: local time with the box's offset (today), UTC with zone 0, or the null value | NWK §7.7.50 time zone; P3 §7.4.2.0 | [spec] "should correctly set" the zone, null if unknown; a handset that adds its own offset would show the wrong hour if we send the zone and it ignores it | default (offset from the box's clock, `dect_time.h` `tz_valid`) | eye | B |
| D-TIME-04 | Date only or time only (coding 10 or 01) instead of both | NWK §7.7.50 coding | [spec] allowed; P3 Table 16 asks for coding 11 | code | eye | C |
| D-TIME-05 | "No valid time/date available" (interpretation 05H) when our clock is not set, instead of sending nothing | NWK §7.7.50 table of interpretations; P3 §7.4.20.3 | [spec] tells a handset that has "Date and Time recovery" (PI_10 bit 1) that it must not trust us; we stay silent | code (`DECT_TIME_INTERP_NO_VALID` defined, not sent) | frames | C |
| D-TIME-06 | We ask the handset for its time ("request to send current time/date", 04H) when our clock is invalid | P3 §7.4.20.3 method 1 | [spec] only toward PPs that set PI_10 bit 1; a PP's reply arrives as a PT-initiated {FACILITY}, which we do decode (`dect_link.c:365`) | code | frames | C |
| D-TIME-07 | "Clock master" = PP: a handset sets the system time and we follow | P3 §7.4.2, §7.4.11.3.2 | [spec] the field is served read-only as 30H "FP" (`dect_iwu.c`); a handset's own time is decoded and handed to the owner as `pp_time` (`dect_link.c:365`), which decides whether to use it | default (30H) | log | C |
| D-TIME-08 | TIME-DATE inside {IWU-INFO} or {CLMS-VARIABLE} instead of {FACILITY} | NWK Tables 20, 37 | [hyp] the only other messages that carry the element; if a Part 1 handset reads its clock from one of them, that explains why it ignores {FACILITY}; unsourced | code | eye | C |
| D-TIME-09 | The time sent just after a reboot is wrong (box clock not yet disciplined): hold, or send with the "no valid" form | P3 §7.4.20.3 | [obs: gap-conformance.md] gate exists (nothing sent without a disciplined clock) | default (gate) | log | C |

#### The base's name and the handset's idle label

During registration the FP's name is broadcast (up to 17 characters) in a connectionless {CLMS-FIXED} message with a <<NETWORK-PARAMETER>>
"device name" (discriminator 10H) so the PP can show a list of bases (P3 §7.10.1.3.2; NWK §7.7.29). Idle handsets show a label such as "DECT 4":
the terminal identity number is the low 8 bits of the individual TPUI assigned in the {LOCATE-ACCEPT} (GAP §14.4; P3 recommends not
using it, GAP §14.4.1 note).

| ID | Variation | Spec | Why | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| D-FPN-01 | Broadcast an FP name during registration mode | P3 §7.10.1.3.2, Tables 77-79; NWK §7.7.29 | [spec] a Part 3 handset in search mode shows the name; without it "Unknown" or the RFPI (note 2). We found no chip mail for the {CLMS-FIXED} data sections (no hit in `src/`); area A | chip (not set by us) | eye | B |
| D-FPN-02 | Idle label from the terminal identity number ("DECT n") | GAP §14.4.2; Table 130 | [spec] derived from the TPUI we assign; we assign from the slot (`tpui_cfg_for_slot`); the number and the label can disagree with the Internal Names List numbers | default | eye | B |
| D-FPN-03 | Name of the system in the DECT System Settings List (firmware, EEPROM, hardware version fields) | P3 §7.4.11.3.9-.11 | [obs: dect_iwu.h] "A Gigaset considers itself synchronised with a base once it has read" its lists; we serve firmware and EEPROM versions, not hardware | default (`dect_iwu.c`, fields 09H, 0AH) | eye | C |

#### Notifications: message lamp, missed calls, list changes

A {FACILITY} (CISS, dummy TI 6) carries <<EVENTS-NOTIFICATION>> (NWK §7.7.55, id 7DH): event type, optional subtype, multiplicity (1 or 2 octets),
with a <<CALL-INFORMATION>> naming the line (P3 §7.4.1.1). Event types: message waiting, missed call, web content, list change, software
upgrade, line use status, handset use status, diagnostic, SMS, proprietary. Voice MWI has multiplicity = messages in the box, zero to switch the
lamp off; it is sent after every location registration (P3 §7.4.1.2). Missed calls carry the number of unread entries and, in the same IE, a
list-change indication (§7.4.1.3). A handset without a link is reached by indirect link establishment (CLSS). Only Part 3 handsets get these from us.

| ID | Variation | Spec | Why | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| D-EVT-01 | MWI voice count per line, sent when the count changes on a link that is up, and after location registration | P3 §7.4.1.2 Table 14 | [obs: Limits.md] built, not seen on air | default (`dect_base.c:3271`, control verb `mwi`) | eye | A |
| D-EVT-02 | MWI or a missed-call count to a handset that has no link: page it at once, or in the next paging round (today) | P3 §7.4.1.1 (CLSS), §7.4.2.1 | [spec] required for an idle handset; ours queues it and pages in the round (`page_want`, `dect_base.c:3002`, `:3410`, `:3704-3805`), so the lamp can lag by the round's cadence; [obs: comment `dect_base.c:3198`] list-change notices have no such path ("learns at its next location registration") | default for MWI and missed calls; code for list-change notices | eye, log | B |
| D-EVT-03 | MWI to a GAP or Part 1 handset | P3 §7.4.1 applies to Part 3; GAP and Part 1 texts have no MWI procedure (checked) | [obs: dect_base.c:3195 comment] we never send it to them, "what it would make of one is untested" | key:`notify=on` forces it | eye | B |
| D-EVT-04 | MWI "unknown" subtype (80H) or "unread" semantics (count vs "yes/no") | NWK §7.7.55.3, P3 §7.4.1.2 notes | [spec] the count may be total or unread, network dependent; a lamp-only handset wants non-zero | default (count as given by the owner) | eye | C |
| D-EVT-05 | Missed-call notification: multiplicity = unread, subtype 01H "a new one just arrived" vs 02H "count only", sent with the list-change indication | NWK §7.7.55.3; P3 §7.4.1.3 | [obs: handset-compatibility.md] the A540 lists and marks read missed calls; the notice itself (idle counter) is not recorded | default (`dect_base.c:3324`) | eye | A |
| D-EVT-06 | List-change indication after location registration for the Line Settings List and List of Supported Lists | P3 §7.4.10.2.2 (mandatory for Line Settings and Internal Names), `dect_base.c:3222` | [obs: profiles/gigaset-a540.profile] the A540 gives up its list session when these arrive and never reads the Internal Names List; `notify = off` cures it | key:`notify` | eye, log | A |
| D-EVT-07 | List-change indication for the Internal Names List when a handset is added, renamed or removed | P3 §7.4.10.2.2 | [spec] mandatory; [obs: handset-compatibility.md] the A540's INT list works also for handsets on other cells | default (`dect_base_names_notify`, `dect_base.c:3440`, only to handsets whose link is up) | eye | B |
| D-EVT-08 | Extended list-change detail (<<LIST-CHANGE-DETAILS>>, 74H) | NWK §7.7.57; PI_10 bit 2 | [spec] Part 5 feature; not described in our spec set beyond the IE | code | frames | C |
| D-EVT-09 | Other event types: line use status, handset use status, diagnostic, web content (RSS), SMS, "software upgrade indication: firmware" | NWK §7.7.55.2-.3 | [spec] Part 5 and Part 4 (TS 102 527-5, -4) define them; neither is in our set | code | frames | C |
| D-EVT-10 | Proprietary events (flag set, 2-octet discriminator = EMC, length, value) | NWK §7.7.55.5, figure 98e | [spec] a vendor's own lamp or counter; the frames of one handset may show some | code | frames | C |

#### Lists and settings (list access)

CAT-iq handsets read and edit lists over a list-access call: {CC-IWU-INFO} messages carrying <<IWU-TO-IWU>> (P3 §7.4.10). We serve the List of Supported
Lists (00H), missed (01H), outgoing (02H), accepted (03H), all calls (04H), contacts (05H), internal names (06H), system settings (07H), line settings
(08H) and all incoming (09H). A handset keeps the copy of the supported-lists list it read once, so what we declare steers which menus it shows.

| ID | Variation | Spec | Why | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| D-LIST-01 | Refuse every list ("list not supported") | P3 §7.4.10.4.1 (start session reject) | [spec] the cleanest way to prove a handset does not need them; for a handset that misbehaves around lists | key:`lists.serve` (parsed, not acted; always served) | frames | A |
| D-LIST-02 | Declare fewer lists in the List of Supported Lists (for example only 06H and 08H) | P3 §7.4.10.5.2 | [obs: handset-compatibility.md] the A540 reads 06 and 08 by itself on reconnect; fewer lists may avoid menus a handset renders badly | code (`supported_lists`, `dect_iwu.c:14`, no per-handset key) | frames, eye | B |
| D-LIST-03 | Contact List fields: add "associated melody" (04H) | P3 §7.4.10.5.7 Table 69, §7.4.19.1 | [spec] only for a PT with PI_7 bit 6; handset edits would then pick melodies | code | frames, eye | C |
| D-LIST-04 | Contact numbers: format stored (with "+", national, spaces) | P3 §7.4.10.5.1.11 | [hyp] a handset dialling from the list sends `1C 15` plus the number (`dect_cc.c:612`); a number with spaces or "+" may or may not survive the controller | default (as stored by the owner) | frames, hand | B |
| D-LIST-05 | Partial delivery of read entries and the data-packet size (56 octets per packet, five entries minimum) | P3 §7.4.10.4.3.2, §7.4.10.4.10 | [obs: dect_iwu.h] "a handset that does not get them keeps resynchronising and runs a list-access call on every link" | default (`DECT_IWU_OUT_MAX` 16) | frames | B |
| D-LIST-06 | Internal Names List entry: terminal number and name up to 16 UTF-8 octets | P3 §7.4.10.5.8 | [obs: handset-compatibility.md] the AS690H has no INT list at all (number typed after INT), the A540 has one | default (`DECT_IWU_NAME_MAX` 16) | eye | B |
| D-LIST-07 | System Settings fields beyond the four served (FP IP address/mask/gateway/DNS 04H-08H, hardware version 0BH, emission mode 0CH) | P3 §7.4.11.3 | [spec] optional for the FT; a base-settings menu may show or hide rows by what we offer | default (`dect_iwu.c`) | eye | C |
| D-LIST-08 | Line Settings fields beyond name, id, attached handsets, multiple-call mode and forwarding: dialling prefix 04H, FP melody 05H, FP volume 06H, blocked number 07H, intrusion 09H, permanent CLIR 0AH | P3 §7.4.11.4 | [spec] optional; M for forwarding on an R/B FT (P3 Table 9, NG1.N.18), which we serve as "off", read only | default (forwarding off, not editable: `dect_iwu.c:611`) | eye | C |
| D-LIST-09 | Fields marked editable vs read-only; a handset that offers an edit gets "procedure not allowed" | P3 §7.4.10.4.9 reason 07H | [spec] hide-versus-reject; a menu that offers edits and then fails reads as a bug | default (settings read-only) | hand, frames | C |
| D-LIST-10 | PIN-protected fields (current PIN returned as FFFFFFFF; new PIN needs the old; three tries) | P3 §7.4.11.3.1, §7.4.11.3.13 | [spec] a handset's "change PIN" menu can only work with these two fields | default | hand | C |
| D-LIST-11 | Call lists: missed, accepted, outgoing, all; read status, number of calls, line name, date and time of each entry | P3 §7.4.10.5.3-.6, §7.4.10.5.1.4 | [obs: handset-compatibility.md] missed calls read and marked read on the A540 (2026-10-30); accepted and outgoing not yet checked on air | default (`dect_calls.c`) | eye, hand | B |
| D-LIST-12 | Handset edits to the phonebook (save, edit, delete entry) | P3 §7.4.10.4.4-.6 | [obs: handset-compatibility.md] edits from the handset not yet seen on air | default | hand, log | B |

#### Keypad, dial codes, DTMF

The handset sends digits in <<MULTI-KEYPAD>> (2CH) in {CC-INFO} messages, or all at once in the SETUP (en bloc) (GAP §8.10; NWK §7.7.27). Keypad
codes below 20H are DECT control codes (Table D.1): 05H dialling pause, 12H go to pulse, 14H go to DTMF (defined length), 15H register recall,
16H go to DTMF (infinite), 17H internal call, 18H service call, 1CH "supplementary service" prefix (P3 Table 17 control codes). GAP lists
register recall, DTMF and pause as mandatory for the PT and optional for an R/B FT (GAP Table 5: N.5, N.6, N.7). <<SINGLE-KEYPAD>> (E9H) is the
one-character form. We collect keypad octets, wait `dialwait` (3000 ms by default) after the last digit unless the handset sends SENDING-COMPLETE,
then hand the string to the owner, and refuse a number that has any character outside `0-9 * # + A-D`.

| ID | Variation | Spec | Why | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| D-KEY-01 | Dial pause after the last digit when no end marker is sent | GAP §8.3 (overlap sending optional for R/B FT), §8.10; NWK §9.3.1.5 | [obs: vendor-specific-support.md §7] 3 s for every handset; the controller does not expose it | lab:`-a dialwait=MS`; uci `opendectcell.main.dialwait` | ear, hand | A |
| D-KEY-02 | Connect at once instead of SETUP-ACK, digits, CONNECT ("en bloc") | GAP §8.2 (en-bloc SETUP) | [spec] a handset that sends the whole number in its SETUP needs no pause | lab:`-a nooverlap` | frames | B |
| D-KEY-03 | SENDING-COMPLETE (A1H) from the handset ends the wait | NWK §7.6.2, §9.3.1.5 | [spec] optional; we honour it (`dect_cc.c:969`) | default | frames | C |
| D-KEY-04 | <<SINGLE-KEYPAD>> (E9H), one digit per element | NWK §7.6.6; P3 §7.4.14.1.1 ("MULTI-KEYPAD or KEYPAD") | [spec] the older form; our dial and DTMF collectors read only 2CH (`dect_cc.c:532`), so such digits vanish | code | frames, hand | B |
| D-KEY-05 | Control codes inside a dialled string (05H pause, 12H, 14H, 16H) | GAP Table 20; NWK Table D.1 | [spec] a stored number with a pause must dial; we refuse any octet that is not a key with "user unknown" (`dect_base.c:1557`) | code | frames, hand | A |
| D-KEY-06 | Register recall (15H alone) mapped to hold or flash toward SIP | GAP N.5, Table 20; NWK D.2.2 note 4 | [spec] mandatory in the PT; 15H alone in an active call is dropped (only 1CH 15H and 17H are control codes, `dect_cc.c:853`) | code | frames, hand | A |
| D-KEY-07 | Go to DTMF (14H, 16H) before digits in a call; "null" (00H) to stop an infinite tone | NWK Table D.1 and note 3; GAP N.6, N.22 | [spec] for a handset that sends it ahead of every digit in call | code (digits are forwarded; the mode codes are ignored) | frames | B |
| D-KEY-08 | DTMF digits in an active call: forwarded as "digit" events, only `0-9 * #` | P3 §7.4.14.1.2 | [spec] "A", "B", "C", "D" would be dropped in a call but are accepted in a dialled number | default (`dect_base.c:2627`) | hand | C |
| D-KEY-09 | FP-side feedback tone for DTMF to a GAP handset (local feedback is the PP's duty in Part 3) | P3 §7.4.14.3 ("For GAP PPs, the FP may optionally send an in-band feedback") | [spec] a handset with no key tones would show silence while the user presses digits | code | ear | C |
| D-KEY-10 | Dial tone while overlap digits are collected: SETUP-ACK with in-band information, then the FP plays a dial tone | P3 §7.4.14.1.1 figure 66, §7.4.15.1; EN 300 175-5 §9.3.1.4 | [obs: dect-stubd, profile `cc.progress_in_band`] the O201 plays nothing on outgoing calls without the in-band indication; cross-area B and C | key:`cc.progress_in_band` | ear | A |
| D-KEY-11 | `#k` before the number to pick line k (backward-compatible line selection for GAP and Part 1) | P3 §7.4.5.2.3 Table 26 | [spec] the FP must not assume it; a `#` first digit is also an ordinary digit | key:`line.selection` (parsed, not acted) | frames | B |
| D-KEY-12 | Internal call by keypad: 17H plus the terminal number | GAP §8.19 Table 31, §14.4.3 Table 131 | [obs: dect_cc.c:612] handled, including the form a Part 3 handset sends on its list-access call | default (`learn_int_key`) | frames | B |
| D-KEY-13 | Digits typed before the SETUP, kept and sent at once ("pre-dial") | P3 §7.4.14.3 case 2 | [spec] the handset's job; our side is only to accept a SETUP that already holds the whole number | default | frames | C |

#### Supplementary services (hold, transfer, conference, forwarding, CLIR, feature keys)

Part 3 does these with control codes in {CC-INFO}: `1C xx` (toggle 31H, conference 32H, release 33H, transfer 34H, accept 35H, reject 36H,
release with replacement 38H, hold 41H, resume 42H; `1C 15` plus a number for a parallel external call; `17` plus a number for a parallel internal
call), with the call id in <<CALL-INFORMATION>> (P3 Table 17, §7.4.3). The older routes are in NWK chapter 10: a keypad protocol (§10.2), a feature-key
protocol with <<FEATURE-ACTIVATE>> and <<FEATURE-INDICATE>> (§10.3), and a functional protocol, either {HOLD}/{RETRIEVE} messages (§10.4.1) or <<FACILITY>> with ISDN components
(§10.4.2, list in §10.6.1). Broadcast capability bits (a30-a34, P3 Table 37) tell the handset which Part 3 services exist.

| ID | Variation | Spec | Why | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| D-SS-01 | Hold and resume with `1C 41` and `1C 42`; toggle with `1C 31` | P3 §7.4.3.5.3, §7.4.3.5.8-.9, Table 17 | [obs: Limits.md] built (up to three calls per handset), not seen on air | default (`dect_cc.c:853` control_code; `dect_base.c:2570` par_control) | hand, frames | B |
| D-SS-02 | Old {HOLD}, {HOLD-ACK}, {HOLD-REJECT}, {RETRIEVE}, {RETRIEVE-ACK}, {RETRIEVE-REJECT} messages | NWK §6.3.3.2-.7, §10.4.1 | [spec] the GAP-era hold; a handset without Part 3 may use it; an unknown message on a call we hold is ignored (EN 300 444 §6.9.4) | code (ignored) | frames, hand | C |
| D-SS-03 | Call transfer, announced (`1C 34` after a second call) and unannounced; CLIP/CNIP of the remote party on transfer | P3 §7.4.3.6.1-.5, NG1.N.8 (M for an R/B FT) | [obs: Limits.md] "Transfer" not built | code | hand, eye | B |
| D-SS-04 | Three-party conference (`1C 32`, call status 0BH "conference connect") | P3 §7.4.3.7, NG1.N.9 (O), bit a31 | [spec] only if we set a31; we do not claim it | code | hand | C |
| D-SS-05 | Intrusion (a32) and call deflection (a33), by control code or settings field | P3 §7.4.3.8, §7.4.4, Table 9 (NG1.N.10, N.11 are O) | [spec] optional for R/B | code | hand | C |
| D-SS-06 | Call forwarding unconditional, no answer, busy: set from the Line Settings fields (0BH-0DH) with activation codes sent to the network | P3 §7.4.13, §7.4.11.4.11-.13 | [spec] mandatory for an R/B FT (NG1.N.18); we serve the three fields as "off", not editable | default (`dect_iwu.c:611-629`) | hand | C |
| D-SS-07 | CLIR, permanent (Line Settings field 0AH, a30) or temporary (a prefix sequence typed before the number) | P3 §7.4.12.2-.3 | [spec] a temporary prefix such as `*31#` reaches the owner inside the number string; the owner would have to know it | default (`*` and `#` allowed in a number; no meaning attached) | hand | C |
| D-SS-08 | <<FEATURE-ACTIVATE>> (38H) from the handset: feature key nn (42H), specific line selection (44H), register recall (01H), cost information (60H), queue entry, subscriber number, echo control | NWK §7.7.16, §10.3, §10.6.2 | [spec] the old stimulus route; we ignore the IE and send no <<FEATURE-INDICATE>> (39H) | code | frames, hand | C |
| D-SS-09 | CRSS and CISS with <<FACILITY>> (ISDN components: CFU, CLIR, CT, CW, hold, etc.), {CISS-REGISTER} | NWK §10.4.2.1-.2, §10.6.1 | [spec] we answer nothing to a {FACILITY} on a call; {CISS-REGISTER} is not handled (only the PT's time {FACILITY}, `dect_link.c:365`) | code | frames | C |
| D-SS-10 | A negative acknowledgement (call status plus tone 09H) for a control code we do not support | P3 §7.4.3.4 | [spec] required; we send it to Part 3 handsets and drop it for others | default (`dect_cc.c:853` control_code) | ear, frames | B |
| D-SS-11 | Headset management: a handset sets PI_7 bit 4 and expects call interception and hand-over between handset and headset | P3 §7.4.16 | [spec] feature NG1.N.21 is C907 for an R/B FT; no profile asks for it | code | hand | C |
| D-SS-12 | Lines where second calls are signalled in band (PSTN style): "Second call type" = 1 in the line type, off-hook CLIP | P3 §7.4.3.10 | [spec] our one line is VoIP; the flag would change the handset's second-call menus | default (SCT 0) | hand | C |

#### Alerting patterns and tones

<<SIGNAL>> (E4H, NWK §7.6.8) carries a tone (00H dial, 01H ring-back, 02H intercept, 03H congestion, 04H busy, 05H confirm, 06H answer, 07H call waiting,
08H off-hook warning, 09H negative acknowledgement, 3FH tones off) or an alerting pattern (40H-47H patterns 0-7, 48H continuous, 4FH off). It
rides in the SETUP or in a {CC-INFO} in the "call received" state (GAP §8.14 Tables 23, 24: the FT must support one, the PT both). P3 Table 73 assigns every tone a
method per handset class: Part 3 handsets get SIGNAL (method 1) or the network's in-band tone (3); GAP and Part 1 handsets get an in-band tone from the
FP (2) or the network's (3); and "the FP shall not send any <<SIGNAL>>" for a tone it supplies in band (§7.4.15.2.2.0).

| ID | Variation | Spec | Why | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| D-TONE-01 | External call: pattern 41H in the SETUP (default); other 40H-48H | GAP §8.14; P3 §7.4.19.0 | [obs: dect-stubd help, `nosignal`] the A540 rings silently without any SIGNAL | key:`cc.signal.external` (acts); lab:`-a signal=N`, `nosignal` | ear | A |
| D-TONE-02 | Internal call: pattern 40H (default) | GAP Table 23; P3 §7.4.19.0 | [spec] the handsets should ring differently for internal and external | default (hard-wired, `dect_base.c:1157`); key:`cc.signal.internal` (parsed, not acted) | ear | B |
| D-TONE-03 | The SIGNAL in a {CC-INFO} after ALERTING instead of in the SETUP | GAP §8.14 Table 24 | [spec] a handset that honours only one of the two methods; the whole row is a gap in our build, the PT being "required to support both" | code | ear | B |
| D-TONE-04 | Patterns 42H-47H by contact (associated melody) toward handsets that set PI_7 bit 6; only 41H to the rest | P3 §7.4.19.1 | [spec] per-contact ring tones; we send one pattern | code | ear | C |
| D-TONE-05 | The FT drives the ring cadence: 48H continuous and 4FH off, alternating in {CC-INFO} | NWK §7.6.8 note 1 | [spec] the old way for a PT without patterns; unsourced for any handset we hold | code | ear | C |
| D-TONE-06 | Ring-back tone on SIGNAL 01H to a Part 3 handset; in-band ring-back to a Part 1 or GAP handset | P3 Table 73, §7.4.15.2.1, §7.4.15.2.2.0 | [spec] mandatory; `dect_cc.c` sends 04H (busy), 07H (call waiting), 09H (negative acknowledgement) and 3FH only; ring-back is whatever the audio path plays; cross-area B and C | code (SIGNAL 01H never sent) | ear | A |
| D-TONE-07 | Dial tone (00H), congestion (03H), off-hook warning (08H), confirm (05H), answer (06H), intercept (02H) | NWK §7.6.8; P3 Table 73 | [spec] optional or "N/A"; listed so the set is whole | code | ear | C |
| D-TONE-08 | Busy: SIGNAL 04H with CS "disconnecting" to a Part 3 handset; in-band busy tone to others | P3 §7.4.8.3, Table 73 | [spec] our busy path is built for call status; a Part 1 handset hears whatever the audio path gives | default (`dect_cc.c:927`) | ear | B |
| D-TONE-09 | In-band availability signalled by <<PROGRESS-INDICATOR>> (in SETUP-ACK, CALL-PROC or ALERTING) or by the CONNECT; avoid it in a {CC-INFO} | P3 §7.4.15.2.2.1 and notes; NWK §7.7.31 | [obs: profiles/motorola-o201.profile] the O201 needs it in SETUP-ACK and CALL-PROC; sending it only with ALERTING gave audio on 0 of 3 calls; cross-area B and C | key:`cc.progress_in_band` | ear | A |
| D-TONE-10 | "Tones off" (3FH) once real in-band audio starts | P3 §7.4.15.1 | [spec] stops a locally generated tone | default (`dect_cc.c:1122`) | ear | C |
| D-TONE-11 | Call-waiting tone (07H) plus the number or name in {CC-INFO}, in this order (call status first, then SIGNAL, number, name) | P3 §7.4.3.5.10-.11, figures 14, 15 | [obs: Limits.md] built, not seen on air | default (`dect_cc.c:1705`) | ear, eye | B |

#### Internal calls, service calls, group ringing, handset locator

An internal call is a SETUP with call class 9 (GAP §8.18; Part 1 §7.3.6 Tables 13 and 14 for an NG PP) and the digits `17H` + terminal number in the keypad
(§8.19); the FP rings the target with a SETUP that carries the caller's number and name (GAP §8.43, §8.44). Part 1 §7.3.6 adds that for a GAP
PP the FP "shall use the 'external call' call class". A service call is class 0BH with keypad `18H` (§8.20, §8.21). The locator makes every idle
handset ring (P3 §7.10.2, feature NG1.A.3, optional for an R/B FT). The call classes the NWK spec lists (§7.6.4) and we refuse with
"service not implemented" are ULE (3), message call (4), DECT/ISDN IIP (7), service call (B), supplementary service call (D) and OA&M (E)
(`served_call_class`, `dect_cc.c:1247`); every other class is processed, including emergency (A) and external handover (C).

| ID | Variation | Spec | Why | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| D-INT-01 | Basic service of an internal SETUP to a GAP or Part 1 handset: class 9 (GAP §8.18) or class 8 "external" (Part 1 §7.3.6 for GAP PPs) | GAP §8.18 Table 30; TS 102 527-1 §7.3.6 Tables 13, 14 | [obs: handset-compatibility.md] the AS690H rang for 29 s and sent nothing; the C430A shows "1"; two specs give two answers | default (class 8 for a known non-NG handset, `dect_base_basic_svc`, `dect_base.c:1072`; class 9 for NG handsets) | ear, eye | A |
| D-INT-02 | Internal caller shown: number and name both in the SETUP (today, Part 1 targets), or number in the SETUP and name in an INFO (today, Part 3 targets) | GAP §8.43, §8.44; P3 `dect_base.c:1899` | [obs: dect_base.c:1899 comment] the A540 read a name in the SETUP as "via Intern DECT" | default (`dect_base.c:1896-1910`) | eye | A |
| D-INT-03 | Internal "general call" (`*` after the internal-call key) rings every other handset | GAP §14.4.3 Table 131 (2AH) | [spec] the PT asks the FP to ring all others; our dial path forwards `*` as an internal number to the owner | n/a (not checked against our code) | hand | C |
| D-INT-04 | Internal call to the FP itself (terminal 0) | GAP §14.4.2, Table 131 | [spec] terminal 0 is the FP; there is nothing to ring on a base without handset | n/a | log | C |
| D-INT-05 | Service call (class 0BH, keypad 18H) to enter FT settings | GAP §8.20, §8.21 | [spec] optional (N.32); a handset that offers the menu has its SETUP released with "service not implemented" | default (refused, `dect_cc.c:1247`); code to serve it | hand, frames | C |
| D-INT-09 | Other call classes a handset may open: supplementary-service call (D), OA&M (E), message call (4), ULE (3), DECT/ISDN IIP (7) | NWK §7.6.4 call-class table | [spec] each is refused today; a handset that opens class D for a service menu would show a failure; the frames of any such SETUP are the cheapest way to learn which handset wants what | default (refused) | frames, log | C |
| D-INT-10 | Emergency call class (A) from a handset | NWK §7.6.4 | [spec] accepted today as an ordinary outgoing call; what the owner does with it is outside the library | n/a (not checked against our code) | frames | C |
| D-INT-06 | Handset locator: ring every idle handset with <<CALLING-PARTY-NAME>> presentation 11 "handset locator", no line id, call id and "CS call setup" | P3 §7.10.2 Table 80; NWK §7.7.53 | [spec] triggered by a button on the FP or in the controller; a locator-aware handset may ring even when silenced (note 2) | code (the presentation value exists in `dect_clip.h`, nothing sends it) | ear | B |
| D-INT-07 | Collective or group ringing by {LCE-REQUEST-PAGE} (connectionless group TPUI or broadcast identifier), then an individual SETUP | NWK §14.4; GAP §8.12.3 | [spec] the old "ring them all" path; we page each handset by itself; cross-area A | code | ear | C |
| D-INT-08 | Handset-to-handset audio codec priority and narrowband fall-back | P3 §7.4.3.9 | [spec] cross-area C; listed because the internal path differs from the external one | n/a (cross-area C) | ear | C |

#### Vendor escapes (proprietary content)

<<ESCAPE-TO-PROPRIETARY>> (7BH, NWK §7.7.45) can ride in almost every message (the tables list it as optional in both directions): a discriminator-type octet
(0 "unspecified", 1 "EMC" followed by the 2-octet manufacturer code), then the vendor's bytes. <<EVENTS-NOTIFICATION>> has its own proprietary form
(§7.7.55.5, discriminator = EMC) and list identifiers 80H up are proprietary (P3 §7.4.10.5.1.14).

| ID | Variation | Spec | Why | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| D-ESC-01 | Receive and ignore: the O201 sends `7B 02 80 04` in its SETUP, decoded as ESCAPE-TO-PROPRIETARY, length 2, discriminator type 0 "unspecified" (octet 3 = 80H, extension bit plus 0), one content octet 04H | NWK §7.7.45 | [obs: vendor-specific-support.md §7.7] "not needed" for the O201's incoming and outgoing calls; recorded so the framing is read as the spec's, not as an EMC | default (ignored) | frames | C |
| D-ESC-02 | Send an escape with an EMC discriminator to a handset of that maker | NWK §7.7.45 | [hyp] a maker's handset may light features on a base that identifies itself; unsourced; nothing to send until a frame shows what a stock base sends | code | frames | C |
| D-ESC-03 | A decoder that prints every escape, proprietary event and unknown IE of a handset with its EMC, for the Command Finder bundle | NWK §7.7.45, §7.7.55.5; `vendor-specific-support.md` §7.2 | [spec] the only way to learn what a vendor does without guessing; belongs to the tool, not to the stack | code | frames | A |
| D-ESC-04 | <<MODEL-IDENTIFIER>> as the handset's identity in the bundle | NWK §7.7.46 | [obs: vendor-specific-support.md §7] the O201 sends none; the Gigasets and others may | default (diagnostic) | frames | B |
| D-ESC-05 | Proprietary list identifiers and fields (a vendor's own list, two-octet subtype) | NWK §7.7.55.3 note 3; P3 §7.4.10.5.1.14 | [spec] only visible in a list-access trace | code | frames | C |

#### Old and unusual procedures

These are in the NWK specification and in the GAP feature list but no current handset is known to need them. Each is a possible explanation for a
handset that behaves oddly, and for a very old one.

| ID | Variation | Spec | Why | Ours today | Check | Rank |
|---|---|---|---|---|---|---|
| D-OLD-01 | {CC-NOTIFY} with <<TIMER-RESTART>> (E5H) to restart or stop a handset's setup or completion timer while a slow call is set up | NWK §6.3.2.13, §7.6.9, §9.3.1.2 (P<CC.03> "may be restarted by the FT"), §9.3.1.9 (P<CC.04>, optional); Annex A.2 (CC.03 20 s, CC.04 100 s) | [spec] a handset with the optional 100-second completion timer releases a call that rings for longer unless we restart it | code | log, frames | B |
| D-OLD-02 | {CLMS-VARIABLE} (PD 6, id 01H): calling number, name, time-date and text to a handset outside a call, addressed by portable identity | NWK Table 37, §12 | [spec] the old "caller ID without a call" and message route; the 7 to 20 octet portable identity is mandatory | code | frames, eye | C |
| D-OLD-03 | {CLMS-FIXED} (B-format, up to 160 bits) broadcast data and group paging | NWK §5.5.1, §6.4.3 | [spec] chip-level broadcast; the base name rides in it (D-FPN-01) | chip | eye | C |
| D-OLD-04 | {COMS-*} connection-oriented message service ({COMS-SETUP}, {COMS-INFO}, {COMS-ACK}) | NWK §6.2.3 Table 3 | [spec] a transport for data and messages; no voice handset is known to use it | n/a | frames | C |
| D-OLD-05 | <<ALPHANUMERIC>> (76H) with 4-bit or 8-bit character sets (DECT, IA5, ASCII, ERMES) | NWK §7.7.3; Annex D.3 | [spec] carries messages in CLMS and COMS; "shall not be used to carry dialling information" | n/a | frames | C |
| D-OLD-06 | <<TEST-HOOK-CONTROL>> (E6H): the FP switches the handset's hook for test purposes | NWK §7.6.10; Table 8 (F to P only) | [spec] test-only; a conformance rig sends it, nothing else should | n/a | frames | C |
| D-OLD-07 | <<IWU-TO-IWU>> and <<IWU-PACKET>> transparent elements, MMS headers in {IWU-INFO} | NWK Table 20, §7.7.22, §7.7.23, §7.7.47-.49 | [spec] the transport for list access (IWU-TO-IWU, which we use) and for F-MMS (PI_6 bit 3); the rest is untouched | default (IWU-TO-IWU only) | frames | C |
| D-OLD-08 | 4-bit DECT characters (digits and space) for numeric displays | NWK Annex D.3, §7.7.3 | [spec] the old compact numeric text; no path sends it | n/a | eye | C |
| D-OLD-09 | Software update over the air: a SETUP whose basic-service nibble is 1001B or 1010B ("Light data services: SUOTA", Class 4 or 3 DPRS management), and the "software upgrade indication: firmware" event | NWK §7.6.4 basic-service table, §7.7.55.3; TS 102 527-4 not in our set | [spec] the text does not say which call class goes with the SUOTA service nibble; classes 3, 4, 7, B, D and E are refused today, the others are processed (the dialled-number path then expects speech); a handset's update client dialling the base is unsourced and untested | default (partly refused, `dect_cc.c:1247`); code | frames | C |
| D-OLD-10 | Call-related cost information (<<FEATURE-INDICATE>> 60H with tariff or pulses), queue management, subscriber-number indication, echo control | NWK §10.6.2, §7.7.16, §7.7.17 | [spec] the DECT-specific services of the 1990s; tariff display on a handset | code | eye | C |
| D-OLD-11 | A GAP handset that never sent <<TERMINAL-CAPABILITY>> sees the base as "full slot only" and no display | GAP Table 29 | [spec] the defaults decide what we may send; see D-CAPS-07 | default | frames | C |

---

### Spec text I could not find or that is ambiguous

1. **GAP and Part 1 have no procedure for date and time or for the message-waiting lamp** (searched `en_300444` and `ts_10252701` for time-date, MWI,
   "message waiting": no hit). Both are Part 3 features (P3 §7.4.1.2, §7.4.2). A Part 1 handset (C430A, AS690H) is therefore outside the standard
   on both; if it shows either, the transport is a vendor one. D-TIME-08 and D-EVT-03 are guesses at where to look.
2. **<<CALLED-PARTY-NAME>> (NWK §7.7.52) says its service is defined in Part 3, but Part 3 V1.7.1 does not use it** (searched). D-NAME-10 has no procedure to
   follow.
3. **Part 1 §7.3.6 and GAP §8.18 disagree on the call class of an internal call to a GAP handset** (external class 8 against internal class 9). Not a
   conflict between versions: Part 1 says "for backward compatibility reasons" and a New Generation FP, GAP table 30 predates it. Which handsets obey which is open (D-INT-01).
4. **docs/handset-profiles.md cites EN 300 175-5 §7.7.30 for <<MODEL-IDENTIFIER>>.** In V2.9.1 §7.7.30 is <<PORTABLE-IDENTITY>>; the model identifier is §7.7.46 (Table 76: 78H). The
   key `match.model` and its description should cite §7.7.46.
5. **The framing in `profiles/gigaset-c430a.profile`** (`02 0a 1a 91 02 02 {len} {text} 0d`) contains `91 02 02`, which are not DECT control codes (Table D.1 stops at 1CH);
   codes from 80H up are "extended" and need an ISO 2022 escape (D.2.4). Whether `91` is part of such an escape or a vendor code is not in the standard. The profile itself says "unconfirmed".
6. **NWK §6.3.2.2 Table 8 and GAP §8.16 allow <<MULTI-DISPLAY>> in any F-to-P CC message,** but nothing says which message a handset actually draws it from; D-DISP-06
   and -07 cannot be settled from the text.
7. **The number of display characters a name may have is not limited by the standard** (CALLING-PARTY-NAME has a length of contents only); the handset's geometry is in
   <<TERMINAL-CAPABILITY>> octets 3f and 3g, which we do not read (D-NAME-07, D-CAPS-02).
8. **P3 Table 73 says tones go "by SIGNAL (method 1) or the network's in-band tone (3)" for a Part 3 PP in front of an external call and does not say how an FP
   picks between them.** Ring-back on a SIP call is in band from the far end only when early media arrives (cross-area B and C).
9. **Which control code, if any, a handset sends for the R key.** GAP lists 15H (register recall); Part 3 lists `1C 15` (a parallel external call); nothing says what a Part 1
   handset sends in a call (D-KEY-06).
10. **Part 3 §7.4.5.2.4.1 (FP managed line selection from a Part 3 PP) has a heading and no body in the text as extracted;** the rule is in §7.4.5.2.2.
11. **The O201's escape element `7B 02 80 04`:** the standard says what the framing means (D-ESC-01) but not what the 04H means; only the handset maker knows.

### Cross-area dependencies

- **A (identity, registration, security, paging, link):** the FP-name broadcast and the Qh=3/4/11 capability bits (a24, a29 "Part 3", a30-a34 CLIR, conference, intrusion,
  deflection, multiple lines) are set by the chip or its mails and decide how a Part 3 handset treats us (P3 §7.4.9.2.3 Table 37, §7.4.6.4.1 backward compatibility).
  Terminal-capability exchange (D-CAPS) happens in registration and location registration. CLSS paging toward a handset without a link (MWI, missed calls, time, list-change
  notices) is a link procedure (D-EVT-02, D-TIME-02). The terminal identity number comes from the assigned TPUI (D-FPN-02). Collective ringing needs {LCE-REQUEST-PAGE} variants (D-INT-07).
  CALL-IDENTITY for ciphering (D-CAPS-10). The list-change notices that upset the A540 (D-EVT-06) also sit on the location-registration path.
- **B (call control):** placement and timing of display text, SIGNAL, CALL-INFORMATION and PROGRESS-INDICATOR relative to SETUP, SETUP-ACK, CALL-PROC, ALERTING and CONNECT
  (D-DISP-06, -07, D-TONE-03, -09, D-CSTAT-05, D-CINFO-01, -03); the internal-call basic service (D-INT-01); the 3-second dial pause and en-bloc dialling (D-KEY-01, -02); timer
  restarts in {CC-NOTIFY} (D-OLD-01); call release reasons that decide what a handset shows after the call.
- **C (audio, codec, U-plane):** every tone that is "in band" (D-TONE-06, -07, -08, -10), early media and ring-back, the O201's wait for PROGRESS-INDICATOR before it plays audio
  (D-KEY-10, D-TONE-09), in-band DTMF feedback (D-KEY-09), and the codec choice for internal calls (D-INT-08).

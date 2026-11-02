# libopendectcell API versions

`OPENDECTCELL_API_VERSION` in `src/libopendectcell/dect_proto.h` is a single integer,
bumped on any change to a public function signature or a public struct
layout: not on behaviour changes and not on comments. It tells a consumer
the API shape changed, not that behaviour changed underneath a stable
signature. This is the record of each version; entries name a behaviour
change where one came with the bump.

**1.** first versioned API. Adds dect_lapc_next_deadline(); dect_mm_cfg gains a
configurable subscription slot and TPUI.

**2.** adds dect_cc_tick() and dect_cc_next_deadline(); struct dect_cc gains
setup_deadline_ms and struct dect_cc_stats gains setup_timeouts, both of
which change a public struct layout.

**3.** adds dect_iwu.h (the <<IWU-TO-IWU>> session responder: dect_iwu_init(),
dect_iwu_set_open_params(), dect_iwu_reply()); struct dect_cc gains an
embedded struct dect_iwu, which changes its layout. {CC-IWU-INFO} is now
answered instead of dropped -- a behaviour change this number does not
capture, which is exactly the limitation noted above.

**4.** adds dect_link.h -- the per-connection C-plane stack (openDECTcell#67):
dect_link_init(), dect_link_rx_segment(), dect_link_tick(),
dect_link_drain(), dect_link_poll_append_mail(), struct dect_link and
struct dect_link_ind. Purely additive: no existing signature or struct
layout changed, so a consumer that does not use it needs no edit --
but it SHOULD, because the glue it replaces is protocol behaviour that
two hand-kept copies would drift on.

**5.** struct dect_cc grows `peer_basic_service` (the <<BASIC-SERVICE>> octet of
the peer's own {CC-SETUP}, DECT_CC_SVC_NONE when it sent none), and two
functions arrive: dect_cc_peer_is_speech() and dect_nwk_find_fixed_ie().
The struct layout change is why this is a bump rather than an addition.

CONSUMERS MUST ACT. Not every call a handset places is a speech call: a
CAT-iq handset opens list-access calls to read the phonebook, and in our
corpus one handset placed 145 of those against 2 voice calls. Anything
that arms an audio path, allocates a media resource or reports "call
connected" on an incoming {CC-SETUP} must ask dect_cc_peer_is_speech()
first. BOTH nibbles have to agree -- a list-access call carries call
class 0010 with basic service 1000 "wideband speech", so testing the
service nibble alone calls a phonebook read a wideband voice call.

**6.** GAP N.4, dialled digits -- the one mandatory GAP feature this project
was missing (EN 300 444 table 1; audit in docs/gap-conformance.md).
{CC-INFO} is now dispatched, <<MULTI-KEYPAD>> (0x2C) is collected into
`struct dect_cc`, and `struct dect_link_ind` grows a `keypad` bit.
New: dect_cc_keypad(), dect_cc_clear_keypad(), DECT_IE_MULTI_KEYPAD,
DECT_CC_KEYPAD_MAX. Two struct layouts changed, hence the bump.

CONSUMERS: a handset dialling on the base sends its digits in one or
several {CC-INFO} messages, and before this they were silently dropped
-- seven runs in our capture corpus hold a handset sending "12345" past
a base that ignored it. Read them with dect_cc_keypad() when
`ind.keypad` is set. They are RAW and lossless: GAP's basic set is 0-9,
* and #, but control codes (pause, register recall, go-to-pulse) may
appear, and deciding what to do with those is policy.

**7.** <MM_ident.1>, the temporary-identity assignment timer. `struct dect_mm`
grows the deadline and three flags, `struct dect_mm_stats` two counters,
and dect_mm_tick() / dect_mm_next_deadline() / dect_mm_tpui_confirmed()
arrive. dect_link_tick() runs the MM timer, so a consumer already
calling it needs no change.

CONSUMERS: a TPUI in {LOCATE-ACCEPT} starts an assignment procedure the
handset must acknowledge, and we page BY that TPUI. Check
dect_mm_tpui_confirmed() before treating a handset as reachable: an
unacknowledged assignment means pages may go to an identity it is not
listening for, which fails silently.

**8.** date and time towards the handset. New `dect_time.h`
(<<TIME-DATE>> 0x23, the CISS {FACILITY} 0x62 that carries it,
struct dect_time, dect_time_valid/put/build_facility), `struct
dect_link` grows a one-deep CISS slot, and dect_link_send_time()
arrives.

CONSUMERS: TS 102 527-3 §7.4.20.2 makes it the FT's duty to send the
date and time after every successful location registration --
unconditionally, "even if the PT does not indicate support" and
"independently of the 'Clock master' setting". A base is where a
handset gets its clock; without this every timestamp it shows, call
lists above all, is invented. The library has no clock, so the caller
supplies the time and decides whether its own is trustworthy.

**9.** CAT-iq list access served for real, and {CC-SETUP} collision handling.
dect_iwu.h is rewritten: the byte-echo responder (dect_iwu_reply,
dect_iwu_set_open_params, DECT_IWU_TAG/REQ_06/...) is GONE, replaced by
a TS 102 527-3 7.4.10 responder -- dect_iwu_handle() filling a struct
dect_iwu_out, plus dect_iwu_add_handset/set_line/set_own/
reset_sessions. struct dect_iwu is entirely new, so struct dect_cc
(which embeds it) changes layout; dect_cc also gains setup_msg[] and
the represent/collision fields, and DECT_CC_TXQ is now 16.

CONSUMERS MUST ACT. A CAT-iq handset will not treat itself as
synchronised -- and a Gigaset refuses incoming calls -- until its list
sync completes, which needs the Internal Names and Line Settings lists
served with real data. The owner supplies the data via
dect_iwu_add_handset()/dect_iwu_set_line() on each link's cc.iwu;
nothing is served if nothing is set. A list-access service call (call
class 0010) is now CALL-PROC'd only, never connected, so anything
keyed on "a call reached ACTIVE" is unaffected but anything counting
CONNECTs will see fewer.

**10.** the fixes from two external reviews (2026-10-24). Layout changes:
struct dect_mm gains pend_id/pend_id_len and dect_mm_stats gains
tpui_unsolicited; struct dect_lapc gains rx_discard and
dect_lapc_stats gains rx_bad_sapi and rx_truncated; struct
dect_link_ind gains page_id/page_id_len. New macros:
DECT_MM_REJECT_IE_ERROR, DECT_LCE_PAGE_RESPONSE,
DECT_SIGNAL_ALERT_PATTERN1.

CONSUMERS MUST ACT on three behaviour changes. (a) page_response is now
set only for {LCE-PAGE-RESPONSE} with its identity, and the caller must
compare page_id with the handset it paged before offering the call.
(b) A {LOCATE-REQUEST} is rejected unless its identity matches
mm.port_id, so arm port_id from your subscription store on every link,
as dect-stubd does. (c) Every FP-originated {CC-SETUP} now carries
<<SIGNAL>> 0x41 unless dect_cc_set_signal() says otherwise.
Also: the transaction value of an FP-originated call is 0 (the lowest
free), CC messages are matched to their transaction, and the API 9 note
above is wrong that a Gigaset refuses incoming calls until its list
sync completes -- the refusal was the transaction value.

**11.** the CC identity gate (2026-10-24). Layout changes: struct dect_cc
gains gate_armed/gate_id/gate_id_len and dect_cc_stats gains
rejected_identity. New: dect_cc_set_gate(),
DECT_CC_REASON_MANDATORY_IE_MISSING, DECT_CC_REASON_UNKNOWN_IDENTITY.

CONSUMERS MUST ACT. dect_link now accepts a handset's {CC-SETUP} only
if its <<PORTABLE-IDENTITY>> equals mm.port_id, and a link with no
port_id takes no call at all; the others get {CC-RELEASE-COM} "unknown
identity" (EN 300 444 table 17). API 10 already required arming
port_id for location registration; a consumer that does not arm it now
loses handset-placed calls and list access too. Treat a rise in
rejected_identity as a handset you do not hold, not as a fault.

**12.** a base holds more than one handset (2026-10-25). Layout changes:
struct dect_mm gains held[]/n_held/adopted (struct dect_mm_held, new).
New: dect_mm_hold(), dect_mm_adopt(), DECT_MM_HELD_MAX.

Behaviour: {LOCATE-REQUEST} and a PT {CC-SETUP} are accepted from any
held handset, and the link adopts it -- mm.port_id, the slot and the
TPUI become that handset's, and mm.adopted is the tag you gave it. A
consumer that holds nothing behaves exactly as under API 11. To serve
several handsets, dect_mm_hold() each after every dect_link_init(),
and read mm.adopted to know which one a link is serving.

**13.** {LCE-PAGE-REJECT} (2026-10-25). Layout changes: struct dect_link
gains lce/lce_len, struct dect_link_ind gains page_ti. New:
dect_link_page_reject(), DECT_LCE_PAGE_REJECT. Nothing changes until
you call it: on a page response whose page_id is not the handset you
paged, queue the reject and release the link (TS 102 527-1
§7.3.8.3.1), as dect-stubd does.

**14.** codec choice for handset-placed calls (2026-10-25). Layout changes:
struct dect_cc gains peer_negotiates/peer_g726/peer_has_g726/
chosen_codec, dect_cc_stats gains codec_unmatched.

Behaviour: when a handset's {CC-SETUP} negotiates (a <<CODEC-LIST>>,
or the wideband basic service), our {CC-CONNECT} now carries
<<CODEC-LIST>> with G.726 alone (TS 102 527-1 §7.3.3) -- the codec the
narrowband audio path carries. A basic-speech SETUP without a list is
answered exactly as before. Read chosen_codec before binding audio.

**15.** dect_cc_set_busy() (2026-10-25). Layout changes: struct dect_cc gains
busy, dect_cc_stats gains rejected_busy. Nothing changes until you
call it: a busy CC refuses every handset-placed call but list access
with "insufficient resources" (EN 300 175-5 §17.3.3). For links with
no audio path behind them.

**16.** diagnostics as data (2026-10-25). New module dect_diag.[ch]: struct
dect_diag_link (one per link, the protocol half from
dect_diag_from_link()), dect_diag_decode_status() for the chip's
0xF625 row (carrier, time slot, ten RSSI values, eight integrity
counters, frames since establishment), dect_diag_frames_update() for
the 16-bit wrap, and dect_diag_json(). Layout change: struct dect_mm
gains peer_model/peer_codecs, what the handset declares about itself
in {LOCATE-REQUEST} or {ACCESS-RIGHTS-REQUEST}. Nothing else changes.

**17.** registration policy (2026-10-25). Layout change: struct dect_mm_cfg
gains registration (enum dect_mm_registration: OPEN, the default and
the old behaviour; CLOSED; SAME, only the held port_id re-enrolling).
Refusals answer {ACCESS-RIGHTS-REJECT} "IPUI not accepted" 0x06. Set
CLOSED on any link that cannot keep a subscription, or the handset
shows "registered" with a key you never stored.

**18.** deferred location registration, for roaming (2026-10-25). Layout
changes: dect_mm_cfg gains defer_unknown; struct dect_mm gains the
held request (locate_pending, _ti, _id, _la); dect_mm_stats gains
locate_deferred and locate_retrans. New: dect_mm_locate_pending(),
dect_mm_locate_resume(). With defer_unknown set, a {LOCATE-REQUEST}
from an identity the link does not hold is held instead of rejected:
fetch the record, dect_mm_hold() it, then resume. The PT waits
<MM_locate.1> = 20 s and retransmits once.

**19.** a held CC-SETUP, for roamers that do not locate (2026-10-25).
Layout change: struct dect_link gains setup/setup_len/setup_id/
setup_id_len. New: dect_link_setup_pending(), dect_link_setup_resume().
With mm.cfg.defer_unknown, a {CC-SETUP} from an IPUI the link cannot
adopt is held instead of turned away: a handset moving within one
location area calls without locating (multi-cell test M4).

**20.** location validity (2026-10-25). Layout change: dect_mm_cfg gains
locate_validity. Non-zero adds <<DURATION>> to {LOCATE-ACCEPT}, so
the PT locates again after that many 40.96 s units.

**21.** dect_dsaa_pin_to_ac() returns int (2026-10-25): -1 for a PIN with a
non-digit or more than two digits per AC octet (8 for GAP's 32-bit
AC). It used to write in front of the buffer for 9 or more digits.
Validate user PINs with it.

**22.** wideband codec choice (2026-10-25). Layout change: struct dect_cc
gains peer_g722, peer_has_g722, wideband. New: dect_cc_set_wideband()
(CONNECT chooses G.722 when offered; off by default),
dect_cc_codec() (the call's codec, including the one the handset
names in ALERTING/CONNECT for a call we place), DECT_CC_CODEC_G722/
G726. Bench tests 10-11: the coded G.722 path works on 07.56.

**23.** overlap receiving (2026-10-25). Layout change: struct dect_cc gains
overlap, overlap_deadline_ms; dect_cc_stats gains overlap_timeouts;
enum dect_cc_state gains DECT_CC_ST_OVERLAP (appended). New:
dect_cc_set_overlap(), dect_cc_accept(), DECT_IE_SENDING_COMPLETE;
dect_nwk_find_fixed_ie() now also finds single-octet elements
(returns 0). **Act on it:** switch statements over dect_cc_state
need the new state; with overlap on you must call dect_cc_accept()
when dialling is complete, or F<CC.01> releases the call at 20 s.

**24.** a held CC-SETUP is answered at once (2026-10-25). Layout change:
struct dect_link gains setup_ti, setup_answered, held[]/held_len[]/
held_n/held_dropped; struct dect_cc gains answered. New:
dect_cc_note_answered(). The link sends {CC-SETUP-ACK} (overlap,
speech) or {CC-CALL-PROC} when it holds a roamer's SETUP, holds what
the handset sends on that call (up to DECT_LINK_HELD), and replays it
on dect_link_setup_resume(); a release from the handset ends the hold
with {CC-RELEASE-COM}. Answering stops the PT's 20 s P<CC.03>, so the
fetch budget is no longer 20 s for a call (bench test 7: "---").

**25.** a handset-placed call in steps (2026-10-25). Layout change: struct
dect_cc gains proceeded, alerted. New: dect_cc_proceed(),
dect_cc_alert(), dect_cc_connect() (dect_cc_accept() is the last).
**Behaviour change:** our {CC-CONNECT} enters ACTIVE (EN 300 175-5
§9.3.1.8); CONNECT PENDING is no longer used for these calls, and
stats.connected counts on CONNECT. The codec is chosen when the
SETUP arrives, so dect_cc_codec() can drive a bind before CONNECT.

**26.** call status and FRITZ!OS's link numbers (2026-10-25). Layout
change: struct dect_cc gains call_status, status_call_id;
struct dect_diag_status gains has_long, connect_frames, uptime. New:
dect_cc_set_call_status() -- <<CALL-INFORMATION>> "CS call connect"
in our CONNECT to a handset's call, which TS 102 527-3 §7.4.6.4.3
requires and the handset's call timer keys on (A-0122) -- and
dect_cc_put_call_status(); dect_diag_rssi_dbm10(), dect_diag_quality()
(A-0121). **Renamed:** the eight event counters are now acrc, xcrc,
zcrc, qbit1, qbit2, handover, slideerr, syncerr (A-0123), also as
JSON keys; the JSON radio object gains "dbm" and "quality".

**27.** registration PIN length (2026-10-25). No layout change. New:
dect_dsaa_pin_check() and DECT_PIN_DIGITS_MIN/DEFAULT/MAX (4/8/8).
**Behaviour change:** dect_mm_cfg.pin no longer defaults to "0000"
when empty; a PIN that is not 4 to 8 digits refuses every
{ACCESS-RIGHTS-REQUEST} ("IPUI not accepted") before KEY-ALLOCATE.

**28.** the internal-call key (2026-10-28). No layout change. **Behaviour
change:** a handset-placed call of class 2 (list access) or 8 whose
keypad starts with 0x17 is re-classed as class 9, keeping its speech
service (EN 300 444 §8.19, TS 102 527-3 §7.4.10.6.2), so
peer_basic_service can change after the SETUP.

**29.** a second call on one link (2026-10-28). No layout change.
**Behaviour change:** a {CC-SETUP} is accepted in DECT_CC_ST_RELEASED
as in NULL (EN 300 175-5 §9.5.1: release ends in the null state), and
the keypad buffer is cleared for each new call. Before, the next call
on a link that stayed up was ignored (same transaction value) or
refused "insufficient resources" (another value).

**30.** overlap timeout (2026-10-28). No layout change. **Behaviour change:**
F<CC.01> expiry sends {CC-RELEASE} "timer expiry" and enters
DECT_CC_ST_RELEASED (release pending), the normal release of GAP
§8.3.2.3, instead of {CC-RELEASE-COM} and NULL.

**31.** dect_cc_info() (2026-10-28). No layout change. New: a {CC-INFO} with
caller-supplied IEs on the call in progress, for information the FT
adds after the SETUP (EN 300 444 §8.41/8.42, <<MULTI-DISPLAY>>).

**32.** dialling from a list (2026-10-28). Layout change: struct dect_cc
gains list_number. **Behaviour change:** "1C15H + number" in the
{CC-INFO} of a list-access call (class 2) re-classes it as a normal
call (class 8, speech service kept), drops the prefix from the keypad
and sets list_number: the number is complete (TS 102 527-3
§7.4.10.6.2). Before, the call hung in PROCEEDING.

**33.** LAPC N250 (2026-10-28). Layout change: struct dect_lapc gains
retry_count and failed, struct dect_lapc_stats gains link_failures.
**Behaviour change:** after three unanswered retransmissions LAPC
sends DISC and sets failed (EN 300 175-4 §9.2.3.6) instead of
retransmitting for ever.

**34.** terminal capability (2026-10-28). Layout change: struct dect_mm
gains peer_cap. New: struct dect_term_cap, dect_term_cap_parse(),
DECT_IE_TERMINAL_CAPABILITY; the MM entity keeps the handset's
<<TERMINAL-CAPABILITY>> from {LOCATE-REQUEST} and
{ACCESS-RIGHTS-REQUEST}, with the NG-DECT Part 1 / Part 3 bits.

**35.** call status per handset (2026-10-28). Layout change: struct dect_link
gains call_status_for and call_status_ctx, an optional owner hook
asked when a handset's {CC-SETUP} arrives, before CC answers it: 1
call status, 0 none, -1 keep (TS 102 527-3 §7.4.6.4.1).

**36.** a failed key allocation (2026-10-28). No layout change. New:
DECT_MM_AUTHENTICATION_REJECT. **Behaviour change:** a wrong RES1 is
answered with {AUTHENTICATION-REJECT} (EN 300 175-5 §13.6, EN 300 444
§8.32.2.4), then {ACCESS-RIGHTS-REJECT} as before.

**37.** SETUP validation (2026-10-28). Layout change: struct dect_cc_stats
gains ie_missing. **Behaviour change:** a {CC-SETUP} without
<<BASIC-SERVICE>> gets {CC-RELEASE-COM} "mandatory information element
missing" (EN 300 175-5 §17.6.1) instead of being connected.

**38.** our own location area (2026-10-28). No layout change. **Behaviour
change:** with a class A RFPI, {LOCATE-ACCEPT} always carries our
<<LOCATION-AREA>> (LI-type 1, LAL 36: 0x64, stock's value, A-0127),
instead of echoing the handset's element or sending none.

**39.** the line pool (2026-10-28, docs/lines-and-calls.md step 1). Layout
change: struct dect_iwu drops line_id and line_name for lines[] and
n_lines (struct dect_iwu_line: id, name, attached bitmap or all
handsets). New: dect_iwu_add_line(), dect_iwu_clear_lines(),
dect_iwu_line(). The Line Settings List serves every line, sorted by
line id, each with its own attached handsets. dect_iwu_set_line()
still makes one line with every handset attached.

**40.** the handset's line choice (2026-10-28, lines-and-calls.md step 2).
Layout change: struct dect_cc gains peer_line, line_confirmed and the
default line. New: dect_callinfo_parse(), dect_cc_set_default_line(),
dect_cc_line(), DECT_CC_LINE_UNSET; dect_cc_put_call_status() takes a
non-const dect_cc. **Behaviour change:** with a default line set, the
first call-status answer to an external handset-placed call names the
selected line (the handset's, or ours for 'None') and its line type
(TS 102 527-3 §7.4.5.2.1, §7.4.5.2.4).

**41.** call ids per call (2026-10-28, lines-and-calls.md step 3). Layout
change: struct dect_link gains call_id_for, an optional owner hook
asked for the call id of a handset's new call at its {CC-SETUP},
before CC answers (TS 102 527-3 §7.4.6.1: unique system-wide).

**42.** call status reason (2026-10-28). Layout change: struct dect_callinfo
(and so struct dect_cc) gains has_status_reason and status_reason.
New: enum dect_callinfo_reason, DECT_CALLINFO_SUB_STATUS_REASON;
encoded after the call status, parsed, and valid only with one
(EN 300 175-5 §7.7.56), for "system busy" (TS 102 527-3 §7.4.8.3) and
the negative acknowledgements of call waiting (§7.4.3.4).

**43.** busy system (2026-10-28). Layout change: struct dect_cc gains
busy_notified and busy_deadline_ms. **Behaviour change:** a busy CC
entity (dect_cc_set_busy) with call status on (a Part 3 handset)
answers a handset's SETUP with CALL-PROC and the TS 102 527-3 §7.4.8.3
notification (busy tone, CS call disconnecting, "system busy"), then
after 5 s tones off, CS idle and {CC-RELEASE}; such a call is never
connectable. Without call status the plain {CC-RELEASE-COM}
"insufficient resources" stays.

**44.** unattached lines (2026-10-28, lines-and-calls.md step 2's rest).
Layout change: struct dect_cc gains line_refused, the stats
line_refused. New: dect_iwu_attached(). **Behaviour change:** a
handset-placed call on a line the handset is not attached to gets
{CC-RELEASE-COM} "incompatible service" (TS 102 527-3 §7.4.5.2.1;
the reason is ours, the clause names none).

**45.** <CC.02> (2026-10-28). Layout change: struct dect_cc gains
release_armed and release_deadline_ms, the stats release_timeouts.
**Behaviour change:** our {CC-RELEASE} unanswered for 36 s is
completed by our own {CC-RELEASE-COM} "timer expiry" and NULL
(EN 300 175-5 §9.5.1, A.1).

**46.** a page response without identity (2026-10-28). No layout change.
**Behaviour change:** an {LCE-PAGE-RESPONSE} without its mandatory
<<PORTABLE-IDENTITY>> is cleared with {LCE-PAGE-REJECT} when the link
knows the handset's IPUI (EN 300 175-5 §17.6.5, table 65), and still
ignored when it does not.

**47.** SETUP flag (2026-10-28, spec sweep C8). No layout change.
**Behaviour change:** a {CC-SETUP} with TI flag 1 is ignored (EN 300
175-5 §17.3.2.1); before, it opened a call or counted as a collision.

**48.** extended TV (2026-10-28, spec sweep E3). No layout change.
**Behaviour change:** dect_nwk_parse() fails for TV 7, so CC, MM and
LCE ignore such a message (EN 300 175-5 §17.3.1); before, octet 1a
was read as the message type.

**49.** release collision (2026-10-28, spec sweep C3). Layout change: the
stats gain release_collisions. **Behaviour change:** a peer
{CC-RELEASE} while ours is pending stops <CC.02> and enters NULL
without a {CC-RELEASE-COM} (EN 300 175-5 §9.5.3).

**50.** unknown transactions (2026-10-28, spec sweep E4, I3). No layout
change. **Behaviour change:** {CC-ALERTING}, {CC-CONNECT},
{CC-CONNECT-ACK}, {CC-INFO} and {CC-IWU-INFO} on a transaction we do
not hold are cleared with {CC-RELEASE-COM} "unknown transaction
identifier" (EN 300 175-5 §17.3.2.1), like every other message type
already was. Before, they were ignored, and an IWU-INFO was answered on
our own transaction.

**51.** the connectionless TV (2026-10-28). No layout change. **Behaviour
change:** CC disregards every message on TV 6, the connectionless
value (EN 300 175-5 §7.3, §10.4.2.3), where it cleared them as unknown
calls; dect_cc_set_next_ti() wraps past 5, so a call never takes 6.

**52.** LCE transaction identifier (2026-10-28, spec sweep LCE7). Layout
change: struct dect_link gains lce_bad_ti. **Behaviour change:** an
LCE message with F = 1 (§17.3.2.6) or TV other than 0 (§17.3.1, table
68) is ignored and counted: no page response reported, no
{LCE-PAGE-REJECT} sent.

**53.** MM transaction identifier (2026-10-28, spec sweep M13). Layout
change: the MM stats gain bad_ti. **Behaviour change:** an MM
message with TV other than 0 is ignored (§17.3.1, table 68), and an
{AUTHENTICATION-REQUEST} counts as the key-allocation answer only
with F = 1, on our transaction.

**54.** unserved call classes (2026-10-28, spec sweep N.32). Layout change:
the stats gain service_refused; new DECT_CC_REASON_SERVICE_NOT_
IMPLEMENTED. **Behaviour change:** a {CC-SETUP} for a ULE, message,
IIP, service, supplementary-service or OA&M call gets {CC-RELEASE-COM}
"service not implemented" (EN 300 444 §8.2.2.3) instead of being
connected without speech.

**55.** access rights without identity (2026-10-28, spec sweep M5). No
layout change. **Behaviour change:** an {ACCESS-RIGHTS-REQUEST}
without <<PORTABLE-IDENTITY>> (or one too long to hold) gets
{ACCESS-RIGHTS-REJECT} "information element error" (§17.6.4) instead
of key allocation and an accept naming nobody.

**56.** TPUI assignment refused (2026-10-28, spec sweep M9). Layout change:
struct dect_mm gains tpui_rejected, the stats tpui_rejected; new
DECT_MM_TEMP_IDENTITY_ASSIGN_REJ. **Behaviour change:** a
{TEMPORARY-IDENTITY-ASSIGN-REJ} ends a pending assignment as failed
and stops <MM_ident.1> (EN 300 444 §8.28.2.3, EN 300 175-5 §13.2.2).

**57.** <MM_key.1> (2026-10-28, spec sweep M3, N.12). Layout change:
struct dect_mm gains key_deadline_ms, the stats key_failed.
**Behaviour change:** a {KEY-ALLOCATE} unanswered for 10 s, or refused
with the PT's {AUTHENTICATION-REJECT}, fails the registration:
{ACCESS-RIGHTS-REJECT} "authentication failed" and DECT_MM_FAILED
(EN 300 444 §8.32.2.1, §8.32.2.3). dect_mm_next_deadline() includes
the timer.

**58.** <MM_auth.1> and CIPHER-REJECT (2026-10-28, spec sweep M11, M12).
Layout change: struct dect_mm gains auth_deadline_ms and
cipher_rejected, the stats auth_timeouts and cipher_rejected; new
DECT_MM_CIPHER_REJECT. **Behaviour change:** our authentication before
ciphering fails after 10 s unanswered, or at once on the PT's
{AUTHENTICATION-REJECT}; a {CIPHER-REJECT} is recorded and the call
kept (EN 300 444 §8.33.2.1). <MM_cipher.1> is not built: its stop
condition is DL_ENC_KEY-ind, a chip event we cannot see yet.

**59.** the proposed AUTH-TYPE (2026-10-28, spec sweep M6). Layout change:
struct dect_mm gains peer_auth and peer_auth_len, from the
{ACCESS-RIGHTS-REQUEST}'s <<AUTH-TYPE>>. No behaviour change; the
daemon logs it with the registration's outcome.

**60.** release reasons (2026-10-28, spec sweep P-8.18). No layout change.
New: dect_cc_release_reason(), dect_cc_busy_system(),
DECT_CC_REASON_NONE, _USER_UNKNOWN, _USER_BUSY. dect_cc_release() is
dect_cc_release_reason(cc, DECT_CC_REASON_NONE), unchanged on the air.
The daemon's internal-call refusals now carry a reason, and "no audio
channel" / "one at a time" are the busy system of TS 102 527-3
§7.4.8.3 for a Part 3 handset.

**61.** parallel-call control codes (2026-10-28, spec sweep D2, D3, C7).
Layout change: the stats gain control_codes and negative_acks.
**Behaviour change:** a {CC-INFO} with "1CH xx" (TS 102 527-3 table
17), or "17H" on a connected call, is a control code, never digits;
a Part 3 handset gets the negative acknowledgement (§7.4.3.4): the
call's status again, "control code not supported" (or CS idle and
"control code failed" for a call id not ours), tone 09H. 1C15H on a
list-access call and 17H before the call connects are unchanged.

**62.** codec change refused (2026-10-28, spec sweep B5). Layout change: the
stats gain service_rejected; new DECT_CC_SERVICE_CHANGE, _ACCEPT,
_REJECT. **Behaviour change:** a {CC-SERVICE-CHANGE} on our call gets
a bare {CC-SERVICE-REJECT} (TS 102 527-1 §7.3.4: accept or reject);
on a transaction we do not hold it is ignored (EN 300 175-5
§17.3.2.1), where it was cleared.

**63.** table 41 refusals (2026-10-28, spec sweep H3). No layout change.
**Behaviour change:** a search of the List of Supported Lists or the
Line Settings List, and a field query of the List of Supported Lists,
get the negative acknowledgement 'procedure not allowed' (TS 102 527-3
table 41 note 1): the search was 'not supported', the query answered.

**64.** Line Settings fields (2026-10-28, spec sweep H4, M1.1). No layout
change. **Behaviour change:** the Line Settings List serves fields
08H 'Multiple calls mode' (30H, single call) and 0BH-0DH call
forwarding (off, no codes, no number), read-only, and names them in
the field query (TS 102 527-3 §7.4.11.4.8-13, table 9).

**65.** DECT System Settings List (2026-10-28, spec sweep H21, H14). Layout
change: struct dect_iwu gains fw_version (default "openDECTcell").
**Behaviour change:** list 07H is served, read-only, and named in the
List of Supported Lists: current and new PIN as FF FF FF FF, clock
master FP, base reset yes, the firmware version, an empty EEPROM
version (TS 102 527-3 §7.4.11.3). Handsets that read list 00H now see
one more list.

**66.** renaming a handset (2026-10-28, spec sweep H20, H10). Layout change:
the IWU sessions gain a lock, struct dect_iwu the save buffer
(save_entry, save_len, save_overflow, save_data) and names_changed,
the stats saved; new DECT_IWU_SAVE_MAX. **Behaviour change:** "edit
entry" works on every list we serve but the List of Supported Lists
(it returns the entry and locks it); "save entry" on the Internal
Names List stores the name, the one editable field (reset with an
empty one), and sets names_changed for the owner; other fields, new
entries and saves without an edit are 'procedure not allowed'. The
name reads as editable, and the field query lists it first.

**67.** search (2026-10-28, spec sweep H12). No layout change. **Behaviour
change:** "search entries" on the Internal Names List finds by the
'Number' (IA5 decimal), with matching options 00H-02H, and delivers
entries like "read entries" (TS 102 527-3 §7.4.10.4.8); no match is
a confirm with counter 0. On the other lists we serve it is
'procedure not allowed' (table 41), the System Settings List included.

**68.** events notification (2026-10-28, spec sweep I1). Layout change:
struct dect_link's CISS slot becomes a queue (ciss[][], ciss_len[],
ciss_n, DECT_LINK_CISS_MAX 4). New: dect_events_build_facility(),
struct dect_event, DECT_IE_EVENTS_NOTIFICATION, DECT_EVENT_*;
dect_link_send_ciss(). No behaviour change on its own;
dect_link_send_time() now queues instead of refusing a second one.

**69.** FT authentication and access rights termination (2026-10-28, spec
sweep C21, N.20, N.26). Layout change: struct dect_mm gains uak_known,
term_state, term_retries, term_deadline_ms, the stats ft_auth and
ft_auth_refused; new enum dect_mm_term_state, dect_mm_set_uak(),
dect_mm_terminate(), DECT_MM_ACCESS_RIGHTS_TERMINATE_*. **Behaviour
change:** a PT-initiated {AUTHENTICATION-REQUEST} (F = 0) is answered
with RES2 = A22(A21(UAK, RS), RAND_P) and RS when the UAK is known,
else {AUTHENTICATION-REJECT}; it was ignored. dect_mm_terminate() runs
EN 300 444 §8.31 with <MM_access.2>.

**70.** call waiting builder (2026-10-28, spec sweep K2, D5, D10). Layout
change: the stats gain calls_waiting. New: dect_cc_call_waiting(), a
{CC-INFO} with SIGNAL 07H, the caller's identity and the waiting
call's <<CALL-INFORMATION>> (CS call setup), for an ACTIVE call with
call status. No behaviour change until the daemon uses it.

**71.** handset profiles (2026-10-28, docs/handset-profiles.md). New module
dect_profile: dect_profile_parse(), _resolve(), _defaults(),
_describe(), _display(), _version_cmp(); struct dect_profile and
struct dect_hs_profile. No behaviour change: nothing reads the
resolved values yet.

**72.** <MM_cipher.1> (2026-10-28, [CR A-0128]). Layout change: struct
dect_mm gains cipher_on and cipher_deadline_ms, the stats
cipher_timeouts; new dect_mm_cipher_on(). **Behaviour change:** 10 s
after our {CIPHER-REQUEST} without the cipher-on indication (the
chip's 0x0209) or a {CIPHER-REJECT}, the procedure is counted as
failed and the call kept (EN 300 444 §8.33.2.2).

**73.** deregistration (2026-10-28, maintainer's decisions). Layout change:
struct dect_mm gains term_by_pt, the stats term_by_pt; new
dect_iwu_remove_handset(). **Behaviour change:** a handset's own
{ACCESS-RIGHTS-TERMINATE-REQUEST} for the identity the link holds is
accepted at once, without authentication, and term_by_pt set;
another identity gets {ACCESS-RIGHTS-TERMINATE-REJECT} "IPUI
unknown". It was ignored.

**74.** authenticated self-deregistration (2026-10-28, maintainer). Layout
change: struct dect_mm gains term_auth and term_auth_deadline_ms, the
stats term_refused. **Behaviour change:** a handset's own
{ACCESS-RIGHTS-TERMINATE-REQUEST} is answered only after our
{AUTHENTICATION-REQUEST} with its UAK (EN 300 175-5 §13.5.2, annex
H.2): ACCEPT on the right RES, else REJECT "authentication failed"
(wrong RES, no UAK, its refusal, <MM_auth.1>). API 73 accepted it
unauthenticated.

**75.** wrong RES ends the call (2026-10-28, spec sweep N.9). Layout change:
the MM stats gain auth_mismatch. **Behaviour change** (daemon): when
our challenge before ciphering is answered with a wrong RES, the call
is released, reason "authentication failed" (EN 300 444 §8.23.2.3);
it went on unciphered.

**76.** deleting a handset from a list (2026-10-28, round 2 R3, H11/H27).
Layout change: struct dect_iwu gains pin, pin_known, pin_ok, ciphered
and delete_request, the handset entries in_call; new
dect_iwu_set_pin(), DECT_LIA_NACK_INCORRECT_PIN, _PIN_REQUIRED.
**Behaviour change:** the Internal Names 'Number' is PIN protected;
'Current PIN code' of the System Settings List is editable to enter
the base PIN, on a ciphered link only (7.4.11.1); "delete entry" on
the Internal Names List, with the PIN entered and the handset not in
a call, is confirmed and sets delete_request (a deregistration).

**77.** authenticated location registration (2026-10-28, task 36). Layout
change: struct dect_mm gains uak_lookup and uak_lookup_ctx, cfg
auth_locate, the stats locate_auth_ok and locate_auth_failed; term_auth
is renamed pt_auth (also for this); new dect_mm_set_uak_lookup(),
dect_mm_uak_lookup. **Behaviour change, only with cfg.auth_locate:** a
known handset's {LOCATE-REQUEST} is answered after our
{AUTHENTICATION-REQUEST}: LOCATE-ACCEPT on the right RES, else
LOCATE-REJECT "authentication failed".

**78.** a handset's clock (2026-10-28, task 27). Layout change: struct
dect_link_ind gains pp_time, pp_time_unknown and pp_time_value; new
dect_time_get(). **Behaviour change:** a CISS {FACILITY} with
<<TIME-DATE>> (TS 102 527-3 §7.4.2.2) is parsed and handed up, marked
unidentified unless it names the link's identity; it went to MM and
was dropped. dect-stubd logs it and keeps its own clock.

**79.** dect_ctl, the control socket's line format (parse, build;
docs/bridge-interface.md §2): control characters refused on input
and replaced on output, so a handset name cannot split a line.

**80.** dect_profile_keys_json() (the key table, with labels and groups, for
a settings page) and dect_profile_override() (a handset's own
settings on top of its profile).

**81.** dect_ipei_text(), a handset's IPEI as printed text (EN 300 175-6
annex C), from its PORTABLE-IDENTITY.

**82.** removed what no consumer called: dect_dsaa_b2(), dect_cs_trigger_mail(),
dect_alaw_decode_buf(), dect_cc_setup() (use dect_cc_setup_id() with
a NULL identity), dect_profile_version_cmp(), DECT_G722_RATE_HZ, the
legacy DECT_IWU_OPEN/CLOSE names, the unused STUB primitive macros,
and dect_lapc/cc/mm_next_deadline(). Made static: dect_lc_flen(),
dect_time_valid(), dect_clip_utf8_trunc(), dect_iwu_line(),
dect_term_cap_parse(), dect_diag_rssi_dbm10(), dect_diag_quality(),
dect_diag_call_state_name(), DECT_DIAG_NO_DBM. Layout: struct
dect_cc loses represent_max (DECT_CC_REPRESENT_MAX), struct
dect_link loses held_dropped, struct dect_link_ind loses
mm_message. New: DECT_CALL_CLASS_*, DECT_SERVICE_WIDEBAND and
dect_call_class(), dect_basic_service(), dect_basic_service_octet()
in dect_nwk.h.

**83.** Parallel calls and the voicemail indicator (2026-10-29). Layout
change: struct dect_cc gains `parallel`, `ctl_pending`, `ctl_code` and
`ctl_call_id`, and loses the state DECT_CC_ST_CONNECT_PENDING, which was
never entered (our {CC-CONNECT} to a handset-placed call goes straight to
ACTIVE, EN 300 175-5 §9.3.1.8), so the later states' values move down by
one. New: dect_cc_prefer_narrowband() (G.726 instead of G.722 when the far
end is narrowband and the handset offered G.726), dect_cc_set_parallel(),
dect_cc_take_control() and dect_cc_call_status() (TS 102 527-3 §7.4.3:
in parallel mode the call-control codes of table 17 are handed to the
owner instead of being refused, and the owner reports each call's status
by call id), DECT_CC_NO_REASON, DECT_CC_NO_SIGNAL, and DECT_EVENT_VOICE
in dect_time.h (§7.4.1.2 message waiting). **Behaviour change, only in
parallel mode:** a {CC-INFO} with a call-control code and the digits after
1C15 (a second external call) no longer reach the keypad.

**84.** dect_cell.h, the cell <-> controller wire protocol v1
(opendectcell-controller `docs/cell-protocol.md`), 2026-10-29: frames
(dect_cell_parse(), dect_cell_put(), dect_cell_body_max()), TLV bodies
(struct dect_cell_tlv and its writers, dect_cell_tlv_check(),
dect_cell_tlv_get(), dect_cell_tlv_next(), the fixed-width getters) and
the mail tunnel's layouts (struct dect_cell_mail, dect_cell_mail_up(),
dect_cell_mail_down(), dect_cell_mail_parse()). Purely additive.

**85.** dect_contacts.h, the Contact List store, 2026-10-30 (struct
dect_contacts and dect_contacts_init(), _at(), _index(), _fold_cmp(),
_put(), _del(), _clear(), _load(), _take_change(), _unlock(),
_find_number(); the contacts file and the change journal it reads and
writes). dect_iwu.h: struct dect_iwu gains `contacts` (NULL = no Contact
List, the behaviour before), `sessions[].locked` and `save_entry` widen to
uint16_t (Contact List entry identifiers go up to 16383),
DECT_IWU_OUT_MAX grows from 12 to 16 and DECT_IWU_SAVE_MAX from 128 to
256. **Behaviour change on every list:** a 'read entries' or 'search
entries' range too large for one reply is now a partial delivery (TS 102
527-3 §7.4.10.4.3.2, bit 8 of the counter) instead of a negative
acknowledgement 'invalid range'.

**86.** dect_calls.h, the call lists, 2026-10-30: struct dect_calls and
dect_calls_init(), _add(), _count(), _at(), _index(), _del(), _clear(),
_missed_unread(), _missed_total(). dect_iwu.h: struct dect_iwu gains
`calls` (NULL = no call lists, the behaviour before); with it the Missed
Calls List (01H), the Outgoing Calls List (02H) and the Incoming Accepted
Calls List (03H) are served, read status marks and edits included.
dect_iwu.c now uses dect_time_put(): link dect_time.c (and dect_nwk.c)
with it.


**87.** dect_calls.h, 2026-10-30: dect_calls_save() and dect_calls_load(),
the call lists as text (one tab-separated line per entry under a
`#opendectcell-calls 1` header). dect-stubd keeps them in /var/run/opendectcell/calls,
so a restart of the service keeps the lists; a reboot empties them.

**88.** dect_calls.h, 2026-10-30: DECT_CALLS_ALL, the All Calls List (04H)
as a view over the three stored lists, taken by dect_calls_count(),
_at(), _index(), _del() and _clear(); dect_calls_type() names the stored
list of an entry. dect_iwu.c serves the All Calls List (TS 102 527-3
7.4.10.5.6) where `calls` is set: the List of Supported Lists gains 04H.

**89.** dect_base.h, 2026-10-30: struct dect_hs_rec, a handset's record
(slot, UAK, the MM identity gate, the call identities), the first piece of
the base stack moving into the library (docs/stack-in-library.md, S1a).
dect-stubd keeps its records as it did, now in this type.

**90.** dect_base.h and dect_base.c, 2026-10-30: struct dect_base, the
table of handset records with its current index and the working copy,
and dect_base_init(), _stash(), _load(), _switch() (S1b). Link
dect_base.c.

**91.** dect_base.h, 2026-10-30: struct dect_base_ops with save(), and
dect_base_changed(): the stack says the current record changed, the host
stores it (S1c). struct dect_base gains `ops` and `ctx` (NULL: nothing is
stored).

**92.** dect_base.h, 2026-10-30: struct dect_base_ops gains send(),
random() and log(), with dect_base_send(), dect_base_random() and
dect_base_log() (S2.1). dect_base.h now includes dect_mail.h.

**93.** dect_base.h, 2026-10-30: struct dect_base_chip (the 0x0247 flag,
the 0x020A config byte, no 0x017C) in struct dect_base, and
dect_base_slot_clear(), _slot_install(), _slot_mask(): the chip's
subscription slots (S2.2). dect_base.h now includes dect_proto.h.

**94.** dect_base.h, 2026-10-30: struct dect_base_link (a connection's
handle, its dect_link, and the registration's progress on it) and
struct dect_base's `main_link` (S2.3a). dect_base.h now includes
dect_link.h.

**95.** dect_base.h, 2026-10-30: dect_base_link_open(), a connection's
C-plane stack set up for the current record, with the records the host
names held on it (S2.3b). dect_base.c now needs dect_link.c and what it
links.

**96.** dect_base.h, 2026-10-30: dect_base_main_segment(), a C-plane
segment on the main link with the challenges drawn and the adopted
record made current (S2.3c).

**97.** dect_base.h, 2026-10-30: dect_base_ops gains install(), and
dect_base_main_registered(): a registration completed on the main link
(new key and identity into the current record, the host's install and
save) (S2.3d).

**98.** dect_base.h, 2026-10-30: `slot0` (subslot=) and dect_base_slot(),
dect_base_side_open() and dect_base_link_segment() (S2.4: side links);
dect_base_main_segment() now uses dect_base_link_segment().

**99.** dect_mm.h, 2026-10-30: struct dect_mm_cfg gains `assign_id` and
`assign_id_len`, an identity to assign in {ACCESS-RIGHTS-ACCEPT} (an IPUI
O for bench B4a and class B); empty keeps the old behaviour.

**100.** dect_calls.h, 2026-10-30: DECT_CALLS_ALL_INCOMING, the All
Incoming Calls List (09H) as a view over the missed and accepted lists.
dect_iwu.c serves it (TS 102 527-3 7.4.10.5.11): the List of Supported
Lists gains 09H; an accepted call reads 'read', one call, and its 'Read
status' is not editable.

**101.** dect_base.h, 2026-10-30: dect_base_uak(), dect_base_cipher_start()
and dect_base_chip_key() (0x0208, the call key): a call's ciphering (S3a).

**102.** dect_base.h, 2026-10-30: struct dect_base_link gains the call
state both kinds of link keep: `dial_len`, `dial_change_ms`, `ic_tried`,
`cipher_done`, `auth_refused` (S3b).

**103.** dect_base.h, 2026-10-30: struct dect_base_call and struct
dect_base's `calls[DECT_BASE_CALLS]`, the external calls the bridge
carries (was dect-stubd's struct bcall), with DECT_BASE_PARTY_PAGING and
DECT_BASE_PARTY_NONE (S3c). dect_base.h now includes dect_clip.h.

**104.** dect_base.h, 2026-10-30: the call table's lookups:
dect_base_call_by_id(), _call_on(), _call_new(), _calls_on(),
_call_active(), _call_by_dect() (S3d).

**105.** dect_base.h, 2026-10-30: struct dect_base holds the side links
(`side[DECT_BASE_SIDES]`, dect_base_link gains `adopted`), dect_base_ops
gains event(), and dect_base_party_slot(), _call_slot(), _call_event()
(S3e). dect_base.h now includes dect_ctl.h.

**106.** dect_base.h, 2026-10-30: enum dect_base_ring_state, struct
dect_base_ring and struct dect_base's `ring`: the ring sequence's state
(was dect-stubd's ring_* fields) (S3f).

**107.** dect_base.h, 2026-10-30: dect_base_tpui() (a record's TPUI by
MM's rule) and dect_base_page() (0x0300) (S3g).

**108.** dect_base.h, 2026-10-30: struct dect_base gains `prof[]`, each
record's handset profile (was dect-stubd's per-handset copy) (S3h).
dect_base.h now includes dect_profile.h.

**109.** dect_base.h, 2026-10-30: struct dect_base_cfg and struct
dect_base's `cfg`: how the base runs calls (narrowband, two calls,
authfirst, locauth, overlap and the dial pause, alerting and binding,
call status, nlr, SETUP retries, the signal, the basic service, ring
timing, the periodic date and time) (S3i).

**110.** dect_base.h, 2026-10-30: the audio binds:
dect_base_audio_param() (0x0251), _audio_assign() (0x0230),
_audio_path() (0x022D) and _audio_release() (0x0233) (S3j).

**111.** dect_base.h, 2026-10-30: struct dect_base_cap and struct
dect_base's `cap[]` (each record's terminal capability), and
dect_base_cfg's `callinfo` (S3k). dect_base.h now includes
dect_callinfo.h.

**112.** dect_base.h, 2026-10-30: DECT_BASE_LINE_ID, DECT_BASE_PAGE_CC,
DECT_BASE_PAGE_WIDEBAND and the ring SETUP's helpers: dect_base_call_id(),
_ring_part3(), _ring_callinfo(), _basic_svc(), _ring_svc(),
_ring_narrow(), _page_info() (S3l).

**113.** dect_base.h, 2026-10-30: struct dect_base's `clip` (the caller
for the next FP-originated SETUP), dect_base_ring_setup() (the ring's
{CC-SETUP}), DECT_BASE_SETUP_RETRY_MS and DECT_BASE_SETUP_TRIES (S3m).

**114.** dect_base.h, 2026-10-30: dect_base_ops' `pump` (queued C-plane
frames to the chip), struct dect_base's `start_ms` and
`first_contact_ms`, the ring's timing constants (DECT_BASE_PAGE_*,
_REQUEUE_MAX, _RELEASE_WAIT_MS, _IN_USE_WAIT_MS) and its sequencing:
dect_base_link_use(), _ring_fixed_len(), _ring_page(), _ring_watch(),
_ring_tick() (S3n).

**115.** dect_base.h, 2026-10-30: struct dect_base's `bridge`,
`next_call` and `line2[]`; dect_base_party_link(), _drain(),
_call_new_id(), _party_line2(), _call_out() (S3o).

**116.** dect_base.h, 2026-10-30: dect_base_link's `bound`, `path_on`,
`connect_staged`, `connect_at`, `cs_connect_sent`; dect_base_cfg's
`no_cipher`; dect_base_call_auth_ok(), _number_complete(), _dial_tick(),
_dial_connect(), _connect_tick(), _cs_connect_tick() (S3p).

**117.** dect_base.h, 2026-10-30: the internal call: enum
dect_base_ic_state, struct dect_base_ic and dect_base's `ic`,
DECT_BASE_IC_NONE, _IC_PAGES, _IC_PAGE_MS, _IC_SVC; dect_base's
`loaded[]` and `name[][]`; dect_base_ops' `lanes`; dect_base_hs_name(),
_cc_ids(), _party_active(), _ic_start(), _ic_setup(), _ic_end(),
_ic_tick() (S3q).

**118.** dect_base.h, 2026-10-30: dect_base_cfg's `info_ie` and
`info_ie_len`, dect_base_link's `info_sent`; dect_base_call_clear(),
_main_free(), _ring_main(), _side_ring_start(), _side_ring_setup(),
_side_ring_tick(), _side_ring_call_tick(), _name_info(), _info_tick()
(S3r).

**119.** dect_base.h, 2026-10-30: parallel calls: dect_base_par_status(),
_par_connect(), _party_index(), _waiting_party(), _call_waiting(),
_par_tick(), _hs_in_call() (S3s).

**120.** dect_base.h, 2026-10-30: dect_base_bridge_tick() (S3t).

**121.** dect_base.h, 2026-10-30: dect_base's `absent[]` and `contacts`
(dect_base.h now includes dect_contacts.h); struct dect_base_reply;
dect_base_listed(), _contact_name(), _ctl_call() (S5a).

**122.** dect_base.h, 2026-10-30: struct dect_base_notice and
dect_base's `note[]`, `call_lists` and `page_want` (dect_base.h now
includes dect_calls.h); dect_base_hs_of_link(), _link_known(),
_notify_ok(), _notify_known(), _locate_notify(), _mwi_send(),
_mwi_locate(), _missed_send(), _hs_lines(), _missed_locate(),
_missed_due(), _missed_changed_tick(), _names_notify() (S4a).

**123.** dect_base.h, 2026-10-30: deregistration: enum
dect_base_dereg_state, struct dect_base_dereg and dect_base's `dereg`,
DECT_BASE_DEREG_PAGES and _PAGE_MS; dect_base_ops' `forget` and `mask`;
dect_base_dereg_erase(), _dereg_begin(), _dereg_tick() (S4b).

**124.** dect_base.h, 2026-10-30: the date and time round: enum
dect_base_time_state, struct dect_base_tu and dect_base's `tu`,
DECT_BASE_TIME_PAGES, _PAGE_MS, _SENT_MS; dect_base_ops' `clock`;
dect_base_time_busy(), _link_flush(), _time_tick() (S4c).

**125.** dect_base.h, 2026-10-30: dect_base_ops' `named`, dect_base_cfg's
`pin`, dect_base's `pin_wrong`, `pin_wrong_since`, `pin_locked_until`;
DECT_BASE_PIN_WRONG_MAX, _PIN_WRONG_WINDOW_MS, _PIN_LOCK_MS;
dect_base_names_spread(), _slot_in_call(), _lists_tick() (S4d).

**126.** dect_base.h, 2026-10-30: dect_base_ops' `wall`;
dect_base_ctl_call() also takes mwi and calllog (S5b).

**127.** dect_base.h, 2026-10-30: dect_base_link's `cc_was_busy`,
`nlr_armed`, `nlr_lia`, `nlr_disc_at`; dect_base_nlr_tick() (S3u).

**128.** dect_base.h, 2026-10-30: dect_base_link's `cipher_started`,
`cipher_reject_seen`, `cipher_timeout_seen`; dect_base_cfg's
`no_chip_key`; dect_base_main_cipher_start(), _main_cipher_tick(),
_side_cipher_tick() (S3v).

**129.** dect_base.h, 2026-10-30: dect_base_ops' `switched`;
dect_base_link's `time_sent_for`; dect_base's `rfpi`, `have_rfpi`,
`cc_id_foreign_warned`, `cc_id_unarmed_warned`; dect_base_serve(),
_main_ids(), _main_located() (S3w).

**130.** dect_base.h, 2026-10-30: dect_base's `line_name[][][]` and
`contacts` no longer const; dect_base_cfg's `line_name`;
dect_base_iwu_lines(), _page_adopt(), _dereg_page_response(),
_time_page_response(), _ic_target_link(), _side_ring_answer() (S3x).

**131.** dect_base.h, 2026-10-30: dect_base's `remote[]`;
dect_base_ring_port_id(), _free_record(), _main_page_response(),
_main_enrolment() (S3y).

**132.** dect_base.h, 2026-10-30: dect_base_link's `located_seen` and
`rejected_seen`; dect_base_cfg's `codeclist`, `codeclist_len`,
`locate_validity`, `defer_unknown`, `ipuio_set`, `ipuio`; dect_base's
`fetched[]`, `reg_open`, `reg_open_until`; dect_base_hs_by_ipui(),
_reg_policy(), _reg_window(), _main_cc_setup(), _main_open(),
_side_start(), _side_promote() (S3z).

**133.** dect_base.h, 2026-10-30: dect_base_link's `lane`; dect_base's
`foreign_conn`; dect_base_ops' `link_up` and `side_down`;
dect_base_cipher_on(), _contact(), _conn_up(), _conn_down() (S6a).

**134.** dect_base.h, 2026-10-30: dect_base_ops' `registered`,
`call_up`, `side_call`, `swapped`; dect_base_cfg's `verbose`;
dect_base's `foreign_seg`, `foreign_seg_handle`, `last_seen_ms[]`;
dect_base_segment() (S6b).

**135.** dect_base.h, 2026-10-30: dect_base_ops' `cap_learned` and
`call_down`; dect_base_cap_tick(), _lapc_note(), _main_dial_tick(),
_tick() (S6c).

**136.** dect_base.h, 2026-10-30: struct dect_base_reply's `named`;
dect_base_name_ok(), _ctl_admin() (S5c).

**137.** dect_base.h, 2026-10-30: dect_base_chip's `uak_table`,
`uak_guess`, `check_delay_ms`, `stub`; `DECT_BASE_UAK_STRIDE`; struct
dect_base_sub_check and dect_base's `check`; dect_base_mem_read(),
_uak_verify(), _slot_audit(), _sub_check_tick(), _mem_reply() (S2.2b).
dect_base_tick() runs the held-back reads.

**138.** dect_base.h, dect_iwu.h, 2026-10-31: dect_base_ops'
`name_visible`; dect_base_cfg's `names_readonly`; dect_iwu's
`names_readonly`; dect_base_names_refresh(). The Internal Names List per
viewer: with `name_visible(ctx, viewer, slot)` set, a handset's list has
only the entries it returns 1 for (a handset always sees itself); with
`cfg.names_readonly`, the Name field's editable bit is clear and an edit
or save is 'procedure not allowed'. Both default to today's behaviour (all
visible, renaming allowed). The filter runs where a link's view is set up
(`dect_base_iwu_lines()`, after every change of the link's own handset)
and in `dect_base_names_spread()`; after a change of the answers call
`dect_base_names_refresh()`. For the controller (list-access.md §1.1).

**139.** dect_mm.h, dect_base.h, 2026-10-31: several registration PINs at
once, and the TPUI test knob. `dect_mm_cfg` gains `n_reg_pins` and
`reg_pins[DECT_MM_REG_PINS_MAX = 32][DECT_MM_PIN_MAX]`; `dect_mm` gains
`reg_pin` (the index of the PIN the handset proved, -1 none);
`dect_base_cfg` gains `reg_pins` (an array of strings owned by the host)
and `n_reg_pins`; `dect_hs_rec` gains `reg_pin1` (1 + that index for a
record created by a registration in this run, 0 otherwise), which the
`save()` callback sees. With `n_reg_pins` 0 `cfg.pin` is the only
candidate, as before; `cfg.pin` stays the list access service's PIN. The
handset's reply is checked against every candidate (the work does not
depend on which matches), the first match wins, and a candidate that is
not 4 to 8 digits is skipped. Also `dect_base_cfg.tpui_offset` (openDECTcell
148daa7, which should have bumped the version: it changed the struct
layout): the TPUI `{0x00, 0x11, slot + N}`, 0 = off. For the controller's
registration flow: one PIN per user's window, all live PINs on every cell
where registration is open.

**140.** dect_base.h, 2026-10-31: a registration is persisted even when the
handset does not do a location registration on the same link, a new
handset takes the free record with the lowest chip slot, and the key
read-back is asked again once when nothing answers. `dect_base_link` gains
`reg_done_ms` and `persist_force`; `dect_base_sub_check` gains `uak_tries`
and `uak_asked`; new `dect_base_registration_tick(b, now)` (called from
`dect_base_tick()`; a host that drives its own ticks must call it every
tick, as `dect_base_sub_check_tick()`) and the constants
`DECT_BASE_LOCATE_GRACE_MS` (2000) and `DECT_BASE_UAK_RETRY_MS` (500).
Behaviour: a handset that already counts itself located at the base, and
re-registers over its own record, skips the location registration, and its
new key was never stored while the handset held it (C430A on the 7412,
2026-10-31): after the grace the library persists the key anyway.
`dect_base_free_record()` returns the free record with the lowest slot,
not the first in list order. In `dect-stubd` the read-back and slot audit
wait 50 ms on every build (they waited 1000 ms on 06.88 and 0 elsewhere):
a key write is effective within 20 ms (bench T-A on 08.25 and 06.88).

**141.** dect_base.h, 2026-10-31: `dect_hs_rec` gains `tpui[3]`, the TPUI the
handset holds (`{byte 0, 0x11, byte 2}`; all zero = derive it from the slot
and `cfg.tpui_offset`, as before). It is applied at every site that builds
an MM or a page from a slot (`tpui_cfg_for_slot()`): the location accept,
the pages, holds, the live links. A host with a TPUI pool sets it on a
record before the handset locates, a free record taken by an enrolment
included; a registration records the TPUI it assigned in `b->rec.tpui`, so
`save()` sees it. `dect-stubd` does not store it (its record file has no
field): after a restart a stored handset's TPUI is derived again, which
gives the same value unless `tpuioffset=` changed. A handset takes a new
TPUI only at its next location registration (bench T-C, 2026-10-31): do not
change one that is in use. Tested on air for the form `{00, 11, N}` only.

**142.** dect_base.h, 2026-10-31: `dect_base_ops` gains the optional
`registration_rejected(ctx, pin, expected)` (NULL = not told). It is called
once per rejected enrolment on the main link: `pin` is 1 when the handset's
reply matched no registration PIN (the count for a per-cell attempt limit),
0 for another failure; `expected` is 1 when the handset's profile says this
model tries PIN 0000 by itself first (`reg.tries_0000_first`), so that
failure is not a guess. The `reg: registration rejected` log line stays and
no longer needs to be parsed (the controller's #83). New field at the end of
the struct, so older hosts that zero-initialise it keep working once
rebuilt.


**143.** dect_mm.h and dect_base.h, 2026-11-01: `dect_mm_cfg.park_pli` and
`dect_base_cfg.park_pli`, the PARK length in `{ACCESS-RIGHTS-ACCEPT}` (and in the
`<<FIXED-IDENTITY>>` of an FP-originated `{CC-SETUP}`); 0 keeps the class's whole
ARI (36 for class A, 31 for class B). Behaviour changed with it, without a layout
change of its own: **the PARK no longer carries the RPN** (the bits of the RFPI
after the PARK are zero in it, so every cell of one system sends the same bytes;
a factory RFPI has RPN 0 and is unchanged), a **class B RFPI (ARC 001)** gets PLI
31 (or `park_pli`, 19 for ARC and EIC: one subscription for every system of one
EIC, `multicell-identities.md` §7.7) and its LOCATE-ACCEPT names the 31-bit PARI as
the location area. Class A is as before. New field at the end of both structs.

**144.** dect_mm.h, dect_base.h, 2026-11-01: **a base holds up to `DECT_HS_MAX` = 16
handset records, six of which can be chip slots.** `DECT_BASE_HS_MAX` (the
arrays of `struct dect_base` and the base's dereg queue), `DECT_MM_HELD_MAX` and
the range of `hs=N` in the bridge verbs and of `dect_mm_hold()`'s slot follow it:
a record's number (`dect_hs_rec.slot`, `cfg.sub_slot`) is 1 to 16, and the chip
slots stay 1 to `DECT_MM_MAX_SLOT` = 6. Only a record numbered 1 to 6 has a key in
the chip; a record numbered 7 to 16 is host-only (keyless admission, its own TPUI,
the per-call key: `more-than-six.md` §6), and `dect_base_slot_install()` and the
audit leave it alone. Not widened here: the Internal Names List (8 entries, the first
eight handsets) and the call lists (terminal ids 1 to 7, a persisted format), so a
record numbered 8 or more had neither (until API 147). Layout change of `struct dect_base`; `dect-stubd`
stays at six stores.

**145.** dect_mm.h, 2026-11-01: **`DECT_HS_MAX` is 32** (was 16). The maintainer's call:
the table is host memory, and a `struct dect_base` grows from 55 336 to 62 856
octets (measured, x86-64). Everything that follows `DECT_HS_MAX` follows (API 144):
the record numbers 1 to 32, `hs=N` in the bridge verbs, `DECT_BASE_HS_MAX`, the held
identities of a link. The chip slots stay 1 to 6, the Internal Names List stays at 8
entries and the call lists at terminal ids 1 to 7, so a record numbered 8 or more has
neither. Layout change of `struct dect_base` and `struct dect_mm`; `dect-stubd` stays
at six stores. A host that keeps a bit per record in an `unsigned` must not shift by
the record number (32 is out of range); `hold_mask` is per record index, 0 to 31.

**146.** dect_base.h, 2026-11-01: **a record can leave the table.** New:
`dect_base_quiet()` (no link, call, ring, internal call, deregistration or time
round is going, so nothing refers to a record by its index) and
`dect_base_record_drop(b, i)`: the record's subscription, name, profile, notices and
Line 2 are wiped and the index is free for the host's next `dect_base_load()`/store;
the current record is moved to another one first; -1 for a free store, a record
that is not there, or a base that is not quiet (try again later). The host has the
data in its own store: nothing is saved. The table's `n_hs` shrinks to the last
record in use. **Behaviour fix:** a record's own TPUI (API 141) was handed to the
MM at link setup only when `tpuioffset=` was set, so a host using per-record TPUIs
without that option got `{00, 11, slot}` in its LOCATE-ACCEPT; it now gets the
record's own. No layout change. For a roamer fetched from the host: `defer_unknown`
(API 18/19) holds the unknown identity, the host adds the record, `dect_mm_hold()`s
it on the live link and resumes (`dect_mm_locate_resume()`,
`dect_link_setup_resume()`).

**147.** dect_iwu.h, dect_calls.h, 2026-11-01: **a record numbered 8 to 32 has an
Internal Names entry and a call list.** The maintainer's call (decision 21). The
terminal identity number is the record number (EN 300 175 / TS 102 527-3 7.4.10.5.1.2.1
allows 8 bits, coded as IA5 digits, "14" is 31H 34H). `DECT_IWU_HANDSETS` is 32 (was 8)
and `DECT_CALLS_TERMINALS` 33 (terminal ids 1 to 32, was 1 to 7): the Internal Names
List holds up to 32 entries, a call is kept per terminal id up to 32, and the lines
bitmap (`attached`, terminal t in bit t-1) and `names_changed` already were 32 bits.
One read still delivers what fits a reply (the entries asked for, 15 data packets at
most), the handset reads on. **Persisted format:** the call-list text keeps header
`#opendectcell-calls 1`; the LINES field is 66 hex digits now (terminal ids 0 to 32), and
a text with 16 (an older daemon's) still loads, its terminal ids 8 to 32 empty. A text
written now is not read back by an older daemon (its lines are skipped; the file lives
in RAM, a reboot empties it). `dect-stubd`'s text buffer grows to 230 octets an entry
(27.6 kB). Layout change of `struct dect_iwu`, `struct dect_calls` and `struct dect_call`.

**148.** dect_base.h, 2026-11-01: **a chip-slot record can be recycled.** New:
`dect_base_record_recycle(b, i, tpui)` turns a record that holds a subscription in one of
the chip's slots (1 to 6) into a free store again, in place: the chip slot is cleared the way
a deregistration clears it (`0x0247` clear, `0x020A` with zeros), the key and everything the
base kept for the record is wiped, the record keeps its slot and gets `tpui` as the TPUI of
the next handset enrolling there, and the host's `mask()` runs. Nothing goes to the host's
`forget()` or `save()`: a host with its own store (the controller's pooled handsets) uses it
to take a handset out of the chip after it enrolled. 0; 1 when the base is not quiet (try
again); -1 for a free store, a record without a chip slot or no record. The current record
stays current. No layout change. A host whose `save()` does not mark the record
(`loaded[cur] = 1`, `absent[cur] = 0`, as `dect-stubd` does) must do so, or the record is not
one to recycle and a second enrolment picks the same free store.

**149.** dect_base.h, 2026-11-01: **an adopted record gets its own entry in the link's Internal
Names List.** New `dect_base_iwu_adopted(b, iwu, slot)`, used where a link takes on a record other
than the one it was built for (a LOCATE-REQUEST or call of another handset, a page adoption, a side
link): the list is rebuilt from the table as it is now (`dect_base_listed()` records, their names),
the handset is the list's own terminal, and the lines and the account's filter apply as before.
Until now the list kept what the link-up built, so a record that was fetched or loaded after the link
came up (a roaming handset of a controller with `defer_unknown`) had no entry, its handset found its
own name missing, and the list still held the free store that was current at link-up. A host that
set the own terminal itself after an adoption (`dect_iwu_set_own` then `dect_base_iwu_lines`) should
call this instead. No layout change.

**150.** dect_mm.h, 2026-11-02: **the TPUI a 06.88 chip hears.** New `dect_tpui_hearable(tpui[3])` and
`dect_tpui_slot_byte(slot)`. The 7412's chip routes a handset's own access (a call, the answer to a
page) by the low nibble of the TPUI's last byte and hears only 1 to 6 (bench 2026-11-02, clean-room
A-0158; 02, 11, 16, 25, 35, 42, 43, f1 answered, 0d, 17, 19, 3d never; it needs no chip slot and
compares no identity); the 7520's chip hears any. **Behaviour change:** the TPUI a record gets when the
host sets none was `{00, 11, slot}`, which for the record numbers 7 to 32 is a value the 06.88 chip
does not hear; it is now `{00, 11, dect_tpui_slot_byte(slot)}`: unchanged for slots 1 to 6, then 0x11
to 0x16, 0x21 to 0x26, and so on. A host that sets every record's TPUI (a pool) is not affected; its
pool must pass `dect_tpui_hearable()`. No layout change.

**151.** dect_base.h, 2026-11-02: **a Contact List per record.** `struct dect_base` gains `rec_contacts[]`; new
`dect_base_record_contacts(b, i, c)`. A controller keeps one phonebook per owner (the user's own contacts and the shared
ones), not one per base: the handset a record serves now reads the record's list when it has one, else `b->contacts`
as before (`dect_base_iwu_lines()` picks it, so every link that takes on a record does). The host owns the store, loads it
with `dect_contacts_load()` and takes what a handset changes with `dect_contacts_take_change()`. A record that is dropped,
recycled or wiped lets go of the pointer. Layout change; no change for a host that sets none.

**152.** dect_base.h, 2026-11-02: **a remote directory.** `struct dect_base` gains `remote_id[]`, `remote_name[][]`, `n_remote`;
new `dect_base_remote_set(b, id, name)`, `dect_base_remote_name(b, id)`, `DECT_BASE_REMOTE_MAX` (32) and
`DECT_BASE_REMOTE_FIRST` (64). In a multi-cell controller a handset's account has handsets on other bases; the Internal
Names List (INT key) was built from this base's records only, so a handset on one cell did not see its account's other
handsets on another. The host now gives each such handset a number 64..127 (never a record's slot) and a name; the entry is
on every link's list as any local entry, filtered by `name_visible(viewer, id)`, and `dect_base_names_refresh()` re-spreads
and tells the handsets. The entry is a name only: a call to it is the host's to route (the library has no peer for it).
Layout change; no change for a host that sets none.

**153.** dect_base.h, dect_mm.h, dect_nwk.h, dect_proto.h, 2026-11-02: **internal calls through the bridge, and {DETACH}.**
`dect_base_cfg` gains `bridge_internal`; `dect_base_call` gains `intern`; `dect_base` gains `detached_ms[]`; `dect_mm_stats`
gains `detached`; `DECT_MM_DETACH` (0x56). With `bridge_internal` and a bridge client, a handset's internal call (call class 9)
is not run inside the base: the number it dialled goes out as `call-out class=int number=...` (bridge-interface.md §2.1) and the
host routes it, to a handset on another base too. The target is rung by `call-in int=1 number=<caller's internal number>
name=<caller's name>`: the SETUP says internal (basic service 9x), the ring pattern is the internal one (SIGNAL 40H), a Part 3
handset gets no line, and the number is shown as network-specific, screened as passed. An internal call to a handset that is in
a call is busy, never call waiting. A {DETACH} from a handset we hold (EN 300 175-5 13.4.2) is counted in the link's MM stats
and stamps `detached_ms[i]`; being a segment it also counts in `last_seen_ms[i]`, so a record is detached while
`detached_ms[i] >= last_seen_ms[i]`. The message is not authenticated: use it for what is shown, never to refuse a call.
Layout change; no change for a host that sets none.


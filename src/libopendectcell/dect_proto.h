/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * STUB control-channel task and primitive identifiers.
 * Sources: docs/stub-pcm-control-channel.md ("Verified on hardware"),
 * docs/models/7520-DECT-analysis.md, clean-room answer A-0003. Only values
 * that are documented there are listed; each carries its source tag.
 */
#ifndef OPENDECTCELL_DECT_PROTO_H
#define OPENDECTCELL_DECT_PROTO_H

/*
 * libopendectcell API version. A single integer, bumped on ANY change to a public
 * function signature or a public struct layout -- not on behaviour changes and
 * not on comments. Consumers compiling against a pinned checkout can refuse to
 * build against an API they were not written for:
 *
 *	#if OPENDECTCELL_API_VERSION != 1
 *	#error libopendectcell API version mismatch
 *	#endif
 *
 * It tells you the API SHAPE changed. It will not tell you that behaviour
 * changed underneath a stable signature, and this project has produced several
 * of those -- treat it as a build-time guard, not a change log.
 *
 * 1: first versioned API. Adds dect_lapc_next_deadline(); dect_mm_cfg gains a
 *    configurable subscription slot and TPUI.
 * 2: adds dect_cc_tick() and dect_cc_next_deadline(); struct dect_cc gains
 *    setup_deadline_ms and struct dect_cc_stats gains setup_timeouts, both of
 *    which change a public struct layout.
 * 3: adds dect_iwu.h (the <<IWU-TO-IWU>> session responder: dect_iwu_init(),
 *    dect_iwu_set_open_params(), dect_iwu_reply()); struct dect_cc gains an
 *    embedded struct dect_iwu, which changes its layout. {CC-IWU-INFO} is now
 *    answered instead of dropped -- a behaviour change this number does not
 *    capture, which is exactly the limitation noted above.
 * 4: adds dect_link.h -- the per-connection C-plane stack (openDECTcell#67):
 *    dect_link_init(), dect_link_rx_segment(), dect_link_tick(),
 *    dect_link_drain(), dect_link_poll_append_mail(), struct dect_link and
 *    struct dect_link_ind. Purely additive: no existing signature or struct
 *    layout changed, so a consumer that does not use it needs no edit --
 *    but it SHOULD, because the glue it replaces is protocol behaviour that
 *    two hand-kept copies would drift on.
 *
 * 5: struct dect_cc grows `peer_basic_service` (the <<BASIC-SERVICE>> octet of
 *    the peer's own {CC-SETUP}, DECT_CC_SVC_NONE when it sent none), and two
 *    functions arrive: dect_cc_peer_is_speech() and dect_nwk_find_fixed_ie().
 *    The struct layout change is why this is a bump rather than an addition.
 *
 *    CONSUMERS MUST ACT. Not every call a handset places is a speech call: a
 *    CAT-iq handset opens list-access calls to read the phonebook, and in our
 *    corpus one handset placed 145 of those against 2 voice calls. Anything
 *    that arms an audio path, allocates a media resource or reports "call
 *    connected" on an incoming {CC-SETUP} must ask dect_cc_peer_is_speech()
 *    first. BOTH nibbles have to agree -- a list-access call carries call
 *    class 0010 with basic service 1000 "wideband speech", so testing the
 *    service nibble alone calls a phonebook read a wideband voice call.
 *
 * 6: GAP N.4, dialled digits -- the one mandatory GAP feature this project
 *    was missing (EN 300 444 table 1; audit in docs/gap-conformance.md).
 *    {CC-INFO} is now dispatched, <<MULTI-KEYPAD>> (0x2C) is collected into
 *    `struct dect_cc`, and `struct dect_link_ind` grows a `keypad` bit.
 *    New: dect_cc_keypad(), dect_cc_clear_keypad(), DECT_IE_MULTI_KEYPAD,
 *    DECT_CC_KEYPAD_MAX. Two struct layouts changed, hence the bump.
 *
 *    CONSUMERS: a handset dialling on the base sends its digits in one or
 *    several {CC-INFO} messages, and before this they were silently dropped
 *    -- seven runs in our capture corpus hold a handset sending "12345" past
 *    a base that ignored it. Read them with dect_cc_keypad() when
 *    `ind.keypad` is set. They are RAW and lossless: GAP's basic set is 0-9,
 *    * and #, but control codes (pause, register recall, go-to-pulse) may
 *    appear, and deciding what to do with those is policy.
 *
 * 7: <MM_ident.1>, the temporary-identity assignment timer. `struct dect_mm`
 *    grows the deadline and three flags, `struct dect_mm_stats` two counters,
 *    and dect_mm_tick() / dect_mm_next_deadline() / dect_mm_tpui_confirmed()
 *    arrive. dect_link_tick() runs the MM timer, so a consumer already
 *    calling it needs no change.
 *
 *    CONSUMERS: a TPUI in {LOCATE-ACCEPT} starts an assignment procedure the
 *    handset must acknowledge, and we page BY that TPUI. Check
 *    dect_mm_tpui_confirmed() before treating a handset as reachable: an
 *    unacknowledged assignment means pages may go to an identity it is not
 *    listening for, which fails silently.
 *
 * 8: date and time towards the handset. New `dect_time.h`
 *    (<<TIME-DATE>> 0x23, the CISS {FACILITY} 0x62 that carries it,
 *    struct dect_time, dect_time_valid/put/build_facility), `struct
 *    dect_link` grows a one-deep CISS slot, and dect_link_send_time()
 *    arrives.
 *
 *    CONSUMERS: TS 102 527-3 §7.4.20.2 makes it the FT's duty to send the
 *    date and time after every successful location registration --
 *    unconditionally, "even if the PT does not indicate support" and
 *    "independently of the 'Clock master' setting". A base is where a
 *    handset gets its clock; without this every timestamp it shows, call
 *    lists above all, is invented. The library has no clock, so the caller
 *    supplies the time and decides whether its own is trustworthy.
 *
 * 9: CAT-iq list access served for real, and {CC-SETUP} collision handling.
 *    dect_iwu.h is rewritten: the byte-echo responder (dect_iwu_reply,
 *    dect_iwu_set_open_params, DECT_IWU_TAG/REQ_06/...) is GONE, replaced by
 *    a TS 102 527-3 7.4.10 responder -- dect_iwu_handle() filling a struct
 *    dect_iwu_out, plus dect_iwu_add_handset/set_line/set_own/
 *    reset_sessions. struct dect_iwu is entirely new, so struct dect_cc
 *    (which embeds it) changes layout; dect_cc also gains setup_msg[] and
 *    the represent/collision fields, and DECT_CC_TXQ is now 16.
 *
 *    CONSUMERS MUST ACT. A CAT-iq handset will not treat itself as
 *    synchronised -- and a Gigaset refuses incoming calls -- until its list
 *    sync completes, which needs the Internal Names and Line Settings lists
 *    served with real data. The owner supplies the data via
 *    dect_iwu_add_handset()/dect_iwu_set_line() on each link's cc.iwu;
 *    nothing is served if nothing is set. A list-access service call (call
 *    class 0010) is now CALL-PROC'd only, never connected, so anything
 *    keyed on "a call reached ACTIVE" is unaffected but anything counting
 *    CONNECTs will see fewer.
 *
 * 10: the fixes from two external reviews (2026-10-24). Layout changes:
 *    struct dect_mm gains pend_id/pend_id_len and dect_mm_stats gains
 *    tpui_unsolicited; struct dect_lapc gains rx_discard and
 *    dect_lapc_stats gains rx_bad_sapi and rx_truncated; struct
 *    dect_link_ind gains page_id/page_id_len. New macros:
 *    DECT_MM_REJECT_IE_ERROR, DECT_LCE_PAGE_RESPONSE,
 *    DECT_SIGNAL_ALERT_PATTERN1.
 *
 *    CONSUMERS MUST ACT on three behaviour changes. (a) page_response is now
 *    set only for {LCE-PAGE-RESPONSE} with its identity, and the caller must
 *    compare page_id with the handset it paged before offering the call.
 *    (b) A {LOCATE-REQUEST} is rejected unless its identity matches
 *    mm.port_id, so arm port_id from your subscription store on every link,
 *    as dect-stubd does. (c) Every FP-originated {CC-SETUP} now carries
 *    <<SIGNAL>> 0x41 unless dect_cc_set_signal() says otherwise.
 *    Also: the transaction value of an FP-originated call is 0 (the lowest
 *    free), CC messages are matched to their transaction, and the API 9 note
 *    above is wrong that a Gigaset refuses incoming calls until its list
 *    sync completes -- the refusal was the transaction value.
 *
 * 11: the CC identity gate (2026-10-24). Layout changes: struct dect_cc
 *    gains gate_armed/gate_id/gate_id_len and dect_cc_stats gains
 *    rejected_identity. New: dect_cc_set_gate(),
 *    DECT_CC_REASON_MANDATORY_IE_MISSING, DECT_CC_REASON_UNKNOWN_IDENTITY.
 *
 *    CONSUMERS MUST ACT. dect_link now accepts a handset's {CC-SETUP} only
 *    if its <<PORTABLE-IDENTITY>> equals mm.port_id, and a link with no
 *    port_id takes no call at all; the others get {CC-RELEASE-COM} "unknown
 *    identity" (EN 300 444 table 17). API 10 already required arming
 *    port_id for location registration; a consumer that does not arm it now
 *    loses handset-placed calls and list access too. Treat a rise in
 *    rejected_identity as a handset you do not hold, not as a fault.
 *
 * 12: a base holds more than one handset (2026-10-25). Layout changes:
 *    struct dect_mm gains held[]/n_held/adopted (struct dect_mm_held, new).
 *    New: dect_mm_hold(), dect_mm_adopt(), DECT_MM_HELD_MAX.
 *
 *    Behaviour: {LOCATE-REQUEST} and a PT {CC-SETUP} are accepted from any
 *    held handset, and the link adopts it -- mm.port_id, the slot and the
 *    TPUI become that handset's, and mm.adopted is the tag you gave it. A
 *    consumer that holds nothing behaves exactly as under API 11. To serve
 *    several handsets, dect_mm_hold() each after every dect_link_init(),
 *    and read mm.adopted to know which one a link is serving.
 *
 * 13: {LCE-PAGE-REJECT} (2026-10-25). Layout changes: struct dect_link
 *    gains lce/lce_len, struct dect_link_ind gains page_ti. New:
 *    dect_link_page_reject(), DECT_LCE_PAGE_REJECT. Nothing changes until
 *    you call it: on a page response whose page_id is not the handset you
 *    paged, queue the reject and release the link (TS 102 527-1
 *    §7.3.8.3.1), as dect-stubd does.
 *
 * 14: codec choice for handset-placed calls (2026-10-25). Layout changes:
 *    struct dect_cc gains peer_negotiates/peer_g726/peer_has_g726/
 *    chosen_codec, dect_cc_stats gains codec_unmatched.
 *
 *    Behaviour: when a handset's {CC-SETUP} negotiates (a <<CODEC-LIST>>,
 *    or the wideband basic service), our {CC-CONNECT} now carries
 *    <<CODEC-LIST>> with G.726 alone (TS 102 527-1 §7.3.3) -- the codec the
 *    narrowband audio path carries. A basic-speech SETUP without a list is
 *    answered exactly as before. Read chosen_codec before binding audio.
 *
 * 15: dect_cc_set_busy() (2026-10-25). Layout changes: struct dect_cc gains
 *    busy, dect_cc_stats gains rejected_busy. Nothing changes until you
 *    call it: a busy CC refuses every handset-placed call but list access
 *    with "insufficient resources" (EN 300 175-5 §17.3.3). For links with
 *    no audio path behind them.
 *
 * 16: diagnostics as data (2026-10-25). New module dect_diag.[ch]: struct
 *    dect_diag_link (one per link, the protocol half from
 *    dect_diag_from_link()), dect_diag_decode_status() for the chip's
 *    0xF625 row (carrier, time slot, ten RSSI values, eight integrity
 *    counters, frames since establishment), dect_diag_frames_update() for
 *    the 16-bit wrap, and dect_diag_json(). Layout change: struct dect_mm
 *    gains peer_model/peer_codecs, what the handset declares about itself
 *    in {LOCATE-REQUEST} or {ACCESS-RIGHTS-REQUEST}. Nothing else changes.
 *
 * 17: registration policy (2026-10-25). Layout change: struct dect_mm_cfg
 *    gains registration (enum dect_mm_registration: OPEN, the default and
 *    the old behaviour; CLOSED; SAME, only the held port_id re-enrolling).
 *    Refusals answer {ACCESS-RIGHTS-REJECT} "IPUI not accepted" 0x06. Set
 *    CLOSED on any link that cannot keep a subscription, or the handset
 *    shows "registered" with a key you never stored.
 *
 * 18: deferred location registration, for roaming (2026-10-25). Layout
 *    changes: dect_mm_cfg gains defer_unknown; struct dect_mm gains the
 *    held request (locate_pending, _ti, _id, _la); dect_mm_stats gains
 *    locate_deferred and locate_retrans. New: dect_mm_locate_pending(),
 *    dect_mm_locate_resume(). With defer_unknown set, a {LOCATE-REQUEST}
 *    from an identity the link does not hold is held instead of rejected:
 *    fetch the record, dect_mm_hold() it, then resume. The PT waits
 *    <MM_locate.1> = 20 s and retransmits once.
 *
 * 19: a held CC-SETUP, for roamers that do not locate (2026-10-25).
 *    Layout change: struct dect_link gains setup/setup_len/setup_id/
 *    setup_id_len. New: dect_link_setup_pending(), dect_link_setup_resume().
 *    With mm.cfg.defer_unknown, a {CC-SETUP} from an IPUI the link cannot
 *    adopt is held instead of turned away: a handset moving within one
 *    location area calls without locating (multi-cell test M4).
 *
 * 20: location validity (2026-10-25). Layout change: dect_mm_cfg gains
 *    locate_validity. Non-zero adds <<DURATION>> to {LOCATE-ACCEPT}, so
 *    the PT locates again after that many 40.96 s units.
 *
 * 21: dect_dsaa_pin_to_ac() returns int (2026-10-25): -1 for a PIN with a
 *    non-digit or more than two digits per AC octet (8 for GAP's 32-bit
 *    AC). It used to write in front of the buffer for 9 or more digits.
 *    Validate user PINs with it.
 *
 * 22: wideband codec choice (2026-10-25). Layout change: struct dect_cc
 *    gains peer_g722, peer_has_g722, wideband. New: dect_cc_set_wideband()
 *    (CONNECT chooses G.722 when offered; off by default),
 *    dect_cc_codec() (the call's codec, including the one the handset
 *    names in ALERTING/CONNECT for a call we place), DECT_CC_CODEC_G722/
 *    G726. Bench tests 10-11: the coded G.722 path works on 07.56.
 *
 * 23: overlap receiving (2026-10-25). Layout change: struct dect_cc gains
 *    overlap, overlap_deadline_ms; dect_cc_stats gains overlap_timeouts;
 *    enum dect_cc_state gains DECT_CC_ST_OVERLAP (appended). New:
 *    dect_cc_set_overlap(), dect_cc_accept(), DECT_IE_SENDING_COMPLETE;
 *    dect_nwk_find_fixed_ie() now also finds single-octet elements
 *    (returns 0). **Act on it:** switch statements over dect_cc_state
 *    need the new state; with overlap on you must call dect_cc_accept()
 *    when dialling is complete, or F<CC.01> releases the call at 20 s.
 *
 * 24: a held CC-SETUP is answered at once (2026-10-25). Layout change:
 *    struct dect_link gains setup_ti, setup_answered, held[]/held_len[]/
 *    held_n/held_dropped; struct dect_cc gains answered. New:
 *    dect_cc_note_answered(). The link sends {CC-SETUP-ACK} (overlap,
 *    speech) or {CC-CALL-PROC} when it holds a roamer's SETUP, holds what
 *    the handset sends on that call (up to DECT_LINK_HELD), and replays it
 *    on dect_link_setup_resume(); a release from the handset ends the hold
 *    with {CC-RELEASE-COM}. Answering stops the PT's 20 s P<CC.03>, so the
 *    fetch budget is no longer 20 s for a call (bench test 7: "---").
 *
 * 25: a handset-placed call in steps (2026-10-25). Layout change: struct
 *    dect_cc gains proceeded, alerted. New: dect_cc_proceed(),
 *    dect_cc_alert(), dect_cc_connect() (dect_cc_accept() is the last).
 *    **Behaviour change:** our {CC-CONNECT} enters ACTIVE (EN 300 175-5
 *    §9.3.1.8); CONNECT PENDING is no longer used for these calls, and
 *    stats.connected counts on CONNECT. The codec is chosen when the
 *    SETUP arrives, so dect_cc_codec() can drive a bind before CONNECT.
 *
 * 26: call status and FRITZ!OS's link numbers (2026-10-25). Layout
 *    change: struct dect_cc gains call_status, status_call_id;
 *    struct dect_diag_status gains has_long, connect_frames, uptime. New:
 *    dect_cc_set_call_status() -- <<CALL-INFORMATION>> "CS call connect"
 *    in our CONNECT to a handset's call, which TS 102 527-3 §7.4.6.4.3
 *    requires and the handset's call timer keys on (A-0122) -- and
 *    dect_cc_put_call_status(); dect_diag_rssi_dbm10(), dect_diag_quality()
 *    (A-0121). **Renamed:** the eight event counters are now acrc, xcrc,
 *    zcrc, qbit1, qbit2, handover, slideerr, syncerr (A-0123), also as
 *    JSON keys; the JSON radio object gains "dbm" and "quality".
 *
 * 27: registration PIN length (2026-10-25). No layout change. New:
 *    dect_dsaa_pin_check() and DECT_PIN_DIGITS_MIN/DEFAULT/MAX (4/8/8).
 *    **Behaviour change:** dect_mm_cfg.pin no longer defaults to "0000"
 *    when empty; a PIN that is not 4 to 8 digits refuses every
 *    {ACCESS-RIGHTS-REQUEST} ("IPUI not accepted") before KEY-ALLOCATE.
 *
 * 28: the internal-call key (2026-10-28). No layout change. **Behaviour
 *    change:** a handset-placed call of class 2 (list access) or 8 whose
 *    keypad starts with 0x17 is re-classed as class 9, keeping its speech
 *    service (EN 300 444 §8.19, TS 102 527-3 §7.4.10.6.2), so
 *    peer_basic_service can change after the SETUP.
 *
 * 29: a second call on one link (2026-10-28). No layout change.
 *    **Behaviour change:** a {CC-SETUP} is accepted in DECT_CC_ST_RELEASED
 *    as in NULL (EN 300 175-5 §9.5.1: release ends in the null state), and
 *    the keypad buffer is cleared for each new call. Before, the next call
 *    on a link that stayed up was ignored (same transaction value) or
 *    refused "insufficient resources" (another value).
 *
 * 30: overlap timeout (2026-10-28). No layout change. **Behaviour change:**
 *    F<CC.01> expiry sends {CC-RELEASE} "timer expiry" and enters
 *    DECT_CC_ST_RELEASED (release pending), the normal release of GAP
 *    §8.3.2.3, instead of {CC-RELEASE-COM} and NULL.
 *
 * 31: dect_cc_info() (2026-10-28). No layout change. New: a {CC-INFO} with
 *    caller-supplied IEs on the call in progress, for information the FT
 *    adds after the SETUP (EN 300 444 §8.41/8.42, <<MULTI-DISPLAY>>).
 *
 * 32: dialling from a list (2026-10-28). Layout change: struct dect_cc
 *    gains list_number. **Behaviour change:** "1C15H + number" in the
 *    {CC-INFO} of a list-access call (class 2) re-classes it as a normal
 *    call (class 8, speech service kept), drops the prefix from the keypad
 *    and sets list_number: the number is complete (TS 102 527-3
 *    §7.4.10.6.2). Before, the call hung in PROCEEDING.
 *
 * 33: LAPC N250 (2026-10-28). Layout change: struct dect_lapc gains
 *    retry_count and failed, struct dect_lapc_stats gains link_failures.
 *    **Behaviour change:** after three unanswered retransmissions LAPC
 *    sends DISC and sets failed (EN 300 175-4 §9.2.3.6) instead of
 *    retransmitting for ever.
 *
 * 34: terminal capability (2026-10-28). Layout change: struct dect_mm
 *    gains peer_cap. New: struct dect_term_cap, dect_term_cap_parse(),
 *    DECT_IE_TERMINAL_CAPABILITY; the MM entity keeps the handset's
 *    <<TERMINAL-CAPABILITY>> from {LOCATE-REQUEST} and
 *    {ACCESS-RIGHTS-REQUEST}, with the NG-DECT Part 1 / Part 3 bits.
 *
 * 35: call status per handset (2026-10-28). Layout change: struct dect_link
 *    gains call_status_for and call_status_ctx, an optional owner hook
 *    asked when a handset's {CC-SETUP} arrives, before CC answers it: 1
 *    call status, 0 none, -1 keep (TS 102 527-3 §7.4.6.4.1).
 *
 * 36: a failed key allocation (2026-10-28). No layout change. New:
 *    DECT_MM_AUTHENTICATION_REJECT. **Behaviour change:** a wrong RES1 is
 *    answered with {AUTHENTICATION-REJECT} (EN 300 175-5 §13.6, EN 300 444
 *    §8.32.2.4), then {ACCESS-RIGHTS-REJECT} as before.
 *
 * 37: SETUP validation (2026-10-28). Layout change: struct dect_cc_stats
 *    gains ie_missing. **Behaviour change:** a {CC-SETUP} without
 *    <<BASIC-SERVICE>> gets {CC-RELEASE-COM} "mandatory information element
 *    missing" (EN 300 175-5 §17.6.1) instead of being connected.
 *
 * 38: our own location area (2026-10-28). No layout change. **Behaviour
 *    change:** with a class A RFPI, {LOCATE-ACCEPT} always carries our
 *    <<LOCATION-AREA>> (LI-type 1, LAL 36: 0x64, stock's value, A-0127),
 *    instead of echoing the handset's element or sending none.
 *
 * 39: the line pool (2026-10-28, docs/lines-and-calls.md step 1). Layout
 *    change: struct dect_iwu drops line_id and line_name for lines[] and
 *    n_lines (struct dect_iwu_line: id, name, attached bitmap or all
 *    handsets). New: dect_iwu_add_line(), dect_iwu_clear_lines(),
 *    dect_iwu_line(). The Line Settings List serves every line, sorted by
 *    line id, each with its own attached handsets. dect_iwu_set_line()
 *    still makes one line with every handset attached.
 *
 * 40: the handset's line choice (2026-10-28, lines-and-calls.md step 2).
 *    Layout change: struct dect_cc gains peer_line, line_confirmed and the
 *    default line. New: dect_callinfo_parse(), dect_cc_set_default_line(),
 *    dect_cc_line(), DECT_CC_LINE_UNSET; dect_cc_put_call_status() takes a
 *    non-const dect_cc. **Behaviour change:** with a default line set, the
 *    first call-status answer to an external handset-placed call names the
 *    selected line (the handset's, or ours for 'None') and its line type
 *    (TS 102 527-3 §7.4.5.2.1, §7.4.5.2.4).
 *
 * 41: call ids per call (2026-10-28, lines-and-calls.md step 3). Layout
 *    change: struct dect_link gains call_id_for, an optional owner hook
 *    asked for the call id of a handset's new call at its {CC-SETUP},
 *    before CC answers (TS 102 527-3 §7.4.6.1: unique system-wide).
 *
 * 42: call status reason (2026-10-28). Layout change: struct dect_callinfo
 *    (and so struct dect_cc) gains has_status_reason and status_reason.
 *    New: enum dect_callinfo_reason, DECT_CALLINFO_SUB_STATUS_REASON;
 *    encoded after the call status, parsed, and valid only with one
 *    (EN 300 175-5 §7.7.56), for "system busy" (TS 102 527-3 §7.4.8.3) and
 *    the negative acknowledgements of call waiting (§7.4.3.4).
 *
 * 43: busy system (2026-10-28). Layout change: struct dect_cc gains
 *    busy_notified and busy_deadline_ms. **Behaviour change:** a busy CC
 *    entity (dect_cc_set_busy) with call status on (a Part 3 handset)
 *    answers a handset's SETUP with CALL-PROC and the TS 102 527-3 §7.4.8.3
 *    notification (busy tone, CS call disconnecting, "system busy"), then
 *    after 5 s tones off, CS idle and {CC-RELEASE}; such a call is never
 *    connectable. Without call status the plain {CC-RELEASE-COM}
 *    "insufficient resources" stays.
 *
 * 44: unattached lines (2026-10-28, lines-and-calls.md step 2's rest).
 *    Layout change: struct dect_cc gains line_refused, the stats
 *    line_refused. New: dect_iwu_attached(). **Behaviour change:** a
 *    handset-placed call on a line the handset is not attached to gets
 *    {CC-RELEASE-COM} "incompatible service" (TS 102 527-3 §7.4.5.2.1;
 *    the reason is ours, the clause names none).
 *
 * 45: <CC.02> (2026-10-28). Layout change: struct dect_cc gains
 *    release_armed and release_deadline_ms, the stats release_timeouts.
 *    **Behaviour change:** our {CC-RELEASE} unanswered for 36 s is
 *    completed by our own {CC-RELEASE-COM} "timer expiry" and NULL
 *    (EN 300 175-5 §9.5.1, A.1).
 *
 * 46: a page response without identity (2026-10-28). No layout change.
 *    **Behaviour change:** an {LCE-PAGE-RESPONSE} without its mandatory
 *    <<PORTABLE-IDENTITY>> is cleared with {LCE-PAGE-REJECT} when the link
 *    knows the handset's IPUI (EN 300 175-5 §17.6.5, table 65), and still
 *    ignored when it does not.
 *
 * 47: SETUP flag (2026-10-28, spec sweep C8). No layout change.
 *    **Behaviour change:** a {CC-SETUP} with TI flag 1 is ignored (EN 300
 *    175-5 §17.3.2.1); before, it opened a call or counted as a collision.
 *
 * 48: extended TV (2026-10-28, spec sweep E3). No layout change.
 *    **Behaviour change:** dect_nwk_parse() fails for TV 7, so CC, MM and
 *    LCE ignore such a message (EN 300 175-5 §17.3.1); before, octet 1a
 *    was read as the message type.
 *
 * 49: release collision (2026-10-28, spec sweep C3). Layout change: the
 *    stats gain release_collisions. **Behaviour change:** a peer
 *    {CC-RELEASE} while ours is pending stops <CC.02> and enters NULL
 *    without a {CC-RELEASE-COM} (EN 300 175-5 §9.5.3).
 *
 * 50: unknown transactions (2026-10-28, spec sweep E4, I3). No layout
 *    change. **Behaviour change:** {CC-ALERTING}, {CC-CONNECT},
 *    {CC-CONNECT-ACK}, {CC-INFO} and {CC-IWU-INFO} on a transaction we do
 *    not hold are cleared with {CC-RELEASE-COM} "unknown transaction
 *    identifier" (EN 300 175-5 §17.3.2.1), like every other message type
 *    already was. Before, they were ignored, and an IWU-INFO was answered on
 *    our own transaction.
 *
 * 51: the connectionless TV (2026-10-28). No layout change. **Behaviour
 *    change:** CC disregards every message on TV 6, the connectionless
 *    value (EN 300 175-5 §7.3, §10.4.2.3), where it cleared them as unknown
 *    calls; dect_cc_set_next_ti() wraps past 5, so a call never takes 6.
 *
 * 52: LCE transaction identifier (2026-10-28, spec sweep LCE7). Layout
 *    change: struct dect_link gains lce_bad_ti. **Behaviour change:** an
 *    LCE message with F = 1 (§17.3.2.6) or TV other than 0 (§17.3.1, table
 *    68) is ignored and counted: no page response reported, no
 *    {LCE-PAGE-REJECT} sent.
 *
 * 53: MM transaction identifier (2026-10-28, spec sweep M13). Layout
 *    change: the MM stats gain bad_ti. **Behaviour change:** an MM
 *    message with TV other than 0 is ignored (§17.3.1, table 68), and an
 *    {AUTHENTICATION-REQUEST} counts as the key-allocation answer only
 *    with F = 1, on our transaction.
 *
 * 54: unserved call classes (2026-10-28, spec sweep N.32). Layout change:
 *    the stats gain service_refused; new DECT_CC_REASON_SERVICE_NOT_
 *    IMPLEMENTED. **Behaviour change:** a {CC-SETUP} for a ULE, message,
 *    IIP, service, supplementary-service or OA&M call gets {CC-RELEASE-COM}
 *    "service not implemented" (EN 300 444 §8.2.2.3) instead of being
 *    connected without speech.
 *
 * 55: access rights without identity (2026-10-28, spec sweep M5). No
 *    layout change. **Behaviour change:** an {ACCESS-RIGHTS-REQUEST}
 *    without <<PORTABLE-IDENTITY>> (or one too long to hold) gets
 *    {ACCESS-RIGHTS-REJECT} "information element error" (§17.6.4) instead
 *    of key allocation and an accept naming nobody.
 *
 * 56: TPUI assignment refused (2026-10-28, spec sweep M9). Layout change:
 *    struct dect_mm gains tpui_rejected, the stats tpui_rejected; new
 *    DECT_MM_TEMP_IDENTITY_ASSIGN_REJ. **Behaviour change:** a
 *    {TEMPORARY-IDENTITY-ASSIGN-REJ} ends a pending assignment as failed
 *    and stops <MM_ident.1> (EN 300 444 §8.28.2.3, EN 300 175-5 §13.2.2).
 *
 * 57: <MM_key.1> (2026-10-28, spec sweep M3, N.12). Layout change:
 *    struct dect_mm gains key_deadline_ms, the stats key_failed.
 *    **Behaviour change:** a {KEY-ALLOCATE} unanswered for 10 s, or refused
 *    with the PT's {AUTHENTICATION-REJECT}, fails the registration:
 *    {ACCESS-RIGHTS-REJECT} "authentication failed" and DECT_MM_FAILED
 *    (EN 300 444 §8.32.2.1, §8.32.2.3). dect_mm_next_deadline() includes
 *    the timer.
 *
 * 58: <MM_auth.1> and CIPHER-REJECT (2026-10-28, spec sweep M11, M12).
 *    Layout change: struct dect_mm gains auth_deadline_ms and
 *    cipher_rejected, the stats auth_timeouts and cipher_rejected; new
 *    DECT_MM_CIPHER_REJECT. **Behaviour change:** our authentication before
 *    ciphering fails after 10 s unanswered, or at once on the PT's
 *    {AUTHENTICATION-REJECT}; a {CIPHER-REJECT} is recorded and the call
 *    kept (EN 300 444 §8.33.2.1). <MM_cipher.1> is not built: its stop
 *    condition is DL_ENC_KEY-ind, a chip event we cannot see yet.
 *
 * 59: the proposed AUTH-TYPE (2026-10-28, spec sweep M6). Layout change:
 *    struct dect_mm gains peer_auth and peer_auth_len, from the
 *    {ACCESS-RIGHTS-REQUEST}'s <<AUTH-TYPE>>. No behaviour change; the
 *    daemon logs it with the registration's outcome.
 *
 * 60: release reasons (2026-10-28, spec sweep P-8.18). No layout change.
 *    New: dect_cc_release_reason(), dect_cc_busy_system(),
 *    DECT_CC_REASON_NONE, _USER_UNKNOWN, _USER_BUSY. dect_cc_release() is
 *    dect_cc_release_reason(cc, DECT_CC_REASON_NONE), unchanged on the air.
 *    The daemon's internal-call refusals now carry a reason, and "no audio
 *    channel" / "one at a time" are the busy system of TS 102 527-3
 *    §7.4.8.3 for a Part 3 handset.
 *
 * 61: parallel-call control codes (2026-10-28, spec sweep D2, D3, C7).
 *    Layout change: the stats gain control_codes and negative_acks.
 *    **Behaviour change:** a {CC-INFO} with "1CH xx" (TS 102 527-3 table
 *    17), or "17H" on a connected call, is a control code, never digits;
 *    a Part 3 handset gets the negative acknowledgement (§7.4.3.4): the
 *    call's status again, "control code not supported" (or CS idle and
 *    "control code failed" for a call id not ours), tone 09H. 1C15H on a
 *    list-access call and 17H before the call connects are unchanged.
 *
 * 62: codec change refused (2026-10-28, spec sweep B5). Layout change: the
 *    stats gain service_rejected; new DECT_CC_SERVICE_CHANGE, _ACCEPT,
 *    _REJECT. **Behaviour change:** a {CC-SERVICE-CHANGE} on our call gets
 *    a bare {CC-SERVICE-REJECT} (TS 102 527-1 §7.3.4: accept or reject);
 *    on a transaction we do not hold it is ignored (EN 300 175-5
 *    §17.3.2.1), where it was cleared.
 *
 * 63: table 41 refusals (2026-10-28, spec sweep H3). No layout change.
 *    **Behaviour change:** a search of the List of Supported Lists or the
 *    Line Settings List, and a field query of the List of Supported Lists,
 *    get the negative acknowledgement 'procedure not allowed' (TS 102 527-3
 *    table 41 note 1): the search was 'not supported', the query answered.
 *
 * 64: Line Settings fields (2026-10-28, spec sweep H4, M1.1). No layout
 *    change. **Behaviour change:** the Line Settings List serves fields
 *    08H 'Multiple calls mode' (30H, single call) and 0BH-0DH call
 *    forwarding (off, no codes, no number), read-only, and names them in
 *    the field query (TS 102 527-3 §7.4.11.4.8-13, table 9).
 *
 * 65: DECT System Settings List (2026-10-28, spec sweep H21, H14). Layout
 *    change: struct dect_iwu gains fw_version (default "openDECTcell").
 *    **Behaviour change:** list 07H is served, read-only, and named in the
 *    List of Supported Lists: current and new PIN as FF FF FF FF, clock
 *    master FP, base reset yes, the firmware version, an empty EEPROM
 *    version (TS 102 527-3 §7.4.11.3). Handsets that read list 00H now see
 *    one more list.
 *
 * 66: renaming a handset (2026-10-28, spec sweep H20, H10). Layout change:
 *    the IWU sessions gain a lock, struct dect_iwu the save buffer
 *    (save_entry, save_len, save_overflow, save_data) and names_changed,
 *    the stats saved; new DECT_IWU_SAVE_MAX. **Behaviour change:** "edit
 *    entry" works on every list we serve but the List of Supported Lists
 *    (it returns the entry and locks it); "save entry" on the Internal
 *    Names List stores the name, the one editable field (reset with an
 *    empty one), and sets names_changed for the owner; other fields, new
 *    entries and saves without an edit are 'procedure not allowed'. The
 *    name reads as editable, and the field query lists it first.
 *
 * 67: search (2026-10-28, spec sweep H12). No layout change. **Behaviour
 *    change:** "search entries" on the Internal Names List finds by the
 *    'Number' (IA5 decimal), with matching options 00H-02H, and delivers
 *    entries like "read entries" (TS 102 527-3 §7.4.10.4.8); no match is
 *    a confirm with counter 0. On the other lists we serve it is
 *    'procedure not allowed' (table 41), the System Settings List included.
 */
#define OPENDECTCELL_API_VERSION 67

/* Task identifiers (the src/dst bytes of a mail). */
#define DECT_TASK_UI		0x14u	/* UI task (observed; 7520 analysis) */
#define DECT_TASK_CONTACT_DST	0x02u	/* contact destination task [CR A-0003] */
#define DECT_TASK_CPLANE	0x03u	/* C-plane / DLC events [CR COMMAND_SET §8.2] */
#define DECT_TASK_AUDIO		0x10u	/* audio manager events [CR COMMAND_SET §8.4] */
#define DECT_TASK_PCM		0x22u	/* PCM task, host side (observed; 7520 analysis) */
#define DECT_TASK_PCM_REMOTE	0x23u	/* PCM task, remote side (observed) */
#define DECT_TASK_MAC_INT	0x26u	/* MAC_INTERRUPT_TASK (7520 analysis) */

/* Primitive identifiers (mail offset 4, little-endian on the wire). */
#define DECT_PRIM_CONTACT	0xF600u	/* host -> STUB contact (observed) */
#define DECT_PRIM_CONTACT_ACK1	0xF601u	/* STUB answer 1, 5 params (observed) */
#define DECT_PRIM_CONTACT_ACK2	0xF602u	/* STUB answer 2, 23 params (observed) */
#define DECT_PRIM_STATUS	0xFFF1u	/* periodic status / liveness (observed) */
#define DECT_PRIM_BUILD_STAMP	0xFFF2u	/* ASCII STUB build stamp (observed) */
#define DECT_PRIM_F618		0xF618u	/* follows a status mail (observed) */

/*
 * 0xFFF1 is a shared debug/status primitive selected by a sub-command byte
 * (the first parameter). STUB->host status uses sub-command 4; host->STUB
 * GenDSP audio uses the sub-commands below [CR A-0004].
 */
#define DECT_PRIM_GENDSP	0xFFF1u	/* host->STUB GenDSP debug (= 0xFFF1) */
#define DECT_SUB_CONNECT	0x0Au	/* params: chn_dst, chn_src */
#define DECT_SUB_LOOP		0x09u	/* params: 32-bit LE channel (1..3 on, 0 off) */
#define DECT_SUB_STATUS		0x04u	/* STUB->host periodic status */

/* GenDSP audio channels [CR A-0004] (also 7520 analysis). */
#define DECT_AUDIO_CHN_PCM0	4u	/* PCM pass-through channels 4..6 */
#define DECT_AUDIO_CHN_SILENCE	12u
#define DECT_AUDIO_CHN_TESTTONE	15u

/*
 * MAC / radio control and registration, destination task 0x02.
 * Sources: clean-room firmware spec [CR COMMAND_SET] (reconstructed from the
 * SC14441 STUB images, §6.1/§6.2); each verified on hardware 2026-10-18 (G5).
 */
#define DECT_PRIM_RADIO_ON_OFF	0x015Bu	/* 1 param: 1 = radio/beacon on, 0 = off */
#define DECT_RADIO_ON		0x01u
#define DECT_RADIO_OFF		0x00u
#define DECT_PRIM_OPEN_REG	0x022Bu	/* data[0]='A' (0x41) sets a44, opens reg */
#define DECT_OPEN_REG_A		0x41u	/* 'A': "access rights requests supported" */
#define DECT_PRIM_CLOSE_REG	0x022Cu	/* 1 param: !=0 clears a44 (closes reg) */

/* STUB -> host MAC/connection events seen during a handset access (task varies). */
#define DECT_PRIM_RADIO_STATUS	0xF623u	/* periodic while radio on [CR COMMAND_SET §8.1] */
#define DECT_PRIM_CONN_UP	0x0200u	/* CONNECTION_ESTABLISHED (dst 0x03) */
#define DECT_PRIM_CONN_DOWN	0x0201u	/* CONNECTION_RELEASED (dst 0x03) */
#define DECT_PRIM_SIGNALLING	0x0203u	/* 5-byte NWK/Cs signalling segment (dst 0x03) */
/*
 * 0x0243 is link-control bookkeeping, not an audio marker [CR A-0072-2]. The
 * stock receiver sits in the connection's link-control / data-link path: it
 * updates a counter/window value and may resume buffered signalling fragments.
 * The examined path never touches audio assignment or DSP route activation.
 * Its two trailing bytes are ordinary numeric inputs to that procedure -- there
 * is no special 0x29 case, so "connection-parameter selector" is [not
 * established]. Keep the pair opaque: log it with its timestamp, never gate on
 * it and never encode it as an enumeration.
 */
#define DECT_PRIM_LINK_CTRL	0x0243u	/* link-control update (dst 0x03) */
#define DECT_PRIM_AUDIO_STATE	0x0242u	/* audio channel state (dst 0x10) [CR §8.4] */
#define DECT_PRIM_ACCESS_EVENT	0xF604u	/* HANDSET_ACCESS_EVENT (short-form access) */
#define DECT_PRIM_ACCESS_CHAL	0x0E0Du	/* ACCESS_CHALLENGE_DATA (dst 0x05) */

/*
 * Host -> STUB C-plane / connection commands (destination task 0x02),
 * [CR COMMAND_SET §6.4]. The STUB runs only the radio/MAC; DLC, NWK, MM and
 * authentication all run here on the host [CR COMMAND_SET line 68]. Outbound
 * C-plane signalling is appended to the per-connection buffer with 0x0244 and
 * then flushed to air with 0x0202.
 */
#define DECT_PRIM_TRIGGER_SIG	0x0202u	/* TRIGGER_SIGNALLING: flush pending C-plane */
#define DECT_PRIM_APPEND_DATA	0x0244u	/* APPEND_CONNECTION_DATA: {handle,n,data[n]} */
#define DECT_PRIM_SEND_ACKED	0x015Cu	/* SEND_ACKED_DATA: 40-byte B-field, by tag */
#define DECT_PRIM_ACKED_CONFIRM	0x015Du	/* ACKED_DATA_CONFIRM reply (task 0x14) */
#define DECT_PRIM_RELEASE_CONN	0x017Au	/* RELEASE_CONNECTION: {handle, reason} */
#define DECT_PRIM_ASSIGN_AUDIO	0x0230u	/* ASSIGN_AUDIO_CHANNEL: {handle, chan_flags} */

/* APPEND_CONNECTION_DATA: per-connection buffer size, data dropped if it won't fit. */
#define DECT_APPEND_BUF_MAX	205u	/* [CR COMMAND_SET §6.4] */

#endif /* OPENDECTCELL_DECT_PROTO_H */

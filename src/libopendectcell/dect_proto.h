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
 */
#define OPENDECTCELL_API_VERSION 22

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

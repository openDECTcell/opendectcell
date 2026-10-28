/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * NWK Call Control (CC): the fixed-part side of a basic-speech call.
 * Specification: ETSI EN 300 175-5 §9, EN 300 444 (GAP).
 *
 * This is the minimal FP state machine for G7 milestone A (outgoing call): the
 * portable part dials, so it originates the CC transaction (SETUP, F=0), and we
 * answer it. For a first bring-up / loopback we accept immediately:
 *
 *   PP -> FP  CC-SETUP        (basic speech; called number; PORTABLE-IDENTITY)
 *   FP -> PP  CC-CALL-PROC    we take the call
 *   FP -> PP  CC-CONNECT      we "answer" (the far end is our loopback)
 *   PP -> FP  CC-CONNECT-ACK
 *   --- ACTIVE: the caller binds the connection's audio channel ---
 *   either    CC-RELEASE / CC-RELEASE-COM on hang-up
 *
 * Transport-agnostic, exactly like dect_mm: feed decoded NWK messages in, pull
 * NWK messages out; the caller carries them over the acknowledged C-plane
 * (dect_lapc -> dect_lc -> dect_cs) and binds audio when the call goes ACTIVE.
 * No I/O. Re-implemented from the public ETSI text as GPL-2.0-or-later.
 *
 * The exact IE content the C4 sends in CC-SETUP (dialled-digit encoding, overlap
 * vs en-bloc) is to be confirmed against a hardware capture; the outbound
 * messages here are intentionally minimal until then.
 */
#ifndef OPENDECTCELL_DECT_CC_H
#define OPENDECTCELL_DECT_CC_H

#include <stddef.h>
#include <stdint.h>

#include "dect_callinfo.h"	/* struct dect_callinfo */
#include "dect_clip.h"	/* struct dect_clip */
#include "dect_iwu.h"	/* struct dect_iwu */
#include "dect_lapc.h"	/* DECT_LAPC_MSG_MAX */

enum dect_cc_state {
	DECT_CC_ST_NULL,		/* no call */
	DECT_CC_ST_PROCEEDING,		/* SETUP seen, CALL-PROC/CONNECT queued */
	DECT_CC_ST_CONNECT_PENDING,	/* CONNECT sent, awaiting CONNECT-ACK */
	DECT_CC_ST_ACTIVE,		/* call up: bind audio */
	DECT_CC_ST_RELEASED,		/* call released */
	/* Incoming call (we originate towards the handset). */
	DECT_CC_ST_CALL_PRESENT,	/* SETUP sent, awaiting the PP's answer */
	DECT_CC_ST_RINGING,		/* PP sent ALERTING: the handset is ringing */
	/* PP-originated, dect_cc_set_overlap(): SETUP-ACK sent, digits come */
	DECT_CC_ST_OVERLAP,
};

/*
 * Pending outbound NWK messages. A list-access "read entries" answers with
 * a confirm plus up to DECT_IWU_OUT_MAX - 1 data packets in one go, each its
 * own {CC-IWU-INFO}; LAPC sends them one acknowledged frame at a time.
 */
#define DECT_CC_TXQ	16u
#define DECT_CC_ID_MAX	20u		/* PORTABLE-/FIXED-IDENTITY contents */

/*
 * Release reasons, EN 300 175-5 V2.9.1 Annex (release reason coding). Verified
 * against the table rather than taken from a trace: [CR A-0055] gave 0x01 for
 * an unhandled transaction, but 0x01 is "unexpected message" and the reason
 * the standard names for this case is 0x02. [CR A-0060] agreed and corrected.
 */
#define DECT_CC_REASON_UNEXPECTED_MESSAGE	0x01u
#define DECT_CC_REASON_UNKNOWN_TRANSACTION	0x02u
#define DECT_CC_REASON_MANDATORY_IE_MISSING	0x03u
#define DECT_CC_REASON_INCOMPATIBLE_SERVICE	0x05u
#define DECT_CC_REASON_SERVICE_NOT_IMPLEMENTED	0x06u
/*
 * "Unknown identity": what EN 300 444 table 17 recommends in the
 * {CC-RELEASE-COM} when "the FT did not recognize the PT by its
 * <<Portable identity>> IE". See dect_cc_set_gate().
 */
#define DECT_CC_REASON_UNKNOWN_IDENTITY		0x0Au
/*
 * "Insufficient resources": the reason EN 300 175-5 §9.5.2.3 prescribes for
 * the abnormal release of the OTHER side's call when two {CC-SETUP}s cross,
 * and the one §17.3.3 prescribes for a {CC-SETUP} received while another
 * call is already in progress. It is what the Gigaset handsets answered
 * every FP-originated call with for a week: their own CAT-iq list-access
 * call was crossing ours.
 */
#define DECT_CC_REASON_INSUFFICIENT_RESOURCES	0x32u

/*
 * How many times an FP-originated {CC-SETUP} is re-presented after the PT
 * released it for a setup collision (§9.5.2.3: "if the FT does re-present
 * it, it shall do so immediately"). The PT waits P<CC.06> = 1.5 s for the
 * re-presentation; two tries cover a second crossing without letting two
 * stubborn peers ping-pong for ever.
 */
#define DECT_CC_REPRESENT_MAX	2u

struct dect_cc_stats {
	uint32_t setup;		/* CC-SETUP received */
	uint32_t connected;	/* reached ACTIVE */
	uint32_t released;
	uint32_t alerting;	/* ALERTING received: the handset is ringing */
	uint32_t closed_unknown;/* transactions we closed with {CC-RELEASE} */
	uint32_t setup_timeouts;	/* F<CC.03> expiries */
	uint32_t overlap_timeouts;	/* F<CC.01> expiries: no dialling end */
	uint32_t keypad;	/* {CC-INFO} messages that carried keypad input */
	/*
	 * {CC-SETUP} collisions (EN 300 175-5 §9.5.2.3 / §17.3.3): a peer
	 * SETUP arrived while a call was already in progress and was released
	 * with "insufficient resources"; and how often our own incoming call,
	 * released by the PT for a collision, was re-presented.
	 */
	uint32_t collisions;
	uint32_t represented;
	/* {CC-SETUP}s turned away by the identity gate, dect_cc_set_gate() */
	uint32_t rejected_identity;
	/* handset-placed calls that negotiated, but offered no G.726 */
	uint32_t codec_unmatched;
	uint32_t ie_missing;	/* SETUPs refused: mandatory IE missing */
	uint32_t line_refused;	/* calls on a line the handset may not use */
	uint32_t release_timeouts;	/* <CC.02> expired, we completed it */
	uint32_t release_collisions;	/* both sides released (§9.5.3) */
	uint32_t service_refused;	/* SETUPs of a call class not served */
	/* handset-placed calls refused by dect_cc_set_busy() */
	uint32_t rejected_busy;
};

/* No <<BASIC-SERVICE>> was present in the peer's {CC-SETUP}. */
/* Keypad characters held per transaction; longer input sets keypad_overflow. */
#define DECT_CC_KEYPAD_MAX	32u

#define DECT_CC_SVC_NONE	0xFFu

struct dect_cc {
	enum dect_cc_state state;
	uint8_t ti_value;		/* transaction value from the PP's SETUP */
	uint8_t ti_flag;		/* our side of the transaction */
	uint8_t incoming;		/* 1 = we originated towards the PP */

	/*
	 * Identities seen in a CC-SETUP the handset sent us. An FP-originated
	 * SETUP has to carry both, and their encodings (identity type, bit
	 * length, padding) are fiddly enough that copying what the handset
	 * itself puts on the wire beats deriving them. The daemon persists
	 * these with the subscription so an incoming call does not depend on
	 * the handset having called first this session.
	 */
	uint8_t peer_port_id[DECT_CC_ID_MAX];
	size_t  peer_port_id_len;
	uint8_t peer_fixed_id[DECT_CC_ID_MAX];
	size_t  peer_fixed_id_len;

	/*
	 * Codec negotiation for a call the handset places (TS 102 527-1
	 * §7.3.3): its SETUP negotiates when it carries <<CODEC-LIST>> or the
	 * wideband basic service, and we then choose one codec in our
	 * {CC-CONNECT}. We choose G.722 when dect_cc_set_wideband() allows it
	 * and the handset offers it, else G.726, as the handset's own 3-octet
	 * entry. For a call WE place, chosen_codec is what the handset named
	 * in its {CC-ALERTING} or {CC-CONNECT}. 0 when no codec was chosen
	 * (basic speech: G.726). Read it with dect_cc_codec().
	 */
	uint8_t peer_negotiates;
	uint8_t peer_g726[3];
	uint8_t peer_has_g726;
	uint8_t peer_g722[3];
	uint8_t peer_has_g722;
	uint8_t chosen_codec;
	uint8_t wideband;	/* dect_cc_set_wideband() */

	uint8_t busy;		/* dect_cc_set_busy() */

	/* The identity gate, see dect_cc_set_gate(). */
	uint8_t gate_armed;
	uint8_t gate_id[DECT_CC_ID_MAX];
	size_t  gate_id_len;

	/*
	 * The <<BASIC-SERVICE>> octet of the peer's own {CC-SETUP}, or
	 * DECT_CC_SVC_NONE if it sent none. High nibble call class, low
	 * nibble basic service (EN 300 175-5 §7.7.5). Worth keeping because
	 * not every call the handset places is a speech call: a CAT-iq
	 * handset opens list-access (call class 0010) calls to read the
	 * phonebook, and binding an audio path to one of those is wrong.
	 */
	uint8_t peer_basic_service;

	/*
	 * Keypad information the peer has sent, in order, as received.
	 *
	 * GAP N.4 "dialled digits (basic)" is MANDATORY for an FT
	 * (EN 300 444 table 1) and we did not implement it until 2026-10-23:
	 * a handset dialling on our base sent {CC-INFO} with
	 * <<MULTI-KEYPAD>> and we dropped every one. Seven runs in the
	 * capture corpus hold the digits "12345" going past us.
	 *
	 * Stored RAW and lossless. GAP's basic set is 0-9, * and #, but a
	 * handset may also send control codes (pause 0x05, register recall
	 * 0x15, go-to-pulse 0x12 -- all optional features we do not claim).
	 * Deciding what to do with those is policy, so it belongs to the
	 * caller, not here.
	 */
	uint8_t  keypad[DECT_CC_KEYPAD_MAX];
	size_t   keypad_len;
	unsigned keypad_overflow:1;	/* more arrived than we can hold */
	/*
	 * The number came complete from a list-access call ("1C15H + number",
	 * TS 102 527-3 §7.4.10.6.2): the owner routes it as dialled, at once.
	 */
	uint8_t  list_number;
	/*
	 * The line of a handset-placed call (TS 102 527-3 §7.4.5.2): what
	 * the handset chose in <<CALL-INFORMATION>> (DECT_CC_LINE_UNSET if
	 * nothing yet, DECT_CALLINFO_LINE_NONE for 'None'), and whether our
	 * answer has named the selected line. The first choice counts.
	 */
	uint8_t  peer_line;
	uint8_t  line_confirmed;
	uint8_t  line_refused;	/* chose a line it is not attached to */
	/*
	 * "Busy system" notified (TS 102 527-3 §7.4.8.3): the call waits out
	 * busy_deadline_ms with the busy tone, then is released.
	 */
	uint8_t  busy_notified;
	uint64_t busy_deadline_ms;
	/*
	 * <CC.02> (EN 300 175-5 §9.5.1, A.1: 36 s): our {CC-RELEASE} awaits
	 * the handset's {CC-RELEASE-COM}; on expiry we send it ourselves.
	 */
	uint8_t  release_armed;
	uint64_t release_deadline_ms;
	/* dect_cc_set_default_line(): the FP-managed line, 0 = none set */
	uint8_t  default_line_set;
	uint8_t  default_line;
	uint8_t  default_line_type;

	/*
	 * Optional <<SIGNAL>> for an FP-originated SETUP: the alerting
	 * pattern the handset should use. Off by default, because the
	 * handset has its own default and we only override it deliberately.
	 */
	uint8_t  signal_on;
	uint8_t  signal_value;
	/*
	 * Optional <<CALL-INFORMATION>> for an FP-originated SETUP: the
	 * line (and call) identifiers that mark the call as external. Empty
	 * by default, so the SETUP proved at G7 is unchanged.
	 */
	struct dect_callinfo callinfo;
	uint8_t  next_ti;		/* transaction value for the next SETUP */
	/*
	 * F<CC.03>, the CC setup timer: 20 s, FT and PT (EN 300 175-5 A.1).
	 * Started when we send a {CC-SETUP}; on expiry the FT shall reject the
	 * call with {CC-RELEASE-COM} and release reason "timer expiry", and
	 * enter NULL (9.3.2.2). 0 means not running.
	 *
	 * Without it a call the handset never answers leaves the entity stuck
	 * in CALL PRESENT for ever, which is what happened all through the
	 * 2026-10-22 ring tests: every refused call left a transaction behind
	 * and only a daemon restart cleared it.
	 */
	uint64_t setup_deadline_ms;
	uint8_t  overlap;		/* dect_cc_set_overlap() */
	uint8_t  answered;		/* dect_cc_note_answered() */
	uint8_t  proceeded;		/* {CC-CALL-PROC} sent on this call */
	uint8_t  call_status;		/* dect_cc_set_call_status() */
	uint8_t  status_call_id;
	uint8_t  alerted;		/* {CC-ALERTING} sent on this call */
	uint64_t overlap_deadline_ms;	/* F<CC.01> */

	/*
	 * The last FP-originated {CC-SETUP} as sent, kept for re-presentation.
	 *
	 * EN 300 175-5 §9.5.2.3, call setup collision: when the PT's own
	 * {CC-SETUP} crosses ours, the PT releases OUR call with
	 * {CC-RELEASE-COM} "insufficient resources", we release ITS call the
	 * same way, and "the FT may re-present the incoming call ... If the
	 * FT does re-present it, it shall do so immediately." The PT, for its
	 * part, "shall wait for the re-presentation of the incoming voice
	 * call from the FT, and not setup any new outgoing call" for
	 * P<CC.06> = 1.5 s. Measured on a Gigaset A540 CAT, 2026-10-24: the
	 * handset opens a CAT-iq list-access call on every link that comes
	 * up, so EVERY FP-originated call to it was a collision, and without
	 * the re-presentation every one of them ended in that 0x32.
	 *
	 * `represent_left` counts down from `represent_max`
	 * (DECT_CC_REPRESENT_MAX unless the caller changes it) per call.
	 */
	uint8_t  setup_msg[DECT_LAPC_MSG_MAX];
	size_t   setup_len;
	uint8_t  represent_left;
	uint8_t  represent_max;

	/* FIFO of outbound NWK messages (e.g. CALL-PROC then CONNECT). */
	uint8_t  out[DECT_CC_TXQ][DECT_LAPC_MSG_MAX];
	size_t   out_len[DECT_CC_TXQ];
	unsigned out_head, out_count;

	/*
	 * <<CODEC-LIST>> to advertise in an FP-originated {CC-SETUP}; empty
	 * means send none. Set with dect_cc_set_codec_list().
	 */
	uint8_t  codec_list[8];
	size_t   codec_list_len;

	struct dect_cc_stats stats;
	struct dect_iwu iwu;	/* <<IWU-TO-IWU>> session responder */
};

void dect_cc_init(struct dect_cc *cc);

/* Advertise this <<CODEC-LIST>> in FP-originated setups; NULL clears it. */
void dect_cc_set_codec_list(struct dect_cc *cc, const uint8_t *list, size_t len);

/*
 * Accept a {CC-SETUP} from a handset only if its <<PORTABLE-IDENTITY>> is
 * `port_id`. Anything else is answered with {CC-RELEASE-COM}, release reason
 * "unknown identity" (EN 300 444 §8.2.2.3 and table 17), or "mandatory
 * information element missing" if it carries no portable identity at all
 * (EN 300 175-5 §17.6.1), and the call never starts. `port_id_len` 0 arms
 * the gate with nothing to match, so every setup is turned away: a base that
 * holds no handset has no handset to recognise. Until this is called the
 * gate is open, as it is after dect_cc_init().
 *
 * dect_link arms it from mm.port_id before every CC message, so the
 * {LOCATE-REQUEST} gate and this one always agree. It used to be open: on
 * 2026-10-24 the A540, rejected at location registration, still ran its
 * list-access call, was served the Internal Names List of whichever handset
 * the daemon held and showed that handset's name as its own -- and in an
 * enrolment window, before any identity was held, its {CC-SETUP} was where
 * the daemon learned the identities it later offered the AS690H's calls to.
 */
void dect_cc_set_gate(struct dect_cc *cc, const uint8_t *port_id,
		      size_t port_id_len);

/*
 * Refuse every handset-placed call except list access: a {CC-SETUP} whose
 * call class is not 0010 gets {CC-RELEASE-COM} "insufficient resources",
 * the reason EN 300 175-5 §17.3.3 gives a SETUP while another call is in
 * progress. For a link with no audio path behind it -- dect-stubd's side
 * links, which serve location registration and list access while the main
 * link carries the call -- so a handset is not connected into silence.
 */
void dect_cc_set_busy(struct dect_cc *cc, int busy);

/* <<CODEC-LIST>> codec identifiers (EN 300 175-5 §7.7.54) */
#define DECT_CC_CODEC_G726	0x02u	/* narrowband, full slot */
#define DECT_CC_CODEC_G722	0x03u	/* wideband, long slot */

/*
 * Allow G.722 in our {CC-CONNECT} for a handset-placed call that offers it
 * (TS 102 527-1 §7.3.3). Off by default: set it only when the audio path
 * behind this link carries wideband (dect-stubd: chip bind flags 0x41,
 * codec byte 0, a G.722 lane).
 */
void dect_cc_set_wideband(struct dect_cc *cc, int on);

/*
 * The codec of the current call: what we chose in {CC-CONNECT}, or what
 * the handset chose in {CC-ALERTING}/{CC-CONNECT} for a call we placed.
 * DECT_CC_CODEC_G722, DECT_CC_CODEC_G726, or 0 = none named (basic
 * speech, G.726).
 */
uint8_t dect_cc_codec(const struct dect_cc *cc);

/*
 * Overlap receiving (EN 300 444 §8.3, EN 300 175-5 §9.3.1.4/5). With it on,
 * a handset-placed speech call whose {CC-SETUP} lacks <<SENDING-COMPLETE>>
 * gets {CC-SETUP-ACK} and waits in DECT_CC_ST_OVERLAP while the digits
 * arrive in {CC-INFO} (dect_cc_keypad()). The owner decides when dialling
 * is complete -- a dial plan, an inter-digit timeout, the controller --
 * and calls dect_cc_accept(). A <<SENDING-COMPLETE>> in a {CC-INFO} moves
 * the call on by itself. F<CC.01> (20 s, restarted by every {CC-INFO})
 * releases a call nobody accepted with "timer expiry".
 *
 * Off: CALL-PROC and CONNECT at once, the digits arriving after CONNECT
 * (GAP figure 2, legal, but the Gigasets then show neither "connected"
 * nor a call timer -- bench test 2, 2026-10-25).
 *
 * Either way an internal call (call class 9) is never connected by CC:
 * with its number complete it waits in DECT_CC_ST_PROCEEDING until the
 * owner has reached the other handset (dect_cc_alert/connect()).
 */
void dect_cc_set_overlap(struct dect_cc *cc, int on);

/*
 * The steps of a handset-placed call, for an owner that has somewhere to
 * route it (EN 300 175-5 §9.3.1.6-8): dect_cc_proceed() sends
 * {CC-CALL-PROC} (dialling is complete), dect_cc_alert() {CC-ALERTING}
 * (the far end rings), dect_cc_connect() {CC-CONNECT} (it answered) and
 * enters ACTIVE -- no {CC-CONNECT-ACK} is expected. Each sends the
 * earlier ones it needs, once. dect_cc_accept() is dect_cc_connect().
 * They act on a speech call in DECT_CC_ST_OVERLAP or PROCEEDING and
 * return 0, else -1 (list access is never connected).
 *
 * §9.3.1.8 also has the FT confirm the U-plane before {CC-CONNECT}: an
 * owner can bind its audio after dect_cc_proceed() -- dect_cc_codec() is
 * known from the SETUP -- and connect once it is up.
 */
/*
 * Call status in a handset-placed call (TS 102 527-3 §7.4.6.4.3): with it
 * on, our {CC-SETUP-ACK}, {CC-CALL-PROC}, {CC-ALERTING} and {CC-CONNECT}
 * carry <<CALL-INFORMATION>> with call id `call_id` and the matching call
 * status (CS call setup ack / proc / alerting / connect). "CS call
 * connect shall always be sent in the {CC-CONNECT}": a CAT-iq handset
 * keys its call timer on it, not on CONNECT itself (clean room A-0122,
 * from the stock capture). Lines come from dect_cc_set_callinfo() if set.
 */
void dect_cc_set_call_status(struct dect_cc *cc, int on, uint8_t call_id);

/* Append our <<CALL-INFORMATION>> with call status `status` at b+off, if
 * call status is on; returns the new offset (unchanged when off or it
 * does not fit). For whoever answers a SETUP on CC's behalf (dect_link). */
size_t dect_cc_put_call_status(struct dect_cc *cc, uint8_t *b,
			       size_t cap, size_t off, uint8_t status);

int dect_cc_proceed(struct dect_cc *cc);
int dect_cc_alert(struct dect_cc *cc);
int dect_cc_connect(struct dect_cc *cc);
int dect_cc_accept(struct dect_cc *cc);

/*
 * The next {CC-SETUP} was already answered with msg_type ({CC-SETUP-ACK}
 * or {CC-CALL-PROC}) by whoever held it -- dect_link, for a roamer whose
 * record was being fetched -- so CC must not send that answer again.
 * Consumed by that SETUP.
 */
void dect_cc_note_answered(struct dect_cc *cc, uint8_t msg_type);

/* Feed a decoded NWK message (the LAPC I-frame payload). Non-CC messages are
 * ignored. Any responses are queued; pull them with dect_cc_poll(). */
void dect_cc_rx(struct dect_cc *cc, const uint8_t *msg, size_t len);

/*
 * Advance the clock. Expires F<CC.03> and queues the abnormal release the
 * specification requires. Call it alongside dect_lapc_tick().
 */
void dect_cc_tick(struct dect_cc *cc, uint64_t now_ms);

/*
 * Is the peer's call a speech call? True for basic speech (0000), wideband
 * speech (1000) and DTAM wideband speech (1011); false for list access, ULE,
 * message calls and anything else, and false when the peer sent no
 * <<BASIC-SERVICE>> at all. Use it before binding an audio path to an
 * incoming call.
 */
int dect_cc_peer_is_speech(const struct dect_cc *cc);

/*
 * Does the peer on this transaction carry <<PORTABLE-IDENTITY>> `port_id`?
 * True on a match, and true when `port_id_len` is 0 -- there is then no
 * subscribed identity to contradict, so the caller decides.
 *
 * Call this before keeping anything a CC transaction tells you about "the"
 * handset. Measured 2026-10-24: one handset runs a CAT-iq list-access call
 * every ~22 s, and the daemon learned ITS identities into whichever
 * subscription store was open -- so the C430A and AS690H were registered with
 * the A540's portable identity, and every FP-originated {CC-SETUP} afterwards
 * offered the call to a handset that was not listening. Both handsets paged,
 * answered, and refused with "insufficient resources", which read as three
 * handsets agreeing when it was one handset being addressed three times.
 */
int dect_cc_peer_is(const struct dect_cc *cc, const uint8_t *port_id,
		    size_t port_id_len);

/*
 * Keypad information collected on this transaction, oldest first, and its
 * length. The buffer is NOT NUL-terminated -- use the length. Returns NULL
 * when nothing has been received.
 */
const uint8_t *dect_cc_keypad(const struct dect_cc *cc, size_t *len);

/* Drop the collected keypad input, e.g. once it has been acted on. */
void dect_cc_clear_keypad(struct dect_cc *cc);

/*
 * When dect_cc_tick() next has work, or UINT64_MAX if no CC timer is running.
 * Pair it with dect_lapc_next_deadline() and arm one timer on the minimum.
 */
uint64_t dect_cc_next_deadline(const struct dect_cc *cc);

/*
 * Start an incoming call: queue an FP-originated CC-SETUP towards the handset,
 * which makes it ring. `port_id` is the PP's PORTABLE-IDENTITY contents as the
 * subscription stored them (`dect_mm.port_id`), `fixed_id` the FIXED-IDENTITY
 * contents identifying this base. `basic_service` is the <<BASIC-SERVICE>>
 * value octet: the low nibble is the service (8 = wideband speech, which is
 * what AVM handsets negotiate here), the high nibble the call class.
 *
 * EN 300 444 makes PORTABLE-IDENTITY, FIXED-IDENTITY and BASIC-SERVICE
 * mandatory in an FP-originated SETUP. The handset answers ALERTING while it
 * rings and CONNECT when the user accepts; we then send CONNECT-ACK and the
 * call is active, at which point the audio bind is the same as for a call the
 * handset places.
 *
 * Returns 0, or -1 if a call is already up or the message would not fit.
 */
int dect_cc_setup(struct dect_cc *cc, const uint8_t *port_id, size_t port_id_len,
		  const uint8_t *fixed_id, size_t fixed_id_len,
		  uint8_t basic_service);

/*
 * As dect_cc_setup(), plus caller identity: CALLING-PARTY-NUMBER and
 * CALLING-PARTY-NAME are appended after BASIC-SERVICE when `id` supplies them
 * with presentation allowed. `id` may be NULL, which is exactly
 * dect_cc_setup().
 *
 * The IEs are appended last so that a SETUP without an identity is
 * byte-identical to the one that was proved at G7. Either IE is silently
 * omitted when it has no content, when presentation is not allowed, or when
 * the message has no room left for it -- a caller's name is never a reason to
 * fail a call that would otherwise ring.
 */
int dect_cc_setup_id(struct dect_cc *cc, const uint8_t *port_id,
		     size_t port_id_len, const uint8_t *fixed_id,
		     size_t fixed_id_len, uint8_t basic_service,
		     const struct dect_clip *id);

/*
 * Choose the <<SIGNAL>> IE of the next FP-originated SETUP. `value` is the
 * signal code: DECT_SIGNAL_ALERT_PATTERN1 (0x41, external call) is the
 * default from dect_cc_init(), DECT_SIGNAL_ALERT_PATTERN0 (0x40) marks an
 * internal call (EN 300 444 table 23).
 *
 * enable = 0 sends no IE. That is for tests only: EN 300 444 §8.14 makes
 * alerting the FT's job, and with no <<SIGNAL>> the Gigaset A540 displays
 * the call and blinks but makes no sound (run 18, 2026-10-24). This comment
 * used to say the handset would "choose"; it does not.
 */
void dect_cc_set_signal(struct dect_cc *cc, int enable, uint8_t value);

/*
 * Set the <<CALL-INFORMATION>> the next FP-originated SETUP carries, after
 * every other IE (the stock order puts 0x7E last). `ci` is copied; NULL goes
 * back to sending none. Like the caller identity it is never fatal: an IE
 * that does not fit, or an invalid `ci`, is left out and the call still
 * rings, so validate with dect_callinfo_valid() where the values come in.
 */
void dect_cc_set_callinfo(struct dect_cc *cc, const struct dect_callinfo *ci);

/*
 * Non-zero while a call transaction exists -- from the moment a SETUP is sent
 * or received until the call is released. The caller needs this to tell a
 * genuinely new link from the chip re-establishing one underneath a call that
 * has not been answered yet: re-initialising the stack in that window loses
 * the transaction, and the handset's CC-CONNECT then goes unanswered.
 */
int dect_cc_busy(const struct dect_cc *cc);

/*
 * Queue a {CC-INFO} on the call in progress carrying `ies` as its
 * information elements, already encoded (EN 300 175-5 §6.3.2.4). For
 * information the FT adds after the SETUP -- caller ID that did not fit
 * (EN 300 444 §8.41/8.42), <<MULTI-DISPLAY>> text. 0 on success, -1 with
 * no call or no room.
 */
int dect_cc_info(struct dect_cc *cc, const uint8_t *ies, size_t len);

#define DECT_CC_LINE_UNSET	0xFFu

/*
 * The line the FP picks when a handset sends 'None' (FP-managed selection,
 * TS 102 527-3 §7.4.5.2.4), and its line type. Once set, our first call
 * status answer to a handset-placed external call names the selected line
 * and its type, as the FP "shall" (§7.4.5.2.1); only where call status
 * goes at all (dect_cc_set_call_status, Part 3 handsets).
 */
void dect_cc_set_default_line(struct dect_cc *cc, uint8_t line_id,
			      uint8_t line_type);

/* The selected line of the call: the handset's, ours for 'None', or UNSET. */
uint8_t dect_cc_line(const struct dect_cc *cc);

/*
 * Choose the transaction value the next FP-originated SETUP will use. Values
 * 0..6 are valid for the originating side (7 is the extended-TI escape).
 * dect_cc_init() leaves it at 0, and callers should normally leave it there.
 *
 * EN 300 444 §6.9.2: "The transaction identifier value for a CC call shall
 * always get assigned the lowest available free number." This entity runs
 * one call at a time, and the handset's own calls carry the other flag, so
 * the lowest free value for a call we originate is always 0.
 *
 * From 2026-10-20 the daemon rotated the value per call, after a call cut off
 * mid-media left a C4 discarding the next SETUP on the same value; this
 * comment then claimed rotating "is what the standard expects". It is not,
 * and it is why every Gigaset refused every incoming call with 0x32: run 15
 * (value 0) rang the A540, and the control in the same session, run 16
 * (value 1, otherwise identical), was refused (docs/call-g7.md, runs 14-16).
 * A transaction left open by a lost call is cleared by releasing it, not
 * dodged by picking another value. Use this only to override for a test.
 */
void dect_cc_set_next_ti(struct dect_cc *cc, uint8_t ti);

/* Queue a CC-RELEASE to hang up an active call. Returns 0, or -1 if idle. */
int dect_cc_release(struct dect_cc *cc);

/* Pull a pending outbound NWK message. Returns its length, or 0 if none. */
size_t dect_cc_poll(struct dect_cc *cc, uint8_t *out, size_t cap);

#endif /* OPENDECTCELL_DECT_CC_H */

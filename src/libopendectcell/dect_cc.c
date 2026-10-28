// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * NWK Call Control FSM (fixed-part, basic speech). See dect_cc.h.
 * ETSI EN 300 175-5 §9 / EN 300 444. The PP originates the call (SETUP, F=0),
 * so our replies are responder-side (F=1) in the same transaction.
 */
#include "dect_cc.h"
#include "dect_iwu.h"
#include "dect_nwk.h"

#include <string.h>

void dect_cc_init(struct dect_cc *cc)
{
	memset(cc, 0, sizeof(*cc));
	cc->state = DECT_CC_ST_NULL;
	cc->peer_basic_service = DECT_CC_SVC_NONE;
	cc->peer_line = DECT_CC_LINE_UNSET;
	cc->ti_flag = DECT_TI_RESPONDER;	/* until a call says otherwise */
	cc->represent_max = DECT_CC_REPRESENT_MAX;
	/*
	 * EN 300 444 §8.14: the FT starts PT alerting with <<SIGNAL>> in the
	 * {CC-SETUP} or in a {CC-INFO}, and must support one of the two.
	 * Without it the Gigaset A540 shows the call and rings silently
	 * (run 18, 2026-10-24). External-call alerting unless told otherwise.
	 */
	cc->signal_on = 1;
	cc->signal_value = DECT_SIGNAL_ALERT_PATTERN1;
	dect_iwu_init(&cc->iwu);
}

/* Reserve the next free TX slot and return a pointer to build into, or NULL. */
static uint8_t *txq_slot(struct dect_cc *cc, unsigned *idx)
{
	if (cc->out_count >= DECT_CC_TXQ)
		return NULL;
	*idx = (cc->out_head + cc->out_count) % DECT_CC_TXQ;
	return cc->out[*idx];
}

static void txq_commit(struct dect_cc *cc, unsigned idx, size_t len)
{
	cc->out_len[idx] = len;
	cc->out_count++;
}

/* Queue a bare CC message (header only) with the given responder/originator flag. */
static void queue_msg(struct dect_cc *cc, uint8_t ti_flag, uint8_t msg_type)
{
	unsigned idx;
	uint8_t *b = txq_slot(cc, &idx);
	size_t off;

	if (!b)
		return;
	off = dect_nwk_build_hdr(b, DECT_LAPC_MSG_MAX, DECT_PD_CC,
				 ti_flag, cc->ti_value, msg_type);
	txq_commit(cc, idx, off);
}

size_t dect_cc_put_call_status(struct dect_cc *cc, uint8_t *b,
			       size_t cap, size_t off, uint8_t status)
{
	struct dect_callinfo ci;
	size_t next;
	uint8_t line = dect_cc_line(cc);
	unsigned cls = cc->peer_basic_service >> 4;

	if (!cc->call_status)
		return off;
	dect_callinfo_init(&ci);
	ci.has_line = cc->callinfo.has_line;
	ci.line_id = cc->callinfo.line_id;
	ci.has_line_type = cc->callinfo.has_line_type;
	ci.line_type = cc->callinfo.line_type;
	/*
	 * §7.4.5.2.1: the FP sends back the selected line and its type, on
	 * an external handset-placed call (not internal, not list access:
	 * A2 E13), once.
	 */
	if (!ci.has_line && !cc->incoming && !cc->line_confirmed &&
	    line != DECT_CC_LINE_UNSET && cls != 0x9u && cls != 0x2u &&
	    cc->default_line_set) {
		ci.has_line = 1;
		ci.line_id = line;
		ci.has_line_type = 1;
		ci.line_type = cc->default_line_type;
		cc->line_confirmed = 1;
	}
	ci.has_call_id = 1;
	ci.call_id = cc->status_call_id;
	ci.has_call_status = 1;
	ci.call_status = status;
	next = dect_callinfo_put(b, cap, off, &ci);
	return next ? next : off;
}

/* A responder message with our call status, for a handset-placed call. */
static void queue_with_status(struct dect_cc *cc, uint8_t msg_type,
			      uint8_t status)
{
	unsigned idx;
	uint8_t *b = txq_slot(cc, &idx);
	size_t off;

	if (!b)
		return;
	off = dect_nwk_build_hdr(b, DECT_LAPC_MSG_MAX, DECT_PD_CC,
				 DECT_TI_RESPONDER, cc->ti_value, msg_type);
	off = dect_cc_put_call_status(cc, b, DECT_LAPC_MSG_MAX, off, status);
	txq_commit(cc, idx, off);
}

/*
 * Queue a {CC-IWU-INFO} carrying one <<IWU-TO-IWU>>, our reply to the peer's.
 * The transaction is the peer's: these records ride the call's transaction,
 * so we answer with the same TI value and our own flag.
 */
static void queue_iwu(struct dect_cc *cc, const uint8_t *body, size_t body_len)
{
	unsigned idx;
	uint8_t *b = txq_slot(cc, &idx);
	size_t off;

	if (!b)
		return;
	off = dect_nwk_build_hdr(b, DECT_LAPC_MSG_MAX, DECT_PD_CC,
				 cc->ti_flag, cc->ti_value, DECT_CC_IWU_INFO);
	off = dect_nwk_put_ie(b, DECT_LAPC_MSG_MAX, off,
			      DECT_IE_IWU_TO_IWU, body, body_len);
	if (!off)
		return;			/* would not fit; drop rather than truncate */
	txq_commit(cc, idx, off);
}

/*
 * Queue a {CC-RELEASE-COM} carrying a <<RELEASE-REASON>>. The bare queue_msg()
 * form sends no reason, which is legal but tells the peer nothing; an abnormal
 * release should say why.
 */
static void queue_release_msg(struct dect_cc *cc, uint8_t type,
			      uint8_t reason)
{
	unsigned idx;
	uint8_t *b = txq_slot(cc, &idx);
	size_t off;

	if (!b)
		return;
	off = dect_nwk_build_hdr(b, DECT_LAPC_MSG_MAX, DECT_PD_CC,
				 cc->ti_flag, cc->ti_value, type);
	if (!off)
		return;
	off = dect_nwk_put_fixed_ie(b, DECT_LAPC_MSG_MAX, off,
				    DECT_IE_RELEASE_REASON, reason);
	if (!off)
		return;
	txq_commit(cc, idx, off);
}

static void queue_release_com(struct dect_cc *cc, uint8_t reason)
{
	queue_release_msg(cc, DECT_CC_RELEASE_COM, reason);
}

/*
 * Close a transaction we have no state for, without disturbing ours.
 *
 * [CR A-0055]: the C4 sends {CC-FACILITY} (type 0x62) carrying a <<FACILITY>>
 * IE of AVM proprietary data. Stock does not answer it in the kernel either --
 * it hands the content to a FRITZ userspace app that makes the proprietary
 * reply. We have no such app, and silence is not a neutral choice: an
 * unanswered CC transaction is retransmitted for as long as the handset cares
 * to, the link never goes quiescent, no disconnect can complete, the handset
 * is never idle-locked, it never answers a page, the chip never allocates a
 * traffic bearer, and no call ever gets a configured audio path.
 *
 * vF423 measured that: twelve 85-octet {CC-FACILITY} messages in sixty
 * seconds, each reassembled correctly and each ignored.
 *
 * So close what we cannot answer. EN 300 175-5 V2.9.1 §17.3.2.1 says exactly
 * how: on a message carrying a transaction identifier that does not relate to
 * an active call, clearing is initiated with a {CC-RELEASE-COM} carrying
 * release reason "unknown transaction identifier", returning the same unknown
 * transaction identifier. Note {CC-RELEASE-COM}, not {CC-RELEASE}: the
 * completion message declares the transaction closed, where a {CC-RELEASE}
 * invites a confirmation the peer has no reason to send. We shipped 0x4d
 * first, from [CR A-0055], and the C4 answered it by dropping the link and
 * starting over -- which is what "I asked you to confirm and you had nothing
 * to confirm" looks like. [CR A-0060] traced stock's 0x4d to its
 * {CC-SERVICE-CHANGE} handler, the one case §17.3.2.1 excludes.
 *
 * The reply carries the PEER's transaction value with the flag inverted -- we
 * are the responding side of their transaction -- and leaves cc->ti_* alone,
 * because a call of ours may be live in a different transaction at the same
 * time.
 */
static void queue_release_other(struct dect_cc *cc, uint8_t peer_ti_flag,
				uint8_t peer_ti_value, uint8_t reason)
{
	unsigned idx;
	uint8_t *b = txq_slot(cc, &idx);
	size_t off;
	uint8_t flag;

	if (!b)
		return;
	flag = peer_ti_flag == DECT_TI_ORIGINATOR ? DECT_TI_RESPONDER
						  : DECT_TI_ORIGINATOR;
	off = dect_nwk_build_hdr(b, DECT_LAPC_MSG_MAX, DECT_PD_CC,
				 flag, peer_ti_value, DECT_CC_RELEASE_COM);
	if (!off)
		return;
	off = dect_nwk_put_fixed_ie(b, DECT_LAPC_MSG_MAX, off,
				    DECT_IE_RELEASE_REASON, reason);
	if (!off)
		return;
	txq_commit(cc, idx, off);
}

/* Append both identity IEs; either may be omitted, neither may fail the call. */
static size_t put_identity(uint8_t *b, size_t off, const struct dect_clip *id)
{
	size_t next = dect_clip_put_number(b, DECT_LAPC_MSG_MAX, off, id);

	if (next)
		off = next;
	next = dect_clip_put_name(b, DECT_LAPC_MSG_MAX, off, id, 0);
	return next ? next : off;
}

/* Append <<CALL-INFORMATION>> if any is set; like the identity, never fatal. */
static size_t put_callinfo(uint8_t *b, size_t off,
			   const struct dect_callinfo *ci)
{
	size_t next = dect_callinfo_put(b, DECT_LAPC_MSG_MAX, off, ci);

	return next ? next : off;
}

int dect_cc_setup(struct dect_cc *cc, const uint8_t *port_id, size_t port_id_len,
		  const uint8_t *fixed_id, size_t fixed_id_len,
		  uint8_t basic_service)
{
	return dect_cc_setup_id(cc, port_id, port_id_len, fixed_id,
				fixed_id_len, basic_service, NULL);
}

int dect_cc_setup_id(struct dect_cc *cc, const uint8_t *port_id,
		     size_t port_id_len, const uint8_t *fixed_id,
		     size_t fixed_id_len, uint8_t basic_service,
		     const struct dect_clip *id)
{
	unsigned idx;
	uint8_t *b;
	size_t off;

	if (cc->state != DECT_CC_ST_NULL && cc->state != DECT_CC_ST_RELEASED)
		return -1;
	b = txq_slot(cc, &idx);
	if (!b)
		return -1;

	/*
	 * We are the originating side of this transaction, so the F bit is the
	 * originator value and the transaction value is ours to pick. Value 0
	 * is fine: the handset's own calls run in a separate transaction, and
	 * the two are told apart by the flag.
	 */
	cc->incoming = 1;
	cc->ti_flag = DECT_TI_ORIGINATOR;
	cc->ti_value = cc->next_ti;
	cc->chosen_codec = 0;		/* the handset names it, see rx */

	off = dect_nwk_build_hdr(b, DECT_LAPC_MSG_MAX, DECT_PD_CC,
				 cc->ti_flag, cc->ti_value, DECT_CC_SETUP);
	if (!off)
		return -1;
	off = dect_nwk_put_ie(b, DECT_LAPC_MSG_MAX, off,
			      DECT_IE_PORTABLE_IDENTITY, port_id, port_id_len);
	if (!off)
		return -1;
	off = dect_nwk_put_ie(b, DECT_LAPC_MSG_MAX, off,
			      DECT_IE_FIXED_IDENTITY, fixed_id, fixed_id_len);
	if (!off)
		return -1;
	/*
	 * Caller identity, and never fatal: if either IE does not fit, the
	 * call still rings without it. Placement is before or after
	 * <<BASIC-SERVICE>> depending on the identity's placement hint.
	 */
	if (id && id->emit_before_basic_service)
		off = put_identity(b, off, id);

	off = dect_nwk_put_fixed_ie(b, DECT_LAPC_MSG_MAX, off,
				    DECT_IE_BASIC_SERVICE, basic_service);
	if (!off)
		return -1;
	if (cc->signal_on) {
		off = dect_nwk_put_fixed_ie(b, DECT_LAPC_MSG_MAX, off,
					    DECT_IE_SIGNAL, cc->signal_value);
		if (!off)
			return -1;
	}

	/*
	 * <<CODEC-LIST>> (EN 300 175-5 §7.7.54), optional in {CC-SETUP} and
	 * listed there alongside <<BASIC-SERVICE>>.
	 *
	 * We used to send none, and the Gigaset A540 answers our page, takes
	 * the SETUP and then returns {CC-RELEASE-COM} with release reason
	 * 0x32. The standard puts 0x32 in the TEMPORARY OVERLOAD block --
	 * "insufficient resources", next to "insufficient bearers available"
	 * -- which reads as a handset that cannot allocate what the call
	 * needs, not one refusing us. A handset that is told nothing about
	 * the codec has to guess what to reserve. The FRITZ!Fon C4 rings
	 * without it, but it is lenient in other ways too.
	 *
	 * One codec, G.726 32 kbit/s, full slot: the same list stock
	 * advertises, byte for byte [CR A-0059].
	 */
	/*
	 * ORDER MATTERS, and this block used to get it wrong. EN 300 175-5
	 * §7.6: "the variable-length information elements are transmitted
	 * over the air according to the ascending numerical order of their
	 * identifiers. This allows receiving equipment to detect the presence
	 * or absence of a particular information element without scanning
	 * through an entire message." A receiver is therefore entitled to
	 * stop looking once it has passed the identifier it wants.
	 *
	 * The identity is 0x6C/0x6D and the codec list is 0x7C, so the
	 * identity goes FIRST. Emitting the codec list first -- which this
	 * did between 893a70e and now, both landing 2026-10-23 -- puts a
	 * descending step in the message right before the caller ID, which is
	 * exactly the shape that makes a conforming handset skip it.
	 *
	 * Neither IE is fatal: if either does not fit, the call still rings.
	 */
	if (id && !id->emit_before_basic_service)
		off = put_identity(b, off, id);

	if (cc->codec_list_len) {
		off = dect_nwk_put_ie(b, DECT_LAPC_MSG_MAX, off,
				      DECT_IE_CODEC_LIST,
				      cc->codec_list, cc->codec_list_len);
		if (!off)
			return -1;
	}

	/* <<CALL-INFORMATION>> is 0x7E, so it is last and stays last. */
	off = put_callinfo(b, off, &cc->callinfo);

	/* Keep a copy: §9.5.2.3 may need this exact message sent again. */
	memcpy(cc->setup_msg, b, off);
	cc->setup_len = off;
	cc->represent_left = cc->represent_max;

	txq_commit(cc, idx, off);
	cc->state = DECT_CC_ST_CALL_PRESENT;
	return 0;
}

void dect_cc_set_callinfo(struct dect_cc *cc, const struct dect_callinfo *ci)
{
	if (ci)
		cc->callinfo = *ci;
	else
		dect_callinfo_init(&cc->callinfo);
}

void dect_cc_set_next_ti(struct dect_cc *cc, uint8_t ti)
{
	cc->next_ti = (uint8_t)(ti % 6u);	/* 6 is connectionless only */
}

int dect_cc_busy(const struct dect_cc *cc)
{
	return cc->state != DECT_CC_ST_NULL && cc->state != DECT_CC_ST_RELEASED;
}

void dect_cc_set_signal(struct dect_cc *cc, int enable, uint8_t value)
{
	cc->signal_on = (uint8_t)(enable != 0);
	cc->signal_value = value;
}

/* Copy an identity IE's contents out of a received message, if it is there. */
static void learn_id(const uint8_t *ies, size_t ies_len, uint8_t id,
		     uint8_t *dst, size_t *dst_len)
{
	size_t n;
	const uint8_t *p = dect_nwk_find_ie(ies, ies_len, id, &n);

	if (!p || n == 0 || n > DECT_CC_ID_MAX)
		return;
	memcpy(dst, p, n);
	*dst_len = n;
}

/*
 * The handset's offer, for the choice on_setup() makes. <<CODEC-LIST>>
 * (EN 300 175-5 §7.7.54): a negotiation octet, then 3 octets per codec --
 * identifier, MAC/DLC service, last-flag + C-plane routing + slot size.
 */
#define CODEC_ID_G726		DECT_CC_CODEC_G726
#define CODEC_ID_G722		DECT_CC_CODEC_G722
#define CODEC_LAST		0x80u	/* octet c, bit 8: last in the list */
#define CODEC_NEGOTIATION	0x90u	/* "codec negotiation possible" */

/*
 * Our choice, made when the SETUP arrives rather than when CONNECT is
 * built, so an owner that installs the U-plane before CONNECT (EN 300
 * 175-5 §9.3.1.8) already knows which audio path to bind: G.722 when the
 * link carries wideband and the handset offered it, else G.726; 0 when
 * the SETUP did not negotiate or offered neither.
 */
static void choose_codec(struct dect_cc *cc)
{
	if (!cc->peer_negotiates)
		cc->chosen_codec = 0;
	else if (cc->wideband && cc->peer_has_g722)
		cc->chosen_codec = CODEC_ID_G722;
	else if (cc->peer_has_g726)
		cc->chosen_codec = CODEC_ID_G726;
	else
		cc->chosen_codec = 0;
}

static void learn_codec_offer(struct dect_cc *cc, const uint8_t *ies,
			      size_t ies_len)
{
	size_t n, i;
	const uint8_t *p = dect_nwk_find_ie(ies, ies_len, DECT_IE_CODEC_LIST,
					    &n);

	cc->peer_has_g726 = 0;
	cc->peer_has_g722 = 0;
	cc->chosen_codec = 0;
	cc->peer_negotiates = p != NULL ||
		(cc->peer_basic_service != DECT_CC_SVC_NONE &&
		 (cc->peer_basic_service & 0x0Fu) == 0x8u);
	if (!p) {
		/*
		 * Wideband service, no list here: the negotiation runs on the
		 * list from location registration, where G.726 is mandatory
		 * (§7.3.1). Its entry, as every handset we hold sends it:
		 * full slot, MAC/DLC service 0.
		 */
		static const uint8_t g726[3] = { CODEC_ID_G726, 0x00u, 0x04u };
		/* and G.722 on a long slot, the service it asked for */
		static const uint8_t g722[3] = { CODEC_ID_G722, 0x00u, 0x01u };

		memcpy(cc->peer_g726, g726, sizeof(g726));
		cc->peer_has_g726 = cc->peer_negotiates;
		memcpy(cc->peer_g722, g722, sizeof(g722));
		cc->peer_has_g722 = cc->peer_negotiates;
		choose_codec(cc);
		return;
	}
	for (i = 1; i + 3u <= n; i += 3u) {
		if (p[i] == CODEC_ID_G726 && !cc->peer_has_g726) {
			memcpy(cc->peer_g726, &p[i], 3);
			cc->peer_has_g726 = 1;
		} else if (p[i] == CODEC_ID_G722 && !cc->peer_has_g722) {
			memcpy(cc->peer_g722, &p[i], 3);
			cc->peer_has_g722 = 1;
		}
	}
	choose_codec(cc);
}

static void learn_basic_service(struct dect_cc *cc, const uint8_t *ies,
				size_t ies_len)
{
	int v = dect_nwk_find_fixed_ie(ies, ies_len, DECT_IE_BASIC_SERVICE);

	cc->peer_basic_service = (v < 0) ? DECT_CC_SVC_NONE : (uint8_t)v;
}

/*
 * EN 300 175-5 §7.7.5 figure 18. BOTH nibbles have to agree before a call
 * gets an audio path.
 *
 * The low nibble alone is not enough, and that is the whole point of this
 * function: a CAT-iq handset's list-access call carries call class 0010 with
 * basic service 1000 "wideband speech", so reading the service nibble by
 * itself calls a phonebook read a wideband voice call. Ours did, 145 times.
 *
 * Call classes that carry speech: normal (1000), internal (1001), emergency
 * (1010) and external handover (1100). The rest -- LiA (0010), ULE (0011),
 * message call (0100), DECT/ISDN IIP (0111), service call (1011),
 * supplementary service (1101) and OA&M (1110) -- do not, whatever their
 * service nibble says. DECT_CC_SVC_NONE is 0xFF, whose class nibble is
 * reserved, so an absent element falls through to 0 on its own.
 */
int dect_cc_peer_is(const struct dect_cc *cc, const uint8_t *port_id,
		    size_t port_id_len)
{
	if (!port_id_len)
		return 1;
	if (cc->peer_port_id_len != port_id_len)
		return 0;
	return memcmp(cc->peer_port_id, port_id, port_id_len) == 0;
}

int dect_cc_peer_is_speech(const struct dect_cc *cc)
{
	switch (cc->peer_basic_service >> 4) {
	case 0x8u:	/* normal call setup */
	case 0x9u:	/* internal call setup */
	case 0xAu:	/* emergency call setup */
	case 0xCu:	/* external handover call setup */
		break;
	default:
		return 0;
	}
	switch (cc->peer_basic_service & 0x0Fu) {
	case 0x0u:	/* basic speech, default setup attributes */
	case 0x8u:	/* wideband speech, default setup attributes */
	case 0xBu:	/* DTAM wideband speech */
		return 1;
	default:
		return 0;
	}
}

/*
 * Collect <<MULTI-KEYPAD>> from a received message. GAP §8.10: the PT sends
 * keypad information "in one or several {CC-INFO} messages", and the FT is
 * mandated to accept it in states F-02 (call present) and F-10 (active).
 * EN 300 175-5 §9.3.1.5 also allows it in the initial {CC-SETUP}, so we take
 * it from both and simply append in arrival order.
 */
static void learn_keypad(struct dect_cc *cc, const uint8_t *ies, size_t ies_len)
{
	size_t n, room;
	const uint8_t *p = dect_nwk_find_ie(ies, ies_len,
					    DECT_IE_MULTI_KEYPAD, &n);

	if (!p || !n)
		return;
	room = sizeof(cc->keypad) - cc->keypad_len;
	if (n > room) {
		n = room;
		cc->keypad_overflow = 1;
	}
	if (!n)
		return;
	memcpy(cc->keypad + cc->keypad_len, p, n);
	cc->keypad_len += n;
}

/*
 * The internal-call key, 0x17 (EN 300 444 §8.19), first in the keypad of a
 * call the handset placed: the call is an internal one, whatever its SETUP
 * said. TS 102 527-3 §7.4.10.6.2 and figure C.21: a handset that picked
 * the party from the Internal Names List sends "17H + number" in one
 * {CC-INFO} on its list-access call, and "the FP shall maintain the call"
 * -- the A540 does exactly that (bench test 19, 2026-10-28). Re-classed as
 * call class 9, keeping the speech service, it takes the internal-call
 * path; a list-access call has had its CALL-PROC already.
 */
/* The handset's line choice from <<CALL-INFORMATION>>; the first counts. */
static void learn_peer_line(struct dect_cc *cc, const uint8_t *ies,
			    size_t ies_len)
{
	struct dect_callinfo ci;
	size_t n;
	const uint8_t *p = dect_nwk_find_ie(ies, ies_len,
					    DECT_IE_CALL_INFORMATION, &n);

	if (cc->incoming || cc->peer_line != DECT_CC_LINE_UNSET || !p)
		return;
	(void)dect_callinfo_parse(p, n, &ci);
	if (!ci.has_line)
		return;
	cc->peer_line = ci.line_id;
	/*
	 * "The PP shall not select a line it is not attached to (in that case
	 * the FP shall disconnect the call)" (TS 102 527-3 §7.4.5.2.1).
	 */
	if (ci.line_id != DECT_CALLINFO_LINE_NONE &&
	    !dect_iwu_attached(&cc->iwu, ci.line_id, cc->iwu.own_terminal_id))
		cc->line_refused = 1;
}

/*
 * The release for a line the handset may not use. The clause names no
 * release reason; "incompatible service" (05H, EN 300 175-5 §7.6.7) is
 * the closest defined one. Abnormal release, as for any refused setup.
 */
static void refuse_line(struct dect_cc *cc)
{
	queue_release_com(cc, DECT_CC_REASON_INCOMPATIBLE_SERVICE);
	cc->state = DECT_CC_ST_NULL;
	cc->line_refused = 0;
	cc->stats.line_refused++;
}

void dect_cc_set_default_line(struct dect_cc *cc, uint8_t line_id,
			      uint8_t line_type)
{
	cc->default_line_set = 1;
	cc->default_line = (uint8_t)(line_id & 0x7Fu);
	cc->default_line_type = line_type;
}

uint8_t dect_cc_line(const struct dect_cc *cc)
{
	if (cc->peer_line == DECT_CALLINFO_LINE_NONE)
		return cc->default_line_set ? cc->default_line
					    : DECT_CC_LINE_UNSET;
	return cc->peer_line;
}

static void learn_int_key(struct dect_cc *cc)
{
	unsigned cls = cc->peer_basic_service >> 4;

	/*
	 * The external counterpart, "1C15H + number" (TS 102 527-3 table 17,
	 * §7.4.10.6.2): a number picked from a list (contacts, call lists) on
	 * the list-access call. The call becomes a normal one (class 8, the
	 * speech service kept) and the prefix is dropped, so the keypad holds
	 * the number only; it arrives "in a single CC-INFO", so it is complete.
	 * Elsewhere 1C15H starts a parallel call, which we do not handle yet.
	 */
	if (!cc->incoming && cls == 0x2u && cc->keypad_len > 2u &&
	    cc->keypad[0] == 0x1Cu && cc->keypad[1] == 0x15u &&
	    cc->peer_basic_service != DECT_CC_SVC_NONE) {
		memmove(cc->keypad, cc->keypad + 2, cc->keypad_len - 2u);
		cc->keypad_len -= 2u;
		if (cc->state == DECT_CC_ST_PROCEEDING)
			cc->proceeded = 1;
		cc->peer_basic_service =
			(uint8_t)(0x80u | (cc->peer_basic_service & 0x0Fu));
		cc->list_number = 1;
		return;
	}
	if (cc->incoming || !cc->keypad_len || cc->keypad[0] != 0x17u)
		return;
	if (cc->peer_basic_service == DECT_CC_SVC_NONE ||
	    (cls != 0x2u && cls != 0x8u))
		return;
	if (cls == 0x2u && cc->state == DECT_CC_ST_PROCEEDING)
		cc->proceeded = 1;
	cc->peer_basic_service =
		(uint8_t)(0x90u | (cc->peer_basic_service & 0x0Fu));
}

const uint8_t *dect_cc_keypad(const struct dect_cc *cc, size_t *len)
{
	if (len)
		*len = cc->keypad_len;
	return cc->keypad_len ? cc->keypad : NULL;
}

void dect_cc_clear_keypad(struct dect_cc *cc)
{
	cc->keypad_len = 0;
	cc->keypad_overflow = 0;
}

/*
 * {CC-CONNECT} for a call the handset placed. If its SETUP negotiated, it
 * names the codec we chose, alone (TS 102 527-1 §7.3.3: "at latest" in
 * CC-CONNECT, "only one codec shall be in the list", negotiation octet
 * "codec negotiation possible"). G.722 when the link carries wideband and
 * the handset offered it, else G.726, from the handset's own entry; if it
 * offered neither we name nothing rather than a codec it did not offer.
 */
static void queue_connect(struct dect_cc *cc)
{
	unsigned idx;
	uint8_t *b;
	size_t off;
	uint8_t cl[4];
	int g722 = cc->chosen_codec == CODEC_ID_G722;

	if (!cc->peer_negotiates) {
		queue_with_status(cc, DECT_CC_CONNECT, DECT_CS_CALL_CONNECT);
		return;
	}
	if (!cc->chosen_codec) {
		cc->stats.codec_unmatched++;
		queue_with_status(cc, DECT_CC_CONNECT, DECT_CS_CALL_CONNECT);
		return;
	}
	b = txq_slot(cc, &idx);
	if (!b)
		return;
	off = dect_nwk_build_hdr(b, DECT_LAPC_MSG_MAX, DECT_PD_CC,
				 DECT_TI_RESPONDER, cc->ti_value,
				 DECT_CC_CONNECT);
	cl[0] = CODEC_NEGOTIATION;
	memcpy(&cl[1], g722 ? cc->peer_g722 : cc->peer_g726, 3);
	cl[3] |= CODEC_LAST;
	if (off)
		off = dect_nwk_put_ie(b, DECT_LAPC_MSG_MAX, off,
				      DECT_IE_CODEC_LIST, cl, sizeof(cl));
	if (!off)
		return;
	/* 0x7E after 0x7C: CS call connect, the handset's timer (A-0122) */
	off = dect_cc_put_call_status(cc, b, DECT_LAPC_MSG_MAX, off,
				      DECT_CS_CALL_CONNECT);
	txq_commit(cc, idx, off);
}

/*
 * The handset's choice for a call we placed: the first entry of the
 * <<CODEC-LIST>> in its {CC-ALERTING} or {CC-CONNECT} (TS 102 527-1
 * §7.3.3, "at latest" in CONNECT). A later list wins.
 */
static void learn_codec_choice(struct dect_cc *cc, const uint8_t *ies,
			       size_t ies_len)
{
	size_t n;
	const uint8_t *p = dect_nwk_find_ie(ies, ies_len, DECT_IE_CODEC_LIST,
					    &n);

	if (p && n >= 4u)
		cc->chosen_codec = p[1];
}

void dect_cc_set_call_status(struct dect_cc *cc, int on, uint8_t call_id)
{
	cc->call_status = on ? 1u : 0u;
	cc->status_call_id = call_id & 0x7Fu;
}

void dect_cc_set_wideband(struct dect_cc *cc, int on)
{
	cc->wideband = on ? 1u : 0u;
}

uint8_t dect_cc_codec(const struct dect_cc *cc)
{
	return cc->chosen_codec;
}

static void accept_now(struct dect_cc *cc);
static void proceed(struct dect_cc *cc);

/* {CC-INFO}: <<SIGNAL>>, then <<CALL-INFORMATION>> {call id, status, reason} */
static void queue_busy_info(struct dect_cc *cc, uint8_t signal, uint8_t status,
			    int with_reason)
{
	struct dect_callinfo ci;
	unsigned idx;
	uint8_t *b = txq_slot(cc, &idx);
	size_t off, next;

	if (!b)
		return;
	off = dect_nwk_build_hdr(b, DECT_LAPC_MSG_MAX, DECT_PD_CC,
				 cc->ti_flag, cc->ti_value, DECT_CC_INFO);
	off = dect_nwk_put_fixed_ie(b, DECT_LAPC_MSG_MAX, off,
				    DECT_IE_SIGNAL, signal);
	dect_callinfo_init(&ci);
	ci.has_call_id = 1;
	ci.call_id = cc->status_call_id;
	ci.has_call_status = 1;
	ci.call_status = status;
	ci.has_status_reason = (uint8_t)(with_reason != 0);
	ci.status_reason = DECT_CSR_SYSTEM_BUSY;
	next = dect_callinfo_put(b, DECT_LAPC_MSG_MAX, off, &ci);
	if (!off || !next)
		return;
	txq_commit(cc, idx, next);
}

/*
 * TS 102 527-3 §7.4.8.3, "busy system" for a Part 3 handset's call when
 * we have no room: a CC message other than {CC-INFO} first (our
 * CALL-PROC), then {CC-INFO} with the busy tone (04H) and CS call
 * disconnecting, reason "system busy"; the tick releases it later. A GAP
 * or Part 1 handset gets the plain {CC-RELEASE-COM} "insufficient
 * resources" instead (it takes no call status). Spec sweep, A2 K8.
 */
#define CC_BUSY_TONE_MS		5000u	/* "after some time out", ours */

static void busy_notify(struct dect_cc *cc)
{
	proceed(cc);
	queue_busy_info(cc, 0x04u, DECT_CS_CALL_DISCONNECTING, 1);
	cc->busy_notified = 1;
	cc->busy_deadline_ms = 0;		/* armed by the tick */
}

static void on_setup(struct dect_cc *cc, int sending_complete)
{
	cc->stats.setup++;
	/*
	 * A list-access service call (call class 0010, EN 300 175-5 7.6.4):
	 * TS 102 527-3 7.4.10.6.1 -- "the FP shall immediately answer with a
	 * {CC-CALL-PROC}, without call status nor call id", and "the LiA
	 * service call is not connected ({CC-CALL PROC} is used, but no
	 * {CC-CONNECT} is used)". We used to answer it like a voice call,
	 * CALL-PROC then CONNECT, telling the handset a call it never placed
	 * was through. The list dialog then rides this transaction until the
	 * handset releases it.
	 */
	if ((cc->peer_basic_service >> 4) == 0x2u) {
		if (cc->answered != DECT_CC_CALL_PROC)
			queue_msg(cc, DECT_TI_RESPONDER, DECT_CC_CALL_PROC);
		cc->state = DECT_CC_ST_PROCEEDING;
		return;
	}
	if (cc->busy) {
		cc->stats.rejected_busy++;
		if (cc->call_status) {		/* a Part 3 handset */
			busy_notify(cc);
			return;
		}
		queue_release_com(cc, DECT_CC_REASON_INSUFFICIENT_RESOURCES);
		cc->state = DECT_CC_ST_NULL;
		return;
	}
	/*
	 * An internal call (call class 9, GAP N.31) is never connected here:
	 * the owner has to reach the other handset first. A complete number
	 * -- or any number without overlap receiving -- gets CALL-PROC and
	 * waits in PROCEEDING for dect_cc_alert/connect().
	 */
	if ((cc->peer_basic_service >> 4) == 0x9u &&
	    (sending_complete || !cc->overlap)) {
		proceed(cc);	/* with or without overlap receiving */
		return;
	}
	if (cc->overlap && !sending_complete) {
		if (cc->answered != DECT_CC_SETUP_ACK)
			queue_with_status(cc, DECT_CC_SETUP_ACK,
					  DECT_CS_CALL_SETUP_ACK);
		cc->state = DECT_CC_ST_OVERLAP;
		cc->overlap_deadline_ms = 0;	/* armed by the tick */
		return;
	}
	accept_now(cc);
}

/*
 * {CC-CALL-PROC}: "I am handling it" (EN 300 175-5 §9.3.1.6). Stops
 * F<CC.01>; once only, and not when whoever held the SETUP already sent it.
 */
static void proceed(struct dect_cc *cc)
{
	if (!cc->proceeded && cc->answered != DECT_CC_CALL_PROC)
		queue_with_status(cc, DECT_CC_CALL_PROC, DECT_CS_CALL_PROC);
	cc->proceeded = 1;
	cc->answered = 0;
	cc->state = DECT_CC_ST_PROCEEDING;
	cc->overlap_deadline_ms = 0;
}

/*
 * {CC-CONNECT}, and with it ACTIVE: EN 300 175-5 §9.3.1.8 has the F-CC
 * "send a {CC-CONNECT} message to the P-CC. It shall then enter the
 * ACTIVE state." No {CC-CONNECT-ACK} comes for a call the handset placed
 * -- the A540 sends none (bench test 12b) -- so waiting in CONNECT PENDING
 * for one left us, and the Monitor, at "connecting" for the whole call.
 */
static void connect_now(struct dect_cc *cc)
{
	queue_connect(cc);
	cc->state = DECT_CC_ST_ACTIVE;
	cc->stats.connected++;
}

/*
 * Accept the call: CALL-PROC then CONNECT (we are the far end for the
 * loopback test, so we answer at once). Both are responder-side in the
 * PP-originated transaction.
 */
static void accept_now(struct dect_cc *cc)
{
	proceed(cc);
	connect_now(cc);
}

/* a handset-placed call that can be connected: not list access */
static int connectable(const struct dect_cc *cc)
{
	return !cc->incoming && !cc->busy_notified &&
	       (cc->peer_basic_service >> 4) != 0x2u &&
	       (cc->state == DECT_CC_ST_OVERLAP ||
		cc->state == DECT_CC_ST_PROCEEDING);
}

int dect_cc_proceed(struct dect_cc *cc)
{
	if (!connectable(cc))
		return -1;
	proceed(cc);
	return 0;
}

int dect_cc_alert(struct dect_cc *cc)
{
	if (!connectable(cc) || cc->alerted)
		return -1;
	proceed(cc);
	queue_with_status(cc, DECT_CC_ALERTING, DECT_CS_CALL_ALERTING);
	cc->alerted = 1;
	return 0;
}

int dect_cc_connect(struct dect_cc *cc)
{
	if (!connectable(cc))
		return -1;
	accept_now(cc);
	return 0;
}

void dect_cc_set_overlap(struct dect_cc *cc, int on)
{
	cc->overlap = on ? 1u : 0u;
}

void dect_cc_note_answered(struct dect_cc *cc, uint8_t msg_type)
{
	cc->answered = msg_type;
}

int dect_cc_accept(struct dect_cc *cc)
{
	return dect_cc_connect(cc);
}

static void on_connect_ack(struct dect_cc *cc)
{
	if (cc->state != DECT_CC_ST_CONNECT_PENDING)
		return;
	cc->state = DECT_CC_ST_ACTIVE;
	cc->stats.connected++;
}

static void on_release(struct dect_cc *cc)
{
	queue_msg(cc, cc->ti_flag, DECT_CC_RELEASE_COM);
	cc->state = DECT_CC_ST_RELEASED;
	cc->stats.released++;
	/* "terminated at latest with call release" (TS 102 527-3 7.4.10.4.1.3) */
	dect_iwu_reset_sessions(&cc->iwu);
}


/* EN 300 175-5 A.1: CC setup timer, 20 s for FT and PT alike. */
#define CC_SETUP_TIMER_MS	20000u
/* Release reason "timer expiry" (EN 300 175-5 7.7.18). */
#define RELEASE_REASON_TIMER_EXPIRY	0x0Du

/* EN 300 175-5 A.1: F<CC.01>, overlap sending, 20 s. */
#define CC_OVERLAP_TIMER_MS	20000u

#define CC_RELEASE_TIMER_MS	36000u	/* <CC.02>, EN 300 175-5 A.1 */

void dect_cc_tick(struct dect_cc *cc, uint64_t now_ms)
{
	if (cc->release_armed && cc->state == DECT_CC_ST_RELEASED) {
		if (!cc->release_deadline_ms) {
			cc->release_deadline_ms = now_ms + CC_RELEASE_TIMER_MS;
		} else if (now_ms >= cc->release_deadline_ms) {
			/* §9.5.1: no RELEASE-COM came; send it and go to NULL */
			queue_release_com(cc, RELEASE_REASON_TIMER_EXPIRY);
			cc->state = DECT_CC_ST_NULL;
			cc->release_armed = 0;
			cc->release_deadline_ms = 0;
			cc->stats.release_timeouts++;
		}
	} else {
		cc->release_armed = 0;		/* answered, or a new call */
		cc->release_deadline_ms = 0;
	}
	if (cc->busy_notified && cc->state == DECT_CC_ST_PROCEEDING) {
		if (!cc->busy_deadline_ms) {
			cc->busy_deadline_ms = now_ms + CC_BUSY_TONE_MS;
		} else if (now_ms >= cc->busy_deadline_ms) {
			/* tones off, CS idle, then the release (§7.4.8.3) */
			queue_busy_info(cc, 0x3Fu, DECT_CS_IDLE, 0);
			cc->busy_notified = 0;
			cc->busy_deadline_ms = 0;
			(void)dect_cc_release(cc);
		}
	} else if (cc->busy_notified && cc->state != DECT_CC_ST_PROCEEDING) {
		cc->busy_notified = 0;		/* the handset hung up first */
		cc->busy_deadline_ms = 0;
	}
	if (cc->state == DECT_CC_ST_OVERLAP) {
		if (!cc->overlap_deadline_ms) {
			cc->overlap_deadline_ms = now_ms + CC_OVERLAP_TIMER_MS;
		} else if (now_ms >= cc->overlap_deadline_ms) {
			/*
			 * GAP 8.3.2.3 and EN 300 175-5 9.3.1.5: the NORMAL
			 * release, {CC-RELEASE} "timer expiry", then release
			 * pending until the handset's {CC-RELEASE-COM}. We sent
			 * {CC-RELEASE-COM} at once, the abnormal release, while
			 * this comment already named the clause (spec sweep,
			 * 2026-10-28).
			 */
			cc->overlap_deadline_ms = 0;
			queue_release_msg(cc, DECT_CC_RELEASE,
					  RELEASE_REASON_TIMER_EXPIRY);
			cc->state = DECT_CC_ST_RELEASED;
			cc->stats.released++;
			cc->stats.overlap_timeouts++;
			cc->release_armed = 1;		/* <CC.02> */
			cc->release_deadline_ms = 0;
		}
	}
	/*
	 * The timer runs only while an FP-originated SETUP is unanswered.
	 * "Stop: a response message has been received" (A.1), and the first
	 * response moves us out of CALL PRESENT -- to RINGING on CC-ALERTING,
	 * to ACTIVE on CC-CONNECT, to RELEASED on a release -- so the state is
	 * the stop condition and no separate clearing is needed.
	 *
	 * It is armed here rather than where the SETUP is queued so that
	 * dect_cc_setup_id() keeps its signature; the tick runs every loop
	 * pass, so the start is late by well under a millisecond against a
	 * 20 second timer.
	 */
	if (!cc->incoming || cc->state != DECT_CC_ST_CALL_PRESENT) {
		cc->setup_deadline_ms = 0;
		return;
	}
	if (!cc->setup_deadline_ms) {
		cc->setup_deadline_ms = now_ms + CC_SETUP_TIMER_MS;
		return;
	}
	if (now_ms < cc->setup_deadline_ms)
		return;
	cc->setup_deadline_ms = 0;
	queue_release_com(cc, RELEASE_REASON_TIMER_EXPIRY);
	cc->state = DECT_CC_ST_NULL;
	cc->stats.setup_timeouts++;
}

uint64_t dect_cc_next_deadline(const struct dect_cc *cc)
{
	uint64_t a = cc->setup_deadline_ms ? cc->setup_deadline_ms : UINT64_MAX;
	uint64_t b = cc->overlap_deadline_ms ? cc->overlap_deadline_ms
					     : UINT64_MAX;
	uint64_t c = cc->busy_deadline_ms ? cc->busy_deadline_ms : UINT64_MAX;
	uint64_t d = cc->release_deadline_ms ? cc->release_deadline_ms
					     : UINT64_MAX;

	if (b < a)
		a = b;
	if (c < a)
		a = c;
	return d < a ? d : a;
}

void dect_cc_set_busy(struct dect_cc *cc, int busy)
{
	cc->busy = busy ? 1u : 0u;
}

void dect_cc_set_gate(struct dect_cc *cc, const uint8_t *port_id,
		      size_t port_id_len)
{
	cc->gate_armed = 1;
	if (!port_id || port_id_len > sizeof(cc->gate_id))
		port_id_len = 0;	/* armed, nothing matches */
	if (port_id_len)
		memcpy(cc->gate_id, port_id, port_id_len);
	cc->gate_id_len = port_id_len;
}

/*
 * Turn away a {CC-SETUP} from a handset the gate does not recognise; true if
 * it was turned away. The reply closes the peer's transaction, not ours, so
 * it goes out like any other transaction we hold no call for.
 */
static int setup_from_stranger(struct dect_cc *cc,
			       const struct dect_nwk_hdr *h,
			       const uint8_t *ies, size_t ies_len)
{
	size_t n;
	const uint8_t *p;

	if (!cc->gate_armed)
		return 0;
	p = dect_nwk_find_ie(ies, ies_len, DECT_IE_PORTABLE_IDENTITY, &n);
	if (p && cc->gate_id_len && n == cc->gate_id_len &&
	    !memcmp(p, cc->gate_id, n))
		return 0;
	queue_release_other(cc, h->ti_flag, h->ti_value,
			    p ? DECT_CC_REASON_UNKNOWN_IDENTITY
			      : DECT_CC_REASON_MANDATORY_IE_MISSING);
	cc->stats.rejected_identity++;
	return 1;
}

/*
 * Does a received message belong to the call this entity holds? The peer's
 * messages on our transaction carry our value and the INVERSE of our flag:
 * we put cc->ti_flag in what we send, the peer puts the other one. With no
 * call held, nothing is ours.
 *
 * Every handler below asks this first. They used not to: a {CC-RELEASE} on
 * any transaction released our call -- the handset's own list-access call
 * ending while ours rang would have killed ours -- and the default branch
 * tested the flag the wrong way round, so an unhandled message on our own
 * call closed it as an "unknown transaction". Found by the external review
 * (the clean-room record/an external review-24102026.md, finding 3).
 */
static int our_transaction(const struct dect_cc *cc,
			   const struct dect_nwk_hdr *h)
{
	return cc->state != DECT_CC_ST_NULL &&
	       h->ti_value == cc->ti_value && h->ti_flag != cc->ti_flag;
}

/*
 * EN 300 175-5 §17.3.2.1: a CC message other than SETUP, RELEASE and
 * RELEASE-COM on a transaction we do not hold is cleared with
 * {CC-RELEASE-COM} "unknown transaction identifier", on that transaction.
 */
static void close_unknown(struct dect_cc *cc, const struct dect_nwk_hdr *h)
{
	queue_release_other(cc, h->ti_flag, h->ti_value,
			    DECT_CC_REASON_UNKNOWN_TRANSACTION);
	cc->stats.closed_unknown++;
}

void dect_cc_rx(struct dect_cc *cc, const uint8_t *msg, size_t len)
{
	struct dect_nwk_hdr h;
	const uint8_t *ies;
	size_t ies_len;

	if (dect_nwk_parse(msg, len, &h, &ies, &ies_len) < 0 || h.pd != DECT_PD_CC)
		return;
	/*
	 * TV 6 is the connectionless (dummy) transaction value, never a call
	 * (EN 300 175-5 §7.3). On it only a {FACILITY} carrying <<FACILITY>>,
	 * <<EVENTS-NOTIFICATION>>, <<TIME-DATE>> or <<IWU-TO-IWU>> means
	 * anything; everything else "shall" be disregarded (§10.4.2.3), and
	 * CC serves none of those. The Gigasets sent a FACILITY of AVM's
	 * proprietary escape there until 2026-10-24 (call-g7.md, [CR A-0055]),
	 * which we cleared as an unknown call (13fa578) and so cycled the link.
	 */
	if (h.ti_value == 6u)
		return;

	switch (h.msg_type) {
	case DECT_CC_SETUP:
		/*
		 * The originating side sends F = 0: a {CC-SETUP} with the flag
		 * set to 1 is ignored (EN 300 175-5 §17.3.2.1), and is not a
		 * collision either (§17.3.3 asks for the flag "correctly set
		 * to 0"). We used to take it as a new call (spec sweep, C8).
		 */
		if (h.ti_flag != DECT_TI_ORIGINATOR)
			break;
		/*
		 * A released call leaves its transaction free: answering a
		 * {CC-RELEASE} with {CC-RELEASE-COM} returns the FT to the null
		 * state (EN 300 175-5 §9.5.1). Accepting a SETUP only in NULL
		 * ignored the next call on the same link -- a redial, or a voice
		 * call after a list-access call -- for as long as the link
		 * stayed up (spec sweep, 2026-10-28). RELEASED stays visible as
		 * "call ended" until then.
		 */
		if (cc->state == DECT_CC_ST_NULL ||
		    cc->state == DECT_CC_ST_RELEASED) {
			if (setup_from_stranger(cc, &h, ies, ies_len)) {
				cc->answered = 0;
				break;
			}
			/*
			 * <<BASIC-SERVICE>> is mandatory in a {CC-SETUP}
			 * (EN 300 175-5 §6.3.2.1): without it, {CC-RELEASE-COM}
			 * "mandatory information element missing" (§17.6.1).
			 * We accepted and connected such a call (spec sweep,
			 * C9). A missing FIXED-IDENTITY is still tolerated, as
			 * §17.6.1 is a "should" and every Gigaset sends it.
			 */
			if (dect_nwk_find_fixed_ie(ies, ies_len,
						   DECT_IE_BASIC_SERVICE) < 0) {
				queue_release_other(cc, h.ti_flag, h.ti_value,
					DECT_CC_REASON_MANDATORY_IE_MISSING);
				cc->stats.ie_missing++;
				cc->answered = 0;
				break;
			}
			dect_cc_clear_keypad(cc);	/* digits are per call */
			cc->list_number = 0;
			cc->busy_notified = 0;
			cc->peer_line = DECT_CC_LINE_UNSET;
			cc->line_confirmed = 0;
			cc->line_refused = 0;
			cc->release_armed = 0;
			cc->ti_value = h.ti_value;
			cc->ti_flag = DECT_TI_RESPONDER;
			cc->incoming = 0;
			cc->proceeded = 0;
			cc->alerted = 0;
			learn_id(ies, ies_len, DECT_IE_PORTABLE_IDENTITY,
				 cc->peer_port_id, &cc->peer_port_id_len);
			learn_id(ies, ies_len, DECT_IE_FIXED_IDENTITY,
				 cc->peer_fixed_id, &cc->peer_fixed_id_len);
			learn_basic_service(cc, ies, ies_len);
			learn_codec_offer(cc, ies, ies_len);
			learn_keypad(cc, ies, ies_len);
			learn_int_key(cc);
			learn_peer_line(cc, ies, ies_len);	/* incoming = 0 now */
			if (cc->line_refused) {
				refuse_line(cc);
				cc->answered = 0;
				break;
			}
			on_setup(cc, dect_nwk_find_fixed_ie(ies, ies_len,
				 DECT_IE_SENDING_COMPLETE) >= 0);
			if (cc->state != DECT_CC_ST_OVERLAP)
				cc->answered = 0;	/* consumed */
		} else if (!our_transaction(cc, &h)) {
			/*
			 * A second call while one is in progress. The peer's
			 * messages on OUR transaction carry the inverse of our
			 * flag, so "same transaction" is same value AND the
			 * other flag; a retransmitted SETUP of the live call
			 * is not a collision, and a crossing SETUP with the
			 * same value but the same (originator) flag is -- the
			 * standard's NOTE 7: only F tells the two apart then.
			 *
			 * EN 300 175-5 §17.3.3: a {CC-SETUP} on a transaction
			 * that is not the call in progress is rejected with
			 * {CC-RELEASE-COM} "insufficient resources" and the
			 * call in progress continues. When OUR call is the
			 * one in progress and still in CALL PRESENT, this is
			 * the setup collision of §9.5.2.3 and the same release
			 * is the FT's prescribed half of resolving it: the PT
			 * releases our call with that reason, we release its
			 * call with that reason, and we re-present ours (see
			 * DECT_CC_RELEASE_COM below).
			 *
			 * We used to ignore the crossing SETUP, which left the
			 * handset's call open until its P<CC.03> expired 20 s
			 * later -- and left the handset waiting for a release
			 * that never came before our re-presentation.
			 */
			queue_release_other(cc, h.ti_flag, h.ti_value,
					    DECT_CC_REASON_INSUFFICIENT_RESOURCES);
			cc->stats.collisions++;
		}
		break;
	case DECT_CC_ALERTING:
		/* Incoming call: the handset is ringing. */
		if (!our_transaction(cc, &h)) {
			close_unknown(cc, &h);
			break;
		}
		if (cc->incoming &&
		    cc->state == DECT_CC_ST_CALL_PRESENT) {
			learn_codec_choice(cc, ies, ies_len);
			cc->state = DECT_CC_ST_RINGING;
			cc->stats.alerting++;
		}
		break;
	case DECT_CC_CONNECT:
		/*
		 * Incoming call: the user answered. The originating side
		 * confirms with CONNECT-ACK, and then the call is active and
		 * the audio bind is the same as for a handset-placed call.
		 */
		if (!our_transaction(cc, &h)) {
			close_unknown(cc, &h);
			break;
		}
		if (cc->incoming &&
		    (cc->state == DECT_CC_ST_CALL_PRESENT ||
		     cc->state == DECT_CC_ST_RINGING)) {
			learn_codec_choice(cc, ies, ies_len);
			queue_msg(cc, cc->ti_flag, DECT_CC_CONNECT_ACK);
			cc->state = DECT_CC_ST_ACTIVE;
			cc->stats.connected++;
		}
		break;
	case DECT_CC_CONNECT_ACK:
		if (our_transaction(cc, &h))
			on_connect_ack(cc);
		else
			close_unknown(cc, &h);
		break;
	case DECT_CC_RELEASE:
		/* EN 300 175-5 §17.3.2.1: unknown transaction, no action. */
		if (!our_transaction(cc, &h))
			break;
		/*
		 * A release collision: our own {CC-RELEASE} is unanswered
		 * (RELEASE PENDING) and the peer's crosses it. §9.5.3: stop
		 * <CC.02> and enter NULL, without the {CC-RELEASE-COM} of the
		 * normal procedure (GAP §8.7.2.1, figure 23). We answered it
		 * (spec sweep, C3).
		 */
		if (cc->release_armed && cc->state == DECT_CC_ST_RELEASED) {
			cc->release_armed = 0;
			cc->release_deadline_ms = 0;
			cc->state = DECT_CC_ST_NULL;
			cc->stats.release_collisions++;
			dect_iwu_reset_sessions(&cc->iwu);
			break;
		}
		on_release(cc);
		break;
	case DECT_CC_IWU_INFO: {
		/*
		 * <<IWU-TO-IWU>> session dialog. The peer opens sessions during
		 * call setup and we used to drop every one; see dect_iwu.h.
		 * Answered in any state of the call we hold, because a session
		 * open is not call state; the answers go out on that call's
		 * transaction (queue_iwu), so one on a transaction we do not
		 * hold is cleared instead (EN 300 175-5 §17.3.2.1). We answered
		 * it on ours (spec sweep, I3); no saved log has one.
		 */
		size_t n;
		const uint8_t *p = dect_nwk_find_ie(ies, ies_len,
						    DECT_IE_IWU_TO_IWU, &n);
		struct dect_iwu_out replies;
		unsigned i;

		if (!our_transaction(cc, &h)) {
			close_unknown(cc, &h);
			break;
		}
		if (!p)
			break;
		replies.count = 0;
		dect_iwu_handle(&cc->iwu, p, n, &replies);
		/* A confirm and its data packets, each in its own message. */
		for (i = 0; i < replies.count; i++)
			queue_iwu(cc, replies.msg[i], replies.len[i]);
		break;
	}
	case DECT_CC_INFO:
		/*
		 * GAP N.4 "dialled digits (basic)", procedure §8.10 -- the one
		 * mandatory GAP feature this project was missing. No reply is
		 * sent: §8.10's figure shows the FT staying in the same state,
		 * and LAPC has already acknowledged the frame.
		 *
		 * Accepted in CALL PRESENT and ACTIVE, which are F-02 and
		 * F-10. Outside those the message is ignored rather than
		 * closed, because {CC-INFO} is a legitimate message on a live
		 * transaction and closing it would drop the call.
		 */
		if (!our_transaction(cc, &h)) {
			close_unknown(cc, &h);
			break;
		}
		if ((cc->state == DECT_CC_ST_CALL_PRESENT ||
		     cc->state == DECT_CC_ST_CONNECT_PENDING ||
		     cc->state == DECT_CC_ST_ACTIVE ||
		     cc->state == DECT_CC_ST_OVERLAP ||
		     cc->state == DECT_CC_ST_PROCEEDING)) {
			size_t before = cc->keypad_len;

			learn_keypad(cc, ies, ies_len);
			learn_int_key(cc);
			learn_peer_line(cc, ies, ies_len);
			if (cc->line_refused) {
				refuse_line(cc);
				break;
			}
			if (cc->keypad_len != before)
				cc->stats.keypad++;
			if (cc->state == DECT_CC_ST_OVERLAP) {
				cc->overlap_deadline_ms = 0;	/* restart */
				if (dect_nwk_find_fixed_ie(ies, ies_len,
				    DECT_IE_SENDING_COMPLETE) < 0)
					;
				else if ((cc->peer_basic_service >> 4) == 0x9u)
					proceed(cc);	/* internal: see on_setup */
				else
					accept_now(cc);
			}
		}
		break;
	case DECT_CC_RELEASE_COM:
		/*
		 * The PT releasing OUR unanswered incoming call with
		 * "insufficient resources" is EN 300 175-5 §9.5.2.3: its own
		 * {CC-SETUP} crossed ours (or was about to -- the standard
		 * applies from CALL INITIATED, before anything is on the air),
		 * it has released our call as required, and it now waits
		 * P<CC.06> = 1.5 s for us to re-present the call, placing no
		 * new call of its own meanwhile. Re-present at once, with the
		 * same message and transaction value, and restart F<CC.03>.
		 *
		 * Bounded by represent_left so two peers cannot ping-pong.
		 *
		 * A release on a transaction we do not hold gets no action
		 * (EN 300 175-5 §17.3.2.1).
		 */
		if (!our_transaction(cc, &h))
			break;
		if (cc->incoming && cc->state == DECT_CC_ST_CALL_PRESENT &&
		    h.ti_flag == DECT_TI_RESPONDER &&
		    h.ti_value == cc->ti_value && cc->setup_len &&
		    cc->represent_left &&
		    dect_nwk_find_fixed_ie(ies, ies_len, DECT_IE_RELEASE_REASON)
		    == (int)DECT_CC_REASON_INSUFFICIENT_RESOURCES) {
			unsigned idx;
			uint8_t *b = txq_slot(cc, &idx);

			if (b) {
				memcpy(b, cc->setup_msg, cc->setup_len);
				txq_commit(cc, idx, cc->setup_len);
				cc->represent_left--;
				cc->stats.represented++;
				cc->setup_deadline_ms = 0;	/* F<CC.03> again */
				break;
			}
		}
		cc->state = DECT_CC_ST_RELEASED;
		cc->release_armed = 0;		/* <CC.02> stopped */
		dect_iwu_reset_sessions(&cc->iwu);
		break;
	default:
		/*
		 * On the call we hold, an unexpected or unrecognised message
		 * ({CC-CALL-PROC}, {CC-SETUP-ACK}, {CC-NOTIFY} ...) is ignored
		 * (EN 300 444 §6.9.4). On a transaction we do not hold, clear
		 * it with "unknown transaction identifier" (EN 300 175-5
		 * §17.3.2.1) rather than ignore it -- see queue_release_other().
		 * Releases never reach here, so a release is never answered
		 * with a release.
		 */
		if (!our_transaction(cc, &h))
			close_unknown(cc, &h);
		break;
	}
}

void dect_cc_set_codec_list(struct dect_cc *cc, const uint8_t *list, size_t len)
{
	if (!list || !len || len > sizeof(cc->codec_list)) {
		cc->codec_list_len = 0;
		return;
	}
	memcpy(cc->codec_list, list, len);
	cc->codec_list_len = len;
}

int dect_cc_info(struct dect_cc *cc, const uint8_t *ies, size_t len)
{
	unsigned idx;
	uint8_t *b;
	size_t off;

	if (!dect_cc_busy(cc) || cc->state == DECT_CC_ST_RELEASED)
		return -1;
	b = txq_slot(cc, &idx);
	if (!b)
		return -1;
	off = dect_nwk_build_hdr(b, DECT_LAPC_MSG_MAX, DECT_PD_CC,
				 cc->ti_flag, cc->ti_value, DECT_CC_INFO);
	if (!off || len > DECT_LAPC_MSG_MAX - off)
		return -1;
	memcpy(b + off, ies, len);
	txq_commit(cc, idx, off + len);
	return 0;
}

int dect_cc_release(struct dect_cc *cc)
{
	if (cc->state == DECT_CC_ST_NULL || cc->state == DECT_CC_ST_RELEASED)
		return -1;
	queue_msg(cc, cc->ti_flag, DECT_CC_RELEASE);
	cc->state = DECT_CC_ST_RELEASED;
	cc->stats.released++;
	cc->release_armed = 1;			/* <CC.02> */
	cc->release_deadline_ms = 0;
	return 0;
}

size_t dect_cc_poll(struct dect_cc *cc, uint8_t *out, size_t cap)
{
	size_t n;

	if (cc->out_count == 0)
		return 0;
	n = cc->out_len[cc->out_head];
	if (cap < n)
		return 0;
	memcpy(out, cc->out[cc->out_head], n);
	cc->out_head = (cc->out_head + 1) % DECT_CC_TXQ;
	cc->out_count--;
	return n;
}

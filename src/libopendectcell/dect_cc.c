// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * NWK Call Control FSM (fixed-part, basic speech). See dect_cc.h.
 * ETSI EN 300 175-5 §9 / EN 300 444. The PP originates the call (SETUP, F=0),
 * so our replies are responder-side (F=1) in the same transaction.
 */
#include "dect_cc.h"
#include "dect_nwk.h"

#include <string.h>

void dect_cc_init(struct dect_cc *cc)
{
	memset(cc, 0, sizeof(*cc));
	cc->state = DECT_CC_ST_NULL;
	cc->ti_flag = DECT_TI_RESPONDER;	/* until a call says otherwise */
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

/*
 * Queue a {CC-RELEASE-COM} carrying a <<RELEASE-REASON>>. The bare queue_msg()
 * form sends no reason, which is legal but tells the peer nothing; an abnormal
 * release should say why.
 */
static void queue_release_com(struct dect_cc *cc, uint8_t reason)
{
	unsigned idx;
	uint8_t *b = txq_slot(cc, &idx);
	size_t off;

	if (!b)
		return;
	off = dect_nwk_build_hdr(b, DECT_LAPC_MSG_MAX, DECT_PD_CC,
				 cc->ti_flag, cc->ti_value,
				 DECT_CC_RELEASE_COM);
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

	if (id && !id->emit_before_basic_service)
		off = put_identity(b, off, id);

	/* Line and call identifiers, last as in the stock order. */
	off = put_callinfo(b, off, &cc->callinfo);

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
	cc->next_ti = (uint8_t)(ti % 7u);
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

static void on_setup(struct dect_cc *cc)
{
	cc->stats.setup++;
	/*
	 * Accept the call: CALL-PROC ("I am handling it") then CONNECT (we are
	 * the far end for the loopback test, so we answer at once). Both are
	 * responder-side in the PP-originated transaction.
	 */
	queue_msg(cc, DECT_TI_RESPONDER, DECT_CC_CALL_PROC);
	queue_msg(cc, DECT_TI_RESPONDER, DECT_CC_CONNECT);
	cc->state = DECT_CC_ST_CONNECT_PENDING;
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
}


/* EN 300 175-5 A.1: CC setup timer, 20 s for FT and PT alike. */
#define CC_SETUP_TIMER_MS	20000u
/* Release reason "timer expiry" (EN 300 175-5 7.7.18). */
#define RELEASE_REASON_TIMER_EXPIRY	0x0Du

void dect_cc_tick(struct dect_cc *cc, uint64_t now_ms)
{
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
	return cc->setup_deadline_ms ? cc->setup_deadline_ms : UINT64_MAX;
}

void dect_cc_rx(struct dect_cc *cc, const uint8_t *msg, size_t len)
{
	struct dect_nwk_hdr h;
	const uint8_t *ies;
	size_t ies_len;

	if (dect_nwk_parse(msg, len, &h, &ies, &ies_len) < 0 || h.pd != DECT_PD_CC)
		return;

	switch (h.msg_type) {
	case DECT_CC_SETUP:
		if (cc->state == DECT_CC_ST_NULL) {
			cc->ti_value = h.ti_value;
			cc->ti_flag = DECT_TI_RESPONDER;
			cc->incoming = 0;
			learn_id(ies, ies_len, DECT_IE_PORTABLE_IDENTITY,
				 cc->peer_port_id, &cc->peer_port_id_len);
			learn_id(ies, ies_len, DECT_IE_FIXED_IDENTITY,
				 cc->peer_fixed_id, &cc->peer_fixed_id_len);
			on_setup(cc);
		}
		break;
	case DECT_CC_ALERTING:
		/* Incoming call: the handset is ringing. */
		if (cc->incoming && cc->state == DECT_CC_ST_CALL_PRESENT) {
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
		if (cc->incoming && (cc->state == DECT_CC_ST_CALL_PRESENT ||
				     cc->state == DECT_CC_ST_RINGING)) {
			queue_msg(cc, cc->ti_flag, DECT_CC_CONNECT_ACK);
			cc->state = DECT_CC_ST_ACTIVE;
			cc->stats.connected++;
		}
		break;
	case DECT_CC_CONNECT_ACK:
		on_connect_ack(cc);
		break;
	case DECT_CC_RELEASE:
		on_release(cc);
		break;
	case DECT_CC_RELEASE_COM:
		cc->state = DECT_CC_ST_RELEASED;
		break;
	default:
		break;
	}
}

int dect_cc_release(struct dect_cc *cc)
{
	if (cc->state == DECT_CC_ST_NULL || cc->state == DECT_CC_ST_RELEASED)
		return -1;
	queue_msg(cc, cc->ti_flag, DECT_CC_RELEASE);
	cc->state = DECT_CC_ST_RELEASED;
	cc->stats.released++;
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

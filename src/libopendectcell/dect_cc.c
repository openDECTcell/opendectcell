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
	queue_msg(cc, DECT_TI_RESPONDER, DECT_CC_RELEASE_COM);
	cc->state = DECT_CC_ST_RELEASED;
	cc->stats.released++;
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
			on_setup(cc);
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
	queue_msg(cc, DECT_TI_RESPONDER, DECT_CC_RELEASE);
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

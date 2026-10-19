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

#include "dect_lapc.h"	/* DECT_LAPC_MSG_MAX */

enum dect_cc_state {
	DECT_CC_ST_NULL,		/* no call */
	DECT_CC_ST_PROCEEDING,		/* SETUP seen, CALL-PROC/CONNECT queued */
	DECT_CC_ST_CONNECT_PENDING,	/* CONNECT sent, awaiting CONNECT-ACK */
	DECT_CC_ST_ACTIVE,		/* call up: bind audio */
	DECT_CC_ST_RELEASED,		/* call released */
};

#define DECT_CC_TXQ	3u		/* pending outbound NWK messages */

struct dect_cc_stats {
	uint32_t setup;		/* CC-SETUP received */
	uint32_t connected;	/* reached ACTIVE */
	uint32_t released;
};

struct dect_cc {
	enum dect_cc_state state;
	uint8_t ti_value;		/* transaction value from the PP's SETUP */

	/* FIFO of outbound NWK messages (e.g. CALL-PROC then CONNECT). */
	uint8_t  out[DECT_CC_TXQ][DECT_LAPC_MSG_MAX];
	size_t   out_len[DECT_CC_TXQ];
	unsigned out_head, out_count;

	struct dect_cc_stats stats;
};

void dect_cc_init(struct dect_cc *cc);

/* Feed a decoded NWK message (the LAPC I-frame payload). Non-CC messages are
 * ignored. Any responses are queued; pull them with dect_cc_poll(). */
void dect_cc_rx(struct dect_cc *cc, const uint8_t *msg, size_t len);

/* Queue a CC-RELEASE to hang up an active call. Returns 0, or -1 if idle. */
int dect_cc_release(struct dect_cc *cc);

/* Pull a pending outbound NWK message. Returns its length, or 0 if none. */
size_t dect_cc_poll(struct dect_cc *cc, uint8_t *out, size_t cap);

#endif /* OPENDECTCELL_DECT_CC_H */

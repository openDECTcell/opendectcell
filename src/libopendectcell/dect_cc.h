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
	/* Incoming call (we originate towards the handset). */
	DECT_CC_ST_CALL_PRESENT,	/* SETUP sent, awaiting the PP's answer */
	DECT_CC_ST_RINGING,		/* PP sent ALERTING: the handset is ringing */
};

#define DECT_CC_TXQ	3u		/* pending outbound NWK messages */
#define DECT_CC_ID_MAX	20u		/* PORTABLE-/FIXED-IDENTITY contents */

struct dect_cc_stats {
	uint32_t setup;		/* CC-SETUP received */
	uint32_t connected;	/* reached ACTIVE */
	uint32_t released;
	uint32_t alerting;	/* ALERTING received: the handset is ringing */
};

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

/* Queue a CC-RELEASE to hang up an active call. Returns 0, or -1 if idle. */
int dect_cc_release(struct dect_cc *cc);

/* Pull a pending outbound NWK message. Returns its length, or 0 if none. */
size_t dect_cc_poll(struct dect_cc *cc, uint8_t *out, size_t cap);

#endif /* OPENDECTCELL_DECT_CC_H */

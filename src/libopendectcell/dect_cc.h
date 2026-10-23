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
};

#define DECT_CC_TXQ	3u		/* pending outbound NWK messages */
#define DECT_CC_ID_MAX	20u		/* PORTABLE-/FIXED-IDENTITY contents */

/*
 * Release reasons, EN 300 175-5 V2.9.1 Annex (release reason coding). Verified
 * against the table rather than taken from a trace: [CR A-0055] gave 0x01 for
 * an unhandled transaction, but 0x01 is "unexpected message" and the reason
 * the standard names for this case is 0x02. [CR A-0060] agreed and corrected.
 */
#define DECT_CC_REASON_UNEXPECTED_MESSAGE	0x01u
#define DECT_CC_REASON_UNKNOWN_TRANSACTION	0x02u
#define DECT_CC_REASON_INCOMPATIBLE_SERVICE	0x05u

struct dect_cc_stats {
	uint32_t setup;		/* CC-SETUP received */
	uint32_t connected;	/* reached ACTIVE */
	uint32_t released;
	uint32_t alerting;	/* ALERTING received: the handset is ringing */
	uint32_t closed_unknown;/* transactions we closed with {CC-RELEASE} */
	uint32_t setup_timeouts;	/* F<CC.03> expiries */
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

	/* FIFO of outbound NWK messages (e.g. CALL-PROC then CONNECT). */
	uint8_t  out[DECT_CC_TXQ][DECT_LAPC_MSG_MAX];
	size_t   out_len[DECT_CC_TXQ];
	unsigned out_head, out_count;

	struct dect_cc_stats stats;
	struct dect_iwu iwu;	/* <<IWU-TO-IWU>> session responder */
};

void dect_cc_init(struct dect_cc *cc);

/* Feed a decoded NWK message (the LAPC I-frame payload). Non-CC messages are
 * ignored. Any responses are queued; pull them with dect_cc_poll(). */
void dect_cc_rx(struct dect_cc *cc, const uint8_t *msg, size_t len);

/*
 * Advance the clock. Expires F<CC.03> and queues the abnormal release the
 * specification requires. Call it alongside dect_lapc_tick().
 */
void dect_cc_tick(struct dect_cc *cc, uint64_t now_ms);

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
 * Ask for a <<SIGNAL>> IE in the next FP-originated SETUP. `value` is the
 * signal code, e.g. DECT_SIGNAL_ALERT_PATTERN0. Passing enable = 0 goes back
 * to sending no IE and letting the handset choose.
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
 * Choose the transaction value the next FP-originated SETUP will use. Values
 * 0..6 are valid for the originating side (7 is the extended-TI escape).
 *
 * We used to hardcode 0 for every call. A call that ends without a clean
 * release can leave the portable part believing that transaction is still
 * open, and it then acknowledges the next SETUP at the link layer and
 * discards the message -- which is exactly the signature seen on hardware on
 * 2026-10-20 after a call was cut off mid-media. Rotating costs nothing and
 * is what the standard expects anyway.
 */
void dect_cc_set_next_ti(struct dect_cc *cc, uint8_t ti);

/* Queue a CC-RELEASE to hang up an active call. Returns 0, or -1 if idle. */
int dect_cc_release(struct dect_cc *cc);

/* Pull a pending outbound NWK message. Returns its length, or 0 if none. */
size_t dect_cc_poll(struct dect_cc *cc, uint8_t *out, size_t cap);

#endif /* OPENDECTCELL_DECT_CC_H */

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * dect_link: the per-connection C-plane stack.
 *
 * The layers -- Cs, Lc, LAPC, NWK, MM, CC -- live in their own modules;
 * this is the glue that wires them together, because the glue is protocol
 * behaviour, not application code: MM is drained before CC, one NWK message
 * is outstanding per LAPC window, the Lc signature is learned from the
 * peer's establishment frame. Every consumer (dect-stubd, the controller)
 * uses this one rather than reimplementing it.
 *
 * One link per connection, with no globals of its own and no I/O: the
 * caller owns the transport and the randomness, this owns the protocol.
 *
 *   dect_link_init()              bring a connection up
 *   dect_link_rx_segment()        feed one received C-plane segment in
 *   dect_link_tick()              drive the timers
 *   dect_link_poll_append_mail()  take the next frame to transmit
 *
 * What stays with the caller, deliberately:
 *
 *   - **randomness.** `rx_segment()` takes RAND_F and RS as arguments rather
 *     than calling a generator, so the library has no opinion about entropy
 *     and remains testable with fixed vectors.
 *   - **policy.** Everything observed is reported through `struct
 *     dect_link_ind`; the link never decides what a page response or a
 *     learned identity *means*. Ringing, subscription storage and audio are
 *     the application's.
 *   - **the audio bind.** It is chip specific, so it stays with the owner.
 */
#ifndef OPENDECTCELL_DECT_LINK_H
#define OPENDECTCELL_DECT_LINK_H

#include <stddef.h>
#include <stdint.h>

#include "dect_cc.h"
#include "dect_cs.h"
#include "dect_lapc.h"
#include "dect_lc.h"
#include "dect_mail.h"
#include "dect_mm.h"
#include "dect_time.h"
#include "dect_nwk.h"
#include "dect_proto.h"

#define DECT_LINK_CISS_MAX	4u
#define DECT_LINK_HELD		4u

/* One connection's stack. Treat the members as read-only from outside. */
struct dect_link {
	uint8_t  handle;	/* chip connection handle this link rides on */
	struct dect_lc_rx  lc;
	struct dect_lapc   lapc;
	struct dect_mm     mm;
	struct dect_cc     cc;
	/*
	 * CISS {FACILITY}s waiting for LAPC: the date and time and the events
	 * notifications that follow a location registration (TS 102 527-3
	 * §7.4.2, §7.4.20). CISS has no state machine here, so a short FIFO
	 * is all it needs.
	 */
	uint8_t  ciss[DECT_LINK_CISS_MAX][DECT_LAPC_MSG_MAX];
	size_t   ciss_len[DECT_LINK_CISS_MAX];
	unsigned ciss_n;
	/*
	 * A held {CC-SETUP} from an unknown IPUI, see dect_link_setup_resume().
	 * It is answered at once (setup_answered: {CC-SETUP-ACK} or
	 * {CC-CALL-PROC}), which stops the handset's 20 s P<CC.03>; what the
	 * handset sends on the call meanwhile -- its digits, a list-access
	 * dialog -- is held too (up to DECT_LINK_HELD, later ones are dropped)
	 * and replayed after it.
	 */
	uint8_t  setup[DECT_LAPC_MSG_MAX];
	size_t   setup_len;
	uint8_t  setup_id[DECT_MM_PORTID_MAX];
	size_t   setup_id_len;
	uint8_t  setup_ti;
	uint8_t  setup_answered;
	uint8_t  held[DECT_LINK_HELD][DECT_LAPC_MSG_MAX];
	size_t   held_len[DECT_LINK_HELD];
	unsigned held_n;
	/*
	 * auth_required (set by the host when it opens the link, with auth_first): list access is not
	 * served before the handset has proved its key. An IPUI is not secret, so a peer that knows one
	 * could otherwise read and change the phonebook and the call lists while the base's authentication
	 * request goes unanswered. The handset's {CC-IWU-INFO} is held, one message, and handled when
	 * mm.authenticated is set (dect_link_tick); it is dropped when the call ends first.
	 */
	uint8_t  auth_required;
	uint8_t  iwu_held[DECT_LAPC_MSG_MAX];
	size_t   iwu_held_len;
	uint32_t lce_bad_ti;		/* LCE messages ignored: F = 1 or TV != 0 */
	/*
	 * Optional: may the handset with this IPUI get call status? Asked
	 * when its {CC-SETUP} arrives, before CC answers it: 1 yes, 0 no, -1
	 * no opinion (keep the setting). TS 102 527-3 §7.4.6.4.1 allows call
	 * status to NG-DECT Part 3 handsets only, and only the owner knows
	 * which handset reported what.
	 */
	int    (*call_status_for)(void *ctx, const uint8_t *ipui, size_t len);
	void    *call_status_ctx;
	/*
	 * Optional, same ctx: the call id for the handset's new call, asked
	 * at its {CC-SETUP} before CC answers (call ids are unique system-
	 * wide, TS 102 527-3 §7.4.6.1, which only the owner can see). 0 keeps
	 * the current id.
	 */
	uint8_t (*call_id_for)(void *ctx, const struct dect_link *l);
	/*
	 * One message sent ahead of MM and CC: an {LCE-PAGE-REJECT}, or the
	 * link's own answer to a held {CC-SETUP} ({CC-SETUP-ACK},
	 * {CC-CALL-PROC}, or {CC-RELEASE-COM} when the handset gives up).
	 */
	uint8_t  lce[DECT_LAPC_MSG_MAX];
	size_t   lce_len;

	/*
	 * Optional sink for every NWK message the link hands to the DLC, for
	 * the owner's log: the library does no I/O of its own, so this is how
	 * the caller sees what went out.
	 */
	void   (*tx_log)(void *ctx, const uint8_t *msg, size_t len);
	void    *tx_log_ctx;
};

/*
 * What one received segment turned out to be. The link reports; the caller
 * decides. `nwk` is valid only when `nwk_len` is non-zero, and it is the
 * reassembled NWK message, so it may carry identities -- do not log it
 * anywhere that leaves the device.
 */
struct dect_link_ind {
	unsigned page_response : 1;	/* {LCE-PAGE-RESPONSE}, identity in page_id */
	unsigned cc_message    : 1;
	/*
	 * A handset sent its date and time (TS 102 527-3 §7.4.2.2): in
	 * pp_time if it named the identity this link holds, else
	 * pp_time_unknown ("the FP shall ignore" an unidentified PP).
	 */
	unsigned pp_time       : 1;
	unsigned pp_time_unknown : 1;
	struct dect_time pp_time_value;
	unsigned lapc_parse_failed : 1;	/* a segment we could not use */
	unsigned keypad        : 1;	/* the peer sent dialled digits */
	/*
	 * The segment belongs to a DIFFERENT connection and was not consumed.
	 * seg_handle says which. Nothing in the link was touched.
	 */
	unsigned foreign       : 1;
	uint8_t  seg_handle;		/* the handle the segment carried */
	int      lc_rc;			/* dect_lc_rx_fragment() return */
	uint8_t  seg_head[3];		/* first 3 octets: addr/ctrl/li */
	struct dect_lapc_frame frame;	/* valid when lc_rc > 0 */
	uint8_t  nwk[DECT_LAPC_MSG_MAX];
	size_t   nwk_len;
	/*
	 * With page_response: the <<PORTABLE-IDENTITY>> contents the handset
	 * answered with. The caller must check it is the handset it paged
	 * (TS 102 527-1 §7.3.8.2.1); another handset may answer.
	 */
	uint8_t  page_id[DECT_CC_ID_MAX];
	size_t   page_id_len;
	uint8_t  page_ti;		/* its transaction value, for the reject */
};

/*
 * Bring a connection up. `mmcfg` is copied; pass the PIN, the RFPI and the
 * narrowband-only flag as for dect_mm_init().
 */
void dect_link_init(struct dect_link *l, uint8_t handle,
		    const struct dect_mm_cfg *mmcfg);

/*
 * Feed one received C-plane segment (a 0x0203 mail) in.
 *
 * Decodes the Cs segment, reassembles the Lc fragment, parses and runs the
 * LAPC frame, routes any complete NWK message by protocol discriminator, and
 * queues whatever MM or CC produced in reply. `rand_f` (8 bytes) and `rs`
 * (16 bytes) are handed to MM for authentication challenges; either may be
 * NULL if the caller knows none is due.
 *
 * Returns 0 if the segment was consumed, -1 if it was not a C-plane segment
 * or belongs to another connection (ind->foreign). `ind` may be NULL.
 */
int dect_link_rx_segment(struct dect_link *l, const struct dect_mail *m,
			 uint64_t now, const uint8_t *rand_f,
			 const uint8_t *rs, struct dect_link_ind *ind);

/*
 * Hand any queued MM/CC message to LAPC. rx_segment() and tick() both do this
 * for you; call it directly after changing MM or CC state from outside, such
 * as starting ciphering, so the reply does not wait for the next segment.
 */
void dect_link_drain(struct dect_link *l, uint64_t now);

/* Drive the LAPC, CC and MM timers. Call once per poll loop. */
void dect_link_tick(struct dect_link *l, uint64_t now);

/*
 * Queue the CISS {FACILITY} that tells the handset the date and time.
 *
 * TS 102 527-3 §7.4.20.2 makes this the FT's duty after a successful
 * location registration, unconditionally -- "even if the PT does not
 * indicate support" and "independently of the 'Clock master' setting".
 * The caller supplies the time, because the library has no clock.
 *
 * Returns 0 if queued, -1 if the time is out of range or the CISS queue
 * (DECT_LINK_CISS_MAX) is full. Sending it is never essential: a handset
 * with the wrong clock still makes calls.
 */
int dect_link_send_time(struct dect_link *l, const struct dect_time *t);

/*
 * Queue a CISS {FACILITY} built by the caller (dect_events_build_facility());
 * it goes out after MM and CC. 0, or -1 if the queue is full or len bad.
 */
int dect_link_send_ciss(struct dect_link *l, const uint8_t *msg, size_t len);

/*
 * Queue {LCE-PAGE-REJECT} for a page response from a handset we did not
 * page: the full IPUI it answered with and the transaction value it used
 * (TS 102 527-1 §7.3.8.3.1, table 20). The caller then releases the link,
 * which the clause requires "immediately". Returns 0 if queued, -1 if the
 * identity does not fit or a reject is already pending.
 */
int dect_link_page_reject(struct dect_link *l, const uint8_t *ipui,
			  size_t ipui_len, uint8_t ti_value);

/*
 * With mm.cfg.defer_unknown, a {CC-SETUP} whose IPUI the link cannot adopt
 * is held, unanswered, instead of being turned away: a roamer in the same
 * location area calls without locating first. Fetch the record,
 * dect_mm_hold() it, then dect_link_setup_resume(): the SETUP goes through
 * the gate again and is served, or turned away as before. Returns 1 served
 * (the identity is held now), -1 turned away, 0 nothing pending. The PT
 * waits <CC.03> = 20 s for an answer (EN 300 175-5 annex A).
 */
int dect_link_setup_pending(const struct dect_link *l, const uint8_t **id,
			    size_t *id_len);
int dect_link_setup_resume(struct dect_link *l, uint64_t now);

/* Watch every NWK message the link sends. Pass NULL to stop watching. */
void dect_link_set_tx_log(struct dect_link *l,
			  void (*fn)(void *ctx, const uint8_t *msg, size_t len),
			  void *ctx);

/*
 * Take the next frame to transmit, already wrapped in an Lc frame and a
 * 0x0244 APPEND_CONNECTION_DATA mail addressed to this link's handle.
 * Returns 1 and fills `out` when there was one, 0 when the queue is empty.
 * Call it in a loop until it returns 0.
 */
int dect_link_poll_append_mail(struct dect_link *l, struct dect_mail *out);

#endif /* OPENDECTCELL_DECT_LINK_H */

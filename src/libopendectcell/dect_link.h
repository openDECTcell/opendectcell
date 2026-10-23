/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * dect_link: the per-connection C-plane stack.
 *
 * libopendectcell has always provided the layers -- Cs, Lc, LAPC, NWK, MM, CC --
 * but not the code that wires them together. That glue lived in
 * `dect-stubd.c` as `reg_segment()`, `drain_nwk()` and `pump_lapc()`,
 * operating on a single global connection. openDECTcell#67: **the glue is protocol
 * behaviour, not application code.** MM is drained before CC, one NWK message
 * is outstanding per LAPC window, the Lc signature is learned from the peer's
 * establishment frame. A second consumer reimplementing that from prose would
 * drift from this one, silently and in exactly the places that are hard to
 * test.
 *
 * So it lives here, per connection, with no globals and no I/O: the caller
 * owns the transport and the randomness, this owns the protocol.
 *
 * Shape (agreed on openDECTcell#67):
 *
 *   dect_link_init()              bring a connection up
 *   dect_link_rx_segment()        feed one received C-plane segment in
 *   dect_link_tick()              drive retransmission timers
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
 *   - **the audio bind.** Explicitly out of scope for #67: it is chip
 *     specific and currently wrong, and moving it would standardise the
 *     defect across both consumers.
 */
#ifndef DECT_LINK_H
#define DECT_LINK_H

#include <stddef.h>
#include <stdint.h>

#include "dect_cc.h"
#include "dect_cs.h"
#include "dect_lapc.h"
#include "dect_lc.h"
#include "dect_mail.h"
#include "dect_mm.h"
#include "dect_nwk.h"
#include "dect_proto.h"

/* One connection's stack. Treat the members as read-only from outside. */
struct dect_link {
	uint8_t  handle;	/* chip connection handle this link rides on */
	struct dect_lc_rx  lc;
	struct dect_lapc   lapc;
	struct dect_mm     mm;
	struct dect_cc     cc;
};

/*
 * What one received segment turned out to be. The link reports; the caller
 * decides. `nwk` is valid only when `nwk_len` is non-zero, and it is the
 * reassembled NWK message, so it may carry identities -- do not log it
 * anywhere that leaves the device.
 */
struct dect_link_ind {
	unsigned page_response : 1;	/* LCE: the peer answered our page */
	unsigned cc_message    : 1;
	unsigned mm_message    : 1;
	unsigned lapc_parse_failed : 1;	/* a segment we could not use */
	unsigned keypad        : 1;	/* the peer sent dialled digits */
	int      lc_rc;			/* dect_lc_rx_fragment() return */
	uint8_t  seg_head[3];		/* first 3 octets: addr/ctrl/li */
	struct dect_lapc_frame frame;	/* valid when lc_rc > 0 */
	uint8_t  nwk[DECT_LAPC_MSG_MAX];
	size_t   nwk_len;
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
 * Returns 0 if the segment was consumed, -1 if it was not a C-plane segment.
 * `ind` may be NULL.
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

/* Drive LAPC retransmission and CC timers. Call once per poll loop. */
void dect_link_tick(struct dect_link *l, uint64_t now);

/*
 * Take the next frame to transmit, already wrapped in an Lc frame and a
 * 0x0244 APPEND_CONNECTION_DATA mail addressed to this link's handle.
 * Returns 1 and fills `out` when there was one, 0 when the queue is empty.
 * Call it in a loop until it returns 0.
 */
int dect_link_poll_append_mail(struct dect_link *l, struct dect_mail *out);

#endif /* DECT_LINK_H */

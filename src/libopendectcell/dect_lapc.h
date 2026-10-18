/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DECT DLC C-plane data link (LAPC), class A (acknowledged).
 * Specification: ETSI EN 300 175-4 V1.5.1, clauses 6 and 7.
 *
 * LAPC is a LAPD-derived link protocol with LAPDm-style length-indicator
 * delimiting (no flags/bit-stuffing). A type-FA frame is:
 *   [ Address | Control | Length-Indicator | Information(0..63) ]
 * and the Lc layer (dect_lc.h) adds fill and the frame checksum below it.
 *
 * This header covers the frame codec: parse a received LAPC frame into its
 * fields, and build I-frames and supervisory (RR) frames. The class-A
 * acknowledged-link state machine is added on top (same module).
 *
 * We are the fixed part (FT). Per §7.3.2 the FT sends commands with C/R = 1 and
 * responses with C/R = 0 (the PT does the opposite). GAP connection-oriented
 * signalling uses SAPI = 0 and LLN = 1 (class A).
 *
 * Re-implemented from the ETSI text as GPL-2.0-or-later; libdect (GPL-2.0-only)
 * used only as a cross-check, no code copied.
 */
#ifndef OPENDECTCELL_DECT_LAPC_H
#define OPENDECTCELL_DECT_LAPC_H

#include <stddef.h>
#include <stdint.h>

#include "dect_lc.h"

/* SAPI values (§7.3.3). */
#define DECT_LAPC_SAPI_CO	0u	/* connection-oriented signalling */
#define DECT_LAPC_SAPI_CL	3u	/* connectionless signalling */

/* LLN values (§7.3.5): class A is the acknowledged residential signalling link. */
#define DECT_LAPC_LLN_U		0u	/* class U (unacknowledged) */
#define DECT_LAPC_LLN_A		1u	/* class A (acknowledged) */

/* Frame type after decoding the control octet. */
enum dect_lapc_type {
	DECT_LAPC_I,		/* information */
	DECT_LAPC_RR,		/* receive ready */
	DECT_LAPC_RNR,		/* receive not ready */
	DECT_LAPC_REJ,		/* reject */
	DECT_LAPC_SABM,		/* set async balanced mode (establish) */
	DECT_LAPC_DM,		/* disconnected mode */
	DECT_LAPC_UI,		/* unnumbered information */
	DECT_LAPC_DISC,		/* disconnect */
	DECT_LAPC_UA,		/* unnumbered acknowledgement */
	DECT_LAPC_UNKNOWN,
};

/* A decoded LAPC frame (host view). */
struct dect_lapc_frame {
	enum dect_lapc_type type;
	uint8_t nlf;		/* new link flag (address) */
	uint8_t lln;		/* logical link number */
	uint8_t sapi;
	uint8_t cr;		/* command/response bit as on the wire */
	uint8_t ns, nr;		/* sequence numbers (I-frames / N(R) for S) */
	uint8_t pf;		/* poll/final bit */
	uint8_t more;		/* M bit: another segment of the NWK message follows */
	const uint8_t *info;	/* points into the input buffer */
	size_t info_len;
};

/*
 * Parse a LAPC frame (addr, ctrl, LI, info) of lapc_len octets as delivered by
 * dect_lc_rx_fragment(). Returns 0 on success, -1 if too short or malformed.
 * f->info points into buf (valid while buf is).
 */
int dect_lapc_parse(const uint8_t *buf, size_t lapc_len, struct dect_lapc_frame *f);

/*
 * Build an I-frame (addr, ctrl, LI, info) into out. cr/nlf/ns/nr/pf/more set the
 * fields; info_len <= DECT_LC_INFO_MAX. Returns the LAPC length (3 + info_len),
 * or 0 on a bad length. Wrap the result with dect_lc_build() before sending.
 */
size_t dect_lapc_build_i(uint8_t *out, size_t out_cap, uint8_t cr, uint8_t nlf,
			 uint8_t ns, uint8_t nr, uint8_t pf, uint8_t more,
			 const uint8_t *info, size_t info_len);

/* Build a supervisory RR frame (no information field). Returns 3, or 0 on error. */
size_t dect_lapc_build_rr(uint8_t *out, size_t out_cap, uint8_t cr,
			  uint8_t nr, uint8_t pf);

#endif /* OPENDECTCELL_DECT_LAPC_H */

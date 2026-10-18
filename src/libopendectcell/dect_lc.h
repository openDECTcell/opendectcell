/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DECT DLC C-plane lower layer (Lc) for the Cs logical channel.
 * Specification: ETSI EN 300 175-4 V1.5.1, clauses 6.1 and 7.6-7.10, Annex B.
 *
 * The Lc entity has no header of its own (EN 300 175-4 §6.1.2): it only
 * buffers, fragments/reassembles and runs a frame checksum. A complete LAPC
 * frame (type FA: Address, Control, Length-Indicator, Information, fill) plus a
 * 2-octet checksum is carried over the Cs channel in 5-octet fragments, one per
 * MAC A-field tail. The total frame length FLEN is a multiple of 5 and at most
 * 70; it is derived from the Length-Indicator octet, so no flag octets are
 * needed for delimiting.
 *
 * On our interface the STUB hands us one 5-octet Cs fragment per 0x0203 mail
 * (see dect_cs.h) and expects the outbound Cs frame as a byte buffer. This
 * module turns a fragment stream into whole LAPC frames and back.
 *
 * Checksum: the ISO/IEC 8073 two-sum (Fletcher-type, mod 255), XORed with a
 * 16-bit link signature LSIG (EN 300 175-4 §7.10, §10.3.1). LSIG = 0 when the
 * PT has no assigned PMID, which is the registration case; for an established
 * call LSIG = the lower 16 bits of the PMID. This is NOT a polynomial CRC (the
 * MAC R-CRC below us is separate and handled by the STUB).
 *
 * Re-implemented from the ETSI text; libdect/kernel net/dect used only as
 * cross-checks, no code copied (they are GPL-2.0-only; this is -or-later).
 * Pure functions and a reassembly state machine on memory buffers: no I/O.
 */
#ifndef OPENDECTCELL_DECT_LC_H
#define OPENDECTCELL_DECT_LC_H

#include <stddef.h>
#include <stdint.h>

#define DECT_LC_FRAG_LEN	5u	/* Cs fragment size (octets) */
#define DECT_LC_FRAME_MAX	70u	/* max FLEN for the Cs channel */
#define DECT_LC_INFO_MAX	63u	/* max Information field (6-bit LI) */
#define DECT_LC_OVERHEAD	5u	/* addr + ctrl + LI + 2-octet checksum */
/* Largest LAPC frame (addr, ctrl, LI, info) handed up/down, without fill/csum. */
#define DECT_LC_LAPC_MAX	(3u + DECT_LC_INFO_MAX)		/* 66 */

/*
 * Compute the two checksum octets for a frame of flen octets. The frame's last
 * two octets (the checksum field) are treated as zero during the sum. Writes
 * out[0] = X (octet FLEN-1) and out[1] = Y (octet FLEN), each XORed with the
 * corresponding half of lsig (LSIGX = high byte, LSIGY = low byte). Exposed for
 * the Annex B golden-vector tests.
 */
void dect_lc_checksum(const uint8_t *frame, size_t flen, uint16_t lsig,
		      uint8_t out[2]);

/* FLEN for a LAPC info length: round (LI + overhead) up to a multiple of 5. */
size_t dect_lc_flen(size_t info_len);

/*
 * Build the on-air Cs frame for a LAPC frame (addr, ctrl, LI octet, info).
 * lapc_len must be 3 + info_len (3 header octets + the information field) and at
 * most DECT_LC_LAPC_MAX. Appends 0xF0 fill up to FLEN-2 and the 2 checksum
 * octets. Returns FLEN (a multiple of 5) on success, or 0 on a bad length or if
 * out_cap < FLEN. The result is what dect_cs_append_mail() sends.
 */
size_t dect_lc_build(const uint8_t *lapc, size_t lapc_len, uint16_t lsig,
		     uint8_t *out, size_t out_cap);

struct dect_lc_rx_stats {
	uint32_t frames;	/* valid LAPC frames delivered */
	uint32_t csum_errors;	/* frames dropped on a checksum mismatch */
	uint32_t length_errors;	/* FLEN out of range / LI too large */
};

struct dect_lc_rx {
	uint8_t  buf[DECT_LC_FRAME_MAX];
	size_t   len;		/* octets accumulated in buf */
	size_t   flen;		/* expected frame length, 0 until the LI octet */
	uint16_t lsig;
	struct dect_lc_rx_stats stats;
};

void dect_lc_rx_init(struct dect_lc_rx *rx, uint16_t lsig);

/*
 * Feed one 5-octet Cs fragment. When a complete frame has arrived and its
 * checksum is valid, copies the LAPC frame (addr, ctrl, LI, info -- without fill
 * or checksum) into out and returns its length (3 + info_len). Returns 0 when
 * more fragments are needed, and -1 when a frame was dropped (checksum or length
 * error); in the -1 case reassembly has resynchronised to expect a new frame.
 */
int dect_lc_rx_fragment(struct dect_lc_rx *rx, const uint8_t seg[DECT_LC_FRAG_LEN],
			uint8_t *out, size_t out_cap);

#endif /* OPENDECTCELL_DECT_LC_H */

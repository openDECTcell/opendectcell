// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DECT DLC Lc layer for the Cs channel. See dect_lc.h.
 * ETSI EN 300 175-4 V1.5.1 §6.1, §7.6-7.10, Annex B.
 */
#include "dect_lc.h"

#include <string.h>

#define LC_FILL		0xF0u	/* fill octet pattern (§7.8) */
#define LC_MOD		255u	/* checksum arithmetic is mod 255 (§B.1) */

/*
 * ISO/IEC 8073 two-sum over octets 1..flen (Annex B.2). The two checksum octets
 * at positions flen-1 and flen are treated as zero. C0 = sum of octets; C1 = sum
 * of running partial sums. Note the last two positions add 0 to C0 but still
 * advance C1.
 */
static void sum_c0_c1(const uint8_t *frame, size_t flen, int zero_tail,
		      unsigned *c0_out, unsigned *c1_out)
{
	unsigned c0 = 0, c1 = 0;
	size_t n;

	for (n = 0; n < flen; n++) {
		unsigned octet = frame[n];

		if (zero_tail && n >= flen - 2u)
			octet = 0;
		c0 = (c0 + octet) % LC_MOD;
		c1 = (c1 + c0) % LC_MOD;
	}
	*c0_out = c0;
	*c1_out = c1;
}

void dect_lc_checksum(const uint8_t *frame, size_t flen, uint16_t lsig,
		      uint8_t out[2])
{
	unsigned c0, c1;
	int x, y;

	sum_c0_c1(frame, flen, 1, &c0, &c1);
	/* Octet(FLEN-1) = C0 - C1, Octet(FLEN) = -2*C0 + C1 (mod 255). */
	x = (int)((c0 + LC_MOD - c1) % LC_MOD);
	y = (int)((c1 + 2u * LC_MOD - (2u * c0) % LC_MOD) % LC_MOD);
	out[0] = (uint8_t)(x ^ (lsig >> 8));
	out[1] = (uint8_t)(y ^ (lsig & 0xFFu));
}

/*
 * The link signature that makes this frame valid: XOR the received checksum with
 * the checksum the frame would carry for LSIG 0. A registered PP signs with
 * LSIG = low 16 bits of its PMID (EN 300 175-4 §7.10), so we learn it from the
 * establishment frame rather than requiring it up front.
 */
static uint16_t derive_lsig(const uint8_t *frame, size_t flen)
{
	uint8_t c0[2];

	dect_lc_checksum(frame, flen, 0, c0);
	return (uint16_t)(((frame[flen - 2] ^ c0[0]) << 8) |
			  (frame[flen - 1] ^ c0[1]));
}

/* Valid iff, with the link signature removed, both C0 and C1 are zero (§B.3). */
static int checksum_ok(const uint8_t *frame, size_t flen, uint16_t lsig)
{
	uint8_t tmp[DECT_LC_FRAME_MAX];
	unsigned c0, c1;

	if (flen < DECT_LC_OVERHEAD || flen > DECT_LC_FRAME_MAX)
		return 0;
	memcpy(tmp, frame, flen);
	tmp[flen - 2] ^= (uint8_t)(lsig >> 8);
	tmp[flen - 1] ^= (uint8_t)(lsig & 0xFFu);
	sum_c0_c1(tmp, flen, 0, &c0, &c1);
	return c0 == 0 && c1 == 0;
}

size_t dect_lc_flen(size_t info_len)
{
	size_t min = info_len + DECT_LC_OVERHEAD;

	return (min + (DECT_LC_FRAG_LEN - 1)) / DECT_LC_FRAG_LEN * DECT_LC_FRAG_LEN;
}

size_t dect_lc_build(const uint8_t *lapc, size_t lapc_len, uint16_t lsig,
		     uint8_t *out, size_t out_cap)
{
	size_t info_len, flen;

	if (lapc_len < 3 || lapc_len > DECT_LC_LAPC_MAX)
		return 0;
	info_len = lapc_len - 3;
	flen = dect_lc_flen(info_len);
	if (flen > DECT_LC_FRAME_MAX || out_cap < flen)
		return 0;

	memcpy(out, lapc, lapc_len);
	memset(out + lapc_len, LC_FILL, flen - 2 - lapc_len);	/* fill to FLEN-2 */
	out[flen - 2] = 0;					/* checksum placeholders */
	out[flen - 1] = 0;
	dect_lc_checksum(out, flen, lsig, &out[flen - 2]);
	return flen;
}

void dect_lc_rx_init(struct dect_lc_rx *rx, uint16_t lsig)
{
	memset(rx, 0, sizeof(*rx));
	rx->lsig = lsig;
}

/* Number of information octets the LI octet at buf[2] announces. */
static size_t li_info_len(uint8_t li_octet)
{
	return li_octet >> 2;		/* bits 8-3 = LI, bits 2-1 = M, N */
}

int dect_lc_rx_fragment(struct dect_lc_rx *rx, const uint8_t seg[DECT_LC_FRAG_LEN],
			uint8_t *out, size_t out_cap)
{
	size_t info_len, lapc_len;

	if (rx->len + DECT_LC_FRAG_LEN > sizeof(rx->buf)) {
		/* Should not happen once flen is known; resync defensively. */
		rx->len = 0;
		rx->flen = 0;
		rx->stats.length_errors++;
		return -1;
	}
	memcpy(rx->buf + rx->len, seg, DECT_LC_FRAG_LEN);
	rx->len += DECT_LC_FRAG_LEN;

	/* After the first fragment the LI octet (index 2) fixes the frame length. */
	if (rx->flen == 0) {
		info_len = li_info_len(rx->buf[2]);
		if (info_len > DECT_LC_INFO_MAX) {
			rx->len = 0;
			rx->stats.length_errors++;
			return -1;
		}
		rx->flen = dect_lc_flen(info_len);
	}

	if (rx->len < rx->flen)
		return 0;		/* need more fragments */

	/*
	 * A full frame is buffered. Lock the link signature, or learn it from
	 * the first frame of the link: a registered PP signs with LSIG = low 16
	 * bits of its PMID, not 0. Once learned it is locked, so a later frame
	 * that fails is a real error rather than a chance to re-derive.
	 *
	 * The first frame is not always the short establishment frame. When the
	 * FP pages a handset, the handset opens the link with its page response,
	 * which is a full multi-fragment message — so restricting the lesson to
	 * single-fragment frames made every incoming call fail here, and kept
	 * failing on each retransmission. derive_lsig() solves for the signature
	 * from the frame itself and does not care how long it is.
	 */
	if (checksum_ok(rx->buf, rx->flen, rx->lsig)) {
		rx->lsig_locked = 1;
	} else if (!rx->lsig_locked) {
		rx->lsig = derive_lsig(rx->buf, rx->flen);
		rx->lsig_locked = 1;
	} else {
		rx->len = 0;
		rx->flen = 0;
		rx->stats.csum_errors++;
		return -1;
	}

	info_len = li_info_len(rx->buf[2]);
	lapc_len = 3 + info_len;
	rx->len = 0;
	rx->flen = 0;
	if (out_cap < lapc_len) {
		rx->stats.length_errors++;
		return -1;
	}
	memcpy(out, rx->buf, lapc_len);
	rx->stats.frames++;
	return (int)lapc_len;
}

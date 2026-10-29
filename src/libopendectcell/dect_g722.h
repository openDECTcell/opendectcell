/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ITU-T G.722 sub-band ADPCM, 64 kbit/s (mode 1) -- the wideband codec the
 * DECT bearer carries on the TDM voice lane.
 *
 * Written from ITU-T Recommendation G.722 (the block diagram of clauses 3 and
 * 4 and its normative tables), not ported from any implementation. Validated
 * against ffmpeg's g722 encoder and decoder used as black boxes; see
 * tests/test_g722.c and tools/g722-crosscheck.sh.
 *
 * One octet carries one pair of 16 kHz samples: bits 7..6 are the high
 * sub-band code, bits 5..0 the low sub-band code. So the stream runs at
 * 8000 octets/s for 16000 samples/s, which is what the chip's wideband lane
 * delivers and accepts (one octet per 125 us frame).
 *
 * Pure functions over an explicit state; no I/O, no allocation. Encoder and
 * decoder states are separate objects even though the type is shared.
 */
#ifndef OPENDECTCELL_DECT_G722_H
#define OPENDECTCELL_DECT_G722_H

#include <stddef.h>
#include <stdint.h>

/* One sub-band's adaptive quantizer and predictor (clause 3, blocks 2-4). */
struct dect_g722_band {
	int s;			/* predicted signal */
	int sp;			/* pole section output */
	int sz;			/* zero section output */
	int r[3];		/* reconstructed signal history */
	int a[3];		/* pole coefficients */
	int ap[3];		/* pole coefficients, updated */
	int p[3];		/* partially reconstructed signal history */
	int d[7];		/* quantized difference history */
	int b[7];		/* zero coefficients */
	int bp[7];		/* zero coefficients, updated */
	int sg[7];		/* sign bits used by the adaptation */
	int nb;			/* logarithmic scale factor */
	int det;		/* linear scale factor */
};

struct dect_g722_state {
	struct dect_g722_band band[2];	/* [0] low sub-band, [1] high */
	int x[24];			/* QMF delay line */
};

/* Reset a state. Use one for encoding and a different one for decoding. */
void dect_g722_init(struct dect_g722_state *st);

/*
 * Encode n 16 kHz samples into n/2 octets. n must be even; an odd trailing
 * sample is not buffered and is simply not encoded. Returns octets written.
 * out must hold at least n/2 bytes.
 */
size_t dect_g722_encode(struct dect_g722_state *st, const int16_t *pcm,
			size_t n, uint8_t *out);

/*
 * Decode n octets into 2*n samples at 16 kHz. Returns samples written.
 * pcm must hold at least 2*n samples.
 */
size_t dect_g722_decode(struct dect_g722_state *st, const uint8_t *in,
			size_t n, int16_t *pcm);

#endif /* OPENDECTCELL_DECT_G722_H */

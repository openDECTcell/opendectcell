// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ITU-T G.722 sub-band ADPCM, 64 kbit/s. See dect_g722.h for the interface
 * and for how this relates to the DECT wideband bearer.
 *
 * Structure follows the Recommendation's own block names so the code can be
 * read against it: the QMF of clause 3.1/4.1, then per sub-band the adaptive
 * quantizer (QUANTL/QUANTH), the inverse quantizers (INVQAL/INVQBL/INVQAH),
 * the scale-factor adaptation (LOGSCL/SCALEL, LOGSCH/SCALEH) and the shared
 * adaptive predictor (block 4: RECONS, PARREC, UPPOL1/2, UPZERO, DELAYA,
 * FILTEP, FILTEZ, PREDIC).
 *
 * Everything is integer arithmetic on `int`. The Recommendation specifies the
 * arithmetic exactly, so a conforming encoder and decoder are bit-exact; the
 * tests rely on that.
 *
 * Its right shifts are arithmetic: a negative value keeps its sign. C leaves
 * `>>` of a negative value implementation-defined (C11 6.5.7); GCC and Clang
 * define it as the arithmetic shift, and the assertion below refuses a
 * compiler that does not.
 */
#include "dect_g722.h"

_Static_assert((-3 >> 1) == -2, "G.722 needs an arithmetic right shift");

/*
 * QMF coefficients, the 24-tap symmetric half-band filter of clause 3.1
 * scaled by 2^13. Stored as the 12 distinct values: index i is tap 2i and
 * index 11-i is tap 2i+1.
 */
static const int qmf_coeffs[12] = {
	3, -11, 12, 32, -210, 951, 3876, -805, 362, -156, 53, -11
};

/* Low sub-band quantizer decision levels, scaled by 2^12 (clause 3.2). */
static const int q6[32] = {
	   0,   35,   72,  110,  150,  190,  233,  276,
	 323,  370,  422,  473,  530,  587,  650,  714,
	 786,  858,  940, 1023, 1121, 1219, 1339, 1458,
	1612, 1765, 1980, 2195, 2557, 2919,    0,    0
};

/* Low sub-band code words, negative and positive branch. */
static const int iln[32] = {
	 0, 63, 62, 31, 30, 29, 28, 27,
	26, 25, 24, 23, 22, 21, 20, 19,
	18, 17, 16, 15, 14, 13, 12, 11,
	10,  9,  8,  7,  6,  5,  4,  0
};
static const int ilp[32] = {
	 0, 61, 60, 59, 58, 57, 56, 55,
	54, 53, 52, 51, 50, 49, 48, 47,
	46, 45, 44, 43, 42, 41, 40, 39,
	38, 37, 36, 35, 34, 33, 32,  0
};

/* High sub-band code words, negative and positive branch. */
static const int ihn[3] = { 0, 1, 0 };
static const int ihp[3] = { 0, 3, 2 };

/* Inverse quantizers: 2-bit (high band), 4-bit (predictor feedback), 6-bit. */
static const int qm2[4] = { -7408, -1616, 7408, 1616 };
static const int qm4[16] = {
	     0, -20456, -12896, -8968,
	 -6288,  -4240,  -2584, -1200,
	 20456,  12896,   8968,  6288,
	  4240,   2584,   1200,     0
};
static const int qm6[64] = {
	  -136,   -136,   -136,   -136,
	-24808, -21904, -19008, -16704,
	-14984, -13512, -12280, -11192,
	-10232,  -9360,  -8576,  -7856,
	 -7192,  -6576,  -6000,  -5456,
	 -4944,  -4464,  -4008,  -3576,
	 -3168,  -2776,  -2400,  -2032,
	 -1688,  -1360,  -1040,   -728,
	 24808,  21904,  19008,  16704,
	 14984,  13512,  12280,  11192,
	 10232,   9360,   8576,   7856,
	  7192,   6576,   6000,   5456,
	  4944,   4464,   4008,   3576,
	  3168,   2776,   2400,   2032,
	  1688,   1360,   1040,    728,
	   432,    136,   -432,   -136
};

/* Scale-factor adaptation (clause 3.3). */
static const int wl[8] = { -60, -30, 58, 172, 334, 538, 1198, 3042 };
static const int rl42[16] = { 0, 7, 6, 5, 4, 3, 2, 1, 7, 6, 5, 4, 3, 2, 1, 0 };
static const int wh[3] = { 0, -214, 798 };
static const int rh2[4] = { 2, 1, 2, 1 };
static const int ilb[32] = {
	2048, 2093, 2139, 2186, 2233, 2282, 2332, 2383,
	2435, 2489, 2543, 2599, 2656, 2714, 2774, 2834,
	2896, 2960, 3025, 3091, 3158, 3228, 3298, 3371,
	3444, 3520, 3597, 3676, 3756, 3838, 3922, 4008
};

/* 16-bit saturation, used wherever the Recommendation says "limit". */
static int sat16(int v)
{
	if (v > 32767)
		return 32767;
	if (v < -32768)
		return -32768;
	return v;
}

/* Sign of a 16-bit quantity as the Recommendation's sign bit: 0 or -1. */
static int sgn(int v)
{
	return (v < 0) ? -1 : 0;
}

/*
 * (coeff * v) >> 15, the Recommendation's scaled product. Every caller passes
 * 16-bit quantities -- table entries, sat16() results, the clamped pole
 * coefficients, the scale factor det (at most 4008 << 1 << 2 = 32064) and
 * the constants 32512 and 32640 -- so |coeff * v| <= 2^15 * 2^15 = 2^30 and
 * the product fits a 32-bit integer.
 */
static int mul15(int coeff, int v)
{
	int32_t p = (int32_t)coeff * (int32_t)v;

	return (int)(p >> 15);
}

/*
 * Block 4: the adaptive predictor, identical for both sub-bands. `dx` is the
 * quantized difference signal for this sample.
 */
static void block4(struct dect_g722_band *b, int dx)
{
	int wd1, wd2, wd3, i;

	/* RECONS */
	b->d[0] = dx;
	b->r[0] = sat16(b->s + dx);

	/* PARREC */
	b->p[0] = sat16(b->sz + dx);

	/* UPPOL2 */
	for (i = 0; i < 3; i++)
		b->sg[i] = sgn(b->p[i]);
	wd1 = sat16(b->a[1] * 4);
	wd2 = (b->sg[0] == b->sg[1]) ? -wd1 : wd1;
	if (wd2 > 32767)
		wd2 = 32767;
	wd3 = (b->sg[0] == b->sg[2]) ? 128 : -128;
	wd3 += (wd2 >> 7);
	wd3 += mul15(b->a[2], 32512);
	if (wd3 > 12288)
		wd3 = 12288;
	else if (wd3 < -12288)
		wd3 = -12288;
	b->ap[2] = wd3;

	/* UPPOL1 */
	b->sg[0] = sgn(b->p[0]);
	b->sg[1] = sgn(b->p[1]);
	wd1 = (b->sg[0] == b->sg[1]) ? 192 : -192;
	wd2 = mul15(b->a[1], 32640);
	b->ap[1] = sat16(wd1 + wd2);
	wd3 = sat16(15360 - b->ap[2]);
	if (b->ap[1] > wd3)
		b->ap[1] = wd3;
	else if (b->ap[1] < -wd3)
		b->ap[1] = -wd3;

	/* UPZERO */
	wd1 = (dx == 0) ? 0 : 128;
	b->sg[0] = sgn(dx);
	for (i = 1; i < 7; i++) {
		b->sg[i] = sgn(b->d[i]);
		wd2 = (b->sg[i] == b->sg[0]) ? wd1 : -wd1;
		wd3 = mul15(b->b[i], 32640);
		b->bp[i] = sat16(wd2 + wd3);
	}

	/* DELAYA */
	for (i = 6; i > 0; i--) {
		b->d[i] = b->d[i - 1];
		b->b[i] = b->bp[i];
	}
	for (i = 2; i > 0; i--) {
		b->r[i] = b->r[i - 1];
		b->p[i] = b->p[i - 1];
		b->a[i] = b->ap[i];
	}

	/* FILTEP */
	wd1 = sat16(b->r[1] + b->r[1]);
	wd1 = mul15(b->a[1], wd1);
	wd2 = sat16(b->r[2] + b->r[2]);
	wd2 = mul15(b->a[2], wd2);
	b->sp = sat16(wd1 + wd2);

	/* FILTEZ */
	b->sz = 0;
	for (i = 6; i > 0; i--) {
		wd1 = sat16(b->d[i] + b->d[i]);
		b->sz += mul15(b->b[i], wd1);
	}
	b->sz = sat16(b->sz);

	/* PREDIC */
	b->s = sat16(b->sp + b->sz);
}

/* LOGSCL + SCALEL: low sub-band scale factor, driven by the 4-bit index. */
static void scale_low(struct dect_g722_band *b, int ril)
{
	int wd1, wd2, wd3;

	wd1 = (b->nb * 127) >> 7;
	b->nb = wd1 + wl[rl42[ril]];
	if (b->nb < 0)
		b->nb = 0;
	else if (b->nb > 18432)
		b->nb = 18432;

	wd1 = (b->nb >> 6) & 31;
	wd2 = 8 - (b->nb >> 11);
	wd3 = (wd2 < 0) ? (ilb[wd1] << -wd2) : (ilb[wd1] >> wd2);
	b->det = wd3 << 2;
}

/* LOGSCH + SCALEH: high sub-band scale factor. */
static void scale_high(struct dect_g722_band *b, int ih)
{
	int wd1, wd2, wd3;

	wd1 = (b->nb * 127) >> 7;
	b->nb = wd1 + wh[rh2[ih]];
	if (b->nb < 0)
		b->nb = 0;
	else if (b->nb > 22528)
		b->nb = 22528;

	wd1 = (b->nb >> 6) & 31;
	wd2 = 10 - (b->nb >> 11);
	wd3 = (wd2 < 0) ? (ilb[wd1] << -wd2) : (ilb[wd1] >> wd2);
	b->det = wd3 << 2;
}

void dect_g722_init(struct dect_g722_state *st)
{
	int i;

	for (i = 0; i < 24; i++)
		st->x[i] = 0;
	for (i = 0; i < 2; i++) {
		struct dect_g722_band *b = &st->band[i];
		int k;

		b->s = 0;
		b->sp = 0;
		b->sz = 0;
		b->nb = 0;
		b->det = 32;		/* the Recommendation's reset value */
		for (k = 0; k < 3; k++) {
			b->r[k] = 0;
			b->a[k] = 0;
			b->ap[k] = 0;
			b->p[k] = 0;
		}
		for (k = 0; k < 7; k++) {
			b->d[k] = 0;
			b->b[k] = 0;
			b->bp[k] = 0;
			b->sg[k] = 0;
		}
	}
}

/* Transmit QMF (clause 3.1): one pair of samples in, two sub-band samples out. */
static void qmf_analyse(struct dect_g722_state *st, int even, int odd,
			int *xlow, int *xhigh)
{
	int sum_even = 0, sum_odd = 0, i;

	for (i = 0; i < 22; i++)
		st->x[i] = st->x[i + 2];
	st->x[22] = even;
	st->x[23] = odd;

	for (i = 0; i < 12; i++) {
		sum_odd += st->x[2 * i] * qmf_coeffs[i];
		sum_even += st->x[2 * i + 1] * qmf_coeffs[11 - i];
	}
	*xlow = (sum_even + sum_odd) >> 14;
	*xhigh = (sum_even - sum_odd) >> 14;
}

/* Receive QMF (clause 4.1): two sub-band samples in, one pair of samples out. */
static void qmf_synthesise(struct dect_g722_state *st, int rlow, int rhigh,
			   int16_t *out)
{
	int xout1 = 0, xout2 = 0, i;

	for (i = 0; i < 22; i++)
		st->x[i] = st->x[i + 2];
	st->x[22] = rlow + rhigh;
	st->x[23] = rlow - rhigh;

	for (i = 0; i < 12; i++) {
		xout2 += st->x[2 * i] * qmf_coeffs[i];
		xout1 += st->x[2 * i + 1] * qmf_coeffs[11 - i];
	}
	out[0] = (int16_t)sat16(xout1 >> 11);
	out[1] = (int16_t)sat16(xout2 >> 11);
}

size_t dect_g722_encode(struct dect_g722_state *st, const int16_t *pcm,
			size_t n, uint8_t *out)
{
	size_t i, written = 0;

	for (i = 0; i + 1 < n; i += 2) {
		struct dect_g722_band *lo = &st->band[0];
		struct dect_g722_band *hi = &st->band[1];
		int xlow, xhigh, el, eh, wd, wd1, ilow, ihigh, mih, j, top;
		int ril, dlow, dhigh;

		qmf_analyse(st, pcm[i], pcm[i + 1], &xlow, &xhigh);

		/*
		 * Low sub-band: SUBTRA then QUANTL. The Recommendation's
		 * QUANTL takes the first decision level j in 1..29 with
		 * wd < (q6[j] * det) >> 12, else 30. The levels grow with j
		 * and det > 0, so the test is false up to some j and true
		 * from there on: a binary search finds the same j in five
		 * steps instead of up to 29.
		 */
		el = sat16(xlow - lo->s);
		wd = (el >= 0) ? el : -(el + 1);
		j = 1;
		top = 30;
		while (j < top) {
			int mid = (j + top) >> 1;

			if (wd < ((q6[mid] * lo->det) >> 12))
				top = mid;
			else
				j = mid + 1;
		}
		ilow = (el < 0) ? iln[j] : ilp[j];

		/* INVQAL: the predictor is driven by the 4 high bits only. */
		ril = ilow >> 2;
		dlow = mul15(lo->det, qm4[ril]);
		scale_low(lo, ril);
		block4(lo, dlow);

		/* High sub-band: SUBTRA then QUANTH, a single decision level. */
		eh = sat16(xhigh - hi->s);
		wd = (eh >= 0) ? eh : -(eh + 1);
		wd1 = (564 * hi->det) >> 12;
		mih = (wd >= wd1) ? 2 : 1;
		ihigh = (eh < 0) ? ihn[mih] : ihp[mih];

		dhigh = mul15(hi->det, qm2[ihigh]);
		scale_high(hi, ihigh);
		block4(hi, dhigh);

		out[written++] = (uint8_t)(((ihigh << 6) | ilow) & 0xFF);
	}
	return written;
}

size_t dect_g722_decode(struct dect_g722_state *st, const uint8_t *in,
			size_t n, int16_t *pcm)
{
	size_t i, written = 0;

	for (i = 0; i < n; i++) {
		struct dect_g722_band *lo = &st->band[0];
		struct dect_g722_band *hi = &st->band[1];
		int ilow = in[i] & 0x3F;
		int ihigh = (in[i] >> 6) & 0x03;
		int ril, rlow, rhigh, dlowt, dhigh;

		/* INVQBL: the full 6 bits reconstruct the output signal. */
		rlow = lo->s + mul15(lo->det, qm6[ilow]);
		if (rlow > 16383)		/* LIMIT, clause 4.2 */
			rlow = 16383;
		else if (rlow < -16384)
			rlow = -16384;

		/* INVQAL: the predictor is driven by the 4 high bits only, so
		 * encoder and decoder stay in step at every bit rate. */
		ril = ilow >> 2;
		dlowt = mul15(lo->det, qm4[ril]);
		scale_low(lo, ril);
		block4(lo, dlowt);

		dhigh = mul15(hi->det, qm2[ihigh]);
		rhigh = sat16(dhigh + hi->s);
		scale_high(hi, ihigh);
		block4(hi, dhigh);

		qmf_synthesise(st, rlow, rhigh, &pcm[written]);
		written += 2;
	}
	return written;
}

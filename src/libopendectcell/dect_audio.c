// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A-law conversion and tone analysis for the voice slots. See dect_audio.h.
 * The A-law routines follow the ITU-T G.711 / Sun reference algorithm.
 */
#include "dect_audio.h"

#include <math.h>
#include <string.h>

#define ALAW_SIGN	0x80
#define ALAW_QUANT	0x0f
#define ALAW_SEG_MASK	0x70
#define ALAW_SEG_SHIFT	4

int16_t dect_alaw_decode(uint8_t a)
{
	int t, seg;

	a ^= 0x55u;
	t = (a & ALAW_QUANT) << 4;
	seg = (a & ALAW_SEG_MASK) >> ALAW_SEG_SHIFT;
	switch (seg) {
	case 0:
		t += 8;
		break;
	case 1:
		t += 0x108;
		break;
	default:
		t += 0x108;
		t <<= seg - 1;
	}
	return (int16_t)((a & ALAW_SIGN) ? t : -t);
}

uint8_t dect_alaw_encode(int16_t pcm)
{
	static const int seg_end[8] = {
		0x1f, 0x3f, 0x7f, 0xff, 0x1ff, 0x3ff, 0x7ff, 0xfff,
	};
	int val = pcm >> 3;	/* 16-bit linear -> 13-bit magnitude range */
	int mask, seg, i;
	unsigned int aval;

	if (val >= 0) {
		mask = 0xd5;		/* sign bit = 1 for positive */
	} else {
		mask = 0x55;
		val = -val - 1;
	}
	for (seg = 0; seg < 8; seg++)
		if (val <= seg_end[seg])
			break;
	if (seg >= 8)
		return (uint8_t)(0x7f ^ mask);
	aval = (unsigned int)seg << ALAW_SEG_SHIFT;
	i = (seg < 2) ? (val >> 1) : (val >> seg);
	aval |= (unsigned int)i & ALAW_QUANT;
	return (uint8_t)(aval ^ (unsigned int)mask);
}

void dect_tone_analyze(const int16_t *pcm, size_t n, uint32_t rate,
		       struct dect_tone_stats *s)
{
	double sum = 0.0, sumsq = 0.0, mean;
	double peak = 0.0;
	uint32_t crossings = 0;
	int prev_sign = 0;
	size_t i;

	s->samples = n;
	s->mean = s->rms = s->freq_hz = 0.0;
	s->peak = 0;
	s->crossings = 0;
	if (!n)
		return;

	for (i = 0; i < n; i++)
		sum += (double)pcm[i];
	mean = sum / (double)n;

	for (i = 0; i < n; i++) {
		double a = (double)pcm[i] - mean;
		int sign = (a > 0.0) ? 1 : (a < 0.0) ? -1 : 0;

		sumsq += a * a;
		if (fabs(a) > peak)
			peak = fabs(a);
		if (sign && prev_sign && sign != prev_sign)
			crossings++;
		if (sign)
			prev_sign = sign;
	}

	s->mean = mean;
	s->rms = sqrt(sumsq / (double)n);
	s->peak = (int16_t)(peak > 32767.0 ? 32767 : (int)peak);
	s->crossings = crossings;
	/* Two mean-crossings per period: freq = crossings/2 * rate / n. */
	s->freq_hz = (double)crossings * (double)rate / (2.0 * (double)n);
}

/*
 * The half-band filter's taps at odd distances 1, 3, ..., 19 from the
 * centre, Q15 (Blackman-windowed sinc); the even ones are zero and the
 * centre is one half.
 */
static const int16_t rs2_tap[10] = {
	10325, -3173, 1614, -894, 489, -252, 117, -46, 13, -1,
};

void dect_rs2_init(struct dect_rs2 *r)
{
	memset(r, 0, sizeof(*r));
}

static void rs2_push(struct dect_rs2 *r, int16_t v)
{
	memmove(r->x, r->x + 1, (DECT_RS2_TAPS - 1u) * sizeof(r->x[0]));
	r->x[DECT_RS2_TAPS - 1u] = v;
}

static int16_t rs2_sat(int32_t v)
{
	return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/*
 * The odd taps around centre c, Q15: |sum| < 2^31 for any input (the taps'
 * magnitudes add up to 16924, times two samples of 2^15).
 */
static int32_t rs2_odd(const int16_t *x, unsigned c)
{
	int32_t acc = 0;
	unsigned k;

	for (k = 0; k < 10u; k++)
		acc += rs2_tap[k] * ((int32_t)x[c - 2u * k - 1u] +
				     (int32_t)x[c + 2u * k + 1u]);
	return acc;
}

size_t dect_rs2_down(struct dect_rs2 *r, const int16_t *in, size_t n,
		     int16_t *out)
{
	const unsigned c = DECT_RS2_TAPS / 2u;
	size_t i;

	for (i = 0; i + 1u < n; i += 2u) {
		rs2_push(r, in[i]);
		rs2_push(r, in[i + 1u]);
		out[i / 2u] = rs2_sat((16384 * (int32_t)r->x[c] +
				       rs2_odd(r->x, c)) >> 15);
	}
	return n / 2u;
}

/*
 * Zeros between the samples, then the low-pass at twice the gain, in its
 * two phases, so the zeros are never multiplied: the history holds the
 * 8 kHz samples only, and x[] is its newest 20. Output at a sample is the
 * sample itself (twice the centre tap); output between x[9] and x[10] is
 * twice the odd taps over the ten samples either side.
 */
size_t dect_rs2_up(struct dect_rs2 *r, const int16_t *in, size_t n,
		   int16_t *out)
{
	const int16_t *x = r->x + DECT_RS2_TAPS - 20u;
	size_t i;

	for (i = 0; i < n; i++) {
		int32_t acc = 0;
		unsigned k;

		rs2_push(r, in[i]);
		out[2u * i] = x[9];
		/* between x[9] and x[10]: taps 1, 3, ... 19 away, both sides */
		for (k = 0; k < 10u; k++)
			acc += rs2_tap[k] * ((int32_t)x[9u - k] +
					     (int32_t)x[10u + k]);
		out[2u * i + 1u] = rs2_sat(acc >> 14);
	}
	return 2u * n;
}

// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A-law conversion and tone analysis for the voice slots. See dect_audio.h.
 * The A-law routines follow the ITU-T G.711 / Sun reference algorithm.
 */
#include "dect_audio.h"

#include <math.h>

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

void dect_alaw_decode_buf(const uint8_t *alaw, int16_t *pcm, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		pcm[i] = dect_alaw_decode(alaw[i]);
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

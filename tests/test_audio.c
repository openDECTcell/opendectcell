// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for src/libopendectcell/dect_audio.c: A-law round-trip and the tone
 * analysis. No hardware.
 */
#include "check.h"
#include "../src/libopendectcell/dect_audio.h"

#include <math.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* A-law silence (linear 0) encodes to 0x55 and decodes back near 0. */
static void test_alaw_silence(void)
{
	CHECK_EQ(dect_alaw_encode(0), 0xd5);	/* +0: sign bit set */
	/* 0x55 is the value observed at rest in the voice slot. */
	CHECK(abs(dect_alaw_decode(0x55)) <= 8);
	CHECK(abs(dect_alaw_decode(0xd5)) <= 8);
}

/* encode(decode(a)) is the identity for every A-law code. */
static void test_alaw_roundtrip_codes(void)
{
	int a, ok = 1;

	for (a = 0; a < 256; a++) {
		int16_t lin = dect_alaw_decode((uint8_t)a);
		uint8_t re = dect_alaw_encode(lin);

		if (re != (uint8_t)a)
			ok = 0;
	}
	CHECK(ok);
}

/* A linear sweep survives A-law within the codec's quantisation error. */
static void test_alaw_quantisation(void)
{
	int v, worst = 0;

	for (v = -32768; v < 32768; v += 101) {
		int16_t back = dect_alaw_decode(dect_alaw_encode((int16_t)v));
		int err = abs(v - back);

		if (err > worst)
			worst = err;
	}
	/* A-law is near-logarithmic: the top segment quantises in ~1024-wide
	 * steps at 16-bit scale, so the worst-case error is about that. */
	CHECK(worst < 1100);
}

/* A 1 kHz sine at 8 kHz, A-law encoded, reads back as ~1 kHz with energy. */
static void test_tone_detect(void)
{
	enum { N = 1600 };			/* 0.2 s at 8 kHz */
	static int16_t pcm[N];
	uint8_t alaw[N];
	struct dect_tone_stats st;
	int i;

	for (i = 0; i < N; i++) {
		double x = 8000.0 * sin(2.0 * M_PI * 1000.0 * i / 8000.0);
		alaw[i] = dect_alaw_encode((int16_t)x);
	}
	dect_alaw_decode_buf(alaw, pcm, N);
	dect_tone_analyze(pcm, N, 8000u, &st);
	CHECK(st.rms > 1000.0);
	CHECK(fabs(st.freq_hz - 1000.0) < 50.0);
	CHECK(st.peak > 3000);
}

/* Silence reads back as ~no energy and no frequency. */
static void test_tone_silence(void)
{
	enum { N = 800 };
	static int16_t pcm[N];
	uint8_t alaw[N];
	struct dect_tone_stats st;
	int i;

	for (i = 0; i < N; i++)
		alaw[i] = 0x55;			/* A-law silence */
	dect_alaw_decode_buf(alaw, pcm, N);
	dect_tone_analyze(pcm, N, 8000u, &st);
	CHECK(st.rms < 16.0);
	CHECK(st.freq_hz < 1.0);
}

/* RMS of a tone through the resampler, after the filter has filled. */
static double rs2_rms(int up, double hz)
{
	enum { N = 1600 };
	static int16_t in[N], out[2 * N];
	struct dect_rs2 r;
	double rate = up ? 8000.0 : 16000.0, sum = 0.0;
	size_t i, n;

	for (i = 0; i < N; i++)
		in[i] = (int16_t)(10000.0 * sin(2.0 * M_PI * hz * (double)i /
						rate));
	dect_rs2_init(&r);
	n = up ? dect_rs2_up(&r, in, N, out) : dect_rs2_down(&r, in, N, out);
	CHECK_EQ(n, up ? 2u * N : N / 2u);
	for (i = n / 4u; i < n; i++)
		sum += (double)out[i] * (double)out[i];
	return sqrt(sum / (double)(n - n / 4u));
}

/*
 * The 2:1 resampler keeps speech (1 kHz, 3.4 kHz) at its level both ways
 * and removes what does not fit at 8 kHz (6 kHz going down) and the image
 * the zeros make going up (a 1 kHz tone's 7 kHz image: the level stays
 * the tone's alone).
 */
static void test_rs2(void)
{
	double ref = 10000.0 / sqrt(2.0);

	CHECK(fabs(rs2_rms(0, 1000.0) / ref - 1.0) < 0.02);
	CHECK(fabs(rs2_rms(0, 3400.0) / ref - 1.0) < 0.06);
	CHECK(rs2_rms(0, 6000.0) / ref < 0.002);
	CHECK(fabs(rs2_rms(1, 1000.0) / ref - 1.0) < 0.02);
	CHECK(fabs(rs2_rms(1, 3400.0) / ref - 1.0) < 0.06);
}

int main(void)
{
	test_alaw_silence();
	test_alaw_roundtrip_codes();
	test_alaw_quantisation();
	test_tone_detect();
	test_tone_silence();
	test_rs2();
	return CHECK_DONE();
}

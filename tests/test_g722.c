// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for src/libopendectcell/dect_g722.c: the ITU-T G.722 encoder and decoder.
 * No hardware.
 *
 * These tests fix behaviour; they do not establish conformance on their own.
 * Conformance was checked separately against ffmpeg's independent G.722
 * implementation (tools/g722-crosscheck.sh) -- see the comment on
 * test_golden_bitstream for exactly what each kind of evidence covers.
 */
#include "check.h"
#include "../src/libopendectcell/dect_g722.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* The QMF pair delays the signal by 22 samples; comparisons allow for it. */
#define QMF_DELAY	22

/* A reproducible wideband test signal: one voice-band tone plus one above
 * 4 kHz, so both sub-bands carry something. */
static void make_signal(int16_t *pcm, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		double t = (double)i / 16000.0;
		double v = 12000.0 * sin(2.0 * M_PI * 697.0 * t)
			 + 6000.0 * sin(2.0 * M_PI * 5300.0 * t);

		pcm[i] = (int16_t)v;
	}
}

static double snr_db(const int16_t *ref, const int16_t *got, size_t n,
		     size_t delay)
{
	double num = 0.0, den = 0.0;
	size_t i;

	for (i = 0; i + delay < n; i++) {
		double r = ref[i];
		double e = r - got[i + delay];

		num += r * r;
		den += e * e;
	}
	return den > 0.0 ? 10.0 * log10(num / den) : 999.0;
}

/* Reset puts both sub-bands in the Recommendation's initial state. */
static void test_init(void)
{
	struct dect_g722_state st;
	int i, zero = 1;

	memset(&st, 0xA5, sizeof(st));
	dect_g722_init(&st);
	CHECK_EQ(st.band[0].det, 32);
	CHECK_EQ(st.band[1].det, 32);
	CHECK_EQ(st.band[0].nb, 0);
	CHECK_EQ(st.band[1].nb, 0);
	CHECK_EQ(st.band[0].s, 0);
	for (i = 0; i < 24; i++)
		if (st.x[i] != 0)
			zero = 0;
	CHECK(zero);
}

/* One octet per sample pair, in both directions. */
static void test_lengths(void)
{
	struct dect_g722_state st;
	int16_t pcm[64], back[128];
	uint8_t code[64];

	memset(pcm, 0, sizeof(pcm));
	dect_g722_init(&st);
	CHECK_EQ(dect_g722_encode(&st, pcm, 64, code), 32);
	CHECK_EQ(dect_g722_encode(&st, pcm, 0, code), 0);
	/* An odd trailing sample is not buffered, so it is simply dropped. */
	CHECK_EQ(dect_g722_encode(&st, pcm, 7, code), 3);

	dect_g722_init(&st);
	CHECK_EQ(dect_g722_decode(&st, code, 32, back), 64);
	CHECK_EQ(dect_g722_decode(&st, code, 0, back), 0);
}

/*
 * Silence encodes to a steady octet and decodes back to silence. The decoder
 * is allowed a little ringing from its own reset state, but not a DC offset.
 */
static void test_silence(void)
{
	struct dect_g722_state st;
	int16_t pcm[320], back[320];
	uint8_t code[160];
	size_t i;
	int loud = 0, hiband = 0;
	long sum = 0;

	memset(pcm, 0, sizeof(pcm));
	dect_g722_init(&st);
	CHECK_EQ(dect_g722_encode(&st, pcm, 320, code), 160);

	dect_g722_init(&st);
	CHECK_EQ(dect_g722_decode(&st, code, 160, back), 320);
	for (i = 0; i < 320; i++) {
		if (abs(back[i]) > 4)
			loud = 1;
		sum += back[i];
	}
	CHECK(!loud);
	/*
	 * G.722's quantizers have no zero level, so digital silence does not
	 * encode to a constant octet and does not decode to exact zero: it
	 * settles into a short limit cycle of inaudible dither. What must
	 * hold is that the result is inaudible, carries no DC, and that the
	 * high sub-band stays on its smallest code throughout -- silence must
	 * not excite the 4-8 kHz band.
	 */
	CHECK(labs(sum) <= 400);
	for (i = 0; i < 160; i++)
		if (((code[i] >> 6) & 3) != 3)
			hiband = 1;
	CHECK(!hiband);
}

/*
 * The band split survives a round trip: energy below 4 kHz comes back in the
 * low sub-band and energy above it in the high sub-band. Measured as the
 * ratio of first-difference energy to first-sum energy, which is a crude but
 * monotonic half-band discriminator -- and a far better check of the octet
 * layout than counting code values, because it fails if the two sub-bands are
 * ever swapped, mis-shifted or mis-masked.
 */
static double band_ratio(double hz)
{
	struct dect_g722_state enc, dec;
	static int16_t pcm[3200], back[3200];
	static uint8_t code[1600];
	double elow = 0.0, ehigh = 0.0;
	size_t i;

	for (i = 0; i < 3200; i++)
		pcm[i] = (int16_t)(14000.0 * sin(2.0 * M_PI * hz
						 * (double)i / 16000.0));
	dect_g722_init(&enc);
	dect_g722_init(&dec);
	dect_g722_encode(&enc, pcm, 3200, code);
	dect_g722_decode(&dec, code, 1600, back);

	for (i = 200; i < 3200; i++) {
		double sum = (double)back[i] + back[i - 1];
		double dif = (double)back[i] - back[i - 1];

		elow += sum * sum;
		ehigh += dif * dif;
	}
	return ehigh / elow;
}

static void test_band_split(void)
{
	double r300 = band_ratio(300.0);
	double r440 = band_ratio(440.0);
	double r5300 = band_ratio(5300.0);
	double r6500 = band_ratio(6500.0);

	CHECK(r300 < 0.02);
	CHECK(r440 < 0.02);
	CHECK(r5300 > 1.0);
	CHECK(r6500 > 5.0);
	/* and monotonic across the crossover */
	CHECK(r300 < r440 && r440 < r5300 && r5300 < r6500);
}

/* A full round trip keeps the waveform: G.722 at 64 kbit/s is comfortably
 * above 20 dB SNR on a two-tone signal. */
static void test_roundtrip_snr(void)
{
	struct dect_g722_state enc, dec;
	static int16_t pcm[16000], back[16000];
	static uint8_t code[8000];
	double snr;

	make_signal(pcm, 16000);
	dect_g722_init(&enc);
	dect_g722_init(&dec);
	CHECK_EQ(dect_g722_encode(&enc, pcm, 16000, code), 8000);
	CHECK_EQ(dect_g722_decode(&dec, code, 8000, back), 16000);

	snr = snr_db(pcm, back, 16000, QMF_DELAY);
	CHECK(snr > 20.0);
	if (snr <= 20.0)
		fprintf(stderr, "  round-trip SNR %.2f dB\n", snr);
}

/* Full-scale input must not wrap: the codec saturates, it does not overflow. */
static void test_full_scale(void)
{
	struct dect_g722_state enc, dec;
	static int16_t pcm[4000], back[4000];
	static uint8_t code[2000];
	size_t i;
	int wrong = 0;

	for (i = 0; i < 4000; i++)
		pcm[i] = (int16_t)((i % 32) < 16 ? 32767 : -32768);
	dect_g722_init(&enc);
	dect_g722_init(&dec);
	dect_g722_encode(&enc, pcm, 4000, code);
	dect_g722_decode(&dec, code, 2000, back);
	/*
	 * A 500 Hz square wave at full scale reconstructs with ringing, but
	 * the sign must track the input throughout: a wrap would invert it.
	 */
	for (i = 100; i < 4000; i++)
		if ((back[i] >= 0) != (pcm[i - QMF_DELAY] >= 0))
			wrong++;
	CHECK_EQ(wrong, 0);
}

/* Encoding is a pure function of the state: two fresh states agree. */
static void test_deterministic(void)
{
	struct dect_g722_state a, b;
	int16_t pcm[800];
	uint8_t ca[400], cb[400];

	make_signal(pcm, 800);
	dect_g722_init(&a);
	dect_g722_init(&b);
	dect_g722_encode(&a, pcm, 800, ca);
	dect_g722_encode(&b, pcm, 800, cb);
	CHECK(memcmp(ca, cb, 400) == 0);
}

/* Encoding in two halves through one state equals encoding in one go: the
 * state carries the QMF history across the call boundary. */
static void test_streaming(void)
{
	struct dect_g722_state one, split;
	int16_t pcm[800];
	uint8_t whole[400], parts[400];

	make_signal(pcm, 800);
	dect_g722_init(&one);
	dect_g722_encode(&one, pcm, 800, whole);

	dect_g722_init(&split);
	dect_g722_encode(&split, pcm, 300, parts);
	dect_g722_encode(&split, pcm + 300, 500, parts + 150);
	CHECK(memcmp(whole, parts, 400) == 0);
}

/*
 * Golden bitstream. This is a regression fence, not a conformance proof: it
 * locks in what this implementation produces today so a refactor cannot
 * silently change the wire format. Conformance rests on the separate
 * cross-check against ffmpeg's independent implementation, which decodes this
 * encoder's output to the same SNR as its own (tools/g722-crosscheck.sh).
 */
static void test_golden_bitstream(void)
{
	static const uint8_t want[32] = {
		0xF7, 0x14, 0x94, 0x20, 0x04, 0xA0, 0xA0, 0x20,
		0x20, 0xA7, 0x2F, 0x1D, 0x90, 0x0C, 0x0C, 0x8F,
		0x19, 0x35, 0xAC, 0x29, 0x2A, 0xAF, 0x3B, 0x55,
		0x90, 0x0F, 0xD1, 0x97, 0x3B, 0xF2, 0xAD, 0x2C
	};
	struct dect_g722_state st;
	int16_t pcm[320];
	uint8_t code[160];

	make_signal(pcm, 320);
	dect_g722_init(&st);
	CHECK_EQ(dect_g722_encode(&st, pcm, 320, code), 160);
	CHECK(memcmp(code, want, sizeof(want)) == 0);
}

/* FNV-1a over a byte buffer, continuing from h. */
static uint32_t fnv1a(uint32_t h, const uint8_t *p, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		h ^= p[i];
		h *= 16777619u;
	}
	return h;
}

/*
 * A long stress signal, 2^17 samples: full-scale square waves, full-range
 * noise, a full-scale chirp, silence and steps between the rails. It drives
 * both scale factors to their limits and every quantizer decision. Integer
 * arithmetic only, so it is the same on every host.
 */
static int16_t stress_sample(size_t i, uint32_t *rng)
{
	uint32_t seg = (uint32_t)(i >> 14), k = (uint32_t)(i & 0x3FFFu);

	*rng = *rng * 1664525u + 1013904223u;
	switch (seg & 7u) {
	case 0:	/* 500 Hz square, full scale */
		return (k % 32u) < 16u ? 32767 : -32768;
	case 1:	/* white noise over the whole range */
		return (int16_t)(*rng >> 16);
	case 2:	/* full-scale sawtooth, its frequency rising */
		return (int16_t)((k * (k + 64u) * 4096u) >> 16);
	case 3:	/* silence after the loud part: the scale factors decay */
		return 0;
	case 4:	/* rail to rail every sample: the highest frequency */
		return (k & 1u) ? 32767 : -32768;
	case 5:	/* noise at -40 dB */
		return (int16_t)((int32_t)(*rng >> 16) / 100 - 164);
	case 6:	/* random steps between the rails */
		return (k % 97u) ? (int16_t)0x7FFF : (int16_t)(*rng >> 16);
	default:	/* one square wave in each sub-band */
		return (int16_t)(((k / 11u) & 1u ? 12000 : -12000) +
				 ((k / 2u) & 1u ? 6000 : -6000));
	}
}

/*
 * A regression fence for the arithmetic: the hashes of what the encoder
 * and decoder make of the stress signal, taken from the straightforward
 * implementation of QUANTL and of the predictor's products. Any rewrite
 * for speed must leave both bit-identical.
 */
static void test_stress_fingerprint(void)
{
	enum { N = 1 << 17 };
	static int16_t pcm[N], back[N];
	static uint8_t code[N / 2];
	struct dect_g722_state enc, dec;
	uint32_t rng = 1, hc, hp;
	size_t i;

	for (i = 0; i < N; i++)
		pcm[i] = stress_sample(i, &rng);
	dect_g722_init(&enc);
	dect_g722_init(&dec);
	CHECK_EQ(dect_g722_encode(&enc, pcm, N, code), (size_t)N / 2);
	CHECK_EQ(dect_g722_decode(&dec, code, N / 2, back), (size_t)N);
	hc = fnv1a(2166136261u, code, sizeof(code));
	hp = 2166136261u;
	for (i = 0; i < N; i++) {	/* little-endian, whatever the host */
		uint8_t le[2] = { (uint8_t)back[i],
				  (uint8_t)((uint16_t)back[i] >> 8) };

		hp = fnv1a(hp, le, 2);
	}
	CHECK_EQ(hc, 0x7d60dfa2u);
	CHECK_EQ(hp, 0xf8e65226u);
}

int main(void)
{
	test_init();
	test_lengths();
	test_silence();
	test_band_split();
	test_roundtrip_snr();
	test_full_scale();
	test_deterministic();
	test_streaming();
	test_golden_bitstream();
	test_stress_fingerprint();
	return CHECK_DONE();
}

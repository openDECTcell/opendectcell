// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for src/stubd/ssc_align.c: the 7412's SSC transport alignment. No
 * hardware. The model is what the bench showed (docs/7412-spi-tdm-and-
 * loader.md §1b): the chip's 16-bit lanes start at the same bit offset of the
 * SSC's stream in both directions, each lane MSB first on the wire, and
 * dect-stubd wants lanes on slot boundaries as little-endian samples.
 */
#include "check.h"
#include "../src/stubd/ssc_align.h"

#include <string.h>

#define FB	8u			/* bytes per frame: 4 slots */
#define BLK	(3u * FB)		/* bytes per transport block */
#define NBLK	6u
#define NB	(BLK * NBLK)		/* bytes in a test stream */

static int getbit(const uint8_t *s, size_t i)
{
	return (s[i >> 3] >> (7u - (i & 7u))) & 1;
}

static void putbit(uint8_t *s, size_t i, int v)
{
	if (v)
		s[i >> 3] |= (uint8_t)(0x80u >> (i & 7u));
	else
		s[i >> 3] &= (uint8_t)~(0x80u >> (i & 7u));
}

/* 16 bits of s from bit i, MSB first */
static unsigned int lane_at(const uint8_t *s, size_t i)
{
	unsigned int v = 0, k;

	for (k = 0; k < 16u; k++)
		v = v << 1 | (unsigned int)getbit(s, i + k);
	return v;
}

static unsigned int lane_value(unsigned int k)
{
	return (k * 0x9e37u + 0x1234u) & 0xffffu;
}

/*
 * Receive: the chip's lane k sits at bit off + 16k of what the SSC delivers.
 * After alignment, slot m holds lane m - 1 (one slot of delay), low byte first.
 */
static void test_rx(unsigned int off)
{
	uint8_t raw[NB + 2], t[BLK + 2];
	struct ssc_align a;
	size_t i, k, nlanes = (NB * 8u - off) / 16u;
	unsigned int bad = 0;

	memset(raw, 0xff, sizeof(raw));
	for (k = 0; k < nlanes; k++)
		for (i = 0; i < 16u; i++)
			putbit(raw, off + 16u * k + i,
			       (int)(lane_value((unsigned int)k) >> (15u - i)) & 1);
	ssc_align_init(&a, off);
	for (i = 0; i < NBLK; i++)
		ssc_align_rx(&a, raw + i * BLK, t, BLK);
	for (k = 1; k < NB / 2u && k - 1u < nlanes; k++) {
		unsigned int v = lane_value((unsigned int)(k - 1u));

		bad += raw[2u * k] != (v & 0xffu) ||
		       raw[2u * k + 1u] != (v >> 8);
	}
	CHECK_EQ(bad, 0);
}

/* Transmit: lane k of the 7520-layout frames must appear at bit off + 16k. */
static void test_tx(unsigned int off)
{
	uint8_t d[NB], out[NB], t[BLK + 2];
	struct ssc_align a;
	size_t i, k;
	unsigned int bad = 0;

	for (k = 0; k < NB / 2u; k++) {
		d[2u * k] = (uint8_t)(lane_value((unsigned int)k) & 0xffu);
		d[2u * k + 1u] = (uint8_t)(lane_value((unsigned int)k) >> 8);
	}
	ssc_align_init(&a, off);
	for (i = 0; i < NBLK; i++) {
		ssc_align_tx(&a, out + i * BLK, t, d + i * BLK, BLK, 0, 0);
		ssc_align_tx_commit(&a, t, BLK);
	}
	for (k = 0; off + 16u * (k + 1u) <= NB * 8u; k++)
		bad += lane_at(out, off + 16u * k) != lane_value((unsigned int)k);
	CHECK_EQ(bad, 0);
	/* the head: idle (all ones) bits before the first lane */
	for (i = 0; i < off; i++)
		bad += !getbit(out, i);
	CHECK_EQ(bad, 0);
}

/*
 * A short write: the driver took w bytes, the rest of that block is
 * dropped, and the stream goes on with the next block without a seam.
 */
static void test_tx_short(unsigned int off)
{
	uint8_t d1[BLK], d2[BLK], out[2u * BLK], t[BLK + 2];
	struct ssc_align a;
	size_t k, w = BLK - FB;
	unsigned int bad = 0;

	for (k = 0; k < BLK / 2u; k++) {
		d1[2u * k] = (uint8_t)k;
		d1[2u * k + 1u] = 0x10;
		d2[2u * k] = (uint8_t)k;
		d2[2u * k + 1u] = 0x20;
	}
	ssc_align_init(&a, off);
	ssc_align_tx(&a, out, t, d1, BLK, 0, 0);
	ssc_align_tx_commit(&a, t, w);
	ssc_align_tx(&a, out + w, t, d2, BLK, 0, 0);
	ssc_align_tx_commit(&a, t, BLK);
	for (k = 0; off + 16u * (k + 1u) <= (w + BLK) * 8u; k++) {
		unsigned int want = (unsigned int)(k < w / 2u ?
				    (0x10u << 8 | k) : (0x20u << 8 | (k - w / 2u)));

		bad += lane_at(out, off + 16u * k) != want;
	}
	CHECK_EQ(bad, 0);
}

/* A burst: idle bits before it, and an idle frame after it. */
static void test_tx_burst(unsigned int off)
{
	uint8_t d[FB], out[2u * FB], t[2u * FB + 2];
	struct ssc_align a;
	size_t i, k;
	unsigned int bad = 0;

	for (k = 0; k < FB; k++)
		d[k] = (uint8_t)(k + 1u);
	ssc_align_init(&a, off);
	a.tx_carry[0] = a.tx_carry[1] = 0x00;	/* stale from an earlier stream */
	ssc_align_tx(&a, out, t, d, FB, FB, 1);
	for (i = 0; i < off; i++)
		bad += !getbit(out, i);
	for (k = 0; k < FB / 2u; k++)
		bad += lane_at(out, off + 16u * k) !=
		       ((unsigned int)d[2u * k + 1u] << 8 | d[2u * k]);
	for (i = off + FB * 8u; i < 2u * FB * 8u; i++)
		bad += !getbit(out, i);
	CHECK_EQ(bad, 0);
}

/* Transmit then receive through the same offset gives the frames back. */
static void test_roundtrip(unsigned int off)
{
	uint8_t d[NB], bus[NB], t[BLK + 2];
	struct ssc_align tx, rx;
	size_t i, k;
	unsigned int bad = 0;

	for (k = 0; k < NB; k++)
		d[k] = (uint8_t)(k * 7u + 3u);
	ssc_align_init(&tx, off);
	ssc_align_init(&rx, off);
	for (i = 0; i < NBLK; i++) {
		ssc_align_tx(&tx, bus + i * BLK, t, d + i * BLK, BLK, 0, 0);
		ssc_align_tx_commit(&tx, t, BLK);
	}
	for (i = 0; i < NBLK; i++)
		ssc_align_rx(&rx, bus + i * BLK, t, BLK);
	for (k = 2; k < NB; k++)	/* one slot of receive delay */
		bad += bus[k] != d[k - 2u];
	CHECK_EQ(bad, 0);
}

/*
 * The 7412's idle frame: three adjacent 0xFEFE lanes, then 0xD5D5 and
 * 0x0C08, the rest 0xFFFF (observed 2026-10-29). Adjacent idle lanes also
 * read 0xFEFE 8 bits later, two per frame; the true offset still wins.
 */
static void test_idle_offset(unsigned int off)
{
	static const unsigned int frame[36] = {
		0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
		0xffff, 0xffff, 0xfefe, 0xfefe, 0xfefe, 0xd5d5, 0x0c08, 0xffff,
		0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
		0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
		0xffff, 0xffff, 0xffff, 0xffff,
	};
	uint8_t s[72u * 16u + 4u];
	unsigned int cnt[16];
	size_t k, i, n = (sizeof(s) * 8u - off) / 16u;

	memset(s, 0xff, sizeof(s));
	for (k = 0; k < n; k++)
		for (i = 0; i < 16u; i++)
			putbit(s, off + 16u * k + i,
			       (int)(frame[k % 36u] >> (15u - i)) & 1);
	CHECK_EQ(ssc_idle_offset(s, sizeof(s), cnt), off);
	CHECK(cnt[off] > cnt[(off + 8u) % 16u]);
	CHECK(cnt[(off + 8u) % 16u] > 0);
}

/*
 * The alignment watch (ssc_idle_lanes): the idle frame's three 0xFEFE lanes
 * at positions 10-12, then the same stream with one 16-bit word lost (the
 * mask rotates, the offset stays) and with one bit lost (the offset moves,
 * the lanes stay).
 */
static void test_idle_lanes(unsigned int off)
{
	static const unsigned int frame[36] = {
		0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
		0xffff, 0xffff, 0xfefe, 0xfefe, 0xfefe, 0xd5d5, 0x0c08, 0xffff,
		0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
		0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
		0xffff, 0xffff, 0xffff, 0xffff,
	};
	uint8_t s[72u * 16u + 4u];
	uint64_t mask, base;
	size_t k, i, n = (sizeof(s) * 8u - off) / 16u;
	int got;

	memset(s, 0xff, sizeof(s));
	for (k = 0; k < n; k++)
		for (i = 0; i < 16u; i++)
			putbit(s, off + 16u * k + i,
			       (int)(frame[k % 36u] >> (15u - i)) & 1);
	got = ssc_idle_lanes(s, sizeof(s), 36u, &mask);
	CHECK_EQ(got, (int)off);
	base = 7ull << 10;
	CHECK_EQ(mask, base);

	/* a word lost before the stream: every lane one position earlier */
	got = ssc_idle_lanes(s + 2, sizeof(s) - 2u, 36u, &mask);
	CHECK_EQ(got, (int)off);
	CHECK_EQ(mask, 7ull << 9);
	CHECK_EQ(ssc_lanes_rotation(base, mask, 36u), 35);

	/* one bit lost: the offset moves by one, the lanes stay */
	if (off) {
		uint8_t t[sizeof(s)];

		memset(t, 0xff, sizeof(t));
		for (k = 1; k < sizeof(s) * 8u; k++)
			putbit(t, k - 1u, getbit(s, k));
		got = ssc_idle_lanes(t, sizeof(t) - 1u, 36u, &mask);
		CHECK_EQ(got, (int)off - 1);
		CHECK_EQ(mask, base);
	}
}

static void test_lanes_rotation(void)
{
	CHECK_EQ(ssc_lanes_rotation(7ull << 10, 7ull << 10, 36u), 0);
	CHECK_EQ(ssc_lanes_rotation(7ull << 10, 7ull << 12, 36u), 2);
	/* across the frame end: 34, 35, 0 */
	CHECK_EQ(ssc_lanes_rotation(7ull << 10, (3ull << 34) | 1u, 36u), 24);
	CHECK_EQ(ssc_lanes_rotation(7ull << 10, 5ull << 10, 36u), -1);
	CHECK_EQ(ssc_lanes_rotation(0, 7ull, 36u), -1);
}

/*
 * The lane delay: a stream of numbered lanes fed in uneven pieces, shorter
 * and longer than the delay, comes out the same stream, k lanes late, with
 * idle lanes in front.
 */
static void test_lane_delay(unsigned int k)
{
	uint8_t in[2u * 100u], out[2u * 100u];
	struct ssc_lane_delay d;
	size_t at = 0, piece = 2u, i;
	int bad = 0;

	for (i = 0; i < 100u; i++) {
		in[2u * i] = (uint8_t)i;
		in[2u * i + 1u] = (uint8_t)(0x80u | i);
	}
	memcpy(out, in, sizeof(out));
	ssc_lane_delay_init(&d, k);
	while (at < sizeof(out)) {
		size_t n = at + piece > sizeof(out) ? sizeof(out) - at : piece;

		ssc_lane_delay_apply(&d, out + at, n);
		at += n;
		piece = piece == 2u ? 14u : piece == 14u ? 6u :
			piece == 6u ? 132u : 2u;
	}
	for (i = 0; i < 100u; i++) {
		if (i < k)
			bad += out[2u * i] != 0xffu || out[2u * i + 1u] != 0xffu;
		else
			bad += memcmp(out + 2u * i, in + 2u * (i - k), 2) != 0;
	}
	CHECK_EQ(bad, 0);
}

static void test_no_idle(void)
{
	uint8_t s[256];
	unsigned int cnt[16];

	memset(s, 0x55, sizeof(s));
	CHECK_EQ(ssc_idle_offset(s, sizeof(s), cnt), -1);
}

int main(void)
{
	unsigned int off;

	for (off = 0; off < 16u; off++) {
		test_rx(off);
		test_tx(off);
		test_tx_short(off);
		test_tx_burst(off);
		test_roundtrip(off);
		test_idle_offset(off);
		test_idle_lanes(off);
	}
	test_lanes_rotation();
	for (off = 0; off <= 40u; off += 5u)
		test_lane_delay(off);
	test_lane_delay(SSC_LANE_DELAY_MAX);
	test_no_idle();
	return CHECK_DONE();
}

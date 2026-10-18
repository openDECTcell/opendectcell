// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for src/libopendectcell/dect_lc.c: the ISO 8073 Cs-channel checksum (against
 * the ETSI EN 300 175-4 Annex B golden vectors) and Cs reassembly/segmentation.
 * No hardware.
 */
#include "check.h"
#include "../src/libopendectcell/dect_lc.h"

#include <string.h>

/* Annex B.4 EXAMPLE 1: data FE02 -> checksum FE00. EXAMPLE 2: 910001 -> B7B5. */
static void test_checksum_vectors(void)
{
	uint8_t f1[4] = { 0xFE, 0x02, 0x00, 0x00 };	/* 2 data + 2 csum octets */
	uint8_t f2[5] = { 0x91, 0x00, 0x01, 0x00, 0x00 };
	uint8_t cs[2];

	dect_lc_checksum(f1, sizeof(f1), 0, cs);
	CHECK_EQ(cs[0], 0xFE);
	CHECK_EQ(cs[1], 0x00);

	dect_lc_checksum(f2, sizeof(f2), 0, cs);
	CHECK_EQ(cs[0], 0xB7);
	CHECK_EQ(cs[1], 0xB5);
}

/* The link signature XORs into the checksum octets (X=high byte, Y=low byte). */
static void test_checksum_lsig(void)
{
	uint8_t f[5] = { 0x91, 0x00, 0x01, 0x00, 0x00 };
	uint8_t cs[2];

	dect_lc_checksum(f, sizeof(f), 0x1234, cs);
	CHECK_EQ(cs[0], 0xB7 ^ 0x12);
	CHECK_EQ(cs[1], 0xB5 ^ 0x34);
}

/* FLEN rounds (info + 5 overhead) up to a multiple of 5, capped by the channel. */
static void test_flen(void)
{
	CHECK_EQ(dect_lc_flen(0), 5);
	CHECK_EQ(dect_lc_flen(1), 10);	/* 1+5 = 6 -> 10 */
	CHECK_EQ(dect_lc_flen(5), 10);	/* 5+5 = 10 */
	CHECK_EQ(dect_lc_flen(22), 30);	/* spec worked example */
	CHECK_EQ(dect_lc_flen(63), 70);	/* 63+5 = 68 -> 70 (max) */
}

/* Build a Cs frame, then reassemble it fragment by fragment back to the LAPC frame. */
static void test_build_reassemble_roundtrip(void)
{
	uint8_t lapc[5] = { 0x13, 0x00, 0x09, 0xAA, 0xBB };	/* addr,ctrl,LI,info[2] */
	uint8_t frame[DECT_LC_FRAME_MAX];
	uint8_t out[DECT_LC_LAPC_MAX];
	struct dect_lc_rx rx;
	size_t flen, off;
	int r = 0;

	flen = dect_lc_build(lapc, sizeof(lapc), 0, frame, sizeof(frame));
	CHECK_EQ(flen, 10);		/* info_len 2 -> FLEN 10 */
	CHECK_EQ(frame[0], 0x13);
	CHECK_EQ(frame[5], 0xF0);	/* fill begins after the info field */

	dect_lc_rx_init(&rx, 0);
	for (off = 0; off < flen; off += DECT_LC_FRAG_LEN)
		r = dect_lc_rx_fragment(&rx, &frame[off], out, sizeof(out));
	CHECK_EQ(r, (int)sizeof(lapc));	/* the last fragment delivers the frame */
	CHECK_EQ(rx.stats.frames, 1);
	CHECK_EQ(memcmp(out, lapc, sizeof(lapc)), 0);
}

/* A corrupted frame is dropped on the checksum, and reassembly resynchronises. */
static void test_reassemble_bad_checksum(void)
{
	uint8_t lapc[5] = { 0x13, 0x00, 0x09, 0xAA, 0xBB };
	uint8_t frame[DECT_LC_FRAME_MAX];
	uint8_t out[DECT_LC_LAPC_MAX];
	struct dect_lc_rx rx;
	size_t flen;
	int r1, r2;

	flen = dect_lc_build(lapc, sizeof(lapc), 0, frame, sizeof(frame));
	CHECK_EQ(flen, 10);
	frame[3] ^= 0x01;		/* flip one info bit */

	dect_lc_rx_init(&rx, 0);
	r1 = dect_lc_rx_fragment(&rx, &frame[0], out, sizeof(out));
	r2 = dect_lc_rx_fragment(&rx, &frame[5], out, sizeof(out));
	CHECK_EQ(r1, 0);		/* first fragment: more needed */
	CHECK_EQ(r2, -1);		/* checksum fails on the complete frame */
	CHECK_EQ(rx.stats.csum_errors, 1);
	CHECK_EQ(rx.stats.frames, 0);
	CHECK_EQ(rx.len, 0);		/* resynchronised */
	CHECK_EQ(rx.flen, 0);
}

/* A minimum (empty info) frame round-trips as a single 5-octet fragment. */
static void test_empty_info(void)
{
	uint8_t lapc[3] = { 0x11, 0x01, 0x01 };		/* RR-like, LI octet = 0 info */
	uint8_t frame[DECT_LC_FRAME_MAX];
	uint8_t out[DECT_LC_LAPC_MAX];
	struct dect_lc_rx rx;
	size_t flen;
	int r;

	flen = dect_lc_build(lapc, sizeof(lapc), 0, frame, sizeof(frame));
	CHECK_EQ(flen, 5);
	dect_lc_rx_init(&rx, 0);
	r = dect_lc_rx_fragment(&rx, frame, out, sizeof(out));
	CHECK_EQ(r, 3);
	CHECK_EQ(memcmp(out, lapc, 3), 0);
}

int main(void)
{
	test_checksum_vectors();
	test_checksum_lsig();
	test_flen();
	test_build_reassemble_roundtrip();
	test_reassemble_bad_checksum();
	test_empty_info();
	return CHECK_DONE();
}

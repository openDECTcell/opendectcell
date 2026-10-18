// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for the LAPC frame codec in src/libopendectcell/dect_lapc.c: parsing control
 * octets, building I/RR frames, and an end-to-end round-trip through the Lc
 * layer. No hardware.
 */
#include "check.h"
#include "../src/libopendectcell/dect_lapc.h"
#include "../src/libopendectcell/dect_lc.h"

#include <string.h>

static void parse_ctrl(uint8_t addr, uint8_t ctrl, uint8_t li,
		       struct dect_lapc_frame *f)
{
	uint8_t buf[3] = { addr, ctrl, li };

	CHECK_EQ(dect_lapc_parse(buf, 3, f), 0);
}

/* Control-octet decoding for each frame type. */
static void test_parse_types(void)
{
	struct dect_lapc_frame f;

	parse_ctrl(0x13, 0x00, 0x01, &f);	/* FP cmd, I, ns=0 nr=0 */
	CHECK_EQ(f.type, DECT_LAPC_I);
	CHECK_EQ(f.cr, 1); CHECK_EQ(f.sapi, 0); CHECK_EQ(f.lln, 1);
	CHECK_EQ(f.ns, 0); CHECK_EQ(f.nr, 0); CHECK_EQ(f.pf, 0);

	parse_ctrl(0x11, 0x32, 0x01, &f);	/* I, ns=1 nr=1 pf=1 */
	CHECK_EQ(f.type, DECT_LAPC_I);
	CHECK_EQ(f.ns, 1); CHECK_EQ(f.nr, 1); CHECK_EQ(f.pf, 1);

	parse_ctrl(0x11, 0x21, 0x01, &f);	/* RR, nr=1 */
	CHECK_EQ(f.type, DECT_LAPC_RR);
	CHECK_EQ(f.nr, 1);

	parse_ctrl(0x11, 0x25, 0x01, &f);	/* RNR, nr=1 */
	CHECK_EQ(f.type, DECT_LAPC_RNR);
	parse_ctrl(0x11, 0x29, 0x01, &f);	/* REJ, nr=1 */
	CHECK_EQ(f.type, DECT_LAPC_REJ);

	parse_ctrl(0x93, 0x3F, 0x01, &f);	/* SABM, P=1, NLF=1 */
	CHECK_EQ(f.type, DECT_LAPC_SABM);
	CHECK_EQ(f.pf, 1); CHECK_EQ(f.nlf, 1);
	parse_ctrl(0x11, 0x73, 0x01, &f);	/* UA, F=1 */
	CHECK_EQ(f.type, DECT_LAPC_UA);
	parse_ctrl(0x11, 0x0F, 0x01, &f);	/* DM */
	CHECK_EQ(f.type, DECT_LAPC_DM);
	parse_ctrl(0x11, 0x43, 0x01, &f);	/* DISC */
	CHECK_EQ(f.type, DECT_LAPC_DISC);
	parse_ctrl(0x11, 0x03, 0x01, &f);	/* UI */
	CHECK_EQ(f.type, DECT_LAPC_UI);
}

/* The M (more-data) bit of the length indicator is decoded. */
static void test_parse_more(void)
{
	uint8_t buf[5] = { 0x13, 0x00, (2u << 2) | 0x02u | 0x01u, 0xAA, 0xBB };
	struct dect_lapc_frame f;

	CHECK_EQ(dect_lapc_parse(buf, 5, &f), 0);
	CHECK_EQ(f.more, 1);
	CHECK_EQ(f.info_len, 2);
	CHECK_EQ(f.info[0], 0xAA);
}

/* A length indicator that disagrees with the frame length is rejected. */
static void test_parse_bad_len(void)
{
	uint8_t buf[4] = { 0x13, 0x00, (5u << 2) | 0x01u, 0xAA };	/* LI=5, only 1 */
	struct dect_lapc_frame f;

	CHECK_EQ(dect_lapc_parse(buf, 4, &f), -1);
}

/* Build an I-frame and parse it back. */
static void test_build_i_roundtrip(void)
{
	uint8_t info[3] = { 0x01, 0x02, 0x03 };
	uint8_t out[DECT_LC_LAPC_MAX];
	struct dect_lapc_frame f;
	size_t n;

	n = dect_lapc_build_i(out, sizeof(out), 1, 1, 0, 1, 0, 1, info, sizeof(info));
	CHECK_EQ(n, 6);
	CHECK_EQ(out[0], 0x93);		/* cr=1, nlf=1, class A, sapi 0 */
	CHECK_EQ(dect_lapc_parse(out, n, &f), 0);
	CHECK_EQ(f.type, DECT_LAPC_I);
	CHECK_EQ(f.nlf, 1); CHECK_EQ(f.nr, 1); CHECK_EQ(f.more, 1);
	CHECK_EQ(f.info_len, 3);
	CHECK_EQ(memcmp(f.info, info, 3), 0);
}

/* Build an RR frame and parse it back. */
static void test_build_rr_roundtrip(void)
{
	uint8_t out[8];
	struct dect_lapc_frame f;
	size_t n;

	n = dect_lapc_build_rr(out, sizeof(out), 0, 1, 1);
	CHECK_EQ(n, 3);
	CHECK_EQ(out[0], 0x11);		/* cr=0 response */
	CHECK_EQ(dect_lapc_parse(out, n, &f), 0);
	CHECK_EQ(f.type, DECT_LAPC_RR);
	CHECK_EQ(f.nr, 1); CHECK_EQ(f.pf, 1);
}

/* LAPC I-frame -> Lc Cs frame -> fragments -> reassembled -> parsed, unchanged. */
static void test_lapc_over_lc(void)
{
	uint8_t info[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
	uint8_t lapc[DECT_LC_LAPC_MAX], cs[DECT_LC_FRAME_MAX], got[DECT_LC_LAPC_MAX];
	struct dect_lc_rx rx;
	struct dect_lapc_frame f;
	size_t n, flen, off;
	int r = 0;

	n = dect_lapc_build_i(lapc, sizeof(lapc), 1, 0, 0, 0, 0, 0, info, sizeof(info));
	flen = dect_lc_build(lapc, n, 0, cs, sizeof(cs));
	CHECK(flen > 0);

	dect_lc_rx_init(&rx, 0);
	for (off = 0; off < flen; off += DECT_LC_FRAG_LEN)
		r = dect_lc_rx_fragment(&rx, &cs[off], got, sizeof(got));
	CHECK_EQ(r, (int)n);
	CHECK_EQ(dect_lapc_parse(got, (size_t)r, &f), 0);
	CHECK_EQ(f.type, DECT_LAPC_I);
	CHECK_EQ(f.info_len, 4);
	CHECK_EQ(memcmp(f.info, info, 4), 0);
}

int main(void)
{
	test_parse_types();
	test_parse_more();
	test_parse_bad_len();
	test_build_i_roundtrip();
	test_build_rr_roundtrip();
	test_lapc_over_lc();
	return CHECK_DONE();
}

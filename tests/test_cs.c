// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for src/libopendectcell/dect_cs.c: decoding inbound 0x0203 Cs segments and
 * encoding the outbound 0x0244 / 0x0202 C-plane transport mails. No hardware.
 */
#include "check.h"
#include "../src/libopendectcell/dect_cs.h"
#include "../src/libopendectcell/dect_proto.h"

#include <string.h>

/* A 0x0203 mail in the documented 13-byte layout decodes to its fields. */
static void test_decode_segment_13(void)
{
	struct dect_mail m;
	struct dect_cs_seg seg;
	/* {handle=2, 0x01, data[5], 0x00,0x00, tag[3], handle=2} (synthetic bytes) */
	uint8_t p[] = { 0x02, 0x01, 0x12, 0x34, 0x56, 0x78, 0x9a,
			0x00, 0x00, 0x11, 0x22, 0x33, 0x02 };

	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_CPLANE;
	m.dst = DECT_TASK_PCM;
	m.prim = DECT_PRIM_SIGNALLING;
	m.param_len = sizeof(p);
	memcpy(m.param, p, sizeof(p));

	CHECK_EQ(dect_cs_decode_segment(&m, &seg), 0);
	CHECK_EQ(seg.handle, 0x02);
	CHECK_EQ(seg.data[0], 0x12);
	CHECK_EQ(seg.data[4], 0x9a);
	CHECK_EQ(seg.tag[0], 0x11);
	CHECK_EQ(seg.tag[2], 0x33);
}

/* The longer 15-byte form (2-byte ref trailer) seen on hardware still decodes. */
static void test_decode_segment_15(void)
{
	struct dect_mail m;
	struct dect_cs_seg seg;
	uint8_t p[] = { 0x00, 0x01, 0xde, 0xad, 0xbe, 0xef, 0x42,
			0x00, 0x00, 0x0a, 0x0b, 0x0c, 0x00, 0x12, 0x34 };

	memset(&m, 0, sizeof(m));
	m.prim = DECT_PRIM_SIGNALLING;
	m.param_len = sizeof(p);
	memcpy(m.param, p, sizeof(p));

	CHECK_EQ(dect_cs_decode_segment(&m, &seg), 0);
	CHECK_EQ(seg.handle, 0x00);
	CHECK_EQ(seg.data[2], 0xbe);
	CHECK_EQ(seg.tag[1], 0x0b);
}

/* The 06.88 STUB's 7-byte short form decodes, with a zero tag. */
static void test_decode_segment_7(void)
{
	struct dect_mail m;
	struct dect_cs_seg seg;
	uint8_t p[] = { 0x00, 0x01, 0x91, 0x00, 0x01, 0xb7, 0xb5 };

	memset(&m, 0, sizeof(m));
	m.prim = DECT_PRIM_SIGNALLING;
	m.param_len = sizeof(p);
	memcpy(m.param, p, sizeof(p));
	memset(seg.tag, 0xff, sizeof(seg.tag));

	CHECK_EQ(dect_cs_decode_segment(&m, &seg), 0);
	CHECK_EQ(seg.handle, 0x00);
	CHECK_EQ(seg.data[0], 0x91);
	CHECK_EQ(seg.data[4], 0xb5);
	CHECK_EQ(seg.tag[0], 0x00);
	CHECK_EQ(seg.tag[2], 0x00);

	m.param[1] = 0x02;			/* marker */
	CHECK_EQ(dect_cs_decode_segment(&m, &seg), -1);
	m.param[1] = 0x01;
	m.param_len = 6;			/* shorter than the short form */
	CHECK_EQ(dect_cs_decode_segment(&m, &seg), -1);
	m.param_len = 8;			/* between the two forms */
	CHECK_EQ(dect_cs_decode_segment(&m, &seg), -1);
}

/* A wrong primitive or a too-short mail is rejected. */
static void test_decode_rejects(void)
{
	struct dect_mail m;
	struct dect_cs_seg seg;

	memset(&m, 0, sizeof(m));
	m.prim = DECT_PRIM_CONN_UP;
	m.param_len = 13;
	CHECK_EQ(dect_cs_decode_segment(&m, &seg), -1);

	m.prim = DECT_PRIM_SIGNALLING;
	m.param_len = 12;		/* one short of the minimum */
	CHECK_EQ(dect_cs_decode_segment(&m, &seg), -1);
}

/*
 * The 0x01 marker at offset 1 and the copy of the handle at offset 12 are
 * fixed in every one of the 42 252 segments in the corpus, 13- and 15-byte
 * forms alike; a segment that breaks either is malformed, not data.
 */
static void test_decode_rejects_bad_marker_or_handle(void)
{
	struct dect_mail m;
	struct dect_cs_seg seg;
	uint8_t p[] = { 0x01, 0x01, 0x12, 0x34, 0x56, 0x78, 0x9a,
			0x00, 0x00, 0x11, 0x22, 0x33, 0x01 };

	memset(&m, 0, sizeof(m));
	m.prim = DECT_PRIM_SIGNALLING;
	m.param_len = sizeof(p);
	memcpy(m.param, p, sizeof(p));
	CHECK_EQ(dect_cs_decode_segment(&m, &seg), 0);	/* well formed */

	m.param[1] = 0x02;				/* marker */
	CHECK_EQ(dect_cs_decode_segment(&m, &seg), -1);
	m.param[1] = 0x01;
	m.param[12] = 0x02;				/* handle copy */
	CHECK_EQ(dect_cs_decode_segment(&m, &seg), -1);
}

/* APPEND_CONNECTION_DATA packs {handle, n, data} and sets src/dst/prim. */
static void test_append_mail(void)
{
	struct dect_mail m;
	uint8_t data[] = { 0x05, 0x06, 0x07 };

	CHECK_EQ(dect_cs_append_mail(&m, DECT_TASK_PCM, 0x03, data, sizeof(data)), 0);
	CHECK_EQ(m.src, DECT_TASK_PCM);
	CHECK_EQ(m.dst, DECT_TASK_CONTACT_DST);
	CHECK_EQ(m.prim, DECT_PRIM_APPEND_DATA);
	CHECK_EQ(m.param_len, 5);
	CHECK_EQ(m.param[0], 0x03);	/* handle */
	CHECK_EQ(m.param[1], 0x03);	/* n */
	CHECK_EQ(m.param[2], 0x05);
	CHECK_EQ(m.param[4], 0x07);
}

/* Zero length and oversized payloads are refused. */
static void test_append_bounds(void)
{
	struct dect_mail m;
	uint8_t big[DECT_APPEND_BUF_MAX + 1];

	memset(big, 0xab, sizeof(big));
	CHECK_EQ(dect_cs_append_mail(&m, DECT_TASK_PCM, 0, big, 0), -1);
	CHECK_EQ(dect_cs_append_mail(&m, DECT_TASK_PCM, 0, big, sizeof(big)), -1);
	CHECK_EQ(dect_cs_append_mail(&m, DECT_TASK_PCM, 0, big, DECT_APPEND_BUF_MAX), 0);
	CHECK_EQ(m.param_len, DECT_APPEND_BUF_MAX + 2u);
}

int main(void)
{
	test_decode_segment_13();
	test_decode_segment_15();
	test_decode_segment_7();
	test_decode_rejects();
	test_decode_rejects_bad_marker_or_handle();
	test_append_mail();
	test_append_bounds();
	return CHECK_DONE();
}

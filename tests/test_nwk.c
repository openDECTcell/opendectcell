// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for src/libopendectcell/dect_nwk.c: the S-format header and variable-length
 * IE codec. No hardware.
 */
#include "check.h"
#include "../src/libopendectcell/dect_nwk.h"

#include <string.h>

/* Header build/parse round-trip and exact wire octets. */
static void test_header(void)
{
	uint8_t out[2];
	struct dect_nwk_hdr h;
	const uint8_t *ies;
	size_t ies_len;

	CHECK_EQ(dect_nwk_build_hdr(out, sizeof(out), DECT_PD_MM,
				    DECT_TI_RESPONDER, 3,
				    DECT_MM_ACCESS_RIGHTS_ACCEPT), 2);
	CHECK_EQ(out[0], 0xB5);		/* F=1, TV=3, PD=5 */
	CHECK_EQ(out[1], 0x45);

	CHECK_EQ(dect_nwk_parse(out, 2, &h, &ies, &ies_len), 0);
	CHECK_EQ(h.pd, DECT_PD_MM);
	CHECK_EQ(h.ti_flag, 1);
	CHECK_EQ(h.ti_value, 3);
	CHECK_EQ(h.msg_type, DECT_MM_ACCESS_RIGHTS_ACCEPT);
	CHECK_EQ(ies_len, 0);
}

/* A short buffer is rejected. */
static void test_header_short(void)
{
	uint8_t b[1] = { 0x00 };
	struct dect_nwk_hdr h;
	const uint8_t *ies;
	size_t ies_len;

	CHECK_EQ(dect_nwk_parse(b, 1, &h, &ies, &ies_len), -1);
}

/* Build a message with several IEs, then find each one. */
static void test_put_find_ie(void)
{
	uint8_t msg[64];
	uint8_t ipui[5] = { 0x00, 0x11, 0x22, 0x33, 0x44 };
	uint8_t rand[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	struct dect_nwk_hdr h;
	const uint8_t *ies, *p;
	size_t off, ies_len, len;

	off = dect_nwk_build_hdr(msg, sizeof(msg), DECT_PD_MM, DECT_TI_ORIGINATOR,
				 1, DECT_MM_ACCESS_RIGHTS_REQUEST);
	off = dect_nwk_put_ie(msg, sizeof(msg), off, DECT_IE_PORTABLE_IDENTITY,
			      ipui, sizeof(ipui));
	CHECK(off > 0);
	off = dect_nwk_put_ie(msg, sizeof(msg), off, DECT_IE_RAND, rand, sizeof(rand));
	CHECK(off > 0);

	CHECK_EQ(dect_nwk_parse(msg, off, &h, &ies, &ies_len), 0);
	CHECK_EQ(h.msg_type, DECT_MM_ACCESS_RIGHTS_REQUEST);

	p = dect_nwk_find_ie(ies, ies_len, DECT_IE_PORTABLE_IDENTITY, &len);
	CHECK(p != NULL);
	CHECK_EQ(len, 5);
	CHECK_EQ(memcmp(p, ipui, 5), 0);

	p = dect_nwk_find_ie(ies, ies_len, DECT_IE_RAND, &len);
	CHECK(p != NULL);
	CHECK_EQ(len, 8);
	CHECK_EQ(memcmp(p, rand, 8), 0);

	/* An IE that is not present returns NULL. */
	p = dect_nwk_find_ie(ies, ies_len, DECT_IE_RES, &len);
	CHECK(p == NULL);
	CHECK_EQ(len, 0);
}

/* A single-octet fixed IE (bit 8 set) between variable IEs is skipped cleanly. */
static void test_skip_fixed_ie(void)
{
	uint8_t ies[16];
	const uint8_t *p;
	size_t off = 0, len;

	ies[off++] = 0x95;		/* a single-octet fixed IE: must be skipped */
	off = dect_nwk_put_ie(ies, sizeof(ies), off, DECT_IE_RES,
			      (const uint8_t[]){ 0xAA, 0xBB, 0xCC, 0xDD }, 4);
	CHECK(off > 0);

	p = dect_nwk_find_ie(ies, off, DECT_IE_RES, &len);
	CHECK(p != NULL);
	CHECK_EQ(len, 4);
	CHECK_EQ(p[0], 0xAA);
}

/* A double-octet fixed IE (0xE0-0xEF) is skipped as two octets, not one. */
static void test_skip_double_octet_ie(void)
{
	uint8_t ies[16];
	const uint8_t *p;
	size_t off = 0, len;

	ies[off++] = DECT_IE_BASIC_SERVICE;	/* 0xE0: double-octet element ... */
	ies[off++] = 0x00;			/* ... + its value octet */
	off = dect_nwk_put_ie(ies, sizeof(ies), off, DECT_IE_PORTABLE_IDENTITY,
			      (const uint8_t[]){ 0x80, 0xB4, 0x01 }, 3);
	CHECK(off > 0);

	/* If the double octet were skipped as one byte, this would misalign. */
	p = dect_nwk_find_ie(ies, off, DECT_IE_PORTABLE_IDENTITY, &len);
	CHECK(p != NULL);
	CHECK_EQ(len, 3);
	CHECK_EQ(p[0], 0x80);
}

/* A declared length that overruns the block is rejected (no over-read). */
static void test_truncated_ie(void)
{
	uint8_t ies[4] = { DECT_IE_RAND, 0x08, 0x01, 0x02 };	/* says 8, has 2 */
	const uint8_t *p;
	size_t len;

	p = dect_nwk_find_ie(ies, sizeof(ies), DECT_IE_RAND, &len);
	CHECK(p == NULL);
}

int main(void)
{
	test_header();
	test_header_short();
	test_put_find_ie();
	test_skip_fixed_ie();
	test_skip_double_octet_ie();
	test_truncated_ie();
	return CHECK_DONE();
}

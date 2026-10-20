// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for src/libopendectcell/dect_clip.c: CALLING-PARTY-NUMBER (0x6C) and
 * CALLING-PARTY-NAME (0x6D), and their appearance in an FP-originated
 * CC-SETUP. No hardware.
 *
 * The byte vectors are synthetic, derived from EN 300 175-5 §7.7.9/§7.7.53.
 * They exercise the serializer's bit positions; they are not captures and say
 * nothing about what a particular handset renders.
 */
#include "check.h"
#include "../src/libopendectcell/dect_clip.h"
#include "../src/libopendectcell/dect_cc.h"
#include "../src/libopendectcell/dect_nwk.h"

#include <string.h>

#define OFF0	2u	/* IEs only ever follow the 2-octet NWK header */

static size_t build_number(uint8_t *buf, size_t cap, const char *num,
			   enum dect_clip_ton ton, enum dect_clip_npi npi,
			   enum dect_clip_presentation pres,
			   enum dect_clip_screening scr)
{
	struct dect_clip id;

	dect_clip_init(&id);
	if (dect_clip_set_number(&id, num, ton, npi, pres, scr) != 0)
		return 0;
	return dect_clip_put_number(buf, cap, OFF0, &id);
}

/* "0123", national TON + E.164 NPI, allowed, network-provided screening. */
static void test_number_vector(void)
{
	static const uint8_t want[] = { 0x6C, 0x06, 0x21, 0x83,
					0x30, 0x31, 0x32, 0x33 };
	uint8_t buf[32];
	size_t off;

	memset(buf, 0, sizeof(buf));
	off = build_number(buf, sizeof(buf), "0123", DECT_CLIP_TON_NATIONAL,
			   DECT_CLIP_NPI_E164, DECT_CLIP_PRES_ALLOWED,
			   DECT_CLIP_SCR_NETWORK);
	CHECK_EQ(off, OFF0 + sizeof(want));
	CHECK(memcmp(buf + OFF0, want, sizeof(want)) == 0);
}

/*
 * The descriptor pair the stock formatter uses on its ordinary path is
 * `01 81`: TON unknown, NPI E.164, presentation allowed, screening
 * user-provided-verified-and-passed. Recorded here because we can produce it,
 * not because it is a mandate -- it asserts a screening verdict a caller
 * should only claim when it holds.
 */
static void test_number_stock_descriptors(void)
{
	uint8_t buf[32];
	size_t off;

	memset(buf, 0, sizeof(buf));
	off = build_number(buf, sizeof(buf), "4711", DECT_CLIP_TON_UNKNOWN,
			   DECT_CLIP_NPI_E164, DECT_CLIP_PRES_ALLOWED,
			   DECT_CLIP_SCR_USER_PASSED);
	CHECK_EQ(off, OFF0 + 2 + 2 + 4);
	CHECK_EQ(buf[OFF0], 0x6C);
	CHECK_EQ(buf[OFF0 + 1], 6);		/* 2 descriptors + 4 digits */
	CHECK_EQ(buf[OFF0 + 2], 0x01);
	CHECK_EQ(buf[OFF0 + 3], 0x81);
}

/* The address is IA5 text, not packed BCD: a '+' survives as 0x2B. */
static void test_number_is_ia5(void)
{
	uint8_t buf[32];
	size_t off;

	memset(buf, 0, sizeof(buf));
	off = build_number(buf, sizeof(buf), "+4930", DECT_CLIP_TON_INTERNATIONAL,
			   DECT_CLIP_NPI_E164, DECT_CLIP_PRES_ALLOWED,
			   DECT_CLIP_SCR_NETWORK);
	CHECK_EQ(off, OFF0 + 2 + 2 + 5);
	CHECK_EQ(buf[OFF0 + 2], 0x11);		/* TON 1, NPI 1 */
	CHECK_EQ(buf[OFF0 + 4], 0x2B);		/* '+' */
	CHECK_EQ(buf[OFF0 + 5], 0x34);		/* '4' */
}

/*
 * Anything but "presentation allowed" emits nothing at all. An absent IE does
 * not say why it is absent, which is the point: withheld, unavailable and
 * never-stated must not be distinguishable on the air.
 */
static void test_number_privacy_omits(void)
{
	enum dect_clip_presentation p[] = {
		DECT_CLIP_PRES_RESTRICTED, DECT_CLIP_PRES_UNAVAILABLE,
		DECT_CLIP_PRES_RESERVED, DECT_CLIP_PRES_UNSET
	};
	uint8_t buf[32];
	size_t i;

	for (i = 0; i < sizeof(p) / sizeof(p[0]); i++) {
		memset(buf, 0xEE, sizeof(buf));
		CHECK_EQ(build_number(buf, sizeof(buf), "0123",
				      DECT_CLIP_TON_NATIONAL,
				      DECT_CLIP_NPI_E164, p[i],
				      DECT_CLIP_SCR_NETWORK), OFF0);
		CHECK_EQ(buf[OFF0], 0xEE);	/* nothing written */
	}
}

/* A fresh identity emits neither IE: presentation starts unset. */
static void test_init_emits_nothing(void)
{
	struct dect_clip id;
	uint8_t buf[32];

	dect_clip_init(&id);
	memset(buf, 0xEE, sizeof(buf));
	CHECK_EQ(dect_clip_put_number(buf, sizeof(buf), OFF0, &id), OFF0);
	CHECK_EQ(dect_clip_put_name(buf, sizeof(buf), OFF0, &id, 0), OFF0);
	CHECK_EQ(buf[OFF0], 0xEE);
}

static void test_number_validation(void)
{
	struct dect_clip id;
	char toolong[DECT_CLIP_NUMBER_MAX + 2];

	dect_clip_init(&id);
	CHECK_EQ(dect_clip_set_number(&id, "", DECT_CLIP_TON_UNKNOWN,
				      DECT_CLIP_NPI_E164,
				      DECT_CLIP_PRES_ALLOWED,
				      DECT_CLIP_SCR_NETWORK), -1);
	CHECK_EQ(dect_clip_set_number(&id, NULL, DECT_CLIP_TON_UNKNOWN,
				      DECT_CLIP_NPI_E164,
				      DECT_CLIP_PRES_ALLOWED,
				      DECT_CLIP_SCR_NETWORK), -1);
	/* a tab, and DEL, which the stock converter does not filter */
	CHECK_EQ(dect_clip_set_number(&id, "01\t23", DECT_CLIP_TON_UNKNOWN,
				      DECT_CLIP_NPI_E164,
				      DECT_CLIP_PRES_ALLOWED,
				      DECT_CLIP_SCR_NETWORK), -1);
	CHECK_EQ(dect_clip_set_number(&id, "01\x7F" "23", DECT_CLIP_TON_UNKNOWN,
				      DECT_CLIP_NPI_E164,
				      DECT_CLIP_PRES_ALLOWED,
				      DECT_CLIP_SCR_NETWORK), -1);
	memset(toolong, '7', sizeof(toolong) - 1);
	toolong[sizeof(toolong) - 1] = '\0';
	CHECK_EQ(dect_clip_set_number(&id, toolong, DECT_CLIP_TON_UNKNOWN,
				      DECT_CLIP_NPI_E164,
				      DECT_CLIP_PRES_ALLOWED,
				      DECT_CLIP_SCR_NETWORK), -1);
}

/* "Zoe" with a diaeresis, as UTF-8: allowed + UTF-8 + network-provided. */
static void test_name_vector(void)
{
	static const uint8_t want[] = { 0x6D, 0x05, 0x07,
					0x5A, 0x6F, 0xC3, 0xAB };
	struct dect_clip id;
	uint8_t buf[32];
	size_t off;

	dect_clip_init(&id);
	CHECK_EQ(dect_clip_set_name(&id, "Zo\xC3\xAB", DECT_CLIP_ALPHABET_UTF8,
				    DECT_CLIP_PRES_ALLOWED,
				    DECT_CLIP_SCR_NETWORK), 0);
	memset(buf, 0, sizeof(buf));
	off = dect_clip_put_name(buf, sizeof(buf), OFF0, &id, 0);
	CHECK_EQ(off, OFF0 + sizeof(want));
	CHECK(memcmp(buf + OFF0, want, sizeof(want)) == 0);
}

/* Metadata values: allowed + UTF-8 + not screened is 0x04, + passed is 0x05,
 * and the DECT standard alphabet with no screening is 0x00. */
static void test_name_metadata(void)
{
	struct dect_clip id;
	uint8_t buf[32];

	dect_clip_init(&id);
	CHECK_EQ(dect_clip_set_name(&id, "Ann", DECT_CLIP_ALPHABET_UTF8,
				    DECT_CLIP_PRES_ALLOWED,
				    DECT_CLIP_SCR_USER_UNSCREENED), 0);
	dect_clip_put_name(buf, sizeof(buf), OFF0, &id, 0);
	CHECK_EQ(buf[OFF0 + 2], 0x04);

	CHECK_EQ(dect_clip_set_name(&id, "Ann", DECT_CLIP_ALPHABET_UTF8,
				    DECT_CLIP_PRES_ALLOWED,
				    DECT_CLIP_SCR_USER_PASSED), 0);
	dect_clip_put_name(buf, sizeof(buf), OFF0, &id, 0);
	CHECK_EQ(buf[OFF0 + 2], 0x05);

	CHECK_EQ(dect_clip_set_name(&id, "Ann", DECT_CLIP_ALPHABET_DECT,
				    DECT_CLIP_PRES_ALLOWED,
				    DECT_CLIP_SCR_USER_UNSCREENED), 0);
	dect_clip_put_name(buf, sizeof(buf), OFF0, &id, 0);
	CHECK_EQ(buf[OFF0 + 2], 0x00);
}

/* On a UTF-8 profile the four German characters survive unchanged -- we never
 * transliterate them to "aeoeuess" behind the caller's back. */
static void test_name_utf8_preserved(void)
{
	static const uint8_t want[] = { 0xC3, 0x84, 0xC3, 0x96,
					0xC3, 0x9C, 0xC3, 0x9F };
	struct dect_clip id;
	uint8_t buf[32];
	size_t off;

	dect_clip_init(&id);
	CHECK_EQ(dect_clip_set_name(&id, "\xC3\x84\xC3\x96\xC3\x9C\xC3\x9F",
				    DECT_CLIP_ALPHABET_UTF8,
				    DECT_CLIP_PRES_ALLOWED,
				    DECT_CLIP_SCR_USER_UNSCREENED), 0);
	off = dect_clip_put_name(buf, sizeof(buf), OFF0, &id, 0);
	CHECK_EQ(off, OFF0 + 2 + 1 + sizeof(want));
	CHECK_EQ(buf[OFF0 + 1], 1 + sizeof(want));
	CHECK(memcmp(buf + OFF0 + 3, want, sizeof(want)) == 0);
}

/*
 * The truncation hazard, which is the whole reason this is a module and not
 * four lines in dect_cc.c: a byte budget that falls inside a multi-byte
 * character must drop the character, never half of it.
 */
static void test_name_utf8_truncation(void)
{
	const char *aouss = "\xC3\x84\xC3\x96\xC3\x9C\xC3\x9F";
	struct dect_clip id;
	uint8_t buf[32];
	size_t off, i;

	CHECK_EQ(dect_clip_utf8_trunc(aouss, 8), 8);
	CHECK_EQ(dect_clip_utf8_trunc(aouss, 7), 6);
	CHECK_EQ(dect_clip_utf8_trunc(aouss, 3), 2);
	CHECK_EQ(dect_clip_utf8_trunc(aouss, 1), 0);
	CHECK_EQ(dect_clip_utf8_trunc(aouss, 0), 0);

	dect_clip_init(&id);
	CHECK_EQ(dect_clip_set_name(&id, aouss, DECT_CLIP_ALPHABET_UTF8,
				    DECT_CLIP_PRES_ALLOWED,
				    DECT_CLIP_SCR_USER_UNSCREENED), 0);

	/* A budget of 3 carries one complete character, not one and a half. */
	memset(buf, 0, sizeof(buf));
	off = dect_clip_put_name(buf, sizeof(buf), OFF0, &id, 3);
	CHECK_EQ(off, OFF0 + 2 + 1 + 2);
	CHECK_EQ(buf[OFF0 + 1], 3);
	CHECK_EQ(buf[OFF0 + 3], 0xC3);
	CHECK_EQ(buf[OFF0 + 4], 0x84);

	/* A budget too small for any character omits the IE entirely. */
	memset(buf, 0xEE, sizeof(buf));
	CHECK_EQ(dect_clip_put_name(buf, sizeof(buf), OFF0, &id, 1), OFF0);
	CHECK_EQ(buf[OFF0], 0xEE);

	/* At no budget is a lone lead or continuation byte ever emitted. */
	for (i = 1; i <= 8; i++) {
		size_t n;

		memset(buf, 0, sizeof(buf));
		off = dect_clip_put_name(buf, sizeof(buf), OFF0, &id, i);
		if (off == OFF0)
			continue;
		n = buf[OFF0 + 1] - 1u;		/* encoded name bytes */
		CHECK_EQ(n % 2u, 0u);		/* every character is 2 bytes */
		CHECK_EQ(dect_clip_utf8_trunc(aouss, n), n);
	}
}

static void test_name_validation(void)
{
	struct dect_clip id;

	dect_clip_init(&id);
	/* malformed: lone continuation byte, truncated sequence, overlong,
	 * surrogate half, and a C0 control */
	CHECK_EQ(dect_clip_set_name(&id, "\x80", DECT_CLIP_ALPHABET_UTF8,
				    DECT_CLIP_PRES_ALLOWED,
				    DECT_CLIP_SCR_NETWORK), -1);
	CHECK_EQ(dect_clip_set_name(&id, "A\xC3", DECT_CLIP_ALPHABET_UTF8,
				    DECT_CLIP_PRES_ALLOWED,
				    DECT_CLIP_SCR_NETWORK), -1);
	CHECK_EQ(dect_clip_set_name(&id, "\xC0\xAF", DECT_CLIP_ALPHABET_UTF8,
				    DECT_CLIP_PRES_ALLOWED,
				    DECT_CLIP_SCR_NETWORK), -1);
	CHECK_EQ(dect_clip_set_name(&id, "\xED\xA0\x80", DECT_CLIP_ALPHABET_UTF8,
				    DECT_CLIP_PRES_ALLOWED,
				    DECT_CLIP_SCR_NETWORK), -1);
	CHECK_EQ(dect_clip_set_name(&id, "A\x01" "B", DECT_CLIP_ALPHABET_UTF8,
				    DECT_CLIP_PRES_ALLOWED,
				    DECT_CLIP_SCR_NETWORK), -1);
	/* a non-ASCII name declared as the DECT alphabet is refused rather
	 * than silently reinterpreted */
	CHECK_EQ(dect_clip_set_name(&id, "\xC3\x84", DECT_CLIP_ALPHABET_DECT,
				    DECT_CLIP_PRES_ALLOWED,
				    DECT_CLIP_SCR_NETWORK), -1);
	/* but a 4-byte code point is fine */
	CHECK_EQ(dect_clip_set_name(&id, "\xF0\x9F\x99\x82",
				    DECT_CLIP_ALPHABET_UTF8,
				    DECT_CLIP_PRES_ALLOWED,
				    DECT_CLIP_SCR_NETWORK), 0);
}

/* An IE that does not fit is dropped, not half-written. */
static void test_no_room(void)
{
	struct dect_clip id;
	uint8_t buf[8];

	dect_clip_init(&id);
	CHECK_EQ(dect_clip_set_number(&id, "0123456789",
				      DECT_CLIP_TON_NATIONAL,
				      DECT_CLIP_NPI_E164,
				      DECT_CLIP_PRES_ALLOWED,
				      DECT_CLIP_SCR_NETWORK), 0);
	CHECK_EQ(dect_clip_put_number(buf, sizeof(buf), OFF0, &id), 0);
}

/*
 * In a CC-SETUP: without an identity the message is byte-identical to the one
 * G7 proved, and with one the two IEs are appended after BASIC-SERVICE and
 * parse back out.
 */
static void test_setup_carries_identity(void)
{
	static const uint8_t port_id[] = { 0x20, 0x10, 0x00, 0x11, 0x22 };
	static const uint8_t fixed_id[] = { 0x20, 0x09, 0x88, 0x77, 0x66 };
	struct dect_cc plain, with;
	struct dect_clip id;
	uint8_t a[DECT_LAPC_MSG_MAX], b[DECT_LAPC_MSG_MAX];
	const uint8_t *ies, *p;
	struct dect_nwk_hdr h;
	size_t la, lb, ies_len, n;

	dect_cc_init(&plain);
	dect_cc_init(&with);
	dect_clip_init(&id);
	CHECK_EQ(dect_clip_set_number(&id, "0123", DECT_CLIP_TON_NATIONAL,
				      DECT_CLIP_NPI_E164,
				      DECT_CLIP_PRES_ALLOWED,
				      DECT_CLIP_SCR_NETWORK), 0);
	CHECK_EQ(dect_clip_set_name(&id, "Zo\xC3\xAB", DECT_CLIP_ALPHABET_UTF8,
				    DECT_CLIP_PRES_ALLOWED,
				    DECT_CLIP_SCR_NETWORK), 0);

	CHECK_EQ(dect_cc_setup(&plain, port_id, sizeof(port_id),
			       fixed_id, sizeof(fixed_id), 0x08), 0);
	CHECK_EQ(dect_cc_setup_id(&with, port_id, sizeof(port_id),
				  fixed_id, sizeof(fixed_id), 0x08, &id), 0);
	la = dect_cc_poll(&plain, a, sizeof(a));
	lb = dect_cc_poll(&with, b, sizeof(b));
	CHECK(la > 0 && lb > la);
	CHECK(memcmp(a, b, la) == 0);		/* the proven prefix is intact */
	CHECK_EQ(lb - la, (2 + 2 + 4) + (2 + 1 + 4));

	CHECK_EQ(dect_nwk_parse(b, lb, &h, &ies, &ies_len), 0);
	CHECK_EQ(h.msg_type, DECT_CC_SETUP);
	p = dect_nwk_find_ie(ies, ies_len, DECT_IE_CALLING_PARTY_NUMBER, &n);
	CHECK(p != NULL);
	CHECK_EQ(n, 6u);
	if (p) {
		CHECK_EQ(p[0], 0x21);
		CHECK_EQ(p[1], 0x83);
		CHECK(memcmp(p + 2, "0123", 4) == 0);
	}
	p = dect_nwk_find_ie(ies, ies_len, DECT_IE_CALLING_PARTY_NAME, &n);
	CHECK(p != NULL);
	CHECK_EQ(n, 5u);
	if (p) {
		CHECK_EQ(p[0], 0x07);
		CHECK(memcmp(p + 1, "Zo\xC3\xAB", 4) == 0);
	}

	/* A withheld identity leaves the SETUP exactly as it was. */
	dect_cc_init(&with);
	id.number_presentation = DECT_CLIP_PRES_RESTRICTED;
	id.name_presentation = DECT_CLIP_PRES_RESTRICTED;
	CHECK_EQ(dect_cc_setup_id(&with, port_id, sizeof(port_id),
				  fixed_id, sizeof(fixed_id), 0x08, &id), 0);
	lb = dect_cc_poll(&with, b, sizeof(b));
	CHECK_EQ(lb, la);
	CHECK(memcmp(a, b, la) == 0);
}

int main(void)
{
	test_number_vector();
	test_number_stock_descriptors();
	test_number_is_ia5();
	test_number_privacy_omits();
	test_init_emits_nothing();
	test_number_validation();
	test_name_vector();
	test_name_metadata();
	test_name_utf8_preserved();
	test_name_utf8_truncation();
	test_name_validation();
	test_no_room();
	test_setup_carries_identity();
	return CHECK_DONE();
}

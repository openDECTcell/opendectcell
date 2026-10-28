// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for src/libopendectcell/dect_callinfo.c: <<CALL-INFORMATION>> (0x7E).
 * No hardware.
 *
 * The subrecord vectors are the worked examples of EN 300 175-5 V2.9.1
 * §7.7.56 (line-id 2 = 00 00 82, call-id 2 = 01 00 82); the rest follow the
 * same field table. They test the serializer, not what a handset displays.
 */
#include "check.h"
#include "../src/libopendectcell/dect_callinfo.h"

#include <string.h>

#define OFF0	2u	/* IEs only ever follow the 2-octet NWK header */

/* The standard's own examples, one subrecord each. */
static void test_standard_examples(void)
{
	static const uint8_t line2[] = { 0x7E, 0x03, 0x00, 0x00, 0x82 };
	static const uint8_t call2[] = { 0x7E, 0x03, 0x01, 0x00, 0x82 };
	struct dect_callinfo ci;
	uint8_t buf[32];
	size_t off;

	dect_callinfo_init(&ci);
	ci.has_line = 1;
	ci.line_id = 2;
	off = dect_callinfo_put(buf, sizeof(buf), OFF0, &ci);
	CHECK_EQ(off, OFF0 + sizeof(line2));
	CHECK(memcmp(buf + OFF0, line2, sizeof(line2)) == 0);

	dect_callinfo_init(&ci);
	ci.has_call_id = 1;
	ci.call_id = 2;
	off = dect_callinfo_put(buf, sizeof(buf), OFF0, &ci);
	CHECK_EQ(off, OFF0 + sizeof(call2));
	CHECK(memcmp(buf + OFF0, call2, sizeof(call2)) == 0);
}

/*
 * What our one-line FP sends for an incoming call: line 0, line type VoIP
 * (NDT = significant delay, SCT = common parallel call procedures).
 */
static void test_line_and_type(void)
{
	static const uint8_t want[] = { 0x7E, 0x06,
					0x00, 0x00, 0x80,
					0x00, 0x05, 0x81 };
	struct dect_callinfo ci;
	uint8_t buf[32];
	size_t off;

	dect_callinfo_init(&ci);
	ci.has_line = 1;
	ci.line_id = 0;
	ci.has_line_type = 1;
	ci.line_type = DECT_CALLINFO_NDT_DELAY;
	off = dect_callinfo_put(buf, sizeof(buf), OFF0, &ci);
	CHECK_EQ(off, OFF0 + sizeof(want));
	CHECK(memcmp(buf + OFF0, want, sizeof(want)) == 0);
}

/* All four, in TS 102 527-3 Figure 42 order; call status is subtype 01. */
static void test_full_order(void)
{
	static const uint8_t want[] = { 0x7E, 0x0C,
					0x00, 0x00, 0x80,
					0x00, 0x05, 0x83,
					0x01, 0x00, 0x81,
					0x02, 0x01, 0x81 };
	struct dect_callinfo ci;
	uint8_t buf[32];
	size_t off;

	dect_callinfo_init(&ci);
	/* set in reverse to show the order is the encoder's, not ours */
	ci.has_call_status = 1;
	ci.call_status = DECT_CS_CALL_SETUP;
	ci.has_call_id = 1;
	ci.call_id = 1;
	ci.has_line_type = 1;
	ci.line_type = DECT_CALLINFO_NDT_DELAY | DECT_CALLINFO_SCT_INBAND;
	ci.has_line = 1;
	ci.line_id = 0;
	off = dect_callinfo_put(buf, sizeof(buf), OFF0, &ci);
	CHECK_EQ(off, OFF0 + sizeof(want));
	CHECK(memcmp(buf + OFF0, want, sizeof(want)) == 0);
}

/* Nothing asked for: nothing written, offset unchanged. */
static void test_empty(void)
{
	struct dect_callinfo ci;
	uint8_t buf[8];

	memset(buf, 0xAA, sizeof(buf));
	dect_callinfo_init(&ci);
	CHECK(dect_callinfo_valid(&ci));
	CHECK_EQ(dect_callinfo_put(buf, sizeof(buf), OFF0, &ci), OFF0);
	CHECK_EQ(buf[OFF0], 0xAA);
}

static void test_refusals(void)
{
	struct dect_callinfo ci;
	uint8_t buf[32];

	/* "None" is the PP's request for FP-managed selection, not ours */
	dect_callinfo_init(&ci);
	ci.has_line = 1;
	ci.line_id = DECT_CALLINFO_LINE_NONE;
	CHECK(!dect_callinfo_valid(&ci));
	CHECK_EQ(dect_callinfo_put(buf, sizeof(buf), OFF0, &ci), 0);
	ci.line_id = 126;
	CHECK(dect_callinfo_valid(&ci));

	/* line type describes a line in the same IE; alone it means nothing */
	dect_callinfo_init(&ci);
	ci.has_line_type = 1;
	ci.line_type = DECT_CALLINFO_NDT_DELAY;
	CHECK(!dect_callinfo_valid(&ci));

	/* bits 7-3 of the line type octet are reserved */
	ci.has_line = 1;
	ci.line_type = 0x04;
	CHECK(!dect_callinfo_valid(&ci));
	ci.line_type = 0x03;
	CHECK(dect_callinfo_valid(&ci));

	/* multi-octet values are not encoded */
	dect_callinfo_init(&ci);
	ci.has_call_id = 1;
	ci.call_id = 0x80;
	CHECK(!dect_callinfo_valid(&ci));
	ci.call_id = 0x7F;
	CHECK(dect_callinfo_valid(&ci));

	/* reserved call statuses (7, 8, above 0x10) are refused */
	dect_callinfo_init(&ci);
	ci.has_call_status = 1;
	ci.call_status = 0x07;
	CHECK(!dect_callinfo_valid(&ci));
	ci.call_status = 0x08;
	CHECK(!dect_callinfo_valid(&ci));
	ci.call_status = 0x11;
	CHECK(!dect_callinfo_valid(&ci));
	ci.call_status = DECT_CS_SCREENING_CONNECT;
	CHECK(dect_callinfo_valid(&ci));
	ci.call_status = DECT_CS_IDLE;
	CHECK(dect_callinfo_valid(&ci));
}

/* An IE that does not fit is 0, never a partial write past cap. */
static void test_no_room(void)
{
	struct dect_callinfo ci;
	uint8_t buf[OFF0 + 4];

	dect_callinfo_init(&ci);
	ci.has_line = 1;
	ci.has_call_id = 1;
	CHECK_EQ(dect_callinfo_put(buf, sizeof(buf), OFF0, &ci), 0);
}

/* What the A540 sends with its dialled digits: line 'None', call id 1. */
static void test_parse_a540_none(void)
{
	static const uint8_t v[] = { 0x00, 0x00, 0xff, 0x01, 0x00, 0x81 };
	static const uint8_t full[] = { 0x00, 0x00, 0x82, 0x00, 0x05, 0x81,
					0x01, 0x00, 0x81, 0x02, 0x01, 0x81 };
	static const uint8_t cut[] = { 0x00, 0x00 };
	static const uint8_t open_value[] = { 0x00, 0x00, 0x05 };
	struct dect_callinfo ci;

	CHECK_EQ(dect_callinfo_parse(v, sizeof(v), &ci), 0);
	CHECK_EQ(ci.has_line, 1);
	CHECK_EQ(ci.line_id, DECT_CALLINFO_LINE_NONE);
	CHECK_EQ(ci.has_call_id, 1);
	CHECK_EQ(ci.call_id, 1);
	CHECK_EQ(ci.has_call_status, 0);
	/* stock's SETUP to the A540 (A-0125): line 2, VoIP, id 1, setup */
	CHECK_EQ(dect_callinfo_parse(full, sizeof(full), &ci), 0);
	CHECK_EQ(ci.line_id, 2);
	CHECK_EQ(ci.line_type, DECT_CALLINFO_NDT_DELAY);
	CHECK_EQ(ci.call_status, DECT_CS_CALL_SETUP);
	CHECK_EQ(dect_callinfo_parse(cut, sizeof(cut), &ci), -1);
	CHECK_EQ(dect_callinfo_parse(open_value, sizeof(open_value), &ci), -1);
}

int main(void)
{
	test_parse_a540_none();
	test_standard_examples();
	test_line_and_type();
	test_full_order();
	test_empty();
	test_refusals();
	test_no_room();
	return CHECK_DONE();
}

// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for src/libopendectcell/dect_iwu.c: the CAT-iq list access responder,
 * TS 102 527-3 V1.7.1 clause 7.4.10. Request bytes are the ones a Gigaset
 * A540 CAT sends (work/box-tmp/vCOLLIDE-20261024-183515.log); the expected
 * replies are built from the standard's tables, not from a capture.
 */
#include "check.h"
#include "../src/libopendectcell/dect_iwu.h"

#include <string.h>

static struct dect_iwu_out out;

static void handle(struct dect_iwu *iwu, const uint8_t *in, size_t n)
{
	out.count = 0;
	dect_iwu_handle(iwu, in, n, &out);
}

static void base_with_two_handsets(struct dect_iwu *iwu)
{
	dect_iwu_init(iwu);
	dect_iwu_set_line(iwu, 1, "Line 1");
	CHECK_EQ(dect_iwu_add_handset(iwu, 2, "DECT 2"), 0);
	CHECK_EQ(dect_iwu_add_handset(iwu, 1, "DECT 1"), 0);	/* sorts first */
	dect_iwu_set_own(iwu, 1);
}

/* The A540's first move: start a session on the List of Supported Lists. */
static void test_start_session_on_supported_lists(void)
{
	static const uint8_t start[] = { 0xC3, 0x00, 0x00, 0x00 };
	/* table 43: list 00, session 1, one entry, no EMC, reason unevaluated,
	 * no sorting fields for this one-entry list */
	static const uint8_t want[] = { 0xC3, 0x01, 0x00, 0x81, 0x81,
					0x00, 0x00, 0x00, 0xFF, 0x00 };
	struct dect_iwu iwu;

	base_with_two_handsets(&iwu);
	handle(&iwu, start, sizeof(start));
	CHECK_EQ(out.count, 1u);
	CHECK_EQ(out.len[0], sizeof(want));
	CHECK_EQ(memcmp(out.msg[0], want, sizeof(want)), 0);
	CHECK_EQ(iwu.stats.opened, 1u);
}

/* Internal Names: two entries, sorted by Number (7.4.10.3) and said so. */
static void test_start_session_on_internal_names_reports_count_and_sort(void)
{
	static const uint8_t start[] = { 0xC3, 0x00, 0x06, 0x00 };
	static const uint8_t want[] = { 0xC3, 0x01, 0x06, 0x81, 0x82,
					0x00, 0x00, 0x00, 0xFF, 0x01, 0x01 };
	struct dect_iwu iwu;

	base_with_two_handsets(&iwu);
	handle(&iwu, start, sizeof(start));
	CHECK_EQ(out.count, 1u);
	CHECK_EQ(out.len[0], sizeof(want));
	CHECK_EQ(memcmp(out.msg[0], want, sizeof(want)), 0);
}

/* A list we do not serve: session 0 and reason 'list not supported'. */
static void test_unsupported_list_is_rejected(void)
{
	static const uint8_t start[] = { 0xC3, 0x00, 0x05, 0x00 };	/* contacts */
	static const uint8_t want[] = { 0xC3, 0x01, 0x05, 0x80, 0x80,
					0x00, 0x00, 0x00, 0x02, 0x00 };
	struct dect_iwu iwu;

	base_with_two_handsets(&iwu);
	handle(&iwu, start, sizeof(start));
	CHECK_EQ(out.count, 1u);
	CHECK_EQ(out.len[0], sizeof(want));
	CHECK_EQ(memcmp(out.msg[0], want, sizeof(want)), 0);
	CHECK_EQ(iwu.stats.rejected, 1u);
	CHECK_EQ(iwu.stats.opened, 0u);
}

/*
 * The read the A540 does on list 00: entry 1, one entry forward, field 01.
 * Expect the confirm AND a data packet last carrying the entry -- the data
 * packet is what the first responder never sent.
 */
static void test_read_supported_lists_delivers_the_entry(void)
{
	static const uint8_t start[] = { 0xC3, 0x00, 0x00, 0x00 };
	static const uint8_t read[] = { 0xC3, 0x06, 0x81, 0x81, 0x01, 0x00, 0x01 };
	/* table 49: session 1, start index 1, one entry delivered */
	static const uint8_t cfm[] = { 0xC3, 0x07, 0x81, 0x81, 0x01 };
	/* table 62: entry id 1, entry length 7, field 01 length 5,
	 * properties, lists 00 06 07 08 */
	static const uint8_t data[] = { 0xC3, 0x14, 0x81,
					0x81, 0x87,
					0x01, 0x85, 0x80, 0x00, 0x06, 0x07,
					0x08 };
	struct dect_iwu iwu;

	base_with_two_handsets(&iwu);
	handle(&iwu, start, sizeof(start));
	handle(&iwu, read, sizeof(read));
	CHECK_EQ(out.count, 2u);
	CHECK_EQ(out.len[0], sizeof(cfm));
	CHECK_EQ(memcmp(out.msg[0], cfm, sizeof(cfm)), 0);
	CHECK_EQ(out.len[1], sizeof(data));
	CHECK_EQ(memcmp(out.msg[1], data, sizeof(data)), 0);
	CHECK_EQ(iwu.stats.entries_sent, 1u);
}

/*
 * The A540's read of the Internal Names List: from entry 1, three entries,
 * fields 01 02 03. We hold two; the confirm says two, and the 'own' bit is
 * set on the reading handset's own entry only.
 */
static void test_read_internal_names(void)
{
	static const uint8_t start[] = { 0xC3, 0x00, 0x06, 0x00 };
	static const uint8_t read[] = { 0xC3, 0x06, 0x81, 0x81, 0x03, 0x00,
					0x01, 0x02, 0x03 };
	static const uint8_t cfm[] = { 0xC3, 0x07, 0x81, 0x81, 0x02 };
	static const uint8_t data[] = {
		0xC3, 0x14, 0x81,
		/* entry 1 (17 octets): "1" own+internal, "DECT 1", interception allowed */
		0x81, 0x91,
		  0x01, 0x82, 0xB0, '1',
		  0x02, 0x87, 0x80, 'D', 'E', 'C', 'T', ' ', '1',
		  0x03, 0x82, 0x80, 0x31,
		/* entry 2 (17 octets): "2" internal only */
		0x82, 0x91,
		  0x01, 0x82, 0xA0, '2',
		  0x02, 0x87, 0x80, 'D', 'E', 'C', 'T', ' ', '2',
		  0x03, 0x82, 0x80, 0x31,
	};
	struct dect_iwu iwu;

	base_with_two_handsets(&iwu);
	handle(&iwu, start, sizeof(start));
	handle(&iwu, read, sizeof(read));
	CHECK_EQ(out.count, 2u);
	CHECK_EQ(out.len[0], sizeof(cfm));
	CHECK_EQ(memcmp(out.msg[0], cfm, sizeof(cfm)), 0);
	CHECK_EQ(out.len[1], sizeof(data));
	CHECK_EQ(memcmp(out.msg[1], data, sizeof(data)), 0);
	CHECK_EQ(iwu.stats.entries_sent, 2u);
}

/* Line Settings: line name, line id "relating to", attached-handsets bitmap. */
static void test_read_line_settings(void)
{
	static const uint8_t start[] = { 0xC3, 0x00, 0x08, 0x00 };
	static const uint8_t read[] = { 0xC3, 0x06, 0x81, 0x81, 0x03, 0x00,
					0x01, 0x02, 0x03 };
	static const uint8_t start_want[] = { 0xC3, 0x01, 0x08, 0x81, 0x81,
					      0x00, 0x00, 0x00, 0xFF, 0x01, 0x02 };
	static const uint8_t cfm[] = { 0xC3, 0x07, 0x81, 0x81, 0x01 };
	static const uint8_t data[] = {
		0xC3, 0x14, 0x81,
		0x81, 0x93,				/* 9 + 5 + 5 octets */
		  0x01, 0x87, 0x80, 'L', 'i', 'n', 'e', ' ', '1',
		  0x02, 0x83, 0x80, 0x03, 0x81,
		  0x03, 0x83, 0x80, 0x82, 0x83,	/* 2 attached: terminals 1 and 2 */
	};
	struct dect_iwu iwu;

	base_with_two_handsets(&iwu);
	handle(&iwu, start, sizeof(start));
	CHECK_EQ(out.count, 1u);
	CHECK_EQ(out.len[0], sizeof(start_want));
	CHECK_EQ(memcmp(out.msg[0], start_want, sizeof(start_want)), 0);
	handle(&iwu, read, sizeof(read));
	CHECK_EQ(out.count, 2u);
	CHECK_EQ(out.len[0], sizeof(cfm));
	CHECK_EQ(memcmp(out.msg[0], cfm, sizeof(cfm)), 0);
	CHECK_EQ(out.len[1], sizeof(data));
	CHECK_EQ(memcmp(out.msg[1], data, sizeof(data)), 0);
}

/*
 * Fields 08H (multiple calls mode, single call) and 0BH-0DH (call
 * forwarding, off, no codes, no number), all read-only (7.4.11.4.8-13).
 */
static void test_line_settings_mode_and_forwarding(void)
{
	static const uint8_t start[] = { 0xC3, 0x00, 0x08, 0x00 };
	static const uint8_t read[] = { 0xC3, 0x06, 0x81, 0x81, 0x04, 0x00,
					0x08, 0x0B, 0x0C, 0x0D };
	static const uint8_t data[] = {
		0xC3, 0x14, 0x81,
		0x81, 0x9A,				/* 4 + 7 + 8 + 7 octets */
		  0x08, 0x82, 0x80, 0x30,
		  0x0B, 0x85, 0x80, 0x30, 0x00, 0x00, 0x00,
		  0x0C, 0x86, 0x80, 0x30, 0x00, 0x00, 0x00, 0x00,
		  0x0D, 0x85, 0x80, 0x30, 0x00, 0x00, 0x00,
	};
	struct dect_iwu iwu;

	static const uint8_t q[] = { 0xC3, 0x04, 0x81 };
	static const uint8_t q_want[] = { 0xC3, 0x05, 0x81, 0x00, 0x07, 0x01,
					  0x02, 0x03, 0x08, 0x0B, 0x0C, 0x0D };

	base_with_two_handsets(&iwu);
	handle(&iwu, start, sizeof(start));
	handle(&iwu, read, sizeof(read));
	CHECK_EQ(out.count, 2u);
	CHECK_EQ(out.len[1], sizeof(data));
	CHECK_EQ(memcmp(out.msg[1], data, sizeof(data)), 0);
	handle(&iwu, q, sizeof(q));			/* none editable */
	CHECK_EQ(out.len[0], sizeof(q_want));
	CHECK_EQ(memcmp(out.msg[0], q_want, sizeof(q_want)), 0);
}

/*
 * The DECT System Settings List (7.4.11.3): one entry; both PINs read as
 * FF FF FF FF, the new one PIN protected; clock master FP, base reset yes,
 * the firmware version, an empty EEPROM version. Nothing editable.
 */
static void test_read_system_settings(void)
{
	static const uint8_t start[] = { 0xC3, 0x00, 0x07, 0x00 };
	static const uint8_t read[] = { 0xC3, 0x06, 0x81, 0x81, 0x01, 0x00,
					0x01, 0x02, 0x03, 0x09, 0x0A, 0x0D };
	static const uint8_t data[] = {
		0xC3, 0x14, 0x81,
		0x81, 0xA8,				/* 7+4+4+15+3+7 octets */
		  0x01, 0x85, 0x80, 0xFF, 0xFF, 0xFF, 0xFF,
		  0x02, 0x82, 0x80, 0x30,
		  0x03, 0x82, 0x80, 0x31,
		  0x09, 0x8D, 0x80, 'o', 'p', 'e', 'n', 'D', 'E', 'C', 'T', 'c', 'e', 'l', 'l',
		  0x0A, 0x81, 0x80,
		  0x0D, 0x85, 0x81, 0xFF, 0xFF, 0xFF, 0xFF,
	};
	static const uint8_t edit[] = { 0xC3, 0x08, 0x81, 0x81, 0x02 };
	struct dect_iwu iwu;

	base_with_two_handsets(&iwu);
	handle(&iwu, start, sizeof(start));
	CHECK_EQ(out.msg[0][1], DECT_LIA_START_SESSION_CFM);
	handle(&iwu, read, sizeof(read));
	CHECK_EQ(out.count, 2u);
	CHECK_EQ(out.len[1], sizeof(data));
	CHECK_EQ(memcmp(out.msg[1], data, sizeof(data)), 0);
	handle(&iwu, edit, sizeof(edit));
	CHECK_EQ(out.msg[0][1], DECT_LIA_NACK);
	CHECK_EQ(out.msg[0][3], DECT_LIA_NACK_NOT_ALLOWED);
}

/* The bitmap only grows with the highest terminal: terminals 1 and 8. */
static void test_attached_handsets_bitmap_spans_two_octets(void)
{
	static const uint8_t start[] = { 0xC3, 0x00, 0x08, 0x00 };
	static const uint8_t read[] = { 0xC3, 0x06, 0x81, 0x81, 0x01, 0x00, 0x03 };
	static const uint8_t data[] = {
		0xC3, 0x14, 0x81,
		0x81, 0x86,
		  0x03, 0x84, 0x80, 0x82, 0x01, 0x81,	/* count 2; bit 1; bit 8 */
	};
	struct dect_iwu iwu;

	dect_iwu_init(&iwu);
	dect_iwu_add_handset(&iwu, 8, "DECT 8");
	dect_iwu_add_handset(&iwu, 1, "DECT 1");
	handle(&iwu, start, sizeof(start));
	handle(&iwu, read, sizeof(read));
	CHECK_EQ(out.count, 2u);
	CHECK_EQ(out.len[1], sizeof(data));
	CHECK_EQ(memcmp(out.msg[1], data, sizeof(data)), 0);
}

/*
 * The line pool: two lines, each attached to one handset, served sorted by
 * line id with their own attached-handsets bitmap (TS 102 527-3 7.4.11.4).
 */
static void test_line_pool(void)
{
	static const uint8_t start[] = { 0xC3, 0x00, 0x08, 0x00 };
	static const uint8_t read[] = { 0xC3, 0x06, 0x81, 0x81, 0x02, 0x00,
					0x02, 0x03 };
	static const uint8_t data[] = {
		0xC3, 0x14, 0x81,
		0x81, 0x8a,				/* line 1: terminal 1 */
		  0x02, 0x83, 0x80, 0x03, 0x81,
		  0x03, 0x83, 0x80, 0x81, 0x81,
		0x82, 0x8a,				/* line 2: terminal 2 */
		  0x02, 0x83, 0x80, 0x03, 0x82,
		  0x03, 0x83, 0x80, 0x81, 0x82,
	};
	struct dect_iwu iwu;

	base_with_two_handsets(&iwu);
	dect_iwu_clear_lines(&iwu);
	CHECK_EQ(dect_iwu_add_line(&iwu, 2, "Line B", 1u << 1), 0);
	CHECK_EQ(dect_iwu_add_line(&iwu, 1, "Line A", 1u << 0), 0);
	CHECK_EQ(dect_iwu_add_line(&iwu, 2, "dup", 0), -1);	/* unique ids */
	CHECK_EQ(dect_iwu_add_line(&iwu, 127, "big", 0), -1);
	CHECK_EQ(iwu.n_lines, 2u);
	CHECK(dect_iwu_line(&iwu, 1) == &iwu.lines[0]);	/* sorted */
	CHECK_EQ(strcmp(dect_iwu_line(&iwu, 2)->name, "Line B"), 0);
	CHECK(dect_iwu_line(&iwu, 3) == NULL);
	handle(&iwu, start, sizeof(start));
	handle(&iwu, read, sizeof(read));
	CHECK_EQ(out.count, 2u);
	CHECK_EQ(out.len[1], sizeof(data));
	CHECK_EQ(memcmp(out.msg[1], data, sizeof(data)), 0);
}

/* Two sessions at once, each with its own identifier; ending one frees it. */
static void test_two_sessions_and_end_session(void)
{
	static const uint8_t s0[] = { 0xC3, 0x00, 0x00, 0x00 };
	static const uint8_t s6[] = { 0xC3, 0x00, 0x06, 0x00 };
	static const uint8_t s8[] = { 0xC3, 0x00, 0x08, 0x00 };
	static const uint8_t end1[] = { 0xC3, 0x02, 0x81 };
	static const uint8_t end1_want[] = { 0xC3, 0x03, 0x81 };
	static const uint8_t end_bogus[] = { 0xC3, 0x02, 0x85 };
	struct dect_iwu iwu;

	base_with_two_handsets(&iwu);
	handle(&iwu, s0, sizeof(s0));
	CHECK_EQ(out.msg[0][3], 0x81u);
	handle(&iwu, s6, sizeof(s6));
	CHECK_EQ(out.msg[0][3], 0x82u);
	/* a third: "maximum number of sessions supported by the FP reached" */
	handle(&iwu, s8, sizeof(s8));
	CHECK_EQ(out.msg[0][3], 0x80u);
	CHECK_EQ(out.msg[0][8], DECT_LIA_REJ_MAX_SESSIONS);
	/* end 1, and the slot comes back */
	handle(&iwu, end1, sizeof(end1));
	CHECK_EQ(out.count, 1u);
	CHECK_EQ(out.len[0], sizeof(end1_want));
	CHECK_EQ(memcmp(out.msg[0], end1_want, sizeof(end1_want)), 0);
	CHECK_EQ(iwu.stats.closed, 1u);
	handle(&iwu, s8, sizeof(s8));
	CHECK_EQ(out.msg[0][3], 0x81u);
	/* a wrong session identifier "should be ignored" */
	handle(&iwu, end_bogus, sizeof(end_bogus));
	CHECK_EQ(out.count, 0u);
	/* re-opening an open list reuses its session instead of a new slot */
	handle(&iwu, s6, sizeof(s6));
	CHECK_EQ(out.msg[0][3], 0x82u);
}

/* Reads on a dead session, past the end, with no fields, and on an empty list. */
static void test_read_error_cases(void)
{
	static const uint8_t s6[] = { 0xC3, 0x00, 0x06, 0x00 };
	static const uint8_t read_bad_session[] = { 0xC3, 0x06, 0x83, 0x81, 0x01, 0x00, 0x01 };
	static const uint8_t read_past_end[] = { 0xC3, 0x06, 0x81, 0x85, 0x01, 0x00, 0x01 };
	static const uint8_t read_mark_only[] = { 0xC3, 0x06, 0x81, 0x81, 0x02, 0x7F };
	static const uint8_t read_last_backward[] = { 0xC3, 0x06, 0x81, 0x80, 0x82, 0x00, 0x01 };
	struct dect_iwu iwu;

	base_with_two_handsets(&iwu);
	handle(&iwu, s6, sizeof(s6));

	handle(&iwu, read_bad_session, sizeof(read_bad_session));
	CHECK_EQ(out.count, 1u);
	CHECK_EQ(out.msg[0][1], DECT_LIA_NACK);
	CHECK_EQ(out.msg[0][2], 0x83u);
	CHECK_EQ(out.msg[0][3], DECT_LIA_NACK_INVALID_SESSION);

	handle(&iwu, read_past_end, sizeof(read_past_end));
	CHECK_EQ(out.count, 1u);
	CHECK_EQ(out.msg[0][1], DECT_LIA_NACK);
	CHECK_EQ(out.msg[0][3], DECT_LIA_NACK_INVALID_START);

	/* no fields requested: a confirm with counter 0 and no data packet */
	handle(&iwu, read_mark_only, sizeof(read_mark_only));
	CHECK_EQ(out.count, 1u);
	CHECK_EQ(out.msg[0][1], DECT_LIA_READ_ENTRIES_CFM);
	CHECK_EQ(out.msg[0][4], 0x00u);

	/* start index 0 = last entry; two backwards from it = both entries */
	handle(&iwu, read_last_backward, sizeof(read_last_backward));
	CHECK_EQ(out.count, 2u);
	CHECK_EQ(out.msg[0][3], 0x81u);		/* smallest index first */
	CHECK_EQ(out.msg[0][4], 0x02u);

	/* a base with no handsets: Internal Names is empty, 'entry not available' */
	dect_iwu_init(&iwu);
	handle(&iwu, s6, sizeof(s6));
	CHECK_EQ(out.msg[0][4], 0x80u);		/* zero entries announced */
	handle(&iwu, read_mark_only, sizeof(read_mark_only));
	CHECK_EQ(out.count, 1u);
	CHECK_EQ(out.msg[0][1], DECT_LIA_NACK);
	CHECK_EQ(out.msg[0][3], DECT_LIA_NACK_NO_ENTRY);
}

/* Query supported entry fields: none editable, three known (table 47). */
static void test_query_supported_fields(void)
{
	static const uint8_t s6[] = { 0xC3, 0x00, 0x06, 0x00 };
	static const uint8_t q[] = { 0xC3, 0x04, 0x81 };
	static const uint8_t want[] = { 0xC3, 0x05, 0x81, 0x00, 0x03, 0x01, 0x02, 0x03 };
	struct dect_iwu iwu;

	base_with_two_handsets(&iwu);
	handle(&iwu, s6, sizeof(s6));
	handle(&iwu, q, sizeof(q));
	CHECK_EQ(out.count, 1u);
	CHECK_EQ(out.len[0], sizeof(want));
	CHECK_EQ(memcmp(out.msg[0], want, sizeof(want)), 0);
}

/* What we do not do, we refuse with the reason the standard names. */
static void test_unsupported_commands_are_refused(void)
{
	static const uint8_t s6[] = { 0xC3, 0x00, 0x06, 0x00 };
	static const uint8_t search[] = { 0xC3, 0x10, 0x81, 0x00, 0x05, 0x31 };
	static const uint8_t edit[] = { 0xC3, 0x08, 0x81, 0x81, 0x02 };
	static const uint8_t save[] = { 0xC3, 0x0A, 0x81, 0x81 };
	static const uint8_t data_last[] = { 0xC3, 0x14, 0x81, 0x81, 0x80 };
	struct dect_iwu iwu;

	base_with_two_handsets(&iwu);
	handle(&iwu, s6, sizeof(s6));
	handle(&iwu, search, sizeof(search));
	CHECK_EQ(out.count, 1u);
	CHECK_EQ(out.msg[0][1], DECT_LIA_NACK);
	CHECK_EQ(out.msg[0][3], DECT_LIA_NACK_NOT_SUPPORTED);
	handle(&iwu, edit, sizeof(edit));
	CHECK_EQ(out.msg[0][3], DECT_LIA_NACK_NOT_ALLOWED);
	/* save: the acknowledgement waits for the data packet last */
	handle(&iwu, save, sizeof(save));
	CHECK_EQ(out.count, 0u);
	handle(&iwu, data_last, sizeof(data_last));
	CHECK_EQ(out.count, 1u);
	CHECK_EQ(out.msg[0][1], DECT_LIA_NACK);
	CHECK_EQ(out.msg[0][3], DECT_LIA_NACK_NOT_ALLOWED);
}

/*
 * Table 41 note 1: a search of the Supported Lists or the Line Settings
 * List, and a field query of the Supported Lists, are 'procedure not
 * allowed'; a search of the Internal Names List is still 'not supported'.
 */
static void test_table_41_refusals(void)
{
	static const uint8_t s0[] = { 0xC3, 0x00, 0x00, 0x00 };
	static const uint8_t s8[] = { 0xC3, 0x00, 0x08, 0x00 };
	static const uint8_t s6[] = { 0xC3, 0x00, 0x06, 0x00 };
	static const uint8_t q[] = { 0xC3, 0x04, 0x81 };
	uint8_t search[] = { 0xC3, 0x10, 0x81, 0x00, 0x05, 0x31 };
	struct dect_iwu iwu;

	base_with_two_handsets(&iwu);
	handle(&iwu, s0, sizeof(s0));		/* session 1: supported lists */
	handle(&iwu, q, sizeof(q));
	CHECK_EQ(out.msg[0][1], DECT_LIA_NACK);
	CHECK_EQ(out.msg[0][3], DECT_LIA_NACK_NOT_ALLOWED);
	handle(&iwu, search, sizeof(search));
	CHECK_EQ(out.msg[0][3], DECT_LIA_NACK_NOT_ALLOWED);

	base_with_two_handsets(&iwu);
	handle(&iwu, s8, sizeof(s8));		/* session 1: line settings */
	handle(&iwu, search, sizeof(search));
	CHECK_EQ(out.msg[0][1], DECT_LIA_NACK);
	CHECK_EQ(out.msg[0][3], DECT_LIA_NACK_NOT_ALLOWED);

	base_with_two_handsets(&iwu);
	handle(&iwu, s6, sizeof(s6));		/* session 1: internal names */
	handle(&iwu, search, sizeof(search));
	CHECK_EQ(out.msg[0][3], DECT_LIA_NACK_NOT_SUPPORTED);
}

/* Not list access, a rejection, our own confirms fed back: silence. */
static void test_other_records_are_ignored_or_flagged(void)
{
	static const uint8_t not_lia[] = { 0xC4, 0x00, 0x05 };	/* PD 04H IA5 */
	static const uint8_t rejection[] = { 0x83, 0x00, 0x00, 0x00 };	/* S/R = 0 */
	static const uint8_t own_cfm[] = { 0xC3, 0x01, 0x00, 0x81, 0x81 };
	static const uint8_t too_short[] = { 0xC3 };
	struct dect_iwu iwu;

	dect_iwu_init(&iwu);
	handle(&iwu, not_lia, sizeof(not_lia));
	CHECK_EQ(out.count, 0u);
	handle(&iwu, rejection, sizeof(rejection));
	CHECK_EQ(out.count, 0u);
	handle(&iwu, own_cfm, sizeof(own_cfm));
	CHECK_EQ(out.count, 0u);
	handle(&iwu, too_short, sizeof(too_short));
	CHECK_EQ(out.count, 0u);
	out.count = 0;
	dect_iwu_handle(&iwu, NULL, 0, &out);
	CHECK_EQ(out.count, 0u);
	CHECK_EQ(iwu.stats.malformed, 3u);
	CHECK_EQ(iwu.stats.ignored, 2u);
}

/* Sessions do not outlive the call. */
static void test_reset_sessions(void)
{
	static const uint8_t s0[] = { 0xC3, 0x00, 0x00, 0x00 };
	static const uint8_t read[] = { 0xC3, 0x06, 0x81, 0x81, 0x01, 0x00, 0x01 };
	struct dect_iwu iwu;

	base_with_two_handsets(&iwu);
	handle(&iwu, s0, sizeof(s0));
	dect_iwu_reset_sessions(&iwu);
	handle(&iwu, read, sizeof(read));
	CHECK_EQ(out.msg[0][1], DECT_LIA_NACK);
	CHECK_EQ(out.msg[0][3], DECT_LIA_NACK_INVALID_SESSION);
}

/* Every message we build fits a LAPC frame, even with eight long names. */
static void test_long_lists_are_segmented_into_packets(void)
{
	static const uint8_t s6[] = { 0xC3, 0x00, 0x06, 0x00 };
	static const uint8_t read[] = { 0xC3, 0x06, 0x81, 0x81, 0x08, 0x00,
					0x01, 0x02, 0x03 };
	struct dect_iwu iwu;
	unsigned i;
	size_t total = 0;

	dect_iwu_init(&iwu);
	for (i = 1; i <= DECT_IWU_HANDSETS; i++)
		CHECK_EQ(dect_iwu_add_handset(&iwu, (uint8_t)i, "Fifteen chars!!"), 0);
	CHECK_EQ(dect_iwu_add_handset(&iwu, 9, "one too many"), -1);
	handle(&iwu, s6, sizeof(s6));
	handle(&iwu, read, sizeof(read));
	CHECK(out.count >= 3u);			/* confirm + at least two packets */
	for (i = 0; i < out.count; i++) {
		CHECK(out.len[i] <= DECT_IWU_MSG_MAX);
		if (i > 0)
			CHECK_EQ(out.msg[i][1], i + 1u == out.count
				 ? DECT_LIA_DATA_LAST : DECT_LIA_DATA);
		if (i > 0)
			total += out.len[i] - 3u;
	}
	/* 8 entries x (2 + (4 + 18 + 4)) octets of content */
	CHECK_EQ(total, 8u * 28u);
	CHECK_EQ(iwu.stats.entries_sent, 8u);
}

int main(void)
{
	test_start_session_on_supported_lists();
	test_start_session_on_internal_names_reports_count_and_sort();
	test_unsupported_list_is_rejected();
	test_read_supported_lists_delivers_the_entry();
	test_read_internal_names();
	test_read_line_settings();
	test_line_settings_mode_and_forwarding();
	test_read_system_settings();
	test_attached_handsets_bitmap_spans_two_octets();
	test_line_pool();
	test_two_sessions_and_end_session();
	test_read_error_cases();
	test_query_supported_fields();
	test_unsupported_commands_are_refused();
	test_table_41_refusals();
	test_other_records_are_ignored_or_flagged();
	test_reset_sessions();
	test_long_lists_are_segmented_into_packets();
	return CHECK_DONE();
}

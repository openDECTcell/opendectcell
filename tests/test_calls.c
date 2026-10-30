// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for src/libopendectcell/dect_calls.c (the call store) and the call lists
 * in dect_iwu.c: TS 102 527-3 V1.7.1 clauses 7.4.10.4 and 7.4.10.5.3-5.
 * Request bytes are built from the standard's tables; numbers are made up.
 */
#include "check.h"
#include "../src/libopendectcell/dect_calls.h"
#include "../src/libopendectcell/dect_iwu.h"

#include <string.h>

static struct dect_calls store;
static struct dect_iwu_out out;
static const struct dect_time t1 = { 26, 10, 30, 9, 15, 0, 4, 1 };
static const struct dect_time t2 = { 26, 10, 30, 9, 20, 0, 4, 1 };

static void handle(struct dect_iwu *iwu, const uint8_t *in, size_t n)
{
	out.count = 0;
	dect_iwu_handle(iwu, in, n, &out);
}

/* Terminal 1 on its line 1, terminal 2 on its line 2: one SIP account. */
static const uint8_t both[DECT_CALLS_TERMINALS] = { 0, 1, 2 };
static const uint8_t only1[DECT_CALLS_TERMINALS] = { 0, 1, 0 };

static void test_store(void)
{
	unsigned a, b;

	dect_calls_init(&store);
	a = dect_calls_add(&store, DECT_CALLS_MISSED, "+49 30 1234", "Anna", &t1, both);
	CHECK(a != 0u);
	CHECK_EQ(strcmp(dect_calls_at(&store, 1, 1, 1)->number, "0049301234"), 0);
	/* the same party again: merged, counted, unread, the later time */
	b = dect_calls_add(&store, DECT_CALLS_MISSED, "0049301234", "", &t2, both);
	CHECK_EQ(b, a);
	CHECK_EQ(dect_calls_count(&store, 1, 1), 1u);
	CHECK_EQ(dect_calls_at(&store, 1, 2, 1)->count, 2u);
	CHECK_EQ(dect_calls_at(&store, 1, 2, 1)->when.minute, 20u);
	CHECK_EQ(strcmp(dect_calls_at(&store, 1, 2, 1)->name, "Anna"), 0);
	/* another party, terminal 1 only: newest first, per terminal */
	b = dect_calls_add(&store, DECT_CALLS_MISSED, "0555", "Bob", &t2, only1);
	CHECK(b != a);
	CHECK_EQ(dect_calls_count(&store, 1, 1), 2u);
	CHECK_EQ(dect_calls_count(&store, 1, 2), 1u);
	CHECK_EQ(dect_calls_at(&store, 1, 1, 1)->id, b);
	CHECK_EQ(dect_calls_index(&store, 1, 2, a), 1u);
	CHECK_EQ(dect_calls_missed_unread(&store, 1, 1), 2u);
	CHECK_EQ(dect_calls_missed_unread(&store, 2, 2), 1u);
	CHECK_EQ(dect_calls_missed_unread(&store, 2, 1), 0u);
	/* accepted calls are never unread */
	(void)dect_calls_add(&store, DECT_CALLS_ACCEPTED, "0555", "", &t1, only1);
	CHECK_EQ(dect_calls_at(&store, 3, 1, 1)->unread, 0u);
	/* the list keeps DECT_CALLS_MAX, the oldest going */
	dect_calls_init(&store);
	for (a = 0; a < DECT_CALLS_MAX + 3u; a++) {
		char num[8] = { (char)('0' + a % 10u), (char)('0' + a / 10u), 0 };

		(void)dect_calls_add(&store, DECT_CALLS_OUTGOING, num, "", &t1, both);
	}
	CHECK_EQ(dect_calls_count(&store, 2, 1), DECT_CALLS_MAX);
	CHECK_EQ(strcmp(dect_calls_at(&store, 2, 1, 1)->number, "24"), 0);
	/* a name cut on a character boundary */
	dect_calls_add(&store, DECT_CALLS_ACCEPTED, "1",
		       "ÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄ", &t1, both);
	CHECK_EQ(strlen(dect_calls_at(&store, 3, 1, 1)->name), 32u);
}

static void session(struct dect_iwu *iwu, uint8_t list)
{
	uint8_t start[] = { 0xC3, DECT_LIA_START_SESSION, 0x00, 0x00 };

	start[2] = list;
	handle(iwu, start, sizeof(start));
	CHECK_EQ(out.msg[0][1], DECT_LIA_START_SESSION_CFM);
}

static void test_lists_and_fields(void)
{
	static const uint8_t read_lists[] = { 0xC3, DECT_LIA_READ_ENTRIES, 0x81,
					      0x81, 0x01, 0x00, 0x01 };
	static const uint8_t query[] = { 0xC3, DECT_LIA_QUERY_FIELDS, 0x81 };
	/* table 47: Read status editable, the six others not */
	static const uint8_t want_query[] = { 0xC3, 0x05, 0x81, 0x01, 0x04, 0x06,
					      0x01, 0x02, 0x03, 0x05, 0x06, 0x07 };
	/* missed: one entry, sorted by Date and time */
	static const uint8_t want_start[] = { 0xC3, 0x01, 0x01, 0x81, 0x81, 0x00,
					      0x00, 0x00, 0xFF, 0x01, 0x03 };
	struct dect_iwu iwu;

	dect_calls_init(&store);
	(void)dect_calls_add(&store, DECT_CALLS_MISSED, "0301234", "Anna", &t1, both);
	dect_iwu_init(&iwu);
	dect_iwu_set_own(&iwu, 2);
	iwu.calls = &store;

	session(&iwu, 0x00);
	handle(&iwu, read_lists, sizeof(read_lists));
	/* data packet: entry 1, list identifiers 00 01 02 03 06 07 08 */
	CHECK_EQ(out.msg[1][1], DECT_LIA_DATA_LAST);
	CHECK_EQ(memcmp(&out.msg[1][8], "\x00\x01\x02\x03\x06\x07\x08", 7), 0);

	dect_iwu_reset_sessions(&iwu);
	session(&iwu, 0x01);
	CHECK_EQ(out.len[0], sizeof(want_start));
	CHECK_EQ(memcmp(out.msg[0], want_start, sizeof(want_start)), 0);
	handle(&iwu, query, sizeof(query));
	CHECK_EQ(out.len[0], sizeof(want_query));
	CHECK_EQ(memcmp(out.msg[0], want_query, sizeof(want_query)), 0);
}

static void test_read_and_mark(void)
{
	/* start 1, one entry, mark read (7FH), every field */
	static const uint8_t read[] = { 0xC3, DECT_LIA_READ_ENTRIES, 0x81, 0x81,
					0x01, 0x7F, 0x01, 0x02, 0x03, 0x04,
					0x05, 0x06, 0x07 };
	struct dect_iwu iwu;
	uint8_t data[96];
	size_t n = 0, i;
	unsigned id;

	dect_calls_init(&store);
	id = dect_calls_add(&store, DECT_CALLS_MISSED, "0301234", "Anna", &t1, both);
	dect_iwu_init(&iwu);
	dect_iwu_set_line(&iwu, 2, "Home");
	dect_iwu_set_own(&iwu, 2);
	iwu.calls = &store;
	session(&iwu, 0x01);
	handle(&iwu, read, sizeof(read));
	CHECK_EQ(out.msg[0][1], DECT_LIA_READ_ENTRIES_CFM);
	CHECK_EQ(out.msg[0][4], 0x01);
	for (i = 1; i < out.count; i++) {
		memcpy(data + n, out.msg[i] + 3, out.len[i] - 3u);
		n += out.len[i] - 3u;
	}
	/* the entry: its id, then the fields in order */
	CHECK_EQ(data[0], (uint8_t)(0x80u | id));
	i = 2;
	CHECK_EQ(data[i], 0x01);			/* Number */
	CHECK_EQ(data[i + 1], 0x88);
	CHECK_EQ(memcmp(&data[i + 3], "0301234", 7), 0);
	i += 10;
	CHECK_EQ(data[i], 0x02);			/* Name */
	i += 3 + 4;
	CHECK_EQ(data[i], 0x03);			/* Date and time */
	CHECK_EQ(data[i + 3], 0xC0);		/* time and date, current */
	i += 2 + (data[i + 1] & 0x7Fu);
	CHECK_EQ(data[i], 0x04);			/* Read status: unread when read */
	CHECK_EQ(data[i + 1], 0x81);
	CHECK_EQ(data[i + 2], 0xE0);
	i += 3;
	CHECK_EQ(data[i], 0x05);			/* Line name: this handset's */
	CHECK_EQ(memcmp(&data[i + 3], "Home", 4), 0);
	i += 3 + 4;
	CHECK_EQ(data[i], 0x06);			/* Line id: external, line 2 */
	CHECK_EQ(data[i + 3], 0x00);
	CHECK_EQ(data[i + 4], 0x82);
	i += 5;
	CHECK_EQ(data[i], 0x07);			/* Number of calls */
	CHECK_EQ(data[i + 3], 0x01);
	/* ... and the mark: read now, for every handset */
	CHECK_EQ(dect_calls_at(&store, 1, 1, 1)->unread, 0u);
	CHECK_EQ(store.missed_changed, 1);
	CHECK_EQ(dect_calls_missed_unread(&store, 1, 1), 0u);
}

static void test_edit_save_delete(void)
{
	struct dect_iwu iwu;
	uint8_t edit[] = { 0xC3, DECT_LIA_EDIT_ENTRY, 0x81, 0x80, 0x04 };
	uint8_t save[] = { 0xC3, DECT_LIA_SAVE_ENTRY, 0x81, 0x80 };
	uint8_t data[] = { 0xC3, DECT_LIA_DATA_LAST, 0x81, 0x80, 0x83, 0x04,
			   0x81, 0xE0 };
	uint8_t del[] = { 0xC3, DECT_LIA_DELETE_ENTRY, 0x81, 0x80 };
	static const uint8_t clear[] = { 0xC3, DECT_LIA_DELETE_LIST, 0x81 };
	static const uint8_t search[] = { 0xC3, DECT_LIA_SEARCH_ENTRIES, 0x81,
					  0x01, 0x01, 'a', 0x01, 0x00, 0x01 };
	unsigned id;

	dect_calls_init(&store);
	id = dect_calls_add(&store, DECT_CALLS_MISSED, "0301234", "", &t1, both);
	dect_calls_at(&store, 1, 1, 1)->unread = 0;
	dect_iwu_init(&iwu);
	dect_iwu_set_own(&iwu, 1);
	iwu.calls = &store;
	session(&iwu, 0x01);

	/* mark it unread again through edit and save of 'Read status' */
	edit[3] = save[3] = data[3] = del[3] = (uint8_t)(0x80u | id);
	handle(&iwu, edit, sizeof(edit));
	CHECK_EQ(out.msg[0][1], DECT_LIA_EDIT_ENTRY_CFM);
	handle(&iwu, save, sizeof(save));
	handle(&iwu, data, sizeof(data));
	CHECK_EQ(out.msg[0][1], DECT_LIA_SAVE_ENTRY_CFM);
	CHECK_EQ(dect_calls_at(&store, 1, 1, 1)->unread, 1u);

	/* no match in a search, no error either */
	handle(&iwu, search, sizeof(search));
	CHECK_EQ(out.msg[0][1], DECT_LIA_SEARCH_ENTRIES_CFM);
	CHECK_EQ(out.msg[0][4], 0x00);

	handle(&iwu, del, sizeof(del));
	CHECK_EQ(out.msg[0][1], DECT_LIA_DELETE_ENTRY_CFM);
	CHECK_EQ(out.msg[0][3], 0x80);			/* none left */
	CHECK_EQ(dect_calls_count(&store, 1, 2), 0u);	/* for terminal 2 too */

	/* the outgoing list: no edit; delete list clears this handset's */
	(void)dect_calls_add(&store, DECT_CALLS_OUTGOING, "0555", "", &t1, both);
	(void)dect_calls_add(&store, DECT_CALLS_OUTGOING, "0666", "", &t1,
			     (const uint8_t[DECT_CALLS_TERMINALS]){ 0, 0, 1 });
	dect_iwu_reset_sessions(&iwu);
	session(&iwu, 0x02);
	edit[3] = (uint8_t)(0x80u | dect_calls_at(&store, 2, 1, 1)->id);
	handle(&iwu, edit, sizeof(edit));
	CHECK_EQ(out.msg[0][3], DECT_LIA_NACK_NOT_ALLOWED);
	handle(&iwu, clear, sizeof(clear));
	CHECK_EQ(out.msg[0][1], DECT_LIA_DELETE_LIST_CFM);
	CHECK_EQ(dect_calls_count(&store, 2, 1), 0u);
	CHECK_EQ(dect_calls_count(&store, 2, 2), 1u);	/* terminal 2's own */
}

static void test_not_offered_without_store(void)
{
	static const uint8_t start[] = { 0xC3, DECT_LIA_START_SESSION, 0x01, 0x00 };
	struct dect_iwu iwu;

	dect_iwu_init(&iwu);
	handle(&iwu, start, sizeof(start));
	CHECK_EQ(out.msg[0][8], DECT_LIA_REJ_LIST_UNSUPPORTED);
}

static void test_save_and_load(void)
{
	static struct dect_calls back;
	char text[8192], again[8192];
	size_t n, m;
	struct dect_time t3 = t1;

	t3.tz_valid = 0;
	dect_calls_init(&store);
	(void)dect_calls_add(&store, DECT_CALLS_MISSED, "0301234", "Anna Ärger", &t1, both);
	(void)dect_calls_add(&store, DECT_CALLS_MISSED, "0301234", "", &t2, both);
	(void)dect_calls_add(&store, DECT_CALLS_OUTGOING, "0555", "", &t3, only1);
	(void)dect_calls_add(&store, DECT_CALLS_ACCEPTED, "", "", &t2, only1);
	n = dect_calls_save(&store, text, sizeof(text));
	CHECK(n > 17u);
	CHECK_EQ(dect_calls_save(&store, again, 20), 0u);	/* does not fit */
	CHECK_EQ(dect_calls_load(&back, "#other\n", 7), -1);
	CHECK_EQ(dect_calls_load(&back, text, n), 0);
	CHECK_EQ(dect_calls_count(&back, 1, 1), 1u);
	CHECK_EQ(dect_calls_at(&back, 1, 2, 1)->count, 2u);
	CHECK_EQ(dect_calls_at(&back, 1, 2, 1)->unread, 1u);
	CHECK_EQ(strcmp(dect_calls_at(&back, 1, 2, 1)->name, "Anna Ärger"), 0);
	CHECK_EQ(dect_calls_at(&back, 1, 2, 1)->when.minute, 20u);
	CHECK_EQ(dect_calls_at(&back, 1, 2, 1)->when.tz_quarters, 4);
	CHECK_EQ(dect_calls_at(&back, 2, 1, 1)->when.tz_valid, 0u);
	CHECK_EQ(dect_calls_count(&back, 2, 2), 0u);		/* only1 kept */
	CHECK_EQ(dect_calls_at(&back, 3, 1, 1)->number[0], '\0');
	/* the same text again, and new ids do not collide */
	m = dect_calls_save(&back, again, sizeof(again));
	CHECK_EQ(m, n);
	CHECK_EQ(memcmp(text, again, n), 0);
	CHECK(dect_calls_add(&back, DECT_CALLS_MISSED, "999", "", &t1, both) >
	      dect_calls_at(&back, 3, 1, 1)->id);
	/* a broken line is skipped, the rest is kept */
	{
		static const char bad[] = "#opendectcell-calls 1\nx\ty\n"
			"2\t7\t0\t1\t261030091500\t4\t0001000000000000\t0555\tBob\n";

		CHECK_EQ(dect_calls_load(&back, bad, sizeof(bad) - 1u), 0);
		CHECK_EQ(dect_calls_count(&back, 2, 1), 1u);
		CHECK_EQ(strcmp(dect_calls_at(&back, 2, 1, 1)->name, "Bob"), 0);
	}
}

int main(void)
{
	test_save_and_load();
	test_store();
	test_lists_and_fields();
	test_read_and_mark();
	test_edit_save_delete();
	test_not_offered_without_store();
	return CHECK_DONE();
}

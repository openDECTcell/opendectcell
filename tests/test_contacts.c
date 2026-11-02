// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for src/libopendectcell/dect_contacts.c (the store, its file formats) and
 * the Contact List in dect_iwu.c: TS 102 527-3 V1.7.1 clauses 7.4.10.4 and
 * 7.4.10.5.7. The request bytes are built from the standard's tables; no
 * handset has sent them to us yet. Names and numbers are made up.
 */
#include "check.h"
#include "../src/libopendectcell/dect_contacts.h"
#include "../src/libopendectcell/dect_iwu.h"

#include <stdio.h>
#include <string.h>

static struct dect_contacts store;
static struct dect_iwu_out out;

static const char three[] =
	"#opendectcell-contacts 1 seq=0\n"
	"0000000a\t-\tSmith\tAnna\t0\t*\t04:0171111\t08:030222\n"
	"0000000b\t-\tbordonado\tLuca\t0\t2\t02:089333\n"
	"0000000c\t-\tÄrger\t\t0\t*\n";

static void load(const char *text)
{
	CHECK_EQ(dect_contacts_load(&store, text, strlen(text)), 0);
}

static void handle(struct dect_iwu *iwu, const uint8_t *in, size_t n)
{
	out.count = 0;
	dect_iwu_handle(iwu, in, n, &out);
}

/* The data content of the data packets after the confirm in out.msg[0]. */
static size_t data_of(uint8_t *buf, size_t cap)
{
	size_t n = 0, i;

	for (i = 1; i < out.count; i++) {
		size_t k = out.len[i] - 3u;	/* disc, command, session */

		if (n + k > cap)
			break;
		memcpy(buf + n, out.msg[i] + 3, k);
		n += k;
	}
	return n;
}

static void start_contacts(struct dect_iwu *iwu)
{
	static const uint8_t start[] = { 0xC3, DECT_LIA_START_SESSION, 0x05, 0x00 };

	handle(iwu, start, sizeof(start));
	CHECK_EQ(out.count, 1u);
}

static void test_fold_order(void)
{
	CHECK_EQ(dect_contacts_fold_cmp("Smith", "smith"), 0);
	CHECK_EQ(dect_contacts_fold_cmp("Ärger", "arger"), 0);
	CHECK_EQ(dect_contacts_fold_cmp("Straße", "STRASSE"), 0);
	/* code points above U+00FF are kept as they are; the Latin-1 table is not read past its end (U+0532, U+07FF) */
	CHECK(dect_contacts_fold_cmp("a", "\xD4\xB2") != 0);
	CHECK(dect_contacts_fold_cmp("\xD4\xB2", "\xD4\xB2") == 0);
	CHECK(dect_contacts_fold_cmp("\xDF\xBF", "z") != 0);
	CHECK(dect_contacts_fold_cmp("Alexander", "BORDONADO") < 0);
	CHECK(dect_contacts_fold_cmp("Müller", "Mueller") > 0);	/* u > e */
	CHECK(dect_contacts_fold_cmp("Sm", "Smith") < 0);
	CHECK(dect_contacts_fold_cmp("", "a") < 0);
}

static void test_load_sorts_and_keeps_identifiers(void)
{
	const struct dect_contact *e;
	unsigned id_smith;

	dect_contacts_init(&store);
	CHECK_EQ(dect_contacts_load(&store, "#something else\n", 16), -1);
	load(three);
	CHECK_EQ(store.n, 3u);
	CHECK_EQ(store.allow_clear, 0);
	/* Ärger, bordonado, Smith: without case, Ä as A */
	CHECK_EQ(strcmp(dect_contacts_at(&store, 1)->name, "Ärger"), 0);
	CHECK_EQ(strcmp(dect_contacts_at(&store, 2)->name, "bordonado"), 0);
	e = dect_contacts_at(&store, 3);
	CHECK_EQ(strcmp(e->first, "Anna"), 0);
	CHECK_EQ(e->n_numbers, 2u);
	CHECK_EQ(e->numbers[0].props, DECT_CONTACT_MOBILE);
	CHECK_EQ(strcmp(e->numbers[1].digits, "030222"), 0);
	CHECK_EQ(e->line, DECT_CONTACT_ALL_LINES);
	CHECK_EQ(dect_contacts_at(&store, 2)->line, 2u);
	id_smith = e->id;

	/* the server dropped bordonado and added Adams: Smith keeps its id */
	load("#opendectcell-contacts 1 seq=0 clear=1\n"
	     "0000000a\t-\tSmith\tAnna\t0\t*\t04:0171111\n"
	     "0000000d\t-\tAdams\t\t0\t*\t08:040444\n"
	     "garbage line\n"
	     "0000000c\t-\tÄrger\t\t0\t*\n");
	CHECK_EQ(store.n, 3u);
	CHECK_EQ(store.dropped, 1u);
	CHECK_EQ(store.allow_clear, 1);
	CHECK_EQ(strcmp(dect_contacts_at(&store, 1)->name, "Adams"), 0);
	CHECK_EQ(dect_contacts_at(&store, 3)->id, id_smith);
	CHECK(dect_contacts_at(&store, 1)->id > 3u);	/* never a freed one */
}

static void test_handset_changes_survive_until_synced(void)
{
	struct dect_contact c;
	char line[256];
	unsigned id, smith;

	dect_contacts_init(&store);
	load(three);
	smith = dect_contacts_at(&store, 3)->id;

	/* a handset renames Smith and creates Zed */
	c = *dect_contacts_at(&store, 3);
	strcpy(c.name, "Smyth");
	CHECK_EQ(dect_contacts_put(&store, smith, &c), smith);
	memset(&c, 0, sizeof(c));
	strcpy(c.name, "Zed");
	c.line = DECT_CONTACT_ALL_LINES;
	c.n_numbers = 1;
	c.numbers[0].props = DECT_CONTACT_FIXED;
	strcpy(c.numbers[0].digits, "0555");
	id = dect_contacts_put(&store, 0, &c);
	CHECK(id != 0u);

	CHECK(dect_contacts_take_change(&store, line, sizeof(line)) > 0);
	CHECK_EQ(strcmp(line, "1\tput\t1\t0000000a\tSmyth\tAnna\t0\t*\t04:0171111\t08:030222"), 0);
	CHECK(dect_contacts_take_change(&store, line, sizeof(line)) > 0);
	snprintf((char *)c.name, sizeof(c.name), "2\tput\t%u\t00000000\tZed", id);
	CHECK_EQ(strncmp(line, c.name, strlen(c.name)), 0);
	CHECK_EQ(dect_contacts_take_change(&store, line, sizeof(line)), 0u);

	/* the sync's file from before it saw them: both stay as on the handset */
	load(three);
	CHECK_EQ(store.n, 4u);
	CHECK_EQ(dect_contacts_index(&store, smith) != 0u, 1);
	CHECK_EQ(strcmp(dect_contacts_at(&store, dect_contacts_index(&store, smith))->name,
			"Smyth"), 0);
	CHECK(dect_contacts_index(&store, id) != 0u);

	/* the file after it applied them: Zed has a key now, same id */
	snprintf(line, sizeof(line),
		 "#opendectcell-contacts 1 seq=2\n"
		 "0000000a\t-\tSmyth\tAnna\t0\t*\t04:0171111\t08:030222\n"
		 "0000000b\t-\tbordonado\tLuca\t0\t2\t02:089333\n"
		 "0000000c\t-\tÄrger\t\t0\t*\n"
		 "0000000e\t%u\tZed\t\t0\t*\t08:0555\n", id);
	load(line);
	CHECK_EQ(store.n, 4u);
	CHECK_EQ(dect_contacts_at(&store, dect_contacts_index(&store, id))->key, 0xEu);
	CHECK_EQ(dect_contacts_at(&store, dect_contacts_index(&store, id))->dirty, 0u);

	/* and the server's later edit wins once the handset's is synced */
	load("#opendectcell-contacts 1 seq=2\n"
	     "0000000a\t-\tSmith-Jones\tAnna\t0\t*\n");
	CHECK_EQ(store.n, 1u);
	CHECK_EQ(strcmp(dect_contacts_at(&store, 1)->name, "Smith-Jones"), 0);
	CHECK_EQ(dect_contacts_at(&store, 1)->id, smith);
}

static void test_deletions_until_synced(void)
{
	char line[128];
	unsigned id;

	dect_contacts_init(&store);
	load(three);
	id = dect_contacts_at(&store, 2)->id;		/* bordonado, key b */
	CHECK_EQ(dect_contacts_del(&store, id), 0);
	CHECK_EQ(store.n, 2u);
	CHECK(dect_contacts_take_change(&store, line, sizeof(line)) > 0);
	snprintf(line + 64, 64, "1\tdel\t%u\t0000000b", id);
	CHECK_EQ(strcmp(line, line + 64), 0);
	load(three);					/* not synced yet */
	CHECK_EQ(store.n, 2u);
	load("#opendectcell-contacts 1 seq=1\n0000000b\t-\tbordonado\tLuca\t0\t2\n");
	CHECK_EQ(store.n, 1u);			/* synced: a re-add counts */
	CHECK_EQ(store.n_tombs, 0u);

	/* 'delete list' empties the store until the sync has applied it */
	load(three);
	CHECK_EQ(dect_contacts_clear(&store), 0);
	CHECK_EQ(store.n, 0u);
	load(three);
	CHECK_EQ(store.n, 0u);
	CHECK(dect_contacts_take_change(&store, line, sizeof(line)) > 0);
	CHECK_EQ(strcmp(line, "2\tclear"), 0);
	load("#opendectcell-contacts 1 seq=2\n");
	CHECK_EQ(store.n, 0u);
	load(three);
	CHECK_EQ(store.n, 3u);
}

static void test_caller_numbers(void)
{
	dect_contacts_init(&store);
	load("#opendectcell-contacts 1 seq=0\n"
	     "00000001\t-\tOffice\t\t0\t*\t08:0301234567\n"
	     "00000002\t-\tShort\t\t0\t*\t08:12\n");
	CHECK(dect_contacts_find_number(&store, "+49 30 1234567") != NULL);
	CHECK(dect_contacts_find_number(&store, "004930 1234567") != NULL);
	CHECK(dect_contacts_find_number(&store, "0301234567") != NULL);
	CHECK(dect_contacts_find_number(&store, "12") != NULL);
	CHECK(dect_contacts_find_number(&store, "312") == NULL);
	CHECK(dect_contacts_find_number(&store, "0301234568") == NULL);
}

static void test_session_and_fields(void)
{
	static const uint8_t query[] = { 0xC3, DECT_LIA_QUERY_FIELDS, 0x81 };
	/* table 43: list 05, session 1, 3 entries, sorted by Name, First name */
	static const uint8_t want_start[] = { 0xC3, 0x01, 0x05, 0x81, 0x83, 0x00,
					      0x00, 0x00, 0xFF, 0x02, 0x01, 0x02 };
	/* table 47: six editable (03H three times: three instances), none else */
	static const uint8_t want_query[] = { 0xC3, 0x05, 0x81, 0x06, 0x01, 0x02,
					      0x03, 0x03, 0x03, 0x05, 0x00 };
	static const uint8_t read_lists[] = { 0xC3, DECT_LIA_START_SESSION, 0x00,
					      0x00 };
	static const uint8_t read_ids[] = { 0xC3, DECT_LIA_READ_ENTRIES, 0x82,
					    0x81, 0x01, 0x00, 0x01 };
	static const uint8_t want_ids[] = { 0x81, 0x88, 0x01, 0x86, 0x80,
					    0x00, 0x05, 0x06, 0x07, 0x08 };
	struct dect_iwu iwu;
	uint8_t data[64];

	dect_contacts_init(&store);
	load(three);
	dect_iwu_init(&iwu);
	iwu.contacts = &store;
	start_contacts(&iwu);
	CHECK_EQ(out.len[0], sizeof(want_start));
	CHECK_EQ(memcmp(out.msg[0], want_start, sizeof(want_start)), 0);
	handle(&iwu, query, sizeof(query));
	CHECK_EQ(out.len[0], sizeof(want_query));
	CHECK_EQ(memcmp(out.msg[0], want_query, sizeof(want_query)), 0);

	/* the List of Supported Lists names the Contact List now */
	handle(&iwu, read_lists, sizeof(read_lists));
	handle(&iwu, read_ids, sizeof(read_ids));
	CHECK_EQ(data_of(data, sizeof(data)), sizeof(want_ids));
	CHECK_EQ(memcmp(data, want_ids, sizeof(want_ids)), 0);
}

static void test_read_contacts(void)
{
	/* session 1, start 3 (Smith), one entry, every field and 03H thrice */
	static const uint8_t read[] = { 0xC3, DECT_LIA_READ_ENTRIES, 0x81, 0x83,
					0x01, 0x00, 0x01, 0x02, 0x03, 0x03,
					0x03, 0x05 };
	struct dect_iwu iwu;
	uint8_t data[128], want[128];
	size_t n = 0;
	unsigned id;

	dect_contacts_init(&store);
	load(three);
	id = dect_contacts_at(&store, 3)->id;
	dect_iwu_init(&iwu);
	iwu.contacts = &store;
	start_contacts(&iwu);
	handle(&iwu, read, sizeof(read));
	CHECK_EQ(out.msg[0][1], DECT_LIA_READ_ENTRIES_CFM);
	CHECK_EQ(out.msg[0][3], 0x83);			/* start index 3 */
	CHECK_EQ(out.msg[0][4], 0x01);			/* one, full delivery */

	want[n++] = (uint8_t)(0x80u | id);
	want[n++] = 0;					/* length, below */
	want[n++] = 0x01; want[n++] = 0x86; want[n++] = 0xC0;
	memcpy(want + n, "Smith", 5); n += 5;
	want[n++] = 0x02; want[n++] = 0x85; want[n++] = 0xC0;
	memcpy(want + n, "Anna", 4); n += 4;
	/* two numbers held, so two instances: mobile, then fixed */
	want[n++] = 0x03; want[n++] = 0x88; want[n++] = 0xC4;
	memcpy(want + n, "0171111", 7); n += 7;
	want[n++] = 0x03; want[n++] = 0x87; want[n++] = 0xC8;
	memcpy(want + n, "030222", 6); n += 6;
	want[n++] = 0x05; want[n++] = 0x82; want[n++] = 0xC0; want[n++] = 0x04;
	want[1] = (uint8_t)(0x80u | (n - 2u));
	CHECK_EQ(data_of(data, sizeof(data)), n);
	CHECK_EQ(memcmp(data, want, n), 0);

	/* Ärger has no number: one empty instance, as 7.4.10.1 wants */
	{
		static const uint8_t read1[] = { 0xC3, DECT_LIA_READ_ENTRIES,
						 0x81, 0x81, 0x01, 0x00, 0x03,
						 0x03 };
		uint8_t empty[] = { 0x03, 0x81, 0xC0 };

		handle(&iwu, read1, sizeof(read1));
		CHECK_EQ(data_of(data, sizeof(data)), 2u + sizeof(empty));
		CHECK_EQ(memcmp(data + 2, empty, sizeof(empty)), 0);
	}
}

static void test_partial_delivery(void)
{
	static const uint8_t read[] = { 0xC3, DECT_LIA_READ_ENTRIES, 0x81, 0x81,
					0x14, 0x00, 0x01, 0x02, 0x03, 0x03,
					0x03, 0x05 };
	static const uint8_t back[] = { 0xC3, DECT_LIA_READ_ENTRIES, 0x81, 0x94,
					0x94, 0x00, 0x01 };
	char text[4096];
	size_t n;
	struct dect_iwu iwu;
	unsigned i, got;

	n = (size_t)snprintf(text, sizeof(text), "#opendectcell-contacts 1 seq=0\n");
	for (i = 0; i < 20u; i++)
		n += (size_t)snprintf(text + n, sizeof(text) - n,
				      "%08x\t-\tName%02uxxxxxxxxxxxxxxxxxxxxx\tFirstxxxxxxxxxx"
				      "\t0\t*\t04:01711111111\t08:03022222222\t02:0899999999\n",
				      i + 1u, i);
	dect_contacts_init(&store);
	load(text);
	CHECK_EQ(store.n, 20u);
	dect_iwu_init(&iwu);
	iwu.contacts = &store;
	start_contacts(&iwu);
	handle(&iwu, read, sizeof(read));
	got = out.msg[0][4];
	CHECK(got & 0x80u);			/* partial: memory limit */
	got &= 0x7Fu;
	CHECK(got >= 5u && got < 20u);		/* "at least 5 entries" */
	CHECK_EQ(out.msg[0][3], 0x81);

	/* backward from 20: the delivered part ends at 20 */
	handle(&iwu, back, sizeof(back));
	got = out.msg[0][4] & 0x7Fu;
	CHECK_EQ(out.msg[0][3], (uint8_t)(0x80u | (21u - got)));
}

static void test_edit_save_and_create(void)
{
	struct dect_iwu iwu;
	uint8_t edit[] = { 0xC3, DECT_LIA_EDIT_ENTRY, 0x81, 0x80, 0x01, 0x02,
			   0x03, 0x03, 0x03, 0x05 };
	uint8_t save[] = { 0xC3, DECT_LIA_SAVE_ENTRY, 0x81, 0x80 };
	uint8_t data[64];
	char line[256];
	size_t n = 0;
	unsigned id, len_at;

	dect_contacts_init(&store);
	load(three);
	id = dect_contacts_at(&store, 3)->id;		/* Smith */
	dect_iwu_init(&iwu);
	iwu.contacts = &store;
	start_contacts(&iwu);

	/* save without edit: not allowed */
	save[3] = (uint8_t)(0x80u | id);
	handle(&iwu, save, sizeof(save));
	data[0] = 0xC3; data[1] = DECT_LIA_DATA_LAST; data[2] = 0x81;
	handle(&iwu, data, 3);
	CHECK_EQ(out.msg[0][1], DECT_LIA_NACK);
	CHECK_EQ(out.msg[0][3], DECT_LIA_NACK_NOT_ALLOWED);

	edit[3] = (uint8_t)(0x80u | id);
	handle(&iwu, edit, sizeof(edit));
	CHECK_EQ(out.msg[0][1], DECT_LIA_EDIT_ENTRY_CFM);
	CHECK(dect_contacts_at(&store, 3)->lock == &iwu);

	/* rename to "Adams", keep the mobile number, delete the fixed one */
	handle(&iwu, save, sizeof(save));
	data[n++] = 0xC3; data[n++] = DECT_LIA_DATA_LAST; data[n++] = 0x81;
	data[n++] = (uint8_t)(0x80u | id);
	len_at = (unsigned)n++;
	data[n++] = 0x01; data[n++] = 0x86; data[n++] = 0xC0;
	memcpy(data + n, "Adams", 5); n += 5;
	data[n++] = 0x03; data[n++] = 0x88; data[n++] = 0xC4;
	memcpy(data + n, "0171111", 7); n += 7;
	data[n++] = 0x03; data[n++] = 0x81; data[n++] = 0xC8;	/* deleted */
	data[len_at] = (uint8_t)(0x80u | (n - len_at - 1u));
	handle(&iwu, data, n);
	CHECK_EQ(out.count, 1u);
	CHECK_EQ(out.msg[0][1], DECT_LIA_SAVE_ENTRY_CFM);
	CHECK_EQ(out.msg[0][3], (uint8_t)(0x80u | id));
	CHECK_EQ(out.msg[0][4], 0x81);			/* now first */
	CHECK_EQ(out.msg[0][5], 0x83);
	CHECK(dect_contacts_at(&store, 1)->lock == NULL);
	CHECK_EQ(dect_contacts_at(&store, 1)->n_numbers, 1u);
	CHECK(dect_contacts_take_change(&store, line, sizeof(line)) > 0);
	CHECK_EQ(strcmp(line, "1\tput\t1\t0000000a\tAdams\tAnna\t0\t*\t04:0171111"), 0);

	/* a new entry: id 0, a name and a work number on line 2 */
	n = 0;
	save[3] = 0x80;
	handle(&iwu, save, sizeof(save));
	data[n++] = 0xC3; data[n++] = DECT_LIA_DATA_LAST; data[n++] = 0x81;
	data[n++] = 0x80;
	len_at = (unsigned)n++;
	data[n++] = 0x01; data[n++] = 0x84; data[n++] = 0xC0;
	memcpy(data + n, "Neu", 3); n += 3;
	data[n++] = 0x03; data[n++] = 0x84; data[n++] = 0xC2;
	memcpy(data + n, "#31", 3); n += 3;
	data[n++] = 0x05; data[n++] = 0x83; data[n++] = 0xC0;
	data[n++] = 0x03; data[n++] = 0x82;
	data[len_at] = (uint8_t)(0x80u | (n - len_at - 1u));
	handle(&iwu, data, n);
	CHECK_EQ(out.msg[0][1], DECT_LIA_SAVE_ENTRY_CFM);
	CHECK_EQ(out.msg[0][5], 0x84);			/* four now */
	CHECK_EQ(store.n, 4u);
	CHECK(dect_contacts_take_change(&store, line, sizeof(line)) > 0);
	snprintf((char *)data, sizeof(data), "2\tput\t%u\t00000000\tNeu\t\t0\t2\t02:#31",
		 out.msg[0][3] & 0x7Fu);
	CHECK_EQ(strcmp(line, (char *)data), 0);

	/* a control character in a name: content not accepted */
	n = 0;
	handle(&iwu, save, sizeof(save));
	data[n++] = 0xC3; data[n++] = DECT_LIA_DATA_LAST; data[n++] = 0x81;
	data[n++] = 0x80; data[n++] = 0x85;
	data[n++] = 0x01; data[n++] = 0x83; data[n++] = 0xC0;
	data[n++] = 'a'; data[n++] = 0x07;
	handle(&iwu, data, n);
	CHECK_EQ(out.msg[0][1], DECT_LIA_NACK);
	CHECK_EQ(out.msg[0][3], DECT_LIA_NACK_FORMAT);
	CHECK_EQ(store.n, 4u);
}

static void test_locks_across_links(void)
{
	static struct dect_iwu a, b;
	uint8_t edit[] = { 0xC3, DECT_LIA_EDIT_ENTRY, 0x81, 0x80, 0x01 };
	uint8_t del[] = { 0xC3, DECT_LIA_DELETE_ENTRY, 0x81, 0x80 };
	static const uint8_t end[] = { 0xC3, DECT_LIA_END_SESSION, 0x81 };
	static const uint8_t clear[] = { 0xC3, DECT_LIA_DELETE_LIST, 0x81 };
	char line[64];
	unsigned id;

	dect_contacts_init(&store);
	load(three);
	id = dect_contacts_at(&store, 2)->id;
	dect_iwu_init(&a);
	dect_iwu_init(&b);
	a.contacts = b.contacts = &store;
	start_contacts(&a);
	start_contacts(&b);
	edit[3] = del[3] = (uint8_t)(0x80u | id);

	handle(&a, edit, sizeof(edit));
	CHECK_EQ(out.msg[0][1], DECT_LIA_EDIT_ENTRY_CFM);
	handle(&b, edit, sizeof(edit));
	CHECK_EQ(out.msg[0][1], DECT_LIA_NACK);
	CHECK_EQ(out.msg[0][3], DECT_LIA_NACK_TEMP_NOT_POSSIBLE);
	handle(&b, del, sizeof(del));
	CHECK_EQ(out.msg[0][3], DECT_LIA_NACK_TEMP_NOT_POSSIBLE);

	/* a's session ends: the entry is free for b */
	handle(&a, end, sizeof(end));
	handle(&b, del, sizeof(del));
	CHECK_EQ(out.msg[0][1], DECT_LIA_DELETE_ENTRY_CFM);
	CHECK_EQ(out.msg[0][3], 0x82);			/* two left */
	CHECK(dect_contacts_take_change(&store, line, sizeof(line)) > 0);

	/* 'delete list': refused unless the owner allows it */
	handle(&b, clear, sizeof(clear));
	CHECK_EQ(out.msg[0][1], DECT_LIA_NACK);
	CHECK_EQ(out.msg[0][3], DECT_LIA_NACK_NOT_ALLOWED);
	store.allow_clear = 1;
	handle(&b, clear, sizeof(clear));
	CHECK_EQ(out.msg[0][1], DECT_LIA_DELETE_LIST_CFM);
	CHECK_EQ(store.n, 0u);

	/* a call release frees a link's locks (the clear synced first) */
	load("#opendectcell-contacts 1 seq=9\n"
	     "0000000a\t-\tSmith\tAnna\t0\t*\n");
	start_contacts(&a);
	edit[3] = (uint8_t)(0x80u | dect_contacts_at(&store, 1)->id);
	handle(&a, edit, sizeof(edit));
	dect_iwu_reset_sessions(&a);
	CHECK(dect_contacts_at(&store, 1)->lock == NULL);
}

static void test_search_by_name(void)
{
	/* option 01H, "sm", forward 2, Name only */
	static const uint8_t search[] = { 0xC3, DECT_LIA_SEARCH_ENTRIES, 0x81,
					  0x01, 0x02, 's', 'm', 0x02, 0x00,
					  0x01 };
	static const uint8_t exact[] = { 0xC3, DECT_LIA_SEARCH_ENTRIES, 0x81,
					 0x00, 0x02, 's', 'm', 0x01, 0x00,
					 0x01 };
	static const uint8_t ae[] = { 0xC3, DECT_LIA_SEARCH_ENTRIES, 0x81,
				      0x00, 0x05, 'a', 'r', 'g', 'e', 'r',
				      0x01, 0x00, 0x01 };
	struct dect_iwu iwu;

	dect_contacts_init(&store);
	load(three);
	dect_iwu_init(&iwu);
	iwu.contacts = &store;
	start_contacts(&iwu);
	handle(&iwu, search, sizeof(search));
	CHECK_EQ(out.msg[0][1], DECT_LIA_SEARCH_ENTRIES_CFM);
	CHECK_EQ(out.msg[0][3], 0x83);			/* Smith */
	CHECK_EQ(out.msg[0][4], 0x01);			/* the end of the list */
	handle(&iwu, exact, sizeof(exact));
	CHECK_EQ(out.msg[0][4], 0x00);			/* no exact "sm" */
	CHECK_EQ(out.count, 1u);
	handle(&iwu, ae, sizeof(ae));
	CHECK_EQ(out.msg[0][3], 0x81);			/* "arger" is Ärger */
	CHECK_EQ(out.msg[0][4], 0x01);
}

int main(void)
{
	test_fold_order();
	test_load_sorts_and_keeps_identifiers();
	test_handset_changes_survive_until_synced();
	test_deletions_until_synced();
	test_caller_numbers();
	test_session_and_fields();
	test_read_contacts();
	test_partial_delivery();
	test_edit_save_and_create();
	test_locks_across_links();
	test_search_by_name();
	return CHECK_DONE();
}

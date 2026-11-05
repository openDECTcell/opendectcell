// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for src/libopendectcell/dect_base.c: the handset table and its working
 * copy (docs/stack-in-library.md, stage S1).
 */
#include "check.h"
#include "../src/libopendectcell/dect_base.h"

#include <stdio.h>
#include <string.h>

static unsigned saved_i = 99, saved_slot;

static void save(void *ctx, unsigned i, const struct dect_hs_rec *r)
{
	CHECK(ctx == &saved_i);
	saved_i = i;
	saved_slot = r->slot;
}

static unsigned sent_prim, n_sent;
static struct dect_mail sent[16];
static char logged[300];

static int send(void *ctx, const struct dect_mail *m)
{
	(void)ctx;
	sent_prim = m->prim;
	if (n_sent < 16u)
		sent[n_sent++] = *m;
	return 0;
}

static void rnd(void *ctx, uint8_t *out, size_t n)
{
	(void)ctx;
	memset(out, 0x5C, n);
}

static char log_all[2048];	/* every line since it was last emptied */

static void log_line(void *ctx, const char *line)
{
	size_t n = strlen(log_all);

	(void)ctx;
	snprintf(logged, sizeof(logged), "%s", line);
	snprintf(log_all + n, sizeof(log_all) - n, "%s\n", line);
}

/* The chip's 0xFFF1 sub 0x08 answer: 17 bytes at addr, each `fill`. */
static void mem_answer(struct dect_base *bb, uint32_t addr, uint8_t fill)
{
	struct dect_mail m;

	memset(&m, 0, sizeof(m));
	m.prim = 0xFFF1u;
	m.param[0] = 0x08u;
	m.param[1] = (uint8_t)addr;
	m.param[2] = (uint8_t)(addr >> 8);
	m.param[3] = (uint8_t)(addr >> 16);
	m.param[4] = (uint8_t)(addr >> 24);
	m.param[5] = DECT_BASE_UAK_STRIDE;
	memset(&m.param[7], fill, DECT_BASE_UAK_STRIDE);
	m.param_len = 7u + DECT_BASE_UAK_STRIDE;
	dect_base_mem_reply(bb, &m);
}

static unsigned installs;
static char event_line[300];

static void event(void *ctx, struct dect_ctl_out *o)
{
	int n = dect_ctl_end(o);

	(void)ctx;
	if (n > 0)
		snprintf(event_line, sizeof(event_line), "%.*s", n, o->buf);
}

static unsigned named_i = 99;

static void named(void *ctx, unsigned i)
{
	(void)ctx;
	named_i = i;
}

static char switched_why[64];

static void switched(void *ctx, const char *why)
{
	(void)ctx;
	snprintf(switched_why, sizeof(switched_why), "%s", why);
}

static unsigned call_downs;

static void call_down(void *ctx)
{
	(void)ctx;
	call_downs++;
}

static void install(void *ctx)
{
	(void)ctx;
	installs++;
}

static unsigned pumps;
static unsigned registered_calls;
static unsigned rejected_calls;
static int rejected_pin, rejected_expected;

static void rejected_cb(void *ctx, int pin, int expected)
{
	(void)ctx;
	rejected_calls++;
	rejected_pin = pin;
	rejected_expected = expected;
}

static void registered_cb(void *ctx, uint64_t now)
{
	(void)ctx;
	(void)now;
	registered_calls++;
}

static void pump(void *ctx, struct dect_base_link *bl)
{
	(void)ctx;
	(void)bl;
	pumps++;
}

static const struct dect_base_ops ops = { .save = save, .send = send,
					  .random = rnd, .log = log_line,
					  .install = install, .event = event,
					  .pump = pump, .named = named,
					  .switched = switched,
					  .call_down = call_down };

/* name_visible(): slots 1 and 2 are one account, 3 is another (API 138). */
static int vis_calls;

static int same_account(void *ctx, unsigned viewer, unsigned slot)
{
	(void)ctx;
	vis_calls++;
	return (viewer == 3u) == (slot == 3u);
}

static unsigned mask_calls;

static void count_mask(void *ctx)
{
	(void)ctx;
	mask_calls++;
}

/*
 * A segment from a handset that has not proved its key is no contact (review finding C1, API 157): a LOCATE-REQUEST naming
 * a held IPUI makes the connection adopt that handset, but last_seen_ms stays 0 until the key has been proved.
 */
/*
 * The in-band indication follows the handset's profile (API 159): a handset whose profile says cc.progress_in_band is told yes, another
 * handset no, an unknown IPUI has no opinion (-1: the link's setting stands).
 */
static void test_progress_in_band_follows_the_profile(void)
{
	static const uint8_t a[] = { 0x80, 0xa8, 0x03, 0x11, 0x22, 0x33, 0x44 };
	static const uint8_t c[] = { 0x80, 0xa8, 0x03, 0x55, 0x66, 0x77, 0x88 };
	static const uint8_t u[] = { 0x80, 0xa8, 0x03, 0x99, 0x99, 0x99, 0x99 };
	struct dect_base b;

	dect_base_init(&b);
	b.n_hs = 2;
	memcpy(b.hs[0].port_id, a, sizeof(a));
	b.hs[0].port_id_len = sizeof(a);
	memcpy(b.hs[1].port_id, c, sizeof(c));
	b.hs[1].port_id_len = sizeof(c);
	b.prof[0].cc_progress_in_band = 1;
	dect_base_conn_up(&b, 1, 0);
	CHECK(b.main_link.link.progress_for != NULL);
	CHECK_EQ(b.main_link.link.progress_for(&b, a, sizeof(a)), 1);
	CHECK_EQ(b.main_link.link.progress_for(&b, c, sizeof(c)), 0);
	CHECK_EQ(b.main_link.link.progress_for(&b, u, sizeof(u)), -1);
}

static void test_an_unproved_handset_is_not_seen(void)
{
	struct dect_base b;
	struct dect_mm *mm;

	dect_base_init(&b);
	b.n_hs = 2;
	mm = &b.main_link.link.mm;
	mm->adopted = 1;
	mm->authenticated = 0;
	dect_base_note_seen(&b, mm, 1000);
	CHECK_EQ(b.last_seen_ms[1], 0u);		/* named, not proved: not seen */
	mm->authenticated = 1;
	dect_base_note_seen(&b, mm, 2000);
	CHECK_EQ(b.last_seen_ms[1], 2000u);		/* proved: seen */
	CHECK_EQ(b.last_seen_ms[0], 0u);		/* and only that record */
	mm->adopted = -1;
	dect_base_note_seen(&b, mm, 3000);		/* no handset adopted: nothing */
	mm->adopted = 5;				/* out of range: nothing */
	dect_base_note_seen(&b, mm, 3000);
	CHECK_EQ(b.last_seen_ms[1], 2000u);
}

int main(void)
{
	struct dect_base b;
	char nm_buf[64];

	test_an_unproved_handset_is_not_seen();
	test_progress_in_band_follows_the_profile();
	dect_base_init(&b);
	b.n_hs = 3;
	b.hs[0].slot = 1;
	b.hs[1].slot = 2;
	b.hs[2].slot = 3;
	dect_base_load(&b, 0);
	CHECK_EQ(b.rec.slot, 1u);

	/* a change to the working copy goes back on a switch */
	b.rec.uak_valid = 1;
	b.rec.uak[0] = 0x5A;
	CHECK_EQ(dect_base_switch(&b, 2), 1);
	CHECK_EQ(b.cur, 2u);
	CHECK_EQ(b.rec.slot, 3u);
	CHECK_EQ(b.rec.uak_valid, 0);
	CHECK_EQ(b.hs[0].uak_valid, 1);
	CHECK_EQ(b.hs[0].uak[0], 0x5A);

	/* already current, or out of range: nothing happens */
	b.rec.port_id_len = 7;
	CHECK_EQ(dect_base_switch(&b, 2), 0);
	CHECK_EQ(dect_base_switch(&b, 3), 0);
	CHECK_EQ(dect_base_switch(&b, DECT_BASE_HS_MAX), 0);
	CHECK_EQ(b.cur, 2u);
	CHECK_EQ(b.hs[2].port_id_len, 0u);	/* not stashed yet */
	dect_base_stash(&b);
	CHECK_EQ(b.hs[2].port_id_len, 7u);
	dect_base_load(&b, DECT_BASE_HS_MAX);	/* out of range: unchanged */
	CHECK_EQ(b.rec.slot, 3u);

	/* a changed record goes to the host's save(), if it has one */
	dect_base_changed(&b);
	CHECK_EQ(saved_i, 99u);
	{
		struct dect_mail m;
		uint8_t r[4] = { 1, 2, 3, 4 };

		/* without a host: no mail, zero bytes, no log */
		memset(&m, 0, sizeof(m));
		m.prim = 0x0247u;
		CHECK_EQ(dect_base_send(&b, &m), -1);
		dect_base_random(&b, r, sizeof(r));
		CHECK_EQ(r[0] | r[3], 0);
		dect_base_log(&b, "unseen %u", 1u);
		CHECK_EQ(logged[0], '\0');
		b.ops = &ops;
		CHECK_EQ(dect_base_send(&b, &m), 0);
		CHECK_EQ(sent_prim, 0x0247u);
		dect_base_random(&b, r, sizeof(r));
		CHECK_EQ(r[3], 0x5C);
		dect_base_log(&b, "sub: slot %u", 3u);
		CHECK_EQ(strcmp(logged, "sub: slot 3"), 0);
		b.ops = NULL;
	}
	b.ops = &ops;
	b.ctx = &saved_i;
	dect_base_changed(&b);
	CHECK_EQ(saved_i, 2u);
	CHECK_EQ(saved_slot, 3u);

	/* the chip's slots (S2.2) */
	{
		uint8_t uak[DECT_MM_UAK_LEN];

		memset(uak, 0xB0, sizeof(uak));
		b.ops = &ops;
		b.chip.att_flag = 0x01;
		b.chip.att_cfg = 0x02;
		n_sent = 0;
		dect_base_slot_clear(&b, (1u << 1) | (1u << 3));
		CHECK_EQ(n_sent, 4u);			/* 2, 4, 5, 6 */
		CHECK_EQ(sent[0].prim, 0x0247u);
		CHECK_EQ(sent[0].dst, DECT_TASK_CONTACT_DST);
		CHECK_EQ(sent[0].param[0], 2);
		CHECK_EQ(sent[0].param[1], 0);
		CHECK_EQ(sent[3].param[0], 6);
		CHECK(strstr(logged, "other than 1 3 --") != NULL);
		n_sent = 0;
		CHECK_EQ(dect_base_slot_install(&b, 2, uak), 0);
		CHECK_EQ(n_sent, 2u);
		CHECK_EQ(sent[0].param_len, 3u);
		CHECK_EQ(sent[0].param[1], 1);		/* op 1 */
		CHECK_EQ(sent[0].param[2], 0x01);	/* att_flag */
		CHECK_EQ(sent[1].prim, 0x020Au);
		CHECK_EQ(sent[1].param_len, 18u);
		CHECK_EQ(sent[1].param[0], 2);
		CHECK_EQ(sent[1].param[17], 0x02);	/* att_cfg */
		n_sent = 0;
		CHECK_EQ(dect_base_slot_install(&b, 7, uak), -1);
		CHECK_EQ(n_sent, 1u);			/* no 0x020A for slot 7 */
		n_sent = 0;
		dect_base_slot_mask(&b, 0x05);
		CHECK_EQ(n_sent, 1u);
		CHECK_EQ(sent[0].prim, 0x017Cu);
		CHECK_EQ(sent[0].param[1], 0x05);
		b.chip.no_mask = 1;
		dect_base_slot_mask(&b, 0x05);
		CHECK_EQ(n_sent, 1u);
	}

	/* the table read back (S2.2b) */
	{
		unsigned k;

		memset(b.rec.uak, 0xB0, DECT_MM_UAK_LEN);
		b.chip.uak_table = 0x9518u;
		/* the read-back: one 17-byte read at slot 2's record */
		n_sent = 0;
		dect_base_uak_verify(&b, 2, 1000);
		CHECK_EQ(n_sent, 1u);
		CHECK_EQ(sent[0].prim, 0xFFF1u);
		CHECK_EQ(sent[0].dst, DECT_TASK_PCM_REMOTE);
		CHECK_EQ(sent[0].param_len, 7u);
		CHECK_EQ(sent[0].param[0], 0x07);
		CHECK_EQ(sent[0].param[1], 0x29);	/* 0x9518 + 0x11 */
		CHECK_EQ(sent[0].param[2], 0x95);
		CHECK_EQ(sent[0].param[5], 0x11);
		mem_answer(&b, 0x9529u, 0xB0);
		CHECK(strstr(logged, "slot 2 at 0x09529: MATCHES") != NULL);
		CHECK_EQ(b.check.uak_addr, 0u);
		dect_base_uak_verify(&b, 2, 1000);
		mem_answer(&b, 0x9529u, 0x00);
		CHECK(strstr(logged, "MISMATCH") != NULL);
		CHECK(strstr(logged, "Unless") == NULL);
		/* a guessed table says so; the key bytes are never logged */
		b.chip.uak_guess = 1;
		dect_base_uak_verify(&b, 2, 1000);
		mem_answer(&b, 0x9529u, 0x00);
		CHECK(strstr(logged, "Unless the STUB is not 08.25") != NULL);
		CHECK(strstr(logged, "b0") == NULL && strstr(logged, "B0") == NULL);
		b.chip.uak_guess = 0;
		/* no slot outside 1..6 */
		n_sent = 0;
		dect_base_uak_verify(&b, 7, 1000);
		CHECK_EQ(n_sent, 0u);

		/* the audit: ours in 2, our other record in 4, a stranger in 5 */
		n_sent = 0;
		dect_base_slot_audit(&b, 2, (1u << 1) | (1u << 3), 1000);
		CHECK_EQ(n_sent, 6u);
		CHECK_EQ(b.check.pending, 0x3Fu);
		log_all[0] = '\0';
		for (k = 1; k <= 6; k++)
			mem_answer(&b, 0x9518u + (k - 1u) * 0x11u,
				   k == 2 || k == 4 || k == 5 ? 0x77 : 0x00);
		CHECK_EQ(b.check.pending, 0u);
		CHECK(strstr(log_all, "audit: 1=- 2=OURS 3=- 4=ours 5=held 6=-")
		      != NULL);
		CHECK(strstr(log_all, "*** 1 FOREIGN SLOT(S) STILL HELD ***")
		      != NULL);
		CHECK(strstr(log_all, "as we intend it") == NULL);
		/* our own slot empty, nothing foreign */
		dect_base_slot_audit(&b, 2, 0, 1000);
		log_all[0] = '\0';
		for (k = 1; k <= 6; k++)
			mem_answer(&b, 0x9518u + (k - 1u) * 0x11u, 0x00);
		CHECK(strstr(log_all, "OUR OWN SLOT 2 IS EMPTY") != NULL);
		CHECK(strstr(log_all, "FOREIGN") == NULL);
		/* everything where we put it */
		dect_base_slot_audit(&b, 2, 0, 1000);
		log_all[0] = '\0';
		for (k = 1; k <= 6; k++)
			mem_answer(&b, 0x9518u + (k - 1u) * 0x11u, k == 2 ? 1 : 0);
		CHECK(strstr(log_all, "slot table is as we intend it") != NULL);
		/* an answer to a read nobody asked for changes nothing */
		log_all[0] = '\0';
		mem_answer(&b, 0x9529u, 0x00);
		CHECK_EQ(log_all[0], '\0');

		/* a build we hold no table for: neither, said once each */
		b.chip.uak_table = 0;
		b.chip.stub = "dectfw_secondlevel_441-07.56.hex";
		n_sent = 0;
		dect_base_uak_verify(&b, 2, 1000);
		CHECK(strstr(logged, "read-back skipped -- STUB "
			      "dectfw_secondlevel_441-07.56.hex") != NULL);
		dect_base_slot_audit(&b, 2, 0, 1000);
		CHECK(strstr(logged, "slot audit skipped") != NULL);
		CHECK_EQ(n_sent, 0u);

		/* 06.88: both wait the delay, then the audit goes first */
		b.chip.uak_table = 0x067Fu;
		b.chip.check_delay_ms = 1000u;
		n_sent = 0;
		dect_base_slot_audit(&b, 1, 0, 5000);
		dect_base_uak_verify(&b, 1, 5040);
		CHECK_EQ(n_sent, 0u);
		dect_base_sub_check_tick(&b, 6039);
		CHECK_EQ(n_sent, 0u);
		dect_base_tick(&b, 6040, 0);
		CHECK_EQ(n_sent, 7u);
		CHECK_EQ(sent[0].param[1], 0x7F);	/* the audit's slot 1 */
		CHECK_EQ(sent[6].param[1], 0x7F);	/* then the read-back */
		CHECK_EQ(b.check.uak_addr, 0x067Fu);
		CHECK_EQ(b.check.at, 0u);
		b.chip.check_delay_ms = 0;
		b.chip.stub = NULL;
		memset(&b.check, 0, sizeof(b.check));
	}

	/* a connection comes up: the current identity and the held ones */
	{
		struct dect_base_link bl;
		struct dect_mm_cfg cfg;

		dect_base_init(&b);
		b.n_hs = 3;
		b.hs[0].slot = 1;
		b.hs[1].slot = 2;
		b.hs[2].slot = 3;
		memcpy(b.hs[0].port_id, "\x80\xa8\x03\x11\x22\x33\x44", 7);
		b.hs[0].port_id_len = 7;
		memcpy(b.hs[1].port_id, "\x80\xa8\x03\x55\x66\x77\x88", 7);
		b.hs[1].port_id_len = 7;
		memcpy(b.hs[2].port_id, "\x80\xa8\x03\x99\xaa\xbb\xcc", 7);
		b.hs[2].port_id_len = 7;
		dect_base_load(&b, 0);
		memset(&cfg, 0, sizeof(cfg));
		cfg.sub_slot = 1;
		memset(&bl, 0, sizeof(bl));
		bl.persisted = 1;
		dect_base_link_open(&b, &bl, 4, &cfg, 1u << 1);	/* not hs[2] */
		CHECK_EQ(bl.active, 1);
		CHECK_EQ(bl.handle, 4);
		CHECK_EQ(bl.persisted, 0);
		CHECK_EQ(bl.link.mm.port_id_len, 7u);
		CHECK_EQ(bl.link.mm.port_id[6], 0x44);
		CHECK_EQ(bl.link.mm.n_held, 2u);	/* hs[1], then hs[0] */
		/*
		 * A record's own TPUI reaches the MM without tpuioffset= (API
		 * 141: the pool); a record without one is derived from its slot.
		 */
		memcpy(b.hs[1].tpui, "\x00\x11\x9a", 3);
		memset(&bl, 0, sizeof(bl));
		dect_base_link_open(&b, &bl, 4, &cfg, 1u << 1);
		CHECK_EQ(bl.link.mm.held[0].tpui[1], 0x11);
		CHECK_EQ(bl.link.mm.held[0].tpui[2], 0x9a);
		CHECK_EQ(bl.link.mm.held[1].tpui[2], 0x00);	/* hs[0]: derived */
		memset(b.hs[1].tpui, 0, 3);
	}

	/* chip slots and a side link (S2.4) */
	{
		struct dect_base_link bl;
		struct dect_mm_cfg cfg;

		CHECK_EQ(dect_base_slot(&b, 1), 2u);
		b.slot0 = 5;
		CHECK_EQ(dect_base_slot(&b, 0), 5u);	/* current, record 0 */
		b.rec.slot = DECT_BASE_HS_MAX + 1u;
		b.slot0 = 0;
		CHECK_EQ(dect_base_slot(&b, 0), DECT_MM_REG_SLOT); /* bad slot */
		b.rec.slot = 1;
		memset(&cfg, 0, sizeof(cfg));
		memset(&bl, 0, sizeof(bl));
		dect_base_side_open(&b, &bl, 5, &cfg, (1u << 0) | (1u << 2));
		CHECK_EQ(bl.active, 1);
		CHECK_EQ(bl.handle, 5);
		CHECK_EQ(bl.link.mm.port_id_len, 0u);	/* nobody expected */
		CHECK_EQ(bl.link.mm.n_held, 2u);
		CHECK_EQ(bl.link.mm.held[0].tag, 0);	/* table order */
		CHECK_EQ(bl.link.mm.held[1].tag, 2);
		CHECK_EQ(bl.link.mm.held[1].slot, 3);
	}

	/* a record's key: the working copy for the current one (S3a) */
	b.hs[1].uak_valid = 1;
	b.hs[1].uak[0] = 0x11;
	b.hs[2].uak_valid = 0;
	b.rec.uak_valid = 1;
	b.rec.uak[0] = 0x22;
	CHECK_EQ(dect_base_uak(&b, 1)[0], 0x11);
	CHECK(dect_base_uak(&b, 2) == NULL);
	CHECK_EQ(dect_base_uak(&b, b.cur)[0], 0x22);
	CHECK(dect_base_uak(&b, DECT_BASE_HS_MAX) == NULL);

	/* the call table (S3d) */
	{
		struct dect_base_call *c1, *c2;

		memset(b.calls, 0, sizeof(b.calls));
		c1 = dect_base_call_new(&b);
		c1->id = 5;
		c1->party = -1;
		c1->dect_id = 1;
		c2 = dect_base_call_new(&b);
		CHECK(c2 != c1);
		c2->id = 6;
		c2->party = -1;
		c2->dect_id = 2;
		c2->cs = DECT_CS_CALL_CONNECT;
		CHECK(dect_base_call_by_id(&b, 6) == c2);
		CHECK(dect_base_call_by_id(&b, 0) == NULL);
		CHECK(dect_base_call_on(&b, -1) == c1);
		CHECK(dect_base_call_on(&b, 0) == NULL);
		CHECK_EQ(dect_base_calls_on(&b, -1), 2u);
		CHECK(dect_base_call_active(&b, -1) == c2);
		CHECK(dect_base_call_by_dect(&b, -1, 1) == c1);
		CHECK(dect_base_call_by_dect(&b, -1, 0) == NULL);
		CHECK(dect_base_call_new(&b) != NULL);	/* the third */
		b.calls[2].id = 7;
		CHECK(dect_base_call_new(&b) == NULL);	/* all taken */
		memset(b.calls, 0, sizeof(b.calls));
	}

	/* whose handset a link serves; an event to the bridge (S3e) */
	{
		struct dect_base_call c;

		b.main_link.link.mm.adopted = -1;
		b.side[1].adopted = 2;
		b.side[0].adopted = -1;
		CHECK_EQ(dect_base_party_slot(&b, -1), dect_base_slot(&b, b.cur));
		CHECK_EQ(dect_base_party_slot(&b, 1), 3u);
		CHECK_EQ(dect_base_party_slot(&b, 0), 0u);
		memset(&c, 0, sizeof(c));
		c.id = 9;
		c.party = DECT_BASE_PARTY_PAGING;
		c.target = 1;
		CHECK_EQ(dect_base_call_slot(&b, &c), 2u);
		b.ops = &ops;
		c.party = 1;
		dect_base_call_event(&b, &c, "ringing", NULL, NULL);
		CHECK_EQ(strcmp(event_line, "ringing call=9 hs=3\n"), 0);
		c.party = 0;				/* nobody yet: no hs */
		dect_base_call_event(&b, &c, "released", "by", "hs");
		CHECK_EQ(strcmp(event_line, "released call=9 by=hs\n"), 0);
		b.ops = NULL;
	}

	/* a record's TPUI and a page (S3g) */
	{
		uint8_t tpui[3];

		dect_base_tpui(&b, 1, tpui);		/* slot 2 */
		CHECK_EQ(tpui[1], 0x11);
		CHECK_EQ(tpui[2], 0x02);
		/* a TPUI off the slot (tpuioffset=, docs/more-than-six.md T-C) */
		b.cfg.tpui_offset = 0x40;
		dect_base_tpui(&b, 1, tpui);
		CHECK_EQ(tpui[1], 0x11);
		CHECK_EQ(tpui[2], 0x42);
		b.cfg.tpui_offset = 0;
		dect_base_tpui(&b, 1, tpui);
		CHECK_EQ(tpui[2], 0x02);
		/* a record's own TPUI wins over the slot and the offset (API 141) */
		{
			const uint8_t own[3] = { 0x00, 0x11, 0x9a };
			const unsigned cur = b.cur;
			uint8_t keep[3];

			b.cfg.tpui_offset = 0x40;
			if (cur == 1u) {
				memcpy(keep, b.rec.tpui, 3);
				memcpy(b.rec.tpui, own, 3);
			} else {
				memcpy(keep, b.hs[1].tpui, 3);
				memcpy(b.hs[1].tpui, own, 3);
			}
			dect_base_tpui(&b, 1, tpui);
			CHECK_EQ(tpui[1], 0x11);
			CHECK_EQ(tpui[2], 0x9a);
			dect_base_tpui(&b, 0, tpui);		/* no TPUI of its own */
			CHECK_EQ(tpui[2], 0x41);
			if (cur == 1u)
				memcpy(b.rec.tpui, keep, 3);
			else
				memcpy(b.hs[1].tpui, keep, 3);
			dect_base_tpui(&b, 1, tpui);
			CHECK_EQ(tpui[2], 0x42);
			b.cfg.tpui_offset = 0;
		}
		dect_base_tpui(&b, 1, tpui);			/* back to slot 2 */
		CHECK_EQ(tpui[2], 0x02);
		b.ops = &ops;
		n_sent = 0;
		dect_base_page(&b, 0x2C, tpui);
		CHECK_EQ(n_sent, 1u);
		CHECK_EQ(sent[0].prim, 0x0300u);
		CHECK_EQ(sent[0].param[0], 0x2C);
		CHECK_EQ(sent[0].param[2], 0x02);
		b.ops = NULL;
	}

	/* the audio binds (S3j) */
	b.ops = &ops;
	n_sent = 0;
	dect_base_audio_param(&b, 1, 2, 0x2B);
	dect_base_audio_assign(&b, 1, 0x41, 0x0204, 0x00, 0x00);
	dect_base_audio_path(&b, 1);
	dect_base_audio_release(&b, 1);
	CHECK_EQ(n_sent, 4u);
	CHECK_EQ(sent[0].prim, 0x0251u);
	CHECK_EQ(sent[0].param[2], 0x2B);
	CHECK_EQ(sent[1].prim, 0x0230u);
	CHECK_EQ(sent[1].param_len, 6u);
	CHECK_EQ(sent[1].param[2], 0x04);	/* mask, little-endian */
	CHECK_EQ(sent[1].param[3], 0x02);
	CHECK_EQ(sent[2].prim, 0x022Du);
	CHECK_EQ(sent[2].param[1], 0x00);
	CHECK_EQ(sent[3].prim, 0x0233u);
	CHECK_EQ(sent[3].param[1], 0x01);
	b.ops = NULL;

	/* the ring's SETUP helpers (S3l) */
	{
		memset(b.calls, 0, sizeof(b.calls));
		memset(b.cap, 0, sizeof(b.cap));
		memset(b.prof, 0, sizeof(b.prof));
		b.cfg.basic_svc = 0x88;
		CHECK_EQ(dect_base_basic_svc(&b, 0, 0x88), 0x88);
		b.cap[0].known = 1;			/* a pure GAP handset */
		CHECK_EQ(dect_base_basic_svc(&b, 0, 0x88), 0x80);
		b.prof[1].cc_basic_service = 2;		/* its profile: narrow */
		CHECK_EQ(dect_base_basic_svc(&b, 1, 0x88), 0x80);
		CHECK_EQ(dect_base_ring_svc(&b, 2, 1), 0x80);
		CHECK_EQ(dect_base_page_info(&b, 0), 0x2C);
		CHECK_EQ(dect_base_page_info(&b, 1), 0x0C);
		b.calls[0].id = 3;
		b.calls[0].dect_id = 1;
		b.main_link.active = 0;
		CHECK_EQ(dect_base_call_id(&b, NULL), 2);	/* 1 is taken */
		b.cfg.callinfo.has_call_id = 1;
		CHECK_EQ(dect_base_call_id(&b, NULL), 0);
		b.cfg.callinfo.has_call_id = 0;
		memset(b.calls, 0, sizeof(b.calls));
		memset(b.cap, 0, sizeof(b.cap));
		memset(b.prof, 0, sizeof(b.prof));
	}

	/* the registration completes: key, gate, install, save, once */
	{
		struct dect_mm *mm = &b.main_link.link.mm;

		b.ops = &ops;
		b.ctx = &saved_i;
		b.rec.uak_valid = 0;
		b.rec.port_id_len = 0;
		b.main_link.persisted = 0;
		CHECK_EQ(dect_base_main_registered(&b), 0);	/* not yet */
		mm->subscribed = 1;
		mm->stats.located = 1;
		memset(mm->uak, 0x77, DECT_MM_UAK_LEN);
		memcpy(mm->port_id, "\x80\xa8\x03\x01\x02\x03\x04", 7);
		mm->port_id_len = 7;
		saved_i = 99;
		installs = 0;
		CHECK_EQ(dect_base_main_registered(&b), 1);
		CHECK_EQ(b.rec.uak_valid, 1);
		CHECK_EQ(b.rec.uak[15], 0x77);
		CHECK_EQ(b.rec.port_id_len, 7u);
		CHECK_EQ(installs, 1u);
		CHECK_EQ(saved_i, b.cur);
		CHECK_EQ(b.rec.tpui[1], 0x11);			/* the TPUI it was assigned */
		CHECK(b.rec.tpui[2] != 0);
		CHECK_EQ(b.main_link.persisted, 1);
		CHECK_EQ(dect_base_main_registered(&b), 0);	/* once */
		CHECK_EQ(installs, 1u);
	}

	/*
	 * A handset that registers and does not locate on the same link (it
	 * counts itself located already, a re-registration over its own record):
	 * its key is persisted after the grace, not lost (API 140).
	 */
	{
		struct dect_mm *mm = &b.main_link.link.mm;
		static struct dect_base_ops rops;
		const uint64_t t0 = 100000;

		rops = ops;
		rops.registered = registered_cb;
		b.ops = &rops;
		b.ctx = &saved_i;
		b.rec.uak_valid = 0;
		b.rec.port_id_len = 0;
		b.main_link.active = 1;
		b.main_link.persisted = 0;
		b.main_link.reg_done_ms = 0;
		b.main_link.persist_force = 0;
		mm->subscribed = 1;
		mm->stats.located = 0;
		memset(mm->uak, 0x55, DECT_MM_UAK_LEN);
		memcpy(mm->port_id, "\x80\xa8\x03\x01\x02\x03\x05", 7);
		mm->port_id_len = 7;
		installs = 0;
		registered_calls = 0;

		dect_base_registration_tick(&b, t0);		/* starts the grace */
		CHECK_EQ(dect_base_main_registered(&b), 0);	/* not located */
		dect_base_registration_tick(&b, t0 + DECT_BASE_LOCATE_GRACE_MS - 1);
		CHECK_EQ(b.main_link.persisted, 0);
		dect_base_registration_tick(&b, t0 + DECT_BASE_LOCATE_GRACE_MS);
		CHECK_EQ(b.main_link.persisted, 1);
		CHECK_EQ(installs, 1u);
		CHECK_EQ(registered_calls, 1u);
		CHECK_EQ(b.rec.uak[0], 0x55);
		dect_base_registration_tick(&b, t0 + 10 * DECT_BASE_LOCATE_GRACE_MS);
		CHECK_EQ(installs, 1u);				/* once */

		/* a handset that does locate is persisted by the mail path, and the
		 * tick does nothing for it */
		b.main_link.persisted = 0;
		b.main_link.reg_done_ms = 0;
		b.main_link.persist_force = 0;
		mm->stats.located = 1;
		installs = 0;
		registered_calls = 0;
		dect_base_registration_tick(&b, t0);
		dect_base_registration_tick(&b, t0 + 10 * DECT_BASE_LOCATE_GRACE_MS);
		CHECK_EQ(b.main_link.persisted, 0);
		CHECK_EQ(installs, 0u);
		mm->stats.located = 0;
		mm->subscribed = 0;
		b.main_link.active = 0;
		b.ops = &ops;
	}

	/* a new handset takes the free record with the lowest slot (API 140) */
	{
		const unsigned n0 = b.n_hs;
		uint8_t sl[3], ld[3], rm[3], ab[3];
		unsigned i;

		for (i = 0; i < 3; i++) {
			sl[i] = (uint8_t)b.hs[i].slot;
			ld[i] = (uint8_t)b.loaded[i];
			rm[i] = (uint8_t)b.remote[i];
			ab[i] = (uint8_t)b.absent[i];
			b.loaded[i] = 0;
			b.remote[i] = 0;
			b.absent[i] = 1;
		}
		b.n_hs = 3;
		b.hs[0].slot = 3;	/* list order 0, 1, 2 = slots 3, 2, 1 */
		b.hs[1].slot = 2;
		b.hs[2].slot = 1;
		CHECK_EQ(dect_base_free_record(&b), 2);		/* slot 1 */
		b.loaded[2] = 1;
		CHECK_EQ(dect_base_free_record(&b), 1);		/* slot 2 */
		b.remote[1] = 1;
		CHECK_EQ(dect_base_free_record(&b), 0);		/* slot 3 */
		b.loaded[0] = 1;
		CHECK_EQ(dect_base_free_record(&b), -1);
		b.n_hs = n0;
		for (i = 0; i < 3; i++) {
			b.hs[i].slot = sl[i];
			b.loaded[i] = ld[i];
			b.remote[i] = rm[i];
			b.absent[i] = ab[i];
		}
	}

	/* the key read-back is asked once more when nothing answers (API 140) */
	{
		const uint64_t t0 = 5000;
		const uint32_t table0 = b.chip.uak_table;
		const uint32_t delay0 = b.chip.check_delay_ms;
		unsigned n0;

		b.ops = &ops;
		b.chip.uak_table = 0x9518;
		b.chip.check_delay_ms = 0;
		memset(&b.check, 0, sizeof(b.check));
		n_sent = 0;
		dect_base_uak_verify(&b, 1, t0);
		n0 = n_sent;
		CHECK_EQ(n0, 1u);				/* the read */
		dect_base_sub_check_tick(&b, t0 + DECT_BASE_UAK_RETRY_MS - 1);
		CHECK_EQ(n_sent, n0);				/* not yet */
		log_all[0] = '\0';
		dect_base_sub_check_tick(&b, t0 + DECT_BASE_UAK_RETRY_MS);
		CHECK_EQ(n_sent, n0 + 1u);			/* once more */
		CHECK(strstr(log_all, "asking once more") != NULL);
		dect_base_sub_check_tick(&b, t0 + 2 * DECT_BASE_UAK_RETRY_MS);
		CHECK_EQ(n_sent, n0 + 1u);			/* not a third */
		CHECK(strstr(log_all, "after two reads") != NULL);
		memset(&b.check, 0, sizeof(b.check));
		b.chip.uak_table = table0;
		b.chip.check_delay_ms = delay0;
	}

	/* a rejected enrolment is told to the host, once per attempt (API 142) */
	{
		struct dect_base_link *bl = &b.main_link;
		struct dect_mm *mm = &bl->link.mm;
		static struct dect_base_ops rops;
		const enum dect_mm_state st0 = mm->state;
		const uint32_t af0 = mm->stats.auth_fail;
		const uint8_t tries0 = b.prof[b.cur].reg_tries_0000_first;

		rops = ops;
		rops.registration_rejected = rejected_cb;
		b.ops = &rops;
		rejected_calls = 0;
		bl->reported_state = DECT_MM_IDLE;
		mm->state = DECT_MM_FAILED;
		mm->stats.auth_fail = 1;
		b.prof[b.cur].reg_tries_0000_first = 0;
		dect_base_main_enrolment(&b);
		CHECK_EQ(rejected_calls, 1u);
		CHECK_EQ(rejected_pin, 1);		/* a wrong PIN */
		CHECK_EQ(rejected_expected, 0);
		dect_base_main_enrolment(&b);
		CHECK_EQ(rejected_calls, 1u);		/* once per outcome */
		/* the model that tries 0000 first: expected */
		bl->reported_state = DECT_MM_IDLE;
		b.prof[b.cur].reg_tries_0000_first = 1;
		dect_base_main_enrolment(&b);
		CHECK_EQ(rejected_calls, 2u);
		CHECK_EQ(rejected_pin, 1);
		CHECK_EQ(rejected_expected, 1);
		/* a failure that was not the PIN */
		bl->reported_state = DECT_MM_IDLE;
		mm->stats.auth_fail = 0;
		dect_base_main_enrolment(&b);
		CHECK_EQ(rejected_calls, 3u);
		CHECK_EQ(rejected_pin, 0);
		CHECK_EQ(rejected_expected, 0);
		mm->state = st0;
		mm->stats.auth_fail = af0;
		b.prof[b.cur].reg_tries_0000_first = tries0;
		bl->reported_state = st0;
		b.ops = &ops;
	}

	/* the ring's sequencing (S3n) */
	{
		struct dect_base_ring *r = &b.ring;

		dect_base_init(&b);
		b.ops = &ops;
		b.n_hs = 1;
		b.rec.slot = 1;
		b.rec.cc_fixed_id_len = 5;
		CHECK_EQ(dect_base_link_use(&b.main_link.link), 0);
		n_sent = 0;
		dect_base_ring_tick(&b, 1000);		/* nothing asked */
		CHECK_EQ(n_sent, 0u);
		r->now = 1;				/* a call-in */
		dect_base_ring_tick(&b, 1000);
		CHECK_EQ(r->state, DECT_BASE_RING_PAGING);
		CHECK_EQ(r->pages, 1u);
		CHECK_EQ(n_sent, 1u);
		CHECK_EQ(sent[0].prim, 0x0300);
		dect_base_ring_tick(&b, 1000 + DECT_BASE_PAGE_INTERVAL_MS - 1);
		CHECK_EQ(n_sent, 1u);			/* not yet */
		dect_base_ring_tick(&b, 1000 + DECT_BASE_PAGE_INTERVAL_MS);
		CHECK_EQ(n_sent, 2u);
		r->pages = DECT_BASE_PAGE_RETRIES;	/* unanswered, no link */
		dect_base_ring_tick(&b, 100000);
		CHECK_EQ(r->state, DECT_BASE_RING_DONE);
		CHECK_EQ(n_sent, 2u);

		/* a linked handset: released first, then paged */
		memset(r, 0, sizeof(*r));
		r->now = 1;
		b.main_link.active = 1;
		pumps = 0;
		n_sent = 0;
		dect_base_ring_tick(&b, 2000);
		CHECK_EQ(r->state, DECT_BASE_RING_RELEASING);
		CHECK_EQ(pumps, 1u);
		CHECK_EQ(n_sent, 0u);
		dect_base_ring_tick(&b, 2000 + DECT_BASE_RELEASE_WAIT_MS - 1);
		CHECK_EQ(n_sent, 0u);			/* still waiting */
		b.main_link.active = 0;			/* the chip: it is down */
		dect_base_ring_tick(&b, 2001);
		CHECK_EQ(r->state, DECT_BASE_RING_PAGING);
		CHECK_EQ(n_sent, 1u);

		/* a voice call on the main link: busy, nothing released */
		memset(r, 0, sizeof(*r));
		r->now = 1;
		b.main_link.active = 1;
		b.main_link.link.cc.state = DECT_CC_ST_ACTIVE;
		CHECK_EQ(dect_base_link_use(&b.main_link.link), 2);
		pumps = 0;
		dect_base_ring_tick(&b, 3000);
		CHECK_EQ(r->state, DECT_BASE_RING_DONE);
		CHECK_EQ(pumps, 0u);
		b.main_link.link.cc.state = DECT_CC_ST_NULL;

		/* no identities to ring with */
		memset(r, 0, sizeof(*r));
		r->now = 1;
		b.main_link.active = 0;
		b.rec.cc_fixed_id_len = 0;
		CHECK_EQ(dect_base_ring_fixed_len(&b), 0u);
		dect_base_ring_tick(&b, 4000);
		CHECK_EQ(r->state, DECT_BASE_RING_DONE);
		b.rec.cc_fixed_id_len = 5;

		/* ring= counts from the host's start */
		memset(r, 0, sizeof(*r));
		b.cfg.ring_ms = 5000;
		b.start_ms = 1000;
		n_sent = 0;
		dect_base_ring_tick(&b, 5999);
		CHECK_EQ(n_sent, 0u);
		dect_base_ring_tick(&b, 6000);
		CHECK_EQ(n_sent, 1u);
		b.cfg.ring_ms = 0;

		/* a queued SETUP lost to the handset's crossing call: again */
		memset(r, 0, sizeof(*r));
		r->now = 1;
		r->state = DECT_BASE_RING_DONE;
		r->watch = 1;
		b.main_link.active = 1;
		b.main_link.link.cc.stats.collisions = 1;
		dect_base_ring_watch(&b);
		CHECK_EQ(r->state, DECT_BASE_RING_IDLE);
		CHECK_EQ(r->requeues, 1u);
		CHECK_EQ(r->watch, 0);
		r->state = DECT_BASE_RING_DONE;		/* ... and it alerts */
		r->watch = 1;
		r->alert0 = 0;
		b.main_link.link.cc.stats.alerting = 1;
		dect_base_ring_watch(&b);
		CHECK_EQ(r->watch, 0);
		CHECK_EQ(r->requeues, 1u);
	}

	/* the calls out (S3o) */
	{
		dect_base_init(&b);
		b.ops = &ops;
		b.next_call = 126;
		b.calls[0].id = 127;			/* taken */
		CHECK_EQ(dect_base_call_new_id(&b), 1u);	/* wraps past it */
		CHECK_EQ(dect_base_call_new_id(&b), 2u);
		b.n_hs = 2;
		b.cur = 1;
		b.main_link.link.mm.adopted = -1;
		b.side[0].adopted = 0;
		b.line2[1] = 1;
		CHECK_EQ(dect_base_party_line2(&b, -1), 1);	/* the current one */
		CHECK_EQ(dect_base_party_line2(&b, 0), 0);
		CHECK(dect_base_party_link(&b, -1) == &b.main_link);
		CHECK(dect_base_party_link(&b, 2) == &b.side[2]);
		CHECK_EQ(dect_base_call_out(&b, -1, 0), 0);	/* no bridge */
	}

	/* a dialled string with a character that is not a key never reaches the bridge (security review M-3) */
	{
		struct dect_cc *cc;
		struct dect_mm_cfg mc;
		static const char good[] = "0301234*#+";
		static const char evil[] = "100@other.example";

		dect_base_init(&b);
		b.ops = &ops;
		b.bridge = 1;
		memset(&mc, 0, sizeof(mc));
		dect_link_init(&b.main_link.link, 1, &mc);
		b.main_link.active = 1;
		cc = &b.main_link.link.cc;
		cc->state = DECT_CC_ST_PROCEEDING;
		memcpy(cc->keypad, evil, sizeof(evil) - 1);
		cc->keypad_len = sizeof(evil) - 1;
		event_line[0] = '\0';
		CHECK_EQ(dect_base_call_out(&b, -1, 0), 1);	/* answered: refused */
		CHECK_EQ(event_line[0], '\0');			/* no call-out went out */
		memcpy(cc->keypad, good, sizeof(good) - 1);
		cc->keypad_len = sizeof(good) - 1;
		cc->state = DECT_CC_ST_PROCEEDING;
		CHECK_EQ(dect_base_call_out(&b, -1, 0), 1);
		CHECK(strstr(event_line, "call-out") != NULL);
	}

	/* a handset's number (S3p) */
	{
		struct dect_cc cc;

		memset(&cc, 0, sizeof(cc));
		cc.state = DECT_CC_ST_PROCEEDING;
		cc.peer_basic_service = 0x80;		/* normal call */
		CHECK_EQ(dect_base_number_complete(&cc), 1);
		cc.incoming = 1;
		CHECK_EQ(dect_base_number_complete(&cc), 0);
		cc.incoming = 0;
		cc.peer_basic_service = 0x20;		/* list access */
		CHECK_EQ(dect_base_number_complete(&cc), 0);
		dect_base_init(&b);
		b.cfg.auth_first = 1;
		CHECK_EQ(dect_base_call_auth_ok(&b, &b.main_link.link), 0);
		b.main_link.link.mm.authenticated = 1;
		CHECK_EQ(dect_base_call_auth_ok(&b, &b.main_link.link), 1);
		b.main_link.link.mm.authenticated = 0;
		b.cfg.no_cipher = 1;
		CHECK_EQ(dect_base_call_auth_ok(&b, &b.main_link.link), 1);
		/* nothing staged, no link: nothing to do */
		b.main_link.connect_staged = 1;
		dect_base_connect_tick(&b, 0);
		CHECK_EQ(b.main_link.connect_staged, 0);
		CHECK_EQ(dect_base_dial_tick(&b, -1, 0), 0);
	}

	/* the internal call (S3q) */
	{
		char nm[40];

		dect_base_init(&b);
		b.ops = &ops;
		b.n_hs = 2;
		b.hs[0].slot = 1;
		b.hs[1].slot = 2;
		b.rec = b.hs[0];
		dect_base_hs_name(&b, 1, nm, sizeof(nm));
		CHECK_EQ(strcmp(nm, "DECT 2"), 0);
		snprintf(b.name[1], sizeof(b.name[1]), "Kitchen");
		dect_base_hs_name(&b, 1, nm, sizeof(nm));
		CHECK_EQ(strcmp(nm, "Kitchen"), 0);
		CHECK_EQ(dect_base_party_active(&b, -1), 0);
		b.side[1].active = 1;
		CHECK_EQ(dect_base_party_active(&b, 1), 1);
		CHECK_EQ(dect_base_party_active(&b, DECT_BASE_IC_NONE), 0);
		b.main_link.link.cc.peer_basic_service = 0x80;
		CHECK_EQ(dect_base_ic_start(&b, -1, 0), 0);	/* not internal */
		b.main_link.link.cc.peer_basic_service = 0x90;
		CHECK_EQ(dect_base_ic_start(&b, -1, 0), 1);	/* no twocalls */
		CHECK_EQ(b.ic.st, DECT_BASE_IC_IDLE);
		b.cfg.two_calls = 1;
		CHECK_EQ(dect_base_ic_start(&b, -1, 0), 1);	/* slot 0: unknown */
		CHECK_EQ(b.ic.st, DECT_BASE_IC_IDLE);
		dect_base_ic_end(&b, "test");
		CHECK_EQ(b.ic.tside, DECT_BASE_IC_NONE);
	}

	/* calls in: the main link's ring, a side ring (S3r) */
	{
		struct dect_base_call *c;

		dect_base_init(&b);
		b.ops = &ops;
		b.n_hs = 2;
		b.hs[1].slot = 2;
		CHECK_EQ(dect_base_main_free(&b), 1);
		c = dect_base_call_new(&b);
		c->id = 5;
		dect_base_ring_main(&b, c, 1);
		CHECK_EQ(c->party, -1);
		CHECK_EQ(b.ring.now, 1);
		CHECK_EQ(dect_base_main_free(&b), 0);	/* its call */
		b.ring.state = DECT_BASE_RING_PAGING;
		dect_base_call_clear(&b, c);
		CHECK_EQ(c->id, 0u);
		CHECK_EQ(b.ring.now, 0);
		CHECK_EQ(b.ring.state, DECT_BASE_RING_DONE);
		CHECK_EQ(dect_base_main_free(&b), 1);
		c = dect_base_call_new(&b);
		c->id = 6;
		dect_base_side_ring_start(&b, c, 1, 100);
		CHECK_EQ(c->party, DECT_BASE_PARTY_PAGING);
		CHECK_EQ(c->tpui[2], 0x02);
		b.main_link.active = 1;			/* the main link busy */
		b.main_link.link.cc.state = DECT_CC_ST_ACTIVE;
		n_sent = 0;
		dect_base_side_ring_tick(&b, c, 100);
		CHECK_EQ(n_sent, 1u);
		CHECK_EQ(sent[0].prim, 0x0300);
		CHECK_EQ(c->pages, 1u);
		c->pages = DECT_BASE_IC_PAGES;		/* unanswered */
		dect_base_side_ring_tick(&b, c, 100 + DECT_BASE_IC_PAGE_MS);
		CHECK_EQ(c->id, 0u);
		/* infoie= wins over the profile's display text */
		b.cfg.info_ie_len = 2;
		CHECK_EQ(dect_base_name_info(&b, &b.main_link.link.cc, 1, &b.clip),
			 0);
	}

	/*
	 * A handset a side ring pages answers on the main link while the main
	 * ring waits for another handset that never answers (7412, 2026-10-31):
	 * the answer must not be rejected, the answering handset takes the main
	 * ring and the other call keeps paging as a side ring.
	 */
	{
		struct dect_base_call *c1, *c2;
		struct dect_link_ind ind;
		static const uint8_t id1[5] = { 0x01, 0x11, 0x11, 0x00, 0x01 };
		static const uint8_t id2[5] = { 0x01, 0x22, 0x22, 0x00, 0x02 };

		dect_base_init(&b);
		b.ops = &ops;
		b.n_hs = 2;
		b.hs[0].slot = 1;
		b.hs[1].slot = 3;
		b.cur = 0;
		memcpy(b.main_link.link.mm.held[0].id, id1, sizeof(id1));
		b.main_link.link.mm.held[0].id_len = sizeof(id1);
		b.main_link.link.mm.held[0].tag = 0;
		memcpy(b.main_link.link.mm.held[1].id, id2, sizeof(id2));
		b.main_link.link.mm.held[1].id_len = sizeof(id2);
		b.main_link.link.mm.held[1].tag = 1;
		b.main_link.link.mm.n_held = 2;
		memcpy(b.rec.cc_port_id, id1, sizeof(id1));
		b.rec.cc_port_id_len = sizeof(id1);
		memcpy(b.hs[1].cc_port_id, id2, sizeof(id2));
		b.hs[1].cc_port_id_len = sizeof(id2);
		c1 = dect_base_call_new(&b);
		c1->id = 9;
		dect_base_ring_main(&b, c1, 0);
		b.ring.state = DECT_BASE_RING_WAIT_PAGE_RESP;
		c2 = dect_base_call_new(&b);
		c2->id = 10;
		dect_base_side_ring_start(&b, c2, 1, 100);
		memset(&ind, 0, sizeof(ind));
		ind.page_response = 1;
		memcpy(ind.page_id, id2, sizeof(id2));
		ind.page_id_len = sizeof(id2);
		n_sent = 0;
		dect_base_main_page_response(&b, &ind, 200);
		CHECK_EQ(c2->party, -1);		/* it took the main ring */
		CHECK_EQ(c1->party, DECT_BASE_PARTY_PAGING);
		CHECK_EQ(c1->target, 0);
		CHECK_EQ(b.ring.hs, 1u);
		CHECK_EQ(b.cur, 1u);
	}

	/*
	 * A record can have a Contact List of its own: the handset it serves reads
	 * that one, the others the base's (a controller keeps one per owner).
	 */
	{
		static struct dect_contacts base_list, own_list;
		struct dect_iwu iwu;

		dect_contacts_init(&base_list);
		dect_contacts_init(&own_list);
		dect_base_init(&b);
		b.n_hs = 2;
		b.hs[0].slot = 1;
		b.hs[1].slot = 3;
		b.cur = 0;
		b.rec.slot = 1;
		b.contacts = &base_list;
		memset(&iwu, 0, sizeof(iwu));
		dect_base_iwu_lines(&b, &iwu, 3);
		CHECK(iwu.contacts == &base_list);
		dect_base_record_contacts(&b, 1, &own_list);
		dect_base_iwu_lines(&b, &iwu, 3);
		CHECK(iwu.contacts == &own_list);		/* the record's own */
		dect_base_iwu_lines(&b, &iwu, 1);
		CHECK(iwu.contacts == &base_list);		/* another record: the base's */
		dect_base_record_contacts(&b, 1, NULL);
		dect_base_iwu_lines(&b, &iwu, 3);
		CHECK(iwu.contacts == &base_list);
		dect_base_record_contacts(&b, DECT_BASE_HS_MAX, &own_list);	/* out of range: ignored */
	}

	/*
	 * A page response nobody waits for (the call that paged the handset was
	 * released a moment before; 7412, 2026-11-02) is rejected and its link
	 * released, not left for the handset's own timers (about two minutes,
	 * and the 06.88 chip has one lane per slot class). A response while a
	 * call still pages (a side ring) is left to the side-ring code.
	 */
	{
		struct dect_link_ind ind;
		struct dect_base_call *c;
		struct dect_mm_cfg cfg;
		static const uint8_t id1[5] = { 0x01, 0x11, 0x11, 0x00, 0x01 };

		dect_base_init(&b);
		b.ops = &ops;
		b.n_hs = 1;
		b.hs[0].slot = 1;
		b.cur = 0;
		memset(&cfg, 0, sizeof(cfg));
		dect_link_init(&b.main_link.link, 1, &cfg);
		memset(&ind, 0, sizeof(ind));
		ind.page_response = 1;
		memcpy(ind.page_id, id1, sizeof(id1));
		ind.page_id_len = sizeof(id1);
		dect_base_main_page_response(&b, &ind, 200);
		CHECK_EQ(ind.page_response, 0);		/* answered with a reject */

		c = dect_base_call_new(&b);
		c->id = 9;
		c->party = DECT_BASE_PARTY_PAGING;
		c->target = 0;
		memset(&ind, 0, sizeof(ind));
		ind.page_response = 1;
		memcpy(ind.page_id, id1, sizeof(id1));
		ind.page_id_len = sizeof(id1);
		dect_base_main_page_response(&b, &ind, 300);
		CHECK_EQ(ind.page_response, 1);		/* a side ring may want it */

		/* the ring of the call that paged it has ended (DONE), not idle */
		c->id = 0;
		b.ring.state = DECT_BASE_RING_DONE;
		memset(&ind, 0, sizeof(ind));
		ind.page_response = 1;
		memcpy(ind.page_id, id1, sizeof(id1));
		ind.page_id_len = sizeof(id1);
		dect_base_main_page_response(&b, &ind, 400);
		CHECK_EQ(ind.page_response, 0);
	}

	/*
	 * More records than the chip has slots (docs/more-than-six.md): record
	 * numbers go to DECT_HS_MAX, the verbs take them, the MM can hold their
	 * identities, and nothing about a record above the chip's six is written
	 * into the chip.
	 */
	{
		struct dect_ctl_msg m;
		struct dect_base_reply r;
		unsigned k;
		const uint8_t tp[3] = { 0x00, 0x11, 0x50 };

		CHECK(DECT_BASE_HS_MAX > DECT_MM_MAX_SLOT);
		dect_base_init(&b);
		b.ops = &ops;
		b.n_hs = DECT_BASE_HS_MAX;
		for (k = 0; k < b.n_hs; k++) {
			b.hs[k].slot = k + 1u;
			b.loaded[k] = 1;
			b.hs[k].uak_valid = 1;
		}
		b.cur = 11;
		b.rec.slot = 12;
		CHECK_EQ(dect_base_slot(&b, 11), 12u);
		CHECK_EQ(dect_base_slot(&b, DECT_BASE_HS_MAX - 1u), (unsigned)DECT_BASE_HS_MAX);
		for (k = 0; k < DECT_BASE_HS_MAX; k++) {
			uint8_t id[5] = { 0x01, 0x11, 0x11, 0x00, (uint8_t)k };

			CHECK_EQ(dect_mm_hold(&b.main_link.link.mm, id, sizeof(id),
					      (uint8_t)(k + 1u), tp, (int)k), 0);
		}
		CHECK_EQ(dect_mm_hold(&b.main_link.link.mm, (const uint8_t *)"\x01\x02", 2,
				      (uint8_t)(DECT_BASE_HS_MAX + 1u), tp, 0), -1);
		CHECK_EQ(dect_ctl_parse("mwi hs=12 line=1 count=2", strlen("mwi hs=12 line=1 count=2"), &m), 0);
		memset(&r, 0, sizeof(r));
		(void)dect_base_ctl_call(&b, &m, 1, 0, &r);
		CHECK(!r.err || (strcmp(r.err, "malformed") != 0 && strcmp(r.err, "bad-hs") != 0));
		/* the chip is not touched for a record above its slots */
		n_sent = 0;
		CHECK_EQ(dect_base_slot_install(&b, DECT_MM_MAX_SLOT + 1u, b.hs[11].uak), -1);
	}

	/*
	 * Dropping a record (API 146): a roamer that stopped renewing leaves
	 * the table, and its index is free again.
	 */
	{
		unsigned k;

		dect_base_init(&b);
		b.ops = &ops;
		b.n_hs = 5;
		for (k = 0; k < 4; k++) {
			b.hs[k].slot = k + 1u;
			b.loaded[k] = 1;
			b.hs[k].uak_valid = 1;
			b.hs[k].port_id[0] = (uint8_t)(0x10 + k);
			b.hs[k].port_id_len = 1;
		}
		b.absent[4] = 1;	/* a free store: not a record to drop */
		b.cur = 0;
		b.rec = b.hs[0];
		CHECK_EQ(dect_base_quiet(&b), 1);
		strcpy(b.name[2], "Roamer");
		b.page_want = 1u << 2;
		CHECK_EQ(dect_base_record_drop(&b, 2), 0);
		CHECK_EQ(b.loaded[2], 0);
		CHECK_EQ(b.hs[2].port_id_len, 0u);
		CHECK_EQ(b.name[2][0], 0);
		CHECK_EQ(b.page_want, 0u);
		CHECK_EQ(dect_base_listed(&b, 2), 0);
		CHECK_EQ(dect_base_listed(&b, 3), 1);
		CHECK_EQ(b.n_hs, 5u);	/* the free store at 4 keeps the table's end */
		CHECK_EQ(dect_base_record_drop(&b, 2), -1);	/* already gone */
		CHECK_EQ(dect_base_record_drop(&b, 4), -1);	/* a free store */
		CHECK_EQ(dect_base_record_drop(&b, 9), -1);

		/* not while anything uses the table's indices */
		b.main_link.active = 1;
		CHECK_EQ(dect_base_quiet(&b), 0);
		CHECK_EQ(dect_base_record_drop(&b, 3), -1);
		CHECK_EQ(b.loaded[3], 1);
		b.main_link.active = 0;
		b.side[0].active = 1;
		CHECK_EQ(dect_base_record_drop(&b, 3), -1);
		b.side[0].active = 0;
		b.calls[0].id = 7;
		CHECK_EQ(dect_base_record_drop(&b, 3), -1);
		b.calls[0].id = 0;
		b.ring.state = DECT_BASE_RING_PAGING;
		CHECK_EQ(dect_base_record_drop(&b, 3), -1);
		b.ring.state = DECT_BASE_RING_IDLE;
		b.dereg.st = DECT_BASE_DEREG_PAGING;
		CHECK_EQ(dect_base_record_drop(&b, 3), -1);
		b.dereg.st = DECT_BASE_DEREG_IDLE;

		/* the current record moves off first, its working copy is not lost for the others */
		b.cur = 3;
		b.rec = b.hs[3];
		b.main_link.link.mm.adopted = 3;
		CHECK_EQ(dect_base_record_drop(&b, 3), 0);
		CHECK(b.cur != 3u && b.loaded[b.cur]);
		CHECK_EQ(b.main_link.link.mm.adopted, -1);
		CHECK_EQ(b.rec.port_id[0], b.hs[b.cur].port_id[0]);

		/* the table shrinks to the last record in use, never below the current one */
		b.absent[4] = 0;
		b.cur = 0;
		b.rec = b.hs[0];
		CHECK_EQ(dect_base_record_drop(&b, 1), 0);
		CHECK_EQ(b.n_hs, 1u);
		CHECK_EQ(dect_base_record_drop(&b, 0), -1);	/* the only record */
	}

	/*
	 * Recycling a record (API 148): a pooled handset enrolled into a chip
	 * slot and leaves it; the slot is cleared, the record is a free store again.
	 */
	{
		static struct dect_base_ops rops;
		static const uint8_t newtp[3] = { 0x00, 0x11, 0x2A };
		unsigned k;

		rops = ops;
		rops.mask = count_mask;
		dect_base_init(&b);
		b.ops = &rops;
		b.n_hs = 4;
		for (k = 0; k < 3; k++) {
			b.hs[k].slot = k + 1u;
			b.loaded[k] = 1;
			b.hs[k].uak_valid = 1;
			b.hs[k].uak[0] = (uint8_t)(0x40 + k);
			b.hs[k].port_id[0] = (uint8_t)(0x10 + k);
			b.hs[k].port_id_len = 1;
		}
		b.hs[3].slot = 4;
		b.hs[3].tpui[2] = 7;
		b.absent[3] = 1;		/* a free store */
		b.hs[8].slot = 9;		/* a host-only record: nothing in the chip */
		b.cur = 0;
		b.rec = b.hs[0];
		strcpy(b.name[1], "Pooled");
		b.page_want = 1u << 1;

		/* not a record, a free store, a record without a chip slot */
		CHECK_EQ(dect_base_record_recycle(&b, 3, newtp), -1);
		CHECK_EQ(dect_base_record_recycle(&b, 9, newtp), -1);
		b.n_hs = 9;
		b.loaded[8] = 1;
		CHECK_EQ(dect_base_record_recycle(&b, 8, newtp), -1);
		b.loaded[8] = 0;
		b.n_hs = 4;

		/* not while anything uses the table's indices */
		b.main_link.active = 1;
		n_sent = 0;
		CHECK_EQ(dect_base_record_recycle(&b, 1, newtp), 1);
		CHECK_EQ(n_sent, 0u);
		CHECK_EQ(b.loaded[1], 1);
		b.main_link.active = 0;

		/* a quiet base: chip slot 2 cleared, the record free with its slot and the new TPUI */
		mask_calls = 0;
		CHECK_EQ(dect_base_record_recycle(&b, 1, newtp), 0);
		CHECK_EQ(n_sent, 2u);
		CHECK_EQ(sent[0].prim, 0x0247u);
		CHECK_EQ(sent[0].param[0], 2);
		CHECK_EQ(sent[0].param[1], 0);
		CHECK_EQ(sent[1].prim, 0x020Au);
		CHECK_EQ(sent[1].param[0], 2);
		CHECK_EQ(sent[1].param[1], 0);		/* the key written as zeros */
		CHECK_EQ(mask_calls, 1u);
		CHECK_EQ(b.loaded[1], 0);
		CHECK_EQ(b.absent[1], 1);
		CHECK_EQ(b.hs[1].slot, 2u);
		CHECK_EQ(b.hs[1].tpui[2], 0x2Au);
		CHECK_EQ(b.hs[1].uak_valid, 0);
		CHECK_EQ(b.hs[1].port_id_len, 0u);
		CHECK_EQ(b.name[1][0], 0);
		CHECK_EQ(b.page_want, 0u);
		CHECK_EQ(b.n_hs, 4u);
		CHECK_EQ(b.loaded[2], 1);		/* the others are as they were */
		CHECK_EQ(b.hs[2].uak[0], 0x42);
		/* the free store with the lowest chip slot takes the next enrolment: it is this one */
		CHECK_EQ(dect_base_record_recycle(&b, 1, newtp), -1);	/* now a free store */

		/* the current record: it stays current, as a free store with the working copy cleared */
		b.cur = 2;
		b.rec = b.hs[2];
		b.main_link.link.mm.adopted = 2;
		n_sent = 0;
		CHECK_EQ(dect_base_record_recycle(&b, 2, newtp), 0);
		CHECK_EQ(b.cur, 2u);
		CHECK_EQ(b.rec.uak_valid, 0);
		CHECK_EQ(b.rec.slot, 3u);
		CHECK_EQ(b.rec.tpui[2], 0x2Au);
		CHECK_EQ(b.rec.port_id_len, 0u);
		CHECK_EQ(b.main_link.link.mm.adopted, -1);
		CHECK_EQ(sent[0].param[0], 3);
	}

	/* parallel calls (S3s) */
	{
		dect_base_init(&b);
		b.ops = &ops;
		b.n_hs = 2;
		b.cur = 1;
		b.main_link.link.mm.adopted = -1;
		b.side[0].adopted = 0;
		CHECK_EQ(dect_base_party_index(&b, -1), 1);
		CHECK_EQ(dect_base_party_index(&b, 0), 0);
		CHECK_EQ(dect_base_party_index(&b, DECT_BASE_PARTY_NONE), -1);
		CHECK_EQ(dect_base_hs_in_call(&b, 1), 0);
		CHECK_EQ(dect_base_waiting_party(&b, 1), DECT_BASE_PARTY_NONE);
		b.main_link.active = 1;
		b.main_link.link.cc.state = DECT_CC_ST_ACTIVE;
		CHECK_EQ(dect_base_hs_in_call(&b, 1), 1);
		CHECK_EQ(dect_base_waiting_party(&b, 1), DECT_BASE_PARTY_NONE);
	}

	/* the bridged calls' progress (S3t) */
	{
		struct dect_base_call *c;

		dect_base_init(&b);
		b.ops = &ops;
		c = dect_base_call_new(&b);
		c->id = 7;
		c->party = -1;
		c->since = 1000;
		b.ring.state = DECT_BASE_RING_DONE;	/* the ring gave up */
		event_line[0] = '\0';
		dect_base_bridge_tick(&b, 5000);
		CHECK_EQ(c->id, 7u);			/* not yet */
		dect_base_bridge_tick(&b, 6001);
		CHECK_EQ(c->id, 0u);
		CHECK_EQ(strcmp(event_line, "released call=7 hs=1 by=base\n"),
			 0);
	}

	/* the call verbs (S5a) */
	{
		struct dect_ctl_msg m;
		struct dect_base_reply r;

		dect_base_init(&b);
		b.ops = &ops;
		b.n_hs = 1;
		b.hs[0].slot = 1;
		b.loaded[0] = 1;
		CHECK_EQ(dect_base_listed(&b, 0), 1);
		b.absent[0] = 1;
		CHECK_EQ(dect_base_listed(&b, 0), 0);
		b.absent[0] = 0;
		CHECK_EQ(dect_base_contact_name(&b, "123", nm_buf, sizeof(nm_buf)),
			 0);			/* no Contact List */
		CHECK_EQ(dect_ctl_parse("status", strlen("status"), &m), 0);
		CHECK_EQ(dect_base_ctl_call(&b, &m, 1, 0, &r), 0);
		CHECK_EQ(dect_ctl_parse("remote-ringing call=3", strlen("remote-ringing call=3"), &m), 0);
		CHECK_EQ(dect_base_ctl_call(&b, &m, 0, 0, &r), 1);
		CHECK_EQ(strcmp(r.err, "not-bridge"), 0);
		CHECK_EQ(dect_base_ctl_call(&b, &m, 1, 0, &r), 1);
		CHECK_EQ(strcmp(r.err, "no-call"), 0);
		CHECK_EQ(dect_ctl_parse("call-in hs=4", strlen("call-in hs=4"), &m), 0);
		CHECK_EQ(dect_base_ctl_call(&b, &m, 1, 0, &r), 1);
		CHECK_EQ(strcmp(r.err, "no-handset"), 0);
		CHECK_EQ(r.val, 4u);
		CHECK_EQ(dect_ctl_parse("call-in hs=1 number=0301234 sip=x", strlen("call-in hs=1 number=0301234 sip=x"), &m),
			 0);
		event_line[0] = '\0';
		CHECK_EQ(dect_base_ctl_call(&b, &m, 1, 0, &r), 1);
		CHECK_EQ(r.sent, 1);
		CHECK_EQ(strcmp(event_line, "call-in-ok call=1 sip=x\n"), 0);
		CHECK_EQ(b.ring.now, 1);		/* the main link rings */
	}

	/* an internal call-in (API 153): its number is the caller's internal one, vouched for */
	{
		struct dect_ctl_msg m;
		struct dect_base_reply r;
		const char *line = "call-in hs=1 number=201 name=Buero int=1 sip=y";

		dect_base_init(&b);
		b.ops = &ops;
		b.n_hs = 1;
		b.hs[0].slot = 1;
		b.loaded[0] = 1;
		CHECK_EQ(dect_ctl_parse(line, strlen(line), &m), 0);
		CHECK_EQ(dect_base_ctl_call(&b, &m, 1, 0, &r), 1);
		CHECK_EQ(r.sent, 1);
		CHECK_EQ(b.calls[0].intern, 1);
		CHECK_EQ(strcmp(b.calls[0].clip.number, "201"), 0);
		CHECK_EQ(b.calls[0].clip.number_ton, (unsigned)DECT_CLIP_TON_NETWORK_SPECIFIC);
		CHECK_EQ(strcmp(b.calls[0].clip.name, "Buero"), 0);
		/* a second one for the handset that is rung is busy, never "waiting" */
		b.cur = 0;
		CHECK_EQ(dect_base_hs_in_call(&b, 0), 1);
		event_line[0] = '\0';
		line = "call-in hs=1 number=202 int=1 sip=z";
		CHECK_EQ(dect_ctl_parse(line, strlen(line), &m), 0);
		CHECK_EQ(dect_base_ctl_call(&b, &m, 1, 0, &r), 1);
		CHECK(strstr(event_line, "busy") != NULL);
		CHECK(strstr(event_line, "in-call") != NULL);
	}

	/* notifications (S4a) */
	{
		dect_base_init(&b);
		b.ops = &ops;
		b.n_hs = 2;
		b.cur = 0;
		b.line2[1] = 1;
		CHECK_EQ(dect_base_hs_lines(&b, 0), 1);
		CHECK_EQ(dect_base_hs_lines(&b, 1), 3);
		b.main_link.link.mm.adopted = -1;
		CHECK_EQ(dect_base_hs_of_link(&b, &b.main_link.link), 0);
		b.side[0].link.mm.adopted = -1;
		CHECK_EQ(dect_base_hs_of_link(&b, &b.side[0].link), -1);
		CHECK(dect_base_link_known(&b, 0) == NULL);	/* not adopted */
		b.side[1].active = 1;
		b.side[1].link.mm.adopted = 1;
		CHECK(dect_base_link_known(&b, 1) == &b.side[1].link);
		CHECK_EQ(dect_base_notify_known(&b, 1), 0);
		b.cap[1].known = 1;
		b.cap[1].part3 = 1;
		CHECK_EQ(dect_base_notify_known(&b, 1), 1);
		/* not loaded: nothing due; loaded, no call lists: none sent */
		dect_base_missed_due(&b, 1, 3, 1);
		CHECK_EQ(b.note[1].mc_due, 0);
		b.loaded[1] = 1;
		b.side[1].active = 0;			/* no link: paged */
		dect_base_missed_due(&b, 1, 3, 1);
		CHECK_EQ(b.note[1].mc_due, 3);
		CHECK_EQ(b.note[1].mc_new, 3);
		CHECK_EQ(b.page_want, 2u);
		b.side[1].active = 1;		/* a link, but no call lists */
		dect_base_missed_due(&b, 1, 1, 0);
		CHECK_EQ(b.note[1].mc_due, 0);
	}

	/* deregistration (S4b) */
	{
		dect_base_init(&b);
		b.ops = &ops;
		b.n_hs = 2;
		b.hs[0].slot = 1;
		b.hs[1].slot = 2;
		b.loaded[0] = b.loaded[1] = 1;
		b.main_link.link.mm.adopted = -1;
		n_sent = 0;
		dect_base_dereg_begin(&b, 5, 0, "test");	/* nobody there */
		CHECK_EQ(b.dereg.st, DECT_BASE_DEREG_IDLE);
		dect_base_dereg_begin(&b, 2, 0, "test");	/* no link: page */
		CHECK_EQ(b.dereg.st, DECT_BASE_DEREG_PAGING);
		dect_base_dereg_begin(&b, 1, 0, "test");	/* waits */
		CHECK_EQ(b.dereg.nq, 1u);
		dect_base_dereg_tick(&b, 0);
		CHECK_EQ(n_sent, 1u);
		CHECK_EQ(sent[0].prim, 0x0300);
		CHECK_EQ(sent[0].param[0], 0x08);
		b.dereg.pages = DECT_BASE_DEREG_PAGES;	/* unreachable */
		dect_base_dereg_tick(&b, DECT_BASE_DEREG_PAGE_MS);
		CHECK_EQ(b.loaded[1], 0);
		CHECK_EQ(b.absent[1], 1);
		CHECK_EQ(sent[1].prim, 0x0247);
		CHECK_EQ(sent[2].prim, 0x020A);
		dect_base_dereg_tick(&b, DECT_BASE_DEREG_PAGE_MS);
		CHECK_EQ(b.dereg.nq, 0u);		/* the queue's turn */
		CHECK_EQ(b.dereg.target, 0);
	}

	/* the date and time round (S4c): no trustworthy clock, no round */
	{
		dect_base_init(&b);
		b.ops = &ops;			/* no clock() */
		b.n_hs = 1;
		b.hs[0].slot = 1;
		b.loaded[0] = 1;
		b.cfg.time_every_min = 1;
		CHECK_EQ(dect_base_time_busy(&b), 0);
		b.ic.st = DECT_BASE_IC_PAGING;
		CHECK_EQ(dect_base_time_busy(&b), 1);
		b.ic.st = DECT_BASE_IC_IDLE;
		n_sent = 0;
		dect_base_time_tick(&b, 1000);
		CHECK_EQ(b.tu.next_round, 61000u);
		dect_base_time_tick(&b, 61000);
		CHECK_EQ(b.tu.in_round, 0);
		CHECK_EQ(n_sent, 0u);
		b.page_want = 1;		/* but a notice due: paged */
		dect_base_time_tick(&b, 62000);
		CHECK_EQ(b.tu.st, DECT_BASE_TIME_PAGING);
		dect_base_time_tick(&b, 62000);
		CHECK_EQ(n_sent, 1u);
		CHECK_EQ(sent[0].prim, 0x0300);
	}

	/* the lists' hooks (S4d) */
	{
		struct dect_iwu *iwu = &b.main_link.link.cc.iwu;

		dect_base_init(&b);
		b.ops = &ops;
		b.n_hs = 2;
		b.hs[0].slot = 1;
		b.hs[1].slot = 2;
		b.loaded[0] = b.loaded[1] = 1;
		b.main_link.active = 1;
		b.main_link.link.mm.adopted = -1;
		iwu->n_handsets = 2;
		iwu->handsets[1].terminal_id = 2;
		snprintf(iwu->handsets[1].name, sizeof(iwu->handsets[1].name),
			 "Hall");
		iwu->names_changed = 2;
		dect_base_lists_tick(&b, 1000);
		CHECK_EQ(strcmp(b.name[1], "Hall"), 0);
		CHECK_EQ(named_i, 1u);
		CHECK_EQ(iwu->names_changed, 0u);
		/* ten wrong PINs in the hour: no PIN for an hour */
		iwu->stats.pin_wrong = DECT_BASE_PIN_WRONG_MAX;
		dect_base_lists_tick(&b, 2000);
		CHECK_EQ(b.pin_wrong, DECT_BASE_PIN_WRONG_MAX);
		CHECK_EQ(b.pin_locked_until, 2000u + DECT_BASE_PIN_LOCK_MS);
		CHECK_EQ(iwu->pin_known, 0);
		/* a deletion from the list is a deregistration */
		iwu->delete_request = 2;
		dect_base_lists_tick(&b, 3000);
		CHECK_EQ(iwu->delete_request, 0u);
		CHECK(b.dereg.st != DECT_BASE_DEREG_IDLE);
		CHECK_EQ(b.dereg.target, 1);
	}

	/* mwi and calllog (S5b) */
	{
		struct dect_ctl_msg m;
		struct dect_base_reply r;

		dect_base_init(&b);
		b.ops = &ops;
		b.n_hs = 1;
		b.hs[0].slot = 1;
		b.loaded[0] = 1;
		CHECK_EQ(dect_ctl_parse("mwi hs=1 line=1 count=3",
				       strlen("mwi hs=1 line=1 count=3"), &m), 0);
		CHECK_EQ(dect_base_ctl_call(&b, &m, 0, 0, &r), 1);
		CHECK_EQ(strcmp(r.err, "not-bridge"), 0);
		CHECK_EQ(dect_base_ctl_call(&b, &m, 1, 0, &r), 1);
		CHECK(r.err == NULL);
		CHECK_EQ(b.note[0].mwi[0], 3u);
		CHECK_EQ(b.page_want, 1u);		/* no link: paged */
		CHECK_EQ(dect_ctl_parse("calllog kind=lost",
				       strlen("calllog kind=lost"), &m), 0);
		CHECK_EQ(dect_base_ctl_call(&b, &m, 1, 0, &r), 1);
		CHECK_EQ(strcmp(r.err, "malformed"), 0);
	}

	/* serving another record (S3w) */
	{
		dect_base_init(&b);
		b.ops = &ops;
		b.n_hs = 2;
		b.cc_id_foreign_warned = 1;
		CHECK_EQ(dect_base_serve(&b, 0, " (test)"), 0);	/* already */
		CHECK_EQ(dect_base_serve(&b, 1, " (test)"), 1);
		CHECK_EQ(b.cur, 1u);
		CHECK_EQ(strcmp(switched_why, " (test)"), 0);
		CHECK_EQ(b.cc_id_foreign_warned, 0);
		/* no RFPI yet: nothing to derive the identities from */
		b.rec.port_id_len = 7;
		dect_base_main_ids(&b, 0);
		CHECK_EQ(b.rec.cc_fixed_id_len, 0u);
	}

	/* page responses and lines (S3x) */
	{
		struct dect_link_ind ind;
		struct dect_iwu *iwu = &b.main_link.link.cc.iwu;

		dect_base_init(&b);
		b.ops = &ops;
		b.n_hs = 1;
		b.hs[0].slot = 1;
		b.line2[0] = 1;
		snprintf(b.line_name[0][1], sizeof(b.line_name[0][1]), "Work");
		dect_base_iwu_lines(&b, iwu, 1);
		CHECK_EQ(iwu->n_lines, 2u);
		CHECK_EQ(strcmp(iwu->lines[0].name, "Line 1"), 0);
		CHECK_EQ(strcmp(iwu->lines[1].name, "Work"), 0);
		memset(&ind, 0, sizeof(ind));
		ind.page_response = 1;
		/* not paging for any of these: nothing consumed */
		CHECK_EQ(dect_base_dereg_page_response(&b, -1, &ind), 0);
		CHECK_EQ(dect_base_time_page_response(&b, -1, &ind), 0);
		CHECK_EQ(dect_base_side_ring_answer(&b, 0, &ind, 0), 0);
		/* a page response with an identity MM does not hold */
		CHECK_EQ(dect_base_page_adopt(&b, 0, &ind, 0, ""), 0);
	}

	/* links set up, the registration window (S3z) */
	{
		dect_base_init(&b);
		b.ops = &ops;
		b.n_hs = 2;
		b.hs[0].slot = 1;
		b.hs[1].slot = 2;
		b.loaded[0] = 1;
		b.absent[1] = 1;
		CHECK_EQ(dect_base_reg_policy(&b), DECT_MM_REG_CLOSED);
		n_sent = 0;
		dect_base_reg_window(&b, 1, 60, 1000, "test");
		CHECK_EQ(sent[0].prim, 0x022B);
		CHECK_EQ(sent[0].param[0], 0x41);
		CHECK_EQ(b.reg_open_until, 61000u);
		CHECK_EQ(dect_base_reg_policy(&b), DECT_MM_REG_OPEN);	/* free */
		b.absent[1] = 0;
		b.rec.port_id_len = 7;
		CHECK_EQ(dect_base_reg_policy(&b), DECT_MM_REG_SAME);
		dect_base_reg_window(&b, 0, 0, 2000, "test");
		CHECK_EQ(sent[1].prim, 0x022C);
		b.main_link.active = 1;
		b.main_link.handle = 1;
		b.cfg.locate_validity = 6;
		CHECK_EQ(dect_base_side_start(&b, 2), 0);
		CHECK_EQ(b.side[0].active, 1);
		CHECK_EQ(b.side[0].link.mm.cfg.locate_validity, 6);	/* told to register again */
		CHECK_EQ(b.side[0].adopted, -1);
		CHECK_EQ(dect_base_side_start(&b, 2), 0);	/* repeated */
		CHECK_EQ(dect_base_side_start(&b, 3), 1);
		CHECK_EQ(dect_base_side_start(&b, 4), 2);
		CHECK_EQ(dect_base_side_start(&b, 5), -1);	/* none free */
	}

	/*
	 * Wrong PINs close the window (API 155): a PIN is not to be guessed at the radio's speed. The
	 * ones a model makes on purpose (PIN 0000 first) and a window that is closed do not count; a
	 * window opened again starts from none.
	 */
	{
		struct dect_base_link *bl;
		struct dect_mm *mm;
		unsigned i;

		dect_base_init(&b);
		b.ops = &ops;
		b.n_hs = 1;
		b.cur = 0;
		bl = &b.main_link;
		mm = &bl->link.mm;
		dect_base_reg_window(&b, 1, 120, 1000, "test");
		for (i = 0; i < DECT_BASE_REG_FAIL_MAX - 1; i++) {
			bl->reported_state = DECT_MM_IDLE;
			mm->state = DECT_MM_FAILED;
			mm->stats.auth_fail = 1;
			dect_base_main_enrolment(&b);
		}
		CHECK_EQ(b.reg_wrong, DECT_BASE_REG_FAIL_MAX - 1u);
		dect_base_tick(&b, 2000, 0);
		CHECK_EQ(b.reg_open, 1);		/* one short of the budget */
		/* a model that tries 0000 first: its attempt is not a guess */
		b.prof[0].reg_tries_0000_first = 1;
		bl->reported_state = DECT_MM_IDLE;
		mm->state = DECT_MM_FAILED;
		dect_base_main_enrolment(&b);
		CHECK_EQ(b.reg_wrong, DECT_BASE_REG_FAIL_MAX - 1u);
		b.prof[0].reg_tries_0000_first = 0;
		bl->reported_state = DECT_MM_IDLE;
		mm->state = DECT_MM_FAILED;
		dect_base_main_enrolment(&b);
		n_sent = 0;
		dect_base_tick(&b, 3000, 0);
		CHECK_EQ(b.reg_open, 0);		/* the budget is spent */
		CHECK(n_sent >= 1 && sent[0].prim == 0x022C);
		/* a window opened again starts over; a smaller budget can be set */
		dect_base_reg_window(&b, 1, 120, 4000, "test");
		CHECK_EQ(b.reg_wrong, 0u);
		b.cfg.reg_fail_max = 2;
		b.reg_wrong = 2;
		dect_base_tick(&b, 5000, 0);
		CHECK_EQ(b.reg_open, 0);
		/* failures with the window closed are not counted */
		b.reg_wrong = 0;
		bl->reported_state = DECT_MM_IDLE;
		mm->state = DECT_MM_FAILED;
		dect_base_main_enrolment(&b);
		CHECK_EQ(b.reg_wrong, 0u);
	}

	/* connections up and down (S6a) */
	{
		dect_base_init(&b);
		b.ops = &ops;
		b.n_hs = 1;
		b.hs[0].slot = 1;
		b.loaded[0] = 1;
		dect_base_conn_up(&b, 1, 0);
		CHECK_EQ(b.main_link.active, 1);
		CHECK_EQ(b.main_link.handle, 1);
		b.main_link.link.cc.state = DECT_CC_ST_ACTIVE;	/* busy */
		dect_base_conn_up(&b, 1, 0);		/* a repeat: kept */
		CHECK_EQ(b.main_link.link.cc.state, DECT_CC_ST_ACTIVE);
		dect_base_conn_up(&b, 2, 0);		/* another: a side link */
		CHECK_EQ(b.side[0].active, 1);
		CHECK_EQ(b.side[0].lane, -1);
		dect_base_conn_down(&b, 3, 1);		/* not ours: ignored */
		CHECK_EQ(b.main_link.active, 1);
		dect_base_conn_down(&b, 2, 1);
		CHECK_EQ(b.side[0].active, 0);
		CHECK_EQ(b.main_link.active, 1);
		dect_base_conn_down(&b, 1, 1);
		CHECK_EQ(b.main_link.active, 0);
		b.have_rfpi = 0;
		{
			struct dect_mail m;

			memset(&m, 0, sizeof(m));
			m.param_len = DECT_MM_RFPI_LEN;
			m.param[4] = 0x60;
			dect_base_contact(&b, &m);
			CHECK_EQ(b.have_rfpi, 1);
			CHECK_EQ(b.rfpi[4], 0x60);
		}
	}

	/* a segment with no link to take it (S6b) */
	{
		struct dect_mail m;

		dect_base_init(&b);
		b.ops = &ops;
		memset(&m, 0, sizeof(m));
		m.param_len = 1;
		m.param[0] = 4;
		CHECK_EQ(dect_base_segment(&b, &m, 0), -1);
	}

	/* one tick for a host with no steps of its own (S6c) */
	{
		dect_base_init(&b);
		b.ops = &ops;
		b.n_hs = 1;
		b.hs[0].slot = 1;
		b.rec = b.hs[0];
		b.rec.cc_fixed_id_len = 5;
		b.ring.now = 1;			/* a call-in */
		n_sent = 0;
		dect_base_tick(&b, 1000, 0);	/* the radio is off: no page */
		CHECK_EQ(n_sent, 0u);
		dect_base_tick(&b, 1000, 1);
		CHECK_EQ(n_sent, 1u);
		CHECK_EQ(sent[0].prim, 0x0300);
		b.ring.now = 0;
		b.ring.state = DECT_BASE_RING_DONE;
		b.main_link.active = 1;
		b.main_link.bound = 1;		/* a call that has ended */
		call_downs = 0;
		dect_base_tick(&b, 2000, 1);
		CHECK_EQ(call_downs, 1u);
		CHECK_EQ(b.main_link.bound, 0);
	}

	/* the administrator's verbs (S5c) */
	{
		struct dect_ctl_msg m;
		struct dect_base_reply r;
		const char *l1 = "rename hs=1 name=Hall";
		const char *l2 = "rename hs=1 name=\xc3";
		const char *l3 = "status";

		CHECK_EQ(dect_base_name_ok("Küche"), 1);
		CHECK_EQ(dect_base_name_ok("\xc3"), 0);	/* cut short */
		CHECK_EQ(dect_base_name_ok(""), 1);		/* the reset */
		dect_base_init(&b);
		b.ops = &ops;
		b.n_hs = 1;
		b.hs[0].slot = 1;
		b.loaded[0] = 1;
		CHECK_EQ(dect_ctl_parse(l1, strlen(l1), &m), 0);
		CHECK_EQ(dect_base_ctl_admin(&b, &m, 0, &r), 1);
		CHECK(r.err == NULL);
		CHECK_EQ(r.named, 0);
		CHECK_EQ(strcmp(b.name[0], "Hall"), 0);
		CHECK_EQ(dect_ctl_parse(l2, strlen(l2), &m), 0);
		CHECK_EQ(dect_base_ctl_admin(&b, &m, 0, &r), 1);
		CHECK_EQ(strcmp(r.err, "bad-name"), 0);
		CHECK_EQ(r.named, -1);
		CHECK_EQ(dect_ctl_parse(l3, strlen(l3), &m), 0);
		CHECK_EQ(dect_base_ctl_admin(&b, &m, 0, &r), 0);
	}
	/*
	 * The Internal Names List per viewer, and read-only (API 138): a
	 * handset sees its account's handsets, never another's; a base with no
	 * name_visible() shows all.
	 */
	{
		static struct dect_base_ops vops;
		struct dect_iwu iwu;
		unsigned t;

		dect_base_init(&b);
		vops.name_visible = same_account;
		b.n_hs = 3;
		b.hs[0].slot = 1;
		b.hs[1].slot = 2;
		b.hs[2].slot = 3;
		dect_iwu_init(&iwu);
		for (t = 1; t <= 3; t++)
			CHECK_EQ(dect_iwu_add_handset(&iwu, (uint8_t)t, "x"), 0);
		dect_iwu_set_own(&iwu, 1);

		/* no host callback: everything, editable */
		dect_base_iwu_lines(&b, &iwu, 1);
		CHECK_EQ(iwu.n_handsets, 3u);
		CHECK_EQ(iwu.names_readonly, 0u);

		/* slot 1 sees 1 and 2 only */
		b.ops = &vops;
		dect_base_iwu_lines(&b, &iwu, 1);
		CHECK_EQ(iwu.n_handsets, 2u);
		CHECK_EQ(iwu.handsets[0].terminal_id, 1u);
		CHECK_EQ(iwu.handsets[1].terminal_id, 2u);

		/* slot 3 sees itself only, even from a full list */
		for (t = 1; t <= 3; t++)
			(void)dect_iwu_add_handset(&iwu, (uint8_t)t, "x");
		dect_iwu_set_own(&iwu, 3);
		dect_base_iwu_lines(&b, &iwu, 3);
		CHECK_EQ(iwu.n_handsets, 1u);
		CHECK_EQ(iwu.handsets[0].terminal_id, 3u);

		/* the switch for read-only goes to the link's list */
		b.cfg.names_readonly = 1;
		dect_base_iwu_lines(&b, &iwu, 3);
		CHECK_EQ(iwu.names_readonly, 1u);

		/* a spread entry reaches only the links whose viewer may see it */
		b.main_link.active = 1;
		dect_iwu_init(&b.main_link.link.cc.iwu);
		dect_iwu_set_own(&b.main_link.link.cc.iwu, 1);
		dect_base_names_spread(&b, 1);		/* slot 2: same account */
		CHECK_EQ(b.main_link.link.cc.iwu.n_handsets, 1u);
		dect_base_names_spread(&b, 2);		/* slot 3: not visible */
		CHECK_EQ(b.main_link.link.cc.iwu.n_handsets, 1u);
		dect_iwu_set_own(&b.main_link.link.cc.iwu, 3);
		dect_base_names_spread(&b, 2);
		CHECK_EQ(b.main_link.link.cc.iwu.n_handsets, 2u);
		CHECK(vis_calls > 0);
	}
	/*
	 * The remote directory (API 152): handsets on other bases are entries
	 * of a link's Internal Names List, filtered like local ones, and they
	 * come and go on the links that are up.
	 */
	{
		static struct dect_base_ops vops;
		struct dect_iwu iwu;

		dect_base_init(&b);
		b.n_hs = 1;
		b.hs[0].slot = 1;
		dect_iwu_init(&iwu);
		(void)dect_iwu_add_handset(&iwu, 1, "x");
		dect_iwu_set_own(&iwu, 1);
		CHECK_EQ(dect_base_remote_set(&b, 5, "low"), -1);	/* not a remote number */
		CHECK_EQ(dect_base_remote_set(&b, 128, "high"), -1);
		CHECK_EQ(dect_base_remote_set(&b, 64, "AS690H"), 0);
		CHECK_EQ(dect_base_remote_set(&b, 65, "Other"), 0);
		CHECK(dect_base_remote_name(&b, 64) && !strcmp(dect_base_remote_name(&b, 64), "AS690H"));
		CHECK(dect_base_remote_name(&b, 66) == NULL);
		dect_base_iwu_lines(&b, &iwu, 1);	/* no name_visible(): all */
		CHECK_EQ(iwu.n_handsets, 3u);
		CHECK_EQ(iwu.handsets[1].terminal_id, 64u);
		vops.name_visible = same_account;	/* viewer 1 sees every remote number */
		b.ops = &vops;
		CHECK_EQ(dect_base_remote_set(&b, 3, "x"), -1);
		b.main_link.active = 1;
		dect_iwu_init(&b.main_link.link.cc.iwu);
		dect_iwu_set_own(&b.main_link.link.cc.iwu, 1);
		CHECK_EQ(dect_base_remote_set(&b, 66, "New"), 0);
		CHECK_EQ(b.main_link.link.cc.iwu.n_handsets, 1u);	/* reached the live link */
		CHECK_EQ(b.main_link.link.cc.iwu.handsets[0].terminal_id, 66u);
		CHECK_EQ(dect_base_remote_set(&b, 66, ""), 0);		/* removed again */
		CHECK_EQ(b.main_link.link.cc.iwu.n_handsets, 0u);
		CHECK_EQ(dect_base_remote_set(&b, 64, NULL), 0);
		CHECK_EQ(b.n_remote, 1u);
		CHECK_EQ(dect_base_remote_set(&b, 99, NULL), 0);	/* unknown: no-op */
		CHECK_EQ(b.n_remote, 1u);
	}
	/*
	 * A link that adopts a record fetched after it came up gets that
	 * record's own entry (API 149): the list built at link-up held the
	 * free store that was current then.
	 */
	{
		static struct dect_base_ops vops;
		struct dect_iwu iwu;

		dect_base_init(&b);
		b.n_hs = 3;
		b.hs[0].slot = 1;	/* a free store: nobody's entry */
		b.absent[0] = 1;
		b.hs[1].slot = 2;
		b.loaded[1] = 1;
		snprintf(b.name[1], sizeof(b.name[1]), "Hall");
		b.hs[2].slot = 7;	/* the roamer, added after the link came up */
		b.loaded[2] = 1;
		snprintf(b.name[2], sizeof(b.name[2]), "A540");
		b.cur = 2;
		b.rec.slot = 7;		/* the current record's slot is the working copy's */
		dect_iwu_init(&iwu);
		CHECK_EQ(dect_iwu_add_handset(&iwu, 1, "DECT 1"), 0);	/* link-up's */
		dect_iwu_set_own(&iwu, 1);
		dect_base_iwu_adopted(&b, &iwu, 7);
		CHECK_EQ(iwu.own_terminal_id, 7u);
		CHECK_EQ(iwu.n_handsets, 2u);
		CHECK_EQ(iwu.handsets[0].terminal_id, 2u);
		CHECK_EQ(strcmp(iwu.handsets[0].name, "Hall"), 0);
		CHECK_EQ(iwu.handsets[1].terminal_id, 7u);
		CHECK_EQ(strcmp(iwu.handsets[1].name, "A540"), 0);
		/* and its account's filter: slot 7 is alone */
		vops.name_visible = same_account;
		b.ops = &vops;
		b.rec.slot = 3;
		dect_base_iwu_adopted(&b, &iwu, 3);
		CHECK_EQ(iwu.n_handsets, 1u);
		CHECK_EQ(iwu.handsets[0].terminal_id, 3u);
	}
	/* Records 8 to 32 are in the Internal Names List too (API 147). */
	{
		unsigned i;

		dect_base_init(&b);
		b.n_hs = DECT_BASE_HS_MAX;
		for (i = 0; i < DECT_BASE_HS_MAX; i++) {
			b.hs[i].slot = i + 1u;
			b.loaded[i] = 1;
		}
		b.main_link.active = 1;
		dect_iwu_init(&b.main_link.link.cc.iwu);
		for (i = 0; i < DECT_BASE_HS_MAX; i++)
			dect_base_names_spread(&b, i);
		CHECK_EQ(b.main_link.link.cc.iwu.n_handsets, DECT_BASE_HS_MAX);
		CHECK_EQ(b.main_link.link.cc.iwu.handsets[31].terminal_id, 32u);
		CHECK_EQ(strcmp(b.main_link.link.cc.iwu.handsets[31].name, "DECT 32"), 0);
	}
	return CHECK_DONE();
}

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

static void log_line(void *ctx, const char *line)
{
	(void)ctx;
	snprintf(logged, sizeof(logged), "%s", line);
}

static unsigned installs;

static void install(void *ctx)
{
	(void)ctx;
	installs++;
}

static const struct dect_base_ops ops = { .save = save, .send = send,
					  .random = rnd, .log = log_line,
					  .install = install };

int main(void)
{
	struct dect_base b;

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
	}

	/* chip slots and a side link (S2.4) */
	{
		struct dect_base_link bl;
		struct dect_mm_cfg cfg;

		CHECK_EQ(dect_base_slot(&b, 1), 2u);
		b.slot0 = 5;
		CHECK_EQ(dect_base_slot(&b, 0), 5u);	/* current, record 0 */
		b.rec.slot = 9;
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
		CHECK_EQ(b.main_link.persisted, 1);
		CHECK_EQ(dect_base_main_registered(&b), 0);	/* once */
		CHECK_EQ(installs, 1u);
	}
	return CHECK_DONE();
}

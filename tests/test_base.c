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
static char event_line[300];

static void event(void *ctx, struct dect_ctl_out *o)
{
	int n = dect_ctl_end(o);

	(void)ctx;
	if (n > 0)
		snprintf(event_line, sizeof(event_line), "%.*s", n, o->buf);
}

static void install(void *ctx)
{
	(void)ctx;
	installs++;
}

static unsigned pumps;

static void pump(void *ctx, struct dect_base_link *bl)
{
	(void)ctx;
	(void)bl;
	pumps++;
}

static const struct dect_base_ops ops = { .save = save, .send = send,
					  .random = rnd, .log = log_line,
					  .install = install, .event = event,
					  .pump = pump };

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
		CHECK_EQ(b.main_link.persisted, 1);
		CHECK_EQ(dect_base_main_registered(&b), 0);	/* once */
		CHECK_EQ(installs, 1u);
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
	return CHECK_DONE();
}

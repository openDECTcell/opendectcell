/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The base station above the links. See dect_base.h.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "dect_base.h"

void dect_base_init(struct dect_base *b)
{
	memset(b, 0, sizeof(*b));
}

void dect_base_changed(struct dect_base *b)
{
	if (b->ops && b->ops->save)
		b->ops->save(b->ctx, b->cur, &b->rec);
}

void dect_base_stash(struct dect_base *b)
{
	if (b->cur < DECT_BASE_HS_MAX)
		b->hs[b->cur] = b->rec;
}

void dect_base_load(struct dect_base *b, unsigned i)
{
	if (i < DECT_BASE_HS_MAX)
		b->rec = b->hs[i];
}

int dect_base_switch(struct dect_base *b, unsigned i)
{
	if (i == b->cur || i >= b->n_hs)
		return 0;
	dect_base_stash(b);
	dect_base_load(b, i);
	b->cur = i;
	return 1;
}

int dect_base_send(struct dect_base *b, const struct dect_mail *m)
{
	return b->ops && b->ops->send ? b->ops->send(b->ctx, m) : -1;
}

void dect_base_random(struct dect_base *b, uint8_t *out, size_t n)
{
	if (b->ops && b->ops->random)
		b->ops->random(b->ctx, out, n);
	else
		memset(out, 0, n);
}

void dect_base_log(struct dect_base *b, const char *fmt, ...)
{
	char line[256];
	va_list ap;

	if (!b->ops || !b->ops->log)
		return;
	va_start(ap, fmt);
	(void)vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	b->ops->log(b->ctx, line);
}

/* One mail to the STUB's contact task. */
static void chip_mail(struct dect_base *b, uint16_t prim, const uint8_t *p,
		      size_t n)
{
	struct dect_mail m;

	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_PCM;
	m.dst = DECT_TASK_CONTACT_DST;
	m.prim = prim;
	if (n > sizeof(m.param))
		n = sizeof(m.param);
	memcpy(m.param, p, n);
	m.param_len = (uint16_t)n;
	(void)dect_base_send(b, &m);
}

void dect_base_slot_clear(struct dect_base *b, unsigned keep_mask)
{
	char kept[32] = "";
	size_t at = 0;
	unsigned slot;

	for (slot = 1u; slot <= DECT_MM_MAX_SLOT; slot++) {
		const uint8_t p[3] = { (uint8_t)slot, 0x00u /* op: clear */, 0x00u };

		if (keep_mask & (1u << slot)) {
			at += (size_t)snprintf(kept + at, sizeof(kept) - at,
					       " %u", slot);
			continue;
		}
		chip_mail(b, 0x0247u, p, sizeof(p));
	}
	dect_base_log(b, "sub: cleared chip subscription slots other than%s "
		      "-- the chip keeps them across runs and would otherwise "
		      "still admit a handset we have no record of", kept);
}

int dect_base_slot_install(struct dect_base *b, unsigned slot,
			   const uint8_t uak[DECT_MM_UAK_LEN])
{
	uint8_t p[2 + DECT_MM_UAK_LEN];

	/*
	 * {slot, op 1: install or refresh, flag}. The op 1 handler tests the
	 * flag's bit 0 and clears a per-slot mask bit on 1, sets it on 0
	 * [CR A-0113].
	 */
	p[0] = (uint8_t)slot;
	p[1] = 0x01u;
	p[2] = b->chip.att_flag;
	chip_mail(b, 0x0247u, p, 3);
	/*
	 * Slot 7 would address the start of the 0xF605 table and the chip's
	 * writer does not range-check it [CR A-SubscriberCapacity-7].
	 */
	if (slot < 1u || slot > DECT_MM_MAX_SLOT) {
		dect_base_log(b, "sub: refusing 0x020A for slot %u (valid "
			      "1..%u)", slot, DECT_MM_MAX_SLOT);
		return -1;
	}
	p[0] = (uint8_t)slot;		/* one-based, 17-byte record stride */
	memcpy(&p[1], uak, DECT_MM_UAK_LEN);
	/*
	 * The byte after the UAK: stock sends 0x01 per Gigaset (0x02 for the
	 * C4); the handler keeps it per slot for the connection's capability
	 * word [CR A-0113].
	 */
	p[1 + DECT_MM_UAK_LEN] = b->chip.att_cfg;
	chip_mail(b, 0x020Au, p, sizeof(p));
	return 0;
}

void dect_base_slot_mask(struct dect_base *b, uint8_t mask)
{
	const uint8_t p[2] = { 0x00u /* unused by the handler */, mask };

	if (b->chip.no_mask)
		return;
	chip_mail(b, 0x017Cu, p, sizeof(p));
	dect_base_log(b, "sub: locked-in handset mask 0x017c 00 %02x "
		      "[CR A-0129]", mask);
}

void dect_base_link_open(struct dect_base *b, struct dect_base_link *bl,
			 uint8_t handle, const struct dect_mm_cfg *cfg,
			 unsigned hold_mask)
{
	struct dect_mm *mm = &bl->link.mm;
	unsigned i;

	bl->active = 1;
	bl->persisted = 0;
	bl->enrol_routed = 0;
	bl->enrol_switched = 0;
	bl->handle = handle;
	dect_link_init(&bl->link, handle, cfg);
	if (b->rec.port_id_len) {
		memcpy(mm->port_id, b->rec.port_id, b->rec.port_id_len);
		mm->port_id_len = b->rec.port_id_len;
	}
	for (i = 0; i < b->n_hs && i < DECT_BASE_HS_MAX; i++) {
		const struct dect_hs_rec *r = &b->hs[i];

		if (i == b->cur || !(hold_mask & (1u << i)) || !r->port_id_len)
			continue;
		if (dect_mm_hold(mm, r->port_id, r->port_id_len,
				 (uint8_t)r->slot, NULL, (int)i) < 0)
			dect_base_log(b, "sub: could not hold record %u on "
				      "this link", i);
	}
	if (b->rec.port_id_len)
		(void)dect_mm_hold(mm, b->rec.port_id, b->rec.port_id_len,
				   (uint8_t)cfg->sub_slot, NULL, (int)b->cur);
}

int dect_base_main_segment(struct dect_base *b, const struct dect_mail *m,
			   uint64_t now, struct dect_link_ind *ind,
			   int *switched)
{
	struct dect_base_link *bl = &b->main_link;
	int r;

	*switched = 0;
	r = dect_base_link_segment(b, bl, m, now, ind);
	if (r >= 0 && bl->link.mm.adopted >= 0 &&
	    (unsigned)bl->link.mm.adopted != b->cur)
		*switched = dect_base_switch(b, (unsigned)bl->link.mm.adopted);
	return r;
}

int dect_base_main_registered(struct dect_base *b)
{
	struct dect_base_link *bl = &b->main_link;
	const struct dect_mm *mm = &bl->link.mm;

	if (bl->persisted || !mm->subscribed || !mm->stats.located)
		return 0;
	/* the key for later calls' ciphering, and across restarts */
	memcpy(b->rec.uak, mm->uak, DECT_MM_UAK_LEN);
	b->rec.uak_valid = 1;
	/*
	 * The identity gate for every later link, as a stored record arms it:
	 * a {LOCATE-REQUEST} is rejected when nothing is armed, and a handset
	 * told "IPUI unknown" may take it as losing its registration.
	 */
	if (mm->port_id_len && mm->port_id_len <= sizeof(b->rec.port_id)) {
		memcpy(b->rec.port_id, mm->port_id, mm->port_id_len);
		b->rec.port_id_len = mm->port_id_len;
		dect_base_log(b, "sub: identity gate armed for the newly "
			      "registered handset (%zu bytes)",
			      b->rec.port_id_len);
	}
	if (b->ops && b->ops->install)
		b->ops->install(b->ctx);
	dect_base_changed(b);
	bl->persisted = 1;
	return 1;
}

unsigned dect_base_slot(const struct dect_base *b, unsigned i)
{
	if (i == b->cur) {
		unsigned v = b->slot0 && b->cur == 0 ? b->slot0 : b->rec.slot;

		return v >= 1u && v <= DECT_MM_MAX_SLOT ? v : DECT_MM_REG_SLOT;
	}
	if (i == 0 && b->slot0)
		return b->slot0;		/* subslot= is the target's */
	return i < DECT_BASE_HS_MAX ? b->hs[i].slot : 0u;
}

void dect_base_side_open(struct dect_base *b, struct dect_base_link *bl,
			 uint8_t handle, const struct dect_mm_cfg *cfg,
			 unsigned hold_mask)
{
	unsigned i;

	bl->active = 1;
	bl->persisted = 0;
	bl->enrol_routed = 0;
	bl->enrol_switched = 0;
	bl->handle = handle;
	dect_link_init(&bl->link, handle, cfg);
	for (i = 0; i < b->n_hs && i < DECT_BASE_HS_MAX; i++) {
		const struct dect_hs_rec *r = i == b->cur ? &b->rec : &b->hs[i];

		if (!r->port_id_len || !(hold_mask & (1u << i)))
			continue;
		(void)dect_mm_hold(&bl->link.mm, r->port_id, r->port_id_len,
				   (uint8_t)dect_base_slot(b, i), NULL, (int)i);
	}
}

int dect_base_link_segment(struct dect_base *b, struct dect_base_link *bl,
			   const struct dect_mail *m, uint64_t now,
			   struct dect_link_ind *ind)
{
	uint8_t rand_f[8], rs[16];

	dect_base_random(b, rand_f, sizeof(rand_f));
	dect_base_random(b, rs, sizeof(rs));
	return dect_link_rx_segment(&bl->link, m, now, rand_f, rs, ind);
}

const uint8_t *dect_base_uak(const struct dect_base *b, unsigned i)
{
	const struct dect_hs_rec *r;

	if (i >= b->n_hs || i >= DECT_BASE_HS_MAX)
		return NULL;
	r = i == b->cur ? &b->rec : &b->hs[i];
	return r->uak_valid ? r->uak : NULL;
}

void dect_base_cipher_start(struct dect_base *b, struct dect_base_link *bl,
			    const uint8_t uak[DECT_MM_UAK_LEN])
{
	uint8_t crand[8], crs[16];

	dect_base_random(b, crand, sizeof(crand));
	dect_base_random(b, crs, sizeof(crs));
	dect_mm_start_cipher(&bl->link.mm, uak, crand, crs);
}

int dect_base_chip_key(struct dect_base *b, const struct dect_base_link *bl)
{
	struct dect_mail m;
	uint8_t dck[DECT_MM_DCK_LEN];
	size_t n = 0;

	if (!dect_mm_cipher_dck(&bl->link.mm, dck))
		return -1;
	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_PCM;
	m.dst = DECT_TASK_CONTACT_DST;
	m.prim = 0x0208u;
	m.param[n++] = bl->handle;
	memcpy(&m.param[n], dck, DECT_MM_DCK_LEN);	/* plain order (bench) */
	n += DECT_MM_DCK_LEN;
	n += 15u;					/* 00 (memset) */
	m.param[n++] = 0xFFu;
	m.param[n++] = 0xFFu;
	m.param[n++] = 0x00u;
	m.param[n++] = 0x00u;
	m.param_len = (uint16_t)n;			/* 28 */
	(void)dect_base_send(b, &m);
	memset(dck, 0, sizeof(dck));
	memset(&m, 0, sizeof(m));			/* it held the key */
	dect_base_log(b, "call: cipher: call key to the chip (0x0208, link %u, "
		      "%u bytes, plain order)", (unsigned)bl->handle,
		      (unsigned)n);
	return 0;
}

struct dect_base_call *dect_base_call_by_id(struct dect_base *b, unsigned id)
{
	unsigned i;

	for (i = 0; id && i < DECT_BASE_CALLS; i++)
		if (b->calls[i].id == id)
			return &b->calls[i];
	return NULL;
}

struct dect_base_call *dect_base_call_on(struct dect_base *b, int party)
{
	unsigned i;

	for (i = 0; i < DECT_BASE_CALLS; i++)
		if (b->calls[i].id && b->calls[i].party == party)
			return &b->calls[i];
	return NULL;
}

struct dect_base_call *dect_base_call_new(struct dect_base *b)
{
	unsigned i;

	for (i = 0; i < DECT_BASE_CALLS; i++)
		if (!b->calls[i].id) {
			memset(&b->calls[i], 0, sizeof(b->calls[i]));
			return &b->calls[i];
		}
	return NULL;
}

unsigned dect_base_calls_on(const struct dect_base *b, int party)
{
	unsigned i, n = 0;

	for (i = 0; i < DECT_BASE_CALLS; i++)
		if (b->calls[i].id && b->calls[i].party == party)
			n++;
	return n;
}

struct dect_base_call *dect_base_call_active(struct dect_base *b, int party)
{
	unsigned i;

	for (i = 0; i < DECT_BASE_CALLS; i++)
		if (b->calls[i].id && b->calls[i].party == party &&
		    b->calls[i].cs == DECT_CS_CALL_CONNECT)
			return &b->calls[i];
	return NULL;
}

struct dect_base_call *dect_base_call_by_dect(struct dect_base *b, int party,
					      uint8_t id)
{
	unsigned i;

	for (i = 0; id && i < DECT_BASE_CALLS; i++)
		if (b->calls[i].id && b->calls[i].party == party &&
		    b->calls[i].dect_id == id)
			return &b->calls[i];
	return NULL;
}

unsigned dect_base_party_slot(const struct dect_base *b, int party)
{
	if (party == -1) {
		int a = b->main_link.link.mm.adopted;

		return dect_base_slot(b, a >= 0 ? (unsigned)a : b->cur);
	}
	if (party >= 0 && (unsigned)party < DECT_BASE_SIDES &&
	    b->side[party].adopted >= 0)
		return dect_base_slot(b, (unsigned)b->side[party].adopted);
	return 0;
}

unsigned dect_base_call_slot(const struct dect_base *b,
			     const struct dect_base_call *c)
{
	if (c->party == DECT_BASE_PARTY_PAGING)
		return c->target >= 0 ? dect_base_slot(b, (unsigned)c->target)
				      : 0u;
	return dect_base_party_slot(b, c->party);
}

void dect_base_call_event(struct dect_base *b, const struct dect_base_call *c,
			  const char *verb, const char *k2, const char *v2)
{
	struct dect_ctl_out o;
	unsigned hs = dect_base_call_slot(b, c);

	if (!b->ops || !b->ops->event)
		return;
	dect_ctl_begin(&o, verb);
	dect_ctl_add_uint(&o, "call", c->id);
	if (hs)
		dect_ctl_add_uint(&o, "hs", hs);
	if (k2)
		dect_ctl_add(&o, k2, v2);
	b->ops->event(b->ctx, &o);
}

void dect_base_tpui(const struct dect_base *b, unsigned i, uint8_t tpui[3])
{
	struct dect_mm_cfg cfg;
	struct dect_mm mm;

	memset(&cfg, 0, sizeof(cfg));
	cfg.sub_slot = (uint8_t)dect_base_slot(b, i);
	dect_mm_init(&mm, &cfg);
	dect_mm_tpui(&mm, tpui);
}

void dect_base_page(struct dect_base *b, uint8_t info, const uint8_t tpui[3])
{
	const uint8_t p[3] = { info, tpui[1], tpui[2] };

	chip_mail(b, 0x0300u, p, sizeof(p));
}

void dect_base_audio_param(struct dect_base *b, uint8_t handle, uint8_t sel,
			   uint8_t gain)
{
	const uint8_t p[3] = { handle, sel, gain };

	chip_mail(b, 0x0251u, p, sizeof(p));
}

void dect_base_audio_assign(struct dect_base *b, uint8_t handle,
			    uint8_t flags, uint16_t mask, uint8_t mode,
			    uint8_t codec)
{
	const uint8_t p[6] = {
		handle, flags, (uint8_t)mask, (uint8_t)(mask >> 8), mode, codec,
	};

	chip_mail(b, 0x0230u, p, sizeof(p));
}

void dect_base_audio_path(struct dect_base *b, uint8_t handle)
{
	const uint8_t p[2] = { handle, 0x00u };	/* bit 7 clear: connect */

	chip_mail(b, 0x022Du, p, sizeof(p));
}

void dect_base_audio_release(struct dect_base *b, uint8_t handle)
{
	const uint8_t p[2] = { handle, 0x01u };	/* [1] must be non-zero */

	chip_mail(b, 0x0233u, p, sizeof(p));
}

uint8_t dect_base_call_id(const struct dect_base *b,
			  const struct dect_link *me)
{
	unsigned id, k;

	if (b->cfg.callinfo.has_call_id)
		return 0;
	for (id = 1; id <= 127u; id++) {
		int used = 0;

		if (b->main_link.active && &b->main_link.link != me &&
		    dect_cc_busy(&b->main_link.link.cc) &&
		    b->main_link.link.cc.status_call_id == id)
			used = 1;
		for (k = 0; !used && k < DECT_BASE_SIDES; k++)
			if (b->side[k].active && &b->side[k].link != me &&
			    dect_cc_busy(&b->side[k].link.cc) &&
			    b->side[k].link.cc.status_call_id == id)
				used = 1;
		for (k = 0; !used && k < DECT_BASE_CALLS; k++)
			if (b->calls[k].id && b->calls[k].dect_id == id)
				used = 1;	/* a parallel call's */
		if (!used)
			return (uint8_t)id;
	}
	return 0;
}

int dect_base_ring_part3(const struct dect_base *b)
{
	const struct dect_callinfo *o = &b->cfg.callinfo;

	return !o->has_line && !o->has_call_id && !o->has_call_status &&
	       b->n_hs && b->cap[b->ring.hs].known && b->cap[b->ring.hs].part3;
}

void dect_base_ring_callinfo(struct dect_base *b, int on)
{
	struct dect_callinfo ci;
	unsigned i;

	if (!on || !dect_base_ring_part3(b)) {
		dect_cc_set_callinfo(&b->main_link.link.cc, &b->cfg.callinfo);
		if (on && b->n_hs && !b->cap[b->ring.hs].known)
			dect_base_log(b, "ring: the target's terminal capability "
				      "is not known yet -- no call information "
				      "(an A540 then rings silently)");
		return;
	}
	dect_callinfo_init(&ci);
	ci.has_line = 1;
	ci.line_id = DECT_BASE_LINE_ID;
	for (i = 0; i < DECT_BASE_CALLS; i++)
		if (b->calls[i].id && b->calls[i].party == -1)
			ci.line_id = (uint8_t)b->calls[i].line;	/* the called one */
	ci.has_line_type = 1;
	ci.line_type = DECT_CALLINFO_NDT_DELAY;
	ci.has_call_id = 1;
	ci.call_id = b->main_link.link.cc.status_call_id;
	ci.has_call_status = 1;
	ci.call_status = DECT_CS_CALL_SETUP;
	dect_cc_set_callinfo(&b->main_link.link.cc, &ci);
	dect_base_log(b, "ring: full call information for a Part 3 handset "
		      "(line %u, VoIP, call id %u, CS call setup)",
		      (unsigned)ci.line_id, (unsigned)ci.call_id);
}

uint8_t dect_base_basic_svc(const struct dect_base *b, int i,
			    uint8_t configured)
{
	if (i >= 0 && (unsigned)i < b->n_hs && b->prof[i].cc_basic_service)
		return (uint8_t)((configured & 0xF0u) |
				 (b->prof[i].cc_basic_service == 1 ? 0x08u
								    : 0x00u));
	if (i >= 0 && (unsigned)i < b->n_hs && b->cap[i].known &&
	    !b->cap[i].ng)
		return 0x80u;
	return configured;
}

uint8_t dect_base_ring_svc(const struct dect_base *b, unsigned t, int narrow)
{
	uint8_t svc = dect_base_basic_svc(b, (int)t, b->cfg.basic_svc);

	return narrow ? (uint8_t)(svc & 0xF0u) : svc;
}

int dect_base_ring_narrow(const struct dect_base *b)
{
	unsigned i;

	for (i = 0; i < DECT_BASE_CALLS; i++)
		if (b->calls[i].id && b->calls[i].party == -1)
			return b->calls[i].narrow;
	return 0;
}

uint8_t dect_base_page_info(const struct dect_base *b, int narrow)
{
	return (uint8_t)(DECT_BASE_PAGE_CC |
			 ((b->cfg.basic_svc & 0x0Fu) == 0x08u && !narrow
			  ? DECT_BASE_PAGE_WIDEBAND : 0u));
}

void dect_base_ring_setup(struct dect_base *b, uint64_t now, const char *why)
{
	struct dect_base_link *bl = &b->main_link;
	struct dect_clip clip;
	unsigned tries = b->cfg.setup_tries ? b->cfg.setup_tries
					    : DECT_BASE_SETUP_TRIES;

	if (!bl->active) {
		dect_base_log(b, "ring: no link up at the page-response "
			      "deadline -- paging again");
		b->ring.state = DECT_BASE_RING_PAGING;
		return;
	}
	if (b->cur != b->ring.hs) {
		dect_base_log(b, "ring: this link serves record %u, not the "
			      "handset we ring -- waiting", b->cur);
		b->ring.resp_deadline = now + DECT_BASE_SETUP_RETRY_MS;
		return;
	}
	dect_cc_set_next_ti(&bl->link.cc, 0);
	{
		uint8_t id = dect_base_call_id(b, &bl->link);

		if (id)
			dect_cc_set_call_status(&bl->link.cc,
						bl->link.cc.call_status, id);
	}
	dect_base_ring_callinfo(b, 1);
	clip = b->clip;
	if (b->n_hs) {
		const struct dect_hs_profile *hp = &b->prof[b->ring.hs];

		if (!b->cfg.signal_set &&
		    hp->cc_signal_external != DECT_SIGNAL_ALERT_PATTERN1)
			dect_cc_set_signal(&bl->link.cc, 1,
					   hp->cc_signal_external);
		if (hp->cc_name_method == 1 || hp->cc_name_method == 3)
			clip.name[0] = '\0';	/* display or none */
	}
	if (!dect_cc_setup_id(&bl->link.cc, b->rec.cc_port_id,
			      b->rec.cc_port_id_len, b->rec.cc_fixed_id,
			      b->rec.cc_fixed_id_len,
			      dect_base_ring_svc(b, b->ring.hs,
						 dect_base_ring_narrow(b)),
			      &clip)) {
		dect_base_ring_callinfo(b, 0);
		b->ring.state = DECT_BASE_RING_DONE;
		b->ring.watch = 1;
		b->ring.coll0 = bl->link.cc.stats.collisions;
		b->ring.alert0 = bl->link.cc.stats.alerting;
		dect_base_log(b, "ring: CC-SETUP queued (%s) -- the handset "
			      "should ring", why);
		return;
	}
	if (++b->ring.setup_tries >= tries) {
		b->ring.state = DECT_BASE_RING_DONE;
		dect_base_log(b, "ring: CC-SETUP refused %u times, the "
			      "handset's own call never cleared; giving up",
			      b->ring.setup_tries);
		return;
	}
	b->ring.resp_deadline = now + DECT_BASE_SETUP_RETRY_MS;
	dect_base_log(b, "ring: CC-SETUP refused (a call is in progress), "
		      "retry %u of %u in %u ms", b->ring.setup_tries, tries,
		      DECT_BASE_SETUP_RETRY_MS);
}

static void pump(struct dect_base *b, struct dect_base_link *bl)
{
	if (b->ops && b->ops->pump)
		b->ops->pump(b->ctx, bl);
}

/*
 * EN 300 175-5 §14.2.3 pages only when no suitable link exists, and the
 * release before paging that the chip needs ([CR A-0053]) cut whatever the
 * link carried: a ring during a call ended the call (spec sweep F-2, LCE1).
 */
int dect_base_link_use(const struct dect_link *l)
{
	if (dect_cc_busy(&l->cc))
		return (l->cc.peer_basic_service >> 4) == 0x2u && !l->cc.incoming
		       ? 1 : 2;
	if (l->mm.state == DECT_MM_WAIT_AUTH || l->mm.ident_pending ||
	    l->mm.cipher_state == DECT_MM_CIPHER_WAIT_REPLY ||
	    dect_mm_locate_pending(&l->mm, NULL, NULL))
		return 1;
	return 0;
}

/*
 * The identities of the handset we ring, wherever they are: in the
 * working copy while it is current, in the table while the link serves
 * another one.
 */
size_t dect_base_ring_fixed_len(const struct dect_base *b)
{
	return b->cur == b->ring.hs || !b->n_hs ? b->rec.cc_fixed_id_len
		: b->hs[b->ring.hs].cc_fixed_id_len;
}

void dect_base_ring_page(struct dect_base *b)
{
	uint8_t tpui[3], info;

	/*
	 * The page carries the low 16 bits of the TPUI we assigned, by MM's
	 * rule for the target's slot: with a pool-assigned TPUI a page built
	 * from the slot would address the wrong handset.
	 */
	dect_base_tpui(b, b->ring.hs, tpui);
	info = dect_base_page_info(b, dect_base_ring_narrow(b));
	dect_base_page(b, info, tpui);
	dect_base_log(b, "ring: paged the handset, attempt %u of %u "
		      "(0x0300 {%02x %02x %02x}, basic service %02x)%s",
		      b->ring.pages, DECT_BASE_PAGE_RETRIES, info,
		      tpui[1], tpui[2], b->cfg.basic_svc,
		      /*
		       * We page BY the TPUI we assigned. If the handset never
		       * acknowledged the assignment (EN 300 175-5 §13.2.2) it
		       * may not be listening for it, and the page is silent in
		       * a way that looks exactly like a handset that will not
		       * answer. Say so rather than letting it look normal.
		       */
		      dect_mm_tpui_confirmed(&b->main_link.link.mm)
		      ? "" : " -- WARNING: TPUI never acknowledged");
}

/*
 * The queued SETUP is gone without an ALERTING: if a {CC-SETUP} of the
 * handset's crossed it (EN 300 175-5 §9.5.2.3), ring again from the start
 * once the link is free. The A540 opens a list call right after a location
 * registration with a list change notice, and does not wait P<CC.06> for
 * our re-presentation, so the ring was lost (bench, 2026-10-28).
 */
void dect_base_ring_watch(struct dect_base *b)
{
	struct dect_base_ring *r = &b->ring;
	const struct dect_base_link *bl = &b->main_link;
	const struct dect_cc *cc = &bl->link.cc;

	if (r->state != DECT_BASE_RING_DONE || !r->watch)
		return;
	if (bl->active && cc->stats.alerting != r->alert0) {
		r->watch = 0;			/* it rings */
		return;
	}
	if (bl->active && cc->incoming && dect_cc_busy(cc))
		return;				/* still presented */
	r->watch = 0;
	if (!bl->active || cc->stats.collisions == r->coll0)
		return;				/* refused or ended otherwise */
	if (r->requeues >= DECT_BASE_REQUEUE_MAX) {
		dect_base_log(b, "ring: lost to the handset's own call again -- "
			      "giving up after %u new attempts", r->requeues);
		return;
	}
	r->requeues++;
	r->state = DECT_BASE_RING_IDLE;
	r->setup_tries = 0;
	r->pages = 0;
	r->wait_since = 0;
	dect_base_log(b, "ring: our call was lost to the handset's own call "
		      "(collision, 9.5.2.3) -- ringing again when the link is "
		      "free (%u of %u)", r->requeues, DECT_BASE_REQUEUE_MAX);
}

/*
 * A link in use is not released under the handset (§14.2.3): a voice call
 * on it -- the ring target's or, on the main link, another handset's --
 * makes this ring "busy" (call waiting is not built yet,
 * lines-and-calls.md step 5); a list session or an MM procedure is waited
 * out, for at most DECT_BASE_IN_USE_WAIT_MS, then released as before.
 * 0: go on; 1: wait or stop (the state says which).
 */
static int ring_in_use(struct dect_base *b, uint64_t now)
{
	struct dect_base_ring *r = &b->ring;
	int use = b->main_link.active ? dect_base_link_use(&b->main_link.link)
				      : 0;
	unsigned k;

	for (k = 0; k < DECT_BASE_SIDES; k++)
		if (b->side[k].active && b->side[k].adopted == (int)r->hs &&
		    dect_base_link_use(&b->side[k].link) == 2)
			use = 2;		/* the target, elsewhere */
	if (use == 2) {
		dect_base_log(b, "ring: the handset (or the main link) is in a "
			      "call -- busy, nothing released, ring abandoned");
		r->state = DECT_BASE_RING_DONE;
		return 1;
	}
	if (use == 1 && !r->wait_since) {
		r->wait_since = now;
		dect_base_log(b, "ring: the link carries a list session or an "
			      "MM procedure -- waiting for it to end");
	}
	if (use == 1 && now - r->wait_since < DECT_BASE_IN_USE_WAIT_MS)
		return 1;
	if (use == 1)
		dect_base_log(b, "ring: still in use after %u ms -- releasing "
			      "it all the same", DECT_BASE_IN_USE_WAIT_MS);
	else if (r->wait_since)
		dect_base_log(b, "ring: the link is free after %llu ms",
			      (unsigned long long)(now - r->wait_since));
	r->wait_since = 0;
	return 0;
}

void dect_base_ring_tick(struct dect_base *b, uint64_t now)
{
	struct dect_base_ring *r = &b->ring;
	struct dect_base_link *bl = &b->main_link;

	if (!b->cfg.ring_ms && !b->cfg.ring_attach_ms && !r->now)
		return;
	dect_base_ring_watch(b);
	if (r->state == DECT_BASE_RING_DONE)
		return;

	/*
	 * Waiting for the page response. Send the SETUP anyway once the
	 * deadline passes, so a handset that never answers the page the way
	 * we expect still gets a call rather than silence.
	 */
	if (r->state == DECT_BASE_RING_WAIT_PAGE_RESP) {
		if (now >= r->resp_deadline)
			dect_base_ring_setup(b, now, r->setup_tries
					     ? "retrying after a refusal"
					     : "no page response, deadline");
		return;
	}

	if (r->state == DECT_BASE_RING_IDLE) {
		if (r->now) {
			/* a call-in from the bridge: now */
		} else if (b->cfg.ring_attach_ms) {
			/* Count from the handset's first contact, not ours. */
			if (!b->first_contact_ms ||
			    now - b->first_contact_ms < b->cfg.ring_attach_ms)
				return;
		} else if (now - b->start_ms < b->cfg.ring_ms) {
			return;
		}
		if (!dect_base_ring_fixed_len(b)) {
			dect_base_log(b, "ring: no call identities -- the handset "
				      "is not registered here, or -s is off so "
				      "nothing was stored");
			r->state = DECT_BASE_RING_DONE;
			return;
		}
		if (ring_in_use(b, now))
			return;
		/*
		 * **Page for every call, even when the handset is attached.**
		 *
		 * This used to take a shortcut: a link is already up, so the
		 * handset does not need paging and the SETUP can go straight
		 * out. That shortcut was the whole of defect 2's intermittency.
		 *
		 * Measured 2026-10-23 over seven runs, a clean 7/7 split:
		 *
		 *   "page response received" -> the mode-2 re-assign takes ->
		 *       audio works
		 *   "a link was already up"  -> the enable word never reaches 1,
		 *       however often mode 2 is asked for (eight times in vF415)
		 *       -> no audio at all
		 *
		 * [CR A-0052] names the mechanism. The link that is already up
		 * is the ATTACHMENT/signalling link, not a traffic bearer. Stock
		 * sets up a call's traffic bearer by PAGING (0x0300, page type
		 * 0x11), and the causal chain is: page -> the handset answers
		 * over the air -> the chip allocates the traffic bearer -> MAC
		 * bearer event 0x20 -> event 0x70 -> the configurator binds the
		 * audio channel. Skip the page and nothing ever asks the chip for
		 * a traffic bearer, so 0x20/0x70 never fire, the configurator
		 * never runs, and the channel keeps its unconfigured default
		 * (enable = 0, dest 6 = the silence source) no matter what we
		 * assign afterwards.
		 *
		 * Paging an idle-locked attached handset is the ordinary DECT
		 * mechanism and does not un-attach it, so this is light, not
		 * heavy: no release, no re-attach. An earlier fix here waited up
		 * to 20 s for the link to "clear" -- vF420/vF420b showed the C4
		 * attaches a second after the base comes up and holds the link
		 * indefinitely, so the wait never fired and could not have.
		 */
		/*
		 * [CR A-0053]: stock never pages a linked handset. It releases
		 * the link first and always pages from idle-locked, and the
		 * release does NOT un-attach the handset -- attachment and
		 * location registration are persistent and separate from the
		 * CC/DLC link, so the handset returns to idle-locked and
		 * pageable without re-registering. Release once per call.
		 *
		 * vF423 is the measurement behind this: with the link up, 16
		 * pages went unanswered, where vF411 -- same build, same mails,
		 * no link up -- got a page response on attempt 1.
		 */
		if (bl->active) {
			dect_base_log(b, "ring: releasing the link before paging "
				      "-- a linked handset does not answer "
				      "pages, and the page is what makes the "
				      "chip allocate a traffic bearer");
			dect_lapc_disconnect(&bl->link.lapc);
			pump(b, bl);
			r->release_until = now + DECT_BASE_RELEASE_WAIT_MS;
			r->state = DECT_BASE_RING_RELEASING;
			return;
		}
		r->state = DECT_BASE_RING_PAGING;
	}

	/*
	 * Wait for the link to actually go down before paging: active is
	 * cleared on the chip's connection-down. Page anyway when the wait
	 * expires: an unanswered page costs one ring attempt, and the
	 * fallback below still rings the handset over the old link.
	 */
	if (r->state == DECT_BASE_RING_RELEASING) {
		if (bl->active && now < r->release_until)
			return;
		dect_base_log(b, "ring: link %s -- paging now",
			      bl->active ? "did not go down in time, paging "
					   "anyway" : "released");
		r->state = DECT_BASE_RING_PAGING;
	}

	if (r->pages && now - r->last < DECT_BASE_PAGE_INTERVAL_MS)
		return;
	if (r->pages >= DECT_BASE_PAGE_RETRIES) {
		/*
		 * An attached handset does not answer our page. Measured
		 * 2026-10-23 (vF423): with the attachment link up, 16 pages
		 * went unanswered, where vF411 -- same build, same mails, no
		 * link up -- got a page response on attempt 1. So [CR A-0052]'s
		 * "paging an idle-locked attached handset is routine" does not
		 * hold for this handset in this state: the C4 ignores the page
		 * while it holds a link.
		 *
		 * Fall back to the SETUP on the existing link so the handset
		 * still rings. That call will have no audio -- the chip never
		 * allocates a traffic bearer without a page response, so the
		 * configurator never runs -- but a call that rings and is
		 * silent beats a handset that does not ring at all, which is
		 * what paging alone gives us here.
		 */
		if (bl->active) {
			dect_base_log(b, "ring: %u pages unanswered -- the "
				      "handset is attached and ignores them; "
				      "falling back to the SETUP on the "
				      "existing link; EXPECT NO AUDIO",
				      r->pages);
			dect_base_ring_setup(b, now, "a link was already up");
			return;
		}
		dect_base_log(b, "ring: the handset did not answer %u pages, "
			      "giving up", r->pages);
		r->state = DECT_BASE_RING_DONE;
		return;
	}
	r->pages++;
	r->last = now;
	dect_base_ring_page(b);
}

struct dect_base_link *dect_base_party_link(struct dect_base *b, int p)
{
	return p < 0 ? &b->main_link : &b->side[p];
}

void dect_base_drain(struct dect_base *b, int p, uint64_t now)
{
	struct dect_base_link *bl = dect_base_party_link(b, p);

	dect_link_drain(&bl->link, now);
	pump(b, bl);
}

unsigned dect_base_call_new_id(struct dect_base *b)
{
	unsigned i;

	do {
		b->next_call = b->next_call % 127u + 1u;
		for (i = 0; i < DECT_BASE_CALLS; i++)
			if (b->calls[i].id == b->next_call)
				break;
	} while (i < DECT_BASE_CALLS);
	return b->next_call;
}

int dect_base_party_line2(const struct dect_base *b, int p)
{
	int a = p == -1 ? (b->main_link.link.mm.adopted >= 0 ?
			   b->main_link.link.mm.adopted : (int)b->cur)
			: p >= 0 ? b->side[p].adopted : -1;

	return a >= 0 && (unsigned)a < b->n_hs && b->line2[a];
}

int dect_base_call_out(struct dect_base *b, int p, uint64_t now)
{
	struct dect_cc *cc = &dect_base_party_link(b, p)->link.cc;
	struct dect_ctl_out o;
	struct dect_base_call *c;
	const uint8_t *k;
	size_t len;
	char num[DECT_CTL_VAL_MAX];

	if (!b->bridge)
		return 0;
	k = dect_cc_keypad(cc, &len);
	if (len >= sizeof(num))
		len = sizeof(num) - 1;
	if (len)
		memcpy(num, k, len);
	num[len] = '\0';
	c = dect_base_call_new(b);
	if (!c) {
		(void)dect_cc_busy_system(cc);
		dect_base_log(b, "bridge: %u calls already -- busy",
			      DECT_BASE_CALLS);
		dect_base_drain(b, p, now);
		return 1;
	}
	if (dect_cc_proceed(cc))
		return 0;
	c->id = dect_base_call_new_id(b);
	c->party = p;
	c->out = 1;
	c->since = now;
	/* the line the handset chose (§7.4.5.2), if it has it; else Line 1 */
	c->line = dect_cc_line(cc);
	if (c->line != DECT_BASE_LINE_ID + 1u || !dect_base_party_line2(b, p))
		c->line = DECT_BASE_LINE_ID;
	dect_ctl_begin(&o, "call-out");
	dect_ctl_add_uint(&o, "call", c->id);
	dect_ctl_add_uint(&o, "hs", dect_base_party_slot(b, p));
	dect_ctl_add_uint(&o, "line", c->line);
	/* nb: the handset's call is narrowband, so the INVITE offers G.711 */
	dect_ctl_add(&o, "codec", dect_cc_codec(cc) == DECT_CC_CODEC_G722 ?
		     "wb" : "nb");
	dect_ctl_add(&o, "number", num);
	if (b->ops && b->ops->event)
		b->ops->event(b->ctx, &o);
	dect_base_log(b, "bridge: call %u out from DECT %u to \"%s\" -- "
		      "waiting for the far end", c->id,
		      dect_base_party_slot(b, p), num);
	dect_base_drain(b, p, now);
	return 1;
}

int dect_base_call_auth_ok(const struct dect_base *b,
			   const struct dect_link *l)
{
	return !b->cfg.auth_first || b->cfg.no_cipher || l->mm.authenticated;
}

int dect_base_number_complete(const struct dect_cc *cc)
{
	unsigned cls = cc->peer_basic_service >> 4;

	return cc->state == DECT_CC_ST_PROCEEDING && !cc->incoming &&
	       !cc->busy_notified &&	/* §7.4.8.3: the busy tone */
	       (cls == 0x8u || cls == 0x9u || cc->list_number);
}

int dect_base_dial_tick(struct dect_base *b, int p, uint64_t now)
{
	struct dect_base_link *bl = dect_base_party_link(b, p);
	struct dect_cc *cc = &bl->link.cc;
	int due = 0;
	size_t len;

	if (bl->active && !bl->ic_tried && dect_base_number_complete(cc) &&
	    dect_base_call_auth_ok(b, &bl->link))
		due = 1;
	if (!bl->active || !dect_cc_busy(cc))
		bl->ic_tried = 0;
	if (!bl->active || cc->state != DECT_CC_ST_OVERLAP) {
		bl->dial_len = 0;
		bl->dial_change_ms = 0;
		return due;
	}
	(void)dect_cc_keypad(cc, &len);
	if (!bl->dial_change_ms || len != bl->dial_len) {
		bl->dial_len = len;
		bl->dial_change_ms = now;
		return due;
	}
	if (!len || now - bl->dial_change_ms < b->cfg.dial_wait_ms)
		return due;	/* no digit yet: F<CC.01> covers it */
	if (!dect_base_call_auth_ok(b, &bl->link))
		return due;	/* the number waits for the proof */
	return 2;
}

void dect_base_dial_connect(struct dect_base *b, uint64_t now)
{
	struct dect_base_link *bl = &b->main_link;

	if (b->cfg.alert_ms || b->cfg.bind_first) {
		/* the steps a routed call takes; connect_tick() finishes */
		if (dect_cc_proceed(&bl->link.cc) == 0) {
			if (b->cfg.alert_ms) {
				(void)dect_cc_alert(&bl->link.cc);
				dect_base_log(b, "call: ALERTING sent, CONNECT in "
					      "%u ms", b->cfg.alert_ms);
			}
			bl->connect_staged = 1;
			bl->connect_at = now + b->cfg.alert_ms;
			dect_base_drain(b, -1, now);
		}
		return;
	}
	if (dect_cc_accept(&bl->link.cc) == 0)
		dect_base_drain(b, -1, now);
}

void dect_base_connect_tick(struct dect_base *b, uint64_t now)
{
	struct dect_base_link *bl = &b->main_link;

	if (!bl->connect_staged)
		return;
	if (!bl->active || bl->link.cc.state != DECT_CC_ST_PROCEEDING) {
		bl->connect_staged = 0;		/* released meanwhile */
		return;
	}
	if (now < bl->connect_at || (b->cfg.bind_first && !bl->path_on))
		return;
	bl->connect_staged = 0;
	dect_base_log(b, "call: CONNECT now%s%s",
		      b->cfg.alert_ms ? ", after ALERTING" : "",
		      b->cfg.bind_first ? ", the U-plane is up (0x022D sent)"
					: "");
	if (dect_cc_connect(&bl->link.cc) == 0)
		dect_base_drain(b, -1, now);
}

void dect_base_cs_connect_tick(struct dect_base *b, uint64_t now)
{
	struct dect_base_link *bl = &b->main_link;
	struct dect_cc *cc = &bl->link.cc;
	struct dect_callinfo ci;
	uint8_t ie[16];
	size_t n;

	if (!bl->active || !dect_cc_busy(cc)) {
		bl->cs_connect_sent = 0;
		return;
	}
	if (bl->cs_connect_sent || !cc->incoming ||
	    cc->state != DECT_CC_ST_ACTIVE || !dect_base_ring_part3(b) ||
	    b->cfg.no_call_status)
		return;
	bl->cs_connect_sent = 1;
	dect_callinfo_init(&ci);
	ci.has_call_id = 1;
	ci.call_id = cc->status_call_id;
	ci.has_call_status = 1;
	ci.call_status = DECT_CS_CALL_CONNECT;
	n = dect_callinfo_put(ie, sizeof(ie), 0, &ci);
	if (!n || dect_cc_info(cc, ie, n)) {
		dect_base_log(b, "call: could not queue the CS call connect");
		return;
	}
	dect_base_log(b, "call: CS call connect sent in CC-INFO (Part 3)");
	dect_base_drain(b, -1, now);
}

void dect_base_hs_name(const struct dect_base *b, unsigned i, char *buf,
		       size_t cap)
{
	if (b->name[i][0])
		snprintf(buf, cap, "%s", b->name[i]);
	else
		snprintf(buf, cap, "DECT %u", dect_base_slot(b, i));
}

int dect_base_party_active(const struct dect_base *b, int p)
{
	return p == -1 ? b->main_link.active
		       : p >= 0 && b->side[p].active;
}

static struct dect_cc *party_cc(struct dect_base *b, int p)
{
	return &dect_base_party_link(b, p)->link.cc;
}

static int lanes(struct dect_base *b, int p)
{
	return b->ops && b->ops->lanes ? b->ops->lanes(b->ctx, p) : 2;
}

void dect_base_cc_ids(const struct dect_base *b, unsigned i,
		      const uint8_t **port, size_t *plen,
		      const uint8_t **fixed, size_t *flen)
{
	const struct dect_hs_rec *r = i == b->cur ? &b->rec : &b->hs[i];

	*port = r->cc_port_id;
	*plen = r->cc_port_id_len;
	*fixed = r->cc_fixed_id;
	*flen = r->cc_fixed_id_len;
}

static void ic_page(struct dect_base *b)
{
	struct dect_base_ic *ic = &b->ic;

	dect_base_page(b, DECT_BASE_PAGE_CC | DECT_BASE_PAGE_WIDEBAND, ic->tpui);
	ic->pages++;
	dect_base_log(b, "intern: paged DECT %u, attempt %u of %u",
		      dect_base_slot(b, (unsigned)ic->target), ic->pages,
		      DECT_BASE_IC_PAGES);
}

int dect_base_ic_start(struct dect_base *b, int p, uint64_t now)
{
	struct dect_base_ic *ic = &b->ic;
	struct dect_cc *cc = party_cc(b, p);
	const uint8_t *k;
	size_t len, j;
	unsigned slot = 0, i;
	struct dect_mm_cfg cfg;
	struct dect_mm mm;

	if ((cc->peer_basic_service >> 4) != 0x9u)
		return 0;			/* not an internal call */
	k = dect_cc_keypad(cc, &len);
	for (j = 0; k && j < len; j++)
		if (k[j] >= '0' && k[j] <= '9')
			slot = slot * 10u + (unsigned)(k[j] - '0');
	if (!b->cfg.two_calls || ic->st != DECT_BASE_IC_IDLE) {
		/*
		 * With a reason the handset can show (EN 300 175-5 §7.7.18):
		 * another internal call in the way is the system busy (TS 102
		 * 527-3 §7.4.8.3 for a Part 3 handset), the feature switched
		 * off "service not implemented". It went without one.
		 */
		dect_base_log(b, "intern: internal call refused (%s)",
			      b->cfg.two_calls ? "one at a time"
					       : "needs -a twocalls");
		if (b->cfg.two_calls)
			(void)dect_cc_busy_system(cc);
		else
			(void)dect_cc_release_reason(cc,
				DECT_CC_REASON_SERVICE_NOT_IMPLEMENTED);
		return 1;
	}
	/* lanes: the target lands on a side link; a side caller needs its own */
	if (lanes(b, p) < (p >= 0 ? 2 : 1)) {
		dect_base_log(b, "intern: no free audio channel -- system busy");
		(void)dect_cc_busy_system(cc);
		return 1;
	}
	for (i = 0; i < b->n_hs; i++)
		if (b->loaded[i] && dect_base_slot(b, i) == slot)
			break;
	if (i >= b->n_hs || (p < 0 && i == b->cur) ||
	    (p >= 0 && (int)i == b->side[p].adopted)) {
		dect_base_log(b, "intern: no other handset in slot %u -- "
			      "releasing, \"user unknown\"", slot);
		(void)dect_cc_release_reason(cc, DECT_CC_REASON_USER_UNKNOWN);
		return 1;
	}
	(void)dect_cc_proceed(cc);
	memset(&cfg, 0, sizeof(cfg));
	cfg.sub_slot = (uint8_t)dect_base_slot(b, i);
	dect_mm_init(&mm, &cfg);
	dect_mm_tpui(&mm, ic->tpui);
	ic->st = DECT_BASE_IC_PAGING;
	ic->caller = p;
	ic->target = (int)i;
	ic->tside = DECT_BASE_IC_NONE;
	ic->pages = 0;
	ic->page_at = now;
	dect_base_log(b, "intern: %s calls DECT %u",
		      p < 0 ? "the main link's handset"
			    : "a side link's handset", slot);
	return 1;
}

void dect_base_ic_setup(struct dect_base *b, int p, uint64_t now,
			const char *how)
{
	struct dect_base_ic *ic = &b->ic;
	const uint8_t *port, *fixed;
	size_t plen, flen;
	struct dect_link *l = &dect_base_party_link(b, p)->link;
	int ci_hs;
	unsigned slot;
	char num[12], name[DECT_IWU_NAME_MAX + 12];
	struct dect_callinfo prev, ci;
	struct dect_clip num_only;
	int r;

	dect_base_cc_ids(b, (unsigned)ic->target, &port, &plen, &fixed, &flen);
	if (!plen || p == ic->caller || dect_cc_busy(&l->cc))
		return;
	dect_cc_set_next_ti(&l->cc, 0);
	/* one call, one id on both handsets (TS 102 527-3 §7.4.6.1 note 2) */
	dect_cc_set_call_status(&l->cc, l->cc.call_status,
				party_cc(b, ic->caller)->status_call_id);
	/*
	 * EN 300 444 table 23: <<SIGNAL>> 40H for an internal call, 41H for
	 * an external one, so the handset rings the internal pattern (stock
	 * does the same, A-StockTrace-1). The SETUP is built here, so the
	 * link's configured value is restored at once.
	 */
	dect_cc_set_signal(&l->cc, b->cfg.signal_on, DECT_SIGNAL_ALERT_PATTERN0);
	/*
	 * Who is calling (EN 300 444 §8.43/§8.44): the caller's terminal
	 * number, network-specific type in the private plan (table 82), and
	 * its handset name in the DECT alphabet (table 83), both "verified
	 * and passed". Bench test 5 (2026-10-28): the C430A showed no caller.
	 */
	ci_hs = ic->caller < 0 ? (int)b->cur : b->side[ic->caller].adopted;
	slot = ci_hs >= 0 ? dect_base_slot(b, (unsigned)ci_hs) : 0u;
	prev = l->cc.callinfo;
	dect_clip_init(&ic->clip);
	if (slot) {
		snprintf(num, sizeof(num), "%u", slot);
		dect_base_hs_name(b, (unsigned)ci_hs, name, sizeof(name));
		(void)dect_clip_set_number(&ic->clip, num,
				DECT_CLIP_TON_NETWORK_SPECIFIC,
				DECT_CLIP_NPI_PRIVATE, DECT_CLIP_PRES_ALLOWED,
				DECT_CLIP_SCR_USER_PASSED);
		(void)dect_clip_set_name(&ic->clip, name,
				DECT_CLIP_ALPHABET_DECT, DECT_CLIP_PRES_ALLOWED,
				DECT_CLIP_SCR_USER_PASSED);
	}
	/*
	 * A Part 3 target: the call id and CS call setup, no line (an
	 * internal call has none), as stock sends the A540: 7e 06 01 00 81 02
	 * 01 81 [CR A-0125]; TS 102 527-3 §7.4.6.3.
	 */
	dect_callinfo_init(&ci);
	if (b->cap[ic->target].known && b->cap[ic->target].part3 &&
	    !b->cfg.no_call_status) {
		ci.has_call_id = 1;
		ci.call_id = l->cc.status_call_id;
		ci.has_call_status = 1;
		ci.call_status = DECT_CS_CALL_SETUP;
	}
	dect_cc_set_callinfo(&l->cc, &ci);
	/*
	 * A Part 3 target gets the number in the SETUP and the name in a
	 * {CC-INFO} once it rings, as stock does [CR A-0130 §1]: the A540
	 * took a name in the SETUP for "via Intern DECT" (bench test 11).
	 */
	ic->name_info = slot && ci.has_call_status && ic->clip.name[0];
	num_only = ic->clip;
	if (ic->name_info)
		num_only.name[0] = '\0';
	r = dect_cc_setup_id(&l->cc, port, plen, fixed, flen,
			     dect_base_basic_svc(b, ic->target,
						 b->cfg.narrowband ? 0x90u
							: DECT_BASE_IC_SVC),
			     slot ? &num_only : NULL);
	dect_cc_set_callinfo(&l->cc, &prev);
	if (r) {
		dect_cc_set_signal(&l->cc, b->cfg.signal_on,
				   b->cfg.signal_value);
		dect_base_log(b, "intern: could not queue the SETUP to the "
			      "target");
		return;
	}
	if (slot)
		dect_base_log(b, "intern: caller shown as %s / \"%s\"%s", num,
			      name, ci.has_call_status ?
			      ", call id and CS call setup (Part 3)" : "");
	dect_cc_set_signal(&l->cc, b->cfg.signal_on, b->cfg.signal_value);
	ic->tside = p;
	ic->st = DECT_BASE_IC_SETUP;
	dect_base_log(b, "intern: the target %s on %s link %u -- SETUP sent",
		      how, p == -1 ? "the main" : "side",
		      dect_base_party_link(b, p)->handle);
	dect_base_drain(b, p, now);
}

void dect_base_ic_end(struct dect_base *b, const char *why)
{
	dect_base_log(b, "intern: call ended (%s)", why);
	b->ic.st = DECT_BASE_IC_IDLE;
	b->ic.tside = DECT_BASE_IC_NONE;
}

/*
 * The internal caller's identity for a Part 3 target, after the SETUP:
 * {CC-INFO} with <<CALLING-PARTY-NUMBER>>, <<CALLING-PARTY-NAME>> in UTF-8
 * (stock's octet 3 is 05H) and <<CALL-INFORMATION>> with the call id only,
 * as stock sends the A540 [CR A-0130 §1].
 */
static void ic_name_info(struct dect_base *b, struct dect_cc *t, uint64_t now)
{
	struct dect_base_ic *ic = &b->ic;
	struct dect_clip id = ic->clip;
	struct dect_callinfo ci;
	uint8_t ies[96];
	size_t off = 0, next;

	ic->name_info = 0;
	if (dect_clip_set_name(&id, ic->clip.name, DECT_CLIP_ALPHABET_UTF8,
			       DECT_CLIP_PRES_ALLOWED,
			       DECT_CLIP_SCR_USER_PASSED))
		id = ic->clip;			/* keep the DECT alphabet */
	next = dect_clip_put_number(ies, sizeof(ies), off, &id);
	if (next)
		off = next;
	next = dect_clip_put_name(ies, sizeof(ies), off, &id, 0);
	if (next)
		off = next;
	dect_callinfo_init(&ci);
	ci.has_call_id = 1;
	ci.call_id = t->status_call_id;
	next = dect_callinfo_put(ies, sizeof(ies), off, &ci);
	if (next)
		off = next;
	if (!off || dect_cc_info(t, ies, off)) {
		dect_base_log(b, "intern: could not queue the caller's name");
		return;
	}
	dect_base_drain(b, ic->tside, now);
	dect_base_log(b, "intern: caller's name \"%s\" sent in CC-INFO (Part "
		      "3, A-0130)", ic->clip.name);
}

void dect_base_ic_tick(struct dect_base *b, uint64_t now)
{
	struct dect_base_ic *ic = &b->ic;
	struct dect_cc *c, *t;
	unsigned k;

	if (ic->st == DECT_BASE_IC_IDLE)
		return;
	c = party_cc(b, ic->caller);
	if (!dect_base_party_active(b, ic->caller) || !dect_cc_busy(c)) {
		if (ic->tside != DECT_BASE_IC_NONE &&
		    dect_base_party_active(b, ic->tside) &&
		    dect_cc_busy(party_cc(b, ic->tside))) {
			(void)dect_cc_release(party_cc(b, ic->tside));
			dect_base_drain(b, ic->tside, now);
		}
		dect_base_ic_end(b, "the caller hung up");
		return;
	}
	if (ic->st == DECT_BASE_IC_PAGING) {
		for (k = 0; k < DECT_BASE_SIDES &&
			    ic->st == DECT_BASE_IC_PAGING; k++)
			if (b->side[k].active &&
			    b->side[k].adopted == ic->target &&
			    !dect_cc_busy(&b->side[k].link.cc))
				dect_base_ic_setup(b, (int)k, now,
					"came up (adopted, no page response)");
		/*
		 * or the main link, if the caller is not on it (finding 5) --
		 * only once it has adopted the target. A main link that has
		 * adopted no one is not presumed to serve the current record:
		 * whichever handset connects next would get the SETUP (the
		 * deregistration did exactly that on the bench, 2026-10-28).
		 * The target's page response is matched by identity by the
		 * host.
		 */
		if (ic->st == DECT_BASE_IC_PAGING && ic->caller != -1 &&
		    b->main_link.active &&
		    !dect_cc_busy(&b->main_link.link.cc) &&
		    b->main_link.link.mm.adopted == ic->target)
			dect_base_ic_setup(b, -1, now,
				"came up (adopted, no page response)");
		if (ic->st != DECT_BASE_IC_PAGING)
			return;
		if (now >= ic->page_at) {
			if (ic->pages >= DECT_BASE_IC_PAGES) {
				(void)dect_cc_release(c);
				dect_base_ic_end(b, "the target did not answer "
						 "the page");
				return;
			}
			ic_page(b);
			ic->page_at = now + DECT_BASE_IC_PAGE_MS;
		}
		return;
	}
	if (ic->tside == DECT_BASE_IC_NONE ||
	    !dect_base_party_active(b, ic->tside) ||
	    !dect_cc_busy(party_cc(b, ic->tside))) {
		(void)dect_cc_release(c);
		dect_base_ic_end(b, "the target hung up or refused");
		return;
	}
	t = party_cc(b, ic->tside);
	if (ic->st == DECT_BASE_IC_SETUP && t->state == DECT_CC_ST_RINGING) {
		(void)dect_cc_alert(c);
		ic->st = DECT_BASE_IC_RINGING;
		dect_base_log(b, "intern: the target rings -- ringback to the "
			      "caller");
		if (ic->name_info)
			ic_name_info(b, t, now);
	}
	if (ic->st != DECT_BASE_IC_UP && t->state == DECT_CC_ST_ACTIVE &&
	    dect_base_call_auth_ok(b, &dect_base_party_link(b, ic->tside)->link)) {
		(void)dect_cc_connect(c);
		ic->st = DECT_BASE_IC_UP;
		dect_base_log(b, "intern: the target answered -- connected, "
			      "bridging");
	}
}

void dect_base_call_clear(struct dect_base *b, struct dect_base_call *c)
{
	/*
	 * With ring_ms=0 the ring runs only while ring.now is set, so a ring
	 * left in PAGING would hold the main link "busy" for good (code
	 * review, 2026-10-29).
	 */
	if (c->party == -1) {
		b->ring.now = 0;
		b->ring.watch = 0;
		if (b->ring.state != DECT_BASE_RING_IDLE)
			b->ring.state = DECT_BASE_RING_DONE;
	}
	c->id = 0;
}

int dect_base_main_free(struct dect_base *b)
{
	return !dect_base_call_on(b, -1) &&
	       !(b->main_link.active && dect_cc_busy(&b->main_link.link.cc)) &&
	       (b->ring.state == DECT_BASE_RING_IDLE ||
		b->ring.state == DECT_BASE_RING_DONE);
}

void dect_base_ring_main(struct dect_base *b, struct dect_base_call *c,
			 unsigned t)
{
	c->party = -1;
	b->ring.hs = t;
	b->ring.state = DECT_BASE_RING_IDLE;
	b->ring.now = 1;
	b->ring.requeues = 0;
	b->ring.setup_tries = 0;
	b->ring.pages = 0;
	b->ring.wait_since = 0;
}

static void side_ring_page(struct dect_base *b, struct dect_base_call *c)
{
	dect_base_page(b, dect_base_page_info(b, c->narrow), c->tpui);
	c->pages++;
	dect_base_log(b, "bridge: paged DECT %u for call %u, attempt %u of %u",
		      dect_base_slot(b, (unsigned)c->target), c->id, c->pages,
		      DECT_BASE_IC_PAGES);
}

void dect_base_side_ring_start(struct dect_base *b, struct dect_base_call *c,
			       unsigned t, uint64_t now)
{
	struct dect_mm_cfg cfg;
	struct dect_mm mm;

	memset(&cfg, 0, sizeof(cfg));
	cfg.sub_slot = (uint8_t)dect_base_slot(b, t);
	dect_mm_init(&mm, &cfg);
	dect_mm_tpui(&mm, c->tpui);
	c->party = DECT_BASE_PARTY_PAGING;
	c->target = (int)t;
	c->pages = 0;
	c->page_at = now;
}

static int side_ring_part3(const struct dect_base *b, unsigned t)
{
	const struct dect_callinfo *o = &b->cfg.callinfo;

	return !o->has_line && !o->has_call_id && !o->has_call_status &&
	       !b->cfg.no_call_status && b->cap[t].known && b->cap[t].part3;
}

void dect_base_side_ring_setup(struct dect_base *b, struct dect_base_call *c,
			       unsigned k, uint64_t now, const char *how)
{
	struct dect_link *l = &b->side[k].link;
	unsigned t = (unsigned)c->target;
	const struct dect_hs_profile *hp = &b->prof[t];
	struct dect_callinfo prev = l->cc.callinfo, ci;
	struct dect_clip clip = c->clip;
	const uint8_t *port, *fixed;
	size_t plen, flen;
	uint8_t id;
	int r;

	dect_base_cc_ids(b, t, &port, &plen, &fixed, &flen);
	if (!plen || dect_cc_busy(&l->cc))
		return;
	dect_cc_set_next_ti(&l->cc, 0);
	id = dect_base_call_id(b, l);
	if (id)
		dect_cc_set_call_status(&l->cc, l->cc.call_status, id);
	dect_callinfo_init(&ci);
	if (side_ring_part3(b, t)) {
		ci.has_line = 1;
		ci.line_id = (uint8_t)c->line;
		ci.has_line_type = 1;
		ci.line_type = DECT_CALLINFO_NDT_DELAY;
		ci.has_call_id = 1;
		ci.call_id = l->cc.status_call_id;
		ci.has_call_status = 1;
		ci.call_status = DECT_CS_CALL_SETUP;
	}
	dect_cc_set_callinfo(&l->cc, &ci);
	if (!b->cfg.signal_set &&
	    hp->cc_signal_external != DECT_SIGNAL_ALERT_PATTERN1)
		dect_cc_set_signal(&l->cc, 1, hp->cc_signal_external);
	if (hp->cc_name_method == 1 || hp->cc_name_method == 3)
		clip.name[0] = '\0';		/* display text or none */
	r = dect_cc_setup_id(&l->cc, port, plen, fixed, flen,
			     dect_base_ring_svc(b, t, c->narrow), &clip);
	dect_cc_set_callinfo(&l->cc, &prev);
	dect_cc_set_signal(&l->cc, b->cfg.signal_on, b->cfg.signal_value);
	if (r) {
		dect_base_log(b, "bridge: could not queue the SETUP to DECT %u "
			      "-- trying again", dect_base_slot(b, t));
		return;
	}
	c->party = (int)k;
	dect_base_log(b, "bridge: call %u: DECT %u %s on side link %u -- SETUP "
		      "sent%s", c->id, dect_base_slot(b, t), how,
		      b->side[k].handle, ci.has_call_status
		      ? " (full call information, Part 3)" : "");
	dect_base_drain(b, (int)k, now);
}

void dect_base_side_ring_tick(struct dect_base *b, struct dect_base_call *c,
			      uint64_t now)
{
	unsigned k;

	/* the main link's call ended meanwhile: ring there, as usual */
	if (dect_base_main_free(b)) {
		dect_base_log(b, "bridge: call %u: the main link is free -- "
			      "ringing DECT %u there", c->id,
			      dect_base_slot(b, (unsigned)c->target));
		b->clip = c->clip;
		dect_base_ring_main(b, c, (unsigned)c->target);
		return;
	}
	for (k = 0; k < DECT_BASE_SIDES && c->party == DECT_BASE_PARTY_PAGING;
	     k++)
		if (b->side[k].active && b->side[k].adopted == c->target &&
		    !dect_cc_busy(&b->side[k].link.cc))
			dect_base_side_ring_setup(b, c, k, now, "came up "
				"(adopted, no page response)");
	if (c->party != DECT_BASE_PARTY_PAGING || now < c->page_at)
		return;
	if (c->pages >= DECT_BASE_IC_PAGES) {
		dect_base_log(b, "bridge: call %u in -- DECT %u did not answer "
			      "the page", c->id,
			      dect_base_slot(b, (unsigned)c->target));
		dect_base_call_event(b, c, "released", "by", "base");
		dect_base_call_clear(b, c);
		return;
	}
	side_ring_page(b, c);
	c->page_at = now + DECT_BASE_IC_PAGE_MS;
}

void dect_base_side_ring_call_tick(struct dect_base *b,
				   struct dect_base_call *c, uint64_t now)
{
	struct dect_cc *cc = party_cc(b, c->party);
	unsigned t = (unsigned)c->target;

	if (!c->info_sent && cc->state == DECT_CC_ST_RINGING) {
		c->info_sent = 1;
		if (dect_base_name_info(b, cc, t, &c->clip) > 0)
			dect_base_drain(b, c->party, now);
	}
	if (!c->cs_sent && cc->state == DECT_CC_ST_ACTIVE &&
	    side_ring_part3(b, t)) {
		struct dect_callinfo ci;
		uint8_t ie[16];
		size_t n;

		c->cs_sent = 1;
		dect_callinfo_init(&ci);
		ci.has_call_id = 1;
		ci.call_id = cc->status_call_id;
		ci.has_call_status = 1;
		ci.call_status = DECT_CS_CALL_CONNECT;
		n = dect_callinfo_put(ie, sizeof(ie), 0, &ci);
		if (n && !dect_cc_info(cc, ie, n))
			dect_base_drain(b, c->party, now);
	}
}

int dect_base_name_info(struct dect_base *b, struct dect_cc *cc, unsigned hs,
			const struct dect_clip *clip)
{
	const struct dect_hs_profile *hp = &b->prof[hs];
	uint8_t ie[96];
	size_t n;

	if (b->cfg.info_ie_len ||
	    (hp->cc_name_method != 1 && hp->cc_name_method != 2) ||
	    (!clip->name[0] && !clip->number[0]))
		return 0;
	n = dect_profile_display(hp, clip->name[0] ? clip->name : clip->number,
				 ie, sizeof(ie));
	if (!n || dect_cc_info(cc, ie, n)) {
		dect_base_log(b, "call: profile display text: could not queue "
			      "it");
		return -1;
	}
	dect_base_log(b, "call: profile display text sent in CC-INFO (%zu "
		      "octets) on ALERTING", n);
	return 1;
}

void dect_base_info_tick(struct dect_base *b, uint64_t now)
{
	struct dect_base_link *bl = &b->main_link;
	struct dect_cc *cc = &bl->link.cc;
	int r;

	if (!bl->active || !dect_cc_busy(cc)) {
		bl->info_sent = 0;
		return;
	}
	if (bl->info_sent || !cc->incoming || cc->state != DECT_CC_ST_RINGING)
		return;
	r = b->n_hs ? dect_base_name_info(b, cc, b->ring.hs, &b->clip) : 0;
	if (r) {
		bl->info_sent = 1;
		if (r > 0)
			dect_base_drain(b, -1, now);
		return;
	}
	if (!b->cfg.info_ie_len)
		return;
	bl->info_sent = 1;
	if (dect_cc_info(cc, b->cfg.info_ie, b->cfg.info_ie_len)) {
		dect_base_log(b, "call: infoie: could not queue the CC-INFO");
		return;
	}
	dect_base_log(b, "call: infoie: CC-INFO with %zu bytes of IEs sent on "
		      "ALERTING", b->cfg.info_ie_len);
	dect_base_drain(b, -1, now);
}

/*
 * Parallel calls on one link, for a Part 3 handset (TS 102 527-3 §7.4.3):
 * call waiting, accepting (holding or releasing the active call) and
 * rejecting it, hold, resume, toggle, releasing one call, and a second
 * call out (1CH 15H + number). The link keeps one CC transaction; the
 * other calls ride it in {CC-INFO} with their own call ids, and the lane
 * carries whichever is in CS call connect. Keypad digits during a call
 * are its DTMF (S3s).
 */
static const char *cs_name(uint8_t cs)
{
	switch (cs) {
	case DECT_CS_CALL_SETUP:	return "waiting";
	case DECT_CS_CALL_SETUP_ACK:	return "being dialled";
	case DECT_CS_CALL_PROC:		return "dialled";
	case DECT_CS_CALL_ALERTING:	return "ringing at the far end";
	case DECT_CS_CALL_CONNECT:	return "connected";
	case DECT_CS_CALL_HOLD:		return "on hold";
	default:			return "released";
	}
}

/* Tell the handset call c's new status (§7.4.6.4) in {CC-INFO}. */
void dect_base_par_status(struct dect_base *b, struct dect_base_call *c,
			  uint8_t cs, uint64_t now)
{
	struct dect_cc *cc = party_cc(b, c->party);

	c->cs = cs;
	if (!c->dect_id)
		c->dect_id = cc->status_call_id;
	if (dect_cc_call_status(cc, c->dect_id, cs, DECT_CC_NO_REASON,
				DECT_CC_NO_SIGNAL) == 0)
		dect_base_drain(b, c->party, now);
	dect_base_log(b, "bridge: call %u %s on DECT %u (call id %u)", c->id,
		      cs_name(cs), dect_base_party_slot(b, c->party),
		      c->dect_id);
}

/* §7.4.3.4: a request on call id id failed -- its status again, a reason. */
static void par_nack(struct dect_base *b, int p, uint8_t id, uint8_t reason,
		     uint64_t now)
{
	const struct dect_base_call *c = dect_base_call_by_dect(b, p, id);

	if (dect_cc_call_status(party_cc(b, p), id, c ? c->cs : DECT_CS_IDLE,
				reason, DECT_SIGNAL_NEGATIVE_ACK_TONE) == 0)
		dect_base_drain(b, p, now);
	dect_base_log(b, "bridge: DECT %u's request on call id %u refused",
		      dect_base_party_slot(b, p), id);
}

/* The active call on link p goes on hold, at the far end too. */
static void par_hold(struct dect_base *b, int p, uint64_t now)
{
	struct dect_base_call *a = dect_base_call_active(b, p);

	if (!a)
		return;
	dect_base_par_status(b, a, DECT_CS_CALL_HOLD, now);
	dect_base_call_event(b, a, "hold", NULL, NULL);
}

/* Call c becomes the active one; verb, if any, tells the bridge. */
void dect_base_par_connect(struct dect_base *b, struct dect_base_call *c,
			   const char *verb, uint64_t now)
{
	dect_base_par_status(b, c, DECT_CS_CALL_CONNECT, now);
	c->dtmf = 1;
	if (verb)
		dect_base_call_event(b, c, verb, NULL, NULL);
}

/* The handset table index link p serves, or -1. */
int dect_base_party_index(const struct dect_base *b, int p)
{
	int a;

	if (p == -1) {
		a = b->main_link.link.mm.adopted;
		return a >= 0 ? a : (int)b->cur;
	}
	return p >= 0 ? b->side[p].adopted : -1;
}

/*
 * The link a call to handset t can wait on: its one call bridged and
 * connected, to a handset that takes call status (Part 3).
 * DECT_BASE_PARTY_NONE if there is none.
 */
int dect_base_waiting_party(struct dect_base *b, unsigned t)
{
	int p;

	for (p = -1; p < (int)DECT_BASE_SIDES; p++) {
		const struct dect_cc *cc;

		if (!dect_base_party_active(b, p) ||
		    dect_base_party_index(b, p) != (int)t)
			continue;
		cc = party_cc(b, p);
		if (cc->state == DECT_CC_ST_ACTIVE && cc->call_status &&
		    dect_base_call_active(b, p) &&
		    dect_base_calls_on(b, p) == 1)
			return p;
	}
	return DECT_BASE_PARTY_NONE;
}

/* Call waiting (§7.4.3.5.2) for call-in c on link p. 0, or -1. */
int dect_base_call_waiting(struct dect_base *b, struct dect_base_call *c, int p,
			   const struct dect_clip *clip, uint64_t now)
{
	struct dect_callinfo ci;
	uint8_t id = dect_base_call_id(b, NULL);

	if (!id)
		return -1;
	dect_callinfo_init(&ci);
	ci.has_line = 1;
	ci.line_id = (uint8_t)c->line;
	ci.has_line_type = 1;
	ci.line_type = DECT_CALLINFO_NDT_DELAY;
	ci.has_call_id = 1;
	ci.call_id = id;
	if (dect_cc_call_waiting(party_cc(b, p), &ci, clip))
		return -1;
	c->party = p;
	c->par = 1;
	c->dect_id = id;
	c->cs = DECT_CS_CALL_SETUP;
	c->busy_seen = 1;
	c->rang = 1;
	dect_base_drain(b, p, now);
	dect_base_call_event(b, c, "ringing", NULL, NULL);
	return 0;
}

/* A parallel call's number is complete: to the bridge. */
static void par_dial_out(struct dect_base *b, struct dect_base_call *d,
			 uint64_t now)
			 {
			 struct dect_ctl_out o;

	d->dialing = 0;
	if (!b->bridge) {
		dect_base_par_status(b, d, DECT_CS_IDLE, now);
		dect_base_call_clear(b, d);
		return;
	}
	dect_ctl_begin(&o, "call-out");
	dect_ctl_add_uint(&o, "call", d->id);
	dect_ctl_add_uint(&o, "hs", dect_base_party_slot(b, d->party));
	dect_ctl_add_uint(&o, "line", d->line);
	/* the link's codec: a parallel call cannot change it */
	dect_ctl_add(&o, "codec", dect_cc_codec(party_cc(b, d->party)) ==
		     DECT_CC_CODEC_G722 ? "wb" : "nb");
	dect_ctl_add(&o, "number", d->num);
	if (b->ops && b->ops->event)
		b->ops->event(b->ctx, &o);
	dect_base_log(b, "bridge: call %u out from DECT %u to \"%s\" (a "
		      "parallel call)", d->id, dect_base_party_slot(b, d->party),
		      d->num);
	dect_base_par_status(b, d, DECT_CS_CALL_PROC, now);
}

/* A control code from the handset on link p (table 17). */
static void par_control(struct dect_base *b, int p, uint8_t code, uint8_t id,
			uint64_t now)
{
	struct dect_base_call *c = dect_base_call_by_dect(b, p, id);
	struct dect_base_call *a = dect_base_call_active(b, p);

	switch (code) {
	case 0x35u:			/* accept the waiting call ... */
	case 0x38u:			/* ... releasing the active one */
		if (!c || !c->par || c->cs != DECT_CS_CALL_SETUP || c->out)
			break;
		if (a && code == 0x38u) {
			dect_base_call_event(b, a, "released", "by", "hs");
			dect_base_par_status(b, a, DECT_CS_IDLE, now);
			dect_base_call_clear(b, a);
		} else {
			par_hold(b, p, now);
		}
		c->answered = 1;
		dect_base_par_connect(b, c, "answered", now);
		return;
	case 0x36u:			/* reject the waiting call */
		if (!c || !c->par || c->cs != DECT_CS_CALL_SETUP || c->out)
			break;
		dect_base_call_event(b, c, "released", "by", "hs");
		dect_base_par_status(b, c, DECT_CS_IDLE, now);
		dect_base_call_clear(b, c);
		return;
	case 0x31u:			/* toggle to c */
	case 0x42u:			/* resume c */
		if (!c || c->cs != DECT_CS_CALL_HOLD)
			break;
		par_hold(b, p, now);
		dect_base_par_connect(b, c, "resume", now);
		return;
	case 0x41u:			/* hold c, the active call */
		if (!c || c != a)
			break;
		par_hold(b, p, now);
		return;
	case 0x33u:			/* release c */
		if (!c || c->cs == DECT_CS_CALL_SETUP)
			break;		/* a waiting call is rejected */
		dect_base_call_event(b, c, "released", "by", "hs");
		dect_base_par_status(b, c, DECT_CS_IDLE, now);
		dect_base_call_clear(b, c);
		return;
	case 0x15u:			/* a parallel call out, its number next */
		/* a call id of its own, none while -a callid= pins one */
		c = dect_base_calls_on(b, p) < 2u && dect_base_call_id(b, NULL) ?
		    dect_base_call_new(b) : NULL;
		if (!c) {
			uint8_t nid = dect_base_call_id(b, NULL);
			struct dect_cc *cc = party_cc(b, p);

			if (nid) {
				/* §7.4.8.3: busy, then the call id freed */
				(void)dect_cc_call_status(cc, nid,
					DECT_CS_CALL_DISCONNECTING,
					DECT_CSR_SYSTEM_BUSY,
					DECT_SIGNAL_BUSY_TONE);
				(void)dect_cc_call_status(cc, nid,
					DECT_CS_IDLE, DECT_CC_NO_REASON,
					DECT_CC_NO_SIGNAL);
				dect_base_drain(b, p, now);
			} else {
				par_nack(b, p, a ? a->dect_id :
					 cc->status_call_id,
					 DECT_CSR_SYSTEM_BUSY, now);
			}
			dect_cc_clear_keypad(cc);	/* its number */
			return;
		}
		par_hold(b, p, now);
		c->id = dect_base_call_new_id(b);
		c->party = p;
		c->par = 1;
		c->out = 1;
		c->busy_seen = 1;
		c->since = now;
		c->line = DECT_BASE_LINE_ID;
		c->dect_id = dect_base_call_id(b, NULL);
		c->dialing = 1;
		c->dial_ms = now;
		dect_base_par_status(b, c, DECT_CS_CALL_SETUP_ACK, now);
		return;
	default:			/* internal, conference, transfer */
		par_nack(b, p, id ? id : a ? a->dect_id :
			 party_cc(b, p)->status_call_id,
			 DECT_CSR_CODE_NOT_SUPPORTED, now);
		if (code == 0x17u)		/* the internal number */
			dect_cc_clear_keypad(party_cc(b, p));
		return;
	}
	par_nack(b, p, id, DECT_CSR_CODE_FAILED, now);
}

/*
 * Link p's control codes and keypad: the number of a parallel call being
 * dialled (complete after the dial pause), else DTMF for the active call.
 * The keypad of a call still being set up is its number: left alone.
 */
void dect_base_par_tick(struct dect_base *b, int p, uint64_t now)
{
	struct dect_cc *cc = party_cc(b, p);
	struct dect_base_call *d = NULL, *a;
	const uint8_t *k;
	uint8_t code, id;
	size_t len, i, n;

	if (dect_cc_take_control(cc, &code, &id))
		par_control(b, p, code, id, now);
	for (i = 0; i < DECT_BASE_CALLS; i++)
		if (b->calls[i].id && b->calls[i].party == p &&
		    b->calls[i].dialing)
			d = &b->calls[i];
	k = dect_cc_keypad(cc, &len);
	if (d) {
		n = strlen(d->num);
		for (i = 0; i < len && n + 1u < sizeof(d->num); i++)
			if ((k[i] >= '0' && k[i] <= '9') || k[i] == '*' ||
			    k[i] == '#')
				d->num[n++] = (char)k[i];
		d->num[n] = '\0';
		if (len) {
			d->dial_ms = now;
			dect_cc_clear_keypad(cc);
		} else if (n && now - d->dial_ms >= b->cfg.dial_wait_ms) {
			par_dial_out(b, d, now);
		}
		return;
	}
	a = dect_base_call_active(b, p);
	if (!len)
		return;
	if (!a) {
		/*
		 * A call with a status here is past its own setup: every call
		 * is held or waiting, and the digits are for none of them.
		 */
		for (i = 0; i < DECT_BASE_CALLS; i++)
			if (b->calls[i].id && b->calls[i].party == p &&
			    b->calls[i].cs) {
				dect_cc_clear_keypad(cc);
				break;
			}
		return;
	}
	if (!a->dtmf)
		return;
	for (i = 0; i < len; i++) {
		char dig[2] = { (char)k[i], '\0' };

		if ((k[i] >= '0' && k[i] <= '9') || k[i] == '*' ||
		    k[i] == '#')
			dect_base_call_event(b, a, "digit", "d", dig);
	}
	dect_cc_clear_keypad(cc);
}

/* Handset t is in a call, or being rung for one. */
int dect_base_hs_in_call(const struct dect_base *b, unsigned t)
{
	unsigned k;
	int a = b->main_link.link.mm.adopted;

	if (b->main_link.active && dect_cc_busy(&b->main_link.link.cc) &&
	    (a >= 0 ? (unsigned)a : b->cur) == t)
		return 1;
	for (k = 0; k < DECT_BASE_SIDES; k++)
		if (b->side[k].active && b->side[k].adopted == (int)t &&
		    dect_cc_busy(&b->side[k].link.cc))
			return 1;
	for (k = 0; k < DECT_BASE_CALLS; k++)
		if (b->calls[k].id && !b->calls[k].out &&
		    ((b->calls[k].party == -1 && b->ring.hs == t) ||
		     (b->calls[k].party == DECT_BASE_PARTY_PAGING &&
		      b->calls[k].target == (int)t)))
			return 1;
	return 0;
}


/* Link p's call is proven (authfirst): what the bridge may act on. */
static int party_auth_ok(struct dect_base *b, int p)
{
	return dect_base_call_auth_ok(b, &dect_base_party_link(b, p)->link);
}

void dect_base_bridge_tick(struct dect_base *b, uint64_t now)
{
	unsigned i;
	int p;

	/*
	 * Control codes and keypad, once per link with a call of ours. A
	 * link without one is not in parallel mode: the library refuses its
	 * codes (§7.4.3.4), and none is left over for a later call.
	 */
	for (p = -1; p < (int)DECT_BASE_SIDES; p++) {
		int ours;

		if (!dect_base_party_active(b, p))
			continue;
		ours = dect_base_calls_on(b, p) != 0;
		dect_cc_set_parallel(party_cc(b, p), ours);
		if (ours && dect_cc_busy(party_cc(b, p)))
			dect_base_par_tick(b, p, now);
	}
	for (i = 0; i < DECT_BASE_CALLS; i++) {
		struct dect_base_call *c = &b->calls[i];
		struct dect_cc *cc;
		int busy;

		if (c->id && c->party == DECT_BASE_PARTY_PAGING)
			dect_base_side_ring_tick(b, c, now);
		if (!c->id)
			continue;
		busy = c->party != DECT_BASE_PARTY_PAGING &&
		       dect_base_party_active(b, c->party) &&
		       dect_cc_busy(party_cc(b, c->party));
		cc = busy ? party_cc(b, c->party) : NULL;
		/*
		 * A main-link call-in exists once the handset alerts: a SETUP
		 * lost to the handset's own call before that is rung again
		 * (dect_base_ring_watch()), not ended.
		 */
		if (cc && (c->out || c->party != -1 ||
			   (cc->incoming && (cc->state == DECT_CC_ST_RINGING ||
					     cc->state == DECT_CC_ST_ACTIVE))))
			c->busy_seen = 1;
		/*
		 * The link's own call connected: it is the active one, and
		 * what the keypad brings from now on is its DTMF (the number
		 * it was dialled with is dropped).
		 */
		if (cc && !c->par && !c->cs && cc->state == DECT_CC_ST_ACTIVE &&
		    party_auth_ok(b, c->party)) {
			c->cs = DECT_CS_CALL_CONNECT;
			c->dect_id = cc->status_call_id;
			c->dtmf = 1;
			dect_cc_clear_keypad(cc);
		}
		if (cc && !c->par && !c->out && cc->incoming) {
			/*
			 * The main link's ring.now stays set until the call is
			 * cleared: the ring's tick then watches it, and rings
			 * again after a collision with the handset's own call.
			 */
			if (c->party >= 0)
				dect_base_side_ring_call_tick(b, c, now);
			if (!c->rang && (cc->state == DECT_CC_ST_RINGING ||
					 cc->state == DECT_CC_ST_ACTIVE)) {
				c->rang = 1;
				dect_base_call_event(b, c, "ringing", NULL,
						     NULL);
			}
			if (!c->answered && cc->state == DECT_CC_ST_ACTIVE &&
			    party_auth_ok(b, c->party)) {
				c->answered = 1;
				dect_base_log(b, "bridge: call %u answered on "
					      "DECT %u", c->id,
					      dect_base_party_slot(b, c->party));
				dect_base_call_event(b, c, "answered", NULL,
						     NULL);
			}
		}
		if (c->busy_seen && !busy) {
			dect_base_log(b, "bridge: call %u ended on the handset",
				      c->id);
			dect_base_call_event(b, c, "released", "by", "hs");
			dect_base_call_clear(b, c);
			continue;
		}
		/* a call-in that never got a handset: the ring gave up */
		if (!c->out && !c->busy_seen &&
		    (now - c->since > 75000u ||
		     (c->party == -1 && b->ring.state == DECT_BASE_RING_DONE &&
		      !b->ring.watch && now - c->since > 5000u))) {
			dect_base_log(b, "bridge: call %u in -- no handset "
				      "answered the page", c->id);
			dect_base_call_event(b, c, "released", "by", "base");
			dect_base_call_clear(b, c);
		}
	}
}

int dect_base_listed(const struct dect_base *b, unsigned i)
{
	return i < b->n_hs && (i == b->cur || b->loaded[i]) && !b->absent[i];
}

int dect_base_contact_name(const struct dect_base *b, const char *num,
			   char *buf, size_t cap)
{
	const struct dect_contact *e;

	if (!b->contacts || !num || !*num)
		return 0;
	e = dect_contacts_find_number(b->contacts, num);
	if (!e)
		return 0;
	snprintf(buf, cap, "%s%s%s", e->first,
		 e->first[0] && e->name[0] ? " " : "", e->name);
	return 1;
}

/* remote-released's reason, as a CC release reason (§2.2 of the page) */
static uint8_t bridge_reason(const char *r)
{
	if (!r || !strcmp(r, "normal"))
		return DECT_CC_REASON_NONE;
	if (!strcmp(r, "busy"))
		return DECT_CC_REASON_USER_BUSY;
	if (!strcmp(r, "unreachable"))
		return DECT_CC_REASON_USER_UNKNOWN;
	if (!strcmp(r, "rejected"))
		return 0x15u;			/* user rejection */
	return DECT_CC_REASON_NONE;
}

static void reply(struct dect_base_reply *r, const char *err, const char *key,
		  unsigned val)
{
	r->err = err;
	r->key = key;
	r->val = val;
}

static void bridge_send(struct dect_base *b, struct dect_ctl_out *o)
{
	if (b->ops && b->ops->event)
		b->ops->event(b->ctx, o);
}

/* call-in "busy" to the bridge, why it is */
static void call_in_busy(struct dect_base *b, const char *sip, const char *why)
{
	struct dect_ctl_out o;

	dect_ctl_begin(&o, "busy");
	if (sip)
		dect_ctl_add(&o, "sip", sip);
	dect_ctl_add(&o, "why", why);
	bridge_send(b, &o);
}

static void call_in(struct dect_base *b, const struct dect_ctl_msg *m,
		    int from_bridge, uint64_t now, struct dect_base_reply *r)
{
	const char *num = dect_ctl_get(m, "number");
	const char *name = dect_ctl_get(m, "name");
	char book_name[2u * DECT_CONTACT_NAME_MAX];
	int book = 0;
	const char *sip = dect_ctl_get(m, "sip");
	const char *why = NULL, *codec;
	struct dect_ctl_out o;
	struct dect_base_call *c;
	struct dect_clip clip;
	int wp;
	unsigned want = 0, t = 0, line = DECT_BASE_LINE_ID;

	/* hs=N: the handset whose line was called (its own account) */
	if (dect_ctl_get(m, "hs")) {
		if (dect_ctl_get_uint(m, "hs", 1, DECT_MM_MAX_SLOT, &want)) {
			reply(r, "bad-hs", NULL, 0);
			return;
		}
		for (t = 0; t < b->n_hs; t++)
			if (dect_base_listed(b, t) &&
			    dect_base_slot(b, t) == want)
				break;
		if (t == b->n_hs) {
			reply(r, "no-handset", "hs", want);
			return;
		}
	}
	if (!from_bridge) {
		reply(r, "not-bridge", NULL, 0);
		return;
	}
	/* line=N: which of the handset's lines was called */
	if (dect_ctl_get(m, "line") &&
	    dect_ctl_get_uint(m, "line", 1, 2, &line))
		line = DECT_BASE_LINE_ID;
	/* codec=nb: the caller offered no G.722 */
	codec = dect_ctl_get(m, "codec");
	c = dect_base_call_new(b);
	r->sent = 1;
	/* a Part 3 handset in a bridged call: call waiting */
	wp = DECT_BASE_PARTY_NONE;
	if (b->n_hs && dect_base_hs_in_call(b, t)) {
		wp = c ? dect_base_waiting_party(b, t) : DECT_BASE_PARTY_NONE;
		if (wp == DECT_BASE_PARTY_NONE)
			why = "in-call";
	} else if (!c || (!dect_base_main_free(b) &&
			  (!b->cfg.two_calls || lanes(b, -1) <= 0))) {
		why = "channels";
	}
	if (why) {
		call_in_busy(b, sip, why);
		return;
	}
	/*
	 * A SIP From is whatever the caller wrote: user provided, not
	 * screened -- "network provided" told the handset the network
	 * vouched for it (security audit 2026-10-29, S1).
	 */
	dect_clip_init(&clip);
	if (num && *num)
		(void)dect_clip_set_number(&clip, num, DECT_CLIP_TON_UNKNOWN,
					   DECT_CLIP_NPI_UNKNOWN,
					   DECT_CLIP_PRES_ALLOWED,
					   DECT_CLIP_SCR_USER_UNSCREENED);
	/*
	 * No name from SIP: the Contact List's, as TS 102 527-3 7.4.10.5.1.3
	 * note 1 recommends ("First Name"), screened "user-provided,
	 * verified and passed" as EN 300 175-5 7.7.53 note 1 asks for a name
	 * from the FP's contact list.
	 */
	if ((!name || !*name) &&
	    dect_base_contact_name(b, num, book_name, sizeof(book_name))) {
		name = book_name;
		book = 1;
	}
	if (name && *name)
		(void)dect_clip_set_name(&clip, name, DECT_CLIP_ALPHABET_UTF8,
					 DECT_CLIP_PRES_ALLOWED,
					 book ? DECT_CLIP_SCR_USER_PASSED
					      : DECT_CLIP_SCR_USER_UNSCREENED);
	c->id = dect_base_call_new_id(b);
	c->line = line;
	c->narrow = codec && !strcmp(codec, "nb");
	c->since = now;
	c->target = (int)t;
	c->clip = clip;
	if (wp != DECT_BASE_PARTY_NONE) {
		if (dect_base_call_waiting(b, c, wp, &clip, now)) {
			c->id = 0;
			call_in_busy(b, sip, "in-call");
			return;
		}
	} else if (dect_base_main_free(b)) {
		b->clip = clip;
		dect_base_ring_main(b, c, t);
	} else {
		dect_base_side_ring_start(b, c, t, now);
	}
	dect_base_log(b, "bridge: call %u in from \"%s\" -- %s DECT %u%s",
		      c->id, num ? num : "", c->par ? "waiting on" : "ringing",
		      dect_base_slot(b, t), c->party == DECT_BASE_PARTY_PAGING
		      ? " on a side link (the main link is in a call)" : "");
	dect_ctl_begin(&o, "call-in-ok");
	dect_ctl_add_uint(&o, "call", c->id);
	if (sip)
		dect_ctl_add(&o, "sip", sip);
	bridge_send(b, &o);
}

int dect_base_ctl_call(struct dect_base *b, const struct dect_ctl_msg *m,
		       int from_bridge, uint64_t now,
		       struct dect_base_reply *r)
{
	struct dect_base_call *c;
	unsigned id;
	int p;

	memset(r, 0, sizeof(*r));
	if (!strcmp(m->verb, "call-in")) {
		call_in(b, m, from_bridge, now, r);
		return 1;
	}
	if (strcmp(m->verb, "remote-ringing") &&
	    strcmp(m->verb, "remote-answered") &&
	    strcmp(m->verb, "remote-released"))
		return 0;
	if (!from_bridge) {
		reply(r, "not-bridge", NULL, 0);
		return 1;
	}
	if (dect_ctl_get_uint(m, "call", 1, 127, &id) ||
	    !(c = dect_base_call_by_id(b, id))) {
		reply(r, "no-call", NULL, 0);
		return 1;
	}
	p = c->party;
	if (!strcmp(m->verb, "remote-released")) {
		const char *rs = dect_ctl_get(m, "reason");

		dect_base_log(b, "bridge: call %u released by the far end (%s)",
			      id, rs ? rs : "normal");
		if (p != DECT_BASE_PARTY_PAGING && dect_base_calls_on(b, p) > 1u) {
			/* the link carries another call: this one goes alone */
			dect_base_par_status(b, c, DECT_CS_IDLE, now);
			dect_base_call_clear(b, c);
			reply(r, NULL, "call", id);
			return 1;
		}
		dect_base_call_clear(b, c);	/* no "released" back */
		if (p == DECT_BASE_PARTY_PAGING) {
			reply(r, NULL, "call", id);
			return 1;		/* paging stops */
		}
		if (dect_base_party_active(b, p) && dect_cc_busy(party_cc(b, p)))
			(void)dect_cc_release_reason(party_cc(b, p),
						     bridge_reason(rs));
	} else if (!c->out || !dect_base_party_active(b, p)) {
		reply(r, "no-call", NULL, 0);
		return 1;
	} else if (c->par) {
		/* a parallel call out: its progress as call status */
		if (!strcmp(m->verb, "remote-ringing")) {
			if (c->cs == DECT_CS_CALL_PROC)
				dect_base_par_status(b, c, DECT_CS_CALL_ALERTING,
						     now);
		} else if (!dect_base_call_active(b, p)) {
			dect_base_par_connect(b, c, NULL, now);
		} else {
			dect_base_par_status(b, c, DECT_CS_CALL_HOLD, now);
			dect_base_call_event(b, c, "hold", NULL, NULL);
		}
	} else if (!strcmp(m->verb, "remote-ringing")) {
		if (party_cc(b, p)->state == DECT_CC_ST_PROCEEDING)
			(void)dect_cc_alert(party_cc(b, p));
	} else {
		const char *codec = dect_ctl_get(m, "codec");

		/*
		 * codec=nb: the far end speaks narrowband, so the handset does
		 * too (G.726, if it offered it) rather than the box converting
		 * its G.722.
		 */
		if (codec && !strcmp(codec, "nb") &&
		    dect_cc_prefer_narrowband(party_cc(b, p)))
			c->narrow = 1;
		dect_base_log(b, "bridge: call %u answered by the far end%s", id,
			      c->narrow ? " -- narrowband, G.726 to the handset"
					: "");
		(void)dect_cc_connect(party_cc(b, p));
	}
	if (dect_base_party_active(b, p))
		dect_base_drain(b, p, now);
	reply(r, NULL, "call", id);
	return 1;
}

/* The record link l serves, or -1 (S4a). */
int dect_base_hs_of_link(const struct dect_base *b, const struct dect_link *l)
{
	int i = l->mm.adopted;

	if (i < 0 && l == &b->main_link.link)
		i = (int)b->cur;
	return i >= 0 && (unsigned)i < b->n_hs ? i : -1;
}

/*
 * A link that has adopted record i, or NULL: a main link that has adopted
 * no one is not presumed to serve the current record -- a deregistration
 * sent on that went to whichever handset connected next (bench,
 * 2026-10-28: the C430A's location registration got the AS690H's
 * TERMINATE-REQUEST).
 */
struct dect_link *dect_base_link_known(struct dect_base *b, int i)
{
	unsigned k;

	if (b->main_link.active && b->main_link.link.mm.adopted == i)
		return &b->main_link.link;
	for (k = 0; k < DECT_BASE_SIDES; k++)
		if (b->side[k].active && b->side[k].link.mm.adopted == i)
			return &b->side[k].link;
	return NULL;
}

/*
 * List change notifications (TS 102 527-3 §7.4.10.2), a CISS {FACILITY}
 * with <<EVENTS-NOTIFICATION>> on a link that is up. Only to Part 3
 * handsets: the notifications are a CAT-iq Part 3 procedure, and a Part 1
 * or GAP handset was never sent one by us; what it would make of one is
 * untested. A handset with no link up learns at its next location
 * registration (the CLSS paging path is not built).
 */
int dect_base_notify_ok(const struct dect_base *b, const struct dect_link *l)
{
	int i = dect_base_hs_of_link(b, l);

	if (i >= 0 && b->prof[i].notify != DECT_PROF_AUTO)
		return b->prof[i].notify == DECT_PROF_ON;
	if (l->mm.peer_cap.present)
		return l->mm.peer_cap.ng_part3;
	/* a link up for a page or a call: what the handset said before */
	return i >= 0 && b->cap[i].known && b->cap[i].part3;
}

/*
 * After a location registration: one indication for the Line Settings
 * List per line the handset is attached to, to that handset only, with
 * the line "relating to" and the entries for that line, 1 (§7.4.10.2.1,
 * note 1; spec sweep M1.3).
 */
void dect_base_locate_notify(struct dect_base *b, struct dect_link *l)
{
	const struct dect_iwu *iwu = &l->cc.iwu;
	unsigned i;

	if (!dect_base_notify_ok(b, l))
		return;
	for (i = 0; i < iwu->n_lines; i++) {
		struct dect_event ev = { DECT_EVENT_LIST_CHANGE,
					 DECT_LIA_LIST_LINE_SETTINGS, 1 };
		uint8_t msg[DECT_LAPC_MSG_MAX];
		size_t n;

		if (!dect_iwu_attached(iwu, iwu->lines[i].id,
				       iwu->own_terminal_id))
			continue;
		n = dect_events_build_facility(msg, sizeof(msg), &ev, 1,
					       iwu->lines[i].id,
					       DECT_EVENT_LINE_RELATING_TO);
		if (n && dect_link_send_ciss(l, msg, n) == 0)
			dect_base_log(b, "reg: list change notice sent for "
				      "line %u (Line Settings List)",
				      iwu->lines[i].id);
	}
	/*
	 * One for the List of Supported Lists too (no line field, so no line;
	 * one entry). A handset keeps the copy it read once: the A540
	 * re-reads only 06 and 08 when it reconnects, so it never learned of
	 * the Contact List or the call lists until told (2026-10-30).
	 */
	if (iwu->contacts || iwu->calls) {
		struct dect_event ev = { DECT_EVENT_LIST_CHANGE,
					 DECT_LIA_LIST_SUPPORTED, 1 };
		uint8_t msg[DECT_LAPC_MSG_MAX];
		size_t n = dect_events_build_facility(msg, sizeof(msg), &ev, 1,
						      DECT_EVENT_NO_LINE, 0);

		if (n && dect_link_send_ciss(l, msg, n) == 0)
			dect_base_log(b, "reg: list change notice sent (List "
				      "of Supported Lists)");
	}
}

/*
 * Voice message waiting notifications (TS 102 527-3 §7.4.1.2, table 14)
 * to handset i on link l, for the lines in its mwi_due: the count with
 * the line, "external call" subtype. Part 3 handsets only, as the other
 * notifications (dect_base_notify_ok()). Returns the number queued.
 */
unsigned dect_base_mwi_send(struct dect_base *b, struct dect_link *l,
			    unsigned i)
{
	unsigned n, sent = 0;

	if (!dect_base_notify_ok(b, l)) {
		b->note[i].mwi_due = 0;
		return 0;
	}
	for (n = 1; n <= 2u; n++) {
		struct dect_event ev = { DECT_EVENT_MESSAGE_WAITING,
					 DECT_EVENT_VOICE,
					 b->note[i].mwi[n - 1u] };
		uint8_t msg[DECT_LAPC_MSG_MAX], bit = (uint8_t)(1u << (n - 1u));
		size_t len;

		if (!(b->note[i].mwi_due & bit))
			continue;
		len = dect_events_build_facility(msg, sizeof(msg), &ev, 1,
						 (uint8_t)n,
						 DECT_EVENT_LINE_EXTERNAL);
		if (!len || dect_link_send_ciss(l, msg, len))
			continue;	/* the queue is full: next time */
		b->note[i].mwi_due &= (uint8_t)~bit;
		sent++;
		dect_base_log(b, "mwi: DECT %u, line %u: %u message(s) waiting",
			      dect_base_slot(b, i), n, ev.multiplicity);
	}
	return sent;
}

/*
 * After a location registration: the waiting messages again, once per
 * line we know them for (§7.4.1.2, "notification update").
 */
void dect_base_mwi_locate(struct dect_base *b, struct dect_link *l)
{
	int i = dect_base_hs_of_link(b, l);

	if (i < 0 || !b->note[i].mwi_known)
		return;
	b->note[i].mwi_due = b->note[i].mwi_known;
	(void)dect_base_mwi_send(b, l, (unsigned)i);
}

/*
 * Missed call notifications (TS 102 527-3 §7.4.1.3, table 15) to handset i
 * on link l, for the lines in its mc_due: the unread missed calls on that
 * line, and with it the list change indication for the Missed Calls List
 * (its entries on the line), "line identifier for external call". Subtype
 * 01H only for a call just missed. Part 3 handsets only
 * (dect_base_notify_ok()). Nothing without the host's call lists.
 */
unsigned dect_base_missed_send(struct dect_base *b, struct dect_link *l,
			       unsigned i)
{
	unsigned n, sent = 0, tid = dect_base_slot(b, i);

	if (!b->call_lists || !dect_base_notify_ok(b, l)) {
		b->note[i].mc_due = b->note[i].mc_new = 0;
		return 0;
	}
	for (n = 1; n <= 2u; n++) {
		uint8_t bit = (uint8_t)(1u << (n - 1u));
		struct dect_event ev[2] = {
			{ DECT_EVENT_MISSED_CALL,
			  b->note[i].mc_new & bit ? 1u : 2u,
			  dect_calls_missed_unread(b->call_lists, tid, n) },
			{ DECT_EVENT_LIST_CHANGE, DECT_LIA_LIST_MISSED,
			  dect_calls_missed_total(b->call_lists, tid, n) },
		};
		uint8_t msg[DECT_LAPC_MSG_MAX];
		size_t len;

		if (!(b->note[i].mc_due & bit))
			continue;
		len = dect_events_build_facility(msg, sizeof(msg), ev, 2,
						 (uint8_t)n,
						 DECT_EVENT_LINE_EXTERNAL);
		if (!len || dect_link_send_ciss(l, msg, len))
			continue;	/* the queue is full: next time */
		b->note[i].mc_due &= (uint8_t)~bit;
		b->note[i].mc_new &= (uint8_t)~bit;
		sent++;
		dect_base_log(b, "missed: DECT %u, line %u: %u unread of %u "
			      "(%s)", tid, n, ev[0].multiplicity,
			      ev[1].multiplicity,
			      ev[0].subtype == 1u ? "a new one" : "update");
	}
	return sent;
}

/* The lines handset i has: 1, and 2 if it has a second. */
uint8_t dect_base_hs_lines(const struct dect_base *b, unsigned i)
{
	return (uint8_t)(b->line2[i] ? 3u : 1u);
}

/*
 * After a location registration: the missed calls once per line the
 * handset has (§7.4.1.3, subtype 02H).
 */
void dect_base_missed_locate(struct dect_base *b, struct dect_link *l)
{
	int i = dect_base_hs_of_link(b, l);

	if (i < 0)
		return;
	b->note[i].mc_due = dect_base_hs_lines(b, (unsigned)i);
	(void)dect_base_missed_send(b, l, (unsigned)i);
}

/* Could handset i hear notifications, from what it told us before? */
int dect_base_notify_known(const struct dect_base *b, unsigned i)
{
	if (b->prof[i].notify != DECT_PROF_AUTO)
		return b->prof[i].notify == DECT_PROF_ON;
	return b->cap[i].known && b->cap[i].part3;
}

/*
 * Due notifications to handset i: now if it has a link, else at the next
 * paging round (tu.want, the host's paging round).
 */
void dect_base_missed_due(struct dect_base *b, unsigned i, uint8_t lines,
			  int fresh)
{
	struct dect_link *l;

	lines &= dect_base_hs_lines(b, i);
	if (!lines || !b->loaded[i] || !dect_base_notify_known(b, i))
		return;
	b->note[i].mc_due |= lines;
	if (fresh)
		b->note[i].mc_new |= lines;
	l = dect_base_link_known(b, (int)i);
	if (l)
		(void)dect_base_missed_send(b, l, i);
	else
		b->page_want |= 1u << i;
}

/*
 * The Missed Calls List changed on a handset (read, deleted): every handset
 * hears the new counts (§7.4.1.3, "entry modified or deleted", subtype
 * 02H) -- one in a link at once, the others at the next paging round.
 */
void dect_base_missed_changed_tick(struct dect_base *b)
{
	unsigned i;

	if (!b->call_lists || !b->call_lists->missed_changed)
		return;
	b->call_lists->missed_changed = 0;
	for (i = 0; i < b->n_hs; i++)
		dect_base_missed_due(b, i, dect_base_hs_lines(b, i), 0);
}

/*
 * The Internal Names List changed -- a name saved, a handset added: tell
 * every handset that is up (§7.4.10.2.2: "at least ... that other PP", and
 * to all on an addition), no line, the number of entries.
 */
void dect_base_names_notify(struct dect_base *b)
{
	struct dect_link *links[1 + DECT_BASE_SIDES];
	unsigned k, n = 0;

	if (b->main_link.active)
		links[n++] = &b->main_link.link;
	for (k = 0; k < DECT_BASE_SIDES; k++)
		if (b->side[k].active)
			links[n++] = &b->side[k].link;
	for (k = 0; k < n; k++) {
		struct dect_event ev = { DECT_EVENT_LIST_CHANGE,
					 DECT_LIA_LIST_INTERNAL_NAMES,
					 links[k]->cc.iwu.n_handsets };
		uint8_t msg[DECT_LAPC_MSG_MAX];
		size_t len;

		if (!dect_base_notify_ok(b, links[k]))
			continue;
		len = dect_events_build_facility(msg, sizeof(msg), &ev, 1,
						 DECT_EVENT_NO_LINE, 0);
		if (len && dect_link_send_ciss(links[k], msg, len) == 0)
			dect_base_log(b, "names: list change notice sent to "
				      "DECT %u",
				      links[k]->cc.iwu.own_terminal_id);
	}
}


void dect_base_dereg_erase(struct dect_base *b, int i, const char *why)
{
	unsigned slot = dect_base_slot(b, (unsigned)i), k;
	uint8_t p[1 + DECT_MM_UAK_LEN + 1];

	dect_base_log(b, "dereg: DECT %u: %s", slot, why);
	if (b->ops && b->ops->forget)
		b->ops->forget(b->ctx, (unsigned)i, why);
	if (slot >= 1u && slot <= DECT_MM_MAX_SLOT) {
		memset(p, 0, sizeof(p));
		p[0] = (uint8_t)slot;
		chip_mail(b, 0x0247u, p, 3);		/* slot, 00, 00 */
		chip_mail(b, 0x020Au, p, sizeof(p));	/* slot, 17 zeros */
		dect_base_log(b, "dereg: chip slot %u cleared (0x0247, 0x020A "
			      "zeros)", slot);
	}
	b->loaded[i] = 0;
	b->hs[i].uak_valid = 0;
	b->absent[i] = 1;		/* free for a new enrolment */
	b->name[i][0] = '\0';
	b->cap[i].known = 0;
	memset(b->note[i].mwi, 0, sizeof(b->note[i].mwi));
	b->note[i].mwi_known = b->note[i].mwi_due = 0;
	b->page_want &= ~(1u << i);
	if ((unsigned)i == b->cur) {
		b->rec.uak_valid = 0;
		b->rec.port_id_len = 0;
		b->rec.cc_port_id_len = 0;
		b->rec.cc_fixed_id_len = 0;
	}
	if (b->main_link.active)
		(void)dect_iwu_remove_handset(&b->main_link.link.cc.iwu,
					      (uint8_t)slot);
	for (k = 0; k < DECT_BASE_SIDES; k++)
		if (b->side[k].active)
			(void)dect_iwu_remove_handset(&b->side[k].link.cc.iwu,
						      (uint8_t)slot);
	if (b->ops && b->ops->mask)
		b->ops->mask(b->ctx);		/* the mask without it */
	dect_base_names_notify(b);		/* §7.4.10.2.2: one removed */
}

void dect_base_dereg_begin(struct dect_base *b, unsigned slot, uint64_t now,
			   const char *who)
{
	struct dect_base_dereg *d = &b->dereg;
	unsigned i;
	struct dect_link *l;
	struct dect_mm_cfg cfg;
	struct dect_mm mm;

	for (i = 0; i < b->n_hs; i++)
		if (b->loaded[i] && dect_base_slot(b, i) == slot)
			break;
	if (!slot || i == b->n_hs) {
		dect_base_log(b, "dereg: %s, but no handset we hold is in slot "
			      "%u", who, slot);
		return;
	}
	dect_base_log(b, "dereg: DECT %u, asked by %s", slot, who);
	if (d->st != DECT_BASE_DEREG_IDLE) {
		unsigned k;

		for (k = 0; k < d->nq; k++)
			if (d->queue[k] == slot)
				break;
		if (k == d->nq && d->nq < DECT_MM_MAX_SLOT &&
		    (int)i != d->target)
			d->queue[d->nq++] = slot;
		dect_base_log(b, "dereg: one at a time -- DECT %u waits its "
			      "turn (%u waiting)", slot, d->nq);
		return;
	}
	d->target = (int)i;
	d->pages = 0;
	d->page_at = now;
	l = dect_base_link_known(b, (int)i);
	if (l && dect_mm_terminate(&l->mm) == 0) {
		d->st = DECT_BASE_DEREG_REQUESTED;
		dect_base_log(b, "dereg: DECT %u: ACCESS-RIGHTS-TERMINATE-"
			      "REQUEST on its link", slot);
		return;
	}
	memset(&cfg, 0, sizeof(cfg));
	cfg.sub_slot = (uint8_t)slot;
	dect_mm_init(&mm, &cfg);
	dect_mm_tpui(&mm, d->tpui);
	d->st = DECT_BASE_DEREG_PAGING;
	dect_base_log(b, "dereg: DECT %u has no link -- paging it", slot);
}

void dect_base_dereg_tick(struct dect_base *b, uint64_t now)
{
	struct dect_base_dereg *d = &b->dereg;
	struct dect_link *l;
	unsigned k;
	int i;

	/* a handset that deregistered itself: erase it the same way */
	for (k = 0; k <= DECT_BASE_SIDES; k++) {
		struct dect_link *pl = k < DECT_BASE_SIDES
			? (b->side[k].active ? &b->side[k].link : NULL)
			: (b->main_link.active ? &b->main_link.link : NULL);

		if (!pl || !pl->mm.term_by_pt)
			continue;
		pl->mm.term_by_pt = 0;
		i = pl->mm.adopted >= 0 ? pl->mm.adopted
		    : (pl == &b->main_link.link ? (int)b->cur : -1);
		if (i >= 0 && (unsigned)i < b->n_hs && b->loaded[i])
			dect_base_dereg_erase(b, i, "the handset deregistered "
					      "itself (accepted)");
	}
	if (d->st == DECT_BASE_DEREG_IDLE && d->nq) {
		unsigned slot = d->queue[0];

		d->nq--;
		memmove(d->queue, d->queue + 1, d->nq * sizeof(d->queue[0]));
		dect_base_dereg_begin(b, slot, now, "the queue");
	}
	if (d->st == DECT_BASE_DEREG_IDLE)
		return;
	i = d->target;
	l = dect_base_link_known(b, i);
	if (d->st == DECT_BASE_DEREG_PAGING) {
		if (l && dect_mm_terminate(&l->mm) == 0) {
			d->st = DECT_BASE_DEREG_REQUESTED;
			dect_base_log(b, "dereg: DECT %u came up -- "
				      "ACCESS-RIGHTS-TERMINATE-REQUEST",
				      dect_base_slot(b, (unsigned)i));
			return;
		}
		if (now < d->page_at)
			return;
		if (d->pages >= DECT_BASE_DEREG_PAGES) {
			d->st = DECT_BASE_DEREG_IDLE;
			dect_base_dereg_erase(b, i, "not reachable (paged 10 "
					      "times): deleted WITHOUT its "
					      "confirmation");
			return;
		}
		/* stock's page for this: C-plane only, the TPUI */
		dect_base_page(b, 0x08, d->tpui);
		d->pages++;
		d->page_at = now + DECT_BASE_DEREG_PAGE_MS;
		return;
	}
	/* REQUESTED */
	if (!l) {
		d->st = DECT_BASE_DEREG_IDLE;
		dect_base_dereg_erase(b, i, "its link went down before it "
				      "answered: deleted WITHOUT its "
				      "confirmation");
		return;
	}
	switch (l->mm.term_state) {
	case DECT_MM_TERM_ACCEPTED:
		d->st = DECT_BASE_DEREG_IDLE;
		dect_base_dereg_erase(b, i, "the handset confirmed "
				      "(TERMINATE-ACCEPT)");
		break;
	case DECT_MM_TERM_REJECTED:
		d->st = DECT_BASE_DEREG_IDLE;
		dect_base_dereg_erase(b, i, "the handset REFUSED "
				      "(TERMINATE-REJECT): deleted from the "
				      "base anyway");
		break;
	case DECT_MM_TERM_FAILED:
		d->st = DECT_BASE_DEREG_IDLE;
		dect_base_dereg_erase(b, i, "no answer to the request, sent "
				      "twice (<MM_access.2>): deleted WITHOUT "
				      "its confirmation");
		break;
	default:
		break;
	}
}

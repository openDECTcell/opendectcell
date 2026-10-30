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

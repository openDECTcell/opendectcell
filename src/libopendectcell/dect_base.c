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

// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for src/libopendectcell/dect_base.c: the handset table and its working
 * copy (docs/stack-in-library.md, stage S1).
 */
#include "check.h"
#include "../src/libopendectcell/dect_base.h"

#include <string.h>

static unsigned saved_i = 99, saved_slot;

static void save(void *ctx, unsigned i, const struct dect_hs_rec *r)
{
	CHECK(ctx == &saved_i);
	saved_i = i;
	saved_slot = r->slot;
}

static const struct dect_base_ops ops = { .save = save };

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
	b.ops = &ops;
	b.ctx = &saved_i;
	dect_base_changed(&b);
	CHECK_EQ(saved_i, 2u);
	CHECK_EQ(saved_slot, 3u);
	return CHECK_DONE();
}

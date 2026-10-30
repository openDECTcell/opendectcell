/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The base station above the links: what dect-stubd holds today and the
 * library takes over stage by stage (docs/stack-in-library.md), so the box
 * and the controller run one implementation.
 *
 * Stage S1a: a handset's record, the part of a subscription that outlives a
 * connection and a restart. The host stores it (dect-stubd in its -s files,
 * the controller in its database); the stack reads and changes it.
 *
 * No I/O and no allocation.
 */
#ifndef OPENDECTCELL_DECT_BASE_H
#define OPENDECTCELL_DECT_BASE_H

#include <stddef.h>
#include <stdint.h>

#include "dect_cc.h"
#include "dect_mm.h"

struct dect_hs_rec {
	unsigned slot;				/* chip subscription slot, 1..6 */
	uint8_t  uak[DECT_MM_UAK_LEN];		/* the long-term key */
	int      uak_valid;
	/*
	 * The MM <<PORTABLE-IDENTITY>> content: the identity gate for location
	 * registration and access rights (the chip admits anything locked to
	 * our RFPI, so this is the only gate).
	 */
	uint8_t  port_id[DECT_MM_PORTID_MAX];
	size_t   port_id_len;
	/* The identities an FP-originated {CC-SETUP} carries. */
	uint8_t  cc_port_id[DECT_CC_ID_MAX];
	size_t   cc_port_id_len;
	uint8_t  cc_fixed_id[DECT_CC_ID_MAX];
	size_t   cc_fixed_id_len;
	/*
	 * 1 once the pair came from a call the handset placed. A pair derived
	 * from the subscription is good enough to ring with; a measured one
	 * may overwrite it.
	 */
	int      cc_id_learned;
};

/*
 * Stage S1b: the handsets the base holds. Records live in a table; one of
 * them is current, the one the main link serves, and the stack works on a
 * copy of it (rec) that goes back into the table when another becomes
 * current. The host loads the table from its store at start and saves the
 * current record when the stack says it changed.
 */
#define DECT_BASE_HS_MAX	DECT_MM_MAX_SLOT

struct dect_base {
	struct dect_hs_rec hs[DECT_BASE_HS_MAX];
	unsigned n_hs;			/* records in use, hs[0] first */
	unsigned cur;			/* the current one */
	struct dect_hs_rec rec;		/* hs[cur], being worked on */
};

void dect_base_init(struct dect_base *b);

/* hs[cur] = rec: the working copy back into the table. */
void dect_base_stash(struct dect_base *b);

/* rec = hs[i], without stashing first (loading the table at start). */
void dect_base_load(struct dect_base *b, unsigned i);

/*
 * Make record i the current one: the working copy goes back first.
 * 1 if the current record changed, 0 if i already was current or is out
 * of range.
 */
int dect_base_switch(struct dect_base *b, unsigned i);

#endif

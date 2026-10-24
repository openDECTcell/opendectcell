/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Diagnostics: what a DECT base can tell you about each handset, as data.
 *
 * The design is docs/diagnostics.md: one struct per link, so the LuCI page
 * and the controller cannot drift apart on what "connected" or "level"
 * means. The library fills the protocol half from what it already holds and
 * decodes chip answers it is handed; it reads nothing itself, because it has
 * no transport and keeps none.
 *
 * The radio half comes from the chip's 0xF625 answer to a 0xF624 poll, the
 * per-link row AVM's DECT Monitor displays. Its layout was settled from our
 * own corpus of 554 answers and from the clean room [CR A-0087..A-0090]; see
 * report_conn_status() in dect-stubd.c, which this module replaces. Offsets
 * are parameter-byte indices, i.e. the chip's answer[N+2] [CR A-0088].
 */
#ifndef OPENDECTCELL_DECT_DIAG_H
#define OPENDECTCELL_DECT_DIAG_H

#include <stddef.h>
#include <stdint.h>

#include "dect_link.h"

#define DECT_DIAG_CARRIERS	10u
#define DECT_DIAG_EVENTS	8u

/* The 0xF625 row, decoded. */
struct dect_diag_status {
	uint8_t  valid;			/* at least the 16-byte head arrived */
	uint8_t  link;			/* [0], echoes the 0xF624 request */
	uint8_t  time_slot;		/* [2], or slot x 2 (only even seen) */
	uint8_t  carrier;		/* [3], 0..9 */
	uint8_t  b5;			/* [5], (record +0x116 bit 1) ? 2 : 1 */
	uint8_t  rssi[DECT_DIAG_CARRIERS];	/* [6..15], larger = stronger */
	uint8_t  has_counters;		/* the 0x26-byte form arrived */
	/*
	 * Per-interval receive-integrity counters (per-slot failures), in the
	 * order 0x10 0x12 0x14 0x16 0x18 0x1C 0x1E 0x20. Which failure each
	 * one counts is a recorded gap [CR A-0090]; dect_diag_event_name()
	 * names the trigger, not the error.
	 */
	uint16_t events[DECT_DIAG_EVENTS];
	uint16_t interval_frames;	/* 0x1A: frames in this poll interval */
	uint16_t frames;		/* 0x24: since establishment, wraps */
};

/* Frames since establishment, unwrapped across polls. Zero it per call. */
struct dect_diag_frames {
	uint64_t total;
	uint16_t last;
	uint8_t  have;
};

struct dect_diag_link {
	uint8_t  handle;
	uint8_t  identity[DECT_MM_PORTID_MAX];	/* the handset's IPUI */
	size_t   identity_len;
	int      handset_tag;		/* mm.adopted: the owner's tag, -1 */
	uint8_t  slot;			/* subscription slot */
	uint8_t  tpui[3];
	uint8_t  located;		/* {LOCATE-ACCEPT} sent on this link */
	uint8_t  tpui_confirmed;
	uint8_t  registered;		/* authenticated on this link */
	uint8_t  model[8];		/* <<MODEL-IDENTIFIER>>, if it sent one */
	size_t   model_len;
	uint8_t  codecs[16];		/* its <<CODEC-LIST>>, if it sent one */
	size_t   codecs_len;
	enum dect_cc_state call_state;
	uint8_t  call_incoming;		/* 1: we called the handset */
	uint8_t  basic_service;		/* its SETUP's, DECT_CC_SVC_NONE if none */
	uint8_t  chosen_codec;		/* 0 = none named */
	uint8_t  ciphered;		/* call-time ciphering READY */
	struct dect_diag_status radio;	/* filled by dect_diag_decode_status() */
	uint64_t call_frames;		/* unwrapped, from dect_diag_frames */
};

/* The protocol half, from a link. The radio half is left untouched. */
void dect_diag_from_link(const struct dect_link *l, struct dect_diag_link *d);

/*
 * Decode a 0xF625 parameter block. Returns 0, or -1 if it is shorter than
 * the 16-byte head (then st->valid is 0). Counters are filled only from the
 * 0x26-byte form.
 */
int dect_diag_decode_status(const uint8_t *p, size_t n,
			    struct dect_diag_status *st);

/* Feed a decoded status into the unwrapped frame count; returns the total. */
uint64_t dect_diag_frames_update(struct dect_diag_frames *f,
				 const struct dect_diag_status *st);

/* The trigger behind event counter i, e.g. "s12b6"; "?" out of range. */
const char *dect_diag_event_name(unsigned i);

/* A short name for a CC state: "idle", "ringing", "active", ... */
const char *dect_diag_call_state_name(enum dect_cc_state st);

/*
 * One JSON object for d, NUL-terminated, into buf. The IPUI is key-adjacent
 * personal data: it goes in only when with_identity is set. Returns the
 * length written, or 0 if buf is too small.
 */
size_t dect_diag_json(const struct dect_diag_link *d, int with_identity,
		      char *buf, size_t cap);

#endif /* OPENDECTCELL_DECT_DIAG_H */

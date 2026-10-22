/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * <<IWU-TO-IWU>> session dialog carried in {CC-IWU-INFO}.
 *
 * EN 300 175-5 §7.7.23 defines the <<IWU-TO-IWU>> information element as a
 * container whose contents are outside the DECT standard. Both the handsets
 * we have use it for a small request/response protocol whose records begin
 * with the discriminator 0xC3, and they open it during call setup.
 *
 * Our stack has never answered it. A Gigaset A540 opens four of these sessions
 * on every call it places -- session ids 0x00, 0x05, 0x06 and 0x08 -- and we
 * drop all of them. Ignoring a peer's session opens is a defect regardless of
 * what else it causes, and TS 102 527-3 list access needs this dialog, so it
 * has to exist either way.
 *
 * This is NOT expected to fix FP-originated calls. An earlier reading [CR
 * A-StockTrace-5 §11a] had stock opening {CC-IWU-INFO} between the {CC-SETUP}
 * and the handset's {CC-ALERTING}; §24 corrects it. In stock captures EVERY
 * open is handset->base -- the base only ever responds, and never opens a
 * dialog -- and base-originated calls to Gigaset handsets ring with no
 * {CC-IWU-INFO} exchange at all. So the dialog's absence during our failed
 * calls is a consequence of the early release, not its cause.
 *
 * The dialog is implemented because it is owed: the peer opens sessions we
 * drop, and TS 102 527-3 list access is built on it. See
 * docs/tasks-2026-10-22.md.
 *
 * Record framing [CR A-StockTrace-5 §23, from 40 records across 6+ complete
 * dialogs]:
 *
 *   PP  c3 00 NN <data>                     open; NN is a session id
 *   FP  c3 01 NN 91 <X> 00 00 00 ff 01 <Y>  open response, ECHOING NN
 *   PP  c3 06 91 ...     FP  c3 07 91 81 03
 *   PP  c3 10 91 ...     FP  c3 12 91 01
 *   FP  c3 14 91 80 <ASCII> ...             data push (list labels)
 *   PP  c3 02 91         FP  c3 03 91       close / close ack
 *
 * The session id is chosen by the peer and echoed by us; it is NOT a counter
 * we generate, and it is constant within a dialog. It appears only in the open
 * and its response. 0x91 is a fixed tag introducing the payload.
 *
 * The payload field SEMANTICS are deliberately not modelled: what 0x82/0x83/
 * 0x84, `ff 01` and `81 03` mean was not decoded, and guessing at them is how
 * this project has produced its wrong answers. The values we emit are the
 * lowest observed ones, and dect_iwu_set_open_params() exists so the bench can
 * sweep them without a rebuild.
 *
 * No I/O, no allocation, transport-agnostic: feed one <<IWU-TO-IWU>> content
 * in, get the reply content out. Re-implemented from the public ETSI text and
 * clean-room facts as GPL-2.0-or-later.
 */
#ifndef OPENDECTCELL_DECT_IWU_H
#define OPENDECTCELL_DECT_IWU_H

#include <stddef.h>
#include <stdint.h>

/* First octet of every record in this dialog. */
#define DECT_IWU_DISC		0xC3u

/* Record types, as observed. Names describe the role, not a decoded meaning. */
#define DECT_IWU_OPEN		0x00u
#define DECT_IWU_OPEN_RESP	0x01u
#define DECT_IWU_CLOSE		0x02u
#define DECT_IWU_CLOSE_ACK	0x03u
#define DECT_IWU_REQ_06		0x06u
#define DECT_IWU_RESP_07	0x07u
#define DECT_IWU_REQ_10		0x10u
#define DECT_IWU_RESP_12	0x12u
#define DECT_IWU_PUSH_14	0x14u

/* Fixed tag that introduces the payload in every record except the PP's open. */
#define DECT_IWU_TAG		0x91u

/* Longest reply we build (the open response). */
#define DECT_IWU_REPLY_MAX	16u

struct dect_iwu_stats {
	uint32_t opened;	/* opens answered */
	uint32_t closed;	/* closes acknowledged */
	uint32_t replied;	/* typed requests answered */
	uint32_t ignored;	/* records we have no reply for */
	uint32_t malformed;	/* too short, or not a 0xC3 record */
};

struct dect_iwu {
	/*
	 * Open-response parameters, semantics unknown. `[not established]`
	 * Defaults are the lowest values observed in stock dialogs.
	 */
	uint8_t open_x;		/* the octet after the 0x91 tag (82/83/84..) */
	uint8_t open_y;		/* the trailing octet (01/02/03) */
	struct dect_iwu_stats stats;
};

void dect_iwu_init(struct dect_iwu *iwu);

/*
 * Override the open-response parameters whose meaning we have not decoded.
 * For bench sweeps; the defaults are what dect_iwu_init() sets.
 */
void dect_iwu_set_open_params(struct dect_iwu *iwu, uint8_t x, uint8_t y);

/*
 * Build the reply to one received <<IWU-TO-IWU>> content.
 *
 * `in` is the IE contents (starting at the 0xC3 discriminator), `out` receives
 * the reply contents to put in an <<IWU-TO-IWU>> of our own. Returns the reply
 * length, or 0 when this record needs no reply -- which includes every record
 * we do not recognise, because answering with a guess is worse than silence.
 */
size_t dect_iwu_reply(struct dect_iwu *iwu, const uint8_t *in, size_t in_len,
		      uint8_t *out, size_t out_max);

#endif /* OPENDECTCELL_DECT_IWU_H */

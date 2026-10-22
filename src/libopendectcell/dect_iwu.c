/* SPDX-License-Identifier: GPL-2.0-or-later */
/* <<IWU-TO-IWU>> session responder. See dect_iwu.h for the record framing. */

#include <string.h>

#include "dect_iwu.h"

/*
 * Open-response defaults. [CR A-StockTrace-5 §23] saw X in {0x82, 0x83, 0x84,
 * ...} and Y in {0x01, 0x02, 0x03} across dialogs, without decoding either,
 * and we have not decoded them here. These are the lowest observed values;
 * dect_iwu_set_open_params() overrides them for bench sweeps.
 */
#define OPEN_X_DEFAULT	0x82u
#define OPEN_Y_DEFAULT	0x01u

void dect_iwu_init(struct dect_iwu *iwu)
{
	memset(iwu, 0, sizeof(*iwu));
	iwu->open_x = OPEN_X_DEFAULT;
	iwu->open_y = OPEN_Y_DEFAULT;
}

void dect_iwu_set_open_params(struct dect_iwu *iwu, uint8_t x, uint8_t y)
{
	iwu->open_x = x;
	iwu->open_y = y;
}

size_t dect_iwu_reply(struct dect_iwu *iwu, const uint8_t *in, size_t in_len,
		      uint8_t *out, size_t out_max)
{
	uint8_t type;

	/* A record is at least the discriminator and a type. */
	if (!in || in_len < 2u || in[0] != DECT_IWU_DISC) {
		iwu->stats.malformed++;
		return 0;
	}
	type = in[1];

	switch (type) {
	case DECT_IWU_OPEN:
		/*
		 * `c3 00 NN <data>` -> `c3 01 NN 91 X 00 00 00 ff 01 Y`.
		 *
		 * NN is the peer's session id and we ECHO it; it is not a
		 * counter of ours. Without the id octet there is no session to
		 * answer for, so treat a short open as malformed rather than
		 * inventing one.
		 */
		if (in_len < 3u) {
			iwu->stats.malformed++;
			return 0;
		}
		if (out_max < 11u)
			return 0;
		out[0] = DECT_IWU_DISC;
		out[1] = DECT_IWU_OPEN_RESP;
		out[2] = in[2];			/* echoed session id */
		out[3] = DECT_IWU_TAG;
		out[4] = iwu->open_x;
		out[5] = 0x00u;
		out[6] = 0x00u;
		out[7] = 0x00u;
		out[8] = 0xFFu;
		out[9] = 0x01u;
		out[10] = iwu->open_y;
		iwu->stats.opened++;
		return 11u;

	case DECT_IWU_CLOSE:
		/* `c3 02 91` -> `c3 03 91`. */
		if (out_max < 3u)
			return 0;
		out[0] = DECT_IWU_DISC;
		out[1] = DECT_IWU_CLOSE_ACK;
		out[2] = DECT_IWU_TAG;
		iwu->stats.closed++;
		return 3u;

	case DECT_IWU_REQ_06:
		/* `c3 06 91 81 03 00 01 02 03` -> `c3 07 91 81 03`. */
		if (out_max < 5u)
			return 0;
		out[0] = DECT_IWU_DISC;
		out[1] = DECT_IWU_RESP_07;
		out[2] = DECT_IWU_TAG;
		out[3] = 0x81u;
		out[4] = 0x03u;
		iwu->stats.replied++;
		return 5u;

	case DECT_IWU_REQ_10:
		/* `c3 10 91 00 05 <digits>..` -> `c3 12 91 01`. */
		if (out_max < 4u)
			return 0;
		out[0] = DECT_IWU_DISC;
		out[1] = DECT_IWU_RESP_12;
		out[2] = DECT_IWU_TAG;
		out[3] = 0x01u;
		iwu->stats.replied++;
		return 4u;

	default:
		/*
		 * Includes our own responses echoed back by a test harness and
		 * the base-to-handset push (0x14), which the peer answers, not
		 * us. Replying with a guess is worse than staying silent: an
		 * unrecognised record is how this dialog ends up desynchronised.
		 */
		iwu->stats.ignored++;
		return 0;
	}
}

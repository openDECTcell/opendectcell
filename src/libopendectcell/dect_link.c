/* SPDX-License-Identifier: GPL-2.0-or-later */
/* dect_link: the per-connection C-plane stack. See dect_link.h. */

#include <string.h>

#include "dect_link.h"

void dect_link_init(struct dect_link *l, uint8_t handle,
		    const struct dect_mm_cfg *mmcfg)
{
	memset(l, 0, sizeof(*l));
	l->handle = handle;
	dect_lc_rx_init(&l->lc, 0);
	dect_lapc_init(&l->lapc, 0);
	dect_mm_init(&l->mm, mmcfg);
	dect_cc_init(&l->cc);
}

/*
 * Hand queued NWK messages to LAPC, one at a time.
 *
 * **MM before CC, and only while nothing is outstanding.** Both parts are
 * protocol behaviour rather than taste: LAPC class A has a window of one, so
 * a second message queued before the first is acknowledged would be lost, and
 * MM carries the authentication and ciphering exchanges that CC depends on.
 */
static void drain_nwk(struct dect_link *l, uint64_t now)
{
	uint8_t msg[DECT_LAPC_MSG_MAX];
	size_t mlen;

	while (!l->lapc.outstanding) {
		mlen = dect_mm_poll(&l->mm, msg, sizeof(msg));
		if (mlen == 0)
			mlen = dect_cc_poll(&l->cc, msg, sizeof(msg));
		if (mlen == 0)
			break;
		if (dect_lapc_send(&l->lapc, msg, mlen, now) != 0)
			break;
	}
}

void dect_link_drain(struct dect_link *l, uint64_t now)
{
	drain_nwk(l, now);
}

void dect_link_tick(struct dect_link *l, uint64_t now)
{
	dect_lapc_tick(&l->lapc, now);
	dect_cc_tick(&l->cc, now);
	dect_mm_tick(&l->mm, now);	/* <MM_ident.1> */
	drain_nwk(l, now);
}

int dect_link_poll_append_mail(struct dect_link *l, struct dect_mail *out)
{
	uint8_t frame[DECT_LC_LAPC_MAX];
	uint8_t cs[DECT_LC_FRAME_MAX];
	size_t n, flen;

	n = dect_lapc_poll_tx(&l->lapc, frame, sizeof(frame));
	if (n == 0)
		return 0;
	/*
	 * Sign with the link's signature: 0 while unregistered, the peer's
	 * PMID-derived LSIG once learned from its establishment frame.
	 */
	flen = dect_lc_build(frame, n, l->lc.lsig, cs, sizeof(cs));
	if (flen == 0)
		return 0;
	if (dect_cs_append_mail(out, DECT_TASK_PCM, l->handle, cs, flen) != 0)
		return 0;
	return 1;
}

int dect_link_rx_segment(struct dect_link *l, const struct dect_mail *m,
			 uint64_t now, const uint8_t *rand_f,
			 const uint8_t *rs, struct dect_link_ind *ind)
{
	struct dect_link_ind local;
	struct dect_cs_seg seg;
	uint8_t lapcbuf[DECT_LC_LAPC_MAX];
	int lapc_len, nwk_len;

	if (!ind)
		ind = &local;
	memset(ind, 0, sizeof(*ind));

	if (dect_cs_decode_segment(m, &seg) < 0)
		return -1;

	memcpy(ind->seg_head, seg.data, sizeof(ind->seg_head));
	lapc_len = dect_lc_rx_fragment(&l->lc, seg.data, lapcbuf,
				       sizeof(lapcbuf));
	ind->lc_rc = lapc_len;
	/*
	 * A segment we cannot use must not stall our own queue. An incoming
	 * call queues its {CC-SETUP} before the peer's first frame arrives,
	 * and an early return left it sitting there for ever.
	 */
	if (lapc_len <= 0) {
		drain_nwk(l, now);
		return 0;
	}
	if (dect_lapc_parse(lapcbuf, (size_t)lapc_len, &ind->frame) < 0) {
		ind->lapc_parse_failed = 1;
		drain_nwk(l, now);
		return 0;
	}

	nwk_len = dect_lapc_rx(&l->lapc, &ind->frame, now, ind->nwk,
			       sizeof(ind->nwk));
	if (nwk_len > 0) {
		struct dect_nwk_hdr h;
		const uint8_t *ies;
		size_t ies_len;

		ind->nwk_len = (size_t)nwk_len;
		if (dect_nwk_parse(ind->nwk, ind->nwk_len, &h, &ies,
				   &ies_len) == 0 && h.pd == DECT_PD_LCE) {
			/*
			 * The peer's answer to our page. Reported, never acted
			 * on: when an FP-originated {CC-SETUP} may go out is
			 * the caller's decision.
			 */
			ind->page_response = 1;
		} else if (dect_nwk_parse(ind->nwk, ind->nwk_len, &h, &ies,
					  &ies_len) == 0 && h.pd == DECT_PD_CC) {
			uint32_t kp_before = l->cc.stats.keypad;

			ind->cc_message = 1;
			dect_cc_rx(&l->cc, ind->nwk, ind->nwk_len);
			/*
			 * Report that dialled digits arrived, so the caller
			 * does not have to poll. What they mean -- a number to
			 * dial, a feature code, a control character -- is
			 * policy and stays with the caller; read them with
			 * dect_cc_keypad().
			 */
			if (l->cc.stats.keypad != kp_before)
				ind->keypad = 1;
		} else {
			ind->mm_message = 1;
			dect_mm_rx(&l->mm, ind->nwk, ind->nwk_len, rand_f, rs);
		}
	}
	drain_nwk(l, now);
	return 0;
}

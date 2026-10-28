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
/*
 * Answer a {CC-SETUP} we hold while its record is fetched, so the handset
 * is not left waiting out its 20 s P<CC.03> with "---" on the display
 * (bench test 7, 2026-10-25). {CC-SETUP-ACK} when CC will collect the
 * digits (overlap receiving, a speech call without <<SENDING-COMPLETE>>),
 * {CC-CALL-PROC} otherwise -- what CC itself would have sent first.
 */
static void answer_held_setup(struct dect_link *l, const uint8_t *ies,
			      size_t ies_len)
{
	int svc = dect_nwk_find_fixed_ie(ies, ies_len, DECT_IE_BASIC_SERVICE);
	int lia = svc >= 0 && ((unsigned)svc >> 4) == 0x2u;
	int complete = dect_nwk_find_fixed_ie(ies, ies_len,
					      DECT_IE_SENDING_COMPLETE) >= 0;
	uint8_t type = (l->cc.overlap && !lia && !complete) ?
		       DECT_CC_SETUP_ACK : DECT_CC_CALL_PROC;
	size_t n;

	l->setup_answered = 0;
	if (l->lce_len)
		return;		/* slot busy: CC answers on resume */
	n = dect_nwk_build_hdr(l->lce, sizeof(l->lce), DECT_PD_CC,
			       DECT_TI_RESPONDER, l->setup_ti, type);
	if (!n)
		return;
	if (!lia)
		n = dect_cc_put_call_status(&l->cc, l->lce, sizeof(l->lce), n,
					    type == DECT_CC_SETUP_ACK ?
					    DECT_CS_CALL_SETUP_ACK :
					    DECT_CS_CALL_PROC);
	l->lce_len = n;
	l->setup_answered = type;
}

static void hold_follow_up(struct dect_link *l, const struct dect_nwk_hdr *h,
			   const uint8_t *msg, size_t len)
{
	if (h->msg_type == DECT_CC_RELEASE ||
	    h->msg_type == DECT_CC_RELEASE_COM) {
		/* the handset gave up: the call ends here, not in CC */
		if (h->msg_type == DECT_CC_RELEASE && l->setup_answered &&
		    !l->lce_len) {
			size_t n = dect_nwk_build_hdr(l->lce, sizeof(l->lce),
						      DECT_PD_CC,
						      DECT_TI_RESPONDER,
						      l->setup_ti,
						      DECT_CC_RELEASE_COM);
			l->lce_len = n;
		}
		l->setup_len = 0;
		l->held_n = 0;
		return;
	}
	if (l->held_n >= DECT_LINK_HELD || len > sizeof(l->held[0])) {
		l->held_dropped++;
		return;
	}
	memcpy(l->held[l->held_n], msg, len);
	l->held_len[l->held_n++] = len;
}

static void drain_nwk(struct dect_link *l, uint64_t now)
{
	uint8_t msg[DECT_LAPC_MSG_MAX];
	size_t mlen;

	while (!l->lapc.outstanding) {
		mlen = 0;
		if (l->lce_len) {
			mlen = l->lce_len;
			memcpy(msg, l->lce, mlen);
			l->lce_len = 0;
		}
		if (mlen == 0)
			mlen = dect_mm_poll(&l->mm, msg, sizeof(msg));
		if (mlen == 0)
			mlen = dect_cc_poll(&l->cc, msg, sizeof(msg));
		if (mlen == 0 && l->ciss_len) {
			mlen = l->ciss_len;
			memcpy(msg, l->ciss, mlen);
			l->ciss_len = 0;
		}
		if (mlen == 0)
			break;
		if (l->tx_log)
			l->tx_log(l->tx_log_ctx, msg, mlen);
		if (dect_lapc_send(&l->lapc, msg, mlen, now) != 0)
			break;
	}
}

void dect_link_drain(struct dect_link *l, uint64_t now)
{
	drain_nwk(l, now);
}

void dect_link_set_tx_log(struct dect_link *l,
			  void (*fn)(void *ctx, const uint8_t *msg, size_t len),
			  void *ctx)
{
	l->tx_log = fn;
	l->tx_log_ctx = ctx;
}

int dect_link_page_reject(struct dect_link *l, const uint8_t *ipui,
			  size_t ipui_len, uint8_t ti_value)
{
	size_t n;

	if (l->lce_len || !ipui || !ipui_len)
		return -1;
	n = dect_nwk_build_hdr(l->lce, sizeof(l->lce), DECT_PD_LCE,
			       DECT_TI_RESPONDER, ti_value,
			       DECT_LCE_PAGE_REJECT);
	if (n)
		n = dect_nwk_put_ie(l->lce, sizeof(l->lce), n,
				    DECT_IE_PORTABLE_IDENTITY, ipui, ipui_len);
	if (!n)
		return -1;
	l->lce_len = n;
	return 0;
}

int dect_link_send_time(struct dect_link *l, const struct dect_time *t)
{
	size_t n;

	if (l->ciss_len)
		return -1;		/* one at a time is enough */
	n = dect_time_build_facility(l->ciss, sizeof(l->ciss), t);
	if (!n)
		return -1;
	l->ciss_len = n;
	return 0;
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

	/*
	 * A link serves ONE connection. A segment carrying a different handle
	 * is another handset's traffic and must not reach this link's Lc
	 * reassembly, LAPC sequence numbers or NWK state machines.
	 *
	 * The handle was always decoded here and never checked. With one
	 * handset on the bench that is invisible; with several it is not. On
	 * 2026-10-24, three handsets and a FRITZ!Fon C4 in range produced
	 * 26-30 connections in 170 s, all feeding one link, and an
	 * FP-originated {CC-SETUP} was never acknowledged by anyone. Reject
	 * the segment and say whose it was, so the caller can route it or
	 * ignore it deliberately rather than by accident.
	 */
	ind->seg_handle = seg.handle;
	if (seg.handle != l->handle) {
		ind->foreign = 1;
		return -1;
	}

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
			 *
			 * Only {LCE-PAGE-RESPONSE} counts, and only with its
			 * mandatory <<PORTABLE-IDENTITY>> (EN 300 175-5 table
			 * 64), which is handed up so the caller can check who
			 * answered. Any LCE message used to count, from any
			 * handset (external review, the clean-room record/
			 * an external review-24102026.md, finding 10).
			 *
			 * Without the identity §17.6.5 wants clearing by
			 * {LCE-PAGE-REJECT}, whose mandatory identity is "the
			 * full IPUI of the PT that is rejected" (table 65): we
			 * can only send it when the link already knows that
			 * IPUI, and ignore the response otherwise. This comment
			 * used to quote the "ignore" half alone (spec sweep).
			 */
			const uint8_t *pid;
			size_t pid_len;

			pid = dect_nwk_find_ie(ies, ies_len,
					       DECT_IE_PORTABLE_IDENTITY, &pid_len);
			if (h.msg_type == DECT_LCE_PAGE_RESPONSE && !pid &&
			    l->mm.port_id_len)
				(void)dect_link_page_reject(l, l->mm.port_id,
							    l->mm.port_id_len,
							    h.ti_value);
			if (h.msg_type == DECT_LCE_PAGE_RESPONSE && pid &&
			    pid_len <= sizeof(ind->page_id)) {
				memcpy(ind->page_id, pid, pid_len);
				ind->page_id_len = pid_len;
				ind->page_ti = h.ti_value;
				ind->page_response = 1;
			}
		} else if (dect_nwk_parse(ind->nwk, ind->nwk_len, &h, &ies,
					  &ies_len) == 0 && h.pd == DECT_PD_CC) {
			uint32_t kp_before = l->cc.stats.keypad;

			ind->cc_message = 1;
			/*
			 * The handset MM would accept, and no other. A held
			 * handset placing a call on a fresh link has not
			 * located on it, so adopt it here as LOCATE would.
			 */
			/*
			 * The rest of a held call: keep it for the replay. A
			 * release from the handset ends the hold instead.
			 */
			if (l->setup_len && h.msg_type != DECT_CC_SETUP &&
			    h.ti_value == l->setup_ti &&
			    h.ti_flag == DECT_TI_ORIGINATOR) {
				hold_follow_up(l, &h, ind->nwk, ind->nwk_len);
				goto drained;
			}
			if (h.msg_type == DECT_CC_SETUP) {
				size_t pid_len;
				const uint8_t *pid = dect_nwk_find_ie(
					ies, ies_len, DECT_IE_PORTABLE_IDENTITY,
					&pid_len);
				int cs = (pid && l->call_status_for) ?
					 l->call_status_for(l->call_status_ctx,
							    pid, pid_len) : -1;

				/* before anything answers it, held or not */
				if (cs >= 0 && !dect_cc_busy(&l->cc))
					dect_cc_set_call_status(
						&l->cc, cs,
						l->cc.status_call_id);
				if (l->call_id_for && !dect_cc_busy(&l->cc)) {
					uint8_t id = l->call_id_for(
						l->call_status_ctx, l);

					if (id)
						dect_cc_set_call_status(
							&l->cc,
							l->cc.call_status, id);
				}

				/*
				 * A roamer that did not locate here (same
				 * location area) calls straight away: hold
				 * its SETUP while the owner fetches the
				 * record, as MM holds a LOCATE-REQUEST.
				 */
				if (!dect_mm_adopt(&l->mm, pid, pid_len) &&
				    pid && l->mm.cfg.defer_unknown &&
				    !l->setup_len &&
				    ind->nwk_len <= sizeof(l->setup) &&
				    pid_len <= sizeof(l->setup_id)) {
					memcpy(l->setup, ind->nwk, ind->nwk_len);
					l->setup_len = ind->nwk_len;
					memcpy(l->setup_id, pid, pid_len);
					l->setup_id_len = pid_len;
					l->setup_ti = h.ti_value;
					l->held_n = 0;
					answer_held_setup(l, ies, ies_len);
					goto drained;
				}
				if (l->setup_len && pid &&
				    pid_len == l->setup_id_len &&
				    !memcmp(pid, l->setup_id, pid_len))
					goto drained;	/* its repeat */
			}
			dect_cc_set_gate(&l->cc, l->mm.port_id,
					 l->mm.port_id_len);
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
drained:
	drain_nwk(l, now);
	return 0;
}

int dect_link_setup_pending(const struct dect_link *l, const uint8_t **id,
			    size_t *id_len)
{
	if (!l->setup_len)
		return 0;
	if (id)
		*id = l->setup_id;
	if (id_len)
		*id_len = l->setup_id_len;
	return 1;
}

int dect_link_setup_resume(struct dect_link *l, uint64_t now)
{
	uint8_t msg[DECT_LAPC_MSG_MAX];
	size_t n = l->setup_len;
	int known;

	if (!n)
		return 0;
	unsigned i;

	memcpy(msg, l->setup, n);
	l->setup_len = 0;
	known = dect_mm_adopt(&l->mm, l->setup_id, l->setup_id_len);
	dect_cc_set_gate(&l->cc, l->mm.port_id, l->mm.port_id_len);
	if (l->setup_answered)
		dect_cc_note_answered(&l->cc, l->setup_answered);
	dect_cc_rx(&l->cc, msg, n);
	/* then what the handset sent on the call while it was held */
	for (i = 0; i < l->held_n; i++)
		dect_cc_rx(&l->cc, l->held[i], l->held_len[i]);
	l->held_n = 0;
	l->setup_answered = 0;
	drain_nwk(l, now);
	return known ? 1 : -1;
}

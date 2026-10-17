// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * STUB control-channel session state machine. See dect_session.h.
 */
#include "dect_session.h"
#include "dect_proto.h"

#include <string.h>

#define DEFAULT_CONTACT_RETRY_MS	2000u	/* StartTimer(0x41, 2000) [CR A-0003] */
#define DEFAULT_LIVENESS_MS		60000u	/* status every 10-30 s (observed) */

void dect_session_init(struct dect_session *s, const struct dect_sess_cfg *cfg)
{
	memset(s, 0, sizeof(*s));
	s->cfg = *cfg;
	if (!s->cfg.contact_retry_ms)
		s->cfg.contact_retry_ms = DEFAULT_CONTACT_RETRY_MS;
	if (!s->cfg.liveness_ms)
		s->cfg.liveness_ms = DEFAULT_LIVENESS_MS;
	s->state = DECT_SESS_DOWN;
}

void dect_session_contact_mail(const struct dect_session *s, struct dect_mail *m)
{
	memset(m, 0, sizeof(*m));
	m->src = s->cfg.src_task;
	m->dst = s->cfg.dst_task;
	m->prim = DECT_PRIM_CONTACT;
	m->param_len = 0;
}

/* Record that a contact is being sent now and ask the caller to send it. */
static enum dect_action send_contact(struct dect_session *s, uint64_t now_ms)
{
	s->last_contact_ms = now_ms;
	s->got_ack1 = s->got_ack2 = 0;
	s->state = DECT_SESS_CONTACTING;
	s->stats.contacts_sent++;
	return DECT_ACT_SEND_CONTACT;
}

enum dect_action dect_session_tick(struct dect_session *s, uint64_t now_ms)
{
	switch (s->state) {
	case DECT_SESS_DOWN:
		return send_contact(s, now_ms);
	case DECT_SESS_CONTACTING:
		if (now_ms - s->last_contact_ms >= s->cfg.contact_retry_ms)
			return send_contact(s, now_ms);
		return DECT_ACT_NONE;
	case DECT_SESS_UP:
		if (now_ms - s->last_rx_ms >= s->cfg.liveness_ms) {
			s->stats.deaths++;
			s->state = DECT_SESS_DOWN;
			return send_contact(s, now_ms);
		}
		return DECT_ACT_NONE;
	}
	return DECT_ACT_NONE;
}

enum dect_action dect_session_rx(struct dect_session *s,
				 const struct dect_mail *m, uint64_t now_ms)
{
	s->last_rx_ms = now_ms;

	switch (m->prim) {
	case DECT_PRIM_CONTACT_ACK1:
		s->got_ack1 = 1;
		s->stats.answers++;
		break;
	case DECT_PRIM_CONTACT_ACK2:
		s->got_ack2 = 1;
		s->stats.answers++;
		break;
	case DECT_PRIM_STATUS:
		s->stats.status_mails++;
		break;
	default:
		break;
	}

	/* Any mail from the STUB means the contact succeeded. */
	if (s->state == DECT_SESS_CONTACTING)
		s->state = DECT_SESS_UP;
	return DECT_ACT_NONE;
}

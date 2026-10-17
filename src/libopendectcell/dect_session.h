/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * STUB control-channel session: the contact handshake and the liveness
 * watchdog, as a pure state machine with an injected monotonic clock.
 *
 * The STUB stays silent until the host sends the contact mail 0xF600
 * [CR A-0003]; it then answers (0xF601/0xF602) and reports periodically
 * with 0xFFF1 status mails. This state machine sends the contact on
 * start-up, resends it until an answer arrives, and re-contacts if the
 * STUB goes quiet for too long. No I/O: the caller does the framing
 * (dect_mail.h), the transport and the timing.
 *
 * Specification: docs/stub-pcm-control-channel.md.
 */
#ifndef OPENDECTCELL_DECT_SESSION_H
#define OPENDECTCELL_DECT_SESSION_H

#include <stdint.h>

#include "dect_mail.h"

enum dect_sess_state {
	DECT_SESS_DOWN,		/* no contact outstanding (start, or STUB dead) */
	DECT_SESS_CONTACTING,	/* contact sent, waiting for the first answer */
	DECT_SESS_UP,		/* STUB answered; liveness tracked */
};

/* What the caller should do after a tick or an rx. */
enum dect_action {
	DECT_ACT_NONE,
	DECT_ACT_SEND_CONTACT,	/* build and send dect_session_contact_mail() */
};

struct dect_sess_cfg {
	uint32_t contact_retry_ms;	/* resend contact if unanswered (e.g. 2000) */
	uint32_t liveness_ms;		/* declare dead after this silence (e.g. 60000) */
	uint8_t  src_task;		/* our task id (DECT_TASK_PCM) */
	uint8_t  dst_task;		/* contact destination (DECT_TASK_CONTACT_DST) */
};

struct dect_sess_stats {
	uint32_t contacts_sent;
	uint32_t answers;		/* 0xF601 + 0xF602 seen */
	uint32_t status_mails;		/* 0xFFF1 seen */
	uint32_t deaths;		/* liveness timeouts */
};

struct dect_session {
	struct dect_sess_cfg cfg;
	enum dect_sess_state state;
	uint64_t last_rx_ms;		/* last mail from the STUB */
	uint64_t last_contact_ms;	/* last contact we sent */
	int got_ack1, got_ack2;		/* answers seen since the last contact */
	struct dect_sess_stats stats;
};

/* contact_retry_ms / liveness_ms of 0 take built-in defaults. */
void dect_session_init(struct dect_session *s, const struct dect_sess_cfg *cfg);

/* Fill the contact mail (src/dst from the config, prim 0xF600, no params). */
void dect_session_contact_mail(const struct dect_session *s, struct dect_mail *m);

/*
 * Advance the clock. Returns DECT_ACT_SEND_CONTACT when the caller must
 * (re)send the contact mail; the state machine has already recorded it as
 * sent at now_ms. now_ms must be monotonic non-decreasing.
 */
enum dect_action dect_session_tick(struct dect_session *s, uint64_t now_ms);

/* Feed a decoded mail received at now_ms. Returns an action (usually NONE). */
enum dect_action dect_session_rx(struct dect_session *s,
				 const struct dect_mail *m, uint64_t now_ms);

#endif /* OPENDECTCELL_DECT_SESSION_H */

// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for src/libopendectcell/dect_session.c: the contact handshake and the
 * liveness watchdog, driven by a fake monotonic clock. No hardware.
 */
#include "check.h"
#include "../src/libopendectcell/dect_session.h"
#include "../src/libopendectcell/dect_proto.h"

#include <string.h>

static struct dect_mail mail(uint16_t prim)
{
	struct dect_mail m;

	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_PCM_REMOTE;
	m.dst = DECT_TASK_PCM;
	m.prim = prim;
	return m;
}

/* The contact mail matches the verified 0xF600 layout and encodes. */
static void test_contact_mail(void)
{
	struct dect_sess_cfg cfg = { .src_task = DECT_TASK_PCM,
				     .dst_task = DECT_TASK_CONTACT_DST };
	struct dect_session s;
	struct dect_mail m;
	uint8_t out[DECT_TX_ELEM_MAX];

	dect_session_init(&s, &cfg);
	dect_session_contact_mail(&s, &m);
	CHECK_EQ(m.src, 0x22);
	CHECK_EQ(m.dst, 0x02);
	CHECK_EQ(m.prim, 0xf600);
	CHECK_EQ(m.param_len, 0);
	CHECK(dect_mail_encode(&m, 0, out, sizeof(out)) == 12);
}

/* Start sends contact once, then holds off until the retry timeout. */
static void test_contact_retry(void)
{
	struct dect_sess_cfg cfg = { .contact_retry_ms = 2000,
				     .src_task = DECT_TASK_PCM,
				     .dst_task = DECT_TASK_CONTACT_DST };
	struct dect_session s;

	dect_session_init(&s, &cfg);
	CHECK_EQ(dect_session_tick(&s, 0), DECT_ACT_SEND_CONTACT);
	CHECK_EQ(s.state, DECT_SESS_CONTACTING);
	CHECK_EQ(s.stats.contacts_sent, 1);
	CHECK_EQ(dect_session_tick(&s, 1000), DECT_ACT_NONE);	/* too soon */
	CHECK_EQ(dect_session_tick(&s, 1999), DECT_ACT_NONE);
	CHECK_EQ(dect_session_tick(&s, 2000), DECT_ACT_SEND_CONTACT);
	CHECK_EQ(s.stats.contacts_sent, 2);
}

/* An answer brings the session up and stops the contact retries. */
static void test_answer_brings_up(void)
{
	struct dect_sess_cfg cfg = { .src_task = DECT_TASK_PCM,
				     .dst_task = DECT_TASK_CONTACT_DST };
	struct dect_session s;
	struct dect_mail a1 = mail(DECT_PRIM_CONTACT_ACK1);
	struct dect_mail a2 = mail(DECT_PRIM_CONTACT_ACK2);

	dect_session_init(&s, &cfg);
	dect_session_tick(&s, 0);			/* contact */
	CHECK_EQ(dect_session_rx(&s, &a1, 14), DECT_ACT_NONE);
	CHECK_EQ(s.state, DECT_SESS_UP);
	dect_session_rx(&s, &a2, 15);
	CHECK_EQ(s.stats.answers, 2);
	/* Well past the retry window, but UP: no more contacts. */
	CHECK_EQ(dect_session_tick(&s, 10000), DECT_ACT_NONE);
	CHECK_EQ(s.stats.contacts_sent, 1);
}

/* Status mails keep the session alive; silence past the timeout re-contacts. */
static void test_liveness_watchdog(void)
{
	struct dect_sess_cfg cfg = { .contact_retry_ms = 2000,
				     .liveness_ms = 60000,
				     .src_task = DECT_TASK_PCM,
				     .dst_task = DECT_TASK_CONTACT_DST };
	struct dect_session s;
	struct dect_mail st = mail(DECT_PRIM_STATUS);

	dect_session_init(&s, &cfg);
	dect_session_tick(&s, 0);
	dect_session_rx(&s, &st, 100);			/* up at t=100 */
	CHECK_EQ(s.state, DECT_SESS_UP);
	CHECK_EQ(dect_session_tick(&s, 20000), DECT_ACT_NONE);
	dect_session_rx(&s, &st, 25000);		/* status resets watchdog */
	CHECK_EQ(s.stats.status_mails, 2);
	CHECK_EQ(dect_session_tick(&s, 84999), DECT_ACT_NONE);	/* <60s since 25000 */
	CHECK_EQ(dect_session_tick(&s, 85000), DECT_ACT_SEND_CONTACT);	/* dead */
	CHECK_EQ(s.stats.deaths, 1);
	CHECK_EQ(s.state, DECT_SESS_CONTACTING);
	CHECK_EQ(s.stats.contacts_sent, 2);
}

/* Defaults are applied when the config leaves them at 0. */
static void test_defaults(void)
{
	struct dect_sess_cfg cfg = { .src_task = DECT_TASK_PCM,
				     .dst_task = DECT_TASK_CONTACT_DST };
	struct dect_session s;

	dect_session_init(&s, &cfg);
	CHECK(s.cfg.contact_retry_ms > 0);
	CHECK(s.cfg.liveness_ms > 0);
}

int main(void)
{
	test_contact_mail();
	test_contact_retry();
	test_answer_brings_up();
	test_liveness_watchdog();
	test_defaults();
	return CHECK_DONE();
}

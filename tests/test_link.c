/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * dect_link: the per-connection C-plane stack (openDECTcell#67).
 *
 * These tests pin the behaviour that used to live in dect-stubd.c and that a
 * second consumer would otherwise have to reimplement from prose -- which is
 * the whole reason the glue moved into the library.
 */
#include <string.h>

#include "check.h"
#include "dect_link.h"

static void init(struct dect_link *l)
{
	struct dect_mm_cfg cfg;

	memset(&cfg, 0, sizeof(cfg));
	dect_link_init(l, 1, &cfg);
}

/* A fresh link owns its own state and has nothing to send. */
static void test_init_is_quiet(void)
{
	struct dect_link l;
	struct dect_mail m;

	init(&l);
	CHECK_EQ(l.handle, 1u);
	CHECK_EQ(l.lapc.outstanding, 0u);
	CHECK_EQ(dect_link_poll_append_mail(&l, &m), 0);
}

/*
 * Two links are independent. The single global connection in dect-stubd.c is
 * exactly what #67 exists to remove, and it is what made two handsets on one
 * base indistinguishable in our 2026-10-23 captures.
 */
static void test_two_links_are_independent(void)
{
	struct dect_link a, b;
	struct dect_mm_cfg cfg;

	memset(&cfg, 0, sizeof(cfg));
	dect_link_init(&a, 1, &cfg);
	dect_link_init(&b, 2, &cfg);

	a.lapc.vs = 1;
	CHECK_EQ(b.lapc.vs, 0u);
	CHECK_EQ(a.handle, 1u);
	CHECK_EQ(b.handle, 2u);
}

/*
 * An outbound NWK message leaves as an APPEND_CONNECTION_DATA mail addressed
 * to this link's handle -- not to some global one.
 */
static void test_outbound_message_becomes_an_append_mail(void)
{
	static const uint8_t port_id[] = { 0x10, 0x20, 0x30 };
	static const uint8_t fixed_id[] = { 0x11, 0x22, 0x33, 0x44, 0x55 };
	struct dect_link l;
	struct dect_mail m;

	init(&l);
	CHECK_EQ(dect_cc_setup(&l.cc, port_id, sizeof(port_id), fixed_id,
			       sizeof(fixed_id), 0x40), 0);
	dect_link_tick(&l, 0);			/* drains CC into LAPC */
	CHECK_EQ(dect_link_poll_append_mail(&l, &m), 1);
	CHECK_EQ(m.prim, 0x0244u);
	CHECK_EQ(m.param[0], 1u);		/* the link's handle */
	CHECK_EQ(l.lapc.outstanding, 1u);	/* class A window of one */
}

/*
 * The LAPC window is one. A second message must wait: queueing both would
 * lose the first, and that is protocol, not policy.
 */
static void test_only_one_message_is_outstanding(void)
{
	static const uint8_t port_id[] = { 0x10, 0x20, 0x30 };
	static const uint8_t fixed_id[] = { 0x11, 0x22, 0x33, 0x44, 0x55 };
	struct dect_link l;
	struct dect_mail m;
	int sent = 0;

	init(&l);
	dect_cc_setup(&l.cc, port_id, sizeof(port_id), fixed_id,
		      sizeof(fixed_id), 0x40);
	dect_cc_release(&l.cc);			/* a second message, queued */
	dect_link_tick(&l, 0);
	while (dect_link_poll_append_mail(&l, &m))
		sent++;
	CHECK_EQ(sent, 1);			/* only the first went out */
}

/*
 * A queued NWK message does NOT reach the wire without a tick.
 *
 * This is the trap that cost us every FP-originated call to a Gigaset A540
 * for a week. dect-stubd's poll loop open-coded dect_link_tick() as
 * dect_lapc_tick() + dect_cc_tick(), which looks equivalent and is not: it
 * drops drain_nwk(). A {CC-SETUP} queued by the ring state machine therefore
 * sat in the CC output queue until the handset happened to send us a segment,
 * because dect_link_rx_segment() is the only other caller of the drain.
 *
 * Measured at 3 860 ms late on hardware, which is long enough for the handset
 * to start a call of its own and turn ours into a {CC-SETUP} collision.
 *
 * So: polling the transmit queue is not enough on its own, and this test says
 * so out loud rather than leaving it to be rediscovered.
 */
static void test_a_queued_message_needs_a_tick(void)
{
	static const uint8_t port_id[] = { 0x10, 0x20, 0x30 };
	static const uint8_t fixed_id[] = { 0x11, 0x22, 0x33, 0x44, 0x55 };
	struct dect_link l;
	struct dect_mail m;

	init(&l);
	CHECK_EQ(dect_cc_setup(&l.cc, port_id, sizeof(port_id), fixed_id,
			       sizeof(fixed_id), 0x40), 0);

	/* Polling alone moves nothing: the message is still in CC. */
	CHECK_EQ(dect_link_poll_append_mail(&l, &m), 0);
	CHECK_EQ(l.lapc.outstanding, 0u);

	/* One tick drains it, and only then is there a frame to send. */
	dect_link_tick(&l, 0);
	CHECK_EQ(dect_link_poll_append_mail(&l, &m), 1);
	CHECK_EQ(m.prim, 0x0244u);
}

/*
 * A segment belonging to ANOTHER connection must not touch this link.
 *
 * A link serves one connection, and the 0x0203 segment carries the handle it
 * belongs to. That handle was decoded and never compared, so every handset on
 * air fed the same Lc reassembly, the same LAPC sequence numbers and the same
 * NWK state machines.
 *
 * With one handset on the bench this is invisible. On 2026-10-24, with three
 * handsets and a FRITZ!Fon C4 in range, there were 26-30 connections in 170 s
 * and an FP-originated {CC-SETUP} was never acknowledged by anyone -- the
 * verdict logic reported "refused" for a call nobody had answered.
 */
static void test_a_segment_for_another_handle_is_rejected(void)
{
	struct dect_link l;
	struct dect_mail m;
	struct dect_link_ind ind;
	uint8_t vs_before, vr_before;

	init(&l);				/* this link is handle 1 */
	vs_before = l.lapc.vs;
	vr_before = l.lapc.vr;

	memset(&m, 0, sizeof(m));
	m.prim = 0x0203u;
	m.param_len = 15;
	m.param[0] = 4;				/* a different connection */
	m.param[1] = 0x01;
	m.param[2] = 0x11;			/* plausible Cs octets */
	m.param[3] = 0x02;
	m.param[4] = 0x59;

	CHECK_EQ(dect_link_rx_segment(&l, &m, 0, NULL, NULL, &ind), -1);
	CHECK_EQ(ind.foreign, 1u);
	CHECK_EQ(ind.seg_handle, 4u);
	CHECK_EQ(ind.nwk_len, 0u);

	/* Nothing in the link moved. */
	CHECK_EQ(l.lapc.vs, vs_before);
	CHECK_EQ(l.lapc.vr, vr_before);
	CHECK_EQ(l.lapc.outstanding, 0u);

	/* And our own handle is still accepted afterwards. */
	m.param[0] = 1;
	CHECK_EQ(dect_link_rx_segment(&l, &m, 0, NULL, NULL, &ind), 0);
	CHECK_EQ(ind.foreign, 0u);
	CHECK_EQ(ind.seg_handle, 1u);
}

/* A segment that is not C-plane is rejected without disturbing the link. */
static void test_non_cplane_mail_is_rejected(void)
{
	struct dect_link l;
	struct dect_mail m;
	struct dect_link_ind ind;

	init(&l);
	memset(&m, 0, sizeof(m));
	m.prim = 0x0242u;			/* channel status, not C-plane */
	CHECK_EQ(dect_link_rx_segment(&l, &m, 0, NULL, NULL, &ind), -1);
	CHECK_EQ(l.lapc.outstanding, 0u);
}

/*
 * A segment we cannot use must still let our own queue drain. An incoming
 * call queues its {CC-SETUP} before the peer's first frame arrives, and an
 * early return once left it sitting in the queue for ever.
 */
static void test_unusable_segment_still_drains_the_queue(void)
{
	static const uint8_t port_id[] = { 0x10, 0x20, 0x30 };
	static const uint8_t fixed_id[] = { 0x11, 0x22, 0x33, 0x44, 0x55 };
	struct dect_link l;
	struct dect_mail m, out;
	struct dect_link_ind ind;

	init(&l);
	dect_cc_setup(&l.cc, port_id, sizeof(port_id), fixed_id,
		      sizeof(fixed_id), 0x40);

	/* A C-plane mail carrying a fragment that cannot complete a frame. */
	memset(&m, 0, sizeof(m));
	m.prim = 0x0203u;
	m.param_len = 15;
	m.param[0] = 1;
	m.param[1] = 0x01;
	dect_link_rx_segment(&l, &m, 0, NULL, NULL, &ind);

	CHECK_EQ(dect_link_poll_append_mail(&l, &out), 1);
	CHECK_EQ(out.prim, 0x0244u);
}

int main(void)
{
	test_init_is_quiet();
	test_two_links_are_independent();
	test_outbound_message_becomes_an_append_mail();
	test_a_queued_message_needs_a_tick();
	test_a_segment_for_another_handle_is_rejected();
	test_only_one_message_is_outstanding();
	test_non_cplane_mail_is_rejected();
	test_unusable_segment_still_drains_the_queue();
	return CHECK_DONE();
}

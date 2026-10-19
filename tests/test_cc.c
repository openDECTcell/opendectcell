// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for src/libopendectcell/dect_cc.c: the FP Call Control FSM for a basic-speech
 * call the portable part originates. No hardware.
 */
#include "check.h"
#include "../src/libopendectcell/dect_cc.h"
#include "../src/libopendectcell/dect_nwk.h"

#include <string.h>

static uint8_t msg_type_of(const uint8_t *msg, size_t len)
{
	struct dect_nwk_hdr h;
	const uint8_t *ies;
	size_t ies_len;

	if (dect_nwk_parse(msg, len, &h, &ies, &ies_len) < 0)
		return 0;
	return h.msg_type;
}

static uint8_t ti_flag_of(const uint8_t *msg, size_t len)
{
	struct dect_nwk_hdr h;
	const uint8_t *ies;
	size_t ies_len;

	dect_nwk_parse(msg, len, &h, &ies, &ies_len);
	return h.ti_flag;
}

/* PP CC-SETUP: BASIC-SERVICE (double-octet) + PORTABLE-IDENTITY (IPUI-like). */
static size_t pp_setup(uint8_t *out, size_t cap, uint8_t ti)
{
	uint8_t pid[7] = { 0x80, 0xB4, 0x00, 0x11, 0x22, 0x33, 0x44 };
	size_t off = dect_nwk_build_hdr(out, cap, DECT_PD_CC, DECT_TI_ORIGINATOR,
					ti, DECT_CC_SETUP);

	out[off++] = DECT_IE_BASIC_SERVICE;	/* 0xE0: double-octet element */
	out[off++] = 0x00;			/* call class 0, basic speech */
	return dect_nwk_put_ie(out, cap, off, DECT_IE_PORTABLE_IDENTITY, pid, sizeof(pid));
}

/* The handset dials: SETUP -> we take the call (CALL-PROC, CONNECT), it acks. */
static void test_incoming_setup(void)
{
	struct dect_cc cc;
	uint8_t buf[64], out[64];
	size_t n;

	dect_cc_init(&cc);

	n = pp_setup(buf, sizeof(buf), 0);
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(cc.stats.setup, 1);
	CHECK_EQ(cc.state, DECT_CC_ST_CONNECT_PENDING);

	/* First response is CALL-PROC, responder-side (F=1). */
	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK(n > 0);
	CHECK_EQ(msg_type_of(out, n), DECT_CC_CALL_PROC);
	CHECK_EQ(ti_flag_of(out, n), DECT_TI_RESPONDER);
	/* Then CONNECT. */
	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_CC_CONNECT);
	CHECK_EQ(ti_flag_of(out, n), DECT_TI_RESPONDER);
	/* Nothing more queued. */
	CHECK_EQ(dect_cc_poll(&cc, out, sizeof(out)), 0);

	/* PP CONNECT-ACK -> ACTIVE. */
	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_CC, DECT_TI_ORIGINATOR,
			       0, DECT_CC_CONNECT_ACK);
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(cc.state, DECT_CC_ST_ACTIVE);
	CHECK_EQ(cc.stats.connected, 1);
}

/* PP hangs up: CC-RELEASE -> we answer RELEASE-COM and go released. */
static void test_release_from_pp(void)
{
	struct dect_cc cc;
	uint8_t buf[64], out[64];
	size_t n;

	dect_cc_init(&cc);
	n = pp_setup(buf, sizeof(buf), 0);
	dect_cc_rx(&cc, buf, n);
	dect_cc_poll(&cc, out, sizeof(out));	/* CALL-PROC */
	dect_cc_poll(&cc, out, sizeof(out));	/* CONNECT */

	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_CC, DECT_TI_ORIGINATOR,
			       0, DECT_CC_RELEASE);
	dect_cc_rx(&cc, buf, n);
	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_CC_RELEASE_COM);
	CHECK_EQ(cc.state, DECT_CC_ST_RELEASED);
	CHECK_EQ(cc.stats.released, 1);
}

/* We initiate the hang-up on an active call. */
static void test_release_from_fp(void)
{
	struct dect_cc cc;
	uint8_t buf[64], out[64];
	size_t n;

	dect_cc_init(&cc);
	CHECK_EQ(dect_cc_release(&cc), -1);	/* nothing to release when idle */

	n = pp_setup(buf, sizeof(buf), 0);
	dect_cc_rx(&cc, buf, n);
	dect_cc_poll(&cc, out, sizeof(out));
	dect_cc_poll(&cc, out, sizeof(out));

	CHECK_EQ(dect_cc_release(&cc), 0);
	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_CC_RELEASE);
	CHECK_EQ(cc.state, DECT_CC_ST_RELEASED);
}

/* A non-CC message (MM PD) is ignored. */
static void test_ignores_non_cc(void)
{
	struct dect_cc cc;
	uint8_t buf[64], out[64];
	size_t n;

	dect_cc_init(&cc);
	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_MM, DECT_TI_ORIGINATOR,
			       0, DECT_MM_ACCESS_RIGHTS_REQUEST);
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(cc.state, DECT_CC_ST_NULL);
	CHECK_EQ(dect_cc_poll(&cc, out, sizeof(out)), 0);
}

/*
 * An FP-originated call: SETUP towards the handset, the handset rings
 * (ALERTING), the user answers (CONNECT) and we confirm with CONNECT-ACK.
 */
static void test_outgoing_call_to_handset(void)
{
	static const uint8_t port_id[] = { 0x80, 0xa8, 0x02, 0x58, 0xf4 };
	static const uint8_t fixed_id[] = { 0x20, 0xa0, 0xa5, 0x03, 0x69 };
	struct dect_cc cc;
	uint8_t out[64], msg[8];
	size_t n;

	dect_cc_init(&cc);
	CHECK_EQ(dect_cc_setup(&cc, port_id, sizeof(port_id),
			       fixed_id, sizeof(fixed_id), 0x88), 0);
	CHECK_EQ(cc.state, DECT_CC_ST_CALL_PRESENT);

	/* The queued SETUP: originator side, with the three mandatory IEs. */
	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(n, 2u + 2u + sizeof(port_id) + 2u + sizeof(fixed_id) + 2u);
	CHECK_EQ(out[1], DECT_CC_SETUP);
	CHECK_EQ(out[2], DECT_IE_PORTABLE_IDENTITY);
	CHECK_EQ(out[2 + 2 + sizeof(port_id)], DECT_IE_FIXED_IDENTITY);
	CHECK_EQ(out[n - 2], DECT_IE_BASIC_SERVICE);
	CHECK_EQ(out[n - 1], 0x88);

	/* The handset rings. */
	n = dect_nwk_build_hdr(msg, sizeof(msg), DECT_PD_CC, DECT_TI_RESPONDER,
			       0, DECT_CC_ALERTING);
	dect_cc_rx(&cc, msg, n);
	CHECK_EQ(cc.state, DECT_CC_ST_RINGING);
	CHECK_EQ(cc.stats.alerting, 1u);

	/* The user answers; we confirm and the call is active. */
	n = dect_nwk_build_hdr(msg, sizeof(msg), DECT_PD_CC, DECT_TI_RESPONDER,
			       0, DECT_CC_CONNECT);
	dect_cc_rx(&cc, msg, n);
	CHECK_EQ(cc.state, DECT_CC_ST_ACTIVE);
	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(n, 2u);
	CHECK_EQ(out[1], DECT_CC_CONNECT_ACK);
}

/* An ALERTING for a call we did not originate must not move the state. */
static void test_alerting_ignored_when_responder(void)
{
	struct dect_cc cc;
	uint8_t msg[8];
	size_t n;

	dect_cc_init(&cc);
	n = dect_nwk_build_hdr(msg, sizeof(msg), DECT_PD_CC, DECT_TI_RESPONDER,
			       0, DECT_CC_ALERTING);
	dect_cc_rx(&cc, msg, n);
	CHECK_EQ(cc.state, DECT_CC_ST_NULL);
	CHECK_EQ(cc.stats.alerting, 0u);
}

int main(void)
{
	test_outgoing_call_to_handset();
	test_alerting_ignored_when_responder();
	test_incoming_setup();
	test_release_from_pp();
	test_release_from_fp();
	test_ignores_non_cc();
	return CHECK_DONE();
}

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

/*
 * A SETUP from the handset carries both identities an FP-originated SETUP
 * needs; we keep them so a call towards the handset needs no guesswork.
 */
static void test_learns_identities_from_setup(void)
{
	static const uint8_t port_id[] = { 0x80, 0xa8, 0x02, 0xde, 0xad, 0x00, 0x02 };
	static const uint8_t fixed_id[] = { 0xa0, 0xa5, 0xde, 0xad, 0x00, 0x00, 0xb8 };
	struct dect_cc cc;
	uint8_t msg[64];
	size_t off;

	dect_cc_init(&cc);
	off = dect_nwk_build_hdr(msg, sizeof(msg), DECT_PD_CC,
				 DECT_TI_ORIGINATOR, 0, DECT_CC_SETUP);
	off = dect_nwk_put_ie(msg, sizeof(msg), off, DECT_IE_PORTABLE_IDENTITY,
			      port_id, sizeof(port_id));
	off = dect_nwk_put_ie(msg, sizeof(msg), off, DECT_IE_FIXED_IDENTITY,
			      fixed_id, sizeof(fixed_id));
	off = dect_nwk_put_fixed_ie(msg, sizeof(msg), off,
				    DECT_IE_BASIC_SERVICE, 0x88);
	dect_cc_rx(&cc, msg, off);

	CHECK_EQ(cc.peer_port_id_len, sizeof(port_id));
	CHECK_EQ(memcmp(cc.peer_port_id, port_id, sizeof(port_id)), 0);
	CHECK_EQ(cc.peer_fixed_id_len, sizeof(fixed_id));
	CHECK_EQ(memcmp(cc.peer_fixed_id, fixed_id, sizeof(fixed_id)), 0);
}

/*
 * dect_cc_busy() must be true for the whole life of a transaction, including
 * the window between an FP-originated SETUP and the handset answering. The
 * daemon uses it to decide whether a repeated chip connection-up is a new
 * link or the chip re-establishing one underneath a call; getting that wrong
 * lost a CC-CONNECT on hardware (run A2, 2026-10-20).
 */
static void test_busy_spans_the_whole_call(void)
{
	static const uint8_t port_id[] = { 0x20, 0x10, 0x00, 0x11, 0x22 };
	static const uint8_t fixed_id[] = { 0x20, 0x09, 0x88, 0x77, 0x66 };
	struct dect_cc cc;
	uint8_t msg[DECT_LAPC_MSG_MAX];
	size_t off;

	dect_cc_init(&cc);
	CHECK(!dect_cc_busy(&cc));		/* idle */

	CHECK_EQ(dect_cc_setup(&cc, port_id, sizeof(port_id),
			       fixed_id, sizeof(fixed_id), 0x88), 0);
	CHECK(dect_cc_busy(&cc));		/* SETUP sent, not yet answered */
	while (dect_cc_poll(&cc, msg, sizeof(msg)) > 0)
		;
	CHECK(dect_cc_busy(&cc));		/* still pending after draining */

	/* the handset rings */
	off = dect_nwk_build_hdr(msg, sizeof(msg), DECT_PD_CC,
				 DECT_TI_RESPONDER, 0, DECT_CC_ALERTING);
	dect_cc_rx(&cc, msg, off);
	CHECK(dect_cc_busy(&cc));

	/* the user answers */
	off = dect_nwk_build_hdr(msg, sizeof(msg), DECT_PD_CC,
				 DECT_TI_RESPONDER, 0, DECT_CC_CONNECT);
	dect_cc_rx(&cc, msg, off);
	CHECK(dect_cc_busy(&cc));
	CHECK(dect_cc_poll(&cc, msg, sizeof(msg)) > 0);	/* CONNECT-ACK */

	/* and hangs up */
	off = dect_nwk_build_hdr(msg, sizeof(msg), DECT_PD_CC,
				 DECT_TI_RESPONDER, 0, DECT_CC_RELEASE);
	dect_cc_rx(&cc, msg, off);
	while (dect_cc_poll(&cc, msg, sizeof(msg)) > 0)
		;
	CHECK(!dect_cc_busy(&cc));		/* released */
}

/*
 * A fresh transaction value per FP-originated call. Reusing 0 after a call
 * that ended without a clean release left a C4 acknowledging the next SETUP
 * at the link layer and then discarding it (hardware, 2026-10-20).
 */
static void test_transaction_value_rotates(void)
{
	static const uint8_t port_id[] = { 0x20, 0x10, 0x00, 0x11, 0x22 };
	static const uint8_t fixed_id[] = { 0x20, 0x09, 0x88, 0x77, 0x66 };
	struct dect_cc cc;
	uint8_t msg[DECT_LAPC_MSG_MAX];
	struct dect_nwk_hdr h;
	const uint8_t *ies;
	size_t ies_len, n;
	unsigned ti;

	for (ti = 0; ti < 8; ti++) {
		dect_cc_init(&cc);
		dect_cc_set_next_ti(&cc, (uint8_t)ti);
		CHECK_EQ(dect_cc_setup(&cc, port_id, sizeof(port_id),
				       fixed_id, sizeof(fixed_id), 0x88), 0);
		n = dect_cc_poll(&cc, msg, sizeof(msg));
		CHECK(n > 0);
		CHECK_EQ(dect_nwk_parse(msg, n, &h, &ies, &ies_len), 0);
		/* 7 wraps to 0: only 0..6 are valid for the originating side */
		CHECK_EQ(h.ti_value, ti % 7u);
		CHECK_EQ(h.ti_flag, DECT_TI_ORIGINATOR);
		CHECK_EQ(h.msg_type, DECT_CC_SETUP);
	}
}

/*
 * <<CALL-INFORMATION>> goes after everything else, caller identity included,
 * and dect_cc_init() forgets it -- so the daemon has to set it again on every
 * new link, exactly like <<SIGNAL>>.
 */
static void test_setup_carries_line_identifier(void)
{
	static const uint8_t port_id[] = { 0x80, 0xa8, 0x02, 0x58, 0xf4 };
	static const uint8_t fixed_id[] = { 0x20, 0xa0, 0xa5, 0x03, 0x69 };
	static const uint8_t ie[] = { 0x7E, 0x06, 0x00, 0x00, 0x80,
				      0x00, 0x05, 0x81 };
	const size_t plain = 2u + 2u + sizeof(port_id) + 2u + sizeof(fixed_id)
			     + 2u;
	struct dect_callinfo ci;
	struct dect_clip id;
	struct dect_cc cc;
	uint8_t out[DECT_LAPC_MSG_MAX];
	size_t n;

	dect_callinfo_init(&ci);
	ci.has_line = 1;
	ci.line_id = 0;
	ci.has_line_type = 1;
	ci.line_type = DECT_CALLINFO_NDT_DELAY;
	dect_clip_init(&id);
	CHECK_EQ(dect_clip_set_number(&id, "0123", DECT_CLIP_TON_UNKNOWN,
				      DECT_CLIP_NPI_E164,
				      DECT_CLIP_PRES_ALLOWED,
				      DECT_CLIP_SCR_USER_UNSCREENED), 0);

	dect_cc_init(&cc);
	dect_cc_set_callinfo(&cc, &ci);
	CHECK_EQ(dect_cc_setup_id(&cc, port_id, sizeof(port_id), fixed_id,
				  sizeof(fixed_id), 0x88, &id), 0);
	n = dect_cc_poll(&cc, out, sizeof(out));
	/* ... BASIC-SERVICE, CALLING-PARTY-NUMBER (2 + 2 + 4), then ours */
	CHECK_EQ(n, plain + 8u + sizeof(ie));
	CHECK_EQ(out[plain], DECT_IE_CALLING_PARTY_NUMBER);
	CHECK(memcmp(out + n - sizeof(ie), ie, sizeof(ie)) == 0);

	/* re-initialised: no IE, the G7 SETUP again */
	dect_cc_init(&cc);
	CHECK_EQ(dect_cc_setup(&cc, port_id, sizeof(port_id), fixed_id,
			       sizeof(fixed_id), 0x88), 0);
	CHECK_EQ(dect_cc_poll(&cc, out, sizeof(out)), plain);

	/* NULL clears; an invalid set is left out, the call still goes */
	dect_cc_init(&cc);
	dect_cc_set_callinfo(&cc, &ci);
	dect_cc_set_callinfo(&cc, NULL);
	CHECK_EQ(dect_cc_setup(&cc, port_id, sizeof(port_id), fixed_id,
			       sizeof(fixed_id), 0x88), 0);
	CHECK_EQ(dect_cc_poll(&cc, out, sizeof(out)), plain);

	dect_cc_init(&cc);
	ci.line_id = DECT_CALLINFO_LINE_NONE;
	dect_cc_set_callinfo(&cc, &ci);
	CHECK_EQ(dect_cc_setup(&cc, port_id, sizeof(port_id), fixed_id,
			       sizeof(fixed_id), 0x88), 0);
	CHECK_EQ(dect_cc_poll(&cc, out, sizeof(out)), plain);
}

int main(void)
{
	test_setup_carries_line_identifier();
	test_transaction_value_rotates();
	test_busy_spans_the_whole_call();
	test_learns_identities_from_setup();
	test_outgoing_call_to_handset();
	test_alerting_ignored_when_responder();
	test_incoming_setup();
	test_release_from_pp();
	test_release_from_fp();
	test_ignores_non_cc();
	return CHECK_DONE();
}

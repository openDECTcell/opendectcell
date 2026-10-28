// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for src/libopendectcell/dect_cc.c: the FP Call Control FSM for a basic-speech
 * call the portable part originates. No hardware.
 */
#include "check.h"
#include "../src/libopendectcell/dect_cc.h"
#include "../src/libopendectcell/dect_clip.h"
#include "../src/libopendectcell/dect_callinfo.h"
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
static size_t pp_setup_svc(uint8_t *out, size_t cap, uint8_t ti, uint8_t svc)
{
	uint8_t pid[7] = { 0x80, 0xB4, 0x00, 0x11, 0x22, 0x33, 0x44 };
	size_t off = dect_nwk_build_hdr(out, cap, DECT_PD_CC, DECT_TI_ORIGINATOR,
					ti, DECT_CC_SETUP);

	out[off++] = DECT_IE_BASIC_SERVICE;	/* 0xE0: double-octet element */
	out[off++] = svc;
	return dect_nwk_put_ie(out, cap, off, DECT_IE_PORTABLE_IDENTITY, pid, sizeof(pid));
}

static size_t pp_setup(uint8_t *out, size_t cap, uint8_t ti)
{
	return pp_setup_svc(out, cap, ti, 0x00);  /* call class 0, basic speech */
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
	CHECK_EQ(cc.state, DECT_CC_ST_ACTIVE);

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

static size_t pp_info(uint8_t *out, size_t cap, const char *digits,
		      int complete);
static uint8_t types_of(struct dect_cc *cc, uint8_t *types, size_t cap);

/*
 * A second call on the same link, after the first was released by either
 * side: the SETUP is taken, not ignored, and the old digits are gone.
 */
static void test_second_call_on_the_same_link(void)
{
	struct dect_cc cc;
	uint8_t buf[64], t[8];
	size_t n, kl;

	dect_cc_init(&cc);
	dect_cc_set_overlap(&cc, 1);
	n = pp_setup(buf, sizeof(buf), 0);
	dect_cc_rx(&cc, buf, n);
	n = pp_info(buf, sizeof(buf), "123", 0);
	dect_cc_rx(&cc, buf, n);
	(void)types_of(&cc, t, sizeof(t));
	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_CC, DECT_TI_ORIGINATOR,
			       0, DECT_CC_RELEASE);
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(types_of(&cc, t, sizeof(t)), 1u);
	CHECK_EQ(t[0], DECT_CC_RELEASE_COM);
	CHECK_EQ(cc.state, DECT_CC_ST_RELEASED);

	n = pp_setup(buf, sizeof(buf), 0);		/* the redial, same TI */
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(types_of(&cc, t, sizeof(t)), 1u);
	CHECK_EQ(t[0], DECT_CC_SETUP_ACK);
	CHECK_EQ(cc.state, DECT_CC_ST_OVERLAP);
	(void)dect_cc_keypad(&cc, &kl);
	CHECK_EQ(kl, 0u);

	/* after our own release, completed by the handset */
	CHECK_EQ(dect_cc_release(&cc), 0);
	(void)types_of(&cc, t, sizeof(t));
	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_CC, DECT_TI_ORIGINATOR,
			       0, DECT_CC_RELEASE_COM);
	dect_cc_rx(&cc, buf, n);
	n = pp_setup(buf, sizeof(buf), 1);
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(types_of(&cc, t, sizeof(t)), 1u);
	CHECK_EQ(t[0], DECT_CC_SETUP_ACK);
	CHECK_EQ(cc.ti_value, 1);
}

/* {CC-INFO} with given IEs on the call in progress, and only then. */
static void test_info_on_the_call(void)
{
	static const uint8_t md[] = { 0x28, 0x03, 'A', 'b', 'c' };
	struct dect_cc cc;
	uint8_t buf[64], out[64];
	size_t n;

	dect_cc_init(&cc);
	CHECK_EQ(dect_cc_info(&cc, md, sizeof(md)), -1);	/* no call */
	n = pp_setup(buf, sizeof(buf), 0);
	dect_cc_rx(&cc, buf, n);
	while (dect_cc_poll(&cc, out, sizeof(out)) > 0)
		;
	CHECK_EQ(dect_cc_info(&cc, md, sizeof(md)), 0);
	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_CC_INFO);
	CHECK_EQ(n, 2u + sizeof(md));
	CHECK(memcmp(out + 2, md, sizeof(md)) == 0);
	CHECK_EQ(out[0] & 0x80u, 0x80u);	/* responder: the PP's call */
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
/*
 * The exact {CC-IWU-INFO} a Gigaset A540 sends during call setup, copied from
 * work/box-tmp/vT3nb.log: `03 60 77 04 c3 00 05 00`. The handset opens four of
 * these per call and every one went unanswered before dect_iwu existed.
 */
static void test_answers_the_handsets_iwu_session_open(void)
{
	static const uint8_t a540_iwu_open[] = {
		0x03, 0x60, 0x77, 0x04, 0xC3, 0x00, 0x05, 0x00
	};
	struct dect_cc cc;
	struct dect_nwk_hdr h;
	const uint8_t *ies, *body;
	size_t ies_len, body_len, n;
	uint8_t out[64];

	dect_cc_init(&cc);
	dect_cc_rx(&cc, a540_iwu_open, sizeof(a540_iwu_open));

	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK(n > 0);
	CHECK_EQ(msg_type_of(out, n), DECT_CC_IWU_INFO);
	CHECK(dect_nwk_parse(out, n, &h, &ies, &ies_len) == 0);

	body = dect_nwk_find_ie(ies, ies_len, DECT_IE_IWU_TO_IWU, &body_len);
	CHECK(body != NULL);
	CHECK_EQ(body_len, 10u);
	CHECK_EQ(body[0], 0xC3u);
	CHECK_EQ(body[1], 0x01u);		/* start session confirm */
	CHECK_EQ(body[2], 0x05u);		/* the list it asked for (contacts) */
	/* ... which we do not serve: session 0, 'list not supported' */
	CHECK_EQ(body[3], 0x80u);
	CHECK_EQ(body[8], DECT_LIA_REJ_LIST_UNSUPPORTED);
	CHECK_EQ(cc.iwu.stats.rejected, 1u);
}

/*
 * A read on a served list answers with TWO messages on the same
 * transaction, the confirm and the data packet, each its own {CC-IWU-INFO}.
 */
static void test_list_read_queues_confirm_and_data_packet(void)
{
	static const uint8_t start[] = { 0x03, 0x60, 0x77, 0x04, 0xC3, 0x00, 0x00, 0x00 };
	static const uint8_t read[] = { 0x03, 0x60, 0x77, 0x07, 0xC3, 0x06, 0x81,
					0x81, 0x01, 0x00, 0x01 };
	struct dect_cc cc;
	struct dect_nwk_hdr h;
	const uint8_t *ies, *body;
	size_t ies_len, body_len, n;
	uint8_t out[64];

	dect_cc_init(&cc);
	dect_cc_rx(&cc, start, sizeof(start));
	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK(n > 0);
	dect_cc_rx(&cc, read, sizeof(read));
	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_CC_IWU_INFO);
	CHECK_EQ(ti_flag_of(out, n), DECT_TI_RESPONDER);
	dect_nwk_parse(out, n, &h, &ies, &ies_len);
	body = dect_nwk_find_ie(ies, ies_len, DECT_IE_IWU_TO_IWU, &body_len);
	CHECK(body != NULL);
	CHECK_EQ(body[1], DECT_LIA_READ_ENTRIES_CFM);
	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_CC_IWU_INFO);
	dect_nwk_parse(out, n, &h, &ies, &ies_len);
	body = dect_nwk_find_ie(ies, ies_len, DECT_IE_IWU_TO_IWU, &body_len);
	CHECK(body != NULL);
	CHECK_EQ(body[1], DECT_LIA_DATA_LAST);
	CHECK_EQ(dect_cc_poll(&cc, out, sizeof(out)), 0);
	CHECK_EQ(cc.iwu.stats.entries_sent, 1u);
}

/*
 * TS 102 527-3 7.4.10.6.1: a list-access service call (call class 0010) is
 * answered with {CC-CALL-PROC} alone and is never connected. A voice call
 * still gets CALL-PROC then CONNECT (the daemon is its own far end).
 */
static void test_list_access_call_gets_call_proc_only(void)
{
	struct dect_cc cc;
	uint8_t buf[64], out[64];
	size_t n;

	dect_cc_init(&cc);
	n = pp_setup_svc(buf, sizeof(buf), 0, 0x28);	/* LiA, wideband */
	dect_cc_rx(&cc, buf, n);
	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_CC_CALL_PROC);
	CHECK_EQ(dect_cc_poll(&cc, out, sizeof(out)), 0);
	CHECK_EQ(cc.state, DECT_CC_ST_PROCEEDING);
	CHECK_EQ(dect_cc_busy(&cc), 1);
	CHECK_EQ(dect_cc_peer_is_speech(&cc), 0);

	/* and it releases like any other call */
	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_CC, DECT_TI_ORIGINATOR,
			       0, DECT_CC_RELEASE);
	dect_cc_rx(&cc, buf, n);
	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_CC_RELEASE_COM);
	CHECK_EQ(cc.state, DECT_CC_ST_RELEASED);

	dect_cc_init(&cc);
	n = pp_setup_svc(buf, sizeof(buf), 0, 0x88);	/* normal, wideband */
	dect_cc_rx(&cc, buf, n);
	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_CC_CALL_PROC);
	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_CC_CONNECT);
	CHECK_EQ(cc.state, DECT_CC_ST_ACTIVE);
}

/*
 * A session open is not call state: answering one must not disturb the FSM,
 * or a dialog during setup would knock the call over.
 */
static void test_iwu_does_not_change_call_state(void)
{
	static const uint8_t a540_iwu_open[] = {
		0x03, 0x60, 0x77, 0x04, 0xC3, 0x00, 0x05, 0x00
	};
	struct dect_cc cc;

	dect_cc_init(&cc);
	dect_cc_rx(&cc, a540_iwu_open, sizeof(a540_iwu_open));
	CHECK_EQ(cc.state, DECT_CC_ST_NULL);
	CHECK_EQ(cc.stats.setup, 0u);
	CHECK_EQ(cc.stats.released, 0u);
}

/* An IWU-INFO with no <<IWU-TO-IWU>> is not something to answer. */
static void test_iwu_info_without_the_ie_is_ignored(void)
{
	struct dect_cc cc;
	uint8_t buf[64], out[64];
	size_t n;

	dect_cc_init(&cc);
	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_CC, DECT_TI_ORIGINATOR,
			       0, DECT_CC_IWU_INFO);
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(dect_cc_poll(&cc, out, sizeof(out)), 0);
}

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

	/*
	 * The queued SETUP: originator side, the three mandatory IEs, and
	 * <<SIGNAL>> 0x41 by default (EN 300 444 §8.14; without it the A540
	 * rings silently, run 18).
	 */
	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(n, 2u + 2u + sizeof(port_id) + 2u + sizeof(fixed_id) + 2u
		    + 2u);
	CHECK_EQ(out[1], DECT_CC_SETUP);
	CHECK_EQ(out[2], DECT_IE_PORTABLE_IDENTITY);
	CHECK_EQ(out[2 + 2 + sizeof(port_id)], DECT_IE_FIXED_IDENTITY);
	CHECK_EQ(out[n - 4], DECT_IE_BASIC_SERVICE);
	CHECK_EQ(out[n - 3], 0x88);
	CHECK_EQ(out[n - 2], DECT_IE_SIGNAL);
	CHECK_EQ(out[n - 1], DECT_SIGNAL_ALERT_PATTERN1);

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
 * EN 300 175-5 §17.3.2.1: a {CC-RELEASE} or {CC-RELEASE-COM} on a
 * transaction we do not hold gets no action, and messages on another
 * transaction must not move our call. The case that matters: the handset's
 * own list-access call (PT side, value 0) ends while our incoming call (FT
 * side, also value 0) is ringing. Its release used to release OUR call.
 */
static void test_foreign_transaction_does_not_touch_our_call(void)
{
	static const uint8_t port_id[] = { 0x80, 0xa8, 0x02, 0x58, 0xf4 };
	static const uint8_t fixed_id[] = { 0x20, 0xa0, 0xa5, 0x03, 0x69 };
	struct dect_cc cc;
	uint8_t buf[16], out[64];
	size_t n;

	dect_cc_init(&cc);
	CHECK_EQ(dect_cc_setup(&cc, port_id, sizeof(port_id),
			       fixed_id, sizeof(fixed_id), 0x88), 0);
	while (dect_cc_poll(&cc, out, sizeof(out)) > 0)
		;					/* our SETUP, F=0, TV 0 */

	/* The handset's transaction 0 (its flag 0) releases: not ours. */
	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_CC, DECT_TI_ORIGINATOR,
			       0, DECT_CC_RELEASE);
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(dect_cc_poll(&cc, out, sizeof(out)), 0);	/* no reply */
	CHECK_EQ(cc.state, DECT_CC_ST_CALL_PRESENT);
	CHECK_EQ(cc.stats.released, 0u);

	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_CC, DECT_TI_ORIGINATOR,
			       3, DECT_CC_RELEASE_COM);
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(cc.state, DECT_CC_ST_CALL_PRESENT);

	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_CC, DECT_TI_ORIGINATOR,
			       0, DECT_CC_ALERTING);
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(cc.state, DECT_CC_ST_CALL_PRESENT);
	CHECK_EQ(cc.stats.alerting, 0u);

	/* Our own transaction still works: F=1 (the peer's side), TV 0. */
	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_CC, DECT_TI_RESPONDER,
			       0, DECT_CC_ALERTING);
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(cc.state, DECT_CC_ST_RINGING);
	CHECK_EQ(cc.stats.alerting, 1u);
	CHECK_EQ(dect_cc_poll(&cc, out, sizeof(out)), 0);
}

/*
 * EN 300 444 §6.9.4: an unexpected or unrecognised CC message on the call
 * we hold is ignored. The default branch used to test the flag the wrong way
 * round and closed our own live call as an "unknown transaction". On a
 * transaction we do not hold, §17.3.2.1 still clears it.
 */
static void test_unhandled_message_on_our_call_is_ignored(void)
{
	static const uint8_t port_id[] = { 0x80, 0xa8, 0x02, 0x58, 0xf4 };
	static const uint8_t fixed_id[] = { 0x20, 0xa0, 0xa5, 0x03, 0x69 };
	static const uint8_t types[] = {
		DECT_CC_CALL_PROC, DECT_CC_SETUP_ACK, DECT_CC_NOTIFY,
		DECT_CC_FACILITY,
	};
	struct dect_cc cc;
	uint8_t buf[16], out[64];
	size_t n, i;

	dect_cc_init(&cc);
	CHECK_EQ(dect_cc_setup(&cc, port_id, sizeof(port_id),
			       fixed_id, sizeof(fixed_id), 0x88), 0);
	while (dect_cc_poll(&cc, out, sizeof(out)) > 0)
		;

	for (i = 0; i < sizeof(types); i++) {
		n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_CC,
				       DECT_TI_RESPONDER, 0, types[i]);
		dect_cc_rx(&cc, buf, n);
		CHECK_EQ(dect_cc_poll(&cc, out, sizeof(out)), 0);
		CHECK_EQ(cc.state, DECT_CC_ST_CALL_PRESENT);
	}
	CHECK_EQ(cc.stats.closed_unknown, 0u);

	/* The same message on a transaction we do not hold is closed. */
	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_CC, DECT_TI_ORIGINATOR,
			       6, DECT_CC_FACILITY);
	dect_cc_rx(&cc, buf, n);
	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(n, 4u);
	CHECK_EQ(out[0], 0xE3u);		/* F=1, TV=6, PD CC */
	CHECK_EQ(out[1], DECT_CC_RELEASE_COM);
	CHECK_EQ(out[3], DECT_CC_REASON_UNKNOWN_TRANSACTION);
	CHECK_EQ(cc.state, DECT_CC_ST_CALL_PRESENT);
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
 * EN 300 444 §6.9.2: an FP-originated call takes the lowest free transaction
 * value, which for this one-call entity is always 0 -- on a fresh entity and
 * again after a call has been released. The Gigaset A540 refuses a SETUP on
 * value 1 with 0x32 and rings on 0 (runs 15/16, docs/call-g7.md).
 */
static void test_transaction_value_is_lowest_free(void)
{
	static const uint8_t port_id[] = { 0x20, 0x10, 0x00, 0x11, 0x22 };
	static const uint8_t fixed_id[] = { 0x20, 0x09, 0x88, 0x77, 0x66 };
	struct dect_cc cc;
	uint8_t msg[DECT_LAPC_MSG_MAX];
	struct dect_nwk_hdr h;
	const uint8_t *ies;
	size_t ies_len, n, off;
	int call;

	dect_cc_init(&cc);
	for (call = 0; call < 2; call++) {
		CHECK_EQ(dect_cc_setup(&cc, port_id, sizeof(port_id),
				       fixed_id, sizeof(fixed_id), 0x88), 0);
		n = dect_cc_poll(&cc, msg, sizeof(msg));
		CHECK(n > 0);
		CHECK_EQ(msg[0], 0x03);		/* TI flag 0, value 0, PD CC */
		CHECK_EQ(dect_nwk_parse(msg, n, &h, &ies, &ies_len), 0);
		CHECK_EQ(h.ti_value, 0);
		CHECK_EQ(h.ti_flag, DECT_TI_ORIGINATOR);
		/* the handset refuses; the next call must still use 0 */
		off = dect_nwk_build_hdr(msg, sizeof(msg), DECT_PD_CC,
					 DECT_TI_RESPONDER, 0,
					 DECT_CC_RELEASE_COM);
		dect_cc_rx(&cc, msg, off);
		while (dect_cc_poll(&cc, msg, sizeof(msg)) > 0)
			;
		CHECK(!dect_cc_busy(&cc));
	}
}

/*
 * dect_cc_set_next_ti() is honoured, for tests that need another value, and
 * 7 (the extended-TI escape) wraps to 0.
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
 * new link. <<SIGNAL>>, by contrast, comes back with its default 0x41, so the
 * "plain" SETUP below includes it.
 */
static void test_setup_carries_line_identifier(void)
{
	static const uint8_t port_id[] = { 0x80, 0xa8, 0x02, 0x58, 0xf4 };
	static const uint8_t fixed_id[] = { 0x20, 0xa0, 0xa5, 0x03, 0x69 };
	static const uint8_t ie[] = { 0x7E, 0x06, 0x00, 0x00, 0x80,
				      0x00, 0x05, 0x81 };
	const size_t plain = 2u + 2u + sizeof(port_id) + 2u + sizeof(fixed_id)
			     + 2u + 2u;	/* BASIC-SERVICE, SIGNAL */
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
	/* ... BASIC-SERVICE, SIGNAL, CALLING-PARTY-NUMBER (2 + 2 + 4), then ours */
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


/*
 * [CR A-0055] The handset sends {CC-FACILITY} (0x62) with AVM proprietary
 * content. We cannot answer it, and silence is not neutral: vF423 measured
 * twelve retransmissions in sixty seconds, which keeps the link busy forever
 * and is why no call on a linked handset ever got audio. Close it instead.
 */
static void test_closes_a_facility_transaction(void)
{
	struct dect_cc cc;
	uint8_t buf[64], out[64];
	size_t n, m;

	dect_cc_init(&cc);
	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_CC, DECT_TI_ORIGINATOR,
			       6, DECT_CC_FACILITY);
	dect_cc_rx(&cc, buf, n);

	/* A {CC-RELEASE} goes out, in the handset's transaction, flag flipped. */
	m = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(m, 4u);
	CHECK_EQ(out[1], DECT_CC_RELEASE_COM);
	CHECK_EQ(out[2], DECT_IE_RELEASE_REASON);
	CHECK_EQ(out[3], DECT_CC_REASON_UNKNOWN_TRANSACTION);
	CHECK_EQ(cc.stats.closed_unknown, 1u);
	/* Our own call state is untouched. */
	CHECK_EQ(cc.state, DECT_CC_ST_NULL);
}

/* Closing an unknown transaction must never disturb a live call of ours. */
static void test_facility_does_not_disturb_a_live_call(void)
{
	static const uint8_t port_id[] = { 0x10, 0x20, 0x30 };
	static const uint8_t fixed_id[] = { 0x11, 0x22, 0x33, 0x44, 0x55 };
	struct dect_cc cc;
	uint8_t buf[64], out[128];
	size_t n;

	dect_cc_init(&cc);
	CHECK_EQ(dect_cc_setup(&cc, port_id, sizeof(port_id), fixed_id,
			       sizeof(fixed_id), 0x88), 0);
	while (dect_cc_poll(&cc, out, sizeof(out)) > 0)
		;			/* drain the SETUP */

	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_CC, DECT_TI_ORIGINATOR,
			       6, DECT_CC_FACILITY);
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(cc.stats.closed_unknown, 1u);
	CHECK_EQ(dect_cc_busy(&cc), 1);		/* the call is still up */
}

/*
 * The handset's own calls are not all speech calls. A CAT-iq handset opens
 * list-access calls (call class 0010) to read the phonebook; binding an audio
 * path to one of those is wrong, and we did it 145 times in one corpus.
 * EN 300 175-5 Â§7.7.5 figure 18: high nibble call class, low nibble service.
 */
/*
 * The identity gate. A CC transaction tells us who the peer is, and the
 * daemon persists that -- so it must be able to tell "the handset I am
 * subscribed to" from "some other handset that happened to speak first".
 * Ground-truth bytes are written out literally here rather than taken from
 * pp_setup(), so that a change to the helper cannot make this agree with
 * itself.
 */
static void test_peer_identity_gate(void)
{
	static const uint8_t subscribed[7] =
		{ 0x80, 0xB4, 0x00, 0x11, 0x22, 0x33, 0x44 };
	static const uint8_t other[7] =
		{ 0x80, 0xB4, 0x00, 0x11, 0x22, 0x33, 0x45 };	/* last byte */
	struct dect_cc cc;
	uint8_t buf[64];
	size_t n;

	dect_cc_init(&cc);
	n = pp_setup(buf, sizeof(buf), 0);
	dect_cc_rx(&cc, buf, n);

	/* The peer identity actually reached the CC entity. */
	CHECK_EQ(cc.peer_port_id_len, sizeof(subscribed));
	CHECK_EQ(memcmp(cc.peer_port_id, subscribed, sizeof(subscribed)), 0);

	/* Same handset. */
	CHECK_EQ(dect_cc_peer_is(&cc, subscribed, sizeof(subscribed)), 1);
	/* A different handset, differing in one byte, must not pass. */
	CHECK_EQ(dect_cc_peer_is(&cc, other, sizeof(other)), 0);
	/* A prefix is not a match: the length has to agree too. */
	CHECK_EQ(dect_cc_peer_is(&cc, subscribed, sizeof(subscribed) - 1), 0);
	/* No subscribed identity -- nothing to contradict, caller decides. */
	CHECK_EQ(dect_cc_peer_is(&cc, NULL, 0), 1);
}

static void test_peer_basic_service_decides_speech(void)
{
	static const struct { uint8_t svc; int speech; } cases[] = {
		{ 0x80, 1 },	/* normal call, basic speech */
		{ 0x88, 1 },	/* normal call, wideband speech */
		{ 0x8B, 1 },	/* normal call, DTAM wideband speech */
		{ 0x28, 0 },	/* LiA service call -- the A540's phonebook read */
		{ 0x20, 0 },	/* LiA, basic speech nibble: still not a speech call */
		{ 0x40, 0 },	/* message call setup */
		{ 0x39, 0 },	/* ULE, light data services */
		{ 0xC8, 1 },	/* external handover of a wideband call */
		{ 0x90, 1 },	/* internal call, basic speech */
		{ 0xB0, 0 },	/* service call setup: not a speech call */
		{ 0x80 | 0x9u, 0 },	/* normal class, SUOTA data service */
	};
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct dect_cc cc;
		uint8_t buf[64];
		size_t n;

		dect_cc_init(&cc);
		n = pp_setup_svc(buf, sizeof(buf), 0, cases[i].svc);
		dect_cc_rx(&cc, buf, n);
		CHECK_EQ(cc.peer_basic_service, cases[i].svc);
		CHECK_EQ(dect_cc_peer_is_speech(&cc), cases[i].speech);
		/*
		 * The call itself is answered either way -- list access works.
		 * A LiA service call (class 0010) is only CALL-PROC'd, never
		 * connected (TS 102 527-3 7.4.10.6.1), and an internal call
		 * (class 1001) waits for the owner to reach the other handset;
		 * every other class is taken through to CONNECT.
		 */
		CHECK_EQ(cc.state, (cases[i].svc >> 4) == 0x2u ||
			 (cases[i].svc >> 4) == 0x9u
			 ? DECT_CC_ST_PROCEEDING : DECT_CC_ST_ACTIVE);
	}
}

/* No <<BASIC-SERVICE>> at all is not a speech call, and must not read as one. */
static void test_setup_without_basic_service_is_not_speech(void)
{
	uint8_t pid[7] = { 0x80, 0xB4, 0x00, 0x11, 0x22, 0x33, 0x44 };
	struct dect_cc cc;
	uint8_t buf[64];
	size_t off, n;

	dect_cc_init(&cc);
	CHECK_EQ(cc.peer_basic_service, DECT_CC_SVC_NONE);
	off = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_CC,
				 DECT_TI_ORIGINATOR, 0, DECT_CC_SETUP);
	n = dect_nwk_put_ie(buf, sizeof(buf), off, DECT_IE_PORTABLE_IDENTITY,
			    pid, sizeof(pid));
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(cc.peer_basic_service, DECT_CC_SVC_NONE);
	CHECK_EQ(dect_cc_peer_is_speech(&cc), 0);
}

/*
 * 0xE0 also occurs inside other elements' payloads. The portable identity
 * below carries one, and a byte scan would read the octet after it as the
 * basic service; the element walk must not.
 */
static void test_basic_service_is_not_found_inside_a_payload(void)
{
	uint8_t pid[7] = { 0x80, 0xB4, 0xE0, 0x88, 0x22, 0x33, 0x44 };
	struct dect_cc cc;
	uint8_t buf[64];
	size_t off, n;

	dect_cc_init(&cc);
	off = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_CC,
				 DECT_TI_ORIGINATOR, 0, DECT_CC_SETUP);
	n = dect_nwk_put_ie(buf, sizeof(buf), off, DECT_IE_PORTABLE_IDENTITY,
			    pid, sizeof(pid));
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(cc.peer_basic_service, DECT_CC_SVC_NONE);
	CHECK_EQ(dect_cc_peer_is_speech(&cc), 0);
}

/*
 * GAP N.4, "dialled digits (basic)", procedure 8.10 -- mandatory for an FT
 * (EN 300 444 table 1) and absent from this project until 2026-10-23.
 *
 * The frame below is not constructed: it is the exact byte sequence a handset
 * sent us, captured in seven runs in the corpus and dropped every time.
 *   03 7b 2c 05 31 32 33 34 35
 *   |  |  |  |  '-- "12345"
 *   |  |  |  '----- length 5
 *   |  |  '-------- <<MULTI-KEYPAD>>
 *   |  '----------- {CC-INFO}
 *   '-------------- TI 0, PD 3 (CC)
 */
static void test_collects_the_dialled_digits_from_a_real_capture(void)
{
	static const uint8_t cc_info[] = {
		0x03, 0x7B, 0x2C, 0x05, 0x31, 0x32, 0x33, 0x34, 0x35
	};
	struct dect_cc cc;
	uint8_t buf[64];
	const uint8_t *kp;
	size_t n, len;

	dect_cc_init(&cc);
	CHECK(dect_cc_keypad(&cc, &len) == NULL);
	CHECK_EQ(len, 0u);

	/* The handset dials: its SETUP first, so we are in a call. */
	n = pp_setup(buf, sizeof(buf), 0);
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(cc.state, DECT_CC_ST_ACTIVE);
	while (dect_cc_poll(&cc, buf, sizeof(buf)) > 0)
		;		/* drain CALL-PROC and CONNECT */

	dect_cc_rx(&cc, cc_info, sizeof(cc_info));
	CHECK_EQ(cc.stats.keypad, 1u);
	kp = dect_cc_keypad(&cc, &len);
	CHECK(kp != NULL);
	CHECK_EQ(len, 5u);
	CHECK_EQ(memcmp(kp, "12345", 5), 0);
	CHECK_EQ(cc.keypad_overflow, 0u);

	/* No reply: 8.10 leaves the FT in the same state, LAPC has acked. */
	CHECK_EQ(dect_cc_poll(&cc, buf, sizeof(buf)), 0);
	CHECK_EQ(cc.state, DECT_CC_ST_ACTIVE);
}

/* "in one or several {CC-INFO} messages" -- digits append in arrival order. */
static void test_digits_accumulate_across_messages(void)
{
	struct dect_cc cc;
	uint8_t buf[64];
	const uint8_t *kp;
	size_t n, len, off;

	dect_cc_init(&cc);
	n = pp_setup(buf, sizeof(buf), 0);
	dect_cc_rx(&cc, buf, n);

	for (off = 0; off < 4; off++) {
		uint8_t m[8];
		size_t k = dect_nwk_build_hdr(m, sizeof(m), DECT_PD_CC,
					      DECT_TI_ORIGINATOR, 0,
					      DECT_CC_INFO);
		uint8_t d = (uint8_t)('0' + off);

		k = dect_nwk_put_ie(m, sizeof(m), k, DECT_IE_MULTI_KEYPAD,
				    &d, 1);
		dect_cc_rx(&cc, m, k);
	}
	kp = dect_cc_keypad(&cc, &len);
	CHECK_EQ(len, 4u);
	CHECK_EQ(memcmp(kp, "0123", 4), 0);
	CHECK_EQ(cc.stats.keypad, 4u);

	dect_cc_clear_keypad(&cc);
	CHECK(dect_cc_keypad(&cc, &len) == NULL);
	CHECK_EQ(len, 0u);
}

/* More than the buffer holds is truncated and flagged, never overrun. */
static void test_keypad_overflow_is_bounded_and_flagged(void)
{
	uint8_t big[DECT_CC_KEYPAD_MAX + 8];
	struct dect_cc cc;
	uint8_t m[128];
	size_t k, len;

	memset(big, '7', sizeof(big));
	dect_cc_init(&cc);
	k = pp_setup(m, sizeof(m), 0);
	dect_cc_rx(&cc, m, k);

	k = dect_nwk_build_hdr(m, sizeof(m), DECT_PD_CC, DECT_TI_ORIGINATOR,
			       0, DECT_CC_INFO);
	k = dect_nwk_put_ie(m, sizeof(m), k, DECT_IE_MULTI_KEYPAD,
			    big, sizeof(big));
	dect_cc_rx(&cc, m, k);

	dect_cc_keypad(&cc, &len);
	CHECK_EQ(len, (size_t)DECT_CC_KEYPAD_MAX);
	CHECK_EQ(cc.keypad_overflow, 1u);
}

/* En-bloc: EN 300 175-5 9.3.1.5 allows the element in the {CC-SETUP} itself. */
static void test_keypad_in_the_setup_is_collected(void)
{
	uint8_t pid[7] = { 0x80, 0xB4, 0x00, 0x11, 0x22, 0x33, 0x44 };
	struct dect_cc cc;
	uint8_t buf[64];
	size_t off, n, len;

	dect_cc_init(&cc);
	off = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_CC,
				 DECT_TI_ORIGINATOR, 0, DECT_CC_SETUP);
	off = dect_nwk_put_ie(buf, sizeof(buf), off, DECT_IE_PORTABLE_IDENTITY,
			      pid, sizeof(pid));
	n = dect_nwk_put_ie(buf, sizeof(buf), off, DECT_IE_MULTI_KEYPAD,
			    (const uint8_t *)"*21#", 4);
	dect_cc_rx(&cc, buf, n);

	CHECK(dect_cc_keypad(&cc, &len) != NULL);
	CHECK_EQ(len, 4u);
	CHECK_EQ(memcmp(dect_cc_keypad(&cc, &len), "*21#", 4), 0);
}

/* A {CC-INFO} on a dead transaction is ignored -- and must not close it. */
static void test_keypad_outside_a_call_is_ignored(void)
{
	static const uint8_t cc_info[] = {
		0x03, 0x7B, 0x2C, 0x05, 0x31, 0x32, 0x33, 0x34, 0x35
	};
	struct dect_cc cc;
	uint8_t out[64];
	size_t len;

	dect_cc_init(&cc);
	dect_cc_rx(&cc, cc_info, sizeof(cc_info));
	CHECK(dect_cc_keypad(&cc, &len) == NULL);
	CHECK_EQ(cc.stats.keypad, 0u);
	CHECK_EQ(cc.state, DECT_CC_ST_NULL);
	CHECK_EQ(dect_cc_poll(&cc, out, sizeof(out)), 0);
}

/*
 * EN 300 175-5 §7.6: "the variable-length information elements are
 * transmitted over the air according to the ascending numerical order of
 * their identifiers. This allows receiving equipment to detect the presence
 * or absence of a particular information element without scanning through an
 * entire message."
 *
 * So a receiver may stop looking once it has passed the identifier it wants,
 * and a descending step can make a conforming handset skip everything after
 * it. We shipped exactly that for part of 2026-10-23 -- the codec list
 * (0x7C) went out ahead of the caller identity (0x6C) -- which is why this is
 * an invariant over the whole built message rather than a check of one pair.
 *
 * Fixed-length elements (bit 8 set) are a separate class and do not take part
 * in the ordering, so they are skipped here exactly as dect_nwk_find_ie()
 * skips them.
 */
static void check_ie_order_ascending(const uint8_t *msg, size_t len)
{
	struct dect_nwk_hdr h;
	const uint8_t *ies;
	size_t ies_len, pos = 0;
	int last = -1;

	CHECK(dect_nwk_parse(msg, len, &h, &ies, &ies_len) == 0);
	while (pos < ies_len) {
		uint8_t ie = ies[pos];

		if (ie & 0x80u) {		/* fixed length: 1 or 2 octets */
			pos += ((ie & 0xF0u) == 0xE0u) ? 2 : 1;
			continue;
		}
		if (pos + 2 > ies_len)
			break;
		CHECK(ie > last);		/* strictly ascending */
		last = (int)ie;
		pos += 2 + ies[pos + 1];
	}
}

/* The full FP-originated SETUP: identities, basic service, signal, caller
 * identity, codec list and call information, all at once. */
static void test_setup_ie_order_is_ascending(void)
{
	static const uint8_t port_id[] = { 0x10, 0x20, 0x30 };
	static const uint8_t fixed_id[] = { 0x11, 0x22, 0x33, 0x44, 0x55 };
	static const uint8_t codecs[] = { 0x90, 0x02, 0x00, 0x84 };
	struct dect_callinfo ci;
	struct dect_clip id;
	struct dect_cc cc;
	uint8_t out[256];
	size_t n;
	int hint;

	for (hint = 0; hint <= 1; hint++) {
		dect_cc_init(&cc);
		dect_cc_set_codec_list(&cc, codecs, sizeof(codecs));
		dect_cc_set_signal(&cc, 1, DECT_SIGNAL_ALERT_PATTERN0);

		dect_callinfo_init(&ci);
		ci.has_line = 1;
		ci.line_id = 1;
		dect_cc_set_callinfo(&cc, &ci);

		memset(&id, 0, sizeof(id));
		dect_clip_set_number(&id, "0123456789",
				     DECT_CLIP_TON_NATIONAL,
				     DECT_CLIP_NPI_E164,
				     DECT_CLIP_PRES_ALLOWED,
				     DECT_CLIP_SCR_NETWORK);
		dect_clip_set_name(&id, "Ada Lovelace",
				   DECT_CLIP_ALPHABET_DECT,
				   DECT_CLIP_PRES_ALLOWED,
				   DECT_CLIP_SCR_NETWORK);
		id.emit_before_basic_service = (uint8_t)hint;

		CHECK_EQ(dect_cc_setup_id(&cc, port_id, sizeof(port_id),
					  fixed_id, sizeof(fixed_id),
					  0x80, &id), 0);
		n = dect_cc_poll(&cc, out, sizeof(out));
		CHECK(n > 0);
		check_ie_order_ascending(out, n);

		/* and the caller identity really is in there */
		{
			struct dect_nwk_hdr h;
			const uint8_t *ies, *p;
			size_t ies_len, plen;

			dect_nwk_parse(out, n, &h, &ies, &ies_len);
			p = dect_nwk_find_ie(ies, ies_len,
					     DECT_IE_CALLING_PARTY_NUMBER,
					     &plen);
			CHECK(p != NULL);
			p = dect_nwk_find_ie(ies, ies_len,
					     DECT_IE_CALLING_PARTY_NAME,
					     &plen);
			CHECK(p != NULL);
		}
	}
}


/*
 * F<CC.03>, the CC setup timer (EN 300 175-5 A.1 and 9.3.2.2). 20 s, started
 * when we send a {CC-SETUP}; on expiry the FT shall reject the call with
 * {CC-RELEASE-COM} / "timer expiry" and enter NULL.
 *
 * These tests were missing until 2026-10-23: the timer had been implemented
 * on 2026-10-22 and nothing exercised it. A 20-second fuse whose whole job is
 * to fire when nothing else happens is exactly the kind of code that stops
 * working in silence, and the conformance table still said "missing" for a
 * day after it was written.
 */
static size_t fp_setup(struct dect_cc *cc, uint8_t *out, size_t cap)
{
	static const uint8_t port_id[] = { 0x80, 0xa8, 0x02, 0x58, 0xf4 };
	static const uint8_t fixed_id[] = { 0x20, 0xa0, 0xa5, 0x03, 0x69 };

	dect_cc_init(cc);
	CHECK_EQ(dect_cc_setup(cc, port_id, sizeof(port_id),
			       fixed_id, sizeof(fixed_id), 0x80), 0);
	CHECK_EQ(cc->state, DECT_CC_ST_CALL_PRESENT);
	return dect_cc_poll(cc, out, cap);	/* drain the SETUP */
}

static void test_setup_timer_releases_an_unanswered_call(void)
{
	struct dect_cc cc;
	uint8_t out[64];
	size_t n;

	CHECK(fp_setup(&cc, out, sizeof(out)) > 0);

	/*
	 * The deadline is armed by the first tick in CALL PRESENT, not by
	 * dect_cc_setup(), so it is 20 s from that tick. A caller that never
	 * ticks never arms it -- which is worth stating in a test, because it
	 * is the one way this timer can silently not exist.
	 */
	CHECK_EQ(dect_cc_next_deadline(&cc), UINT64_MAX);
	dect_cc_tick(&cc, 1000u);
	CHECK_EQ(dect_cc_next_deadline(&cc), 21000u);

	/* One millisecond early: nothing. */
	dect_cc_tick(&cc, 20999u);
	CHECK_EQ(cc.state, DECT_CC_ST_CALL_PRESENT);
	CHECK_EQ(dect_cc_poll(&cc, out, sizeof(out)), 0u);
	CHECK_EQ(cc.stats.setup_timeouts, 0u);

	/* On the deadline: release, with a reason, and back to NULL. */
	dect_cc_tick(&cc, 21000u);
	CHECK_EQ(cc.state, DECT_CC_ST_NULL);
	CHECK_EQ(cc.stats.setup_timeouts, 1u);
	CHECK_EQ(dect_cc_next_deadline(&cc), UINT64_MAX);

	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK(n >= 4u);
	if (n >= 4u) {
		CHECK_EQ(msg_type_of(out, n), DECT_CC_RELEASE_COM);
		CHECK_EQ(ti_flag_of(out, n), DECT_TI_ORIGINATOR);
		/*
		 * <<RELEASE-REASON>> 0x0D, "timer expiry" -- a bare
		 * RELEASE-COM is legal but tells the handset nothing.
		 */
		CHECK_EQ(out[n - 2], DECT_IE_RELEASE_REASON);
		CHECK_EQ(out[n - 1], 0x0Du);
	}

	/* It fires once, not once per tick. */
	dect_cc_tick(&cc, 60000u);
	CHECK_EQ(dect_cc_poll(&cc, out, sizeof(out)), 0u);
	CHECK_EQ(cc.stats.setup_timeouts, 1u);
}

static void test_setup_timer_stops_when_the_handset_answers(void)
{
	struct dect_cc cc;
	uint8_t out[64], msg[8];
	size_t n;

	CHECK(fp_setup(&cc, out, sizeof(out)) > 0);
	dect_cc_tick(&cc, 1000u);
	CHECK_EQ(dect_cc_next_deadline(&cc), 21000u);

	/*
	 * A.1 stops the timer when "a response message has been received",
	 * and every response moves the entity out of CALL PRESENT. So the
	 * state is the stop condition; there is no separate stop call, and
	 * this test is what keeps that equivalence honest.
	 */
	n = dect_nwk_build_hdr(msg, sizeof(msg), DECT_PD_CC, DECT_TI_RESPONDER,
			       0, DECT_CC_ALERTING);
	dect_cc_rx(&cc, msg, n);
	CHECK_EQ(cc.state, DECT_CC_ST_RINGING);

	dect_cc_tick(&cc, 21000u);
	CHECK_EQ(dect_cc_next_deadline(&cc), UINT64_MAX);
	CHECK_EQ(cc.state, DECT_CC_ST_RINGING);
	CHECK_EQ(dect_cc_poll(&cc, out, sizeof(out)), 0u);
	CHECK_EQ(cc.stats.setup_timeouts, 0u);

	/* And long past it, the call is still up. */
	dect_cc_tick(&cc, 120000u);
	CHECK_EQ(cc.state, DECT_CC_ST_RINGING);
	CHECK_EQ(dect_cc_poll(&cc, out, sizeof(out)), 0u);
}

static void test_setup_timer_does_not_run_for_a_pp_originated_call(void)
{
	struct dect_cc cc;
	uint8_t out[64], msg[64];
	size_t n;

	/*
	 * F<CC.03> starts when *we* send a SETUP. A call the handset
	 * originates must never be torn down by it, however long the far end
	 * takes to answer.
	 *
	 * This test pins the BEHAVIOUR, not the `!cc->incoming` branch in
	 * dect_cc_tick(). Mutation-testing it on 2026-10-23 showed that
	 * deleting that term changes nothing: a PP-originated SETUP goes to
	 * CONNECT PENDING (on_setup() queues CALL-PROC and CONNECT at once)
	 * and never sits in CALL PRESENT, so the state guard alone already
	 * excludes it, and `incoming == 0 && state == CALL_PRESENT` is
	 * unreachable through the public API. The term is defensive and
	 * currently redundant -- worth knowing before someone "simplifies"
	 * either it or this test.
	 */
	dect_cc_init(&cc);
	n = pp_setup_svc(msg, sizeof(msg), 0, 0x80);
	dect_cc_rx(&cc, msg, n);
	CHECK_EQ(cc.incoming, 0u);
	while (dect_cc_poll(&cc, out, sizeof(out)) > 0)
		;

	dect_cc_tick(&cc, 1000u);
	CHECK_EQ(dect_cc_next_deadline(&cc), UINT64_MAX);
	dect_cc_tick(&cc, 300000u);
	CHECK_EQ(dect_cc_poll(&cc, out, sizeof(out)), 0u);
	CHECK_EQ(cc.stats.setup_timeouts, 0u);
}

/*
 * EN 300 175-5 §9.5.2.3, the FT's half: while our incoming call is in CALL
 * PRESENT, a {CC-SETUP} from the handset on its own transaction is a setup
 * collision. We release THAT call with {CC-RELEASE-COM} "insufficient
 * resources" on its transaction (flag inverted, its value), and our call is
 * untouched. Measured on the A540: it opens a list-access call 60-160 ms after
 * answering the page, every time, and we used to ignore it.
 */
static void test_setup_collision_releases_the_handsets_call(void)
{
	static const uint8_t port_id[] = { 0x80, 0xa8, 0x02, 0x58, 0xf4 };
	static const uint8_t fixed_id[] = { 0x20, 0xa0, 0xa5, 0x03, 0x69 };
	struct dect_cc cc;
	uint8_t buf[64], out[64];
	size_t n;

	dect_cc_init(&cc);
	dect_cc_set_next_ti(&cc, 1);
	CHECK_EQ(dect_cc_setup(&cc, port_id, sizeof(port_id),
			       fixed_id, sizeof(fixed_id), 0x80), 0);
	n = dect_cc_poll(&cc, out, sizeof(out));	/* our SETUP, TV 1 */
	CHECK_EQ(out[0] >> 4, 0x1u);			/* F=0, TV=1 */

	/* The handset's own call crosses ours: SETUP, F=0, TV 0, LiA class. */
	n = pp_setup_svc(buf, sizeof(buf), 0, 0x28);
	dect_cc_rx(&cc, buf, n);

	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(n, 4u);
	CHECK_EQ(out[0], 0x83u);			/* F=1, TV=0, PD CC */
	CHECK_EQ(out[1], DECT_CC_RELEASE_COM);
	CHECK_EQ(out[2], DECT_IE_RELEASE_REASON);
	CHECK_EQ(out[3], DECT_CC_REASON_INSUFFICIENT_RESOURCES);
	CHECK_EQ(cc.stats.collisions, 1u);
	CHECK_EQ(cc.stats.closed_unknown, 0u);
	/* Ours is still pending, on its own transaction. */
	CHECK_EQ(cc.state, DECT_CC_ST_CALL_PRESENT);
	CHECK_EQ(cc.ti_value, 1u);
	CHECK_EQ(cc.incoming, 1u);
	CHECK_EQ(dect_cc_poll(&cc, out, sizeof(out)), 0);
}

/*
 * §9.5.2.3, the other half: the handset releases OUR call with "insufficient
 * resources" (its {CC-SETUP} crossed ours, or was about to). We re-present
 * the identical {CC-SETUP} at once, at most DECT_CC_REPRESENT_MAX times, and
 * F<CC.03> restarts. A third such release ends the call.
 */
static void test_insufficient_resources_represents_the_setup(void)
{
	static const uint8_t port_id[] = { 0x80, 0xa8, 0x02, 0x58, 0xf4 };
	static const uint8_t fixed_id[] = { 0x20, 0xa0, 0xa5, 0x03, 0x69 };
	struct dect_cc cc;
	uint8_t first[64], out[64], rel[8];
	size_t n, first_len, off;
	unsigned i;

	dect_cc_init(&cc);
	dect_cc_set_next_ti(&cc, 1);
	CHECK_EQ(dect_cc_setup(&cc, port_id, sizeof(port_id),
			       fixed_id, sizeof(fixed_id), 0x80), 0);
	first_len = dect_cc_poll(&cc, first, sizeof(first));
	CHECK(first_len > 0);
	dect_cc_tick(&cc, 1000);			/* arms F<CC.03> */
	CHECK_EQ(dect_cc_next_deadline(&cc), 21000u);

	/* {CC-RELEASE-COM} (1, TV1) <<RELEASE-REASON>> insufficient resources */
	off = dect_nwk_build_hdr(rel, sizeof(rel), DECT_PD_CC, DECT_TI_RESPONDER,
				 1, DECT_CC_RELEASE_COM);
	off = dect_nwk_put_fixed_ie(rel, sizeof(rel), off, DECT_IE_RELEASE_REASON,
				    DECT_CC_REASON_INSUFFICIENT_RESOURCES);

	for (i = 1; i <= DECT_CC_REPRESENT_MAX; i++) {
		dect_cc_rx(&cc, rel, off);
		CHECK_EQ(cc.state, DECT_CC_ST_CALL_PRESENT);
		CHECK_EQ(cc.stats.represented, i);
		n = dect_cc_poll(&cc, out, sizeof(out));
		CHECK_EQ(n, first_len);
		CHECK_EQ(memcmp(out, first, first_len), 0);	/* byte-identical */
		/* F<CC.03> was restarted: it is armed afresh on the next tick. */
		CHECK_EQ(dect_cc_next_deadline(&cc), UINT64_MAX);
		dect_cc_tick(&cc, 5000 * i);
		CHECK_EQ(dect_cc_next_deadline(&cc), 5000u * i + 20000u);
	}

	/* The budget is spent: the next one releases the call for real. */
	dect_cc_rx(&cc, rel, off);
	CHECK_EQ(cc.state, DECT_CC_ST_RELEASED);
	CHECK_EQ(cc.stats.represented, DECT_CC_REPRESENT_MAX);
	CHECK_EQ(dect_cc_poll(&cc, out, sizeof(out)), 0);
	CHECK_EQ(dect_cc_busy(&cc), 0);
}

/* Any other release reason, or a release on another transaction, is final. */
static void test_other_release_reasons_are_not_represented(void)
{
	static const uint8_t port_id[] = { 0x80, 0xa8, 0x02, 0x58, 0xf4 };
	static const uint8_t fixed_id[] = { 0x20, 0xa0, 0xa5, 0x03, 0x69 };
	struct dect_cc cc;
	uint8_t out[64], rel[8];
	size_t off;

	/* "User busy" on our transaction: released, nothing re-sent. */
	dect_cc_init(&cc);
	dect_cc_set_next_ti(&cc, 1);
	CHECK_EQ(dect_cc_setup(&cc, port_id, sizeof(port_id),
			       fixed_id, sizeof(fixed_id), 0x80), 0);
	dect_cc_poll(&cc, out, sizeof(out));
	off = dect_nwk_build_hdr(rel, sizeof(rel), DECT_PD_CC, DECT_TI_RESPONDER,
				 1, DECT_CC_RELEASE_COM);
	off = dect_nwk_put_fixed_ie(rel, sizeof(rel), off, DECT_IE_RELEASE_REASON,
				    0x14);
	dect_cc_rx(&cc, rel, off);
	CHECK_EQ(cc.state, DECT_CC_ST_RELEASED);
	CHECK_EQ(cc.stats.represented, 0u);
	CHECK_EQ(dect_cc_poll(&cc, out, sizeof(out)), 0);

	/* A bare {CC-RELEASE-COM} with no reason at all: likewise. */
	dect_cc_init(&cc);
	dect_cc_set_next_ti(&cc, 1);
	CHECK_EQ(dect_cc_setup(&cc, port_id, sizeof(port_id),
			       fixed_id, sizeof(fixed_id), 0x80), 0);
	dect_cc_poll(&cc, out, sizeof(out));
	off = dect_nwk_build_hdr(rel, sizeof(rel), DECT_PD_CC, DECT_TI_RESPONDER,
				 1, DECT_CC_RELEASE_COM);
	dect_cc_rx(&cc, rel, off);
	CHECK_EQ(cc.state, DECT_CC_ST_RELEASED);
	CHECK_EQ(cc.stats.represented, 0u);
}

/*
 * §17.3.3: a second {CC-SETUP} from the handset while ITS first call is
 * active is rejected with "insufficient resources" and the first call goes
 * on. This is the same mechanism seen from the other side of CALL PRESENT.
 */
static void test_second_setup_during_an_active_call_is_rejected(void)
{
	struct dect_cc cc;
	uint8_t buf[64], out[64];
	size_t n;

	dect_cc_init(&cc);
	n = pp_setup(buf, sizeof(buf), 0);
	dect_cc_rx(&cc, buf, n);
	dect_cc_poll(&cc, out, sizeof(out));	/* CALL-PROC */
	dect_cc_poll(&cc, out, sizeof(out));	/* CONNECT */
	CHECK_EQ(cc.state, DECT_CC_ST_ACTIVE);

	n = pp_setup(buf, sizeof(buf), 3);	/* another call, TV 3 */
	dect_cc_rx(&cc, buf, n);
	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(n, 4u);
	CHECK_EQ(out[0], 0xB3u);			/* F=1, TV=3 */
	CHECK_EQ(out[1], DECT_CC_RELEASE_COM);
	CHECK_EQ(out[3], DECT_CC_REASON_INSUFFICIENT_RESOURCES);
	CHECK_EQ(cc.state, DECT_CC_ST_ACTIVE);
	CHECK_EQ(cc.ti_value, 0u);
	CHECK_EQ(cc.stats.collisions, 1u);

	/* A retransmitted SETUP of the live call itself is not a collision. */
	n = pp_setup(buf, sizeof(buf), 0);
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(dect_cc_poll(&cc, out, sizeof(out)), 0);
	CHECK_EQ(cc.stats.collisions, 1u);
}

/*
 * The identity gate: a {CC-SETUP} from a handset other than the subscribed
 * one is closed with {CC-RELEASE-COM} "unknown identity" (EN 300 444 table
 * 17) and no call starts. On 2026-10-24 the A540, rejected at location
 * registration, still ran its list-access call against a daemon serving the
 * C430A and took the C430A's Internal Names entry as its own.
 */
static void test_gate_turns_away_another_handset(void)
{
	static const uint8_t other[7] =
		{ 0x80, 0xB4, 0x00, 0x11, 0x22, 0x33, 0x45 };	/* last byte */
	struct dect_cc cc;
	uint8_t buf[64], out[64];
	size_t n;

	dect_cc_init(&cc);
	dect_cc_set_gate(&cc, other, sizeof(other));
	n = pp_setup_svc(buf, sizeof(buf), 2, 0x28);	/* LiA, TV 2 */
	dect_cc_rx(&cc, buf, n);

	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(n, 4u);
	CHECK_EQ(out[0], 0xA3u);			/* F=1, TV=2, PD CC */
	CHECK_EQ(out[1], DECT_CC_RELEASE_COM);
	CHECK_EQ(out[2], DECT_IE_RELEASE_REASON);
	CHECK_EQ(out[3], DECT_CC_REASON_UNKNOWN_IDENTITY);
	CHECK_EQ(dect_cc_poll(&cc, out, sizeof(out)), 0);
	/* No call, nothing learned from it. */
	CHECK_EQ(cc.state, DECT_CC_ST_NULL);
	CHECK_EQ(cc.stats.setup, 0u);
	CHECK_EQ(cc.stats.rejected_identity, 1u);
	CHECK_EQ(cc.peer_port_id_len, 0u);
}

static void test_gate_admits_the_subscribed_handset(void)
{
	static const uint8_t subscribed[7] =
		{ 0x80, 0xB4, 0x00, 0x11, 0x22, 0x33, 0x44 };
	struct dect_cc cc;
	uint8_t buf[64], out[64];
	size_t n;

	dect_cc_init(&cc);
	dect_cc_set_gate(&cc, subscribed, sizeof(subscribed));
	n = pp_setup(buf, sizeof(buf), 0);
	dect_cc_rx(&cc, buf, n);

	CHECK_EQ(cc.stats.setup, 1u);
	CHECK_EQ(cc.stats.rejected_identity, 0u);
	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_CC_CALL_PROC);
}

/* Armed with no identity: a base holding no handset recognises none. */
static void test_gate_without_an_identity_turns_away_every_setup(void)
{
	struct dect_cc cc;
	uint8_t buf[64], out[64];
	size_t n;

	dect_cc_init(&cc);
	dect_cc_set_gate(&cc, NULL, 0);
	n = pp_setup(buf, sizeof(buf), 0);
	dect_cc_rx(&cc, buf, n);

	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(n, 4u);
	CHECK_EQ(out[1], DECT_CC_RELEASE_COM);
	CHECK_EQ(out[3], DECT_CC_REASON_UNKNOWN_IDENTITY);
	CHECK_EQ(cc.state, DECT_CC_ST_NULL);
}

/* No <<PORTABLE-IDENTITY>> at all: a missing mandatory element, §17.6.1. */
static void test_gate_setup_without_an_identity(void)
{
	static const uint8_t subscribed[7] =
		{ 0x80, 0xB4, 0x00, 0x11, 0x22, 0x33, 0x44 };
	struct dect_cc cc;
	uint8_t buf[64], out[64];
	size_t n;

	dect_cc_init(&cc);
	dect_cc_set_gate(&cc, subscribed, sizeof(subscribed));
	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_CC,
			       DECT_TI_ORIGINATOR, 0, DECT_CC_SETUP);
	buf[n++] = DECT_IE_BASIC_SERVICE;
	buf[n++] = 0x00;
	dect_cc_rx(&cc, buf, n);

	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(n, 4u);
	CHECK_EQ(out[1], DECT_CC_RELEASE_COM);
	CHECK_EQ(out[3], DECT_CC_REASON_MANDATORY_IE_MISSING);
	CHECK_EQ(cc.state, DECT_CC_ST_NULL);
}

/*
 * TS 102 527-1 §7.3.3: a handset's SETUP that negotiates (a <<CODEC-LIST>>,
 * or the wideband basic service) gets the chosen codec, alone, at the latest
 * in our {CC-CONNECT}. We choose G.726, the codec our audio path carries,
 * from the handset's own entry. The A540's real list: G.722 long slot, then
 * G.726 full slot.
 */
static size_t pp_setup_codecs(uint8_t *out, size_t cap, uint8_t svc,
			      const uint8_t *cl, size_t cl_len)
{
	size_t n = pp_setup_svc(out, cap, 0, svc);

	return cl ? dect_nwk_put_ie(out, cap, n, DECT_IE_CODEC_LIST, cl, cl_len)
		  : n;
}

static size_t connect_of(struct dect_cc *cc, uint8_t *out, size_t cap)
{
	size_t n;

	while ((n = dect_cc_poll(cc, out, cap)) > 0)
		if (msg_type_of(out, n) == DECT_CC_CONNECT)
			return n;
	return 0;
}

static void test_connect_names_the_chosen_codec(void)
{
	static const uint8_t a540[] = { 0x90, 0x03, 0x00, 0x01,
					0x02, 0x00, 0x84 };
	static const uint8_t want[] = { 0x7C, 0x04, 0x90, 0x02, 0x00, 0x84 };
	struct dect_cc cc;
	uint8_t buf[64], out[64];
	size_t n;

	dect_cc_init(&cc);
	n = pp_setup_codecs(buf, sizeof(buf), 0x88, a540, sizeof(a540));
	dect_cc_rx(&cc, buf, n);
	n = connect_of(&cc, out, sizeof(out));
	CHECK_EQ(n, 2u + sizeof(want));
	CHECK(memcmp(&out[2], want, sizeof(want)) == 0);
	CHECK_EQ(cc.chosen_codec, 0x02u);

	/* G.726 first in the offer, not last: still one codec, flagged last */
	{
		static const uint8_t g726_first[] = { 0x90, 0x02, 0x00, 0x04,
						      0x03, 0x00, 0x81 };

		dect_cc_init(&cc);
		n = pp_setup_codecs(buf, sizeof(buf), 0x88, g726_first,
				    sizeof(g726_first));
		dect_cc_rx(&cc, buf, n);
		n = connect_of(&cc, out, sizeof(out));
		CHECK_EQ(n, 2u + sizeof(want));
		CHECK(memcmp(&out[2], want, sizeof(want)) == 0);
	}
}

/* Wideband service without a list: G.726 from the registration list. */
static void test_wideband_setup_without_a_list_gets_g726(void)
{
	static const uint8_t want[] = { 0x7C, 0x04, 0x90, 0x02, 0x00, 0x84 };
	struct dect_cc cc;
	uint8_t buf[64], out[64];
	size_t n;

	dect_cc_init(&cc);
	n = pp_setup_codecs(buf, sizeof(buf), 0x88, NULL, 0);
	dect_cc_rx(&cc, buf, n);
	n = connect_of(&cc, out, sizeof(out));
	CHECK_EQ(n, 2u + sizeof(want));
	CHECK(memcmp(&out[2], want, sizeof(want)) == 0);
}

/* Basic speech, no list: nothing to negotiate, the CONNECT stays bare. */
static void test_basic_speech_connect_names_no_codec(void)
{
	struct dect_cc cc;
	uint8_t buf[64], out[64];
	size_t n;

	dect_cc_init(&cc);
	n = pp_setup_codecs(buf, sizeof(buf), 0x80, NULL, 0);
	dect_cc_rx(&cc, buf, n);
	n = connect_of(&cc, out, sizeof(out));
	CHECK_EQ(n, 2u);
	CHECK_EQ(cc.chosen_codec, 0u);
}

/* An offer without G.726: we name nothing it did not offer, and count it. */
static void test_offer_without_g726_names_nothing(void)
{
	static const uint8_t g722_only[] = { 0x90, 0x03, 0x00, 0x81 };
	struct dect_cc cc;
	uint8_t buf[64], out[64];
	size_t n;

	dect_cc_init(&cc);
	n = pp_setup_codecs(buf, sizeof(buf), 0x88, g722_only,
			    sizeof(g722_only));
	dect_cc_rx(&cc, buf, n);
	n = connect_of(&cc, out, sizeof(out));
	CHECK_EQ(n, 2u);
	CHECK_EQ(cc.stats.codec_unmatched, 1u);
	CHECK_EQ(cc.chosen_codec, 0u);
}

/*
 * With wideband allowed, CONNECT names G.722 from the handset's own
 * entry, flagged last (bench tests 10-11: the coded path works on 07.56).
 */
static void test_wideband_link_chooses_g722(void)
{
	static const uint8_t a540[] = { 0x90, 0x03, 0x00, 0x01,
					0x02, 0x00, 0x84 };
	static const uint8_t g726_only[] = { 0x90, 0x02, 0x00, 0x84 };
	static const uint8_t want722[] = { 0x7C, 0x04, 0x90, 0x03, 0x00,
					   0x81 };
	static const uint8_t want726[] = { 0x7C, 0x04, 0x90, 0x02, 0x00,
					   0x84 };
	struct dect_cc cc;
	uint8_t buf[64], out[64];
	size_t n;

	dect_cc_init(&cc);
	dect_cc_set_wideband(&cc, 1);
	n = pp_setup_codecs(buf, sizeof(buf), 0x88, a540, sizeof(a540));
	dect_cc_rx(&cc, buf, n);
	n = connect_of(&cc, out, sizeof(out));
	CHECK_EQ(n, 2u + sizeof(want722));
	CHECK(memcmp(&out[2], want722, sizeof(want722)) == 0);
	CHECK_EQ(dect_cc_codec(&cc), DECT_CC_CODEC_G722);

	/* wideband service, no list: G.722 long slot from registration */
	dect_cc_init(&cc);
	dect_cc_set_wideband(&cc, 1);
	n = pp_setup_codecs(buf, sizeof(buf), 0x88, NULL, 0);
	dect_cc_rx(&cc, buf, n);
	n = connect_of(&cc, out, sizeof(out));
	CHECK_EQ(n, 2u + sizeof(want722));
	CHECK(memcmp(&out[2], want722, sizeof(want722)) == 0);

	/* a handset that offers only G.726 gets G.726 */
	dect_cc_init(&cc);
	dect_cc_set_wideband(&cc, 1);
	n = pp_setup_codecs(buf, sizeof(buf), 0x88, g726_only,
			    sizeof(g726_only));
	dect_cc_rx(&cc, buf, n);
	n = connect_of(&cc, out, sizeof(out));
	CHECK_EQ(n, 2u + sizeof(want726));
	CHECK(memcmp(&out[2], want726, sizeof(want726)) == 0);
	CHECK_EQ(dect_cc_codec(&cc), DECT_CC_CODEC_G726);
}

/* A {CC-INFO} from the handset on its call (transaction 0). */
static size_t pp_info(uint8_t *out, size_t cap, const char *digits,
		      int complete)
{
	size_t n = dect_nwk_build_hdr(out, cap, DECT_PD_CC,
				      DECT_TI_ORIGINATOR, 0, DECT_CC_INFO);

	if (complete)
		out[n++] = DECT_IE_SENDING_COMPLETE;
	if (digits)
		n = dect_nwk_put_ie(out, cap, n, DECT_IE_MULTI_KEYPAD,
				    (const uint8_t *)digits, strlen(digits));
	return n;
}

static uint8_t types_of(struct dect_cc *cc, uint8_t *types, size_t cap)
{
	uint8_t out[64];
	size_t n, k = 0;

	while ((n = dect_cc_poll(cc, out, sizeof(out))) > 0 && k < cap)
		types[k++] = msg_type_of(out, n);
	return (uint8_t)k;
}

/*
 * Overlap receiving (GAP figure 1): SETUP-ACK, digits in CC-INFO, and the
 * owner accepts when dialling is complete.
 */
static void test_overlap_receiving(void)
{
	struct dect_cc cc;
	uint8_t buf[64], t[8];
	const uint8_t *k;
	size_t n, klen;

	dect_cc_init(&cc);
	dect_cc_set_overlap(&cc, 1);
	n = pp_setup_svc(buf, sizeof(buf), 0, 0x88);
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(types_of(&cc, t, sizeof(t)), 1u);
	CHECK_EQ(t[0], DECT_CC_SETUP_ACK);
	CHECK_EQ(cc.state, DECT_CC_ST_OVERLAP);
	CHECK(dect_cc_busy(&cc));

	n = pp_info(buf, sizeof(buf), "123", 0);
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(types_of(&cc, t, sizeof(t)), 0u);	/* nothing sent */
	k = dect_cc_keypad(&cc, &klen);
	CHECK(klen == 3 && k && !memcmp(k, "123", 3));
	CHECK_EQ(cc.state, DECT_CC_ST_OVERLAP);

	CHECK_EQ(dect_cc_accept(&cc), 0);
	CHECK_EQ(types_of(&cc, t, sizeof(t)), 2u);
	CHECK_EQ(t[0], DECT_CC_CALL_PROC);
	CHECK_EQ(t[1], DECT_CC_CONNECT);
	CHECK_EQ(cc.state, DECT_CC_ST_ACTIVE);
	CHECK_EQ(dect_cc_accept(&cc), -1);		/* only once */

	/* <<SENDING-COMPLETE>> in a CC-INFO moves it on by itself */
	dect_cc_init(&cc);
	dect_cc_set_overlap(&cc, 1);
	n = pp_setup_svc(buf, sizeof(buf), 0, 0x88);
	dect_cc_rx(&cc, buf, n);
	(void)types_of(&cc, t, sizeof(t));
	n = pp_info(buf, sizeof(buf), "42", 1);
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(types_of(&cc, t, sizeof(t)), 2u);
	CHECK_EQ(t[0], DECT_CC_CALL_PROC);
	CHECK_EQ(cc.state, DECT_CC_ST_ACTIVE);

	/* ... and in the SETUP: no SETUP-ACK at all */
	dect_cc_init(&cc);
	dect_cc_set_overlap(&cc, 1);
	n = pp_setup_svc(buf, sizeof(buf), 0, 0x88);
	buf[n++] = DECT_IE_SENDING_COMPLETE;
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(types_of(&cc, t, sizeof(t)), 2u);
	CHECK_EQ(t[0], DECT_CC_CALL_PROC);
	CHECK_EQ(t[1], DECT_CC_CONNECT);

	/* list access is not dialled: CALL-PROC as before */
	dect_cc_init(&cc);
	dect_cc_set_overlap(&cc, 1);
	n = pp_setup_svc(buf, sizeof(buf), 0, 0x28);
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(types_of(&cc, t, sizeof(t)), 1u);
	CHECK_EQ(t[0], DECT_CC_CALL_PROC);
}

/*
 * The steps for an owner that routes the call: CALL-PROC, ALERTING,
 * CONNECT, each once; ACTIVE on CONNECT (§9.3.1.8, no CONNECT-ACK); the
 * codec known from the SETUP, so the U-plane can go up before CONNECT.
 */
static void test_handset_call_in_steps(void)
{
	static const uint8_t a540[] = { 0x90, 0x03, 0x00, 0x01,
					0x02, 0x00, 0x84 };
	struct dect_cc cc;
	uint8_t buf[64], t[8], out[64];
	size_t n;

	dect_cc_init(&cc);
	dect_cc_set_overlap(&cc, 1);
	dect_cc_set_wideband(&cc, 1);
	n = pp_setup_codecs(buf, sizeof(buf), 0x88, a540, sizeof(a540));
	dect_cc_rx(&cc, buf, n);
	(void)types_of(&cc, t, sizeof(t));			/* SETUP-ACK */
	CHECK_EQ(dect_cc_codec(&cc), DECT_CC_CODEC_G722);	/* already */

	CHECK_EQ(dect_cc_proceed(&cc), 0);
	CHECK_EQ(types_of(&cc, t, sizeof(t)), 1u);
	CHECK_EQ(t[0], DECT_CC_CALL_PROC);
	CHECK_EQ(cc.state, DECT_CC_ST_PROCEEDING);
	CHECK_EQ(dect_cc_next_deadline(&cc), UINT64_MAX);	/* F<CC.01> off */

	CHECK_EQ(dect_cc_alert(&cc), 0);
	CHECK_EQ(types_of(&cc, t, sizeof(t)), 1u);		/* no 2nd PROC */
	CHECK_EQ(t[0], DECT_CC_ALERTING);
	CHECK_EQ(dect_cc_alert(&cc), -1);

	CHECK_EQ(dect_cc_connect(&cc), 0);
	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_CC_CONNECT);
	CHECK_EQ(out[5], DECT_CC_CODEC_G722);	/* 7c 04 90 [03] 00 81 */
	CHECK_EQ(dect_cc_poll(&cc, out, sizeof(out)), 0u);
	CHECK_EQ(cc.state, DECT_CC_ST_ACTIVE);
	CHECK_EQ(cc.stats.connected, 1u);
	CHECK_EQ(dect_cc_connect(&cc), -1);

	/* list access is never connected */
	dect_cc_init(&cc);
	dect_cc_set_overlap(&cc, 1);
	n = pp_setup_svc(buf, sizeof(buf), 0, 0x28);
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(cc.state, DECT_CC_ST_PROCEEDING);
	CHECK_EQ(dect_cc_connect(&cc), -1);
	CHECK_EQ(dect_cc_alert(&cc), -1);
}

/* the trailing <<CALL-INFORMATION>> of a message: call id 1, status st */
static int ends_with_status(const uint8_t *m, size_t n, uint8_t st)
{
	const uint8_t want[] = { 0x7E, 0x06, 0x01, 0x00, 0x81,
				 0x02, 0x01, (uint8_t)(0x80u | st) };

	return n >= sizeof(want) &&
	       memcmp(m + n - sizeof(want), want, sizeof(want)) == 0;
}

/*
 * TS 102 527-3 §7.4.6.4.3: "CS call connect" shall always be sent in our
 * {CC-CONNECT}; stock also marks SETUP-ACK, CALL-PROC and ALERTING
 * (clean room A-0122). The CONNECT is exactly A-0122's test element after
 * the codec list.
 */
static void test_call_status_in_our_answers(void)
{
	static const uint8_t a540[] = { 0x90, 0x03, 0x00, 0x01,
					0x02, 0x00, 0x84 };
	static const uint8_t connect[] = { 0x7C, 0x04, 0x90, 0x03, 0x00, 0x81,
					   0x7E, 0x06, 0x01, 0x00, 0x81,
					   0x02, 0x01, 0x85 };
	struct dect_cc cc;
	uint8_t buf[64], out[64];
	size_t n;

	dect_cc_init(&cc);
	dect_cc_set_overlap(&cc, 1);
	dect_cc_set_wideband(&cc, 1);
	dect_cc_set_call_status(&cc, 1, 1);
	n = pp_setup_codecs(buf, sizeof(buf), 0x88, a540, sizeof(a540));
	dect_cc_rx(&cc, buf, n);
	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_CC_SETUP_ACK);
	CHECK(ends_with_status(out, n, DECT_CS_CALL_SETUP_ACK));
	CHECK_EQ(dect_cc_alert(&cc), 0);
	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_CC_CALL_PROC);
	CHECK(ends_with_status(out, n, DECT_CS_CALL_PROC));
	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_CC_ALERTING);
	CHECK(ends_with_status(out, n, DECT_CS_CALL_ALERTING));
	CHECK_EQ(dect_cc_connect(&cc), 0);
	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_CC_CONNECT);
	CHECK_EQ(n, 2u + sizeof(connect));
	CHECK(memcmp(&out[2], connect, sizeof(connect)) == 0);

	/* basic speech, nothing negotiated: CONNECT carries only the status */
	dect_cc_init(&cc);
	dect_cc_set_call_status(&cc, 1, 1);
	n = pp_setup_codecs(buf, sizeof(buf), 0x80, NULL, 0);
	dect_cc_rx(&cc, buf, n);
	(void)dect_cc_poll(&cc, out, sizeof(out));		/* CALL-PROC */
	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_CC_CONNECT);
	CHECK_EQ(n, 2u + 8u);
	CHECK(ends_with_status(out, n, DECT_CS_CALL_CONNECT));
}

/* An internal call (class 9) is never connected by CC itself. */
static void test_internal_call_waits_for_the_owner(void)
{
	struct dect_cc cc;
	uint8_t buf[64], t[8];
	size_t n;

	dect_cc_init(&cc);
	dect_cc_set_overlap(&cc, 1);
	n = pp_setup_svc(buf, sizeof(buf), 0, 0x98);
	buf[n++] = DECT_IE_SENDING_COMPLETE;
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(types_of(&cc, t, sizeof(t)), 1u);
	CHECK_EQ(t[0], DECT_CC_CALL_PROC);		/* no CONNECT */
	CHECK_EQ(cc.state, DECT_CC_ST_PROCEEDING);

	dect_cc_init(&cc);
	dect_cc_set_overlap(&cc, 1);
	n = pp_setup_svc(buf, sizeof(buf), 0, 0x98);
	dect_cc_rx(&cc, buf, n);
	(void)types_of(&cc, t, sizeof(t));		/* SETUP-ACK */
	n = pp_info(buf, sizeof(buf), "2", 1);
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(types_of(&cc, t, sizeof(t)), 1u);
	CHECK_EQ(t[0], DECT_CC_CALL_PROC);
	CHECK_EQ(cc.state, DECT_CC_ST_PROCEEDING);
	CHECK_EQ(dect_cc_alert(&cc), 0);		/* the owner's steps */
	CHECK_EQ(dect_cc_connect(&cc), 0);
	CHECK_EQ(cc.state, DECT_CC_ST_ACTIVE);

	/* without overlap receiving too (review, finding 12) */
	dect_cc_init(&cc);
	n = pp_setup_svc(buf, sizeof(buf), 0, 0x98);
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(types_of(&cc, t, sizeof(t)), 1u);
	CHECK_EQ(t[0], DECT_CC_CALL_PROC);
	CHECK_EQ(cc.state, DECT_CC_ST_PROCEEDING);
}

/*
 * The internal-call key 0x17 makes a call internal (EN 300 444 §8.19): on
 * a list-access call, as the A540 sends it after reading the Internal
 * Names List (TS 102 527-3 figure C.21, bench test 19), and on a normal
 * call in overlap sending.
 */
static void test_int_key_makes_an_internal_call(void)
{
	struct dect_cc cc;
	uint8_t buf[64], t[8];
	size_t n;

	/* list access (0x28), then 17H + "2" in one CC-INFO */
	dect_cc_init(&cc);
	dect_cc_set_overlap(&cc, 1);
	n = pp_setup_svc(buf, sizeof(buf), 0, 0x28);
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(types_of(&cc, t, sizeof(t)), 1u);
	CHECK_EQ(t[0], DECT_CC_CALL_PROC);
	n = pp_info(buf, sizeof(buf), "\x17" "2", 0);
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(cc.peer_basic_service, 0x98);	/* wideband kept */
	CHECK_EQ(cc.state, DECT_CC_ST_PROCEEDING);
	CHECK_EQ(types_of(&cc, t, sizeof(t)), 0u);	/* the owner routes it */
	CHECK_EQ(dect_cc_alert(&cc), 0);
	CHECK_EQ(types_of(&cc, t, sizeof(t)), 1u);	/* no second CALL-PROC */
	CHECK_EQ(t[0], DECT_CC_ALERTING);
	CHECK_EQ(dect_cc_connect(&cc), 0);
	CHECK_EQ(cc.state, DECT_CC_ST_ACTIVE);

	/* normal call, overlap: 17H, then the number with SENDING-COMPLETE */
	dect_cc_init(&cc);
	dect_cc_set_overlap(&cc, 1);
	n = pp_setup_svc(buf, sizeof(buf), 0, 0x88);
	dect_cc_rx(&cc, buf, n);
	(void)types_of(&cc, t, sizeof(t));		/* SETUP-ACK */
	n = pp_info(buf, sizeof(buf), "\x17", 0);
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(cc.peer_basic_service, 0x98);
	n = pp_info(buf, sizeof(buf), "1", 1);
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(types_of(&cc, t, sizeof(t)), 1u);
	CHECK_EQ(t[0], DECT_CC_CALL_PROC);		/* not CONNECT */
	CHECK_EQ(cc.state, DECT_CC_ST_PROCEEDING);

	/* plain digits leave a list-access call alone */
	dect_cc_init(&cc);
	n = pp_setup_svc(buf, sizeof(buf), 0, 0x28);
	dect_cc_rx(&cc, buf, n);
	(void)types_of(&cc, t, sizeof(t));
	n = pp_info(buf, sizeof(buf), "2", 0);
	dect_cc_rx(&cc, buf, n);
	CHECK_EQ(cc.peer_basic_service, 0x28);
}

/* F<CC.01>: 20 s, restarted by every CC-INFO, then "timer expiry". */
static void test_overlap_timer(void)
{
	struct dect_cc cc;
	uint8_t buf[64], out[64];
	size_t n;

	dect_cc_init(&cc);
	dect_cc_set_overlap(&cc, 1);
	n = pp_setup_svc(buf, sizeof(buf), 0, 0x88);
	dect_cc_rx(&cc, buf, n);
	while (dect_cc_poll(&cc, out, sizeof(out)) > 0)
		;
	dect_cc_tick(&cc, 1000);			/* armed: 21000 */
	CHECK_EQ(dect_cc_next_deadline(&cc), 21000u);
	n = pp_info(buf, sizeof(buf), "1", 0);
	dect_cc_rx(&cc, buf, n);
	dect_cc_tick(&cc, 11000);			/* re-armed: 31000 */
	dect_cc_tick(&cc, 30999);
	CHECK_EQ(cc.state, DECT_CC_ST_OVERLAP);
	CHECK_EQ(dect_cc_poll(&cc, out, sizeof(out)), 0u);
	dect_cc_tick(&cc, 31000);
	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_CC_RELEASE);	/* GAP 8.3.2.3 */
	CHECK(n >= 4u && out[n - 2] == DECT_IE_RELEASE_REASON &&
	      out[n - 1] == 0x0Du);			/* timer expiry */
	CHECK_EQ(cc.state, DECT_CC_ST_RELEASED);	/* release pending */
	CHECK_EQ(cc.stats.overlap_timeouts, 1u);
}

/* A call we place: the handset names the codec in ALERTING or CONNECT. */
static void test_handset_choice_is_learned_on_our_call(void)
{
	static const uint8_t port_id[] = { 0x80, 0xa8, 0x02, 0x11, 0x22, 0x33 };
	static const uint8_t fixed_id[] = { 0xa0, 0x11, 0x22, 0x33, 0x44 };
	static const uint8_t g722[] = { 0x90, 0x03, 0x00, 0x81 };
	struct dect_cc cc;
	uint8_t msg[64], out[64];
	size_t n;

	dect_cc_init(&cc);
	CHECK_EQ(dect_cc_setup(&cc, port_id, sizeof(port_id),
			       fixed_id, sizeof(fixed_id), 0x88), 0);
	while (dect_cc_poll(&cc, out, sizeof(out)) > 0)
		;
	n = dect_nwk_build_hdr(msg, sizeof(msg), DECT_PD_CC, DECT_TI_RESPONDER,
			       0, DECT_CC_ALERTING);
	n = dect_nwk_put_ie(msg, sizeof(msg), n, DECT_IE_CODEC_LIST, g722,
			    sizeof(g722));
	dect_cc_rx(&cc, msg, n);
	CHECK_EQ(cc.state, DECT_CC_ST_RINGING);
	CHECK_EQ(dect_cc_codec(&cc), DECT_CC_CODEC_G722);

	/* a fresh call forgets it; no list anywhere means basic speech */
	dect_cc_init(&cc);
	CHECK_EQ(dect_cc_setup(&cc, port_id, sizeof(port_id),
			       fixed_id, sizeof(fixed_id), 0x80), 0);
	while (dect_cc_poll(&cc, out, sizeof(out)) > 0)
		;
	n = dect_nwk_build_hdr(msg, sizeof(msg), DECT_PD_CC, DECT_TI_RESPONDER,
			       0, DECT_CC_ALERTING);
	dect_cc_rx(&cc, msg, n);
	n = dect_nwk_build_hdr(msg, sizeof(msg), DECT_PD_CC, DECT_TI_RESPONDER,
			       0, DECT_CC_CONNECT);
	dect_cc_rx(&cc, msg, n);
	CHECK_EQ(cc.state, DECT_CC_ST_ACTIVE);
	CHECK_EQ(dect_cc_codec(&cc), 0u);
}

/*
 * A busy link takes list access and refuses speech: "insufficient
 * resources", as §17.3.3 does for a SETUP while another call is up.
 */
static void test_busy_link_refuses_speech_and_takes_list_access(void)
{
	struct dect_cc cc;
	uint8_t buf[64], out[64];
	size_t n;

	dect_cc_init(&cc);
	dect_cc_set_busy(&cc, 1);
	n = pp_setup(buf, sizeof(buf), 0);		/* basic speech */
	dect_cc_rx(&cc, buf, n);
	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(n, 4u);
	CHECK_EQ(out[0], 0x83u);			/* F=1, TV=0, CC */
	CHECK_EQ(out[1], DECT_CC_RELEASE_COM);
	CHECK_EQ(out[3], DECT_CC_REASON_INSUFFICIENT_RESOURCES);
	CHECK_EQ(cc.state, DECT_CC_ST_NULL);
	CHECK_EQ(cc.stats.rejected_busy, 1u);
	CHECK_EQ(dect_cc_poll(&cc, out, sizeof(out)), 0);

	dect_cc_init(&cc);
	dect_cc_set_busy(&cc, 1);
	n = pp_setup_svc(buf, sizeof(buf), 1, 0x28);	/* list access */
	dect_cc_rx(&cc, buf, n);
	n = dect_cc_poll(&cc, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_CC_CALL_PROC);
	CHECK_EQ(cc.stats.rejected_busy, 0u);
}

int main(void)
{
	test_setup_collision_releases_the_handsets_call();
	test_insufficient_resources_represents_the_setup();
	test_other_release_reasons_are_not_represented();
	test_second_setup_during_an_active_call_is_rejected();
	test_setup_carries_line_identifier();
	test_foreign_transaction_does_not_touch_our_call();
	test_unhandled_message_on_our_call_is_ignored();
	test_transaction_value_is_lowest_free();
	test_transaction_value_rotates();
	test_busy_spans_the_whole_call();
	test_learns_identities_from_setup();
	test_outgoing_call_to_handset();
	test_alerting_ignored_when_responder();
	test_incoming_setup();
	test_release_from_pp();
	test_release_from_fp();
	test_answers_the_handsets_iwu_session_open();
	test_list_read_queues_confirm_and_data_packet();
	test_list_access_call_gets_call_proc_only();
	test_iwu_does_not_change_call_state();
	test_iwu_info_without_the_ie_is_ignored();
	test_ignores_non_cc();
	test_closes_a_facility_transaction();
	test_facility_does_not_disturb_a_live_call();
	test_peer_identity_gate();
	test_gate_turns_away_another_handset();
	test_gate_admits_the_subscribed_handset();
	test_gate_without_an_identity_turns_away_every_setup();
	test_gate_setup_without_an_identity();
	test_connect_names_the_chosen_codec();
	test_wideband_link_chooses_g722();
	test_handset_choice_is_learned_on_our_call();
	test_overlap_receiving();
	test_overlap_timer();
	test_handset_call_in_steps();
	test_call_status_in_our_answers();
	test_internal_call_waits_for_the_owner();
	test_int_key_makes_an_internal_call();
	test_second_call_on_the_same_link();
	test_info_on_the_call();
	test_wideband_setup_without_a_list_gets_g726();
	test_basic_speech_connect_names_no_codec();
	test_offer_without_g726_names_nothing();
	test_busy_link_refuses_speech_and_takes_list_access();
	test_peer_basic_service_decides_speech();
	test_setup_without_basic_service_is_not_speech();
	test_basic_service_is_not_found_inside_a_payload();
	test_collects_the_dialled_digits_from_a_real_capture();
	test_digits_accumulate_across_messages();
	test_keypad_overflow_is_bounded_and_flagged();
	test_keypad_in_the_setup_is_collected();
	test_keypad_outside_a_call_is_ignored();
	test_setup_ie_order_is_ascending();
	test_setup_timer_releases_an_unanswered_call();
	test_setup_timer_stops_when_the_handset_answers();
	test_setup_timer_does_not_run_for_a_pp_originated_call();
	return CHECK_DONE();
}

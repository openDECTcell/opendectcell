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

/*
 * Feed one inbound NWK message through the link the way the chip delivers
 * it: a LAPC I-frame (NLF set, N(S) 0) in Lc framing with LSIG 0, cut into
 * 15-byte 0x0203 segments for handle 1. *ind is the indication of the
 * segment that completed the message.
 */
static void feed_nwk(struct dect_link *l, const uint8_t *nwk, size_t n,
		     struct dect_link_ind *ind)
{
	uint8_t frame[DECT_LC_LAPC_MAX], cs[DECT_LC_FRAME_MAX];
	struct dect_link_ind cur;
	struct dect_mail m;
	size_t fl, cl, off;

	fl = dect_lapc_build_i(frame, sizeof(frame), 0, 1, DECT_LAPC_LLN_A, 0, 0,
			       0, 0, nwk, n);
	cl = dect_lc_build(frame, fl, 0, cs, sizeof(cs));
	CHECK(fl > 0 && cl > 0);
	memset(ind, 0, sizeof(*ind));
	for (off = 0; off + 5u <= cl; off += 5u) {
		memset(&m, 0, sizeof(m));
		m.prim = 0x0203u;
		m.param_len = 15;
		m.param[0] = 1;
		m.param[1] = 0x01;
		memcpy(&m.param[2], cs + off, 5);
		m.param[12] = 1;
		dect_link_rx_segment(l, &m, 0, NULL, NULL, &cur);
		if (cur.nwk_len)
			*ind = cur;
	}
}

/*
 * Only {LCE-PAGE-RESPONSE} (0x71) with its mandatory <<PORTABLE-IDENTITY>>
 * is a page response, and the identity is handed up so the caller can check
 * who answered (TS 102 527-1 §7.3.8.2.1). Any LCE message used to count.
 */
static void test_page_response_carries_the_identity(void)
{
	static const uint8_t ipui[] = { 0x80, 0xa8, 0x02, 0x11, 0x22, 0x33, 0x44 };
	struct dect_link l;
	struct dect_link_ind ind;
	uint8_t msg[32];
	size_t n;

	init(&l);
	n = dect_nwk_build_hdr(msg, sizeof(msg), DECT_PD_LCE, DECT_TI_ORIGINATOR,
			       0, DECT_LCE_PAGE_RESPONSE);
	n = dect_nwk_put_ie(msg, sizeof(msg), n, DECT_IE_PORTABLE_IDENTITY,
			    ipui, sizeof(ipui));
	feed_nwk(&l, msg, n, &ind);
	CHECK(ind.nwk_len > 0);
	CHECK_EQ(ind.page_response, 1u);
	CHECK_EQ(ind.page_id_len, sizeof(ipui));
	CHECK(memcmp(ind.page_id, ipui, sizeof(ipui)) == 0);

	/* Another LCE message type is not a page response. */
	init(&l);
	n = dect_nwk_build_hdr(msg, sizeof(msg), DECT_PD_LCE, DECT_TI_ORIGINATOR,
			       0, 0x72u);
	n = dect_nwk_put_ie(msg, sizeof(msg), n, DECT_IE_PORTABLE_IDENTITY,
			    ipui, sizeof(ipui));
	feed_nwk(&l, msg, n, &ind);
	CHECK(ind.nwk_len > 0);
	CHECK_EQ(ind.page_response, 0u);

	/* Nor is a page response without its mandatory identity. */
	init(&l);
	n = dect_nwk_build_hdr(msg, sizeof(msg), DECT_PD_LCE, DECT_TI_ORIGINATOR,
			       0, DECT_LCE_PAGE_RESPONSE);
	feed_nwk(&l, msg, n, &ind);
	CHECK(ind.nwk_len > 0);
	CHECK_EQ(ind.page_response, 0u);
}

/*
 * The link arms the CC identity gate from MM, so the handset whose
 * {LOCATE-REQUEST} MM would reject cannot place a call either, and a link
 * that holds no identity takes no call at all.
 */
static void test_cc_admits_only_the_handset_mm_holds(void)
{
	static const uint8_t ours[] = { 0x80, 0xa8, 0x03, 0x11, 0x22, 0x33, 0x44 };
	static const uint8_t other[] = { 0x80, 0xa8, 0x02, 0x55, 0x66, 0x77, 0x88 };
	struct dect_link l;
	struct dect_link_ind ind;
	uint8_t msg[32];
	size_t n, k;
	struct {
		const uint8_t *armed;	/* mm.port_id, NULL = none */
		const uint8_t *held;	/* dect_mm_hold(), NULL = none */
		const uint8_t *caller;
		uint32_t setup, rejected;
		int adopted;
	} c[] = {
		{ ours, NULL,  ours,  1, 0, -1 },
		{ ours, NULL,  other, 0, 1, -1 },
		{ NULL, NULL,  ours,  0, 1, -1 },
		/* a held handset calls on a fresh link: adopted, admitted */
		{ ours, other, other, 1, 0,  5 },
		{ NULL, other, other, 1, 0,  5 },
	};

	for (k = 0; k < sizeof(c) / sizeof(c[0]); k++) {
		init(&l);
		if (c[k].armed) {
			memcpy(l.mm.port_id, c[k].armed, sizeof(ours));
			l.mm.port_id_len = sizeof(ours);
		}
		if (c[k].held)
			CHECK_EQ(dect_mm_hold(&l.mm, c[k].held, sizeof(ours),
					      3, NULL, 5), 0);
		n = dect_nwk_build_hdr(msg, sizeof(msg), DECT_PD_CC,
				       DECT_TI_ORIGINATOR, 0, DECT_CC_SETUP);
		n = dect_nwk_put_ie(msg, sizeof(msg), n,
				    DECT_IE_PORTABLE_IDENTITY, c[k].caller,
				    sizeof(ours));
		msg[n++] = DECT_IE_BASIC_SERVICE;	/* mandatory */
		msg[n++] = 0x80;
		feed_nwk(&l, msg, n, &ind);
		CHECK_EQ(ind.cc_message, 1u);
		CHECK_EQ(l.cc.stats.setup, c[k].setup);
		CHECK_EQ(l.cc.stats.rejected_identity, c[k].rejected);
		CHECK_EQ(l.mm.adopted, c[k].adopted);
	}
}

static uint8_t tx_seen_buf[64];
static size_t tx_seen_len;

static void tx_seen(void *ctx, const uint8_t *msg, size_t len)
{
	(void)ctx;
	if (!tx_seen_len && len <= sizeof(tx_seen_buf)) {
		memcpy(tx_seen_buf, msg, len);
		tx_seen_len = len;
	}
}

/*
 * A page response from a handset we did not page gets {LCE-PAGE-REJECT}:
 * LCE, the PT's transaction value with our flag, type 0x72, and its full
 * IPUI (TS 102 527-1 §7.3.8.3.1, table 20). It goes out before anything
 * MM or CC has queued.
 */
/*
 * EN 300 175-5 §17.6.5: a page response without its mandatory identity
 * is cleared with {LCE-PAGE-REJECT}, when the link knows whom to name.
 */
static void test_page_response_without_identity_is_rejected(void)
{
	static const uint8_t ipui[] = { 0x80, 0xa8, 0x02, 0x11, 0x22, 0x33, 0x44 };
	struct dect_link l;
	struct dect_link_ind ind;
	uint8_t msg[32];
	size_t n;

	init(&l);					/* IPUI unknown */
	n = dect_nwk_build_hdr(msg, sizeof(msg), DECT_PD_LCE, DECT_TI_ORIGINATOR,
			       2, DECT_LCE_PAGE_RESPONSE);
	feed_nwk(&l, msg, n, &ind);
	CHECK_EQ(ind.page_response, 0u);
	CHECK_EQ(l.lce_len, 0u);			/* nothing to name */

	init(&l);
	memcpy(l.mm.port_id, ipui, sizeof(ipui));
	l.mm.port_id_len = sizeof(ipui);
	tx_seen_len = 0;
	dect_link_set_tx_log(&l, tx_seen, NULL);
	feed_nwk(&l, msg, n, &ind);
	CHECK_EQ(ind.page_response, 0u);
	CHECK_EQ(tx_seen_len, 2u + 2u + sizeof(ipui));	/* sent at once */
	CHECK_EQ(tx_seen_buf[0], 0xA0u);		/* F=1, TV=2, PD LCE */
	CHECK_EQ(tx_seen_buf[1], DECT_LCE_PAGE_REJECT);
	CHECK(memcmp(&tx_seen_buf[4], ipui, sizeof(ipui)) == 0);
}

static void test_page_reject_is_built_from_the_response(void)
{
	static const uint8_t ipui[] = { 0x80, 0xa8, 0x02, 0x11, 0x22, 0x33, 0x44 };
	struct dect_link l;
	struct dect_link_ind ind;
	uint8_t msg[32], sent[64];
	size_t n;

	init(&l);
	n = dect_nwk_build_hdr(msg, sizeof(msg), DECT_PD_LCE, DECT_TI_ORIGINATOR,
			       3, DECT_LCE_PAGE_RESPONSE);
	n = dect_nwk_put_ie(msg, sizeof(msg), n, DECT_IE_PORTABLE_IDENTITY,
			    ipui, sizeof(ipui));
	feed_nwk(&l, msg, n, &ind);
	CHECK_EQ(ind.page_response, 1u);
	CHECK_EQ(ind.page_ti, 3u);

	CHECK_EQ(dect_link_page_reject(&l, ind.page_id, ind.page_id_len,
				       ind.page_ti), 0);
	CHECK_EQ(dect_link_page_reject(&l, ind.page_id, ind.page_id_len,
				       ind.page_ti), -1);	/* one deep */
	CHECK_EQ(l.lce_len, 2u + 2u + sizeof(ipui));
	memcpy(sent, l.lce, l.lce_len);
	CHECK_EQ(sent[0], 0xB0u);		/* F=1, TV=3, PD LCE */
	CHECK_EQ(sent[1], DECT_LCE_PAGE_REJECT);
	CHECK_EQ(sent[2], DECT_IE_PORTABLE_IDENTITY);
	CHECK_EQ(sent[3], sizeof(ipui));
	CHECK(memcmp(&sent[4], ipui, sizeof(ipui)) == 0);
	CHECK_EQ(dect_link_page_reject(&l, NULL, 0, 0), -1);

	/* and it leaves through the link like any NWK message */
	tx_seen_len = 0;
	dect_link_set_tx_log(&l, tx_seen, NULL);
	dect_link_drain(&l, 0);
	CHECK_EQ(tx_seen_len, 2u + 2u + sizeof(ipui));
	CHECK_EQ(tx_seen_buf[1], DECT_LCE_PAGE_REJECT);
	CHECK_EQ(l.lce_len, 0u);
}

/*
 * A roamer in the same location area calls without locating: with
 * defer_unknown its {CC-SETUP} is held while the record is fetched, then
 * served on resume -- or turned away if the record never came.
 */
static void test_unknown_setup_is_held_for_a_fetch(void)
{
	static const uint8_t roamer[] = { 0x80, 0xa8, 0x02, 0x55, 0x66, 0x77, 0x88 };
	struct dect_mm_cfg cfg;
	struct dect_link l;
	struct dect_link_ind ind;
	uint8_t msg[32];
	size_t n;
	int pass;

	for (pass = 0; pass < 2; pass++) {
		memset(&cfg, 0, sizeof(cfg));
		cfg.defer_unknown = 1;
		dect_link_init(&l, 1, &cfg);
		n = dect_nwk_build_hdr(msg, sizeof(msg), DECT_PD_CC,
				       DECT_TI_ORIGINATOR, 0, DECT_CC_SETUP);
		n = dect_nwk_put_ie(msg, sizeof(msg), n,
				    DECT_IE_PORTABLE_IDENTITY, roamer,
				    sizeof(roamer));
		msg[n++] = DECT_IE_BASIC_SERVICE;	/* mandatory */
		msg[n++] = 0x80;
		feed_nwk(&l, msg, n, &ind);
		CHECK_EQ(ind.cc_message, 1u);
		CHECK_EQ(l.cc.stats.setup, 0u);			/* held */
		CHECK_EQ(l.cc.stats.rejected_identity, 0u);
		CHECK_EQ(dect_link_setup_pending(&l, NULL, NULL), 1);

		if (pass == 0) {	/* the record arrives */
			CHECK_EQ(dect_mm_hold(&l.mm, roamer, sizeof(roamer),
					      4, NULL, 6), 0);
			CHECK_EQ(dect_link_setup_resume(&l, 0), 1);
			CHECK_EQ(l.cc.stats.setup, 1u);
			CHECK_EQ(l.mm.adopted, 6);
		} else {		/* it never does */
			CHECK_EQ(dect_link_setup_resume(&l, 0), -1);
			CHECK_EQ(l.cc.stats.setup, 0u);
			CHECK_EQ(l.cc.stats.rejected_identity, 1u);
		}
		CHECK_EQ(dect_link_setup_pending(&l, NULL, NULL), 0);
		CHECK_EQ(dect_link_setup_resume(&l, 0), 0);
	}
}

static size_t roamer_setup(uint8_t *msg, size_t cap, const uint8_t *id,
			   size_t id_len, uint8_t svc)
{
	size_t n = dect_nwk_build_hdr(msg, cap, DECT_PD_CC, DECT_TI_ORIGINATOR,
				      0, DECT_CC_SETUP);

	n = dect_nwk_put_ie(msg, cap, n, DECT_IE_PORTABLE_IDENTITY, id, id_len);
	msg[n++] = DECT_IE_BASIC_SERVICE;
	msg[n++] = svc;
	return n;
}

/*
 * A held SETUP is answered at once -- SETUP-ACK for a call whose digits
 * CC will collect, CALL-PROC for list access -- and what the handset
 * sends on the call meanwhile is held and replayed after it, with no
 * second answer (bench test 7: "---" on the A540 while we were silent).
 */
static void test_held_setup_is_answered_and_its_digits_kept(void)
{
	static const uint8_t roamer[] = { 0x80, 0xa8, 0x02, 0x55, 0x66, 0x77, 0x88 };
	struct dect_mm_cfg cfg;
	struct dect_link l;
	struct dect_link_ind ind;
	uint8_t msg[64], out[64];
	const uint8_t *k;
	size_t n, klen;

	memset(&cfg, 0, sizeof(cfg));
	cfg.defer_unknown = 1;
	dect_link_init(&l, 1, &cfg);
	dect_cc_set_overlap(&l.cc, 1);
	tx_seen_len = 0;
	dect_link_set_tx_log(&l, tx_seen, NULL);
	n = roamer_setup(msg, sizeof(msg), roamer, sizeof(roamer), 0x88);
	feed_nwk(&l, msg, n, &ind);
	CHECK_EQ(dect_link_setup_pending(&l, NULL, NULL), 1);
	CHECK_EQ(tx_seen_len, 2u);
	CHECK_EQ(tx_seen_buf[0], 0x83u);	/* F=1, TV=0, PD CC */
	CHECK_EQ(tx_seen_buf[1], DECT_CC_SETUP_ACK);

	/* its digits arrive while the record is still on its way */
	n = dect_nwk_build_hdr(msg, sizeof(msg), DECT_PD_CC,
			       DECT_TI_ORIGINATOR, 0, DECT_CC_INFO);
	n = dect_nwk_put_ie(msg, sizeof(msg), n, DECT_IE_MULTI_KEYPAD,
			    (const uint8_t *)"123", 3);
	feed_nwk(&l, msg, n, &ind);
	CHECK_EQ(l.held_n, 1u);
	CHECK_EQ(l.cc.stats.keypad, 0u);

	CHECK_EQ(dect_mm_hold(&l.mm, roamer, sizeof(roamer), 4, NULL, 6), 0);
	CHECK_EQ(dect_link_setup_resume(&l, 0), 1);
	CHECK_EQ(l.cc.state, DECT_CC_ST_OVERLAP);
	k = dect_cc_keypad(&l.cc, &klen);
	CHECK(klen == 3 && k && !memcmp(k, "123", 3));
	CHECK_EQ(dect_cc_poll(&l.cc, out, sizeof(out)), 0u);	/* no 2nd ACK */
	CHECK_EQ(dect_cc_accept(&l.cc), 0);
	n = dect_cc_poll(&l.cc, out, sizeof(out));
	CHECK_EQ(out[1], DECT_CC_CALL_PROC);
	n = dect_cc_poll(&l.cc, out, sizeof(out));
	CHECK_EQ(out[1], DECT_CC_CONNECT);

	/* list access: CALL-PROC, and CC does not repeat it */
	dect_link_init(&l, 1, &cfg);
	dect_cc_set_overlap(&l.cc, 1);
	tx_seen_len = 0;
	dect_link_set_tx_log(&l, tx_seen, NULL);
	n = roamer_setup(msg, sizeof(msg), roamer, sizeof(roamer), 0x28);
	feed_nwk(&l, msg, n, &ind);
	CHECK_EQ(tx_seen_buf[1], DECT_CC_CALL_PROC);
	CHECK_EQ(dect_mm_hold(&l.mm, roamer, sizeof(roamer), 4, NULL, 6), 0);
	CHECK_EQ(dect_link_setup_resume(&l, 0), 1);
	CHECK_EQ(l.cc.state, DECT_CC_ST_PROCEEDING);
	CHECK_EQ(dect_cc_poll(&l.cc, out, sizeof(out)), 0u);

	/* the handset gives up while held: RELEASE-COM, and no call left */
	dect_link_init(&l, 1, &cfg);
	dect_cc_set_overlap(&l.cc, 1);
	n = roamer_setup(msg, sizeof(msg), roamer, sizeof(roamer), 0x88);
	feed_nwk(&l, msg, n, &ind);
	n = dect_nwk_build_hdr(msg, sizeof(msg), DECT_PD_CC,
			       DECT_TI_ORIGINATOR, 0, DECT_CC_RELEASE);
	feed_nwk(&l, msg, n, &ind);
	CHECK_EQ(dect_link_setup_pending(&l, NULL, NULL), 0);
	CHECK_EQ(l.lce_len, 2u);
	CHECK_EQ(l.lce[0], 0x83u);
	CHECK_EQ(l.lce[1], DECT_CC_RELEASE_COM);
	CHECK_EQ(dect_link_setup_resume(&l, 0), 0);
}

/* A fresh link owns its own state and has nothing to send. */
static int cs_answer;

static int cs_for(void *ctx, const uint8_t *ipui, size_t len)
{
	(void)ctx;
	(void)ipui;
	(void)len;
	return cs_answer;
}

/*
 * The owner decides call status per handset before CC answers its SETUP
 * (TS 102 527-3 §7.4.6.4.1: none to GAP or Part 1 handsets): no -> the
 * answer carries no <<CALL-INFORMATION>>; yes or no opinion -> it does.
 */
static void test_call_status_is_asked_per_handset(void)
{
	static const uint8_t ours[] = { 0x80, 0xa8, 0x03, 0x11, 0x22, 0x33, 0x44 };
	static const int answer[] = { 0, 1, -1 };
	static const int has_status[] = { 0, 1, 1 };
	struct dect_link l;
	struct dect_link_ind ind;
	uint8_t msg[32];
	size_t n, k, i;
	int found;

	for (k = 0; k < 3; k++) {
		init(&l);
		memcpy(l.mm.port_id, ours, sizeof(ours));
		l.mm.port_id_len = sizeof(ours);
		dect_cc_set_call_status(&l.cc, 1, 1);
		l.call_status_for = cs_for;
		cs_answer = answer[k];
		tx_seen_len = 0;
		dect_link_set_tx_log(&l, tx_seen, NULL);
		n = dect_nwk_build_hdr(msg, sizeof(msg), DECT_PD_CC,
				       DECT_TI_ORIGINATOR, 0, DECT_CC_SETUP);
		n = dect_nwk_put_ie(msg, sizeof(msg), n,
				    DECT_IE_PORTABLE_IDENTITY, ours,
				    sizeof(ours));
		msg[n++] = DECT_IE_BASIC_SERVICE;	/* mandatory */
		msg[n++] = 0x80;
		feed_nwk(&l, msg, n, &ind);
		CHECK(tx_seen_len >= 2u);
		CHECK_EQ(tx_seen_buf[1], DECT_CC_CALL_PROC);
		found = 0;
		for (i = 2; i < tx_seen_len; i++)
			if (tx_seen_buf[i] == DECT_IE_CALL_INFORMATION)
				found = 1;
		CHECK_EQ(found, has_status[k]);
	}
}

static uint8_t id_for(void *ctx, const struct dect_link *l)
{
	(void)ctx;
	(void)l;
	return 5;
}

/* The owner's call id goes into the first answer to the handset's SETUP. */
static void test_call_id_is_asked_per_call(void)
{
	static const uint8_t ours[] = { 0x80, 0xa8, 0x03, 0x11, 0x22, 0x33, 0x44 };
	static const uint8_t id5[] = { 0x01, 0x00, 0x85 };
	struct dect_link l;
	struct dect_link_ind ind;
	uint8_t msg[32];
	size_t n, i;
	int found = 0;

	init(&l);
	memcpy(l.mm.port_id, ours, sizeof(ours));
	l.mm.port_id_len = sizeof(ours);
	dect_cc_set_call_status(&l.cc, 1, 1);
	l.call_id_for = id_for;
	tx_seen_len = 0;
	dect_link_set_tx_log(&l, tx_seen, NULL);
	n = dect_nwk_build_hdr(msg, sizeof(msg), DECT_PD_CC,
			       DECT_TI_ORIGINATOR, 0, DECT_CC_SETUP);
	n = dect_nwk_put_ie(msg, sizeof(msg), n, DECT_IE_PORTABLE_IDENTITY,
			    ours, sizeof(ours));
	msg[n++] = DECT_IE_BASIC_SERVICE;
	msg[n++] = 0x80;
	feed_nwk(&l, msg, n, &ind);
	CHECK_EQ(l.cc.status_call_id, 5);
	for (i = 2; i + 3u <= tx_seen_len; i++)
		if (!memcmp(tx_seen_buf + i, id5, sizeof(id5)))
			found = 1;
	CHECK_EQ(found, 1);
}

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
	m.param[12] = 4;			/* the handle again, as the chip sends it */
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
	m.param[12] = 1;
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
	m.param[12] = 1;
	dect_link_rx_segment(&l, &m, 0, NULL, NULL, &ind);

	CHECK_EQ(dect_link_poll_append_mail(&l, &out), 1);
	CHECK_EQ(out.prim, 0x0244u);
}

int main(void)
{
	test_call_status_is_asked_per_handset();
	test_page_response_without_identity_is_rejected();
	test_call_id_is_asked_per_call();
	test_init_is_quiet();
	test_page_response_carries_the_identity();
	test_cc_admits_only_the_handset_mm_holds();
	test_page_reject_is_built_from_the_response();
	test_unknown_setup_is_held_for_a_fetch();
	test_held_setup_is_answered_and_its_digits_kept();
	test_two_links_are_independent();
	test_outbound_message_becomes_an_append_mail();
	test_a_queued_message_needs_a_tick();
	test_a_segment_for_another_handle_is_rejected();
	test_only_one_message_is_outstanding();
	test_non_cplane_mail_is_rejected();
	test_unusable_segment_still_drains_the_queue();
	return CHECK_DONE();
}

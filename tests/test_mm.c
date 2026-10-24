// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for src/libopendectcell/dect_mm.c: the FP key-allocation registration FSM,
 * driven by a simulated PP that computes its responses with the same DSAA.
 * No hardware.
 */
#include "check.h"
#include "../src/libopendectcell/dect_mm.h"
#include "../src/libopendectcell/dect_nwk.h"
#include "../src/libopendectcell/dect_dsaa.h"

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

/* PP ACCESS-RIGHTS-REQUEST with a PORTABLE-IDENTITY (IPUI-N like). */
static size_t pp_access_request(uint8_t *out, size_t cap, uint8_t ti)
{
	uint8_t pid[7] = { 0x80, 0xB4, 0x00, 0x11, 0x22, 0x33, 0x44 };
	size_t off = dect_nwk_build_hdr(out, cap, DECT_PD_MM, DECT_TI_ORIGINATOR,
					ti, DECT_MM_ACCESS_RIGHTS_REQUEST);

	return dect_nwk_put_ie(out, cap, off, DECT_IE_PORTABLE_IDENTITY, pid, sizeof(pid));
}

/*
 * PP AUTHENTICATION-REQUEST: RES1 = A12(A11(K,RS),RAND_F) from the PIN and the
 * KEY-ALLOCATE's RAND_F/RS, plus the PP's own challenge RAND_P.
 */
static size_t pp_auth_request(uint8_t *out, size_t cap, uint8_t ti,
			      const char *pin, const uint8_t rand_f[8],
			      const uint8_t rs[8], const uint8_t rand_p[8],
			      int corrupt)
{
	uint8_t ac[4], k[16], ks[16], dck[8], res1[4];
	size_t off;

	dect_dsaa_pin_to_ac(pin, ac, sizeof(ac));
	dect_dsaa_b1(ac, sizeof(ac), k);
	dect_dsaa_a11(k, rs, ks);
	dect_dsaa_a12(ks, rand_f, dck, res1);
	if (corrupt)
		res1[0] ^= 0xff;

	off = dect_nwk_build_hdr(out, cap, DECT_PD_MM, DECT_TI_ORIGINATOR, ti,
				 DECT_MM_AUTHENTICATION_REQUEST);
	off = dect_nwk_put_ie(out, cap, off, DECT_IE_RES, res1, sizeof(res1));
	return dect_nwk_put_ie(out, cap, off, DECT_IE_RAND, rand_p, 8);
}

/* Full successful key-allocation registration with the correct PIN. */
static void test_register_ok(void)
{
	struct dect_mm_cfg cfg = { .pin = "1234", .authenticate = 1,
				   .rfpi = { 1, 2, 3, 4, 5 } };
	struct dect_mm mm;
	uint8_t rand_f[8] = { 0xde, 0xad, 0xbe, 0xef, 0x12, 0x34, 0x56, 0x78 };
	uint8_t rand_p[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	uint8_t rs[8] = { 0 };		/* DSAA1 seed (first 8 of rs16) */
	uint8_t rs16[16] = { 0 };	/* 16-byte RS handed to the FSM */
	uint8_t buf[256], out[256];
	struct dect_nwk_hdr h;
	const uint8_t *ies, *ka_rand;
	size_t n, ies_len, ka_rand_len;

	dect_mm_init(&mm, &cfg);

	/* ACCESS-RIGHTS-REQUEST -> KEY-ALLOCATE (carrying RAND_F). */
	n = pp_access_request(buf, sizeof(buf), 2);
	dect_mm_rx(&mm, buf, n, rand_f, rs16);
	n = dect_mm_poll(&mm, out, sizeof(out));
	CHECK(n > 0);
	CHECK_EQ(msg_type_of(out, n), DECT_MM_KEY_ALLOCATE);
	CHECK_EQ(mm.state, DECT_MM_WAIT_AUTH);
	dect_nwk_parse(out, n, &h, &ies, &ies_len);
	/* KEY-ALLOCATE opens an FT-initiated transaction: F=0 [CR A-0022]. */
	CHECK_EQ(h.ti_flag, DECT_TI_ORIGINATOR);
	ka_rand = dect_nwk_find_ie(ies, ies_len, DECT_IE_RAND, &ka_rand_len);
	CHECK(ka_rand != NULL && ka_rand_len == 8);
	CHECK_EQ(memcmp(ka_rand, rand_f, 8), 0);
	/* KEY-ALLOCATE must match AVM's mode-2 wire form [CR A-0011]. */
	{
		const uint8_t *at, *rsie;
		size_t at_len, rsie_len;
		const uint8_t want_at[2] = { 0x01, 0x88 };	/* DSAA1 */

		at = dect_nwk_find_ie(ies, ies_len, DECT_IE_ALLOCATION_TYPE, &at_len);
		CHECK(at != NULL && at_len == 2);
		CHECK_EQ(memcmp(at, want_at, 2), 0);
		rsie = dect_nwk_find_ie(ies, ies_len, DECT_IE_RS, &rsie_len);
		CHECK(rsie != NULL && rsie_len == 8);
	}

	/* PP AUTHENTICATION-REQUEST -> AUTHENTICATION-REPLY then ACCESS-RIGHTS-ACCEPT. */
	n = pp_auth_request(buf, sizeof(buf), 2, "1234", rand_f, rs, rand_p, 0);
	dect_mm_rx(&mm, buf, n, rand_f, rs16);
	CHECK_EQ(mm.stats.auth_ok, 1);

	n = dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_MM_AUTHENTICATION_REPLY);
	/* The reply's RES2 must equal A22(A21(K,RS), RAND_P). */
	{
		uint8_t ac[4], k[16], uak[16], want[4];
		const uint8_t *res2;
		size_t res2_len;

		dect_dsaa_pin_to_ac("1234", ac, sizeof(ac));
		dect_dsaa_b1(ac, sizeof(ac), k);
		dect_dsaa_a21(k, rs, uak);
		dect_dsaa_a22(uak, rand_p, want);
		dect_nwk_parse(out, n, &h, &ies, &ies_len);
		/* AUTH-REPLY stays in the FT-initiated transaction: F=0 [CR A-0022]. */
		CHECK_EQ(h.ti_flag, DECT_TI_ORIGINATOR);
		res2 = dect_nwk_find_ie(ies, ies_len, DECT_IE_RES, &res2_len);
		CHECK(res2 != NULL && res2_len == 4);
		CHECK_EQ(memcmp(res2, want, 4), 0);
		/* The allocated UAK (A21 output) is retained for chip persistence. */
		CHECK_EQ(mm.subscribed, 1);
		CHECK_EQ(memcmp(mm.uak, uak, sizeof(uak)), 0);
	}

	n = dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_MM_ACCESS_RIGHTS_ACCEPT);
	/* ACCESS-RIGHTS-ACCEPT closes the PP-initiated transaction: F=1 [CR A-0022]. */
	dect_nwk_parse(out, n, &h, &ies, &ies_len);
	CHECK_EQ(h.ti_flag, DECT_TI_RESPONDER);
	CHECK_EQ(mm.state, DECT_MM_REGISTERED);
	CHECK_EQ(mm.stats.accepted, 1);

	/*
	 * The identities an FP-originated {CC-SETUP} needs are both already
	 * here, in this message, at registration time -- the handset's IPUI
	 * and our own PARK. dect_mm_fixed_identity() must hand CC byte-for-byte
	 * what MM just put on the wire, because the whole point of exposing it
	 * is that there is ONE encoding of the PARK and not two.
	 *
	 * This is what lets us ring a handset that has never placed a call.
	 * dect-stubd used to copy the pair out of the first call the handset
	 * made, so "ring the handset" silently depended on the handset having
	 * dialled us first; on a handset that never does, it could not ring at
	 * all. Measured against the A540: our accept and the {CC-SETUP} it
	 * later places carry identical <<PORTABLE-IDENTITY>> and
	 * <<FIXED-IDENTITY>> elements.
	 */
	{
		/*
		 * Ground truth, not self-consistency. Comparing the builder
		 * against {ACCESS-RIGHTS-ACCEPT} alone proves nothing once the
		 * accept is built BY the builder -- mutating the encoding
		 * moves both sides together and such a test passes happily.
		 * (Checked: it does.) So pin the bytes.
		 *
		 * 0xA0 = ext + type 32 PARK; 0xA5 = ext + 37 = PLI+1 for a
		 * 36-bit class-A ARI. Measured on hardware against the A540,
		 * where our accept carries a0 a5 de ad 00 00 b8 for RFPI
		 * de ad 00 00 b8, and the {CC-SETUP} the handset itself places
		 * carries the same seven octets.
		 */
		static const uint8_t want[] = { 0xA0u, 0xA5u, 1, 2, 3, 4, 5 };
		uint8_t fid[2 + DECT_MM_RFPI_LEN];
		const uint8_t *wire;
		size_t wire_len, got;

		got = dect_mm_fixed_identity(&mm, fid, sizeof(fid));
		CHECK_EQ(got, sizeof(fid));
		CHECK_EQ(sizeof(want), sizeof(fid));
		CHECK_EQ(memcmp(fid, want, sizeof(want)), 0);

		/* And MM really does put those bytes on the wire. */
		wire = dect_nwk_find_ie(ies, ies_len, DECT_IE_FIXED_IDENTITY,
					&wire_len);
		CHECK(wire != NULL);
		CHECK_EQ(wire_len, got);
		CHECK_EQ(memcmp(wire, want, got), 0);

		/* And the portable identity CC needs is the one MM holds. */
		wire = dect_nwk_find_ie(ies, ies_len, DECT_IE_PORTABLE_IDENTITY,
					&wire_len);
		CHECK(wire != NULL);
		CHECK_EQ(wire_len, mm.port_id_len);
		CHECK_EQ(memcmp(wire, mm.port_id, wire_len), 0);

		/* Too small a buffer writes nothing rather than truncating. */
		CHECK_EQ(dect_mm_fixed_identity(&mm, fid, sizeof(fid) - 1u), 0);
	}
}

/* A wrong PIN (RES1 mismatch) is rejected. */
static void test_register_bad_pin(void)
{
	struct dect_mm_cfg cfg = { .pin = "1234", .authenticate = 1 };
	struct dect_mm mm;
	uint8_t rand_f[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	uint8_t rand_p[8] = { 9, 9, 9, 9, 9, 9, 9, 9 };
	uint8_t rs[8] = { 0 };		/* DSAA1 seed (first 8 of rs16) */
	uint8_t rs16[16] = { 0 };	/* 16-byte RS handed to the FSM */
	uint8_t buf[256], out[256];
	size_t n;

	dect_mm_init(&mm, &cfg);
	n = pp_access_request(buf, sizeof(buf), 1);
	dect_mm_rx(&mm, buf, n, rand_f, rs16);
	dect_mm_poll(&mm, out, sizeof(out));	/* KEY-ALLOCATE */

	/* PP computes RES1 from the wrong PIN "9999". */
	n = pp_auth_request(buf, sizeof(buf), 1, "9999", rand_f, rs, rand_p, 0);
	dect_mm_rx(&mm, buf, n, rand_f, rs16);
	n = dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_MM_ACCESS_RIGHTS_REJECT);
	CHECK_EQ(mm.state, DECT_MM_FAILED);
	CHECK_EQ(mm.stats.auth_fail, 1);
}

/* After registration the PP runs location registration; we must LOCATE-ACCEPT. */
static void test_locate(void)
{
	struct dect_mm_cfg cfg = { .pin = "1234", .authenticate = 1 };
	struct dect_mm mm;
	uint8_t la[1] = { 0x64 };	/* LOCATION-AREA, LAL=36 bits */
	uint8_t buf[64], out[64];
	struct dect_nwk_hdr h;
	const uint8_t *ies, *rx_la;
	size_t n, ies_len, rx_la_len;

	dect_mm_init(&mm, &cfg);

	/* PP LOCATE-REQUEST (PP-initiated: F=0) carrying a LOCATION-AREA. */
	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_MM, DECT_TI_ORIGINATOR,
			       0, DECT_MM_LOCATE_REQUEST);
	n = dect_nwk_put_ie(buf, sizeof(buf), n, DECT_IE_LOCATION_AREA, la, sizeof(la));
	dect_mm_rx(&mm, buf, n, NULL, NULL);

	n = dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_MM_LOCATE_ACCEPT);
	CHECK_EQ(mm.stats.located, 1);
	dect_nwk_parse(out, n, &h, &ies, &ies_len);
	/* LOCATE-ACCEPT closes the PP-initiated transaction: F=1. */
	CHECK_EQ(h.ti_flag, DECT_TI_RESPONDER);
	rx_la = dect_nwk_find_ie(ies, ies_len, DECT_IE_LOCATION_AREA, &rx_la_len);
	CHECK(rx_la != NULL && rx_la_len == 1);
	CHECK_EQ(rx_la[0], 0x64);
	/* Must assign a TPUI via PORTABLE-IDENTITY (type A0, 20-bit) [CR A-0023]. */
	{
		const uint8_t *pid;
		size_t pid_len;
		const uint8_t want_tpui[5] = { 0xA0, 0x94, 0x00, 0x11, 0x01 };

		pid = dect_nwk_find_ie(ies, ies_len, DECT_IE_PORTABLE_IDENTITY, &pid_len);
		CHECK(pid != NULL && pid_len == 5);
		CHECK_EQ(memcmp(pid, want_tpui, 5), 0);
	}
}

/*
 * Call-time ciphering: the FP challenges the PP with the UAK, derives the DCK
 * from the PP's reply, and asks it to start DSC.
 */
static void test_cipher(void)
{
	struct dect_mm_cfg cfg = { .authenticate = 1 };
	struct dect_mm mm;
	uint8_t uak[16] = { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
			    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f };
	uint8_t rand_f[8] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01, 0x02 };
	uint8_t rs[16] = { 0 };
	uint8_t buf[64], out[64], got_dck[8];
	struct dect_nwk_hdr h;
	const uint8_t *ies;
	size_t n, ies_len;
	uint8_t ks[16], want_dck[8], res[4];

	dect_mm_init(&mm, &cfg);
	dect_mm_start_cipher(&mm, uak, rand_f, rs);

	/* AUTHENTICATION-REQUEST goes out, FP-originated (F=0). */
	n = dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_MM_AUTHENTICATION_REQUEST);
	dect_nwk_parse(out, n, &h, &ies, &ies_len);
	CHECK_EQ(h.ti_flag, DECT_TI_ORIGINATOR);

	/* PP computes RES = A12(A11(UAK,RS),RAND_F) and replies (F=1). */
	dect_dsaa_a11(uak, rs, ks);
	dect_dsaa_a12(ks, rand_f, want_dck, res);
	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_MM, DECT_TI_RESPONDER,
			       0, DECT_MM_AUTHENTICATION_REPLY);
	n = dect_nwk_put_ie(buf, sizeof(buf), n, DECT_IE_RES, res, 4);
	dect_mm_rx(&mm, buf, n, rand_f, rs);

	CHECK_EQ(mm.cipher_state, DECT_MM_CIPHER_READY);
	CHECK_EQ(dect_mm_cipher_dck(&mm, got_dck), 1);
	CHECK_EQ(memcmp(got_dck, want_dck, 8), 0);

	/* And a CIPHER-REQUEST is queued. */
	n = dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_MM_CIPHER_REQUEST);
}


/*
 * A GAP FP serves subscribed handsets. Measured 2026-10-23: with one
 * handset's subscription loaded, a different handset had its location
 * registration accepted thirty times in ninety seconds, because the chip
 * admits anything locked to our RFPI and this was the only gate.
 */
static void test_locate_request_from_a_stranger_is_rejected(void)
{
	static const uint8_t ours[]    = { 0x80, 0xa8, 0x02, 0x11, 0x22, 0x33 };
	static const uint8_t stranger[] = { 0x80, 0xa8, 0x02, 0x44, 0x55, 0x66 };
	struct dect_mm mm;
	uint8_t buf[64], out[64];
	size_t n, m;

	struct dect_mm_cfg cfg;

	memset(&cfg, 0, sizeof(cfg));
	dect_mm_init(&mm, &cfg);
	memcpy(mm.port_id, ours, sizeof(ours));
	mm.port_id_len = sizeof(ours);

	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_MM, DECT_TI_ORIGINATOR,
			       0, DECT_MM_LOCATE_REQUEST);
	n = dect_nwk_put_ie(buf, sizeof(buf), n, DECT_IE_PORTABLE_IDENTITY,
			    stranger, sizeof(stranger));
	dect_mm_rx(&mm, buf, n, NULL, NULL);

	m = dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(out[1], DECT_MM_LOCATE_REJECT);
	CHECK_EQ(out[2], DECT_IE_REJECT_REASON);
	CHECK_EQ(out[4], DECT_MM_REJECT_IPUI_UNKNOWN);
	CHECK_EQ(mm.stats.locate_rej, 1u);
	(void)m;
}

/* ... and the handset we DO hold is still accepted. */
static void test_locate_request_from_our_handset_is_accepted(void)
{
	static const uint8_t ours[] = { 0x80, 0xa8, 0x02, 0x11, 0x22, 0x33 };
	struct dect_mm mm;
	uint8_t buf[64], out[64];
	size_t n;

	struct dect_mm_cfg cfg;

	memset(&cfg, 0, sizeof(cfg));
	dect_mm_init(&mm, &cfg);
	memcpy(mm.port_id, ours, sizeof(ours));
	mm.port_id_len = sizeof(ours);

	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_MM, DECT_TI_ORIGINATOR,
			       0, DECT_MM_LOCATE_REQUEST);
	n = dect_nwk_put_ie(buf, sizeof(buf), n, DECT_IE_PORTABLE_IDENTITY,
			    ours, sizeof(ours));
	dect_mm_rx(&mm, buf, n, NULL, NULL);

	dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(out[1], DECT_MM_LOCATE_ACCEPT);
	CHECK_EQ(mm.stats.locate_rej, 0u);
}

/*
 * Drive MM to the point where a {LOCATE-ACCEPT} has gone out carrying a TPUI
 * -- which is what starts the temporary-identity assignment procedure.
 */
static void mm_setup_located(struct dect_mm *mm, uint8_t *out, size_t cap)
{
	static struct dect_mm_cfg cfg = { .pin = "1234",
					  .rfpi = { 1, 2, 3, 4, 5 } };
	uint8_t la[1] = { 0x64 };
	uint8_t buf[64];
	size_t n;

	dect_mm_init(mm, &cfg);
	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_MM,
			       DECT_TI_ORIGINATOR, 0, DECT_MM_LOCATE_REQUEST);
	n = dect_nwk_put_ie(buf, sizeof(buf), n, DECT_IE_LOCATION_AREA,
			    la, sizeof(la));
	dect_mm_rx(mm, buf, n, NULL, NULL);
	n = dect_mm_poll(mm, out, cap);
	CHECK_EQ(msg_type_of(out, n), DECT_MM_LOCATE_ACCEPT);
}

/*
 * <MM_ident.1>, the temporary-identity assignment timer (EN 300 175-5
 * §13.2.2, §A.2). A TPUI in our {LOCATE-ACCEPT} starts the assignment
 * procedure; the FT runs a 10 s timer and stops it on the PT's
 * {TEMPORARY-IDENTITY-ASSIGN-ACK}.
 *
 * The ack is `05 5d` -- two octets, no elements -- which is how it appears in
 * our own captures, unidentified and ignored, before 2026-10-23.
 */
static void test_tpui_assignment_is_confirmed_by_the_ack(void)
{
	static const uint8_t ack[] = { 0x05, 0x5D };
	struct dect_mm mm;
	uint8_t out[128];

	mm_setup_located(&mm, out, sizeof(out));
	/* The assignment has gone out and is not yet confirmed. */
	CHECK_EQ(dect_mm_tpui_confirmed(&mm), 0);

	dect_mm_tick(&mm, 1000);			/* arms the deadline */
	CHECK_EQ(dect_mm_next_deadline(&mm), 11000u);	/* 10 s, FT value */

	dect_mm_rx(&mm, ack, sizeof(ack), NULL, NULL);
	CHECK_EQ(dect_mm_tpui_confirmed(&mm), 1);
	CHECK_EQ(mm.stats.tpui_acked, 1u);
	CHECK_EQ(mm.stats.tpui_timeouts, 0u);

	/* Timer stopped: no deadline, and later ticks change nothing. */
	CHECK_EQ(dect_mm_next_deadline(&mm), UINT64_MAX);
	dect_mm_tick(&mm, 999999);
	CHECK_EQ(dect_mm_tpui_confirmed(&mm), 1);
	CHECK_EQ(mm.stats.tpui_timeouts, 0u);
}

/* No ack within 10 s: the assignment is recorded as unconfirmed, not retried. */
static void test_tpui_assignment_times_out_at_ten_seconds(void)
{
	struct dect_mm mm;
	uint8_t out[128];

	mm_setup_located(&mm, out, sizeof(out));
	dect_mm_tick(&mm, 1000);
	dect_mm_tick(&mm, 10999);		/* one millisecond early */
	CHECK_EQ(mm.stats.tpui_timeouts, 0u);
	CHECK_EQ(dect_mm_tpui_confirmed(&mm), 0);

	dect_mm_tick(&mm, 11000);		/* due */
	CHECK_EQ(mm.stats.tpui_timeouts, 1u);
	CHECK_EQ(mm.tpui_timed_out, 1u);
	CHECK_EQ(dect_mm_tpui_confirmed(&mm), 0);
	CHECK_EQ(dect_mm_next_deadline(&mm), UINT64_MAX);

	/* It fires once, not every tick. */
	dect_mm_tick(&mm, 30000);
	CHECK_EQ(mm.stats.tpui_timeouts, 1u);
}

/* An ack arriving late, after the timeout, does not resurrect the assignment
 * silently -- it is counted, and the caller can still see it timed out. */
static void test_a_late_ack_is_visible_as_late(void)
{
	static const uint8_t ack[] = { 0x05, 0x5D };
	struct dect_mm mm;
	uint8_t out[128];

	mm_setup_located(&mm, out, sizeof(out));
	dect_mm_tick(&mm, 1000);
	dect_mm_tick(&mm, 11000);
	CHECK_EQ(mm.tpui_timed_out, 1u);

	dect_mm_rx(&mm, ack, sizeof(ack), NULL, NULL);
	CHECK_EQ(mm.stats.tpui_acked, 1u);
	CHECK_EQ(dect_mm_tpui_confirmed(&mm), 1);
	CHECK_EQ(mm.tpui_timed_out, 1u);	/* the history is kept */
}

/* With no assignment outstanding the timer never arms. */
static void test_no_assignment_means_no_timer(void)
{
	struct dect_mm_cfg cfg = { .pin = "1234", .rfpi = { 1, 2, 3, 4, 5 } };
	struct dect_mm mm;

	dect_mm_init(&mm, &cfg);
	dect_mm_tick(&mm, 1000);
	CHECK_EQ(dect_mm_next_deadline(&mm), UINT64_MAX);
	dect_mm_tick(&mm, 999999);
	CHECK_EQ(mm.stats.tpui_timeouts, 0u);
	CHECK_EQ(dect_mm_tpui_confirmed(&mm), 0);
}

int main(void)
{
	test_locate_request_from_a_stranger_is_rejected();
	test_locate_request_from_our_handset_is_accepted();
	test_register_ok();
	test_register_bad_pin();
	test_locate();
	test_cipher();
	test_tpui_assignment_is_confirmed_by_the_ack();
	test_tpui_assignment_times_out_at_ten_seconds();
	test_a_late_ack_is_visible_as_late();
	test_no_assignment_means_no_timer();
	return CHECK_DONE();
}

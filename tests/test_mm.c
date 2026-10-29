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

	/* the answer to our KEY-ALLOCATE: our transaction, so F = 1 */
	off = dect_nwk_build_hdr(out, cap, DECT_PD_MM, DECT_TI_RESPONDER, ti,
				 DECT_MM_AUTHENTICATION_REQUEST);
	off = dect_nwk_put_ie(out, cap, off, DECT_IE_RES, res1, sizeof(res1));
	return dect_nwk_put_ie(out, cap, off, DECT_IE_RAND, rand_p, 8);
}

/*
 * MM has TV 0 only (EN 300 175-5 table 68): another value is ignored
 * (§17.3.1). And the key-allocation answer must ride our transaction (F = 1):
 * a handset-initiated AUTHENTICATION-REQUEST is not it.
 */
static void test_mm_transaction_rules(void)
{
	struct dect_mm_cfg cfg = { .pin = "1234", .authenticate = 1,
				   .rfpi = { 1, 2, 3, 4, 5 } };
	struct dect_mm mm;
	uint8_t rand_f[8] = { 0xde, 0xad, 0xbe, 0xef, 0x12, 0x34, 0x56, 0x78 };
	uint8_t rand_p[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	uint8_t rs[8] = { 0 };
	uint8_t rs16[16] = { 0 };
	uint8_t buf[256], out[256];
	size_t n;

	dect_mm_init(&mm, &cfg);
	n = pp_access_request(buf, sizeof(buf), 2);
	dect_mm_rx(&mm, buf, n, rand_f, rs16);
	CHECK_EQ(dect_mm_poll(&mm, out, sizeof(out)), 0u);
	CHECK_EQ(mm.stats.access_req, 0u);
	CHECK_EQ(mm.stats.bad_ti, 1u);

	n = pp_access_request(buf, sizeof(buf), 0);
	dect_mm_rx(&mm, buf, n, rand_f, rs16);
	n = dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_MM_KEY_ALLOCATE);

	n = pp_auth_request(buf, sizeof(buf), 0, "1234", rand_f, rs, rand_p, 0);
	buf[0] &= 0x7Fu;			/* F = 0: the handset's own */
	dect_mm_rx(&mm, buf, n, rand_f, rs16);
	/* its authentication of us, refused without a UAK -- not taken
	 * as the key-allocation answer */
	n = dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_MM_AUTHENTICATION_REJECT);
	CHECK_EQ(mm.stats.auth_ok, 0u);
	n = pp_auth_request(buf, sizeof(buf), 0, "1234", rand_f, rs, rand_p, 0);
	CHECK_EQ(mm.state, DECT_MM_WAIT_AUTH);

	buf[0] |= 0x80u;			/* F = 1: the answer */
	dect_mm_rx(&mm, buf, n, rand_f, rs16);
	CHECK_EQ(mm.stats.auth_ok, 1u);
}

/*
 * The <<AUTH-TYPE>> a handset proposes is kept for the log: the C4 asks for
 * DSAA2 (`0a 03 02 48 00`), the Gigasets for DSAA (`01 48 00`), both with
 * the authentication code, key 8.
 */
static void test_proposed_auth_type_is_kept(void)
{
	static const uint8_t c4_at[3] = { 0x02, 0x48, 0x00 };
	uint8_t pid[7] = { 0x80, 0xB4, 0x00, 0x11, 0x22, 0x33, 0x44 };
	struct dect_mm_cfg cfg = { .pin = "1234", .authenticate = 1,
				   .rfpi = { 1, 2, 3, 4, 5 } };
	struct dect_mm mm;
	uint8_t rand_f[8] = { 1 }, rs16[16] = { 0 };
	uint8_t buf[64];
	size_t n;

	dect_mm_init(&mm, &cfg);
	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_MM, DECT_TI_ORIGINATOR,
			       0, DECT_MM_ACCESS_RIGHTS_REQUEST);
	n = dect_nwk_put_ie(buf, sizeof(buf), n, DECT_IE_PORTABLE_IDENTITY,
			    pid, sizeof(pid));
	n = dect_nwk_put_ie(buf, sizeof(buf), n, DECT_IE_AUTH_TYPE,
			    c4_at, sizeof(c4_at));
	dect_mm_rx(&mm, buf, n, rand_f, rs16);
	CHECK_EQ(mm.peer_auth_len, 3u);
	CHECK(memcmp(mm.peer_auth, c4_at, 3) == 0);
	CHECK_EQ(mm.stats.key_alloc_sent, 1u);		/* DSAA all the same */
}

/* An ACCESS-RIGHTS-REQUEST without its identity: rejected 60H (§17.6.4). */
static void test_access_request_without_identity_is_rejected(void)
{
	struct dect_mm_cfg cfg = { .pin = "1234", .authenticate = 1,
				   .rfpi = { 1, 2, 3, 4, 5 } };
	struct dect_mm mm;
	uint8_t rand_f[8] = { 1 }, rs16[16] = { 0 };
	uint8_t buf[64], out[64];
	size_t n;

	dect_mm_init(&mm, &cfg);
	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_MM, DECT_TI_ORIGINATOR,
			       0, DECT_MM_ACCESS_RIGHTS_REQUEST);
	dect_mm_rx(&mm, buf, n, rand_f, rs16);
	n = dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_MM_ACCESS_RIGHTS_REJECT);
	CHECK_EQ(out[4], DECT_MM_REJECT_IE_ERROR);
	CHECK_EQ(mm.stats.key_alloc_sent, 0u);
	CHECK_EQ(dect_mm_poll(&mm, out, sizeof(out)), 0u);
}

/*
 * <MM_key.1>: a KEY-ALLOCATE unanswered for 10 s has failed (EN 300 444
 * §8.32.2.1), and so has one the PT refuses (§8.32.2.3); either way the
 * access rights are rejected "authentication failed", once.
 */
static void test_key_allocation_fails_on_timeout_or_refusal(void)
{
	struct dect_mm_cfg cfg = { .pin = "1234", .authenticate = 1,
				   .rfpi = { 1, 2, 3, 4, 5 } };
	struct dect_mm mm;
	uint8_t rand_f[8] = { 1 }, rs16[16] = { 0 };
	uint8_t buf[64], out[64];
	size_t n;
	int k;

	for (k = 0; k < 2; k++) {
		dect_mm_init(&mm, &cfg);
		n = pp_access_request(buf, sizeof(buf), 0);
		dect_mm_rx(&mm, buf, n, rand_f, rs16);
		n = dect_mm_poll(&mm, out, sizeof(out));
		CHECK_EQ(msg_type_of(out, n), DECT_MM_KEY_ALLOCATE);
		dect_mm_tick(&mm, 1000);			/* armed */
		CHECK_EQ(dect_mm_next_deadline(&mm), 11000u);
		if (!k) {
			dect_mm_tick(&mm, 10999);
			CHECK_EQ(dect_mm_poll(&mm, out, sizeof(out)), 0u);
			dect_mm_tick(&mm, 11000);
		} else {
			n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_MM,
					       DECT_TI_RESPONDER, 0,
					       DECT_MM_AUTHENTICATION_REJECT);
			dect_mm_rx(&mm, buf, n, rand_f, rs16);
		}
		n = dect_mm_poll(&mm, out, sizeof(out));
		CHECK_EQ(msg_type_of(out, n), DECT_MM_ACCESS_RIGHTS_REJECT);
		CHECK_EQ(out[4], DECT_MM_REJECT_AUTH_FAILED);
		CHECK_EQ(mm.state, DECT_MM_FAILED);
		CHECK_EQ(mm.stats.key_failed, 1u);
		CHECK_EQ(dect_mm_next_deadline(&mm), UINT64_MAX);
		dect_mm_tick(&mm, 30000);			/* once */
		CHECK_EQ(dect_mm_poll(&mm, out, sizeof(out)), 0u);
		CHECK_EQ(mm.stats.key_failed, 1u);
	}
}

/*
 * The handset authenticates us (EN 300 444 §8.23): with the stored UAK we
 * answer RES2 = A22(A21(UAK, RS), RAND_P) and the RS, on its transaction.
 */
static void test_ft_authentication(void)
{
	static const uint8_t at[3] = { 0x01, 0x18, 0x00 };	/* DSAA, UAK 8 */
	uint8_t uak[16] = { 0x31, 0x41, 0x59, 0x26 };
	uint8_t rand_p[8] = { 9, 8, 7, 6, 5, 4, 3, 2 };
	uint8_t rs16[16] = { 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC };
	uint8_t buf[64], out[64], ks[16], want[4];
	struct dect_mm_cfg cfg = { .rfpi = { 1, 2, 3, 4, 5 } };
	struct dect_mm mm;
	struct dect_nwk_hdr h;
	const uint8_t *ies, *res, *rs;
	size_t n, ies_len, rl, sl;

	dect_mm_init(&mm, &cfg);
	dect_mm_set_uak(&mm, uak);
	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_MM, DECT_TI_ORIGINATOR,
			       0, DECT_MM_AUTHENTICATION_REQUEST);
	n = dect_nwk_put_ie(buf, sizeof(buf), n, DECT_IE_AUTH_TYPE, at, 3);
	n = dect_nwk_put_ie(buf, sizeof(buf), n, DECT_IE_RAND, rand_p, 8);
	dect_mm_rx(&mm, buf, n, NULL, rs16);
	n = dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_MM_AUTHENTICATION_REPLY);
	CHECK_EQ(dect_nwk_parse(out, n, &h, &ies, &ies_len), 0);
	CHECK_EQ(h.ti_flag, DECT_TI_RESPONDER);
	res = dect_nwk_find_ie(ies, ies_len, DECT_IE_RES, &rl);
	rs = dect_nwk_find_ie(ies, ies_len, DECT_IE_RS, &sl);
	CHECK(res && rl == 4u && rs && sl == 8u);
	dect_dsaa_a21(uak, rs16, ks);
	dect_dsaa_a22(ks, rand_p, want);
	CHECK(res && memcmp(res, want, 4) == 0);
	CHECK(rs && memcmp(rs, rs16, 8) == 0);
	CHECK_EQ(mm.stats.ft_auth, 1u);
}

/*
 * FT-initiated termination of access rights (EN 300 444 §8.31): the request
 * names the IPUI and our PARK; ACCEPT ends it; with no answer <MM_access.2>
 * retransmits once at 10 s and fails at 20 s.
 */
static void test_terminate_access_rights(void)
{
	static const uint8_t ipui[] = { 0x80, 0xa8, 0x02, 0x11, 0x22, 0x33 };
	struct dect_mm_cfg cfg = { .rfpi = { 1, 2, 3, 4, 5 } };
	struct dect_mm mm;
	struct dect_nwk_hdr h;
	const uint8_t *ies, *p;
	uint8_t buf[64], out[64];
	size_t n, ies_len, pl;
	int k;

	dect_mm_init(&mm, &cfg);
	CHECK_EQ(dect_mm_terminate(&mm), -1);		/* nobody to name */
	for (k = 0; k < 2; k++) {
		dect_mm_init(&mm, &cfg);
		memcpy(mm.port_id, ipui, sizeof(ipui));
		mm.port_id_len = sizeof(ipui);
		CHECK_EQ(dect_mm_terminate(&mm), 0);
		CHECK_EQ(dect_mm_terminate(&mm), -1);	/* one at a time */
		n = dect_mm_poll(&mm, out, sizeof(out));
		CHECK_EQ(dect_nwk_parse(out, n, &h, &ies, &ies_len), 0);
		CHECK_EQ(h.msg_type, DECT_MM_ACCESS_RIGHTS_TERMINATE_REQUEST);
		CHECK_EQ(h.ti_flag, DECT_TI_ORIGINATOR);
		p = dect_nwk_find_ie(ies, ies_len, DECT_IE_PORTABLE_IDENTITY,
				     &pl);
		CHECK(p && pl == sizeof(ipui) && !memcmp(p, ipui, pl));
		CHECK(dect_nwk_find_ie(ies, ies_len, DECT_IE_FIXED_IDENTITY,
				       &pl) != NULL);
		dect_mm_tick(&mm, 1000);			/* armed */
		if (!k) {
			n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_MM,
				DECT_TI_RESPONDER, 0,
				DECT_MM_ACCESS_RIGHTS_TERMINATE_ACCEPT);
			dect_mm_rx(&mm, buf, n, NULL, NULL);
			CHECK_EQ(mm.term_state, DECT_MM_TERM_ACCEPTED);
			CHECK_EQ(dect_mm_next_deadline(&mm), UINT64_MAX);
		} else {
			dect_mm_tick(&mm, 11000);		/* retransmit */
			n = dect_mm_poll(&mm, out, sizeof(out));
			CHECK_EQ(msg_type_of(out, n),
				 DECT_MM_ACCESS_RIGHTS_TERMINATE_REQUEST);
			CHECK_EQ(mm.term_state, DECT_MM_TERM_PENDING);
			dect_mm_tick(&mm, 21000);		/* give up */
			CHECK_EQ(mm.term_state, DECT_MM_TERM_FAILED);
			CHECK_EQ(dect_mm_poll(&mm, out, sizeof(out)), 0u);
		}
	}
}

/*
 * A handset deregisters itself (EN 300 175-5 §13.5.2): we authenticate it
 * first with its UAK. The right RES: ACCEPT and term_by_pt. A wrong RES, no
 * UAK, its AUTHENTICATION-REJECT or <MM_auth.1>: REJECT "authentication
 * failed". Another identity: REJECT "IPUI unknown". An attacker who knows
 * an IPUI -- it is on the air in every LOCATE-REQUEST -- gets no further.
 */
static void test_handset_deregisters_itself(void)
{
	static const uint8_t ipui[] = { 0x80, 0xa8, 0x02, 0x11, 0x22, 0x33 };
	static const uint8_t other[] = { 0x80, 0xa8, 0x02, 0x44, 0x55, 0x66 };
	uint8_t uak[16] = { 0x27, 0x18, 0x28, 0x18 };
	uint8_t rand_f[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	uint8_t rs[16] = { 9, 9, 9, 9, 8, 8, 8, 8 };
	struct dect_mm_cfg cfg = { .rfpi = { 1, 2, 3, 4, 5 } };
	struct dect_mm mm;
	struct dect_nwk_hdr h;
	const uint8_t *ies, *at;
	uint8_t buf[64], out[64], ks[16], dck[8], res[4];
	size_t n, ies_len, al;
	int k;

	dect_dsaa_a11(uak, rs, ks);
	dect_dsaa_a12(ks, rand_f, dck, res);
	/* k: 0 right RES, 1 wrong RES, 2 no UAK, 3 other IPUI, 4 timeout,
	 * 5 the handset refuses our challenge */
	for (k = 0; k < 6; k++) {
		dect_mm_init(&mm, &cfg);
		memcpy(mm.port_id, ipui, sizeof(ipui));
		mm.port_id_len = sizeof(ipui);
		if (k != 2)
			dect_mm_set_uak(&mm, uak);
		n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_MM,
				       DECT_TI_ORIGINATOR, 0,
				       DECT_MM_ACCESS_RIGHTS_TERMINATE_REQUEST);
		n = dect_nwk_put_ie(buf, sizeof(buf), n,
				    DECT_IE_PORTABLE_IDENTITY,
				    k == 3 ? other : ipui, sizeof(ipui));
		dect_mm_rx(&mm, buf, n, rand_f, rs);
		n = dect_mm_poll(&mm, out, sizeof(out));
		CHECK_EQ(dect_nwk_parse(out, n, &h, &ies, &ies_len), 0);
		if (k == 2 || k == 3) {
			CHECK_EQ(h.msg_type,
				 DECT_MM_ACCESS_RIGHTS_TERMINATE_REJECT);
			CHECK_EQ(out[4], k == 2 ? DECT_MM_REJECT_AUTH_FAILED
						: DECT_MM_REJECT_IPUI_UNKNOWN);
			CHECK_EQ(mm.term_by_pt, 0u);
			continue;
		}
		/* the challenge, on our own transaction */
		CHECK_EQ(h.msg_type, DECT_MM_AUTHENTICATION_REQUEST);
		CHECK_EQ(h.ti_flag, DECT_TI_ORIGINATOR);
		at = dect_nwk_find_ie(ies, ies_len, DECT_IE_AUTH_TYPE, &al);
		CHECK(at && al == 3u && at[1] == 0x18u && at[2] == 0x08u);
		CHECK_EQ(mm.term_by_pt, 0u);		/* not yet */
		if (k == 4) {
			dect_mm_tick(&mm, 1000);
			dect_mm_tick(&mm, 11000);		/* <MM_auth.1> */
		} else if (k == 5) {
			n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_MM,
					       DECT_TI_RESPONDER, 0,
					       DECT_MM_AUTHENTICATION_REJECT);
			dect_mm_rx(&mm, buf, n, rand_f, rs);
		} else {
			uint8_t r[4];

			memcpy(r, res, 4);
			if (k == 1)
				r[0] ^= 0x01u;
			n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_MM,
					       DECT_TI_RESPONDER, 0,
					       DECT_MM_AUTHENTICATION_REPLY);
			n = dect_nwk_put_ie(buf, sizeof(buf), n, DECT_IE_RES,
					    r, 4);
			dect_mm_rx(&mm, buf, n, rand_f, rs);
		}
		n = dect_mm_poll(&mm, out, sizeof(out));
		CHECK_EQ(dect_nwk_parse(out, n, &h, &ies, &ies_len), 0);
		CHECK_EQ(h.ti_flag, DECT_TI_RESPONDER);
		if (!k) {
			CHECK_EQ(h.msg_type,
				 DECT_MM_ACCESS_RIGHTS_TERMINATE_ACCEPT);
			CHECK_EQ(mm.term_by_pt, 1u);
		} else {
			CHECK_EQ(h.msg_type,
				 DECT_MM_ACCESS_RIGHTS_TERMINATE_REJECT);
			CHECK_EQ(out[4], DECT_MM_REJECT_AUTH_FAILED);
			CHECK_EQ(mm.term_by_pt, 0u);
			CHECK_EQ(mm.stats.term_refused, 1u);
		}
		CHECK_EQ(mm.pt_auth, 0u);
		CHECK_EQ(dect_mm_next_deadline(&mm), UINT64_MAX);
	}
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
	n = pp_access_request(buf, sizeof(buf), 0);
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
	n = pp_auth_request(buf, sizeof(buf), 0, "1234", rand_f, rs, rand_p, 0);
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
	 * Authenticated, so the identity the request asked for is now the one
	 * we hold, and the accept carries it back.
	 */
	{
		static const uint8_t asked[7] = { 0x80, 0xB4, 0x00, 0x11, 0x22,
						  0x33, 0x44 };	/* pp_access_request() */
		const uint8_t *pid;
		size_t pid_len;

		CHECK_EQ(mm.port_id_len, sizeof(asked));
		CHECK(memcmp(mm.port_id, asked, sizeof(asked)) == 0);
		pid = dect_nwk_find_ie(ies, ies_len, DECT_IE_PORTABLE_IDENTITY,
				       &pid_len);
		CHECK(pid != NULL && pid_len == sizeof(asked) &&
		      memcmp(pid, asked, sizeof(asked)) == 0);
	}

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
	n = pp_access_request(buf, sizeof(buf), 0);
	dect_mm_rx(&mm, buf, n, rand_f, rs16);
	dect_mm_poll(&mm, out, sizeof(out));	/* KEY-ALLOCATE */

	/* PP computes RES1 from the wrong PIN "9999". */
	n = pp_auth_request(buf, sizeof(buf), 0, "9999", rand_f, rs, rand_p, 0);
	dect_mm_rx(&mm, buf, n, rand_f, rs16);
	n = dect_mm_poll(&mm, out, sizeof(out));	/* §13.6 / GAP 8.32.2.4 */
	CHECK_EQ(msg_type_of(out, n), DECT_MM_AUTHENTICATION_REJECT);
	CHECK_EQ(out[0] & 0x80u, 0x00u);		/* our transaction, F=0 */
	n = dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_MM_ACCESS_RIGHTS_REJECT);
	CHECK_EQ(out[2], DECT_IE_REJECT_REASON);
	CHECK_EQ(out[4], DECT_MM_REJECT_AUTH_FAILED);
	CHECK_EQ(mm.state, DECT_MM_FAILED);
	CHECK_EQ(mm.stats.auth_fail, 1);
}

/* An 8-digit PIN fills the AC and registers. */
static void test_register_8_digit_pin(void)
{
	struct dect_mm_cfg cfg = { .pin = "31415926", .authenticate = 1 };
	struct dect_mm mm;
	uint8_t rand_f[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	uint8_t rand_p[8] = { 9, 9, 9, 9, 9, 9, 9, 9 };
	uint8_t rs[8] = { 0 };
	uint8_t rs16[16] = { 0 };
	uint8_t buf[256], out[256];
	size_t n;

	dect_mm_init(&mm, &cfg);
	n = pp_access_request(buf, sizeof(buf), 0);
	dect_mm_rx(&mm, buf, n, rand_f, rs16);
	n = dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_MM_KEY_ALLOCATE);
	n = pp_auth_request(buf, sizeof(buf), 0, "31415926", rand_f, rs,
			    rand_p, 0);
	dect_mm_rx(&mm, buf, n, rand_f, rs16);
	CHECK_EQ(mm.stats.auth_ok, 1);
	CHECK_EQ(mm.subscribed, 1);
}

/*
 * No usable PIN, no registration: refused before KEY-ALLOCATE, so not even
 * "0000" (the old default for "") gets a challenge. Three digits and a
 * PIN filling cfg.pin without a terminator count as unusable too.
 */
static void test_register_without_a_usable_pin(void)
{
	static const char *const pins[] = { "", "123", "12a4", NULL };
	uint8_t rand_f[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	uint8_t rs16[16] = { 0 };
	uint8_t buf[256], out[256];
	unsigned i;
	size_t n;

	for (i = 0; i < 4u; i++) {
		struct dect_mm_cfg cfg = { .authenticate = 1 };
		struct dect_mm mm;

		if (pins[i])
			strcpy(cfg.pin, pins[i]);
		else
			memset(cfg.pin, '1', sizeof(cfg.pin));
		dect_mm_init(&mm, &cfg);
		n = pp_access_request(buf, sizeof(buf), 0);
		dect_mm_rx(&mm, buf, n, rand_f, rs16);
		n = dect_mm_poll(&mm, out, sizeof(out));
		CHECK_EQ(msg_type_of(out, n), DECT_MM_ACCESS_RIGHTS_REJECT);
		CHECK_EQ(out[4], DECT_MM_REJECT_IPUI_NOT_ACCEPTED);
		CHECK_EQ(mm.stats.key_alloc_sent, 0);
	}
}

/* After registration the PP runs location registration; we must LOCATE-ACCEPT. */
/*
 * LOCATE-ACCEPT names our own location area (class A: LAL 36, octet 0x64,
 * as stock, A-0127), whether the handset sent none or another one.
 */
static void test_locate_accept_names_our_area(void)
{
	struct dect_mm_cfg cfg = { .pin = "1234", .authenticate = 1 };
	static const uint8_t ours[] = { 0x80, 0xa8, 0x02, 0x11, 0x22, 0x33 };
	static const uint8_t other[1] = { 0x5f };
	struct dect_mm mm;
	struct dect_nwk_hdr h;
	uint8_t buf[64], out[64];
	const uint8_t *ies, *la;
	size_t n, ies_len, la_len, k;

	for (k = 0; k < 2; k++) {
		dect_mm_init(&mm, &cfg);		/* rfpi 0: class A */
		memcpy(mm.port_id, ours, sizeof(ours));
		mm.port_id_len = sizeof(ours);
		n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_MM,
				       DECT_TI_ORIGINATOR, 0,
				       DECT_MM_LOCATE_REQUEST);
		n = dect_nwk_put_ie(buf, sizeof(buf), n,
				    DECT_IE_PORTABLE_IDENTITY, ours,
				    sizeof(ours));
		if (k)
			n = dect_nwk_put_ie(buf, sizeof(buf), n,
					    DECT_IE_LOCATION_AREA, other,
					    sizeof(other));
		dect_mm_rx(&mm, buf, n, NULL, NULL);
		n = dect_mm_poll(&mm, out, sizeof(out));
		CHECK_EQ(msg_type_of(out, n), DECT_MM_LOCATE_ACCEPT);
		dect_nwk_parse(out, n, &h, &ies, &ies_len);
		la = dect_nwk_find_ie(ies, ies_len, DECT_IE_LOCATION_AREA,
				      &la_len);
		CHECK(la != NULL && la_len == 1u);
		if (la && la_len == 1u)
			CHECK_EQ(la[0], 0x64);
	}
}

static void test_locate(void)
{
	struct dect_mm_cfg cfg = { .pin = "1234", .authenticate = 1 };
	struct dect_mm mm;
	uint8_t la[1] = { 0x64 };	/* LOCATION-AREA, LAL=36 bits */
	uint8_t buf[64], out[64];
	struct dect_nwk_hdr h;
	const uint8_t *ies, *rx_la;
	size_t n, ies_len, rx_la_len;

	static const uint8_t ours[] = { 0x80, 0xa8, 0x02, 0x11, 0x22, 0x33 };

	dect_mm_init(&mm, &cfg);
	memcpy(mm.port_id, ours, sizeof(ours));	/* a subscription we hold */
	mm.port_id_len = sizeof(ours);

	/*
	 * PP LOCATE-REQUEST (PP-initiated: F=0) carrying its identity, which
	 * is mandatory, and a LOCATION-AREA.
	 */
	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_MM, DECT_TI_ORIGINATOR,
			       0, DECT_MM_LOCATE_REQUEST);
	n = dect_nwk_put_ie(buf, sizeof(buf), n, DECT_IE_PORTABLE_IDENTITY,
			    ours, sizeof(ours));
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
 * <<TERMINAL-CAPABILITY>> as the three bench handsets send it in
 * {LOCATE-REQUEST} (bench logs 2026-10-28): the A540 is NG-DECT Part 3,
 * the C430A and AS690H Part 1 only.
 */
static void test_term_cap(void)
{
	static const uint8_t a540[] = { 0x25, 0x35, 0x0a, 0x00, 0x0c, 0x01,
		0x8e, 0x02, 0x00, 0x00, 0x00, 0x00, 0x40, 0x96, 0x82 };
	static const uint8_t c430a[] = { 0x25, 0x15, 0x0a, 0x00, 0x30, 0x03,
		0x90, 0x02, 0x00, 0x00, 0x00, 0x00, 0x40, 0x92, 0x82 };
	static const uint8_t as690h[] = { 0x25, 0x15, 0x0a, 0x00, 0x0c, 0x01,
		0x8e, 0x02, 0x00, 0x00, 0x00, 0x00, 0x40, 0x92, 0x82 };
	static const uint8_t open3[] = { 0x25, 0x35, 0x0a };	/* never ends */
	struct dect_term_cap tc;

	CHECK_EQ(dect_term_cap_parse(a540, sizeof(a540), &tc), 0);
	CHECK_EQ(tc.n_profile, 7u);
	CHECK_EQ(tc.profile[0] & 0x02u, 0x02u);		/* GAP */
	CHECK_EQ(tc.profile[6], 0x16u);
	CHECK_EQ(tc.ng_part1, 1);
	CHECK_EQ(tc.ng_part3, 1);
	CHECK_EQ(tc.display, 5u);
	CHECK_EQ(tc.tone, 2u);

	CHECK_EQ(dect_term_cap_parse(c430a, sizeof(c430a), &tc), 0);
	CHECK_EQ(tc.ng_part1, 1);
	CHECK_EQ(tc.ng_part3, 0);
	CHECK_EQ(dect_term_cap_parse(as690h, sizeof(as690h), &tc), 0);
	CHECK_EQ(tc.ng_part3, 0);

	CHECK_EQ(dect_term_cap_parse(open3, sizeof(open3), &tc), -1);
	CHECK_EQ(tc.present, 0);
	CHECK_EQ(dect_term_cap_parse(a540, 7, &tc), 0);	/* no PI group */
	CHECK_EQ(tc.n_profile, 0u);
	CHECK_EQ(tc.ng_part3, 0);
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
	{
		/* EN 300 444 table 43: UPC 1, cipher key number 8 */
		size_t al;
		const uint8_t *at = dect_nwk_find_ie(ies, ies_len,
						     DECT_IE_AUTH_TYPE, &al);

		CHECK(at != NULL);
		CHECK_EQ(al, 3u);
		CHECK_EQ(at[0], 0x01);		/* DSAA */
		CHECK_EQ(at[2] & 0x1Fu, 0x18u);	/* UPC + key 8 */
	}

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

	/* And a CIPHER-REQUEST is queued, naming the key just stored. */
	n = dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_MM_CIPHER_REQUEST);
	dect_nwk_parse(out, n, &h, &ies, &ies_len);
	{
		size_t cl;
		const uint8_t *ci = dect_nwk_find_ie(ies, ies_len,
						     DECT_IE_CIPHER_INFO, &cl);

		CHECK(ci != NULL && cl == 2u);
		if (ci && cl == 2u)
			CHECK_EQ(ci[1] & 0x0Fu, 0x08u);	/* the same key 8 */
	}
}


/*
 * Before ciphering: <MM_auth.1> fails an unanswered AUTHENTICATION-REQUEST
 * after 10 s, and the PT's AUTHENTICATION-REJECT fails it at once. After
 * the CIPHER-REQUEST, a CIPHER-REJECT is recorded and the call kept.
 */
static void test_cipher_timer_and_rejects(void)
{
	struct dect_mm_cfg cfg = { .authenticate = 1 };
	struct dect_mm mm;
	uint8_t uak[16] = { 0x10 }, rand_f[8] = { 0xaa }, rs[16] = { 0 };
	uint8_t buf[64], out[64], ks[16], dck[8], res[4];
	size_t n;

	dect_mm_init(&mm, &cfg);
	dect_mm_start_cipher(&mm, uak, rand_f, rs);
	(void)dect_mm_poll(&mm, out, sizeof(out));
	dect_mm_tick(&mm, 500);
	CHECK_EQ(dect_mm_next_deadline(&mm), 10500u);
	dect_mm_tick(&mm, 10499);
	CHECK_EQ(mm.cipher_state, DECT_MM_CIPHER_WAIT_REPLY);
	dect_mm_tick(&mm, 10500);
	CHECK_EQ(mm.cipher_state, DECT_MM_CIPHER_FAILED);
	CHECK_EQ(mm.stats.auth_timeouts, 1u);
	CHECK_EQ(dect_mm_next_deadline(&mm), UINT64_MAX);

	dect_mm_init(&mm, &cfg);
	dect_mm_start_cipher(&mm, uak, rand_f, rs);
	(void)dect_mm_poll(&mm, out, sizeof(out));
	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_MM, DECT_TI_RESPONDER,
			       0, DECT_MM_AUTHENTICATION_REJECT);
	dect_mm_rx(&mm, buf, n, NULL, NULL);
	CHECK_EQ(mm.cipher_state, DECT_MM_CIPHER_FAILED);
	CHECK_EQ(mm.stats.auth_timeouts, 0u);

	dect_mm_init(&mm, &cfg);
	dect_mm_start_cipher(&mm, uak, rand_f, rs);
	(void)dect_mm_poll(&mm, out, sizeof(out));
	dect_dsaa_a11(uak, rs, ks);
	dect_dsaa_a12(ks, rand_f, dck, res);
	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_MM, DECT_TI_RESPONDER,
			       0, DECT_MM_AUTHENTICATION_REPLY);
	n = dect_nwk_put_ie(buf, sizeof(buf), n, DECT_IE_RES, res, 4);
	dect_mm_rx(&mm, buf, n, rand_f, rs);
	CHECK_EQ(mm.cipher_state, DECT_MM_CIPHER_READY);
	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_MM, DECT_TI_RESPONDER,
			       0, DECT_MM_CIPHER_REJECT);
	dect_mm_rx(&mm, buf, n, NULL, NULL);
	CHECK_EQ(mm.cipher_rejected, 1u);
	CHECK_EQ(mm.stats.cipher_rejected, 1u);
	CHECK_EQ(mm.cipher_state, DECT_MM_CIPHER_READY);	/* call kept */
	dect_mm_tick(&mm, 1000);
	dect_mm_tick(&mm, 20000);
	CHECK_EQ(mm.stats.cipher_timeouts, 0u);		/* stopped by REJECT */

	/* a wrong RES is counted as the mismatch the owner releases on */
	dect_mm_init(&mm, &cfg);
	dect_mm_start_cipher(&mm, uak, rand_f, rs);
	(void)dect_mm_poll(&mm, out, sizeof(out));
	{
		uint8_t bad[4];

		memcpy(bad, res, 4);
		bad[3] ^= 0x80u;
		n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_MM,
				       DECT_TI_RESPONDER, 0,
				       DECT_MM_AUTHENTICATION_REPLY);
		n = dect_nwk_put_ie(buf, sizeof(buf), n, DECT_IE_RES, bad, 4);
		dect_mm_rx(&mm, buf, n, rand_f, rs);
	}
	CHECK_EQ(mm.cipher_state, DECT_MM_CIPHER_FAILED);
	CHECK_EQ(mm.stats.auth_mismatch, 1u);
	CHECK_EQ(dect_mm_poll(&mm, out, sizeof(out)), 0u);	/* no CIPHER */

	/* <MM_cipher.1>: 0x0209 in time stops it; without it, one timeout */
	{
		int k;

		for (k = 0; k < 2; k++) {
			dect_mm_init(&mm, &cfg);
			dect_mm_start_cipher(&mm, uak, rand_f, rs);
			(void)dect_mm_poll(&mm, out, sizeof(out));
			n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_MM,
					       DECT_TI_RESPONDER, 0,
					       DECT_MM_AUTHENTICATION_REPLY);
			n = dect_nwk_put_ie(buf, sizeof(buf), n, DECT_IE_RES,
					    res, 4);
			dect_mm_rx(&mm, buf, n, rand_f, rs);
			dect_mm_tick(&mm, 1000);		/* armed */
			CHECK_EQ(dect_mm_next_deadline(&mm), 11000u);
			if (!k)
				dect_mm_cipher_on(&mm);
			dect_mm_tick(&mm, 11000);
			dect_mm_tick(&mm, 30000);
			CHECK_EQ(mm.cipher_on, k ? 0u : 1u);
			CHECK_EQ(mm.stats.cipher_timeouts, k ? 1u : 0u);
			CHECK_EQ(mm.cipher_state, DECT_MM_CIPHER_READY);
			CHECK_EQ(dect_mm_next_deadline(&mm), UINT64_MAX);
		}
	}
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

/*
 * <<PORTABLE-IDENTITY>> is mandatory in {LOCATE-REQUEST} (EN 300 175-5 table
 * 56). Without it the identity check used to be skipped and the request
 * accepted; §17.6.4 prescribes {LOCATE-REJECT} "information element error".
 */
static void test_locate_request_without_identity_is_rejected(void)
{
	static const uint8_t ours[] = { 0x80, 0xa8, 0x02, 0x11, 0x22, 0x33 };
	uint8_t la[1] = { 0x64 };
	struct dect_mm_cfg cfg;
	struct dect_mm mm;
	uint8_t buf[64], out[64];
	size_t n;

	memset(&cfg, 0, sizeof(cfg));
	dect_mm_init(&mm, &cfg);
	memcpy(mm.port_id, ours, sizeof(ours));
	mm.port_id_len = sizeof(ours);

	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_MM, DECT_TI_ORIGINATOR,
			       0, DECT_MM_LOCATE_REQUEST);
	n = dect_nwk_put_ie(buf, sizeof(buf), n, DECT_IE_LOCATION_AREA, la,
			    sizeof(la));
	dect_mm_rx(&mm, buf, n, NULL, NULL);

	dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(out[1], DECT_MM_LOCATE_REJECT);
	CHECK_EQ(out[2], DECT_IE_REJECT_REASON);
	CHECK_EQ(out[4], DECT_MM_REJECT_IE_ERROR);
	CHECK_EQ(mm.stats.located, 0u);
}

/* With no subscription held at all, no IPUI is known: reject, not accept. */
static void test_locate_request_with_no_subscription_is_rejected(void)
{
	static const uint8_t someone[] = { 0x80, 0xa8, 0x02, 0x11, 0x22, 0x33 };
	struct dect_mm_cfg cfg;
	struct dect_mm mm;
	uint8_t buf[64], out[64];
	size_t n;

	memset(&cfg, 0, sizeof(cfg));
	dect_mm_init(&mm, &cfg);

	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_MM, DECT_TI_ORIGINATOR,
			       0, DECT_MM_LOCATE_REQUEST);
	n = dect_nwk_put_ie(buf, sizeof(buf), n, DECT_IE_PORTABLE_IDENTITY,
			    someone, sizeof(someone));
	dect_mm_rx(&mm, buf, n, NULL, NULL);

	dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(out[1], DECT_MM_LOCATE_REJECT);
	CHECK_EQ(out[4], DECT_MM_REJECT_IPUI_UNKNOWN);
	CHECK_EQ(mm.stats.located, 0u);
}

/*
 * A stranger who fails authentication must not leave its identity behind:
 * {ACCESS-RIGHTS-REQUEST} used to overwrite the identity we hold before the
 * PIN was checked, so the stranger's next {LOCATE-REQUEST} on that link was
 * accepted and ours would have been refused.
 */
static void test_failed_registration_keeps_the_held_identity(void)
{
	static const uint8_t ours[] = { 0x80, 0xa8, 0x02, 0x11, 0x22, 0x33 };
	static const uint8_t stranger[7] = { 0x80, 0xB4, 0x00, 0x11, 0x22,
					     0x33, 0x44 };	/* pp_access_request() */
	struct dect_mm_cfg cfg = { .pin = "1234", .authenticate = 1 };
	struct dect_mm mm;
	uint8_t buf[64], out[64];
	uint8_t rand_f[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	uint8_t rs[16] = { 8, 7, 6, 5, 4, 3, 2, 1 };	/* dect_mm_rx() reads 16 */
	uint8_t rand_p[8] = { 0 };
	size_t n;

	dect_mm_init(&mm, &cfg);
	memcpy(mm.port_id, ours, sizeof(ours));
	mm.port_id_len = sizeof(ours);

	n = pp_access_request(buf, sizeof(buf), 0);
	dect_mm_rx(&mm, buf, n, rand_f, rs);
	while (dect_mm_poll(&mm, out, sizeof(out)) > 0)
		;					/* KEY-ALLOCATE */
	n = pp_auth_request(buf, sizeof(buf), 0, "0000", rand_f, rs, rand_p, 0);
	dect_mm_rx(&mm, buf, n, rand_f, rs);		/* wrong PIN */
	n = dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_MM_AUTHENTICATION_REJECT);
	n = dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_MM_ACCESS_RIGHTS_REJECT);
	CHECK_EQ(mm.port_id_len, sizeof(ours));
	CHECK(memcmp(mm.port_id, ours, sizeof(ours)) == 0);

	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_MM, DECT_TI_ORIGINATOR,
			       0, DECT_MM_LOCATE_REQUEST);
	n = dect_nwk_put_ie(buf, sizeof(buf), n, DECT_IE_PORTABLE_IDENTITY,
			    stranger, sizeof(stranger));
	dect_mm_rx(&mm, buf, n, NULL, NULL);
	dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(out[1], DECT_MM_LOCATE_REJECT);
	CHECK_EQ(out[4], DECT_MM_REJECT_IPUI_UNKNOWN);
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
	static const uint8_t ours[] = { 0x80, 0xa8, 0x02, 0x11, 0x22, 0x33 };
	uint8_t la[1] = { 0x64 };
	uint8_t buf[64];
	size_t n;

	dect_mm_init(mm, &cfg);
	/* a subscription we hold, and the identity is mandatory in the request */
	memcpy(mm->port_id, ours, sizeof(ours));
	mm->port_id_len = sizeof(ours);
	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_MM,
			       DECT_TI_ORIGINATOR, 0, DECT_MM_LOCATE_REQUEST);
	n = dect_nwk_put_ie(buf, sizeof(buf), n, DECT_IE_PORTABLE_IDENTITY,
			    ours, sizeof(ours));
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

/* The PT refuses the TPUI: the assignment ends at once, as a failure. */
static void test_tpui_reject_ends_the_assignment(void)
{
	static const uint8_t rej[] = { 0x05, 0x5F };
	static const uint8_t ack[] = { 0x05, 0x5D };
	struct dect_mm mm;
	uint8_t out[128];

	mm_setup_located(&mm, out, sizeof(out));
	dect_mm_tick(&mm, 1000);
	dect_mm_rx(&mm, rej, sizeof(rej), NULL, NULL);
	CHECK_EQ(mm.stats.tpui_rejected, 1u);
	CHECK_EQ(mm.tpui_rejected, 1u);
	CHECK_EQ(dect_mm_tpui_confirmed(&mm), 0);
	CHECK_EQ(dect_mm_next_deadline(&mm), UINT64_MAX);
	dect_mm_tick(&mm, 11000);
	CHECK_EQ(mm.stats.tpui_timeouts, 0u);	/* stopped, not expired */
	dect_mm_rx(&mm, ack, sizeof(ack), NULL, NULL);
	CHECK_EQ(mm.stats.tpui_unsolicited, 1u);
	CHECK_EQ(dect_mm_tpui_confirmed(&mm), 0);
	dect_mm_rx(&mm, rej, sizeof(rej), NULL, NULL);	/* nothing pending */
	CHECK_EQ(mm.stats.tpui_rejected, 1u);
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

/* An ACK with no assignment behind it confirms nothing. */
static void test_an_unsolicited_ack_confirms_nothing(void)
{
	static const uint8_t ack[] = { 0x05, 0x5D };
	struct dect_mm_cfg cfg = { .pin = "1234", .rfpi = { 1, 2, 3, 4, 5 } };
	struct dect_mm mm;

	dect_mm_init(&mm, &cfg);
	dect_mm_rx(&mm, ack, sizeof(ack), NULL, NULL);
	CHECK_EQ(dect_mm_tpui_confirmed(&mm), 0);
	CHECK_EQ(mm.stats.tpui_acked, 0u);
	CHECK_EQ(mm.stats.tpui_unsolicited, 1u);
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

/*
 * The subscription slot drives BOTH the chip UAK slot and the assigned TPUI.
 *
 * This is what makes more than one handset possible at all. dect_mm_tpui()
 * derives {0x00, 0x11, slot}, and dect-stubd left mcfg.sub_slot at 0 for every
 * handset, so a second one took the first's UAK slot at
 * 0x9518 + (slot-1)*0x11 AND was handed the same TPUI 0x1101 -- after which a
 * page for 0x1101 could be answered by either handset and a per-handset result
 * meant nothing.
 */
static void test_sub_slot_drives_the_tpui(void)
{
	struct dect_mm_cfg cfg = { .pin = "1234", .rfpi = { 1, 2, 3, 4, 5 } };
	struct dect_mm mm;
	uint8_t id[3];
	unsigned slot;

	/* 0 means "the single-cell default", not "no slot". */
	cfg.sub_slot = 0;
	dect_mm_init(&mm, &cfg);
	CHECK_EQ(dect_mm_sub_slot(&mm), DECT_MM_REG_SLOT);
	dect_mm_tpui(&mm, id);
	CHECK_EQ(id[0], 0x00u);
	CHECK_EQ(id[1], 0x11u);
	CHECK_EQ(id[2], DECT_MM_REG_SLOT);

	/* Every valid slot gives a distinct TPUI -- the whole point. */
	for (slot = 1u; slot <= DECT_MM_MAX_SLOT; slot++) {
		cfg.sub_slot = (uint8_t)slot;
		dect_mm_init(&mm, &cfg);
		CHECK_EQ(dect_mm_sub_slot(&mm), slot);
		dect_mm_tpui(&mm, id);
		CHECK_EQ(id[2], slot);
	}

	/* Out of range falls back rather than indexing off the end. */
	cfg.sub_slot = DECT_MM_MAX_SLOT + 1u;
	dect_mm_init(&mm, &cfg);
	CHECK_EQ(dect_mm_sub_slot(&mm), DECT_MM_REG_SLOT);

	/* An explicitly configured TPUI still wins over the slot. */
	cfg.sub_slot = 3u;
	cfg.tpui[0] = 0x00u; cfg.tpui[1] = 0x22u; cfg.tpui[2] = 0x07u;
	dect_mm_init(&mm, &cfg);
	dect_mm_tpui(&mm, id);
	CHECK_EQ(id[1], 0x22u);
	CHECK_EQ(id[2], 0x07u);
}

/*
 * A base serves every handset subscribed to it. One held besides port_id is
 * accepted at location registration, and the link takes on that handset:
 * its identity, its slot and so its TPUI, and the owner's tag. On 2026-10-25
 * the A540 was turned away twenty times a minute by a daemon that knew only
 * the AS690H.
 */
static size_t locate_from(uint8_t *buf, size_t cap, const uint8_t *id,
			  size_t id_len)
{
	uint8_t la[1] = { 0x64 };
	size_t n = dect_nwk_build_hdr(buf, cap, DECT_PD_MM, DECT_TI_ORIGINATOR,
				      0, DECT_MM_LOCATE_REQUEST);

	n = dect_nwk_put_ie(buf, cap, n, DECT_IE_PORTABLE_IDENTITY, id, id_len);
	return dect_nwk_put_ie(buf, cap, n, DECT_IE_LOCATION_AREA, la,
			       sizeof(la));
}

static const uint8_t lookup_uak[16] = { 0x16, 0x18, 0x03, 0x39 };

static int lookup(void *ctx, const uint8_t *id, size_t len,
		  uint8_t uak[DECT_MM_UAK_LEN])
{
	(void)ctx;
	if (len < 1u || id[len - 1u] != 0x33u)	/* only "ours" has a key */
		return -1;
	memcpy(uak, lookup_uak, DECT_MM_UAK_LEN);
	return 0;
}

/*
 * cfg.auth_locate (annex H.2 example 2): a known handset's LOCATE-REQUEST
 * gets our AUTHENTICATION-REQUEST first; the right RES gives LOCATE-ACCEPT,
 * a wrong one or <MM_auth.1> LOCATE-REJECT "authentication failed"; its
 * repeat meanwhile is not answered twice. With the option off the accept
 * comes at once, as before; a held identity without a key is rejected
 * (test_locate_cannot_skip_the_challenge).
 */
static void test_location_registration_authenticated(void)
{
	static const uint8_t ours[] = { 0x80, 0xa8, 0x02, 0x11, 0x22, 0x33 };
	uint8_t rand_f[8] = { 7, 7, 7, 7, 1, 2, 3, 4 };
	uint8_t rs[16] = { 0x42, 0x24 };
	struct dect_mm_cfg cfg = { .pin = "1234", .rfpi = { 1, 2, 3, 4, 5 },
				   .sub_slot = 1, .auth_locate = 1 };
	struct dect_mm mm;
	uint8_t buf[64], out[64], ks[16], dck[8], res[4];
	size_t n;
	int k;

	dect_dsaa_a11(lookup_uak, rs, ks);
	dect_dsaa_a12(ks, rand_f, dck, res);
	for (k = 0; k < 4; k++) {	/* right, wrong, timeout, option off */
		cfg.auth_locate = k == 3 ? 0 : 1;
		dect_mm_init(&mm, &cfg);
		memcpy(mm.port_id, ours, sizeof(ours));
		mm.port_id_len = sizeof(ours);
		dect_mm_set_uak_lookup(&mm, lookup, NULL);
		n = locate_from(buf, sizeof(buf), ours, sizeof(ours));
		dect_mm_rx(&mm, buf, n, rand_f, rs);
		n = dect_mm_poll(&mm, out, sizeof(out));
		if (k == 3) {
			CHECK_EQ(msg_type_of(out, n), DECT_MM_LOCATE_ACCEPT);
			continue;
		}
		CHECK_EQ(msg_type_of(out, n), DECT_MM_AUTHENTICATION_REQUEST);
		n = locate_from(buf, sizeof(buf), ours, sizeof(ours));
		dect_mm_rx(&mm, buf, n, rand_f, rs);		/* its repeat */
		CHECK_EQ(dect_mm_poll(&mm, out, sizeof(out)), 0u);
		if (k == 2) {
			dect_mm_tick(&mm, 1000);
			dect_mm_tick(&mm, 11000);
		} else {
			uint8_t r[4];

			memcpy(r, res, 4);
			if (k == 1)
				r[1] ^= 0x10u;
			n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_MM,
					       DECT_TI_RESPONDER, 0,
					       DECT_MM_AUTHENTICATION_REPLY);
			n = dect_nwk_put_ie(buf, sizeof(buf), n, DECT_IE_RES,
					    r, 4);
			dect_mm_rx(&mm, buf, n, rand_f, rs);
		}
		n = dect_mm_poll(&mm, out, sizeof(out));
		if (!k) {
			CHECK_EQ(msg_type_of(out, n), DECT_MM_LOCATE_ACCEPT);
			CHECK_EQ(mm.stats.locate_auth_ok, 1u);
		} else {
			CHECK_EQ(msg_type_of(out, n), DECT_MM_LOCATE_REJECT);
			CHECK_EQ(mm.stats.locate_auth_failed, 1u);
		}
		CHECK_EQ(mm.pt_auth, 0u);
		/* only the matching RES proves the handset */
		CHECK_EQ(mm.authenticated, k == 0 ? 1u : 0u);
	}
}

/*
 * Security audit 2026-10-29, A2: a LOCATE-REQUEST that arrives while
 * another challenge of ours is in flight (here the one for its access-
 * rights termination) used to fall through to LOCATE-ACCEPT unchallenged;
 * and a held identity with no key was accepted without one. Now the first
 * is not answered (the handset repeats it) and the second is rejected.
 */
static void test_locate_cannot_skip_the_challenge(void)
{
	static const uint8_t ours[] = { 0x80, 0xa8, 0x02, 0x11, 0x22, 0x33 };
	static const uint8_t keyless[] = { 0x80, 0xa8, 0x02, 0x11, 0x22, 0x44 };
	uint8_t rand_f[8] = { 7, 7, 7, 7, 1, 2, 3, 4 };
	uint8_t rs[16] = { 0x42, 0x24 };
	struct dect_mm_cfg cfg = { .pin = "1234", .rfpi = { 1, 2, 3, 4, 5 },
				   .sub_slot = 1, .auth_locate = 1 };
	struct dect_mm mm;
	uint8_t buf[64], out[64];
	size_t n;

	/* a termination challenge in flight, then a LOCATE-REQUEST */
	dect_mm_init(&mm, &cfg);
	memcpy(mm.port_id, ours, sizeof(ours));
	mm.port_id_len = sizeof(ours);
	dect_mm_set_uak(&mm, lookup_uak);
	dect_mm_set_uak_lookup(&mm, lookup, NULL);
	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_MM,
			       DECT_TI_ORIGINATOR, 0,
			       DECT_MM_ACCESS_RIGHTS_TERMINATE_REQUEST);
	n = dect_nwk_put_ie(buf, sizeof(buf), n, DECT_IE_PORTABLE_IDENTITY,
			    ours, sizeof(ours));
	dect_mm_rx(&mm, buf, n, rand_f, rs);
	n = dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_MM_AUTHENTICATION_REQUEST);
	n = locate_from(buf, sizeof(buf), ours, sizeof(ours));
	dect_mm_rx(&mm, buf, n, rand_f, rs);
	CHECK_EQ(dect_mm_poll(&mm, out, sizeof(out)), 0u);
	CHECK_EQ(mm.authenticated, 0u);

	/* a held identity the owner has no key for */
	dect_mm_init(&mm, &cfg);
	memcpy(mm.port_id, keyless, sizeof(keyless));
	mm.port_id_len = sizeof(keyless);
	dect_mm_set_uak_lookup(&mm, lookup, NULL);
	n = locate_from(buf, sizeof(buf), keyless, sizeof(keyless));
	dect_mm_rx(&mm, buf, n, rand_f, rs);
	n = dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_MM_LOCATE_REJECT);
	CHECK_EQ(mm.stats.locate_auth_failed, 1u);
	CHECK_EQ(mm.authenticated, 0u);
}

static void test_a_held_handset_is_accepted_and_adopted(void)
{
	static const uint8_t ours[]  = { 0x80, 0xa8, 0x02, 0x11, 0x22, 0x33 };
	static const uint8_t other[] = { 0x80, 0xa8, 0x03, 0x44, 0x55, 0x66 };
	static const uint8_t tpui3[] = { 0x05, 0x05, 0xa0, 0x94, 0x00, 0x11,
					 0x03 };
	struct dect_mm_cfg cfg = { .pin = "1234", .rfpi = { 1, 2, 3, 4, 5 },
				   .sub_slot = 2 };
	struct dect_mm mm;
	uint8_t buf[64], out[64];
	size_t n;

	dect_mm_init(&mm, &cfg);
	CHECK_EQ(mm.adopted, -1);
	memcpy(mm.port_id, ours, sizeof(ours));
	mm.port_id_len = sizeof(ours);
	CHECK_EQ(dect_mm_hold(&mm, other, sizeof(other), 3, NULL, 7), 0);

	n = locate_from(buf, sizeof(buf), other, sizeof(other));
	dect_mm_rx(&mm, buf, n, NULL, NULL);
	n = dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_MM_LOCATE_ACCEPT);
	CHECK_EQ(mm.adopted, 7);
	CHECK_EQ(mm.port_id_len, sizeof(other));
	CHECK(memcmp(mm.port_id, other, sizeof(other)) == 0);
	CHECK_EQ(dect_mm_sub_slot(&mm), 3u);
	/* the TPUI handed out is the held handset's, from its slot */
	CHECK(n >= 2 + sizeof(tpui3) &&
	      memcmp(&out[2], tpui3, sizeof(tpui3)) == 0);
}

/* Holding handsets does not open the door to strangers. */
static void test_a_stranger_is_still_rejected_with_handsets_held(void)
{
	static const uint8_t ours[]     = { 0x80, 0xa8, 0x02, 0x11, 0x22, 0x33 };
	static const uint8_t other[]    = { 0x80, 0xa8, 0x03, 0x44, 0x55, 0x66 };
	static const uint8_t stranger[] = { 0x80, 0xa8, 0x03, 0x44, 0x55, 0x67 };
	struct dect_mm_cfg cfg;
	struct dect_mm mm;
	uint8_t buf[64], out[64];
	size_t n;

	memset(&cfg, 0, sizeof(cfg));
	dect_mm_init(&mm, &cfg);
	memcpy(mm.port_id, ours, sizeof(ours));
	mm.port_id_len = sizeof(ours);
	CHECK_EQ(dect_mm_hold(&mm, other, sizeof(other), 3, NULL, 1), 0);

	n = locate_from(buf, sizeof(buf), stranger, sizeof(stranger));
	dect_mm_rx(&mm, buf, n, NULL, NULL);
	dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(out[1], DECT_MM_LOCATE_REJECT);
	CHECK_EQ(out[4], DECT_MM_REJECT_IPUI_UNKNOWN);
	CHECK_EQ(mm.adopted, -1);
	CHECK(memcmp(mm.port_id, ours, sizeof(ours)) == 0);
}

/* A base may hold everyone and arm port_id with nobody. */
static void test_held_handsets_need_no_port_id(void)
{
	static const uint8_t other[] = { 0x80, 0xa8, 0x03, 0x44, 0x55, 0x66 };
	static const uint8_t pool[3] = { 0x00, 0x22, 0x09 };
	struct dect_mm_cfg cfg;
	struct dect_mm mm;
	uint8_t buf[64], out[64], id[3];
	size_t n;

	memset(&cfg, 0, sizeof(cfg));
	dect_mm_init(&mm, &cfg);
	CHECK_EQ(dect_mm_hold(&mm, other, sizeof(other), 5, pool, 4), 0);

	n = locate_from(buf, sizeof(buf), other, sizeof(other));
	dect_mm_rx(&mm, buf, n, NULL, NULL);
	n = dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_MM_LOCATE_ACCEPT);
	CHECK_EQ(mm.adopted, 4);
	/* an explicit TPUI in the held entry wins over its slot */
	dect_mm_tpui(&mm, id);
	CHECK(memcmp(id, pool, sizeof(pool)) == 0);
}

static void test_hold_rejects_what_it_cannot_serve(void)
{
	static const uint8_t id[] = { 0x80, 0xa8, 0x03, 0x44, 0x55, 0x66 };
	uint8_t big[DECT_MM_PORTID_MAX + 1];
	struct dect_mm_cfg cfg;
	struct dect_mm mm;
	unsigned i;

	memset(&cfg, 0, sizeof(cfg));
	memset(big, 0x80, sizeof(big));
	dect_mm_init(&mm, &cfg);
	CHECK_EQ(dect_mm_hold(&mm, id, sizeof(id), 0, NULL, 0), -1);
	CHECK_EQ(dect_mm_hold(&mm, id, sizeof(id), DECT_MM_MAX_SLOT + 1u, NULL,
			      0), -1);
	CHECK_EQ(dect_mm_hold(&mm, id, 0, 1, NULL, 0), -1);
	CHECK_EQ(dect_mm_hold(&mm, big, sizeof(big), 1, NULL, 0), -1);
	CHECK_EQ(mm.n_held, 0u);
	for (i = 0; i < DECT_MM_HELD_MAX; i++)
		CHECK_EQ(dect_mm_hold(&mm, id, sizeof(id), 1, NULL, (int)i), 0);
	CHECK_EQ(dect_mm_hold(&mm, id, sizeof(id), 1, NULL, 99), -1);
}

/*
 * Registration policy. A closed link refuses every {ACCESS-RIGHTS-REQUEST};
 * a "same" link lets only the handset it holds re-enrol. Found by review on
 * 2026-10-25: side links ran the whole enrolment and the daemon never
 * stored the key, so the handset showed "registered" with a key the base
 * did not have.
 */
static void test_registration_policy(void)
{
	static const uint8_t held[7] = { 0x80, 0xB4, 0x00, 0x11, 0x22,
					 0x33, 0x44 };	/* pp_access_request() */
	static const uint8_t other[7] = { 0x80, 0xB4, 0x00, 0x11, 0x22,
					  0x33, 0x45 };
	struct dect_mm_cfg cfg = { .pin = "1234", .authenticate = 1 };
	uint8_t rand_f[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	uint8_t rs[16] = { 8, 7, 6, 5, 4, 3, 2, 1 };
	struct dect_mm mm;
	uint8_t buf[64], out[64];
	size_t n;

	cfg.registration = DECT_MM_REG_CLOSED;
	dect_mm_init(&mm, &cfg);
	n = pp_access_request(buf, sizeof(buf), 0);
	dect_mm_rx(&mm, buf, n, rand_f, rs);
	n = dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_MM_ACCESS_RIGHTS_REJECT);
	CHECK_EQ(out[4], DECT_MM_REJECT_IPUI_NOT_ACCEPTED);
	CHECK_EQ(mm.stats.key_alloc_sent, 0u);

	/* "same": a different handset is refused ... */
	cfg.registration = DECT_MM_REG_SAME;
	dect_mm_init(&mm, &cfg);
	memcpy(mm.port_id, other, sizeof(other));
	mm.port_id_len = sizeof(other);
	n = pp_access_request(buf, sizeof(buf), 0);
	dect_mm_rx(&mm, buf, n, rand_f, rs);
	n = dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_MM_ACCESS_RIGHTS_REJECT);

	/* ... and the one it holds goes on to key allocation */
	dect_mm_init(&mm, &cfg);
	memcpy(mm.port_id, held, sizeof(held));
	mm.port_id_len = sizeof(held);
	n = pp_access_request(buf, sizeof(buf), 0);
	dect_mm_rx(&mm, buf, n, rand_f, rs);
	n = dect_mm_poll(&mm, out, sizeof(out));
	CHECK(msg_type_of(out, n) != DECT_MM_ACCESS_RIGHTS_REJECT);
	CHECK_EQ(mm.stats.key_alloc_sent, 1u);

	/* "same" with nothing held: nobody */
	dect_mm_init(&mm, &cfg);
	n = pp_access_request(buf, sizeof(buf), 0);
	dect_mm_rx(&mm, buf, n, rand_f, rs);
	n = dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_MM_ACCESS_RIGHTS_REJECT);
}

/*
 * A roamer: an unknown {LOCATE-REQUEST} is held while the cell fetches the
 * record (multi-cell test M4). Its repeat is counted, not answered; once
 * the record is held, resume() accepts on the handset's own transaction
 * with that handset's TPUI; without it, resume() rejects.
 */
static size_t locate_ti(uint8_t *buf, size_t cap, uint8_t ti,
			const uint8_t *id, size_t id_len)
{
	uint8_t la[1] = { 0x64 };
	size_t n = dect_nwk_build_hdr(buf, cap, DECT_PD_MM, DECT_TI_ORIGINATOR,
				      ti, DECT_MM_LOCATE_REQUEST);

	n = dect_nwk_put_ie(buf, cap, n, DECT_IE_PORTABLE_IDENTITY, id, id_len);
	return dect_nwk_put_ie(buf, cap, n, DECT_IE_LOCATION_AREA, la,
			       sizeof(la));
}

static void test_deferred_locate_for_a_roamer(void)
{
	static const uint8_t roamer[] = { 0x80, 0xa8, 0x03, 0x77, 0x66, 0x55 };
	static const uint8_t tpui4[] = { 0x05, 0x05, 0xa0, 0x94, 0x00, 0x11,
					 0x04 };
	struct dect_mm_cfg cfg = { .pin = "1234", .rfpi = { 1, 2, 3, 4, 5 },
				   .defer_unknown = 1 };
	struct dect_mm mm;
	uint8_t buf[64], out[64];
	const uint8_t *id;
	size_t n, id_len;

	dect_mm_init(&mm, &cfg);
	n = locate_ti(buf, sizeof(buf), 0, roamer, sizeof(roamer));
	dect_mm_rx(&mm, buf, n, NULL, NULL);
	CHECK_EQ(dect_mm_poll(&mm, out, sizeof(out)), 0u);	/* held */
	CHECK_EQ(mm.stats.locate_deferred, 1u);
	CHECK_EQ(dect_mm_locate_pending(&mm, &id, &id_len), 1);
	CHECK(id_len == sizeof(roamer) && !memcmp(id, roamer, id_len));

	/* the PT repeats after 20 s: counted, still held */
	dect_mm_rx(&mm, buf, n, NULL, NULL);
	CHECK_EQ(dect_mm_poll(&mm, out, sizeof(out)), 0u);
	CHECK_EQ(mm.stats.locate_retrans, 1u);
	CHECK_EQ(mm.stats.locate_deferred, 1u);

	/* the controller's answer arrives */
	CHECK_EQ(dect_mm_hold(&mm, roamer, sizeof(roamer), 4, NULL, 9), 0);
	CHECK_EQ(dect_mm_locate_resume(&mm), 1);
	n = dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_MM_LOCATE_ACCEPT);
	CHECK_EQ(out[0], 0x85u);		/* F=1, TV=0, PD MM */
	CHECK(memcmp(&out[2], tpui4, sizeof(tpui4)) == 0);
	CHECK_EQ(mm.adopted, 9);
	CHECK_EQ(dect_mm_locate_pending(&mm, NULL, NULL), 0);
	CHECK_EQ(dect_mm_locate_resume(&mm), 0);	/* nothing left */

	/* no record: resume rejects */
	dect_mm_init(&mm, &cfg);
	n = locate_ti(buf, sizeof(buf), 0, roamer, sizeof(roamer));
	dect_mm_rx(&mm, buf, n, NULL, NULL);
	CHECK_EQ(dect_mm_locate_resume(&mm), -1);
	dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(out[1], DECT_MM_LOCATE_REJECT);
	CHECK_EQ(out[4], DECT_MM_REJECT_IPUI_UNKNOWN);
}

/* <<DURATION>> sits after <<LOCATION-AREA>> (table 54), only when set */
static void test_locate_validity_adds_a_duration(void)
{
	static const uint8_t ours[] = { 0x80, 0xa8, 0x02, 0x11, 0x22, 0x33 };
	static const uint8_t dur[] = { DECT_IE_DURATION, 0x02, 0x71, 0x03 };
	struct dect_mm_cfg cfg = { .pin = "1234", .rfpi = { 1, 2, 3, 4, 5 } };
	struct dect_mm mm;
	uint8_t buf[64], out[64];
	const uint8_t *la;
	size_t n, len;
	int pass;

	for (pass = 0; pass < 2; pass++) {
		cfg.locate_validity = pass ? 3 : 0;
		dect_mm_init(&mm, &cfg);
		memcpy(mm.port_id, ours, sizeof(ours));
		mm.port_id_len = sizeof(ours);
		n = locate_ti(buf, sizeof(buf), 0, ours, sizeof(ours));
		dect_mm_rx(&mm, buf, n, NULL, NULL);
		n = dect_mm_poll(&mm, out, sizeof(out));
		CHECK_EQ(msg_type_of(out, n), DECT_MM_LOCATE_ACCEPT);
		CHECK_EQ(dect_nwk_find_ie(&out[2], n - 2, DECT_IE_DURATION,
					  &len) != NULL, pass);
		la = dect_nwk_find_ie(&out[2], n - 2, DECT_IE_LOCATION_AREA,
				      &len);
		CHECK(la != NULL);
		if (pass && la)
			CHECK(memcmp(la + len, dur, sizeof(dur)) == 0);
	}
}

/* EN 300 175-6 annex C, its example: 00012 0000137 9 */
static void test_ipei_text(void)
{
	static const uint8_t example[] = { 0x80, 0xa8, 0x00, 0x00, 0xc0, 0x00, 0x89 };
	static const uint8_t handset[] = { 0x80, 0xa8, 0x02, 0x12, 0x34, 0x56, 0x78 };
	static const uint8_t put_o[] = { 0x80, 0xa8, 0x10, 0x00, 0xc0, 0x00, 0x89 };
	char t[DECT_IPEI_TEXT_LEN];

	CHECK(dect_ipei_text(example, sizeof(example), t, sizeof(t)) == 0);
	CHECK(strcmp(t, "00012 0000137 9") == 0);
	CHECK(dect_ipei_text(handset, sizeof(handset), t, sizeof(t)) == 0);
	CHECK(strcmp(t, "08483 0284280 0") == 0);
	CHECK(dect_ipei_text(put_o, sizeof(put_o), t, sizeof(t)) == -1);
	CHECK(dect_ipei_text(example, 6, t, sizeof(t)) == -1);
	CHECK(dect_ipei_text(example, sizeof(example), t, 8) == -1);
}

int main(void)
{
	test_ipei_text();
	test_mm_transaction_rules();
	test_access_request_without_identity_is_rejected();
	test_proposed_auth_type_is_kept();
	test_ft_authentication();
	test_terminate_access_rights();
	test_handset_deregisters_itself();
	test_location_registration_authenticated();
	test_locate_cannot_skip_the_challenge();
	test_key_allocation_fails_on_timeout_or_refusal();
	test_locate_request_from_a_stranger_is_rejected();
	test_locate_request_from_our_handset_is_accepted();
	test_a_held_handset_is_accepted_and_adopted();
	test_a_stranger_is_still_rejected_with_handsets_held();
	test_held_handsets_need_no_port_id();
	test_hold_rejects_what_it_cannot_serve();
	test_registration_policy();
	test_deferred_locate_for_a_roamer();
	test_locate_validity_adds_a_duration();
	test_locate_request_without_identity_is_rejected();
	test_locate_request_with_no_subscription_is_rejected();
	test_failed_registration_keeps_the_held_identity();
	test_sub_slot_drives_the_tpui();
	test_register_ok();
	test_register_bad_pin();
	test_register_8_digit_pin();
	test_register_without_a_usable_pin();
	test_locate();
	test_cipher();
	test_cipher_timer_and_rejects();
	test_term_cap();
	test_locate_accept_names_our_area();
	test_tpui_assignment_is_confirmed_by_the_ack();
	test_tpui_assignment_times_out_at_ten_seconds();
	test_a_late_ack_is_visible_as_late();
	test_tpui_reject_ends_the_assignment();
	test_no_assignment_means_no_timer();
	test_an_unsolicited_ack_confirms_nothing();
	return CHECK_DONE();
}

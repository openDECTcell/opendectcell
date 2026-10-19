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

int main(void)
{
	test_register_ok();
	test_register_bad_pin();
	test_locate();
	test_cipher();
	return CHECK_DONE();
}

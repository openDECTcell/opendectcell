// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for src/libopendectcell/dect_mm.c: the FP registration FSM, driven by a
 * simulated PP that computes its authentication response with the same DSAA.
 * No hardware.
 */
#include "check.h"
#include "../src/libopendectcell/dect_mm.h"
#include "../src/libopendectcell/dect_nwk.h"
#include "../src/libopendectcell/dect_dsaa.h"

#include <string.h>

/* Build a PP ACCESS-RIGHTS-REQUEST with a PORTABLE-IDENTITY (IPUI-N like). */
static size_t pp_access_request(uint8_t *out, size_t cap, uint8_t ti)
{
	uint8_t pid[7] = { 0x80, 0xB4, 0x00, 0x11, 0x22, 0x33, 0x44 };	/* type/len + 5 */
	size_t off = dect_nwk_build_hdr(out, cap, DECT_PD_MM, DECT_TI_ORIGINATOR,
					ti, DECT_MM_ACCESS_RIGHTS_REQUEST);

	return dect_nwk_put_ie(out, cap, off, DECT_IE_PORTABLE_IDENTITY, pid, sizeof(pid));
}

/* The PP's AUTHENTICATION-REPLY: RES1 computed from the PIN, RAND_F, RS. */
static size_t pp_auth_reply(uint8_t *out, size_t cap, uint8_t ti,
			    const char *pin, const uint8_t rand_f[8],
			    int corrupt)
{
	uint8_t ac[4], k[16], ks[16], dck[8], res[4];
	uint8_t rs[8] = { 0 };
	size_t off;

	dect_dsaa_pin_to_ac(pin, ac, sizeof(ac));
	dect_dsaa_b1(ac, sizeof(ac), k);
	dect_dsaa_a11(k, rs, ks);
	dect_dsaa_a12(ks, rand_f, dck, res);
	if (corrupt)
		res[0] ^= 0xff;

	off = dect_nwk_build_hdr(out, cap, DECT_PD_MM, DECT_TI_ORIGINATOR, ti,
				 DECT_MM_AUTHENTICATION_REPLY);
	return dect_nwk_put_ie(out, cap, off, DECT_IE_RES, res, sizeof(res));
}

static uint8_t msg_type_of(const uint8_t *msg, size_t len)
{
	struct dect_nwk_hdr h;
	const uint8_t *ies;
	size_t ies_len;

	if (dect_nwk_parse(msg, len, &h, &ies, &ies_len) < 0)
		return 0;
	return h.msg_type;
}

/* Full successful registration with authentication (correct PIN). */
static void test_register_ok(void)
{
	struct dect_mm_cfg cfg = { .pin = "1234", .authenticate = 1,
				   .rfpi = { 1, 2, 3, 4, 5 } };
	struct dect_mm mm;
	uint8_t rand_f[8] = { 0xde, 0xad, 0xbe, 0xef, 0x12, 0x34, 0x56, 0x78 };
	uint8_t buf[256], out[256];
	size_t n;

	dect_mm_init(&mm, &cfg);

	n = pp_access_request(buf, sizeof(buf), 2);
	dect_mm_rx(&mm, buf, n, rand_f);
	n = dect_mm_poll(&mm, out, sizeof(out));
	CHECK(n > 0);
	CHECK_EQ(msg_type_of(out, n), DECT_MM_AUTHENTICATION_REQUEST);
	CHECK_EQ(mm.state, DECT_MM_WAIT_AUTH_REPLY);

	n = pp_auth_reply(buf, sizeof(buf), 2, "1234", rand_f, 0);
	dect_mm_rx(&mm, buf, n, rand_f);
	n = dect_mm_poll(&mm, out, sizeof(out));
	CHECK(n > 0);
	CHECK_EQ(msg_type_of(out, n), DECT_MM_ACCESS_RIGHTS_ACCEPT);
	CHECK_EQ(mm.state, DECT_MM_REGISTERED);
	CHECK_EQ(mm.stats.auth_ok, 1);
	CHECK_EQ(mm.stats.accepted, 1);

	/* ACCESS-RIGHTS-ACCEPT echoes the PP identity and carries FIXED-IDENTITY. */
	{
		struct dect_nwk_hdr h;
		const uint8_t *ies, *p;
		size_t ies_len, l;

		dect_nwk_parse(out, n, &h, &ies, &ies_len);
		p = dect_nwk_find_ie(ies, ies_len, DECT_IE_PORTABLE_IDENTITY, &l);
		CHECK(p != NULL);
		p = dect_nwk_find_ie(ies, ies_len, DECT_IE_FIXED_IDENTITY, &l);
		CHECK(p != NULL);
		CHECK_EQ(l, 7);			/* 2 header + 5 RFPI bytes */
		CHECK_EQ(p[6], 5);		/* last RFPI byte */
	}
}

/* A wrong PIN (PP computes a different RES) is rejected. */
static void test_register_bad_pin(void)
{
	struct dect_mm_cfg cfg = { .pin = "1234", .authenticate = 1 };
	struct dect_mm mm;
	uint8_t rand_f[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	uint8_t buf[256], out[256];
	size_t n;

	dect_mm_init(&mm, &cfg);
	n = pp_access_request(buf, sizeof(buf), 1);
	dect_mm_rx(&mm, buf, n, rand_f);
	dect_mm_poll(&mm, out, sizeof(out));

	/* PP replies with RES from the wrong PIN "9999". */
	n = pp_auth_reply(buf, sizeof(buf), 1, "9999", rand_f, 0);
	dect_mm_rx(&mm, buf, n, rand_f);
	n = dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_MM_ACCESS_RIGHTS_REJECT);
	CHECK_EQ(mm.state, DECT_MM_FAILED);
	CHECK_EQ(mm.stats.auth_fail, 1);
}

/* With authentication disabled, access rights are granted immediately. */
static void test_register_no_auth(void)
{
	struct dect_mm_cfg cfg = { .authenticate = 0, .rfpi = { 9, 8, 7, 6, 5 } };
	struct dect_mm mm;
	uint8_t rand_f[8] = { 0 };
	uint8_t buf[256], out[256];
	size_t n;

	dect_mm_init(&mm, &cfg);
	n = pp_access_request(buf, sizeof(buf), 0);
	dect_mm_rx(&mm, buf, n, rand_f);
	n = dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(msg_type_of(out, n), DECT_MM_ACCESS_RIGHTS_ACCEPT);
	CHECK_EQ(mm.state, DECT_MM_REGISTERED);
}

int main(void)
{
	test_register_ok();
	test_register_bad_pin();
	test_register_no_auth();
	return CHECK_DONE();
}

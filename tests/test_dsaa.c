// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for src/libopendectcell/dect_dsaa.c against the published DSAA test vector
 * (Tews thesis §3.4.1) and the ETSI EN 300 175-7 Annex K key-allocation vectors.
 * No hardware.
 */
#include "check.h"
#include "../src/libopendectcell/dect_dsaa.h"

#include <string.h>

static void check_bytes(const char *what, const uint8_t *got,
			const uint8_t *want, size_t n)
{
	if (memcmp(got, want, n) != 0) {
		size_t i;

		check_failures++;
		fprintf(stderr, "%s mismatch:\n  got ", what);
		for (i = 0; i < n; i++)
			fprintf(stderr, "%02x", got[i]);
		fprintf(stderr, "\n  want ");
		for (i = 0; i < n; i++)
			fprintf(stderr, "%02x", want[i]);
		fprintf(stderr, "\n");
	}
	check_count++;
}

/* PIN "9124" -> AC ff ff 91 24, then B1 fills K = (ffff9124) x4. */
static void test_pin_ac_b1(void)
{
	uint8_t ac[4], k[16];
	uint8_t want_ac[4] = { 0xff, 0xff, 0x91, 0x24 };
	uint8_t want_k[16];
	int i;

	dect_dsaa_pin_to_ac("9124", ac, sizeof(ac));
	check_bytes("AC", ac, want_ac, 4);

	dect_dsaa_b1(ac, sizeof(ac), k);
	for (i = 0; i < 16; i++)
		want_k[i] = want_ac[i % 4];
	check_bytes("K", k, want_k, 16);
}

/* GAP's AC is 32 bits: 8 digits fill it; more, or a non-digit, refused */
static void test_pin_ac_limits(void)
{
	uint8_t ac[4];
	uint8_t want8[4] = { 0x12, 0x34, 0x56, 0x78 };
	uint8_t none[4] = { 0xff, 0xff, 0xff, 0xff };
	uint8_t want5[4] = { 0xff, 0xf1, 0x23, 0x45 };

	check_bytes("rc 8", (uint8_t[]){ (uint8_t)dect_dsaa_pin_to_ac(
		"12345678", ac, sizeof(ac)) }, (uint8_t[]){ 0 }, 1);
	check_bytes("AC 8", ac, want8, 4);
	dect_dsaa_pin_to_ac("12345", ac, sizeof(ac));
	check_bytes("AC 5", ac, want5, 4);
	check_bytes("rc 9", (uint8_t[]){ (uint8_t)dect_dsaa_pin_to_ac(
		"123456789", ac, sizeof(ac)) }, (uint8_t[]){ 0xff }, 1);
	check_bytes("AC 9", ac, none, 4);
	check_bytes("rc x", (uint8_t[]){ (uint8_t)dect_dsaa_pin_to_ac(
		"12a4", ac, sizeof(ac)) }, (uint8_t[]){ 0xff }, 1);
	check_bytes("AC x", ac, none, 4);
}

/*
 * Thesis §3.4.1: the UAK after step2 is 93638b457afd40fa585feb6030d572a2. In the
 * key-allocation flow the UAK is KS' = A21(K, RS) (= DSAA XOR 0xAA per byte), so
 * this validates the full DSAA core plus the A21 bit inversion.
 */
static void test_dsaa_core_vector(void)
{
	uint8_t ac[4], k[16], ks[16], rs[8] = { 0 };
	uint8_t want[16] = { 0x93, 0x63, 0x8b, 0x45, 0x7a, 0xfd, 0x40, 0xfa,
			     0x58, 0x5f, 0xeb, 0x60, 0x30, 0xd5, 0x72, 0xa2 };

	dect_dsaa_pin_to_ac("9124", ac, sizeof(ac));
	dect_dsaa_b1(ac, sizeof(ac), k);
	dect_dsaa_a21(k, rs, ks);
	check_bytes("UAK = A21 core", ks, want, 16);
}

/* ETSI EN 300 175-7 Annex K: the full key-allocation / authentication flow. */
static void test_annex_k(void)
{
	uint8_t ac[4], k[16], ks[16], ks_[16], dck[8], res[4];
	uint8_t rs[8] = { 0 };
	uint8_t rand1[8] = { 0xd0, 0x1f, 0xf9, 0xe2, 0x11, 0x68, 0x04, 0x21 };
	uint8_t rand2[8] = { 0xa0, 0x3f, 0xf2, 0xc5, 0x23, 0xd0, 0x08, 0x42 };
	uint8_t rand3[8] = { 0x9b, 0x11, 0x4f, 0x25, 0x54, 0x51, 0x88, 0x59 };
	uint8_t want_res1[4] = { 0xf2, 0x8c, 0xf9, 0x11 };
	uint8_t want_res2[4] = { 0xee, 0xba, 0x4d, 0xb9 };
	uint8_t want_res3[4] = { 0x96, 0x1b, 0x3a, 0x9f };
	uint8_t want_dck[8]  = { 0x85, 0x60, 0xdc, 0x32, 0x4e, 0xa4, 0x9f, 0x37 };

	dect_dsaa_pin_to_ac("9124", ac, sizeof(ac));
	dect_dsaa_b1(ac, sizeof(ac), k);

	/* FT authenticates the PT. */
	dect_dsaa_a11(k, rs, ks);
	dect_dsaa_a12(ks, rand1, dck, res);
	check_bytes("RES1", res, want_res1, 4);

	/* PT authenticates the FT; the UAK is KS' = A21(K, RS). */
	dect_dsaa_a21(k, rs, ks_);
	dect_dsaa_a22(ks_, rand2, res);
	check_bytes("RES2", res, want_res2, 4);

	/* DCK allocation using the UAK. */
	dect_dsaa_a11(ks_, rs, ks_);
	dect_dsaa_a12(ks_, rand3, dck, res);
	check_bytes("RES3", res, want_res3, 4);
	check_bytes("DCK", dck, want_dck, 8);
}

int main(void)
{
	test_pin_ac_b1();
	test_pin_ac_limits();
	test_dsaa_core_vector();
	test_annex_k();
	return CHECK_DONE();
}

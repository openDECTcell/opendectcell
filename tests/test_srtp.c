// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * dect_srtp.c against the RFCs' test vectors: RFC 3711 B.2 (AES-CM
 * keystream) and B.3 (key derivation), RFC 7714 16.1.1 (AEAD_AES_128_GCM
 * on a packet); then both profiles keyed from master keys, round trip,
 * replay, a forged tag, and the rollover counter across a sequence wrap.
 * Needs mbedTLS: make check-cell MBEDTLS=...
 */
#include "../src/cell/dect_srtp.c"

#include <stdio.h>

#include "check.h"

static size_t unhex(const char *h, uint8_t *out)
{
	size_t n = 0;
	unsigned v;

	while (*h) {
		if (*h == ' ') {
			h++;
			continue;
		}
		if (sscanf(h, "%2x", &v) != 1)
			break;
		out[n++] = (uint8_t)v;
		h += 2;
	}
	return n;
}

static int same(const uint8_t *a, const char *hex)
{
	uint8_t b[256];
	size_t n = unhex(hex, b);

	return memcmp(a, b, n) == 0;
}

static void rfc3711_b3(void)
{
	uint8_t mk[16], ms[14], out[20];

	unhex("E1F97A0D3E018BE0D64FA32C06DE4139", mk);
	unhex("0EC675AD498AFEEBB6960B3AABE6", ms);
	CHECK(prf(mk, ms, 14, 0x00, out, 16) == 0 &&
	      same(out, "C61E7A93744F39EE10734AFE3FF7A087"));
	CHECK(prf(mk, ms, 14, 0x02, out, 14) == 0 &&
	      same(out, "30CBBC08863D8C85D49DB34A9AE1"));
	CHECK(prf(mk, ms, 14, 0x01, out, 20) == 0 &&
	      same(out, "CEBE321F6FF7716B6FD4AB49AF256A156D38BAA4"));
}

static void rfc3711_b2(void)
{
	struct dect_srtp c;
	uint8_t key[16], pkt[64 + DECT_SRTP_OVERHEAD] = { 0x80 };

	memset(&c, 0, sizeof(c));
	mbedtls_aes_init(&c.aes);
	mbedtls_gcm_init(&c.gcm);
	c.profile = DECT_SRTP_AES_CM_SHA1_80;
	unhex("2B7E151628AED2A6ABF7158809CF4F3C", key);
	CHECK(mbedtls_aes_setkey_enc(&c.aes, key, 128) == 0);
	unhex("F0F1F2F3F4F5F6F7F8F9FAFBFCFD", c.salt);
	/* SEQ 0, SSRC 0, ROC 0: 48 zero octets come out as the keystream */
	CHECK(dect_srtp_protect(&c, pkt, 12 + 48, sizeof(pkt)) == 12 + 48 + 10);
	CHECK(same(pkt + 12, "E03EAD0935C95E80E166B16DD92B4EB4"
			     "D23513162B02D0F72A43A2FE4A5F97AB"
			     "41E95B3BB0A2E8DD477901E4FCA894C0"));
	dect_srtp_free(&c);
}

static void rfc7714_16_1_1(void)
{
	static const char *plain =
		"8040f17b 8041f8d3 5501a0b2 47616c6c 69612065 7374206f "
		"6d6e6973 20646976 69736120 696e2070 61727465 73207472 6573";
	static const char *wire =
		"8040f17b 8041f8d3 5501a0b2 f24de3a3 fb34de6c acba861c "
		"9d7e4bca be633bd5 0d294e6f 42a5f47a 51c7d19b 36de3adf "
		"8833899d 7f27beb1 6a9152cf 765ee439 0cce";
	struct dect_srtp tx, rx;
	uint8_t key[16], pkt[128], w[128];
	size_t n = unhex(plain, pkt), wn = unhex(wire, w);
	unsigned i;

	for (i = 0; i < 16u; i++)
		key[i] = (uint8_t)i;
	memset(&tx, 0, sizeof(tx));
	mbedtls_aes_init(&tx.aes);
	mbedtls_gcm_init(&tx.gcm);
	tx.profile = DECT_SRTP_AEAD_AES_128_GCM;
	CHECK(mbedtls_gcm_setkey(&tx.gcm, MBEDTLS_CIPHER_ID_AES, key, 128) == 0);
	memcpy(tx.salt, "Quid pro quo", 12);
	rx = tx;
	mbedtls_gcm_init(&rx.gcm);
	CHECK(mbedtls_gcm_setkey(&rx.gcm, MBEDTLS_CIPHER_ID_AES, key, 128) == 0);
	CHECK(dect_srtp_protect(&tx, pkt, n, sizeof(pkt)) == (long)wn);
	CHECK(memcmp(pkt, w, wn) == 0);
	CHECK(dect_srtp_unprotect(&rx, pkt, wn) == (long)n);
	unhex(plain, w);
	CHECK(memcmp(pkt, w, n) == 0);
	dect_srtp_free(&tx);
	dect_srtp_free(&rx);
}

/* Both profiles from master keys: round trip, replay, forgery, the ROC. */
static void round_trip(unsigned profile, size_t klen)
{
	struct dect_srtp tx, rx;
	uint8_t mk[30], pkt[200], copy[200];
	static const uint16_t seqs[] = { 65533, 65534, 65535, 0, 1, 2 };
	unsigned i;
	long n;

	for (i = 0; i < sizeof(mk); i++)
		mk[i] = (uint8_t)(0xA0u + i);
	CHECK(dect_srtp_init(&tx, profile, mk, klen) == 0);
	CHECK(dect_srtp_init(&rx, profile, mk, klen + 1u) == -1);
	CHECK(dect_srtp_init(&rx, profile, mk, klen) == 0);
	for (i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
		size_t len = 12u + 160u;
		unsigned j;

		memset(pkt, 0, sizeof(pkt));
		pkt[0] = 0x80;
		pkt[1] = 9;			/* G.722 */
		pkt[2] = (uint8_t)(seqs[i] >> 8);
		pkt[3] = (uint8_t)seqs[i];
		pkt[8] = 0x12;
		pkt[11] = 0x34;
		for (j = 12; j < len; j++)
			pkt[j] = (uint8_t)(j + i);
		memcpy(copy, pkt, len);
		n = dect_srtp_protect(&tx, pkt, len, sizeof(pkt));
		CHECK(n > (long)len && memcmp(pkt + 12, copy + 12, 160) != 0);
		if (i == 2u) {			/* a forged tag first */
			uint8_t bad[200];

			memcpy(bad, pkt, (size_t)n);
			bad[n - 1] ^= 1;
			CHECK(dect_srtp_unprotect(&rx, bad, (size_t)n) == -1);
		}
		CHECK(dect_srtp_unprotect(&rx, pkt, (size_t)n) == (long)len &&
		      memcmp(pkt, copy, len) == 0);
	}
	CHECK(tx.roc == 1u && rx.top == (1u << 16 | 2u));
	/* the last packet again: a replay */
	memcpy(copy, pkt, 12u + 160u);
	n = dect_srtp_protect(&tx, copy, 12u + 160u, sizeof(copy));
	memcpy(pkt, copy, (size_t)n);
	CHECK(dect_srtp_unprotect(&rx, pkt, (size_t)n) == -1);
	dect_srtp_free(&tx);
	dect_srtp_free(&rx);
}

int main(void)
{
	rfc3711_b3();
	rfc3711_b2();
	rfc7714_16_1_1();
	round_trip(DECT_SRTP_AES_CM_SHA1_80, 30);
	round_trip(DECT_SRTP_AEAD_AES_128_GCM, 28);
	CHECK(dect_rtp_header_len((const uint8_t *)"\x90\0\0\0\0\0\0\0\0\0\0\0"
				  "\0\0\0\1abcd", 20) == 20);
	CHECK(dect_rtp_header_len((const uint8_t *)"\x40", 1) == 0);
	return CHECK_DONE();
}

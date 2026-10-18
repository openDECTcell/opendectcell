// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DECT Standard Authentication Algorithm. See dect_dsaa.h.
 *
 * Structure (E. Tews, "DECT Security Analysis", ch. 3, algorithms 1-11;
 * ETSI EN 300 175-7): DSAA builds on a 64-bit SP-network block cipher
 * ("cassable") whose key schedule is a bit permutation parameterised by
 * (start, step). step1/step2 chain two cassable instances; DSAA runs step1 over
 * the middle 64 key bits and step2 over the outer 64. The S-box and mixing
 * layers below are the published DSAA constants.
 *
 * Bit/byte convention (Tews §3.3): byte 0 is the least significant byte and
 * bit 0 its least significant bit, so each 8-byte value loads little-endian
 * into a uint64 and rev() is a byte swap.
 */
#include "dect_dsaa.h"

#include <string.h>

/* DSAA S-box (Tews algorithm 11; identical across the public DSAA sources). */
static const uint8_t sbox[256] = {
	0xb0, 0x68, 0x6f, 0xf6, 0x7d, 0xe8, 0x16, 0x85,
	0x39, 0x7c, 0x7f, 0xde, 0x43, 0xf0, 0x59, 0xa9,
	0xfb, 0x80, 0x32, 0xae, 0x5f, 0x25, 0x8c, 0xf5,
	0x94, 0x6b, 0xd8, 0xea, 0x88, 0x98, 0xc2, 0x29,
	0xcf, 0x3a, 0x50, 0x96, 0x1c, 0x08, 0x95, 0xf4,
	0x82, 0x37, 0x0a, 0x56, 0x2c, 0xff, 0x4f, 0xc4,
	0x60, 0xa5, 0x83, 0x21, 0x30, 0xf8, 0xf3, 0x28,
	0xfa, 0x93, 0x49, 0x34, 0x42, 0x78, 0xbf, 0xfc,
	0x61, 0xc6, 0xf1, 0xa7, 0x1a, 0x53, 0x03, 0x4d,
	0x86, 0xd3, 0x04, 0x87, 0x7e, 0x8f, 0xa0, 0xb7,
	0x31, 0xb3, 0xe7, 0x0e, 0x2f, 0xcc, 0x69, 0xc3,
	0xc0, 0xd9, 0xc8, 0x13, 0xdc, 0x8b, 0x01, 0x52,
	0xc1, 0x48, 0xef, 0xaf, 0x73, 0xdd, 0x5c, 0x2e,
	0x19, 0x91, 0xdf, 0x22, 0xd5, 0x3d, 0x0d, 0xa3,
	0x58, 0x81, 0x3e, 0xfd, 0x62, 0x44, 0x24, 0x2d,
	0xb6, 0x8d, 0x5a, 0x05, 0x17, 0xbe, 0x27, 0x54,
	0x5d, 0x9d, 0xd6, 0xad, 0x6c, 0xed, 0x64, 0xce,
	0xf2, 0x72, 0x3f, 0xd4, 0x46, 0xa4, 0x10, 0xa2,
	0x3b, 0x89, 0x97, 0x4c, 0x6e, 0x74, 0x99, 0xe4,
	0xe3, 0xbb, 0xee, 0x70, 0x00, 0xbd, 0x65, 0x20,
	0x0f, 0x7a, 0xe9, 0x9e, 0x9b, 0xc7, 0xb5, 0x63,
	0xe6, 0xaa, 0xe1, 0x8a, 0xc5, 0x07, 0x06, 0x1e,
	0x5e, 0x1d, 0x35, 0x38, 0x77, 0x14, 0x11, 0xe2,
	0xb9, 0x84, 0x18, 0x9f, 0x2a, 0xcb, 0xda, 0xf7,
	0xa6, 0xb2, 0x66, 0x7b, 0xb1, 0x9c, 0x6d, 0x6a,
	0xf9, 0xfe, 0xca, 0xc9, 0xa8, 0x41, 0xbc, 0x79,
	0xdb, 0xb8, 0x67, 0xba, 0xac, 0x36, 0xab, 0x92,
	0x4b, 0xd7, 0xe5, 0x9a, 0x76, 0xcd, 0x15, 0x1f,
	0x4e, 0x4a, 0x57, 0x71, 0x1b, 0x55, 0x09, 0x51,
	0x33, 0x0c, 0xb4, 0x8e, 0x2b, 0xe0, 0xd0, 0x5b,
	0x47, 0x75, 0x45, 0x40, 0x02, 0xd1, 0x3c, 0xec,
	0x23, 0xeb, 0x0b, 0xd2, 0xa1, 0x90, 0x26, 0x12,
};

static uint64_t load64(const uint8_t b[8])
{
	uint64_t x = 0;
	unsigned i;

	for (i = 0; i < 8; i++)
		x |= (uint64_t)b[i] << (8u * i);
	return x;
}

static void store64(uint64_t x, uint8_t b[8])
{
	unsigned i;

	for (i = 0; i < 8; i++)
		b[i] = (uint8_t)(x >> (8u * i));
}

static uint64_t rev64(uint64_t x)
{
	uint8_t b[8], r[8];
	unsigned i;

	store64(x, b);
	for (i = 0; i < 8; i++)
		r[i] = b[7 - i];
	return load64(r);
}

/* Algorithm 10: byte-wise S-box substitution. */
static uint64_t sub64(uint64_t in)
{
	uint64_t out = 0;
	unsigned i;

	for (i = 0; i < 8; i++)
		out |= (uint64_t)sbox[(in >> (8u * i)) & 0xffu] << (8u * i);
	return out;
}

/* Algorithm 9: bit permutation; input bit i goes to output bit (start+i*step). */
static uint64_t bitperm(unsigned start, unsigned step, uint64_t in)
{
	uint64_t out = 0;
	unsigned i, pos = start;

	for (i = 0; i < 64; i++) {
		if ((in >> i) & 1u)
			out |= (uint64_t)1 << pos;
		pos = (pos + step) & 63u;
	}
	return out;
}

/* Byte b of a 64-bit value (0 = least significant). */
#define BYTE(x, n)	((uint8_t)((x) >> (8u * (n))))

/* Algorithms 6-8: the three Z256-linear diffusion layers. */
static uint64_t mix1(uint64_t in)
{
	uint8_t b[8], o[8];
	unsigned i;

	for (i = 0; i < 8; i++)
		b[i] = BYTE(in, i);
	o[0] = (uint8_t)(b[4] + 2 * b[0]);  o[4] = (uint8_t)(b[0] + 3 * b[4]);
	o[1] = (uint8_t)(b[5] + 2 * b[1]);  o[5] = (uint8_t)(b[1] + 3 * b[5]);
	o[2] = (uint8_t)(b[6] + 2 * b[2]);  o[6] = (uint8_t)(b[2] + 3 * b[6]);
	o[3] = (uint8_t)(b[7] + 2 * b[3]);  o[7] = (uint8_t)(b[3] + 3 * b[7]);
	return load64(o);
}

static uint64_t mix2(uint64_t in)
{
	uint8_t b[8], o[8];
	unsigned i;

	for (i = 0; i < 8; i++)
		b[i] = BYTE(in, i);
	o[0] = (uint8_t)(b[2] + 2 * b[0]);  o[2] = (uint8_t)(b[0] + 3 * b[2]);
	o[1] = (uint8_t)(b[3] + 2 * b[1]);  o[3] = (uint8_t)(b[1] + 3 * b[3]);
	o[4] = (uint8_t)(b[6] + 2 * b[4]);  o[6] = (uint8_t)(b[4] + 3 * b[6]);
	o[5] = (uint8_t)(b[7] + 2 * b[5]);  o[7] = (uint8_t)(b[5] + 3 * b[7]);
	return load64(o);
}

static uint64_t mix3(uint64_t in)
{
	uint8_t b[8], o[8];
	unsigned i;

	for (i = 0; i < 8; i++)
		b[i] = BYTE(in, i);
	o[0] = (uint8_t)(b[1] + 2 * b[0]);  o[1] = (uint8_t)(b[0] + 3 * b[1]);
	o[2] = (uint8_t)(b[3] + 2 * b[2]);  o[3] = (uint8_t)(b[2] + 3 * b[3]);
	o[4] = (uint8_t)(b[5] + 2 * b[4]);  o[5] = (uint8_t)(b[4] + 3 * b[5]);
	o[6] = (uint8_t)(b[7] + 2 * b[6]);  o[7] = (uint8_t)(b[6] + 3 * b[7]);
	return load64(o);
}

/* Algorithm 3: the cassable block cipher (2 rounds x 3 sub-rounds). */
static uint64_t cassable(unsigned start, unsigned step, uint64_t key, uint64_t m)
{
	uint64_t t = key, s = m;
	unsigned i;

	for (i = 0; i < 2; i++) {
		t = bitperm(start, step, t);  s = mix1(sub64(s ^ t));
		t = bitperm(start, step, t);  s = mix2(sub64(s ^ t));
		t = bitperm(start, step, t);  s = mix3(sub64(s ^ t));
	}
	return s;
}

/* Algorithm 2: step1 chains cassable(46,35) then cassable(25,47). */
static uint64_t step1(uint64_t rand, uint64_t key)
{
	uint64_t k = cassable(46, 35, rand, key);

	return cassable(25, 47, k, rand);
}

/* Algorithm 4: step2 chains cassable(60,27) then cassable(55,39). */
static uint64_t step2(uint64_t rand, uint64_t key)
{
	uint64_t k = cassable(60, 27, rand, key);

	return cassable(55, 39, k, rand);
}

void dect_dsaa(const uint8_t rand[8], const uint8_t key[16], uint8_t out[16])
{
	uint64_t r = load64(rand);
	uint64_t kmid = load64(&key[4]);		/* key[32..95] */
	uint8_t k2b[8];
	uint64_t t, b;
	uint8_t tb[8], bb[8];

	/* key2 = rev(key[96..127]) || rev(key[0..31]) = rev(key[12..15])||rev(key[0..3]) */
	k2b[0] = key[15]; k2b[1] = key[14]; k2b[2] = key[13]; k2b[3] = key[12];
	k2b[4] = key[3];  k2b[5] = key[2];  k2b[6] = key[1];  k2b[7] = key[0];

	t = step1(rev64(r), rev64(kmid));
	b = step2(t, load64(k2b));

	/* out = rev(b[32..63]) || rev(t) || rev(b[0..31]) */
	store64(t, tb);
	store64(b, bb);
	out[0] = bb[7]; out[1] = bb[6]; out[2] = bb[5]; out[3] = bb[4];
	out[4] = tb[7]; out[5] = tb[6]; out[6] = tb[5]; out[7] = tb[4];
	out[8] = tb[3]; out[9] = tb[2]; out[10] = tb[1]; out[11] = tb[0];
	out[12] = bb[3]; out[13] = bb[2]; out[14] = bb[1]; out[15] = bb[0];
}

void dect_dsaa_pin_to_ac(const char *pin, uint8_t *ac, size_t ac_len)
{
	size_t i, len = strlen(pin);

	memset(ac, 0xff, ac_len);
	for (i = 0; i < len; i++) {
		unsigned shift = (i & 1u) ? 4u : 0u;
		size_t idx = ac_len - i / 2 - 1;

		ac[idx] &= (uint8_t)~(0xfu << shift);
		ac[idx] |= (uint8_t)((unsigned)(pin[len - i - 1] - '0') << shift);
	}
}

void dect_dsaa_b1(const uint8_t *val, size_t len, uint8_t k[16])
{
	size_t i;

	for (i = 0; i < DECT_AUTH_KEY_LEN; i++)
		k[i] = val[i % len];
}

void dect_dsaa_b2(const uint8_t *uak, size_t uak_len,
		  const uint8_t *upi, size_t upi_len, uint8_t k[16])
{
	size_t i;

	for (i = 0; i < DECT_AUTH_KEY_LEN; i++)
		k[i] = (uint8_t)(uak[i % uak_len] ^ upi[i % upi_len]);
}

void dect_dsaa_a11(const uint8_t k[16], const uint8_t rs[8], uint8_t ks[16])
{
	dect_dsaa(rs, k, ks);
}

void dect_dsaa_a12(const uint8_t ks[16], const uint8_t rand_f[8],
		   uint8_t dck[8], uint8_t res1[4])
{
	uint8_t e[16];

	dect_dsaa(rand_f, ks, e);
	memcpy(dck, &e[4], DECT_CIPHER_KEY_LEN);
	memcpy(res1, &e[12], DECT_AUTH_RES_LEN);
}

void dect_dsaa_a21(const uint8_t k[16], const uint8_t rs[8], uint8_t ks[16])
{
	unsigned i;

	dect_dsaa_a11(k, rs, ks);
	for (i = 0; i < DECT_AUTH_KEY_LEN; i++)
		ks[i] ^= 0xaau;
}

void dect_dsaa_a22(const uint8_t ks[16], const uint8_t rand_p[8], uint8_t res2[4])
{
	uint8_t e[16];

	dect_dsaa(rand_p, ks, e);
	memcpy(res2, &e[12], DECT_AUTH_RES_LEN);
}

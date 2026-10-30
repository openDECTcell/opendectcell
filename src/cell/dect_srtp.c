// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SRTP, profiles AES_CM_128_HMAC_SHA1_80 and AEAD_AES_128_GCM (dect_srtp.h).
 * From RFC 3711 (§3.3, §4.1.1, §4.2, §4.3, Appendix A) and RFC 7714 (§8,
 * §11); checked against their test vectors in tests/test_srtp.c.
 */
#include <string.h>

#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>

#include "dect_srtp.h"

#define HMAC_TAG	10u	/* 80 bits */
#define GCM_TAG		16u

size_t dect_rtp_header_len(const uint8_t *pkt, size_t len)
{
	size_t h;

	if (len < 12u || (pkt[0] >> 6) != 2u)
		return 0;
	h = 12u + 4u * (pkt[0] & 0x0Fu);
	if (pkt[0] & 0x10u) {		/* X: 4 octets, then length words */
		if (len < h + 4u)
			return 0;
		h += 4u + 4u * ((size_t)pkt[h + 2] << 8 | pkt[h + 3]);
	}
	return h <= len ? h : 0;
}

/*
 * RFC 3711 §4.3.3: the AES-CM PRF, keystream from IV = x * 2^16, where x is
 * the master salt XOR (label || index DIV kdr) at octet 7; the rate is 0,
 * so the index part is zero. A 12-octet salt (GCM) sits left-aligned in
 * the 14 octets, the rest zero.
 */
static int prf(const uint8_t *mkey, const uint8_t *msalt, size_t slen,
	       uint8_t label, uint8_t *out, size_t n)
{
	mbedtls_aes_context aes;
	uint8_t iv[16] = { 0 }, block[16];
	size_t off = 0;
	int r;

	memcpy(iv, msalt, slen);
	iv[7] ^= label;
	memset(out, 0, n);
	mbedtls_aes_init(&aes);
	r = mbedtls_aes_setkey_enc(&aes, mkey, 128) ||
	    mbedtls_aes_crypt_ctr(&aes, n, &off, iv, block, out, out);
	mbedtls_aes_free(&aes);
	mbedtls_platform_zeroize(block, sizeof(block));
	return r ? -1 : 0;
}

int dect_srtp_init(struct dect_srtp *c, unsigned profile, const uint8_t *key,
		   size_t len)
{
	uint8_t k[16];
	size_t slen = profile == DECT_SRTP_AES_CM_SHA1_80 ? 14u : 12u;
	int r;

	memset(c, 0, sizeof(*c));
	mbedtls_aes_init(&c->aes);
	mbedtls_gcm_init(&c->gcm);
	if ((profile != DECT_SRTP_AES_CM_SHA1_80 &&
	     profile != DECT_SRTP_AEAD_AES_128_GCM) || len != 16u + slen)
		return -1;
	c->profile = profile;
	r = prf(key, key + 16, slen, 0x00, k, sizeof(k)) ||
	    prf(key, key + 16, slen, 0x02, c->salt, slen);
	if (!r && profile == DECT_SRTP_AES_CM_SHA1_80)
		r = prf(key, key + 16, slen, 0x01, c->auth, sizeof(c->auth)) ||
		    mbedtls_aes_setkey_enc(&c->aes, k, 128);
	else if (!r)
		r = mbedtls_gcm_setkey(&c->gcm, MBEDTLS_CIPHER_ID_AES, k, 128);
	mbedtls_platform_zeroize(k, sizeof(k));
	if (r) {
		dect_srtp_free(c);
		return -1;
	}
	return 0;
}

void dect_srtp_free(struct dect_srtp *c)
{
	mbedtls_aes_free(&c->aes);
	mbedtls_gcm_free(&c->gcm);
	mbedtls_platform_zeroize(c, sizeof(*c));
}

static uint32_t ssrc_of(const uint8_t *p)
{
	return (uint32_t)p[8] << 24 | (uint32_t)p[9] << 16 |
	       (uint32_t)p[10] << 8 | p[11];
}

/* §4.1.1: the counter block, (salt * 2^16) ^ (SSRC * 2^64) ^ (i * 2^16) */
static void cm_iv(const struct dect_srtp *c, uint32_t ssrc, uint64_t idx,
		  uint8_t iv[16])
{
	unsigned j;

	memset(iv, 0, 16);
	memcpy(iv, c->salt, 14);
	for (j = 0; j < 4u; j++)
		iv[4 + j] ^= (uint8_t)(ssrc >> (24u - 8u * j));
	for (j = 0; j < 6u; j++)
		iv[8 + j] ^= (uint8_t)(idx >> (40u - 8u * j));
}

/* RFC 7714 §8.1: (00 00 || SSRC || ROC || SEQ) ^ salt, 12 octets */
static void gcm_iv(const struct dect_srtp *c, uint32_t ssrc, uint64_t idx,
		   uint8_t iv[12])
{
	unsigned j;

	memset(iv, 0, 12);
	for (j = 0; j < 4u; j++)
		iv[2 + j] = (uint8_t)(ssrc >> (24u - 8u * j));
	for (j = 0; j < 6u; j++)
		iv[6 + j] = (uint8_t)(idx >> (40u - 8u * j));
	for (j = 0; j < 12u; j++)
		iv[j] ^= c->salt[j];
}

/* §4.2: HMAC-SHA1 over the packet and the ROC, cut to 80 bits */
static int hmac_tag(const struct dect_srtp *c, const uint8_t *pkt, size_t len,
		    uint32_t roc, uint8_t tag[20])
{
	const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
	mbedtls_md_context_t ctx;
	const uint8_t r[4] = { (uint8_t)(roc >> 24), (uint8_t)(roc >> 16),
			       (uint8_t)(roc >> 8), (uint8_t)roc };
	int e;

	mbedtls_md_init(&ctx);
	e = !md || mbedtls_md_setup(&ctx, md, 1) ||
	    mbedtls_md_hmac_starts(&ctx, c->auth, sizeof(c->auth)) ||
	    mbedtls_md_hmac_update(&ctx, pkt, len) ||
	    mbedtls_md_hmac_update(&ctx, r, sizeof(r)) ||
	    mbedtls_md_hmac_finish(&ctx, tag);
	mbedtls_md_free(&ctx);
	return e ? -1 : 0;
}

long dect_srtp_protect(struct dect_srtp *c, uint8_t *pkt, size_t len,
		       size_t cap)
{
	size_t h = dect_rtp_header_len(pkt, len);
	uint16_t seq;
	uint64_t idx;

	if (!h || !c->profile || len + DECT_SRTP_OVERHEAD > cap)
		return -1;
	seq = (uint16_t)(pkt[2] << 8 | pkt[3]);
	/* §3.3.1: the ROC counts the sender's own wraps of SEQ */
	if (c->started && seq < c->last_seq &&
	    (unsigned)(c->last_seq - seq) > 0x8000u)
		c->roc++;
	c->started = 1;
	c->last_seq = seq;
	idx = (uint64_t)c->roc << 16 | seq;
	if (c->profile == DECT_SRTP_AES_CM_SHA1_80) {
		uint8_t iv[16], ks[16], tag[20];
		size_t off = 0;

		cm_iv(c, ssrc_of(pkt), idx, iv);
		if (mbedtls_aes_crypt_ctr(&c->aes, len - h, &off, iv, ks,
					  pkt + h, pkt + h) ||
		    hmac_tag(c, pkt, len, c->roc, tag))
			return -1;
		memcpy(pkt + len, tag, HMAC_TAG);
		return (long)(len + HMAC_TAG);
	} else {
		uint8_t iv[12];

		gcm_iv(c, ssrc_of(pkt), idx, iv);
		if (mbedtls_gcm_crypt_and_tag(&c->gcm, MBEDTLS_GCM_ENCRYPT,
					      len - h, iv, sizeof(iv), pkt, h,
					      pkt + h, pkt + h, GCM_TAG,
					      pkt + len))
			return -1;
		return (long)(len + GCM_TAG);
	}
}

/* Appendix A: the index of SEQ nearest the highest one seen. */
static uint64_t guess_index(const struct dect_srtp *c, uint16_t seq)
{
	uint32_t roc = (uint32_t)(c->top >> 16);
	uint16_t s_l = (uint16_t)c->top;
	uint32_t v = roc;

	if (!c->started)
		return seq;
	if (s_l < 0x8000u) {
		if (seq > s_l && (unsigned)(seq - s_l) > 0x8000u && roc)
			v = roc - 1u;
	} else if ((unsigned)s_l - 0x8000u > seq) {
		v = roc + 1u;
	}
	return (uint64_t)v << 16 | seq;
}

static int ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
	uint8_t d = 0;
	size_t i;

	for (i = 0; i < n; i++)
		d |= a[i] ^ b[i];
	return d == 0;
}

long dect_srtp_unprotect(struct dect_srtp *c, uint8_t *pkt, size_t len)
{
	size_t h = dect_rtp_header_len(pkt, len);
	size_t tl = c->profile == DECT_SRTP_AES_CM_SHA1_80 ? HMAC_TAG : GCM_TAG;
	uint16_t seq;
	uint64_t idx;
	size_t n;

	if (!h || !c->profile || len < h + tl)
		return -1;
	n = len - tl;
	seq = (uint16_t)(pkt[2] << 8 | pkt[3]);
	idx = guess_index(c, seq);
	/* §3.3.2: a replay, or older than the window, before any crypto */
	if (c->started && idx <= c->top &&
	    (c->top - idx >= 64u || (c->window >> (c->top - idx)) & 1u))
		return -1;
	if (c->profile == DECT_SRTP_AES_CM_SHA1_80) {
		uint8_t iv[16], ks[16], tag[20];
		size_t off = 0;

		if (hmac_tag(c, pkt, n, (uint32_t)(idx >> 16), tag) ||
		    !ct_equal(tag, pkt + n, HMAC_TAG))
			return -1;
		cm_iv(c, ssrc_of(pkt), idx, iv);
		if (mbedtls_aes_crypt_ctr(&c->aes, n - h, &off, iv, ks,
					  pkt + h, pkt + h))
			return -1;
	} else {
		uint8_t iv[12];

		gcm_iv(c, ssrc_of(pkt), idx, iv);
		if (mbedtls_gcm_auth_decrypt(&c->gcm, n - h, iv, sizeof(iv),
					     pkt, h, pkt + n, GCM_TAG,
					     pkt + h, pkt + h))
			return -1;
	}
	/* authenticated: the window moves on */
	if (!c->started || idx > c->top) {
		c->window = c->started && idx - c->top < 64u ?
			    c->window << (idx - c->top) | 1u : 1u;
		c->top = idx;
		c->started = 1;
	} else {
		c->window |= (uint64_t)1u << (c->top - idx);
	}
	return (long)n;
}

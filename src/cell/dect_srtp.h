/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SRTP for the cell's media (opendectcell-controller docs/cell-protocol.md §8):
 * RTP packets protected as RFC 3711 and RFC 7714 say, for the two profiles
 * MEDIA_OPEN names. One context per direction, keyed with the master key
 * and salt the controller sent; session keys from the default AES-CM PRF.
 *
 *   1  AES_CM_128_HMAC_SHA1_80: master key 16 + salt 14 octets; AES-128
 *      counter mode, an 80-bit HMAC-SHA1 tag over the packet and the ROC
 *   2  AEAD_AES_128_GCM: master key 16 + salt 12 octets; AES-128-GCM with
 *      the header as associated data, a 128-bit tag (RFC 7714 §8, §11)
 *
 * No MKI, key derivation rate 0, no SRTCP (the cell link has no RTCP in
 * v1, §8.3). The receiver keeps the RFC 3711 Appendix A index estimate and
 * a 64-packet replay window.
 */
#ifndef DECT_SRTP_H
#define DECT_SRTP_H

#include <stddef.h>
#include <stdint.h>

#include <mbedtls/aes.h>
#include <mbedtls/gcm.h>

#define DECT_SRTP_AES_CM_SHA1_80	1u
#define DECT_SRTP_AEAD_AES_128_GCM	2u

/* octets the protection adds at most: the GCM tag */
#define DECT_SRTP_OVERHEAD	16u

struct dect_srtp {
	unsigned profile;
	mbedtls_aes_context aes;	/* profile 1: the session key */
	mbedtls_gcm_context gcm;	/* profile 2 */
	uint8_t salt[14];		/* the session salt (12 used by GCM) */
	uint8_t auth[20];		/* profile 1: the session auth key */
	/* sender: the rollover counter, bumped where the sequence wraps */
	uint32_t roc;
	uint16_t last_seq;
	int      started;
	/* receiver: the highest index authenticated, and those before it */
	uint64_t top;
	uint64_t window;
};

/*
 * Derive the session keys; key is master key || master salt, 30 octets for
 * profile 1, 28 for profile 2. 0, or -1 for a profile or length not ours.
 */
int dect_srtp_init(struct dect_srtp *c, unsigned profile, const uint8_t *key,
		   size_t len);
void dect_srtp_free(struct dect_srtp *c);

/*
 * Protect the RTP packet pkt[0..len) in place; cap is the buffer's size.
 * The new length, or -1 (not RTP, or no room for the tag).
 */
long dect_srtp_protect(struct dect_srtp *c, uint8_t *pkt, size_t len,
		       size_t cap);

/*
 * Check and decrypt an SRTP packet in place: the RTP packet's length, or -1
 * if it fails authentication, is a replay, or is too old for the window.
 */
long dect_srtp_unprotect(struct dect_srtp *c, uint8_t *pkt, size_t len);

/* An RTP header's length, CSRCs and extension included; 0 if not RTP. */
size_t dect_rtp_header_len(const uint8_t *pkt, size_t len);

#endif

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DECT Standard Authentication Algorithm (DSAA) and the A11/A12/A21/A22
 * authentication processes, for registering a GAP handset.
 *
 * DSAA was reverse-engineered and published (deDECTed.org; E. Tews, "DECT
 * Security Analysis", TU Darmstadt, ch. 3 and appendix B.1; Lucks et al.,
 * CT-RSA 2010). The process definitions and the Annex K test vectors are in
 * ETSI EN 300 175-7. This is an independent GPL-2.0-or-later implementation
 * written from those public descriptions (libdect, GPL-2.0-only, used only to
 * cross-check the Annex K vectors -- no code copied), so the AGPL controller can
 * link libopendectcell.
 *
 * All values are byte arrays in the order they are written in the standard
 * (the DSAA "least significant byte first" convention). No I/O.
 */
#ifndef OPENDECTCELL_DECT_DSAA_H
#define OPENDECTCELL_DECT_DSAA_H

#include <stddef.h>
#include <stdint.h>

#define DECT_AUTH_KEY_LEN	16u	/* K / KS / UAK */
#define DECT_AUTH_RAND_LEN	8u	/* RAND / RS */
#define DECT_AUTH_RES_LEN	4u	/* RES1 / RES2 (XRES) */
#define DECT_CIPHER_KEY_LEN	8u	/* DCK */

/* PIN (decimal string) -> authentication code AC: BCD, left-padded with 0xF. */
/* -1 (AC all 0xff) for a non-digit or more digits than ac_len holds */
int dect_dsaa_pin_to_ac(const char *pin, uint8_t *ac, size_t ac_len);

/* B1: derive the 16-byte auth key K by repeating the UAK or AC. */
void dect_dsaa_b1(const uint8_t *val, size_t len, uint8_t k[DECT_AUTH_KEY_LEN]);

/* B2: derive K from a UAK and a UPI (K[i] = uak[i%] XOR upi[i%]). */
void dect_dsaa_b2(const uint8_t *uak, size_t uak_len,
		  const uint8_t *upi, size_t upi_len, uint8_t k[DECT_AUTH_KEY_LEN]);

/* The core 128-bit-key / 64-bit-rand -> 128-bit DSAA function. */
void dect_dsaa(const uint8_t rand[DECT_AUTH_RAND_LEN],
	       const uint8_t key[DECT_AUTH_KEY_LEN],
	       uint8_t out[DECT_AUTH_KEY_LEN]);

/* A11: session key KS from auth key K and random seed RS (KS = DSAA(RS, K)). */
void dect_dsaa_a11(const uint8_t k[DECT_AUTH_KEY_LEN],
		   const uint8_t rs[DECT_AUTH_RAND_LEN],
		   uint8_t ks[DECT_AUTH_KEY_LEN]);

/* A12: from KS and RAND_F produce DCK (middle 8 bytes) and RES1 (last 4). */
void dect_dsaa_a12(const uint8_t ks[DECT_AUTH_KEY_LEN],
		   const uint8_t rand_f[DECT_AUTH_RAND_LEN],
		   uint8_t dck[DECT_CIPHER_KEY_LEN], uint8_t res1[DECT_AUTH_RES_LEN]);

/* A21: like A11, then invert every second bit (XOR 0xAA per byte). */
void dect_dsaa_a21(const uint8_t k[DECT_AUTH_KEY_LEN],
		   const uint8_t rs[DECT_AUTH_RAND_LEN],
		   uint8_t ks[DECT_AUTH_KEY_LEN]);

/* A22: from KS' and RAND_P produce RES2 (last 4 bytes of DSAA output). */
void dect_dsaa_a22(const uint8_t ks[DECT_AUTH_KEY_LEN],
		   const uint8_t rand_p[DECT_AUTH_RAND_LEN],
		   uint8_t res2[DECT_AUTH_RES_LEN]);

#endif /* OPENDECTCELL_DECT_DSAA_H */

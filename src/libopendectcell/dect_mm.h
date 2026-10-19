/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * NWK Mobility Management: the fixed-part registration ("obtaining access
 * rights") procedure for a GAP handset.
 * Specification: ETSI EN 300 175-5 §13.2/§13.8, EN 300 444 (GAP).
 *
 * Flow driven here (FP-initiated authentication, as in libdect's fp-mm example):
 *   PP -> FP  ACCESS-RIGHTS-REQUEST   (PP's PORTABLE-IDENTITY)
 *   FP -> PP  AUTHENTICATION-REQUEST  (AUTH-TYPE=DSAA/AC, RAND_F, RS)
 *   PP -> FP  AUTHENTICATION-REPLY    (RES1)
 *   FP        verify RES1 == A12(A11(K,RS), RAND_F)  with K = B1(AC(PIN))
 *   FP -> PP  ACCESS-RIGHTS-ACCEPT    (PP PORTABLE-IDENTITY, FP FIXED-IDENTITY)
 *            or ACCESS-RIGHTS-REJECT on mismatch.
 *
 * Transport-agnostic: feed decoded NWK messages in, pull NWK messages out. The
 * caller carries them over the acknowledged C-plane (dect_lapc -> dect_lc ->
 * dect_cs). The 64-bit RAND_F is injected so the FSM is deterministic in tests.
 *
 * The authentication/crypto path is validated by unit tests. The FIXED-IDENTITY
 * (PARK) encoding built from the base RFPI is the main point to confirm against
 * real hardware (marked in the .c).
 */
#ifndef OPENDECTCELL_DECT_MM_H
#define OPENDECTCELL_DECT_MM_H

#include <stddef.h>
#include <stdint.h>

#include "dect_lapc.h"	/* DECT_LAPC_MSG_MAX */

#define DECT_MM_PIN_MAX		16u
#define DECT_MM_RFPI_LEN	5u
#define DECT_MM_PORTID_MAX	20u	/* saved PORTABLE-IDENTITY contents */
#define DECT_MM_UAK_LEN		16u	/* allocated long-term key (128-bit) */
#define DECT_MM_REG_SLOT	1u	/* chip subscription slot for the handset */

enum dect_mm_state {
	DECT_MM_IDLE,
	DECT_MM_WAIT_AUTH,	/* KEY-ALLOCATE sent, awaiting the PP's AUTH-REQUEST */
	DECT_MM_REGISTERED,
	DECT_MM_FAILED,
};

#define DECT_MM_TXQ	3u		/* pending outbound NWK messages */

struct dect_mm_cfg {
	char    pin[DECT_MM_PIN_MAX];		/* decimal PIN (AC); "" = default */
	uint8_t rfpi[DECT_MM_RFPI_LEN];		/* base identity for FIXED-IDENTITY */
	uint8_t authenticate;			/* 1 = challenge the PP (default) */
};

struct dect_mm_stats {
	uint32_t access_req;
	uint32_t key_alloc_sent;
	uint32_t auth_req;	/* PP AUTHENTICATION-REQUEST seen */
	uint32_t auth_ok;
	uint32_t auth_fail;
	uint32_t accepted;
	uint32_t rejected;
	uint32_t locate_req;	/* PP LOCATE-REQUEST seen */
	uint32_t located;	/* LOCATE-ACCEPT sent */
};

struct dect_mm {
	struct dect_mm_cfg cfg;
	enum dect_mm_state state;

	uint8_t ti_value;			/* transaction from the PP */
	uint8_t rand_f[8];			/* our challenge (RAND_F) */
	/*
	 * Random seed (RS). AVM's mode-2 KEY-ALLOCATE carries a 16-octet RS IE
	 * (0E 10) [CR A-0011 Q1], so the whole buffer goes on the wire. DSAA1
	 * authentication (A11/A21) consumes only the first 8 octets; RS is still
	 * all-zero here, so wire form and crypto input stay consistent.
	 */
	uint8_t rs[16];				/* random seed (RS) */
	uint8_t port_id[DECT_MM_PORTID_MAX];	/* PP PORTABLE-IDENTITY contents */
	size_t  port_id_len;

	/*
	 * Allocated long-term key (UAK). Key allocation derives it from the AC
	 * (PIN); it is the 16-byte credential the chip stores for the handset via
	 * 0x020A [CR A-0026]. Valid once `subscribed` is set (authentication OK).
	 */
	uint8_t uak[DECT_MM_UAK_LEN];
	uint8_t subscribed;			/* 1 = authenticated, uak valid */

	/* FIFO of outbound NWK messages (e.g. AUTH-REPLY then ACCESS-RIGHTS-ACCEPT). */
	uint8_t  out[DECT_MM_TXQ][DECT_LAPC_MSG_MAX];
	size_t   out_len[DECT_MM_TXQ];
	unsigned out_head, out_count;

	struct dect_mm_stats stats;
};

void dect_mm_init(struct dect_mm *mm, const struct dect_mm_cfg *cfg);

/*
 * Feed a decoded NWK message (the LAPC I-frame payload). rand_f supplies the
 * 64-bit challenge RAND_F and rs the 128-bit random seed RS used when a
 * KEY-ALLOCATE is produced (ignored otherwise); both must be freshly random on
 * hardware. Any responses are queued; pull them with dect_mm_poll().
 */
void dect_mm_rx(struct dect_mm *mm, const uint8_t *msg, size_t len,
		const uint8_t rand_f[8], const uint8_t rs[16]);

/* Pull a pending outbound NWK message. Returns its length, or 0 if none. */
size_t dect_mm_poll(struct dect_mm *mm, uint8_t *out, size_t cap);

#endif /* OPENDECTCELL_DECT_MM_H */

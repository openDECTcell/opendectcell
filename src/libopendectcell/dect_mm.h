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

/* <<REJECT-REASON>> codes, EN 300 175-5 §7.7.34 */
#define DECT_MM_REJECT_IPUI_UNKNOWN	0x02u

#define DECT_MM_PIN_MAX		16u
#define DECT_MM_RFPI_LEN	5u
#define DECT_MM_PORTID_MAX	20u	/* saved PORTABLE-IDENTITY contents */
#define DECT_MM_UAK_LEN		16u	/* allocated long-term key (128-bit) */
#define DECT_MM_REG_SLOT	1u	/* chip subscription slot for the handset */
/*
 * Hard ceiling on the 0x020A slot number, not a configuration limit
 * [CR A-SubscriberCapacity-7]. Credential records sit at a 17-byte stride from
 * a fixed table base, indexed one-based:
 *
 *     record(slot) = table_base + 17 * (slot - 1)
 *
 * The address reached by slot 7 coincides with the start of the separate
 * radio-maintained table the 0xF605 subsystem uses, and the analysed writer
 * does not range-check the index itself. Slot 0 takes a different command path.
 * Never emit 0x020A with a slot outside 1..6, not even as an experiment.
 */
#define DECT_MM_MAX_SLOT	6u

#define DECT_MM_DCK_LEN		8u	/* derived cipher key (64-bit) */

enum dect_mm_state {
	DECT_MM_IDLE,
	DECT_MM_WAIT_AUTH,	/* KEY-ALLOCATE sent, awaiting the PP's AUTH-REQUEST */
	DECT_MM_REGISTERED,
	DECT_MM_FAILED,
};

enum dect_mm_cipher_state {
	DECT_MM_CIPHER_NONE,
	DECT_MM_CIPHER_WAIT_REPLY,	/* AUTHENTICATION-REQUEST sent to the PP */
	DECT_MM_CIPHER_READY,		/* DCK derived, CIPHER-REQUEST sent */
	DECT_MM_CIPHER_FAILED,
};

#define DECT_MM_TXQ	3u		/* pending outbound NWK messages */

struct dect_mm_cfg {
	char    pin[DECT_MM_PIN_MAX];		/* decimal PIN (AC); "" = default */
	uint8_t rfpi[DECT_MM_RFPI_LEN];		/* base identity for FIXED-IDENTITY */
	uint8_t authenticate;			/* 1 = challenge the PP (default) */
	/*
	 * Chip subscription slot for this handset, 1..DECT_MM_MAX_SLOT.
	 * 0 selects DECT_MM_REG_SLOT, which is what a single-cell base wants.
	 */
	uint8_t sub_slot;
	/*
	 * The 20-bit TPUI we assign in LOCATE-ACCEPT, as its three content
	 * octets. All-zero derives {0x00, 0x11, sub_slot}, the historical
	 * behaviour.
	 *
	 * Deriving the TPUI from the slot couples a handset's identity to a
	 * chip index, and a TPUI is unique within a LOCATION AREA, not within
	 * one RFP (EN 300 175-5): on a multi-cell FP, slot indices repeat, so
	 * slot-derived TPUIs collide and paging reaches the wrong handset.
	 * Assign them from an installation-wide pool and keep the slot private
	 * to the cell. See openDECTcell issues #65 and #76.
	 */
	uint8_t tpui[3];
	/*
	 * 1 = advertise only the narrowband codec in <<CODEC-LIST>>.
	 *
	 * The default list offers G.722 (codec identifier 3) on a LONG SLOT
	 * (j=640) first, then G.726 (identifier 2) on a full slot. A long slot
	 * is the larger allocation, and a PP that cannot get one releases with
	 * RELEASE-REASON 0x32 "insufficient resources" -- which is exactly how
	 * the Gigaset A540 refuses our FP-originated calls, while a FRITZ!Fon
	 * C4 on the same base rings. Offering the full-slot codec alone tests
	 * whether the long slot is what it cannot allocate.
	 *
	 * `[not established]` that this is the cause. See
	 * docs/tasks-2026-10-22.md T3.
	 */
	uint8_t narrowband_only;
};

struct dect_mm_stats {
	uint32_t access_req;
	uint32_t key_alloc_sent;
	uint32_t auth_req;	/* PP AUTHENTICATION-REQUEST seen */
	uint32_t auth_ok;
	uint32_t auth_fail;
	uint32_t accepted;
	uint32_t rejected;
	uint32_t locate_req;
	uint32_t locate_rej;	/* rejected: not the handset we hold */	/* PP LOCATE-REQUEST seen */
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

	/*
	 * Call-time ciphering: the FP authenticates the PP with the stored UAK to
	 * derive the DCK, then starts DSC. Separate from the registration FSM so it
	 * can run on a later (call) connection. rand_f/rs above carry the challenge.
	 */
	enum dect_mm_cipher_state cipher_state;
	uint8_t cipher_uak[DECT_MM_UAK_LEN];	/* key for the call-time auth */
	uint8_t dck[DECT_MM_DCK_LEN];		/* derived cipher key (valid when READY) */
	uint8_t cipher_res[4];			/* the PP's verified RES (valid when READY) */

	/* FIFO of outbound NWK messages (e.g. AUTH-REPLY then ACCESS-RIGHTS-ACCEPT). */
	uint8_t  out[DECT_MM_TXQ][DECT_LAPC_MSG_MAX];
	size_t   out_len[DECT_MM_TXQ];
	unsigned out_head, out_count;

	struct dect_mm_stats stats;
};

void dect_mm_init(struct dect_mm *mm, const struct dect_mm_cfg *cfg);

/* The configured subscription slot, with the default already applied. */
uint8_t dect_mm_sub_slot(const struct dect_mm *mm);

/*
 * The three content octets of the TPUI we assign, with the default already
 * applied. Anything that pages this handset must use its low 16 bits, so take
 * them from here rather than rebuilding them from the slot.
 */
void dect_mm_tpui(const struct dect_mm *mm, uint8_t out[3]);

/*
 * Feed a decoded NWK message (the LAPC I-frame payload). rand_f supplies the
 * 64-bit challenge RAND_F and rs the 128-bit random seed RS used when a
 * KEY-ALLOCATE is produced (ignored otherwise); both must be freshly random on
 * hardware. Any responses are queued; pull them with dect_mm_poll().
 */
void dect_mm_rx(struct dect_mm *mm, const uint8_t *msg, size_t len,
		const uint8_t rand_f[8], const uint8_t rs[16]);

/*
 * Start call-time ciphering (FP-initiated): authenticate the PP with the stored
 * UAK and a fresh RAND_F/RS, so both sides derive the DCK. Queues an
 * AUTHENTICATION-REQUEST; when the PP's AUTHENTICATION-REPLY arrives via
 * dect_mm_rx() and its RES verifies, the DCK is stored and a CIPHER-REQUEST is
 * queued. (The AUTH-TYPE and CIPHER-INFO IE contents are provisional pending
 * [CR Q-0015].)
 */
void dect_mm_start_cipher(struct dect_mm *mm, const uint8_t uak[DECT_MM_UAK_LEN],
			  const uint8_t rand_f[8], const uint8_t rs[16]);

/* When ciphering is READY, copy the 8-byte DCK out and return 1; else 0. */
int dect_mm_cipher_dck(const struct dect_mm *mm, uint8_t dck[DECT_MM_DCK_LEN]);

/* Pull a pending outbound NWK message. Returns its length, or 0 if none. */
size_t dect_mm_poll(struct dect_mm *mm, uint8_t *out, size_t cap);

#endif /* OPENDECTCELL_DECT_MM_H */

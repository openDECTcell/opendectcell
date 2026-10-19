// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * NWK Mobility Management registration FSM (key-allocation flow). See dect_mm.h.
 * ETSI EN 300 175-5 §13.3 (key allocation) / §13.2 (access rights); IE encodings
 * per §7.7.
 *
 * First-time GAP subscription uses key allocation so the PIN (AC) becomes a
 * persistent UAK:
 *   PP -> FP  ACCESS-RIGHTS-REQUEST  (PP PORTABLE-IDENTITY)
 *   FP -> PP  KEY-ALLOCATE          (ALLOCATION-TYPE=DSAA, RAND_F, RS)
 *   PP -> FP  AUTHENTICATION-REQUEST(RES1, RAND_P)   [PP prompts for the PIN here]
 *   FP        verify RES1 == A12(A11(K,RS),RAND_F), K = B1(AC(PIN))
 *   FP -> PP  AUTHENTICATION-REPLY  (RES2 = A22(A21(K,RS), RAND_P))
 *   FP -> PP  ACCESS-RIGHTS-ACCEPT  (PP PORTABLE-IDENTITY, FP FIXED-IDENTITY)
 *            or ACCESS-RIGHTS-REJECT on an RES1 mismatch.
 */
#include "dect_mm.h"
#include "dect_nwk.h"
#include "dect_dsaa.h"

#include <string.h>

/* ALLOCATION-TYPE / FIXED-IDENTITY constants (EN 300 175-5 §7.7). */
/*
 * ALLOCATION-TYPE payload for KEY-ALLOCATE. The first octet is the
 * authentication-algorithm identifier (0x01 = DSAA, 0x02 = DSAA2), the second a
 * packed (UAK number | authentication-code number).
 *
 * The C4 *proposes* DSAA2 (0x02) in its ACCESS-RIGHTS-REQUEST, but GAP mandates
 * DSAA1 support and the handset is documented as interoperable with third-party
 * (often DSAA1-only) bases. We select DSAA1 (0x01) here because our crypto
 * (dect_dsaa) implements it and is test-verified; if the C4 accepts a DSAA1
 * KEY-ALLOCATE it will compute RES1 with DSAA1 and registration can complete.
 * This matches AVM's shorter "non-mode-2" 26-byte KEY-ALLOCATE variant
 * (8-byte RS) [CR A-0011 Q1]. If the C4 refuses DSAA1, DSAA2 must be added.
 */
#define ALLOC_TYPE_ALG		0x01u	/* DSAA1 (0x02 = DSAA2, not yet implemented) */
#define ALLOC_TYPE_FLAGS	0x88u
#define KEY_ALLOC_RS_LEN	8u	/* DSAA1 RS = 64 bits (0E 08) */
#define FIXED_ID_TYPE_PARK	0x20u
#define OCTET_GROUP_END		0x80u
#define ARC_A_LEN		36u	/* class-A ARI length in bits */
#define AC_LEN			4u

static void pin_ac(const struct dect_mm *mm, uint8_t ac[AC_LEN])
{
	const char *pin = mm->cfg.pin[0] ? mm->cfg.pin : "0000";

	dect_dsaa_pin_to_ac(pin, ac, AC_LEN);
}

void dect_mm_init(struct dect_mm *mm, const struct dect_mm_cfg *cfg)
{
	memset(mm, 0, sizeof(*mm));
	mm->cfg = *cfg;
	mm->cfg.authenticate = cfg->authenticate ? 1u : 0u;
	mm->state = DECT_MM_IDLE;
}

/* Reserve the next free TX slot and return a pointer to build into, or NULL. */
static uint8_t *txq_slot(struct dect_mm *mm, unsigned *idx)
{
	if (mm->out_count >= DECT_MM_TXQ)
		return NULL;
	*idx = (mm->out_head + mm->out_count) % DECT_MM_TXQ;
	return mm->out[*idx];
}

static void txq_commit(struct dect_mm *mm, unsigned idx, size_t len)
{
	mm->out_len[idx] = len;
	mm->out_count++;
}

/*
 * Build the NWK S-format header. ti_flag selects the transaction direction:
 * KEY-ALLOCATE and AUTHENTICATION-REPLY belong to the FT-initiated key-allocation
 * transaction, so the FP is the ORIGINATOR there (F=0, header 05 ..) [CR A-0022
 * "Critical correction", ETSI EN 300 175-5 §7.3 / EN 300 444 §8.45.9]. The
 * ACCESS-RIGHTS-ACCEPT/REJECT close the PP-initiated access-rights transaction,
 * so there the FP is the RESPONDER (F=1, header 85 ..).
 */
static size_t hdr(const struct dect_mm *mm, uint8_t *buf, uint8_t ti_flag,
		  uint8_t msg_type)
{
	return dect_nwk_build_hdr(buf, DECT_LAPC_MSG_MAX, DECT_PD_MM,
				  ti_flag, mm->ti_value, msg_type);
}

/* FP -> PP KEY-ALLOCATE: ALLOCATION-TYPE (DSAA), RAND (RAND_F), RS. */
static void send_key_allocate(struct dect_mm *mm)
{
	uint8_t alloc[2] = { ALLOC_TYPE_ALG, ALLOC_TYPE_FLAGS };
	unsigned idx;
	uint8_t *b = txq_slot(mm, &idx);
	size_t off;

	if (!b)
		return;
	off = hdr(mm, b, DECT_TI_ORIGINATOR, DECT_MM_KEY_ALLOCATE);
	off = dect_nwk_put_ie(b, DECT_LAPC_MSG_MAX, off, DECT_IE_ALLOCATION_TYPE,
			      alloc, sizeof(alloc));
	off = dect_nwk_put_ie(b, DECT_LAPC_MSG_MAX, off, DECT_IE_RAND,
			      mm->rand_f, sizeof(mm->rand_f));
	off = dect_nwk_put_ie(b, DECT_LAPC_MSG_MAX, off, DECT_IE_RS,
			      mm->rs, KEY_ALLOC_RS_LEN);
	txq_commit(mm, idx, off);
	mm->state = DECT_MM_WAIT_AUTH;
	mm->stats.key_alloc_sent++;
}

/* FP -> PP AUTHENTICATION-REPLY: RES2 (our response to the PP's challenge). */
static void send_auth_reply(struct dect_mm *mm, const uint8_t res2[4])
{
	unsigned idx;
	uint8_t *b = txq_slot(mm, &idx);
	size_t off;

	if (!b)
		return;
	off = hdr(mm, b, DECT_TI_ORIGINATOR, DECT_MM_AUTHENTICATION_REPLY);
	off = dect_nwk_put_ie(b, DECT_LAPC_MSG_MAX, off, DECT_IE_RES, res2, 4);
	txq_commit(mm, idx, off);
}

/*
 * FP -> PP ACCESS-RIGHTS-ACCEPT: echo the PP's PORTABLE-IDENTITY and add our
 * FIXED-IDENTITY (PARK) from the base RFPI. (The PARK encoding is the item still
 * to confirm against hardware.)
 */
static void send_access_accept(struct dect_mm *mm)
{
	uint8_t fixed_id[2 + DECT_MM_RFPI_LEN];
	unsigned idx;
	uint8_t *b = txq_slot(mm, &idx);
	size_t off;

	if (!b)
		return;
	off = hdr(mm, b, DECT_TI_RESPONDER, DECT_MM_ACCESS_RIGHTS_ACCEPT);
	if (mm->port_id_len)
		off = dect_nwk_put_ie(b, DECT_LAPC_MSG_MAX, off,
				      DECT_IE_PORTABLE_IDENTITY,
				      mm->port_id, mm->port_id_len);
	fixed_id[0] = OCTET_GROUP_END | FIXED_ID_TYPE_PARK;
	fixed_id[1] = (uint8_t)(OCTET_GROUP_END | (ARC_A_LEN + 1));
	memcpy(&fixed_id[2], mm->cfg.rfpi, DECT_MM_RFPI_LEN);
	off = dect_nwk_put_ie(b, DECT_LAPC_MSG_MAX, off, DECT_IE_FIXED_IDENTITY,
			      fixed_id, sizeof(fixed_id));
	txq_commit(mm, idx, off);
	mm->state = DECT_MM_REGISTERED;
	mm->stats.accepted++;
}

static void send_access_reject(struct dect_mm *mm)
{
	uint8_t reason[1] = { 0x01u };
	unsigned idx;
	uint8_t *b = txq_slot(mm, &idx);
	size_t off;

	if (!b)
		return;
	off = hdr(mm, b, DECT_TI_RESPONDER, DECT_MM_ACCESS_RIGHTS_REJECT);
	off = dect_nwk_put_ie(b, DECT_LAPC_MSG_MAX, off, DECT_IE_REJECT_REASON,
			      reason, sizeof(reason));
	txq_commit(mm, idx, off);
	mm->state = DECT_MM_FAILED;
	mm->stats.rejected++;
}

/*
 * FP -> PP LOCATE-ACCEPT: confirm location registration. After ACCESS-RIGHTS-
 * ACCEPT the C4 immediately runs location registration (sends LOCATE-REQUEST)
 * and will not report success until it is accepted. LOCATE-ACCEPT must carry a
 * PORTABLE-IDENTITY assigning an individual TPUI plus the LOCATION-AREA; both are
 * mandatory (EN 300 175-5 §6.3.6.17) and a LOCATION-AREA-only reply draws a
 * TEMPORARY-IDENTITY-ASSIGN-REJECT [CR A-0023]. LOCATE-REQUEST is PP-initiated,
 * so the FP replies as RESPONDER (F=1).
 *
 * TPUI encoding [CR A-0023]: PORTABLE-IDENTITY type TPUI (A0), 20-bit length
 * (94), then a 20-bit value. AVM's ordinary six-slot allocation packs the value
 * as {nibble 0, 0x11, slot(1..6)}. We enrol one handset, in slot 1.
 */
#define TPUI_ID_TYPE	0xA0u	/* PORTABLE-IDENTITY octet 3: identity type TPUI */
#define TPUI_ID_LEN	0x94u	/* octet 3a: ext + 20-bit identity length */
#define TPUI_PREFIX	0x11u	/* AVM fixed prefix byte of the 20-bit TPUI */

static void send_locate_accept(struct dect_mm *mm, const uint8_t *la, size_t la_len)
{
	uint8_t tpui[5] = { TPUI_ID_TYPE, TPUI_ID_LEN, 0x00, TPUI_PREFIX,
			    DECT_MM_REG_SLOT };
	unsigned idx;
	uint8_t *b = txq_slot(mm, &idx);
	size_t off;

	if (!b)
		return;
	off = hdr(mm, b, DECT_TI_RESPONDER, DECT_MM_LOCATE_ACCEPT);
	off = dect_nwk_put_ie(b, DECT_LAPC_MSG_MAX, off, DECT_IE_PORTABLE_IDENTITY,
			      tpui, sizeof(tpui));
	if (la && la_len)
		off = dect_nwk_put_ie(b, DECT_LAPC_MSG_MAX, off,
				      DECT_IE_LOCATION_AREA, la, la_len);
	txq_commit(mm, idx, off);
	mm->stats.located++;
}

static void on_locate_request(struct dect_mm *mm, const uint8_t *ies,
			      size_t ies_len)
{
	const uint8_t *la;
	size_t la_len;

	mm->stats.locate_req++;
	la = dect_nwk_find_ie(ies, ies_len, DECT_IE_LOCATION_AREA, &la_len);
	send_locate_accept(mm, la, la_len);
}

/*
 * Call-time ciphering (FP-initiated authentication + CIPHER-REQUEST). The IE
 * contents below are provisional and to be confirmed against AVM [CR Q-0015].
 */
#define AUTH_TYPE_CIPHER { 0x01u, 0x18u, 0x10u }	/* DSAA1, UAK #8, flags (TBD) */
#define CIPHER_INFO_DSC  { 0x81u, 0x01u }		/* Y, DSC; DCK (TBD) */

/* FP -> PP AUTHENTICATION-REQUEST: challenge the PP with RAND_F/RS (K = UAK). */
static void send_auth_request_fp(struct dect_mm *mm)
{
	uint8_t at[3] = AUTH_TYPE_CIPHER;
	unsigned idx;
	uint8_t *b = txq_slot(mm, &idx);
	size_t off;

	if (!b)
		return;
	off = hdr(mm, b, DECT_TI_ORIGINATOR, DECT_MM_AUTHENTICATION_REQUEST);
	off = dect_nwk_put_ie(b, DECT_LAPC_MSG_MAX, off, DECT_IE_AUTH_TYPE, at, sizeof(at));
	off = dect_nwk_put_ie(b, DECT_LAPC_MSG_MAX, off, DECT_IE_RAND,
			      mm->rand_f, sizeof(mm->rand_f));
	off = dect_nwk_put_ie(b, DECT_LAPC_MSG_MAX, off, DECT_IE_RS,
			      mm->rs, KEY_ALLOC_RS_LEN);
	txq_commit(mm, idx, off);
}

/* FP -> PP CIPHER-REQUEST: ask the PP to start DSC with the derived DCK. */
static void send_cipher_request(struct dect_mm *mm)
{
	uint8_t ci[2] = CIPHER_INFO_DSC;
	unsigned idx;
	uint8_t *b = txq_slot(mm, &idx);
	size_t off;

	if (!b)
		return;
	off = hdr(mm, b, DECT_TI_ORIGINATOR, DECT_MM_CIPHER_REQUEST);
	off = dect_nwk_put_ie(b, DECT_LAPC_MSG_MAX, off, DECT_IE_CIPHER_INFO, ci, sizeof(ci));
	txq_commit(mm, idx, off);
}

/*
 * The PP's AUTHENTICATION-REPLY (to our call-time challenge) carries RES. Verify
 * it against A12(A11(UAK,RS),RAND_F), keep the DCK (middle 8 bytes of A12), and
 * ask the PP to start ciphering.
 */
static void on_auth_reply_cipher(struct dect_mm *mm, const uint8_t *ies,
				 size_t ies_len)
{
	const uint8_t *res;
	size_t res_len;
	uint8_t ks[DECT_AUTH_KEY_LEN], dck[DECT_CIPHER_KEY_LEN], xres[4];

	res = dect_nwk_find_ie(ies, ies_len, DECT_IE_RES, &res_len);
	if (!res || res_len != 4) {
		mm->cipher_state = DECT_MM_CIPHER_FAILED;
		return;
	}
	dect_dsaa_a11(mm->cipher_uak, mm->rs, ks);
	dect_dsaa_a12(ks, mm->rand_f, dck, xres);
	if (memcmp(res, xres, 4) != 0) {
		mm->cipher_state = DECT_MM_CIPHER_FAILED;
		return;
	}
	memcpy(mm->dck, dck, DECT_MM_DCK_LEN);
	mm->cipher_state = DECT_MM_CIPHER_READY;
	send_cipher_request(mm);
}

void dect_mm_start_cipher(struct dect_mm *mm, const uint8_t uak[DECT_MM_UAK_LEN],
			  const uint8_t rand_f[8], const uint8_t rs[16])
{
	memcpy(mm->cipher_uak, uak, DECT_MM_UAK_LEN);
	memcpy(mm->rand_f, rand_f, sizeof(mm->rand_f));
	memcpy(mm->rs, rs, sizeof(mm->rs));
	mm->cipher_state = DECT_MM_CIPHER_WAIT_REPLY;
	send_auth_request_fp(mm);
}

int dect_mm_cipher_dck(const struct dect_mm *mm, uint8_t dck[DECT_MM_DCK_LEN])
{
	if (mm->cipher_state != DECT_MM_CIPHER_READY)
		return 0;
	memcpy(dck, mm->dck, DECT_MM_DCK_LEN);
	return 1;
}

static void on_access_rights_request(struct dect_mm *mm, const uint8_t *ies,
				     size_t ies_len)
{
	const uint8_t *pid;
	size_t pid_len;

	mm->stats.access_req++;
	pid = dect_nwk_find_ie(ies, ies_len, DECT_IE_PORTABLE_IDENTITY, &pid_len);
	if (pid && pid_len <= sizeof(mm->port_id)) {
		memcpy(mm->port_id, pid, pid_len);
		mm->port_id_len = pid_len;
	} else {
		mm->port_id_len = 0;
	}
	send_key_allocate(mm);
}

/*
 * The PP's AUTHENTICATION-REQUEST carries RES1 (its response to our KEY-ALLOCATE)
 * and RAND (RAND_P, its challenge to us). Verify RES1, answer RAND_P with RES2,
 * then grant access rights.
 */
static void on_auth_request(struct dect_mm *mm, const uint8_t *ies, size_t ies_len)
{
	const uint8_t *res1, *rand_p;
	size_t res1_len, rand_p_len;
	uint8_t ac[AC_LEN], k[DECT_AUTH_KEY_LEN];
	uint8_t ks[DECT_AUTH_KEY_LEN], uak[DECT_AUTH_KEY_LEN];
	uint8_t dck[DECT_CIPHER_KEY_LEN], xres1[4], res2[4];

	mm->stats.auth_req++;
	res1 = dect_nwk_find_ie(ies, ies_len, DECT_IE_RES, &res1_len);
	rand_p = dect_nwk_find_ie(ies, ies_len, DECT_IE_RAND, &rand_p_len);
	if (!res1 || res1_len != 4 || !rand_p || rand_p_len != DECT_AUTH_RAND_LEN) {
		mm->stats.auth_fail++;
		send_access_reject(mm);
		return;
	}

	pin_ac(mm, ac);
	dect_dsaa_b1(ac, AC_LEN, k);
	dect_dsaa_a11(k, mm->rs, ks);
	dect_dsaa_a12(ks, mm->rand_f, dck, xres1);	/* expected RES1 */
	if (memcmp(res1, xres1, 4) != 0) {
		mm->stats.auth_fail++;
		send_access_reject(mm);
		return;
	}
	mm->stats.auth_ok++;

	/* Answer the PP's challenge: RES2 = A22(A21(K,RS), RAND_P). */
	dect_dsaa_a21(k, mm->rs, uak);
	dect_dsaa_a22(uak, rand_p, res2);

	/*
	 * A21(K,RS) = KS' is the key allocated by this procedure: it becomes the
	 * handset's long-term UAK [CR A-0022]. Keep it so the chip subscription
	 * record (0x020A) can be written [CR A-0026].
	 */
	memcpy(mm->uak, uak, DECT_MM_UAK_LEN);
	mm->subscribed = 1;

	send_auth_reply(mm, res2);
	send_access_accept(mm);
}

void dect_mm_rx(struct dect_mm *mm, const uint8_t *msg, size_t len,
		const uint8_t rand_f[8], const uint8_t rs[16])
{
	struct dect_nwk_hdr h;
	const uint8_t *ies;
	size_t ies_len;

	if (dect_nwk_parse(msg, len, &h, &ies, &ies_len) < 0 || h.pd != DECT_PD_MM)
		return;

	switch (h.msg_type) {
	case DECT_MM_ACCESS_RIGHTS_REQUEST:
		mm->ti_value = h.ti_value;
		memcpy(mm->rand_f, rand_f, sizeof(mm->rand_f));
		/* RS is a fresh 16-byte random seed supplied by the caller; a zero
		 * seed is not AVM-equivalent and the C4 may reject it [CR A-0022]. */
		memcpy(mm->rs, rs, sizeof(mm->rs));
		on_access_rights_request(mm, ies, ies_len);
		break;
	case DECT_MM_AUTHENTICATION_REQUEST:
		if (mm->state == DECT_MM_WAIT_AUTH)
			on_auth_request(mm, ies, ies_len);
		break;
	case DECT_MM_AUTHENTICATION_REPLY:
		if (mm->cipher_state == DECT_MM_CIPHER_WAIT_REPLY)
			on_auth_reply_cipher(mm, ies, ies_len);
		break;
	case DECT_MM_LOCATE_REQUEST:
		mm->ti_value = h.ti_value;	/* new PP-initiated transaction */
		on_locate_request(mm, ies, ies_len);
		break;
	default:
		break;
	}
}

size_t dect_mm_poll(struct dect_mm *mm, uint8_t *out, size_t cap)
{
	size_t n;

	if (mm->out_count == 0)
		return 0;
	n = mm->out_len[mm->out_head];
	if (cap < n)
		return 0;
	memcpy(out, mm->out[mm->out_head], n);
	mm->out_head = (mm->out_head + 1) % DECT_MM_TXQ;
	mm->out_count--;
	return n;
}

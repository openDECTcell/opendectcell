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
#define AUTH_ALG_DSAA		0x01u
#define ALLOC_KEY_NUM		1u	/* UAK number to allocate */
#define ALLOC_CODE_NUM		1u	/* authentication-code number */
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

static size_t hdr(const struct dect_mm *mm, uint8_t *buf, uint8_t msg_type)
{
	return dect_nwk_build_hdr(buf, DECT_LAPC_MSG_MAX, DECT_PD_MM,
				  DECT_TI_RESPONDER, mm->ti_value, msg_type);
}

/* FP -> PP KEY-ALLOCATE: ALLOCATION-TYPE (DSAA), RAND (RAND_F), RS. */
static void send_key_allocate(struct dect_mm *mm)
{
	uint8_t alloc[2] = {
		AUTH_ALG_DSAA, (uint8_t)((ALLOC_KEY_NUM << 4) | ALLOC_CODE_NUM),
	};
	unsigned idx;
	uint8_t *b = txq_slot(mm, &idx);
	size_t off;

	if (!b)
		return;
	off = hdr(mm, b, DECT_MM_KEY_ALLOCATE);
	off = dect_nwk_put_ie(b, DECT_LAPC_MSG_MAX, off, DECT_IE_ALLOCATION_TYPE,
			      alloc, sizeof(alloc));
	off = dect_nwk_put_ie(b, DECT_LAPC_MSG_MAX, off, DECT_IE_RAND,
			      mm->rand_f, sizeof(mm->rand_f));
	off = dect_nwk_put_ie(b, DECT_LAPC_MSG_MAX, off, DECT_IE_RS,
			      mm->rs, sizeof(mm->rs));
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
	off = hdr(mm, b, DECT_MM_AUTHENTICATION_REPLY);
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
	off = hdr(mm, b, DECT_MM_ACCESS_RIGHTS_ACCEPT);
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
	off = hdr(mm, b, DECT_MM_ACCESS_RIGHTS_REJECT);
	off = dect_nwk_put_ie(b, DECT_LAPC_MSG_MAX, off, DECT_IE_REJECT_REASON,
			      reason, sizeof(reason));
	txq_commit(mm, idx, off);
	mm->state = DECT_MM_FAILED;
	mm->stats.rejected++;
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

	send_auth_reply(mm, res2);
	send_access_accept(mm);
}

void dect_mm_rx(struct dect_mm *mm, const uint8_t *msg, size_t len,
		const uint8_t rand_f[8])
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
		memset(mm->rs, 0, sizeof(mm->rs));	/* RS = 0 for now */
		on_access_rights_request(mm, ies, ies_len);
		break;
	case DECT_MM_AUTHENTICATION_REQUEST:
		if (mm->state == DECT_MM_WAIT_AUTH)
			on_auth_request(mm, ies, ies_len);
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

// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * NWK Mobility Management registration FSM. See dect_mm.h.
 * ETSI EN 300 175-5 §13; IE encodings per EN 300 175-5 §7.7.
 */
#include "dect_mm.h"
#include "dect_nwk.h"
#include "dect_dsaa.h"

#include <string.h>

/* AUTH-TYPE / FIXED-IDENTITY constants (EN 300 175-5 §7.7.4/§7.7.18). */
#define AUTH_ALG_DSAA		0x01u
#define AUTH_KEY_TYPE_AC	0x4u	/* authentication code (PIN) */
#define AUTH_KEY_NUM_IPUI_PARK	0x8u	/* key relates to the IPUI/PARK pair */
#define AUTH_FLAG_UPC		0x10u	/* update cipher key */
#define FIXED_ID_TYPE_PARK	0x20u
#define OCTET_GROUP_END		0x80u
#define ARC_A_LEN		36u	/* class-A ARI length in bits */

#define AC_LEN			4u

/* Build the AC (authentication code) from the PIN, defaulting to "0000". */
static void pin_ac(const struct dect_mm *mm, uint8_t ac[AC_LEN])
{
	const char *pin = mm->cfg.pin[0] ? mm->cfg.pin : "0000";

	dect_dsaa_pin_to_ac(pin, ac, AC_LEN);
}

/* Compute the expected RES1 for our challenge: A12(A11(K,RS), RAND_F).res1. */
static void expected_res1(const struct dect_mm *mm, uint8_t res[4])
{
	uint8_t ac[AC_LEN], k[DECT_AUTH_KEY_LEN], ks[DECT_AUTH_KEY_LEN];
	uint8_t dck[DECT_CIPHER_KEY_LEN];

	pin_ac(mm, ac);
	dect_dsaa_b1(ac, AC_LEN, k);
	dect_dsaa_a11(k, mm->rs, ks);
	dect_dsaa_a12(ks, mm->rand_f, dck, res);
}

void dect_mm_init(struct dect_mm *mm, const struct dect_mm_cfg *cfg)
{
	memset(mm, 0, sizeof(*mm));
	mm->cfg = *cfg;
	mm->cfg.authenticate = cfg->authenticate ? 1u : 0u;
	mm->state = DECT_MM_IDLE;
}

/* Queue an MM message with the responder transaction flag and our TI value. */
static size_t begin_msg(const struct dect_mm *mm, uint8_t *buf, size_t cap,
			uint8_t msg_type)
{
	return dect_nwk_build_hdr(buf, cap, DECT_PD_MM, DECT_TI_RESPONDER,
				  mm->ti_value, msg_type);
}

/* FP -> PP AUTHENTICATION-REQUEST with AUTH-TYPE, RAND, RS. */
static void send_auth_request(struct dect_mm *mm)
{
	uint8_t auth_type[3] = {
		AUTH_ALG_DSAA,
		(uint8_t)((AUTH_KEY_TYPE_AC << 4) | AUTH_KEY_NUM_IPUI_PARK),
		AUTH_FLAG_UPC,
	};
	size_t off = begin_msg(mm, mm->out, sizeof(mm->out),
			       DECT_MM_AUTHENTICATION_REQUEST);

	off = dect_nwk_put_ie(mm->out, sizeof(mm->out), off, DECT_IE_AUTH_TYPE,
			      auth_type, sizeof(auth_type));
	off = dect_nwk_put_ie(mm->out, sizeof(mm->out), off, DECT_IE_RAND,
			      mm->rand_f, sizeof(mm->rand_f));
	off = dect_nwk_put_ie(mm->out, sizeof(mm->out), off, DECT_IE_RS,
			      mm->rs, sizeof(mm->rs));
	mm->out_len = off;
	mm->state = DECT_MM_WAIT_AUTH_REPLY;
	mm->stats.auth_sent++;
}

/*
 * FP -> PP ACCESS-RIGHTS-ACCEPT: echo the PP's PORTABLE-IDENTITY and add our
 * FIXED-IDENTITY (PARK) built from the base RFPI.
 *
 * HARDWARE NOTE: the FIXED-IDENTITY/PARK encoding from the 5-byte RFPI is the
 * part to confirm against a real handset. Per EN 300 175-5 §7.7.18 the contents
 * are [0x80|type, 0x80|(ARI_len+1), ARI...]; class-A ARI is 36 bits. We place
 * the RFPI bytes as the ARI; the exact alignment (and PARK vs ARI type) may need
 * adjustment once observed on air.
 */
static void send_access_accept(struct dect_mm *mm)
{
	uint8_t fixed_id[2 + DECT_MM_RFPI_LEN];
	size_t off = begin_msg(mm, mm->out, sizeof(mm->out),
			       DECT_MM_ACCESS_RIGHTS_ACCEPT);

	if (mm->port_id_len)
		off = dect_nwk_put_ie(mm->out, sizeof(mm->out), off,
				      DECT_IE_PORTABLE_IDENTITY,
				      mm->port_id, mm->port_id_len);

	fixed_id[0] = OCTET_GROUP_END | FIXED_ID_TYPE_PARK;
	fixed_id[1] = (uint8_t)(OCTET_GROUP_END | (ARC_A_LEN + 1));
	memcpy(&fixed_id[2], mm->cfg.rfpi, DECT_MM_RFPI_LEN);
	off = dect_nwk_put_ie(mm->out, sizeof(mm->out), off,
			      DECT_IE_FIXED_IDENTITY, fixed_id, sizeof(fixed_id));

	mm->out_len = off;
	mm->state = DECT_MM_REGISTERED;
	mm->stats.accepted++;
}

static void send_access_reject(struct dect_mm *mm)
{
	uint8_t reason[1] = { 0x01u };	/* generic; refine per EN 300 175-5 */
	size_t off = begin_msg(mm, mm->out, sizeof(mm->out),
			       DECT_MM_ACCESS_RIGHTS_REJECT);

	off = dect_nwk_put_ie(mm->out, sizeof(mm->out), off,
			      DECT_IE_REJECT_REASON, reason, sizeof(reason));
	mm->out_len = off;
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

	if (mm->cfg.authenticate)
		send_auth_request(mm);
	else
		send_access_accept(mm);
}

static void on_auth_reply(struct dect_mm *mm, const uint8_t *ies, size_t ies_len)
{
	const uint8_t *res;
	size_t res_len;
	uint8_t xres[4];

	res = dect_nwk_find_ie(ies, ies_len, DECT_IE_RES, &res_len);
	if (!res || res_len != sizeof(xres)) {
		send_access_reject(mm);
		mm->stats.auth_fail++;
		return;
	}

	expected_res1(mm, xres);
	if (memcmp(res, xres, sizeof(xres)) == 0) {
		mm->stats.auth_ok++;
		send_access_accept(mm);
	} else {
		mm->stats.auth_fail++;
		send_access_reject(mm);
	}
}

void dect_mm_rx(struct dect_mm *mm, const uint8_t *msg, size_t len,
		const uint8_t rand_f[8])
{
	struct dect_nwk_hdr h;
	const uint8_t *ies;
	size_t ies_len;

	if (dect_nwk_parse(msg, len, &h, &ies, &ies_len) < 0)
		return;
	if (h.pd != DECT_PD_MM)
		return;

	switch (h.msg_type) {
	case DECT_MM_ACCESS_RIGHTS_REQUEST:
		mm->ti_value = h.ti_value;
		memcpy(mm->rand_f, rand_f, sizeof(mm->rand_f));
		memset(mm->rs, 0, sizeof(mm->rs));
		on_access_rights_request(mm, ies, ies_len);
		break;
	case DECT_MM_AUTHENTICATION_REPLY:
		if (mm->state == DECT_MM_WAIT_AUTH_REPLY)
			on_auth_reply(mm, ies, ies_len);
		break;
	default:
		break;
	}
}

size_t dect_mm_poll(struct dect_mm *mm, uint8_t *out, size_t cap)
{
	size_t n = mm->out_len;

	if (n == 0 || cap < n)
		return 0;
	memcpy(out, mm->out, n);
	mm->out_len = 0;
	return n;
}

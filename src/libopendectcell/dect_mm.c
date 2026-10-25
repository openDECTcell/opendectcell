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

/* NUL-terminated within cfg.pin and DECT_PIN_DIGITS_MIN..MAX digits */
static int pin_usable(const struct dect_mm *mm)
{
	return memchr(mm->cfg.pin, 0, sizeof(mm->cfg.pin)) &&
	       !dect_dsaa_pin_check(mm->cfg.pin, DECT_PIN_DIGITS_MIN);
}

static void pin_ac(const struct dect_mm *mm, uint8_t ac[AC_LEN])
{
	dect_dsaa_pin_to_ac(mm->cfg.pin, ac, AC_LEN);
}

void dect_mm_init(struct dect_mm *mm, const struct dect_mm_cfg *cfg)
{
	memset(mm, 0, sizeof(*mm));
	mm->cfg = *cfg;
	mm->cfg.authenticate = cfg->authenticate ? 1u : 0u;
	mm->state = DECT_MM_IDLE;
	mm->adopted = -1;
}

int dect_mm_hold(struct dect_mm *mm, const uint8_t *id, size_t id_len,
		 uint8_t slot, const uint8_t tpui[3], int tag)
{
	struct dect_mm_held *h;

	if (mm->n_held >= DECT_MM_HELD_MAX || !id || !id_len ||
	    id_len > DECT_MM_PORTID_MAX || slot < 1u || slot > DECT_MM_MAX_SLOT)
		return -1;
	h = &mm->held[mm->n_held++];
	memset(h, 0, sizeof(*h));
	memcpy(h->id, id, id_len);
	h->id_len = id_len;
	h->slot = slot;
	if (tpui)
		memcpy(h->tpui, tpui, sizeof(h->tpui));
	h->tag = tag;
	return 0;
}

int dect_mm_locate_pending(const struct dect_mm *mm, const uint8_t **id,
			   size_t *id_len)
{
	if (!mm->locate_pending)
		return 0;
	if (id)
		*id = mm->locate_id;
	if (id_len)
		*id_len = mm->locate_id_len;
	return 1;
}

static void send_locate_accept(struct dect_mm *mm, const uint8_t *la,
			       size_t la_len);
static void send_locate_reject(struct dect_mm *mm, uint8_t reason);

int dect_mm_locate_resume(struct dect_mm *mm)
{
	if (!mm->locate_pending)
		return 0;
	mm->locate_pending = 0;
	mm->ti_value = mm->locate_ti;		/* answer its transaction */
	if (dect_mm_adopt(mm, mm->locate_id, mm->locate_id_len)) {
		send_locate_accept(mm, mm->locate_la_len ? mm->locate_la : NULL,
				   mm->locate_la_len);
		return 1;
	}
	send_locate_reject(mm, DECT_MM_REJECT_IPUI_UNKNOWN);
	return -1;
}

int dect_mm_adopt(struct dect_mm *mm, const uint8_t *id, size_t id_len)
{
	unsigned i;

	if (!id || !id_len)
		return 0;
	for (i = 0; i < mm->n_held; i++) {
		const struct dect_mm_held *h = &mm->held[i];

		if (h->id_len != id_len || memcmp(h->id, id, id_len))
			continue;
		memcpy(mm->port_id, h->id, h->id_len);
		mm->port_id_len = h->id_len;
		mm->cfg.sub_slot = h->slot;
		memcpy(mm->cfg.tpui, h->tpui, sizeof(mm->cfg.tpui));
		mm->adopted = h->tag;
		return 1;
	}
	return mm->port_id_len && id_len == mm->port_id_len &&
	       !memcmp(id, mm->port_id, id_len);
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
/*
 * <<CODEC-LIST>> for wideband speech.
 *
 * ETSI TS 102 527-1 V1.5.1 7.3.1 is a "shall": equipment supporting New
 * Generation DECT wideband speech adds this element to {ACCESS-RIGHTS-REQUEST},
 * {ACCESS-RIGHTS-ACCEPT}, {LOCATE-REQUEST} and {LOCATE-ACCEPT}, it must contain
 * at least G.722 and G.726, and the negotiation indicator must always say
 * "codec negotiation possible". We were sending it in none of them, while the
 * handset sent its own list in both of its messages -- so no codec was ever
 * agreed, and 7.3.3 then explains the symptom: a CC-SETUP need not repeat the
 * list "in case the most recent list sent during registration/location
 * registration is still the valid one", so a wideband SETUP arrives with no
 * agreed codec behind it, the PT cannot bring up the U-plane and rejects the
 * call with "insufficient resources".
 *
 * Contents and coding per EN 300 175-5 7.7.54: octet 3 is the negotiation
 * indicator (bit 8 set, bits 7-5 = 001 "codec negotiation"); each entry is
 * three octets, {0 + codec identifier}, {direction + MAC/DLC service} and
 * {ext + C-plane routing + slot size}, with the extension bit set on the last.
 * G.722 is identifier 3 at 64 kbit/s on a long slot (j=640) and G.726 is
 * identifier 2 at 32 kbit/s on a full slot -- the same pairing the A540 CAT
 * offers us and the same one observed in a stock wideband call.
 */
#define CODEC_NEG_POSSIBLE	0x90u	/* bit 8 + 001 "codec negotiation" */

static const uint8_t codec_list_wb[] = {
	CODEC_NEG_POSSIBLE,
	0x03u, 0x00u, 0x01u,	/* G.722, 64 kbit/s, long slot j=640  */
	0x02u, 0x00u, 0x84u,	/* G.726, 32 kbit/s, full slot, last  */
};

/*
 * Full-slot codec only, for cfg.narrowband_only. Same IE, one entry: the
 * G.722 entry above asks for a long slot (j=640), this one does not.
 */
static const uint8_t codec_list_nb[] = {
	CODEC_NEG_POSSIBLE,
	0x02u, 0x00u, 0x84u,	/* G.726, 32 kbit/s, full slot, last  */
};

static size_t put_codec_list(const struct dect_mm *mm, uint8_t *b, size_t off)
{
	const uint8_t *list = codec_list_wb;
	size_t len = sizeof(codec_list_wb);
	size_t n;

	if (mm->cfg.narrowband_only) {
		list = codec_list_nb;
		len = sizeof(codec_list_nb);
	}
	n = dect_nwk_put_ie(b, DECT_LAPC_MSG_MAX, off,
			    DECT_IE_CODEC_LIST, list, len);

	/*
	 * put_ie returns 0 if the element would not fit, and the callers here
	 * commit `off` unchecked -- so hand back the original offset rather
	 * than 0, which would commit a zero-length message.
	 */
	return n ? n : off;
}

/*
 * The <<FIXED-IDENTITY>> contents for this FP: type 32 PARK, length PLI+1,
 * then the whole PARK including its non-significant bits -- EN 300 444
 * table 10 and table 21 both require exactly that, for the PT's outgoing
 * {CC-SETUP} and for the FT's incoming one.
 *
 * Exposed because CC needs the identical bytes. They used to be built only
 * here, so an FP-originated call had no way to name the FP and dect-stubd
 * waited to copy the pair out of the first call the handset placed instead.
 * That made "ring the handset" depend on the handset having dialled us first,
 * which is not a DECT requirement -- it was just this encoding living in one
 * function.
 */
size_t dect_mm_fixed_identity(const struct dect_mm *mm, uint8_t *out,
			      size_t cap)
{
	if (!mm || !out || cap < 2u + DECT_MM_RFPI_LEN)
		return 0;
	out[0] = OCTET_GROUP_END | FIXED_ID_TYPE_PARK;
	out[1] = (uint8_t)(OCTET_GROUP_END | (ARC_A_LEN + 1));
	memcpy(&out[2], mm->cfg.rfpi, DECT_MM_RFPI_LEN);
	return 2u + DECT_MM_RFPI_LEN;
}

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
	if (!dect_mm_fixed_identity(mm, fixed_id, sizeof(fixed_id)))
		return;
	off = dect_nwk_put_ie(b, DECT_LAPC_MSG_MAX, off, DECT_IE_FIXED_IDENTITY,
			      fixed_id, sizeof(fixed_id));
	off = put_codec_list(mm, b, off);	/* TS 102 527-1 7.3.1 */
	txq_commit(mm, idx, off);
	mm->state = DECT_MM_REGISTERED;
	mm->stats.accepted++;
}

static void send_access_reject_reason(struct dect_mm *mm, uint8_t why)
{
	uint8_t reason[1] = { why };
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
 * A failed PIN is "authentication failed" (EN 300 175-5 §7.7.34). We used
 * to send 0x01, which means "TPUI unknown".
 */
static void send_access_reject(struct dect_mm *mm)
{
	send_access_reject_reason(mm, DECT_MM_REJECT_AUTH_FAILED);
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

uint8_t dect_mm_sub_slot(const struct dect_mm *mm)
{
	uint8_t slot = mm->cfg.sub_slot;

	if (slot < 1u || slot > DECT_MM_MAX_SLOT)
		return DECT_MM_REG_SLOT;
	return slot;
}

void dect_mm_tpui(const struct dect_mm *mm, uint8_t out[3])
{
	const uint8_t *t = mm->cfg.tpui;

	if (t[0] || t[1] || t[2]) {	/* configured: use it verbatim */
		out[0] = t[0]; out[1] = t[1]; out[2] = t[2];
		return;
	}
	out[0] = 0x00;			/* historical: derived from the slot */
	out[1] = TPUI_PREFIX;
	out[2] = dect_mm_sub_slot(mm);
}

static void send_locate_accept(struct dect_mm *mm, const uint8_t *la, size_t la_len)
{
	uint8_t id[3];
	uint8_t tpui[5] = { TPUI_ID_TYPE, TPUI_ID_LEN, 0, 0, 0 };
	unsigned idx;
	uint8_t *b = txq_slot(mm, &idx);
	size_t off;

	if (!b)
		return;
	dect_mm_tpui(mm, id);
	memcpy(&tpui[2], id, sizeof(id));
	off = hdr(mm, b, DECT_TI_RESPONDER, DECT_MM_LOCATE_ACCEPT);
	off = dect_nwk_put_ie(b, DECT_LAPC_MSG_MAX, off, DECT_IE_PORTABLE_IDENTITY,
			      tpui, sizeof(tpui));
	if (la && la_len)
		off = dect_nwk_put_ie(b, DECT_LAPC_MSG_MAX, off,
				      DECT_IE_LOCATION_AREA, la, la_len);
	if (mm->cfg.locate_validity) {
		/* no lock limits, defined time limit 1 (§7.7.13) */
		const uint8_t dur[2] = { 0x71, mm->cfg.locate_validity };

		off = dect_nwk_put_ie(b, DECT_LAPC_MSG_MAX, off,
				      DECT_IE_DURATION, dur, sizeof(dur));
	}
	off = put_codec_list(mm, b, off);	/* TS 102 527-1 7.3.1 */
	txq_commit(mm, idx, off);
	mm->stats.located++;

	/*
	 * This message carries a TPUI, so per EN 300 175-5 §13.2.2 it starts
	 * the temporary-identity assignment procedure and the FT starts
	 * <MM_ident.1>. The deadline is armed in the tick (we have no clock
	 * here); until the PT acks, the assignment is NOT confirmed.
	 */
	mm->tpui_confirmed = 0;
	mm->tpui_timed_out = 0;
	mm->ident_pending = 1;
}

/*
 * FP -> PP LOCATE-REJECT (EN 300 175-5 §6.3.6.18, Table 55): protocol
 * discriminator, transaction identifier, message type and a mandatory
 * <<REJECT-REASON>> (§7.7.34). Reason 0x02 is "IPUI unknown", which is
 * exactly our case -- the handset is not the one we hold a record for.
 */
static void send_locate_reject(struct dect_mm *mm, uint8_t reason)
{
	unsigned idx;
	uint8_t *b = txq_slot(mm, &idx);
	size_t off;

	if (!b)
		return;
	off = hdr(mm, b, DECT_TI_RESPONDER, DECT_MM_LOCATE_REJECT);
	if (!off)
		return;
	off = dect_nwk_put_ie(b, DECT_LAPC_MSG_MAX, off,
			      DECT_IE_REJECT_REASON, &reason, 1);
	if (!off)
		return;
	txq_commit(mm, idx, off);
	mm->stats.locate_rej++;
}

/*
 * Accept a location registration only from the handset we actually hold.
 *
 * We used to accept every {LOCATE-REQUEST} unconditionally. Measured
 * 2026-10-23 (vF418, vF426): with one handset's subscription loaded, a
 * DIFFERENT handset -- one the chip had no record of at all -- attached and
 * had its location registration accepted, thirty times in ninety seconds.
 * The chip admits anything locked to our RFPI, so this check is the only
 * gate there is, and without it the store said one thing while the air said
 * another, which made every identity question of the week ambiguous.
 *
 * A GAP FP serves subscribed handsets. If the <<PORTABLE-IDENTITY>> does not
 * match the one we hold, reject it.
 */
/* Keep what the handset declares about itself, for diagnostics. */
static void learn_self_description(struct dect_mm *mm, const uint8_t *ies,
				   size_t ies_len)
{
	size_t n;
	const uint8_t *p;

	p = dect_nwk_find_ie(ies, ies_len, DECT_IE_MODEL_IDENTIFIER, &n);
	if (p && n <= sizeof(mm->peer_model)) {
		memcpy(mm->peer_model, p, n);
		mm->peer_model_len = n;
	}
	p = dect_nwk_find_ie(ies, ies_len, DECT_IE_CODEC_LIST, &n);
	if (p && n <= sizeof(mm->peer_codecs)) {
		memcpy(mm->peer_codecs, p, n);
		mm->peer_codecs_len = n;
	}
}

static void on_locate_request(struct dect_mm *mm, const uint8_t *ies,
			      size_t ies_len)
{
	const uint8_t *la, *pid;
	size_t la_len, pid_len;

	mm->stats.locate_req++;

	pid = dect_nwk_find_ie(ies, ies_len, DECT_IE_PORTABLE_IDENTITY,
			       &pid_len);
	/*
	 * <<PORTABLE-IDENTITY>> is mandatory here (EN 300 175-5 table 56), and
	 * a missing mandatory element gets the reject with "information
	 * element error" (§17.6.4). The check used to require the element to
	 * be present before comparing it, so leaving it out skipped the
	 * check and won a {LOCATE-ACCEPT}. Found by the second external
	 * review (the clean-room record/an external review-1-24102026.md, finding 1).
	 */
	if (!pid) {
		send_locate_reject(mm, DECT_MM_REJECT_IE_ERROR);
		return;
	}
	/* Only handsets we hold a subscription for; none held, none known. */
	if (!dect_mm_adopt(mm, pid, pid_len)) {
		if (mm->cfg.defer_unknown && pid_len <= sizeof(mm->locate_id)) {
			if (mm->locate_pending && pid_len == mm->locate_id_len &&
			    !memcmp(pid, mm->locate_id, pid_len)) {
				mm->locate_ti = mm->ti_value;	/* its repeat */
				mm->stats.locate_retrans++;
				return;
			}
			la = dect_nwk_find_ie(ies, ies_len,
					      DECT_IE_LOCATION_AREA, &la_len);
			memcpy(mm->locate_id, pid, pid_len);
			mm->locate_id_len = pid_len;
			mm->locate_la_len = la && la_len <= sizeof(mm->locate_la)
					    ? la_len : 0;
			if (mm->locate_la_len)
				memcpy(mm->locate_la, la, la_len);
			mm->locate_ti = mm->ti_value;
			mm->locate_pending = 1;
			mm->stats.locate_deferred++;
			return;
		}
		send_locate_reject(mm, DECT_MM_REJECT_IPUI_UNKNOWN);
		return;
	}
	learn_self_description(mm, ies, ies_len);

	la = dect_nwk_find_ie(ies, ies_len, DECT_IE_LOCATION_AREA, &la_len);
	send_locate_accept(mm, la, la_len);
}

/*
 * Call-time ciphering (FP-initiated authentication + CIPHER-REQUEST).
 * CIPHER-INFO is AVM's exact over-air content `19 02 81 98` [CR A-0027 Q3]:
 * 0x81 enables the standard cipher, 0x98 identifies the DCK / key number 8.
 * AUTH-TYPE is still provisional (A-0011 saw AVM use a 0x18 second octet, i.e.
 * UAK key number 8, which matches the 0x98 above); confirm against hardware.
 */
#define AUTH_TYPE_CIPHER { 0x01u, 0x18u, 0x10u }	/* DSAA1, UAK #8, flags (TBD) */
#define CIPHER_INFO_DSC  { 0x81u, 0x98u }		/* [CR A-0027] enable, DCK #8 */

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
	memcpy(mm->cipher_res, xres, sizeof(mm->cipher_res));
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
	learn_self_description(mm, ies, ies_len);
	pid = dect_nwk_find_ie(ies, ies_len, DECT_IE_PORTABLE_IDENTITY, &pid_len);
	/*
	 * No usable PIN, no registration. An empty PIN used to mean "0000",
	 * so an owner that forgot to set one opened the base to anybody.
	 */
	if (mm->cfg.registration == DECT_MM_REG_CLOSED || !pin_usable(mm) ||
	    (mm->cfg.registration == DECT_MM_REG_SAME &&
	     (!pid || !mm->port_id_len || pid_len != mm->port_id_len ||
	      memcmp(pid, mm->port_id, pid_len)))) {
		send_access_reject_reason(mm, DECT_MM_REJECT_IPUI_NOT_ACCEPTED);
		return;
	}
	/*
	 * Held aside until the PIN is proven (on_auth_request()). This used to
	 * overwrite port_id at once, so a stranger with the wrong PIN replaced
	 * the identity we hold for the rest of the link.
	 */
	if (pid && pid_len <= sizeof(mm->pend_id)) {
		memcpy(mm->pend_id, pid, pid_len);
		mm->pend_id_len = pid_len;
	} else {
		mm->pend_id_len = 0;
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
		send_access_reject_reason(mm, DECT_MM_REJECT_IE_ERROR);
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
	/* Authenticated: the requested identity is now the one we hold. */
	memcpy(mm->port_id, mm->pend_id, mm->pend_id_len);
	mm->port_id_len = mm->pend_id_len;

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
	case DECT_MM_TEMP_IDENTITY_ASSIGN_ACK:
		/*
		 * The PT has taken the TPUI we assigned. Stop <MM_ident.1>.
		 * No reply: EN 300 175-5 §13.2.2 has the FT issue a local
		 * confirm primitive, nothing on the air.
		 *
		 * Only for an assignment we made: one still pending, or one
		 * that timed out (a late ACK is accepted but stays visible as
		 * late, see dect_mm_tick()). An ACK with no assignment behind
		 * it used to mark the TPUI confirmed all the same; paging
		 * relies on that flag. Found by the second external review,
		 * the clean-room record/an external review-1-24102026.md, finding 4.
		 */
		if (!mm->ident_pending && !mm->tpui_timed_out) {
			mm->stats.tpui_unsolicited++;
			break;
		}
		mm->ident_pending = 0;
		mm->ident_deadline_ms = 0;
		mm->tpui_confirmed = 1;
		mm->stats.tpui_acked++;
		break;
	default:
		break;
	}
}

/* <MM_ident.1>: FT value 10 seconds (EN 300 175-5 §A.2). */
#define MM_IDENT1_MS	10000u

void dect_mm_tick(struct dect_mm *mm, uint64_t now_ms)
{
	if (!mm->ident_pending) {
		mm->ident_deadline_ms = 0;
		return;
	}
	if (!mm->ident_deadline_ms) {
		mm->ident_deadline_ms = now_ms + MM_IDENT1_MS;
		return;
	}
	if (now_ms < mm->ident_deadline_ms)
		return;
	/*
	 * "If timer <MM_ident.1> expires, the FT shall abort the procedure and
	 * release the transaction" (§13.2.2, the {LOCATE-ACCEPT} path -- the
	 * retransmission in the standalone {TEMPORARY-IDENTITY-ASSIGN} path
	 * does not apply, because the assignment rode in the LOCATE-ACCEPT and
	 * there is no separate message to repeat).
	 *
	 * We record it rather than tearing the link down: the registration
	 * itself succeeded, and only the TPUI is in doubt. The caller decides
	 * what an unconfirmed TPUI is worth -- see dect_mm_tpui_confirmed().
	 */
	mm->ident_pending = 0;
	mm->ident_deadline_ms = 0;
	mm->tpui_timed_out = 1;
	mm->stats.tpui_timeouts++;
}

uint64_t dect_mm_next_deadline(const struct dect_mm *mm)
{
	return mm->ident_deadline_ms ? mm->ident_deadline_ms : UINT64_MAX;
}

int dect_mm_tpui_confirmed(const struct dect_mm *mm)
{
	return mm->tpui_confirmed;
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

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
#define DECT_MM_REJECT_IE_ERROR		0x60u	/* "information element error" */
#define DECT_MM_REJECT_IPUI_NOT_ACCEPTED 0x06u
#define DECT_MM_REJECT_AUTH_FAILED	0x10u	/* "authentication failed" */

/*
 * Who may obtain access rights ({ACCESS-RIGHTS-REQUEST}) on a link. A base
 * accepts registrations only while the user has opened it to them, and a
 * link that cannot keep a new subscription must not accept one: the
 * handset would show "registered" with a key the base never stored.
 */
enum dect_mm_registration {
	DECT_MM_REG_OPEN = 0,	/* anyone with the PIN (the default) */
	DECT_MM_REG_CLOSED,	/* nobody: "IPUI not accepted" */
	DECT_MM_REG_SAME,	/* only port_id re-enrolling, with the PIN */
};

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
	/*
	 * Decimal PIN (AC), DECT_PIN_DIGITS_MIN..MAX digits. Anything else
	 * (including "") refuses every registration: there is no default.
	 */
	char    pin[DECT_MM_PIN_MAX];
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
	/* enum dect_mm_registration */
	uint8_t registration;
	/*
	 * 1 = hold a {LOCATE-REQUEST} from an identity the link does not
	 * know, instead of rejecting it, so the owner can fetch the record
	 * (a roamer, from the controller) and then dect_mm_locate_resume().
	 * The PT waits <MM_locate.1> = 20 s, retransmits once, then aborts.
	 */
	uint8_t defer_unknown;
	/*
	 * Validity of a location registration, in units of 2^8 multiframes
	 * (40.96 s): {LOCATE-ACCEPT} carries <<DURATION>> "defined time
	 * limit 1" (EN 300 175-5 §7.7.13), after which the PT locates again
	 * (§13.4.1). 0 = no <<DURATION>>, i.e. infinite. For tests that need
	 * a handset to locate on demand (multi-cell M4).
	 */
	uint8_t locate_validity;
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
	uint32_t locate_rej;	/* rejected: not a handset we hold */
	uint32_t located;	/* LOCATE-ACCEPT sent */
	uint32_t tpui_acked;	/* {TEMPORARY-IDENTITY-ASSIGN-ACK} received */
	uint32_t tpui_timeouts;	/* <MM_ident.1> expiries */
	uint32_t tpui_unsolicited; /* ACKs with no assignment made: ignored */
	uint32_t locate_deferred;  /* unknown identities held for the owner */
	uint32_t locate_retrans;   /* the PT repeated a held request */
};

/*
 * A handset this base holds a subscription for, besides (or instead of) the
 * one in port_id. See dect_mm_hold() and dect_mm_adopt().
 */
struct dect_mm_held {
	uint8_t id[DECT_MM_PORTID_MAX];	/* its <<PORTABLE-IDENTITY>> contents */
	size_t  id_len;
	uint8_t slot;			/* its chip subscription slot */
	uint8_t tpui[3];		/* its TPUI; all-zero derives from slot */
	int     tag;			/* the owner's handle for it */
};

#define DECT_MM_HELD_MAX	DECT_MM_MAX_SLOT

/*
 * <<TERMINAL-CAPABILITY>> (EN 300 175-5 §7.7.41, figure 85): what the
 * handset says it can do. Octet groups 3 and 4 end at the octet with bit 8
 * set. TS 102 527-3 table 35: profile indicator_7 bit 2 = NG-DECT Part 1
 * (wideband), bit 3 = Part 3 (extended services). "A Part 3 FP shall not
 * send any call status to GAP or NG Part 1 devices" (527-3 §7.4.6.4.1).
 * Measured 2026-10-28: the A540 sends PI_7 0x16 (Part 1 and 3), the C430A
 * and AS690H 0x12 (Part 1 only).
 */
#define DECT_TERM_CAP_PROFILES	11u

struct dect_term_cap {
	uint8_t present;
	uint8_t display;		/* octet 3 bits 4..1 */
	uint8_t tone;			/* octet 3 bits 7..5 */
	uint8_t profile[DECT_TERM_CAP_PROFILES];	/* bit 8 cleared */
	uint8_t n_profile;
	uint8_t ng_part1;		/* PI_7 bit 2 */
	uint8_t ng_part3;		/* PI_7 bit 3 */
};

/* 0 and *tc filled on a well-formed value, -1 otherwise (tc cleared). */
int dect_term_cap_parse(const uint8_t *v, size_t n, struct dect_term_cap *tc);

struct dect_mm {
	struct dect_mm_cfg cfg;
	enum dect_mm_state state;

	uint8_t ti_value;			/* transaction from the PP */

	/*
	 * <MM_ident.1>, the temporary-identity assignment timer. EN 300 175-5
	 * §13.2.2 and §A.2: a TPUI in our {LOCATE-ACCEPT} *starts the identity
	 * assignment procedure*, the FT shall start this timer (FT value
	 * **10 s**), stop it on {TEMPORARY-IDENTITY-ASSIGN-ACK}, and on expiry
	 * "abort the procedure and release the transaction".
	 *
	 * We ran neither until 2026-10-23: the handset's ack arrived as the
	 * unidentified `05 5d` and we ignored it, so an assignment was treated
	 * as successful the moment we sent it. That matters because we page by
	 * the assigned TPUI -- if a handset never took it, every page goes to
	 * an identity it is not listening for, which is silent and looks
	 * exactly like a handset that will not answer.
	 */
	uint64_t ident_deadline_ms;		/* 0 = not running */
	unsigned tpui_confirmed : 1;		/* the PT acked the assignment */
	unsigned tpui_timed_out : 1;		/* <MM_ident.1> expired */
	unsigned ident_pending  : 1;		/* assignment awaiting its ack */
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
	 * The identity an {ACCESS-RIGHTS-REQUEST} asked for, held here until
	 * authentication succeeds and only then copied into port_id, so a
	 * stranger with the wrong PIN cannot displace the identity we hold.
	 */
	uint8_t pend_id[DECT_MM_PORTID_MAX];
	size_t  pend_id_len;

	/*
	 * Every handset the base holds, and the tag of the one this link has
	 * adopted (-1: none, port_id is whatever the owner put there).
	 */
	struct dect_mm_held held[DECT_MM_HELD_MAX];
	unsigned n_held;
	int      adopted;

	/*
	 * What the handset said about itself in its {LOCATE-REQUEST} or
	 * {ACCESS-RIGHTS-REQUEST}, for diagnostics: <<MODEL-IDENTIFIER>>
	 * (the C4 sends 0b 74 08; the A540 sends none) and its
	 * <<CODEC-LIST>> contents. Empty when absent.
	 */
	uint8_t peer_model[8];
	size_t  peer_model_len;
	uint8_t peer_codecs[16];
	size_t  peer_codecs_len;
	struct dect_term_cap peer_cap;	/* the last one it sent */

	/* A held {LOCATE-REQUEST}, see cfg.defer_unknown. */
	uint8_t locate_pending;
	uint8_t locate_ti;
	uint8_t locate_id[DECT_MM_PORTID_MAX];
	size_t  locate_id_len;
	uint8_t locate_la[8];
	size_t  locate_la_len;

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

/*
 * Hold a subscription for another handset on this link: its identity, its
 * chip slot, its TPUI (NULL or all-zero derives one from the slot) and a tag
 * the owner chooses. Returns 0, or -1 if the table is full or the entry
 * invalid. dect_mm_init() empties the table, so hold them after each init.
 *
 * A base serves every handset subscribed to it. With port_id alone it knew
 * one: on 2026-10-25 a daemon serving the AS690H turned away the A540, which
 * IS subscribed here, twenty times a minute (`{LOCATE-REJECT}`, then every
 * list-access call), and the A540 lost its name and kept resynchronising.
 */
int dect_mm_hold(struct dect_mm *mm, const uint8_t *id, size_t id_len,
		 uint8_t slot, const uint8_t tpui[3], int tag);

/*
 * Make this link serve the handset with this identity, if the base holds
 * it: port_id, the slot and the TPUI become that handset's, and
 * mm->adopted its tag. Returns 1 if the identity is held (or is already
 * port_id), 0 if the base does not know it. The {LOCATE-REQUEST} gate uses
 * it, and dect_link calls it for a {CC-SETUP}'s identity before arming the
 * CC gate, so the two gates admit the same handsets.
 */
int dect_mm_adopt(struct dect_mm *mm, const uint8_t *id, size_t id_len);

/*
 * A held {LOCATE-REQUEST}: returns 1 and its identity, or 0 if none is
 * pending (cfg.defer_unknown).
 */
int dect_mm_locate_pending(const struct dect_mm *mm, const uint8_t **id,
			   size_t *id_len);

/*
 * Answer the held request: {LOCATE-ACCEPT} if its identity is held now
 * (dect_mm_hold() it first), otherwise {LOCATE-REJECT} "IPUI unknown".
 * Returns 1 accepted, -1 rejected, 0 if nothing was pending.
 */
int dect_mm_locate_resume(struct dect_mm *mm);

/* The configured subscription slot, with the default already applied. */
uint8_t dect_mm_sub_slot(const struct dect_mm *mm);

/*
 * Build this FP's <<FIXED-IDENTITY>> contents (type 32 PARK, whole PARK) into
 * `out`. Returns the length written, or 0 if `cap` is too small. CC needs the
 * same bytes MM sends in {ACCESS-RIGHTS-ACCEPT}, so there is one encoding and
 * one place that knows it.
 */
size_t dect_mm_fixed_identity(const struct dect_mm *mm, uint8_t *out,
			      size_t cap);

/*
 * The three content octets of the TPUI we assign, with the default already
 * applied. Anything that pages this handset must use its low 16 bits, so take
 * them from here rather than rebuilding them from the slot.
 */
void dect_mm_tpui(const struct dect_mm *mm, uint8_t out[3]);

/*
 * Run the MM timers. Currently only <MM_ident.1>; safe to call every pass.
 */
void dect_mm_tick(struct dect_mm *mm, uint64_t now_ms);

/* Earliest MM deadline, or UINT64_MAX when no timer is running. */
uint64_t dect_mm_next_deadline(const struct dect_mm *mm);

/*
 * Has the handset acknowledged the TPUI we assigned it?
 *
 * Paging uses that TPUI, so an unconfirmed assignment is a page the handset
 * may never hear. Returns 0 before any assignment, after a timeout, and
 * while the acknowledgement is still outstanding.
 */
int dect_mm_tpui_confirmed(const struct dect_mm *mm);

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

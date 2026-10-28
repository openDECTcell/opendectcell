/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DECT NWK layer message codec (S-format header + information elements).
 * Specification: ETSI EN 300 175-5 V1.x, clauses 6 and 7.
 *
 * A NWK message in S-format starts with a 2-octet header:
 *   octet 1: F(bit8) | transaction value TV(bits7-5) | protocol discriminator PD(bits4-1)
 *   octet 2: message type (bits 7-1; bit 8 reserved 0)
 * followed by information elements. A variable-length IE is
 *   [ id (bit8=0) | length | contents(length) ]; a fixed/single-octet IE has
 *   bit 8 = 1 and no length octet. This codec builds and reads variable-length
 *   IEs (all the IEs the MM registration procedure needs) and skips single-octet
 *   fixed IEs when scanning.
 *
 * Re-implemented from the ETSI text as GPL-2.0-or-later; libdect (GPL-2.0-only)
 * used only as a cross-check, no code copied. Pure functions, no I/O.
 */
#ifndef OPENDECTCELL_DECT_NWK_H
#define OPENDECTCELL_DECT_NWK_H

#include <stddef.h>
#include <stdint.h>

/* Protocol discriminators (§7.2). */
#define DECT_PD_LCE	0x0u
#define DECT_LCE_PAGE_RESPONSE	0x71u	/* EN 300 175-5 §6.3.7.1 */
#define DECT_LCE_PAGE_REJECT	0x72u	/* TS 102 527-1 table 20 */
#define DECT_PD_CC	0x3u
#define DECT_PD_CISS	0x4u
#define DECT_PD_MM	0x5u
#define DECT_PD_CLMS	0x6u
#define DECT_PD_COMS	0x7u

/* Transaction flag (octet 1, bit 8): 0 = from originator, 1 = from responder. */
#define DECT_TI_ORIGINATOR	0u
#define DECT_TI_RESPONDER	1u

/* MM message types (§7.4.x; octet 2). */
#define DECT_MM_AUTHENTICATION_REQUEST	0x40u
#define DECT_MM_AUTHENTICATION_REPLY	0x41u
#define DECT_MM_KEY_ALLOCATE		0x42u
#define DECT_MM_ACCESS_RIGHTS_REQUEST	0x44u
#define DECT_MM_ACCESS_RIGHTS_ACCEPT	0x45u
#define DECT_MM_ACCESS_RIGHTS_REJECT	0x47u
#define DECT_MM_CIPHER_REQUEST		0x4Cu
#define DECT_MM_LOCATE_REQUEST		0x54u
#define DECT_MM_LOCATE_ACCEPT		0x55u
#define DECT_MM_LOCATE_REJECT		0x57u
/*
 * {TEMPORARY-IDENTITY-ASSIGN-ACK}: the PT confirming a TPUI we assigned.
 *
 * Identified 2026-10-23 from three agreeing lines of evidence, because the
 * spec's message-type table does not survive pdftotext (two reconstructions
 * of it failed their own calibration against values we already use):
 *   - our corpus holds the message as exactly `05 5d` -- PD 5 (MM), two
 *     octets, no information elements;
 *   - EN 300 175-5 table 62 gives {TEMPORARY-IDENTITY-ASSIGN-ACK} as P=>F
 *     with only PD, TI and message type mandatory, i.e. two octets with no
 *     elements. It is the only MM message of that shape in this exchange;
 *   - it arrives immediately after our {LOCATE-ACCEPT}, which carries a
 *     TPUI -- and §13.2.2 says a TPUI in {LOCATE-ACCEPT} starts exactly this
 *     assignment procedure.
 * The numeric value is not read off the coding table; treat it as very
 * probable rather than certain, and say so if it ever misbehaves.
 */
#define DECT_MM_TEMP_IDENTITY_ASSIGN_ACK	0x5Du

/* CC (Call Control) message types (§7.4.1; octet 2). */
#define DECT_CC_ALERTING		0x01u
#define DECT_CC_CALL_PROC		0x02u
#define DECT_CC_SETUP			0x05u
#define DECT_CC_CONNECT			0x07u
#define DECT_CC_SETUP_ACK		0x0Du
#define DECT_CC_CONNECT_ACK		0x0Fu
#define DECT_CC_FACILITY		0x62u	/* [CR A-0055] supplementary services */
#define DECT_CC_RELEASE			0x4Du
#define DECT_CC_RELEASE_COM		0x5Au
#define DECT_CC_IWU_INFO		0x60u
#define DECT_CC_NOTIFY			0x6Eu
#define DECT_CC_INFO			0x7Bu

/* Variable-length IE identifiers (§7.6). */
#define DECT_IE_INFO_TYPE		0x01u
#define DECT_IE_IDENTITY_TYPE		0x02u
#define DECT_IE_PORTABLE_IDENTITY	0x05u
#define DECT_IE_FIXED_IDENTITY		0x06u
#define DECT_IE_LOCATION_AREA		0x07u
#define DECT_IE_NWK_ASSIGNED_IDENTITY	0x09u
/*
 * <<MULTI-KEYPAD>>: a list of DECT standard characters (EN 300 175-5 §7.7.27,
 * annex D -- closely based on IA5), variable length, carried in {CC-INFO}.
 * This is how a handset sends the number it dialled.
 *
 * The identifier is read off our own captures rather than off the spec's
 * coding table, which pdftotext renders column-wise and which I reconstructed
 * wrongly once: seven runs in the corpus hold
 *   03 7b 2c 05 31 32 33 34 35
 * = CC {CC-INFO} <<0x2C>> len 5 "12345". Bit 8 = 0 marks a variable-length
 * element, which 0x2C satisfies.
 */
#define DECT_IE_MULTI_KEYPAD		0x2Cu
#define DECT_IE_AUTH_TYPE		0x0Au
#define DECT_IE_ALLOCATION_TYPE		0x0Bu
#define DECT_IE_RAND			0x0Cu
#define DECT_IE_RES			0x0Du
#define DECT_IE_RS			0x0Eu
#define DECT_IE_REJECT_REASON		0x60u
#define DECT_IE_TERMINAL_CAPABILITY	0x63u	/* EN 300 175-5 §7.7.41 */
#define DECT_IE_DURATION		0x72u
#define DECT_IE_IWU_TO_IWU		0x77u
#define DECT_IE_CODEC_LIST		0x7Cu	/* [CR EN 300 175-5 7.7.54] */
#define DECT_IE_MODEL_IDENTIFIER	0x78u
#define DECT_IE_ESCAPE_TO_PROPRIETARY	0x7Bu
#define DECT_IE_CIPHER_INFO		0x19u
#define DECT_IE_CALL_ATTRIBUTES		0x13u	/* CC, variable length */
#define DECT_IE_CONNECTION_ATTRIBUTES	0x17u	/* CC, variable length */

/*
 * Double-octet fixed IEs (§7.5.3): first octet 0xE0 | sub-id, then one value
 * octet. Used by CC. dect_nwk_find_ie() skips these correctly; they are not
 * built through dect_nwk_put_ie (which is for variable-length IEs).
 */
#define DECT_IE_BASIC_SERVICE		0xE0u	/* + {call class, service} */
#define DECT_IE_SENDING_COMPLETE	0xA1u	/* single octet, §7.6.2 */
/*
 * <<BASIC-SERVICE>> value octet (§7.6.4): high nibble call class, low nibble
 * basic service. Call classes, which we had only half-written down:
 *
 *   0x2 LiA service setup   0x3 ULE service     0x4 message call
 *   0x7 DECT/ISDN IIP       0x8 NORMAL call     0x9 internal call
 *   0xA emergency           0xB service call    0xC external handover
 *   0xD supplementary svc   0xE OA&M
 *
 * Services: 0x0 basic speech defaults, 0x8 wideband speech defaults, 0x9/0xA
 * SUOTA, 0xB DTAM wideband, 0xF other.
 *
 * So our ring SETUP's 0x88 is "normal call, wideband speech", which is right,
 * and the class-3 SETUP that arrived silently in v33 was a ULE service call.
 * A Gigaset A540's own 0xE0 0x28 sessions are class 2 -- CAT-iq LIST ACCESS,
 * not voice calls -- which is what the <<IWU-TO-IWU>> dialog in dect_iwu.c
 * carries. See docs/tasks-2026-10-22.md.
 */
#define DECT_IE_RELEASE_REASON		0xE2u	/* + reason code */
/*
 * <<SIGNAL>> (§7.6.8): tells the portable part which tone or alerting pattern
 * to produce. Values 0x40..0x47 are "alerting on, pattern 0..7", 0x48 is the
 * continuous pattern and 0x4F is "alerting off"; 0x00..0x08 are tones.
 * Without it the handset falls back on whatever its call class implies, which
 * is why a call-class-3 SETUP arrived silently (v33).
 */
#define DECT_IE_SIGNAL			0xE4u	/* + signal value */
#define DECT_SIGNAL_ALERT_PATTERN0	0x40u	/* GAP table 23: internal call */
#define DECT_SIGNAL_ALERT_PATTERN1	0x41u	/* GAP table 23: external call */
#define DECT_SIGNAL_ALERT_OFF		0x4Fu

struct dect_nwk_hdr {
	uint8_t pd;		/* protocol discriminator */
	uint8_t ti_flag;	/* transaction flag (F bit) */
	uint8_t ti_value;	/* transaction value 0..6 */
	uint8_t msg_type;
};

/*
 * Parse the 2-octet header. On success returns 0, fills h, and points *ies /
 * *ies_len at the IE block (may be empty). Returns -1 if len < 2.
 */
int dect_nwk_parse(const uint8_t *msg, size_t len, struct dect_nwk_hdr *h,
		   const uint8_t **ies, size_t *ies_len);

/* Write the 2-octet header at out. Returns 2, or 0 if cap < 2. */
size_t dect_nwk_build_hdr(uint8_t *out, size_t cap, uint8_t pd, uint8_t ti_flag,
			  uint8_t ti_value, uint8_t msg_type);

/*
 * Append a variable-length IE [id, len, data] at out+off. Returns the new
 * offset, or 0 on overflow or len > 255. data may be NULL when len is 0.
 */
size_t dect_nwk_put_ie(uint8_t *out, size_t cap, size_t off, uint8_t id,
		       const uint8_t *data, size_t len);

/*
 * Append a double-octet fixed IE [id, value] at out+off, for the 0xE0 family
 * above. Returns the new offset, or 0 on overflow.
 */
size_t dect_nwk_put_fixed_ie(uint8_t *out, size_t cap, size_t off, uint8_t id,
			     uint8_t value);

/*
 * Find the first variable-length IE with the given id in an IE block. Returns a
 * pointer to its contents and writes the content length to *out_len, or NULL if
 * absent. Single-octet fixed IEs (bit 8 = 1) are skipped.
 */
/*
 * Find a fixed-format element and return its value octet: double-octet
 * (0xE0-0xEF: <<BASIC-SERVICE>>, <<RELEASE-REASON>>, ...) its value, a
 * single-octet one (<<SENDING-COMPLETE>> 0xA1, ...) 0; -1 if absent. Walks
 * the list the same way dect_nwk_find_ie() does, so a byte inside another
 * element's payload cannot be mistaken for an element id.
 */
int dect_nwk_find_fixed_ie(const uint8_t *ies, size_t ies_len, uint8_t id);

const uint8_t *dect_nwk_find_ie(const uint8_t *ies, size_t ies_len, uint8_t id,
				size_t *out_len);

#endif /* OPENDECTCELL_DECT_NWK_H */

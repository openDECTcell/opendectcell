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

/* CC (Call Control) message types (§7.4.1; octet 2). */
#define DECT_CC_ALERTING		0x01u
#define DECT_CC_CALL_PROC		0x02u
#define DECT_CC_SETUP			0x05u
#define DECT_CC_CONNECT			0x07u
#define DECT_CC_SETUP_ACK		0x0Du
#define DECT_CC_CONNECT_ACK		0x0Fu
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
#define DECT_IE_AUTH_TYPE		0x0Au
#define DECT_IE_ALLOCATION_TYPE		0x0Bu
#define DECT_IE_RAND			0x0Cu
#define DECT_IE_RES			0x0Du
#define DECT_IE_RS			0x0Eu
#define DECT_IE_REJECT_REASON		0x60u
#define DECT_IE_DURATION		0x72u
#define DECT_IE_IWU_TO_IWU		0x77u
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
#define DECT_IE_RELEASE_REASON		0xE2u	/* + reason code */

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
const uint8_t *dect_nwk_find_ie(const uint8_t *ies, size_t ies_len, uint8_t id,
				size_t *out_len);

#endif /* OPENDECTCELL_DECT_NWK_H */

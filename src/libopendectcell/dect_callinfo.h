/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * <<CALL-INFORMATION>>: line identifier, line type, call identifier and call
 * status. Specification: ETSI EN 300 175-5 §7.7.56 (encoding) and
 * TS 102 527-3 §7.4.5-§7.4.6 (CAT-iq 2.0 use).
 *
 * The profile makes the line identifier the thing that tells a handset an
 * incoming call is external: the FP shall send one in every incoming external
 * CC-SETUP, even with a single line, and an internal call carries none. A
 * SETUP without this IE is therefore shaped like an internal call. See
 * docs/call-g7.md, "Intern": the incoming SETUP carries no line identifier.
 *
 * Each subrecord is optional and must be asked for, so an unset structure
 * emits nothing and a SETUP stays byte-identical to the one proved at G7.
 * Values are refused rather than guessed: only single-octet identifier
 * values (the standard's continuation rule says which octet is last, not how
 * a longer value is assembled), only defined call statuses, and line type
 * flags only together with the line they describe.
 *
 * Re-implemented from the public ETSI text as GPL-2.0-or-later. Pure
 * functions, no I/O.
 */
#ifndef OPENDECTCELL_DECT_CALLINFO_H
#define OPENDECTCELL_DECT_CALLINFO_H

#include <stddef.h>
#include <stdint.h>

#define DECT_IE_CALL_INFORMATION	0x7Eu

/* Identifier type (octet 3) and the subtypes we encode (octet 3a). */
#define DECT_CALLINFO_TYPE_LINE		0x00u
#define DECT_CALLINFO_TYPE_CALL		0x01u
#define DECT_CALLINFO_TYPE_STATUS	0x02u
#define DECT_CALLINFO_SUB_LINE_EXTERNAL	0x00u
#define DECT_CALLINFO_SUB_LINE_TYPE	0x05u
#define DECT_CALLINFO_SUB_CALL_ID	0x00u
#define DECT_CALLINFO_SUB_CALL_STATUS	0x01u
#define DECT_CALLINFO_SUB_STATUS_REASON	0x02u	/* EN 300 175-5 §7.7.56 */

/* Call status reason values (EN 300 175-5 §7.7.56). */
enum dect_callinfo_reason {
	DECT_CSR_SYSTEM_BUSY		= 0x00,
	DECT_CSR_LINE_IN_USE		= 0x01,
	DECT_CSR_CODE_NOT_SUPPORTED	= 0x02,
	DECT_CSR_CODE_FAILED		= 0x03,
	DECT_CSR_USER_BUSY		= 0x10,
	DECT_CSR_NUMBER_NOT_AVAILABLE	= 0x11,
};

/*
 * Line id 127 is "None": the line is not yet known. A PP sends it to ask for
 * FP-managed line selection; an FP presenting an incoming call knows its line,
 * so it is not ours to send.
 */
#define DECT_CALLINFO_LINE_NONE		0x7Fu

/* Line type information flags (value octet bits 1 and 2). */
#define DECT_CALLINFO_NDT_DELAY		0x01u	/* significant delay, e.g. VoIP */
#define DECT_CALLINFO_SCT_INBAND	0x02u	/* second calls in-band, e.g. PSTN */

/* Call status values (§7.7.56, call status subtype). */
enum dect_callinfo_status {
	DECT_CS_IDLE			= 0x00,
	DECT_CS_CALL_SETUP		= 0x01,
	DECT_CS_CALL_SETUP_ACK		= 0x02,
	DECT_CS_CALL_PROC		= 0x03,
	DECT_CS_CALL_ALERTING		= 0x04,
	DECT_CS_CALL_CONNECT		= 0x05,
	DECT_CS_CALL_DISCONNECTING	= 0x06,
	DECT_CS_CALL_HOLD		= 0x09,
	DECT_CS_CALL_INTERCEPTED	= 0x0A,
	DECT_CS_CONFERENCE_CONNECT	= 0x0B,
	DECT_CS_CALL_UNDER_TRANSFER	= 0x0C,
	DECT_CS_CALL_REMOTE_HOLD	= 0x0D,
	DECT_CS_CALL_REMOTE_CONNECT	= 0x0E,
	DECT_CS_SCREENING_SETUP		= 0x0F,
	DECT_CS_SCREENING_CONNECT	= 0x10,
};

struct dect_callinfo {
	uint8_t has_line;	/* line identifier for external call */
	uint8_t line_id;	/* 0..126 */
	uint8_t has_line_type;	/* line type information; needs has_line */
	uint8_t line_type;	/* DECT_CALLINFO_NDT_* | DECT_CALLINFO_SCT_* */
	uint8_t has_call_id;
	uint8_t call_id;	/* 0..127, FP-assigned, system-wide */
	uint8_t has_call_status;
	uint8_t call_status;	/* enum dect_callinfo_status */
	uint8_t has_status_reason;	/* needs has_call_status */
	uint8_t status_reason;	/* enum dect_callinfo_reason */
};

/* Clear to "nothing to send". */
void dect_callinfo_init(struct dect_callinfo *ci);

/*
 * Read a received <<CALL-INFORMATION>> value (without tag and length):
 * {type, subtype, value...} records, each value ending at the octet with
 * bit 8 set. Fills the subrecords we know (line id incl. 'None' 7FH, line
 * type, call id, call status) and skips the rest. 0, or -1 if a record is
 * cut short (*ci keeps what came before).
 */
int dect_callinfo_parse(const uint8_t *v, size_t n, struct dect_callinfo *ci);

/*
 * Non-zero if `ci` can be encoded: every present value is in range, the call
 * status is a defined one, and line type information is only present with a
 * line identifier. An empty structure is valid (it encodes to nothing).
 */
int dect_callinfo_valid(const struct dect_callinfo *ci);

/*
 * Append <<CALL-INFORMATION>> at out+off, with the present subrecords in the
 * order of TS 102 527-3 Figure 42:
 *
 *	7E  L  [00 00 line] [00 05 type] [01 00 call] [02 01 status]
 *
 * each value octet with bit 8 set (single-octet value). Returns the new
 * offset; `off` unchanged when nothing is present; 0 when the IE does not fit
 * or `ci` is not valid.
 */
size_t dect_callinfo_put(uint8_t *out, size_t cap, size_t off,
			 const struct dect_callinfo *ci);

#endif /* OPENDECTCELL_DECT_CALLINFO_H */

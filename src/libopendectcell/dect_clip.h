/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Caller identity: CALLING-PARTY-NUMBER and CALLING-PARTY-NAME.
 * Specification: ETSI EN 300 175-5 §7.7.9 and §7.7.53. Both are ordinary
 * variable-length NWK information elements, not vendor extensions.
 *
 * The identity is held as typed fields rather than as a loose byte string,
 * because the hard part of CLIP is not the encoding -- it is not leaking a
 * number the caller withheld. A missing string is not privacy metadata: an
 * absent number can mean withheld, unavailable or simply unknown, and those
 * must stay distinguishable all the way up to the decision to disclose.
 *
 * So this module refuses rather than guesses:
 *   - presentation must be stated explicitly; "not stated" omits the content
 *   - anything but "presentation allowed" omits the content as well, which is
 *     what the standard advises when there is nothing we may show
 *   - screening is never claimed as verified unless the caller says so
 *   - the name is encoded once and truncated only at a complete code point
 *
 * Encoding the restricted case is deliberately not supported: the one stock
 * metadata value we have for it (`0x65`) is ambiguous about which bits carry
 * presentation, and omission is the safe reading either way. See the comment
 * on dect_clip_put_name().
 *
 * Re-implemented from the public ETSI text as GPL-2.0-or-later. Pure
 * functions, no I/O.
 */
#ifndef OPENDECTCELL_DECT_CLIP_H
#define OPENDECTCELL_DECT_CLIP_H

#include <stddef.h>
#include <stdint.h>

/* Variable-length IE identifiers (§7.6). */
#define DECT_IE_CALLING_PARTY_NUMBER	0x6Cu
#define DECT_IE_CALLING_PARTY_NAME	0x6Du

/* Our own storage bounds, not protocol limits. The IE length octet allows far
 * more; stock branches were seen capping a number at 21 or 23 bytes and
 * computing a name budget of at most 58, none of which is universal. */
#define DECT_CLIP_NUMBER_MAX	32u
#define DECT_CLIP_NAME_MAX	64u

/* Type of number (§7.7.9, number descriptor bits 7-5). */
enum dect_clip_ton {
	DECT_CLIP_TON_UNKNOWN		= 0,
	DECT_CLIP_TON_INTERNATIONAL	= 1,
	DECT_CLIP_TON_NATIONAL		= 2,
	DECT_CLIP_TON_NETWORK_SPECIFIC	= 3,
	DECT_CLIP_TON_SUBSCRIBER	= 4,
	DECT_CLIP_TON_ABBREVIATED	= 6,
};

/* Numbering plan identification (number descriptor bits 4-1). */
enum dect_clip_npi {
	DECT_CLIP_NPI_UNKNOWN		= 0,
	DECT_CLIP_NPI_E164		= 1,
	DECT_CLIP_NPI_X121		= 3,
	DECT_CLIP_NPI_TCPIP		= 7,
	DECT_CLIP_NPI_NATIONAL		= 8,
	DECT_CLIP_NPI_PRIVATE		= 9,
	DECT_CLIP_NPI_SIP_FROM		= 10,
	DECT_CLIP_NPI_INTERNET_CHAR	= 11,
	DECT_CLIP_NPI_LAN_MAC		= 12,
	DECT_CLIP_NPI_X400		= 13,
	DECT_CLIP_NPI_PROFILE_ALPHA	= 14,
	DECT_CLIP_NPI_EXTENSION		= 15,
};

/*
 * Presentation. DECT_CLIP_PRES_UNSET is ours, not the standard's: it marks
 * "upstream did not tell us", which must never be silently read as allowed.
 */
enum dect_clip_presentation {
	DECT_CLIP_PRES_ALLOWED		= 0,
	DECT_CLIP_PRES_RESTRICTED	= 1,
	DECT_CLIP_PRES_UNAVAILABLE	= 2,
	DECT_CLIP_PRES_RESERVED		= 3,	/* name IE: handset locator */
	DECT_CLIP_PRES_UNSET		= 0xFF,
};

/* Screening indicator (bits 2-1 of the second descriptor / the metadata). */
enum dect_clip_screening {
	DECT_CLIP_SCR_USER_UNSCREENED	= 0,
	DECT_CLIP_SCR_USER_PASSED	= 1,
	DECT_CLIP_SCR_USER_FAILED	= 2,
	DECT_CLIP_SCR_NETWORK		= 3,
};

/* Used alphabet for the name (metadata bits 5-3). */
enum dect_clip_alphabet {
	DECT_CLIP_ALPHABET_DECT		= 0,	/* EN 300 175-5 Annex D */
	DECT_CLIP_ALPHABET_UTF8		= 1,
	DECT_CLIP_ALPHABET_NETWORK	= 7,
};

struct dect_clip {
	/* Number. `number` is IA5 text, NUL-terminated, not packed BCD. */
	char	number[DECT_CLIP_NUMBER_MAX + 1];
	uint8_t	number_ton;		/* enum dect_clip_ton */
	uint8_t	number_npi;		/* enum dect_clip_npi */
	uint8_t	number_presentation;	/* enum dect_clip_presentation */
	uint8_t	number_screening;	/* enum dect_clip_screening */

	/* Name, in the declared alphabet. UTF-8 is held as UTF-8. */
	char	name[DECT_CLIP_NAME_MAX + 1];
	uint8_t	name_presentation;
	uint8_t	name_screening;
	uint8_t	name_alphabet;		/* enum dect_clip_alphabet */

	/*
	 * Placement hint for a bench experiment, not a property of the
	 * identity: emit the two IEs before <<BASIC-SERVICE>> rather than
	 * after it, to test whether a handset (the FRITZ!Fon C4) reads them
	 * there. The variable-length elements are in ascending identifier
	 * order either way, as EN 300 175-5 §7.6 requires; only their place
	 * relative to the fixed-length <<BASIC-SERVICE>> moves. 0, the
	 * default, is the standard order.
	 */
	uint8_t	emit_before_basic_service;
};

/*
 * Clear an identity to "nothing known": both strings empty and both
 * presentations DECT_CLIP_PRES_UNSET, so nothing is emitted until something
 * is deliberately set.
 */
void dect_clip_init(struct dect_clip *id);

/*
 * Set the number. Returns 0, or -1 if it is too long for DECT_CLIP_NUMBER_MAX
 * or contains anything outside printable IA5 (0x20..0x7E). DEL (0x7F) is
 * rejected: the stock converter does not filter it, so we must.
 */
int dect_clip_set_number(struct dect_clip *id, const char *number,
			 enum dect_clip_ton ton, enum dect_clip_npi npi,
			 enum dect_clip_presentation pres,
			 enum dect_clip_screening scr);

/*
 * Set the name. For DECT_CLIP_ALPHABET_UTF8 the string must be well-formed
 * UTF-8; for DECT_CLIP_ALPHABET_DECT it must be printable ASCII, because
 * silently reinterpreting UTF-8 bytes as a different alphabet is how mojibake
 * reaches a handset. Returns 0, or -1 on a malformed string, a control
 * character, or more than DECT_CLIP_NAME_MAX bytes.
 */
int dect_clip_set_name(struct dect_clip *id, const char *name,
		       enum dect_clip_alphabet alphabet,
		       enum dect_clip_presentation pres,
		       enum dect_clip_screening scr);

/*
 * Append CALLING-PARTY-NUMBER at out+off, as
 *
 *	6C  (2+N)  (ext=0 | TON | NPI)  (ext=1 | pres | screening)  number[N]
 *
 * Returns the new offset. Omitting the IE is not a failure and returns `off`
 * unchanged -- that happens when there is no number, or when presentation is
 * anything other than allowed. 0 means the IE did not fit; since an IE is
 * only ever appended after a 2-octet NWK header, a real `off` is never 0.
 */
size_t dect_clip_put_number(uint8_t *out, size_t cap, size_t off,
			    const struct dect_clip *id);

/*
 * Append CALLING-PARTY-NAME at out+off, as
 *
 *	6D  (1+M)  (pres | alphabet | screening)  name[M]
 *
 * `budget` caps the encoded name bytes (0 = no cap beyond the buffer). The
 * name is truncated only at a complete code point, so a budget that would
 * split a multi-byte character drops that whole character rather than
 * emitting a stray lead or continuation byte. The length octet is recomputed
 * from the bytes actually written. Same return convention as above.
 *
 * Only presentation "allowed" is encoded. The stock metadata value seen for a
 * restricted name (`0x65`) is self-inconsistent about whether presentation
 * sits in bits 7-6 or 8-7 -- under the field layout used here it decodes as
 * the reserved value, not as restricted -- so encoding restriction from it
 * would be a guess. Omission is correct under either reading.
 */
size_t dect_clip_put_name(uint8_t *out, size_t cap, size_t off,
			  const struct dect_clip *id, size_t budget);

/*
 * A number for the log without its middle (API 156): the first two and the last two characters, the
 * rest as '*'; a number of four characters or fewer entirely as '*'. Returns the length written.
 */
size_t dect_mask_number(const char *s, size_t len, char *out, size_t cap);

#endif /* OPENDECTCELL_DECT_CLIP_H */

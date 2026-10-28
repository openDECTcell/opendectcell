// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DECT NWK message codec. See dect_nwk.h.
 * ETSI EN 300 175-5 V1.x §6-7.
 */
#include "dect_nwk.h"

#include <string.h>

#define S_F_SHIFT	7u	/* octet 1 bit 8: transaction flag */
#define S_TV_SHIFT	4u	/* octet 1 bits 7-5: transaction value */
#define S_PD_MASK	0x0Fu	/* octet 1 bits 4-1: protocol discriminator */
#define S_MSG_MASK	0x7Fu	/* octet 2 bits 7-1: message type */
#define IE_FIXED	0x80u	/* IE octet bit 8: fixed-format IE (no length octet) */
#define IE_DOUBLE_MASK	0xF0u	/* fixed IEs with this high nibble ... */
#define IE_DOUBLE	0xE0u	/* ... are double-octet elements (2 bytes) */

int dect_nwk_parse(const uint8_t *msg, size_t len, struct dect_nwk_hdr *h,
		   const uint8_t **ies, size_t *ies_len)
{
	if (len < 2)
		return -1;

	h->ti_flag  = (msg[0] >> S_F_SHIFT) & 0x01u;
	h->ti_value = (msg[0] >> S_TV_SHIFT) & 0x07u;
	h->pd       = msg[0] & S_PD_MASK;
	h->msg_type = msg[1] & S_MSG_MASK;
	/*
	 * TV 7 announces an extended transaction value in octet 1a (§7.3).
	 * Without extended TVs the message "shall be ignored" (§17.3.1); we
	 * used to read octet 1a as the message type (spec sweep, E3).
	 */
	if (h->ti_value == 7u)
		return -1;
	*ies     = msg + 2;
	*ies_len = len - 2;
	return 0;
}

size_t dect_nwk_build_hdr(uint8_t *out, size_t cap, uint8_t pd, uint8_t ti_flag,
			  uint8_t ti_value, uint8_t msg_type)
{
	if (cap < 2)
		return 0;

	out[0] = (uint8_t)(((ti_flag & 0x01u) << S_F_SHIFT) |
			   ((ti_value & 0x07u) << S_TV_SHIFT) |
			   (pd & S_PD_MASK));
	out[1] = (uint8_t)(msg_type & S_MSG_MASK);
	return 2;
}

size_t dect_nwk_put_ie(uint8_t *out, size_t cap, size_t off, uint8_t id,
		       const uint8_t *data, size_t len)
{
	if (len > 0xFFu || off + 2 + len > cap)
		return 0;

	out[off]     = id;		/* bit 8 = 0 => variable length */
	out[off + 1] = (uint8_t)len;
	if (len > 0)
		memcpy(&out[off + 2], data, len);
	return off + 2 + len;
}

size_t dect_nwk_put_fixed_ie(uint8_t *out, size_t cap, size_t off, uint8_t id,
			     uint8_t value)
{
	if (off + 2 > cap)
		return 0;

	out[off]     = id;		/* bit 8 = 1 => fixed length */
	out[off + 1] = value;
	return off + 2;
}

int dect_nwk_find_fixed_ie(const uint8_t *ies, size_t ies_len, uint8_t id)
{
	size_t pos = 0;

	while (pos < ies_len) {
		uint8_t ie = ies[pos];

		if (ie & IE_FIXED) {
			int dbl = (ie & IE_DOUBLE_MASK) == IE_DOUBLE;

			if (ie == id && !dbl)
				return 0;	/* single octet: present */
			if (ie == id && dbl)
				return (pos + 1 < ies_len) ? ies[pos + 1] : -1;
			pos += dbl ? 2 : 1;
			continue;
		}
		if (pos + 2 > ies_len)
			break;
		if (pos + 2 + ies[pos + 1] > ies_len)
			break;
		pos += 2 + ies[pos + 1];
	}
	return -1;
}

const uint8_t *dect_nwk_find_ie(const uint8_t *ies, size_t ies_len, uint8_t id,
				size_t *out_len)
{
	size_t pos = 0;

	while (pos < ies_len) {
		uint8_t ie = ies[pos];

		if (ie & IE_FIXED) {		/* fixed-format IE: skip it */
			/* 0xE0-0xEF are double-octet elements (BASIC-SERVICE,
			 * RELEASE-REASON, …); all other fixed IEs are one octet. */
			pos += ((ie & IE_DOUBLE_MASK) == IE_DOUBLE) ? 2 : 1;
			continue;
		}
		if (pos + 2 > ies_len)
			break;			/* truncated variable-length IE */
		{
			size_t len = ies[pos + 1];

			if (pos + 2 + len > ies_len)
				break;		/* declared length overruns */
			if (ie == id) {
				*out_len = len;
				return (len > 0) ? &ies[pos + 2] : &ies[pos + 2];
			}
			pos += 2 + len;
		}
	}
	*out_len = 0;
	return NULL;
}

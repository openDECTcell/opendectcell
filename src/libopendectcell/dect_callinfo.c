// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * <<CALL-INFORMATION>> serializer. See dect_callinfo.h.
 * ETSI EN 300 175-5 §7.7.56.
 */
#include "dect_callinfo.h"
#include "dect_nwk.h"

#include <string.h>

/* Bit 8 of an identifier value octet marks the last octet of the value. */
#define VALUE_LAST	0x80u

void dect_callinfo_init(struct dect_callinfo *ci)
{
	memset(ci, 0, sizeof(*ci));
}

static int status_defined(uint8_t s)
{
	return s <= DECT_CS_CALL_DISCONNECTING
	       || (s >= DECT_CS_CALL_HOLD && s <= DECT_CS_SCREENING_CONNECT);
}

int dect_callinfo_valid(const struct dect_callinfo *ci)
{
	if (ci->has_line && ci->line_id >= DECT_CALLINFO_LINE_NONE)
		return 0;
	if (ci->has_line_type
	    && (!ci->has_line
		|| (ci->line_type & ~(DECT_CALLINFO_NDT_DELAY
				      | DECT_CALLINFO_SCT_INBAND))))
		return 0;
	if (ci->has_call_id && ci->call_id > 0x7Fu)
		return 0;
	if (ci->has_call_status && !status_defined(ci->call_status))
		return 0;
	return 1;
}

static size_t put_sub(uint8_t *b, size_t n, uint8_t type, uint8_t sub,
		      uint8_t value)
{
	b[n++] = type;
	b[n++] = sub;
	b[n++] = (uint8_t)(VALUE_LAST | value);
	return n;
}

int dect_callinfo_parse(const uint8_t *v, size_t n, struct dect_callinfo *ci)
{
	size_t i = 0;

	dect_callinfo_init(ci);
	while (i < n) {
		uint8_t type, sub, val;
		size_t j;

		if (n - i < 3u)
			return -1;
		type = v[i];
		sub = v[i + 1u];
		for (j = i + 2u; j < n && !(v[j] & 0x80u); j++)
			;
		if (j >= n)
			return -1;		/* the value never ended */
		val = (uint8_t)(v[i + 2u] & 0x7Fu);	/* first value octet */
		if (type == DECT_CALLINFO_TYPE_LINE &&
		    sub == DECT_CALLINFO_SUB_LINE_EXTERNAL) {
			ci->has_line = 1;
			ci->line_id = val;
		} else if (type == DECT_CALLINFO_TYPE_LINE &&
			   sub == DECT_CALLINFO_SUB_LINE_TYPE) {
			ci->has_line_type = 1;
			ci->line_type = val;
		} else if (type == DECT_CALLINFO_TYPE_CALL &&
			   sub == DECT_CALLINFO_SUB_CALL_ID) {
			ci->has_call_id = 1;
			ci->call_id = val;
		} else if (type == DECT_CALLINFO_TYPE_STATUS &&
			   sub == DECT_CALLINFO_SUB_CALL_STATUS) {
			ci->has_call_status = 1;
			ci->call_status = val;
		}
		i = j + 1u;
	}
	return 0;
}

size_t dect_callinfo_put(uint8_t *out, size_t cap, size_t off,
			 const struct dect_callinfo *ci)
{
	uint8_t body[4 * 3];
	size_t n = 0;

	if (!dect_callinfo_valid(ci))
		return 0;
	if (ci->has_line)
		n = put_sub(body, n, DECT_CALLINFO_TYPE_LINE,
			    DECT_CALLINFO_SUB_LINE_EXTERNAL, ci->line_id);
	if (ci->has_line_type)
		n = put_sub(body, n, DECT_CALLINFO_TYPE_LINE,
			    DECT_CALLINFO_SUB_LINE_TYPE, ci->line_type);
	if (ci->has_call_id)
		n = put_sub(body, n, DECT_CALLINFO_TYPE_CALL,
			    DECT_CALLINFO_SUB_CALL_ID, ci->call_id);
	if (ci->has_call_status)
		n = put_sub(body, n, DECT_CALLINFO_TYPE_STATUS,
			    DECT_CALLINFO_SUB_CALL_STATUS, ci->call_status);
	if (n == 0)
		return off;
	return dect_nwk_put_ie(out, cap, off, DECT_IE_CALL_INFORMATION,
			       body, n);
}

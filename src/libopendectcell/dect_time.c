// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * <<TIME-DATE>> and the CISS {FACILITY} that carries it. See dect_time.h for
 * why, and for the clauses.
 */
#include "dect_time.h"
#include "dect_nwk.h"

#include <string.h>

/*
 * BCD, "with the nibble carrying the most significant digit transmitted in
 * bits 8-5 of each octet" (EN 300 175-5 §7.7.50, tables 77 and 78).
 */
static uint8_t bcd(uint8_t v)
{
	return (uint8_t)(((v / 10u) << 4) | (v % 10u));
}

/*
 * Time zone: "the difference, expressed in quarters of an hour, between the
 * local time and GMT. In the first of the two semi-octets, the first bit
 * represents the algebraic sign of this difference (0: positive; 1:
 * negative)."
 *
 * The first semi-octet is bits 8-5, consistent with the BCD convention the
 * same clause states for hour/minute/second, so the sign is bit 8 and the
 * tens digit occupies bits 7-5. That caps the magnitude at 79 quarter-hours
 * (19 h 45), which covers every real offset (the widest in use is 14 h).
 */
static uint8_t tz_octet(const struct dect_time *t)
{
	int16_t q = t->tz_quarters;
	uint8_t sign = 0;

	if (!t->tz_valid)
		return 0x00u;		/* "null value" -- TS 102 527-3 §7.4.2.0 */
	if (q < 0) {
		sign = 0x80u;
		q = (int16_t)-q;
	}
	return (uint8_t)(sign | bcd((uint8_t)q));
}

/* Is every field in range? Encoding refuses rather than emitting nonsense. */
static int time_valid(const struct dect_time *t)
{
	if (!t)
		return 0;
	if (t->year > 99u || t->month < 1u || t->month > 12u)
		return 0;
	if (t->day < 1u || t->day > 31u)
		return 0;
	if (t->hour > 23u || t->minute > 59u || t->second > 59u)
		return 0;
	/*
	 * Leap seconds would be second == 60. The element has no room for the
	 * idea and a handset has no use for it, so it is out of range here.
	 */
	if (t->tz_valid && (t->tz_quarters < -79 || t->tz_quarters > 79))
		return 0;
	return 1;
}

size_t dect_time_put(uint8_t *out, size_t cap, size_t off,
		     const struct dect_time *t)
{
	uint8_t body[8];

	if (!time_valid(t))
		return off;

	/* Octet 3: coding in bits 8-7, interpretation in bits 6-1. */
	body[0] = (uint8_t)((DECT_TIME_CODING_TIME_DATE << 6)
			    | DECT_TIME_INTERP_CURRENT);
	/* Octets 4-10, in the order table 77 then table 78 give them. */
	body[1] = bcd(t->year);
	body[2] = bcd(t->month);
	body[3] = bcd(t->day);
	body[4] = bcd(t->hour);
	body[5] = bcd(t->minute);
	body[6] = bcd(t->second);
	body[7] = tz_octet(t);

	return dect_nwk_put_ie(out, cap, off, DECT_IE_TIME_DATE,
			       body, sizeof(body));
}

size_t dect_time_build_facility(uint8_t *out, size_t cap,
				const struct dect_time *t)
{
	size_t off, end;

	/*
	 * PD = CISS, not CC. The message type 0x62 is the same number in both
	 * sets, which makes this an easy thing to get subtly wrong.
	 *
	 * The transaction identifier is the fixed dummy value 6. We send it
	 * with the originator flag because the FP starts this transaction.
	 */
	off = dect_nwk_build_hdr(out, cap, DECT_PD_CISS, DECT_TI_ORIGINATOR,
				 DECT_TIME_FACILITY_TI, DECT_CISS_FACILITY);
	if (!off)
		return 0;
	end = dect_time_put(out, cap, off, t);
	if (end == off)		/* the element did not go in: send nothing */
		return 0;
	return end;
}

/* An extensible value, 1 or 2 octets (TS 102 527-3 table 39 note 1). */
static size_t put_mult(uint8_t *out, size_t cap, size_t off, unsigned v)
{
	if (v < 128u) {
		if (off + 1u > cap)
			return 0;
		out[off++] = (uint8_t)(0x80u | v);
		return off;
	}
	if (v > 16383u || off + 2u > cap)
		return 0;
	out[off++] = (uint8_t)((v >> 7) & 0x7Fu);
	out[off++] = (uint8_t)(0x80u | (v & 0x7Fu));
	return off;
}

size_t dect_events_build_facility(uint8_t *out, size_t cap,
				  const struct dect_event *ev, unsigned n,
				  uint8_t line_id, uint8_t line_subtype)
{
	size_t off, len_at;
	unsigned i;

	if (!ev || !n)
		return 0;
	off = dect_nwk_build_hdr(out, cap, DECT_PD_CISS, DECT_TI_ORIGINATOR,
				 DECT_TIME_FACILITY_TI, DECT_CISS_FACILITY);
	if (!off || off + 2u > cap)
		return 0;
	/* §7.7.55.1: [7DH][L] then per event [type, bit 8 = 0: a subtype
	 * follows][subtype, bit 8 = 1][multiplicity] */
	out[off++] = DECT_IE_EVENTS_NOTIFICATION;
	len_at = off++;
	for (i = 0; i < n; i++) {
		if (ev[i].type > 0x3Fu || ev[i].subtype > 0x7Fu ||
		    off + 2u > cap)
			return 0;
		out[off++] = ev[i].type;
		out[off++] = (uint8_t)(0x80u | ev[i].subtype);
		off = put_mult(out, cap, off, ev[i].multiplicity);
		if (!off)
			return 0;
	}
	if (off - len_at - 1u > 0xFFu)
		return 0;
	out[len_at] = (uint8_t)(off - len_at - 1u);
	if (line_id != DECT_EVENT_NO_LINE) {
		/* <<CALL-INFORMATION>> after it (7EH > 7DH): {line 0, subtype,
		 * value with bit 8}; 'None' (127) is not allowed (table 13) */
		if (line_id > 126u || off + 5u > cap)
			return 0;
		out[off++] = 0x7Eu;
		out[off++] = 3u;
		out[off++] = 0x00u;
		out[off++] = line_subtype;
		out[off++] = (uint8_t)(0x80u | line_id);
	}
	return off;
}

static int unbcd(uint8_t b, uint8_t *v)
{
	if ((b >> 4) > 9u || (b & 0x0Fu) > 9u)
		return -1;
	*v = (uint8_t)((b >> 4) * 10u + (b & 0x0Fu));
	return 0;
}

int dect_time_get(const uint8_t *v, size_t n, struct dect_time *t)
{
	uint8_t q;

	if (n < 8u || (v[0] >> 6) != DECT_TIME_CODING_TIME_DATE)
		return -1;
	if (unbcd(v[1], &t->year) || unbcd(v[2], &t->month) ||
	    unbcd(v[3], &t->day) || unbcd(v[4], &t->hour) ||
	    unbcd(v[5], &t->minute) || unbcd(v[6], &t->second) ||
	    unbcd(v[7] & 0x7Fu, &q))
		return -1;
	t->tz_valid = v[7] != 0x00u;	/* 00H is the null value */
	t->tz_quarters = (int16_t)((v[7] & 0x80u) ? -(int)q : (int)q);
	return time_valid(t) ? 0 : -1;
}

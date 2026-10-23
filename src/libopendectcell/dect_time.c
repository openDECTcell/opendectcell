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

int dect_time_valid(const struct dect_time *t)
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

	if (!dect_time_valid(t))
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

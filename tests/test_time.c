// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for src/libopendectcell/dect_time.c: <<TIME-DATE>> and the CISS
 * {FACILITY} that carries it to a handset. No hardware.
 *
 * The two headline cases are the standard's own worked examples, so a pass
 * here is a pass against EN 300 175-5 rather than against our reading of it.
 */
#include "check.h"
#include "../src/libopendectcell/dect_time.h"
#include "../src/libopendectcell/dect_nwk.h"

#include <string.h>

static struct dect_time sample(void)
{
	struct dect_time t;

	memset(&t, 0, sizeof(t));
	t.year = 9; t.month = 1; t.day = 14;
	t.hour = 18; t.minute = 58; t.second = 54;
	t.tz_valid = 0;
	return t;
}

/*
 * EN 300 175-5 §7.7.50:
 *   EXAMPLE 1: "Date 14/01/2009 shall be coded with Octet 4 = 09H (year),
 *               Octet 5 = 01H (month), Octet 6 = 14H."
 *   EXAMPLE 2: "Time 18h58min54sec shall be coded with Octet 4 = 18H (hour),
 *               Octet 5 = 58H (minute), Octet 6 = 54H."
 */
static void test_the_standards_own_examples(void)
{
	struct dect_time t = sample();
	uint8_t buf[32];
	size_t n = dect_time_put(buf, sizeof(buf), 0, &t);

	CHECK_EQ(n, 10u);		/* id + length + 8 content octets */
	CHECK_EQ(buf[0], DECT_IE_TIME_DATE);
	CHECK_EQ(buf[1], 8u);
	/* Octet 3: coding 11 (time and date), interpretation 0 (current). */
	CHECK_EQ(buf[2], 0xC0u);
	CHECK_EQ(buf[3], 0x09u);	/* year  -- example 1 */
	CHECK_EQ(buf[4], 0x01u);	/* month -- example 1 */
	CHECK_EQ(buf[5], 0x14u);	/* day   -- example 1 */
	CHECK_EQ(buf[6], 0x18u);	/* hour   -- example 2 */
	CHECK_EQ(buf[7], 0x58u);	/* minute -- example 2 */
	CHECK_EQ(buf[8], 0x54u);	/* second -- example 2 */
	CHECK_EQ(buf[9], 0x00u);	/* time zone: the null value */
}

/* Sign in bit 8 of the first semi-octet, magnitude in BCD quarter-hours. */
static void test_time_zone_sign_and_magnitude(void)
{
	static const struct { int16_t q; uint8_t want; } cases[] = {
		{   0, 0x00u },		/* GMT */
		{   4, 0x04u },		/* GMT+1h  = 4 quarters */
		{   8, 0x08u },		/* GMT+2h */
		{  -4, 0x84u },		/* GMT-1h, sign set */
		{  -20, 0xA0u },	/* GMT-5h  = 20 quarters -> 0x20 | sign */
		{  23, 0x23u },		/* GMT+5h45 (Nepal) */
		{  56, 0x56u },		/* GMT+14h, the widest real offset */
		{ -56, 0xD6u },		/* and its mirror */
	};
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct dect_time t = sample();
		uint8_t buf[32];

		t.tz_valid = 1;
		t.tz_quarters = cases[i].q;
		CHECK_EQ(dect_time_put(buf, sizeof(buf), 0, &t), 10u);
		CHECK_EQ(buf[9], cases[i].want);
	}
}

/* Out of range is refused, never encoded as nonsense. */
static void test_out_of_range_is_refused(void)
{
	static const struct dect_time bad[] = {
		{ .year = 100, .month = 1, .day = 1 },
		{ .year = 0, .month = 0,  .day = 1 },	/* month 0 */
		{ .year = 0, .month = 13, .day = 1 },
		{ .year = 0, .month = 1,  .day = 0 },	/* day 0 */
		{ .year = 0, .month = 1,  .day = 32 },
		{ .year = 0, .month = 1,  .day = 1, .hour = 24 },
		{ .year = 0, .month = 1,  .day = 1, .minute = 60 },
		{ .year = 0, .month = 1,  .day = 1, .second = 60 },
		{ .year = 0, .month = 1,  .day = 1, .tz_valid = 1,
		  .tz_quarters = 80 },
		{ .year = 0, .month = 1,  .day = 1, .tz_valid = 1,
		  .tz_quarters = -80 },
	};
	size_t i;

	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		uint8_t buf[32];

		CHECK_EQ(dect_time_valid(&bad[i]), 0);
		CHECK_EQ(dect_time_put(buf, sizeof(buf), 0, &bad[i]), 0u);
		CHECK_EQ(dect_time_build_facility(buf, sizeof(buf), &bad[i]), 0u);
	}
}

/*
 * The whole message. TS 102 527-3 §7.4.2.0: "the {FACILITY} message shall be
 * used with dummy transaction identifier value 6 and the protocol
 * discriminator CISS".
 */
static void test_facility_header_is_ciss_with_ti_six(void)
{
	struct dect_time t = sample();
	struct dect_nwk_hdr h;
	const uint8_t *ies, *p;
	uint8_t buf[64];
	size_t n, ies_len, plen;

	n = dect_time_build_facility(buf, sizeof(buf), &t);
	CHECK(n > 0);
	CHECK_EQ(dect_nwk_parse(buf, n, &h, &ies, &ies_len), 0);
	CHECK_EQ(h.pd, DECT_PD_CISS);		/* NOT DECT_PD_CC */
	CHECK_EQ(h.ti_value, DECT_TIME_FACILITY_TI);
	CHECK_EQ(h.ti_value, 6u);
	CHECK_EQ(h.msg_type, DECT_CISS_FACILITY);

	p = dect_nwk_find_ie(ies, ies_len, DECT_IE_TIME_DATE, &plen);
	CHECK(p != NULL);
	CHECK_EQ(plen, 8u);
	CHECK_EQ(p[0], 0xC0u);
}

/* A buffer too small yields nothing rather than a truncated message. */
static void test_small_buffer_yields_nothing(void)
{
	struct dect_time t = sample();
	uint8_t buf[4];

	CHECK_EQ(dect_time_build_facility(buf, sizeof(buf), &t), 0u);
	CHECK_EQ(dect_time_put(buf, sizeof(buf), 0, &t), 0u);
}

int main(void)
{
	test_the_standards_own_examples();
	test_time_zone_sign_and_magnitude();
	test_out_of_range_is_refused();
	test_facility_header_is_ciss_with_ti_six();
	test_small_buffer_yields_nothing();
	return CHECK_DONE();
}

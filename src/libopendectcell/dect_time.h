/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Date and time towards the handset: the <<TIME-DATE>> element and the
 * {FACILITY} message that carries it.
 *
 * Specification: EN 300 175-5 §7.7.50 for the element, and TS 102 527-3
 * §7.4.2 ("Date and Time synchronization") and §7.4.20 ("Date and Time
 * recovery") for when and how to send it. Both are public ETSI text;
 * re-implemented here, no vendor material.
 *
 * Why this exists: a DECT base is where a handset gets its clock, and we
 * never sent one. Every timestamp a handset showed -- call lists above all --
 * was whatever it had invented. TS 102 527-3 §7.4.20.2 is explicit about the
 * FT's duty:
 *
 *   "If the system Date and Time on FT side is valid, the FT shall
 *    systematically use the 'FT initiated Date and Time synchronization
 *    procedure' after successful PT location registration, in order to send
 *    the current system 'Date and Time' to the PT."
 *
 * and it adds that the FT sends it "even if the PT does not indicate support"
 * for the recovery procedure, and "independently of the 'Clock master'
 * setting value". So this is unconditional on our side.
 *
 * The transport, per §7.4.2.0: a {FACILITY} message carrying <<TIME-DATE>>,
 * over an already established link, with **protocol discriminator CISS and
 * dummy transaction identifier value 6**. Not CC -- the message type 0x62 is
 * shared, but the PD is not.
 *
 * Pure functions, no clock and no I/O: the caller supplies the time. That
 * keeps the encoder testable against the standard's own examples and leaves
 * "what time is it" where it belongs.
 */
#ifndef OPENDECTCELL_DECT_TIME_H
#define OPENDECTCELL_DECT_TIME_H

#include <stddef.h>
#include <stdint.h>

/* <<TIME-DATE>>, variable length (EN 300 175-5 table 76: 0010 0011). */
#define DECT_IE_TIME_DATE		0x23u

/* {FACILITY} in the CISS message set (table 70: 0110 0010). */
#define DECT_CISS_FACILITY		0x62u

/*
 * "the {FACILITY} message shall be used with dummy transaction identifier
 * value 6 and the protocol discriminator CISS" -- TS 102 527-3 §7.4.2.0.
 */
#define DECT_TIME_FACILITY_TI		6u

/* Coding field, octet 3 bits 8-7 (§7.7.50). */
#define DECT_TIME_CODING_TIME		0x1u
#define DECT_TIME_CODING_DATE		0x2u
#define DECT_TIME_CODING_TIME_DATE	0x3u	/* the default, and what we send */

/* Interpretation field, octet 3 bits 6-1. */
#define DECT_TIME_INTERP_CURRENT	0x00u	/* the current time/date */
#define DECT_TIME_INTERP_DURATION	0x01u
#define DECT_TIME_INTERP_REQUEST	0x04u	/* request to send current time/date */
#define DECT_TIME_INTERP_NO_VALID	0x05u	/* no valid time/date available */

/*
 * A wall-clock instant. `year` is the last two digits, as the element carries
 * (§7.7.50: "Only the last 2 digits of the year shall be used").
 *
 * `tz_quarters` is the offset from GMT in quarter-hours, the element's own
 * unit -- +60 is GMT+15h, -4 is GMT-1h. Set `tz_valid` to 0 when the offset
 * is not known; §7.4.2.0 says the field "shall be set to null value" then,
 * which we encode as zero.
 */
struct dect_time {
	uint8_t year;		/* 0..99 */
	uint8_t month;		/* 1..12 */
	uint8_t day;		/* 1..31 */
	uint8_t hour;		/* 0..23, 24-hour clock */
	uint8_t minute;		/* 0..59 */
	uint8_t second;		/* 0..59 */
	int16_t tz_quarters;	/* -79..79, quarter-hours from GMT */
	uint8_t tz_valid;	/* 0 => send the null value */
};

/*
 * Read a received <<TIME-DATE>> value (without tag and length): the 8-octet
 * time-and-date form we send ourselves. 0 and *t filled if it is one and in
 * range, else -1. A handset sends it in TS 102 527-3 §7.4.2.2.
 */
int dect_time_get(const uint8_t *v, size_t n, struct dect_time *t);

/*
 * Append <<TIME-DATE>> with coding "time and date" and interpretation
 * "the current time/date", as TS 102 527-3 table 16 prescribes. Returns the
 * new offset, or `off` unchanged if it does not fit or `t` is out of range.
 */
size_t dect_time_put(uint8_t *out, size_t cap, size_t off,
		     const struct dect_time *t);

/*
 * Build the whole CISS {FACILITY} message carrying the above. Returns its
 * length, or 0 on failure. The result is a complete NWK message, ready to
 * hand to the DLC.
 */
size_t dect_time_build_facility(uint8_t *out, size_t cap,
				const struct dect_time *t);

/*
 * Events notification towards the handset: <<EVENTS-NOTIFICATION>>
 * (EN 300 175-5 §7.7.55) in the same CISS {FACILITY} with dummy TI 6
 * (TS 102 527-3 §7.4.1.1), optionally with the line it concerns in
 * <<CALL-INFORMATION>> (table 13). Used for the list change indication
 * (§7.4.10.2), later the missed call and message waiting notifications.
 */
#define DECT_IE_EVENTS_NOTIFICATION	0x7Du
#define DECT_EVENT_MESSAGE_WAITING	0x00u
#define DECT_EVENT_MISSED_CALL		0x01u
#define DECT_EVENT_LIST_CHANGE		0x03u
#define DECT_EVENT_LINE_EXTERNAL	0x00u	/* line id subtypes, table 13 */
#define DECT_EVENT_LINE_RELATING_TO	0x03u
#define DECT_EVENT_NO_LINE		0xFFu

struct dect_event {
	uint8_t  type;		/* DECT_EVENT_* */
	uint8_t  subtype;	/* always sent (§7.4.1.1); for a list change,
				 * the list identifier */
	unsigned multiplicity;	/* 0..16383 */
};

/*
 * The {FACILITY} with n events and, unless line_id is DECT_EVENT_NO_LINE,
 * <<CALL-INFORMATION>> naming that line with line_subtype. Returns the
 * length, 0 if it does not fit or an argument is out of range.
 */
size_t dect_events_build_facility(uint8_t *out, size_t cap,
				  const struct dect_event *ev, unsigned n,
				  uint8_t line_id, uint8_t line_subtype);

#endif /* OPENDECTCELL_DECT_TIME_H */

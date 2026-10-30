// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for src/libopendectcell/dect_cell.c: the cell <-> controller frames
 * (opendectcell-controller docs/cell-protocol.md). No hardware.
 */
#include "check.h"
#include "../src/libopendectcell/dect_cell.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void test_frame_round_trip(void)
{
	static const uint8_t body[] = { 0x00, 0x00, 0x01, 0x2C };
	/* §3.1: length = 4 + body, then type, corr, body */
	static const uint8_t want[] = { 0x00, 0x08, 0x00, 0x03, 0x12, 0x34,
					0x00, 0x00, 0x01, 0x2C };
	uint8_t out[32];
	struct dect_cell_frame f;
	size_t n = dect_cell_put(out, sizeof(out), DECT_CELL_HEARTBEAT,
				 0x1234, body, sizeof(body));

	CHECK_EQ(n, sizeof(want));
	CHECK(!memcmp(out, want, sizeof(want)));
	CHECK_EQ(dect_cell_parse(out, n, &f), (long)n);
	CHECK_EQ(f.type, DECT_CELL_HEARTBEAT);
	CHECK_EQ(f.corr, 0x1234);
	CHECK_EQ(f.len, 4);
	CHECK(!memcmp(f.body, body, 4));

	/* more octets needed, from one octet up to one short */
	CHECK_EQ(dect_cell_parse(out, 1, &f), 0);
	CHECK_EQ(dect_cell_parse(out, n - 1u, &f), 0);
	/* no room for it */
	CHECK_EQ(dect_cell_put(out, 9, DECT_CELL_HEARTBEAT, 0, body, 4), 0);
}

static void test_framing_lost(void)
{
	/* §3.4: length < 4 or > 4100 */
	static const uint8_t short_len[] = { 0x00, 0x03, 0, 0, 0 };
	static const uint8_t long_len[] = { 0x10, 0x05 };
	static const uint8_t max_len[] = { 0x10, 0x04 };
	struct dect_cell_frame f;

	CHECK_EQ(dect_cell_parse(short_len, sizeof(short_len), &f), -1);
	CHECK_EQ(dect_cell_parse(long_len, sizeof(long_len), &f), -1);
	CHECK_EQ(dect_cell_parse(max_len, sizeof(max_len), &f), 0);
}

static void test_body_bounds(void)
{
	CHECK_EQ(dect_cell_body_max(DECT_CELL_HEARTBEAT), 4);
	CHECK_EQ(dect_cell_body_max(DECT_CELL_MAIL_UP), 258);
	CHECK_EQ(dect_cell_body_max(DECT_CELL_MAIL_DOWN), 254);
	CHECK_EQ(dect_cell_body_max(DECT_CELL_ENROLL_RESPONSE), 4096);
	CHECK_EQ(dect_cell_body_max(0x0F00), 0);	/* not known here */
}

static void test_tlv(void)
{
	uint8_t buf[64];
	struct dect_cell_tlv t;
	const uint8_t *v;
	size_t n, pos = 0;
	uint8_t u8;
	uint16_t u16;
	uint32_t u32;

	dect_cell_tlv_init(&t, buf, sizeof(buf));
	dect_cell_tlv_u8(&t, 1, 7);
	dect_cell_tlv_u16(&t, 2, 0xBEEF);
	dect_cell_tlv_u32(&t, 3, 0x01020304);
	dect_cell_tlv_str(&t, 4, "07.56");
	dect_cell_tlv_u8(&t, 1, 9);		/* repeated: the last wins */
	CHECK(!t.err);
	CHECK_EQ(t.len, 4 + 5 + 7 + 8 + 4);
	CHECK(!memcmp(buf, "\x01\x00\x01\x07\x02\x00\x02\xBE\xEF", 9));
	CHECK_EQ(dect_cell_tlv_check(buf, t.len), 0);

	CHECK_EQ(dect_cell_tlv_get_u8(buf, t.len, 1, &u8), 0);
	CHECK_EQ(u8, 9);
	CHECK_EQ(dect_cell_tlv_get_u16(buf, t.len, 2, &u16), 0);
	CHECK_EQ(u16, 0xBEEF);
	CHECK_EQ(dect_cell_tlv_get_u32(buf, t.len, 3, &u32), 0);
	CHECK_EQ(u32, 0x01020304);
	v = dect_cell_tlv_get(buf, t.len, 4, &n);
	CHECK(v && n == 5 && !memcmp(v, "07.56", 5));
	CHECK(dect_cell_tlv_get(buf, t.len, 5, &n) == NULL);
	/* a wrong width is absent, not misread */
	CHECK_EQ(dect_cell_tlv_get_u16(buf, t.len, 1, &u16), -1);

	/* each repetition in order */
	v = dect_cell_tlv_next(buf, t.len, 1, &pos, &n);
	CHECK(v && n == 1 && v[0] == 7);
	v = dect_cell_tlv_next(buf, t.len, 1, &pos, &n);
	CHECK(v && n == 1 && v[0] == 9);
	CHECK(dect_cell_tlv_next(buf, t.len, 1, &pos, &n) == NULL);

	/* a value running past the body, and a stray octet */
	CHECK_EQ(dect_cell_tlv_check(buf, t.len - 1u), -1);
	CHECK_EQ(dect_cell_tlv_check(buf, 2), -1);
	CHECK_EQ(dect_cell_tlv_check(buf, 0), 0);

	/* writing past cap sets err once and keeps what fitted */
	dect_cell_tlv_init(&t, buf, 6);
	dect_cell_tlv_u16(&t, 1, 1);
	dect_cell_tlv_u16(&t, 2, 2);
	CHECK(t.err);
	CHECK_EQ(t.len, 5);
}

static void test_mail(void)
{
	static const uint8_t seg[] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
				       0x07 };
	/* §7.1: rx_time, src, dst, prim, plen, param */
	static const uint8_t want_up[] = { 0x00, 0x0F, 0x42, 0x40, 0x02, 0x22,
					   0x02, 0x03, 0x07, 1, 2, 3, 4, 5,
					   6, 7 };
	struct dect_cell_mail m = { 1000000u, 0x02, 0x22, 0x0203, seg,
				    sizeof(seg) };
	struct dect_cell_mail r;
	uint8_t out[300];
	size_t n = dect_cell_mail_up(out, sizeof(out), &m);

	CHECK_EQ(n, sizeof(want_up));
	CHECK(!memcmp(out, want_up, sizeof(want_up)));
	CHECK_EQ(dect_cell_mail_parse(out, n, 1, &r), 0);
	CHECK_EQ(r.rx_time, 1000000u);
	CHECK_EQ(r.prim, 0x0203);
	CHECK_EQ(r.plen, 7);
	CHECK(!memcmp(r.param, seg, 7));

	/* MAIL_DOWN: the same without rx_time; exactly 5 + plen */
	n = dect_cell_mail_down(out, sizeof(out), &m);
	CHECK_EQ(n, 12);
	CHECK_EQ(dect_cell_mail_parse(out, n, 0, &r), 0);
	CHECK_EQ(r.src, 0x02);
	CHECK_EQ(r.dst, 0x22);
	CHECK_EQ(dect_cell_mail_parse(out, n - 1u, 0, &r), -1);
	CHECK_EQ(dect_cell_mail_parse(out, n, 1, &r), -1);
	out[4] = 250;				/* over DECT_CELL_MAIL_PARAM_MAX */
	CHECK_EQ(dect_cell_mail_parse(out, 255, 0, &r), -1);

	m.plen = 250;
	CHECK_EQ(dect_cell_mail_down(out, sizeof(out), &m), 0);
}

/*
 * Hostile input (the sanitizers do the judging, so this runs under them):
 * valid frames, TLV bodies and mail bodies of every shape, then cut, grown
 * and byte-flipped at random from a fixed seed. Each input sits in a heap
 * block of exactly its length, so any read past it is an error. What must
 * hold is only what the header promises: a parse takes no more octets than
 * it was given, a body that passes the TLV check can be walked, a getter
 * that returns a pointer returns one inside the body.
 */
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static uint32_t rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return (uint32_t)(rng_state >> 16);
}

static void walk(const uint8_t *body, size_t len)
{
	size_t n, pos, i;
	const uint8_t *v;
	uint8_t u8;
	uint16_t u16;
	uint32_t u32;
	struct dect_cell_mail m;

	if (!dect_cell_tlv_check(body, len)) {
		for (i = 0; i < 24; i++) {
			uint8_t tag = (uint8_t)i;

			v = dect_cell_tlv_get(body, len, tag, &n);
			if (v)
				CHECK(v >= body && v + n <= body + len);
			pos = 0;
			while ((v = dect_cell_tlv_next(body, len, tag, &pos, &n))
			       != NULL) {
				CHECK(v >= body && v + n <= body + len);
				CHECK(pos <= len);
			}
			(void)dect_cell_tlv_get_u8(body, len, tag, &u8);
			(void)dect_cell_tlv_get_u16(body, len, tag, &u16);
			(void)dect_cell_tlv_get_u32(body, len, tag, &u32);
		}
	}
	if (!dect_cell_mail_parse(body, len, 0, &m) && m.plen)
		CHECK(m.param >= body && m.param + m.plen <= body + len);
	if (!dect_cell_mail_parse(body, len, 1, &m) && m.plen)
		CHECK(m.param >= body && m.param + m.plen <= body + len);
}

static void feed(const uint8_t *src, size_t n)
{
	uint8_t *buf = malloc(n ? n : 1u);
	struct dect_cell_frame f;
	size_t off = 0;
	long r;

	CHECK(buf != NULL);
	if (!buf)
		return;
	memcpy(buf, src, n);
	walk(buf, n);			/* as a body */
	while (off < n) {		/* as a stream of frames */
		r = dect_cell_parse(buf + off, n - off, &f);
		if (r <= 0)
			break;
		CHECK((size_t)r <= n - off);
		CHECK(f.body >= buf + off && f.body + f.len <= buf + n);
		(void)dect_cell_body_max(f.type);
		walk(f.body, f.len);
		off += (size_t)r;
	}
	free(buf);
}

static void test_hostile_input(void)
{
	uint8_t seed[1100], tmp[1100];
	struct dect_cell_tlv t;
	size_t len, i, k;
	unsigned round;

	for (round = 0; round < 60000u; round++) {
		/* a seed: a TLV body of up to six random tags, or a frame, or
		 * a mail body */
		dect_cell_tlv_init(&t, seed, sizeof(seed));
		for (k = rnd() % 7u; k; k--) {
			uint8_t v[64];
			size_t vn = rnd() % sizeof(v);

			for (i = 0; i < vn; i++)
				v[i] = (uint8_t)rnd();
			dect_cell_tlv_put(&t, (uint8_t)(rnd() % 20u), v, vn);
		}
		len = t.err ? 0 : t.len;
		switch (rnd() % 3u) {
		case 0:
			break;
		case 1:
			len = dect_cell_put(tmp, sizeof(tmp),
					    (uint16_t)(rnd() % 0x400u),
					    (uint16_t)rnd(), seed, len);
			memcpy(seed, tmp, len);
			break;
		default: {
			struct dect_cell_mail m = { rnd(), (uint8_t)rnd(),
				(uint8_t)rnd(), (uint16_t)rnd(), seed,
				rnd() % 200u };

			len = (rnd() & 1u) ? dect_cell_mail_up(tmp, sizeof(tmp),
							       &m)
					   : dect_cell_mail_down(tmp,
							  sizeof(tmp), &m);
			memcpy(seed, tmp, len);
		}
		}
		/* the mutations: flips, a cut, a growth of random octets */
		for (k = rnd() % 5u; k && len; k--)
			seed[rnd() % len] ^= (uint8_t)(1u << (rnd() % 8u));
		if (rnd() % 4u == 0 && len)
			len = rnd() % (len + 1u);
		if (rnd() % 8u == 0)
			for (k = rnd() % 8u; k && len < sizeof(seed); k--)
				seed[len++] = (uint8_t)rnd();
		feed(seed, len);
	}
	CHECK(1);
}

int main(void)
{
	test_frame_round_trip();
	test_framing_lost();
	test_body_bounds();
	test_tlv();
	test_mail();
	test_hostile_input();
	return CHECK_DONE();
}

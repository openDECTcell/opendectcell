// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for src/libopendectcell/dect_mail.c against docs/stub-pcm-control-channel.md.
 * Pure logic, no hardware.
 */
#include "check.h"
#include "../src/libopendectcell/dect_mail.h"

#include <string.h>

/*
 * Build a STUB -> host receive frame independently of the library:
 *   0xAA, seq, L(LE), [src dst prim(LE) params], sum16(LE) of (L field + body).
 * Returns the total length.
 */
static size_t build_rx(uint8_t *b, uint8_t seq, uint8_t src, uint8_t dst,
		       uint16_t prim, const uint8_t *par, size_t plen)
{
	unsigned int L = (unsigned int)(2u + plen);
	unsigned int sum = 0, i, total;

	b[0] = 0xaa;
	b[1] = seq;
	b[2] = (uint8_t)L;
	b[3] = (uint8_t)(L >> 8);
	b[4] = src;
	b[5] = dst;
	b[6] = (uint8_t)prim;
	b[7] = (uint8_t)(prim >> 8);
	memcpy(b + 8, par, plen);
	total = 4u + L + 2u + 2u;
	for (i = 2; i < total - 2u; i++)
		sum += b[i];
	b[total - 2] = (uint8_t)sum;
	b[total - 1] = (uint8_t)(sum >> 8);
	return total;
}

/* The contact mail 0xF600 encodes to the exact element in the doc. */
static void test_encode_contact(void)
{
	struct dect_mail m = { .src = 0x22, .dst = 0x02, .prim = 0xf600,
			       .param_len = 0 };
	static const uint8_t want[] = {
		0x05, 0x00,		/* word count (n+5)/2 = 5 */
		0x00, 0x00,		/* seq 0 */
		0x02, 0x00,		/* L = 2 */
		0x22, 0x02,		/* src, dst */
		0x00, 0xf6,		/* prim 0xF600 LE */
		0x1c, 0x01,		/* checksum 0x011C LE */
	};
	uint8_t out[DECT_TX_ELEM_MAX];
	int n = dect_mail_encode(&m, 0, out, sizeof(out));

	CHECK_EQ(n, (int)sizeof(want));
	CHECK(n == (int)sizeof(want) && memcmp(out, want, sizeof(want)) == 0);
}

/* An encoded element decodes back to the same mail through the RX parser. */
static void test_tx_rx_roundtrip(void)
{
	/*
	 * The TX element and the RX frame differ only in the header (word
	 * count vs 0xAA + 1-byte seq) and the seq width, but the body and
	 * checksum match. Build an RX frame from the same fields and parse it.
	 */
	uint8_t par[5] = { 0x11, 0x22, 0x33, 0x44, 0x55 };
	uint8_t frame[64];
	size_t fl = build_rx(frame, 7, 0x02, 0x14, 0xf601, par, sizeof(par));
	struct dect_rx rx;
	struct dect_mail got;

	dect_rx_init(&rx);
	dect_rx_push(&rx, frame, fl);
	CHECK_EQ(dect_rx_pull(&rx, &got), 1);
	CHECK_EQ(got.src, 0x02);
	CHECK_EQ(got.dst, 0x14);
	CHECK_EQ(got.prim, 0xf601);
	CHECK_EQ(got.param_len, 5);
	CHECK(memcmp(got.param, par, 5) == 0);
	CHECK_EQ(rx.stats.mails, 1);
	CHECK_EQ(dect_rx_pull(&rx, &got), 0);
}

/* Idle 0xFF bytes before and between mails are discarded. */
static void test_idle_and_two_mails(void)
{
	uint8_t buf[256];
	size_t n = 0;
	struct dect_rx rx;
	struct dect_mail got;

	memset(buf, 0xff, 8);
	n = 8;
	n += build_rx(buf + n, 10, 0x23, 0x22, 0xfff1, (uint8_t[]){ 4 }, 1);
	memset(buf + n, 0xff, 4);
	n += 4;
	n += build_rx(buf + n, 11, 0x23, 0x14, 0xf618, (uint8_t[]){ 0x37, 0x01 }, 2);

	dect_rx_init(&rx);
	dect_rx_push(&rx, buf, n);
	CHECK_EQ(dect_rx_pull(&rx, &got), 1);
	CHECK_EQ(got.prim, 0xfff1);
	CHECK_EQ(dect_rx_pull(&rx, &got), 1);
	CHECK_EQ(got.prim, 0xf618);
	CHECK_EQ(dect_rx_pull(&rx, &got), 0);
	CHECK_EQ(rx.stats.mails, 2);
	CHECK_EQ(rx.stats.seq_gaps, 0);
}

/* A non-incrementing sequence number is counted as a gap. */
static void test_seq_gap(void)
{
	uint8_t a[32], b[32];
	size_t la = build_rx(a, 20, 0x23, 0x22, 0xfff1, (uint8_t[]){ 4 }, 1);
	size_t lb = build_rx(b, 25, 0x23, 0x22, 0xfff1, (uint8_t[]){ 4 }, 1);
	struct dect_rx rx;
	struct dect_mail got;

	dect_rx_init(&rx);
	dect_rx_push(&rx, a, la);
	dect_rx_push(&rx, b, lb);
	CHECK_EQ(dect_rx_pull(&rx, &got), 1);	/* seq 20: first, no gap */
	CHECK_EQ(dect_rx_pull(&rx, &got), 1);	/* seq 25: gap */
	CHECK_EQ(rx.stats.seq_gaps, 1);
}

/* A corrupted checksum is skipped; the following mail still parses. */
static void test_csum_resync(void)
{
	uint8_t a[32], b[32];
	size_t la = build_rx(a, 30, 0x23, 0x22, 0xfff1, (uint8_t[]){ 4 }, 1);
	size_t lb = build_rx(b, 31, 0x23, 0x22, 0xf618, (uint8_t[]){ 0x37, 0x01 }, 2);
	struct dect_rx rx;
	struct dect_mail got;

	a[la - 1] ^= 0xff;		/* break the checksum of the first mail */
	dect_rx_init(&rx);
	dect_rx_push(&rx, a, la);
	dect_rx_push(&rx, b, lb);
	/* The broken mail yields nothing by itself... */
	CHECK_EQ(dect_rx_pull(&rx, &got), 1);	/* resyncs and finds the 2nd */
	CHECK_EQ(got.prim, 0xf618);
	CHECK(rx.stats.csum_errors >= 1);
	CHECK(rx.stats.resyncs >= 1);
	CHECK_EQ(rx.stats.seq_gaps, 0);		/* first good mail after resync */
}

/* A length field over 0xFB is a framing error and triggers a resync. */
static void test_bad_length(void)
{
	uint8_t buf[32];
	size_t lb;
	struct dect_rx rx;
	struct dect_mail got;

	buf[0] = 0xaa;
	buf[1] = 1;
	buf[2] = 0xfc;		/* L = 0xFC > 0xFB */
	buf[3] = 0x00;
	lb = build_rx(buf + 4, 2, 0x23, 0x22, 0xf618, (uint8_t[]){ 0x37, 0x01 }, 2);

	dect_rx_init(&rx);
	dect_rx_push(&rx, buf, 4 + lb);
	CHECK_EQ(dect_rx_pull(&rx, &got), 1);	/* finds the valid mail after */
	CHECK_EQ(got.prim, 0xf618);
	CHECK(rx.stats.framing_errors >= 1);
}

/*
 * A length field below 2 cannot hold src, dst and prim, so it is a framing
 * error -- never a mail whose parameter length is L - 2 computed in unsigned
 * arithmetic (65534 or 65535 bytes copied into a 249-byte buffer). The first
 * case is the realistic one: a stray 0xAA followed by zero bytes, whose
 * checksum over zeros is zero and therefore always "matches".
 */
static void test_short_length(void)
{
	static const uint8_t zeros[] = {
		0xaa, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
	/* L = 0: L field, src, dst, then sum16 = 0x22 + 0x02. */
	static const uint8_t l0[] = {
		0xaa, 0x02, 0x00, 0x00, 0x22, 0x02, 0x24, 0x00,
	};
	/* L = 1: L field, src, dst, one byte, then sum16 = 1 + 0x22 + 0x02 + 0x10. */
	static const uint8_t l1[] = {
		0xaa, 0x03, 0x01, 0x00, 0x22, 0x02, 0x10, 0x35, 0x00,
	};
	const struct { const uint8_t *b; size_t n; } bad[] = {
		{ zeros, sizeof(zeros) }, { l0, sizeof(l0) }, { l1, sizeof(l1) },
	};
	size_t i;

	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		uint8_t buf[64];
		size_t n;
		struct dect_rx rx;
		struct dect_mail got;

		memcpy(buf, bad[i].b, bad[i].n);
		n = bad[i].n;
		n += build_rx(buf + n, 9, 0x23, 0x22, 0xf618,
			      (uint8_t[]){ 0x37, 0x01 }, 2);
		dect_rx_init(&rx);
		dect_rx_push(&rx, buf, n);
		CHECK_EQ(dect_rx_pull(&rx, &got), 1);	/* the valid mail after */
		CHECK_EQ(got.prim, 0xf618);
		CHECK_EQ(got.param_len, 2);
		CHECK_EQ(rx.stats.mails, 1);
		CHECK(rx.stats.framing_errors >= 1);
	}
}

/* A mail split across two pushes is assembled before it parses. */
static void test_partial(void)
{
	uint8_t par[5] = { 1, 2, 3, 4, 5 };
	uint8_t frame[64];
	size_t fl = build_rx(frame, 7, 0x02, 0x14, 0xf601, par, sizeof(par));
	struct dect_rx rx;
	struct dect_mail got;

	dect_rx_init(&rx);
	dect_rx_push(&rx, frame, 6);		/* only the head */
	CHECK_EQ(dect_rx_pull(&rx, &got), 0);	/* incomplete */
	dect_rx_push(&rx, frame + 6, fl - 6);	/* the rest */
	CHECK_EQ(dect_rx_pull(&rx, &got), 1);
	CHECK_EQ(got.prim, 0xf601);
}

/* Oversized mails are rejected by the encoder. */
static void test_encode_limits(void)
{
	struct dect_mail m = { .src = 1, .dst = 2, .prim = 3 };
	uint8_t out[DECT_TX_ELEM_MAX];

	m.param_len = DECT_MAIL_PARAM_MAX;	/* n = 6 + 0xF9 > 0xFC */
	CHECK_EQ(dect_mail_encode(&m, 0, out, sizeof(out)), -1);
	m.param_len = 0xf6u - 6u;		/* n = 0xF6, fits */
	CHECK(dect_mail_encode(&m, 0, out, sizeof(out)) > 0);
	m.param_len = 2;			/* out buffer too small */
	CHECK_EQ(dect_mail_encode(&m, 0, out, 4), -1);
}

/*
 * The big-endian transport (dect_mail_set_swap(1), the 7412's STUB): the
 * length field and the primitive go on the wire most significant byte
 * first, both ways; the element header, the sequence number and the
 * checksum keep their order.
 */
static void test_swapped_byte_order(void)
{
	struct dect_mail m = { .src = 0x22, .dst = 0x02, .prim = 0x0244,
			       .param_len = 3, .param = { 0x01, 0x02, 0x03 } };
	static const uint8_t want[] = {
		0x07, 0x00,		/* word count (9 + 5) / 2 */
		0x05, 0x00,		/* seq 5 */
		0x00, 0x05,		/* L = 5, big-endian */
		0x22, 0x02,		/* src, dst */
		0x02, 0x44,		/* prim 0x0244, big-endian */
		0x01, 0x02, 0x03,
		0x75, 0x00,		/* checksum 0x0075 LE */
		0xff,			/* pad to even */
	};
	uint8_t out[DECT_TX_ELEM_MAX], frame[32];
	struct dect_rx rx;
	struct dect_mail got;
	int n;

	dect_mail_set_swap(1);
	n = dect_mail_encode(&m, 5, out, sizeof(out));
	CHECK_EQ(n, (int)sizeof(want));
	CHECK(n == (int)sizeof(want) && memcmp(out, want, sizeof(want)) == 0);

	/* A received frame in the same order: 0xAA, seq, then as above. */
	frame[0] = 0xaa;
	frame[1] = 0x09;
	memcpy(frame + 2, want + 4, 9u + 2u);	/* mail and checksum */
	dect_rx_init(&rx);
	dect_rx_push(&rx, frame, 2u + 9u + 2u);
	CHECK_EQ(dect_rx_pull(&rx, &got), 1);
	CHECK_EQ(got.src, 0x22);
	CHECK_EQ(got.dst, 0x02);
	CHECK_EQ(got.prim, 0x0244);
	CHECK_EQ(got.param_len, 3);
	CHECK_EQ(memcmp(got.param, m.param, 3), 0);

	/* The little-endian frame of the same mail is not read the same. */
	n = (int)build_rx(frame, 0x0a, 0x22, 0x02, 0x0244, m.param, 3);
	dect_rx_init(&rx);
	dect_rx_push(&rx, frame, (size_t)n);
	CHECK(dect_rx_pull(&rx, &got) == 0 || got.prim != 0x0244);

	dect_mail_set_swap(0);
	n = dect_mail_encode(&m, 5, out, sizeof(out));
	CHECK(n > 5 && out[4] == 0x05 && out[5] == 0x00);	/* LE again */
}

int main(void)
{
	test_encode_contact();
	test_tx_rx_roundtrip();
	test_idle_and_two_mails();
	test_seq_gap();
	test_csum_resync();
	test_bad_length();
	test_short_length();
	test_partial();
	test_encode_limits();
	test_swapped_byte_order();
	return CHECK_DONE();
}

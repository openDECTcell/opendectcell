// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Host <-> STUB mail framing (docs/stub-pcm-control-channel.md).
 * See dect_mail.h for the wire layout and the API contract.
 */
#include "dect_mail.h"

#include <string.h>

/* 16-bit sum of n bytes, as used by both framing directions. */
static uint16_t sum16(const uint8_t *p, size_t n)
{
	unsigned int s = 0;
	size_t i;

	for (i = 0; i < n; i++)
		s += p[i];
	return (uint16_t)s;
}

int dect_mail_encode(const struct dect_mail *m, uint16_t seq,
		     uint8_t *out, size_t out_cap)
{
	uint8_t mail[DECT_MAIL_TX_MAX];
	size_t n, len, wc;
	uint16_t sum, L;

	if (m->param_len > DECT_MAIL_PARAM_MAX)
		return -1;
	n = 6u + m->param_len;		/* L(2) src dst prim(2) params */
	if (n > DECT_MAIL_TX_MAX)
		return -1;

	L = (uint16_t)(2u + m->param_len);	/* bytes after the task bytes */
	mail[0] = (uint8_t)L;
	mail[1] = (uint8_t)(L >> 8);
	mail[2] = m->src;
	mail[3] = m->dst;
	mail[4] = (uint8_t)m->prim;
	mail[5] = (uint8_t)(m->prim >> 8);
	memcpy(mail + 6, m->param, m->param_len);
	sum = sum16(mail, n);

	/* element: word_count, seq, mail, checksum, 0xFF pad to even */
	len = 4u + n + 2u;
	len += (len & 1u);		/* pad to even */
	if (len > out_cap)
		return -1;

	wc = (n + 5u) / 2u;		/* words after the count word, rounded up */
	out[0] = (uint8_t)wc;
	out[1] = (uint8_t)(wc >> 8);
	out[2] = (uint8_t)seq;
	out[3] = (uint8_t)(seq >> 8);
	memcpy(out + 4, mail, n);
	out[4u + n] = (uint8_t)sum;
	out[5u + n] = (uint8_t)(sum >> 8);
	if ((4u + n + 2u) & 1u)
		out[6u + n] = 0xff;
	return (int)len;
}

void dect_rx_init(struct dect_rx *rx)
{
	memset(rx, 0, sizeof(*rx));
	rx->last_seq = -1;
}

size_t dect_rx_push(struct dect_rx *rx, const uint8_t *data, size_t n)
{
	size_t room = sizeof(rx->buf) - rx->len;

	if (n > room) {
		rx->stats.framing_errors++;
		n = room;
	}
	memcpy(rx->buf + rx->len, data, n);
	rx->len += n;
	return n;
}

/* Drop the first k bytes of the buffer. */
static void consume(struct dect_rx *rx, size_t k)
{
	if (k >= rx->len) {
		rx->len = 0;
		return;
	}
	memmove(rx->buf, rx->buf + k, rx->len - k);
	rx->len -= k;
}

int dect_rx_pull(struct dect_rx *rx, struct dect_mail *out)
{
	/*
	 * Scan the buffered bytes for a valid mail. On a framing or checksum
	 * error, skip the suspect 0xAA and resync; drop everything before the
	 * first 0xAA so idle bytes do not accumulate. Keep an incomplete tail.
	 */
	for (;;) {
		size_t i = 0;
		unsigned int L, want, total;
		uint16_t sum;

		/* Discard until the first possible start marker. */
		while (i < rx->len && rx->buf[i] != 0xaa)
			i++;
		if (i) {
			consume(rx, i);
			i = 0;
		}
		if (rx->len < 10u)	/* smallest possible mail (doc) */
			return 0;

		L = rx->buf[2] | (unsigned int)rx->buf[3] << 8;
		total = 4u + L + 2u + 2u;	/* AA seq len body sum */
		if (L > DECT_MAIL_L_MAX) {
			rx->stats.framing_errors++;
			rx->stats.resyncs++;
			rx->resynced = 1;
			consume(rx, 1);		/* this 0xAA was not a header */
			continue;
		}
		if (rx->len < total)
			return 0;		/* need more bytes */

		sum = sum16(rx->buf + 2, 2u + L + 2u);	/* length field + body */
		want = rx->buf[total - 2] | (unsigned int)rx->buf[total - 1] << 8;
		if (sum != want) {
			rx->stats.csum_errors++;
			rx->stats.resyncs++;
			rx->resynced = 1;
			consume(rx, 1);
			continue;
		}

		/* Valid mail. Body: src, dst, prim(LE), parameters. */
		out->src = rx->buf[4];
		out->dst = rx->buf[5];
		out->prim = (uint16_t)(rx->buf[6] | (unsigned int)rx->buf[7] << 8);
		out->param_len = (uint16_t)(L - 2u);
		memcpy(out->param, rx->buf + 8, out->param_len);

		if (rx->last_seq >= 0 && !rx->resynced &&
		    rx->buf[1] != (uint8_t)(rx->last_seq + 1))
			rx->stats.seq_gaps++;
		rx->last_seq = rx->buf[1];
		rx->resynced = 0;
		rx->stats.mails++;
		consume(rx, total);
		return 1;
	}
}

// SPDX-License-Identifier: GPL-2.0-or-later
/* Bit and byte alignment for the generic SSC PCM transport; see ssc_align.h. */
#include <string.h>

#include "ssc_align.h"

void ssc_align_init(struct ssc_align *a, unsigned int off)
{
	a->off = off % 16u;
	memset(a->rx_carry, 0xff, sizeof(a->rx_carry));
	memset(a->tx_carry, 0xff, sizeof(a->tx_carry));
}

/* Swap the two bytes of every 16-bit slot. */
static void ssc_swap_pairs(uint8_t *p, size_t len)
{
	size_t i;

	for (i = 0; i + 1u < len; i += 2u) {
		uint8_t t = p[i];

		p[i] = p[i + 1u];
		p[i + 1u] = t;
	}
}

/*
 * dst[i] = the 8 bits of src starting at bit 8 * i + bit, MSB first.
 * src must hold len + (bit + 7) / 8 bytes; bit is 0..16.
 */
static void ssc_bit_window(uint8_t *dst, const uint8_t *src, size_t len,
			   unsigned int bit)
{
	unsigned int by = bit / 8u, bi = bit % 8u;
	size_t i;

	for (i = 0; i < len; i++)
		dst[i] = (uint8_t)(bi ? (unsigned int)src[i + by] << bi |
					(unsigned int)src[i + by + 1u] >> (8u - bi)
				      : src[i + by]);
}

void ssc_align_rx(struct ssc_align *a, uint8_t *fr, uint8_t *t, size_t len)
{
	if (len < 2u)
		return;
	t[0] = a->rx_carry[0];
	t[1] = a->rx_carry[1];
	memcpy(t + 2, fr, len);
	a->rx_carry[0] = fr[len - 2u];
	a->rx_carry[1] = fr[len - 1u];
	ssc_bit_window(fr, t, len, a->off);
	ssc_swap_pairs(fr, len);
}

void ssc_align_tx(struct ssc_align *a, uint8_t *out, uint8_t *t,
		  const uint8_t *frames, size_t len, size_t tail, int burst)
{
	if (burst)
		memset(a->tx_carry, 0xff, sizeof(a->tx_carry));
	t[0] = a->tx_carry[0];
	t[1] = a->tx_carry[1];
	memcpy(t + 2, frames, len);
	memset(t + 2 + len, 0xff, tail);
	ssc_swap_pairs(t + 2, len);
	ssc_bit_window(out, t, len + tail, 16u - a->off);
}

void ssc_align_tx_commit(struct ssc_align *a, const uint8_t *t, size_t w)
{
	if (w < 2u)
		return;
	a->tx_carry[0] = t[w];
	a->tx_carry[1] = t[w + 1u];
}

int ssc_idle_offset(const uint8_t *data, size_t len, unsigned int cnt[16])
{
	size_t nb = len * 8u, b;
	unsigned int o, best = 0;

	memset(cnt, 0, 16u * sizeof(cnt[0]));
	for (o = 0; o < 16u; o++)
		for (b = o; b + 24u <= nb; b += 16u) {
			const uint8_t *p = data + b / 8u;
			uint32_t v = (uint32_t)p[0] << 16 |
				     (uint32_t)p[1] << 8 | p[2];

			cnt[o] += ((v >> (8u - b % 8u)) & 0xffffu) == 0xfefeu;
		}
	for (o = 1; o < 16u; o++)
		if (cnt[o] > cnt[best])
			best = o;
	return cnt[best] ? (int)best : -1;
}

int ssc_idle_lanes(const uint8_t *data, size_t len, unsigned int slots,
		   uint64_t *mask)
{
	unsigned int cnt[16], hit[64], tot[64], p;
	size_t nb = len * 8u, b, k;
	int off;

	*mask = 0;
	if (!slots || slots > 64u)
		return -1;
	off = ssc_idle_offset(data, len, cnt);
	if (off < 0)
		return -1;
	memset(hit, 0, sizeof(hit));
	memset(tot, 0, sizeof(tot));
	for (k = 0, b = (size_t)off; b + 24u <= nb; k++, b += 16u) {
		const uint8_t *q = data + b / 8u;
		uint32_t v = (uint32_t)q[0] << 16 | (uint32_t)q[1] << 8 | q[2];

		p = (unsigned int)(k % slots);
		tot[p]++;
		hit[p] += ((v >> (8u - b % 8u)) & 0xffffu) == 0xfefeu;
	}
	for (p = 0; p < slots; p++)
		if (tot[p] && hit[p] * 4u >= tot[p] * 3u)
			*mask |= 1ull << p;
	return off;
}

int ssc_lanes_rotation(uint64_t from, uint64_t to, unsigned int slots)
{
	uint64_t all = slots >= 64u ? ~0ull : (1ull << slots) - 1u;
	unsigned int k;

	from &= all;
	to &= all;
	if (!from || !to || !slots || slots > 64u)
		return -1;
	for (k = 0; k < slots; k++) {
		uint64_t r = k ? ((from << k) | (from >> (slots - k))) & all
			       : from;

		if (r == to)
			return (int)k;
	}
	return -1;
}

void ssc_lane_delay_init(struct ssc_lane_delay *d, unsigned int lanes)
{
	d->lanes = lanes > SSC_LANE_DELAY_MAX ? SSC_LANE_DELAY_MAX : lanes;
	memset(d->carry, 0xff, sizeof(d->carry));
}

void ssc_lane_delay_apply(struct ssc_lane_delay *d, uint8_t *buf, size_t len)
{
	uint8_t keep[sizeof(d->carry)];
	size_t n = 2u * d->lanes;

	if (!n)
		return;
	if (len >= n) {
		/* the block's last n bytes are the next carry */
		memcpy(keep, buf + len - n, n);
		memmove(buf + n, buf, len - n);
		memcpy(buf, d->carry, n);
		memcpy(d->carry, keep, n);
	} else {
		/* a block shorter than the delay comes all from the carry */
		memcpy(keep, buf, len);
		memcpy(buf, d->carry, len);
		memmove(d->carry, d->carry + len, n - len);
		memcpy(d->carry + n - len, keep, len);
	}
}

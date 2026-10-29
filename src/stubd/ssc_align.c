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

void ssc_swap_pairs(uint8_t *p, size_t len)
{
	size_t i;

	for (i = 0; i + 1u < len; i += 2u) {
		uint8_t t = p[i];

		p[i] = p[i + 1u];
		p[i + 1u] = t;
	}
}

void ssc_bit_window(uint8_t *dst, const uint8_t *src, size_t len,
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

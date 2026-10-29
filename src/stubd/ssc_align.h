/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Bit and byte alignment for the generic SSC PCM transport (the 7412's
 * /dev/ssc-pcm0). The SSC has no frame sync: its words start at an arbitrary
 * bit offset into the chip's frame, the same in both directions, and it
 * delivers bus order (each 16-bit slot high byte first). These helpers turn
 * that into the 7520's layout -- slots on 16-bit boundaries, each a
 * little-endian sample -- and back. No I/O; docs/7412-spi-tdm-and-loader.md
 * §1b.
 */
#ifndef OPENDECTCELL_SSC_ALIGN_H
#define OPENDECTCELL_SSC_ALIGN_H

#include <stddef.h>
#include <stdint.h>

struct ssc_align {
	unsigned int off;	/* bit offset of the chip's lanes, 0..15 */
	uint8_t rx_carry[2];	/* last two received bytes */
	uint8_t tx_carry[2];	/* last two stream bytes sent */
};

/* Start both directions at offset off, with idle (all ones) carries. */
void ssc_align_init(struct ssc_align *a, unsigned int off);

/* Swap the two bytes of every 16-bit slot. */
void ssc_swap_pairs(uint8_t *p, size_t len);

/*
 * dst[i] = the 8 bits of src starting at bit 8 * i + bit, MSB first.
 * src must hold len + (bit + 7) / 8 bytes; bit is 0..16.
 */
void ssc_bit_window(uint8_t *dst, const uint8_t *src, size_t len,
		    unsigned int bit);

/*
 * Receive, in place: advance the stream by off bits and swap each slot.
 * The head comes from the previous call's last two bytes, which delays the
 * stream by one slot. t is scratch of len + 2 bytes.
 */
void ssc_align_rx(struct ssc_align *a, uint8_t *fr, uint8_t *t, size_t len);

/*
 * Transmit: swap each slot of len bytes of frames, append tail idle bytes,
 * and delay the stream by off bits into out (len + tail bytes). t is
 * scratch of len + tail + 2 bytes and must be passed to ssc_align_tx_commit()
 * unchanged. burst: the bytes before this write were idle (the driver's idle
 * frame), not the previous call's.
 */
void ssc_align_tx(struct ssc_align *a, uint8_t *out, uint8_t *t,
		  const uint8_t *frames, size_t len, size_t tail, int burst);

/* After the driver took w bytes of out: carry the stream on from there. */
void ssc_align_tx_commit(struct ssc_align *a, const uint8_t *t, size_t w);

/*
 * The bit offset at which most 16-bit lanes of data read 0xFEFE, the chip's
 * idle pattern; cnt[16] receives the count per offset. -1 when no lane does.
 */
int ssc_idle_offset(const uint8_t *data, size_t len, unsigned int cnt[16]);

/*
 * Where the idle lanes sit in data: returns the bit offset as
 * ssc_idle_offset() finds it (-1: none) and sets *mask to the lane
 * positions within a frame of slots lanes, counted from that offset in
 * data[0], that read 0xFEFE in at least three quarters of the frames. A
 * shift of the chip's frame against the SSC's stream shows here: lost whole
 * lanes rotate the mask, a lost or gained bit changes the offset. slots is
 * 1..64.
 */
int ssc_idle_lanes(const uint8_t *data, size_t len, unsigned int slots,
		   uint64_t *mask);

/*
 * k such that moving every lane position i of from to (i + k) % slots
 * gives to; -1 when no rotation does (or either mask is empty).
 */
int ssc_lanes_rotation(uint64_t from, uint64_t to, unsigned int slots);

#endif /* OPENDECTCELL_SSC_ALIGN_H */

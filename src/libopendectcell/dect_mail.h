/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host <-> STUB "mail" framing on the PCM control slot.
 * Specification: docs/stub-pcm-control-channel.md.
 *
 * Pure functions and a stream state machine on memory buffers: no file,
 * device or clock I/O, so the same code runs on the box (dect-stubd),
 * in the controller and in host tests. Transport and timers are injected
 * by the caller.
 *
 * A "mail" is the STUB message carried inside the framing of either
 * direction. On the wire the body after the length field is
 *   [ src, dst, prim(LE), parameters... ]
 * and the length field L counts the bytes after the two task bytes, i.e.
 * L = 2 + param_len (the two primitive bytes plus the parameters).
 */
#ifndef OPENDECTCELL_DECT_MAIL_H
#define OPENDECTCELL_DECT_MAIL_H

#include <stddef.h>
#include <stdint.h>

/*
 * Receive framing limits the length field L to 0xFB; the body is then
 * L + 2 bytes. Transmit limits the mail byte count n (= 6 + param_len)
 * to 0xFC. The largest decodable parameter block is L_MAX - 2.
 */
#define DECT_MAIL_L_MAX       0xFBu
#define DECT_MAIL_TX_MAX      0xFCu
#define DECT_MAIL_PARAM_MAX   (DECT_MAIL_L_MAX - 2u)   /* 0xF9 */

/* A decoded mail, host byte order. */
struct dect_mail {
	uint8_t  src;		/* source task */
	uint8_t  dst;		/* destination task */
	uint16_t prim;		/* primitive id (mail offset 4, LE on the wire) */
	uint16_t param_len;	/* number of parameter bytes */
	uint8_t  param[DECT_MAIL_PARAM_MAX];
};

/*
 * Encode a mail as a host -> STUB transmit element (no 0xAA header):
 *   [ word_count(LE) | seq(LE) | L(LE) src dst prim(LE) params | sum(LE) ]
 * padded with 0xFF to an even length. word_count = (n + 5) / 2 counts the
 * 16-bit words after the count word itself (seq, mail, sum), rounded up.
 * The checksum is the 16-bit sum of the n mail bytes (L field and body).
 *
 * Returns the element length in bytes (always even), or -1 if the mail is
 * too large or out does not fit. out_cap should be >= DECT_TX_ELEM_MAX.
 */
#define DECT_TX_ELEM_MAX  (4u + DECT_MAIL_TX_MAX + 2u + 1u)
int dect_mail_encode(const struct dect_mail *m, uint16_t seq,
		     uint8_t *out, size_t out_cap);

/*
 * STUB -> host stream parser. Control-slot bytes (two per frame, DMA-buffer
 * order) are pushed in; complete mails are pulled out. The parser hunts for
 * the 0xAA start marker, checks the length and the 16-bit checksum, and
 * resynchronises on any framing or checksum error. Idle 0xFF bytes between
 * mails are discarded.
 */
#define DECT_RX_BUF  1024u

struct dect_rx_stats {
	uint32_t mails;		/* valid mails delivered */
	uint32_t seq_gaps;	/* sequence number did not increment by 1 */
	uint32_t csum_errors;	/* checksum mismatches */
	uint32_t framing_errors;/* length field out of range */
	uint32_t resyncs;	/* times the 0xAA hunt restarted after an error */
};

struct dect_rx {
	uint8_t  buf[DECT_RX_BUF];
	size_t   len;		/* unconsumed bytes in buf */
	int      last_seq;	/* -1 until the first mail */
	int      resynced;	/* next good mail does not count as a gap */
	struct dect_rx_stats stats;
};

void dect_rx_init(struct dect_rx *rx);

/*
 * Append control-slot bytes. Returns the number of bytes accepted; bytes
 * that do not fit (the caller pulls too rarely) are dropped and counted as
 * a framing error. Call dect_rx_pull() until it returns 0 after pushing.
 */
size_t dect_rx_push(struct dect_rx *rx, const uint8_t *data, size_t n);

/*
 * Pull the next complete mail into out. Returns 1 if a mail was produced,
 * 0 if more bytes are needed. Updates rx->stats.
 */
int dect_rx_pull(struct dect_rx *rx, struct dect_mail *out);

#endif /* OPENDECTCELL_DECT_MAIL_H */

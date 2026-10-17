// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Host side of the STUB upload protocol. Written from
 * docs/stub-loader-protocol.md; section references point there.
 */
#include "loader.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Bytes sent between two checks for chip messages in stage 2 (§1). */
#define POLL_CHUNK 256

static void logf_(const struct fwl_io *io, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void logf_(const struct fwl_io *io, const char *fmt, ...)
{
	char msg[160];
	va_list ap;

	if (!io->log)
		return;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	io->log(io->ctx, msg);
}

unsigned fwl_stage2_baud(const char *name)
{
	if (name && strstr(name, "_slow"))
		return 115200;
	if (name && strstr(name, "_fast"))
		return 460800;
	return 230400;
}

unsigned fwl_stage1_baud(unsigned mode_word, int slow)
{
	if ((mode_word & 0x300) == 0x300)
		return 19200;
	return slow ? 9600 : 115200;
}

int fwl_stage1_slow(unsigned mode_word, unsigned attempt)
{
	/* Attempts 1-11 use the first mode, 12-16 the other one (§2). */
	int first_is_slow = (mode_word & 0x100) != 0;
	return attempt <= FWL_STAGE1_ATTEMPTS - 5 ? first_is_slow
						  : !first_is_slow;
}

/* Step 1: wait for STX. */
static int wait_stx(const struct fwl_io *io)
{
	for (int i = 0; i < FWL_STX_TRIES; i++) {
		int b = io->read_byte(io->ctx, FWL_BYTE_TIMEOUT_MS);
		if (b == FWL_STX)
			return 0;
		if (b >= 0)
			logf_(io, "false answer 0x%02x while waiting for STX", b);
		else if (b != -ETIMEDOUT)
			return b;
	}
	return -ETIMEDOUT;
}

/* Read the error bytes that follow a NAK. */
static void read_nak_code(const struct fwl_io *io, size_t n,
			  struct fwl_result *res)
{
	res->last_nak_len = 0;
	for (size_t i = 0; i < n && i < sizeof(res->last_nak_code); i++) {
		int b = io->read_byte(io->ctx, FWL_BYTE_TIMEOUT_MS);
		if (b < 0)
			break;
		res->last_nak_code[res->last_nak_len++] = (uint8_t)b;
	}
}

/* Step 3: answer to SOH + length. */
static int wait_ack(const struct fwl_io *io, size_t hdr_len,
		    struct fwl_result *res)
{
	for (int stx_seen = 0;;) {
		int b = io->read_byte(io->ctx, FWL_BYTE_TIMEOUT_MS);
		switch (b) {
		case FWL_ACK:
			return 0;
		case FWL_STX:
			logf_(io, "STX violation");
			if (++stx_seen >= 3)
				return -EAGAIN;
			continue;
		case FWL_NAK:
			read_nak_code(io, hdr_len, res);
			logf_(io, "NAK after length (%u code bytes)",
			      res->last_nak_len);
			return -EAGAIN;
		case -ETIMEDOUT:
			logf_(io, "timeout waiting for ACK");
			return -ETIMEDOUT;
		default:
			if (b < 0)
				return b;
			logf_(io, "unexpected 0x%02x waiting for ACK", b);
			return -EAGAIN;
		}
	}
}

/* Step 4, stage 2: react to bytes the chip sends during the data. */
static int check_rx(const struct fwl_io *io, struct fwl_result *res)
{
	for (;;) {
		int b = io->poll_byte(io->ctx);
		if (b == -EAGAIN)
			return 0;
		if (b < 0)
			return b;
		if (b == 0x00)
			continue; /* ignored (§1) */
		if (b == FWL_NAK) {
			read_nak_code(io, 1, res);
			logf_(io, "NAK during data");
		} else {
			logf_(io, "0x%02x during data", b);
		}
		return -EIO;
	}
}

int fwl_stage(const struct fwl_io *io, const struct fwi_image *img,
	      size_t hdr_len, struct fwl_result *res)
{
	int r;

	if (img->len < hdr_len + 1)
		return -EINVAL;
	if ((r = wait_stx(io)))
		return r;

	/* Step 2: SOH and the image's length header, unchanged. */
	uint8_t soh = FWL_SOH;
	if ((r = io->write(io->ctx, &soh, 1)) ||
	    (r = io->write(io->ctx, img->data, hdr_len)))
		return r;
	if ((r = wait_ack(io, hdr_len, res)))
		return r;

	/* Step 4: data without the final XOR byte. */
	const uint8_t *p = img->data + hdr_len;
	size_t left = img->len - hdr_len - 1;
	while (left) {
		size_t n = left < POLL_CHUNK ? left : POLL_CHUNK;
		if ((r = io->write(io->ctx, p, n)))
			return r;
		p += n;
		left -= n;
		/* Not after the last chunk: the chip's checksum answer may
		 * already be waiting (step 5). */
		if (hdr_len == 4 && left && (r = check_rx(io, res)))
			return r;
	}

	/* Steps 5 and 6: the chip's XOR against ours. */
	uint8_t want = img->data[img->len - 1];
	int b = io->read_byte(io->ctx, FWL_BYTE_TIMEOUT_MS);
	if (b == want) {
		uint8_t ack = FWL_ACK;
		return io->write(io->ctx, &ack, 1);
	}
	if (b == FWL_NAK) {
		read_nak_code(io, 1, res);
		logf_(io, "NAK instead of checksum");
		return -EIO;
	}
	if (b < 0 && b != -ETIMEDOUT)
		return b;
	uint8_t nak = FWL_NAK;
	logf_(io, "checksum mismatch: chip %d, image 0x%02x", b, want);
	r = io->write(io->ctx, &nak, 1);
	return r ? r : -EIO;
}

/* Reset sequence before a stage-1 attempt (§2). */
static int chip_reset(const struct fwl_io *io, unsigned baud, int slow)
{
	int r;

	if (slow && (r = io->strap(io->ctx, 1)))
		return r;
	if ((r = io->reset(io->ctx, 1)))
		return r;
	io->sleep_ms(io->ctx, FWL_RESET_HOLD_MS);
	if ((r = io->set_baud(io->ctx, baud)) || (r = io->reset(io->ctx, 0)))
		return r;
	io->sleep_ms(io->ctx, FWL_RESET_SETTLE_MS);
	if (slow && (r = io->strap(io->ctx, 0)))
		return r;
	return 0;
}

static int is_protocol_error(int r)
{
	return r == -EAGAIN || r == -ETIMEDOUT || r == -EIO;
}

static int stage1(const struct fwl_io *io, const struct fwl_params *p,
		  const struct fwi_image *img, struct fwl_result *res)
{
	int r = -ETIMEDOUT, need_reset = 1;

	for (unsigned a = 1; a <= FWL_STAGE1_ATTEMPTS; a++) {
		res->stage1_attempts = a;
		if (need_reset) {
			res->stage1_slow = fwl_stage1_slow(p->mode_word, a);
			res->stage1_baud = fwl_stage1_baud(p->mode_word,
							   res->stage1_slow);
			logf_(io, "stage 1 attempt %u: %s mode, %u baud", a,
			      res->stage1_slow ? "slow" : "fast",
			      res->stage1_baud);
			if ((r = chip_reset(io, res->stage1_baud,
					    res->stage1_slow)))
				return r;
		}
		r = fwl_stage(io, img, 2, res);
		if (!r)
			return 0;
		if (!is_protocol_error(r))
			return r;
		res->last_error = r;
		need_reset = r != -EAGAIN;
	}
	return r;
}

static int stage2(const struct fwl_io *io, const struct fwl_params *p,
		  const struct fwi_image *img, struct fwl_result *res)
{
	int r = -ETIMEDOUT, need_init = 1;

	for (unsigned a = 1; a <= FWL_STAGE2_ATTEMPTS; a++) {
		res->stage2_attempts = a;
		if (need_init) {
			io->sleep_ms(io->ctx, FWL_STAGE2_DELAY_MS);
			if ((r = io->set_baud(io->ctx, p->stage2_baud)))
				return r;
		}
		r = fwl_stage(io, img, 4, res);
		if (!r)
			return 0;
		if (!is_protocol_error(r))
			return r;
		res->last_error = r;
		need_init = r != -EAGAIN;
	}
	return r;
}

int fwl_upload(const struct fwl_io *io, const struct fwl_params *p,
	       const struct fwi_image *first, const struct fwi_image *second,
	       struct fwl_result *res)
{
	struct fwl_result dummy;
	int r = -ETIMEDOUT;

	if (!res)
		res = &dummy;
	memset(res, 0, sizeof(*res));

	for (unsigned round = 1; round <= FWL_UPLOAD_ROUNDS; round++) {
		res->rounds = round;
		res->stage2_attempts = 0;
		r = stage1(io, p, first, res);
		if (r && !is_protocol_error(r))
			return r;
		if (r) {
			logf_(io, "stage 1 failed (%d)", r);
			continue;
		}
		logf_(io, "stage 1 done");
		io->sleep_ms(io->ctx, FWL_STAGE_GAP_MS);
		r = stage2(io, p, second, res);
		if (!r) {
			logf_(io, "stage 2 done: STUB uploaded");
			return 0;
		}
		if (!is_protocol_error(r))
			return r;
		logf_(io, "stage 2 failed (%d)", r);
	}
	return r == -EAGAIN ? -EIO : r;
}

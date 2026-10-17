/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host side of the STUB upload protocol. Specification:
 * docs/stub-loader-protocol.md. Images come from fwimage.h.
 *
 * All I/O goes through struct fwl_io, so the engine runs unchanged on the
 * box (UART + /dev/dect0) and against the chip simulator in tests/.
 */
#ifndef OPENDECTCELL_LOADER_H
#define OPENDECTCELL_LOADER_H

#include "fwimage.h"

#include <stdint.h>

/* Control bytes (doc §1). */
#define FWL_SOH 0x01
#define FWL_STX 0x02
#define FWL_ACK 0x06
#define FWL_NAK 0x15

/* Defaults from doc §1-§3. */
#define FWL_BYTE_TIMEOUT_MS  125
#define FWL_STX_TRIES        5
#define FWL_STAGE1_ATTEMPTS  16
#define FWL_STAGE2_ATTEMPTS  5
#define FWL_UPLOAD_ROUNDS    3
#define FWL_RESET_HOLD_MS    10
#define FWL_RESET_SETTLE_MS  15
#define FWL_STAGE_GAP_MS     10
#define FWL_STAGE2_DELAY_MS  50

/* Platform callbacks. Every function returns 0 or a negative errno. */
struct fwl_io {
	void *ctx;
	/* Next received byte (0-255), or -ETIMEDOUT after timeout_ms. */
	int (*read_byte)(void *ctx, unsigned timeout_ms);
	/* Non-blocking: a received byte, or -EAGAIN if none is pending. */
	int (*poll_byte)(void *ctx);
	int (*write)(void *ctx, const uint8_t *buf, size_t len);
	int (*set_baud)(void *ctx, unsigned baud);
	/* Drive DECT reset: 1 = held in reset, 0 = released. */
	int (*reset)(void *ctx, int asserted);
	/* Boot-mode strap ("rd" line): 1 = driven low, 0 = normal function. */
	int (*strap)(void *ctx, int low);
	void (*sleep_ms)(void *ctx, unsigned ms);
	/* Optional progress and error messages. */
	void (*log)(void *ctx, const char *msg);
};

/* Stage-2 baud rate from the first-level file name (doc §3). */
unsigned fwl_stage2_baud(const char *firstlevel_name);

/* Stage-1 baud rate for a mode word and boot mode (doc §2). */
unsigned fwl_stage1_baud(unsigned mode_word, int slow);

/* Boot mode of a stage-1 attempt, numbered from 1 (doc §2). 1 = slow. */
int fwl_stage1_slow(unsigned mode_word, unsigned attempt);

struct fwl_params {
	unsigned mode_word;     /* the loader mode value-style value, e.g. 0x105 */
	unsigned stage2_baud;   /* from fwl_stage2_baud() */
};

struct fwl_result {
	unsigned rounds;          /* full two-stage rounds used */
	unsigned stage1_attempts; /* in the last round */
	unsigned stage2_attempts;
	int stage1_slow;          /* boot mode that worked */
	unsigned stage1_baud;
	int last_error;           /* last protocol error, for logging */
	uint8_t last_nak_code[4]; /* error bytes after the last NAK */
	unsigned last_nak_len;
	int fatal;                /* platform error: do not retry */
};

/*
 * Run one stage-1 or stage-2 handshake on an already reset/configured
 * link (doc §1). hdr_len is 2 for stage 1, 4 for stage 2.
 * Return 0, or:
 *   -EAGAIN    wait for STX again on the same link (NAK after the length,
 *              repeated STX)
 *   -ETIMEDOUT no STX or no answer: start over
 *   -EIO       NAK or noise during data, or a wrong XOR: start over
 *   other negative errno from the I/O callbacks
 */
int fwl_stage(const struct fwl_io *io, const struct fwi_image *img,
	      size_t hdr_len, struct fwl_result *res);

/*
 * Full upload: chip reset, stage 1, stage 2, with the attempt budgets of
 * doc §1. `res` may be NULL. Return 0 or a negative errno.
 */
int fwl_upload(const struct fwl_io *io, const struct fwl_params *p,
	       const struct fwi_image *first, const struct fwi_image *second,
	       struct fwl_result *res);

#endif

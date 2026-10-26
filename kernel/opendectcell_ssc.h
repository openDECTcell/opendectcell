/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * openDECTcell - PCM/TDM receive path through the Lantiq VR9 SSC (SPI-TDM).
 *
 * Written from docs/7412-spi-tdm-and-loader.md and docs/models/7412-DECT-analysis.md.
 * The register map is the same block mainline drives with spi-lantiq-ssc.
 */
#ifndef _OPENDECTCELL_SSC_H
#define _OPENDECTCELL_SSC_H

#include <linux/clk.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>

#include "opendectcell.h"

/* Board facts for the SSC path, all physical addresses. */
struct dect_ssc_board {
	phys_addr_t ssc_base;
	size_t ssc_size;
	phys_addr_t pinmux_base;	/* XWAY GPIO/pinmux block */
	size_t pinmux_size;
	unsigned int pin_fsc;		/* frame sync / CS: stays a GPIO (GPIO9) */
	unsigned int pin_cs;		/* SSC chip select / frame sync (GPIO10) */
	unsigned int pin_dcl;		/* bit clock (GPIO18) */
	unsigned int pin_di;		/* data chip -> SoC (GPIO17) */
	unsigned int pin_do;		/* data SoC -> chip (GPIO16) */
	unsigned int pin_clk_en;	/* DCL enable line (GPIO7), GPIO output */
};

struct dect_ssc {
	const struct dect_ssc_board *board;
	struct mutex *lock;		/* the device mutex, held by callers */
	struct clk *clk;		/* the SSC module clock gate (clkdev) */
	void __iomem *base;		/* SSC registers */
	void __iomem *pinmux;		/* XWAY GPIO/pinmux block */

	unsigned int step;
	unsigned int slots;
	unsigned int frame_words;	/* 32-bit words per frame */
	unsigned int frame_bytes;
	unsigned int frames_per_block;
	unsigned int n_desc;
	unsigned int block_bytes;

	/* Blocks for read(), filled by the receive engine */
	spinlock_t ring_lock;
	wait_queue_head_t wq;
	u8 *ring;
	unsigned int head, tail;
	bool drop_pending;
	u32 seq;
	struct dect_pcm_stats st;

	/* Receive engine (bring-up placeholder: hrtimer + FIFO polling) */
	struct hrtimer timer;
	ktime_t period;
	unsigned int block_fill;
	u8 *block_buf;
	bool running;
};

int dect_ssc_init(struct dect_ssc *p, const struct dect_ssc_board *board,
		  struct mutex *lock);
void dect_ssc_exit(struct dect_ssc *p);

/* Called with *p->lock held. */
void dect_ssc_teardown(struct dect_ssc *p);
long dect_ssc_ioctl(struct dect_ssc *p, unsigned int cmd, void __user *argp);

/* Take *p->lock themselves. */
ssize_t dect_ssc_read(struct dect_ssc *p, char __user *buf, size_t count,
		      bool nonblock);
ssize_t dect_ssc_write(struct dect_ssc *p, const char __user *buf,
		       size_t count, bool nonblock);
__poll_t dect_ssc_poll(struct dect_ssc *p, struct file *file,
		       poll_table *wait);

#endif /* _OPENDECTCELL_SSC_H */

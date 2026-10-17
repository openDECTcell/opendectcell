/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * openDECTcell - PCM/TDM receive path through the IPQ4019 audio subsystem.
 *
 * Written from docs/pcm-tdm-ipq4019.md (section numbers in comments).
 */
#ifndef _OPENDECTCELL_PCM_H
#define _OPENDECTCELL_PCM_H

#include <linux/clk.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>

#include "opendectcell.h"

struct dect_pcm_pin {
	unsigned int gpio;
	unsigned int func;
	bool output;			/* TLMM output enable */
};

/* One DMA direction: an MBOX, its stereo block and its interrupt. */
struct dect_pcm_dir {
	phys_addr_t mbox_base;
	phys_addr_t stereo_base;
	u32 irq_spi;
};

/* Board facts for the PCM path, all physical addresses (doc section 3). */
struct dect_pcm_board {
	phys_addr_t adss_base;		/* ADSS global + ADCC registers */
	size_t adss_size;
	struct dect_pcm_dir rx;		/* MBOX3 / STEREO3 */
	struct dect_pcm_dir tx;		/* MBOX0 / STEREO0 */
	const char *gcc_compatible;
	u32 gcc_ahb_clk;		/* GCC_AUDIO_AHB_CLK id */
	phys_addr_t gcc_ahb_cbcr;	/* its branch register, read only */
	const char *gic_compatible;
	u32 irq_type;
	const struct dect_pcm_pin *pins;	/* set while receiving */
	unsigned int n_pins;
	const struct dect_pcm_pin *tx_pins;	/* set while transmitting */
	unsigned int n_tx_pins;
};

struct dect_pcm;

struct dect_pcm_chan {
	struct dect_pcm *p;
	bool tx;
	u32 irq_spi;
	void __iomem *mbox;
	void __iomem *stereo;
	int irq;
	void *dma_cpu;
	dma_addr_t dma_addr;
	size_t dma_size;
	unsigned int next_desc;
};

struct dect_pcm {
	const struct dect_pcm_board *board;
	struct mutex *lock;		/* the device mutex, held by callers */
	void __iomem *tlmm;		/* TLMM control register of tlmm_first */
	unsigned int tlmm_first;
	struct platform_device *dma_pdev;

	struct dect_pcm_config cfg;
	unsigned int step;
	unsigned int block_bytes;

	struct clk *ahb_clk;		/* only if we had to enable it */
	void __iomem *gcc_ahb_cbcr;
	void __iomem *adss;
	struct dect_pcm_chan rx;
	struct dect_pcm_chan tx;

	/* What step 2 changed, so that teardown restores it. */
	u32 ahb_cbcr_set;
	bool b1k_set;
	u32 pin_saved[4];
	u32 tx_pin_saved[4];
	bool tx_configured;		/* step 4 set up transmit too */

	/* DMA ring layout (step 6), the same for both directions */
	unsigned int desc_stride;
	unsigned int buf_stride;
	unsigned int buf_offset;

	/* Blocks for read(), filled by the interrupt handler */
	spinlock_t ring_lock;
	wait_queue_head_t wq;
	u8 *ring;
	unsigned int ring_slots;
	unsigned int slot_bytes;
	unsigned int head, tail;
	bool drop_pending;
	u32 seq;
	struct dect_pcm_stats st;
	u8 tx_frame[32];		/* under ring_lock */
};

int dect_pcm_init(struct dect_pcm *p, const struct dect_pcm_board *board,
		  struct mutex *lock, void __iomem *tlmm,
		  unsigned int tlmm_first);
void dect_pcm_exit(struct dect_pcm *p);

/* Called with *p->lock held. */
void dect_pcm_teardown(struct dect_pcm *p);
long dect_pcm_ioctl(struct dect_pcm *p, unsigned int cmd, void __user *argp);

/* Take *p->lock themselves. */
ssize_t dect_pcm_read(struct dect_pcm *p, char __user *buf, size_t count,
		      bool nonblock);
__poll_t dect_pcm_poll(struct dect_pcm *p, struct file *file,
		       poll_table *wait);

#endif /* _OPENDECTCELL_PCM_H */

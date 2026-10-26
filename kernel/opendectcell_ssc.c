// SPDX-License-Identifier: GPL-2.0-only
/*
 * openDECTcell - PCM/TDM receive path through the Lantiq VR9 SSC (SPI-TDM).
 *
 * Bring-up path only: steps 1 (clock), 3 (TDM) program the SSC as observed on
 * the 7412; step 2 muxes the pins; step 6 runs a receive engine that drains
 * the SSC RX FIFO from an hrtimer. The intended production path is an
 * SSC FIFO/DMA interrupt; see the maintainers' notes. Do not treat as production ready.
 *
 * Facts: docs/7412-spi-tdm-and-loader.md. The register map matches mainline
 * spi-lantiq-ssc for the same block.
 */
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/hrtimer.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "opendectcell_ssc.h"

/* VR9 SSC register map (physical base 0x1E100800). */
#define SSC_CLC		0x00
#define SSC_ID		0x08
#define SSC_CON		0x10
#define SSC_STATE	0x14
#define SSC_WHBSTATE	0x18
#define SSC_RB		0x24
#define SSC_RXFCON	0x30
#define SSC_TXFCON	0x34
#define SSC_FSTAT	0x38
#define SSC_BR		0x40

#define SSC_CLC_RMC_S		8

#define SSC_ID_RXFS_S		16
#define SSC_ID_TXFS_S		24

#define SSC_CON_MSB_FIRST	BIT(4)
#define SSC_CON_PO		BIT(6)
#define SSC_CON_WIDTH_S		16
#define SSC_CON_ENABLE_BYTE_VALID BIT(22)
#define SSC_CON_WIDTH(w)	((((w) - 1) & 0x1f) << SSC_CON_WIDTH_S)

#define SSC_STATE_ENABLED	BIT(0)

#define SSC_WHBSTATE_DISABLE	BIT(0)
#define SSC_WHBSTATE_ENABLE	BIT(1)
#define SSC_WHBSTATE_SLAVE_MODE	BIT(2)
#define SSC_WHBSTATE_CLR_ALL_ERR 0x00000f50

#define SSC_XFCON_ENABLE	BIT(0)
#define SSC_XFCON_FLUSH		BIT(1)
#define SSC_XFCON_ITL_S		8

#define SSC_FSTAT_RX_WORDS_M	0x3f

/* XWAY GPIO/pinmux: function index in ALT0/ALT1 bits, 0x30 per port. */
#define PINMUX_PORT_OFF		0x30
#define PINMUX_DIR		0x08
#define PINMUX_ALT0		0x0c
#define PINMUX_ALT1		0x10

/* PMU module gate: SPI (SSC) is module 8. */
#define DECT_PMU_BASE		0x1f102000
#define DECT_PMU_PWDCR		0x1c
#define DECT_PMU_PWDSR		0x20
#define DECT_PMU_SPI		BIT(8)

#define DECT_SSC_FRAME_HZ	8000
#define DECT_SSC_DEF_SLOTS	8
#define DECT_SSC_DEF_FPB	32
#define DECT_SSC_DEF_NDESC	4
#define DECT_SSC_MAX_NDESC	16

static inline u32 ssc_rd(struct dect_ssc *s, u32 off)
{
	return readl(s->base + off);
}

static inline void ssc_wr(struct dect_ssc *s, u32 off, u32 val)
{
	writel(val, s->base + off);
}

static int ssc_set_run(struct dect_ssc *s, bool run, unsigned int timeout_us)
{
	unsigned int waited = 0;

	/* WHBSTATE is write-modified and reads back as zero: never
	 * read-modify-write it, write exactly the command bit. */
	ssc_wr(s, SSC_WHBSTATE, run ? SSC_WHBSTATE_ENABLE :
	       SSC_WHBSTATE_DISABLE);
	wmb();
	while (!!(ssc_rd(s, SSC_STATE) & SSC_STATE_ENABLED) != run) {
		if (waited >= timeout_us)
			return -ETIMEDOUT;
		udelay(100);
		waited += 100;
	}
	return 0;
}

static void ssc_pinmux_bit(struct dect_ssc *s, unsigned int pin,
			   unsigned int reg_off, bool set)
{
	void __iomem *reg = s->pinmux +
		(pin / 16) * PINMUX_PORT_OFF + reg_off;
	u32 bit = BIT(pin % 16);
	u32 v = readl(reg);

	v = set ? (v | bit) : (v & ~bit);
	writel(v, reg);
}

/* SPI function is index 1 (ALT0) on GPIO16/17/18, index 2 (ALT1) on GPIO9. */
static void ssc_pinmux_mux(struct dect_ssc *s, unsigned int pin, bool alt0,
			   bool alt1)
{
	ssc_pinmux_bit(s, pin, PINMUX_ALT0, alt0);
	ssc_pinmux_bit(s, pin, PINMUX_ALT1, alt1);
}

/* ------------------------------------------------------------------ */
/* Steps                                                              */
/* ------------------------------------------------------------------ */
static int ssc_step_clock(struct dect_ssc *s)
{
	void __iomem *pmu;
	u32 id;

	/* Enable the SSC module clock gate (PMU module SPI). */
	pmu = ioremap(DECT_PMU_BASE, 0x1000);
	if (pmu) {
		writel(readl(pmu + DECT_PMU_PWDCR) & ~DECT_PMU_SPI,
		       pmu + DECT_PMU_PWDCR);
		iounmap(pmu);
	}

	ssc_wr(s, SSC_CLC, 1 << SSC_CLC_RMC_S);
	id = ssc_rd(s, SSC_ID);
	pr_info("opendectcell: SSC id=0x%08x (rx_fifo=%u tx_fifo=%u)\n", id,
		((id >> SSC_ID_RXFS_S) & 0x3f) + 1,
		((id >> SSC_ID_TXFS_S) & 0x3f) + 1);
	if (!id || id == 0xffffffff)
		return -ENODEV;
	return 0;
}

static int ssc_step_iface(struct dect_ssc *s)
{
	const struct dect_ssc_board *b = s->board;

	if (!s->pinmux)
		return -ENODEV;
	/* DI/DO/CLK are GPIO16/17/18 (SPI = ALT0); FSC is GPIO9 (SPI = ALT1). */
	ssc_pinmux_mux(s, b->pin_di, true, false);
	ssc_pinmux_mux(s, b->pin_do, true, false);
	ssc_pinmux_mux(s, b->pin_dcl, true, false);
	ssc_pinmux_mux(s, b->pin_fsc, false, true);
	/* Slave mode: only the data-out pad is driven by the SSC. */
	ssc_pinmux_bit(s, b->pin_di, PINMUX_DIR, false);
	ssc_pinmux_bit(s, b->pin_do, PINMUX_DIR, true);
	ssc_pinmux_bit(s, b->pin_dcl, PINMUX_DIR, false);
	ssc_pinmux_bit(s, b->pin_fsc, PINMUX_DIR, false);
	return 0;
}

static int ssc_step_tdm(struct dect_ssc *s)
{
	u32 itl = s->frame_words ?: DECT_SSC_DEF_SLOTS / 2;

	if (ssc_set_run(s, false, 100000))
		pr_warn("openDECTcell: SSC did not enter configuration mode\n");

	ssc_wr(s, SSC_WHBSTATE,
	       SSC_WHBSTATE_SLAVE_MODE | SSC_WHBSTATE_CLR_ALL_ERR);
	ssc_wr(s, SSC_BR, 1);
	if (itl > 63)
		itl = 63;
	ssc_wr(s, SSC_RXFCON,
	       SSC_XFCON_ENABLE | SSC_XFCON_FLUSH | (itl << SSC_XFCON_ITL_S));
	ssc_wr(s, SSC_TXFCON,
	       SSC_XFCON_ENABLE | SSC_XFCON_FLUSH | (1 << SSC_XFCON_ITL_S));
	ssc_wr(s, SSC_CON,
	       SSC_CON_WIDTH(32) | SSC_CON_MSB_FIRST | SSC_CON_PO |
	       SSC_CON_ENABLE_BYTE_VALID);

	if (ssc_set_run(s, true, 100000)) {
		pr_err("opendectcell: SSC did not enter run mode (STATE=0x%08x)\n",
		       ssc_rd(s, SSC_STATE));
		return -ETIMEDOUT;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Receive engine (bring-up placeholder)                              */
/* ------------------------------------------------------------------ */
static void ssc_enqueue_block(struct dect_ssc *s)
{
	unsigned int next;

	spin_lock(&s->ring_lock);
	next = (s->head + 1) % s->n_desc;
	if (next == s->tail) {
		s->tail = (s->tail + 1) % s->n_desc;
		s->drop_pending = true;
		s->st.dropped++;
	}
	memcpy(s->ring + (size_t)s->head * s->block_bytes, s->block_buf,
	       s->block_bytes);
	s->head = next;
	s->seq++;
	s->st.blocks++;
	spin_unlock(&s->ring_lock);
	wake_up_interruptible(&s->wq);
}

static void ssc_drain_frame(struct dect_ssc *s)
{
	unsigned int words = ssc_rd(s, SSC_FSTAT) & SSC_FSTAT_RX_WORDS_M;
	unsigned int i;

	if (words > s->frame_words)
		words = s->frame_words;
	for (i = 0; i < words; i++) {
		u32 v = ssc_rd(s, SSC_RB);
		u8 *p = s->block_buf +
			(size_t)s->block_fill * s->frame_bytes + i * 4;

		p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
	}
	if (++s->block_fill >= s->frames_per_block) {
		s->block_fill = 0;
		ssc_enqueue_block(s);
	}
}

static enum hrtimer_restart ssc_timer_fn(struct hrtimer *t)
{
	struct dect_ssc *s = container_of(t, struct dect_ssc, timer);

	s->st.irqs++;
	ssc_drain_frame(s);
	hrtimer_forward_now(t, s->period);
	return HRTIMER_RESTART;
}

static int ssc_engine_start(struct dect_ssc *s)
{
	s->block_fill = 0;
	if (!s->block_buf)
		return -ENOMEM;
	hrtimer_init(&s->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	s->timer.function = ssc_timer_fn;
	s->period = ktime_set(0, 1000000000ULL / DECT_SSC_FRAME_HZ);
	s->running = true;
	hrtimer_start(&s->timer, s->period, HRTIMER_MODE_REL);
	return 0;
}

static void ssc_engine_stop(struct dect_ssc *s)
{
	if (!s->running)
		return;
	hrtimer_cancel(&s->timer);
	s->running = false;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */
int dect_ssc_init(struct dect_ssc *p, const struct dect_ssc_board *board,
		  struct mutex *lock)
{
	p->board = board;
	p->lock = lock;
	p->slots = DECT_SSC_DEF_SLOTS;
	p->frame_words = p->slots / 2;
	p->frame_bytes = p->frame_words * 4;
	p->base = ioremap(board->ssc_base, board->ssc_size);
	if (!p->base)
		return -ENOMEM;
	p->pinmux = ioremap(board->pinmux_base, board->pinmux_size);
	if (!p->pinmux) {
		iounmap(p->base);
		p->base = NULL;
		return -ENOMEM;
	}
	spin_lock_init(&p->ring_lock);
	init_waitqueue_head(&p->wq);
	return 0;
}

void dect_ssc_teardown(struct dect_ssc *p)
{
	ssc_engine_stop(p);
	if (p->base)
		ssc_set_run(p, false, 20000);
	kfree(p->block_buf);
	p->block_buf = NULL;
	kfree(p->ring);
	p->ring = NULL;
	p->head = p->tail = 0;
	p->step = DECT_PCM_STEP_OFF;
}

void dect_ssc_exit(struct dect_ssc *p)
{
	dect_ssc_teardown(p);
	if (p->pinmux) {
		iounmap(p->pinmux);
		p->pinmux = NULL;
	}
	if (p->base) {
		iounmap(p->base);
		p->base = NULL;
	}
}

/* ------------------------------------------------------------------ */
/* ioctl                                                              */
/* ------------------------------------------------------------------ */
static int ssc_config(struct dect_ssc *s, struct dect_pcm_config *c)
{
	unsigned int slots = c->slots ?: DECT_SSC_DEF_SLOTS;
	unsigned int fpb = c->frames_per_block ?: DECT_SSC_DEF_FPB;
	unsigned int ndesc = c->n_desc ?: DECT_SSC_DEF_NDESC;

	if (s->step != DECT_PCM_STEP_OFF)
		return -EBUSY;
	if (slots < 2 || slots > 16 || (slots & 1))
		return -EINVAL;
	if (fpb < 1 || fpb > 4095 / (slots / 2 * 4))
		return -EINVAL;
	if (ndesc < 2 || ndesc > DECT_SSC_MAX_NDESC)
		return -EINVAL;

	s->slots = slots;
	s->frames_per_block = fpb;
	s->n_desc = ndesc;
	s->frame_words = slots / 2;
	s->frame_bytes = s->frame_words * 4;
	s->block_bytes = fpb * s->frame_bytes;

	s->ring = kzalloc((size_t)s->n_desc * s->block_bytes, GFP_KERNEL);
	s->block_buf = kzalloc(s->block_bytes, GFP_KERNEL);
	if (!s->ring || !s->block_buf) {
		kfree(s->ring);
		kfree(s->block_buf);
		s->ring = NULL;
		s->block_buf = NULL;
		return -ENOMEM;
	}
	c->slots = slots;
	c->frames_per_block = fpb;
	c->n_desc = ndesc;
	return 0;
}

static int ssc_step(struct dect_ssc *s, unsigned int n)
{
	int ret;

	if (n == DECT_PCM_STEP_OFF) {
		dect_ssc_teardown(s);
		return 0;
	}
	if (n != s->step + 1)
		return -EINVAL;

	switch (n) {
	case DECT_PCM_STEP_CLOCK:
		ret = ssc_step_clock(s);
		break;
	case DECT_PCM_STEP_IFACE:
		ret = ssc_step_iface(s);
		break;
	case DECT_PCM_STEP_PADCLK:
		ret = 0;	/* no SSC equivalent */
		break;
	case DECT_PCM_STEP_TDM:
		ret = ssc_step_tdm(s);
		break;
	case DECT_PCM_STEP_STEREO:
		ret = 0;	/* no SSC equivalent */
		break;
	case DECT_PCM_STEP_DMA:
		ret = ssc_engine_start(s);
		break;
	default:
		ret = -EINVAL;
	}
	if (!ret)
		s->step = n;
	return ret;
}

static int ssc_regs(struct dect_ssc *s, struct dect_pcm_regs *r)
{
	memset(r, 0, sizeof(*r));
	r->step = s->step;
	r->valid = DECT_PCM_REGS_GLB;
	r->glb[0] = ssc_rd(s, SSC_CON);
	r->glb[1] = ssc_rd(s, SSC_STATE);
	r->glb[2] = ssc_rd(s, SSC_WHBSTATE);
	r->glb[3] = ssc_rd(s, SSC_FSTAT);
	r->glb[4] = ssc_rd(s, SSC_ID);
	return 0;
}

long dect_ssc_ioctl(struct dect_ssc *s, unsigned int cmd, void __user *argp)
{
	struct dect_pcm_config cfg;
	struct dect_pcm_regs regs;
	struct dect_pcm_stats st;
	__u32 n;
	int ret;

	switch (cmd) {
	case DECT_IOC_PCM_CONFIG:
		if (copy_from_user(&cfg, argp, sizeof(cfg)))
			return -EFAULT;
		ret = ssc_config(s, &cfg);
		if (ret)
			return ret;
		return copy_to_user(argp, &cfg, sizeof(cfg)) ? -EFAULT : 0;
	case DECT_IOC_PCM_STEP:
		if (copy_from_user(&n, argp, sizeof(n)))
			return -EFAULT;
		return ssc_step(s, n);
	case DECT_IOC_PCM_REGS:
		ret = ssc_regs(s, &regs);
		if (ret)
			return ret;
		return copy_to_user(argp, &regs, sizeof(regs)) ? -EFAULT : 0;
	case DECT_IOC_PCM_STATS:
		spin_lock(&s->ring_lock);
		st = s->st;
		st.step = s->step;
		st.block_bytes = s->block_bytes;
		spin_unlock(&s->ring_lock);
		return copy_to_user(argp, &st, sizeof(st)) ? -EFAULT : 0;
	default:
		return -ENOTTY;
	}
}

/* ------------------------------------------------------------------ */
/* read / write / poll                                                */
/* ------------------------------------------------------------------ */
ssize_t dect_ssc_read(struct dect_ssc *p, char __user *buf, size_t count,
		      bool nonblock)
{
	struct dect_pcm_block hdr;
	unsigned long flags;
	size_t need;
	int ret;

	if (p->step != DECT_PCM_STEP_DMA || !p->ring)
		return -ENODATA;
	need = sizeof(hdr) + p->block_bytes;
	if (count < need)
		return -EINVAL;

	for (;;) {
		spin_lock_irqsave(&p->ring_lock, flags);
		if (p->tail != p->head)
			break;
		spin_unlock_irqrestore(&p->ring_lock, flags);
		if (nonblock)
			return -EAGAIN;
		ret = wait_event_interruptible(p->wq, p->tail != p->head);
		if (ret)
			return ret;
	}

	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = DECT_PCM_BLOCK_MAGIC;
	hdr.seq = p->seq;
	hdr.len = p->block_bytes;
	hdr.ts_ns = ktime_get_ns();
	if (p->drop_pending) {
		hdr.flags |= DECT_PCM_BF_DROPPED;
		p->drop_pending = false;
	}
	spin_unlock_irqrestore(&p->ring_lock, flags);

	if (copy_to_user(buf, &hdr, sizeof(hdr)))
		return -EFAULT;
	if (copy_to_user(buf + sizeof(hdr),
			 p->ring + (size_t)p->tail * p->block_bytes,
			 p->block_bytes))
		return -EFAULT;

	spin_lock_irqsave(&p->ring_lock, flags);
	p->tail = (p->tail + 1) % p->n_desc;
	spin_unlock_irqrestore(&p->ring_lock, flags);
	return need;
}

ssize_t dect_ssc_write(struct dect_ssc *p, const char __user *buf,
		       size_t count, bool nonblock)
{
	/* Transmit is not implemented yet (receive-only bring-up). */
	return -ENODATA;
}

__poll_t dect_ssc_poll(struct dect_ssc *p, struct file *file, poll_table *wait)
{
	poll_wait(file, &p->wq, wait);
	if (p->step == DECT_PCM_STEP_DMA && p->tail != p->head)
		return EPOLLIN | EPOLLRDNORM;
	return 0;
}

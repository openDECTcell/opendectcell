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
#include <linux/kfifo.h>
#include <linux/hrtimer.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
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
#define SSC_TB		0x20
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
#define SSC_WHBSTATE_MASTER_MODE 0x00000008
#define SSC_WHBSTATE_CLR_ALL_ERR 0x00000f50

#define SSC_XFCON_ENABLE	BIT(0)
#define SSC_XFCON_FLUSH		BIT(1)
#define SSC_XFCON_ITL_S		8

#define SSC_FSTAT_RX_WORDS_M	0x3f
#define SSC_FSTAT_TX_WORDS_M	0x3f00
#define SSC_STATE_RX_EOM		0x80000000
#define SSC_STATE_TX_EOM		0x08000000

/* XWAY GPIO/pinmux: function index in ALT0/ALT1 bits, 0x30 per port. */
#define PINMUX_PORT_OFF		0x30
#define PINMUX_OUT		0x00
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
#define DECT_SSC_TX_QUEUE	32768

static inline u32 ssc_rd(struct dect_ssc *s, u32 off)
{
	return __raw_readl(s->base + off);
}

static inline void ssc_wr(struct dect_ssc *s, u32 off, u32 val)
{
	__raw_writel(val, s->base + off);
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
	u32 v = __raw_readl(reg);

	v = set ? (v | bit) : (v & ~bit);
	__raw_writel(v, reg);
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
	struct clk *clk;
	u32 id;

	/*
	 * Enable the SSC module clock. The kernel registers this gate through
	 * clkdev as "1e100800.spi"; mainline adds it powered down
	 * (clkdev_add_pmu(..., deactivate = 1)), so enabling it here is what
	 * actually brings the block up. Where the lookup fails, fall back to
	 * clearing the PMU SPI gate bit directly. The clock is taken once, on
	 * the first step 1, and held until module unload (dect_ssc_exit()).
	 */
	if (s->clk)
		goto enabled;
	clk = clk_get_sys("1e100800.spi", NULL);
	if (!IS_ERR(clk)) {
		if (clk_prepare_enable(clk)) {
			pr_warn("openDECTcell: cannot enable the SSC clock\n");
			clk_put(clk);
		} else {
			s->clk = clk;
		}
	} else {
		pmu = ioremap(DECT_PMU_BASE, 0x1000);
		if (pmu) {
			__raw_writel(__raw_readl(pmu + DECT_PMU_PWDCR) & ~DECT_PMU_SPI,
			       pmu + DECT_PMU_PWDCR);
			iounmap(pmu);
		}
	}

enabled:
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
	/*
	 * Put the SSC pins into the SPI function: GPIO16 (spi_di), GPIO17
	 * (spi_do), GPIO18 (spi_clk) and GPIO10 (spi_cs4) are all SPI = f1
	 * (ALT0), as mainline's spi_pins/spi_cs4_pins groups do. GPIO9 (fsc)
	 * and GPIO7 (clk_en) stay plain GPIOs.
	 */
	ssc_pinmux_mux(s, b->pin_do, true, false);
	ssc_pinmux_mux(s, b->pin_di, true, false);
	ssc_pinmux_mux(s, b->pin_dcl, true, false);
	ssc_pinmux_mux(s, b->pin_cs, true, false);

	/* Pad directions as in HW209: data-out and the CS/frame-sync line are
	 * driven; the clock and data-in pads must be inputs. */
	ssc_pinmux_bit(s, b->pin_do, PINMUX_DIR, true);
	ssc_pinmux_bit(s, b->pin_di, PINMUX_DIR, false);
	ssc_pinmux_bit(s, b->pin_dcl, PINMUX_DIR, false);
	ssc_pinmux_bit(s, b->pin_cs, PINMUX_DIR, true);

	/* GPIO9 (the frame-sync monitor) stays a plain input. */
	ssc_pinmux_mux(s, b->pin_fsc, false, false);
	ssc_pinmux_bit(s, b->pin_fsc, PINMUX_DIR, false);

	/* The DCL enable line is a plain output, driven high. */
	ssc_pinmux_mux(s, b->pin_clk_en, false, false);
	ssc_pinmux_bit(s, b->pin_clk_en, PINMUX_DIR, true);
	ssc_pinmux_bit(s, b->pin_clk_en, PINMUX_OUT, true);
	return 0;
}

static int ssc_master;
module_param_named(master, ssc_master, int, 0644);
MODULE_PARM_DESC(master, "run the SSC as the PCM bit-clock master");

static int ssc_step_tdm(struct dect_ssc *s)
{
	u32 itl = s->frame_words ?: DECT_SSC_DEF_SLOTS / 2;

	if (ssc_set_run(s, false, 100000))
		pr_warn("openDECTcell: SSC did not enter configuration mode\n");

	ssc_wr(s, SSC_WHBSTATE, (ssc_master ? SSC_WHBSTATE_MASTER_MODE :
				 SSC_WHBSTATE_SLAVE_MODE) |
	       SSC_WHBSTATE_CLR_ALL_ERR);
	/* Slave ignores the baud reload; master uses it for the bit clock. */
	ssc_wr(s, SSC_BR, ssc_master ? 48 : 1);
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
	unsigned int i, got = s->frame_got;

	if (words > s->frame_words * 2)
		words = s->frame_words * 2;
	/* Read words until the controller marks end-of-frame (RX_EOM); the
	 * chip's FSC delimits frames, so this keeps the slots aligned. */
	for (i = 0; i < words && got < s->frame_words; i++) {
		u32 v = ssc_rd(s, SSC_RB);
		u8 *p = s->block_buf +
			(size_t)s->block_fill * s->frame_bytes + got * 4;

		p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
		got++;
		if (ssc_rd(s, SSC_STATE) & SSC_STATE_RX_EOM)
			break;
	}
	s->frame_got = got;
	if (got < s->frame_words)
		return;
	s->frame_got = 0;
	wake_up_interruptible(&s->tx_wq);
	/* Refill the transmit FIFO with the current frame template while
	 * there is room (the slave shifts it out as the peer clocks). */
	if (s->tx_idle) {
		u8 frame[32];
		unsigned long flags;
		unsigned int txl, room;

		spin_lock_irqsave(&s->ring_lock, flags);
		if (kfifo_len(&s->tx_q) >= s->slots * 2 &&
		    kfifo_out(&s->tx_q, frame, s->slots * 2) == s->slots * 2) {
			s->st.tx_frames++;
		} else {
			memcpy(frame, s->tx_idle, s->slots * 2);
		}
		spin_unlock_irqrestore(&s->ring_lock, flags);

		txl = (ssc_rd(s, SSC_FSTAT) & SSC_FSTAT_TX_WORDS_M) >> 8;
		room = 8 > txl ? 8 - txl : 0;
		for (i = 0; i < s->frame_words && i < room; i++) {
			u8 *p = frame + i * 4;

			ssc_wr(s, SSC_TB,
			       (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
		}
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
	mutex_init(&p->read_lock);
	mutex_init(&p->write_lock);
	init_waitqueue_head(&p->wq);
	init_waitqueue_head(&p->tx_wq);
	return 0;
}

/* Called with read_lock and write_lock held, the engine stopped. */
static void ssc_free_buffers(struct dect_ssc *p)
{
	kfree(p->block_buf);
	p->block_buf = NULL;
	kfree(p->ring);
	p->ring = NULL;
	kfree(p->tx_idle);
	p->tx_idle = NULL;
	if (p->tx_q_ready)
		kfifo_free(&p->tx_q);
	p->tx_q_ready = false;
	p->head = p->tail = 0;
}

void dect_ssc_teardown(struct dect_ssc *p)
{
	ssc_engine_stop(p);
	/*
	 * Only stop a controller this engine took over (step 1 or later). At
	 * step 0 the SSC may be run by another driver -- the generic SSC PCM
	 * transport on the 7412 -- and closing /dev/dect0 after dect-fwload
	 * used to switch it off (observed on a 7412, 2026-10-29).
	 */
	if (p->base && p->step != DECT_PCM_STEP_OFF)
		ssc_set_run(p, false, 20000);
	/*
	 * read() and write() run without *p->lock: wake the sleepers, which
	 * see step 0 and leave, and wait for a copy in progress before the
	 * buffers go (security audit 2026-10-29).
	 */
	p->step = DECT_PCM_STEP_OFF;
	wake_up_interruptible(&p->wq);
	wake_up_interruptible(&p->tx_wq);
	mutex_lock(&p->read_lock);
	mutex_lock(&p->write_lock);
	ssc_free_buffers(p);
	mutex_unlock(&p->write_lock);
	mutex_unlock(&p->read_lock);
}

void dect_ssc_exit(struct dect_ssc *p)
{
	dect_ssc_teardown(p);
	if (p->clk) {
		clk_disable_unprepare(p->clk);
		clk_put(p->clk);
		p->clk = NULL;
	}
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

	/* A second CONFIG replaces the buffers of the first. */
	mutex_lock(&s->read_lock);
	mutex_lock(&s->write_lock);
	ssc_free_buffers(s);
	mutex_unlock(&s->write_lock);
	mutex_unlock(&s->read_lock);

	s->ring = kzalloc((size_t)s->n_desc * s->block_bytes, GFP_KERNEL);
	s->block_buf = kzalloc(s->block_bytes, GFP_KERNEL);
	s->tx_idle = kmalloc(s->slots * 2, GFP_KERNEL);
	if (s->tx_idle)
		memset(s->tx_idle, 0xff, s->slots * 2);
	if (!s->ring || !s->block_buf || !s->tx_idle)
		goto err;
	if (kfifo_alloc(&s->tx_q, DECT_SSC_TX_QUEUE, GFP_KERNEL))
		goto err;
	s->tx_q_ready = true;

	c->slots = slots;
	c->frames_per_block = fpb;
	c->n_desc = ndesc;
	return 0;
err:
	mutex_lock(&s->read_lock);
	mutex_lock(&s->write_lock);
	ssc_free_buffers(s);
	mutex_unlock(&s->write_lock);
	mutex_unlock(&s->read_lock);
	return -ENOMEM;
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
	/* At step 0 the SSC clock may be gated: do not touch the block. */
	if (s->step == DECT_PCM_STEP_OFF)
		return -ENODATA;
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
	unsigned long flags;
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
		/* the 8 kHz hrtimer takes ring_lock in hard-irq context */
		spin_lock_irqsave(&s->ring_lock, flags);
		st = s->st;
		st.step = s->step;
		st.block_bytes = s->block_bytes;
		spin_unlock_irqrestore(&s->ring_lock, flags);
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
	ssize_t ret;

	if (mutex_lock_interruptible(&p->read_lock))
		return -ERESTARTSYS;
	for (;;) {
		/* Under read_lock: teardown may have run while asleep. */
		if (p->step != DECT_PCM_STEP_DMA || !p->ring) {
			ret = -ENODATA;
			goto out;
		}
		need = sizeof(hdr) + p->block_bytes;
		if (count < need) {
			ret = -EINVAL;
			goto out;
		}
		spin_lock_irqsave(&p->ring_lock, flags);
		if (p->tail != p->head)
			break;
		spin_unlock_irqrestore(&p->ring_lock, flags);
		if (nonblock) {
			ret = -EAGAIN;
			goto out;
		}
		mutex_unlock(&p->read_lock);
		ret = wait_event_interruptible(p->wq, p->tail != p->head ||
					       p->step != DECT_PCM_STEP_DMA);
		if (ret)
			return ret;
		if (mutex_lock_interruptible(&p->read_lock))
			return -ERESTARTSYS;
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

	/* read_lock keeps the ring allocated across the copy. */
	if (copy_to_user(buf, &hdr, sizeof(hdr)) ||
	    copy_to_user(buf + sizeof(hdr),
			 p->ring + (size_t)p->tail * p->block_bytes,
			 p->block_bytes)) {
		ret = -EFAULT;
		goto out;
	}

	spin_lock_irqsave(&p->ring_lock, flags);
	p->tail = (p->tail + 1) % p->n_desc;
	spin_unlock_irqrestore(&p->ring_lock, flags);
	ret = need;
out:
	mutex_unlock(&p->read_lock);
	return ret;
}

/* Room for a frame, or the engine was torn down. */
static bool ssc_tx_room(struct dect_ssc *p, unsigned int frame)
{
	return kfifo_avail(&p->tx_q) >= frame ||
	       p->step != DECT_PCM_STEP_DMA;
}

ssize_t dect_ssc_write(struct dect_ssc *p, const char __user *buf,
		       size_t count, bool nonblock)
{
	unsigned int frame;
	size_t done = 0;
	ssize_t ret;

	if (mutex_lock_interruptible(&p->write_lock))
		return -ERESTARTSYS;
	for (;;) {
		unsigned int avail;
		size_t n;
		u8 tmp[256];
		unsigned long flags;

		/* Under write_lock: teardown may have run while asleep. */
		if (p->step != DECT_PCM_STEP_DMA || !p->tx_q_ready) {
			if (!done)
				pr_info("opendectcell: write ENODATA step=%u txq=%d\n",
					p->step, p->tx_q_ready);
			ret = done ? done : -ENODATA;
			goto out;
		}
		frame = p->slots * 2;
		if (!count || (count - done) % frame) {
			if (!done)
				pr_info("opendectcell: write EINVAL count=%zu frame=%u\n",
					count, frame);
			ret = done ? done : -EINVAL;
			goto out;
		}
		if (done >= count)
			break;

		avail = kfifo_avail(&p->tx_q);
		n = min_t(size_t, count - done, avail);
		n = min_t(size_t, n, sizeof(tmp) / frame * frame);
		n -= n % frame;
		if (!n) {
			if (nonblock) {
				ret = done ? done : -EAGAIN;
				goto out;
			}
			mutex_unlock(&p->write_lock);
			if (wait_event_interruptible(p->tx_wq,
						     ssc_tx_room(p, frame)))
				return done ? done : -ERESTARTSYS;
			if (mutex_lock_interruptible(&p->write_lock))
				return done ? done : -ERESTARTSYS;
			continue;
		}
		if (copy_from_user(tmp, buf + done, n)) {
			ret = done ? done : -EFAULT;
			goto out;
		}
		spin_lock_irqsave(&p->ring_lock, flags);
		kfifo_in(&p->tx_q, tmp, n);
		spin_unlock_irqrestore(&p->ring_lock, flags);
		done += n;
	}
	ret = done;
out:
	mutex_unlock(&p->write_lock);
	return ret;
}

int dect_ssc_set_tx_frame(struct dect_ssc *p, const struct dect_pcm_tx_frame *f)
{
	unsigned long flags;

	if (!p->tx_idle)
		return -ENODATA;
	if (f->len != p->slots * 2 || f->reserved)
		return -EINVAL;
	spin_lock_irqsave(&p->ring_lock, flags);
	memcpy(p->tx_idle, f->frame, f->len);
	spin_unlock_irqrestore(&p->ring_lock, flags);
	return 0;
}

__poll_t dect_ssc_poll(struct dect_ssc *p, struct file *file, poll_table *wait)
{
	poll_wait(file, &p->wq, wait);
	if (p->step == DECT_PCM_STEP_DMA && p->tail != p->head)
		return EPOLLIN | EPOLLRDNORM;
	return 0;
}

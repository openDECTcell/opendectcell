// SPDX-License-Identifier: GPL-2.0-only
/*
 * openDECTcell - PCM/TDM receive path through the IPQ4019 audio subsystem
 * (ADSS) for the AVM 7520/7530: the SoC is a TDM slave of the DECT chip.
 *
 * Written from docs/pcm-tdm-ipq4019.md; "doc §n" refers to its sections.
 * Receive only: pins 24-27 and the transmit MBOX are not touched.
 */
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/cache.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include "opendectcell_pcm.h"

/* TLMM pin control (doc §2) */
#define TLMM_STRIDE		0x1000
#define TLMM_MUX		GENMASK(5, 2)

/* ADSS global registers (doc §5) */
#define GLB_CHIP_CTRL_I2S	0x10
#define  GLB_INTERFACE_EN	BIT(0)
#define GLB_I2S_RST		0x14
#define  GLB_I2S_RST_ALL	0xf
#define GLB_CLK_I2S_CTRL	0x18
#define  GLB_CLK_OE_ALL		(BIT(16) | BIT(17) | BIT(27) | BIT(28))
#define GLB_TDM_CTRL		0x1c
#define  TDM_TX_CHAN_NUM	GENMASK(3, 0)
#define  TDM_TX_SYNC_NUM	GENMASK(8, 4)
#define  TDM_RX_CHAN_NUM	GENMASK(19, 16)
#define  TDM_RX_SYNC_NUM	GENMASK(24, 20)
#define  TDM_TX_DELAY		BIT(25)
#define  TDM_RX_DELAY		BIT(26)
#define GLB_AUDIO_MODE		0x30
#define  MODE_XMIT_TDM		BIT(0)
#define  MODE_RECV_TDM		BIT(2)
#define  MODE_PORT_EN_ALL	GENMASK(9, 4)
#define  MODE_I2S3_FS_OE	BIT(8)
#define  MODE_I2S3_RXD_OE	BIT(9)
#define  MODE_B1K		BIT(28)

/* ADSS clock controller, offsets from the ADSS base (doc §6) */
#define ADCC_RXB_CFG_MUXR	0x104
#define ADCC_RXB_MISC		0x108
#define ADCC_RXB_CBCR		0x10c
#define ADCC_RXM_CFG_RCGR	0x124
#define ADCC_RXM_MISC		0x128
#define ADCC_RXM_CBCR		0x12c
#define ADCC_TXB_CFG_MUXR	0x144
#define ADCC_TXB_MISC		0x148
#define ADCC_TXB_CBCR		0x14c
#define ADCC_TXM_CFG_RCGR	0x164
#define ADCC_TXM_MISC		0x168
#define ADCC_TXM_CBCR		0x16c
#define ADCC_AHB_CBCR		0x200
#define ADCC_AHB_I2S3_CBCR	0x208
#define ADCC_AHB_MBOX3_CBCR	0x210
#define  CBCR_EN		BIT(0)
#define  CBCR_OFF		BIT(31)
#define  ADCC_SRC_PAD		0x200	/* source 2 in bits 10:8 */

/* Stereo block (doc §7) */
#define ST_CONFIG		0x00
#define  ST_MASTER		BIT(8)
#define  ST_MCK_SEL		BIT(10)
#define  ST_I2S_WORD_32		BIT(11)
#define  ST_DATA_WORD_SIZE	GENMASK(13, 12)
#define  ST_MIC_WORD_32		BIT(16)
#define  ST_RESET		BIT(19)
#define  ST_MIC_RESET		BIT(20)
#define  ST_ENABLE		BIT(21)
#define ST_TX_SAMPLES_LSB	0x0c
#define ST_RX_SAMPLES_LSB	0x14
#define ST_RX_SAMPLES_MSB	0x18
#define ST_OVERRUN		0x20
#define ST_RX_COMPLETE		0x28

/* MBOX DMA (doc §9); receive uses the "TX" registers */
#define MBOX_DMA_POLICY		0x10
#define  POL_SW_RESET		BIT(31)
#define  POL_TX_INT_TYPE	BIT(17)
#define  POL_SRAM_AC		GENMASK(15, 12)
#define  POL_TX_FIFO_THRESH	GENMASK(7, 4)
#define MBOX_TX_DESC_BASE	0x20
#define MBOX_TX_CONTROL		0x24
#define  CTRL_STOP		BIT(0)
#define  CTRL_START		BIT(1)
#define MBOX_INT_STATUS		0x44
#define MBOX_INT_ENABLE		0x4c
#define  INT_RX_UNDERFLOW	BIT(4)
#define  INT_TX_OVERFLOW	BIT(5)
#define  INT_TX_DMA_COMPLETE	BIT(6)
#define  INT_RX_DMA_COMPLETE	BIT(10)
#define  INT_RX_FIFO_UNDERFLOW	BIT(12)
#define  INT_TX_FIFO_OVERFLOW	BIT(13)
#define  INT_BI_TX_RX		BIT(14)
#define  INT_ERRORS		(INT_RX_UNDERFLOW | INT_TX_OVERFLOW | \
				 INT_RX_FIFO_UNDERFLOW | INT_TX_FIFO_OVERFLOW)
#define  INT_COMPLETE		(INT_TX_DMA_COMPLETE | INT_RX_DMA_COMPLETE)
#define  INT_KNOWN		(INT_ERRORS | INT_COMPLETE | INT_BI_TX_RX)
#define MBOX_FIFO_RESET		0x58
#define  FIFO_RESET_ALL		0x5

/* MBOX descriptor (doc §9), little-endian words */
#define DESC_LEN		GENMASK(11, 0)
#define DESC_SIZE		GENMASK(23, 12)
#define DESC_EI			BIT(25)
#define DESC_OWN		BIT(31)
#define DMA_ADDR_MASK		0x0fffffffU

struct dect_mbox_desc {
	__le32 ctrl;
	__le32 buf;
	__le32 next;
	__le32 spdif[36];
};

#define PCM_RING_SLOTS		256U	/* about 1 s of 4 ms blocks */
#define PCM_MAX_BLOCK		4095U
#define PCM_MAX_DESC		16U

static const unsigned int adcc_dump[17] = {
	0x104, 0x108, 0x10c, 0x124, 0x128, 0x12c,
	0x144, 0x148, 0x14c, 0x164, 0x168, 0x16c,
	0x200, 0x204, 0x208, 0x20c, 0x210,
};

static const unsigned int mbox_dump[10] = {
	0x10, 0x18, 0x1c, 0x20, 0x24, 0x38, 0x40, 0x44, 0x4c, 0x58,
};

/* The four receive/transmit clock branches, gated in this order. */
static const unsigned int adcc_cbcr[4] = {
	ADCC_RXB_CBCR, ADCC_RXM_CBCR, ADCC_TXB_CBCR, ADCC_TXM_CBCR,
};

/* Audio bus branches that must be on for the receive path (doc §6). */
static const unsigned int adcc_ahb[3] = {
	ADCC_AHB_CBCR, ADCC_AHB_I2S3_CBCR, ADCC_AHB_MBOX3_CBCR,
};

static void reg_rmw(void __iomem *reg, u32 clear, u32 set)
{
	writel((readl(reg) & ~clear) | set, reg);
}

static inline bool cbcr_running(u32 v)
{
	return (v & CBCR_EN) || !(v & CBCR_OFF);
}

static void __iomem *pcm_pin_ctl(struct dect_pcm *p, unsigned int gpio)
{
	return p->tlmm + (gpio - p->tlmm_first) * TLMM_STRIDE;
}

static void pcm_undo_clock(struct dect_pcm *p)
{
	if (!p->ahb_clk)
		return;
	clk_disable_unprepare(p->ahb_clk);
	clk_put(p->ahb_clk);
	p->ahb_clk = NULL;
}

/*
 * Step 1 (doc §4): the GCC audio bus clock, before any ADSS access.
 *
 * On the 7520 this branch runs on a hardware vote while its enable bit is
 * clear; disabling it through the clock framework then warns that the
 * status is stuck on. So the framework is only used if the clock is off.
 */
static int pcm_step_clock(struct dect_pcm *p)
{
	const struct dect_pcm_board *b = p->board;
	struct of_phandle_args args = {
		.args_count = 1,
		.args = { b->gcc_ahb_clk },
	};
	struct clk *clk;
	u32 cbcr;
	int ret;

	cbcr = readl(p->gcc_ahb_cbcr);
	if (!(cbcr & CBCR_OFF)) {
		pr_info("pcm 1: GCC audio AHB clock running (%08x)\n", cbcr);
		goto running;
	}

	args.np = of_find_compatible_node(NULL, NULL, b->gcc_compatible);
	if (!args.np)
		return -ENODEV;
	clk = of_clk_get_from_provider(&args);
	of_node_put(args.np);
	if (IS_ERR(clk))
		return PTR_ERR(clk);
	ret = clk_prepare_enable(clk);
	if (ret) {
		clk_put(clk);
		return ret;
	}
	p->ahb_clk = clk;
	pr_info("pcm 1: GCC audio AHB clock enabled (%08x -> %08x)\n", cbcr,
		readl(p->gcc_ahb_cbcr));
	if (readl(p->gcc_ahb_cbcr) & CBCR_OFF) {
		pcm_undo_clock(p);
		return -EIO;
	}

running:
	pr_info("pcm 1: CHIP_CTRL_I2S %08x AHB %08x I2S3 %08x MBOX3 %08x\n",
		readl(p->adss + GLB_CHIP_CTRL_I2S),
		readl(p->adss + ADCC_AHB_CBCR),
		readl(p->adss + ADCC_AHB_I2S3_CBCR),
		readl(p->adss + ADCC_AHB_MBOX3_CBCR));
	return 0;
}

/* Step 2 (doc §8 steps 2-3, §6 AHB branches). */
static int pcm_step_iface(struct dect_pcm *p)
{
	unsigned int i;
	u32 v;

	reg_rmw(p->adss + GLB_CHIP_CTRL_I2S, 0, GLB_INTERFACE_EN);
	usleep_range(4000, 5000);

	v = readl(p->adss + GLB_AUDIO_MODE);
	p->b1k_set = !(v & MODE_B1K);
	writel(v | MODE_B1K, p->adss + GLB_AUDIO_MODE);

	p->ahb_cbcr_set = 0;
	for (i = 0; i < ARRAY_SIZE(adcc_ahb); i++) {
		v = readl(p->adss + adcc_ahb[i]);
		if (!(v & CBCR_EN)) {
			writel(v | CBCR_EN, p->adss + adcc_ahb[i]);
			p->ahb_cbcr_set |= BIT(i);
		}
	}

	pr_info("pcm 2: CHIP_CTRL_I2S %08x AUDIO_MODE %08x AHB set %x\n",
		readl(p->adss + GLB_CHIP_CTRL_I2S),
		readl(p->adss + GLB_AUDIO_MODE), p->ahb_cbcr_set);
	return 0;
}

static void pcm_undo_iface(struct dect_pcm *p)
{
	unsigned int i;

	reg_rmw(p->adss + GLB_CHIP_CTRL_I2S, GLB_INTERFACE_EN, 0);
	if (p->b1k_set)
		reg_rmw(p->adss + GLB_AUDIO_MODE, MODE_B1K, 0);
	for (i = ARRAY_SIZE(adcc_ahb); i-- > 0;)
		if (p->ahb_cbcr_set & BIT(i))
			reg_rmw(p->adss + adcc_ahb[i], CBCR_EN, 0);
	p->ahb_cbcr_set = 0;
	p->b1k_set = false;
}

/* Step 3 (doc §8 step 4, §6 pad-clock selection). */
static int pcm_step_padclk(struct dect_pcm *p)
{
	static const unsigned int misc[4] = {
		ADCC_RXB_MISC, ADCC_TXB_MISC, ADCC_RXM_MISC, ADCC_TXM_MISC,
	};
	static const unsigned int mux[4] = {
		ADCC_RXB_CFG_MUXR, ADCC_TXB_CFG_MUXR,
		ADCC_RXM_CFG_RCGR, ADCC_TXM_CFG_RCGR,
	};
	unsigned int i;

	writel(GLB_I2S_RST_ALL, p->adss + GLB_I2S_RST);
	usleep_range(4000, 5000);
	writel(0, p->adss + GLB_I2S_RST);

	for (i = 0; i < ARRAY_SIZE(adcc_cbcr); i++)
		writel(0, p->adss + adcc_cbcr[i]);
	usleep_range(10000, 12000);
	for (i = 0; i < ARRAY_SIZE(misc); i++)
		writel(0, p->adss + misc[i]);
	for (i = 0; i < ARRAY_SIZE(mux); i++)
		writel(ADCC_SRC_PAD, p->adss + mux[i]);
	usleep_range(10000, 12000);
	for (i = 0; i < ARRAY_SIZE(adcc_cbcr); i++)
		writel(CBCR_EN, p->adss + adcc_cbcr[i]);
	usleep_range(1000, 2000);

	/* Bit 31 clear = the pad clock reaches the ADSS (chip must run). */
	pr_info("pcm 3: RXB_CBCR %08x RXM_CBCR %08x TXB_CBCR %08x TXM_CBCR %08x\n",
		readl(p->adss + ADCC_RXB_CBCR), readl(p->adss + ADCC_RXM_CBCR),
		readl(p->adss + ADCC_TXB_CBCR), readl(p->adss + ADCC_TXM_CBCR));
	return 0;
}

static void pcm_undo_padclk(struct dect_pcm *p)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(adcc_cbcr); i++)
		writel(0, p->adss + adcc_cbcr[i]);
}

/* Step 4 (doc §8 steps 6-7, §7 stereo slave configuration, §2 pins). */
static int pcm_step_tdm(struct dect_pcm *p)
{
	const struct dect_pcm_board *b = p->board;
	u32 chan = p->cfg.slots - 1;
	unsigned long flags;
	unsigned int i;
	u32 v;

	local_irq_save(flags);
	reg_rmw(p->adss + GLB_TDM_CTRL, TDM_TX_CHAN_NUM | TDM_RX_CHAN_NUM,
		FIELD_PREP(TDM_TX_CHAN_NUM, chan) |
		FIELD_PREP(TDM_RX_CHAN_NUM, chan));
	/* Slave: the SoC never drives a clock. */
	reg_rmw(p->adss + GLB_CLK_I2S_CTRL, GLB_CLK_OE_ALL, 0);
	/* Receiver port enables for FSC and DI (not clock drivers). */
	reg_rmw(p->adss + GLB_AUDIO_MODE, 0, MODE_I2S3_RXD_OE | MODE_I2S3_FS_OE);
	reg_rmw(p->adss + GLB_AUDIO_MODE, 0, MODE_XMIT_TDM | MODE_RECV_TDM);

	v = readl(p->stereo + ST_CONFIG);
	v &= ~(ST_MASTER | ST_ENABLE | ST_DATA_WORD_SIZE | ST_I2S_WORD_32 |
	       ST_MIC_WORD_32 | ST_MCK_SEL);
	/* 16-bit words; MCK_SEL keeps the external clock usable. */
	v |= FIELD_PREP(ST_DATA_WORD_SIZE, 1) | ST_MCK_SEL;
	writel(v, p->stereo + ST_CONFIG);

	reg_rmw(p->adss + GLB_TDM_CTRL,
		TDM_RX_SYNC_NUM | TDM_TX_SYNC_NUM | TDM_RX_DELAY | TDM_TX_DELAY,
		(p->cfg.rx_delay ? TDM_RX_DELAY : 0) |
		(p->cfg.tx_delay ? TDM_TX_DELAY : 0));
	local_irq_restore(flags);

	for (i = 0; i < b->n_pins; i++) {
		void __iomem *ctl = pcm_pin_ctl(p, b->pins[i].gpio);

		p->pin_saved[i] = readl(ctl);
		reg_rmw(ctl, TLMM_MUX, FIELD_PREP(TLMM_MUX, b->pins[i].func));
	}

	pr_info("pcm 4: TDM_CTRL %08x AUDIO_MODE %08x CLK_I2S_CTRL %08x ST_CONFIG %08x\n",
		readl(p->adss + GLB_TDM_CTRL), readl(p->adss + GLB_AUDIO_MODE),
		readl(p->adss + GLB_CLK_I2S_CTRL), readl(p->stereo + ST_CONFIG));
	return 0;
}

/* Teardown as in doc §8, plus the pins back to what they were. */
static void pcm_undo_tdm(struct dect_pcm *p)
{
	const struct dect_pcm_board *b = p->board;
	unsigned int i;

	reg_rmw(p->stereo + ST_CONFIG, ST_ENABLE | ST_MASTER, 0);
	reg_rmw(p->adss + GLB_TDM_CTRL, TDM_TX_CHAN_NUM | TDM_RX_CHAN_NUM, 0);
	reg_rmw(p->adss + GLB_CLK_I2S_CTRL, GLB_CLK_OE_ALL, 0);
	/* Port enables (doc §8); TDM mode bits too, as OpenWrt booted. */
	reg_rmw(p->adss + GLB_AUDIO_MODE,
		MODE_PORT_EN_ALL | MODE_XMIT_TDM | MODE_RECV_TDM, 0);

	for (i = 0; i < b->n_pins; i++)
		reg_rmw(pcm_pin_ctl(p, b->pins[i].gpio), TLMM_MUX,
			p->pin_saved[i] & TLMM_MUX);
}

/* Step 5 (doc §11): receiver on without DMA; the sample counter runs. */
static int pcm_step_stereo(struct dect_pcm *p)
{
	reg_rmw(p->stereo + ST_CONFIG, 0, ST_ENABLE);
	return 0;
}

static void pcm_undo_stereo(struct dect_pcm *p)
{
	reg_rmw(p->stereo + ST_CONFIG, ST_ENABLE, 0);
}

static struct dect_mbox_desc *pcm_desc(struct dect_pcm *p, unsigned int i)
{
	return p->dma_cpu + i * p->desc_stride;
}

static void pcm_deliver(struct dect_pcm *p, unsigned int idx, u32 ctrl,
			u64 ts)
{
	struct dect_pcm_block *hdr;
	unsigned int next;
	u8 *slot;

	spin_lock(&p->ring_lock);
	p->st.blocks++;
	next = (p->head + 1) % p->ring_slots;
	if (next == p->tail) {
		p->st.dropped++;
		p->drop_pending = true;
		p->seq++;
		spin_unlock(&p->ring_lock);
		return;
	}
	slot = p->ring + p->head * p->slot_bytes;
	hdr = (struct dect_pcm_block *)slot;
	hdr->magic = DECT_PCM_BLOCK_MAGIC;
	hdr->seq = p->seq++;
	hdr->flags = p->drop_pending ? DECT_PCM_BF_DROPPED : 0;
	hdr->len = p->block_bytes;
	hdr->ts_ns = ts;
	hdr->desc = idx;
	hdr->desc_ctrl = ctrl;
	memcpy(slot + sizeof(*hdr),
	       p->dma_cpu + p->buf_offset + idx * p->buf_stride,
	       p->block_bytes);
	p->drop_pending = false;
	p->head = next;
	spin_unlock(&p->ring_lock);
}

/* doc §9 "Interrupt": acknowledge, then hand back every completed block. */
static irqreturn_t pcm_irq(int irq, void *data)
{
	struct dect_pcm *p = data;
	u32 status, ctrl, rearm;
	unsigned int n;
	u64 ts;

	status = readl(p->mbox + MBOX_INT_STATUS);
	if (!(status & INT_KNOWN))
		return IRQ_NONE;
	ts = ktime_get_ns();
	/* Inverted acknowledge: bits written as 0 are cleared. */
	writel(status & ~INT_KNOWN, p->mbox + MBOX_INT_STATUS);

	spin_lock(&p->ring_lock);
	p->st.irqs++;
	if (status & INT_ERRORS) {
		p->st.errors++;
		p->st.last_error_status = status;
	}
	spin_unlock(&p->ring_lock);

	if (!(status & INT_COMPLETE))
		return IRQ_HANDLED;

	rearm = DESC_OWN | DESC_EI | FIELD_PREP(DESC_SIZE, p->block_bytes) |
		FIELD_PREP(DESC_LEN, p->block_bytes);
	for (n = 0; n < p->cfg.n_desc; n++) {
		struct dect_mbox_desc *d = pcm_desc(p, p->next_desc);

		ctrl = le32_to_cpu(READ_ONCE(d->ctrl));
		if (ctrl & DESC_OWN)
			break;
		dma_rmb();
		pcm_deliver(p, p->next_desc, ctrl, ts);
		WRITE_ONCE(d->ctrl, cpu_to_le32(rearm));
		p->next_desc = (p->next_desc + 1) % p->cfg.n_desc;
	}
	wmb();	/* descriptors back to the hardware */

	spin_lock(&p->ring_lock);
	if (!n)
		p->st.empty++;
	else if (n > 1)
		p->st.multi++;
	spin_unlock(&p->ring_lock);

	if (n)
		wake_up_interruptible(&p->wq);
	return IRQ_HANDLED;
}

static int pcm_map_irq(struct dect_pcm *p)
{
	const struct dect_pcm_board *b = p->board;
	struct of_phandle_args args = {
		.args_count = 3,
		.args = { 0 /* SPI */, b->irq_spi, b->irq_type },
	};
	unsigned int irq;

	args.np = of_find_compatible_node(NULL, NULL, b->gic_compatible);
	if (!args.np)
		return -ENODEV;
	irq = irq_create_of_mapping(&args);
	of_node_put(args.np);
	return irq ? irq : -EINVAL;
}

/* Allocate the ring: n descriptors, each with its own buffer. */
static int pcm_dma_alloc(struct dect_pcm *p)
{
	struct device *dev = &p->dma_pdev->dev;
	unsigned int i, n = p->cfg.n_desc;
	dma_addr_t end;

	p->desc_stride = ALIGN(sizeof(struct dect_mbox_desc), L1_CACHE_BYTES);
	p->buf_stride = ALIGN(p->block_bytes, L1_CACHE_BYTES);
	p->buf_offset = n * p->desc_stride;
	p->dma_size = p->buf_offset + n * p->buf_stride;
	p->dma_cpu = dma_alloc_coherent(dev, p->dma_size, &p->dma_addr,
					GFP_KERNEL);
	if (!p->dma_cpu)
		return -ENOMEM;

	/* The MBOX takes 28-bit addresses; the top nibble goes to SRAM_AC. */
	end = p->dma_addr + p->dma_size - 1;
	if ((p->dma_addr >> 28) != (end >> 28) || (u64)end > U32_MAX) {
		pr_err("DMA memory %pad+%zx crosses a 256 MiB window\n",
		       &p->dma_addr, p->dma_size);
		dma_free_coherent(dev, p->dma_size, p->dma_cpu, p->dma_addr);
		p->dma_cpu = NULL;
		return -ENOMEM;
	}

	memset(p->dma_cpu, 0, p->dma_size);
	for (i = 0; i < n; i++) {
		struct dect_mbox_desc *d = pcm_desc(p, i);
		dma_addr_t buf = p->dma_addr + p->buf_offset + i * p->buf_stride;
		dma_addr_t next = p->dma_addr + ((i + 1) % n) * p->desc_stride;

		memset(p->dma_cpu + p->buf_offset + i * p->buf_stride, 0xee,
		       p->buf_stride);
		d->buf = cpu_to_le32((u32)buf & DMA_ADDR_MASK);
		d->next = cpu_to_le32((u32)next & DMA_ADDR_MASK);
		d->ctrl = cpu_to_le32(DESC_OWN | DESC_EI |
				      FIELD_PREP(DESC_SIZE, p->block_bytes) |
				      FIELD_PREP(DESC_LEN, p->block_bytes));
	}
	wmb();	/* ring complete before the hardware sees its address */
	p->next_desc = 0;
	return 0;
}

static void pcm_dma_free(struct dect_pcm *p)
{
	dma_free_coherent(&p->dma_pdev->dev, p->dma_size, p->dma_cpu,
			  p->dma_addr);
	p->dma_cpu = NULL;
}

static void pcm_ring_free(struct dect_pcm *p)
{
	u8 *ring;

	spin_lock_irq(&p->ring_lock);
	ring = p->ring;
	p->ring = NULL;
	p->head = 0;
	p->tail = 0;
	spin_unlock_irq(&p->ring_lock);
	vfree(ring);
}

/* doc §9 "Prepare" */
static void pcm_dma_prepare(struct dect_pcm *p)
{
	unsigned int off;

	reg_rmw(p->mbox + MBOX_DMA_POLICY, 0, POL_SW_RESET);
	usleep_range(4000, 5000);
	reg_rmw(p->mbox + MBOX_DMA_POLICY, POL_SW_RESET, 0);

	reg_rmw(p->stereo + ST_CONFIG, ST_ENABLE, 0);
	for (off = ST_TX_SAMPLES_LSB; off <= ST_RX_COMPLETE; off += 4)
		writel(0, p->stereo + off);
	reg_rmw(p->stereo + ST_CONFIG, 0, ST_RESET | ST_MIC_RESET);
	usleep_range(5000, 6000);
	writel(FIFO_RESET_ALL, p->mbox + MBOX_FIFO_RESET);
	reg_rmw(p->stereo + ST_CONFIG, ST_RESET | ST_MIC_RESET, 0);

	reg_rmw(p->mbox + MBOX_DMA_POLICY, POL_SRAM_AC | POL_TX_FIFO_THRESH,
		POL_TX_INT_TYPE |
		FIELD_PREP(POL_SRAM_AC, (u32)(p->dma_addr >> 28)) |
		FIELD_PREP(POL_TX_FIFO_THRESH, 8));
	writel((u32)p->dma_addr & DMA_ADDR_MASK, p->mbox + MBOX_TX_DESC_BASE);
}

/* Step 6 (doc §9 "Prepare" and "Start"). */
static int pcm_step_dma(struct dect_pcm *p)
{
	int irq, ret;

	p->slot_bytes = ALIGN(sizeof(struct dect_pcm_block) + p->block_bytes,
			      8);
	p->ring_slots = PCM_RING_SLOTS;
	p->ring = vmalloc(p->ring_slots * p->slot_bytes);
	if (!p->ring)
		return -ENOMEM;
	p->head = 0;
	p->tail = 0;
	p->seq = 0;
	p->drop_pending = false;
	memset(&p->st, 0, sizeof(p->st));

	ret = pcm_dma_alloc(p);
	if (ret)
		goto err_ring;

	irq = pcm_map_irq(p);
	if (irq < 0) {
		ret = irq;
		goto err_dma;
	}
	p->irq = irq;

	/* Quiet MBOX before the handler can run. */
	reg_rmw(p->mbox + MBOX_INT_ENABLE, INT_TX_DMA_COMPLETE, 0);
	pcm_dma_prepare(p);

	ret = request_irq(p->irq, pcm_irq, 0, "opendectcell-pcm", p);
	if (ret)
		goto err_irq;

	reg_rmw(p->mbox + MBOX_INT_ENABLE, 0, INT_TX_DMA_COMPLETE);
	writel(CTRL_START, p->mbox + MBOX_TX_CONTROL);
	reg_rmw(p->stereo + ST_CONFIG, 0, ST_ENABLE);

	pr_info("pcm 6: DMA at %pad, %u x %u bytes, irq %d, POLICY %08x\n",
		&p->dma_addr, p->cfg.n_desc, p->block_bytes, p->irq,
		readl(p->mbox + MBOX_DMA_POLICY));
	return 0;

err_irq:
	irq_dispose_mapping(p->irq);
	p->irq = 0;
err_dma:
	pcm_dma_free(p);
err_ring:
	pcm_ring_free(p);
	return ret;
}

/* doc §9 "Stop" */
static void pcm_undo_dma(struct dect_pcm *p)
{
	reg_rmw(p->mbox + MBOX_INT_ENABLE, INT_TX_DMA_COMPLETE, 0);
	writel(CTRL_STOP, p->mbox + MBOX_TX_CONTROL);
	usleep_range(10000, 12000);
	reg_rmw(p->stereo + ST_CONFIG, ST_ENABLE, 0);

	free_irq(p->irq, p);
	irq_dispose_mapping(p->irq);
	p->irq = 0;
	pcm_dma_free(p);
	pcm_ring_free(p);
}

void dect_pcm_teardown(struct dect_pcm *p)
{
	unsigned int step = p->step;

	if (step >= DECT_PCM_STEP_DMA)
		pcm_undo_dma(p);
	if (step >= DECT_PCM_STEP_STEREO)
		pcm_undo_stereo(p);
	if (step >= DECT_PCM_STEP_TDM)
		pcm_undo_tdm(p);
	if (step >= DECT_PCM_STEP_PADCLK)
		pcm_undo_padclk(p);
	if (step >= DECT_PCM_STEP_IFACE)
		pcm_undo_iface(p);
	if (step >= DECT_PCM_STEP_CLOCK)
		pcm_undo_clock(p);
	if (step)
		pr_info("pcm: stopped from step %u\n", step);
	WRITE_ONCE(p->step, DECT_PCM_STEP_OFF);
	wake_up_interruptible(&p->wq);
}

static int pcm_step(struct dect_pcm *p, u32 target)
{
	static int (*const fn[])(struct dect_pcm *) = {
		[DECT_PCM_STEP_CLOCK] = pcm_step_clock,
		[DECT_PCM_STEP_IFACE] = pcm_step_iface,
		[DECT_PCM_STEP_PADCLK] = pcm_step_padclk,
		[DECT_PCM_STEP_TDM] = pcm_step_tdm,
		[DECT_PCM_STEP_STEREO] = pcm_step_stereo,
		[DECT_PCM_STEP_DMA] = pcm_step_dma,
	};
	int ret;

	if (target == DECT_PCM_STEP_OFF) {
		dect_pcm_teardown(p);
		return 0;
	}
	if (target >= ARRAY_SIZE(fn))
		return -EINVAL;
	if (target != p->step + 1 &&
	    !(target == DECT_PCM_STEP_DMA && p->step == DECT_PCM_STEP_TDM))
		return -EPROTO;

	ret = fn[target](p);
	if (!ret)
		WRITE_ONCE(p->step, target);
	return ret;
}

static int pcm_config(struct dect_pcm *p, struct dect_pcm_config *c)
{
	u32 flags = c->flags & (DECT_PCM_CF_RX_DELAY_SET |
				DECT_PCM_CF_TX_DELAY_SET);
	unsigned int i;

	if (p->step != DECT_PCM_STEP_OFF)
		return -EBUSY;
	for (i = 0; i < ARRAY_SIZE(c->reserved); i++)
		if (c->reserved[i])
			return -EINVAL;

	if (!c->slots)
		c->slots = 8;
	if (!c->rx_delay && !(flags & DECT_PCM_CF_RX_DELAY_SET))
		c->rx_delay = 1;
	if (!c->tx_delay && !(flags & DECT_PCM_CF_TX_DELAY_SET))
		c->tx_delay = 1;
	if (!c->frames_per_block)
		c->frames_per_block = 32;
	if (!c->n_desc)
		c->n_desc = 4;
	c->flags = flags;

	if (c->slots > 16 || c->rx_delay > 1 || c->tx_delay > 1 ||
	    c->n_desc < 2 || c->n_desc > PCM_MAX_DESC ||
	    c->frames_per_block > PCM_MAX_BLOCK ||
	    c->slots * 2 * c->frames_per_block > PCM_MAX_BLOCK)
		return -EINVAL;

	p->cfg = *c;
	p->block_bytes = c->slots * 2 * c->frames_per_block;
	return 0;
}

static void pcm_regs(struct dect_pcm *p, struct dect_pcm_regs *r)
{
	static const unsigned int glb[5] = {
		GLB_CHIP_CTRL_I2S, GLB_I2S_RST, GLB_CLK_I2S_CTRL,
		GLB_TDM_CTRL, GLB_AUDIO_MODE,
	};
	unsigned int i;
	u32 ahb;

	r->step = p->step;
	for (i = 0; i < ARRAY_SIZE(r->tlmm); i++)
		r->tlmm[i] = readl(pcm_pin_ctl(p, p->tlmm_first + i));
	if (p->step < DECT_PCM_STEP_CLOCK)
		return;

	r->valid |= DECT_PCM_REGS_GLB;
	for (i = 0; i < ARRAY_SIZE(glb); i++)
		r->glb[i] = readl(p->adss + glb[i]);
	for (i = 0; i < ARRAY_SIZE(adcc_dump); i++)
		r->adcc[i] = readl(p->adss + adcc_dump[i]);

	/* Only read a block whose bus branch is on. */
	ahb = readl(p->adss + ADCC_AHB_CBCR);
	if (!cbcr_running(ahb))
		return;
	if (cbcr_running(readl(p->adss + ADCC_AHB_I2S3_CBCR))) {
		r->valid |= DECT_PCM_REGS_STEREO;
		for (i = 0; i < ARRAY_SIZE(r->stereo); i++)
			r->stereo[i] = readl(p->stereo + 4 * i);
	}
	if (cbcr_running(readl(p->adss + ADCC_AHB_MBOX3_CBCR))) {
		r->valid |= DECT_PCM_REGS_MBOX;
		for (i = 0; i < ARRAY_SIZE(mbox_dump); i++)
			r->mbox[i] = readl(p->mbox + mbox_dump[i]);
	}
}

static void pcm_stats(struct dect_pcm *p, struct dect_pcm_stats *s)
{
	spin_lock_irq(&p->ring_lock);
	*s = p->st;
	spin_unlock_irq(&p->ring_lock);

	s->step = p->step;
	s->block_bytes = p->block_bytes;
	if (p->step >= DECT_PCM_STEP_TDM) {
		s->rx_samples_lsb = readl(p->stereo + ST_RX_SAMPLES_LSB);
		s->rx_samples_msb = readl(p->stereo + ST_RX_SAMPLES_MSB);
		s->overruns = readl(p->stereo + ST_OVERRUN);
	}
	if (p->step >= DECT_PCM_STEP_DMA)
		s->int_status = readl(p->mbox + MBOX_INT_STATUS);
}

long dect_pcm_ioctl(struct dect_pcm *p, unsigned int cmd, void __user *argp)
{
	union {
		struct dect_pcm_config cfg;
		struct dect_pcm_regs regs;
		struct dect_pcm_stats stats;
	} u;
	u32 step;
	long ret;

	memset(&u, 0, sizeof(u));
	switch (cmd) {
	case DECT_IOC_PCM_CONFIG:
		if (copy_from_user(&u.cfg, argp, sizeof(u.cfg)))
			return -EFAULT;
		ret = pcm_config(p, &u.cfg);
		if (!ret && copy_to_user(argp, &u.cfg, sizeof(u.cfg)))
			ret = -EFAULT;
		return ret;
	case DECT_IOC_PCM_STEP:
		if (copy_from_user(&step, argp, sizeof(step)))
			return -EFAULT;
		return pcm_step(p, step);
	case DECT_IOC_PCM_REGS:
		pcm_regs(p, &u.regs);
		return copy_to_user(argp, &u.regs, sizeof(u.regs)) ?
			-EFAULT : 0;
	case DECT_IOC_PCM_STATS:
		pcm_stats(p, &u.stats);
		return copy_to_user(argp, &u.stats, sizeof(u.stats)) ?
			-EFAULT : 0;
	}
	return -ENOTTY;
}

static bool pcm_readable(struct dect_pcm *p)
{
	return READ_ONCE(p->head) != READ_ONCE(p->tail) ||
	       READ_ONCE(p->step) != DECT_PCM_STEP_DMA;
}

ssize_t dect_pcm_read(struct dect_pcm *p, char __user *buf, size_t count,
		      bool nonblock)
{
	unsigned int tail;
	size_t done = 0;
	bool running;
	int ret;

	for (;;) {
		mutex_lock(p->lock);
		if (p->ring && count < p->slot_bytes) {
			mutex_unlock(p->lock);
			return -EINVAL;
		}
		while (p->ring && count - done >= p->slot_bytes) {
			spin_lock_irq(&p->ring_lock);
			tail = p->tail;
			if (tail == p->head) {
				spin_unlock_irq(&p->ring_lock);
				break;
			}
			spin_unlock_irq(&p->ring_lock);

			/* The producer never writes the slot at tail. */
			if (copy_to_user(buf + done,
					 p->ring + tail * p->slot_bytes,
					 p->slot_bytes)) {
				mutex_unlock(p->lock);
				return done ? done : -EFAULT;
			}
			spin_lock_irq(&p->ring_lock);
			p->tail = (tail + 1) % p->ring_slots;
			spin_unlock_irq(&p->ring_lock);
			done += p->slot_bytes;
		}
		running = p->step == DECT_PCM_STEP_DMA;
		mutex_unlock(p->lock);

		if (done)
			return done;
		if (!running)
			return -ENODATA;
		if (nonblock)
			return -EAGAIN;
		ret = wait_event_interruptible(p->wq, pcm_readable(p));
		if (ret)
			return ret;
	}
}

__poll_t dect_pcm_poll(struct dect_pcm *p, struct file *file,
		       poll_table *wait)
{
	poll_wait(file, &p->wq, wait);
	if (READ_ONCE(p->head) != READ_ONCE(p->tail))
		return EPOLLIN | EPOLLRDNORM;
	if (READ_ONCE(p->step) != DECT_PCM_STEP_DMA)
		return EPOLLHUP;
	return 0;
}

int dect_pcm_init(struct dect_pcm *p, const struct dect_pcm_board *board,
		  struct mutex *lock, void __iomem *tlmm,
		  unsigned int tlmm_first)
{
	struct platform_device_info pdi = {
		.name = "opendectcell-dma",
		.id = PLATFORM_DEVID_NONE,
		.dma_mask = DMA_BIT_MASK(32),
	};
	struct dect_pcm_config cfg = {};
	unsigned int i;
	int ret;

	if (board->n_pins > ARRAY_SIZE(p->pin_saved))
		return -EINVAL;
	for (i = 0; i < board->n_pins; i++)
		if (board->pins[i].gpio < tlmm_first ||
		    board->pins[i].gpio >= tlmm_first + 8)
			return -EINVAL;

	p->board = board;
	p->lock = lock;
	p->tlmm = tlmm;
	p->tlmm_first = tlmm_first;
	spin_lock_init(&p->ring_lock);
	init_waitqueue_head(&p->wq);

	/* Mapping only; nothing is accessed before step 1. */
	ret = -ENOMEM;
	p->gcc_ahb_cbcr = ioremap(board->gcc_ahb_cbcr, sizeof(u32));
	p->adss = ioremap(board->adss_base, board->adss_size);
	p->mbox = ioremap(board->mbox_base, SZ_4K);
	p->stereo = ioremap(board->stereo_base, SZ_4K);
	if (!p->gcc_ahb_cbcr || !p->adss || !p->mbox || !p->stereo)
		goto err;

	p->dma_pdev = platform_device_register_full(&pdi);
	if (IS_ERR(p->dma_pdev)) {
		ret = PTR_ERR(p->dma_pdev);
		p->dma_pdev = NULL;
		goto err;
	}

	ret = pcm_config(p, &cfg);
	if (ret)
		goto err_pdev;
	return 0;

err_pdev:
	platform_device_unregister(p->dma_pdev);
err:
	if (p->stereo)
		iounmap(p->stereo);
	if (p->mbox)
		iounmap(p->mbox);
	if (p->adss)
		iounmap(p->adss);
	if (p->gcc_ahb_cbcr)
		iounmap(p->gcc_ahb_cbcr);
	return ret;
}

/* Call after dect_pcm_teardown(). */
void dect_pcm_exit(struct dect_pcm *p)
{
	platform_device_unregister(p->dma_pdev);
	iounmap(p->stereo);
	iounmap(p->mbox);
	iounmap(p->adss);
	iounmap(p->gcc_ahb_cbcr);
}

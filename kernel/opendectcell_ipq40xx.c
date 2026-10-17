// SPDX-License-Identifier: GPL-2.0-only
/*
 * openDECTcell - DECT reset and boot strap control for the AVM 7520/7530.
 *
 * Phase A (reset, boot strap), read-only GPIO polling diagnostics and
 * Phase B (PCM receive through the ADSS TDM block, opendectcell_pcm.c).
 */
#include <linux/bits.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/irqflags.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "opendectcell.h"
#include "opendectcell_pcm.h"

/* IPQ4019 TLMM: ctl_reg = 0x1000 * GPIO; mux selector is bits [5:2]. */
#define IPQ40XX_GPIO_STRIDE	0x1000
#define IPQ40XX_MUX_SHIFT	2
#define IPQ40XX_MUX_MASK	GENMASK(5, 2)

/* Read-only TLMM inputs, as defined in pinctrl-ipq4019.c. */
#define IPQ40XX_GPIO_IN_OFFSET	0x4
#define DECT_PCM_GPIO_FIRST	20U
#define DECT_PCM_GPIO_LAST	27U
#define DECT_CAL_ROUNDS		2048U
#define DECT_CAL_EDGES		256U
#define DECT_CLOCK_TEST_NS	(2ULL * NSEC_PER_MSEC)
#define DECT_POLL_MAX_ITER	500000U
/* 1.024 MHz * 8 ms = 8192 observed rising edges at most. */
#define DECT_CAPTURE_MAX_EDGES	8192U

static struct gpiod_lookup_table fritz7530_gpio_lookup = {
	.dev_id = "dect0",
	.table = {
		GPIO_LOOKUP("1000000.pinctrl", 40, "reset",
			    GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("1000000.pinctrl", 16, "strap",
			    GPIO_ACTIVE_HIGH),
		{},
	},
};

/* GPIO 20-22 receive clocks and frame sync (function 2), 23 DI (3). */
static const struct dect_pcm_pin fritz7530_pcm_pins[] = {
	{ 20, 2 }, { 21, 2 }, { 22, 2 }, { 23, 3 },
};

/* GPIO 25, 26 transmit DCL and FSC (inputs), 27 DO (output); 24 MCL stays. */
static const struct dect_pcm_pin fritz7530_pcm_tx_pins[] = {
	{ 25, 3 }, { 26, 3 }, { 27, 3, true },
};

/* docs/pcm-tdm-ipq4019.md sections 2-4 */
static const struct dect_pcm_board fritz7530_pcm = {
	.adss_base = 0x07700000,
	.adss_size = 0x300,
	.rx = { 0x0770e000, 0x0770f000, 159 },	/* MBOX3, STEREO3, SPI */
	.tx = { 0x07708000, 0x07709000, 156 },	/* MBOX0, STEREO0, SPI */
	.gcc_compatible = "qcom,gcc-ipq4019",
	.gcc_ahb_clk = 19,		/* GCC_AUDIO_AHB_CLK */
	.gcc_ahb_cbcr = 0x01800000 + 0x1b010,
	.gic_compatible = "qcom,msm-qgic2",
	.irq_type = 4,			/* IRQ_TYPE_LEVEL_HIGH */
	.pins = fritz7530_pcm_pins,
	.n_pins = ARRAY_SIZE(fritz7530_pcm_pins),
	.tx_pins = fritz7530_pcm_tx_pins,
	.n_tx_pins = ARRAY_SIZE(fritz7530_pcm_tx_pins),
};

struct dect_board {
	const char *compatible;
	const char *transport;
	const char *serial_device;
	const char *tlmm_compatible;
	unsigned int strap_offset;
	unsigned int uart_mux;
	unsigned int pcm_gpio_first;
	unsigned int pcm_gpio_last;
	struct gpiod_lookup_table *gpio_lookup;
	const struct dect_pcm_board *pcm;
};

static const struct dect_board dect_boards[] = {
	{
		.compatible = "avm,fritzbox-7530",
		.transport = "ipq40xx-tdm",
		.serial_device = "78af000.serial",
		.tlmm_compatible = "qcom,ipq4019-pinctrl",
		.strap_offset = 16,
		.uart_mux = 1,	/* GPIO16 function 1: blsp_uart0 */
		.pcm_gpio_first = DECT_PCM_GPIO_FIRST,
		.pcm_gpio_last = DECT_PCM_GPIO_LAST,
		.gpio_lookup = &fritz7530_gpio_lookup,
		.pcm = &fritz7530_pcm,
	},
};

struct opendectcell {
	struct miscdevice misc;
	struct mutex lock;
	const struct dect_board *board;
	struct device *serial;
	void __iomem *strap_ctl;
	void __iomem *pcm_regs;
	void __iomem *dcl_in;
	void __iomem *fsc_in;
	void __iomem *di_in;
	void __iomem *do_in;
	struct gpio_desc *reset;
	struct gpio_desc *strap;
	bool opened;
	bool uart_restore_pending;
	struct dect_pcm pcm;
};

static struct opendectcell dect;

/*
 * Linux 6.12 pinctrl_force_default() accepts struct pinctrl_dev *, not
 * struct device *, and only handles controller hog states.  Selecting the
 * serial device's already-current "default" state is a no-op, too.
 *
 * Restore only GPIO16's mux bits, using the IPQ4019 register definition
 * from mainline pinctrl-ipq4019.c.  Never modify GPIO17 or other bits.
 * This is board-specific, and cannot serialize against the pinctrl-msm
 * driver's private spinlock.  Do not change UART pinctrl concurrently.
 */
static int dect_restore_uart_mux(struct opendectcell *dw)
{
	u32 value, expected;

	expected = dw->board->uart_mux << IPQ40XX_MUX_SHIFT;
	value = readl(dw->strap_ctl);
	value &= ~IPQ40XX_MUX_MASK;
	writel(value | expected, dw->strap_ctl);

	if ((readl(dw->strap_ctl) & IPQ40XX_MUX_MASK) != expected) {
		pr_err("failed to restore GPIO%u UART mux\n",
		       dw->board->strap_offset);
		return -EIO;
	}

	return 0;
}

static int dect_unstrap_locked(struct opendectcell *dw)
{
	int ret;

	if (dw->strap) {
		gpiod_put(dw->strap);
		dw->strap = NULL;
		dw->uart_restore_pending = true;
	}

	if (!dw->uart_restore_pending)
		return 0;

	ret = dect_restore_uart_mux(dw);
	if (!ret)
		dw->uart_restore_pending = false;

	return ret;
}

static void dect_close_locked(struct opendectcell *dw)
{
	int ret;

	dect_pcm_teardown(&dw->pcm);

	ret = dect_unstrap_locked(dw);
	if (ret)
		pr_err("UART restoration failed during cleanup: %d\n", ret);

	if (dw->reset) {
		/* Raw high releases this active-low reset before GPIO release. */
		gpiod_set_value(dw->reset, 1);
		gpiod_put(dw->reset);
		dw->reset = NULL;
	}
}

static int dect_open(struct inode *inode, struct file *file)
{
	struct opendectcell *dw = &dect;
	int ret;

	ret = nonseekable_open(inode, file);
	if (ret)
		return ret;

	mutex_lock(&dw->lock);
	if (dw->opened) {
		ret = -EBUSY;
		goto out;
	}

	/* Request and start with reset released (GPIO40 = 1). */
	dw->reset = gpiod_get(dw->misc.this_device, "reset",
				   GPIOD_OUT_HIGH);
	if (IS_ERR(dw->reset)) {
		ret = PTR_ERR(dw->reset);
		dw->reset = NULL;
		goto out;
	}

	dw->opened = true;
	file->private_data = dw;
	ret = 0;
out:
	mutex_unlock(&dw->lock);
	return ret;
}

static int dect_release(struct inode *inode, struct file *file)
{
	struct opendectcell *dw = file->private_data;

	mutex_lock(&dw->lock);
	dect_close_locked(dw);
	dw->opened = false;
	mutex_unlock(&dw->lock);
	return 0;
}

static ssize_t dect_read(struct file *file, char __user *buf, size_t count,
			 loff_t *ppos)
{
	struct opendectcell *dw = file->private_data;

	return dect_pcm_read(&dw->pcm, buf, count,
			     file->f_flags & O_NONBLOCK);
}

static ssize_t dect_write(struct file *file, const char __user *buf,
			  size_t count, loff_t *ppos)
{
	struct opendectcell *dw = file->private_data;

	return dect_pcm_write(&dw->pcm, buf, count,
			      file->f_flags & O_NONBLOCK);
}

static __poll_t dect_poll(struct file *file, poll_table *wait)
{
	struct opendectcell *dw = file->private_data;

	return dect_pcm_poll(&dw->pcm, file, wait);
}

/*
 * GPIO capture is deliberately diagnostic only. Reading GPIO_IN does not
 * claim the pins or change their mux and works independently of ADSS.
 * readl_relaxed() is used to avoid unnecessary ordering barriers; the four
 * pins are sampled sequentially, so their values are not simultaneous.
 *
 * Calibration is done with interrupts enabled. The actual capture disables
 * local interrupts for no more than the user-requested (<= 8 ms) interval,
 * checked after every 64 DCL polls. Because checking itself takes time,
 * the window can exceed its target by a small amount; no sleeps are used.
 */
static void dect_capture_calibrate(struct opendectcell *dw,
				  struct dect_ioc_capture *c)
{
	u64 begin, elapsed, deadline;
	u32 i, prev, level, edges = 0;
	u32 polls = 0;
	u32 sink = 0;

	begin = ktime_get_ns();
	for (i = 0; i < DECT_CAL_ROUNDS; i++) {
		sink ^= readl_relaxed(dw->dcl_in);
		sink ^= readl_relaxed(dw->fsc_in);
		sink ^= readl_relaxed(dw->di_in);
		sink ^= readl_relaxed(dw->do_in);
	}
	elapsed = ktime_get_ns() - begin;
	/* Ceiling avoids overstating available performance. */
	c->ns_per_read = max_t(u32, 1, DIV_ROUND_UP_ULL(elapsed,
							     DECT_CAL_ROUNDS * 4));
	/* Keep the result observable without modifying any device register. */
	barrier();
	(void)sink;

	prev = readl_relaxed(dw->dcl_in) & 1;
	begin = ktime_get_ns();
	deadline = begin + DECT_CLOCK_TEST_NS;
	while (polls < DECT_POLL_MAX_ITER && edges < DECT_CAL_EDGES) {
		level = readl_relaxed(dw->dcl_in) & 1;
		if (!prev && level)
			edges++;
		prev = level;
		if (!(++polls & 63) && ktime_get_ns() >= deadline)
			break;
	}
	elapsed = ktime_get_ns() - begin;
	if (edges != DECT_CAL_EDGES || !elapsed) {
		c->flags |= edges ? DECT_CAPTURE_F_TOO_SLOW :
				    DECT_CAPTURE_F_NO_CLOCK;
		return;
	}

	c->dcl_period_ns = max_t(u32, 1,
					  div_u64(elapsed, DECT_CAL_EDGES));
	c->observed_clock_hz = div_u64(1000000000ULL,
						c->dcl_period_ns);
	c->reads_per_period = c->dcl_period_ns / c->ns_per_read;
	c->reads_per_half_period = (c->dcl_period_ns / 2) /
						c->ns_per_read;

	/*
	 * Sanity check the observed ~1.024 MHz: missing edges in the
	 * calibration cannot safely be mistaken for a slower real clock.
	 * Three MMIO reads per half cycle are a minimum, not a guarantee.
	 */
	if (c->observed_clock_hz < 900000 ||
	    c->observed_clock_hz > 1200000 ||
	    c->reads_per_half_period < 3)
		c->flags |= DECT_CAPTURE_F_TOO_SLOW;
}

static void dect_capture_samples(struct opendectcell *dw,
				 struct dect_ioc_capture *c, u8 *data)
{
	unsigned long irqflags;
	u64 deadline, elapsed, expected;
	u32 polls = 0, prev, level, samples = 0;
	u8 value;

	local_irq_save(irqflags);
	prev = readl_relaxed(dw->dcl_in) & 1;
	c->start_ns = ktime_get_ns();
	deadline = c->start_ns + (u64)c->duration_us * NSEC_PER_USEC;
	while (samples < DECT_CAPTURE_MAX_EDGES) {
		level = readl_relaxed(dw->dcl_in) & 1;
		if (!prev && level) {
			value = 1;
			value |= (readl_relaxed(dw->fsc_in) & 1) << 1;
			value |= (readl_relaxed(dw->di_in) & 1) << 2;
			value |= (readl_relaxed(dw->do_in) & 1) << 3;
			data[samples++] = value;
		}
		prev = level;
		if (!(++polls & 63) && ktime_get_ns() >= deadline)
			break;
	}
	c->end_ns = ktime_get_ns();
	local_irq_restore(irqflags);
	c->sample_count = samples;

	/* Heuristic only: sampled transitions cannot prove continuity. */
	elapsed = c->end_ns - c->start_ns;
	expected = div_u64(elapsed * c->observed_clock_hz,
			   NSEC_PER_SEC);
	if (expected > 16 && (u64)samples * 10 < expected * 9)
		c->flags |= DECT_CAPTURE_F_INCOMPLETE;
}

static long dect_ioctl_capture(struct opendectcell *dw, void __user *argp)
{
	struct dect_ioc_capture c;
	void __user *user_samples;
	u8 *data;
	unsigned int i;
	long ret = 0;

	if (copy_from_user(&c, argp, sizeof(c)))
		return -EFAULT;
	if (!c.duration_us || c.duration_us > DECT_CAPTURE_MAX_US ||
	    c.sample_capacity < DECT_CAPTURE_BUFFER_BYTES ||
	    !c.samples_ptr ||
	    (u64)(unsigned long)c.samples_ptr != c.samples_ptr ||
	    c.reserved0)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(c.reserved); i++)
		if (c.reserved[i])
			return -EINVAL;

	user_samples = (void __user *)(unsigned long)c.samples_ptr;
	if (!access_ok(user_samples, c.sample_capacity))
		return -EFAULT;
	data = kvzalloc(DECT_CAPTURE_BUFFER_BYTES, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	c.flags = 0;
	c.sample_count = 0;
	c.ns_per_read = 0;
	c.dcl_period_ns = 0;
	c.reads_per_period = 0;
	c.reads_per_half_period = 0;
	c.observed_clock_hz = 0;
	c.start_ns = 0;
	c.end_ns = 0;

	mutex_lock(&dw->lock);
	dect_capture_calibrate(dw, &c);
	if (!c.flags)
		dect_capture_samples(dw, &c, data);
	mutex_unlock(&dw->lock);

	if (c.sample_count &&
	    copy_to_user(user_samples, data, c.sample_count))
		ret = -EFAULT;
	if (copy_to_user(argp, &c, sizeof(c)))
		ret = -EFAULT;
	kvfree(data);
	return ret;
}

static long dect_ioctl(struct file *file, unsigned int cmd,
		       unsigned long arg)
{
	struct opendectcell *dw = file->private_data;
	struct dect_ioc_info info = {};
	struct gpio_desc *strap;
	__u32 value;
	int ret = 0;

	switch (cmd) {
	case DECT_IOC_INFO:
		info.version = DECT_IOC_VERSION;
		info.stage = DECT_STAGE_B;
		info.features = DECT_FEATURE_RESET | DECT_FEATURE_STRAP |
			DECT_FEATURE_GPIO_CAPTURE | DECT_FEATURE_PCM_RX;
		strscpy(info.board, dw->board->compatible,
			sizeof(info.board));
		strscpy(info.transport, dw->board->transport,
			sizeof(info.transport));
		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	case DECT_IOC_CAPTURE:
		return dect_ioctl_capture(dw, (void __user *)arg);
	case DECT_IOC_PCM_CONFIG:
	case DECT_IOC_PCM_STEP:
	case DECT_IOC_PCM_REGS:
	case DECT_IOC_PCM_STATS:
	case DECT_IOC_PCM_TX_FRAME:
		mutex_lock(&dw->lock);
		ret = dect_pcm_ioctl(&dw->pcm, cmd, (void __user *)arg);
		mutex_unlock(&dw->lock);
		return ret;
	case DECT_IOC_RESET:
	case DECT_IOC_STRAP:
		if (copy_from_user(&value, (void __user *)arg,
				   sizeof(value)))
			return -EFAULT;
		if (value > 1)
			return -EINVAL;
		break;
	default:
		return -ENOTTY;
	}

	mutex_lock(&dw->lock);
	if (cmd == DECT_IOC_RESET) {
		/* RESET(1) -> GPIO40 low; RESET(0) -> GPIO40 high. */
		gpiod_set_value(dw->reset, !value);
	} else if (value) {
		if (!dw->strap) {
			/* Retry an earlier failed restoration, if necessary. */
			ret = dect_unstrap_locked(dw);
			if (ret)
				goto out;

			strap = gpiod_get(dw->misc.this_device, "strap",
					  GPIOD_OUT_LOW);
			if (IS_ERR(strap)) {
				ret = PTR_ERR(strap);
				goto out;
			}
			dw->strap = strap;
		}
	} else {
		ret = dect_unstrap_locked(dw);
	}
out:
	mutex_unlock(&dw->lock);
	return ret;
}

static const struct file_operations dect_fops = {
	.owner = THIS_MODULE,
	.open = dect_open,
	.release = dect_release,
	.read = dect_read,
	.write = dect_write,
	.poll = dect_poll,
	.unlocked_ioctl = dect_ioctl,
};

static int dect_map_strap_ctl(struct opendectcell *dw)
{
	struct device_node *np;
	struct resource resource;
	resource_size_t offset;
	int ret;

	np = of_find_compatible_node(NULL, NULL,
				     dw->board->tlmm_compatible);
	if (!np)
		return -ENODEV;

	ret = of_address_to_resource(np, 0, &resource);
	of_node_put(np);
	if (ret)
		return ret;

	offset = dw->board->strap_offset * IPQ40XX_GPIO_STRIDE;
	if (resource_size(&resource) < offset + sizeof(u32))
		return -EINVAL;

	/* Do not request the region: pinctrl-msm already owns TLMM. */
	dw->strap_ctl = ioremap(resource.start + offset, sizeof(u32));
	if (!dw->strap_ctl)
		return -ENOMEM;

	return 0;
}

static int dect_map_pcm_inputs(struct opendectcell *dw)
{
	const struct dect_board *b = dw->board;
	struct device_node *np;
	struct resource resource;
	resource_size_t offset, length;
	int ret;

	np = of_find_compatible_node(NULL, NULL, b->tlmm_compatible);
	if (!np)
		return -ENODEV;
	ret = of_address_to_resource(np, 0, &resource);
	of_node_put(np);
	if (ret)
		return ret;

	offset = b->pcm_gpio_first * IPQ40XX_GPIO_STRIDE;
	length = (b->pcm_gpio_last - b->pcm_gpio_first + 1) *
		 IPQ40XX_GPIO_STRIDE;
	if (b->pcm_gpio_first > 21 || b->pcm_gpio_last < 27 ||
	    resource_size(&resource) < offset + length)
		return -EINVAL;

	/* The pinctrl driver owns this region: map only, never write. */
	dw->pcm_regs = ioremap(resource.start + offset, length);
	if (!dw->pcm_regs)
		return -ENOMEM;

	dw->dcl_in = dw->pcm_regs +
		(21 - b->pcm_gpio_first) * IPQ40XX_GPIO_STRIDE +
		IPQ40XX_GPIO_IN_OFFSET;
	dw->fsc_in = dw->pcm_regs +
		(22 - b->pcm_gpio_first) * IPQ40XX_GPIO_STRIDE +
		IPQ40XX_GPIO_IN_OFFSET;
	dw->di_in = dw->pcm_regs +
		(23 - b->pcm_gpio_first) * IPQ40XX_GPIO_STRIDE +
		IPQ40XX_GPIO_IN_OFFSET;
	dw->do_in = dw->pcm_regs +
		(27 - b->pcm_gpio_first) * IPQ40XX_GPIO_STRIDE +
		IPQ40XX_GPIO_IN_OFFSET;
	return 0;
}

static int __init dect_init(void)
{
	struct device_node *root;
	const char *model = "unknown";
	const char *compatible = "unknown";
	const struct dect_board *board = NULL;
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(dect_boards); i++) {
		if (of_machine_is_compatible(dect_boards[i].compatible)) {
			board = &dect_boards[i];
			break;
		}
	}

	if (!board) {
		root = of_find_node_by_path("/");
		if (root) {
			of_property_read_string(root, "model", &model);
			of_property_read_string(root, "compatible", &compatible);
		}
		pr_err("unsupported board: model=%s compatible=%s\n",
		       model, compatible);
		of_node_put(root);
		return -ENODEV;
	}

	mutex_init(&dect.lock);
	dect.board = board;
	dect.serial = bus_find_device_by_name(&platform_bus_type, NULL,
					      board->serial_device);
	if (!dect.serial) {
		pr_err("UART platform device %s not found\n",
		       board->serial_device);
		return -ENODEV;
	}

	ret = dect_map_strap_ctl(&dect);
	if (ret) {
		pr_err("cannot map GPIO16 TLMM mux: %d\n", ret);
		goto err_serial;
	}

	ret = dect_map_pcm_inputs(&dect);
	if (ret) {
		pr_err("cannot map PCM input GPIOs: %d\n", ret);
		goto err_map;
	}

	ret = dect_pcm_init(&dect.pcm, board->pcm, &dect.lock, dect.pcm_regs,
			    board->pcm_gpio_first);
	if (ret) {
		pr_err("cannot set up the PCM path: %d\n", ret);
		goto err_pcm;
	}

	dect.misc.minor = MISC_DYNAMIC_MINOR;
	dect.misc.name = "dect0";
	dect.misc.mode = 0600;
	dect.misc.fops = &dect_fops;

	gpiod_add_lookup_table(board->gpio_lookup);
	ret = misc_register(&dect.misc);
	if (ret) {
		gpiod_remove_lookup_table(board->gpio_lookup);
		goto err_pcm_init;
	}

	pr_info("Phase A + B loaded for %s\n", board->compatible);
	return 0;

err_pcm_init:
	dect_pcm_exit(&dect.pcm);
err_pcm:
	iounmap(dect.pcm_regs);
err_map:
	iounmap(dect.strap_ctl);
err_serial:
	put_device(dect.serial);
	return ret;
}

static void __exit dect_exit(void)
{
	misc_deregister(&dect.misc);
	mutex_lock(&dect.lock);
	dect_close_locked(&dect);
	mutex_unlock(&dect.lock);
	gpiod_remove_lookup_table(dect.board->gpio_lookup);
	dect_pcm_exit(&dect.pcm);
	iounmap(dect.pcm_regs);
	iounmap(dect.strap_ctl);
	put_device(dect.serial);
	pr_info("Phase A + B unloaded\n");
}

module_init(dect_init);
module_exit(dect_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("openDECTcell contributors");
MODULE_DESCRIPTION("DECT chip control and PCM receive for AVM 7520/7530");

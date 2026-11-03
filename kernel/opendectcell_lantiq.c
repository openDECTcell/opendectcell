// SPDX-License-Identifier: GPL-2.0-only
/*
 * openDECTcell - DECT chip control for the AVM 7412 and 7362 SL.
 *
 * Lantiq VR9 boards: reset and boot control for the DECT chip, through the
 * same /dev/dect0 ioctls as the ipq40xx module. The PCM/TDM link is not
 * ours here: kmod-lantiq-ssc-pcm runs the VR9 SSC (/dev/ssc-pcm0, 7412),
 * kmod-lantiq-tdm-pcm the VR9 TDM block (/dev/tdm-pcm0, 7362 SL), and
 * dect-stubd talks to them directly. This module's own SSC engine
 * (opendectcell_ssc.c) went when nothing used it any more (2026-10-29).
 *
 * Facts: docs/models/7412-DECT-analysis.md, docs/7412-spi-tdm-and-loader.md,
 * docs/models/7362SL-DECT-analysis.md.
 */
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "opendectcell.h"

/* Reset GPIO 33, the boot-mode strap ("rd") GPIO 8 and the PCM clock enable
 * GPIO 7 ("the PCM clock-enable line"), all active high in the table; the module
 * drives reset/strap low to assert. GPIO7 must be high before the chip's
 * reset: the chip only starts its PCM bit clock (DCL) if it sees it. */
static struct gpiod_lookup_table fritz7412_gpio_lookup = {
	.dev_id = "dect0",
	.table = {
		GPIO_LOOKUP("gpio-xway", 33, "reset", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("gpio-xway", 8, "strap", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("gpio-xway", 7, "clk-en", GPIO_ACTIVE_HIGH),
		{},
	},
};

/* The 7362 SL: reset GPIO 30, low holds the chip in reset (stock board
 * table, [CR 7362SL/A-0001]); no clock-enable line. The boot strap is the
 * chip's TX line, GPIO 11, which is the USIF's RX pin: the ROM answers only
 * if it is held low while reset is released, as on the 7520 (observed on a
 * 7362 SL, 2026-11-01, docs/journals/bench-7362sl.md). */
static struct gpiod_lookup_table fritz7362sl_gpio_lookup = {
	.dev_id = "dect0",
	.table = {
		GPIO_LOOKUP("gpio-xway", 30, "reset", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("gpio-xway", 11, "strap", GPIO_ACTIVE_HIGH),
		{},
	},
};

/*
 * VR9 GPIO port 0 registers, as mainline pinctrl-xway.c lays them out:
 * direction at +0x08, alternate-function selects ALT0 at +0x0C and ALT1 at
 * +0x10, one bit per pin. Requesting a pin as a GPIO switches it to the GPIO
 * function and pinctrl-lantiq never switches it back, so a strap on a UART
 * pin must be handed back to the UART by hand.
 */
#define XWAY_GPIO_BASE		0x1e100b10
#define XWAY_P0_DIR		0x08
#define XWAY_P0_ALT0		0x0c
#define XWAY_P0_ALT1		0x10

struct dect_board {
	const char *compatible;
	const char *transport;
	struct gpiod_lookup_table *gpio_lookup;
	/*
	 * Port-0 pin the strap borrows from a peripheral, and that
	 * peripheral's function (bit 0 = ALT0, bit 1 = ALT1); -1: none.
	 */
	int strap_pin;
	unsigned int strap_alt;
};

static const struct dect_board dect_boards[] = {
	{
		.compatible = "avm,fritz7412",
		.transport = "none",	/* PCM: kmod-lantiq-ssc-pcm */
		.gpio_lookup = &fritz7412_gpio_lookup,
		.strap_pin = -1,
	},
	{
		.compatible = "avm,fritz7362sl",
		.transport = "none",	/* PCM: kmod-lantiq-tdm-pcm */
		.gpio_lookup = &fritz7362sl_gpio_lookup,
		.strap_pin = 11,	/* USIF uart_rx: function 1, ALT0 */
		.strap_alt = 1,
	},
};

struct opendectcell {
	struct miscdevice misc;
	struct mutex lock;
	const struct dect_board *board;
	struct gpio_desc *reset;
	bool opened;
	struct gpio_desc *strap;
	struct gpio_desc *clk_en;
	void __iomem *gpio_base;	/* port 0, map only: pinctrl owns it */
};

static struct opendectcell dect;

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

	/* Request and start with reset released (the line high). */
	dw->reset = gpiod_get(dw->misc.this_device, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(dw->reset)) {
		ret = PTR_ERR(dw->reset);
		dw->reset = NULL;
		goto out;
	}
	/*
	 * The boot-mode strap ("rd", GPIO8) is claimed lazily, on the first
	 * DECT_IOC_STRAP: claiming it changes the pin, which the 7412's chip
	 * does not tolerate at reset, so an unused strap must stay untouched.
	 */

	dw->opened = true;
	file->private_data = dw;
	ret = 0;
out:
	mutex_unlock(&dw->lock);
	return ret;
}

/*
 * Let go of the strap: the line becomes an input again (never left driven),
 * and where it is borrowed from a peripheral (the 7362 SL's USIF RX) that
 * peripheral gets its pin function back. The read-modify-write cannot
 * serialize against pinctrl-xway; nothing else changes port 0's mux while
 * the chip is being loaded.
 */
static void dect_unstrap_locked(struct opendectcell *dw)
{
	const struct dect_board *b = dw->board;
	u32 bit, v;

	if (!dw->strap)
		return;
	gpiod_direction_input(dw->strap);
	gpiod_put(dw->strap);
	dw->strap = NULL;
	if (b->strap_pin < 0 || !dw->gpio_base)
		return;
	bit = BIT(b->strap_pin);
	v = __raw_readl(dw->gpio_base + XWAY_P0_DIR);
	__raw_writel(v & ~bit, dw->gpio_base + XWAY_P0_DIR);
	v = __raw_readl(dw->gpio_base + XWAY_P0_ALT0);
	v = (b->strap_alt & 1) ? v | bit : v & ~bit;
	__raw_writel(v, dw->gpio_base + XWAY_P0_ALT0);
	v = __raw_readl(dw->gpio_base + XWAY_P0_ALT1);
	v = (b->strap_alt & 2) ? v | bit : v & ~bit;
	__raw_writel(v, dw->gpio_base + XWAY_P0_ALT1);
}

static void dect_close_locked(struct opendectcell *dw)
{
	dect_unstrap_locked(dw);
	if (dw->reset) {
		/* Raw high releases this active-low reset before release. */
		gpiod_set_value(dw->reset, 1);
		gpiod_put(dw->reset);
		dw->reset = NULL;
	}
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

static long dect_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct opendectcell *dw = file->private_data;
	struct dect_ioc_info info = {};
	__u32 value;

	switch (cmd) {
	case DECT_IOC_INFO:
		info.version = DECT_IOC_VERSION;
		info.stage = DECT_STAGE_A;
		info.features = DECT_FEATURE_RESET | DECT_FEATURE_STRAP;
		strscpy(info.board, dw->board->compatible, sizeof(info.board));
		strscpy(info.transport, dw->board->transport,
			sizeof(info.transport));
		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	case DECT_IOC_RESET:
		if (copy_from_user(&value, (void __user *)arg, sizeof(value)))
			return -EFAULT;
		if (value > 1)
			return -EINVAL;
		mutex_lock(&dw->lock);
		if (dw->reset)
			/* RESET(1) -> the line low; RESET(0) -> high. */
			gpiod_set_value(dw->reset, !value);
		mutex_unlock(&dw->lock);
		return 0;
	case DECT_IOC_STRAP:
		/* The chip samples its "rd" line (GPIO8) during reset. */
		if (copy_from_user(&value, (void __user *)arg, sizeof(value)))
			return -EFAULT;
		mutex_lock(&dw->lock);
		if (value) {
			/* fwload's "strap low": claim and drive low. */
			if (!dw->strap) {
				dw->strap = gpiod_get(dw->misc.this_device, "strap",
						      GPIOD_OUT_LOW);
				if (IS_ERR(dw->strap)) {
					dw->strap = NULL;
					mutex_unlock(&dw->lock);
					return -EOPNOTSUPP;
				}
			} else {
				gpiod_set_value(dw->strap, 0);
			}
		} else {
			/* fwload's "strap release" */
			dect_unstrap_locked(dw);
		}
		mutex_unlock(&dw->lock);
		return 0;
	case DECT_IOC_PCM_CONFIG:
	case DECT_IOC_PCM_STEP:
	case DECT_IOC_PCM_REGS:
	case DECT_IOC_PCM_STATS:
	case DECT_IOC_PCM_TX_FRAME:
	case DECT_IOC_CAPTURE:
		/* PCM is kmod-lantiq-ssc-pcm's or kmod-lantiq-tdm-pcm's */
		return -EOPNOTSUPP;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations dect_fops = {
	.owner = THIS_MODULE,
	.open = dect_open,
	.release = dect_release,
	.unlocked_ioctl = dect_ioctl,
};

static int __init dect_init(void)
{
	const struct dect_board *board = NULL;
	struct device_node *root;
	const char *model = "unknown", *compatible = "unknown";
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
			of_node_put(root);
		}
		pr_err("opendectcell: unsupported board: model=%s compatible=%s\n",
		       model, compatible);
		return -ENODEV;
	}

	mutex_init(&dect.lock);
	dect.board = board;

	dect.misc.minor = MISC_DYNAMIC_MINOR;
	dect.misc.name = "dect0";
	dect.misc.mode = 0600;
	dect.misc.fops = &dect_fops;
	dect.misc.parent = NULL;

	gpiod_add_lookup_table(board->gpio_lookup);
	ret = misc_register(&dect.misc);
	if (ret) {
		gpiod_remove_lookup_table(board->gpio_lookup);
		return ret;
	}

	/*
	 * PCM clock enable (GPIO7): high from module load to unload. The chip
	 * only starts its PCM bit clock if the line is high before its reset,
	 * and the stock firmware never lowers it again [CR A-0001]. Holding it
	 * per open() dropped it between dect-fwload and dect-stubd, and the
	 * bus stayed silent (observed on a 7412, 2026-10-28). Optional: another
	 * driver may already hold the line high.
	 */
	dect.clk_en = gpiod_get(dect.misc.this_device, "clk-en", GPIOD_OUT_HIGH);
	if (IS_ERR(dect.clk_en)) {
		pr_info("openDECTcell: clock-enable line not claimed: %ld\n",
			PTR_ERR(dect.clk_en));
		dect.clk_en = NULL;
	}

	/* Do not request the region: pinctrl-xway owns it. */
	if (board->strap_pin >= 0) {
		dect.gpio_base = ioremap(XWAY_GPIO_BASE, XWAY_P0_ALT1 + 4);
		if (!dect.gpio_base) {
			if (dect.clk_en)
				gpiod_put(dect.clk_en);
			misc_deregister(&dect.misc);
			gpiod_remove_lookup_table(board->gpio_lookup);
			return -ENOMEM;
		}
	}

	pr_info("openDECTcell: chip control loaded for %s\n", board->compatible);
	return 0;
}

static void __exit dect_exit(void)
{
	misc_deregister(&dect.misc);
	mutex_lock(&dect.lock);
	dect_close_locked(&dect);
	mutex_unlock(&dect.lock);
	if (dect.gpio_base)
		iounmap(dect.gpio_base);
	if (dect.clk_en)
		gpiod_put(dect.clk_en);
	gpiod_remove_lookup_table(dect.board->gpio_lookup);
	pr_info("openDECTcell: chip control unloaded\n");
}

module_init(dect_init);
module_exit(dect_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("openDECTcell contributors");
MODULE_DESCRIPTION("DECT chip control for the AVM 7412 and 7362 SL");

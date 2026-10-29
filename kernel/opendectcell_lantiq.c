// SPDX-License-Identifier: GPL-2.0-only
/*
 * openDECTcell - DECT chip control for the AVM 7412.
 *
 * Lantiq VR9 board: reset and boot control for the DECT chip, through the
 * same /dev/dect0 ioctls as the ipq40xx module. The PCM/TDM link is not
 * ours here: kmod-lantiq-ssc-pcm runs the VR9 SSC (/dev/ssc-pcm0), and
 * dect-stubd talks to it directly. This module's own SSC engine
 * (opendectcell_ssc.c) went when nothing used it any more (2026-10-29).
 *
 * Facts: docs/models/7412-DECT-analysis.md, docs/7412-spi-tdm-and-loader.md.
 */
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
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

struct dect_board {
	const char *compatible;
	const char *transport;
	struct gpiod_lookup_table *gpio_lookup;
};

static const struct dect_board dect_boards[] = {
	{
		.compatible = "avm,fritz7412",
		.transport = "none",	/* PCM: kmod-lantiq-ssc-pcm */
		.gpio_lookup = &fritz7412_gpio_lookup,
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

	/* Request and start with reset released (GPIO33 high). */
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

static void dect_close_locked(struct opendectcell *dw)
{
	if (dw->strap) {
		gpiod_set_value(dw->strap, 0);
		gpiod_put(dw->strap);
		dw->strap = NULL;
	}
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
			/* RESET(1) -> GPIO33 low; RESET(0) -> GPIO33 high. */
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
		} else if (dw->strap) {
			/*
			 * fwload's "strap release": release GPIO ownership so pinmux
			 * returns this line to UART RX.
			 */
			gpiod_put(dw->strap);
			dw->strap = NULL;
		}
		mutex_unlock(&dw->lock);
		return 0;
	case DECT_IOC_PCM_CONFIG:
	case DECT_IOC_PCM_STEP:
	case DECT_IOC_PCM_REGS:
	case DECT_IOC_PCM_STATS:
	case DECT_IOC_PCM_TX_FRAME:
	case DECT_IOC_CAPTURE:
		/* PCM is kmod-lantiq-ssc-pcm's (/dev/ssc-pcm0) */
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

	pr_info("openDECTcell: chip control loaded for %s\n", board->compatible);
	return 0;
}

static void __exit dect_exit(void)
{
	misc_deregister(&dect.misc);
	mutex_lock(&dect.lock);
	dect_close_locked(&dect);
	mutex_unlock(&dect.lock);
	if (dect.clk_en)
		gpiod_put(dect.clk_en);
	gpiod_remove_lookup_table(dect.board->gpio_lookup);
	pr_info("openDECTcell: chip control unloaded\n");
}

module_init(dect_init);
module_exit(dect_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("openDECTcell contributors");
MODULE_DESCRIPTION("DECT chip control for the AVM 7412");

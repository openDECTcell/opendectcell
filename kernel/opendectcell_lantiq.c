// SPDX-License-Identifier: GPL-2.0-only
/*
 * openDECTcell - DECT chip control and PCM/TDM receive for the AVM 7412.
 *
 * Lantiq VR9 board: reset and boot control for the DECT chip (Phase A), and
 * the PCM/TDM receive path over the VR9 SSC in SPI-slave mode (Phase B,
 * opendectcell_ssc.c). The /dev/dect0 interface is identical to the ipq40xx one.
 *
 * Facts: docs/models/7412-DECT-analysis.md, docs/7412-spi-tdm-and-loader.md.
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
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "opendectcell.h"
#include "opendectcell_ssc.h"

/* VR9 SSC and the XWAY GPIO/pinmux block (xrx200 physical addresses). */
#define FRITZ7412_SSC_SSC_BASE		0x1e100800
#define FRITZ7412_SSC_SSC_SIZE		0x100
#define FRITZ7412_SSC_PINMUX_BASE	0x1e100b10
#define FRITZ7412_SSC_PINMUX_SIZE	0xa0

static const struct dect_ssc_board fritz7412_ssc = {
	.ssc_base = FRITZ7412_SSC_SSC_BASE,
	.ssc_size = FRITZ7412_SSC_SSC_SIZE,
	.pinmux_base = FRITZ7412_SSC_PINMUX_BASE,
	.pinmux_size = FRITZ7412_SSC_PINMUX_SIZE,
	.pin_fsc = 9,		/* frame sync / CS; a plain GPIO on this board */
	.pin_dcl = 18,		/* bit clock */
	.pin_di = 17,		/* chip -> SoC */
	.pin_do = 16,		/* SoC -> chip */
	.pin_clk_en = 7,	/* DCL enable line, GPIO output */
};

/* Reset GPIO 33, active high in the table; RESET(1) drives it low. */
static struct gpiod_lookup_table fritz7412_gpio_lookup = {
	.dev_id = "dect0",
	.table = {
		GPIO_LOOKUP("gpio-xway", 33, "reset", GPIO_ACTIVE_HIGH),
		{},
	},
};

struct dect_board {
	const char *compatible;
	const char *transport;
	struct gpiod_lookup_table *gpio_lookup;
	const struct dect_ssc_board *ssc;
};

static const struct dect_board dect_boards[] = {
	{
		.compatible = "avm,fritz7412",
		.transport = "lantiq-ssc",
		.gpio_lookup = &fritz7412_gpio_lookup,
		.ssc = &fritz7412_ssc,
	},
};

struct opendectcell {
	struct miscdevice misc;
	struct mutex lock;
	const struct dect_board *board;
	struct gpio_desc *reset;
	bool opened;
	struct dect_ssc ssc;
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

	dw->opened = true;
	file->private_data = dw;
	ret = 0;
out:
	mutex_unlock(&dw->lock);
	return ret;
}

static void dect_close_locked(struct opendectcell *dw)
{
	dect_ssc_teardown(&dw->ssc);
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

static ssize_t dect_read(struct file *file, char __user *buf, size_t count,
			 loff_t *ppos)
{
	struct opendectcell *dw = file->private_data;

	return dect_ssc_read(&dw->ssc, buf, count,
			     file->f_flags & O_NONBLOCK);
}

static ssize_t dect_write(struct file *file, const char __user *buf,
			  size_t count, loff_t *ppos)
{
	struct opendectcell *dw = file->private_data;

	return dect_ssc_write(&dw->ssc, buf, count,
			      file->f_flags & O_NONBLOCK);
}

static __poll_t dect_poll(struct file *file, poll_table *wait)
{
	struct opendectcell *dw = file->private_data;

	return dect_ssc_poll(&dw->ssc, file, wait);
}

static long dect_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct opendectcell *dw = file->private_data;
	struct dect_ioc_info info = {};
	__u32 value;
	int ret = 0;

	switch (cmd) {
	case DECT_IOC_INFO:
		info.version = DECT_IOC_VERSION;
		info.stage = DECT_STAGE_B;
		info.features = DECT_FEATURE_RESET | DECT_FEATURE_PCM_RX;
		strscpy(info.board, dw->board->compatible, sizeof(info.board));
		strscpy(info.transport, dw->board->transport,
			sizeof(info.transport));
		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	case DECT_IOC_PCM_CONFIG:
	case DECT_IOC_PCM_STEP:
	case DECT_IOC_PCM_REGS:
	case DECT_IOC_PCM_STATS:
		if (mutex_lock_interruptible(&dw->lock))
			return -ERESTARTSYS;
		ret = dect_ssc_ioctl(&dw->ssc, cmd, (void __user *)arg);
		mutex_unlock(&dw->lock);
		return ret;
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
	case DECT_IOC_CAPTURE:
	case DECT_IOC_PCM_TX_FRAME:
		/* The 7412 has no boot strap pin; its PCM pins belong to the
		 * SSC peripheral (no GPIO polling capture); transmit is not
		 * implemented yet. */
		return -EOPNOTSUPP;
	default:
		return -ENOTTY;
	}
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

	ret = dect_ssc_init(&dect.ssc, board->ssc, &dect.lock);
	if (ret) {
		pr_err("openDECTcell: cannot set up the SSC path: %d\n", ret);
		return ret;
	}

	dect.misc.minor = MISC_DYNAMIC_MINOR;
	dect.misc.name = "dect0";
	dect.misc.mode = 0600;
	dect.misc.fops = &dect_fops;
	dect.misc.parent = NULL;

	gpiod_add_lookup_table(board->gpio_lookup);
	ret = misc_register(&dect.misc);
	if (ret) {
		gpiod_remove_lookup_table(board->gpio_lookup);
		dect_ssc_exit(&dect.ssc);
		return ret;
	}

	pr_info("openDECTcell: Lantiq transport loaded for %s\n", board->compatible);
	return 0;
}

static void __exit dect_exit(void)
{
	misc_deregister(&dect.misc);
	mutex_lock(&dect.lock);
	dect_close_locked(&dect);
	mutex_unlock(&dect.lock);
	gpiod_remove_lookup_table(dect.board->gpio_lookup);
	dect_ssc_exit(&dect.ssc);
	pr_info("openDECTcell: Lantiq transport unloaded\n");
}

module_init(dect_init);
module_exit(dect_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("openDECTcell contributors");
MODULE_DESCRIPTION("DECT chip control and PCM/TDM receive for the AVM 7412");

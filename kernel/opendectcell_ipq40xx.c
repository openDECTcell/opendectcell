// SPDX-License-Identifier: GPL-2.0-only
/*
 * openDECTcell - DECT reset and boot strap control for the AVM 7520/7530.
 *
 * Phase A only: no firmware loader and no PCM/TDM data path.
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
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "opendectcell.h"

/* IPQ4019 TLMM: ctl_reg = 0x1000 * GPIO; mux selector is bits [5:2]. */
#define IPQ40XX_GPIO_STRIDE	0x1000
#define IPQ40XX_MUX_SHIFT	2
#define IPQ40XX_MUX_MASK	GENMASK(5, 2)

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

struct dect_board {
	const char *compatible;
	const char *transport;
	const char *serial_device;
	const char *tlmm_compatible;
	unsigned int strap_offset;
	unsigned int uart_mux;
	struct gpiod_lookup_table *gpio_lookup;
};

static const struct dect_board dect_boards[] = {
	{
		.compatible = "avm,fritzbox-7530",
		.transport = "ipq40xx-tdm",
		.serial_device = "78af000.serial",
		.tlmm_compatible = "qcom,ipq4019-pinctrl",
		.strap_offset = 16,
		.uart_mux = 1,	/* GPIO16 function 1: blsp_uart0 */
		.gpio_lookup = &fritz7530_gpio_lookup,
	},
};

struct opendectcell {
	struct miscdevice misc;
	struct mutex lock;
	const struct dect_board *board;
	struct device *serial;
	void __iomem *strap_ctl;
	struct gpio_desc *reset;
	struct gpio_desc *strap;
	bool opened;
	bool uart_restore_pending;
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
		info.stage = DECT_STAGE_A;
		info.features = DECT_FEATURE_RESET | DECT_FEATURE_STRAP;
		strscpy(info.board, dw->board->compatible,
			sizeof(info.board));
		strscpy(info.transport, dw->board->transport,
			sizeof(info.transport));
		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
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

	dect.misc.minor = MISC_DYNAMIC_MINOR;
	dect.misc.name = "dect0";
	dect.misc.mode = 0600;
	dect.misc.fops = &dect_fops;

	gpiod_add_lookup_table(board->gpio_lookup);
	ret = misc_register(&dect.misc);
	if (ret) {
		gpiod_remove_lookup_table(board->gpio_lookup);
		goto err_map;
	}

	pr_info("Phase A loaded for %s (GPIO reset and strap only)\n",
		board->compatible);
	return 0;

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
	iounmap(dect.strap_ctl);
	put_device(dect.serial);
	pr_info("Phase A unloaded\n");
}

module_init(dect_init);
module_exit(dect_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("openDECTcell contributors");
MODULE_DESCRIPTION("DECT reset and UART boot strap for IPQ4019 FRITZ!Box");

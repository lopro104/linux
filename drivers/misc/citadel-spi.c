// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Citadel (Titan M) transport driver
 *
 * Copyright (C) 2017 Fernando Lugo <flugo@google.com>
 *
 * Ported from google/msm-4.14 to the mainline SPI/gpiod APIs.
 */

#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/ioctl.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <uapi/linux/citadel.h>

#define CITADEL_TPM_READ	0x80000000

#define MAX_DATA_SIZE		2044
#define CITADEL_MAX_DEVICES	4
#define CITADEL_TPM_TIMEOUT_MS	10

struct citadel_data {
	dev_t			devt;
	struct cdev		cdev;
	struct spi_device	*spi;
	struct gpio_desc	*ctdl_ap_irq;
	int			irq;
	wait_queue_head_t	waitq;
	struct gpio_desc	*ctdl_rst;
	atomic_t		users;
	void			*tx_buf;
	void			*rx_buf;
};

static const struct class citadel_class = {
	.name = "citadel",
};
static dev_t citadel_devt;

static int citadel_wait_cmd_done(struct citadel_data *citadel)
{
	struct spi_device *spi = citadel->spi;
	struct spi_message m;
	int ret;
	unsigned long to = jiffies + 1 +	/* at least one jiffy */
		msecs_to_jiffies(CITADEL_TPM_TIMEOUT_MS);
	struct spi_transfer spi_xfer = {
		.rx_buf = citadel->rx_buf,
		.len = 1,
		.cs_change = 1,
	};
	u8 *val = citadel->rx_buf;

	/*
	 * We have sent the initial four-byte command to Citadel on MOSI, and
	 * now we're waiting for bit0 of the MISO byte to be set, indicating
	 * that we can continue with the rest of the transaction. If that bit
	 * is not immediately set, we'll keep sending one more don't-care byte
	 * on MOSI just to read MISO until it is. If Citadel is in deep sleep
	 * it sends 0x5A until it wakes up (~40ms), so we time out and let
	 * userspace retry. If it unexpectedly reboots it returns 0xFF/0xDF.
	 */
	do {
		if (time_after(jiffies, to)) {
			dev_warn(&spi->dev, "Citadel SPI timed out\n");
			return -EBUSY;
		}
		spi_message_init(&m);
		spi_message_add_tail(&spi_xfer, &m);
		ret = spi_sync_locked(spi, &m);
		if (ret)
			return ret;
	} while (!*val);

	/* Return EAGAIN if unexpected bytes were received. */
	return *val & 0x01 ? 0 : -EAGAIN;
}

static int citadel_tpm_datagram(struct citadel_data *citadel,
				struct citadel_ioc_tpm_datagram *dg)
{
	int is_read;
	int citadel_is_awake;
	int ret;
	int ignore_result = 0;
	struct spi_device *spi = citadel->spi;
	struct spi_message m;
	struct spi_transfer spi_xfer = {
		.tx_buf = citadel->tx_buf,
		.rx_buf = citadel->rx_buf,
		.len = 4,
		.cs_change = 1,
	};
	u32 *command = citadel->tx_buf;
	u32 *response = citadel->rx_buf;

	/* Read == from SPI, to userland. */
	is_read = dg->command & CITADEL_TPM_READ;

	/* Lock the SPI bus until we're completely done */
	spi_bus_lock(spi->controller);

	/* The command must be big-endian */
	*command = cpu_to_be32(dg->command);

	spi_message_init(&m);
	spi_message_add_tail(&spi_xfer, &m);

	ret = spi_sync_locked(spi, &m);
	if (ret)
		goto exit;

	/*
	 * Verify that citadel is idle. 0xdf is what citadel sends when the
	 * SPI FIFO is empty and it is in TPM wait mode. Once a command is
	 * sent, the last bit shows whether Citadel is ready to send the
	 * response: a 0x01 byte, possibly preceded by 0x00 bytes.
	 */
	citadel_is_awake = *response == be32_to_cpu(0xdfdfdfde);

	if (citadel_is_awake) {
		ret = citadel_wait_cmd_done(citadel);
		if (ret)
			citadel_is_awake = 0;
	}

	if (!dg->len || !citadel_is_awake) {
		/* Transfer one more byte and throw it away to release CS */
		is_read = 1;
		dg->len = 1;
		ignore_result = 1;
	}

	spi_xfer.cs_change = 0;
	spi_xfer.len = dg->len;
	if (is_read) {
		spi_xfer.rx_buf = citadel->rx_buf;
		spi_xfer.tx_buf = NULL;
	} else {
		spi_xfer.rx_buf = NULL;
		spi_xfer.tx_buf = citadel->tx_buf;

		if (copy_from_user(citadel->tx_buf,
				   (void __user *)(uintptr_t)dg->buf, dg->len)) {
			ret = -EFAULT;
			goto exit;
		}
	}

	spi_message_init(&m);
	spi_message_add_tail(&spi_xfer, &m);
	ret = spi_sync_locked(spi, &m);
	if (ret)
		goto exit;

	/* Citadel was asleep: toggling CS wakes it, caller must retry later */
	if (!citadel_is_awake)
		ret = -EAGAIN;

	if (ignore_result)
		goto exit;

	if (is_read && copy_to_user((void __user *)(uintptr_t)dg->buf,
				    citadel->rx_buf, dg->len))
		ret = -EFAULT;

exit:
	spi_bus_unlock(spi->controller);

	return ret;
}

static int citadel_reset(struct citadel_data *citadel)
{
	struct spi_device *spi = citadel->spi;

	/* Synchronize with the datagrams by locking the SPI bus */
	spi_bus_lock(spi->controller);

	/* Assert reset for at least 3ms after VDDIOM is stable; 10ms is safe */
	gpiod_set_value_cansleep(citadel->ctdl_rst, 1);
	msleep(10);

	/* Clear reset and wait for Citadel to become functional */
	gpiod_set_value_cansleep(citadel->ctdl_rst, 0);
	msleep(100);

	spi_bus_unlock(spi->controller);
	return 0;
}

static long citadel_ioctl(struct file *filp, unsigned int cmd,
			  unsigned long arg)
{
	struct citadel_data *citadel = filp->private_data;
	struct citadel_ioc_tpm_datagram dg;

	if (_IOC_TYPE(cmd) != CITADEL_IOC_MAGIC)
		return -ENOTTY;

	switch (cmd) {
	case CITADEL_IOC_TPM_DATAGRAM:
		if (_IOC_SIZE(cmd) != sizeof(dg))
			return -EINVAL;

		if (copy_from_user(&dg, (void __user *)arg, sizeof(dg)))
			return -EFAULT;

		if (dg.len > MAX_DATA_SIZE)
			return -E2BIG;

		return citadel_tpm_datagram(citadel, &dg);
	case CITADEL_IOC_RESET:
		return citadel_reset(citadel);
	}
	return -EINVAL;
}

static ssize_t citadel_read(struct file *filp, char __user *buf, size_t count,
			    loff_t *f_pos)
{
	struct citadel_data *citadel = filp->private_data;
	size_t c = 0;
	size_t len;
	int ret;

	while (count) {
		len = min_t(size_t, count, MAX_DATA_SIZE);
		ret = spi_read(citadel->spi, citadel->rx_buf, len);
		if (ret)
			return ret;
		if (copy_to_user(buf + c, citadel->rx_buf, len))
			return -EFAULT;
		c += len;
		count -= len;
	}

	return c;
}

static ssize_t citadel_write(struct file *filp, const char __user *buf,
			     size_t count, loff_t *f_pos)
{
	struct citadel_data *citadel = filp->private_data;
	size_t c = 0;
	size_t len;
	int ret;

	while (count) {
		len = min_t(size_t, count, MAX_DATA_SIZE);
		if (copy_from_user(citadel->tx_buf, buf + c, len))
			return -EFAULT;
		ret = spi_write(citadel->spi, citadel->tx_buf, len);
		if (ret)
			return ret;
		c += len;
		count -= len;
	}

	return c;
}

static int citadel_open(struct inode *inode, struct file *filp)
{
	struct citadel_data *citadel;

	citadel = container_of(inode->i_cdev, struct citadel_data, cdev);

	/* we only support 1 user at the same time */
	if (!atomic_add_unless(&citadel->users, 1, 1))
		return -EBUSY;

	filp->private_data = citadel;
	return nonseekable_open(inode, filp);
}

static __poll_t citadel_poll(struct file *filp, poll_table *wait)
{
	struct citadel_data *citadel = filp->private_data;

	poll_wait(filp, &citadel->waitq, wait);
	return gpiod_get_value(citadel->ctdl_ap_irq) ? EPOLLIN : 0;
}

static int citadel_release(struct inode *inode, struct file *filp)
{
	struct citadel_data *citadel = filp->private_data;

	atomic_dec(&citadel->users);

	return 0;
}

static const struct file_operations citadel_fops = {
	.owner		= THIS_MODULE,
	.write		= citadel_write,
	.read		= citadel_read,
	.open		= citadel_open,
	.poll		= citadel_poll,
	.release	= citadel_release,
	.unlocked_ioctl	= citadel_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static const struct of_device_id citadel_dt_ids[] = {
	{ .compatible = "google,citadel" },
	{},
};
MODULE_DEVICE_TABLE(of, citadel_dt_ids);

static irqreturn_t citadel_irq_handler(int irq, void *handle)
{
	struct citadel_data *citadel = handle;

	wake_up_interruptible(&citadel->waitq);
	return IRQ_HANDLED;
}

static void citadel_free_bufs(void *data)
{
	struct citadel_data *citadel = data;

	free_pages((unsigned long)citadel->rx_buf, get_order(MAX_DATA_SIZE));
	free_pages((unsigned long)citadel->tx_buf, get_order(MAX_DATA_SIZE));
}

static int citadel_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct citadel_data *citadel;
	struct device *chrdev;
	int ret;
	u32 minor;

	/* use chip select as minor */
	minor = spi_get_chipselect(spi, 0);
	if (minor >= CITADEL_MAX_DEVICES)
		return dev_err_probe(dev, -ENXIO, "minor %u out of range\n", minor);

	citadel = devm_kzalloc(dev, sizeof(*citadel), GFP_KERNEL);
	if (!citadel)
		return -ENOMEM;

	citadel->tx_buf = (void *)__get_free_pages(GFP_KERNEL,
						   get_order(MAX_DATA_SIZE));
	citadel->rx_buf = (void *)__get_free_pages(GFP_KERNEL,
						   get_order(MAX_DATA_SIZE));
	if (!citadel->tx_buf || !citadel->rx_buf) {
		citadel_free_bufs(citadel);
		return -ENOMEM;
	}
	ret = devm_add_action_or_reset(dev, citadel_free_bufs, citadel);
	if (ret)
		return ret;

	init_waitqueue_head(&citadel->waitq);
	atomic_set(&citadel->users, 0);
	citadel->spi = spi;
	citadel->devt = MKDEV(MAJOR(citadel_devt), minor);
	spi_set_drvdata(spi, citadel);

	citadel->ctdl_ap_irq = devm_gpiod_get(dev, "ctdl-ap-irq", GPIOD_IN);
	if (IS_ERR(citadel->ctdl_ap_irq))
		return dev_err_probe(dev, PTR_ERR(citadel->ctdl_ap_irq),
				     "failed to get ctdl-ap-irq gpio\n");

	citadel->irq = gpiod_to_irq(citadel->ctdl_ap_irq);
	if (citadel->irq < 0)
		return dev_err_probe(dev, citadel->irq, "no irq for ctdl-ap-irq\n");

	ret = devm_request_irq(dev, citadel->irq, citadel_irq_handler,
			       IRQF_TRIGGER_RISING | IRQF_ONESHOT,
			       dev_name(dev), citadel);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request irq\n");

	enable_irq_wake(citadel->irq);

	citadel->ctdl_rst = devm_gpiod_get(dev, "ctdl-rst", GPIOD_OUT_LOW);
	if (IS_ERR(citadel->ctdl_rst))
		return dev_err_probe(dev, PTR_ERR(citadel->ctdl_rst),
				     "failed to get ctdl-rst gpio\n");

	chrdev = device_create(&citadel_class, dev, citadel->devt, NULL,
			       "citadel%u", minor);
	if (IS_ERR(chrdev))
		return PTR_ERR(chrdev);

	cdev_init(&citadel->cdev, &citadel_fops);
	citadel->cdev.owner = THIS_MODULE;
	ret = cdev_add(&citadel->cdev, citadel->devt, 1);
	if (ret) {
		device_destroy(&citadel_class, citadel->devt);
		return ret;
	}

	return 0;
}

static void citadel_remove(struct spi_device *spi)
{
	struct citadel_data *citadel = spi_get_drvdata(spi);

	cdev_del(&citadel->cdev);
	device_destroy(&citadel_class, citadel->devt);
}

static struct spi_driver citadel_spi_driver = {
	.driver = {
		.name		= "citadel",
		.of_match_table	= citadel_dt_ids,
	},
	.probe	= citadel_probe,
	.remove	= citadel_remove,
};

static int __init citadel_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&citadel_devt, 0, CITADEL_MAX_DEVICES,
				  "citadel");
	if (ret) {
		pr_err("citadel: failed to alloc cdev region %d\n", ret);
		return ret;
	}

	ret = class_register(&citadel_class);
	if (ret) {
		unregister_chrdev_region(citadel_devt, CITADEL_MAX_DEVICES);
		return ret;
	}

	ret = spi_register_driver(&citadel_spi_driver);
	if (ret < 0) {
		class_unregister(&citadel_class);
		unregister_chrdev_region(citadel_devt, CITADEL_MAX_DEVICES);
	}
	return ret;
}

static void __exit citadel_exit(void)
{
	spi_unregister_driver(&citadel_spi_driver);
	class_unregister(&citadel_class);
	unregister_chrdev_region(citadel_devt, CITADEL_MAX_DEVICES);
}

module_init(citadel_init);
module_exit(citadel_exit);

MODULE_AUTHOR("Fernando Lugo, <flugo@google.com>");
MODULE_DESCRIPTION("Google Citadel (Titan M) TPM transport driver");
MODULE_LICENSE("GPL");

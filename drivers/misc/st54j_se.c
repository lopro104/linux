// SPDX-License-Identifier: GPL-2.0
/*
 * ST54J embedded secure element SPI transport
 *
 * A raw SPI pipe at /dev/st54j_se plus a reset ioctl; the APDU/T=1 framing
 * lives in the secure_element HAL.
 *
 * Copyright (C) 2018 ST Microelectronics S.A.
 * Copyright 2019 Google Inc.
 */
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>
#include <uapi/linux/st54j_se.h>

#define ST54_MAX_BUF 258U

struct st54j_se_dev {
	struct spi_device *spi;
	struct mutex mutex;
	struct miscdevice device;
	bool device_open;
	struct gpio_desc *reset_gpio;
	u8 *kbuf;
};

static long st54j_se_ioctl(struct file *filp, unsigned int cmd,
			   unsigned long arg)
{
	struct st54j_se_dev *ese_dev = filp->private_data;

	mutex_lock(&ese_dev->mutex);
	switch (cmd) {
	case ST54J_SE_RESET:
		dev_info(&ese_dev->spi->dev, "resetting the eSE\n");
		/* pulse reset for 5 ms */
		gpiod_set_value_cansleep(ese_dev->reset_gpio, 1);
		usleep_range(5000, 5500);
		gpiod_set_value_cansleep(ese_dev->reset_gpio, 0);
		break;
	}
	mutex_unlock(&ese_dev->mutex);

	return 0;
}

static int st54j_se_open(struct inode *inode, struct file *filp)
{
	struct st54j_se_dev *ese_dev = container_of(filp->private_data,
						    struct st54j_se_dev, device);
	int ret = 0;

	mutex_lock(&ese_dev->mutex);
	if (ese_dev->device_open) {
		ret = -EBUSY;
	} else {
		ese_dev->device_open = true;
		filp->private_data = ese_dev;
	}
	mutex_unlock(&ese_dev->mutex);

	return ret;
}

static int st54j_se_release(struct inode *ino, struct file *filp)
{
	struct st54j_se_dev *ese_dev = filp->private_data;

	mutex_lock(&ese_dev->mutex);
	ese_dev->device_open = false;
	mutex_unlock(&ese_dev->mutex);

	return 0;
}

static ssize_t st54j_se_write(struct file *filp, const char __user *ubuf,
			      size_t len, loff_t *offset)
{
	struct st54j_se_dev *ese_dev = filp->private_data;
	size_t bytes = len;
	ssize_t ret = len;

	if (len > INT_MAX)
		return -EINVAL;

	mutex_lock(&ese_dev->mutex);
	while (bytes > 0) {
		size_t block = min(bytes, ST54_MAX_BUF);
		int rc;

		if (copy_from_user(ese_dev->kbuf, ubuf, block)) {
			ret = -EFAULT;
			break;
		}

		rc = spi_write(ese_dev->spi, ese_dev->kbuf, block);
		if (rc < 0) {
			dev_dbg(&ese_dev->spi->dev, "SPI write failed: %d\n", rc);
			ret = rc;
			break;
		}
		ubuf += block;
		bytes -= block;
	}
	mutex_unlock(&ese_dev->mutex);

	return ret;
}

static ssize_t st54j_se_read(struct file *filp, char __user *ubuf, size_t len,
			     loff_t *offset)
{
	struct st54j_se_dev *ese_dev = filp->private_data;
	size_t bytes = len;
	ssize_t ret = len;

	if (len > INT_MAX)
		return -EINVAL;

	mutex_lock(&ese_dev->mutex);
	while (bytes > 0) {
		size_t block = min(bytes, ST54_MAX_BUF);
		int rc;

		memset(ese_dev->kbuf, 0, ST54_MAX_BUF);
		rc = spi_read(ese_dev->spi, ese_dev->kbuf, block);
		if (rc < 0) {
			dev_err(&ese_dev->spi->dev, "SPI read failed: %d\n", rc);
			ret = rc;
			break;
		}
		if (copy_to_user(ubuf, ese_dev->kbuf, block)) {
			ret = -EFAULT;
			break;
		}
		ubuf += block;
		bytes -= block;
	}
	mutex_unlock(&ese_dev->mutex);

	return ret;
}

static const struct file_operations st54j_se_dev_fops = {
	.owner = THIS_MODULE,
	.read = st54j_se_read,
	.write = st54j_se_write,
	.open = st54j_se_open,
	.release = st54j_se_release,
	.unlocked_ioctl = st54j_se_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static int st54j_se_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct st54j_se_dev *ese_dev;
	int ret;

	ese_dev = devm_kzalloc(dev, sizeof(*ese_dev), GFP_KERNEL);
	if (!ese_dev)
		return -ENOMEM;

	ese_dev->kbuf = devm_kzalloc(dev, ST54_MAX_BUF, GFP_KERNEL);
	if (!ese_dev->kbuf)
		return -ENOMEM;

	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return dev_err_probe(dev, ret, "SPI setup failed\n");

	/* Take the eSE out of reset */
	ese_dev->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ese_dev->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ese_dev->reset_gpio),
				     "failed to get the reset GPIO\n");

	ese_dev->spi = spi;
	ret = devm_mutex_init(dev, &ese_dev->mutex);
	if (ret)
		return ret;

	ese_dev->device.minor = MISC_DYNAMIC_MINOR;
	ese_dev->device.name = "st54j_se";
	ese_dev->device.fops = &st54j_se_dev_fops;
	ese_dev->device.parent = dev;

	spi_set_drvdata(spi, ese_dev);

	ret = misc_register(&ese_dev->device);
	if (ret)
		return dev_err_probe(dev, ret, "misc_register failed\n");

	return 0;
}

static void st54j_se_remove(struct spi_device *spi)
{
	struct st54j_se_dev *ese_dev = spi_get_drvdata(spi);

	misc_deregister(&ese_dev->device);
}

static const struct of_device_id st54j_se_match_table[] = {
	{ .compatible = "st,st54j-se" },
	{ }
};
MODULE_DEVICE_TABLE(of, st54j_se_match_table);

static const struct spi_device_id st54j_se_id[] = {
	{ "st54j-se" },
	{ }
};
MODULE_DEVICE_TABLE(spi, st54j_se_id);

static struct spi_driver st54j_se_driver = {
	.driver = {
		.name = "st54j_se",
		.of_match_table = st54j_se_match_table,
	},
	.id_table = st54j_se_id,
	.probe = st54j_se_probe,
	.remove = st54j_se_remove,
};
module_spi_driver(st54j_se_driver);

MODULE_DESCRIPTION("ST54J eSE SPI transport");
MODULE_AUTHOR("ST Microelectronics");
MODULE_LICENSE("GPL");

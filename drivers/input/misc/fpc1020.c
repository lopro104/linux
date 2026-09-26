// SPDX-License-Identifier: GPL-2.0-only
/*
 * FPC1020 fingerprint sensor platform driver
 *
 * Controls the electrical side of an FPC1020-family sensor whose SPI bus
 * is owned by the TrustZone fingerprint app: supplies, the reset line and
 * the interrupt line. It never talks to the sensor itself.
 *
 * The sysfs interface (irq, wakeup_enable, hw_reset, device_prepare,
 * regulator_enable, pinctl_set, clk_enable) is ABI for the closed FPC
 * fingerprint HAL, which polls "irq" for sensor interrupts.
 *
 * Copyright (c) 2015 Fingerprint Cards AB <tech@fingerprints.com>
 *
 * Ported from google/msm-4.14 (drivers/input/misc/fpc1020_platform_tee.c)
 * to gpiod, managed resources and the current wakeup source API.
 */

#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#define FPC_TTW_HOLD_TIME_MS		3000

#define RESET_LOW_SLEEP_MIN_US		5000
#define RESET_LOW_SLEEP_MAX_US		(RESET_LOW_SLEEP_MIN_US + 100)
#define RESET_HIGH_SLEEP1_MIN_US	100
#define RESET_HIGH_SLEEP1_MAX_US	(RESET_HIGH_SLEEP1_MIN_US + 100)
#define RESET_HIGH_SLEEP2_MIN_US	5000
#define RESET_HIGH_SLEEP2_MAX_US	(RESET_HIGH_SLEEP2_MIN_US + 100)
#define PWR_ON_SLEEP_MIN_US		100
#define PWR_ON_SLEEP_MAX_US		(PWR_ON_SLEEP_MIN_US + 900)

struct fpc1020_vreg {
	const char *name;
	int uv;
};

static const struct fpc1020_vreg fpc1020_vregs[] = {
	{ "vdd_ana", 1800000 },
	{ "vcc_spi", 1800000 },
	{ "vdd_io", 1800000 },
};

struct fpc1020_data {
	struct device *dev;

	struct gpio_desc *irq_gpio;
	struct gpio_desc *rst_gpio;
	int irq;

	/* NULL when the supply is not described in DT */
	struct regulator *vreg[ARRAY_SIZE(fpc1020_vregs)];
	bool vreg_on[ARRAY_SIZE(fpc1020_vregs)];

	struct wakeup_source *ttw_ws;
	struct mutex lock; /* serialises the sysfs controls */
	bool prepared;
	atomic_t wakeup_enabled; /* read from the IRQ handler */
};

static int vreg_setup(struct fpc1020_data *fpc1020, const char *name,
		      bool enable)
{
	struct device *dev = fpc1020->dev;
	struct regulator *vreg;
	size_t i;
	int rc;

	for (i = 0; i < ARRAY_SIZE(fpc1020_vregs); i++) {
		const char *n = fpc1020_vregs[i].name;

		if (!strncmp(n, name, strlen(n)))
			break;
	}

	if (i == ARRAY_SIZE(fpc1020_vregs)) {
		dev_err(dev, "Regulator %s not found\n", name);
		return -EINVAL;
	}

	vreg = fpc1020->vreg[i];
	if (!vreg || fpc1020->vreg_on[i] == enable)
		return 0;

	if (enable) {
		if (regulator_count_voltages(vreg) > 0) {
			rc = regulator_set_voltage(vreg, fpc1020_vregs[i].uv,
						   fpc1020_vregs[i].uv);
			if (rc)
				dev_err(dev, "Unable to set voltage on %s, %d\n",
					name, rc);
		}

		rc = regulator_enable(vreg);
		if (rc) {
			dev_err(dev, "error enabling %s: %d\n", name, rc);
			return rc;
		}
	} else {
		regulator_disable(vreg);
		dev_dbg(dev, "disabled %s\n", name);
	}
	fpc1020->vreg_on[i] = enable;

	return 0;
}

/*
 * Downstream switched the reset line through pinctrl states with these
 * names, and userspace can still select them through "pinctl_set". Map them
 * onto the reset GPIO; the IRQ pin config is the static "default" state.
 */
static int select_pin_ctl(struct fpc1020_data *fpc1020, const char *name)
{
	if (sysfs_streq(name, "fpc1020_reset_reset")) {
		gpiod_set_value_cansleep(fpc1020->rst_gpio, 1);
		return 0;
	}

	if (sysfs_streq(name, "fpc1020_reset_active")) {
		gpiod_set_value_cansleep(fpc1020->rst_gpio, 0);
		return 0;
	}

	if (sysfs_streq(name, "fpc1020_irq_active"))
		return 0;

	dev_err(fpc1020->dev, "%s: '%s' not found\n", __func__, name);

	return -EINVAL;
}

/* Clocks are not handled by the platform variant; kept for compatibility. */
static ssize_t clk_enable_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	dev_dbg(dev, "clk_enable sysfs node not enabled in platform driver\n");

	return count;
}
static DEVICE_ATTR_WO(clk_enable);

static ssize_t pinctl_set_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);
	int rc;

	mutex_lock(&fpc1020->lock);
	rc = select_pin_ctl(fpc1020, buf);
	mutex_unlock(&fpc1020->lock);

	return rc ? rc : count;
}
static DEVICE_ATTR_WO(pinctl_set);

static ssize_t regulator_enable_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);
	char name[16];
	bool enable;
	char op;
	int rc;

	if (sscanf(buf, "%15[^,],%c", name, &op) != 2)
		return -EINVAL;

	if (op == 'e')
		enable = true;
	else if (op == 'd')
		enable = false;
	else
		return -EINVAL;

	mutex_lock(&fpc1020->lock);
	rc = vreg_setup(fpc1020, name, enable);
	mutex_unlock(&fpc1020->lock);

	return rc ? rc : count;
}
static DEVICE_ATTR_WO(regulator_enable);

static int hw_reset(struct fpc1020_data *fpc1020)
{
	gpiod_set_value_cansleep(fpc1020->rst_gpio, 0);
	usleep_range(RESET_HIGH_SLEEP1_MIN_US, RESET_HIGH_SLEEP1_MAX_US);

	gpiod_set_value_cansleep(fpc1020->rst_gpio, 1);
	usleep_range(RESET_LOW_SLEEP_MIN_US, RESET_LOW_SLEEP_MAX_US);

	gpiod_set_value_cansleep(fpc1020->rst_gpio, 0);
	usleep_range(RESET_HIGH_SLEEP2_MIN_US, RESET_HIGH_SLEEP2_MAX_US);

	dev_info(fpc1020->dev, "IRQ after reset %d\n",
		 gpiod_get_value_cansleep(fpc1020->irq_gpio));

	return 0;
}

static ssize_t hw_reset_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);
	int rc;

	if (strncmp(buf, "reset", strlen("reset")))
		return -EINVAL;

	mutex_lock(&fpc1020->lock);
	rc = hw_reset(fpc1020);
	mutex_unlock(&fpc1020->lock);

	return rc ? rc : count;
}
static DEVICE_ATTR_WO(hw_reset);

/*
 * Power the sensor up or down in the order the sensor spec requires. After
 * power-up the TEE side must soft-reset the sensor, since chip select is
 * not under our control.
 */
static int device_prepare(struct fpc1020_data *fpc1020, bool enable)
{
	int rc = 0;

	mutex_lock(&fpc1020->lock);
	if (enable && !fpc1020->prepared) {
		gpiod_set_value_cansleep(fpc1020->rst_gpio, 1);

		rc = vreg_setup(fpc1020, "vcc_spi", true);
		if (rc)
			goto out;

		rc = vreg_setup(fpc1020, "vdd_io", true);
		if (rc)
			goto err_vcc_spi;

		rc = vreg_setup(fpc1020, "vdd_ana", true);
		if (rc)
			goto err_vdd_io;

		usleep_range(PWR_ON_SLEEP_MIN_US, PWR_ON_SLEEP_MAX_US);

		gpiod_set_value_cansleep(fpc1020->rst_gpio, 0);
		fpc1020->prepared = true;
	} else if (!enable && fpc1020->prepared) {
		gpiod_set_value_cansleep(fpc1020->rst_gpio, 1);
		usleep_range(PWR_ON_SLEEP_MIN_US, PWR_ON_SLEEP_MAX_US);

		vreg_setup(fpc1020, "vdd_ana", false);
		vreg_setup(fpc1020, "vdd_io", false);
		vreg_setup(fpc1020, "vcc_spi", false);
		fpc1020->prepared = false;
	}
	goto out;

err_vdd_io:
	vreg_setup(fpc1020, "vdd_io", false);
err_vcc_spi:
	vreg_setup(fpc1020, "vcc_spi", false);
out:
	mutex_unlock(&fpc1020->lock);

	return rc;
}

static ssize_t device_prepare_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);
	int rc;

	if (!strncmp(buf, "enable", strlen("enable")))
		rc = device_prepare(fpc1020, true);
	else if (!strncmp(buf, "disable", strlen("disable")))
		rc = device_prepare(fpc1020, false);
	else
		return -EINVAL;

	return rc ? rc : count;
}
static DEVICE_ATTR_WO(device_prepare);

/* Whether a sensor interrupt may wake the platform (touch-to-wake). */
static ssize_t wakeup_enable_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);

	if (!strncmp(buf, "enable", strlen("enable")))
		atomic_set(&fpc1020->wakeup_enabled, 1);
	else if (!strncmp(buf, "disable", strlen("disable")))
		atomic_set(&fpc1020->wakeup_enabled, 0);
	else
		return -EINVAL;

	return count;
}
static DEVICE_ATTR_WO(wakeup_enable);

/*
 * Reads the level of the IRQ line. The IRQ handler sysfs_notify()s this
 * node so userspace can poll() it. Writes are accepted and ignored.
 */
static ssize_t irq_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%i\n",
			  gpiod_get_value_cansleep(fpc1020->irq_gpio));
}

static ssize_t irq_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	return count;
}
static DEVICE_ATTR_RW(irq);

static struct attribute *fpc1020_attrs[] = {
	&dev_attr_pinctl_set.attr,
	&dev_attr_device_prepare.attr,
	&dev_attr_regulator_enable.attr,
	&dev_attr_hw_reset.attr,
	&dev_attr_wakeup_enable.attr,
	&dev_attr_clk_enable.attr,
	&dev_attr_irq.attr,
	NULL
};
ATTRIBUTE_GROUPS(fpc1020);

static irqreturn_t fpc1020_irq_handler(int irq, void *handle)
{
	struct fpc1020_data *fpc1020 = handle;

	if (atomic_read(&fpc1020->wakeup_enabled))
		__pm_wakeup_event(fpc1020->ttw_ws, FPC_TTW_HOLD_TIME_MS);

	sysfs_notify(&fpc1020->dev->kobj, NULL, dev_attr_irq.attr.name);

	return IRQ_HANDLED;
}

static void fpc1020_unregister_ws(void *data)
{
	wakeup_source_unregister(data);
}

static void fpc1020_disable_irq_wake(void *data)
{
	struct fpc1020_data *fpc1020 = data;

	disable_irq_wake(fpc1020->irq);
}

static int fpc1020_get_regulators(struct fpc1020_data *fpc1020)
{
	struct device *dev = fpc1020->dev;
	struct regulator *vreg;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(fpc1020_vregs); i++) {
		vreg = devm_regulator_get_optional(dev, fpc1020_vregs[i].name);
		if (IS_ERR(vreg)) {
			if (PTR_ERR(vreg) != -ENODEV)
				return dev_err_probe(dev, PTR_ERR(vreg),
						     "failed to get %s\n",
						     fpc1020_vregs[i].name);
			vreg = NULL;
		}
		fpc1020->vreg[i] = vreg;
	}

	return 0;
}

static int fpc1020_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fpc1020_data *fpc1020;
	int rc;

	fpc1020 = devm_kzalloc(dev, sizeof(*fpc1020), GFP_KERNEL);
	if (!fpc1020)
		return -ENOMEM;

	fpc1020->dev = dev;
	platform_set_drvdata(pdev, fpc1020);
	atomic_set(&fpc1020->wakeup_enabled, 0);

	rc = devm_mutex_init(dev, &fpc1020->lock);
	if (rc)
		return rc;

	fpc1020->irq_gpio = devm_gpiod_get(dev, "irq", GPIOD_IN);
	if (IS_ERR(fpc1020->irq_gpio))
		return dev_err_probe(dev, PTR_ERR(fpc1020->irq_gpio),
				     "failed to get irq gpio\n");

	/* Hold the sensor in reset until it is powered */
	fpc1020->rst_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(fpc1020->rst_gpio))
		return dev_err_probe(dev, PTR_ERR(fpc1020->rst_gpio),
				     "failed to get reset gpio\n");

	rc = fpc1020_get_regulators(fpc1020);
	if (rc)
		return rc;

	fpc1020->irq = gpiod_to_irq(fpc1020->irq_gpio);
	if (fpc1020->irq < 0)
		return dev_err_probe(dev, fpc1020->irq, "no irq for irq gpio\n");

	/* Registered before the IRQ so devm frees it only after the IRQ. */
	fpc1020->ttw_ws = wakeup_source_register(dev, "fpc_ttw_ws");
	if (!fpc1020->ttw_ws)
		return -ENOMEM;

	rc = devm_add_action_or_reset(dev, fpc1020_unregister_ws,
				      fpc1020->ttw_ws);
	if (rc)
		return rc;

	if (device_property_read_bool(dev, "wakeup-source"))
		device_init_wakeup(dev, true);

	rc = devm_request_threaded_irq(dev, fpc1020->irq, NULL,
				       fpc1020_irq_handler,
				       IRQF_TRIGGER_RISING | IRQF_ONESHOT,
				       dev_name(dev), fpc1020);
	if (rc)
		return dev_err_probe(dev, rc, "could not request irq %d\n",
				     fpc1020->irq);

	/*
	 * The IRQ is always armed for wakeup, like downstream; whether it
	 * actually keeps the system awake is gated by "wakeup_enable".
	 */
	rc = enable_irq_wake(fpc1020->irq);
	if (!rc) {
		rc = devm_add_action_or_reset(dev, fpc1020_disable_irq_wake,
					      fpc1020);
		if (rc)
			return rc;
	}

	if (device_property_read_bool(dev, "fpc,enable-on-boot")) {
		dev_info(dev, "Enabling hardware\n");
		device_prepare(fpc1020, true);
	}

	mutex_lock(&fpc1020->lock);
	rc = hw_reset(fpc1020);
	mutex_unlock(&fpc1020->lock);
	if (rc)
		return rc;

	dev_info(dev, "%s: ok\n", __func__);

	return 0;
}

static void fpc1020_remove(struct platform_device *pdev)
{
	struct fpc1020_data *fpc1020 = platform_get_drvdata(pdev);

	device_prepare(fpc1020, false);
}

static const struct of_device_id fpc1020_of_match[] = {
	{ .compatible = "fpc,fpc1020" },
	{ }
};
MODULE_DEVICE_TABLE(of, fpc1020_of_match);

static struct platform_driver fpc1020_driver = {
	.driver = {
		.name = "fpc1020",
		.of_match_table = fpc1020_of_match,
		.dev_groups = fpc1020_groups,
	},
	.probe = fpc1020_probe,
	.remove = fpc1020_remove,
};
module_platform_driver(fpc1020_driver);

MODULE_AUTHOR("Aleksej Makarov");
MODULE_AUTHOR("Henrik Tillman <henrik.tillman@fingerprints.com>");
MODULE_DESCRIPTION("FPC1020 fingerprint sensor platform driver");
MODULE_LICENSE("GPL");

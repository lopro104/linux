// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024, Danila Tikhonov <danila@jiaxyga.com>
 */

#include <linux/iio/consumer.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/ktime.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/devm-helpers.h>
#include <linux/workqueue.h>

/* BATT offsets */
#define QG_S2_NORMAL_AVG_V_DATA0_REG	0x80 /* 2-byte 0x80-0x81 */
#define QG_S2_NORMAL_AVG_I_DATA0_REG	0x82 /* 2-byte 0x82-0x83 */
#define QG_LAST_ADC_V_DATA0_REG		0xc0 /* 2-byte 0xc0-0xc1 */
#define QG_LAST_ADC_I_DATA0_REG		0xc2 /* 2-byte 0xc2-0xc3 */

/* SRAM offsets */
#define QG_SDAM_OCV_OFFSET		0x4c /* 4-byte 0x4c-0x4f */
#define QG_SDAM_LEARNED_CAPACITY_OFFSET	0x68 /* 2-byte 0x68-0x69 */

struct qcom_qg_chip {
	struct device *dev;
	struct regmap *regmap;
	unsigned int base;

	struct iio_channel *batt_therm_chan;

	struct nvmem_device *sdam;

	struct power_supply *batt_psy;
	struct power_supply_battery_info *batt_info;

	/* Reported (smoothed) capacity, see qcom_qg_smooth_capacity() */
	struct mutex soc_lock;
	int soc;
	ktime_t soc_time;

	/* Coulomb counter, see qcom_qg_cc_work() */
	struct delayed_work cc_work;
	s64 charge_uah;		/* remaining charge, valid if cc_valid */
	int full_uah;
	ktime_t cc_time;
	bool cc_valid;
};

/* Battery current treated as idle (neither charging nor discharging) */
#define QG_IDLE_CURRENT_UA	10000
/* The reported capacity moves by at most 1% per this interval */
#define QG_SOC_STEP_MS		20000
/* Coulomb counter sampling period while awake */
#define QG_CC_PERIOD_MS		5000
/* A longer gap between samples means we were suspended */
#define QG_CC_SUSPEND_GAP_MS	30000
/* After this long asleep the cell has rested: trust the OCV again */
#define QG_CC_RESYNC_GAP_MS	(10 * 60 * 1000)
/* Assumed battery drain while suspended */
#define QG_CC_SLEEP_CURRENT_UA	10000
/* Below this current the OCV estimate is good enough to correct drift */
#define QG_CC_REST_CURRENT_UA	50000

static int qcom_qg_get_current(struct qcom_qg_chip *chip, u8 offset, int *val)
{
	s16 temp;
	u8 readval[2];
	int ret;

	ret = regmap_bulk_read(chip->regmap, chip->base + offset, readval, 2);
	if (ret) {
		dev_err(chip->dev, "Failed to read current: %d\n", ret);
		return ret;
	}

	temp = (s16)(readval[1] << 8 | readval[0]);
	*val = div_s64((s64)temp * 152588, 1000);

	/*
	 * PSY API expects charging batteries to report a positive current, which is inverted
	 * to what the PMIC reports.
	 */
	*val = -*val;

	return 0;
}

static int qcom_qg_get_voltage(struct qcom_qg_chip *chip, u8 offset, int *val)
{
	int ret, temp;
	u8 readval[2];

	ret = regmap_bulk_read(chip->regmap, chip->base + offset, readval, 2);
	if (ret) {
		dev_err(chip->dev, "Failed to read voltage: %d\n", ret);
		return ret;
	}

	temp = readval[1] << 8 | readval[0];
	*val = div_u64((u64)temp * 194637, 1000);

	return 0;
}

/*
 * Capacity is estimated from the averaged battery voltage. If the battery
 * node provides OCV-capacity tables (ocv-capacity-celsius /
 * ocv-capacity-table-N), look the voltage up in the table closest to the
 * current battery temperature; Li-ion voltage is far from linear in state
 * of charge, so this is much more accurate at the low end. Otherwise fall
 * back to a linear interpolation between the design min and max voltages.
 */
static int qcom_qg_get_ocv_capacity(struct qcom_qg_chip *chip, int *val)
{
	int ret, voltage_now, temp;
	int voltage_min = chip->batt_info->voltage_min_design_uv;
	int voltage_max = chip->batt_info->voltage_max_design_uv;

	/*
	 * Use the last ADC sample: the S2 averages only update when the gauge
	 * completes a FIFO, which with this driver's default configuration is
	 * rare, so they go stale for long stretches.
	 */
	ret = qcom_qg_get_voltage(chip,
				QG_LAST_ADC_V_DATA0_REG, &voltage_now);
	if (ret) {
		dev_err(chip->dev, "Failed to get current voltage: %d\n", ret);
		return ret;
	}

	if (chip->batt_info->ocv_table_size[0] > 0) {
		int ocv = voltage_now, current_avg, ri;

		/* millidegC -> degC; assume room temperature if unreadable */
		if (iio_read_channel_processed(chip->batt_therm_chan, &temp) < 0)
			temp = 25000;

		/*
		 * The OCV tables are for a battery at rest; under load or while
		 * charging the terminal voltage is off by I * R_internal, which
		 * made the reported capacity jump by 10-25%. Estimate the open
		 * circuit voltage from the current (positive while charging)
		 * when the battery's internal resistance is known.
		 */
		if (!qcom_qg_get_current(chip, QG_LAST_ADC_I_DATA0_REG,
					 &current_avg)) {
			ri = power_supply_vbat2ri(chip->batt_info, voltage_now,
						  current_avg > 0);
			if (ri > 0)
				ocv -= div_s64((s64)current_avg * ri, 1000000);
		}

		ret = power_supply_batinfo_ocv2cap(chip->batt_info, ocv,
						   temp / 1000);
		if (ret >= 0) {
			*val = clamp(ret, 0, 100);
			return 0;
		}
	}

	if (voltage_now <= voltage_min)
		*val = 0;
	else if (voltage_now >= voltage_max)
		*val = 100;
	else
		*val = (((voltage_now - voltage_min) * 100) /
						(voltage_max - voltage_min));

	return 0;
}

/*
 * The voltage-based estimate is only right for a resting cell, so track the
 * remaining charge by integrating the measured current while awake, the way
 * the downstream QG driver does with the gauge's own accumulators. Start
 * (and restart after a long suspend, once the cell has rested) from the OCV
 * estimate, and let the count drift slowly towards that estimate whenever
 * the battery is nearly idle so errors cannot pile up.
 */
static void qcom_qg_cc_work(struct work_struct *work)
{
	struct qcom_qg_chip *chip = container_of(to_delayed_work(work),
						 struct qcom_qg_chip, cc_work);
	ktime_t now = ktime_get_boottime();
	int ocv_soc, current_ua, voltage_uv;
	s64 dt_ms, ocv_uah;

	if (qcom_qg_get_ocv_capacity(chip, &ocv_soc) ||
	    qcom_qg_get_current(chip, QG_LAST_ADC_I_DATA0_REG, &current_ua) ||
	    qcom_qg_get_voltage(chip, QG_LAST_ADC_V_DATA0_REG, &voltage_uv))
		goto out;

	ocv_uah = div_s64((s64)ocv_soc * chip->full_uah, 100);
	dt_ms = ktime_ms_delta(now, chip->cc_time);

	if (!chip->cc_valid || dt_ms > QG_CC_RESYNC_GAP_MS) {
		chip->charge_uah = ocv_uah;
		chip->cc_valid = true;
	} else if (dt_ms > QG_CC_SUSPEND_GAP_MS) {
		chip->charge_uah -= div_s64((s64)QG_CC_SLEEP_CURRENT_UA * dt_ms,
					    3600000);
	} else {
		chip->charge_uah += div_s64((s64)current_ua * dt_ms, 3600000);
		if (abs(current_ua) < QG_CC_REST_CURRENT_UA)
			chip->charge_uah += div_s64(ocv_uah - chip->charge_uah,
						    64);
	}

	/* Charge termination: supplied, at the float voltage, tiny current */
	if (power_supply_am_i_supplied(chip->batt_psy) > 0 &&
	    voltage_uv > chip->batt_info->voltage_max_design_uv - 50000 &&
	    current_ua >= 0 && current_ua < QG_CC_REST_CURRENT_UA)
		chip->charge_uah = chip->full_uah;

	chip->charge_uah = clamp_t(s64, chip->charge_uah, 0, chip->full_uah);
	chip->cc_time = now;
out:
	schedule_delayed_work(&chip->cc_work,
			      msecs_to_jiffies(QG_CC_PERIOD_MS));
}

static int qcom_qg_get_capacity(struct qcom_qg_chip *chip, int *val)
{
	if (!chip->cc_valid)
		return qcom_qg_get_ocv_capacity(chip, val);

	*val = DIV_ROUND_CLOSEST_ULL((u64)chip->charge_uah * 100,
				     chip->full_uah);
	return 0;
}

/*
 * Like Google's battery driver does for the UI SOC: let the reported
 * capacity follow the instantaneous estimate by at most 1% per
 * QG_SOC_STEP_MS, never rising while discharging nor falling while
 * charging. Steps accumulate while nobody asks (e.g. in suspend), so the
 * value catches up afterwards.
 */
static int qcom_qg_smooth_capacity(struct qcom_qg_chip *chip, int raw,
				   int current_ua)
{
	ktime_t now = ktime_get_boottime();
	s64 steps;
	int diff, soc;

	guard(mutex)(&chip->soc_lock);

	if (chip->soc < 0) {
		chip->soc = raw;
		chip->soc_time = now;
		return raw;
	}

	steps = ktime_ms_delta(now, chip->soc_time) / QG_SOC_STEP_MS;
	diff = raw - chip->soc;

	if ((diff > 0 && current_ua < -QG_IDLE_CURRENT_UA) ||
	    (diff < 0 && current_ua > QG_IDLE_CURRENT_UA) || !diff) {
		/* Not allowed to move this way now: don't bank the time */
		chip->soc_time = now;
		return chip->soc;
	}

	if (!steps)
		return chip->soc;

	soc = chip->soc + clamp_t(s64, diff, -steps, steps);
	chip->soc_time = ktime_add_ms(chip->soc_time,
				      steps * QG_SOC_STEP_MS);
	chip->soc = soc;

	return soc;
}

static int qcom_qg_get_status(struct qcom_qg_chip *chip, int *val)
{
	int ret, current_ua, soc;

	ret = qcom_qg_get_current(chip, QG_LAST_ADC_I_DATA0_REG, &current_ua);
	if (ret)
		return ret;

	if (current_ua > QG_IDLE_CURRENT_UA) {
		*val = POWER_SUPPLY_STATUS_CHARGING;
	} else if (power_supply_am_i_supplied(chip->batt_psy) > 0 &&
		   current_ua > -QG_IDLE_CURRENT_UA) {
		scoped_guard(mutex, &chip->soc_lock)
			soc = chip->soc;
		*val = soc >= 100 ? POWER_SUPPLY_STATUS_FULL :
				    POWER_SUPPLY_STATUS_NOT_CHARGING;
	} else {
		*val = POWER_SUPPLY_STATUS_DISCHARGING;
	}

	return 0;
}

static enum power_supply_property qcom_qg_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_AVG,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TEMP,
};

static int qcom_qg_get_property(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct qcom_qg_chip *chip = power_supply_get_drvdata(psy);
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		ret = qcom_qg_get_status(chip, &val->intval);
		if (ret)
			return ret;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = chip->batt_info->voltage_max_design_uv;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		val->intval = chip->batt_info->voltage_min_design_uv;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = qcom_qg_get_voltage(chip,
				QG_LAST_ADC_V_DATA0_REG, &val->intval);
		if (ret)
			return ret;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_AVG:
		ret = qcom_qg_get_voltage(chip,
				QG_S2_NORMAL_AVG_V_DATA0_REG, &val->intval);
		if (ret)
			return ret;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		ret = nvmem_device_read(chip->sdam, QG_SDAM_OCV_OFFSET, 4, &val->intval);
		if (ret < 0)
			return ret;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = qcom_qg_get_current(chip,
				QG_LAST_ADC_I_DATA0_REG, &val->intval);
		if (ret)
			return ret;
		break;
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		ret = qcom_qg_get_current(chip,
				QG_S2_NORMAL_AVG_I_DATA0_REG, &val->intval);
		if (ret)
			return ret;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = chip->batt_info->charge_full_design_uah;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		ret = nvmem_device_read(chip->sdam,
				QG_SDAM_LEARNED_CAPACITY_OFFSET, 2, &val->intval);
		if (ret < 0)
			return ret;
		val->intval *= 1000; /* mAh to uAh */
		break;
	case POWER_SUPPLY_PROP_CAPACITY: {
		int raw, current_ua;

		ret = qcom_qg_get_capacity(chip, &raw);
		if (ret)
			return ret;
		ret = qcom_qg_get_current(chip, QG_LAST_ADC_I_DATA0_REG,
					  &current_ua);
		if (ret)
			return ret;
		val->intval = qcom_qg_smooth_capacity(chip, raw, current_ua);
		break;
	}
	case POWER_SUPPLY_PROP_TEMP:
		ret = iio_read_channel_processed
					(chip->batt_therm_chan, &val->intval);
		if (ret < 0)
			return ret;
		val->intval /= 100; /* 1/1000 °C (millidegC) to 1/10 °C */
		break;
	default:
		dev_err(chip->dev, "invalid property: %d\n", psp);
		return -EINVAL;
	}
	return 0;
}

static struct power_supply_desc batt_psy_desc = {
	.name = "qcom_qg",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = qcom_qg_props,
	.num_properties = ARRAY_SIZE(qcom_qg_props),
	.get_property = qcom_qg_get_property,
};

static int qcom_qg_probe(struct platform_device *pdev)
{
	struct qcom_qg_chip *chip;
	struct power_supply_config psy_cfg = {};
	int ret;

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &pdev->dev;
	chip->soc = -1;
	ret = devm_mutex_init(&pdev->dev, &chip->soc_lock);
	if (ret)
		return ret;

	/* Regmap */
	chip->regmap = dev_get_regmap(chip->dev->parent, NULL);
	if (!chip->regmap)
		return dev_err_probe(chip->dev, -ENODEV,
				     "Failed to locate the regmap\n");

	/* Get base address */
	ret = device_property_read_u32(chip->dev, "reg", &chip->base);
	if (ret < 0)
		return dev_err_probe(chip->dev, ret,
				     "Couldn't read base address\n");

	/* ADC for thermal channel */
	chip->batt_therm_chan = devm_iio_channel_get(chip->dev, "batt-therm");
	if (IS_ERR(chip->batt_therm_chan))
		return dev_err_probe(chip->dev, PTR_ERR(chip->batt_therm_chan),
				     "Couldn't get batt-therm IIO channel\n");

	/* NVMEM for SDAM access */
	chip->sdam = devm_nvmem_device_get(chip->dev, NULL);
	if (IS_ERR(chip->sdam))
		return dev_err_probe(chip->dev, PTR_ERR(chip->sdam),
				     "Couldn't get SDAM nvmem device\n");

	psy_cfg.drv_data = chip;
	psy_cfg.fwnode = dev_fwnode(chip->dev);

	/* Power supply */
	chip->batt_psy =
		devm_power_supply_register(chip->dev, &batt_psy_desc, &psy_cfg);
	if (IS_ERR(chip->batt_psy))
		return dev_err_probe(chip->dev, PTR_ERR(chip->batt_psy),
				     "Failed to register power supply\n");

	/* Battery info */
	ret = power_supply_get_battery_info(chip->batt_psy, &chip->batt_info);
	if (ret)
		return dev_err_probe(chip->dev, ret,
				     "Failed to get battery info\n");

	chip->full_uah = chip->batt_info->charge_full_design_uah;
	if (chip->full_uah > 0) {
		ret = devm_delayed_work_autocancel(chip->dev, &chip->cc_work,
						   qcom_qg_cc_work);
		if (ret)
			return ret;
		schedule_delayed_work(&chip->cc_work, 0);
	}

	platform_set_drvdata(pdev, chip);

	return 0;
}

static const struct of_device_id qcom_qg_of_match[] = {
	{ .compatible = "qcom,pm6150-qg", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, qcom_qg_of_match);

static struct platform_driver qcom_qg_driver = {
	.driver = {
		.name = "qcom,qcom_qg",
		.of_match_table = qcom_qg_of_match,
	},
	.probe = qcom_qg_probe,
};

module_platform_driver(qcom_qg_driver);

MODULE_AUTHOR("Danila Tikhonov <danila@jiaxyga.com>");
MODULE_DESCRIPTION("Qualcomm PMIC QGauge (QG) driver");
MODULE_LICENSE("GPL");

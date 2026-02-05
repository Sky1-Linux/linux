// SPDX-License-Identifier: GPL-2.0
/*
 * CIX Sky1 fan control driver
 *
 * Copyright (C) 2024 CIX Technology Group Co., Ltd.
 *
 * Fan control via ACPI methods on \_SB.HWMN device (CIXHA024):
 *   SFMT - Set mute mode (fan off)
 *   SFAT - Set auto mode (thermal controlled)
 *   SFPF - Set performance mode (max speed)
 *
 * These methods delegate to \_SB.EC0 embedded controller.
 */

#include <linux/acpi.h>
#include <linux/hwmon.h>
#include <linux/module.h>
#include <linux/platform_device.h>

/* Fan mode maps to pwm_enable values */
enum cix_fan_mode {
	CIX_FAN_MODE_OFF = 0,	/* SFMT - mute/off */
	CIX_FAN_MODE_FULL = 1,	/* SFPF - performance/full speed */
	CIX_FAN_MODE_AUTO = 2,	/* SFAT - auto/thermal controlled */
};

struct cix_fan_data {
	struct device *dev;
	acpi_handle handle;
	struct mutex lock;
	int pwm_enable;		/* 0=off, 1=full, 2=auto */
};

static int cix_fan_set_mode(struct cix_fan_data *data, int mode)
{
	const char *method;
	acpi_status status;

	switch (mode) {
	case CIX_FAN_MODE_OFF:
		method = "SFMT";
		break;
	case CIX_FAN_MODE_FULL:
		method = "SFPF";
		break;
	case CIX_FAN_MODE_AUTO:
		method = "SFAT";
		break;
	default:
		return -EINVAL;
	}

	status = acpi_evaluate_object(data->handle, (char *)method, NULL, NULL);
	if (ACPI_FAILURE(status)) {
		dev_dbg(data->dev, "ACPI method %s failed: %s\n",
			method, acpi_format_exception(status));
		return -EIO;
	}

	data->pwm_enable = mode;
	return 0;
}

static int cix_fan_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
{
	struct cix_fan_data *data = dev_get_drvdata(dev);

	if (type != hwmon_pwm || attr != hwmon_pwm_enable)
		return -EOPNOTSUPP;

	mutex_lock(&data->lock);
	*val = data->pwm_enable;
	mutex_unlock(&data->lock);

	return 0;
}

static int cix_fan_write(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long val)
{
	struct cix_fan_data *data = dev_get_drvdata(dev);
	int ret;

	if (type != hwmon_pwm || attr != hwmon_pwm_enable)
		return -EOPNOTSUPP;

	if (val < 0 || val > 2)
		return -EINVAL;

	mutex_lock(&data->lock);
	ret = cix_fan_set_mode(data, val);
	mutex_unlock(&data->lock);

	return ret;
}

static umode_t cix_fan_is_visible(const void *drvdata,
				  enum hwmon_sensor_types type,
				  u32 attr, int channel)
{
	if (type == hwmon_pwm && attr == hwmon_pwm_enable)
		return 0644;

	return 0;
}

static const struct hwmon_channel_info * const cix_fan_info[] = {
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_ENABLE),
	NULL
};

static const struct hwmon_ops cix_fan_hwmon_ops = {
	.is_visible = cix_fan_is_visible,
	.read = cix_fan_read,
	.write = cix_fan_write,
};

static const struct hwmon_chip_info cix_fan_chip_info = {
	.ops = &cix_fan_hwmon_ops,
	.info = cix_fan_info,
};

static int cix_fan_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cix_fan_data *data;
	struct device *hwmon_dev;
	acpi_handle handle;

	handle = ACPI_HANDLE(dev);
	if (!handle)
		return -ENODEV;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = dev;
	data->handle = handle;
	data->pwm_enable = CIX_FAN_MODE_AUTO;
	mutex_init(&data->lock);

	hwmon_dev = devm_hwmon_device_register_with_info(dev, "cix_fan", data,
							 &cix_fan_chip_info,
							 NULL);
	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);

	dev_info(dev, "CIX fan control initialized\n");
	return 0;
}

static const struct acpi_device_id cix_fan_acpi_match[] = {
	{ "CIXHA024", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, cix_fan_acpi_match);

static struct platform_driver cix_fan_driver = {
	.probe = cix_fan_probe,
	.driver = {
		.name = "cix-fan",
		.acpi_match_table = cix_fan_acpi_match,
	},
};
module_platform_driver(cix_fan_driver);

MODULE_AUTHOR("CIX Technology Group Co., Ltd.");
MODULE_DESCRIPTION("CIX Sky1 fan control driver");
MODULE_LICENSE("GPL");

/*
 * Device Property Register Device Driver
 *
 * Copyright (C) 2014 Cumulus Networks, Inc.
 *
 * Author: Dustin Byford <dustin@cumulusnetworks.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/acpi.h>
#include <linux/property.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/swc/register.h>
#include <linux/regmap.h>
#include <linux/platform_device.h>
#include <linux/nls.h>
#include "swc-fw-util.h"

MODULE_AUTHOR("Dustin Byford <dustin@cumulusnetworks.com>");
MODULE_DESCRIPTION("Firmware Defined Fan Device Driver");
MODULE_LICENSE("GPL v2");

#define SWC_CPLD_REGISTER_MAX_OFFSETS 8
struct swc_fan_data {
	struct device *dev;
	struct device *hwmon;

	struct device *pwm;
	int pwm_offset;
	int pwm_min;
	int pwm_max;

	struct device *speed;
	int speed_offset;
	int speed_scale;
	int speed_min;
	int speed_max;

	struct gpio_desc *alarm;
	struct gpio_desc *present;
	bool is_present;

	int num_attrs;
	// pwm1{,_label} fan1_{input,alarm,min,max,label} + NULL
	struct attribute *swc_fan_attrs[16];
	struct attribute_group swc_fan_attr_group;
	// swc_fan_attrs + NULL
	struct attribute_group *swc_fan_groups[2];
};

static const struct platform_device_id swc_fan_ids[] = {
	{ "swc-fan", 0 },
	{ }
};
MODULE_DEVICE_TABLE(platform, swc_fan_ids);

static const struct of_device_id swc_fan_of_match[] = {
	{ .compatible = "swc-fan", },
	{ },
};
MODULE_DEVICE_TABLE(of, swc_fan_of_match);

static inline int pwm_to_state(int min, int max, int pwm)
{
	int state = (pwm * max) / 255;
	return state < min ? min : state;
}

static inline int state_to_pwm(int min, int max, int state)
{
	return (255 * state) / max;
}

static ssize_t set_pwm(struct device *dev, struct device_attribute *attr,
		       const char *buf, size_t count)
{
	struct swc_fan_data *data = dev_get_drvdata(dev);
	unsigned long pwm;
	int state;
	int err;

	if (kstrtoul(buf, 0, &pwm))
		return -EINVAL;

	state = pwm_to_state(data->pwm_min, data->pwm_max, pwm);
	err = swc_cpld_register_set(data->pwm, data->pwm_offset, state);
	if (err) {
		dev_err(dev, "failed to set fan pwm\n");
		return -EFAULT;
	}

	return count;
}

static ssize_t show_pwm(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct swc_fan_data *data = dev_get_drvdata(dev);
	int state;
	int pwm;
	int err;

	err = swc_cpld_register_get(data->pwm, data->pwm_offset, &state);
	if (err) {
		dev_err(dev, "failed to get fan pwm\n");
		return -EFAULT;
	}

	pwm = state_to_pwm(data->pwm_min, data->pwm_max, state);
	return sprintf(buf, "%u\n", pwm);
}

static SENSOR_DEVICE_ATTR(pwm1, S_IRUGO | S_IWUSR, show_pwm, set_pwm, 0);

static ssize_t show_rpm_min(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct swc_fan_data *data = dev_get_drvdata(dev);
	return sprintf(buf, "%d\n", data->speed_min);
}
static SENSOR_DEVICE_ATTR(fan1_min, S_IRUGO, show_rpm_min, NULL, 0);

static ssize_t show_rpm_max(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct swc_fan_data *data = dev_get_drvdata(dev);
	return sprintf(buf, "%d\n", data->speed_max);
}
static SENSOR_DEVICE_ATTR(fan1_max, S_IRUGO, show_rpm_max, NULL, 0);

static ssize_t show_rpm(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct swc_fan_data *data = dev_get_drvdata(dev);
	int val;
	int err;

	err = swc_cpld_register_get(data->speed, data->speed_offset, &val);
	if (err) {
		dev_err(dev, "failed to get fan rpm\n");
		return -EFAULT;
	}

	return sprintf(buf, "%d\n", val * data->speed_scale);
}
static SENSOR_DEVICE_ATTR(fan1_input, S_IRUGO, show_rpm, NULL, 0);

static ssize_t show_fan_alarm(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct swc_fan_data *data = dev_get_drvdata(dev);
	return sprintf(buf, "%d\n", gpiod_get_value(data->alarm));
}
static SENSOR_DEVICE_ATTR(fan1_alarm, S_IRUGO, show_fan_alarm, NULL, 0);

static ssize_t show_label(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct swc_fan_data *data = dev_get_drvdata(dev);
        struct acpi_device *adev = ACPI_COMPANION(data->dev);
        int result;

	if (!adev) {
		dev_err(dev, "no ACPI companion\n");
		return 0;
	}
                                         
	if (!adev->pnp.str_obj) {
		dev_err(dev, "no _STR\n");
		return 0;
	}
                                         
        result = utf16s_to_utf8s(
                (wchar_t *)adev->pnp.str_obj->buffer.pointer,
                adev->pnp.str_obj->buffer.length,
                UTF16_LITTLE_ENDIAN, buf,
                PAGE_SIZE);

	if (strcmp(attr->attr.name, "pwm1_label") == 0)
		result += sprintf(&buf[result], " (PWM)");
	else if (strcmp(attr->attr.name, "fan1_label") == 0)
		result += sprintf(&buf[result], " speed (RPM)");
                
        buf[result++] = '\n';

	return result;
}
static SENSOR_DEVICE_ATTR(pwm1_label, S_IRUGO, show_label, NULL, 0);
static SENSOR_DEVICE_ATTR(fan1_label, S_IRUGO, show_label, NULL, 0);

static void swc_fan_add_attr(struct swc_fan_data *data,
			     struct attribute *attr)
{
	if (data->num_attrs >= ARRAY_SIZE(data->swc_fan_attrs)) {
		pr_err("attr array too small\n");
		return;
	}

	data->swc_fan_attrs[data->num_attrs++] = attr;
}

static int swc_fan_register_hwmon(struct device *dev)
{
	struct swc_fan_data *data = dev_get_drvdata(dev);

	if (data->pwm) {
		swc_fan_add_attr(data, &sensor_dev_attr_pwm1.dev_attr.attr);
		if (ACPI_COMPANION(dev)->pnp.str_obj)
			swc_fan_add_attr(data, &sensor_dev_attr_pwm1_label.dev_attr.attr);
	}
	if (data->speed) {
		swc_fan_add_attr(data, &sensor_dev_attr_fan1_input.dev_attr.attr);
		if (ACPI_COMPANION(dev)->pnp.str_obj)
			swc_fan_add_attr(data, &sensor_dev_attr_fan1_label.dev_attr.attr);
	}
	if (data->speed_min != -1) {
		swc_fan_add_attr(data, &sensor_dev_attr_fan1_min.dev_attr.attr);
	}
	if (data->speed_max != -1) {
		swc_fan_add_attr(data, &sensor_dev_attr_fan1_max.dev_attr.attr);
	}
	if (data->alarm) {
		swc_fan_add_attr(data, &sensor_dev_attr_fan1_alarm.dev_attr.attr);
	}

	data->swc_fan_attr_group.attrs = data->swc_fan_attrs;
	data->swc_fan_groups[0] = &data->swc_fan_attr_group;

	if (data->num_attrs) {
		dev_info(dev, "registering hwmon with %d attrs\n", data->num_attrs);

		data->hwmon = hwmon_device_register_with_groups(
			dev, dev_name(dev), data,
			(const struct attribute_group **)&data->swc_fan_groups);
		if (IS_ERR(data->hwmon)) {
			dev_err(dev, "Failed to register hwmon device\n");
			return PTR_ERR(data->hwmon);
		}
	}

	return 0;
}

static int swc_fan_probe(struct platform_device *pdev)
{
	struct swc_fan_data *data;
	int err;

	dev_info(&pdev->dev, "swc_fan_probe()\n");

	data = devm_kzalloc(&pdev->dev, sizeof *data, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	if (device_property_present(&pdev->dev, "disabled"))
		return -ENODEV;

	data->pwm = swc_fw_util_get_ref_physical(&pdev->dev, "pwm");
	if (PTR_ERR(data->pwm) == -ENODEV) {
		return -EPROBE_DEFER;
	} else if (IS_ERR(data->pwm)) {
		data->pwm = NULL;
	} else {
		struct acpi_reference_args ref;
		u32 range[2];

		err = acpi_dev_get_property_reference(ACPI_COMPANION(&pdev->dev),
						      "pwm", 0, &ref);
		if (err) {
			dev_err(&pdev->dev, "failed to get pwm device\n");
			return -EINVAL;
		}
		if (ref.nargs > 1) {
			dev_err(&pdev->dev, "too many args to 'pwm'\n");
			return -EINVAL;
		}

		if (ref.nargs == 0)
			data->pwm_offset = 0;
		else
			data->pwm_offset = ref.args[0];

		err = device_property_read_u32_array(&pdev->dev, "pwm-range",
						     range, 2);
		if (err) {
			dev_err(&pdev->dev, "failed to get pwm-range\n");
			return -EINVAL;
		}
		data->pwm_min = range[0];
		data->pwm_max = range[1];
	}

	data->speed_min = -1;
	data->speed_max = -1;
	data->speed = swc_fw_util_get_ref_physical(&pdev->dev, "speed");
	if (PTR_ERR(data->speed) == -ENODEV) {
		return -EPROBE_DEFER;
	} else if (IS_ERR(data->speed)) {
		data->speed = NULL;
	} else {
		struct acpi_reference_args ref;
		u32 range[2];
		u32 scale;

		err = acpi_dev_get_property_reference(ACPI_COMPANION(&pdev->dev),
						      "speed", 0, &ref);
		if (err) {
			dev_err(&pdev->dev, "failed to get speed device\n");
			return -EINVAL;
		}
		if (ref.nargs > 1) {
			dev_err(&pdev->dev, "too many args to 'speed'\n");
			return -EINVAL;
		}
		if (ref.nargs == 0)
			data->speed_offset = 0;
		else
			data->speed_offset = ref.args[0];

		err = device_property_read_u32_array(&pdev->dev, "speed-range",
						     range, 2);
		if (!err) {
			data->speed_min = range[0];
			data->speed_max = range[1];
		}

		err = device_property_read_u32(&pdev->dev, "speed-scale",
					       &scale);
		if (err)
			data->speed_scale = 1;
		else
			data->speed_scale = scale;
	}

	data->alarm = devm_gpiod_get(&pdev->dev, "alarm");
	if (PTR_ERR(data->alarm) == -ENODEV)
		return -EPROBE_DEFER;
	else if (IS_ERR(data->alarm))
		data->alarm = NULL;
	else
		dev_info(&pdev->dev, "using alarm gpio\n");

	data->present = devm_gpiod_get(&pdev->dev, "present");
	if (PTR_ERR(data->present) == -ENODEV)
		return -EPROBE_DEFER;
	else if (IS_ERR(data->present))
		data->present = NULL;
	else
		dev_info(&pdev->dev, "using presence gpio\n");

	data->dev = &pdev->dev;
	data->pwm = get_device(data->pwm);
	data->speed = get_device(data->speed);

	platform_set_drvdata(pdev, data);

	if (data->present) {
		dev_info(&pdev->dev, "fan is modular\n");
		data->is_present = gpiod_get_value(data->present);
	} else {
		data->is_present = true;
	}

	if (data->is_present) {
		err = swc_fan_register_hwmon(&pdev->dev);
		if (err) {
			dev_err(&pdev->dev, "failed to register hwmon\n");
			goto fail;
		}
	}
	
	// XXX - setup presence poll or interrupt

	dev_info(&pdev->dev, "added fan\n");
	return 0;

fail:
	put_device(data->pwm);
	put_device(data->speed);
	return err;
}

static int swc_fan_remove(struct platform_device *pdev)
{
	struct swc_fan_data *data = platform_get_drvdata(pdev);

	if (data->hwmon)
		hwmon_device_unregister(data->hwmon);

	put_device(data->pwm);
	put_device(data->speed);

	dev_info(&pdev->dev, "removed\n");

	return 0;
}

static struct platform_driver swc_fan_driver = {
	.driver = {
		.name		= "swc-fan",
		.owner		= THIS_MODULE,
		.of_match_table = swc_fan_of_match,
	},
	.probe = swc_fan_probe,
	.remove = swc_fan_remove,
	.id_table = swc_fan_ids,
};
module_platform_driver(swc_fan_driver);

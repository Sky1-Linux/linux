// SPDX-License-Identifier: GPL-2.0
/*
 * CIX Bus Performance State Control driver
 *
 * Minimal driver for controlling CI700/NI700 bus frequency via SCMI
 * performance domains. Provides sysfs interface for testing bus DVFS.
 *
 * Copyright 2024 Cix Technology Group Co., Ltd.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/pm_domain.h>
#include <linux/pm_opp.h>
#include <linux/pm_runtime.h>

struct cix_bus_perf {
	struct device *dev;
	unsigned long cur_freq;
	unsigned long min_freq;
	unsigned long max_freq;
};

static int cix_bus_set_freq(struct device *dev, unsigned long freq)
{
	struct dev_pm_opp *opp;
	unsigned int level;
	int ret;

	opp = dev_pm_opp_find_freq_ceil(dev, &freq);
	if (IS_ERR(opp))
		return PTR_ERR(opp);

	level = dev_pm_opp_get_level(opp);
	dev_pm_opp_put(opp);

	ret = dev_pm_genpd_set_performance_state(dev, level);
	if (ret)
		dev_err(dev, "failed to set perf state %u: %d\n", level, ret);

	return ret;
}

static ssize_t cur_freq_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct cix_bus_perf *bp = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%lu\n", bp->cur_freq);
}

static ssize_t cur_freq_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct cix_bus_perf *bp = dev_get_drvdata(dev);
	unsigned long freq;
	struct dev_pm_opp *opp;
	int ret;

	ret = kstrtoul(buf, 0, &freq);
	if (ret)
		return ret;

	/* Find nearest OPP */
	opp = dev_pm_opp_find_freq_ceil(dev, &freq);
	if (IS_ERR(opp)) {
		freq = bp->max_freq;
		opp = dev_pm_opp_find_freq_floor(dev, &freq);
		if (IS_ERR(opp))
			return PTR_ERR(opp);
	}
	dev_pm_opp_put(opp);

	ret = cix_bus_set_freq(dev, freq);
	if (ret)
		return ret;

	bp->cur_freq = freq;
	dev_info(dev, "Set frequency to %lu Hz\n", freq);
	return count;
}
static DEVICE_ATTR_RW(cur_freq);

static ssize_t available_frequencies_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct dev_pm_opp *opp;
	unsigned long freq = 0;
	int len = 0;

	while (1) {
		opp = dev_pm_opp_find_freq_ceil(dev, &freq);
		if (IS_ERR(opp))
			break;
		len += sysfs_emit_at(buf, len, "%lu ", freq);
		dev_pm_opp_put(opp);
		freq++;
	}

	if (len > 0)
		buf[len - 1] = '\n';

	return len;
}
static DEVICE_ATTR_RO(available_frequencies);

static struct attribute *bus_perf_attrs[] = {
	&dev_attr_cur_freq.attr,
	&dev_attr_available_frequencies.attr,
	NULL,
};
ATTRIBUTE_GROUPS(bus_perf);

static int cix_bus_perf_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cix_bus_perf *bp;
	struct dev_pm_opp *opp;
	unsigned long freq;
	int ret;

	bp = devm_kzalloc(dev, sizeof(*bp), GFP_KERNEL);
	if (!bp)
		return -ENOMEM;

	bp->dev = dev;
	platform_set_drvdata(pdev, bp);

	/* Enable runtime PM to attach to genpd */
	pm_runtime_enable(dev);
	pm_runtime_get_sync(dev);

	/* Check if OPPs were added by genpd attach */
	ret = dev_pm_opp_get_opp_count(dev);
	if (ret <= 0) {
		dev_info(dev, "No OPPs available from power domain\n");
		pm_runtime_put(dev);
		pm_runtime_disable(dev);
		return ret < 0 ? ret : -ENODEV;
	}

	dev_info(dev, "Found %d OPPs\n", ret);

	/* Get frequency range */
	freq = 0;
	opp = dev_pm_opp_find_freq_ceil(dev, &freq);
	if (!IS_ERR(opp)) {
		bp->min_freq = freq;
		dev_pm_opp_put(opp);
	}

	freq = ULONG_MAX;
	opp = dev_pm_opp_find_freq_floor(dev, &freq);
	if (!IS_ERR(opp)) {
		bp->max_freq = freq;
		bp->cur_freq = freq;
		dev_pm_opp_put(opp);
	}

	dev_info(dev, "Frequency range: %lu - %lu Hz\n",
		 bp->min_freq, bp->max_freq);

	return 0;
}

static void cix_bus_perf_remove(struct platform_device *pdev)
{
	pm_runtime_put(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
}

static const struct of_device_id cix_bus_perf_match[] = {
	{ .compatible = "cix,bus-ci700" },
	{ .compatible = "cix,bus-ni700" },
	{},
};
MODULE_DEVICE_TABLE(of, cix_bus_perf_match);

static struct platform_driver cix_bus_perf_driver = {
	.probe = cix_bus_perf_probe,
	.remove = cix_bus_perf_remove,
	.driver = {
		.name = "cix-bus-perf",
		.of_match_table = cix_bus_perf_match,
		.dev_groups = bus_perf_groups,
	},
};
module_platform_driver(cix_bus_perf_driver);

MODULE_DESCRIPTION("CIX Bus Performance State Control Driver");
MODULE_LICENSE("GPL");

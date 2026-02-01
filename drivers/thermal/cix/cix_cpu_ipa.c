// SPDX-License-Identifier: GPL-2.0-only
/*
 * CIX CPU IPA (Intelligent Power Allocation) driver
 *
 * Reads per-CPU power measurements from a memory-mapped region populated
 * by firmware. Provides static (leakage) and dynamic power per CPU.
 *
 * Copyright 2024 Cix Technology Group Co., Ltd.
 */

#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <asm/cpu.h>

#define REG_OFFSET 0x40

struct cpu_ipa_info {
	u32 off_cnt;
	u32 rsvd[13];
	s32 dynamic_power;
	s32 static_power;
};

struct cix_cpu_ipa {
	struct device *dev;
	void __iomem *regs;
};

static struct cix_cpu_ipa *cix_ipa;

static int cix_get_static_power(int cpu)
{
	int pcpu;
	struct cpu_ipa_info __iomem *info;

	if (!cix_ipa || !cix_ipa->regs)
		return 0;

	pcpu = MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 1);
	info = cix_ipa->regs + pcpu * REG_OFFSET;

	return readl(&info->static_power);
}

static int cix_get_dynamic_power(int cpu)
{
	int pcpu;
	struct cpu_ipa_info __iomem *info;

	if (!cix_ipa || !cix_ipa->regs)
		return 0;

	pcpu = MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 1);
	info = cix_ipa->regs + pcpu * REG_OFFSET;

	return readl(&info->dynamic_power);
}

/* Sysfs interface for power monitoring */
static ssize_t cpu_power_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	int cpu;
	int len = 0;
	int total_static = 0, total_dynamic = 0;

	len += sysfs_emit_at(buf, len, "CPU\tStatic(mW)\tDynamic(mW)\n");

	for_each_online_cpu(cpu) {
		int s = cix_get_static_power(cpu);
		int d = cix_get_dynamic_power(cpu);

		len += sysfs_emit_at(buf, len, "%d\t%d\t\t%d\n", cpu, s, d);
		total_static += s;
		total_dynamic += d;
	}

	len += sysfs_emit_at(buf, len, "Total\t%d\t\t%d\n",
			     total_static, total_dynamic);

	return len;
}
static DEVICE_ATTR_RO(cpu_power);

static struct attribute *cpu_ipa_attrs[] = {
	&dev_attr_cpu_power.attr,
	NULL,
};
ATTRIBUTE_GROUPS(cpu_ipa);

static int cpu_ipa_probe(struct platform_device *pdev)
{
	struct cix_cpu_ipa *ipa;

	ipa = devm_kzalloc(&pdev->dev, sizeof(*ipa), GFP_KERNEL);
	if (!ipa)
		return -ENOMEM;

	ipa->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ipa->regs))
		return PTR_ERR(ipa->regs);

	ipa->dev = &pdev->dev;
	platform_set_drvdata(pdev, ipa);
	cix_ipa = ipa;

	dev_info(&pdev->dev, "CPU power monitoring initialized\n");
	return 0;
}

static const struct of_device_id cpu_ipa_of_match[] = {
	{ .compatible = "cix,cpu-ipa" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, cpu_ipa_of_match);

static struct platform_driver cpu_ipa_driver = {
	.probe = cpu_ipa_probe,
	.driver = {
		.name = "cix-cpu-ipa",
		.of_match_table = cpu_ipa_of_match,
		.dev_groups = cpu_ipa_groups,
	},
};
module_platform_driver(cpu_ipa_driver);

MODULE_DESCRIPTION("CIX CPU IPA Power Monitoring Driver");
MODULE_LICENSE("GPL");

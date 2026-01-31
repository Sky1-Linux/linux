// SPDX-License-Identifier: GPL-2.0
/*
 * Sky1 Hardware Spinlock driver
 *
 * Copyright 2024 Cix Technology Group Co., Ltd.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/pm_runtime.h>
#include <linux/hwspinlock.h>
#include <linux/platform_device.h>
#include <linux/of.h>

#include "hwspinlock_internal.h"

/* Number of Hardware Spinlocks */
#define SKY1_HWSPINLOCK_NUM	100

/* Hardware spinlock register offsets */
#define SKY1_HWSPINLOCK_OFFSET(x)	(0x900 + 0x4 * (x))

#define SKY1_HWSPINLOCK_OWNER_ID	0x01

struct sky1_hwspinlock_data {
	void __iomem *io_base;
	struct dentry *debugfs;
};

#ifdef CONFIG_DEBUG_FS

static int sky1_hwspinlock_status_show(struct seq_file *seqf, void *unused)
{
	struct sky1_hwspinlock_data *priv = seqf->private;
	int i;
	u32 val;

	seq_printf(seqf, "Sky1 Hardware Spinlock Status\n");
	seq_printf(seqf, "Number of locks: %d\n\n", SKY1_HWSPINLOCK_NUM);

	for (i = 0; i < SKY1_HWSPINLOCK_NUM; i++) {
		val = readl(priv->io_base + SKY1_HWSPINLOCK_OFFSET(i));
		if (val & 0xFF)
			seq_printf(seqf, "  lock[%02d]: owner=0x%02x\n", i, val & 0xFF);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sky1_hwspinlock_status);

static void sky1_hwspinlock_debugfs_init(struct platform_device *pdev,
					 struct sky1_hwspinlock_data *priv)
{
	priv->debugfs = debugfs_create_dir("sky1_hwspinlock", NULL);
	debugfs_create_file("status", 0444, priv->debugfs, priv,
			    &sky1_hwspinlock_status_fops);
}

static void sky1_hwspinlock_debugfs_exit(struct sky1_hwspinlock_data *priv)
{
	debugfs_remove_recursive(priv->debugfs);
}

#else

static void sky1_hwspinlock_debugfs_init(struct platform_device *pdev,
					 struct sky1_hwspinlock_data *priv) { }
static void sky1_hwspinlock_debugfs_exit(struct sky1_hwspinlock_data *priv) { }

#endif

static int sky1_hwspinlock_trylock(struct hwspinlock *lock)
{
	void __iomem *lock_addr = lock->priv;

	/* Check if already owned by us */
	if (SKY1_HWSPINLOCK_OWNER_ID == (readl(lock_addr) & 0xFF))
		return 0;

	/* Attempt to acquire the lock */
	writel(SKY1_HWSPINLOCK_OWNER_ID, lock_addr);

	/* Read back to check if we got it */
	return (SKY1_HWSPINLOCK_OWNER_ID == (readl(lock_addr) & 0xFF)) ? 1 : 0;
}

static void sky1_hwspinlock_unlock(struct hwspinlock *lock)
{
	void __iomem *lock_addr = lock->priv;

	/* Release the lock by writing owner ID */
	writel(SKY1_HWSPINLOCK_OWNER_ID, lock_addr);
}

static const struct hwspinlock_ops sky1_hwspinlock_ops = {
	.trylock = sky1_hwspinlock_trylock,
	.unlock = sky1_hwspinlock_unlock,
};

static int sky1_hwspinlock_probe(struct platform_device *pdev)
{
	struct sky1_hwspinlock_data *priv;
	struct hwspinlock_device *bank;
	struct hwspinlock *hwlock;
	int idx, ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->io_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->io_base))
		return PTR_ERR(priv->io_base);

	bank = devm_kzalloc(&pdev->dev, struct_size(bank, lock, SKY1_HWSPINLOCK_NUM),
			    GFP_KERNEL);
	if (!bank)
		return -ENOMEM;

	for (idx = 0; idx < SKY1_HWSPINLOCK_NUM; idx++) {
		hwlock = &bank->lock[idx];
		hwlock->priv = priv->io_base + SKY1_HWSPINLOCK_OFFSET(idx);
	}

	platform_set_drvdata(pdev, priv);
	pm_runtime_enable(&pdev->dev);

	ret = devm_hwspin_lock_register(&pdev->dev, bank, &sky1_hwspinlock_ops,
					0, SKY1_HWSPINLOCK_NUM);
	if (ret) {
		pm_runtime_disable(&pdev->dev);
		return ret;
	}

	sky1_hwspinlock_debugfs_init(pdev, priv);

	return 0;
}

static void sky1_hwspinlock_remove(struct platform_device *pdev)
{
	struct sky1_hwspinlock_data *priv = platform_get_drvdata(pdev);

	sky1_hwspinlock_debugfs_exit(priv);
	pm_runtime_disable(&pdev->dev);
}

static const struct of_device_id sky1_hwspinlock_of_match[] = {
	{ .compatible = "sky1,hwspinlock", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, sky1_hwspinlock_of_match);

static struct platform_driver sky1_hwspinlock_driver = {
	.probe = sky1_hwspinlock_probe,
	.remove = sky1_hwspinlock_remove,
	.driver = {
		.name = "sky1-hwspinlock",
		.of_match_table = sky1_hwspinlock_of_match,
	},
};
module_platform_driver(sky1_hwspinlock_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Sky1 Hardware Spinlock driver");
MODULE_AUTHOR("Cix Technology Group Co., Ltd.");

// SPDX-License-Identifier: GPL-2.0
/*
 * Sky1 Hardware Spinlock driver
 *
 * Copyright 2024 Cix Technology Group Co., Ltd.
 */

#include <linux/kernel.h>
#include <linux/module.h>
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
	struct hwspinlock_device *bank;
	struct hwspinlock *hwlock;
	void __iomem *io_base;
	int idx, ret;

	io_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(io_base))
		return PTR_ERR(io_base);

	bank = devm_kzalloc(&pdev->dev, struct_size(bank, lock, SKY1_HWSPINLOCK_NUM),
			    GFP_KERNEL);
	if (!bank)
		return -ENOMEM;

	for (idx = 0; idx < SKY1_HWSPINLOCK_NUM; idx++) {
		hwlock = &bank->lock[idx];
		hwlock->priv = io_base + SKY1_HWSPINLOCK_OFFSET(idx);
	}

	pm_runtime_enable(&pdev->dev);

	ret = devm_hwspin_lock_register(&pdev->dev, bank, &sky1_hwspinlock_ops,
					0, SKY1_HWSPINLOCK_NUM);
	if (ret) {
		pm_runtime_disable(&pdev->dev);
		return ret;
	}

	return 0;
}

static void sky1_hwspinlock_remove(struct platform_device *pdev)
{
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

// SPDX-License-Identifier: GPL-2.0
/*
 * Sky1 NVMEM (eFuse) driver
 *
 * Copyright 2024 Cix Technology Group Co., Ltd.
 *
 * Reads efuse data via ARM SMCCC firmware interface.
 */

#include <linux/arm-smccc.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/platform_device.h>

/* SMC function ID for fuse read request */
#define SKY1_SIP_FUSE_READ	\
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_32, \
			   62, 9)

struct sky1_nvmem {
	struct device *dev;
};

static int sky1_nvmem_read(void *context, unsigned int offset,
			   void *val, size_t bytes)
{
	struct sky1_nvmem *priv = context;
	struct arm_smccc_res res;
	u8 *buf = val;
	size_t i;

	for (i = 0; i < bytes; i++) {
		arm_smccc_smc(SKY1_SIP_FUSE_READ, offset + i, 1,
			      0, 0, 0, 0, 0, &res);
		if (res.a0 == 0) {
			dev_err(priv->dev, "Failed to read fuse at offset %u\n",
				offset + (unsigned int)i);
			return -EIO;
		}
		buf[i] = res.a1 & 0xff;
	}

	return 0;
}

static int sky1_nvmem_probe(struct platform_device *pdev)
{
	struct nvmem_config config = {
		.name = "sky1-efuse",
		.dev = &pdev->dev,
		.owner = THIS_MODULE,
		.word_size = 1,
		.size = 0x500,  /* Total efuse size - OPN at 0x4dc needs 0x4e4 */
		.read_only = true,
		.reg_read = sky1_nvmem_read,
		.add_legacy_fixed_of_cells = true,
	};
	struct sky1_nvmem *priv;
	struct nvmem_device *nvmem;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &pdev->dev;
	config.priv = priv;

	nvmem = devm_nvmem_register(&pdev->dev, &config);
	if (IS_ERR(nvmem))
		return PTR_ERR(nvmem);

	return 0;
}

static const struct of_device_id sky1_nvmem_of_match[] = {
	{ .compatible = "cix,sky1-nvmem-fw" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sky1_nvmem_of_match);

static struct platform_driver sky1_nvmem_driver = {
	.probe = sky1_nvmem_probe,
	.driver = {
		.name = "sky1-nvmem",
		.of_match_table = sky1_nvmem_of_match,
	},
};
module_platform_driver(sky1_nvmem_driver);

MODULE_AUTHOR("Cix Technology Group Co., Ltd.");
MODULE_DESCRIPTION("Sky1 NVMEM (eFuse) driver");
MODULE_LICENSE("GPL");

// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2017 Cadence
// Cadence PCIe controller driver.
// Author: Cyrille Pitchen <cyrille.pitchen@free-electrons.com>

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>

#include "pcie-cadence.h"
#include "../../pci.h"

u8 cdns_pcie_find_capability(struct cdns_pcie *pcie, u8 cap)
{
	return PCI_FIND_NEXT_CAP(cdns_pcie_read_cfg, PCI_CAPABILITY_LIST,
				 cap, NULL, pcie);
}
EXPORT_SYMBOL_GPL(cdns_pcie_find_capability);

u16 cdns_pcie_find_ext_capability(struct cdns_pcie *pcie, u8 cap)
{
	return PCI_FIND_NEXT_EXT_CAP(cdns_pcie_read_cfg, 0, cap, NULL, pcie);
}
EXPORT_SYMBOL_GPL(cdns_pcie_find_ext_capability);

/*
 * Physical Layer Configuration Register 0
 * Used for LTSSM Detect Quiet min delay on Sky1 hardware.
 */
#define CDNS_PCIE_PHY_LAYER_CFG0		(CDNS_PCIE_LM_BASE + 0x0400)
#define CDNS_PCIE_PHY_DETECT_QUIET_MIN_DELAY_MASK	GENMASK(26, 24)
#define CDNS_PCIE_PHY_DETECT_QUIET_MIN_DELAY_SHIFT	24
#define CDNS_PCIE_PHY_DETECT_QUIET_MIN_DELAY(delay) \
	(((delay) << CDNS_PCIE_PHY_DETECT_QUIET_MIN_DELAY_SHIFT) & \
	 CDNS_PCIE_PHY_DETECT_QUIET_MIN_DELAY_MASK)

void cdns_pcie_detect_quiet_min_delay_set(struct cdns_pcie *pcie)
{
	u32 delay = 0x3;
	u32 ltssm_control_cap;

	/*
	 * Set the LTSSM Detect Quiet state min. delay to 2ms.
	 * Sky1 uses PHY_LAYER_CFG0 register at offset 0x1400.
	 */
	ltssm_control_cap = cdns_pcie_readl(pcie, CDNS_PCIE_PHY_LAYER_CFG0);
	ltssm_control_cap = ((ltssm_control_cap &
			    ~CDNS_PCIE_PHY_DETECT_QUIET_MIN_DELAY_MASK) |
			    CDNS_PCIE_PHY_DETECT_QUIET_MIN_DELAY(delay));

	cdns_pcie_writel(pcie, CDNS_PCIE_PHY_LAYER_CFG0, ltssm_control_cap);
}
EXPORT_SYMBOL_GPL(cdns_pcie_detect_quiet_min_delay_set);

void cdns_pcie_set_outbound_region(struct cdns_pcie *pcie, u8 busnr, u8 fn,
				   u32 r, bool is_io,
				   u64 cpu_addr, u64 pci_addr, size_t size)
{
	/*
	 * roundup_pow_of_two() returns an unsigned long, which is not suited
	 * for 64bit values.
	 */
	u64 sz = 1ULL << fls64(size - 1);
	int nbits = ilog2(sz);
	u32 addr0, addr1, desc0, desc1, ctrl0 = 0;

	if (nbits < 8)
		nbits = 8;

	/* Set the PCI address */
	addr0 = CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_NBITS(nbits) |
		(lower_32_bits(pci_addr) & GENMASK(31, 8));
	addr1 = upper_32_bits(pci_addr);

	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_PCI_ADDR0(r), addr0);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_PCI_ADDR1(r), addr1);

	/* Set the PCIe header descriptor */
	if (is_io)
		desc0 = CDNS_PCIE_AT_OB_REGION_DESC0_TYPE_IO;
	else
		desc0 = CDNS_PCIE_AT_OB_REGION_DESC0_TYPE_MEM;
	desc1 = 0;

	/*
	 * Sky1 vendor IP uses CTRL0 register bits [26] and [25] to control
	 * whether bus/device/function numbers are supplied by software.
	 *
	 * In Root Complex mode, set SUPPLY_BUS and SUPPLY_DEV_FN bits,
	 * and provide bus number in DESC1[31:24], devfn in DESC1[23:16].
	 *
	 * In Endpoint mode, clear these bits to use captured values,
	 * but still set function number in DESC1.
	 */
	if (pcie->is_rc) {
		/* The device and function numbers are always 0. */
		desc1 = CDNS_PCIE_AT_OB_REGION_DESC1_BUS(busnr) |
			CDNS_PCIE_AT_OB_REGION_DESC1_DEVFN(0);
		ctrl0 = CDNS_PCIE_AT_OB_REGION_CTRL0_SUPPLY_BUS |
			CDNS_PCIE_AT_OB_REGION_CTRL0_SUPPLY_DEV_FN;
	} else {
		/*
		 * Use captured values for bus and device numbers but still
		 * need to set the function number.
		 */
		desc1 |= CDNS_PCIE_AT_OB_REGION_DESC1_DEVFN(fn);
	}

	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_DESC0(r), desc0);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_DESC1(r), desc1);

	/* Set the CPU address */
	if (pcie->ops && pcie->ops->cpu_addr_fixup)
		cpu_addr = pcie->ops->cpu_addr_fixup(pcie, cpu_addr);

	addr0 = CDNS_PCIE_AT_OB_REGION_CPU_ADDR0_NBITS(nbits) |
		(lower_32_bits(cpu_addr) & GENMASK(31, 8));
	addr1 = upper_32_bits(cpu_addr);

	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_CPU_ADDR0(r), addr0);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_CPU_ADDR1(r), addr1);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_CTRL0(r), ctrl0);
}
EXPORT_SYMBOL_GPL(cdns_pcie_set_outbound_region);

void cdns_pcie_set_outbound_region_for_normal_msg(struct cdns_pcie *pcie,
						  u8 busnr, u8 fn,
						  u32 r, u64 cpu_addr)
{
	u32 addr0, addr1, desc0, desc1, ctrl0 = 0;

	desc0 = CDNS_PCIE_AT_OB_REGION_DESC0_TYPE_NORMAL_MSG;
	desc1 = 0;

	/* See cdns_pcie_set_outbound_region() comments above. */
	if (pcie->is_rc) {
		desc1 = CDNS_PCIE_AT_OB_REGION_DESC1_BUS(busnr) |
			CDNS_PCIE_AT_OB_REGION_DESC1_DEVFN(0);
		ctrl0 = CDNS_PCIE_AT_OB_REGION_CTRL0_SUPPLY_BUS |
			CDNS_PCIE_AT_OB_REGION_CTRL0_SUPPLY_DEV_FN;
	} else {
		desc1 |= CDNS_PCIE_AT_OB_REGION_DESC1_DEVFN(fn);
	}

	/* Set the CPU address */
	if (pcie->ops && pcie->ops->cpu_addr_fixup)
		cpu_addr = pcie->ops->cpu_addr_fixup(pcie, cpu_addr);

	addr0 = CDNS_PCIE_AT_OB_REGION_CPU_ADDR0_NBITS(17) |
		(lower_32_bits(cpu_addr) & GENMASK(31, 8));
	addr1 = upper_32_bits(cpu_addr);

	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_PCI_ADDR0(r), 0);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_PCI_ADDR1(r), 0);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_DESC0(r), desc0);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_DESC1(r), desc1);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_CPU_ADDR0(r), addr0);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_CPU_ADDR1(r), addr1);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_CTRL0(r), ctrl0);
}
EXPORT_SYMBOL_GPL(cdns_pcie_set_outbound_region_for_normal_msg);

void cdns_pcie_reset_outbound_region(struct cdns_pcie *pcie, u32 r)
{
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_PCI_ADDR0(r), 0);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_PCI_ADDR1(r), 0);

	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_DESC0(r), 0);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_DESC1(r), 0);

	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_CPU_ADDR0(r), 0);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_CPU_ADDR1(r), 0);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_CTRL0(r), 0);
}
EXPORT_SYMBOL_GPL(cdns_pcie_reset_outbound_region);

void cdns_pcie_disable_phy(struct cdns_pcie *pcie)
{
	int i = pcie->phy_count;

	while (i--) {
		phy_power_off(pcie->phy[i]);
		phy_exit(pcie->phy[i]);
	}
}
EXPORT_SYMBOL_GPL(cdns_pcie_disable_phy);

int cdns_pcie_enable_phy(struct cdns_pcie *pcie)
{
	int ret;
	int i;

	for (i = 0; i < pcie->phy_count; i++) {
		ret = phy_init(pcie->phy[i]);
		if (ret < 0)
			goto err_phy;

		ret = phy_power_on(pcie->phy[i]);
		if (ret < 0) {
			phy_exit(pcie->phy[i]);
			goto err_phy;
		}
	}

	return 0;

err_phy:
	while (--i >= 0) {
		phy_power_off(pcie->phy[i]);
		phy_exit(pcie->phy[i]);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(cdns_pcie_enable_phy);

int cdns_pcie_init_phy(struct device *dev, struct cdns_pcie *pcie)
{
	struct device_node *np = dev->of_node;
	int phy_count;
	struct phy **phy;
	struct device_link **link;
	int i;
	int ret;
	const char *name;

	phy_count = of_property_count_strings(np, "phy-names");
	if (phy_count < 1) {
		dev_info(dev, "no \"phy-names\" property found; PHY will not be initialized\n");
		pcie->phy_count = 0;
		return 0;
	}

	phy = devm_kcalloc(dev, phy_count, sizeof(*phy), GFP_KERNEL);
	if (!phy)
		return -ENOMEM;

	link = devm_kcalloc(dev, phy_count, sizeof(*link), GFP_KERNEL);
	if (!link)
		return -ENOMEM;

	for (i = 0; i < phy_count; i++) {
		of_property_read_string_index(np, "phy-names", i, &name);
		phy[i] = devm_phy_get(dev, name);
		if (IS_ERR(phy[i])) {
			ret = PTR_ERR(phy[i]);
			goto err_phy;
		}
		link[i] = device_link_add(dev, &phy[i]->dev, DL_FLAG_STATELESS);
		if (!link[i]) {
			devm_phy_put(dev, phy[i]);
			ret = -EINVAL;
			goto err_phy;
		}
	}

	pcie->phy_count = phy_count;
	pcie->phy = phy;
	pcie->link = link;

	ret =  cdns_pcie_enable_phy(pcie);
	if (ret)
		goto err_phy;

	return 0;

err_phy:
	while (--i >= 0) {
		device_link_del(link[i]);
		devm_phy_put(dev, phy[i]);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(cdns_pcie_init_phy);

static int cdns_pcie_suspend_noirq(struct device *dev)
{
	struct cdns_pcie *pcie = dev_get_drvdata(dev);

	cdns_pcie_disable_phy(pcie);

	return 0;
}

static int cdns_pcie_resume_noirq(struct device *dev)
{
	struct cdns_pcie *pcie = dev_get_drvdata(dev);
	int ret;

	ret = cdns_pcie_enable_phy(pcie);
	if (ret) {
		dev_err(dev, "failed to enable PHY\n");
		return ret;
	}

	return 0;
}

const struct dev_pm_ops cdns_pcie_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(cdns_pcie_suspend_noirq,
				  cdns_pcie_resume_noirq)
};

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cadence PCIe controller driver");
MODULE_AUTHOR("Cyrille Pitchen <cyrille.pitchen@free-electrons.com>");

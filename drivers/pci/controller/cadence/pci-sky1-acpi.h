/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _PCIE_SKY1_ACPI_H
#define _PCIE_SKY1_ACPI_H

struct acpi_device;

/*
 * Platform data passed from pci-sky1-acpi.c scan handler to pci-sky1.c
 * probe function.  CIXH2020 platform devices are created without fwnode
 * to bypass fw_devlink (ACPI _DSD references create mandatory supplier
 * links to devices like CIXHA010 that have no driver).  The ACPI device
 * is passed via platform data and set as companion at probe time.
 */
struct sky1_pcie_acpi_pdata {
	struct acpi_device *adev;
};

#endif /* _PCIE_SKY1_ACPI_H */

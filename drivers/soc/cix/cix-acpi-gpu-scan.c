// SPDX-License-Identifier: GPL-2.0
/*
 * ACPI scan handler to override GPU _CCA on CIX Sky1.
 *
 * The Sky1 DSDT declares the GPU (CIXH5000) with _CCA=1 (coherent),
 * but the SoC was designed for non-coherent GPU operation:
 *
 *   - Device Tree has system-coherency=0 and no dma-coherent on the GPU
 *   - The GPU shares framebuffer memory with the DPU which is non-coherent
 *   - When the GPU enables ACE-Lite coherency (from _CCA=1) while the DPU
 *     reads non-coherently from DRAM, the DPU sees stale data causing
 *     visible rendering artefacts (interleaved patches filling in slowly)
 *
 * This scan handler intercepts CIXH5000 during ACPI device enumeration
 * and clears the coherent_dma flag BEFORE the platform device and its
 * DMA/IOMMU domain are configured.  This ensures the entire DMA path
 * (allocations, mappings, cache maintenance) matches DT behaviour.
 *
 * The Panthor driver also has a Sky1-specific quirk that sets the GPU
 * coherency protocol to NONE, but without this scan handler the kernel
 * DMA layer still treats the device as coherent.
 */

#ifdef CONFIG_ACPI

#include <linux/acpi.h>
#include <linux/init.h>

static bool sky1_gpu_acpi_detected;

static int sky1_gpu_scan_attach(struct acpi_device *adev,
				const struct acpi_device_id *id)
{
	if (!sky1_gpu_acpi_detected)
		return 0;

	dev_info(&adev->dev,
		 "Sky1: overriding GPU _CCA to non-coherent (DT parity)\n");
	adev->flags.coherent_dma = 0;

	/* Return 0 to allow normal platform device creation to proceed */
	return 0;
}

static const struct acpi_device_id sky1_gpu_scan_ids[] = {
	{ "CIXH5000", 0 },
	{ },
};

static struct acpi_scan_handler sky1_gpu_scan_handler = {
	.ids = sky1_gpu_scan_ids,
	.attach = sky1_gpu_scan_attach,
};

static int __init sky1_gpu_acpi_init(void)
{
	struct acpi_table_header *header;

	if (acpi_disabled)
		return 0;

	/* Use MCFG OEM ID to identify Sky1, same as PCIe and USB handlers */
	if (ACPI_FAILURE(acpi_get_table(ACPI_SIG_MCFG, 0, &header)))
		return 0;

	sky1_gpu_acpi_detected = !memcmp(header->oem_id, "CIXTEK", 6) &&
				  !memcmp(header->oem_table_id, "SKY1EDK2", 8);
	acpi_put_table(header);

	if (!sky1_gpu_acpi_detected)
		return 0;

	pr_info("Sky1 GPU: registering CIXH5000 scan handler\n");
	return acpi_scan_add_handler(&sky1_gpu_scan_handler);
}
arch_initcall(sky1_gpu_acpi_init);

#endif /* CONFIG_ACPI */

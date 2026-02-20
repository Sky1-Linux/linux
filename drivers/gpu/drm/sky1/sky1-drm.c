// SPDX-License-Identifier: GPL-2.0-only
/*
 * sky1-drm - DRM render node bridge for CIX Sky1 SoC
 *
 * The Mali GPU on Sky1 uses a non-standard device interface (/dev/mali0)
 * instead of a DRM render node.  This module provides a standard DRM render
 * node with GEM shmem buffer allocation and PRIME (DMA-BUF) support, enabling
 * GBM and Mesa Zink to discover and use the GPU via the normal Linux graphics
 * stack.
 *
 * Uses a platform device so the render node appears on DRM_BUS_PLATFORM,
 * allowing Mesa's kmsro pipe_loader to discover it as a compatible render
 * device for the linlondp DPU.
 *
 * Modeled on drivers/gpu/drm/vgem/vgem_drv.c, minus fence ioctls.
 */

#include <linux/dma-buf.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>

#define DRIVER_NAME	"sky1-drm"
#define DRIVER_DESC	"Sky1 GPU render node bridge"
#define DRIVER_MAJOR	1
#define DRIVER_MINOR	0

static struct sky1_drm_device {
	struct drm_device drm;
	struct platform_device *pdev;
} *sky1_device;

static struct drm_gem_object *sky1_gem_create_object(struct drm_device *dev,
						     size_t size)
{
	struct drm_gem_shmem_object *obj;

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return ERR_PTR(-ENOMEM);

	/* Write-combining ensures CPU writes are visible to DMA devices
	 * (DPU, other consumers) without explicit cache management. */
	obj->map_wc = true;

	return &obj->base;
}

DEFINE_DRM_GEM_FOPS(sky1_drm_fops);

static const struct drm_driver sky1_drm_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_RENDER,
	.fops			= &sky1_drm_fops,

	DRM_GEM_SHMEM_DRIVER_OPS,
	.gem_create_object	= sky1_gem_create_object,

	.name	= DRIVER_NAME,
	.desc	= DRIVER_DESC,
	.major	= DRIVER_MAJOR,
	.minor	= DRIVER_MINOR,
};

static int __init sky1_drm_init(void)
{
	int ret;
	struct platform_device *pdev;

	pdev = platform_device_register_simple(DRIVER_NAME, -1, NULL, 0);
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);

	if (!devres_open_group(&pdev->dev, NULL, GFP_KERNEL)) {
		ret = -ENOMEM;
		goto out_destroy;
	}

	dma_coerce_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));

	sky1_device = devm_drm_dev_alloc(&pdev->dev, &sky1_drm_driver,
					 struct sky1_drm_device, drm);
	if (IS_ERR(sky1_device)) {
		ret = PTR_ERR(sky1_device);
		goto out_devres;
	}
	sky1_device->pdev = pdev;

	ret = drm_dev_register(&sky1_device->drm, 0);
	if (ret)
		goto out_devres;

	return 0;

out_devres:
	devres_release_group(&pdev->dev, NULL);
out_destroy:
	platform_device_unregister(pdev);
	return ret;
}

static void __exit sky1_drm_exit(void)
{
	struct platform_device *pdev = sky1_device->pdev;

	drm_dev_unregister(&sky1_device->drm);
	devres_release_group(&pdev->dev, NULL);
	platform_device_unregister(pdev);
}

module_init(sky1_drm_init);
module_exit(sky1_drm_exit);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");

// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright 2018 Marty E. Plummer <hanetzer@startmail.com> */
/* Copyright 2019 Linaro, Ltd., Rob Herring <robh@kernel.org> */
/* Copyright 2019 Collabora ltd. */

#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include <drm/drm_drv.h>
#include <drm/drm_managed.h>

#include "panthor_device.h"
#include "panthor_gpu.h"
#include "panthor_regs.h"

/**
 * struct panthor_gpu - GPU block management data.
 */
struct panthor_gpu {
	/** @irq: GPU irq. */
	struct panthor_irq irq;

	/** @reqs_lock: Lock protecting access to pending_reqs. */
	spinlock_t reqs_lock;

	/** @pending_reqs: Pending GPU requests. */
	u32 pending_reqs;

	/** @reqs_acked: GPU request wait queue. */
	wait_queue_head_t reqs_acked;

	/** @cache_flush_lock: Lock to serialize cache flushes */
	struct mutex cache_flush_lock;
};

#define GPU_INTERRUPTS_MASK	\
	(GPU_IRQ_FAULT | \
	 GPU_IRQ_PROTM_FAULT | \
	 GPU_IRQ_RESET_COMPLETED | \
	 GPU_IRQ_CLEAN_CACHES_COMPLETED)

/*
 * AMBA_ENABLE register bit fields (arch12+ / G720)
 * Bits 0-4: Coherency protocol (0=ACE_LITE, 1=ACE, 31=NONE)
 * Bit 5: Shareable cache support
 *
 * Note: For arch12+, coherency is configured via AMBA_ENABLE (0x304),
 * not by direct write. The mode value goes in bits 0-4.
 */
#define AMBA_ENABLE_COHERENCY_MASK	0x1F
#define AMBA_ENABLE_SHAREABLE_CACHE	BIT(5)

static void panthor_gpu_coherency_set(struct panthor_device *ptdev)
{
	/*
	 * Sky1/G720 (arch12+): Use AMBA_ENABLE register with read-modify-write.
	 * The coherency protocol is in bits 0-4, not the whole register.
	 * Vendor uses ACE_LITE (mode 0) for coherency.
	 */
	if (of_device_is_compatible(ptdev->base.dev->of_node, "cix,sky1-mali")) {
		u32 val = gpu_read(ptdev, GPU_COHERENCY_PROTOCOL);

		/* Clear coherency bits (0-4) to set ACE_LITE (mode 0) */
		val &= ~AMBA_ENABLE_COHERENCY_MASK;

		gpu_write(ptdev, GPU_COHERENCY_PROTOCOL, val);
		return;
	}

	gpu_write(ptdev, GPU_COHERENCY_PROTOCOL,
		ptdev->coherent ? GPU_COHERENCY_PROT_BIT(ACE_LITE) : GPU_COHERENCY_NONE);
}

/*
 * Set AMBA shareable cache support for arch12+ GPUs.
 * Enables the GPU to participate in shareable cache operations.
 * Called after coherency_set, before L2 power-on.
 */
static void panthor_gpu_amba_set_shareable_cache(struct panthor_device *ptdev)
{
	u32 features, enable;

	/* Only for Sky1/G720 with coherency enabled */
	if (!of_device_is_compatible(ptdev->base.dev->of_node, "cix,sky1-mali"))
		return;

	if (!ptdev->coherent)
		return;

	/* Check if shareable cache support is available in AMBA_FEATURES (0x300) */
	features = gpu_read(ptdev, GPU_COHERENCY_FEATURES);
	if (!(features & AMBA_ENABLE_SHAREABLE_CACHE))
		return;

	/* Set shareable cache support bit in AMBA_ENABLE (0x304) */
	enable = gpu_read(ptdev, GPU_COHERENCY_PROTOCOL);
	enable |= AMBA_ENABLE_SHAREABLE_CACHE;
	gpu_write(ptdev, GPU_COHERENCY_PROTOCOL, enable);
}

static void panthor_gpu_irq_handler(struct panthor_device *ptdev, u32 status)
{
	gpu_write(ptdev, GPU_INT_CLEAR, status);

	if (status & GPU_IRQ_FAULT) {
		u32 fault_status = gpu_read(ptdev, GPU_FAULT_STATUS);
		u64 address = gpu_read64(ptdev, GPU_FAULT_ADDR);

		drm_warn(&ptdev->base, "GPU Fault 0x%08x (%s) at 0x%016llx\n",
			 fault_status, panthor_exception_name(ptdev, fault_status & 0xFF),
			 address);
	}
	if (status & GPU_IRQ_PROTM_FAULT)
		drm_warn(&ptdev->base, "GPU Fault in protected mode\n");

	spin_lock(&ptdev->gpu->reqs_lock);
	if (status & ptdev->gpu->pending_reqs) {
		ptdev->gpu->pending_reqs &= ~status;
		wake_up_all(&ptdev->gpu->reqs_acked);
	}
	spin_unlock(&ptdev->gpu->reqs_lock);
}
PANTHOR_IRQ_HANDLER(gpu, GPU, panthor_gpu_irq_handler);

/**
 * panthor_gpu_unplug() - Called when the GPU is unplugged.
 * @ptdev: Device to unplug.
 */
void panthor_gpu_unplug(struct panthor_device *ptdev)
{
	unsigned long flags;

	/* Make sure the IRQ handler is not running after that point. */
	if (!IS_ENABLED(CONFIG_PM) || pm_runtime_active(ptdev->base.dev))
		panthor_gpu_irq_suspend(&ptdev->gpu->irq);

	/* Wake-up all waiters. */
	spin_lock_irqsave(&ptdev->gpu->reqs_lock, flags);
	ptdev->gpu->pending_reqs = 0;
	wake_up_all(&ptdev->gpu->reqs_acked);
	spin_unlock_irqrestore(&ptdev->gpu->reqs_lock, flags);
}

/**
 * panthor_gpu_init() - Initialize the GPU block
 * @ptdev: Device.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
int panthor_gpu_init(struct panthor_device *ptdev)
{
	struct panthor_gpu *gpu;
	u32 pa_bits;
	int ret, irq;

	gpu = drmm_kzalloc(&ptdev->base, sizeof(*gpu), GFP_KERNEL);
	if (!gpu)
		return -ENOMEM;

	spin_lock_init(&gpu->reqs_lock);
	init_waitqueue_head(&gpu->reqs_acked);
	mutex_init(&gpu->cache_flush_lock);
	ptdev->gpu = gpu;

	dma_set_max_seg_size(ptdev->base.dev, UINT_MAX);
	pa_bits = GPU_MMU_FEATURES_PA_BITS(ptdev->gpu_info.mmu_features);
	ret = dma_set_mask_and_coherent(ptdev->base.dev, DMA_BIT_MASK(pa_bits));
	if (ret)
		return ret;

	irq = platform_get_irq_byname(to_platform_device(ptdev->base.dev), "gpu");
	if (irq < 0)
		return irq;

	ret = panthor_request_gpu_irq(ptdev, &ptdev->gpu->irq, irq, GPU_INTERRUPTS_MASK);
	if (ret)
		return ret;

	/*
	 * G720 (arch >= 12): Issue a soft reset via PWR_COMMAND to enable
	 * the HOST_POWER register block. Without this, the HOST_POWER
	 * registers return 0 and shader power control doesn't work.
	 */
	if (GPU_ARCH_MAJOR(ptdev->gpu_info.gpu_id) >= 12) {
		ret = panthor_gpu_soft_reset(ptdev);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * panthor_gpu_block_power_off() - Power-off a specific block of the GPU
 * @ptdev: Device.
 * @blk_name: Block name.
 * @pwroff_reg: Power-off register for this block.
 * @pwrtrans_reg: Power transition register for this block.
 * @mask: Sub-elements to power-off.
 * @timeout_us: Timeout in microseconds.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
int panthor_gpu_block_power_off(struct panthor_device *ptdev,
				const char *blk_name,
				u32 pwroff_reg, u32 pwrtrans_reg,
				u64 mask, u32 timeout_us)
{
	u32 val;
	int ret;

	ret = gpu_read64_relaxed_poll_timeout(ptdev, pwrtrans_reg, val,
					      !(mask & val), 100, timeout_us);
	if (ret) {
		drm_err(&ptdev->base,
			"timeout waiting on %s:%llx power transition", blk_name,
			mask);
		return ret;
	}

	gpu_write64(ptdev, pwroff_reg, mask);

	ret = gpu_read64_relaxed_poll_timeout(ptdev, pwrtrans_reg, val,
					      !(mask & val), 100, timeout_us);
	if (ret) {
		drm_err(&ptdev->base,
			"timeout waiting on %s:%llx power transition", blk_name,
			mask);
		return ret;
	}

	return 0;
}

/**
 * panthor_gpu_block_power_on() - Power-on a specific block of the GPU
 * @ptdev: Device.
 * @blk_name: Block name.
 * @pwron_reg: Power-on register for this block.
 * @pwrtrans_reg: Power transition register for this block.
 * @rdy_reg: Power transition ready register.
 * @mask: Sub-elements to power-on.
 * @timeout_us: Timeout in microseconds.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
int panthor_gpu_block_power_on(struct panthor_device *ptdev,
			       const char *blk_name,
			       u32 pwron_reg, u32 pwrtrans_reg,
			       u32 rdy_reg, u64 mask, u32 timeout_us)
{
	u32 val;
	int ret;

	ret = gpu_read64_relaxed_poll_timeout(ptdev, pwrtrans_reg, val,
					      !(mask & val), 100, timeout_us);
	if (ret) {
		drm_err(&ptdev->base,
			"timeout waiting on %s:%llx power transition", blk_name,
			mask);
		return ret;
	}

	gpu_write64(ptdev, pwron_reg, mask);

	ret = gpu_read64_relaxed_poll_timeout(ptdev, rdy_reg, val,
					      (mask & val) == mask,
					      100, timeout_us);
	if (ret) {
		drm_err(&ptdev->base, "timeout waiting on %s:%llx readiness",
			blk_name, mask);
		return ret;
	}

	return 0;
}

/**
 * panthor_gpu_l2_power_on() - Power-on the L2-cache
 * @ptdev: Device.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
int panthor_gpu_l2_power_on(struct panthor_device *ptdev)
{
	if (ptdev->gpu_info.l2_present != 1) {
		/*
		 * Only support one core group now.
		 * ~(l2_present - 1) unsets all bits in l2_present except
		 * the bottom bit. (l2_present - 2) has all the bits in
		 * the first core group set. AND them together to generate
		 * a mask of cores in the first core group.
		 */
		u64 core_mask = ~(ptdev->gpu_info.l2_present - 1) &
				(ptdev->gpu_info.l2_present - 2);
		drm_info_once(&ptdev->base, "using only 1st core group (%lu cores from %lu)\n",
			      hweight64(core_mask),
			      hweight64(ptdev->gpu_info.shader_present));
	}

	/*
	 * CIX Sky1 needs special PHBA (Page-Based Hardware Attribute)
	 * and system cache allocation configuration before L2 activation.
	 */
	if (of_device_is_compatible(ptdev->base.dev->of_node, "cix,sky1-mali")) {
		gpu_write(ptdev, GPU_SYSC_PBHA_OVERRIDE(3), 0x22000000);
		gpu_write(ptdev, GPU_SYSC_ALLOC(0), 0x00230000);
		gpu_write(ptdev, GPU_SYSC_ALLOC(1), 0x00000023);
		gpu_write(ptdev, GPU_SYSC_ALLOC(2), 0x00000000);
		gpu_write(ptdev, GPU_SYSC_ALLOC(3), 0x00000000);
		gpu_write(ptdev, GPU_SYSC_ALLOC(4), 0x00523222);
		gpu_write(ptdev, GPU_SYSC_ALLOC(5), 0x00523200);
		gpu_write(ptdev, GPU_SYSC_ALLOC(6), 0x00000022);
		gpu_write(ptdev, GPU_SYSC_ALLOC(7), 0x00000032);

		/* Required to get LS_MEM_* related counters working */
		gpu_write(ptdev, 0x306C, 0xFFFFFFFF);
		gpu_write(ptdev, 0x3070, 0xFFFFFFFF);
		gpu_write(ptdev, 0x307C, 0xFFFFFFFF);
		gpu_write(ptdev, 0x3074, 0xFFFFFFFF);
		gpu_write(ptdev, 0x3068, 0x1);

		/* Sky1 requires PWR_OVERRIDE1 for power transition latency */
		gpu_write(ptdev, GPU_PWR_KEY, GPU_PWR_KEY_UNLOCK);
		gpu_write(ptdev, GPU_PWR_OVERRIDE1, 0xFFFFFF);

		/* Enable Ray Tracing Unit subdomain for shader cores */
		gpu_write(ptdev, SHADER_PWRFEATURES, SHADER_PWRFEATURES_RTU_EN);
	}

	/* Set the desired coherency mode before the power up of L2 */
	panthor_gpu_coherency_set(ptdev);

	/* Enable shareable cache support if available (arch12+) */
	panthor_gpu_amba_set_shareable_cache(ptdev);

	return panthor_gpu_power_on(ptdev, L2, 1, 20000);
}

/**
 * panthor_gpu_g720_shader_power_on() - Power on shader cores for G720
 * @ptdev: Device.
 *
 * G720 (arch >= 12) uses a different power control interface with
 * PWR_CMDARG and PWR_COMMAND registers instead of SHADER_PWRON.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
int panthor_gpu_g720_shader_power_on(struct panthor_device *ptdev)
{
	u64 shader_mask = ptdev->gpu_info.shader_present;
	bool is_sky1 = of_device_is_compatible(ptdev->base.dev->of_node, "cix,sky1-mali");
	u64 pwr_status;
	u64 val;
	int ret;

	if (!shader_mask) {
		drm_err(&ptdev->base, "G720: no shader cores present");
		return -EINVAL;
	}

	/*
	 * Sky1: HOST_POWER interface is non-functional (registers return 0).
	 * Go directly to legacy SHADER_PWRON path without trying HOST_POWER.
	 */
	if (is_sky1) {
		/* Check if already powered via legacy SHADER_READY */
		val = gpu_read64(ptdev, SHADER_READY);
		if ((val & shader_mask) == shader_mask) {
			drm_dbg(&ptdev->base,
				"Sky1: shaders already powered (ready=0x%llx)", val);
			return 0;
		}

		/* Power on via legacy SHADER_PWRON */
		gpu_write64(ptdev, SHADER_PWRON, shader_mask);

		ret = gpu_read64_relaxed_poll_timeout(ptdev, SHADER_READY,
						      val,
						      (shader_mask & val) == shader_mask,
						      100, 100000);
		if (ret) {
			drm_err(&ptdev->base,
				"Sky1: shader power-on failed (ready=0x%llx)", val);
			return ret;
		}
		drm_info(&ptdev->base,
			 "Sky1: shaders powered via legacy path (ready=0x%llx)", val);
		return 0;
	}

	/* Non-Sky1 G720 platforms: Try HOST_POWER interface first */

	/* Debug: read various HOST_POWER registers */
	pwr_status = gpu_read64(ptdev, PWR_STATUS);
	val = gpu_read64(ptdev, HOST_POWER_SHADER_PRESENT);
	drm_info(&ptdev->base,
		 "G720: PWR_STATUS=0x%016llx HP_SHADER_PRESENT=0x%llx (vs gpu_info=0x%llx)",
		 pwr_status, val, shader_mask);

	/* Check if already powered */
	val = gpu_read64(ptdev, HOST_POWER_SHADER_READY);
	if ((val & shader_mask) == shader_mask) {
		drm_info(&ptdev->base, "G720: shaders already powered (ready=0x%llx)", val);
		return 0;
	}

	/*
	 * If PWR_STATUS is 0, the HOST_POWER interface may not be initialized.
	 * Try sending RETRACT command to request shader power control from MCU.
	 */
	if (pwr_status == 0) {
		drm_info(&ptdev->base, "G720: PWR_STATUS=0, attempting to retract power control");

		/* Send RETRACT command to request shader power control from MCU */
		gpu_write64(ptdev, PWR_CMDARG, 0);
		gpu_write(ptdev, PWR_COMMAND,
			  PWR_COMMAND_DOMAIN_SHADER | PWR_COMMAND_RETRACT);

		/* Wait for ALLOW_SHADER to become set */
		ret = gpu_read64_relaxed_poll_timeout(ptdev, PWR_STATUS,
						      pwr_status,
						      pwr_status & PWR_STATUS_ALLOW_SHADER,
						      100, 50000);
		if (ret) {
			drm_warn(&ptdev->base,
				 "G720: RETRACT failed, PWR_STATUS still 0x%llx",
				 pwr_status);
			/* Continue to legacy path below */
		} else {
			drm_info(&ptdev->base,
				 "G720: RETRACT succeeded, PWR_STATUS=0x%llx", pwr_status);
		}
	}

	/*
	 * If shader power is delegated to firmware, we cannot control it.
	 * The firmware should power on shaders based on core_en_mask.
	 */
	if (pwr_status & PWR_STATUS_DELEGATED_SHADER) {
		drm_info(&ptdev->base,
			 "G720: shader power delegated to firmware, skipping host power-on");
		return 0;
	}

	/*
	 * If ALLOW_SHADER is not set after RETRACT attempt, fall back to
	 * legacy SHADER_PWRON register. This may work if HOST_POWER isn't
	 * fully functional but legacy interface is available.
	 */
	if (!(pwr_status & PWR_STATUS_ALLOW_SHADER)) {
		drm_info(&ptdev->base,
			 "G720: HOST_POWER unavailable, trying legacy SHADER_PWRON");

		gpu_write64(ptdev, SHADER_PWRON, shader_mask);

		ret = gpu_read64_relaxed_poll_timeout(ptdev, SHADER_READY,
						      val,
						      (shader_mask & val) == shader_mask,
						      100, 100000);
		if (ret) {
			drm_err(&ptdev->base,
				"G720: legacy shader power-on failed (ready=0x%llx)",
				val);
			return ret;
		}
		drm_info(&ptdev->base,
			 "G720: legacy shader power-on succeeded (ready=0x%llx)", val);
		return 0;
	}

	/* Wait for any pending power transitions */
	ret = gpu_read64_relaxed_poll_timeout(ptdev, HOST_POWER_SHADER_PWRTRANS,
					      val, !(shader_mask & val),
					      100, 20000);
	if (ret) {
		drm_err(&ptdev->base,
			"G720: timeout waiting for shader power transition");
		return ret;
	}

	/* Write shader core mask to PWR_CMDARG */
	gpu_write64(ptdev, PWR_CMDARG, shader_mask);

	/* Issue power-up command for shader domain */
	gpu_write(ptdev, PWR_COMMAND,
		  PWR_COMMAND_DOMAIN_SHADER | PWR_COMMAND_POWER_UP);

	/* Wait for shaders to become ready */
	ret = gpu_read64_relaxed_poll_timeout(ptdev, HOST_POWER_SHADER_READY,
					      val, (shader_mask & val) == shader_mask,
					      100, 100000);
	if (ret) {
		drm_err(&ptdev->base,
			"G720: timeout waiting for shader ready (mask=0x%llx ready=0x%llx)",
			shader_mask, val);
		return ret;
	}

	drm_info(&ptdev->base, "G720: shader cores powered on (ready=0x%llx)",
		 val);

	return 0;
}

/**
 * panthor_gpu_flush_caches() - Flush caches
 * @ptdev: Device.
 * @l2: L2 flush type.
 * @lsc: LSC flush type.
 * @other: Other flush type.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
int panthor_gpu_flush_caches(struct panthor_device *ptdev,
			     u32 l2, u32 lsc, u32 other)
{
	bool timedout = false;
	unsigned long flags;

	/* Serialize cache flush operations. */
	guard(mutex)(&ptdev->gpu->cache_flush_lock);

	spin_lock_irqsave(&ptdev->gpu->reqs_lock, flags);
	if (!drm_WARN_ON(&ptdev->base,
			 ptdev->gpu->pending_reqs & GPU_IRQ_CLEAN_CACHES_COMPLETED)) {
		ptdev->gpu->pending_reqs |= GPU_IRQ_CLEAN_CACHES_COMPLETED;
		gpu_write(ptdev, GPU_CMD, GPU_FLUSH_CACHES(l2, lsc, other));
	}
	spin_unlock_irqrestore(&ptdev->gpu->reqs_lock, flags);

	if (!wait_event_timeout(ptdev->gpu->reqs_acked,
				!(ptdev->gpu->pending_reqs & GPU_IRQ_CLEAN_CACHES_COMPLETED),
				msecs_to_jiffies(100))) {
		spin_lock_irqsave(&ptdev->gpu->reqs_lock, flags);
		if ((ptdev->gpu->pending_reqs & GPU_IRQ_CLEAN_CACHES_COMPLETED) != 0 &&
		    !(gpu_read(ptdev, GPU_INT_RAWSTAT) & GPU_IRQ_CLEAN_CACHES_COMPLETED))
			timedout = true;
		else
			ptdev->gpu->pending_reqs &= ~GPU_IRQ_CLEAN_CACHES_COMPLETED;
		spin_unlock_irqrestore(&ptdev->gpu->reqs_lock, flags);
	}

	if (timedout) {
		drm_err(&ptdev->base, "Flush caches timeout");
		return -ETIMEDOUT;
	}

	return 0;
}

/**
 * panthor_gpu_soft_reset() - Issue a soft-reset
 * @ptdev: Device.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
int panthor_gpu_soft_reset(struct panthor_device *ptdev)
{
	bool timedout = false;
	unsigned long flags;

	spin_lock_irqsave(&ptdev->gpu->reqs_lock, flags);
	if (!drm_WARN_ON(&ptdev->base,
			 ptdev->gpu->pending_reqs & GPU_IRQ_RESET_COMPLETED)) {
		ptdev->gpu->pending_reqs |= GPU_IRQ_RESET_COMPLETED;
		gpu_write(ptdev, GPU_INT_CLEAR, GPU_IRQ_RESET_COMPLETED);

		/* Sky1 requires PWR_OVERRIDE1 before soft reset */
		if (of_device_is_compatible(ptdev->base.dev->of_node, "cix,sky1-mali")) {
			gpu_write(ptdev, GPU_PWR_KEY, GPU_PWR_KEY_UNLOCK);
			gpu_write(ptdev, GPU_PWR_OVERRIDE1, 0xFFFFFF);
		}

		gpu_write(ptdev, GPU_CMD, GPU_SOFT_RESET);
	}
	spin_unlock_irqrestore(&ptdev->gpu->reqs_lock, flags);

	if (!wait_event_timeout(ptdev->gpu->reqs_acked,
				!(ptdev->gpu->pending_reqs & GPU_IRQ_RESET_COMPLETED),
				msecs_to_jiffies(100))) {
		spin_lock_irqsave(&ptdev->gpu->reqs_lock, flags);
		if ((ptdev->gpu->pending_reqs & GPU_IRQ_RESET_COMPLETED) != 0 &&
		    !(gpu_read(ptdev, GPU_INT_RAWSTAT) & GPU_IRQ_RESET_COMPLETED))
			timedout = true;
		else
			ptdev->gpu->pending_reqs &= ~GPU_IRQ_RESET_COMPLETED;
		spin_unlock_irqrestore(&ptdev->gpu->reqs_lock, flags);
	}

	if (timedout) {
		drm_err(&ptdev->base, "Soft reset timeout");
		return -ETIMEDOUT;
	}

	return 0;
}

/**
 * panthor_gpu_suspend() - Suspend the GPU block.
 * @ptdev: Device.
 *
 * Suspend the GPU irq. This should be called last in the suspend procedure,
 * after all other blocks have been suspented.
 */
void panthor_gpu_suspend(struct panthor_device *ptdev)
{
	/* On a fast reset, simply power down the L2. */
	if (!ptdev->reset.fast)
		panthor_gpu_soft_reset(ptdev);
	else
		panthor_gpu_power_off(ptdev, L2, 1, 20000);

	panthor_gpu_irq_suspend(&ptdev->gpu->irq);
}

/**
 * panthor_gpu_resume() - Resume the GPU block.
 * @ptdev: Device.
 *
 * Resume the IRQ handler and power-on the L2-cache.
 * The FW takes care of powering the other blocks.
 */
void panthor_gpu_resume(struct panthor_device *ptdev)
{
	panthor_gpu_irq_resume(&ptdev->gpu->irq, GPU_INTERRUPTS_MASK);
	panthor_gpu_l2_power_on(ptdev);
}


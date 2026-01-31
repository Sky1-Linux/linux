# SPDX-License-Identifier: GPL-2.0

subdir-ccflags-y += -I$(src)
subdir-ccflags-y += -I$(src)/include
subdir-ccflags-y += -I$(src)/zhouyi
ccflags-y += -DKMD_VERSION=\"1.0.0\"
ccflags-$(CONFIG_ARMCHINA_NPU_SOC_SKY1) += -DCONFIG_SKY1
ccflags-$(CONFIG_PM_DEVFREQ) += -DCONFIG_ENABLE_DEVFREQ

obj-$(CONFIG_ARMCHINA_NPU) += armchina_npu.o
armchina_npu-y := aipu.o aipu_common.o aipu_io.o aipu_irq.o  \
			aipu_job_manager.o aipu_mm.o aipu_dma_buf.o aipu_priv.o \
			aipu_tcb.o

include $(src)/zhouyi/Makefile
include $(src)/sky1/Makefile
include $(src)/default/Makefile
include $(src)/r329/Makefile
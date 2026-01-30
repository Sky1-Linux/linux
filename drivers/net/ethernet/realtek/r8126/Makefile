# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_R8126) += r8126.o
r8126-objs := r8126_n.o rtl_eeprom.o rtltool.o

ccflags-y += -DCONFIG_R8126_NAPI
ccflags-y += -DCONFIG_R8126_VLAN
ccflags-y += -DENABLE_S5WOL
ccflags-y += -DENABLE_EEE
ccflags-y += -DENABLE_TX_NO_CLOSE
ccflags-y += -DENABLE_GIGA_LITE
ccflags-y += -DCONFIG_SOC_LAN
ccflags-y += -DCONFIG_ASPM

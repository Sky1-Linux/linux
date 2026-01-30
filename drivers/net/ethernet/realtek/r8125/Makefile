# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_R8125) += r8125.o
r8125-objs := r8125_n.o rtl_eeprom.o rtltool.o

ccflags-y += -DCONFIG_R8125_NAPI
ccflags-y += -DCONFIG_R8125_VLAN
ccflags-y += -DENABLE_S5WOL
ccflags-y += -DENABLE_EEE
ccflags-y += -DENABLE_TX_NO_CLOSE
ccflags-y += -DENABLE_GIGA_LITE
ccflags-y += -DCONFIG_SOC_LAN
ccflags-y += -DCONFIG_ASPM

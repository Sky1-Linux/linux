/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * CIX CPU IPA power monitoring interface
 *
 * Copyright 2024 Cix Technology Group Co., Ltd.
 */

#ifndef __LINUX_CIX_CPU_IPA_H
#define __LINUX_CIX_CPU_IPA_H

#include <linux/cpumask.h>

#if IS_ENABLED(CONFIG_CIX_CPU_IPA)

int cix_get_static_power_cpus(const struct cpumask *cpus);
int cix_get_dynamic_power_cpus(const struct cpumask *cpus);

#else

static inline int cix_get_static_power_cpus(const struct cpumask *cpus)
{
	return 0;
}

static inline int cix_get_dynamic_power_cpus(const struct cpumask *cpus)
{
	return 0;
}

#endif /* CONFIG_CIX_CPU_IPA */

#endif /* __LINUX_CIX_CPU_IPA_H */

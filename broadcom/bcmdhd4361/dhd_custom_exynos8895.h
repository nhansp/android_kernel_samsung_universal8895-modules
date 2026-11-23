/* SPDX-License-Identifier: GPL-2.0-only */
/* Entry points the Exynos 8895 platform glue calls in the dhd driver */
#ifndef _DHD_CUSTOM_EXYNOS8895_H_
#define _DHD_CUSTOM_EXYNOS8895_H_

int dhd_module_init(void);
void dhd_module_exit(void);
int dhd_init_wlan_mem(void);
void dhd_exit_wlan_mem(void);
void *dhd_wlan_mem_prealloc(int section, unsigned long size);

#endif /* _DHD_CUSTOM_EXYNOS8895_H_ */

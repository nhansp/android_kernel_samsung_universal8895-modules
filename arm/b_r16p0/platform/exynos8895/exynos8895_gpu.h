/* SPDX-License-Identifier: GPL-2.0 */
/* Exynos 8895 kbase platform state, shared with the platform sysfs. */

#ifndef _EXYNOS8895_GPU_H_
#define _EXYNOS8895_GPU_H_

#include <linux/clk.h>
#include <linux/kobject.h>
#include <linux/mutex.h>
#include <linux/pm_qos.h>
#include <linux/types.h>

#include <soc/samsung/exynos8895-bus-qos.h>
#include <soc/samsung/exynos8895-mm-qos.h>

struct kbase_device;
struct kbase_pm_policy;

struct exynos8895_gpu {
	struct kbase_device *kbdev;
	struct clk *clk;
	struct mutex lock;		/* cur_rate, want_rate */
	unsigned long cur_rate, want_rate;
	u32 autosuspend_ms;
	struct exynos_bus_qos_request mif_req;
	struct mm_qos_request cl0_min_req;	/* little cluster min */
	struct mm_qos_request cl1_max_req;	/* big cluster max */

	/*
	 * Protects the sysfs fields below. Never held across a PM QoS or PM
	 * policy call: those can run the devfreq target, which takes @lock.
	 */
	struct mutex sysfs_lock;
	unsigned int *level_khz;	/* the DVFS table, ascending */
	int nlevels;			/* 0 = no OPP table, no sysfs */
	int lock_min_khz;		/* 0 = unlocked */
	int lock_min_input;		/* the value written */
	int lock_max_khz;		/* 0 = unlocked */
	int lock_max_input;		/* the value written */
	int pin_khz;			/* the "clock" node, 0 = off */
	const struct kbase_pm_policy *pin_policy;	/* restored by pin_khz = 0 */
	struct dev_pm_qos_request min_qos;
	struct dev_pm_qos_request max_qos;
	struct dev_pm_qos_request pin_min_qos;
	struct dev_pm_qos_request pin_max_qos;
	struct kobject *gpu_kobj;			/* /sys/kernel/gpu */
	bool sysfs_ready;
};

int exynos8895_gpu_create_sysfs(struct exynos8895_gpu *gpu);
void exynos8895_gpu_delete_sysfs(struct exynos8895_gpu *gpu);

#endif /* _EXYNOS8895_GPU_H_ */

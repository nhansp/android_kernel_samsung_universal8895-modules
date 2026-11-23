/* SPDX-License-Identifier: GPL-2.0 */
/* Exynos 8895 kbase platform state */

#ifndef _EXYNOS8895_GPU_H_
#define _EXYNOS8895_GPU_H_

#include <linux/clk.h>
#include <linux/mutex.h>
#include <linux/types.h>

#include <soc/samsung/exynos8895-bus-qos.h>
#include <soc/samsung/exynos8895-mm-qos.h>

struct kbase_device;

struct exynos8895_gpu {
	struct kbase_device *kbdev;
	struct clk *clk;
	struct mutex lock;		/* cur_rate, want_rate */
	unsigned long cur_rate, want_rate;
	u32 autosuspend_ms;
	struct exynos_bus_qos_request mif_req;
	struct mm_qos_request cl0_min_req;	/* little cluster min */
	struct mm_qos_request cl1_max_req;	/* big cluster max */
};

#endif /* _EXYNOS8895_GPU_H_ */

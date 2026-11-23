// SPDX-License-Identifier: GPL-2.0
/*
 * Exynos 8895 (Mali-G71) kbase platform: pd_g3d through genpd, the G3D clock
 * through ACPM, OPPs from the DT, no regulator (the APM owns the G3D rail).
 */

#include <linux/clk.h>
#include <linux/devfreq.h>
#include <linux/limits.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pm_opp.h>
#include <linux/pm_runtime.h>
#include <linux/seq_file.h>

#include <dt-bindings/clock/samsung,exynos8895-acpm.h>
#include <soc/samsung/exynos8895-mm-qos.h>

#include <mali_kbase.h>
#include <mali_kbase_defs.h>
#include <mali_kbase_config.h>
#include "mali_kbase_config_platform.h"
#include "mali_kbase_exynos8895.h"
#include "exynos8895_gpu.h"

/* Samsung's gpu_dvfs_start_clock */
#define EXYNOS8895_G3D_START_HZ		260000000UL

/*
 * Samsung's gpu_dvfs_table from gpu_min_clock to gpu_max_clock: the load
 * thresholds and down stay count of each level, then its MIF, little cluster
 * minimum and big cluster maximum votes.
 */
static const struct devfreq_table_interactive_level exynos8895_gpu_levels[] = {
	{ 260000000UL, 78, 85, 1 },
	{ 338000000UL, 78, 85, 1 },
	{ 385000000UL, 78, 85, 1 },
	{ 455000000UL, 78, 85, 1 },
	{ 546000000UL, 78, 99, 1 },
};

struct exynos8895_gpu_qos {
	unsigned int khz;
	s32 mif_khz;
	s32 cl0_min_khz;
	s32 cl1_max_khz;
};

static const struct exynos8895_gpu_qos exynos8895_gpu_qos_tbl[] = {
	{ 260000,  421000,       0, INT_MAX },
	{ 338000, 1014000,  949000, INT_MAX },
	{ 385000, 1352000, 1248000, INT_MAX },
	{ 455000, 1540000, 1456000, 2002000 },
	{ 546000, 1794000, 1690000, 2002000 },
};

static_assert(ARRAY_SIZE(exynos8895_gpu_levels) == ARRAY_SIZE(exynos8895_gpu_qos_tbl));

/* Samsung's interactive_info: highspeed clock, load and delay */
struct devfreq_table_interactive_data exynos8895_gpu_dvfs_data = {
	.levels = exynos8895_gpu_levels,
	.nlevels = ARRAY_SIZE(exynos8895_gpu_levels),
	.highspeed_hz = 455000000UL,
	.highspeed_load = 95,
	.highspeed_delay = 0,
	.base_level = -1,
};

static const struct exynos8895_gpu_qos *exynos8895_gpu_qos_of(unsigned long gpu_rate_hz)
{
	unsigned long khz = gpu_rate_hz / 1000;
	int i;

	for (i = ARRAY_SIZE(exynos8895_gpu_qos_tbl) - 1; i >= 0; i--)
		if (khz >= exynos8895_gpu_qos_tbl[i].khz)
			return &exynos8895_gpu_qos_tbl[i];
	return NULL;
}

static void exynos8895_gpu_qos_apply(struct exynos8895_gpu *gpu, unsigned long rate)
{
	const struct exynos8895_gpu_qos *q = exynos8895_gpu_qos_of(rate);

	exynos_bus_qos_update_request(&gpu->mif_req, q ? q->mif_khz : 0);
	mm_qos_update_request(&gpu->cl0_min_req, q ? q->cl0_min_khz : 0);
	mm_qos_update_request(&gpu->cl1_max_req, q ? q->cl1_max_khz : INT_MAX);
}

static struct kbase_vendor_callbacks exynos8895_vendor_cb;

static struct kbase_device *exynos8895_kbdev;

/*
 * Set now if the GPU is runtime active (pd_g3d on), else at the next runtime
 * resume. Always 0, so that devfreq accounts the requested rate.
 */
int exynos8895_gpu_set_rate(struct kbase_device *kbdev, unsigned long rate)
{
	struct exynos8895_gpu *gpu = kbdev->platform_context;
	bool active, is_up;

	mutex_lock(&gpu->lock);
	is_up = rate > gpu->cur_rate;
	gpu->want_rate = rate;
	active = (pm_runtime_get_if_active(kbdev->dev) > 0);
	mutex_unlock(&gpu->lock);

	if (active) {
		if (is_up)
			exynos8895_gpu_qos_apply(gpu, rate);
		mutex_lock(&gpu->lock);
		if (!clk_set_rate(gpu->clk, rate))
			gpu->cur_rate = rate;
		mutex_unlock(&gpu->lock);
		if (!is_up)
			exynos8895_gpu_qos_apply(gpu, rate);
		pm_runtime_put_autosuspend(kbdev->dev);
	}

	return 0;
}

static int exynos8895_platform_init(struct kbase_device *kbdev)
{
	struct device *dev = kbdev->dev;
	struct exynos8895_gpu *gpu;
	int ret;

	gpu = devm_kzalloc(dev, sizeof(*gpu), GFP_KERNEL);
	if (!gpu)
		return -ENOMEM;

	gpu->kbdev = kbdev;
	mutex_init(&gpu->lock);

	gpu->clk = kbdev->clock;
	if (!gpu->clk) {
		gpu->clk = devm_clk_get(dev, "clk_mali");
		if (IS_ERR(gpu->clk))
			return dev_err_probe(dev, PTR_ERR(gpu->clk),
					     "exynos8895: no g3d clock\n");
	}

	gpu->autosuspend_ms = AUTO_SUSPEND_DELAY;
	of_property_read_u32(dev->of_node, "samsung,autosuspend-delay-ms",
			     &gpu->autosuspend_ms);

	/* pd_g3d is on: genpd powered it at attach, runtime PM is not enabled yet */
	gpu->want_rate = EXYNOS8895_G3D_START_HZ;
	ret = clk_set_rate(gpu->clk, gpu->want_rate);
	if (ret) {
		dev_warn(dev, "exynos8895: initial g3d rate: %d\n", ret);
		gpu->cur_rate = 0;	/* the next runtime resume retries */
	} else {
		gpu->cur_rate = gpu->want_rate;
	}

	kbdev->platform_context = gpu;
	kbdev->vendor_callbacks = &exynos8895_vendor_cb;
	exynos8895_kbdev = kbdev;

	exynos_bus_qos_add_request(&gpu->mif_req, EXYNOS_BUS_QOS_MIF, 0);
	mm_qos_add_request(&gpu->cl0_min_req, MM_QOS_CL0_MIN, 0);
	mm_qos_add_request(&gpu->cl1_max_req, MM_QOS_CL1_MAX, INT_MAX);

	dev_info(dev, "exynos8895: g3d %lu Hz via ACPM idx %d, %d OPPs, autosuspend %u ms\n",
		 clk_get_rate(gpu->clk), EXYNOS8895_CLK_ACPM_DVFS_G3D,
		 dev_pm_opp_get_opp_count(dev), gpu->autosuspend_ms);

	return 0;
}

static void exynos8895_platform_term(struct kbase_device *kbdev)
{
	struct exynos8895_gpu *gpu = kbdev->platform_context;

	if (gpu) {
		mm_qos_remove_request(&gpu->cl1_max_req);
		mm_qos_remove_request(&gpu->cl0_min_req);
		exynos_bus_qos_remove_request(&gpu->mif_req);
	}
	exynos8895_kbdev = NULL;
	kbdev->platform_context = NULL;
}

struct kbase_platform_funcs_conf exynos8895_platform_funcs = {
	.platform_init_func = exynos8895_platform_init,
	.platform_term_func = exynos8895_platform_term,
};

static int exynos8895_pm_power_on(struct kbase_device *kbdev)
{
	int err = pm_runtime_get_sync(kbdev->dev);

	/* the usage count is kept on failure: exynos8895_pm_power_off() drops it */
	if (err < 0) {
		dev_err(kbdev->dev, "pm_runtime_get_sync: %d\n", err);
		return 1;
	}

	return err == 1 ? 0 : 1;
}

static void exynos8895_pm_power_off(struct kbase_device *kbdev)
{
	pm_runtime_mark_last_busy(kbdev->dev);
	pm_runtime_put_autosuspend(kbdev->dev);
}

static int exynos8895_pm_runtime_init(struct kbase_device *kbdev)
{
	struct exynos8895_gpu *gpu = kbdev->platform_context;

	pm_runtime_set_autosuspend_delay(kbdev->dev, gpu->autosuspend_ms);
	pm_runtime_use_autosuspend(kbdev->dev);

	pm_runtime_set_active(kbdev->dev);
	pm_runtime_enable(kbdev->dev);

	if (!pm_runtime_enabled(kbdev->dev)) {
		dev_warn(kbdev->dev, "pm_runtime not enabled");
		return -ENOSYS;
	}

	return 0;
}

static void exynos8895_pm_runtime_term(struct kbase_device *kbdev)
{
	pm_runtime_disable(kbdev->dev);
}

/* pd_g3d is on again: apply the rate devfreq asked for while it was off */
static int exynos8895_pm_runtime_on(struct kbase_device *kbdev)
{
	struct exynos8895_gpu *gpu = kbdev->platform_context;

	exynos8895_gpu_qos_apply(gpu, gpu->want_rate);

	mutex_lock(&gpu->lock);
	if (gpu->want_rate != gpu->cur_rate &&
	    !clk_set_rate(gpu->clk, gpu->want_rate))
		gpu->cur_rate = gpu->want_rate;
	mutex_unlock(&gpu->lock);

	return 0;
}

static void exynos8895_pm_runtime_off(struct kbase_device *kbdev)
{
	struct exynos8895_gpu *gpu = kbdev->platform_context;

	exynos8895_gpu_qos_apply(gpu, 0);
}

static void exynos8895_pm_resume(struct kbase_device *kbdev)
{
	int ret = exynos8895_pm_runtime_on(kbdev);

	WARN_ON(ret);
}

static void exynos8895_pm_suspend(struct kbase_device *kbdev)
{
	exynos8895_pm_runtime_off(kbdev);
}

struct kbase_pm_callback_conf exynos8895_pm_callbacks = {
	.power_on_callback = exynos8895_pm_power_on,
	.power_off_callback = exynos8895_pm_power_off,
	.power_suspend_callback = exynos8895_pm_suspend,
	.power_resume_callback = exynos8895_pm_resume,
	.power_runtime_init_callback = exynos8895_pm_runtime_init,
	.power_runtime_term_callback = exynos8895_pm_runtime_term,
	.power_runtime_on_callback = exynos8895_pm_runtime_on,
	.power_runtime_off_callback = exynos8895_pm_runtime_off,
};

/* Samsung's CL/VK boost behind ioctls 42/43 is not supported */
int gpu_vendor_dispatch(struct kbase_context *kctx, u32 flags)
{
	return 0;
}

int gpu_is_power_on(void)
{
	struct kbase_device *kbdev = exynos8895_kbdev;

	return kbdev ? pm_runtime_active(kbdev->dev) : 0;
}

struct kbase_device *gpu_get_device_structure(void)
{
	return exynos8895_kbdev;
}

int gpu_memory_seq_show(struct seq_file *sfile, void *data)
{
	struct list_head *entry;
	const struct list_head *kbdev_list;

	kbdev_list = kbase_dev_list_get();
	list_for_each(entry, kbdev_list) {
		struct kbase_device *kbdev;
		struct kbasep_kctx_list_element *element;

		kbdev = list_entry(entry, struct kbase_device, entry);
		seq_printf(sfile, "%-16s  %10u\n", kbdev->devname,
			   atomic_read(&kbdev->memdev.used_pages));
		mutex_lock(&kbdev->kctx_list_lock);
		list_for_each_entry(element, &kbdev->kctx_list, link)
			seq_printf(sfile, "  %s-0x%p %10u\n", "kctx",
				   element->kctx,
				   atomic_read(&element->kctx->used_pages));
		mutex_unlock(&kbdev->kctx_list_lock);
	}
	kbase_dev_list_put(kbdev_list);
	return 0;
}

// SPDX-License-Identifier: GPL-2.0
/*
 * Exynos 8895 kbase platform sysfs: Samsung's GPU DVFS nodes, which the power
 * HAL writes, on top of kbase devfreq. Every clock here is in kHz.
 */

#include <linux/devfreq.h>
#include <linux/err.h>
#include <linux/kobject.h>
#include <linux/math64.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include <mali_kbase.h>
#include <mali_kbase_defs.h>
#include <mali_kbase_config.h>
#include "mali_kbase_config_platform.h"
#include "mali_kbase_exynos8895.h"
#include "exynos8895_gpu.h"

static struct exynos8895_gpu *exynos8895_gpu_kobj;

#define HZ_PER_KHZ_UL	1000UL

#define gpu_err(gpu, fmt, ...) dev_err((gpu)->kbdev->dev, fmt, ##__VA_ARGS__)

/* platform_context is set before the files are created and cleared after */
static struct exynos8895_gpu *exynos8895_gpu_of_dev(struct device *dev)
{
	struct kbase_device *kbdev = dev_get_drvdata(dev);

	return kbdev ? (struct exynos8895_gpu *)kbdev->platform_context : NULL;
}

static int exynos8895_gpu_read_levels(struct exynos8895_gpu *gpu)
{
	struct device *dev = gpu->kbdev->dev;
	struct dev_pm_opp *opp;
	unsigned long hz;
	int count, i;

	count = dev_pm_opp_get_opp_count(dev);
	if (count < 1)
		return count < 0 ? count : -ENODEV;

	gpu->level_khz = devm_kcalloc(dev, count, sizeof(*gpu->level_khz),
				      GFP_KERNEL);
	if (!gpu->level_khz)
		return -ENOMEM;

	hz = 0;
	for (i = 0; i < count; i++) {
		opp = dev_pm_opp_find_freq_ceil(dev, &hz);
		if (IS_ERR(opp))
			break;
		gpu->level_khz[i] = hz / HZ_PER_KHZ_UL;
		dev_pm_opp_put(opp);
		if (!hz)
			break;
		hz++;
	}
	gpu->nlevels = i;

	return i ? 0 : -ENODEV;
}

static int exynos8895_gpu_get_level(struct exynos8895_gpu *gpu, int clock)
{
	int i;

	if (clock < (int)gpu->level_khz[0] ||
	    clock > (int)gpu->level_khz[gpu->nlevels - 1])
		return -1;

	for (i = 0; i < gpu->nlevels; i++)
		if ((int)gpu->level_khz[i] == clock)
			return i;

	return -1;
}

static int exynos8895_gpu_level_clock(struct exynos8895_gpu *gpu, int clock)
{
	int i;

	for (i = gpu->nlevels - 1; i >= 0; i--)
		if (clock - (int)gpu->level_khz[i] >= 0)
			return (int)gpu->level_khz[i];

	return -1;
}

/* 0 while the GPU is not runtime active, as Samsung's nodes report */
static int exynos8895_gpu_cur_clock(struct exynos8895_gpu *gpu)
{
	int khz = 0;

	if (gpu_is_power_on()) {
		mutex_lock(&gpu->lock);
		khz = gpu->cur_rate / HZ_PER_KHZ_UL;
		mutex_unlock(&gpu->lock);
	}

	return khz;
}

static int exynos8895_gpu_utilization(struct exynos8895_gpu *gpu)
{
	struct devfreq *devfreq = gpu->kbdev->devfreq;
	unsigned int busy, total;
	u64 ret;

	if (!devfreq)
		return -1;

	mutex_lock(&devfreq->lock);
	busy = devfreq->last_status.busy_time;
	total = devfreq->last_status.total_time;
	mutex_unlock(&devfreq->lock);

	if (!total)
		return -1;

	ret = div64_u64((u64)busy * 100, total);

	return ret > 100 ? 100 : (int)ret;
}

/*
 * A lock is a devfreq min or max frequency PM QoS request. The value is rounded
 * down to a level; "0" and the extreme level of the table unlock.
 */
static int exynos8895_gpu_set_lock(struct exynos8895_gpu *gpu, int khz,
				   bool is_min)
{
	int clock;

	if (khz == 0) {
		clock = 0;
	} else {
		clock = exynos8895_gpu_level_clock(gpu, khz);
		if (exynos8895_gpu_get_level(gpu, clock) < 0) {
			gpu_err(gpu, "%s_lock: invalid clock value (%d)\n",
				is_min ? "dvfs_min" : "dvfs_max", khz);
			return -ENOENT;
		}

		if (is_min && clock == (int)gpu->level_khz[0])
			clock = 0;
		if (!is_min && clock == (int)gpu->level_khz[gpu->nlevels - 1])
			clock = 0;
	}

	mutex_lock(&gpu->sysfs_lock);
	if (is_min) {
		gpu->lock_min_input = khz;
		gpu->lock_min_khz = clock;
	} else {
		gpu->lock_max_input = khz;
		gpu->lock_max_khz = clock;
	}
	mutex_unlock(&gpu->sysfs_lock);

	return dev_pm_qos_update_request(is_min ? &gpu->min_qos : &gpu->max_qos,
					clock ? clock :
					(is_min ? PM_QOS_MIN_FREQUENCY_DEFAULT_VALUE :
						  PM_QOS_MAX_FREQUENCY_DEFAULT_VALUE));
}

static int exynos8895_gpu_set_lock_min(struct exynos8895_gpu *gpu, int khz)
{
	return exynos8895_gpu_set_lock(gpu, khz, true);
}

static int exynos8895_gpu_set_lock_max(struct exynos8895_gpu *gpu, int khz)
{
	return exynos8895_gpu_set_lock(gpu, khz, false);
}

/*
 * A non-zero clock pins the rate with a min and max request pair, which wins
 * over the locks, and the always_on power policy; "0" restores the policy.
 */
static int exynos8895_gpu_set_pin(struct exynos8895_gpu *gpu, int khz)
{
	const struct kbase_pm_policy *const *policy_list;
	const struct kbase_pm_policy *policy = NULL;
	int policy_count, i, ret;

	if (khz == 0) {
		mutex_lock(&gpu->sysfs_lock);
		if (gpu->pin_khz) {
			policy = gpu->pin_policy;
			gpu->pin_policy = NULL;
			gpu->pin_khz = 0;
		}
		mutex_unlock(&gpu->sysfs_lock);

		if (policy)
			kbase_pm_set_policy(gpu->kbdev, policy);

		ret = dev_pm_qos_update_request(&gpu->pin_min_qos,
						PM_QOS_MIN_FREQUENCY_DEFAULT_VALUE);
		if (ret < 0)
			return ret;

		return dev_pm_qos_update_request(&gpu->pin_max_qos,
						 PM_QOS_MAX_FREQUENCY_DEFAULT_VALUE);
	}

	if (exynos8895_gpu_get_level(gpu, khz) < 0) {
		gpu_err(gpu, "clock: invalid clock value (%d)\n", khz);
		return -ENOENT;
	}

	mutex_lock(&gpu->sysfs_lock);
	if (!gpu->pin_khz)
		gpu->pin_policy = kbase_pm_get_policy(gpu->kbdev);
	gpu->pin_khz = khz;
	mutex_unlock(&gpu->sysfs_lock);

	ret = dev_pm_qos_update_request(&gpu->pin_min_qos, khz);
	if (ret < 0)
		return ret;

	ret = dev_pm_qos_update_request(&gpu->pin_max_qos, khz);
	if (ret < 0)
		return ret;

	policy_count = kbase_pm_list_policies(&policy_list);
	for (i = 0; i < policy_count; i++) {
		if (sysfs_streq(policy_list[i]->name, "always_on")) {
			kbase_pm_set_policy(gpu->kbdev, policy_list[i]);
			return 0;
		}
	}

	gpu_err(gpu, "clock: no always_on power policy, rate pinned by QoS only\n");

	return 0;
}

static ssize_t exynos8895_gpu_store_int(struct exynos8895_gpu *gpu,
					const char *buf, size_t count,
					int (*set)(struct exynos8895_gpu *, int),
					const char *name)
{
	int khz, ret;

	if (count && buf[count - 1] == '\n')
		count--;
	if (!count)
		return -EINVAL;

	khz = 0;
	if (kstrtoint(buf, 0, &khz)) {
		gpu_err(gpu, "%s: invalid value\n", name);
		return -ENOENT;
	}

	ret = set(gpu, khz);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t dvfs_table_show(struct device *dev, struct device_attribute *attr,
			       char *buf)
{
	struct exynos8895_gpu *gpu = exynos8895_gpu_of_dev(dev);
	ssize_t ret = 0;
	int i;

	for (i = gpu->nlevels - 1; i >= 0; i--)
		ret += sysfs_emit_at(buf, ret, " %u", gpu->level_khz[i]);
	ret += sysfs_emit_at(buf, ret, "\n\n");

	return ret;
}
static DEVICE_ATTR_RO(dvfs_table);

/* "<lock> / <value written>", or "-1" while unlocked */
static ssize_t dvfs_min_lock_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct exynos8895_gpu *gpu = exynos8895_gpu_of_dev(dev);
	int khz, input;

	mutex_lock(&gpu->sysfs_lock);
	khz = gpu->lock_min_khz;
	input = gpu->lock_min_input;
	mutex_unlock(&gpu->sysfs_lock);

	if (khz > 0)
		return sysfs_emit(buf, "%d / %d\n", khz, input);

	return sysfs_emit(buf, "-1\n");
}

static ssize_t dvfs_min_lock_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct exynos8895_gpu *gpu = exynos8895_gpu_of_dev(dev);

	return exynos8895_gpu_store_int(gpu, buf, count,
					exynos8895_gpu_set_lock_min,
					"dvfs_min_lock");
}
static DEVICE_ATTR_RW(dvfs_min_lock);

static ssize_t dvfs_max_lock_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct exynos8895_gpu *gpu = exynos8895_gpu_of_dev(dev);
	int khz, input;

	mutex_lock(&gpu->sysfs_lock);
	khz = gpu->lock_max_khz;
	input = gpu->lock_max_input;
	mutex_unlock(&gpu->sysfs_lock);

	if (khz > 0)
		return sysfs_emit(buf, "%d / %d\n", khz, input);

	return sysfs_emit(buf, "-1\n");
}

static ssize_t dvfs_max_lock_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct exynos8895_gpu *gpu = exynos8895_gpu_of_dev(dev);

	return exynos8895_gpu_store_int(gpu, buf, count,
					exynos8895_gpu_set_lock_max,
					"dvfs_max_lock");
}
static DEVICE_ATTR_RW(dvfs_max_lock);

static ssize_t clock_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct exynos8895_gpu *gpu = exynos8895_gpu_of_dev(dev);

	return sysfs_emit(buf, "%d\n", exynos8895_gpu_cur_clock(gpu));
}

static ssize_t clock_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct exynos8895_gpu *gpu = exynos8895_gpu_of_dev(dev);

	return exynos8895_gpu_store_int(gpu, buf, count,
					exynos8895_gpu_set_pin, "clock");
}
static DEVICE_ATTR_RW(clock);

static ssize_t utilization_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct exynos8895_gpu *gpu = exynos8895_gpu_of_dev(dev);

	return sysfs_emit(buf, "%d\n",
			  gpu_is_power_on() * exynos8895_gpu_utilization(gpu));
}
static DEVICE_ATTR_RO(utilization);

/* the last target, not 0 while the GPU is off: the power HAL drops its boost on 0 */
static ssize_t cur_freq_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct exynos8895_gpu *gpu = exynos8895_gpu_of_dev(dev);

	return sysfs_emit(buf, "%lu\n", gpu->kbdev->current_nominal_freq / 1000);
}
static DEVICE_ATTR_RO(cur_freq);

static ssize_t capacity_headroom_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n",
			  READ_ONCE(exynos8895_gpu_dvfs_data.capacity_headroom_khz));
}

static ssize_t capacity_headroom_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	unsigned int khz;

	if (kstrtouint(buf, 0, &khz))
		return -EINVAL;
	WRITE_ONCE(exynos8895_gpu_dvfs_data.capacity_headroom_khz, khz);
	return count;
}
static DEVICE_ATTR_RW(capacity_headroom);

static const struct attribute *exynos8895_gpu_attrs[] = {
	&dev_attr_dvfs_table.attr,
	&dev_attr_dvfs_min_lock.attr,
	&dev_attr_dvfs_max_lock.attr,
	&dev_attr_clock.attr,
	&dev_attr_utilization.attr,
	&dev_attr_cur_freq.attr,
	&dev_attr_capacity_headroom.attr,
	NULL,
};

static ssize_t kernel_show_gpu_min_clock(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *buf)
{
	int khz;

	mutex_lock(&exynos8895_gpu_kobj->sysfs_lock);
	khz = exynos8895_gpu_kobj->lock_min_khz ?
		exynos8895_gpu_kobj->lock_min_khz :
		(int)exynos8895_gpu_kobj->level_khz[0];
	mutex_unlock(&exynos8895_gpu_kobj->sysfs_lock);

	return sysfs_emit(buf, "%d\n", khz);
}

static ssize_t kernel_set_gpu_min_clock(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	return exynos8895_gpu_store_int(exynos8895_gpu_kobj, buf, count,
					exynos8895_gpu_set_lock_min,
					"gpu_min_clock");
}

static ssize_t kernel_show_gpu_max_clock(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *buf)
{
	int khz;

	mutex_lock(&exynos8895_gpu_kobj->sysfs_lock);
	khz = exynos8895_gpu_kobj->lock_max_khz ?
		exynos8895_gpu_kobj->lock_max_khz :
		(int)exynos8895_gpu_kobj->level_khz[exynos8895_gpu_kobj->nlevels - 1];
	mutex_unlock(&exynos8895_gpu_kobj->sysfs_lock);

	return sysfs_emit(buf, "%d\n", khz);
}

static ssize_t kernel_set_gpu_max_clock(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	return exynos8895_gpu_store_int(exynos8895_gpu_kobj, buf, count,
					exynos8895_gpu_set_lock_max,
					"gpu_max_clock");
}

static ssize_t kernel_show_gpu_clock(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n",
			  exynos8895_gpu_cur_clock(exynos8895_gpu_kobj));
}

static ssize_t kernel_show_gpu_freq_table(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  char *buf)
{
	struct exynos8895_gpu *gpu = exynos8895_gpu_kobj;
	ssize_t ret = 0;
	int i;

	for (i = 0; i < gpu->nlevels; i++)
		ret += sysfs_emit_at(buf, ret, "%u ", gpu->level_khz[i]);
	ret += sysfs_emit_at(buf, ret, "\n");

	return ret;
}

static ssize_t kernel_show_gpu_busy(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%3d%%\n",
			  exynos8895_gpu_utilization(exynos8895_gpu_kobj));
}

/* only the Mali-G71 of this SoC is named; the fallback is Samsung's */
static ssize_t kernel_show_gpu_model(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	struct kbase_device *kbdev = exynos8895_gpu_kobj->kbdev;
	u32 gpu_id = kbdev->gpu_props.props.raw_props.gpu_id;
	unsigned int product_id = gpu_id >> GPU_ID_VERSION_PRODUCT_ID_SHIFT;
	const char *name = "(Unknown Mali GPU)";

	if (GPU_ID_IS_NEW_FORMAT(product_id) &&
	    product_id ==
	    (GPU_ID2_PRODUCT_TMIX >> GPU_ID_VERSION_PRODUCT_ID_SHIFT))
		name = "Mali-G71";

	return sysfs_emit(buf, "%s\n", name);
}

static struct kobj_attribute gpu_min_clock_attribute =
	__ATTR(gpu_min_clock, 0644, kernel_show_gpu_min_clock,
	       kernel_set_gpu_min_clock);

static struct kobj_attribute gpu_max_clock_attribute =
	__ATTR(gpu_max_clock, 0644, kernel_show_gpu_max_clock,
	       kernel_set_gpu_max_clock);

static struct kobj_attribute gpu_clock_attribute =
	__ATTR(gpu_clock, 0444, kernel_show_gpu_clock, NULL);

static struct kobj_attribute gpu_freq_table_attribute =
	__ATTR(gpu_freq_table, 0444, kernel_show_gpu_freq_table, NULL);

static struct kobj_attribute gpu_busy_attribute =
	__ATTR(gpu_busy, 0444, kernel_show_gpu_busy, NULL);

static struct kobj_attribute gpu_model_attribute =
	__ATTR(gpu_model, 0444, kernel_show_gpu_model, NULL);

static struct attribute *exynos8895_kernel_attrs[] = {
	&gpu_min_clock_attribute.attr,
	&gpu_max_clock_attribute.attr,
	&gpu_clock_attribute.attr,
	&gpu_freq_table_attribute.attr,
	&gpu_busy_attribute.attr,
	&gpu_model_attribute.attr,
	NULL,
};

static const struct attribute_group exynos8895_kernel_attr_group = {
	.attrs = exynos8895_kernel_attrs,
};

int exynos8895_gpu_create_sysfs(struct exynos8895_gpu *gpu)
{
	struct device *dev = gpu->kbdev->dev;
	int ret, i;

	gpu->sysfs_ready = false;

	ret = exynos8895_gpu_read_levels(gpu);
	if (ret) {
		dev_err(dev, "exynos8895: no DVFS table, no sysfs (%d)\n", ret);
		return 0;
	}

	ret = dev_pm_qos_add_request(dev, &gpu->min_qos,
				     DEV_PM_QOS_MIN_FREQUENCY,
				     PM_QOS_MIN_FREQUENCY_DEFAULT_VALUE);
	if (ret)
		return ret;
	ret = dev_pm_qos_add_request(dev, &gpu->max_qos,
				     DEV_PM_QOS_MAX_FREQUENCY,
				     PM_QOS_MAX_FREQUENCY_DEFAULT_VALUE);
	if (ret)
		goto err_min_qos;
	ret = dev_pm_qos_add_request(dev, &gpu->pin_min_qos,
				     DEV_PM_QOS_MIN_FREQUENCY,
				     PM_QOS_MIN_FREQUENCY_DEFAULT_VALUE);
	if (ret)
		goto err_max_qos;
	ret = dev_pm_qos_add_request(dev, &gpu->pin_max_qos,
				     DEV_PM_QOS_MAX_FREQUENCY,
				     PM_QOS_MAX_FREQUENCY_DEFAULT_VALUE);
	if (ret)
		goto err_pin_min_qos;

	for (i = 0; exynos8895_gpu_attrs[i]; i++) {
		ret = sysfs_create_file(&dev->kobj, exynos8895_gpu_attrs[i]);
		if (ret) {
			dev_err(dev, "exynos8895: sysfs %s: %d\n",
				exynos8895_gpu_attrs[i]->name, ret);
			goto err_files;
		}
	}

	exynos8895_gpu_kobj = gpu;
	gpu->gpu_kobj = kobject_create_and_add("gpu", kernel_kobj);
	if (!gpu->gpu_kobj) {
		ret = -ENOMEM;
		goto err_kobj;
	}
	ret = sysfs_create_group(gpu->gpu_kobj, &exynos8895_kernel_attr_group);
	if (ret) {
		kobject_put(gpu->gpu_kobj);
		gpu->gpu_kobj = NULL;
		goto err_kobj;
	}

	gpu->sysfs_ready = true;

	return 0;

err_kobj:
	exynos8895_gpu_kobj = NULL;
err_files:
	while (--i >= 0)
		sysfs_remove_file(&dev->kobj, exynos8895_gpu_attrs[i]);
err_pin_min_qos:
	dev_pm_qos_remove_request(&gpu->pin_min_qos);
err_max_qos:
	dev_pm_qos_remove_request(&gpu->max_qos);
err_min_qos:
	dev_pm_qos_remove_request(&gpu->min_qos);

	return ret;
}

void exynos8895_gpu_delete_sysfs(struct exynos8895_gpu *gpu)
{
	int i;

	if (!gpu->sysfs_ready)
		return;

	if (gpu->gpu_kobj) {
		sysfs_remove_group(gpu->gpu_kobj, &exynos8895_kernel_attr_group);
		kobject_put(gpu->gpu_kobj);
		gpu->gpu_kobj = NULL;
	}
	exynos8895_gpu_kobj = NULL;

	for (i = 0; exynos8895_gpu_attrs[i]; i++)
		sysfs_remove_file(&gpu->kbdev->dev->kobj,
				  exynos8895_gpu_attrs[i]);

	dev_pm_qos_remove_request(&gpu->pin_max_qos);
	dev_pm_qos_remove_request(&gpu->pin_min_qos);
	dev_pm_qos_remove_request(&gpu->max_qos);
	dev_pm_qos_remove_request(&gpu->min_qos);

	gpu->sysfs_ready = false;
}

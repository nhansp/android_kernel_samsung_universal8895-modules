/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Interfaces of the 4.4 kernel the FIMC-IS2 sources were written for, mapped
 * to their current equivalents. Kbuild includes this in every object, so the
 * kernel headers whose names the macros reuse are included first.
 */
#ifndef __FIMC_IS_COMPAT_H
#define __FIMC_IS_COMPAT_H

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/minmax.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pm_qos.h>
#include <linux/random.h>
#include <linux/sched/clock.h>
#include <linux/slab.h>
#include <linux/time32.h>
#include <linux/timekeeping.h>
#include <linux/timer.h>
#include <linux/videodev2.h>
#include <linux/vmalloc.h>
#include <soc/samsung/exynos8895-mm-qos.h>
#include "../fimc-is-fpsimd.h"

/* /dev/video100 is the first FIMC-IS node */
#define EXYNOS_VIDEONODE_FIMC_IS	100

#define IRQF_GIC_MULTI_TARGET		0

/* core.ioctl codes for the subdev ops that no longer exist */
#define FIMC_IS_SUBDEV_G_CTRL		_IOWR('V', BASE_VIDIOC_PRIVATE + 1, struct v4l2_control)
#define FIMC_IS_SUBDEV_S_CTRL		_IOWR('V', BASE_VIDIOC_PRIVATE + 2, struct v4l2_control)
#define FIMC_IS_SUBDEV_G_EXT_CTRLS _IOWR('V', BASE_VIDIOC_PRIVATE + 3, struct v4l2_ext_controls)
#define FIMC_IS_SUBDEV_S_EXT_CTRLS _IOWR('V', BASE_VIDIOC_PRIVATE + 4, struct v4l2_ext_controls)
#define FIMC_IS_SUBDEV_S_PARM		_IOWR('V', BASE_VIDIOC_PRIVATE + 5, struct v4l2_streamparm)

/* buffer flag of the vendor V4L2 core, cleared at QBUF */
#define V4L2_BUF_FLAG_USE_SYNC		0x00008000

/* subdev name length of the 4.4 V4L2 core */
#define V4L2_SUBDEV_NAME_SIZE		32

#define pr_warning(fmt, ...)		pr_warn(fmt, ##__VA_ARGS__)
#define __gpio_set_value(g, v)		gpio_set_value(g, v)

#define del_timer(t)			timer_delete(t)
#define del_timer_sync(t)		timer_delete_sync(t)

#define init_kthread_worker(w)		kthread_init_worker(w)
#define init_kthread_work(w, f)		kthread_init_work(w, f)
#define queue_kthread_work(w, k)	kthread_queue_work(w, k)
#define flush_kthread_work(k)		kthread_flush_work(k)
#define flush_kthread_worker(w)		kthread_flush_worker(w)

#define timeval __kernel_old_timeval

static inline void do_gettimeofday(struct __kernel_old_timeval *tv)
{
	struct timespec64 ts;

	ktime_get_real_ts64(&ts);
	tv->tv_sec = ts.tv_sec;
	tv->tv_usec = ts.tv_nsec / NSEC_PER_USEC;
}

/* the Samsung PM QoS classes are requests of the multimedia QoS driver */
#define pm_qos_request			mm_qos_request
#define pm_qos_add_request		mm_qos_add_request
#define pm_qos_update_request		mm_qos_update_request
#define pm_qos_remove_request		mm_qos_remove_request
#define pm_qos_request_active		mm_qos_request_active

#define PM_QOS_INTCAM_THROUGHPUT	MM_QOS_INTCAM
#define PM_QOS_CAM_THROUGHPUT		MM_QOS_CAM
#define PM_QOS_CLUSTER0_FREQ_MIN	MM_QOS_CL0_MIN
#define PM_QOS_CLUSTER0_FREQ_MAX	MM_QOS_CL0_MAX
#define PM_QOS_CLUSTER1_FREQ_MIN	MM_QOS_CL1_MIN
#define PM_QOS_CLUSTER1_FREQ_MAX	MM_QOS_CL1_MAX
#define PM_QOS_DEVICE_THROUGHPUT	MM_QOS_INT
#define PM_QOS_BUS_THROUGHPUT		MM_QOS_MIF
#define PM_QOS_CPU_ONLINE_MIN		MM_QOS_CPU_ONLINE_MIN

/* set_fs() is gone; kernel_read() and kernel_write() take kernel buffers */
typedef struct { int unused; } mm_segment_t;
#define get_fs()			((mm_segment_t){ 0 })
#define set_fs(x)			((void)(x))
#define KERNEL_DS			((mm_segment_t){ 0 })
#define vfs_read(f, b, n, p)		kernel_read(f, (void *)(b), n, p)
#define vfs_write(f, b, n, p)		kernel_write(f, (const void *)(b), n, p)

#endif /* __FIMC_IS_COMPAT_H */

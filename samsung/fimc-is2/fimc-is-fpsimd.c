// SPDX-License-Identifier: GPL-2.0-only
/*
 * FP/SIMD state around calls into the FIMC-IS libraries, which use the
 * FP/SIMD registers in task, softirq and hardirq context.
 */

#include <linux/kernel.h>
#include <linux/preempt.h>
#include <linux/interrupt.h>
#include <linux/percpu.h>
#include <linux/kthread.h>
#include <asm/fpsimd.h>
#include <asm/neon.h>
#include <asm/simd.h>

#include "fimc-is-fpsimd.h"

struct fimc_is_fp_level {
	struct user_fpsimd_state st;
	int depth;
};

static DEFINE_PER_CPU(struct fimc_is_fp_level, fp_hardirq);
static DEFINE_PER_CPU(struct fimc_is_fp_level, fp_softirq);
static DEFINE_PER_CPU(struct fimc_is_fp_level, fp_atomic);

void fimc_is_fp_enter(struct fimc_is_fp_ctx *ctx)
{
	if (in_hardirq() || in_serving_softirq() || irqs_disabled() || !preemptible()) {
		struct fimc_is_fp_level *l = in_hardirq() ? this_cpu_ptr(&fp_hardirq) :
					     in_serving_softirq() ? this_cpu_ptr(&fp_softirq) :
					     this_cpu_ptr(&fp_atomic);
		if (l->depth++ == 0)
			fpsimd_save_state(&l->st);
		ctx->mode = FP_MODE_SAVED;
		return;
	}

	WARN_ON(in_hardirq());
	if (test_thread_flag(TIF_KERNEL_FPSTATE)) {
		ctx->mode = FP_MODE_NESTED;
		return;
	}

	kernel_neon_begin(&ctx->st);
	ctx->mode = FP_MODE_KERNEL_NEON;
}

void fimc_is_fp_exit(struct fimc_is_fp_ctx *ctx)
{
	if (ctx->mode == FP_MODE_SAVED) {
		struct fimc_is_fp_level *l = in_hardirq() ? this_cpu_ptr(&fp_hardirq) :
					     in_serving_softirq() ? this_cpu_ptr(&fp_softirq) :
					     this_cpu_ptr(&fp_atomic);
		if (--l->depth == 0)
			fpsimd_load_state(&l->st);
	} else if (ctx->mode == FP_MODE_KERNEL_NEON) {
		kernel_neon_end(&ctx->st);
	}
}

int fimc_is_neon_kthread_fn(void *data)
{
	struct user_fpsimd_state st;
	int ret;

	kernel_neon_begin(&st);
	ret = kthread_worker_fn(data);
	kernel_neon_end(&st);
	return ret;
}

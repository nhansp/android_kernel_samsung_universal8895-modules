/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __FIMC_IS_FPSIMD_H__
#define __FIMC_IS_FPSIMD_H__

#include <asm/fpsimd.h>
#include <asm/neon.h>

enum fimc_is_fp_mode {
	FP_MODE_NONE = 0,
	FP_MODE_SAVED,
	FP_MODE_KERNEL_NEON,
	FP_MODE_NESTED,
};

struct fimc_is_fp_ctx {
	struct user_fpsimd_state st;
	enum fimc_is_fp_mode mode;
};

void fimc_is_fp_enter(struct fimc_is_fp_ctx *ctx);
void fimc_is_fp_exit(struct fimc_is_fp_ctx *ctx);

int fimc_is_neon_kthread_fn(void *data);

/*
 * The vendor code brackets library calls with fpsimd_get()/fpsimd_put() and
 * library interrupt handlers with fimc_is_enter/exit_lib_isr(), each pair in
 * one block, so the pair shares a context declared by the opening call.
 */
#define fpsimd_get()						\
	struct fimc_is_fp_ctx __fimc_is_fp;			\
	fimc_is_fp_enter(&__fimc_is_fp)
#define fpsimd_put()		fimc_is_fp_exit(&__fimc_is_fp)
#define fimc_is_enter_lib_isr()	fpsimd_get()
#define fimc_is_exit_lib_isr()	fpsimd_put()

#endif /* __FIMC_IS_FPSIMD_H__ */

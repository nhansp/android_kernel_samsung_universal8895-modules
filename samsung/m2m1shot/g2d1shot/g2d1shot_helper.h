/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2015 Samsung Electronics Co., Ltd.
 *	http://www.samsung.com/
 *
 * Samsung Graphics 2D driver
 */

#ifndef __EXYNOS_G2D1SHOT_HELPER_H_
#define __EXYNOS_G2D1SHOT_HELPER_H_

/*
 * g2d_debug value:
 *	0: no print
 *	1: err
 *	2: info
 *	3: perf
 *	4: oneline (simple)
 *	5: debug
 */

enum debug_level {
	DBG_NO,
	DBG_ERR,
	DBG_INFO,
	DBG_PERF,
	DBG_ONELINE,
	DBG_DEBUG,
};

#define g2d_print(level, fmt, args...)					\
do {									\
	if (g2d_debug >= level)						\
		pr_info("[%s:%d] " fmt, __func__, __LINE__, ##args);	\
} while (0)

#define g2d_err(fmt, args...)	g2d_print(DBG_ERR, fmt, ##args)
#define g2d_info(fmt, args...)	g2d_print(DBG_INFO, fmt, ##args)
#define g2d_debug(fmt, args...)	g2d_print(DBG_DEBUG, fmt, ##args)
#define g2d_dbg_begin()		g2d_print(DBG_DEBUG, "%s\n", "++ begin")
#define g2d_dbg_end()		g2d_print(DBG_DEBUG, "%s\n", "-- end")
#define g2d_dbg_end_err()	g2d_print(DBG_DEBUG, "%s\n", "-- end, but err")

void g2d_dump_regs(struct g2d1shot_dev *g2d_dev);
int g2d_disp_info(struct m2m1shot2_context *ctx);
#endif /* __EXYNOS_G2D1SHOT_HELPER_H_ */

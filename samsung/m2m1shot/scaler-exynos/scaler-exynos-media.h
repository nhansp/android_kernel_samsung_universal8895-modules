/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * The Exynos V4L2 controls, formats and video node number the scaler uses,
 * from the vendor videodev2_exynos_media.h and videonode.h.
 */

#ifndef SCALER_EXYNOS_MEDIA_H_
#define SCALER_EXYNOS_MEDIA_H_

#include <linux/videodev2.h>

/* CID base for Exynos controls (USER_CLASS) */
#define V4L2_CID_EXYNOS_BASE		(V4L2_CTRL_CLASS_USER | 0x2000)

/* for color space conversion equation selection */
#define V4L2_CID_CSC_EQ			(V4L2_CID_EXYNOS_BASE + 101)
#define V4L2_CID_CSC_RANGE		(V4L2_CID_EXYNOS_BASE + 102)

/* for DRM playback scenario */
#define V4L2_CID_CONTENT_PROTECTION	(V4L2_CID_EXYNOS_BASE + 201)

/*
 *	V I D E O   I M A G E   F O R M A T
 */
/* 1 plane -- one Y, one Cb + Cr interleaved, non contiguous  */
#define V4L2_PIX_FMT_NV12N		v4l2_fourcc('N', 'N', '1', '2')

/* 1 plane -- one Y, one Cb, one Cr, non contiguous */
#define V4L2_PIX_FMT_YUV420N		v4l2_fourcc('Y', 'N', '1', '2')

/* 1 plane -- 8bit Y, 2bit Y, 8bit Cb + Cr interleaved, 2bit Cb + Cr interleaved, non contiguous */
#define V4L2_PIX_FMT_NV12N_10B		v4l2_fourcc('B', 'N', '1', '2')

/* helper macros */
#ifndef __ALIGN_UP
#define __ALIGN_UP(x, a)		(((x) + ((a) - 1)) & ~((a) - 1))
#endif

#define NV12N_Y_SIZE(w, h)		(__ALIGN_UP((w), 16) * __ALIGN_UP((h), 16) + 256)
#define NV12N_CBCR_SIZE(w, h)		\
	(__ALIGN_UP((__ALIGN_UP((w), 16) * (__ALIGN_UP((h), 16) / 2) + 256), 16))
#define NV12N_CBCR_BASE(base, w, h)	((base) + NV12N_Y_SIZE((w), (h)))
#define NV12N_10B_Y_2B_SIZE(w, h)	((__ALIGN_UP((w) / 4, 16) * __ALIGN_UP((h), 16) + 64))
#define NV12N_10B_CBCR_2B_SIZE(w, h)	\
	((__ALIGN_UP((w) / 4, 16) * (__ALIGN_UP((h), 16) / 2) + 64))
#define NV12N_10B_CBCR_BASE(base, w, h)	\
	((base) + NV12N_Y_SIZE((w), (h)) + NV12N_10B_Y_2B_SIZE((w), (h)))

#define YUV420N_Y_SIZE(w, h)		(__ALIGN_UP((w), 16) * __ALIGN_UP((h), 16) + 256)
#define YUV420N_CB_SIZE(w, h)		\
	(__ALIGN_UP((__ALIGN_UP((w) / 2, 16) * (__ALIGN_UP((h), 16) / 2) + 256), 16))
#define YUV420N_CR_SIZE(w, h)		\
	(__ALIGN_UP((__ALIGN_UP((w) / 2, 16) * (__ALIGN_UP((h), 16) / 2) + 256), 16))
#define YUV420N_CB_BASE(base, w, h)	((base) + YUV420N_Y_SIZE((w), (h)))
#define YUV420N_CR_BASE(base, w, h)	\
	(YUV420N_CB_BASE((base), (w), (h)) + YUV420N_CB_SIZE((w), (h)))

/* video node number of scaler instance x */
#define EXYNOS_VIDEONODE_SCALER(x)		(50 + (x))

#endif /* SCALER_EXYNOS_MEDIA_H_ */

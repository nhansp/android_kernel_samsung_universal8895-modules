/*
 * drivers/media/platform/exynos/mfc/s5p_mfc_mem.h
 *
 * Copyright (c) 2016 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __S5P_MFC_MEM_H
#define __S5P_MFC_MEM_H __FILE__

#include <linux/dma-mapping.h>
#include <media/videobuf2-dma-contig.h>

#include "s5p_mfc_common.h"

/* Offset base used to differentiate between CAPTURE and OUTPUT
*  while mmaping */
#define DST_QUEUE_OFF_BASE      (TASK_SIZE / 2)

struct s5p_mfc_mem_cookie {
	struct device *dev;
	size_t size;
	void *vaddr;
	dma_addr_t daddr;
};

void *s5p_mfc_mem_alloc(struct device *dev, size_t size);
void s5p_mfc_mem_free(void *cookie);

static inline dma_addr_t s5p_mfc_mem_get_daddr(void *cookie)
{
	return ((struct s5p_mfc_mem_cookie *)cookie)->daddr;
}

static inline dma_addr_t s5p_mfc_mem_get_daddr_vb(
	struct vb2_buffer *v, unsigned int n)
{
	return vb2_dma_contig_plane_dma_addr(v, n);
}

static inline void *s5p_mfc_mem_get_vaddr(void *cookie)
{
	return ((struct s5p_mfc_mem_cookie *)cookie)->vaddr;
}

int s5p_mfc_mem_buf_prepare(struct vb2_buffer *vb);
void s5p_mfc_mem_buf_finish(struct vb2_buffer *vb);

const struct vb2_mem_ops *s5p_mfc_mem_ops(void);

void s5p_mfc_mem_set_cacheable(struct device *dev, bool cacheable);
void s5p_mfc_mem_clean(void *vb_priv, void *start, off_t offset,
							size_t size);
void s5p_mfc_mem_invalidate(void *vb_priv, void *start, off_t offset,
							size_t size);
int s5p_mfc_mem_clean_vb(struct vb2_buffer *vb, u32 num_planes);
int s5p_mfc_mem_inv_vb(struct vb2_buffer *vb, u32 num_planes);

int s5p_mfc_mem_get_user_shared_handle(struct s5p_mfc_ctx *ctx,
		struct mfc_user_shared_handle *handle);
int s5p_mfc_mem_cleanup_user_shared_handle(struct s5p_mfc_ctx *ctx,
		struct mfc_user_shared_handle *handle);

#endif /* __S5P_MFC_MEM_H */

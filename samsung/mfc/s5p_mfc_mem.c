/*
 * drivers/media/platform/exynos/mfc/s5p_mfc_mem.c
 *
 * Copyright (c) 2016 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/dma-buf.h>
#include <linux/slab.h>

#include "s5p_mfc_mem.h"

void *s5p_mfc_mem_alloc(struct device *dev, size_t size)
{
	struct s5p_mfc_mem_cookie *cookie;

	cookie = kzalloc(sizeof(*cookie), GFP_KERNEL);
	if (!cookie)
		return ERR_PTR(-ENOMEM);

	cookie->vaddr = dma_alloc_attrs(dev, size, &cookie->daddr, GFP_KERNEL,
					DMA_ATTR_WRITE_COMBINE);
	if (!cookie->vaddr) {
		kfree(cookie);
		return ERR_PTR(-ENOMEM);
	}
	cookie->dev = dev;
	cookie->size = size;

	return cookie;
}

void s5p_mfc_mem_free(void *cookie)
{
	struct s5p_mfc_mem_cookie *c = cookie;

	if (IS_ERR_OR_NULL(c))
		return;

	dma_free_attrs(c->dev, c->size, c->vaddr, c->daddr, DMA_ATTR_WRITE_COMBINE);
	kfree(c);
}

const struct vb2_mem_ops *s5p_mfc_mem_ops(void)
{
	return &vb2_dma_contig_memops;
}

/* vb2-dma-contig MMAP buffers are coherent, there is no cached variant */
void s5p_mfc_mem_set_cacheable(struct device *dev, bool cacheable)
{
}

/* Internal buffers are write-combined, there is nothing to clean or invalidate */
void s5p_mfc_mem_clean(void *vb_priv, void *start, off_t offset,
							size_t size)
{
}

void s5p_mfc_mem_invalidate(void *vb_priv, void *start, off_t offset,
							size_t size)
{
}

static enum dma_data_direction mfc_mem_vb_dir(struct vb2_buffer *vb)
{
	return V4L2_TYPE_IS_OUTPUT(vb->type) ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
}

/*
 * vb2 does no cache maintenance for DMABUF planes, the vendor vb2-ion synced
 * them around every use; the exporter does it in its CPU access ops.
 */
static void mfc_mem_vb_for_device(struct vb2_buffer *vb, u32 num_planes)
{
	enum dma_data_direction dir = mfc_mem_vb_dir(vb);
	unsigned int i;

	if (vb->memory != VB2_MEMORY_DMABUF)
		return;

	for (i = 0; i < num_planes && i < vb->num_planes; i++)
		if (vb->planes[i].dbuf)
			dma_buf_end_cpu_access(vb->planes[i].dbuf, dir);
}

static void mfc_mem_vb_for_cpu(struct vb2_buffer *vb, u32 num_planes,
			       enum dma_data_direction dir)
{
	unsigned int i;

	if (vb->memory != VB2_MEMORY_DMABUF)
		return;

	for (i = 0; i < num_planes && i < vb->num_planes; i++)
		if (vb->planes[i].dbuf)
			dma_buf_begin_cpu_access(vb->planes[i].dbuf, dir);
}

int s5p_mfc_mem_buf_prepare(struct vb2_buffer *vb)
{
	mfc_mem_vb_for_device(vb, vb->num_planes);

	return 0;
}

void s5p_mfc_mem_buf_finish(struct vb2_buffer *vb)
{
	mfc_mem_vb_for_cpu(vb, vb->num_planes, mfc_mem_vb_dir(vb));
}

int s5p_mfc_mem_clean_vb(struct vb2_buffer *vb, u32 num_planes)
{
	mfc_mem_vb_for_device(vb, num_planes);

	return 0;
}

int s5p_mfc_mem_inv_vb(struct vb2_buffer *vb, u32 num_planes)
{
	mfc_mem_vb_for_cpu(vb, num_planes, DMA_FROM_DEVICE);

	return 0;
}

int s5p_mfc_mem_get_user_shared_handle(struct s5p_mfc_ctx *ctx,
	struct mfc_user_shared_handle *handle)
{
	int ret;

	handle->dmabuf = dma_buf_get(handle->fd);
	if (IS_ERR(handle->dmabuf)) {
		mfc_err_ctx("Failed to import fd\n");
		ret = PTR_ERR(handle->dmabuf);
		handle->dmabuf = NULL;
		return ret;
	}

	ret = dma_buf_vmap_unlocked(handle->dmabuf, &handle->map);
	if (ret || handle->map.is_iomem) {
		mfc_err_ctx("Failed to get kernel virtual address\n");
		if (!ret)
			dma_buf_vunmap_unlocked(handle->dmabuf, &handle->map);
		dma_buf_put(handle->dmabuf);
		handle->dmabuf = NULL;
		return -EINVAL;
	}
	handle->vaddr = handle->map.vaddr;

	mfc_debug(2, "User Handle: fd = %d, virtual addr = 0x%p\n",
				handle->fd, handle->vaddr);

	return 0;
}

int s5p_mfc_mem_cleanup_user_shared_handle(struct s5p_mfc_ctx *ctx,
		struct mfc_user_shared_handle *handle)
{
	if (handle->fd == -1 || !handle->dmabuf)
		return 0;

	if (handle->vaddr)
		dma_buf_vunmap_unlocked(handle->dmabuf, &handle->map);

	dma_buf_put(handle->dmabuf);
	handle->dmabuf = NULL;
	handle->vaddr = NULL;

	return 0;
}

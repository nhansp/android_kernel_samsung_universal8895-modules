/*
 * Samsung Exynos5 SoC series FIMC-IS driver
 *
 * exynos5 fimc-is core functions
 *
 * Copyright (c) 2011 Samsung Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/firmware.h>
#include <linux/dma-mapping.h>
#include <linux/dma-buf.h>
#include <linux/scatterlist.h>
#include <linux/videodev2.h>
#include <linux/videodev2_exynos_camera.h>
#include <linux/vmalloc.h>
#include <linux/of.h>

#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-dma-contig.h>

#include "fimc-is-core.h"
#include "fimc-is-cmd.h"
#include "fimc-is-regs.h"
#include "fimc-is-err.h"
#include "fimc-is-mem.h"

/*
 * VB2 DMA-contig buffer operations
 */
static ulong fimc_is_vb2_dma_plane_kvaddr(struct fimc_is_vb2_buf *vbuf, u32 plane)
{
	return (ulong)vb2_plane_vaddr(&vbuf->vb.vb2_buf, plane);
}

static ulong fimc_is_vb2_dma_plane_cookie(struct fimc_is_vb2_buf *vbuf, u32 plane)
{
	return (ulong)vb2_dma_contig_plane_dma_addr(&vbuf->vb.vb2_buf, plane);
}

static dma_addr_t fimc_is_vb2_dma_plane_dvaddr(struct fimc_is_vb2_buf *vbuf, u32 plane)
{
	return vb2_dma_contig_plane_dma_addr(&vbuf->vb.vb2_buf, plane);
}

static void fimc_is_vb2_dma_plane_prepare(struct fimc_is_vb2_buf *vbuf, u32 plane, bool exact)
{
	struct vb2_buffer *vb = &vbuf->vb.vb2_buf;
	enum dma_data_direction dir;

	if (plane >= vb->num_planes)
		return;

	dir = V4L2_TYPE_IS_OUTPUT(vb->type) ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
	if (vb->planes[plane].dbuf)
		dma_buf_end_cpu_access(vb->planes[plane].dbuf, dir);
}

static void fimc_is_vb2_dma_plane_finish(struct fimc_is_vb2_buf *vbuf, u32 plane, bool exact)
{
	struct vb2_buffer *vb = &vbuf->vb.vb2_buf;
	enum dma_data_direction dir;

	if (plane >= vb->num_planes)
		return;

	dir = V4L2_TYPE_IS_OUTPUT(vb->type) ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
	if (vb->planes[plane].dbuf)
		dma_buf_begin_cpu_access(vb->planes[plane].dbuf, dir);
}

static void fimc_is_vb2_dma_buf_prepare(struct fimc_is_vb2_buf *vbuf, bool exact)
{
	struct vb2_buffer *vb = &vbuf->vb.vb2_buf;
	u32 plane;

	for (plane = 0; plane < vb->num_planes; plane++)
		fimc_is_vb2_dma_plane_prepare(vbuf, plane, exact);
}

static void fimc_is_vb2_dma_buf_finish(struct fimc_is_vb2_buf *vbuf, bool exact)
{
	struct vb2_buffer *vb = &vbuf->vb.vb2_buf;
	u32 plane;

	for (plane = 0; plane < vb->num_planes; plane++)
		fimc_is_vb2_dma_plane_finish(vbuf, plane, exact);
}

static const struct fimc_is_vb2_buf_ops fimc_is_vb2_buf_ops_dma = {
	.plane_kvaddr	= fimc_is_vb2_dma_plane_kvaddr,
	.plane_cookie	= fimc_is_vb2_dma_plane_cookie,
	.plane_dvaddr	= fimc_is_vb2_dma_plane_dvaddr,
	.plane_prepare	= fimc_is_vb2_dma_plane_prepare,
	.plane_finish	= fimc_is_vb2_dma_plane_finish,
	.buf_prepare	= fimc_is_vb2_dma_buf_prepare,
	.buf_finish	= fimc_is_vb2_dma_buf_finish,
};

/*
 * Private buffer operations via noncontiguous DMA
 */
struct fimc_is_priv_dma_buf {
	struct fimc_is_priv_buf base;
	struct device *dev;
	struct sg_table *sgt;
	void *vaddr;
	dma_addr_t daddr;
	size_t size;
};

static void fimc_is_priv_dma_buf_free(struct fimc_is_priv_buf *pbuf)
{
	struct fimc_is_priv_dma_buf *dbuf;

	if (!pbuf)
		return;

	dbuf = container_of(pbuf, struct fimc_is_priv_dma_buf, base);
	dma_vunmap_noncontiguous(dbuf->dev, dbuf->vaddr);
	dma_free_noncontiguous(dbuf->dev, dbuf->size, dbuf->sgt, DMA_BIDIRECTIONAL);
	kfree(dbuf);
}

static ulong fimc_is_priv_dma_buf_kvaddr(struct fimc_is_priv_buf *pbuf)
{
	return (ulong)pbuf->kvaddr;
}

static dma_addr_t fimc_is_priv_dma_buf_dvaddr(struct fimc_is_priv_buf *pbuf)
{
	struct fimc_is_priv_dma_buf *dbuf = container_of(pbuf, struct fimc_is_priv_dma_buf, base);

	return dbuf->daddr;
}

static phys_addr_t fimc_is_priv_dma_buf_phaddr(struct fimc_is_priv_buf *pbuf)
{
	return 0;
}

/*
 * The buffer is one IOVA range over the sg entries in order, so the range is
 * synced per overlapping entry.
 */
static void fimc_is_priv_dma_buf_sync(struct fimc_is_priv_buf *pbuf,
				      off_t offset, size_t size, enum dma_data_direction dir,
		bool for_device)
{
	struct fimc_is_priv_dma_buf *dbuf = container_of(pbuf, struct fimc_is_priv_dma_buf, base);
	struct scatterlist *sg;
	size_t pos = 0, start, end, s, e;
	unsigned int i;

	if (offset < 0 || offset >= dbuf->size)
		return;
	start = offset;
	end = min_t(size_t, dbuf->size, start + size);

	for_each_sgtable_sg(dbuf->sgt, sg, i) {
		s = max(pos, start);
		e = min(pos + sg->length, end);
		if (s < e) {
			if (for_device)
				dma_sync_single_for_device(dbuf->dev, dbuf->daddr + s,
							   e - s, dir);
			else
				dma_sync_single_for_cpu(dbuf->dev, dbuf->daddr + s,
							e - s, dir);
		}
		pos += sg->length;
		if (pos >= end)
			break;
	}
}

static void fimc_is_priv_dma_buf_sync_for_device(struct fimc_is_priv_buf *pbuf,
						 off_t offset, size_t size, enum dma_data_direction dir)
{
	fimc_is_priv_dma_buf_sync(pbuf, offset, size, dir, true);
}

static void fimc_is_priv_dma_buf_sync_for_cpu(struct fimc_is_priv_buf *pbuf,
					      off_t offset, size_t size, enum dma_data_direction dir)
{
	fimc_is_priv_dma_buf_sync(pbuf, offset, size, dir, false);
}

static const struct fimc_is_priv_buf_ops fimc_is_priv_buf_ops_dma = {
	.free			= fimc_is_priv_dma_buf_free,
	.kvaddr			= fimc_is_priv_dma_buf_kvaddr,
	.dvaddr			= fimc_is_priv_dma_buf_dvaddr,
	.phaddr			= fimc_is_priv_dma_buf_phaddr,
	.sync_for_device	= fimc_is_priv_dma_buf_sync_for_device,
	.sync_for_cpu		= fimc_is_priv_dma_buf_sync_for_cpu,
};

static void *fimc_is_mem_dma_init(struct platform_device *pdev, long flag)
{
	return &pdev->dev;
}

static void fimc_is_mem_dma_cleanup(void *ctx)
{
}

static int fimc_is_mem_dma_resume(void *ctx)
{
	return 0;
}

static void fimc_is_mem_dma_suspend(void *ctx)
{
}

static void fimc_is_mem_dma_set_cached(void *ctx, bool cacheable)
{
}

static int fimc_is_mem_dma_set_alignment(void *ctx, size_t alignment)
{
	return 0;
}

static struct fimc_is_priv_buf *fimc_is_priv_dma_buf_alloc(void *ctx, size_t size, size_t align)
{
	struct device *dev = ctx;
	struct fimc_is_priv_dma_buf *dbuf;
	struct sg_table *sgt;
	void *vaddr;

	dbuf = kzalloc(sizeof(*dbuf), GFP_KERNEL);
	if (!dbuf)
		return ERR_PTR(-ENOMEM);

	size = PAGE_ALIGN(size);
	sgt = dma_alloc_noncontiguous(dev, size, DMA_BIDIRECTIONAL, GFP_KERNEL, 0);
	if (!sgt) {
		err("failed to allocate a private buffer, size: %zd", size);
		kfree(dbuf);
		return ERR_PTR(-ENOMEM);
	}

	vaddr = dma_vmap_noncontiguous(dev, size, sgt);
	if (!vaddr) {
		dma_free_noncontiguous(dev, size, sgt, DMA_BIDIRECTIONAL);
		kfree(dbuf);
		return ERR_PTR(-ENOMEM);
	}

	dbuf->dev = dev;
	dbuf->sgt = sgt;
	dbuf->vaddr = vaddr;
	dbuf->daddr = sg_dma_address(sgt->sgl);
	dbuf->size = size;

	dbuf->base.cookie = dbuf;
	dbuf->base.size = size;
	dbuf->base.align = align;
	dbuf->base.ctx = ctx;
	dbuf->base.kvaddr = vaddr;
	dbuf->base.ops = &fimc_is_priv_buf_ops_dma;

	return &dbuf->base;
}

static const struct fimc_is_mem_ops fimc_is_mem_ops_dma = {
	.init			= fimc_is_mem_dma_init,
	.cleanup		= fimc_is_mem_dma_cleanup,
	.resume			= fimc_is_mem_dma_resume,
	.suspend		= fimc_is_mem_dma_suspend,
	.set_cached		= fimc_is_mem_dma_set_cached,
	.set_alignment		= fimc_is_mem_dma_set_alignment,
	.alloc			= fimc_is_priv_dma_buf_alloc,
};

/* kmalloc private buffer operations (for small debug structures) */
static void fimc_is_vb2_km_free(struct fimc_is_priv_buf *pbuf)
{
	if (!pbuf)
		return;
	kfree(pbuf->kvaddr);
	kfree(pbuf);
}

static ulong fimc_is_vb2_km_kvaddr(struct fimc_is_priv_buf *pbuf)
{
	if (!pbuf)
		return 0;
	return (ulong)pbuf->kvaddr;
}

static phys_addr_t fimc_is_vb2_km_phaddr(struct fimc_is_priv_buf *pbuf)
{
	if (!pbuf)
		return 0;
	return virt_to_phys(pbuf->kvaddr);
}

static const struct fimc_is_priv_buf_ops fimc_is_priv_buf_ops_km = {
	.free			= fimc_is_vb2_km_free,
	.kvaddr			= fimc_is_vb2_km_kvaddr,
	.phaddr			= fimc_is_vb2_km_phaddr,
};

static struct fimc_is_priv_buf *fimc_is_kmalloc(size_t size, size_t align)
{
	struct fimc_is_priv_buf *buf;

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return ERR_PTR(-ENOMEM);

	buf->kvaddr = kzalloc(DEBUG_REGION_SIZE + 0x10, GFP_KERNEL);
	if (!buf->kvaddr) {
		kfree(buf);
		return ERR_PTR(-ENOMEM);
	}

	buf->size = size;
	buf->align = align;
	buf->ops = &fimc_is_priv_buf_ops_km;

	return buf;
}

int fimc_is_mem_init(struct fimc_is_mem *mem, struct platform_device *pdev)
{
	mem->fimc_is_mem_ops = &fimc_is_mem_ops_dma;
	mem->vb2_mem_ops = &vb2_dma_contig_memops;
	mem->fimc_is_vb2_buf_ops = &fimc_is_vb2_buf_ops_dma;
	mem->kmalloc = &fimc_is_kmalloc;
	mem->default_ctx = &pdev->dev;
	mem->priv = &pdev->dev;

	return 0;
}

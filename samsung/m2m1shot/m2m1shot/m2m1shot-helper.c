// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2014 Samsung Electronics Co., Ltd.
 * Contact: Cho KyongHo <pullip.cho@samsung.com>
 */
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>

#include <media/m2m1shot-helper.h>

int m2m1shot_map_dma_buf(struct device *dev,
			struct m2m1shot_buffer_plane_dma *plane,
			enum dma_data_direction dir)
{
	if (plane->dmabuf) {
		plane->sgt = dma_buf_map_attachment_unlocked(plane->attachment,
							     dir);
		if (IS_ERR(plane->sgt)) {
			dev_err(dev, "%s: failed to map attachment of dma_buf\n",
					__func__);
			return PTR_ERR(plane->sgt);
		}
	}

	return 0;
}
EXPORT_SYMBOL(m2m1shot_map_dma_buf);

void m2m1shot_unmap_dma_buf(struct device *dev,
			struct m2m1shot_buffer_plane_dma *plane,
			enum dma_data_direction dir)
{
	if (plane->dmabuf && plane->sgt) {
		dma_buf_unmap_attachment_unlocked(plane->attachment,
						  plane->sgt, dir);
		plane->sgt = NULL;
	}
}
EXPORT_SYMBOL(m2m1shot_unmap_dma_buf);

static int m2m1shot_map_userptr(struct device *dev,
				struct m2m1shot_buffer_dma *buf,
				int plane_idx,
				enum dma_data_direction dir)
{
	struct m2m1shot_buffer_plane_dma *plane = &buf->plane[plane_idx];
	unsigned long addr = buf->buffer->plane[plane_idx].userptr;
	unsigned int offset = addr & ~PAGE_MASK;
	unsigned int npages;
	struct page **pages;
	struct sg_table *sgt;
	int pinned, ret;

	npages = (PAGE_ALIGN(addr + plane->bytes_used) -
		  (addr & PAGE_MASK)) >> PAGE_SHIFT;

	pages = kvmalloc_array(npages, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	pinned = pin_user_pages_fast(addr & PAGE_MASK, npages,
				     FOLL_LONGTERM |
				     (dir == DMA_FROM_DEVICE ? FOLL_WRITE : 0),
				     pages);
	if (pinned != (int)npages) {
		if (pinned > 0)
			unpin_user_pages(pages, pinned);
		kvfree(pages);
		return -EFAULT;
	}

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt) {
		unpin_user_pages(pages, npages);
		kvfree(pages);
		return -ENOMEM;
	}

	ret = sg_alloc_table_from_pages(sgt, pages, npages, offset,
					plane->bytes_used, GFP_KERNEL);
	if (ret) {
		kfree(sgt);
		unpin_user_pages(pages, npages);
		kvfree(pages);
		return ret;
	}

	ret = dma_map_sgtable(dev, sgt, dir, 0);
	if (ret) {
		sg_free_table(sgt);
		kfree(sgt);
		unpin_user_pages(pages, npages);
		kvfree(pages);
		return ret;
	}

	plane->sgt = sgt;
	plane->upages = pages;
	plane->num_upages = npages;
	plane->map_dir = dir;
	plane->dma_addr = sg_dma_address(sgt->sgl);

	return 0;
}

static void m2m1shot_unmap_userptr(struct device *dev,
				   struct m2m1shot_buffer_plane_dma *plane)
{
	enum dma_data_direction dir = plane->map_dir;

	if (!plane->sgt)
		return;

	dma_unmap_sgtable(dev, plane->sgt, dir, 0);
	sg_free_table(plane->sgt);
	kfree(plane->sgt);
	plane->sgt = NULL;
	if (plane->upages) {
		unpin_user_pages(plane->upages, plane->num_upages);
		kvfree(plane->upages);
		plane->upages = NULL;
		plane->num_upages = 0;
	}
	plane->dma_addr = 0;
}

int m2m1shot_dma_addr_map(struct device *dev,
			  struct m2m1shot_buffer_dma *buf,
			  int plane_idx, enum dma_data_direction dir)
{
	struct m2m1shot_buffer_plane_dma *plane = &buf->plane[plane_idx];

	if (plane->dmabuf) {
		if (!plane->sgt)
			return -EINVAL;
		/* the IOMMU maps the whole table at one contiguous address */
		plane->map_dir = dir;
		plane->dma_addr = sg_dma_address(plane->sgt->sgl) +
				  plane->offset;
		return 0;
	}

	return m2m1shot_map_userptr(dev, buf, plane_idx, dir);
}
EXPORT_SYMBOL(m2m1shot_dma_addr_map);

void m2m1shot_dma_addr_unmap(struct device *dev,
			struct m2m1shot_buffer_dma *buf, int plane_idx)
{
	struct m2m1shot_buffer_plane_dma *plane = &buf->plane[plane_idx];

	if (plane->dmabuf) {
		plane->dma_addr = 0;
		return;
	}

	m2m1shot_unmap_userptr(dev, plane);
}
EXPORT_SYMBOL(m2m1shot_dma_addr_unmap);

void m2m1shot_sync_for_device(struct device *dev,
			      struct m2m1shot_buffer_plane_dma *plane,
			      enum dma_data_direction dir)
{
	if (plane->sgt)
		dma_sync_sgtable_for_device(dev, plane->sgt, dir);
}
EXPORT_SYMBOL(m2m1shot_sync_for_device);

void m2m1shot_sync_for_cpu(struct device *dev,
			   struct m2m1shot_buffer_plane_dma *plane,
			   enum dma_data_direction dir)
{
	if (plane->sgt)
		dma_sync_sgtable_for_cpu(dev, plane->sgt, dir);
}
EXPORT_SYMBOL(m2m1shot_sync_for_cpu);

MODULE_DESCRIPTION("Exynos m2m1shot DMA mapping helpers");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("DMA_BUF");

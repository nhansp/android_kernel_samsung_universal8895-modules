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
#include <linux/iommu.h>
#include <linux/scatterlist.h>
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

/*
 * The DT keeps dma-iommu away from this IOVA (memory-region with
 * iommu-addresses). The DMA API mapping stays for the free.
 */
int s5p_mfc_mem_map_fixed(void *cookie, dma_addr_t iova)
{
	struct s5p_mfc_mem_cookie *c = cookie;
	struct iommu_domain *domain = iommu_get_domain_for_dev(c->dev);
	struct sg_table sgt;
	ssize_t mapped;
	int ret;

	if (!domain)
		return -ENODEV;

	ret = dma_get_sgtable_attrs(c->dev, &sgt, c->vaddr, c->daddr, c->size,
				    DMA_ATTR_WRITE_COMBINE);
	if (ret)
		return ret;

	mapped = iommu_map_sgtable(domain, iova, &sgt, IOMMU_READ | IOMMU_WRITE);
	sg_free_table(&sgt);
	if (mapped < 0)
		return mapped;
	if (mapped < c->size) {
		iommu_unmap(domain, iova, mapped);
		return -ENOMEM;
	}

	c->fixed = iova;
	return 0;
}

void s5p_mfc_mem_free(void *cookie)
{
	struct s5p_mfc_mem_cookie *c = cookie;

	if (IS_ERR_OR_NULL(c))
		return;

	if (c->fixed)
		iommu_unmap(iommu_get_domain_for_dev(c->dev), c->fixed,
			    PAGE_ALIGN(c->size));
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

/* The vb2 mapping goes away on requeue, this one outlives it for the firmware */
static bool mfc_dpb_pin_valid(const struct s5p_mfc_dpb_pin *pin)
{
	return pin->dbuf[0];
}

static void mfc_dpb_pin_release(struct s5p_mfc_dpb_pin *pin)
{
	int i;

	for (i = 0; i < MFC_MAX_PLANES; i++) {
		if (pin->sgt[i])
			dma_buf_unmap_attachment_unlocked(pin->attach[i],
							  pin->sgt[i], pin->dir);
		if (pin->attach[i])
			dma_buf_detach(pin->dbuf[i], pin->attach[i]);
		if (pin->dbuf[i])
			dma_buf_put(pin->dbuf[i]);
	}
	memset(pin, 0, sizeof(*pin));
}

static unsigned long mfc_dpb_contiguous_size(struct sg_table *sgt)
{
	dma_addr_t expected = sg_dma_address(sgt->sgl);
	unsigned long size = 0;
	struct scatterlist *sg;
	unsigned int i;

	for_each_sgtable_dma_sg(sgt, sg, i) {
		if (sg_dma_address(sg) != expected)
			break;
		expected += sg_dma_len(sg);
		size += sg_dma_len(sg);
	}

	return size;
}

static int mfc_dpb_pin_acquire(struct s5p_mfc_ctx *ctx, struct vb2_buffer *vb,
			       struct s5p_mfc_dpb_pin *pin)
{
	struct s5p_mfc_dev *dev = ctx->dev;
	int i, ret;

	if (vb->num_planes > MFC_MAX_PLANES)
		return -EINVAL;

	pin->dir = vb->vb2_queue->dma_dir;

	for (i = 0; i < vb->num_planes; i++) {
		struct dma_buf_attachment *attach;
		struct sg_table *sgt;

		if (!vb->planes[i].dbuf) {
			ret = -EINVAL;
			goto err;
		}
		get_dma_buf(vb->planes[i].dbuf);
		pin->dbuf[i] = vb->planes[i].dbuf;

		attach = dma_buf_attach(pin->dbuf[i], dev->device);
		if (IS_ERR(attach)) {
			ret = PTR_ERR(attach);
			mfc_err_ctx("[IOVMM] Failed to attach plane %d (err %d)\n",
				    i, ret);
			goto err;
		}
		pin->attach[i] = attach;

		sgt = dma_buf_map_attachment_unlocked(attach, pin->dir);
		if (IS_ERR(sgt)) {
			ret = PTR_ERR(sgt);
			mfc_err_ctx("[IOVMM] Failed to map plane %d (err %d)\n",
				    i, ret);
			goto err;
		}
		pin->sgt[i] = sgt;

		if (mfc_dpb_contiguous_size(sgt) < vb2_plane_size(vb, i)) {
			mfc_err_ctx("[IOVMM] plane %d is not contiguous in IOVA\n", i);
			ret = -EFAULT;
			goto err;
		}
		pin->addr[i] = sg_dma_address(sgt->sgl);

		mfc_debug(2, "[IOVMM] index %d buf[%d] addr: %#llx\n",
			  vb->index, i, pin->addr[i]);
	}

	return 0;

err:
	mfc_dpb_pin_release(pin);
	return ret;
}

static bool mfc_dpb_pin_same(const struct s5p_mfc_dpb_pin *pin,
			     struct vb2_buffer *vb)
{
	int i;

	for (i = 0; i < vb->num_planes; i++)
		if (pin->dbuf[i] != vb->planes[i].dbuf)
			return false;

	return true;
}

static bool mfc_dpb_fw_may_ref(struct s5p_mfc_dec *dec, int index)
{
	return test_bit(index, &dec->available_dpb) ||
		(dec->dynamic_used & BIT(index));
}

static void mfc_dpb_pin_sweep(struct s5p_mfc_dec *dec)
{
	int i;

	for (i = 0; i < MFC_MAX_DPBS; i++)
		if (mfc_dpb_pin_valid(&dec->dpb_pin_prev[i]) &&
		    !mfc_dpb_fw_may_ref(dec, i))
			mfc_dpb_pin_release(&dec->dpb_pin_prev[i]);
}

int s5p_mfc_dpb_pin_get(struct s5p_mfc_ctx *ctx, struct vb2_buffer *vb,
			dma_addr_t addr[MFC_MAX_PLANES])
{
	struct s5p_mfc_dev *dev = ctx->dev;
	struct s5p_mfc_dec *dec = ctx->dec_priv;
	struct s5p_mfc_dpb_pin fresh = {};
	struct s5p_mfc_dpb_pin *cur, *prev;
	int index = vb->index;
	int i, ret = 0;

	if (index >= MFC_MAX_DPBS)
		return -EINVAL;

	mutex_lock(&dec->dpb_mutex);

	mfc_dpb_pin_sweep(dec);

	cur = &dec->dpb_pin[index];
	prev = &dec->dpb_pin_prev[index];
	if (!mfc_dpb_pin_valid(cur) || !mfc_dpb_pin_same(cur, vb)) {
		ret = mfc_dpb_pin_acquire(ctx, vb, &fresh);
		if (ret)
			goto out;

		if (mfc_dpb_pin_valid(cur)) {
			mfc_debug(2, "[IOVMM] DPB[%d] was changed %#llx->%#llx (used: %#x)\n",
				  index, cur->addr[0], fresh.addr[0],
				  dec->dynamic_used);
			MFC_TRACE_CTX("DPB[%d] %#llx->%#llx (%#x)\n",
				      index, cur->addr[0], fresh.addr[0],
				      dec->dynamic_used);

			if (!mfc_dpb_fw_may_ref(dec, index)) {
				mfc_dpb_pin_release(cur);
				mfc_dpb_pin_release(prev);
			} else if (test_bit(index, &dec->dpb_sent)) {
				/* cur was decoded into, so it replaces prev */
				mfc_dpb_pin_release(prev);
				*prev = *cur;
			} else {
				mfc_dpb_pin_release(cur);
			}
		}
		*cur = fresh;
		clear_bit(index, &dec->dpb_sent);
	}

	for (i = 0; i < vb->num_planes; i++)
		addr[i] = cur->addr[i];
out:
	mutex_unlock(&dec->dpb_mutex);

	return ret;
}

void s5p_mfc_dpb_pin_release_all(struct s5p_mfc_ctx *ctx)
{
	struct s5p_mfc_dec *dec = ctx->dec_priv;
	int i;

	mutex_lock(&dec->dpb_mutex);

	for (i = 0; i < MFC_MAX_DPBS; i++) {
		mfc_dpb_pin_release(&dec->dpb_pin[i]);
		mfc_dpb_pin_release(&dec->dpb_pin_prev[i]);
	}
	dec->dpb_sent = 0;

	mutex_unlock(&dec->dpb_mutex);
}

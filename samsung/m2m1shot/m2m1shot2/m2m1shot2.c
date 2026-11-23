// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2015 Samsung Electronics Co., Ltd.
 * Contact: Cho KyongHo <pullip.cho@samsung.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/vmalloc.h>
#include <linux/sched.h>
#include <linux/compat.h>
#include <linux/dma-fence.h>
#include <linux/sync_file.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/timer.h>
#include <linux/workqueue.h>

#include <media/m2m1shot2.h>

#define M2M1SHOT2_FENCE_MASK (M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE | \
			      M2M1SHOT2_IMGFLAG_RELEASE_FENCE)

static void m2m1shot2_fence_cb(struct dma_fence *f, struct dma_fence_cb *cb);
static void m2m1shot2_timeout_handler(struct timer_list *t);
static void m2m1shot2_schedule(struct m2m1shot2_device *m21dev);
static void m2m1shot2_schedule_queuework(struct kref *kref);
static void m2m1shot2_context_schedule_start(struct kref *kref);

/*
 * A release fence can outlive its context through a sync_file fd, so it has its
 * own lock and names its timeline after the device, not the context.
 */
struct m2m1shot2_fence {
	struct dma_fence base;		/* first: freed by dma_fence_free() */
	spinlock_t lock;
	struct m2m1shot2_device *m21dev;
};

static const char *m2m1shot2_fence_get_driver_name(struct dma_fence *fence)
{
	return "m2m1shot2";
}

static const char *m2m1shot2_fence_get_timeline_name(struct dma_fence *fence)
{
	struct m2m1shot2_fence *f = container_of(fence, struct m2m1shot2_fence, base);

	return dev_name(f->m21dev->dev);
}

static const struct dma_fence_ops m2m1shot2_fence_ops = {
	.get_driver_name = m2m1shot2_fence_get_driver_name,
	.get_timeline_name = m2m1shot2_fence_get_timeline_name,
	.release = dma_fence_free,
};

static int m2m1shot2_open(struct inode *inode, struct file *filp)
{
	struct m2m1shot2_device *m21dev = container_of(filp->private_data,
						struct m2m1shot2_device, misc);
	struct m2m1shot2_context *ctx;
	unsigned long flags;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->fence_ctx = dma_fence_context_alloc(1);
	spin_lock_init(&ctx->fence_timeout_lock);

	INIT_LIST_HEAD(&ctx->node);

	spin_lock_irqsave(&m21dev->lock_ctx, flags);
	list_add_tail(&ctx->node, &m21dev->contexts);
	spin_unlock_irqrestore(&m21dev->lock_ctx, flags);

	ctx->m21dev = m21dev;
	mutex_init(&ctx->mutex);
	init_completion(&ctx->complete);
	complete_all(&ctx->complete); /* prevent waiting for completion */

	filp->private_data = ctx;

	for (ret = 0; ret < M2M1SHOT2_MAX_IMAGES; ret++) {
		ctx->source[ret].img.index = ret;
		ctx->source[ret].img.ctx = ctx;
	}
	ctx->target.index = M2M1SHOT2_MAX_IMAGES;
	ctx->target.ctx = ctx;

	ret = m21dev->ops->init_context(ctx);
	if (ret)
		goto err_init;

	spin_lock(&m21dev->lock_priority);
	ctx->priority = M2M1SHOT2_DEFAULT_PRIORITY;
	m21dev->prior_stats[ctx->priority] += 1;
	spin_unlock(&m21dev->lock_priority);

	timer_setup(&ctx->timer, m2m1shot2_timeout_handler, 0);

	return 0;

err_init:
	spin_lock_irqsave(&m21dev->lock_ctx, flags);
	list_del_init(&ctx->node);
	spin_unlock_irqrestore(&m21dev->lock_ctx, flags);
	kfree(ctx);
	return ret;
}

struct m2m1shot2_context *m2m1shot2_current_context(
				const struct m2m1shot2_device *m21dev)
{
	return m21dev->current_ctx;
}
EXPORT_SYMBOL(m2m1shot2_current_context);

static void m2m1shot2_put_userptr(struct device *dev,
				  struct m2m1shot2_dma_buffer *plane)
{
	enum dma_data_direction dir = (enum dma_data_direction)(unsigned long)plane->priv;
	unsigned int npages;
	struct page **pages;

	if (!plane->userptr.addr)
		return;

	if (plane->userptr.attachment) {
		struct dma_buf *dmabuf = plane->userptr.attachment->dmabuf;

		dma_buf_unmap_attachment_unlocked(plane->userptr.attachment,
						  plane->sgt, dir);
		dma_buf_detach(dmabuf, plane->userptr.attachment);
		dma_buf_put(dmabuf);
		plane->userptr.attachment = NULL;
		plane->sgt = NULL;
		goto out;
	}

	npages = (PAGE_ALIGN(plane->userptr.addr + plane->userptr.length) -
		  (plane->userptr.addr & PAGE_MASK)) >> PAGE_SHIFT;
	pages = (struct page **)plane->userptr.vma;

	if (plane->sgt) {
		dma_unmap_sgtable(dev, plane->sgt, dir, 0);
		sg_free_table(plane->sgt);
		kfree(plane->sgt);
		plane->sgt = NULL;
	}

	if (pages) {
		unpin_user_pages(pages, npages);
		kvfree(pages);
		plane->userptr.vma = NULL;
	}
out:
	plane->userptr.addr = 0;
	plane->userptr.length = 0;
	plane->dma_addr = 0;
	plane->priv = NULL;
}

/*
 * A PFN mapping of a dma-buf cannot be pinned. Return the dma-buf behind it,
 * referenced, if it covers [addr, addr + length) with the access needed.
 */
static struct dma_buf *m2m1shot2_userptr_to_dmabuf(unsigned long addr,
						   u32 length, bool write,
						   unsigned long *offset)
{
	vm_flags_t need = VM_SHARED | VM_READ | (write ? VM_WRITE : 0);
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	struct dma_buf *dmabuf = NULL;

	mmap_read_lock(mm);
	vma = vma_lookup(mm, addr);
	if (!vma || !(vma->vm_flags & (VM_IO | VM_PFNMAP)) ||
	    (vma->vm_flags & need) != need ||
	    !vma->vm_file || !is_dma_buf_file(vma->vm_file) ||
	    length > vma->vm_end - addr)
		goto out;

	dmabuf = vma->vm_file->private_data;
	*offset = (vma->vm_pgoff << PAGE_SHIFT) + (addr - vma->vm_start);
	if (*offset >= dmabuf->size || length > dmabuf->size - *offset) {
		dmabuf = NULL;
		goto out;
	}
	get_dma_buf(dmabuf);
out:
	mmap_read_unlock(mm);
	return dmabuf;
}

static int m2m1shot2_get_userptr_dmabuf(struct m2m1shot2_device *m21dev,
					struct m2m1shot2_dma_buffer *plane,
					struct dma_buf *dmabuf,
					unsigned long offset,
					unsigned long addr, u32 length,
					enum dma_data_direction dir)
{
	struct dma_buf_attachment *attachment;
	struct sg_table *sgt;
	int ret;

	attachment = dma_buf_attach(dmabuf, m21dev->dev);
	if (IS_ERR(attachment)) {
		ret = PTR_ERR(attachment);
		goto err_put;
	}

	sgt = dma_buf_map_attachment_unlocked(attachment, dir);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto err_detach;
	}

	if (sgt->nents != 1 || sg_dma_len(sgt->sgl) < offset + length) {
		dev_err(m21dev->dev,
			"m2m1shot2: dma-buf behind userptr maps to %u segments\n",
			sgt->nents);
		ret = -EINVAL;
		goto err_unmap;
	}

	plane->sgt = sgt;
	plane->dma_addr = sg_dma_address(sgt->sgl) + offset;
	plane->userptr.attachment = attachment;
	plane->userptr.vma = NULL;
	plane->userptr.addr = addr;
	plane->userptr.length = length;
	plane->priv = (void *)(unsigned long)dir;

	return 0;

err_unmap:
	dma_buf_unmap_attachment_unlocked(attachment, sgt, dir);
err_detach:
	dma_buf_detach(dmabuf, attachment);
err_put:
	dma_buf_put(dmabuf);
	return ret;
}

static int m2m1shot2_get_userptr(struct m2m1shot2_device *m21dev,
				 struct m2m1shot2_dma_buffer *plane,
				 unsigned long addr, u32 length,
				 enum dma_data_direction dir)
{
	unsigned int offset, npages;
	struct page **pages;
	struct sg_table *sgt;
	struct dma_buf *dmabuf;
	unsigned long dmabuf_offset;
	int pinned, ret;

	if (plane->userptr.addr != 0)
		m2m1shot2_put_userptr(m21dev->dev, plane);

	dmabuf = m2m1shot2_userptr_to_dmabuf(addr, length,
					     dir != DMA_TO_DEVICE,
					     &dmabuf_offset);
	if (dmabuf)
		return m2m1shot2_get_userptr_dmabuf(m21dev, plane, dmabuf,
						    dmabuf_offset, addr,
						    length, dir);

	offset = addr & ~PAGE_MASK;
	npages = (PAGE_ALIGN(addr + length) - (addr & PAGE_MASK)) >> PAGE_SHIFT;

	pages = kvmalloc_array(npages, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	pinned = pin_user_pages_fast(addr & PAGE_MASK, npages,
				     FOLL_LONGTERM | (dir == DMA_FROM_DEVICE ? FOLL_WRITE : 0),
				     pages);
	if (pinned != npages) {
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

	ret = sg_alloc_table_from_pages(sgt, pages, npages, offset, length, GFP_KERNEL);
	if (ret) {
		kfree(sgt);
		unpin_user_pages(pages, npages);
		kvfree(pages);
		return ret;
	}

	ret = dma_map_sgtable(m21dev->dev, sgt, dir, 0);
	if (ret) {
		sg_free_table(sgt);
		kfree(sgt);
		unpin_user_pages(pages, npages);
		kvfree(pages);
		return ret;
	}

	/* the G2D takes one address per plane */
	if (sgt->nents != 1) {
		dev_err(m21dev->dev,
			"m2m1shot2: userptr of %u bytes maps to %u segments\n",
			length, sgt->nents);
		dma_unmap_sgtable(m21dev->dev, sgt, dir, 0);
		sg_free_table(sgt);
		kfree(sgt);
		unpin_user_pages(pages, npages);
		kvfree(pages);
		return -EINVAL;
	}

	plane->sgt = sgt;
	plane->dma_addr = sg_dma_address(sgt->sgl);
	plane->userptr.attachment = NULL;
	plane->userptr.vma = (struct vm_area_struct *)pages;
	plane->userptr.addr = addr;
	plane->userptr.length = length;
	plane->priv = (void *)(unsigned long)dir;

	return 0;
}

static void m2m1shot2_put_dmabuf(struct m2m1shot2_dma_buffer *plane)
{
	if (plane->dmabuf.attachment) {
		dma_buf_detach(plane->dmabuf.dmabuf, plane->dmabuf.attachment);
		plane->dmabuf.attachment = NULL;
	}
	if (plane->dmabuf.dmabuf) {
		dma_buf_put(plane->dmabuf.dmabuf);
		plane->dmabuf.dmabuf = NULL;
	}
	plane->sgt = NULL;
	plane->dma_addr = 0;
}

static int m2m1shot2_get_dmabuf(struct m2m1shot2_device *m21dev,
				struct m2m1shot2_dma_buffer *plane,
				int fd, u32 off, size_t payload,
				enum dma_data_direction dir)
{
	struct dma_buf *dmabuf = dma_buf_get(fd);
	int ret = -EINVAL;

	if (IS_ERR(dmabuf)) {
		dev_err(m21dev->dev, "%s: failed to get dmabuf from fd %d\n",
			__func__, fd);
		return PTR_ERR(dmabuf);
	}

	if (dmabuf->size < off) {
		dev_err(m21dev->dev,
			"%s: too large offset %u for dmabuf of %zu\n",
			__func__, off, dmabuf->size);
		goto err;
	}

	if ((dmabuf->size - off) < payload) {
		dev_err(m21dev->dev,
			"%s: too small dmabuf %zu/%u but required %zu\n",
			__func__, dmabuf->size, off, payload);
		goto err;
	}

	if (dmabuf == plane->dmabuf.dmabuf &&
	    dmabuf->size == plane->dmabuf.dmabuf->size &&
	    dmabuf->file == plane->dmabuf.dmabuf->file) {
		/* do not attach dmabuf again for the same buffer */
		dma_buf_put(dmabuf);
		plane->dmabuf.offset = off;
		return 0;
	}

	/* release previous buffer */
	if (plane->dmabuf.dmabuf)
		m2m1shot2_put_dmabuf(plane);

	plane->dmabuf.attachment = dma_buf_attach(dmabuf, m21dev->dev);
	if (IS_ERR(plane->dmabuf.attachment)) {
		dev_err(m21dev->dev, "%s: failed to attach to dmabuf\n", __func__);
		ret = PTR_ERR(plane->dmabuf.attachment);
		plane->dmabuf.attachment = NULL;
		goto err;
	}

	plane->dmabuf.dmabuf = dmabuf;
	plane->dmabuf.offset = off;
	return 0;

err:
	dma_buf_put(dmabuf);
	return ret;
}

static void m2m1shot2_put_buffer(struct m2m1shot2_context *ctx, u32 memory,
				 struct m2m1shot2_dma_buffer plane[],
				 unsigned int num_planes)
{
	unsigned int i;

	if (memory == M2M1SHOT2_BUFTYPE_DMABUF) {
		for (i = 0; i < num_planes; i++)
			m2m1shot2_put_dmabuf(&plane[i]);
	} else if (memory == M2M1SHOT2_BUFTYPE_USERPTR) {
		for (i = 0; i < num_planes; i++)
			m2m1shot2_put_userptr(ctx->m21dev->dev, &plane[i]);
	}
}

/*
 * Returns true if the callback was removed before it ran. The fence is held
 * across dma_fence_remove_callback(), called without fence_timeout_lock because
 * the callback takes that lock under the fence lock.
 */
static bool m2m1shot2_cancel_fence_wait(struct m2m1shot2_context *ctx,
					struct m2m1shot2_context_image *img)
{
	struct dma_fence *fence;
	unsigned long flags;
	bool removed;

	spin_lock_irqsave(&ctx->fence_timeout_lock, flags);
	if (!(img->flags & M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE)) {
		spin_unlock_irqrestore(&ctx->fence_timeout_lock, flags);
		return false;
	}
	fence = dma_fence_get(img->fence);
	spin_unlock_irqrestore(&ctx->fence_timeout_lock, flags);

	removed = dma_fence_remove_callback(fence, &img->cb);
	if (removed) {
		spin_lock_irqsave(&ctx->fence_timeout_lock, flags);
		img->flags &= ~M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE;
		dma_fence_put(img->fence);
		img->fence = NULL;
		spin_unlock_irqrestore(&ctx->fence_timeout_lock, flags);
	}
	dma_fence_put(fence);

	return removed;
}

static void m2m1shot2_put_image(struct m2m1shot2_context *ctx,
				struct m2m1shot2_context_image *img)
{
	m2m1shot2_cancel_fence_wait(ctx, img);
	img->flags &= ~M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE;
	img->fence = NULL;

	m2m1shot2_put_buffer(ctx, img->memory, img->plane, img->num_planes);
	img->memory = M2M1SHOT2_BUFTYPE_NONE;
}

static void m2m1shot2_put_source_images(struct m2m1shot2_context *ctx)
{
	unsigned int i;

	for (i = 0; i < ctx->num_sources; i++)
		m2m1shot2_put_image(ctx, &ctx->source[i].img);

	ctx->num_sources = 0;
}

static void m2m1shot2_put_images(struct m2m1shot2_context *ctx)
{
	m2m1shot2_put_source_images(ctx);
	m2m1shot2_put_image(ctx, &ctx->target);
}

static int m2m1shot2_map_image(struct m2m1shot2_device *m21dev,
			       struct m2m1shot2_context_image *img,
			       enum dma_data_direction dir)
{
	unsigned int i;

	if (img->memory == M2M1SHOT2_BUFTYPE_USERPTR) {
		if (!(img->flags & M2M1SHOT2_IMGFLAG_NO_CACHECLEAN)) {
			for (i = 0; i < img->num_planes; i++) {
				if (img->plane[i].sgt)
					dma_sync_sgtable_for_device(m21dev->dev,
								    img->plane[i].sgt, dir);
			}
		}
		return 0;
	}

	if (img->memory != M2M1SHOT2_BUFTYPE_DMABUF)
		return 0;

	for (i = 0; i < img->num_planes; i++) {
		struct dma_buf_attachment *attach = img->plane[i].dmabuf.attachment;
		struct sg_table *sgt;

		attach->dma_map_attrs = 0;
		if (img->flags & M2M1SHOT2_IMGFLAG_NO_CACHECLEAN)
			attach->dma_map_attrs |= DMA_ATTR_SKIP_CPU_SYNC;

		sgt = dma_buf_map_attachment_unlocked(attach, dir);
		if (IS_ERR(sgt)) {
			int ret = PTR_ERR(sgt);

			dev_err(m21dev->dev, "failed to map dmabuf-attachment: %d\n", ret);
			img->plane[i].sgt = NULL;
			while (i-- > 0) {
				dma_buf_unmap_attachment_unlocked(img->plane[i].dmabuf.attachment,
								  img->plane[i].sgt, dir);
				img->plane[i].sgt = NULL;
				img->plane[i].dma_addr = 0;
			}
			return ret;
		}

		if (sgt->nents != 1) {
			dev_err(m21dev->dev,
				"m2m1shot2: dma-buf of %zu bytes maps to %u segments\n",
				img->plane[i].dmabuf.dmabuf->size, sgt->nents);
			dma_buf_unmap_attachment_unlocked(attach, sgt, dir);
			img->plane[i].sgt = NULL;
			while (i-- > 0) {
				dma_buf_unmap_attachment_unlocked(img->plane[i].dmabuf.attachment,
								  img->plane[i].sgt, dir);
				img->plane[i].sgt = NULL;
				img->plane[i].dma_addr = 0;
			}
			return -EINVAL;
		}

		img->plane[i].sgt = sgt;
		img->plane[i].dma_addr = sg_dma_address(sgt->sgl) + img->plane[i].dmabuf.offset;
	}

	return 0;
}

static void m2m1shot2_unmap_image(struct m2m1shot2_device *m21dev,
				  struct m2m1shot2_context_image *img,
				  enum dma_data_direction dir)
{
	unsigned int i;

	if (img->memory == M2M1SHOT2_BUFTYPE_USERPTR) {
		if (!(img->flags & M2M1SHOT2_IMGFLAG_NO_CACHEINV)) {
			for (i = 0; i < img->num_planes; i++) {
				if (img->plane[i].sgt)
					dma_sync_sgtable_for_cpu(m21dev->dev,
								 img->plane[i].sgt, dir);
			}
		}
		return;
	}

	if (img->memory != M2M1SHOT2_BUFTYPE_DMABUF)
		return;

	for (i = 0; i < img->num_planes; i++) {
		struct m2m1shot2_dma_buffer *plane = &img->plane[i];

		if (!plane->sgt)
			continue;

		plane->dmabuf.attachment->dma_map_attrs = 0;
		if (img->flags & M2M1SHOT2_IMGFLAG_NO_CACHEINV)
			plane->dmabuf.attachment->dma_map_attrs |= DMA_ATTR_SKIP_CPU_SYNC;

		dma_buf_unmap_attachment_unlocked(plane->dmabuf.attachment,
						  plane->sgt, dir);
		plane->sgt = NULL;
		plane->dma_addr = 0;
	}
}

static int m2m1shot2_map_images(struct m2m1shot2_context *ctx)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ctx->num_sources; i++) {
		ret = m2m1shot2_map_image(ctx->m21dev,
					  &ctx->source[i].img, DMA_TO_DEVICE);
		if (ret) {
			while (i-- > 0)
				m2m1shot2_unmap_image(ctx->m21dev,
						      &ctx->source[i].img, DMA_TO_DEVICE);
			return ret;
		}
	}

	ret = m2m1shot2_map_image(ctx->m21dev, &ctx->target, DMA_FROM_DEVICE);
	if (ret) {
		for (i = 0; i < ctx->num_sources; i++)
			m2m1shot2_unmap_image(ctx->m21dev,
					      &ctx->source[i].img, DMA_TO_DEVICE);
	}

	return ret;
}

static void m2m1shot2_unmap_images(struct m2m1shot2_context *ctx)
{
	unsigned int i;

	for (i = 0; i < ctx->num_sources; i++)
		m2m1shot2_unmap_image(ctx->m21dev,
				      &ctx->source[i].img, DMA_TO_DEVICE);

	m2m1shot2_unmap_image(ctx->m21dev, &ctx->target, DMA_FROM_DEVICE);
}

static void __m2m1shot2_finish_context(struct m2m1shot2_context *ctx,
				       bool success)
{
	struct m2m1shot2_device *m21dev = ctx->m21dev;
	unsigned long flags;

	if (ctx->release_fence) {
		dma_fence_signal(ctx->release_fence);
		dma_fence_put(ctx->release_fence);
		ctx->release_fence = NULL;
	}

	spin_lock_irqsave(&m21dev->lock_ctx, flags);

	/* effective only to the current processing context */
	if (WARN_ON(ctx != m21dev->current_ctx)) {
		spin_unlock_irqrestore(&m21dev->lock_ctx, flags);
		return;
	}

	m21dev->current_ctx = NULL;
	list_add_tail(&ctx->node, &m21dev->contexts);

	spin_unlock_irqrestore(&m21dev->lock_ctx, flags);

	if (!success)
		set_bit(M2M1S2_CTXSTATE_ERROR, &ctx->state);

	set_bit(M2M1S2_CTXSTATE_PROCESSED, &ctx->state);
	clear_bit(M2M1S2_CTXSTATE_PROCESSING, &ctx->state);

	complete_all(&ctx->complete);
}

static void m2m1shot2_schedule(struct m2m1shot2_device *m21dev)
{
	struct m2m1shot2_context *ctx;
	unsigned long flags;

retry:
	spin_lock_irqsave(&m21dev->lock_ctx, flags);

	if (m21dev->current_ctx != NULL) {
		/* the client is currently processing a context */
		spin_unlock_irqrestore(&m21dev->lock_ctx, flags);
		return;
	}

	if (list_empty(&m21dev->active_contexts)) {
		/* no context to process */
		spin_unlock_irqrestore(&m21dev->lock_ctx, flags);
		return;
	}

	ctx = list_first_entry(&m21dev->active_contexts,
			       struct m2m1shot2_context, node);
	m21dev->current_ctx = ctx;
	list_del_init(&ctx->node);

	spin_unlock_irqrestore(&m21dev->lock_ctx, flags);

	clear_bit(M2M1S2_CTXSTATE_PENDING, &ctx->state);

	if (m21dev->ops->device_run(ctx) < 0) {
		__m2m1shot2_finish_context(ctx, false);
		goto retry;
	}
}

void m2m1shot2_finish_context(struct m2m1shot2_context *ctx, bool success)
{
	struct m2m1shot2_device *m21dev = ctx->m21dev;

	__m2m1shot2_finish_context(ctx, success);
	m2m1shot2_schedule(m21dev);
}
EXPORT_SYMBOL(m2m1shot2_finish_context);

void m2m1shot2_finish_context_deferred(struct m2m1shot2_context *ctx, bool success)
{
	struct m2m1shot2_device *m21dev = ctx->m21dev;

	__m2m1shot2_finish_context(ctx, success);
	queue_work(m21dev->schedule_workqueue, &m21dev->schedule_work);
}
EXPORT_SYMBOL(m2m1shot2_finish_context_deferred);

static void m2m1shot2_device_schedule_work(struct work_struct *work)
{
	struct m2m1shot2_device *m21dev =
		container_of(work, struct m2m1shot2_device, schedule_work);

	m2m1shot2_schedule(m21dev);
}

static bool m2m1shot2_wait_process(struct m2m1shot2_device *m21dev,
				   struct m2m1shot2_context *ctx)
{
	bool success = true;

	set_bit(M2M1S2_CTXSTATE_WAITING, &ctx->state);

	wait_for_completion(&ctx->complete);

	clear_bit(M2M1S2_CTXSTATE_WAITING, &ctx->state);

	if (ctx->state & ((1 << M2M1S2_CTXSTATE_PENDING) |
			  (1 << M2M1S2_CTXSTATE_PROCESSING))) {
		pr_err("m2m1shot finish state : %lx\n", ctx->state);
		WARN_ON_ONCE(1);
		return false;
	}

	clear_bit(M2M1S2_CTXSTATE_PROCESSED, &ctx->state);

	if (test_bit(M2M1S2_CTXSTATE_ERROR, &ctx->state)) {
		success = false;
		clear_bit(M2M1S2_CTXSTATE_ERROR, &ctx->state);
	}

	m2m1shot2_unmap_images(ctx);

	return success;
}

static void m2m1shot2_schedule_context(struct m2m1shot2_context *ctx)
{
	struct m2m1shot2_device *m21dev = ctx->m21dev;
	unsigned long flags;

	timer_delete(&ctx->timer);

	spin_lock_irqsave(&ctx->m21dev->lock_ctx, flags);

	if (WARN_ON_ONCE(list_empty(&ctx->node))) {
		spin_unlock_irqrestore(&ctx->m21dev->lock_ctx, flags);
		return;
	}

	list_move_tail(&ctx->node, &ctx->m21dev->active_contexts);

	set_bit(M2M1S2_CTXSTATE_PROCESSING, &ctx->state);

	spin_unlock_irqrestore(&ctx->m21dev->lock_ctx, flags);

	m2m1shot2_schedule(m21dev);
}

static void m2m1shot2_context_schedule_work(struct work_struct *work)
{
	m2m1shot2_schedule_context(
			container_of(work, struct m2m1shot2_context, work));
}

static void m2m1shot2_context_schedule_start(struct kref *kref)
{
	m2m1shot2_schedule_context(
			container_of(kref, struct m2m1shot2_context, starter));
}

static void m2m1shot2_schedule_queuework(struct kref *kref)
{
	struct m2m1shot2_context *ctx =
			container_of(kref, struct m2m1shot2_context, starter);
	bool failed;

	failed = !queue_work(ctx->m21dev->schedule_workqueue, &ctx->work);
	WARN_ON_ONCE(failed);
}

static void m2m1shot2_fence_cb(struct dma_fence *f, struct dma_fence_cb *cb)
{
	struct m2m1shot2_context_image *img =
		container_of(cb, struct m2m1shot2_context_image, cb);
	struct m2m1shot2_context *ctx = img->ctx;
	unsigned long flags;

	if (WARN_ON_ONCE(!ctx))
		return;

	spin_lock_irqsave(&ctx->fence_timeout_lock, flags);

	if (!(img->flags & M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE)) {
		spin_unlock_irqrestore(&ctx->fence_timeout_lock, flags);
		return;
	}

	img->flags &= ~M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE;
	dma_fence_put(img->fence);
	img->fence = NULL;

	kref_put(&ctx->starter, m2m1shot2_schedule_queuework);

	spin_unlock_irqrestore(&ctx->fence_timeout_lock, flags);
}

static void m2m1shot2_timeout_handler(struct timer_list *t)
{
	struct m2m1shot2_context *ctx = timer_container_of(ctx, t, timer);
	struct m2m1shot2_source_image *image = ctx->source;
	unsigned long flags;
	int i;

	spin_lock_irqsave(&ctx->fence_timeout_lock, flags);
	pr_err("%s: %d Fence(s) timed out after %d msec.\n", __func__,
		kref_read(&ctx->starter), M2M1S2_TIMEOUT_INTERVAL);

	rcu_read_lock();
	for (i = 0; i < ctx->num_sources; i++) {
		if (image[i].img.flags & M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE)
			pr_err("%s:    SOURCE[%d]: [%p] %s\n",
				__func__, i, image[i].img.fence,
				dma_fence_timeline_name(image[i].img.fence));
	}

	if (ctx->target.flags & M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE)
		pr_err("%s:    TARGET:    [%p] %s\n",
			__func__, ctx->target.fence,
			dma_fence_timeline_name(ctx->target.fence));
	rcu_read_unlock();

	if (ctx->release_fence)
		pr_err("%s:    Pending release fence: %p\n",
			__func__, ctx->release_fence);

	/* re-arm only until the queued work runs: its timer_delete() is not sync */
	if (kref_read(&ctx->starter) == 0) {
		if (test_bit(M2M1S2_CTXSTATE_PENDING, &ctx->state) &&
		    !test_bit(M2M1S2_CTXSTATE_PROCESSING, &ctx->state)) {
			pr_err("All fences have been signaled. (work_busy? %d)\n",
			       work_busy(&ctx->work));
			mod_timer(&ctx->timer,
				  jiffies + msecs_to_jiffies(M2M1S2_TIMEOUT_INTERVAL));
		}
		spin_unlock_irqrestore(&ctx->fence_timeout_lock, flags);
		return;
	}
	spin_unlock_irqrestore(&ctx->fence_timeout_lock, flags);

	for (i = 0; i < ctx->num_sources; i++)
		m2m1shot2_cancel_fence_wait(ctx, &image[i].img);
	m2m1shot2_cancel_fence_wait(ctx, &ctx->target);

	spin_lock_irqsave(&ctx->fence_timeout_lock, flags);
	if (kref_read(&ctx->starter) > 0)
		m2m1shot2_schedule_queuework(&ctx->starter);
	spin_unlock_irqrestore(&ctx->fence_timeout_lock, flags);
}

static int m2m1shot2_start_context(struct m2m1shot2_device *m21dev,
				   struct m2m1shot2_context *ctx)
{
	int refcount;
	int ret;

	ret = m2m1shot2_map_images(ctx);
	if (ret)
		return ret;

	INIT_WORK(&ctx->work, m2m1shot2_context_schedule_work);

	reinit_completion(&ctx->complete);

	clear_bit(M2M1S2_CTXSTATE_PROCESSED, &ctx->state);
	set_bit(M2M1S2_CTXSTATE_PENDING, &ctx->state);

	refcount = kref_read(&ctx->starter);
	if (refcount > 1)
		mod_timer(&ctx->timer,
			  jiffies + msecs_to_jiffies(M2M1S2_TIMEOUT_INTERVAL));

	kref_put(&ctx->starter, m2m1shot2_schedule_queuework);
	return 0;
}

static void m2m1shot2_cancel_context(struct m2m1shot2_context *ctx)
{
	unsigned long flags;

	m2m1shot2_wait_process(ctx->m21dev, ctx);

	spin_lock_irqsave(&ctx->m21dev->lock_ctx, flags);
	if (WARN_ON_ONCE(list_empty(&ctx->node))) {
		spin_unlock_irqrestore(&ctx->m21dev->lock_ctx, flags);
		return;
	}
	list_del_init(&ctx->node);
	spin_unlock_irqrestore(&ctx->m21dev->lock_ctx, flags);

	if (ctx->release_fence) {
		dma_fence_signal(ctx->release_fence);
		dma_fence_put(ctx->release_fence);
		ctx->release_fence = NULL;
	}

	if (ctx->flags & M2M1SHOT2_FLAG_NONBLOCK)
		m2m1shot2_put_images(ctx);

	WARN(!M2M1S2_CTXSTATE_IDLE(ctx),
	     "state should be IDLE but %#lx\n", ctx->state);
}

static void m2m1shot2_destroy_context(struct work_struct *work)
{
	struct m2m1shot2_context *ctx =
		container_of(work, struct m2m1shot2_context, dwork);

	mutex_lock(&ctx->mutex);

	m2m1shot2_cancel_context(ctx);

	timer_delete_sync(&ctx->timer);

	spin_lock(&ctx->m21dev->lock_priority);
	ctx->m21dev->prior_stats[ctx->priority] -= 1;
	spin_unlock(&ctx->m21dev->lock_priority);

	if (ctx->m21dev->ops->prepare_perf)
		ctx->m21dev->ops->prepare_perf(ctx, NULL);

	ctx->m21dev->ops->free_context(ctx);

	mutex_unlock(&ctx->mutex);

	kfree(ctx);
}

static int m2m1shot2_release(struct inode *inode, struct file *filp)
{
	struct m2m1shot2_context *ctx = filp->private_data;

	INIT_WORK(&ctx->dwork, m2m1shot2_destroy_context);
	queue_work(ctx->m21dev->destroy_workqueue, &ctx->dwork);

	return 0;
}

static int m2m1shot2_get_buffer(struct m2m1shot2_context *ctx,
				struct m2m1shot2_context_image *img,
				struct m2m1shot2_image *src,
				size_t payload[],
				enum dma_data_direction dir)
{
	unsigned int num_planes = src->num_planes;
	int ret = 0;
	unsigned int i;

	if (src->memory != img->memory)
		m2m1shot2_put_buffer(ctx, img->memory,
				     img->plane, img->num_planes);

	img->memory = M2M1SHOT2_BUFTYPE_NONE;

	if (src->memory == M2M1SHOT2_BUFTYPE_DMABUF) {
		for (i = 0; i < src->num_planes; i++) {
			ret = m2m1shot2_get_dmabuf(ctx->m21dev,
						   &img->plane[i], src->plane[i].fd,
						   src->plane[i].offset, payload[i], dir);
			if (ret) {
				m2m1shot2_put_buffer(ctx, src->memory, img->plane,
						     M2M1SHOT2_MAX_PLANES);
				return ret;
			}
			img->plane[i].payload = payload[i];
		}
	} else if (src->memory == M2M1SHOT2_BUFTYPE_USERPTR) {
		for (i = 0; i < src->num_planes; i++) {
			if (src->plane[i].offset != 0) {
				dev_err(ctx->m21dev->dev,
					"%s: offset should be 0 with userptr\n",
					__func__);
				ret = -EINVAL;
			} else {
				ret = m2m1shot2_get_userptr(ctx->m21dev,
							    &img->plane[i], src->plane[i].userptr,
							    src->plane[i].length, dir);
			}

			if (ret) {
				m2m1shot2_put_buffer(ctx, src->memory, img->plane,
						     M2M1SHOT2_MAX_PLANES);
				return ret;
			}
			img->plane[i].payload = payload[i];
		}
	} else {
		dev_err(ctx->m21dev->dev,
			"%s: invalid memory type %d\n", __func__, src->memory);
		return -EINVAL;
	}

	m2m1shot2_put_buffer(ctx, src->memory, &img->plane[num_planes],
			     M2M1SHOT2_MAX_PLANES - num_planes);

	img->num_planes = num_planes;
	img->memory = src->memory;

	return 0;
}

static bool m2m1shot2_check_fence_wait(struct m2m1shot2_context *ctx,
				       unsigned int fence_count,
				       struct dma_fence *fence,
				       struct dma_fence_cb *cb)
{
	unsigned int i;
	int ret;

	for (i = 0; i < fence_count; i++) {
		if (ctx->source[i].img.fence == fence)
			return false;
	}

	kref_get(&ctx->starter);
	ret = dma_fence_add_callback(fence, cb, m2m1shot2_fence_cb);
	if (ret < 0) {
		if (ret != -ENOENT)
			dev_err(ctx->m21dev->dev,
				"Error occurred in an acquire fence (%d)\n", ret);
		kref_put(&ctx->starter, m2m1shot2_context_schedule_start);
		dma_fence_put(fence);
		return false;
	}

	return true;
}

static int m2m1shot2_get_source(struct m2m1shot2_context *ctx,
				unsigned int index,
				struct m2m1shot2_source_image *img,
				struct m2m1shot2_image *src)
{
	struct device *dev = ctx->m21dev->dev;
	size_t payload[M2M1SHOT2_MAX_PLANES];
	unsigned int i, num_planes;
	int ret;

	if (!M2M1SHOT2_BUFTYPE_VALID(src->memory)) {
		dev_err(dev,
			"%s: invalid memory type %u specified for image %u\n",
			__func__, src->memory, index);
		return -EINVAL;
	}

	if (src->flags & M2M1SHOT2_IMGFLAG_SECURE) {
		dev_warn_once(dev,
			      "m2m1shot2: secure image rejected (content path protection is not supported)\n");
		return -EPERM;
	}

	img->img.fmt.fmt = src->fmt;
	img->img.flags = src->flags;
	img->ext = src->ext;
	img->img.fmt.colorspace = src->colorspace;

	ret = ctx->m21dev->ops->prepare_format(&img->img.fmt, index,
					       DMA_TO_DEVICE, payload, &num_planes);
	if (ret) {
		dev_err(dev, "%s: invalid format specified for image %u\n",
			__func__, index);
		return ret;
	}

	if (src->memory == M2M1SHOT2_BUFTYPE_EMPTY) {
		m2m1shot2_put_image(ctx, &img->img);
		img->img.memory = src->memory;
		img->img.num_planes = 0;
		img->img.flags &= ~(M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE |
				    M2M1SHOT2_IMGFLAG_RELEASE_FENCE);
		return 0;
	}

	if (WARN_ON_ONCE(num_planes < 1 || num_planes > M2M1SHOT2_MAX_PLANES))
		return -EINVAL;

	if (num_planes != src->num_planes) {
		dev_err(dev, "%s: wrong number of planes %u of image %u.\n",
			__func__, src->num_planes, index);
		return -EINVAL;
	}

	for (i = 0; i < num_planes; i++) {
		if (src->plane[i].length < payload[i]) {
			dev_err(dev,
				"%s: too small size %u (plane %u / image %u)\n",
				__func__, src->plane[i].length, i, index);
			return -EINVAL;
		}
	}

	return m2m1shot2_get_buffer(ctx, &img->img,
				    src, payload, DMA_TO_DEVICE);
}

static int m2m1shot2_get_sources(struct m2m1shot2_context *ctx,
				 struct m2m1shot2_image sources[],
				 bool blocking)
{
	struct device *dev = ctx->m21dev->dev;
	struct m2m1shot2_source_image *image = ctx->source;
	unsigned int i;
	int ret;

	for (i = 0; i < ctx->num_sources; i++) {
		struct m2m1shot2_image *source = &sources[i];

		if (blocking && (source->flags & M2M1SHOT2_FENCE_MASK)) {
			dev_err(dev, "%s: fence set for blocking mode\n", __func__);
			ret = -EINVAL;
			goto err;
		}

		ret = m2m1shot2_get_source(ctx, i, &image[i], source);
		if (ret)
			goto err;

		ret = ctx->m21dev->ops->prepare_source(ctx, i, &image[i]);
		if (ret) {
			m2m1shot2_put_image(ctx, &image[i].img);
			goto err;
		}

		image[i].img.fence = NULL;

		if (image[i].img.flags & M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE) {
			struct dma_fence *fence = sync_file_get_fence(source->fence);

			if (!fence) {
				dev_err(dev, "%s: invalid acquire fence %d\n",
					__func__, source->fence);
				ret = -EINVAL;
				image[i].img.flags &= ~M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE;
				m2m1shot2_put_image(ctx, &image[i].img);
				goto err;
			}

			image[i].img.fence = fence;
			if (!m2m1shot2_check_fence_wait(ctx, i, fence, &image[i].img.cb)) {
				image[i].img.flags &= ~M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE;
				image[i].img.fence = NULL;
			}
		}
	}

	return 0;

err:
	while (i-- > 0)
		m2m1shot2_put_image(ctx, &ctx->source[i].img);

	ctx->num_sources = 0;
	return ret;
}

static int m2m1shot2_get_target(struct m2m1shot2_context *ctx,
				struct m2m1shot2_image *dst)
{
	struct device *dev = ctx->m21dev->dev;
	struct m2m1shot2_context_image *img = &ctx->target;
	size_t payload[M2M1SHOT2_MAX_PLANES];
	unsigned int i, num_planes;
	int ret;

	if (!M2M1SHOT2_BUFTYPE_VALID(dst->memory)) {
		dev_err(dev, "%s: invalid memory type %u specified for target\n",
			__func__, dst->memory);
		return -EINVAL;
	}

	if (dst->memory == M2M1SHOT2_BUFTYPE_EMPTY) {
		dev_err(dev, "%s: M2M1SHOT2_BUFTYPE_EMPTY is not valid for target\n",
			__func__);
		return -EINVAL;
	}

	if (dst->flags & M2M1SHOT2_IMGFLAG_SECURE) {
		dev_warn_once(dev,
			      "m2m1shot2: secure image rejected (content path protection is not supported)\n");
		return -EPERM;
	}

	img->fmt.fmt = dst->fmt;
	img->fmt.colorspace = dst->colorspace;

	ret = ctx->m21dev->ops->prepare_format(&img->fmt, 0,
					       DMA_FROM_DEVICE, payload, &num_planes);
	if (ret) {
		dev_err(dev, "%s: invalid format specified for target\n", __func__);
		return ret;
	}

	if (WARN_ON_ONCE(num_planes < 1 || num_planes > M2M1SHOT2_MAX_PLANES))
		return -EINVAL;

	if (num_planes != dst->num_planes) {
		dev_err(dev, "%s: wrong number of planes %u for target\n",
			__func__, dst->num_planes);
		return -EINVAL;
	}

	for (i = 0; i < num_planes; i++) {
		if ((payload[i] != 0) && (dst->plane[i].length < payload[i])) {
			dev_err(dev, "%s: too small size %u (plane %u)\n",
				__func__, dst->plane[i].length, i);
			return -EINVAL;
		}
	}

	ret = m2m1shot2_get_buffer(ctx, img, dst, payload, DMA_FROM_DEVICE);
	if (ret)
		return ret;

	img->flags = dst->flags;
	img->fence = NULL;

	if (img->flags & M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE) {
		struct dma_fence *fence = sync_file_get_fence(dst->fence);

		if (!fence) {
			dev_err(dev, "%s: invalid acquire fence %d\n",
				__func__, dst->fence);
			img->flags &= ~M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE;
			m2m1shot2_put_buffer(ctx, dst->memory,
					     img->plane, img->num_planes);
			img->memory = M2M1SHOT2_BUFTYPE_NONE;
			return -EINVAL;
		}

		img->fence = fence;
		if (!m2m1shot2_check_fence_wait(ctx, ctx->num_sources,
						fence, &img->cb)) {
			img->flags &= ~M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE;
			img->fence = NULL;
		}
	}

	return 0;
}

static int m2m1shot2_get_userdata(struct m2m1shot2_context *ctx,
				  struct m2m1shot2 *data,
				  struct m2m1shot2_image sources[])
{
	struct device *dev = ctx->m21dev->dev;
	bool blocking;
	int ret;

	if ((data->num_sources < 1) ||
	    (data->num_sources > M2M1SHOT2_MAX_IMAGES)) {
		dev_err(dev, "%s: Invalid number of source images %u\n",
			__func__, data->num_sources);
		return -EINVAL;
	}

	blocking = !(data->flags & M2M1SHOT2_FLAG_NONBLOCK);

	while (ctx->num_sources > data->num_sources) {
		ctx->num_sources--;
		m2m1shot2_put_image(ctx, &ctx->source[ctx->num_sources].img);
	}

	ctx->ctx_private = 0;
	ctx->ctx_private2 = 0;
	ctx->num_sources = data->num_sources;
	ctx->flags = data->flags;

	ret = m2m1shot2_get_sources(ctx, sources, blocking);
	if (ret)
		return ret;

	if (blocking && (data->target.flags & M2M1SHOT2_FENCE_MASK)) {
		dev_err(dev, "%s: fence set for blocking mode\n", __func__);
		ret = -EINVAL;
		goto err;
	}

	ret = m2m1shot2_get_target(ctx, &data->target);
	if (ret)
		goto err;

	ret = ctx->m21dev->ops->prepare_target(ctx, &ctx->target);
	if (ret)
		goto err;

	return 0;

err:
	m2m1shot2_put_source_images(ctx);
	return ret;
}

struct m2m1shot2_uaddr {
	s32 __user *target_fence;
	s32 __user *source_fence[M2M1SHOT2_MAX_IMAGES];
	u32 __user *flags;
	void __user *work_delay_uaddr;
	u32 __user *target_payload[M2M1SHOT2_MAX_PLANES];
	bool is_compat;
};

struct m2m1shot2_release_fds {
	struct sync_file *sync_file;
	unsigned int count;
	int fd[M2M1SHOT2_MAX_IMAGES + 1];
};

static int m2m1shot2_create_release_fence(struct m2m1shot2_context *ctx,
					  const struct m2m1shot2_uaddr *ua,
					  struct m2m1shot2_release_fds *rel)
{
	struct m2m1shot2_fence *mfence;
	struct sync_file *sync_file;
	struct dma_fence *fence;
	unsigned int i, ifd = 0;
	s32 __user *user_pfds[M2M1SHOT2_MAX_IMAGES + 1];
	unsigned int num_fences = 0;
	int ret = 0;

	rel->count = 0;

	for (i = 0; i < ctx->num_sources; i++) {
		if (ctx->source[i].img.flags & M2M1SHOT2_IMGFLAG_RELEASE_FENCE)
			user_pfds[num_fences++] = ua->source_fence[i];
	}

	if (ctx->target.flags & M2M1SHOT2_IMGFLAG_RELEASE_FENCE)
		user_pfds[num_fences++] = ua->target_fence;

	if (num_fences == 0)
		return 0;

	mfence = kzalloc(sizeof(*mfence), GFP_KERNEL);
	if (!mfence)
		return -ENOMEM;

	spin_lock_init(&mfence->lock);
	mfence->m21dev = ctx->m21dev;
	fence = &mfence->base;
	dma_fence_init(fence, &m2m1shot2_fence_ops, &mfence->lock,
		       ctx->fence_ctx, ctx->timeline_max + 1);

	sync_file = sync_file_create(fence);
	if (!sync_file) {
		dma_fence_put(fence);
		return -ENOMEM;
	}

	for (i = 0; i < num_fences; i++) {
		int fd = get_unused_fd_flags(O_CLOEXEC);

		if (fd < 0) {
			ret = fd;
			goto err;
		}
		rel->fd[ifd++] = fd;

		if (put_user(fd, user_pfds[i])) {
			ret = -EFAULT;
			goto err;
		}
	}

	rel->sync_file = sync_file;
	rel->count = num_fences;

	ctx->timeline_max++;
	ctx->release_fence = fence;

	return 0;

err:
	while (ifd-- > 0)
		put_unused_fd(rel->fd[ifd]);
	fput(sync_file->file);
	dma_fence_put(fence);
	return ret;
}

/* installed only once the task started: callers ignore fds of a failed ioctl */
static void m2m1shot2_install_release_fds(struct m2m1shot2_release_fds *rel)
{
	unsigned int i;

	for (i = 0; i < rel->count; i++) {
		if (i > 0)
			get_file(rel->sync_file->file);
		fd_install(rel->fd[i], rel->sync_file->file);
	}
}

static void m2m1shot2_discard_release_fds(struct m2m1shot2_release_fds *rel)
{
	unsigned int i;

	if (!rel->count)
		return;

	for (i = 0; i < rel->count; i++)
		put_unused_fd(rel->fd[i]);
	fput(rel->sync_file->file);
}

static int m2m1shot2_wait_put_user(struct m2m1shot2_context *ctx,
				   const struct m2m1shot2_uaddr *ua, u32 userflag)
{
	int ret = 0;
	unsigned int i;

	if (!m2m1shot2_wait_process(ctx->m21dev, ctx)) {
		userflag |= M2M1SHOT2_FLAG_ERROR;
		ret = put_user(userflag, ua->flags);
	} else {
		for (i = 0; i < ctx->target.num_planes; i++)
			ret |= put_user(ctx->target.plane[i].payload,
					ua->target_payload[i]);
		if (!ua->is_compat)
			ret |= put_user(ctx->work_delay_in_nsec,
					(u64 __user *)ua->work_delay_uaddr);
		else
			ret |= put_user((compat_ulong_t)ctx->work_delay_in_nsec,
					(compat_ulong_t __user *)ua->work_delay_uaddr);
	}

	return ret ? -EFAULT : 0;
}

static int m2m1shot2_prepare_priority_context(struct m2m1shot2_context *ctx)
{
	struct m2m1shot2_device *m21dev = ctx->m21dev;
	struct m2m1shot2_context *pctx;
	unsigned long flags;
	int cond = ctx->priority;

	spin_lock_irqsave(&m21dev->lock_ctx, flags);
	list_for_each_entry(pctx, &m21dev->active_contexts, node) {
		if (pctx->priority < cond)
			goto err_busy;
	}

	list_for_each_entry(pctx, &m21dev->contexts, node) {
		if (pctx->priority < cond && M2M1S2_CTXSTATE_BUSY(pctx))
			goto err_busy;
	}

	pctx = m2m1shot2_current_context(m21dev);
	if (pctx && pctx->priority < cond)
		goto err_busy;

	spin_unlock_irqrestore(&m21dev->lock_ctx, flags);
	return 0;

err_busy:
	spin_unlock_irqrestore(&m21dev->lock_ctx, flags);
	return -EBUSY;
}

static int m2m1shot2_do_process(struct m2m1shot2_context *ctx,
				struct m2m1shot2 *data,
				struct m2m1shot2_image sources[],
				const struct m2m1shot2_uaddr *ua)
{
	struct m2m1shot2_device *m21dev = ctx->m21dev;
	struct m2m1shot2_release_fds rel;
	int i, ret;

	/* wait for completion of the previous task */
	if (!M2M1S2_CTXSTATE_IDLE(ctx))
		m2m1shot2_wait_process(m21dev, ctx);

	spin_lock(&m21dev->lock_priority);
	for (i = ctx->priority + 1; i < M2M1SHOT2_PRIORITY_END; i++) {
		if (m21dev->prior_stats[i] > 0)
			break;
	}
	spin_unlock(&m21dev->lock_priority);

	if (i < M2M1SHOT2_PRIORITY_END)
		return -EBUSY;

	kref_init(&ctx->starter);

	ret = m2m1shot2_get_userdata(ctx, data, sources);
	if (ret < 0)
		return ret;

	ctx->release_fence = NULL;
	rel.count = 0;
	if (data->flags & M2M1SHOT2_FLAG_NONBLOCK) {
		ret = m2m1shot2_create_release_fence(ctx, ua, &rel);
		if (ret < 0) {
			m2m1shot2_put_images(ctx);
			return ret;
		}
	}

	ret = m2m1shot2_start_context(ctx->m21dev, ctx);
	if (ret < 0) {
		if (ctx->release_fence) {
			dma_fence_signal(ctx->release_fence);
			dma_fence_put(ctx->release_fence);
			ctx->release_fence = NULL;
		}
		m2m1shot2_discard_release_fds(&rel);
		m2m1shot2_put_images(ctx);
		return ret;
	}

	m2m1shot2_install_release_fds(&rel);

	if (!(data->flags & M2M1SHOT2_FLAG_NONBLOCK)) {
		ret = m2m1shot2_wait_put_user(ctx, ua, data->flags);
		m2m1shot2_put_images(ctx);
	}

	return ret;
}

static noinline_for_stack int m2m1shot2_ioctl_perf(struct m2m1shot2_context *ctx,
						   void __user *arg)
{
	struct m2m1shot2_performance_data data;
	int i;

	if (!ctx->m21dev->ops->prepare_perf) {
		dev_err(ctx->m21dev->dev,
			"%s: it doesn't exist the prepare performance\n", __func__);
		return -ENOTTY;
	}

	if (copy_from_user(&data, arg, sizeof(data))) {
		dev_err(ctx->m21dev->dev,
			"%s: Failed to performance data\n", __func__);
		return -EFAULT;
	}

	if (data.num_frames >= M2M1SHOT2_PERF_MAX_FRAMES)
		return -EINVAL;

	for (i = 0; i < data.num_frames; i++) {
		if (data.frame[i].num_layers >= M2M1SHOT2_MAX_IMAGES)
			return -EINVAL;
	}

	return ctx->m21dev->ops->prepare_perf(ctx, &data);
}

static noinline_for_stack int m2m1shot2_ioctl_process(struct m2m1shot2_context *ctx,
						      void __user *arg)
{
	struct m2m1shot2 __user *uptr = arg;
	struct m2m1shot2 data;
	struct m2m1shot2_image *sources;
	struct m2m1shot2_uaddr ua;
	int i, ret;

	if (copy_from_user(&data, uptr, sizeof(data)))
		return -EFAULT;

	if (data.num_sources < 1 || data.num_sources > M2M1SHOT2_MAX_IMAGES)
		return -EINVAL;

	sources = kmalloc_array(data.num_sources, sizeof(*sources), GFP_KERNEL);
	if (!sources)
		return -ENOMEM;

	if (copy_from_user(sources, data.sources,
			   sizeof(sources[0]) * data.num_sources)) {
		kfree(sources);
		return -EFAULT;
	}

	ua.is_compat = false;
	ua.flags = &uptr->flags;
	ua.work_delay_uaddr = &uptr->work_delay_in_nsec;
	ua.target_fence = &uptr->target.fence;
	for (i = 0; i < data.num_sources; i++)
		ua.source_fence[i] = &data.sources[i].fence;
	for (i = 0; i < M2M1SHOT2_MAX_PLANES; i++)
		ua.target_payload[i] = &uptr->target.plane[i].payload;

	ret = m2m1shot2_do_process(ctx, &data, sources, &ua);
	kfree(sources);
	return ret;
}

static noinline_for_stack int m2m1shot2_ioctl_wait_process(struct m2m1shot2_context *ctx,
							   void __user *arg)
{
	struct m2m1shot2 __user *uptr = arg;
	struct m2m1shot2 data;
	struct m2m1shot2_uaddr ua;
	int i;

	if (M2M1S2_CTXSTATE_IDLE(ctx))
		return -EAGAIN;

	if (copy_from_user(&data, uptr, sizeof(data))) {
		dev_err(ctx->m21dev->dev,
			"%s: Failed to read userdata\n", __func__);
		return -EFAULT;
	}

	ua.is_compat = false;
	ua.flags = &uptr->flags;
	ua.work_delay_uaddr = &uptr->work_delay_in_nsec;
	for (i = 0; i < M2M1SHOT2_MAX_PLANES; i++)
		ua.target_payload[i] = &uptr->target.plane[i].payload;

	return m2m1shot2_wait_put_user(ctx, &ua, data.flags);
}

static noinline_for_stack int m2m1shot2_ioctl_custom(struct m2m1shot2_context *ctx,
						     void __user *arg)
{
	struct m2m1shot2_custom_data data;

	if (!ctx->m21dev->ops->custom_ioctl) {
		dev_err(ctx->m21dev->dev,
			"%s: it doesn't exist the custom ioctl\n", __func__);
		return -ENOTTY;
	}
	if (copy_from_user(&data, arg, sizeof(data))) {
		dev_err(ctx->m21dev->dev,
			"%s: Failed to read userdata\n", __func__);
		return -EFAULT;
	}

	return ctx->m21dev->ops->custom_ioctl(ctx, data.cmd, data.arg);
}

static long m2m1shot2_ioctl(struct file *filp,
			    unsigned int cmd, unsigned long arg)
{
	struct m2m1shot2_context *ctx = filp->private_data;
	struct m2m1shot2_device *m21dev = ctx->m21dev;
	int ret = 0;

	mutex_lock(&ctx->mutex);

	switch (cmd) {
	case M2M1SHOT2_IOC_REQUEST_PERF:
		ret = m2m1shot2_ioctl_perf(ctx, (void __user *)arg);
		break;
	case M2M1SHOT2_IOC_SET_PRIORITY:
	{
		enum m2m1shot2_priority data;

		get_user(data, (enum m2m1shot2_priority __user *)arg);

		if ((data < M2M1SHOT2_LOW_PRIORITY) ||
		    (data >= M2M1SHOT2_PRIORITY_END)) {
			dev_err(m21dev->dev,
				"%s: m2m1shot2 does not allow %d priority\n",
				__func__, data);
			ret = -EINVAL;
			break;
		}

		if (data != ctx->priority) {
			spin_lock(&m21dev->lock_priority);
			m21dev->prior_stats[ctx->priority] -= 1;
			m21dev->prior_stats[data] += 1;
			ctx->priority = data;
			spin_unlock(&m21dev->lock_priority);
		}

		ret = m2m1shot2_prepare_priority_context(ctx);
		break;
	}
	case M2M1SHOT2_IOC_PROCESS:
		ret = m2m1shot2_ioctl_process(ctx, (void __user *)arg);
		break;
	case M2M1SHOT2_IOC_WAIT_PROCESS:
		ret = m2m1shot2_ioctl_wait_process(ctx, (void __user *)arg);
		break;
	case M2M1SHOT2_IOC_CUSTOM:
		ret = m2m1shot2_ioctl_custom(ctx, (void __user *)arg);
		break;
	default:
		dev_err(ctx->m21dev->dev,
			"%s: unknown ioctl command %#x\n", __func__, cmd);
		ret = -EINVAL;
		break;
	}

	mutex_unlock(&ctx->mutex);
	return ret;
}

#ifdef CONFIG_COMPAT
struct compat_m2m1shot2_buffer {
	union {
		compat_ulong_t	userptr;
		__s32		fd;
	};
	__u32		offset;
	__u32		length;
	__u32		payload;
	compat_ulong_t	reserved;
};

struct compat_m2m1shot2_image {
	__u32				flags;
	__s32				fence;
	__u8				memory;
	__u8				num_planes;
	struct compat_m2m1shot2_buffer	plane[M2M1SHOT2_MAX_PLANES];
	struct m2m1shot2_format		fmt;
	struct m2m1shot2_extra		ext;
	__u32				colorspace;
	__u32				reserved[3];
};

struct compat_m2m1shot2 {
	compat_uptr_t			sources;
	struct compat_m2m1shot2_image	target;
	__u8				num_sources;
	__u32				flags;
	__u32				reserved1;
	__u32				reserved2;
	compat_ulong_t			work_delay_in_nsec;
	compat_ulong_t			reserved4;
};

#define COMPAT_M2M1SHOT2_IOC_PROCESS	  _IOWR('M', 4, struct compat_m2m1shot2)
#define COMPAT_M2M1SHOT2_IOC_WAIT_PROCESS _IOR('M', 5, struct compat_m2m1shot2)

static void m2m1shot2_compat_to_native_image(struct m2m1shot2_image *dst,
					     const struct compat_m2m1shot2_image *src)
{
	unsigned int i;

	memset(dst, 0, sizeof(*dst));
	dst->flags = src->flags;
	dst->fence = src->fence;
	dst->memory = src->memory;
	dst->num_planes = src->num_planes;
	for (i = 0; i < min_t(unsigned int, src->num_planes, M2M1SHOT2_MAX_PLANES); i++) {
		dst->plane[i].userptr = src->plane[i].userptr;
		dst->plane[i].offset = src->plane[i].offset;
		dst->plane[i].length = src->plane[i].length;
		dst->plane[i].payload = src->plane[i].payload;
		dst->plane[i].reserved = src->plane[i].reserved;
	}
	dst->fmt = src->fmt;
	dst->ext = src->ext;
	dst->colorspace = src->colorspace;
	dst->reserved[0] = src->reserved[0];
	dst->reserved[1] = src->reserved[1];
	dst->reserved[2] = src->reserved[2];
}

static noinline_for_stack int m2m1shot2_compat_ioctl_process(struct m2m1shot2_context *ctx,
							     void __user *arg)
{
	struct compat_m2m1shot2 __user *cptr = compat_ptr((unsigned long)arg);
	struct compat_m2m1shot2 cdata;
	struct compat_m2m1shot2_image *csources;
	struct m2m1shot2 data;
	struct m2m1shot2_image *sources;
	struct m2m1shot2_uaddr ua;
	int i, ret;

	if (copy_from_user(&cdata, cptr, sizeof(cdata)))
		return -EFAULT;

	if (cdata.num_sources < 1 || cdata.num_sources > M2M1SHOT2_MAX_IMAGES)
		return -EINVAL;

	csources = kmalloc_array(cdata.num_sources, sizeof(*csources), GFP_KERNEL);
	if (!csources)
		return -ENOMEM;

	sources = kmalloc_array(cdata.num_sources, sizeof(*sources), GFP_KERNEL);
	if (!sources) {
		kfree(csources);
		return -ENOMEM;
	}

	if (copy_from_user(csources, compat_ptr(cdata.sources),
			   sizeof(csources[0]) * cdata.num_sources)) {
		kfree(sources);
		kfree(csources);
		return -EFAULT;
	}

	data.flags = cdata.flags;
	data.num_sources = cdata.num_sources;
	data.work_delay_in_nsec = cdata.work_delay_in_nsec;
	m2m1shot2_compat_to_native_image(&data.target, &cdata.target);

	for (i = 0; i < cdata.num_sources; i++)
		m2m1shot2_compat_to_native_image(&sources[i], &csources[i]);

	kfree(csources);

	ua.is_compat = true;
	ua.flags = &cptr->flags;
	ua.work_delay_uaddr = &cptr->work_delay_in_nsec;
	ua.target_fence = &cptr->target.fence;
	for (i = 0; i < cdata.num_sources; i++) {
		struct compat_m2m1shot2_image __user *csrc = compat_ptr(cdata.sources);

		ua.source_fence[i] = &csrc[i].fence;
	}
	for (i = 0; i < M2M1SHOT2_MAX_PLANES; i++)
		ua.target_payload[i] = &cptr->target.plane[i].payload;

	mutex_lock(&ctx->mutex);
	ret = m2m1shot2_do_process(ctx, &data, sources, &ua);
	mutex_unlock(&ctx->mutex);

	kfree(sources);
	return ret;
}

static noinline_for_stack int m2m1shot2_compat_ioctl_wait(struct m2m1shot2_context *ctx,
							  void __user *arg)
{
	struct compat_m2m1shot2 __user *cptr = compat_ptr((unsigned long)arg);
	struct compat_m2m1shot2 cdata;
	struct m2m1shot2_uaddr ua;
	int i, ret;

	mutex_lock(&ctx->mutex);
	if (M2M1S2_CTXSTATE_IDLE(ctx)) {
		mutex_unlock(&ctx->mutex);
		return -EAGAIN;
	}

	if (copy_from_user(&cdata, cptr, sizeof(cdata))) {
		mutex_unlock(&ctx->mutex);
		return -EFAULT;
	}

	ua.is_compat = true;
	ua.flags = &cptr->flags;
	ua.work_delay_uaddr = &cptr->work_delay_in_nsec;
	for (i = 0; i < M2M1SHOT2_MAX_PLANES; i++)
		ua.target_payload[i] = &cptr->target.plane[i].payload;

	ret = m2m1shot2_wait_put_user(ctx, &ua, cdata.flags);
	mutex_unlock(&ctx->mutex);
	return ret;
}

static long m2m1shot2_compat_ioctl32(struct file *filp,
				     unsigned int cmd, unsigned long arg)
{
	struct m2m1shot2_context *ctx = filp->private_data;

	switch (cmd) {
	case COMPAT_M2M1SHOT2_IOC_PROCESS:
		return m2m1shot2_compat_ioctl_process(ctx, (void __user *)arg);
	case COMPAT_M2M1SHOT2_IOC_WAIT_PROCESS:
		return m2m1shot2_compat_ioctl_wait(ctx, (void __user *)arg);
	case M2M1SHOT2_IOC_CUSTOM:
	case M2M1SHOT2_IOC_SET_PRIORITY:
	case M2M1SHOT2_IOC_REQUEST_PERF:
		return m2m1shot2_ioctl(filp, cmd, (unsigned long)compat_ptr(arg));
	default:
		dev_err(ctx->m21dev->dev, "%s: unknown ioctl command %#x\n", __func__, cmd);
		return -EINVAL;
	}
}
#endif

static const struct file_operations m2m1shot2_fops = {
	.owner          = THIS_MODULE,
	.open           = m2m1shot2_open,
	.release        = m2m1shot2_release,
	.unlocked_ioctl	= m2m1shot2_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= m2m1shot2_compat_ioctl32,
#endif
};

struct m2m1shot2_device *m2m1shot2_create_device(struct device *dev,
					const struct m2m1shot2_devops *ops,
					const char *nodename, int id,
					unsigned long attr)
{
	struct m2m1shot2_device *m21dev;
	char *name;
	size_t name_size;
	int ret = -ENOMEM;

	if (!ops || !ops->init_context || !ops->free_context ||
	    !ops->prepare_format || !ops->device_run) {
		dev_err(dev, "%s: m2m1shot2_devops is insufficient\n", __func__);
		return ERR_PTR(-EINVAL);
	}

	if (!nodename) {
		dev_err(dev, "%s: node name is not specified\n", __func__);
		return ERR_PTR(-EINVAL);
	}

	name_size = strlen(nodename) + 1;
	if (id >= 0)
		name_size += 3;

	name = kmalloc(name_size, GFP_KERNEL);
	if (!name)
		return ERR_PTR(-ENOMEM);

	if (id < 0)
		strscpy(name, nodename, name_size);
	else
		scnprintf(name, name_size, "%s%d", nodename, id);

	m21dev = kzalloc(sizeof(*m21dev), GFP_KERNEL);
	if (!m21dev)
		goto err_m21dev;

	spin_lock_init(&m21dev->lock_ctx);
	spin_lock_init(&m21dev->lock_priority);
	mutex_init(&m21dev->lock_qos);
	INIT_LIST_HEAD(&m21dev->contexts);
	INIT_LIST_HEAD(&m21dev->active_contexts);
	INIT_LIST_HEAD(&m21dev->qos_contexts);
	INIT_WORK(&m21dev->schedule_work, m2m1shot2_device_schedule_work);

	m21dev->schedule_workqueue =
		alloc_ordered_workqueue("m2m1shot2-scheduler", WQ_MEM_RECLAIM);
	if (!m21dev->schedule_workqueue) {
		dev_err(dev, "failed to create workqueue for scheduling\n");
		ret = -ENOMEM;
		goto err_workqueue;
	}

	m21dev->destroy_workqueue =
		alloc_ordered_workqueue("m2m1shot2-destructor", 0);
	if (!m21dev->destroy_workqueue) {
		dev_err(dev, "failed to create workqueue for context destruction\n");
		ret = -ENOMEM;
		goto err_workqueue2;
	}

	m21dev->dev = dev;
	m21dev->ops = ops;
	m21dev->attr = attr;

	m21dev->misc.minor = MISC_DYNAMIC_MINOR;
	m21dev->misc.name = name;
	m21dev->misc.fops = &m2m1shot2_fops;
	ret = misc_register(&m21dev->misc);
	if (ret)
		goto err_misc;

	dev_info(dev, "registered as m2m1shot2 device\n");
	return m21dev;

err_misc:
	destroy_workqueue(m21dev->destroy_workqueue);
err_workqueue2:
	destroy_workqueue(m21dev->schedule_workqueue);
err_workqueue:
	kfree(m21dev);
err_m21dev:
	kfree(name);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL(m2m1shot2_create_device);

void m2m1shot2_destroy_device(struct m2m1shot2_device *m21dev)
{
	destroy_workqueue(m21dev->destroy_workqueue);
	destroy_workqueue(m21dev->schedule_workqueue);
	misc_deregister(&m21dev->misc);
	kfree(m21dev->misc.name);
	kfree(m21dev);
}
EXPORT_SYMBOL(m2m1shot2_destroy_device);

/* the userspace ABI of the vendor kernel, native and compat */
static_assert(sizeof(struct m2m1shot2) == 272);
static_assert(sizeof(struct m2m1shot2_image) == 232);
static_assert(sizeof(struct m2m1shot2_buffer) == 32);
static_assert(sizeof(struct m2m1shot2_performance_data) == 360);
static_assert(sizeof(struct m2m1shot2_custom_data) == 8);
static_assert(offsetof(struct m2m1shot2, target) == 8);
static_assert(offsetof(struct m2m1shot2, num_sources) == 240);
static_assert(offsetof(struct m2m1shot2, flags) == 244);
static_assert(offsetof(struct m2m1shot2, work_delay_in_nsec) == 256);
static_assert(offsetof(struct m2m1shot2_image, plane) == 16);
static_assert(offsetof(struct m2m1shot2_image, fmt) == 144);
static_assert(offsetof(struct m2m1shot2_image, ext) == 188);
static_assert(offsetof(struct m2m1shot2_image, colorspace) == 212);

#ifdef CONFIG_COMPAT
static_assert(sizeof(struct compat_m2m1shot2) == 204);
static_assert(sizeof(struct compat_m2m1shot2_image) == 176);
static_assert(sizeof(struct compat_m2m1shot2_buffer) == 20);
static_assert(offsetof(struct compat_m2m1shot2, target) == 4);
static_assert(offsetof(struct compat_m2m1shot2, num_sources) == 180);
static_assert(offsetof(struct compat_m2m1shot2, flags) == 184);
static_assert(offsetof(struct compat_m2m1shot2, work_delay_in_nsec) == 196);
static_assert(offsetof(struct compat_m2m1shot2_image, plane) == 12);
static_assert(offsetof(struct compat_m2m1shot2_image, fmt) == 92);
static_assert(offsetof(struct compat_m2m1shot2_image, ext) == 136);
static_assert(offsetof(struct compat_m2m1shot2_image, colorspace) == 160);
#endif

MODULE_DESCRIPTION("Exynos extended non-streaming m2m media processing (m2m1shot2)");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("DMA_BUF");

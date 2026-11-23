/*
 *
 * (C) COPYRIGHT ARM Limited. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU licence.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can access it online at
 * http://www.gnu.org/licenses/gpl-2.0.html.
 *
 * SPDX-License-Identifier: GPL-2.0
 *
 *//*
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU licence.
 *
 * A copy of the licence is included with the program, and can also be obtained
 * from Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "linux/mman.h"
#include "../mali_kbase.h"

/* mali_kbase_mmap.c
 *
 * This file contains Linux specific implementation of
 * kbase_get_unmapped_area() interface.
 */


/*
 * The VMA tree cannot be walked any more: search with vm_unmapped_area() and
 * lower high_limit until the shader code or same 4GB alignment holds, as later
 * Arm releases do.
 */

/**
 * shader_code_align_and_check() - Align the specified pointer according to shader code
 *                     requirement.
 *
 * @gap_end:           Highest possible start address for alignment. The caller must ensure
 *                     the input has already been properly aligned with info contained fields.
 * @info:              vm_unmapped_area_info structure passed, containing alignment, length
 *                     and limits for the allocation
 *
 * Return: true if gap_end is now aligned correctly, false otherwise
 */
static bool shader_code_align_and_check(unsigned long *gap_end,
					struct vm_unmapped_area_info *info)
{
	unsigned long align_adjust = (info->align_offset ? info->align_offset :
			info->length);
	unsigned long align_floor = info->low_limit + align_adjust;

	/* Check for 4GB boundary */
	if (0 == (*gap_end & BASE_MEM_MASK_4GB) && *gap_end >= align_floor)
		(*gap_end) -= align_adjust;
	if (0 == ((*gap_end + info->length) & BASE_MEM_MASK_4GB) &&
	    *gap_end >= align_floor)
		(*gap_end) -= align_adjust;

	return ((*gap_end & BASE_MEM_MASK_4GB) &&
		((*gap_end + info->length) & BASE_MEM_MASK_4GB));
}

/**
 * align_4gb_no_straddle() - Align the specified pointer not to straddle over a
 *                           4GB boundary.
 *
 * @gap_end:           Highest possible start address for alignment. The caller must ensure
 *                     the input has already been properly aligned with info contained fields.
 * @info:              vm_unmapped_area_info structure passed, containing alignment, length
 *                     and limits for the allocation
 *
 * Return: true is always expected and the gap_end is aligned correctly
 */
static bool align_4gb_no_straddle(unsigned long *gap_end,
				  struct vm_unmapped_area_info *info)
{
	unsigned long start = *gap_end;
	unsigned long end = *gap_end + info->length;
	unsigned long mask = ~((unsigned long)U32_MAX);

	/* Check if 4GB boundary is straddled */
	if ((start & mask) != ((end - 1) & mask)) {
		unsigned long offset = end - (end & mask);
		/* This is to ensure that alignment doesn't get
		 * disturbed in an attempt to prevent straddling at
		 * 4GB boundary. The GPU VA is aligned to 2MB when the
		 * allocation size is > 2MB and there is enough CPU &
		 * GPU virtual space.
		 */
		unsigned long rounded_offset =
				ALIGN(offset, info->align_mask + 1);

		start -= rounded_offset;
		end -= rounded_offset;

		*gap_end = start;

		/* The preceding 4GB boundary shall not get straddled,
		 * even after accounting for the alignment, as the
		 * size of allocation is limited to 4GB and the initial
		 * start location was already aligned.
		 */
		if (WARN_ON((start & mask) != ((end - 1) & mask)))
			return false;
	}

	return true;
}

/**
 * kbase_unmapped_area_topdown() - allocates new areas top-down from
 *                                 below the stack limit.
 * @info:              Information about the memory area to allocate.
 * @is_shader_code:    Boolean which denotes whether the allocated area is
 *                      intended for the use by shader core in which case a
 *                      special alignment requirements apply.
 * @is_same_4gb_page: Boolean which indicates whether the allocated area needs
 *                    to reside completely within a 4GB chunk.
 *
 * Return: address of the found gap end (high limit) if area is found;
 *         -ENOMEM if search is unsuccessful
 */
static unsigned long kbase_unmapped_area_topdown(struct vm_unmapped_area_info
		*info, bool is_shader_code, bool is_same_4gb_page)
{
	struct vm_unmapped_area_info tmp_info = *info;
	unsigned long length;

	tmp_info.flags |= VM_UNMAPPED_AREA_TOPDOWN;
	if (!(is_shader_code || is_same_4gb_page))
		return vm_unmapped_area(&tmp_info);

	length = info->length + info->align_mask;

	/* Due to additional alignment requirement, shader_code or same_4gb_page
	 * needs iterations for alignment search and confirmation check.
	 */
	while (true) {
		unsigned long saved_high_lmt = tmp_info.high_limit;
		unsigned long gap_end, start, rev_high_limit;

		gap_end = vm_unmapped_area(&tmp_info);
		if (IS_ERR_VALUE(gap_end))
			return gap_end;

		start = gap_end;
		if (is_shader_code) {
			bool shader_code_aligned;
			unsigned long align_cmp_ref;

			while (true) {
				/* The loop ends when the alignment holds or
				 * cannot progress any further.
				 */
				align_cmp_ref = gap_end;

				shader_code_aligned =
					shader_code_align_and_check(&gap_end, &tmp_info);
				if (shader_code_aligned || align_cmp_ref == gap_end)
					break;
			}

			if (!shader_code_aligned)
				break;

			if (start == gap_end)
				return gap_end;

			rev_high_limit = gap_end + length;
		} else {
			/* must be same_4gb_page case */
			if (unlikely(!align_4gb_no_straddle(&gap_end, &tmp_info)))
				break;

			if (start == gap_end)
				return gap_end;

			rev_high_limit = gap_end + length;
		}

		if (rev_high_limit < info->low_limit)
			break;

		if (WARN_ONCE(rev_high_limit >= saved_high_lmt,
			      "Unexpected recurring high_limit in search, %lx => %lx\n"
			      "\tinfo-input: limit=[%lx, %lx], mask=%lx, len=%lx\n",
			      saved_high_lmt, rev_high_limit, info->low_limit, info->high_limit,
			      info->align_mask, info->length))
			rev_high_limit = saved_high_lmt -
					 (info->align_offset ? info->align_offset : info->length);

		/* Repeat the search with a decreasing rev_high_limit */
		tmp_info.high_limit = rev_high_limit;
	}

	return -ENOMEM;
}


/* This function is based on Linux kernel's arch_get_unmapped_area, but
 * simplified slightly. Modifications come from the fact that some values
 * about the memory area are known in advance.
 */
unsigned long kbase_get_unmapped_area(struct file *filp,
		const unsigned long addr, const unsigned long len,
		const unsigned long pgoff, const unsigned long flags)
{
	struct kbase_context *kctx = filp->private_data;
	struct mm_struct *mm = current->mm;
	struct vm_unmapped_area_info info = {};
	unsigned long align_offset = 0;
	unsigned long align_mask = 0;
	unsigned long high_limit = mm->mmap_base;
	unsigned long low_limit = PAGE_SIZE;
	int cpu_va_bits = BITS_PER_LONG;
	int gpu_pc_bits =
	      kctx->kbdev->gpu_props.props.core_props.log2_program_counter_size;
	bool is_shader_code = false;
	bool is_same_4gb_page = false;
	unsigned long ret;

	/* err on fixed address */
	if ((flags & MAP_FIXED) || addr)
		return -EINVAL;

#ifdef CONFIG_64BIT
	/* too big? */
	if (len > TASK_SIZE - SZ_2M)
		return -ENOMEM;

	if (!kbase_ctx_flag(kctx, KCTX_COMPAT)) {

		high_limit = min_t(unsigned long, mm->mmap_base,
				(kctx->same_va_end << PAGE_SHIFT));

		/* If there's enough (> 33 bits) of GPU VA space, align
		 * to 2MB boundaries.
		 */
		if (kctx->kbdev->gpu_props.mmu.va_bits > 33) {
			if (len >= SZ_2M) {
				align_offset = SZ_2M;
				align_mask = SZ_2M - 1;
			}
		}

		low_limit = SZ_2M;
	} else {
		cpu_va_bits = 32;
	}
#endif /* CONFIG_64BIT */
	if ((PFN_DOWN(BASE_MEM_COOKIE_BASE) <= pgoff) &&
		(PFN_DOWN(BASE_MEM_FIRST_FREE_ADDRESS) > pgoff)) {
			int cookie = pgoff - PFN_DOWN(BASE_MEM_COOKIE_BASE);
			struct kbase_va_region *reg;

			/* Need to hold gpu vm lock when using reg */
			kbase_gpu_vm_lock(kctx);
			reg = kctx->pending_regions[cookie];
			if (!reg) {
				kbase_gpu_vm_unlock(kctx);
				return -EINVAL;
			}
			if (!(reg->flags & KBASE_REG_GPU_NX)) {
				if (cpu_va_bits > gpu_pc_bits) {
					align_offset = 1ULL << gpu_pc_bits;
					align_mask = align_offset - 1;
					is_shader_code = true;
				}
			} else if (reg->flags & KBASE_REG_TILER_ALIGN_TOP) {
				unsigned long extent_bytes =
				     (unsigned long)(reg->extent << PAGE_SHIFT);
				/* kbase_check_alloc_sizes() already satisfies
				 * these checks, but they're here to avoid
				 * maintenance hazards due to the assumptions
				 * involved */
				WARN_ON(reg->extent > (ULONG_MAX >> PAGE_SHIFT));
				WARN_ON(reg->initial_commit > (ULONG_MAX >> PAGE_SHIFT));
				WARN_ON(!is_power_of_2(extent_bytes));
				align_mask = extent_bytes - 1;
				align_offset =
				      extent_bytes - (reg->initial_commit << PAGE_SHIFT);
			} else if (reg->flags & KBASE_REG_GPU_VA_SAME_4GB_PAGE) {
				is_same_4gb_page = true;
			}
			kbase_gpu_vm_unlock(kctx);
#ifndef CONFIG_64BIT
	} else {
		return current->mm->get_unmapped_area(filp, addr, len, pgoff,
						      flags);
#endif
	}

	info.flags = 0;
	info.length = len;
	info.low_limit = low_limit;
	info.high_limit = high_limit;
	info.align_offset = align_offset;
	info.align_mask = align_mask;

	ret = kbase_unmapped_area_topdown(&info, is_shader_code,
			is_same_4gb_page);

	if (IS_ERR_VALUE(ret) && high_limit == mm->mmap_base &&
			high_limit < (kctx->same_va_end << PAGE_SHIFT)) {
		/* Retry above mmap_base */
		info.low_limit = mm->mmap_base;
		info.high_limit = min_t(u64, TASK_SIZE,
					(kctx->same_va_end << PAGE_SHIFT));

		ret = kbase_unmapped_area_topdown(&info, is_shader_code,
				is_same_4gb_page);
	}

	return ret;
}

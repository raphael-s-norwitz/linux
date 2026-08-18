/*
 * Copyright (c) 2013-2015, Mellanox Technologies. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <rdma/ib_umem_odp.h>
#include <rdma/iter.h>
#include "mlx5_ib.h"

/*
 * Stage-3 destination-side umem bind for a CRIU-restored user MR.
 * Composes ib_umem_pin() -- which pins the user pages and builds the
 * sg_append_table without dma_map_sgtable, so sg_dma_address /
 * sg_dma_len are left unset -- with mlx5_vfmig_bind_user_mr(), which
 * iommu_maps each sg at the placeholder IOVA range LOAD_VHCA_STATE
 * installed for (KIND_MR, @mkey_index), populates sg_dma_address /
 * sg_dma_len, and flips the placeholder awaiting_bind true -> false.
 *
 * On bind failure the pinned umem is unwound via ib_umem_release(); the
 * vfmig unmap_sg path skips zero-iova sgs, so a partial bind (already
 * rolled back inside mlx5_vfmig_bind_user_mr) sees no double unmap.
 *
 * @mkey_index must match the source mkey >> 8 the SAVE-side retag
 * emitted as the HOST_USER_PAGE record's fw_id. Returns the populated
 * umem (the caller stores it in mr->umem) or ERR_PTR with all resources
 * released.
 */
struct ib_umem *mlx5_ib_umem_restore_mr(struct mlx5_ib_dev *dev,
					u32 mkey_index, unsigned long addr,
					size_t size, int access)
{
	struct ib_umem *umem;
	int err;

	umem = ib_umem_pin(&dev->ib_dev, addr, size, access);
	if (IS_ERR(umem))
		return umem;

	err = mlx5_vfmig_bind_user_mr(dev->mdev, mkey_index,
				      &umem->sgt_append.sgt);
	if (err) {
		ib_umem_release(umem);
		return ERR_PTR(err);
	}
	return umem;
}

/*
 * Stage-3 destination-side umem bind for a CRIU-restored user CQ's
 * CQE-ring buffer. Same composition as mlx5_ib_umem_restore_mr modulo
 * the kind: ib_umem_pin() with IB_ACCESS_LOCAL_WRITE (the FW writes
 * CQEs into the buffer, matching the source-side create-time
 * ib_umem_get access) followed by mlx5_vfmig_bind_user_cq(), which
 * iommu_maps each sg at the (KIND_CQ, @cqn) placeholder IOVA range.
 *
 * @cqn must match the source cqn the SAVE-side retag emitted. On bind
 * failure the pinned umem is unwound via ib_umem_release(); returns the
 * populated umem (stored by the caller in cq->buf.umem) or ERR_PTR.
 */
struct ib_umem *mlx5_ib_umem_restore_cq(struct mlx5_ib_dev *dev, u32 cqn,
					unsigned long addr, size_t size)
{
	struct ib_umem *umem;
	int err;

	umem = ib_umem_pin(&dev->ib_dev, addr, size, IB_ACCESS_LOCAL_WRITE);
	if (IS_ERR(umem))
		return umem;

	err = mlx5_vfmig_bind_user_cq(dev->mdev, cqn,
				      &umem->sgt_append.sgt);
	if (err) {
		ib_umem_release(umem);
		return ERR_PTR(err);
	}
	return umem;
}

/*
 * Stage-3 destination-side umem bind for a CRIU-restored user QP's
 * WQ-ring (RQ + SQ in one contiguous mapping). Same composition as
 * mlx5_ib_umem_restore_cq modulo the kind: ib_umem_pin() with access 0
 * (the FW reads WQE descriptors out of the buffer, matching the
 * source-side _create_user_qp ib_umem_get(... 0); the doorbell records
 * live in a separate page bound by mlx5_ib_db_map_user_restore) followed
 * by mlx5_vfmig_bind_user_qp(), which iommu_maps each sg at the
 * (KIND_QP, @qpn) placeholder IOVA range.
 *
 * @size must be (rq_wqe_count << rq_wqe_shift) + (sq_wqe_count <<
 * ilog2(MLX5_SEND_WQE_BB)) -- the byte length set_user_buf_size composes
 * for QPC-managed (RC / UC / UD) QPs and the length the SAVE-side retag
 * emitted for the placeholder. This helper is QPC-only; raw_packet QPs
 * split the ring across two umems and would need a separate helper.
 *
 * @qpn must match the source qpn the SAVE-side retag emitted. On bind
 * failure the pinned umem is unwound via ib_umem_release(); returns the
 * populated umem (stored by the caller in base->ubuffer.umem) or ERR_PTR.
 */
struct ib_umem *mlx5_ib_umem_restore_qp(struct mlx5_ib_dev *dev, u32 qpn,
					unsigned long addr, size_t size)
{
	struct ib_umem *umem;
	int err;

	umem = ib_umem_pin(&dev->ib_dev, addr, size, 0);
	if (IS_ERR(umem))
		return umem;

	err = mlx5_vfmig_bind_user_qp(dev->mdev, qpn,
				      &umem->sgt_append.sgt);
	if (err) {
		ib_umem_release(umem);
		return ERR_PTR(err);
	}
	return umem;
}

/*
 * Fill in a physical address list. ib_umem_num_dma_blocks() entries will be
 * filled in the pas array.
 */
void mlx5_ib_populate_pas(struct ib_umem *umem, size_t page_size, __be64 *pas,
			  u64 access_flags)
{
	struct ib_block_iter biter;

	rdma_umem_for_each_dma_block (umem, &biter, page_size) {
		*pas = cpu_to_be64(rdma_block_iter_dma_address(&biter) |
				   access_flags);
		pas++;
	}
}

/*
 * Compute the page shift and page_offset for mailboxes that use a quantized
 * page_offset. The granulatity of the page offset scales according to page
 * size.
 */
unsigned long __mlx5_umem_find_best_quantized_pgoff(
	struct ib_umem *umem, unsigned long pgsz_bitmap,
	unsigned int page_offset_bits, u64 pgoff_bitmask, unsigned int scale,
	unsigned int *page_offset_quantized)
{
	const u64 page_offset_mask = (1UL << page_offset_bits) - 1;
	unsigned long page_size;
	u64 page_offset;

	page_size = ib_umem_find_best_pgoff(umem, pgsz_bitmap, pgoff_bitmask);
	if (!page_size)
		return 0;

	/*
	 * page size is the largest possible page size.
	 *
	 * Reduce the page_size, and thus the page_offset and quanta, until the
	 * page_offset fits into the mailbox field. Once page_size < scale this
	 * loop is guaranteed to terminate.
	 */
	page_offset = ib_umem_dma_offset(umem, page_size);
	while (page_offset & ~(u64)(page_offset_mask * (page_size / scale))) {
		page_size /= 2;
		page_offset = ib_umem_dma_offset(umem, page_size);
	}

	/*
	 * The address is not aligned, or otherwise cannot be represented by the
	 * page_offset.
	 */
	if (!(pgsz_bitmap & page_size))
		return 0;

	*page_offset_quantized =
		(unsigned long)page_offset / (page_size / scale);
	if (WARN_ON(*page_offset_quantized > page_offset_mask))
		return 0;
	return page_size;
}

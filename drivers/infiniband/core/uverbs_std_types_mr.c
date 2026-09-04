/*
 * Copyright (c) 2018, Mellanox Technologies inc.  All rights reserved.
 * Copyright (c) 2020, Intel Corporation.  All rights reserved.
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

#include <linux/dma-buf.h>
#include <linux/file.h>
#include "rdma_core.h"
#include "uverbs.h"
#include <rdma/uverbs_std_types.h>
#include "restrack.h"

static int uverbs_free_mr(struct ib_uobject *uobject,
			  enum rdma_remove_reason why,
			  struct uverbs_attr_bundle *attrs)
{
	struct ib_umr_object *umr_uobj = to_ib_umr_object(uobject);
	int ret;

	ret = ib_dereg_mr_user((struct ib_mr *)uobject->object,
			       &attrs->driver_udata);
	if (ret)
		return ret;

	if (umr_uobj->dmabuf) {
		dma_buf_put(umr_uobj->dmabuf);
		umr_uobj->dmabuf = NULL;
	}

	return 0;
}

static int UVERBS_HANDLER(UVERBS_METHOD_ADVISE_MR)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_pd *pd =
		uverbs_attr_get_obj(attrs, UVERBS_ATTR_ADVISE_MR_PD_HANDLE);
	enum ib_uverbs_advise_mr_advice advice;
	struct ib_device *ib_dev = pd->device;
	struct ib_sge *sg_list;
	int num_sge;
	u32 flags;
	int ret;

	/* FIXME: Extend the UAPI_DEF_OBJ_NEEDS_FN stuff.. */
	if (!ib_dev->ops.advise_mr)
		return -EOPNOTSUPP;

	ret = uverbs_get_const(&advice, attrs, UVERBS_ATTR_ADVISE_MR_ADVICE);
	if (ret)
		return ret;

	ret = uverbs_get_flags32(&flags, attrs, UVERBS_ATTR_ADVISE_MR_FLAGS,
				 IB_UVERBS_ADVISE_MR_FLAG_FLUSH);
	if (ret)
		return ret;

	num_sge = uverbs_attr_ptr_get_array_size(
		attrs, UVERBS_ATTR_ADVISE_MR_SGE_LIST, sizeof(struct ib_sge));
	if (num_sge <= 0)
		return num_sge;

	sg_list = uverbs_attr_get_alloced_ptr(attrs,
					      UVERBS_ATTR_ADVISE_MR_SGE_LIST);
	return ib_dev->ops.advise_mr(pd, advice, flags, sg_list, num_sge,
				     attrs);
}

static int UVERBS_HANDLER(UVERBS_METHOD_DM_MR_REG)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_dm_mr_attr attr = {};
	struct ib_uobject *uobj =
		uverbs_attr_get_uobject(attrs, UVERBS_ATTR_REG_DM_MR_HANDLE);
	struct ib_dm *dm =
		uverbs_attr_get_obj(attrs, UVERBS_ATTR_REG_DM_MR_DM_HANDLE);
	struct ib_pd *pd =
		uverbs_attr_get_obj(attrs, UVERBS_ATTR_REG_DM_MR_PD_HANDLE);
	struct ib_device *ib_dev = pd->device;

	struct ib_mr *mr;
	int ret;

	if (!ib_dev->ops.reg_dm_mr)
		return -EOPNOTSUPP;

	ret = uverbs_copy_from(&attr.offset, attrs, UVERBS_ATTR_REG_DM_MR_OFFSET);
	if (ret)
		return ret;

	ret = uverbs_copy_from(&attr.length, attrs,
			       UVERBS_ATTR_REG_DM_MR_LENGTH);
	if (ret)
		return ret;

	ret = uverbs_get_flags32(&attr.access_flags, attrs,
				 UVERBS_ATTR_REG_DM_MR_ACCESS_FLAGS,
				 IB_ACCESS_SUPPORTED);
	if (ret)
		return ret;

	if (!(attr.access_flags & IB_ZERO_BASED))
		return -EINVAL;

	ret = ib_check_mr_access(ib_dev, attr.access_flags);
	if (ret)
		return ret;

	if (attr.offset > dm->length || attr.length > dm->length ||
	    attr.length > dm->length - attr.offset)
		return -EINVAL;

	mr = pd->device->ops.reg_dm_mr(pd, dm, &attr, attrs);
	if (IS_ERR(mr))
		return PTR_ERR(mr);

	mr->device  = pd->device;
	mr->pd      = pd;
	mr->type    = IB_MR_TYPE_DM;
	mr->dm      = dm;
	mr->uobject = uobj;
	mr->access_flags = attr.access_flags;
	/* DM MRs have no user VA -- mr->user_addr stays 0. */
	atomic_inc(&pd->usecnt);
	atomic_inc(&dm->usecnt);

	rdma_restrack_new(&mr->res, RDMA_RESTRACK_MR);
	rdma_restrack_set_name(&mr->res, NULL);
	rdma_restrack_add(&mr->res);
	uobj->object = mr;

	uverbs_finalize_uobj_create(attrs, UVERBS_ATTR_REG_DM_MR_HANDLE);

	ret = uverbs_copy_to(attrs, UVERBS_ATTR_REG_DM_MR_RESP_LKEY, &mr->lkey,
			     sizeof(mr->lkey));
	if (ret)
		return ret;

	ret = uverbs_copy_to(attrs, UVERBS_ATTR_REG_DM_MR_RESP_RKEY,
			     &mr->rkey, sizeof(mr->rkey));
	return ret;
}

static int UVERBS_HANDLER(UVERBS_METHOD_QUERY_MR)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_mr *mr =
		uverbs_attr_get_obj(attrs, UVERBS_ATTR_QUERY_MR_HANDLE);
	int ret;

	ret = uverbs_copy_to(attrs, UVERBS_ATTR_QUERY_MR_RESP_LKEY, &mr->lkey,
			     sizeof(mr->lkey));
	if (ret)
		return ret;

	ret = uverbs_copy_to(attrs, UVERBS_ATTR_QUERY_MR_RESP_RKEY,
			     &mr->rkey, sizeof(mr->rkey));

	if (ret)
		return ret;

	ret = uverbs_copy_to(attrs, UVERBS_ATTR_QUERY_MR_RESP_LENGTH,
			     &mr->length, sizeof(mr->length));

	if (ret)
		return ret;

	ret = uverbs_copy_to(attrs, UVERBS_ATTR_QUERY_MR_RESP_IOVA,
			     &mr->iova, sizeof(mr->iova));

	if (IS_UVERBS_COPY_ERR(ret))
		return ret;

	/*
	 * Core-owned bookkeeping populated by the REG_MR / RESTORE_MR
	 * generic handlers. These attrs are UA_OPTIONAL so existing
	 * userspace callers that only requested the four legacy outs
	 * above stay byte-compatible (uverbs_copy_to silently no-ops
	 * when the caller did not provide the OUT slot).
	 */
	ret = uverbs_copy_to(attrs, UVERBS_ATTR_QUERY_MR_RESP_USER_ADDR,
			     &mr->user_addr, sizeof(mr->user_addr));
	if (IS_UVERBS_COPY_ERR(ret))
		return ret;

	ret = uverbs_copy_to(attrs, UVERBS_ATTR_QUERY_MR_RESP_ACCESS_FLAGS,
			     &mr->access_flags, sizeof(mr->access_flags));

	return IS_UVERBS_COPY_ERR(ret) ? ret : 0;
}

static int UVERBS_HANDLER(UVERBS_METHOD_REG_DMABUF_MR)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_uobject *uobj =
		uverbs_attr_get_uobject(attrs, UVERBS_ATTR_REG_DMABUF_MR_HANDLE);
	struct ib_pd *pd =
		uverbs_attr_get_obj(attrs, UVERBS_ATTR_REG_DMABUF_MR_PD_HANDLE);
	struct ib_device *ib_dev = pd->device;

	u64 offset, length, iova;
	struct dma_buf *dmabuf;
	u32 fd, access_flags;
	struct ib_mr *mr;
	int ret;

	if (!ib_dev->ops.reg_user_mr_dmabuf)
		return -EOPNOTSUPP;

	ret = uverbs_copy_from(&offset, attrs,
			       UVERBS_ATTR_REG_DMABUF_MR_OFFSET);
	if (ret)
		return ret;

	ret = uverbs_copy_from(&length, attrs,
			       UVERBS_ATTR_REG_DMABUF_MR_LENGTH);
	if (ret)
		return ret;

	ret = uverbs_copy_from(&iova, attrs,
			       UVERBS_ATTR_REG_DMABUF_MR_IOVA);
	if (ret)
		return ret;

	if ((offset & ~PAGE_MASK) != (iova & ~PAGE_MASK))
		return -EINVAL;

	ret = uverbs_copy_from(&fd, attrs,
			       UVERBS_ATTR_REG_DMABUF_MR_FD);
	if (ret)
		return ret;

	ret = uverbs_get_flags32(&access_flags, attrs,
				 UVERBS_ATTR_REG_DMABUF_MR_ACCESS_FLAGS,
				 IB_ACCESS_LOCAL_WRITE |
				 IB_ACCESS_REMOTE_READ |
				 IB_ACCESS_REMOTE_WRITE |
				 IB_ACCESS_REMOTE_ATOMIC |
				 IB_ACCESS_RELAXED_ORDERING);
	if (ret)
		return ret;

	ret = ib_check_mr_access(ib_dev, access_flags);
	if (ret)
		return ret;

	/*
	 * Retain our own reference to the dma_buf, independent of the one
	 * the provider takes internally via ib_umem_dmabuf_get(). This gives
	 * uverbs core a stable, provider-independent handle on the backing
	 * file for MR_EXPORT_DMABUF_FD, released in uverbs_free_mr().
	 *
	 * Known limitation (accepted, not fixed): this dma_buf_get(fd) and
	 * the provider's own, independent dma_buf_get(fd) inside
	 * ib_umem_dmabuf_get() a few frames below are two unsynchronized
	 * resolutions of the same raw fd number, with no serialization
	 * between concurrent MR-registration ioctls on this ufile. See the
	 * "Known limitation" comment on UVERBS_HANDLER(UVERBS_METHOD_MR_
	 * EXPORT_DMABUF_FD) below for the full rationale.
	 */
	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	mr = pd->device->ops.reg_user_mr_dmabuf(pd, offset, length, iova, fd,
						access_flags, NULL,
						attrs);
	if (IS_ERR(mr)) {
		dma_buf_put(dmabuf);
		return PTR_ERR(mr);
	}

	mr->device = pd->device;
	mr->pd = pd;
	mr->type = IB_MR_TYPE_USER;
	mr->uobject = uobj;
	mr->access_flags = access_flags;
	/* DMABUF MRs don't carry a user VA -- mr->user_addr stays 0. */
	atomic_inc(&pd->usecnt);

	rdma_restrack_new(&mr->res, RDMA_RESTRACK_MR);
	rdma_restrack_set_name(&mr->res, NULL);
	rdma_restrack_add(&mr->res);
	uobj->object = mr;
	to_ib_umr_object(uobj)->dmabuf = dmabuf;

	uverbs_finalize_uobj_create(attrs, UVERBS_ATTR_REG_DMABUF_MR_HANDLE);

	ret = uverbs_copy_to(attrs, UVERBS_ATTR_REG_DMABUF_MR_RESP_LKEY,
			     &mr->lkey, sizeof(mr->lkey));
	if (ret)
		return ret;

	ret = uverbs_copy_to(attrs, UVERBS_ATTR_REG_DMABUF_MR_RESP_RKEY,
			     &mr->rkey, sizeof(mr->rkey));
	return ret;
}

static int UVERBS_HANDLER(UVERBS_METHOD_REG_MR)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_uobject *uobj =
		uverbs_attr_get_uobject(attrs, UVERBS_ATTR_REG_MR_HANDLE);
	struct ib_pd *pd =
		uverbs_attr_get_obj(attrs, UVERBS_ATTR_REG_MR_PD_HANDLE);
	u32 valid_access_flags = IB_ACCESS_SUPPORTED;
	u64 length, iova, fd_offset = 0, addr = 0;
	struct ib_device *ib_dev = pd->device;
	struct ib_dmah *dmah = NULL;
	struct dma_buf *dmabuf = NULL;
	bool has_fd_offset = false;
	bool has_addr = false;
	bool has_fd = false;
	u32 access_flags;
	struct ib_mr *mr;
	int fd;
	int ret;

	ret = uverbs_copy_from(&iova, attrs, UVERBS_ATTR_REG_MR_IOVA);
	if (ret)
		return ret;

	ret = uverbs_copy_from(&length, attrs, UVERBS_ATTR_REG_MR_LENGTH);
	if (ret)
		return ret;

	if (uverbs_attr_is_valid(attrs, UVERBS_ATTR_REG_MR_ADDR)) {
		ret = uverbs_copy_from(&addr, attrs,
				       UVERBS_ATTR_REG_MR_ADDR);
		if (ret)
			return ret;
		has_addr = true;
	}

	if (uverbs_attr_is_valid(attrs, UVERBS_ATTR_REG_MR_FD_OFFSET)) {
		ret = uverbs_copy_from(&fd_offset, attrs,
				       UVERBS_ATTR_REG_MR_FD_OFFSET);
		if (ret)
			return ret;
		has_fd_offset = true;
	}

	if (uverbs_attr_is_valid(attrs, UVERBS_ATTR_REG_MR_FD)) {
		ret = uverbs_get_raw_fd(&fd, attrs,
					UVERBS_ATTR_REG_MR_FD);
		if (ret)
			return ret;
		has_fd = true;
	}

	if (has_fd) {
		if (!ib_dev->ops.reg_user_mr_dmabuf)
			return -EOPNOTSUPP;

		/* FD requires offset and can't come with addr */
		if (!has_fd_offset || has_addr)
			return -EINVAL;

		if ((fd_offset & ~PAGE_MASK) != (iova & ~PAGE_MASK))
			return -EINVAL;

		valid_access_flags = IB_ACCESS_LOCAL_WRITE |
				     IB_ACCESS_REMOTE_READ |
				     IB_ACCESS_REMOTE_WRITE |
				     IB_ACCESS_REMOTE_ATOMIC |
				     IB_ACCESS_RELAXED_ORDERING;
	} else {
		if (!has_addr || has_fd_offset)
			return -EINVAL;

		if ((addr & ~PAGE_MASK) != (iova & ~PAGE_MASK))
			return -EINVAL;
	}

	if (uverbs_attr_is_valid(attrs, UVERBS_ATTR_REG_MR_DMA_HANDLE)) {
		dmah = uverbs_attr_get_obj(attrs,
					   UVERBS_ATTR_REG_MR_DMA_HANDLE);
		if (IS_ERR(dmah))
			return PTR_ERR(dmah);
	}

	ret = uverbs_get_flags32(&access_flags, attrs,
				 UVERBS_ATTR_REG_MR_ACCESS_FLAGS,
				 valid_access_flags);
	if (ret)
		return ret;

	ret = ib_check_mr_access(ib_dev, access_flags);
	if (ret)
		return ret;

	if (has_fd) {
		/*
		 * Retain our own reference to the dma_buf, independent of
		 * the one the provider takes internally via
		 * ib_umem_dmabuf_get(). This gives uverbs core a stable,
		 * provider-independent handle on the backing file for
		 * MR_EXPORT_DMABUF_FD, released in uverbs_free_mr().
		 *
		 * Known limitation (accepted, not fixed): see the "Known
		 * limitation" comment on
		 * UVERBS_HANDLER(UVERBS_METHOD_MR_EXPORT_DMABUF_FD) below --
		 * this dma_buf_get(fd) and the provider's own independent
		 * dma_buf_get(fd) inside ib_umem_dmabuf_get() are two
		 * unsynchronized resolutions of the same raw fd number.
		 */
		dmabuf = dma_buf_get(fd);
		if (IS_ERR(dmabuf))
			return PTR_ERR(dmabuf);

		mr = pd->device->ops.reg_user_mr_dmabuf(pd, fd_offset, length,
							iova, fd, access_flags,
							dmah, attrs);
		if (IS_ERR(mr)) {
			dma_buf_put(dmabuf);
			return PTR_ERR(mr);
		}
	} else {
		mr = pd->device->ops.reg_user_mr(pd, addr, length, iova,
						 access_flags, dmah,
						 &attrs->driver_udata);
		if (IS_ERR(mr))
			return PTR_ERR(mr);
	}

	mr->device = pd->device;
	mr->pd = pd;
	mr->type = IB_MR_TYPE_USER;
	mr->uobject = uobj;
	mr->access_flags = access_flags;
	/*
	 * user_addr is the user VA the caller registered against
	 * (REG_MR_ADDR). DMABUF MRs don't have one -- the fd-pinned
	 * pages live in their own VA space -- so we leave user_addr
	 * at 0 in that lane. QUERY_MR's RESP_USER_ADDR == 0 must be
	 * read by userspace as "MR has no user VA," not "MR registered
	 * at NULL."
	 */
	if (!has_fd)
		mr->user_addr = addr;
	atomic_inc(&pd->usecnt);
	if (dmah) {
		mr->dmah = dmah;
		atomic_inc(&dmah->usecnt);
	}
	rdma_restrack_new(&mr->res, RDMA_RESTRACK_MR);
	rdma_restrack_set_name(&mr->res, NULL);
	rdma_restrack_add(&mr->res);
	uobj->object = mr;
	if (has_fd)
		to_ib_umr_object(uobj)->dmabuf = dmabuf;

	uverbs_finalize_uobj_create(attrs, UVERBS_ATTR_REG_MR_HANDLE);

	ret = uverbs_copy_to(attrs, UVERBS_ATTR_REG_MR_RESP_LKEY,
			     &mr->lkey, sizeof(mr->lkey));
	if (ret)
		return ret;

	ret = uverbs_copy_to(attrs, UVERBS_ATTR_REG_MR_RESP_RKEY,
			     &mr->rkey, sizeof(mr->rkey));
	return ret;
}

/*
 * Security policy: none beyond the READ lookup of the MR handle, which the
 * IDR attribute below already enforces. That lookup resolves against the
 * caller's own ufile, so this structurally cannot return an fd for an MR
 * belonging to a different uverbs file -- the same property that makes
 * MR_DESTROY safe without a capability check.
 *
 * The dma_buf handed back is one the MR's registrar necessarily already
 * held an fd for (that is the only way ib_umem_dmabuf_get() can have been
 * reached), and anything holding this ufile can already read and write that
 * memory by posting work requests against the MR. So this is a more
 * convenient handle on memory already reachable, not new authority. The
 * boundary that matters is possession of the uverbs fd itself, which is
 * guarded where it is acquired -- ptrace-level checks on pidfd_getfd().
 *
 * Known limitation (accepted, not fixed): the dma_buf returned here is
 * whatever uverbs core resolved via its own dma_buf_get(fd) at MR
 * registration time (see UVERBS_METHOD_REG_DMABUF_MR / UVERBS_METHOD_
 * REG_MR above). The DMA-BUF-capable provider (e.g. mlx5) performs an
 * independent, unsynchronized dma_buf_get(fd) resolution of the same raw
 * fd number inside ib_umem_dmabuf_get(), separated from core's
 * resolution by real intervening work (provider UMR/QP setup,
 * allocations), with no serialization between concurrent MR-registration
 * ioctls on the same ufile. A second thread of the *same* process racing
 * close(fd) + open()/dup2() onto that fd number in that window could
 * make core's retained reference and the provider's actual DMA-mapped
 * buffer diverge, so this call would then return an fd identifying a
 * dma_buf other than the one the MR's pages actually came from. This is
 * self-inflicted only -- it requires the registering process to race its
 * own fd table, not a cross-process or privilege-boundary attack -- and
 * is accepted as a known limitation rather than fixed, since a complete
 * fix requires every DMA-BUF-capable provider's reg_user_mr_dmabuf
 * implementation to accept an already-resolved dma_buf/file instead of
 * re-resolving the fd, which conflicts with this feature's "no
 * provider-specific changes" design goal.
 */
static int UVERBS_HANDLER(UVERBS_METHOD_MR_EXPORT_DMABUF_FD)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_uobject *uobj;
	struct dma_buf *dmabuf;
	int fd;
	int ret;

	uobj = uverbs_attr_get_uobject(attrs,
				       UVERBS_ATTR_MR_EXPORT_DMABUF_FD_HANDLE);
	dmabuf = to_ib_umr_object(uobj)->dmabuf;

	if (!dmabuf)
		return -EOPNOTSUPP;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0)
		return fd;

	get_dma_buf(dmabuf);

	ret = uverbs_copy_to(attrs, UVERBS_ATTR_MR_EXPORT_DMABUF_FD_RESP_FD,
			     &fd, sizeof(fd));
	if (ret) {
		fput(dmabuf->file);
		put_unused_fd(fd);
		return ret;
	}

	fd_install(fd, dmabuf->file);
	return 0;
}

DECLARE_UVERBS_NAMED_METHOD(
	UVERBS_METHOD_ADVISE_MR,
	UVERBS_ATTR_IDR(UVERBS_ATTR_ADVISE_MR_PD_HANDLE,
			UVERBS_OBJECT_PD,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_CONST_IN(UVERBS_ATTR_ADVISE_MR_ADVICE,
			     enum ib_uverbs_advise_mr_advice,
			     UA_MANDATORY),
	UVERBS_ATTR_FLAGS_IN(UVERBS_ATTR_ADVISE_MR_FLAGS,
			     enum ib_uverbs_advise_mr_flag,
			     UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_ADVISE_MR_SGE_LIST,
			   UVERBS_ATTR_MIN_SIZE(sizeof(struct ib_uverbs_sge)),
			   UA_MANDATORY,
			   UA_ALLOC_AND_COPY));

DECLARE_UVERBS_NAMED_METHOD(
	UVERBS_METHOD_QUERY_MR,
	UVERBS_ATTR_IDR(UVERBS_ATTR_QUERY_MR_HANDLE,
			UVERBS_OBJECT_MR,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_QUERY_MR_RESP_RKEY,
			    UVERBS_ATTR_TYPE(u32),
			    UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_QUERY_MR_RESP_LKEY,
			    UVERBS_ATTR_TYPE(u32),
			    UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_QUERY_MR_RESP_LENGTH,
			    UVERBS_ATTR_TYPE(u64),
			    UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_QUERY_MR_RESP_IOVA,
			    UVERBS_ATTR_TYPE(u64),
			    UA_OPTIONAL),
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_QUERY_MR_RESP_USER_ADDR,
			    UVERBS_ATTR_TYPE(u64),
			    UA_OPTIONAL),
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_QUERY_MR_RESP_ACCESS_FLAGS,
			    UVERBS_ATTR_TYPE(u32),
			    UA_OPTIONAL));

DECLARE_UVERBS_NAMED_METHOD(
	UVERBS_METHOD_DM_MR_REG,
	UVERBS_ATTR_IDR(UVERBS_ATTR_REG_DM_MR_HANDLE,
			UVERBS_OBJECT_MR,
			UVERBS_ACCESS_NEW,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_REG_DM_MR_OFFSET,
			   UVERBS_ATTR_TYPE(u64),
			   UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_REG_DM_MR_LENGTH,
			   UVERBS_ATTR_TYPE(u64),
			   UA_MANDATORY),
	UVERBS_ATTR_IDR(UVERBS_ATTR_REG_DM_MR_PD_HANDLE,
			UVERBS_OBJECT_PD,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_FLAGS_IN(UVERBS_ATTR_REG_DM_MR_ACCESS_FLAGS,
			     enum ib_access_flags),
	UVERBS_ATTR_IDR(UVERBS_ATTR_REG_DM_MR_DM_HANDLE,
			UVERBS_OBJECT_DM,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_REG_DM_MR_RESP_LKEY,
			    UVERBS_ATTR_TYPE(u32),
			    UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_REG_DM_MR_RESP_RKEY,
			    UVERBS_ATTR_TYPE(u32),
			    UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD(
	UVERBS_METHOD_REG_DMABUF_MR,
	UVERBS_ATTR_IDR(UVERBS_ATTR_REG_DMABUF_MR_HANDLE,
			UVERBS_OBJECT_MR,
			UVERBS_ACCESS_NEW,
			UA_MANDATORY),
	UVERBS_ATTR_IDR(UVERBS_ATTR_REG_DMABUF_MR_PD_HANDLE,
			UVERBS_OBJECT_PD,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_REG_DMABUF_MR_OFFSET,
			   UVERBS_ATTR_TYPE(u64),
			   UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_REG_DMABUF_MR_LENGTH,
			   UVERBS_ATTR_TYPE(u64),
			   UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_REG_DMABUF_MR_IOVA,
			   UVERBS_ATTR_TYPE(u64),
			   UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_REG_DMABUF_MR_FD,
			   UVERBS_ATTR_TYPE(u32),
			   UA_MANDATORY),
	UVERBS_ATTR_FLAGS_IN(UVERBS_ATTR_REG_DMABUF_MR_ACCESS_FLAGS,
			     enum ib_access_flags),
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_REG_DMABUF_MR_RESP_LKEY,
			    UVERBS_ATTR_TYPE(u32),
			    UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_REG_DMABUF_MR_RESP_RKEY,
			    UVERBS_ATTR_TYPE(u32),
			    UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD(
	UVERBS_METHOD_REG_MR,
	UVERBS_ATTR_IDR(UVERBS_ATTR_REG_MR_HANDLE,
			UVERBS_OBJECT_MR,
			UVERBS_ACCESS_NEW,
			UA_MANDATORY),
	UVERBS_ATTR_IDR(UVERBS_ATTR_REG_MR_PD_HANDLE,
			UVERBS_OBJECT_PD,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_IDR(UVERBS_ATTR_REG_MR_DMA_HANDLE,
			UVERBS_OBJECT_DMAH,
			UVERBS_ACCESS_READ,
			UA_OPTIONAL),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_REG_MR_IOVA,
			   UVERBS_ATTR_TYPE(u64),
			   UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_REG_MR_LENGTH,
			   UVERBS_ATTR_TYPE(u64),
			   UA_MANDATORY),
	UVERBS_ATTR_FLAGS_IN(UVERBS_ATTR_REG_MR_ACCESS_FLAGS,
			     enum ib_access_flags,
			     UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_REG_MR_ADDR,
			   UVERBS_ATTR_TYPE(u64),
			   UA_OPTIONAL),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_REG_MR_FD_OFFSET,
			   UVERBS_ATTR_TYPE(u64),
			   UA_OPTIONAL),
	UVERBS_ATTR_RAW_FD(UVERBS_ATTR_REG_MR_FD,
			   UA_OPTIONAL),
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_REG_MR_RESP_LKEY,
			    UVERBS_ATTR_TYPE(u32),
			    UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_REG_MR_RESP_RKEY,
			    UVERBS_ATTR_TYPE(u32),
			    UA_MANDATORY),
	UVERBS_ATTR_UHW());

DECLARE_UVERBS_NAMED_METHOD(
	UVERBS_METHOD_MR_EXPORT_DMABUF_FD,
	UVERBS_ATTR_IDR(UVERBS_ATTR_MR_EXPORT_DMABUF_FD_HANDLE,
			UVERBS_OBJECT_MR,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_MR_EXPORT_DMABUF_FD_RESP_FD,
			    UVERBS_ATTR_TYPE(s32),
			    UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD_DESTROY(
	UVERBS_METHOD_MR_DESTROY,
	UVERBS_ATTR_IDR(UVERBS_ATTR_DESTROY_MR_HANDLE,
			UVERBS_OBJECT_MR,
			UVERBS_ACCESS_DESTROY,
			UA_MANDATORY));

DECLARE_UVERBS_NAMED_OBJECT(
	UVERBS_OBJECT_MR,
	UVERBS_TYPE_ALLOC_IDR_SZ(sizeof(struct ib_umr_object), uverbs_free_mr),
	&UVERBS_METHOD(UVERBS_METHOD_ADVISE_MR),
	&UVERBS_METHOD(UVERBS_METHOD_DM_MR_REG),
	&UVERBS_METHOD(UVERBS_METHOD_MR_DESTROY),
	&UVERBS_METHOD(UVERBS_METHOD_QUERY_MR),
	&UVERBS_METHOD(UVERBS_METHOD_REG_DMABUF_MR),
	&UVERBS_METHOD(UVERBS_METHOD_REG_MR),
	&UVERBS_METHOD(UVERBS_METHOD_MR_EXPORT_DMABUF_FD));

const struct uapi_definition uverbs_def_obj_mr[] = {
	UAPI_DEF_CHAIN_OBJ_TREE_NAMED(UVERBS_OBJECT_MR,
				      UAPI_DEF_OBJ_NEEDS_FN(dereg_mr)),
	{}
};

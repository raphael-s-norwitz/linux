// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * UVERBS_OBJECT_RESTORE namespace -- CRIU-restore-specific uverbs
 * methods. Each method installs a new uobject of a concrete class
 * (PD / CQ / QP / MR / SRQ / AH / COMP_CHANNEL / ASYNC_EVENT) at a
 * caller-chosen ufile handle (atomic xa_insert via the core helper
 * rdma_alloc_begin_uobject_at_handle()), then dispatches through a
 * driver-specific ib_device_ops.restore_<type> callback that makes
 * the hw side usable. The caller is responsible for having opened
 * the parent ucontext in CRIU-restore mode; the dispatcher rejects
 * any other ucontext with -EPERM via the per-driver
 * ib_device_ops.ucontext_is_restore_mode predicate.
 *
 * See tools/testing/criu_rdma/design/uobject_restore.md.
 */

#include <rdma/uverbs_std_types.h>
#include <rdma/uverbs_ioctl.h>
#include <rdma/uverbs_types.h>
#include "rdma_core.h"
#include "uverbs.h"
#include "restrack.h"

/*
 * Per-method (ucontext) gate. Returns 0 on success, -errno otherwise.
 * Splits out the two checks the dispatchers all share so the
 * per-class handlers stay tight.
 */
static int restore_check_ucontext(struct uverbs_attr_bundle *attrs,
				  struct ib_ucontext **out_ctx)
{
	struct ib_ucontext *ctx;

	ctx = ib_uverbs_get_ucontext(attrs);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	/*
	 * Opt-in default: missing callback => no ucontext on this
	 * device may restore. See ib_device_ops.ucontext_is_restore_mode
	 * in include/rdma/ib_verbs.h for the contract.
	 */
	if (!ctx->device->ops.ucontext_is_restore_mode ||
	    !ctx->device->ops.ucontext_is_restore_mode(ctx))
		return -EPERM;

	*out_ctx = ctx;
	return 0;
}

static int UVERBS_HANDLER(UVERBS_METHOD_RESTORE_PD)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_ucontext *ctx;
	struct ib_device *ib_dev;
	struct ib_uobject *uobj;
	struct ib_pd *pd;
	u32 target_handle;
	int ret;

	ret = restore_check_ucontext(attrs, &ctx);
	if (ret)
		return ret;
	ib_dev = ctx->device;
	if (!ib_dev->ops.restore_pd)
		return -EOPNOTSUPP;

	ret = uverbs_copy_from(&target_handle, attrs,
			       UVERBS_ATTR_RESTORE_PD_HANDLE);
	if (ret)
		return ret;

	/*
	 * Reserve the target ufile handle atomically. Returns -EBUSY if
	 * the handle is already taken (concurrent ALLOC_PD, prior
	 * RESTORE_* with the same target, etc.). The returned uobj is
	 * pre-locked with usecnt = -1, mirroring uobj_alloc().
	 */
	uobj = rdma_alloc_begin_uobject_at_handle(attrs, UVERBS_OBJECT_PD,
						  target_handle);
	if (IS_ERR(uobj))
		return PTR_ERR(uobj);

	pd = rdma_zalloc_drv_obj(ib_dev, ib_pd);
	if (!pd) {
		ret = -ENOMEM;
		goto err_uobj;
	}

	pd->device = ib_dev;
	pd->uobject = uobj;
	atomic_set(&pd->usecnt, 0);

	rdma_restrack_new(&pd->res, RDMA_RESTRACK_PD);
	rdma_restrack_set_name(&pd->res, NULL);

	ret = ib_dev->ops.restore_pd(pd, target_handle, &attrs->driver_udata);
	if (ret)
		goto err_restrack;
	rdma_restrack_add(&pd->res);

	uobj->object = pd;
	rdma_alloc_commit_uobject(uobj, attrs);
	return 0;

err_restrack:
	rdma_restrack_put(&pd->res);
	kfree(pd);
err_uobj:
	rdma_alloc_abort_uobject(uobj, attrs, false);
	return ret;
}

DECLARE_UVERBS_NAMED_METHOD(
	UVERBS_METHOD_RESTORE_PD,
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_RESTORE_PD_HANDLE,
			   UVERBS_ATTR_TYPE(__u32), UA_MANDATORY),
	UVERBS_ATTR_UHW());

static int UVERBS_HANDLER(UVERBS_METHOD_RESTORE_MR)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_ucontext *ctx;
	struct ib_device *ib_dev;
	struct ib_uobject *uobj;
	struct ib_pd *pd;
	struct ib_mr *mr;
	u32 target_handle, lkey_hint, rkey_hint, access_flags;
	u64 addr, length, iova;
	int ret;

	ret = restore_check_ucontext(attrs, &ctx);
	if (ret)
		return ret;
	ib_dev = ctx->device;
	if (!ib_dev->ops.restore_mr)
		return -EOPNOTSUPP;

	/*
	 * Parent PD: resolved by the IDR attr machinery using the
	 * caller-supplied ufile handle; the read-side refcount is
	 * auto-managed across the call.
	 */
	pd = uverbs_attr_get_obj(attrs, UVERBS_ATTR_RESTORE_MR_PD_HANDLE);
	if (IS_ERR(pd))
		return PTR_ERR(pd);
	if (pd->device != ib_dev)
		return -EINVAL;

	ret = uverbs_copy_from(&target_handle, attrs,
			       UVERBS_ATTR_RESTORE_MR_HANDLE);
	if (ret)
		return ret;

	ret = uverbs_copy_from(&addr, attrs, UVERBS_ATTR_RESTORE_MR_ADDR);
	if (ret)
		return ret;
	ret = uverbs_copy_from(&length, attrs,
			       UVERBS_ATTR_RESTORE_MR_LENGTH);
	if (ret)
		return ret;
	ret = uverbs_copy_from(&iova, attrs, UVERBS_ATTR_RESTORE_MR_IOVA);
	if (ret)
		return ret;
	ret = uverbs_get_flags32(&access_flags, attrs,
				 UVERBS_ATTR_RESTORE_MR_ACCESS_FLAGS,
				 IB_ACCESS_SUPPORTED);
	if (ret)
		return ret;
	ret = ib_check_mr_access(ib_dev, access_flags);
	if (ret)
		return ret;
	ret = uverbs_copy_from(&lkey_hint, attrs,
			       UVERBS_ATTR_RESTORE_MR_LKEY_HINT);
	if (ret)
		return ret;
	ret = uverbs_copy_from(&rkey_hint, attrs,
			       UVERBS_ATTR_RESTORE_MR_RKEY_HINT);
	if (ret)
		return ret;

	/*
	 * Reserve the target ufile handle for the new MR uobject. Same
	 * atomic xa_insert pattern as RESTORE_PD; -EBUSY on collision.
	 */
	uobj = rdma_alloc_begin_uobject_at_handle(attrs, UVERBS_OBJECT_MR,
						  target_handle);
	if (IS_ERR(uobj))
		return PTR_ERR(uobj);

	mr = ib_dev->ops.restore_mr(pd, target_handle, addr, length, iova,
				    access_flags, lkey_hint, rkey_hint,
				    &attrs->driver_udata);
	if (IS_ERR(mr)) {
		ret = PTR_ERR(mr);
		goto err_uobj;
	}

	mr->device = ib_dev;
	mr->pd = pd;
	mr->type = IB_MR_TYPE_USER;
	mr->uobject = uobj;
	mr->iova = iova;
	mr->length = length;
	mr->user_addr = addr;
	mr->access_flags = access_flags;
	atomic_inc(&pd->usecnt);

	rdma_restrack_new(&mr->res, RDMA_RESTRACK_MR);
	rdma_restrack_set_name(&mr->res, NULL);
	rdma_restrack_add(&mr->res);

	uobj->object = mr;
	rdma_alloc_commit_uobject(uobj, attrs);

	/*
	 * Return the actual installed lkey/rkey. Equals the caller's
	 * hint when the driver honoured it (mlx5 v0); differs when
	 * the driver assigned its own (rxe). Userspace compares to
	 * detect the latter case.
	 */
	ret = uverbs_copy_to(attrs, UVERBS_ATTR_RESTORE_MR_RESP_LKEY,
			     &mr->lkey, sizeof(mr->lkey));
	if (ret)
		return ret;
	ret = uverbs_copy_to(attrs, UVERBS_ATTR_RESTORE_MR_RESP_RKEY,
			     &mr->rkey, sizeof(mr->rkey));
	return ret;

err_uobj:
	rdma_alloc_abort_uobject(uobj, attrs, false);
	return ret;
}

DECLARE_UVERBS_NAMED_METHOD(
	UVERBS_METHOD_RESTORE_MR,
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_RESTORE_MR_HANDLE,
			   UVERBS_ATTR_TYPE(__u32), UA_MANDATORY),
	UVERBS_ATTR_IDR(UVERBS_ATTR_RESTORE_MR_PD_HANDLE,
			UVERBS_OBJECT_PD,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_RESTORE_MR_ADDR,
			   UVERBS_ATTR_TYPE(__u64), UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_RESTORE_MR_LENGTH,
			   UVERBS_ATTR_TYPE(__u64), UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_RESTORE_MR_IOVA,
			   UVERBS_ATTR_TYPE(__u64), UA_MANDATORY),
	UVERBS_ATTR_FLAGS_IN(UVERBS_ATTR_RESTORE_MR_ACCESS_FLAGS,
			     enum ib_access_flags,
			     UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_RESTORE_MR_LKEY_HINT,
			   UVERBS_ATTR_TYPE(__u32), UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_RESTORE_MR_RKEY_HINT,
			   UVERBS_ATTR_TYPE(__u32), UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_RESTORE_MR_RESP_LKEY,
			    UVERBS_ATTR_TYPE(__u32), UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_RESTORE_MR_RESP_RKEY,
			    UVERBS_ATTR_TYPE(__u32), UA_MANDATORY),
	UVERBS_ATTR_UHW());

static int UVERBS_HANDLER(UVERBS_METHOD_RESTORE_CQ)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_ucontext *ctx;
	struct ib_device *ib_dev;
	struct ib_uobject *uobj;
	struct ib_ucq_object *obj;
	struct ib_cq_init_attr attr = {};
	struct ib_cq *cq;
	u32 target_handle;
	u64 user_handle;
	int ret;

	ret = restore_check_ucontext(attrs, &ctx);
	if (ret)
		return ret;
	ib_dev = ctx->device;
	/*
	 * destroy_cq is checked alongside restore_cq because the cleanup
	 * path (uverbs_free_cq -> ib_destroy_cq_user) calls it
	 * unconditionally on any CQ uobject -- including ones we just
	 * created here via restore. A driver that lacks destroy_cq would
	 * leak the kernel object on uobject teardown; reject up front.
	 */
	if (!ib_dev->ops.restore_cq || !ib_dev->ops.destroy_cq)
		return -EOPNOTSUPP;

	/*
	 * v0 limitation: a CQ that was bound to a comp_channel on the
	 * source cannot be re-bound here -- the comp_channel uobject
	 * itself is not yet restorable (UVERBS_METHOD_RESTORE_COMP_CHANNEL
	 * is not implemented). UVERBS_ATTR_RESTORE_CQ_COMP_CHANNEL is
	 * declared UA_OPTIONAL for forward compat with that future work;
	 * if any v0 caller actually passes one, hard-fail before we
	 * touch hw state. CRIU plugin policy at v0: only restore CQs
	 * created without a comp_channel. (EVENT_FD does not need this
	 * gate -- ib_uverbs_get_async_event() falls back to the ufile
	 * default when absent, which is what v0 callers want.)
	 */
	if (uverbs_attr_is_valid(attrs, UVERBS_ATTR_RESTORE_CQ_COMP_CHANNEL))
		return -EOPNOTSUPP;

	ret = uverbs_copy_from(&target_handle, attrs,
			       UVERBS_ATTR_RESTORE_CQ_HANDLE);
	if (ret)
		return ret;
	ret = uverbs_copy_from(&attr.cqe, attrs,
			       UVERBS_ATTR_RESTORE_CQ_CQE);
	if (ret || !attr.cqe)
		return ret ? : -EINVAL;
	ret = uverbs_copy_from(&user_handle, attrs,
			       UVERBS_ATTR_RESTORE_CQ_USER_HANDLE);
	if (ret)
		return ret;
	ret = uverbs_copy_from(&attr.comp_vector, attrs,
			       UVERBS_ATTR_RESTORE_CQ_COMP_VECTOR);
	if (ret)
		return ret;
	ret = uverbs_get_flags32(&attr.flags, attrs,
				 UVERBS_ATTR_RESTORE_CQ_FLAGS,
				 IB_UVERBS_CQ_FLAGS_TIMESTAMP_COMPLETION |
					 IB_UVERBS_CQ_FLAGS_IGNORE_OVERRUN);
	if (ret)
		return ret;
	if (attr.comp_vector >= attrs->ufile->device->num_comp_vectors)
		return -EINVAL;

	/*
	 * Reserve the target ufile handle for the new CQ uobject. The
	 * idr-class allocator returns the embedded ib_uobject inside a
	 * heap-allocated struct ib_ucq_object (size set by the
	 * UVERBS_TYPE_ALLOC_IDR_SZ in DECLARE_UVERBS_NAMED_OBJECT for
	 * UVERBS_OBJECT_CQ). container_of() reaches the wider obj.
	 */
	uobj = rdma_alloc_begin_uobject_at_handle(attrs, UVERBS_OBJECT_CQ,
						  target_handle);
	if (IS_ERR(uobj))
		return PTR_ERR(uobj);
	obj = container_of(uobj, struct ib_ucq_object, uevent.uobject);

	INIT_LIST_HEAD(&obj->comp_list);
	INIT_LIST_HEAD(&obj->uevent.event_list);
	obj->uevent.event_file =
		ib_uverbs_get_async_event(attrs,
					  UVERBS_ATTR_RESTORE_CQ_EVENT_FD);
	uobj->user_handle = user_handle;

	cq = rdma_zalloc_drv_obj(ib_dev, ib_cq);
	if (!cq) {
		ret = -ENOMEM;
		goto err_event_file;
	}

	cq->device = ib_dev;
	cq->uobject = obj;
	cq->comp_handler = ib_uverbs_comp_handler;
	cq->event_handler = ib_uverbs_cq_event_handler;
	cq->cq_context = NULL; /* v0: no comp_channel */
	atomic_set(&cq->usecnt, 0);

	rdma_restrack_new(&cq->res, RDMA_RESTRACK_CQ);
	rdma_restrack_set_name(&cq->res, NULL);

	ret = ib_dev->ops.restore_cq(cq, target_handle, &attr,
				     &attrs->driver_udata);
	if (ret)
		goto err_restrack;
	rdma_restrack_add(&cq->res);

	uobj->object = cq;
	rdma_alloc_commit_uobject(uobj, attrs);

	return uverbs_copy_to(attrs, UVERBS_ATTR_RESTORE_CQ_RESP_CQE,
			      &cq->cqe, sizeof(cq->cqe));

err_restrack:
	rdma_restrack_put(&cq->res);
	kfree(cq);
err_event_file:
	if (obj->uevent.event_file)
		uverbs_uobject_put(&obj->uevent.event_file->uobj);
	rdma_alloc_abort_uobject(uobj, attrs, false);
	return ret;
}

DECLARE_UVERBS_NAMED_METHOD(
	UVERBS_METHOD_RESTORE_CQ,
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_RESTORE_CQ_HANDLE,
			   UVERBS_ATTR_TYPE(__u32), UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_RESTORE_CQ_CQE,
			   UVERBS_ATTR_TYPE(__u32), UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_RESTORE_CQ_USER_HANDLE,
			   UVERBS_ATTR_TYPE(__u64), UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_RESTORE_CQ_COMP_VECTOR,
			   UVERBS_ATTR_TYPE(__u32), UA_MANDATORY),
	UVERBS_ATTR_FLAGS_IN(UVERBS_ATTR_RESTORE_CQ_FLAGS,
			     enum ib_uverbs_ex_create_cq_flags),
	UVERBS_ATTR_FD(UVERBS_ATTR_RESTORE_CQ_COMP_CHANNEL,
		       UVERBS_OBJECT_COMP_CHANNEL,
		       UVERBS_ACCESS_READ,
		       UA_OPTIONAL),
	UVERBS_ATTR_FD(UVERBS_ATTR_RESTORE_CQ_EVENT_FD,
		       UVERBS_OBJECT_ASYNC_EVENT,
		       UVERBS_ACCESS_READ,
		       UA_OPTIONAL),
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_RESTORE_CQ_RESP_CQE,
			    UVERBS_ATTR_TYPE(__u32), UA_MANDATORY),
	UVERBS_ATTR_UHW());

DECLARE_UVERBS_GLOBAL_METHODS(UVERBS_OBJECT_RESTORE,
			      &UVERBS_METHOD(UVERBS_METHOD_RESTORE_PD),
			      &UVERBS_METHOD(UVERBS_METHOD_RESTORE_MR),
			      &UVERBS_METHOD(UVERBS_METHOD_RESTORE_CQ));

const struct uapi_definition uverbs_def_obj_restore[] = {
	UAPI_DEF_CHAIN_OBJ_TREE_NAMED(UVERBS_OBJECT_RESTORE),
	{},
};

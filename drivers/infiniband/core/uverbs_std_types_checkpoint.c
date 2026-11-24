// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * The CHECKPOINT_* methods of UVERBS_OBJECT_RESTORE: read-only queries a
 * checkpointer runs on a live context to capture state the create verbs
 * set up and no other query reports. They sit beside the RESTORE methods
 * so that every out-of-tree method ID is in one object.
 */

#include <rdma/uverbs_std_types.h>
#include <rdma/uverbs_ioctl.h>
#include "rdma_core.h"
#include "uverbs.h"

/* Counter and number of events queued on @ev_queue from @obj_list. */
static void read_events(struct ib_uverbs_event_queue *ev_queue, u32 *counter,
			struct list_head *obj_list, __u32 *reported,
			__u32 *queued)
{
	spin_lock_irq(&ev_queue->lock);
	*reported = *counter;
	*queued = list_count_nodes(obj_list);
	spin_unlock_irq(&ev_queue->lock);
}

static void read_uevent(struct ib_uevent_object *uevent,
			struct ib_uverbs_uobj_events *resp)
{
	struct ib_uverbs_async_event_file *async_file = uevent->event_file;

	resp->user_handle = uevent->uobject.user_handle;
	if (!async_file)
		return;
	resp->async_file_id = async_file->ev_queue.id;
	read_events(&async_file->ev_queue, &uevent->events_reported,
		    &uevent->event_list, &resp->async_events_reported,
		    &resp->async_events_queued);
}

static int UVERBS_HANDLER(UVERBS_METHOD_CHECKPOINT_QUERY_EVENTS)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_uverbs_uobj_events resp = {};
	struct ib_uobject *cq_uobj, *qp_uobj;

	cq_uobj = uverbs_attr_get_uobject(
		attrs, UVERBS_ATTR_CHECKPOINT_QUERY_EVENTS_CQ_HANDLE);
	qp_uobj = uverbs_attr_get_uobject(
		attrs, UVERBS_ATTR_CHECKPOINT_QUERY_EVENTS_QP_HANDLE);
	if (IS_ERR(cq_uobj) == IS_ERR(qp_uobj))
		return -EINVAL;

	if (!IS_ERR(cq_uobj)) {
		struct ib_ucq_object *ucq = container_of(
			cq_uobj, struct ib_ucq_object, uevent.uobject);
		struct ib_cq *cq = cq_uobj->object;
		struct ib_uverbs_event_queue *channel = cq->cq_context;

		read_uevent(&ucq->uevent, &resp);
		if (channel) {
			resp.comp_channel_id = channel->id;
			read_events(channel, &ucq->comp_events_reported,
				    &ucq->comp_list, &resp.comp_events_reported,
				    &resp.comp_events_queued);
		}
	} else {
		struct ib_uqp_object *uqp = container_of(
			qp_uobj, struct ib_uqp_object, uevent.uobject);

		read_uevent(&uqp->uevent, &resp);
	}

	return uverbs_copy_to_struct_or_zero(
		attrs, UVERBS_ATTR_CHECKPOINT_QUERY_EVENTS_RESP, &resp,
		sizeof(resp));
}

DECLARE_UVERBS_NAMED_METHOD(
	UVERBS_METHOD_CHECKPOINT_QUERY_EVENTS,
	UVERBS_ATTR_IDR(UVERBS_ATTR_CHECKPOINT_QUERY_EVENTS_CQ_HANDLE,
			UVERBS_OBJECT_CQ,
			UVERBS_ACCESS_READ,
			UA_OPTIONAL),
	UVERBS_ATTR_IDR(UVERBS_ATTR_CHECKPOINT_QUERY_EVENTS_QP_HANDLE,
			UVERBS_OBJECT_QP,
			UVERBS_ACCESS_READ,
			UA_OPTIONAL),
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_CHECKPOINT_QUERY_EVENTS_RESP,
			    UVERBS_ATTR_STRUCT(struct ib_uverbs_uobj_events,
					       comp_events_queued),
			    UA_MANDATORY));

static int UVERBS_HANDLER(UVERBS_METHOD_CHECKPOINT_QUERY_DEFAULT_ASYNC)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_uverbs_async_event_file *async_file;
	struct ib_ucontext *ucontext;
	u32 id = 0;

	ucontext = ib_uverbs_get_ucontext(attrs);
	if (IS_ERR(ucontext))
		return PTR_ERR(ucontext);

	async_file = READ_ONCE(attrs->ufile->default_async_file);
	if (async_file)
		id = async_file->ev_queue.id;

	return uverbs_copy_to(
		attrs, UVERBS_ATTR_CHECKPOINT_QUERY_DEFAULT_ASYNC_ID,
		&id, sizeof(id));
}

DECLARE_UVERBS_NAMED_METHOD(
	UVERBS_METHOD_CHECKPOINT_QUERY_DEFAULT_ASYNC,
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_CHECKPOINT_QUERY_DEFAULT_ASYNC_ID,
			    UVERBS_ATTR_TYPE(u32), UA_MANDATORY));

DECLARE_UVERBS_GLOBAL_METHODS(
	UVERBS_OBJECT_RESTORE,
	&UVERBS_METHOD(UVERBS_METHOD_CHECKPOINT_QUERY_EVENTS),
	&UVERBS_METHOD(UVERBS_METHOD_CHECKPOINT_QUERY_DEFAULT_ASYNC));

const struct uapi_definition uverbs_def_obj_checkpoint[] = {
	UAPI_DEF_CHAIN_OBJ_TREE_NAMED(UVERBS_OBJECT_RESTORE),
	{},
};

/*
 * Copyright (c) 2018, Mellanox Technologies inc.  All rights reserved.
 * Copyright (c) 2020, Intel Corporation. All rights reserved.
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

#ifndef IB_USER_IOCTL_CMDS_H
#define IB_USER_IOCTL_CMDS_H

#define UVERBS_ID_NS_MASK 0xF000
#define UVERBS_ID_NS_SHIFT 12

enum uverbs_default_objects {
	UVERBS_OBJECT_DEVICE, /* No instances of DEVICE are allowed */
	UVERBS_OBJECT_PD,
	UVERBS_OBJECT_COMP_CHANNEL,
	UVERBS_OBJECT_CQ,
	UVERBS_OBJECT_QP,
	UVERBS_OBJECT_SRQ,
	UVERBS_OBJECT_AH,
	UVERBS_OBJECT_MR,
	UVERBS_OBJECT_MW,
	UVERBS_OBJECT_FLOW,
	UVERBS_OBJECT_XRCD,
	UVERBS_OBJECT_RWQ_IND_TBL,
	UVERBS_OBJECT_WQ,
	UVERBS_OBJECT_FLOW_ACTION,
	UVERBS_OBJECT_DM,
	UVERBS_OBJECT_COUNTERS,
	UVERBS_OBJECT_ASYNC_EVENT,
	UVERBS_OBJECT_DMAH,
	UVERBS_OBJECT_DMABUF,
	UVERBS_OBJECT_COMP_CNTR,
	/*
	 * Pseudo-object that aggregates the per-resource
	 * UVERBS_METHOD_RESTORE_<TYPE> methods used by CRIU restore. No
	 * instances of OBJECT_RESTORE are allowed; like
	 * UVERBS_OBJECT_DEVICE it exists only as a method-namespace
	 * holder. The methods themselves install new uobjects of the
	 * appropriate concrete type (PD / CQ / QP / ...) at
	 * caller-chosen ufile handles. Gated per-driver via
	 * ib_device_ops.ucontext_is_restore_mode.
	 */
	UVERBS_OBJECT_RESTORE,
};

enum {
	UVERBS_ID_DRIVER_NS = 1UL << UVERBS_ID_NS_SHIFT,
	UVERBS_ATTR_UHW_IN = UVERBS_ID_DRIVER_NS,
	UVERBS_ATTR_UHW_OUT,
	UVERBS_ID_DRIVER_NS_WITH_UHW,
};

enum uverbs_methods_device {
	UVERBS_METHOD_INVOKE_WRITE,
	UVERBS_METHOD_INFO_HANDLES,
	UVERBS_METHOD_QUERY_PORT,
	UVERBS_METHOD_GET_CONTEXT,
	UVERBS_METHOD_QUERY_CONTEXT,
	UVERBS_METHOD_QUERY_GID_TABLE,
	UVERBS_METHOD_QUERY_GID_ENTRY,
	UVERBS_METHOD_QUERY_PORT_SPEED,
	UVERBS_METHOD_QUERY_COMP_CNTR_CAPS,
};

/*
 * Methods under UVERBS_OBJECT_RESTORE. Each installs a new uobject of
 * the named concrete class at a caller-chosen ufile handle (the
 * RESTORE_<TYPE>_HANDLE attr). The handler dispatches through the
 * driver's ib_device_ops.restore_<type> callback after first checking
 * ib_device_ops.ucontext_is_restore_mode on the caller's ucontext;
 * absence of either callback yields -EOPNOTSUPP / -EPERM respectively.
 */
enum uverbs_methods_restore {
	UVERBS_METHOD_RESTORE_PD,
	UVERBS_METHOD_RESTORE_MR,
	UVERBS_METHOD_RESTORE_CQ,
	UVERBS_METHOD_RESTORE_QP,
};

enum uverbs_attrs_restore_pd {
	/*
	 * Mandatory u32 input. The target ufile handle the restored PD
	 * uobject must occupy. Reserved via xa_insert(); if the handle
	 * is already taken in the calling ufile's idr the method
	 * returns -EBUSY.
	 */
	UVERBS_ATTR_RESTORE_PD_HANDLE,
};

/*
 * UVERBS_METHOD_RESTORE_MR attributes.
 *
 * The wire-visible identity (lkey, rkey) is carried in the
 * UVERBS_ATTR_RESTORE_MR_LKEY_HINT / _RKEY_HINT core attrs. These
 * are *hints*: drivers that can preserve the source's keys (mlx5)
 * consume them; drivers that cannot (rxe -- key bits are tied to
 * the rxe_pool slot allocator) ignore them and return their own
 * fresh keys via RESP_LKEY/_RKEY. The actual lkey/rkey the kernel
 * installed is *always* returned via RESP_LKEY/_RKEY, regardless
 * of whether the driver honoured the hint, so generic userspace
 * (CRIU plugin, INFO_HANDLES consumers) can compare without
 * needing per-driver decoders.
 *
 * Driver-private state (e.g. mlx5's FW mkey index, separately
 * carried in struct mlx5_ib_restore_mr_req) travels through
 * UVERBS_ATTR_UHW(). The driver is responsible for validating the
 * consistency between its UHW payload and the generic lkey/rkey
 * hints (e.g. mlx5 enforces (lkey_hint >> 8) == (rkey_hint >> 8)
 * == mkey_index for the v0 basic-user-MR case).
 */
enum uverbs_attrs_restore_mr {
	/*
	 * Mandatory u32 input. Target ufile handle the restored MR
	 * uobject must occupy. -EBUSY on collision; same semantics as
	 * UVERBS_ATTR_RESTORE_PD_HANDLE.
	 */
	UVERBS_ATTR_RESTORE_MR_HANDLE,
	/*
	 * Mandatory IDR input. Parent PD's ufile handle. Dispatcher
	 * resolves it to the live ib_pd; the new MR's usecnt edge is
	 * installed against this PD.
	 */
	UVERBS_ATTR_RESTORE_MR_PD_HANDLE,
	/* Mandatory u64 inputs: user virtual address + length + iova. */
	UVERBS_ATTR_RESTORE_MR_ADDR,
	UVERBS_ATTR_RESTORE_MR_LENGTH,
	UVERBS_ATTR_RESTORE_MR_IOVA,
	/* Mandatory enum ib_access_flags input. */
	UVERBS_ATTR_RESTORE_MR_ACCESS_FLAGS,
	/*
	 * Mandatory u32 inputs. Wire-visible identity hints; driver
	 * may honour or ignore.
	 */
	UVERBS_ATTR_RESTORE_MR_LKEY_HINT,
	UVERBS_ATTR_RESTORE_MR_RKEY_HINT,
	/*
	 * Mandatory u32 outputs. Actual lkey/rkey the kernel installed.
	 * Equals the hints on a driver that honours them (mlx5 v0);
	 * differs on a driver that ignored them (rxe).
	 */
	UVERBS_ATTR_RESTORE_MR_RESP_LKEY,
	UVERBS_ATTR_RESTORE_MR_RESP_RKEY,
};

/*
 * UVERBS_METHOD_RESTORE_CQ attributes.
 *
 * RESTORE_CQ deliberately omits a "cqn_hint" core attr. Unlike MR's
 * lkey/rkey -- which are wire-visible RDMA-spec identifiers carried
 * in every read/write opcode and therefore MUST survive restore --
 * the CQ identifier is not part of any wire-protocol header. Drivers
 * with FW-side cqn (mlx5_vfmig) carry the source's cqn through their
 * private UHW payload (see struct mlx5_ib_restore_cq_req, lands in
 * S5 B1) and the driver enforces continuity internally. Drivers
 * with no cqn concept (rxe) simply allocate a fresh pool slot the
 * same way rxe_create_cq() does. K8a's NLDEV RES_HANDLE for the
 * restored CQ matches RESTORE_CQ_HANDLE -- the ufile handle which
 * the dispatcher reserves via rdma_alloc_begin_uobject_at_handle()
 * -- so user-visible identity (RES_HANDLE) is preserved out of band.
 *
 * COMP_CHANNEL is declared UA_OPTIONAL for forward compat with a
 * future UVERBS_METHOD_RESTORE_COMP_CHANNEL, but the v0 dispatcher
 * hard-rejects with -EOPNOTSUPP if any caller actually passes one.
 * v0 callers (CRIU plugin) must not pass a CC reference; CQs that
 * were bound to a comp_channel on the source side cannot be
 * restored end-to-end until RESTORE_COMP_CHANNEL lands.
 *
 * EVENT_FD is also UA_OPTIONAL. The dispatcher routes through
 * ib_uverbs_get_async_event(), which falls back to the ufile's
 * default_async_file when the attr is absent (the v0 path: CRIU
 * plugin does not pass an event_fd because RESTORE_ASYNC_EVENT has
 * not landed yet). When RESTORE_ASYNC_EVENT does land in S8, the
 * attr will resolve to the restored async-event uobject without any
 * UAPI bump.
 */
enum uverbs_attrs_restore_cq {
	/*
	 * Mandatory u32 input. Target ufile handle the restored CQ
	 * uobject must occupy. -EBUSY on collision; same semantics as
	 * UVERBS_ATTR_RESTORE_PD_HANDLE / _MR_HANDLE.
	 */
	UVERBS_ATTR_RESTORE_CQ_HANDLE,
	/*
	 * Mandatory u32 input. The CQ size requested by userspace
	 * (the @cqe arg to ibv_create_cq on the source side). The
	 * driver may round up; the actual installed size is returned
	 * via UVERBS_ATTR_RESTORE_CQ_RESP_CQE.
	 */
	UVERBS_ATTR_RESTORE_CQ_CQE,
	/*
	 * Mandatory u64 input. The user-supplied tag the source
	 * passed as ibv_create_cq()'s cq_context parameter. Stored
	 * verbatim in struct ib_uobject::user_handle so libibverbs
	 * sees the same value via cq->cq_context post-restore.
	 */
	UVERBS_ATTR_RESTORE_CQ_USER_HANDLE,
	/*
	 * Mandatory u32 input. The completion vector (~ EQ index for
	 * mlx5; not strictly meaningful for rxe) the source CQ was
	 * bound to. Must be < num_comp_vectors of the restore-side
	 * device.
	 */
	UVERBS_ATTR_RESTORE_CQ_COMP_VECTOR,
	/*
	 * Optional FLAGS_IN. ib_uverbs_ex_create_cq_flags subset:
	 * IB_UVERBS_CQ_FLAGS_TIMESTAMP_COMPLETION,
	 * IB_UVERBS_CQ_FLAGS_IGNORE_OVERRUN. Absent -> 0.
	 */
	UVERBS_ATTR_RESTORE_CQ_FLAGS,
	/*
	 * Optional FD reference into UVERBS_OBJECT_COMP_CHANNEL.
	 * Forward-compat slot for RESTORE_COMP_CHANNEL; v0 dispatcher
	 * rejects -EOPNOTSUPP if any caller supplies one.
	 */
	UVERBS_ATTR_RESTORE_CQ_COMP_CHANNEL,
	/*
	 * Optional FD reference into UVERBS_OBJECT_ASYNC_EVENT.
	 * Resolves via ib_uverbs_get_async_event(); absent ->
	 * ufile->default_async_file (the v0 CRIU plugin path).
	 */
	UVERBS_ATTR_RESTORE_CQ_EVENT_FD,
	/*
	 * Mandatory u32 output. The actual cqe count the kernel
	 * installed -- equals UVERBS_ATTR_RESTORE_CQ_CQE for drivers
	 * that don't round up, may be larger otherwise. Mirrors
	 * UVERBS_ATTR_CREATE_CQ_RESP_CQE on the create-side path.
	 */
	UVERBS_ATTR_RESTORE_CQ_RESP_CQE,
};

/*
 * UVERBS_METHOD_RESTORE_QP -- install a QP uobject at the caller-
 * specified target ufile handle, on top of the source-side QP state
 * the destination VHCA inherited (mlx5) or out of a fresh kernel-
 * side wrapper (rxe). The dispatcher gates on the parent
 * ucontext->ops.ucontext_is_restore_mode predicate so non-CRIU
 * userspace cannot reach this path. v0 covers RC, UC and UD; XRC,
 * GSI, RAW_PACKET, DRIVER (DCT/DCI) are dispatcher-rejected with
 * -EOPNOTSUPP and re-enabled in later stages with the matching
 * driver-private UHW shape.
 *
 * Field-level shape mirrors the create-side ioctl method
 * UVERBS_METHOD_QP_CREATE so the capabilities the source's
 * ibv_create_qp() supplied (qp_type, cap, send_cq, recv_cq, srq,
 * create_flags) round-trip across SAVE/LOAD; the captured runtime
 * QP state (RESET / INIT / RTR / RTS / SQD / SQE / ERR) is
 * surfaced via UVERBS_ATTR_RESTORE_QP_STATE so the driver can
 * skip the source-side MODIFY_QP chain if its FW has already
 * preserved the qpc state. Driver-private FW state (mlx5's
 * adopted qpn, source userspace VAs of WQ-ring/DBR umems, BFREG
 * index, ECE options etc.; see struct mlx5_ib_restore_qp_req)
 * travels through @udata via UVERBS_ATTR_UHW(); rxe ignores @udata.
 */
enum uverbs_attrs_restore_qp {
	/*
	 * Mandatory u32 input. Target ufile handle the restored QP
	 * uobject must occupy. -EBUSY on collision; same semantics as
	 * UVERBS_ATTR_RESTORE_{PD,MR,CQ}_HANDLE.
	 */
	UVERBS_ATTR_RESTORE_QP_HANDLE,
	/*
	 * Mandatory IDR reference into UVERBS_OBJECT_PD. Parent
	 * protection domain (already restored via
	 * UVERBS_METHOD_RESTORE_PD).
	 */
	UVERBS_ATTR_RESTORE_QP_PD_HANDLE,
	/*
	 * Mandatory IDR reference into UVERBS_OBJECT_CQ. Send
	 * completion queue (already restored via
	 * UVERBS_METHOD_RESTORE_CQ). Must live on the same device.
	 */
	UVERBS_ATTR_RESTORE_QP_SEND_CQ_HANDLE,
	/*
	 * Mandatory IDR reference into UVERBS_OBJECT_CQ. Receive
	 * completion queue (already restored). May alias the send
	 * CQ for self-loopback / DUAL_RC patterns.
	 */
	UVERBS_ATTR_RESTORE_QP_RECV_CQ_HANDLE,
	/*
	 * Optional IDR reference into UVERBS_OBJECT_SRQ for QPs
	 * created with a shared receive queue. Absent for v0 RC/UD;
	 * UVERBS_METHOD_RESTORE_SRQ has not landed yet (S6c).
	 */
	UVERBS_ATTR_RESTORE_QP_SRQ_HANDLE,
	/*
	 * Mandatory u32 input. The IBTA QP type (enum ib_qp_type
	 * value: IB_QPT_RC, IB_QPT_UD, ...). Drivers reject
	 * unsupported types with -EOPNOTSUPP; v0 accepts RC, UC, UD.
	 */
	UVERBS_ATTR_RESTORE_QP_TYPE,
	/*
	 * Mandatory u32 input. The captured final QP state at
	 * SAVE_VHCA_STATE-time (enum ib_qp_state value: IB_QPS_INIT,
	 * IB_QPS_RTR, IB_QPS_RTS, ...). The driver uses this to
	 * decide whether the adopted FW QPC is already in the right
	 * state or whether a destination-side MODIFY_QP chain is
	 * required (mlx5 v0: no chain needed -- empirically validated
	 * via design/uobject_restore.md K7 STRONG PASS).
	 */
	UVERBS_ATTR_RESTORE_QP_STATE,
	/*
	 * Mandatory u64 input. The user-supplied tag the source
	 * passed as ibv_create_qp()'s qp_context parameter. Stored
	 * verbatim in ib_uobject::user_handle so libibverbs sees the
	 * same value via qp->qp_context post-restore.
	 */
	UVERBS_ATTR_RESTORE_QP_USER_HANDLE,
	/*
	 * Mandatory PTR_IN(struct ib_uverbs_qp_cap). The capability
	 * tuple ibv_create_qp() returned to the source (max_send_wr,
	 * max_recv_wr, max_send_sge, max_recv_sge, max_inline_data).
	 * Reused verbatim from the create-side wire ABI -- the cap
	 * struct shape has been stable since 2009. Drivers stamp
	 * their kernel-side ib_qp_cap from this and may round up
	 * the WQ-ring size to the same value the source-side
	 * create_qp() rounded to (the WQ umem byte length is a
	 * function of these fields, so equivalent rounding is
	 * required for the umem_restore handshake).
	 */
	UVERBS_ATTR_RESTORE_QP_CAP,
	/*
	 * Optional FLAGS_IN(enum ib_uverbs_qp_create_flags). The
	 * IBTA + vendor create_flags the source passed to
	 * ibv_create_qp_ex() (CROSS_CHANNEL, BLOCK_MULTICAST_LOOPBACK,
	 * SCATTER_FCS, ...). Absent -> 0.
	 */
	UVERBS_ATTR_RESTORE_QP_CREATE_FLAGS,
	/*
	 * Optional FD reference into UVERBS_OBJECT_ASYNC_EVENT for
	 * QPs whose source-side async event channel was not the
	 * ufile default. Forward-compat with RESTORE_ASYNC_EVENT (S8).
	 * Absent -> ufile->default_async_file, which is what the v0
	 * CRIU plugin path supplies.
	 */
	UVERBS_ATTR_RESTORE_QP_EVENT_FD,
	/*
	 * Mandatory u32 output. The actual qpn the kernel installed.
	 * For mlx5 this equals the source-side qpn the UHW supplied
	 * (FW resource-id continuity is the whole point of the
	 * adoption path); for rxe it is whatever rxe_pool_alloc()
	 * returned. Userspace compares to detect the latter case
	 * and to fail-fast on cross-arch migration.
	 */
	UVERBS_ATTR_RESTORE_QP_RESP_QPN,
};

enum uverbs_attrs_invoke_write_cmd_attr_ids {
	UVERBS_ATTR_CORE_IN,
	UVERBS_ATTR_CORE_OUT,
	UVERBS_ATTR_WRITE_CMD,
};

enum uverbs_attrs_query_port_cmd_attr_ids {
	UVERBS_ATTR_QUERY_PORT_PORT_NUM,
	UVERBS_ATTR_QUERY_PORT_RESP,
};

enum uverbs_attrs_query_port_speed_cmd_attr_ids {
	UVERBS_ATTR_QUERY_PORT_SPEED_PORT_NUM,
	UVERBS_ATTR_QUERY_PORT_SPEED_RESP,
};

enum uverbs_attrs_query_comp_cntr_caps_attr_ids {
	UVERBS_ATTR_QUERY_COMP_CNTR_CAPS_MAX_COUNTERS,
	UVERBS_ATTR_QUERY_COMP_CNTR_CAPS_MAX_VALUE,
	UVERBS_ATTR_QUERY_COMP_CNTR_CAPS_SUPPORTED_QP_ATTACH_OPS,
};

enum uverbs_attrs_get_context_attr_ids {
	UVERBS_ATTR_GET_CONTEXT_NUM_COMP_VECTORS,
	UVERBS_ATTR_GET_CONTEXT_CORE_SUPPORT,
	UVERBS_ATTR_GET_CONTEXT_FD_ARR,
};

enum uverbs_attrs_query_context_attr_ids {
	UVERBS_ATTR_QUERY_CONTEXT_NUM_COMP_VECTORS,
	UVERBS_ATTR_QUERY_CONTEXT_CORE_SUPPORT,
};

enum uverbs_attrs_create_cq_cmd_attr_ids {
	UVERBS_ATTR_CREATE_CQ_HANDLE,
	UVERBS_ATTR_CREATE_CQ_CQE,
	UVERBS_ATTR_CREATE_CQ_USER_HANDLE,
	UVERBS_ATTR_CREATE_CQ_COMP_CHANNEL,
	UVERBS_ATTR_CREATE_CQ_COMP_VECTOR,
	UVERBS_ATTR_CREATE_CQ_FLAGS,
	UVERBS_ATTR_CREATE_CQ_RESP_CQE,
	UVERBS_ATTR_CREATE_CQ_EVENT_FD,
	UVERBS_ATTR_CREATE_CQ_BUFFER_VA,
	UVERBS_ATTR_CREATE_CQ_BUFFER_LENGTH,
	UVERBS_ATTR_CREATE_CQ_BUFFER_FD,
	UVERBS_ATTR_CREATE_CQ_BUFFER_OFFSET,
	UVERBS_ATTR_CREATE_CQ_BUF_UMEM,
};

enum uverbs_attrs_destroy_cq_cmd_attr_ids {
	UVERBS_ATTR_DESTROY_CQ_HANDLE,
	UVERBS_ATTR_DESTROY_CQ_RESP,
};

enum uverbs_attrs_create_flow_action_esp {
	UVERBS_ATTR_CREATE_FLOW_ACTION_ESP_HANDLE,
	UVERBS_ATTR_FLOW_ACTION_ESP_ATTRS,
	UVERBS_ATTR_FLOW_ACTION_ESP_ESN,
	UVERBS_ATTR_FLOW_ACTION_ESP_KEYMAT,
	UVERBS_ATTR_FLOW_ACTION_ESP_REPLAY,
	UVERBS_ATTR_FLOW_ACTION_ESP_ENCAP,
};

enum uverbs_attrs_modify_flow_action_esp {
	UVERBS_ATTR_MODIFY_FLOW_ACTION_ESP_HANDLE =
		UVERBS_ATTR_CREATE_FLOW_ACTION_ESP_HANDLE,
};

enum uverbs_attrs_destroy_flow_action_esp {
	UVERBS_ATTR_DESTROY_FLOW_ACTION_HANDLE,
};

enum uverbs_attrs_create_qp_cmd_attr_ids {
	UVERBS_ATTR_CREATE_QP_HANDLE,
	UVERBS_ATTR_CREATE_QP_XRCD_HANDLE,
	UVERBS_ATTR_CREATE_QP_PD_HANDLE,
	UVERBS_ATTR_CREATE_QP_SRQ_HANDLE,
	UVERBS_ATTR_CREATE_QP_SEND_CQ_HANDLE,
	UVERBS_ATTR_CREATE_QP_RECV_CQ_HANDLE,
	UVERBS_ATTR_CREATE_QP_IND_TABLE_HANDLE,
	UVERBS_ATTR_CREATE_QP_USER_HANDLE,
	UVERBS_ATTR_CREATE_QP_CAP,
	UVERBS_ATTR_CREATE_QP_TYPE,
	UVERBS_ATTR_CREATE_QP_FLAGS,
	UVERBS_ATTR_CREATE_QP_SOURCE_QPN,
	UVERBS_ATTR_CREATE_QP_EVENT_FD,
	UVERBS_ATTR_CREATE_QP_RESP_CAP,
	UVERBS_ATTR_CREATE_QP_RESP_QP_NUM,
	UVERBS_ATTR_CREATE_QP_BUF_UMEM,
	UVERBS_ATTR_CREATE_QP_RQ_BUF_UMEM,
	UVERBS_ATTR_CREATE_QP_SQ_BUF_UMEM,
};

enum uverbs_attrs_destroy_qp_cmd_attr_ids {
	UVERBS_ATTR_DESTROY_QP_HANDLE,
	UVERBS_ATTR_DESTROY_QP_RESP,
};

enum uverbs_attrs_qp_attach_comp_cntr_cmd_attr_ids {
	UVERBS_ATTR_QP_ATTACH_COMP_CNTR_HANDLE,
	UVERBS_ATTR_QP_ATTACH_COMP_CNTR_CNTR_HANDLE,
	UVERBS_ATTR_QP_ATTACH_COMP_CNTR_OP_MASK,
};

enum uverbs_methods_qp {
	UVERBS_METHOD_QP_CREATE,
	UVERBS_METHOD_QP_DESTROY,
	UVERBS_METHOD_QP_ATTACH_COMP_CNTR,
};

enum uverbs_attrs_create_srq_cmd_attr_ids {
	UVERBS_ATTR_CREATE_SRQ_HANDLE,
	UVERBS_ATTR_CREATE_SRQ_PD_HANDLE,
	UVERBS_ATTR_CREATE_SRQ_XRCD_HANDLE,
	UVERBS_ATTR_CREATE_SRQ_CQ_HANDLE,
	UVERBS_ATTR_CREATE_SRQ_USER_HANDLE,
	UVERBS_ATTR_CREATE_SRQ_MAX_WR,
	UVERBS_ATTR_CREATE_SRQ_MAX_SGE,
	UVERBS_ATTR_CREATE_SRQ_LIMIT,
	UVERBS_ATTR_CREATE_SRQ_MAX_NUM_TAGS,
	UVERBS_ATTR_CREATE_SRQ_TYPE,
	UVERBS_ATTR_CREATE_SRQ_EVENT_FD,
	UVERBS_ATTR_CREATE_SRQ_RESP_MAX_WR,
	UVERBS_ATTR_CREATE_SRQ_RESP_MAX_SGE,
	UVERBS_ATTR_CREATE_SRQ_RESP_SRQ_NUM,
	UVERBS_ATTR_CREATE_SRQ_BUF_UMEM,
};

enum uverbs_attrs_destroy_srq_cmd_attr_ids {
	UVERBS_ATTR_DESTROY_SRQ_HANDLE,
	UVERBS_ATTR_DESTROY_SRQ_RESP,
};

enum uverbs_methods_srq {
	UVERBS_METHOD_SRQ_CREATE,
	UVERBS_METHOD_SRQ_DESTROY,
};

enum uverbs_methods_cq {
	UVERBS_METHOD_CQ_CREATE,
	UVERBS_METHOD_CQ_DESTROY,
};

enum uverbs_attrs_create_wq_cmd_attr_ids {
	UVERBS_ATTR_CREATE_WQ_HANDLE,
	UVERBS_ATTR_CREATE_WQ_PD_HANDLE,
	UVERBS_ATTR_CREATE_WQ_CQ_HANDLE,
	UVERBS_ATTR_CREATE_WQ_USER_HANDLE,
	UVERBS_ATTR_CREATE_WQ_TYPE,
	UVERBS_ATTR_CREATE_WQ_EVENT_FD,
	UVERBS_ATTR_CREATE_WQ_MAX_WR,
	UVERBS_ATTR_CREATE_WQ_MAX_SGE,
	UVERBS_ATTR_CREATE_WQ_FLAGS,
	UVERBS_ATTR_CREATE_WQ_RESP_MAX_WR,
	UVERBS_ATTR_CREATE_WQ_RESP_MAX_SGE,
	UVERBS_ATTR_CREATE_WQ_RESP_WQ_NUM,
};

enum uverbs_attrs_destroy_wq_cmd_attr_ids {
	UVERBS_ATTR_DESTROY_WQ_HANDLE,
	UVERBS_ATTR_DESTROY_WQ_RESP,
};

enum uverbs_methods_wq {
	UVERBS_METHOD_WQ_CREATE,
	UVERBS_METHOD_WQ_DESTROY,
};

enum uverbs_methods_actions_flow_action_ops {
	UVERBS_METHOD_FLOW_ACTION_ESP_CREATE,
	UVERBS_METHOD_FLOW_ACTION_DESTROY,
	UVERBS_METHOD_FLOW_ACTION_ESP_MODIFY,
};

enum uverbs_attrs_alloc_dm_cmd_attr_ids {
	UVERBS_ATTR_ALLOC_DM_HANDLE,
	UVERBS_ATTR_ALLOC_DM_LENGTH,
	UVERBS_ATTR_ALLOC_DM_ALIGNMENT,
};

enum uverbs_attrs_free_dm_cmd_attr_ids {
	UVERBS_ATTR_FREE_DM_HANDLE,
};

enum uverbs_methods_dm {
	UVERBS_METHOD_DM_ALLOC,
	UVERBS_METHOD_DM_FREE,
};

enum uverbs_attrs_alloc_dmah_cmd_attr_ids {
	UVERBS_ATTR_ALLOC_DMAH_HANDLE,
	UVERBS_ATTR_ALLOC_DMAH_CPU_ID,
	UVERBS_ATTR_ALLOC_DMAH_TPH_MEM_TYPE,
	UVERBS_ATTR_ALLOC_DMAH_PH,
};

enum uverbs_attrs_free_dmah_cmd_attr_ids {
	UVERBS_ATTR_FREE_DMA_HANDLE,
};

enum uverbs_methods_dmah {
	UVERBS_METHOD_DMAH_ALLOC,
	UVERBS_METHOD_DMAH_FREE,
};

enum uverbs_attrs_alloc_dmabuf_cmd_attr_ids {
	UVERBS_ATTR_ALLOC_DMABUF_HANDLE,
	UVERBS_ATTR_ALLOC_DMABUF_PGOFF,
};

enum uverbs_methods_dmabuf {
	UVERBS_METHOD_DMABUF_ALLOC,
};

enum uverbs_attrs_reg_dm_mr_cmd_attr_ids {
	UVERBS_ATTR_REG_DM_MR_HANDLE,
	UVERBS_ATTR_REG_DM_MR_OFFSET,
	UVERBS_ATTR_REG_DM_MR_LENGTH,
	UVERBS_ATTR_REG_DM_MR_PD_HANDLE,
	UVERBS_ATTR_REG_DM_MR_ACCESS_FLAGS,
	UVERBS_ATTR_REG_DM_MR_DM_HANDLE,
	UVERBS_ATTR_REG_DM_MR_RESP_LKEY,
	UVERBS_ATTR_REG_DM_MR_RESP_RKEY,
};

enum uverbs_methods_mr {
	UVERBS_METHOD_DM_MR_REG,
	UVERBS_METHOD_MR_DESTROY,
	UVERBS_METHOD_ADVISE_MR,
	UVERBS_METHOD_QUERY_MR,
	UVERBS_METHOD_REG_DMABUF_MR,
	UVERBS_METHOD_REG_MR,
};

enum uverbs_attrs_mr_destroy_ids {
	UVERBS_ATTR_DESTROY_MR_HANDLE,
};

enum uverbs_attrs_advise_mr_cmd_attr_ids {
	UVERBS_ATTR_ADVISE_MR_PD_HANDLE,
	UVERBS_ATTR_ADVISE_MR_ADVICE,
	UVERBS_ATTR_ADVISE_MR_FLAGS,
	UVERBS_ATTR_ADVISE_MR_SGE_LIST,
};

enum uverbs_attrs_query_mr_cmd_attr_ids {
	UVERBS_ATTR_QUERY_MR_HANDLE,
	UVERBS_ATTR_QUERY_MR_RESP_LKEY,
	UVERBS_ATTR_QUERY_MR_RESP_RKEY,
	UVERBS_ATTR_QUERY_MR_RESP_LENGTH,
	UVERBS_ATTR_QUERY_MR_RESP_IOVA,
	/*
	 * The user VA the MR was registered against (the 'addr' arg to
	 * reg_user_mr). 0 for non-user MRs (DMABUF, DM, FR, etc.).
	 * Returns -ENODATA via attribute absence in the response bundle
	 * when the kernel does not have the value; userspace MUST treat
	 * a missing attribute as "unknown" rather than 0.
	 *
	 * Security boundary: the MR handle lookup is gated by the
	 * standard uverbs IDR machinery, which requires the caller to
	 * own the ufile that owns the MR. This is strictly tighter than
	 * NLDEV's CAP_NET_ADMIN gate, which is the reason we chose the
	 * uverbs ioctl over an NLDEV TLV -- CRIU is the only known
	 * consumer and CRIU already has the holder's uverbsfd open at
	 * dump time.
	 */
	UVERBS_ATTR_QUERY_MR_RESP_USER_ADDR,
	UVERBS_ATTR_QUERY_MR_RESP_ACCESS_FLAGS,
};

enum uverbs_attrs_reg_dmabuf_mr_cmd_attr_ids {
	UVERBS_ATTR_REG_DMABUF_MR_HANDLE,
	UVERBS_ATTR_REG_DMABUF_MR_PD_HANDLE,
	UVERBS_ATTR_REG_DMABUF_MR_OFFSET,
	UVERBS_ATTR_REG_DMABUF_MR_LENGTH,
	UVERBS_ATTR_REG_DMABUF_MR_IOVA,
	UVERBS_ATTR_REG_DMABUF_MR_FD,
	UVERBS_ATTR_REG_DMABUF_MR_ACCESS_FLAGS,
	UVERBS_ATTR_REG_DMABUF_MR_RESP_LKEY,
	UVERBS_ATTR_REG_DMABUF_MR_RESP_RKEY,
};

enum uverbs_attrs_reg_mr_cmd_attr_ids {
	UVERBS_ATTR_REG_MR_HANDLE,
	UVERBS_ATTR_REG_MR_PD_HANDLE,
	UVERBS_ATTR_REG_MR_DMA_HANDLE,
	UVERBS_ATTR_REG_MR_IOVA,
	UVERBS_ATTR_REG_MR_ADDR,
	UVERBS_ATTR_REG_MR_LENGTH,
	UVERBS_ATTR_REG_MR_ACCESS_FLAGS,
	UVERBS_ATTR_REG_MR_FD,
	UVERBS_ATTR_REG_MR_FD_OFFSET,
	UVERBS_ATTR_REG_MR_RESP_LKEY,
	UVERBS_ATTR_REG_MR_RESP_RKEY,
};

enum uverbs_attrs_create_counters_cmd_attr_ids {
	UVERBS_ATTR_CREATE_COUNTERS_HANDLE,
};

enum uverbs_attrs_destroy_counters_cmd_attr_ids {
	UVERBS_ATTR_DESTROY_COUNTERS_HANDLE,
};

enum uverbs_attrs_read_counters_cmd_attr_ids {
	UVERBS_ATTR_READ_COUNTERS_HANDLE,
	UVERBS_ATTR_READ_COUNTERS_BUFF,
	UVERBS_ATTR_READ_COUNTERS_FLAGS,
};

enum uverbs_methods_actions_counters_ops {
	UVERBS_METHOD_COUNTERS_CREATE,
	UVERBS_METHOD_COUNTERS_DESTROY,
	UVERBS_METHOD_COUNTERS_READ,
};

enum uverbs_attrs_info_handles_id {
	UVERBS_ATTR_INFO_OBJECT_ID,
	UVERBS_ATTR_INFO_TOTAL_HANDLES,
	UVERBS_ATTR_INFO_HANDLES_LIST,
};

enum uverbs_methods_pd {
	UVERBS_METHOD_PD_DESTROY,
};

enum uverbs_attrs_pd_destroy_ids {
	UVERBS_ATTR_DESTROY_PD_HANDLE,
};

enum uverbs_methods_mw {
	UVERBS_METHOD_MW_DESTROY,
};

enum uverbs_attrs_mw_destroy_ids {
	UVERBS_ATTR_DESTROY_MW_HANDLE,
};

enum uverbs_methods_xrcd {
	UVERBS_METHOD_XRCD_DESTROY,
};

enum uverbs_attrs_xrcd_destroy_ids {
	UVERBS_ATTR_DESTROY_XRCD_HANDLE,
};

enum uverbs_methods_ah {
	UVERBS_METHOD_AH_DESTROY,
};

enum uverbs_attrs_ah_destroy_ids {
	UVERBS_ATTR_DESTROY_AH_HANDLE,
};

enum uverbs_methods_rwq_ind_tbl {
	UVERBS_METHOD_RWQ_IND_TBL_DESTROY,
};

enum uverbs_attrs_rwq_ind_tbl_destroy_ids {
	UVERBS_ATTR_DESTROY_RWQ_IND_TBL_HANDLE,
};

enum uverbs_methods_flow {
	UVERBS_METHOD_FLOW_DESTROY,
};

enum uverbs_attrs_flow_destroy_ids {
	UVERBS_ATTR_DESTROY_FLOW_HANDLE,
};

enum uverbs_method_async_event {
	UVERBS_METHOD_ASYNC_EVENT_ALLOC,
};

enum uverbs_attrs_async_event_create {
	UVERBS_ATTR_ASYNC_EVENT_ALLOC_FD_HANDLE,
};

enum uverbs_attrs_query_gid_table_cmd_attr_ids {
	UVERBS_ATTR_QUERY_GID_TABLE_ENTRY_SIZE,
	UVERBS_ATTR_QUERY_GID_TABLE_FLAGS,
	UVERBS_ATTR_QUERY_GID_TABLE_RESP_ENTRIES,
	UVERBS_ATTR_QUERY_GID_TABLE_RESP_NUM_ENTRIES,
};

enum uverbs_attrs_query_gid_entry_cmd_attr_ids {
	UVERBS_ATTR_QUERY_GID_ENTRY_PORT,
	UVERBS_ATTR_QUERY_GID_ENTRY_GID_INDEX,
	UVERBS_ATTR_QUERY_GID_ENTRY_FLAGS,
	UVERBS_ATTR_QUERY_GID_ENTRY_RESP_ENTRY,
};

enum uverbs_methods_comp_cntr {
	UVERBS_METHOD_COMP_CNTR_CREATE,
	UVERBS_METHOD_COMP_CNTR_DESTROY,
	UVERBS_METHOD_COMP_CNTR_MODIFY,
	UVERBS_METHOD_COMP_CNTR_READ,
};

enum uverbs_attrs_create_comp_cntr_cmd_attr_ids {
	UVERBS_ATTR_CREATE_COMP_CNTR_HANDLE,
};

enum uverbs_attrs_destroy_comp_cntr_cmd_attr_ids {
	UVERBS_ATTR_DESTROY_COMP_CNTR_HANDLE,
};

enum uverbs_attrs_modify_comp_cntr_cmd_attr_ids {
	UVERBS_ATTR_MODIFY_COMP_CNTR_HANDLE,
	UVERBS_ATTR_MODIFY_COMP_CNTR_ENTRY,
	UVERBS_ATTR_MODIFY_COMP_CNTR_OP,
	UVERBS_ATTR_MODIFY_COMP_CNTR_VALUE,
};

enum uverbs_attrs_read_comp_cntr_cmd_attr_ids {
	UVERBS_ATTR_READ_COMP_CNTR_HANDLE,
	UVERBS_ATTR_READ_COMP_CNTR_ENTRY,
	UVERBS_ATTR_READ_COMP_CNTR_RESP_VALUE,
};

#endif

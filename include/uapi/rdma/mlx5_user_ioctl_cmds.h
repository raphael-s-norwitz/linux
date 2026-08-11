/*
 * Copyright (c) 2018, Mellanox Technologies inc.  All rights reserved.
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

#ifndef MLX5_USER_IOCTL_CMDS_H
#define MLX5_USER_IOCTL_CMDS_H

#include <linux/types.h>
#include <rdma/ib_user_ioctl_cmds.h>

enum mlx5_ib_create_flow_action_attrs {
	/* This attribute belong to the driver namespace */
	MLX5_IB_ATTR_CREATE_FLOW_ACTION_FLAGS = (1U << UVERBS_ID_NS_SHIFT),
};

enum mlx5_ib_dm_methods {
	MLX5_IB_METHOD_DM_MAP_OP_ADDR  = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_METHOD_DM_QUERY,
};

enum mlx5_ib_dm_map_op_addr_attrs {
	MLX5_IB_ATTR_DM_MAP_OP_ADDR_REQ_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_DM_MAP_OP_ADDR_REQ_OP,
	MLX5_IB_ATTR_DM_MAP_OP_ADDR_RESP_START_OFFSET,
	MLX5_IB_ATTR_DM_MAP_OP_ADDR_RESP_PAGE_INDEX,
};

enum mlx5_ib_query_dm_attrs {
	MLX5_IB_ATTR_QUERY_DM_REQ_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_QUERY_DM_RESP_START_OFFSET,
	MLX5_IB_ATTR_QUERY_DM_RESP_PAGE_INDEX,
	MLX5_IB_ATTR_QUERY_DM_RESP_LENGTH,
};

enum mlx5_ib_alloc_dm_attrs {
	MLX5_IB_ATTR_ALLOC_DM_RESP_START_OFFSET = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_ALLOC_DM_RESP_PAGE_INDEX,
	MLX5_IB_ATTR_ALLOC_DM_REQ_TYPE,
};

enum mlx5_ib_devx_methods {
	MLX5_IB_METHOD_DEVX_OTHER  = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_METHOD_DEVX_QUERY_UAR,
	MLX5_IB_METHOD_DEVX_QUERY_EQN,
	MLX5_IB_METHOD_DEVX_SUBSCRIBE_EVENT,
};

enum  mlx5_ib_devx_other_attrs {
	MLX5_IB_ATTR_DEVX_OTHER_CMD_IN = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_DEVX_OTHER_CMD_OUT,
};

enum mlx5_ib_devx_obj_create_attrs {
	MLX5_IB_ATTR_DEVX_OBJ_CREATE_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_DEVX_OBJ_CREATE_CMD_IN,
	MLX5_IB_ATTR_DEVX_OBJ_CREATE_CMD_OUT,
};

enum  mlx5_ib_devx_query_uar_attrs {
	MLX5_IB_ATTR_DEVX_QUERY_UAR_USER_IDX = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_DEVX_QUERY_UAR_DEV_IDX,
};

enum mlx5_ib_devx_obj_destroy_attrs {
	MLX5_IB_ATTR_DEVX_OBJ_DESTROY_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
};

enum mlx5_ib_devx_obj_modify_attrs {
	MLX5_IB_ATTR_DEVX_OBJ_MODIFY_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_DEVX_OBJ_MODIFY_CMD_IN,
	MLX5_IB_ATTR_DEVX_OBJ_MODIFY_CMD_OUT,
};

enum mlx5_ib_devx_obj_query_attrs {
	MLX5_IB_ATTR_DEVX_OBJ_QUERY_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_DEVX_OBJ_QUERY_CMD_IN,
	MLX5_IB_ATTR_DEVX_OBJ_QUERY_CMD_OUT,
};

enum mlx5_ib_devx_obj_query_async_attrs {
	MLX5_IB_ATTR_DEVX_OBJ_QUERY_ASYNC_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_DEVX_OBJ_QUERY_ASYNC_CMD_IN,
	MLX5_IB_ATTR_DEVX_OBJ_QUERY_ASYNC_FD,
	MLX5_IB_ATTR_DEVX_OBJ_QUERY_ASYNC_WR_ID,
	MLX5_IB_ATTR_DEVX_OBJ_QUERY_ASYNC_OUT_LEN,
};

enum mlx5_ib_devx_subscribe_event_attrs {
	MLX5_IB_ATTR_DEVX_SUBSCRIBE_EVENT_FD_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_DEVX_SUBSCRIBE_EVENT_OBJ_HANDLE,
	MLX5_IB_ATTR_DEVX_SUBSCRIBE_EVENT_TYPE_NUM_LIST,
	MLX5_IB_ATTR_DEVX_SUBSCRIBE_EVENT_FD_NUM,
	MLX5_IB_ATTR_DEVX_SUBSCRIBE_EVENT_COOKIE,
};

enum  mlx5_ib_devx_query_eqn_attrs {
	MLX5_IB_ATTR_DEVX_QUERY_EQN_USER_VEC = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_DEVX_QUERY_EQN_DEV_EQN,
};

enum mlx5_ib_devx_obj_methods {
	MLX5_IB_METHOD_DEVX_OBJ_CREATE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_METHOD_DEVX_OBJ_DESTROY,
	MLX5_IB_METHOD_DEVX_OBJ_MODIFY,
	MLX5_IB_METHOD_DEVX_OBJ_QUERY,
	MLX5_IB_METHOD_DEVX_OBJ_ASYNC_QUERY,
};

enum mlx5_ib_var_alloc_attrs {
	MLX5_IB_ATTR_VAR_OBJ_ALLOC_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_VAR_OBJ_ALLOC_MMAP_OFFSET,
	MLX5_IB_ATTR_VAR_OBJ_ALLOC_MMAP_LENGTH,
	MLX5_IB_ATTR_VAR_OBJ_ALLOC_PAGE_ID,
	MLX5_IB_ATTR_VAR_OBJ_ALLOC_FLAGS,
};

enum mlx5_ib_var_obj_destroy_attrs {
	MLX5_IB_ATTR_VAR_OBJ_DESTROY_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
};

enum mlx5_ib_var_obj_methods {
	MLX5_IB_METHOD_VAR_OBJ_ALLOC = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_METHOD_VAR_OBJ_DESTROY,
};

enum mlx5_ib_uar_alloc_attrs {
	MLX5_IB_ATTR_UAR_OBJ_ALLOC_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_UAR_OBJ_ALLOC_TYPE,
	MLX5_IB_ATTR_UAR_OBJ_ALLOC_MMAP_OFFSET,
	MLX5_IB_ATTR_UAR_OBJ_ALLOC_MMAP_LENGTH,
	MLX5_IB_ATTR_UAR_OBJ_ALLOC_PAGE_ID,
};

enum mlx5_ib_uar_obj_destroy_attrs {
	MLX5_IB_ATTR_UAR_OBJ_DESTROY_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
};

enum mlx5_ib_uar_obj_methods {
	MLX5_IB_METHOD_UAR_OBJ_ALLOC = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_METHOD_UAR_OBJ_DESTROY,
};

enum mlx5_ib_devx_umem_reg_attrs {
	MLX5_IB_ATTR_DEVX_UMEM_REG_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_DEVX_UMEM_REG_ADDR,
	MLX5_IB_ATTR_DEVX_UMEM_REG_LEN,
	MLX5_IB_ATTR_DEVX_UMEM_REG_ACCESS,
	MLX5_IB_ATTR_DEVX_UMEM_REG_OUT_ID,
	MLX5_IB_ATTR_DEVX_UMEM_REG_PGSZ_BITMAP,
	MLX5_IB_ATTR_DEVX_UMEM_REG_DMABUF_FD,
};

enum mlx5_ib_devx_umem_dereg_attrs {
	MLX5_IB_ATTR_DEVX_UMEM_DEREG_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
};

enum mlx5_ib_pp_obj_methods {
	MLX5_IB_METHOD_PP_OBJ_ALLOC = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_METHOD_PP_OBJ_DESTROY,
};

enum mlx5_ib_pp_alloc_attrs {
	MLX5_IB_ATTR_PP_OBJ_ALLOC_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_PP_OBJ_ALLOC_CTX,
	MLX5_IB_ATTR_PP_OBJ_ALLOC_FLAGS,
	MLX5_IB_ATTR_PP_OBJ_ALLOC_INDEX,
};

enum mlx5_ib_pp_obj_destroy_attrs {
	MLX5_IB_ATTR_PP_OBJ_DESTROY_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
};

enum mlx5_ib_devx_umem_methods {
	MLX5_IB_METHOD_DEVX_UMEM_REG = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_METHOD_DEVX_UMEM_DEREG,
};

enum mlx5_ib_devx_async_cmd_fd_alloc_attrs {
	MLX5_IB_ATTR_DEVX_ASYNC_CMD_FD_ALLOC_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
};

enum mlx5_ib_devx_async_event_fd_alloc_attrs {
	MLX5_IB_ATTR_DEVX_ASYNC_EVENT_FD_ALLOC_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_DEVX_ASYNC_EVENT_FD_ALLOC_FLAGS,
};

enum mlx5_ib_devx_async_cmd_fd_methods {
	MLX5_IB_METHOD_DEVX_ASYNC_CMD_FD_ALLOC = (1U << UVERBS_ID_NS_SHIFT),
};

enum mlx5_ib_devx_async_event_fd_methods {
	MLX5_IB_METHOD_DEVX_ASYNC_EVENT_FD_ALLOC = (1U << UVERBS_ID_NS_SHIFT),
};

enum mlx5_ib_objects {
	MLX5_IB_OBJECT_DEVX = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_OBJECT_DEVX_OBJ,
	MLX5_IB_OBJECT_DEVX_UMEM,
	MLX5_IB_OBJECT_FLOW_MATCHER,
	MLX5_IB_OBJECT_DEVX_ASYNC_CMD_FD,
	MLX5_IB_OBJECT_DEVX_ASYNC_EVENT_FD,
	MLX5_IB_OBJECT_VAR,
	MLX5_IB_OBJECT_PP,
	MLX5_IB_OBJECT_UAR,
	MLX5_IB_OBJECT_STEERING_ANCHOR,
	/*
	 * Verb-only namespace (no per-instance state, no IDR) for the
	 * VFMIG (CRIU SR-IOV migration) per-ucontext save/restore verbs.
	 */
	MLX5_IB_OBJECT_VFMIG,
};

enum mlx5_ib_flow_matcher_create_attrs {
	MLX5_IB_ATTR_FLOW_MATCHER_CREATE_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_FLOW_MATCHER_MATCH_MASK,
	MLX5_IB_ATTR_FLOW_MATCHER_FLOW_TYPE,
	MLX5_IB_ATTR_FLOW_MATCHER_MATCH_CRITERIA,
	MLX5_IB_ATTR_FLOW_MATCHER_FLOW_FLAGS,
	MLX5_IB_ATTR_FLOW_MATCHER_FT_TYPE,
	MLX5_IB_ATTR_FLOW_MATCHER_IB_PORT,
};

enum mlx5_ib_flow_matcher_destroy_attrs {
	MLX5_IB_ATTR_FLOW_MATCHER_DESTROY_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
};

enum mlx5_ib_flow_matcher_methods {
	MLX5_IB_METHOD_FLOW_MATCHER_CREATE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_METHOD_FLOW_MATCHER_DESTROY,
};

enum mlx5_ib_flow_steering_anchor_create_attrs {
	MLX5_IB_ATTR_STEERING_ANCHOR_CREATE_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_STEERING_ANCHOR_FT_TYPE,
	MLX5_IB_ATTR_STEERING_ANCHOR_PRIORITY,
	MLX5_IB_ATTR_STEERING_ANCHOR_FT_ID,
};

enum mlx5_ib_flow_steering_anchor_destroy_attrs {
	MLX5_IB_ATTR_STEERING_ANCHOR_DESTROY_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
};

enum mlx5_ib_steering_anchor_methods {
	MLX5_IB_METHOD_STEERING_ANCHOR_CREATE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_METHOD_STEERING_ANCHOR_DESTROY,
};

enum mlx5_ib_device_query_context_attrs {
	MLX5_IB_ATTR_QUERY_CONTEXT_RESP_UCTX = (1U << UVERBS_ID_NS_SHIFT),
};

enum mlx5_ib_create_cq_attrs {
	MLX5_IB_ATTR_CREATE_CQ_UAR_INDEX = UVERBS_ID_DRIVER_NS_WITH_UHW,
	MLX5_IB_ATTR_CREATE_CQ_DBR_BUF_UMEM,
};

enum mlx5_ib_create_qp_attrs {
	MLX5_IB_ATTR_CREATE_QP_DBR_BUF_UMEM = UVERBS_ID_DRIVER_NS_WITH_UHW,
};

enum mlx5_ib_create_srq_attrs {
	MLX5_IB_ATTR_CREATE_SRQ_DBR_BUF_UMEM = UVERBS_ID_DRIVER_NS_WITH_UHW,
};

enum mlx5_ib_reg_dmabuf_mr_attrs {
	MLX5_IB_ATTR_REG_DMABUF_MR_ACCESS_FLAGS = (1U << UVERBS_ID_NS_SHIFT),
};

#define MLX5_IB_DW_MATCH_PARAM 0xA0

struct mlx5_ib_match_params {
	__u32	match_params[MLX5_IB_DW_MATCH_PARAM];
};

enum mlx5_ib_flow_type {
	MLX5_IB_FLOW_TYPE_NORMAL,
	MLX5_IB_FLOW_TYPE_SNIFFER,
	MLX5_IB_FLOW_TYPE_ALL_DEFAULT,
	MLX5_IB_FLOW_TYPE_MC_DEFAULT,
};

enum mlx5_ib_create_flow_flags {
	MLX5_IB_ATTR_CREATE_FLOW_FLAGS_DEFAULT_MISS = 1 << 0,
	MLX5_IB_ATTR_CREATE_FLOW_FLAGS_DROP = 1 << 1,
};

enum mlx5_ib_create_flow_attrs {
	MLX5_IB_ATTR_CREATE_FLOW_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_CREATE_FLOW_MATCH_VALUE,
	MLX5_IB_ATTR_CREATE_FLOW_DEST_QP,
	MLX5_IB_ATTR_CREATE_FLOW_DEST_DEVX,
	MLX5_IB_ATTR_CREATE_FLOW_MATCHER,
	MLX5_IB_ATTR_CREATE_FLOW_ARR_FLOW_ACTIONS,
	MLX5_IB_ATTR_CREATE_FLOW_TAG,
	MLX5_IB_ATTR_CREATE_FLOW_ARR_COUNTERS_DEVX,
	MLX5_IB_ATTR_CREATE_FLOW_ARR_COUNTERS_DEVX_OFFSET,
	MLX5_IB_ATTR_CREATE_FLOW_FLAGS,
};

enum mlx5_ib_destroy_flow_attrs {
	MLX5_IB_ATTR_DESTROY_FLOW_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
};

enum mlx5_ib_flow_methods {
	MLX5_IB_METHOD_CREATE_FLOW = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_METHOD_DESTROY_FLOW,
};

enum mlx5_ib_flow_action_methods {
	MLX5_IB_METHOD_FLOW_ACTION_CREATE_MODIFY_HEADER = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_METHOD_FLOW_ACTION_CREATE_PACKET_REFORMAT,
};

enum mlx5_ib_create_flow_action_create_modify_header_attrs {
	MLX5_IB_ATTR_CREATE_MODIFY_HEADER_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_CREATE_MODIFY_HEADER_ACTIONS_PRM,
	MLX5_IB_ATTR_CREATE_MODIFY_HEADER_FT_TYPE,
};

enum mlx5_ib_create_flow_action_create_packet_reformat_attrs {
	MLX5_IB_ATTR_CREATE_PACKET_REFORMAT_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_CREATE_PACKET_REFORMAT_TYPE,
	MLX5_IB_ATTR_CREATE_PACKET_REFORMAT_FT_TYPE,
	MLX5_IB_ATTR_CREATE_PACKET_REFORMAT_DATA_BUF,
};

enum mlx5_ib_query_pd_attrs {
	MLX5_IB_ATTR_QUERY_PD_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_QUERY_PD_RESP_PDN,
};

enum mlx5_ib_pd_methods {
	MLX5_IB_METHOD_PD_QUERY = (1U << UVERBS_ID_NS_SHIFT),

};

enum mlx5_ib_device_methods {
	MLX5_IB_METHOD_QUERY_PORT = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_METHOD_GET_DATA_DIRECT_SYSFS_PATH,
};

enum mlx5_ib_query_port_attrs {
	MLX5_IB_ATTR_QUERY_PORT_PORT_NUM = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_QUERY_PORT,
};

enum mlx5_ib_get_data_direct_sysfs_path_attrs {
	MLX5_IB_ATTR_GET_DATA_DIRECT_SYSFS_PATH = (1U << UVERBS_ID_NS_SHIFT),
};

/*
 * VFMIG ucontext vendor verbs. Methods on the verb-only object
 * MLX5_IB_OBJECT_VFMIG; the calling fd's ucontext is implicit (resolved
 * via ib_uverbs_get_ucontext()).
 *
 * Wire-protocol contract: identifiers in UAR_TABLE / BFREG_COUNT are
 * opaque FW UAR ids and per-bfreg-slot counts respectively, as returned
 * by mlx5 firmware. They are valid only against a destination VHCA whose
 * state was imported by SAVE_VHCA_STATE / LOAD_VHCA_STATE from the
 * source VHCA they were captured on.
 */
enum mlx5_ib_vfmig_methods {
	MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_METHOD_VFMIG_RESTORE_UCONTEXT,
	/*
	 * Dynamic-UAR (lib_uar_dyn=true / MLX5_LIB_CAP_DYN_UAR) save/restore.
	 * libmlx5 in this mode does not use bfregi->sys_pages[]; UARs are
	 * MLX5_IB_OBJECT_UAR uobjects allocated lazily via UAR_OBJ_ALLOC.
	 * QUERY_DYN_UARS snapshots every outstanding such uobject; a later
	 * RESTORE_DYN_UARS replays them at the same handles/offsets.
	 */
	MLX5_IB_METHOD_VFMIG_QUERY_DYN_UARS,
	MLX5_IB_METHOD_VFMIG_RESTORE_DYN_UARS,
};

/*
 * Two-pass call convention:
 *
 *  Pass 1 (sizing): pass META only. Kernel fills META; UAR_TABLE and
 *    BFREG_COUNT are absent (UA_OPTIONAL). Userspace reads
 *    meta.num_sys_pages and meta.total_num_bfregs from the result and
 *    allocates u32[num_sys_pages] and u32[total_num_bfregs] buffers.
 *
 *  Pass 2 (snapshot): pass META + UAR_TABLE + BFREG_COUNT, with the
 *    arrays sized exactly as learned from pass 1. Kernel snapshots
 *    bfregi->sys_pages[] and bfregi->count[] under bfregi->lock.
 *
 * The handler rejects with -EINVAL if the array buffer sizes don't
 * match the current ucontext shape, so racing a call against e.g. a
 * concurrent dynamic-UAR mmap is detectable rather than silently
 * truncating.
 */
enum mlx5_ib_vfmig_query_ucontext_attrs {
	MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_UAR_TABLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_BFREG_COUNT,
	MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_META,
};

/*
 * RESTORE_UCONTEXT consumes a snapshot previously emitted by
 * QUERY_UCONTEXT on a source ucontext (whose VHCA was then
 * SAVE_VHCA_STATE'd and LOAD_VHCA_STATE'd onto this destination VHCA)
 * and seeds the destination ucontext's bfregi->sys_pages[] verbatim,
 * skipping the per-slot ALLOC_UAR commands mlx5_ib_alloc_ucontext would
 * otherwise issue. This relies on the destination VHCA already holding
 * those FW UAR ids reserved as part of LOAD_VHCA_STATE.
 *
 * Preconditions (all enforced by the handler; -EINVAL on failure):
 *   1. ucontext was created with MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE
 *      (so sys_pages[] is sentinel-INVALID and ready to be seeded), and
 *      lib_uar_dyn=false (v0 doesn't cover dynamic-UAR ucontexts).
 *   2. META cross-check: every shape-defining field must match what the
 *      destination's mlx5_ib_alloc_ucontext computed for THIS ucontext.
 *   3. UAR_TABLE length == num_sys_pages * sizeof(__u32); the static
 *      slots [0..num_static) must all be valid FW ids.
 *   4. BFREG_COUNT, if supplied, length == total_num_bfregs *
 *      sizeof(__u32), and v0 requires every entry zero (non-zero implies
 *      live QPs / claimed dyn UARs not yet rebuilt).
 *
 * On success sys_pages[] is seeded under bfregi->lock and
 * c->vfmig_restore_pending is cleared, so a second RESTORE on the same
 * ucontext fails precondition #1: exactly one RESTORE per ucontext.
 */
enum mlx5_ib_vfmig_restore_ucontext_attrs {
	MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_UAR_TABLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_BFREG_COUNT,
	MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_META,
};

/*
 * QUERY_DYN_UARS attribute IDs.
 *
 *   RECORDS: array of struct mlx5_ib_vfmig_dyn_uar_record. OPTIONAL --
 *            omit for the sizing pass. If supplied, its length must equal
 *            COUNT * sizeof(struct mlx5_ib_vfmig_dyn_uar_record).
 *   COUNT:   __u32. Kernel always writes the number of dynamic UARs held
 *            by this ucontext, so the sizing pass tells userspace how big
 *            RECORDS must be and the snapshot pass confirms it.
 */
enum mlx5_ib_vfmig_query_dyn_uars_attrs {
	MLX5_IB_ATTR_VFMIG_QUERY_DYN_UARS_RECORDS = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_VFMIG_QUERY_DYN_UARS_COUNT,
};

/*
 * RESTORE_DYN_UARS replays a QUERY_DYN_UARS snapshot onto a destination
 * lib_uar_dyn=true ucontext (opened with MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE),
 * recreating each MLX5_IB_OBJECT_UAR uobject at its source handle and
 * mmap offset without issuing ALLOC_UAR (the FW UAR ids are already held
 * by the destination VHCA via LOAD_VHCA_STATE). Single-shot: it consumes
 * c->vfmig_restore_pending like RESTORE_UCONTEXT does.
 *
 *   RECORDS: array of struct mlx5_ib_vfmig_dyn_uar_record, length ==
 *            COUNT (from the QUERY_DYN_UARS sizing pass) * record size.
 */
enum mlx5_ib_vfmig_restore_dyn_uars_attrs {
	MLX5_IB_ATTR_VFMIG_RESTORE_DYN_UARS_RECORDS = (1U << UVERBS_ID_NS_SHIFT),
};

/*
 * Per-dynamic-UAR record exchanged across migration.
 *
 *   handle:      ufile->idr handle of the MLX5_IB_OBJECT_UAR uobject on
 *                the source ucontext (userspace's opaque UAR handle).
 *   uar_index:   FW UAR id (page_idx). Valid only against a destination
 *                VHCA whose state was imported via LOAD_VHCA_STATE.
 *   mmap_offset: libmlx5-wire-format mmap offset of the source
 *                rdma_user_mmap_entry (what libmlx5 passed to mmap()).
 *   alloc_type:  MLX5_IB_UAPI_UAR_ALLOC_TYPE_BF (write-combining) or
 *                MLX5_IB_UAPI_UAR_ALLOC_TYPE_NC (uncached).
 */
struct mlx5_ib_vfmig_dyn_uar_record {
	__u32	handle;
	__u32	uar_index;
	__aligned_u64 mmap_offset;
	__u8	alloc_type;
	__u8	reserved0[7];
};

/*
 * Ucontext create-time settings snapshotted by QUERY_UCONTEXT and
 * cross-checked by RESTORE_UCONTEXT. @devx_uid is the exception: it
 * carries the source ucontext's mlx5_ib_ucontext::devx_uid (0 =
 * non-DEVX) across the SAVE/LOAD seam as diagnostics only --
 * RESTORE_UCONTEXT logs a mismatch and continues rather than gating,
 * since the default libmlx5 ucontext auto-allocates a fresh DEVX uid
 * so source and dest uids differ by construction.
 * The layout is fixed wire ABI; reserved fields must be zero.
 */
struct mlx5_ib_vfmig_ucontext_meta {
	__u32	num_static_sys_pages;
	__u32	num_sys_pages;
	__u32	num_dyn_bfregs;
	__u32	num_low_latency_bfregs;
	__u32	total_num_bfregs;
	__u32	reserved0;
	__aligned_u64 lib_caps;
	__u8	lib_uar_4k;
	__u8	lib_uar_dyn;
	__u8	cqe_version;
	__u8	reserved1[3];
	__u16	devx_uid;
};

#endif

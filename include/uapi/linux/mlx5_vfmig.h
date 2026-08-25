/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * UAPI for mlx5 host-driven VF migration / CRIU restore.
 *
 * One char device per mlx5_core PF, exposed as /dev/mlx5_vfmig/<bdf>,
 * lets the host PF introspect and migrate the firmware state of its
 * SR-IOV VFs without going through a guest VM. This header grows one
 * command at a time as the kernel side lands each piece of
 * functionality; it currently covers only PF-side introspection.
 *
 * All commands address a VF by its @vf_id, the SR-IOV VF index in
 * 0..num_vfs-1 (i.e. the virtfn<vf_id> under the PF).
 */

#ifndef _UAPI_LINUX_MLX5_VFMIG_H
#define _UAPI_LINUX_MLX5_VFMIG_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define MLX5_VFMIG_IOC_MAGIC	0xB5

/*
 * MLX5_VFMIG_IOC_MARK_RESTORED:
 *   Latch VF @vf_id as having had its firmware state restored. A later
 *   mlx5_core probe of that VF consumes the bit to skip re-init of state
 *   that was loaded out of band. Returns 0 on success, -EINVAL if @vf_id
 *   is out of range or @flags carries unknown bits, -EALREADY if the bit
 *   was already set and no @flags were requested.
 *
 *   @flags:
 *     MLX5_VFMIG_MARK_RESTORED_DEFER_RESUME -- snapshot-ordering restore
 *       mirror: the next probe applies LOAD_VHCA_STATE but leaves the
 *       VHCA parked (STOP) instead of resuming it, so CRIU can bring the
 *       datapath live with an explicit MLX5_VFMIG_IOC_RESUME_VHCA at
 *       RESUME_DEVICES_LATE, once every MR/ring VMA has been restored.
 *       Honored even when the restored bit is already set (e.g. installed
 *       by the LOAD ioctl's close()), in which case the call is a no-op
 *       success rather than -EALREADY.
 */
#define MLX5_VFMIG_MARK_RESTORED_DEFER_RESUME	(1u << 0)
#define MLX5_VFMIG_MARK_RESTORED_FLAG_ALL \
	(MLX5_VFMIG_MARK_RESTORED_DEFER_RESUME)

struct mlx5_vfmig_mark_restored {
	__u32 vf_id;	/* in */
	__u32 flags;	/* in: subset of MLX5_VFMIG_MARK_RESTORED_FLAG_* */
};

#define MLX5_VFMIG_IOC_MARK_RESTORED \
	_IOW(MLX5_VFMIG_IOC_MAGIC, 0x01, struct mlx5_vfmig_mark_restored)

/*
 * MLX5_VFMIG_IOC_GET_VHCA_ID:
 *   PF-side query of the VF's vhca_id via QUERY_HCA_CAP(other_function=1).
 *   Lets userspace confirm the PF can address the VF without binding any
 *   driver to it. Returns 0 on success, -EINVAL if @vf_id is out of range
 *   or @reserved is non-zero.
 */
struct mlx5_vfmig_get_vhca_id {
	__u32 vf_id;	/* in  */
	__u16 vhca_id;	/* out */
	__u16 reserved;
};

#define MLX5_VFMIG_IOC_GET_VHCA_ID \
	_IOWR(MLX5_VFMIG_IOC_MAGIC, 0x02, struct mlx5_vfmig_get_vhca_id)

/*
 * MLX5_VFMIG_IOC_QUERY_VF:
 *   Diagnostic snapshot of one VF on the owning PF: its live vhca_id
 *   (queried via QUERY_HCA_CAP(other_function=1)), the "restored" bit
 *   latched on the PF, the "tracked" state, the orchestrator-stamped
 *   @vf_uuid (if any), and the total number of VFs provisioned.
 *   Userspace iterates @vf_id 0..num_vfs-1 to enumerate. Returns 0 on
 *   success, or -ERANGE if @vf_id >= num_vfs (with @num_vfs still filled
 *   in so callers can size their iteration; @vhca_id / @restored /
 *   @tracked / @vf_uuid are zeroed).
 *
 *   Output fields:
 *     vhca_id:   live VHCA identifier.
 *     restored:  1 if MARK_RESTORED was issued (or a LOAD blob staged)
 *                for this VF, i.e. its next probe should skip INIT_HCA.
 *     tracked:   1 if MLX5_VFMIG_IOC_SET_TRACKED{enable=1} attached a
 *                per-VF deterministic IOVA domain to this VF (see that
 *                command). Cleared on SR-IOV teardown (sriov_numvfs=0).
 *     vf_uuid:   16-byte orchestrator-stamped per-VF identity tag set
 *                via MLX5_VFMIG_IOC_SET_VF_UUID. All-zeros means unset.
 *                Cleared on SR-IOV teardown (sriov_numvfs=0).
 *
 *   ABI note: this struct grew to add @vf_uuid + @reserved_out. The
 *   encoded ioctl number changes with the struct size (sizeof in the
 *   _IOWR macro), so old userspace built against the smaller struct gets
 *   -ENOTTY from a new kernel rather than a partial read.
 */
struct mlx5_vfmig_query_vf {
	__u32 vf_id;		/* in  */
	__u32 num_vfs;		/* out: total VFs provisioned on this PF */
	__u16 vhca_id;		/* out */
	__u8  restored;		/* out: 1 if MARK_RESTORED was issued */
	__u8  tracked;		/* out: 1 if a vfmig IOVA domain is attached */
	__u8  vf_uuid[16];	/* out: orchestrator-stamped UUID,
				 *      all-zeros if unset
				 */
	__u8  reserved_out[8];	/* out: zeroed */
};

#define MLX5_VFMIG_IOC_QUERY_VF \
	_IOWR(MLX5_VFMIG_IOC_MAGIC, 0x03, struct mlx5_vfmig_query_vf)

/*
 * MLX5_VFMIG_IOC_ENABLE_MIGRATABLE:
 *   Set the per-VF cmd_hca_cap_2.migratable bit, the firmware gate for
 *   SUSPEND/SAVE/LOAD/RESUME. This MUST be called while the VF is
 *   unbound (no driver attached): firmware accepts the modify-cap on a
 *   VHCA in pre-ENABLE_HCA state but rejects it on one mlx5_core has
 *   already probed.
 *
 *   Idempotent: returns 0 with no firmware traffic if the bit is
 *   already set. Returns -EOPNOTSUPP if the PF firmware does not
 *   advertise migration / vhca_resource_manager, -EINVAL if @vf_id is
 *   out of range or @reserved is non-zero. The bit is intentionally
 *   left set across mlx5_core probes.
 */
struct mlx5_vfmig_enable_migratable {
	__u32 vf_id;	/* in */
	__u32 reserved;
};

#define MLX5_VFMIG_IOC_ENABLE_MIGRATABLE \
	_IOW(MLX5_VFMIG_IOC_MAGIC, 0x06, struct mlx5_vfmig_enable_migratable)

/*
 * MLX5_VFMIG_IOC_SET_TRACKED:
 *   Toggle "vfmig owns this VF's address space" for VF @vf_id. With
 *   @enable=1 the driver allocates an unmanaged paging iommu_domain and
 *   attaches it to the VF in place of its default DMA domain, staking out
 *   a deterministic per-VF IOVA window; @enable=0 detaches and frees it,
 *   restoring the default DMA domain. QUERY_VF reports the resulting
 *   state in @tracked.
 *
 *   This is the foundation for reproducing, on a restore host, the exact
 *   IOVAs a SAVE captured inside the firmware blob. The deterministic
 *   allocator and the SAVE/LOAD page replay that populate the domain land
 *   in later kernels; on its own this command only creates/destroys the
 *   domain.
 *
 *   The VF MUST be unbound (no driver attached) -- both because a bound
 *   driver's DMA mappings live in the domain being replaced, and because
 *   while our unmanaged domain is attached the VF has no working
 *   dma_alloc_coherent() until the allocator routing lands. Enable it
 *   before binding the workload; the state is cleared on SR-IOV teardown
 *   (sriov_numvfs=0) and PF unload.
 *
 *   Idempotent: a toggle to the state the VF is already in is a no-op
 *   (returns 0, no iommu traffic).
 *
 *   Errors:
 *     -EFAULT     copy_from_user.
 *     -EINVAL     @vf_id out of range, @enable > 1, or @flags/@reserved
 *                 non-zero.
 *     -ENODEV     the VF pci_dev could not be found.
 *     -EBUSY      the VF is bound to a driver (must be unbound first).
 *     -EOPNOTSUPP the per-VF IOVA window does not fit the IOMMU aperture.
 *     other <0    iommu core error from domain alloc/attach.
 */
struct mlx5_vfmig_set_tracked {
	__u32 vf_id;		/* in  */
	__u32 enable;		/* in: 0 = untrack, 1 = track */
	__u32 flags;		/* in: reserved, must be 0 */
	__u32 reserved;		/* in: reserved, must be 0 */
};

#define MLX5_VFMIG_IOC_SET_TRACKED \
	_IOW(MLX5_VFMIG_IOC_MAGIC, 0x07, struct mlx5_vfmig_set_tracked)

/*
 * Direction selector for SUSPEND_VHCA / RESUME_VHCA @flags. The firmware
 * migration FSM has a RUNNING <-> RUNNING_P2P <-> STOP ladder driven by
 * per-direction SUSPEND/RESUME:
 *   RUNNING       both directions live
 *   RUNNING_P2P   responder answers peers; initiator quiesced
 *   STOP          fully parked
 *
 * @flags picks which direction(s) an ioctl drives:
 *   0 (== INITIATOR | RESPONDER)  the full fused ladder in one call
 *   INITIATOR                     SUSPEND: RUNNING->P2P;  RESUME: P2P->RUNNING
 *   RESPONDER                     SUSPEND: P2P->STOP;     RESUME: STOP->P2P
 *
 * A directional request out of order for the current state (e.g.
 * SUSPEND(RESPONDER) while still RUNNING) is rejected with -EINVAL; a
 * request already satisfied is a no-op (returns 0, no firmware traffic).
 */
#define MLX5_VFMIG_DIR_FLAG_INITIATOR	(1u << 0)
#define MLX5_VFMIG_DIR_FLAG_RESPONDER	(1u << 1)
#define MLX5_VFMIG_DIR_FLAG_ALL \
	(MLX5_VFMIG_DIR_FLAG_INITIATOR | MLX5_VFMIG_DIR_FLAG_RESPONDER)

/*
 * MLX5_VFMIG_IOC_SUSPEND_VHCA:
 *   Quiesce VF @vf_id's datapath toward STOP (PF-issued SUSPEND_VHCA on
 *   the VF's vhca_id, other_function=1) so no peer RDMA and no VF self-DMA
 *   lands mid-migration. With @flags == 0 this issues SUSPEND(INITIATOR)
 *   then SUSPEND(RESPONDER), walking RUNNING -> RUNNING_P2P -> STOP in one
 *   call; @flags may instead select a single ladder step (see
 *   MLX5_VFMIG_DIR_FLAG_*). The VF may be bound or unbound but must be
 *   migration-enabled (see ENABLE_MIGRATABLE).
 *
 *   Idempotent: returns 0 with no firmware traffic if the requested depth
 *   is already reached. On a partial failure the reached depth is latched
 *   (truthfully) and the error returned; recover with RESUME_VHCA.
 *
 *   Returns 0 on success; -EINVAL if @vf_id is out of range, @flags has
 *   unknown bits or is out of order for the current state, or @reserved is
 *   non-zero; -EOPNOTSUPP if the VF is not migration-enabled; or a negative
 *   firmware error if a SUSPEND step fails.
 */
struct mlx5_vfmig_suspend_vhca {
	__u32 vf_id;		/* in  */
	__u32 flags;		/* in: 0 or a subset of MLX5_VFMIG_DIR_FLAG_* */
	__u32 reserved[2];
};

#define MLX5_VFMIG_IOC_SUSPEND_VHCA \
	_IOW(MLX5_VFMIG_IOC_MAGIC, 0x13, struct mlx5_vfmig_suspend_vhca)

/*
 * MLX5_VFMIG_IOC_RESUME_VHCA:
 *   Un-quiesce VF @vf_id's datapath, the inverse of SUSPEND_VHCA. With
 *   @flags == 0 this issues RESUME(RESPONDER) then RESUME(INITIATOR),
 *   walking STOP -> RUNNING_P2P -> RUNNING in one call; @flags may instead
 *   select a single ladder step (see MLX5_VFMIG_DIR_FLAG_*). Use it on the
 *   source to recover a VF after an aborted migration.
 *
 *   Idempotent: returns 0 with no firmware traffic if the requested depth
 *   is already reached. On a partial failure the reached depth is latched
 *   (truthfully) and the error returned.
 *
 *   Returns 0 on success; -EINVAL if @vf_id is out of range, @flags has
 *   unknown bits or is out of order for the current state, or @reserved is
 *   non-zero; or a negative firmware error if a RESUME step fails.
 */
struct mlx5_vfmig_resume_vhca {
	__u32 vf_id;		/* in  */
	__u32 flags;		/* in: 0 or a subset of MLX5_VFMIG_DIR_FLAG_* */
	__u32 reserved[2];
};

#define MLX5_VFMIG_IOC_RESUME_VHCA \
	_IOW(MLX5_VFMIG_IOC_MAGIC, 0x14, struct mlx5_vfmig_resume_vhca)

/*
 * KEEP_SUSPENDED: when SAVE self-suspended the VF (it was not already
 * parked via SUSPEND_VHCA), leave it parked on close instead of resuming
 * it. Ignored when the VF was pre-parked by the caller (that suspend is
 * the caller's to resume).
 */
#define MLX5_VFMIG_SAVE_FLAG_KEEP_SUSPENDED	(1u << 0)
#define MLX5_VFMIG_SAVE_FLAG_ALL \
	(MLX5_VFMIG_SAVE_FLAG_KEEP_SUSPENDED)

/*
 * MLX5_VFMIG_IOC_SAVE_VHCA_STATE:
 *   Open a read-only data session that captures VF @vf_id's current
 *   firmware state into a blob. The VF must be migration-enabled (see
 *   ENABLE_MIGRATABLE).
 *
 *   SAVE needs the VHCA quiesced to STOP. If the caller already parked it
 *   via MLX5_VFMIG_IOC_SUSPEND_VHCA, SAVE captures it as-is and leaves the
 *   resume to the caller's RESUME_VHCA. Otherwise SAVE transiently
 *   suspends the VF itself and, on close, resumes exactly the ladder steps
 *   it issued -- unless @flags carries MLX5_VFMIG_SAVE_FLAG_KEEP_SUSPENDED,
 *   in which case a self-suspended VF is left parked. The persistent
 *   SUSPEND/RESUME_VHCA datapath state is never changed by SAVE.
 *
 *   On the ioctl call the driver synchronously queries the VF's vhca_id,
 *   sizes the snapshot via QUERY_VHCA_MIGRATION_STATE, allocates a PD +
 *   image pages + MKEY (DMA_FROM_DEVICE), runs SAVE_VHCA_STATE to populate
 *   the pages, and returns @save_fd, an anon-inode fd. Userspace read()s
 *   the blob from it (any chunk size) until EOF. The stream begins with a
 *   16-byte FW_DATA record header (record_size, flags=0, tag=0) followed by
 *   the firmware payload, so it can later be fed verbatim into
 *   LOAD_VHCA_STATE. Closing @save_fd frees the firmware resources.
 *
 *   Returns 0 with @save_fd populated on success; -EINVAL if @vf_id is out
 *   of range or @flags / @reserved carry unknown bits; -EOPNOTSUPP if the
 *   VF is not migration-enabled; -EBUSY if a save session already exists
 *   for this vf_id; -ENODEV if the PF is gone; or a negative firmware error
 *   if a QUERY/SUSPEND/SAVE step fails.
 */
struct mlx5_vfmig_save_state {
	__u32 vf_id;	/* in  */
	__u32 flags;	/* in: 0 or MLX5_VFMIG_SAVE_FLAG_* */
	__s32 save_fd;	/* out */
	__u32 reserved;
};

#define MLX5_VFMIG_IOC_SAVE_VHCA_STATE \
	_IOWR(MLX5_VFMIG_IOC_MAGIC, 0x05, struct mlx5_vfmig_save_state)

/*
 * MLX5_VFMIG_IOC_LOAD_VHCA_STATE:
 *   Open a write-only data session that stages a previously-saved state
 *   blob for VF @vf_id. It is the inverse of SAVE_VHCA_STATE and consumes
 *   the exact byte stream SAVE produced.
 *
 *   The VF must be migration-enabled (see ENABLE_MIGRATABLE). Unlike a
 *   direct LOAD_VHCA_STATE, no VHCA suspend is required (or performed) at
 *   ioctl time: the blob is only *staged* here. The actual
 *   LOAD_VHCA_STATE runs from the VF's next mlx5_core probe, which
 *   suspends the freshly-enabled VHCA (RUNNING -> STOP), issues the load
 *   and resumes it. The VF must therefore be unbound from its driver
 *   while a blob is staged and then (re-)bound to apply it.
 *
 *   The ioctl returns @load_fd, a write-only anon-inode fd. Userspace
 *   write()s the blob to it (any chunk size; partial writes are fine).
 *   The stream is a 16-byte FW_DATA record header (record_size, flags=0,
 *   tag=0) followed by that many payload bytes; once the full payload is
 *   received the driver stages it into DMA pages + MKEY. Exactly one
 *   FW_DATA record is supported; trailing bytes are rejected. Closing
 *   @load_fd after a complete blob hands the staged pages to the VF's
 *   pending-load slot (and latches "restored"); closing it early frees
 *   everything with no lasting effect.
 *
 *   Returns 0 with @load_fd populated on success; -EINVAL if @vf_id is out
 *   of range or @flags / @reserved are non-zero; -EOPNOTSUPP if the VF is
 *   not migration-enabled; -EBUSY if a save or load session already exists
 *   for this vf_id; -ENODEV if the PF is gone. write() errors surface
 *   -EPROTO/-EINVAL for a malformed stream. Any firmware error from the
 *   deferred LOAD_VHCA_STATE surfaces in the VF probe (dmesg), not here.
 */
struct mlx5_vfmig_load_state {
	__u32 vf_id;	/* in  */
	__u32 flags;	/* in: reserved, must be 0 */
	__s32 load_fd;	/* out */
	__u32 reserved;
};

#define MLX5_VFMIG_IOC_LOAD_VHCA_STATE \
	_IOWR(MLX5_VFMIG_IOC_MAGIC, 0x04, struct mlx5_vfmig_load_state)

/*
 * MLX5_VFMIG_IOC_SET_VF_UUID:
 *   Stamp the orchestrator's 16-byte UUID onto VF @vf_id's per-VF slot.
 *   Read back via MLX5_VFMIG_IOC_QUERY_VF on either the source or the
 *   destination host. This is the orchestrator's handle on "this VF
 *   carries this workload's identity"; it is the only identity tag CRIU's
 *   dump and restore paths consult when binding a saved-state image to a
 *   destination VF, because neither @vhca_id (per-PF allocator, unstable
 *   across SAVE/LOAD) nor @vf_id (per-PF slot, may differ source vs
 *   destination) is a workload-stable identifier.
 *
 *   The orchestrator (the SR-IOV provisioning layer) is the intended
 *   caller, on both hosts, before the workload binds the VF and before
 *   any LOAD_VHCA_STATE. CRIU never writes a UUID -- both dump and
 *   restore only READ @vf_uuid via QUERY_VF.
 *
 *   Lifecycle:
 *     - Initial state on sriov_numvfs=N: all-zeros (unset).
 *     - SET_VF_UUID(vf_id, U) first call:      @vf_uuid := U.
 *     - SET_VF_UUID(vf_id, U) repeat (same U): no-op, returns 0.
 *     - SET_VF_UUID(vf_id, V), V != U, U != 0: -EBUSY, unchanged. Guards
 *         against re-tagging a slot that already carries an identity.
 *     - sriov_numvfs=0 / PF unload:            all-zeros (torn down).
 *
 *   "Set once until teardown": to repurpose a vf_id slot for a different
 *   workload, cycle sriov_numvfs=0 -> N first (the same cycle that already
 *   tears down the slot's other per-VF resources). Idempotent re-stamps
 *   with the same UUID need no teardown.
 *
 *   Errors:
 *     -EFAULT  copy_from_user.
 *     -EINVAL  @vf_id out of range, @reserved non-zero, or @vf_uuid
 *              all-zeros (all-zeros is the "unset" sentinel and is
 *              rejected as a write).
 *     -EBUSY   a different non-zero UUID is already set on @vf_id.
 *     -ENODEV  PF is gone.
 */
struct mlx5_vfmig_set_vf_uuid {
	__u32 vf_id;		/* in  */
	__u32 reserved;		/* in: must be 0 */
	__u8  vf_uuid[16];	/* in: orchestrator-supplied 16-byte
				 *     UUID; must not be all-zeros.
				 */
};

#define MLX5_VFMIG_IOC_SET_VF_UUID \
	_IOW(MLX5_VFMIG_IOC_MAGIC, 0x12, struct mlx5_vfmig_set_vf_uuid)

#endif /* _UAPI_LINUX_MLX5_VFMIG_H */

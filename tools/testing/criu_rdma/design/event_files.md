# Completion channels and async event files across a migration

## Summary

A process that waits for completions with `ibv_req_notify_cq()` and
`ibv_get_cq_event()`, or reads `ibv_get_async_event()`, cannot be
checkpointed today. RESTORE_CQ refuses a completion channel, a restored
CQ has no `user_handle`, and the event counters that rdma-core's destroy
waits on restart at zero. Nothing tells CRIU which `[infinibandevent]`
file is a channel, or which objects use which file.

The kernel needs five additions, all in RDMA core except the last:

1. fdinfo on both event file kinds gains a type, a per-context file ID,
   whether the file is the context's default async file, and its queued
   event count.
2. Dump-side queries return, for one CQ or QP, its `user_handle`, the
   IDs of its completion channel and async file, its event counters and
   how many of its events are queued unread; and, for the context, the
   ID of its default async file.
3. RESTORE_CQ accepts a completion channel, the counters to start from,
   and a number of completion events to queue on the channel. RESTORE_QP
   accepts its counter. In restore mode, both refuse to fall back to a
   default async file that does not exist yet.
4. Async events queued but unread at dump are carried across: a
   non-destructive peek on dump, and a re-queue on restore.
5. mlx5 replays a completion event that firmware raised for a restored
   CQ before RESTORE_CQ registered it, instead of dropping it.

The dump-side queries are CHECKPOINT_* methods of the existing
`UVERBS_OBJECT_RESTORE`. Choosing the default async file needs no new
method: CRIU opens the restore context with the ioctl GET_CONTEXT, which
creates no async file, and allocates the image's default first.

CRIU refuses to dump a context that has an event file no FD carries, an
object with no async file, or an SRQ or WQ with event state.

Findings below were checked against `criu-rdma-7.2-ofed-devel` at
b14689dae8fe. The ABI is a proposal for review.

## How the pieces fit today

### Event files

Both kinds are FD uobjects backed by anon inodes named
`[infinibandevent]`: `UVERBS_OBJECT_COMP_CHANNEL`
(`uverbs_std_types.c`, `uverbs_event_fops`) and
`UVERBS_OBJECT_ASYNC_EVENT` (`uverbs_std_types_async_fd.c`,
`uverbs_async_event_fops`). Each embeds a `struct ib_uverbs_event_queue`:
a spinlock, a list of queued `struct ib_uverbs_event`, a wait queue and
an fasync queue.

- A **completion channel** is created only through the write command
  `IB_USER_VERBS_CMD_CREATE_COMP_CHANNEL` (`uverbs_cmd.c`,
  `ib_uverbs_create_comp_channel()`). It has no hardware state.
- An **async file** is created by the write-path `GET_CONTEXT`
  (`uverbs_cmd.c:289`) or by `UVERBS_METHOD_ASYNC_EVENT_ALLOC`. Each one
  registers its own `ib_event_handler`, so device and port events go to
  every async file of the context.

Both anon files share one inode, so `st_ino` does not tell them apart.
Their fdinfo prints only `ctxn:` (`ib_uverbs_fd_show_fdinfo()`).

`INFO_HANDLES` lists FD uobjects, but not by FD number:
`alloc_commit_fd_uobject()` (`rdma_core.c:765`) sets `uobj->id = 0`
once the file is installed. Every completion channel and async file
reports handle 0, so `INFO_HANDLES` cannot identify them at all.

### How events reach a file

- **Completion:** the driver calls `cq->comp_handler(cq,
  cq->cq_context)`, which is `ib_uverbs_comp_handler()`. `cq_context` is
  the channel's `ev_queue`, or NULL, in which case the event is dropped.
  The queued event carries `cq->uobject->uevent.uobject.user_handle`,
  and rdma-core turns it back into the `struct ibv_cq *`.
- **Async, per object:** `ib_uverbs_{cq,qp,srq,wq}_event_handler()` queue
  on `uevent.event_file`, chosen at create. It is the `EVENT_FD`
  attribute if given, else `ufile->default_async_file`
  (`ib_uverbs_get_async_event()`).
- Each queued event is on the file's list and on its object's list
  (`comp_list` or `uevent.event_list`), so destroying the object frees
  its queued events. Both lists are protected by the file's
  `ev_queue->lock`.

### Counters

`ib_uverbs_event_read()` increments `*event->counter` when the process
reads an event, not when it is queued. The counter is
`ib_ucq_object.comp_events_reported` for completions and
`ib_uevent_object.events_reported` for async object events. The destroy
verbs return them, and rdma-core waits until its acknowledged counts
match:

```c
while (cq->comp_events_completed != resp.comp_events_reported ||
       cq->async_events_completed != resp.async_events_reported)
	pthread_cond_wait(&cq->cond, &cq->mutex);
```

A restored object starts at zero while the process keeps its
acknowledged counts, so its destroy never returns. This already affects
any CQ or QP that received an async event before the dump.

### Restore today

`UVERBS_METHOD_RESTORE_CQ` (`uverbs_std_types_restore.c`):

- declares `UVERBS_ATTR_RESTORE_CQ_COMP_CHANNEL`, but returns
  `-EOPNOTSUPP` if it is passed (line 331);
- sets `cq->cq_context = NULL` (line 389);
- takes `USER_HANDLE`, which CRIU passes as 0 because no dump query
  reports it. vfmig `QUERY_QP` reports the QP's; vfmig `QUERY_CQ` does
  not.

The plugin opens the restore context with the write-path `GET_CONTEXT`
(`plugins/rdma/mlx5_sriov_vfmig/vfmig_uverbs.c`). That creates an async
file, which becomes `default_async_file`, and the plugin closes its FD
(line 536). The context keeps the file alive, so events for restored
objects and for objects created later through the write path
(`uverbs_cmd.c` 1087, 1483, 2908, 3396) go to a file nobody can read.

The ioctl `UVERBS_METHOD_GET_CONTEXT`
(`uverbs_std_types_device.c:245`) creates no async file and takes the
same driver UHW. rdma-core follows it with `ASYNC_EVENT_ALLOC`, which
becomes the default (`ib_uverbs_init_async_event_file()`).

### mlx5 CQs across LOAD

- RESTORE_CQ wires a restored CQ like a created user CQ
  (`mlx5_ib_set_user_cq_callbacks()`). The firmware EQ calls
  `mlx5_add_cq_to_tasklet()`, which calls `mlx5_ib_cq_comp()`, which
  calls `ibcq->comp_handler(ibcq, ibcq->cq_context)`. So a channel set on
  the restored `ib_cq` receives its events.
- `mlx5_core_adopt_cq()` enables EQE dispatch as soon as it registers
  the cqn. The core must therefore set `cq_context` before calling
  `ops.restore_cq`, which it already does for the NULL value.
- The CQ's arm state is in its firmware context (`cqc.st`: 6 armed for
  solicited completions only, 9 armed, 0xa fired), and it travels
  through SAVE/LOAD, so RESTORE_CQ needs no re-arm input. See "Harness:
  CQ arm state across LOAD" below.
- The source's completion EQs are adopted at VF probe
  (`restored_state_growth.md`), which runs before CRIU's RESTORE_CQ. An
  EQE for a CQ that is not registered yet is logged at debug level as
  "Completion event for bogus CQ" (`eq.c:142`) and dropped. That EQE was
  the arm firing, so the process may never be woken. CQ and QP async
  events in the same window are dropped the same way (`eq.c:728` and
  the QP resource lookup).

How likely that is depends on what can still write a CQE between LOAD
and RESTORE_CQ. A completion event needs an armed CQ and a new CQE. The
RoCE RX fence, which SAVE carries to the destination and which the
plugin lifts only at RESUME_IBDEV, after every RESTORE_CQ, drops every
incoming packet. That rules out receive completions, and also RC send
completions, which wait for ACKs. What it does not rule out:

- UD and UC sends still queued in firmware at SAVE. They complete
  without an ACK once the destination VF runs.
- RC sends still unacknowledged at SAVE. With the ACKs dropped they
  retry, and if the window outlasts the retries (`timeout` x
  `retry_cnt`), the QP fails with an error CQE.

`probes/fence_rc_timeout.sh` checks the RC case on a live VF with the
fence raised (CX-7, 2026-11-23). Whether the fenced VF sends and loses
the ACK or receives nothing, the sender gets "transport retry counter
exceeded", its QP moves to ERR, and neither side gets an async event.
So an RC timeout in the window produces an error CQE, and a completion
event if the CQ is armed, which section 5 replays; it loses no async
event. Loopback between two contexts on the fenced VF is dropped like
wire traffic, so it adds nothing to the list.

So the window is narrow, but nothing in the kernel closes it.

## Proposed interface

### 1. Event file identity in fdinfo

Both kinds print, after the existing `ctxn:` line:

```
ibdev_index:	3
ibdev:	mlx5_2
type:	comp_channel            (or async_event)
event_file_id:	2
default:	1                   (async_event only)
events_queued:	0
```

- `ctxn` is the context's restrack ID, which is unique per device
  only, so a process with contexts on two devices can have two event
  files with the same `ctxn`. A uverbs cdev FD names its device through
  the cdev's `rdev`, but an anon event file has none. `ibdev_index`
  (`ib_device.index`) names it: it is the device's netlink index and
  does not change when the device is renamed. `ibdev` is the current
  name, for people reading it. The uverbs cdev's fdinfo prints both
  lines too, so the pair (`ibdev_index`, `ctxn`) identifies a context
  the same way from either kind of FD. Neither is printed once the
  device is disassociated, like `ctxn`.
- `event_file_id` is unique among the context's event files of both
  kinds. It is assigned at creation from a per-`ib_uverbs_file` counter
  starting at 1, is never reused, and stays the same across `dup`,
  `fork`, `SCM_RIGHTS` and FD moves, since it belongs to the file rather
  than an FD. On the destination it is only a lookup key: a restored
  file gets a new ID, and CRIU maps the image ID to the restored FD.
- `default` is 1 when the file is `ufile->default_async_file`.
- `events_queued` is the length of `ev_queue->event_list`, read under
  `ev_queue->lock`. It is a snapshot: an event can be queued right
  after the read. It is meant for diagnostics and for checking a peek
  (see "Events queued at dump"), not as a gate on its own.

This is RDMA core only, in `uverbs_main.c`. It adds no generic procfs
or MM changes.

Implementation:

- `struct ib_uverbs_event_queue` gets a `u32 id`.
- `struct ib_uverbs_file` gets an `atomic_t event_file_ids`.
- `ib_uverbs_init_event_queue()` takes the `ib_uverbs_file` and assigns
  the ID. Its two callers are `ib_uverbs_create_comp_channel()` and
  `ib_uverbs_init_async_event_file()`.
- Wrap-around after 2^32 creations in one context is accepted; restore
  refuses an image with duplicate IDs.

### 2. Per-object event state on dump

Dump-side methods are appended to `enum uverbs_methods_restore`, after
RESTORE_QP, as `UVERBS_METHOD_CHECKPOINT_*`. They live in their own
file, `uverbs_std_types_checkpoint.c`, which adds them to the RESTORE
object's method tree. Unlike the RESTORE methods they do not call
`restore_check_ucontext()`: they run on the live source context, and
only read.

A new object is not possible. The uverbs API key gives objects 5 bits:
21 core IDs (`UVERBS_API_OBJ_KEY_NUM_CORE`) and 11 driver IDs. RESTORE
takes the last core ID and mlx5 uses all 11 driver IDs, so a 22nd core
object fails `uapi_key_obj()`, and with it every device's uverbs
registration. Adding the methods to `UVERBS_OBJECT_CQ`, `_DEVICE` or
`_ASYNC_EVENT` instead would interleave out-of-tree method IDs with
upstream's, which an OFED build cannot afford. RESTORE is already
out-of-tree, so every out-of-tree method ID stays in one block.

The first method is `UVERBS_METHOD_CHECKPOINT_QUERY_EVENTS`:

| attr                    | dir | type                           | notes                        |
|-------------------------|-----|--------------------------------|------------------------------|
| `..._CQ_HANDLE`         | in  | IDR CQ, READ, optional         | exactly one of CQ / QP       |
| `..._QP_HANDLE`         | in  | IDR QP, READ, optional         |                              |
| `..._RESP`              | out | `struct ib_uverbs_uobj_events` | mandatory                    |

```c
struct ib_uverbs_uobj_events {
	__aligned_u64 user_handle;
	__u32 async_file_id;		/* 0: none */
	__u32 comp_channel_id;		/* CQ only; 0: none */
	__u32 async_events_reported;
	__u32 comp_events_reported;	/* CQ only */
	__u32 async_events_queued;	/* this object's, on its async file */
	__u32 comp_events_queued;	/* CQ only, on its channel */
};
```

- Passing neither handle or both returns `-EINVAL`. An SRQ handle is
  added with RESTORE_SRQ (see "SRQs and WQs").
- The READ lock on the uobject keeps the object from being destroyed
  during the call. The counters and queue lengths are read under the
  owning file's `ev_queue->lock`, because `ib_uverbs_event_read()`
  changes them under that lock.
- This covers the hand-off's requests 2, 3 and 4 in one place: core owns
  all these fields, so no new exports are needed and vfmig QUERY_CQ and
  QUERY_QP stay as they are. vfmig QUERY_QP's `RESP_USER_HANDLE` becomes
  redundant but stays for compatibility.
- An ID that no FD in the dumped tree carries means the file was closed
  while an object, or the context's default, still holds it. CRIU
  refuses the dump. See "Event files with no FD" for why that is the
  right default.
- An `async_file_id` of 0 means the object was created with no
  `EVENT_FD` while the context had no default. rdma-core never does
  that, and RESTORE_CQ and RESTORE_QP cannot reproduce it (see section
  3), so CRIU refuses the dump.

The second method is `UVERBS_METHOD_CHECKPOINT_QUERY_DEFAULT_ASYNC`:

| attr                          | dir | type    | notes                        |
|-------------------------------|-----|---------|------------------------------|
| `..._ID`                      | out | `__u32` | 0: the context has no default |

fdinfo's `default:` line finds the default only if an FD to it is in
the dumped tree. This method also finds a default that no FD carries,
which CRIU must see in order to refuse the dump, and a context with no
default at all, which is legal (no object fell back to it). It is a new
method rather than an attribute on the upstream
`UVERBS_METHOD_QUERY_CONTEXT`, so that no out-of-tree attribute ID sits
beside upstream's, and because QUERY_CONTEXT fails with `-EOPNOTSUPP`
on every driver but mlx5, which alone has `query_ucontext`.

### 3. RESTORE_CQ and RESTORE_QP inputs

- **`UVERBS_ATTR_RESTORE_CQ_COMP_CHANNEL`.** Handled as
  `UVERBS_METHOD_CQ_CREATE` handles it (`uverbs_std_types_cq.c`
  100-129): take a reference on the channel uobject, and set
  `cq->cq_context = &ev_file->ev_queue` before `ops.restore_cq`. Drop
  the reference on the error paths. The FD lookup already refuses a
  channel from another context (`lookup_get_fd_uobject()` compares
  `uobject->ufile`). A restored CQ is freed through `uverbs_free_cq()`,
  which releases the channel through `cq_context` as for any CQ.
- **New `UVERBS_ATTR_RESTORE_CQ_COMP_EVENTS_REPORTED` and
  `UVERBS_ATTR_RESTORE_CQ_ASYNC_EVENTS_REPORTED`,** `__u32`, optional
  (default 0). They seed `comp_events_reported` and
  `uevent.events_reported`.
- **New `UVERBS_ATTR_RESTORE_CQ_COMP_EVENTS_QUEUED`,** `__u32`,
  optional (default 0), requires `COMP_CHANNEL`. The kernel queues that
  many completion events for this CQ on the channel, as
  `ib_uverbs_comp_handler()` would: `element` is the restored CQ's
  `user_handle`, and the counter is its `comp_events_reported`. It does
  this before `ops.restore_cq`, so the events are queued ahead of any
  the destination raises for this CQ, including the early-EQE replay of
  section 5. CRIU passes `comp_events_queued` from
  CHECKPOINT_QUERY_EVENTS. See "Events queued at dump".
- **New `UVERBS_ATTR_RESTORE_QP_ASYNC_EVENTS_REPORTED`,** `__u32`,
  optional. It seeds `uevent.events_reported`.

The counters seed to the values reported at dump. Since a counter
advances only when an event is read, events queued at dump are not in
it, and no correction is needed for them: re-queued events advance it
when the process reads them, as on the source.

**No silent fallback in restore mode.** Without `EVENT_FD`, the create
verbs bind an object to `ufile->default_async_file`, or to nothing if
the context has none yet. On restore that is a trap: if CRIU restores a
CQ before it allocates the image's default async file, the CQ ends up
with no async file, its async events are dropped, and nothing reports
it. So in a restore-mode context, RESTORE_CQ and RESTORE_QP without
`EVENT_FD` fail with `-EINVAL` while the context has no default async
file. With a default they use it, as the create verbs do. The source
state this refuses, an object with no async file, is already refused at
dump (section 2).

No `RESTORE_COMP_CHANNEL` method: the plugin creates channels on the
restore context with the ordinary write command and passes the FD.

### 4. Default async file

No kernel change. There are two ways to open a context on a uverbs FD,
and they differ only in the async file:

- **Write command `IB_USER_VERBS_CMD_GET_CONTEXT`** (`uverbs_cmd.c`,
  `ib_uverbs_get_context()`). The request is `struct
  ib_uverbs_get_context` with the driver's request appended, and the
  response is `struct ib_uverbs_get_context_resp` (`async_fd`,
  `num_comp_vectors`) with the driver's response appended. It calls the
  driver's `alloc_ucontext`, then creates an async file, which becomes
  the default, and returns its FD in `async_fd`.
- **Ioctl `UVERBS_METHOD_GET_CONTEXT`** (`uverbs_std_types_device.c`).
  The attributes are `NUM_COMP_VECTORS` and `CORE_SUPPORT` out,
  optional `FD_ARR` in (ucaps), and the driver request and response as
  UHW in and out. It calls the same `ib_alloc_ucontext()` and
  `ib_init_ucontext()`, and creates no async file. The first
  `ASYNC_EVENT_ALLOC` on the context then becomes the default.

mlx5's `alloc_ucontext` reads the request and writes the response
through `udata` and decides the layout from `udata->inlen` and
`udata->outlen` alone (`main.c` 2262-2269, 2406), so it sees the same
thing on both paths. Its VFMIG_RESTORE and VFMIG_ADOPT_TD branches read
only `req.flags`, `req.vfmig_tdn` and `req.vfmig_td_uid` from the copied
request. A restore-mode open with `MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE` in
`mlx5_ib_alloc_ucontext_req_v2` therefore works unchanged through the
ioctl. One mlx5 rule matters for several contexts on one VF: once a
restore-mode context from a LOAD has been destroyed, mlx5 refuses
further restore-mode contexts for that LOAD (`main.c` 2292-2303). CRIU
must open all of a VF's restore contexts before closing any.

What changes is in the plugin (`vfmig_send_get_context_v2()` in
`vfmig_uverbs.c`):

- it builds an ioctl with the same driver request as UHW in and
  `mlx5_ib_alloc_ucontext_resp` as UHW out, instead of packing a write
  command;
- it reads `num_comp_vectors` from its attribute rather than the write
  response;
- it no longer receives an `async_fd` to close.

It then allocates the image's default async file first, with
`ASYNC_EVENT_ALLOC`, so that file becomes the default.

This is also what the source process did, if it used rdma-core: its
`ibv_cmd_get_context()` uses the ioctl when the kernel has it and then
allocates `context->async_fd` with `ASYNC_EVENT_ALLOC`. On an older
rdma-core that used the write command, the write response's `async_fd`
is `context->async_fd`. Either way the default is the process's own
async FD, and fdinfo's `default:` line reports it, so CRIU does not need
to know which path the source took. The rdma-core behaviour should be
confirmed with `strace` of a v39 process on the test hosts.

Still to check on the CRIU side: whether anything else relies on the
write path, for example core CRIU code that expects the extra async FD
or parses the write response.

Alternative if that is not acceptable: a flag on `ASYNC_EVENT_ALLOC`, in
a restore-mode context only, that replaces the default. That is more
code in core and needs a rule for objects already bound to the old
default.

### 5. Completion EQEs that arrive before RESTORE_CQ (mlx5)

When the VF probe applies LOAD, `mlx5_vfmig_vf_record_restored_cqs()`
stores the cqn of every user CQ that LOAD brought back in
`priv.vfmig_unadopted_cqs`, an xarray with IRQ-safe locking. This runs
before the completion EQs exist. `mlx5_eq_comp_int()`'s miss branch
sets `XA_MARK_0` on a stored cqn. `mlx5_core_adopt_cq()` erases the
cqn after registering the CQ, and if it was marked, calls
`cq->comp(cq, NULL)` once. `mlx5_add_cq_to_tasklet()` does not use the
EQE.

- The interrupt handler never allocates: the entries exist from probe,
  and it only sets a mark. For a cqn that is not stored, the miss branch
  logs "bogus CQ" as before.
- An EQE that races with the adoption can produce one extra event. An
  extra completion event is harmless to a correct rdma-core user, who
  polls the CQ and re-arms; a lost one is not.
- Async CQ and QP events in the same window are rarer: CQ overrun, QP
  fatal, communication established. They are dropped where the lookup
  misses: `cq_err_event_notifier()` in mlx5_core's `eq.c` for CQs, and
  `rsc_event_notifier()` in mlx5_ib's `qpc.c` for QPs. Both now warn,
  so a loss is visible; replaying them is phase 3.

#### Which cqns are recorded, and for how long

The kernel does not see the restore "finish": each RESTORE_CQ is one
call, and the kernel does not know how many will come. Rather than add
a "restore done" call the plugin must make on every path, including
failed restores, the record is bounded by what LOAD brought back:

- **Scope.** LOAD installs an awaiting-bind placeholder in the vfmig
  IOVA domain for each source user CQ, keyed by `KIND_CQ` and cqn. Only
  those cqns are stored. Source kernel CQs that firmware still holds
  but nobody restores (`restored_state_growth.md` lists mlx5e's drop-RQ
  CQ and the GSI and MAD CQs) are never recorded, so they cannot leave
  stale entries.
- **Lifetime of an entry.** `mlx5_core_adopt_cq()` erases its cqn,
  replaying the event if it was marked. Once every placeholder CQ has
  been adopted, the record is empty and the miss branch returns after
  an `xa_empty()` check.
- **Leftovers.** A cqn stored but never adopted belongs to a user CQ the
  restore skipped or failed on. `mlx5_mdev_uninit()`, at VF unbind,
  frees the record. It warns with the number of CQs never restored, and
  names each one whose event was lost. A later LOAD or recycle needs the
  VF unbound, so it always starts from an empty record. A devlink reload
  keeps the mdev and does not repeat the LOAD, so the record stays.
- **Reuse.** Only `mlx5_core_adopt_cq()` consults the record, never CQ
  create, so a leftover entry cannot give a spurious event to a new CQ
  that reuses the cqn. Leaving the record on is therefore cheap: a miss
  costs one lock and lookup, and only while unadopted cqns remain.

An earlier draft turned the record off at the destination's RX fence
lift, at RESUME_IBDEV. That ties mlx5_core's EQ code to a plugin step
and gains nothing once the record is scoped to LOAD's CQs.

#### Rejected alternative

Replaying an event whenever the adopted CQ's `cqc.st` reads "fired"
needs a firmware query per CQ. It also cannot tell an
event raised and read on the source from one dropped on the
destination, so it would add spurious events to every CQ that fired
before the dump.

## Events queued at dump

A queued event is one the kernel has delivered to the file but the
process has not read. Losing it is worse than losing a packet: an armed
CQ fires once, so a lost completion event can leave the process asleep
forever, and a lost QP error leaves it waiting on a dead QP. So the
events are buffered in the image and re-queued on restore.

### When to take the snapshot

Events keep arriving while the process is frozen, because the hardware
and the kernel queue them, not the process. A snapshot is complete only
once nothing more can arrive for the file. The sources are:

| source                                    | stops at                                            |
|-------------------------------------------|-----------------------------------------------------|
| completions, CQ and QP async events       | the VF's SUSPEND for SAVE: no more EQEs             |
| device and port events (port state, GID or LID change, client reregister) | never, until the source process is gone |

The snapshot is therefore taken after SUSPEND. Device and port events
that arrive after it are lost. That is acceptable, since they describe
the source host's port, and the restored file gets the destination
device's events from then on. Events taken in the snapshot are not
removed from the source, so a dump that fails afterwards resumes the
process with its queues intact.

`events_queued` in fdinfo is racy on its own for the same reason, which
is why it is diagnostic only.

### Completion events: a count per CQ

A queued completion event carries nothing but its CQ: `element` is the
CQ's `user_handle`, and the counter is the CQ's. So the per-CQ
`comp_events_queued` from CHECKPOINT_QUERY_EVENTS, read after SUSPEND,
is all the image needs, and RESTORE_CQ's `COMP_EVENTS_QUEUED` rebuilds
the events (section 3). No peek or re-queue method is needed for them.

What this loses is the order between CQs on one channel: on restore,
each CQ's events are queued when that CQ is restored, so they follow
RESTORE_CQ order rather than the source's arrival order. A process
cannot rely on that order, since completion events for different CQs
race in the hardware anyway. Within one CQ order does not arise, as its
events are identical.

Events for one CQ come out ahead of any the destination raises for that
CQ, because RESTORE_CQ queues them before `ops.restore_cq` makes the CQ
live. Another CQ's live event can still be ahead of them on a shared
channel, which is the same cross-CQ reordering.

### Async events: peek

Async events differ by type and object, so they are carried as a list.

- **`UVERBS_METHOD_CHECKPOINT_PEEK_EVENTS`.** It takes one async event
  FD and returns that file's queued events in FIFO order without
  dequeuing them. It refuses a completion channel.

  ```c
  struct ib_uverbs_queued_event {
  	__aligned_u64 element;	/* as read() returns it */
  	__u32 event_type;
  	__u32 obj_type;		/* UVERBS_OBJECT_CQ/QP, 0: device/port */
  	__u32 obj_handle;	/* the object's handle in the context */
  	__u32 reserved;
  };
  ```

  The output array and a `__u64` total follow `INFO_HANDLES`: the
  kernel fills as many entries as fit and returns the full count, and
  CRIU retries with a larger buffer if the count is bigger. Entries are
  copied to a kernel buffer under `ev_queue->lock`, then to userspace.
- **Finding the object.** The kernel derives `obj_type` and
  `obj_handle` from the event. `event->counter` is
  `&ib_uevent_object.events_reported`, and the uobject's `uapi_object`
  gives the type. A NULL counter marks a device or port event. The
  object cannot go away during the peek: destroying it removes its
  events under the same `ev_queue->lock` (`ib_uverbs_release_uevent()`).
- **Bounds.** The queue has no kernel limit: each event is a
  `GFP_ATOMIC` allocation. In practice async events are rare. The peek
  has no limit either, and the image stores what it returns.

### Async events: re-queue

`UVERBS_METHOD_RESTORE_EVENTS`, in the RESTORE namespace and restore
mode only. It takes the restored async event FD and an array of the
same entries, after the objects are restored and before the process
runs.

- For each entry it looks up the object by handle and queues the event
  on the file and on the object's list, with the object's counter, as
  the handlers do. Restored IDR objects keep their source handles
  (`rdma_alloc_begin_uobject_at_handle()`), so the source handle names
  the restored object.
- It refuses an entry whose object uses a different async file than the
  one passed.
- **Order.** The buffered events are older than anything the
  destination raises, and a restored CQ or QP is live from its RESTORE
  call, so live events can already be queued when RESTORE_EVENTS runs.
  RESTORE_EVENTS therefore inserts the buffered events at the head of
  the queue, in their original order, ahead of anything already there.
- **Device and port events** are re-queued as they were, with a NULL
  counter. Dropping them could hang an application waiting for
  `IB_EVENT_PORT_ACTIVE`. One that is stale on the destination, such as
  a port-down for a port that is up, is followed by the destination's
  own events.

## Phasing

Phase 1 is everything a restored process needs to use channels and
async files under traffic:

- fdinfo identity (section 1);
- CHECKPOINT_QUERY_EVENTS and CHECKPOINT_QUERY_DEFAULT_ASYNC (section 2);
- RESTORE_CQ's `COMP_CHANNEL`, counters and `COMP_EVENTS_QUEUED`,
  RESTORE_QP's counter, and the `EVENT_FD` check (section 3);
- the mlx5 replay of early completion EQEs (section 5).

Phase 2 is CHECKPOINT_PEEK_EVENTS and RESTORE_EVENTS for async events.
Until it lands, CRIU refuses a dump in which any async file has queued
events, naming the file and the count.

Phase 3 replays async CQ and QP events that firmware raises on the
destination before RESTORE_CQ or RESTORE_QP registers the object
(section 5). The record extends the completion one with the event type,
and is scoped and cleared the same way. It is the last phase because
these events need a hardware fault, a CQ overrun or a state change
that the RX fence prevents; an RC timeout in the window raises none
(section "mlx5 CQs across LOAD").

The end of phase 1 is the point to hand the kernel ABI to CRIU. The
plugin's switch to the ioctl GET_CONTEXT (section 4) needs no kernel
change and can start before it.

## Event files with no FD

An event file can outlive every FD to it, because the kernel holds its
own references:

- a CQ holds its completion channel (`cq_context`);
- a CQ, QP or SRQ holds its async file (`uevent.event_file`);
- the context holds its default async file (`default_async_file`).

If the process closes the FD, the file stays alive, and its events
still queue, but nothing can ever read them. The queue then grows
without bound, one allocation per event.

This is not a legitimate use:

- rdma-core keeps `context->async_fd` open until `ibv_close_device()`.
- `ibv_destroy_comp_channel()` returns `EBUSY` while a CQ still uses the
  channel.
- An application using rdma-core cannot reach this state without
  closing FDs it does not own, which is a bug, for example a daemon
  that closes every FD after `fork()` while a library still uses them.

The one real producer today is the plugin itself. Its write-path
GET_CONTEXT creates a default async file and closes the FD, so every
restored context carries a hidden default file, and a restored process
dumped again would hit this case. Opening the restore context with the
ioctl (section 4) removes that.

So CRIU refuses the dump when an object or the context's default names
an event file ID that no FD in the dumped tree carries. The error names
the file ID and the objects that use it. Supporting it would mean
restoring a file only to close it, to reproduce a state that loses
events by construction.

## SRQs and WQs

Four kinds of uverbs object have an `ib_uevent_object`: CQs, QPs, SRQs
and WQs. Each one gets async events on its async file and has an
`events_reported` counter that its destroy waits on. Only CQs get
completion events, so only CQs use completion channels.

- **SRQ (shared receive queue).** One receive queue that several QPs
  (RC, UD, XRC) take receive buffers from. Its events are
  `IB_EVENT_SRQ_LIMIT_REACHED`, raised once when the number of posted
  buffers falls below a limit set with `ibv_modify_srq()`, and
  `IB_EVENT_SRQ_ERR`. QPs attached to an SRQ also get
  `IB_EVENT_QP_LAST_WQE_REACHED`. `ibv_destroy_srq()` waits on its
  counter like the CQ and QP destroys. RESTORE_SRQ therefore needs the
  same three things as RESTORE_QP: `EVENT_FD`, a seeded
  `ASYNC_EVENTS_REPORTED`, and an SRQ handle in CHECKPOINT_QUERY_EVENTS.
  The limit is armed once and disarmed when it fires, so whether it is
  armed has to survive the migration too. It is in the firmware SRQ
  context (`lwm`) and should travel through SAVE/LOAD like a CQ's arm
  state; the same harness can check it.
- **WQ (work queue).** Not part of RC. `ibv_create_wq()` creates a
  standalone receive queue for receive-side scaling: several WQs behind
  an indirection table (`RWQ_IND_TBL`) feeding a raw packet QP, as
  DPDK-style Ethernet receive uses. Its event is `IB_EVENT_WQ_FATAL`.
  It matters only if raw packet QPs are restored, which is not planned,
  so it is out of scope here.

Until RESTORE_SRQ exists, CRIU refuses to dump a context that has an
SRQ or a WQ, naming the object. RESTORE_QP already rejects a QP on an
SRQ, but refusing at dump fails before SAVE, with a clearer reason. The
refusal is lifted for SRQs when RESTORE_SRQ lands with the three
additions above.

## Harness: CQ arm state across LOAD

The design assumes firmware carries a CQ's arm state through SAVE/LOAD,
so that a CQ armed on the source raises an event for its next
completion on the destination. This can be checked now, before any of
the kernel changes above, because CQ restore already works without a
channel.

The arm state is readable without new verbs. mlx5 debugfs has a
directory per CQ, `/sys/kernel/debug/mlx5/<bdf>/CQs/<cqn>/`, and every
read of its fields issues a firmware QUERY_CQ (`cq_read_field()` in
`debugfs.c`). Adopted CQs are added to it too (`mlx5_core_adopt_cq()`
calls `mlx5_debug_cq_add()`). mlx5_core's debugfs now has a `state`
field that returns `cqc.st`.

`probes/cq_arm_state.sh` runs the test with `probes/cq_arm_holder`,
which creates CQs in these states and keeps them there until the dump:

| CQ | how                                                     | expected `st` |
|----|---------------------------------------------------------|---------------|
| A  | `ibv_req_notify_cq(cq, 0)`, no completion               | 9 (armed)     |
| B  | `ibv_req_notify_cq(cq, 1)`, no completion               | 6 (solicited) |
| C  | armed, then one completion, not re-armed                | 0xa (fired)   |
| D  | never armed                                             | as read on the source |

The CQs have no completion channel: arming is firmware state, and the
kernel drops the event of a CQ without one. C is fired by a self-loop
RC QP that the holder destroys again with its PD, so the image holds
only the context and the four CQs.

The script reads `state` for each cqn on the source before the dump,
and on the destination after restore, before any traffic. It passes if
they match. A mismatch on A or B would mean a restored process waiting
on that CQ never wakes, and the kernel would have to re-arm at
RESTORE_CQ from a value carried in the image.

Result (CX-7, 2026-11-23), VF 0 to VF 1 and VF 0 to the same slot:
every CQ reads the same on both sides, A 0x9, B 0x6, C 0xa, and the
restored holder polls all four. A CQ that was never armed reads 0xa,
the same as one that fired and was not re-armed: firmware creates CQs
unarmed, in the fired state.

Once RESTORE_CQ takes a channel, the functional version follows: arm
CQ A on a channel, migrate, send one message to it, and expect
`ibv_get_cq_event()` to return A.

## Restore order for CRIU

On dump, after the VF is suspended for SAVE:

1. fdinfo of every event file: device, context, type, ID, default.
2. CHECKPOINT_QUERY_DEFAULT_ASYNC for each context: the default async file
   ID.
3. CHECKPOINT_QUERY_EVENTS for every CQ and QP.
4. CHECKPOINT_PEEK_EVENTS for every async file.

All four are software-only, so they work on a suspended VF. CRIU
refuses the dump if any of these hold:

- the default, or a file an object uses, has no FD in the dumped tree;
- an object has `async_file_id` 0;
- the context has an SRQ or a WQ;
- an async file has queued events and the peek is not available yet.

On restore, for each context:

1. Open the context with the ioctl `GET_CONTEXT` (VFMIG_RESTORE UHW),
   then RESTORE_UCONTEXT. Open every restore context of a VF before
   closing any (section 4).
2. Allocate the image's default async file first, then the other async
   files, then the completion channels. Keep a map from image file ID to
   new FD. The kernel refuses a RESTORE_CQ or RESTORE_QP without
   `EVENT_FD` until the default exists.
3. RESTORE_PD, MR, CQ (with `COMP_CHANNEL`, `EVENT_FD`, the counters
   and `COMP_EVENTS_QUEUED`), QP (with `EVENT_FD` and the counter).
4. RESTORE_EVENTS for every async file with buffered events.
5. Install the FDs at the process's numbers.
6. RESUME_IBDEV lifts the RX fence.

## Validation

A probe in `tools/testing/criu_rdma/probes`, without migration:

- the fdinfo `type`, `event_file_id` and `default` of a channel and two
  async files, checked again after `dup2`, after `fork`, and after
  passing the FD through a socket;
- QUERY_EVENTS on a CQ bound to a channel and an explicit async file,
  and on a QP using the default, comparing IDs to fdinfo;
- QUERY_DEFAULT_ASYNC on a context opened with the write GET_CONTEXT (default
  is its `async_fd`), one opened with the ioctl and no async file (0),
  and one whose default FD was closed (an ID no FD carries);
- the counters before and after the process reads an armed completion
  and an async event (`ibv_ack_cq_events` does not change them);
- `events_queued` and `comp_events_queued` with one unread completion;
- CHECKPOINT_PEEK_EVENTS with an unread QP event and a port event
  queued: the entries in order, each with the right object, and the
  queue still intact afterwards (the process then reads the same
  events);
- negative cases: neither or both handles, a channel FD passed to
  PEEK_EVENTS, a channel FD from another context in RESTORE_CQ,
  `COMP_EVENTS_QUEUED` without `COMP_CHANNEL`, RESTORE_CQ and
  RESTORE_QP without `EVENT_FD` before the default exists, seeding or
  RESTORE_EVENTS on a non-restore context, and RESTORE_EVENTS naming an
  object on another file.

`probes/event_files_probe` covers these except CHECKPOINT_PEEK_EVENTS,
RESTORE_EVENTS and seeding, which are phase 2. It passed on CX-7
(mlx5_0 and mlx5_1, the completion event through mlx5_1's GID 0) and on
rxe over the same host's eth3, which also takes the restore-mode
refusals (2026-11-24).

End-to-end, in the CRIU gates:

- a holder arms a CQ on a channel, and after restore receives the event
  with `ibv_get_cq_event()` and acks it;
- an async event, such as a QP moved to error, reaches the restored FD;
- `ibv_destroy_cq` and `ibv_destroy_qp` return after restore on objects
  that had events before the dump;
- a completion event left unread at dump is the first event
  `ibv_get_cq_event()` returns for that CQ after restore, ahead of one
  raised on the destination;
- a dump that fails after the queries resumes the process, which then
  reads the events it had queued;
- the early-EQE replay, in `probes/cq_early_replay.sh`: an armed CQ whose
  only completion is an RC send that a self-loop QP retries against its
  own RNR NAKs. Behind the destination's fence the loopback is dropped,
  so the send fails on its ACK timeouts after LOAD and before
  RESTORE_CQ. `mlx5_core_adopt_cq()`'s debug line must name that CQ and
  not an armed CQ without traffic, the restored process must find the
  error CQE, and VF unbind afterwards must warn of no leftover cqns.
  Until CRIU restores completion channels this checks the replay in
  mlx5_core only; with them, the event must also reach the restored
  channel;
- refusals: a dump with an SRQ, and one whose default async FD the
  holder closed, fail before SAVE with the reason named.

## Open questions

- **rdma-core's context open.** Confirm with `strace` that v39 uses the
  ioctl GET_CONTEXT and `ASYNC_EVENT_ALLOC`, and check that nothing in
  CRIU relies on the write path's extra async FD.
- **SRQ limit.** Confirm that an SRQ's armed limit survives SAVE/LOAD,
  with the same harness as the CQ arm state, once RESTORE_SRQ exists.

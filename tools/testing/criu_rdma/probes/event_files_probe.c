// SPDX-License-Identifier: GPL-2.0
/*
 * Checks the event file interface for CRIU without a migration.
 *
 *   event_files_probe <ibdev> [sgid_index]
 *
 * - fdinfo of the uverbs FD, a completion channel and two async event
 *   files, again after dup(), in a forked child and after SCM_RIGHTS;
 * - CHECKPOINT_QUERY_EVENTS on a CQ with a channel and the default async
 *   file, a CQ with the second async file, and a QP, against fdinfo;
 * - CHECKPOINT_QUERY_DEFAULT_ASYNC on a context from the write GET_CONTEXT,
 *   before and after its async FD is closed, on one from the ioctl
 *   GET_CONTEXT before and after ASYNC_EVENT_ALLOC, and on an FD with no
 *   context;
 * - the counters around a completion event (a self-loop RC SEND through
 *   GID <sgid_index>; skipped without it) and a QP async event
 *   (LAST_WQE_REACHED from a QP on an SRQ moved to error);
 * - refusals: QUERY_EVENTS with neither handle or both, RESTORE_CQ on a
 *   context not in restore mode, and a channel from another context. On
 *   rxe a restore-mode context also checks RESTORE_CQ and RESTORE_QP
 *   without EVENT_FD before and after a default async file exists, and
 *   COMP_EVENTS_QUEUED with and without COMP_CHANNEL.
 *
 * Every ioctl carries the driver's ID, and contexts and CQs made by hand
 * need driver data, so it runs on mlx5 and rxe only. Prints PASS, FAIL
 * or SKIP per check and exits 1 if
 * any check failed.
 */
#define _GNU_SOURCE
/* Ahead of verbs.h, so that the system's older copy is not used. */
#include "../../../../include/uapi/rdma/ib_user_ioctl_cmds.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include <infiniband/verbs.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/rdma_user_ioctl_cmds.h>

/* include/uapi/rdma/ib_user_ioctl_verbs.h */
struct uobj_events {
	uint64_t user_handle;
	uint32_t async_file_id;
	uint32_t comp_channel_id;
	uint32_t async_events_reported;
	uint32_t comp_events_reported;
	uint32_t async_events_queued;
	uint32_t comp_events_queued;
};
_Static_assert(sizeof(struct uobj_events) == 32, "struct ib_uverbs_uobj_events");

/* struct mlx5_ib_alloc_ucontext_req_v2, up to lib_caps */
struct mlx5_uctx_req {
	uint32_t total_num_bfregs;
	uint32_t num_low_latency_bfregs;
	uint32_t flags;
	uint32_t comp_mask;
	uint8_t max_cqe_version;
	uint8_t reserved0;
	uint16_t reserved1;
	uint32_t reserved2;
	uint64_t lib_caps;
};

/* struct mlx5_ib_create_cq */
struct mlx5_cq_req {
	uint64_t buf_addr;
	uint64_t db_addr;
	uint32_t cqe_size;
	uint8_t cqe_comp_en;
	uint8_t cqe_comp_res_format;
	uint16_t flags;
	uint16_t uar_page_index;
	uint16_t reserved0;
	uint32_t reserved1;
};

/* struct rxe_alloc_ucontext_req */
struct rxe_uctx_req {
	uint32_t flags;
	uint32_t reserved;
};
#define RXE_RESTORE_MODE (1u << 0)

#define QPT_RC		2	/* enum ib_qp_type */
#define QPS_RESET	0	/* enum ib_qp_state */

enum { DRV_OTHER, DRV_MLX5, DRV_RXE };

static const char *ibdev;
static int drv;
static int failed;

static void report(const char *tag, const char *fmt, va_list ap)
{
	printf("%s ", tag);
	vprintf(fmt, ap);
	putchar('\n');
	fflush(stdout);
}

static int check(int ok, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	report(ok ? "PASS" : "FAIL", fmt, ap);
	va_end(ap);
	if (!ok)
		failed = 1;
	return ok;
}

static void skip(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	report("SKIP", fmt, ap);
	va_end(ap);
}

/* Copies the value of "<key>:" in @fd's fdinfo to @buf; -1 if absent. */
static int fdinfo(int fd, const char *key, char *buf, size_t len)
{
	size_t klen = strlen(key);
	char path[64], line[256], *v;
	int ret = -1;
	FILE *f;

	snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
	f = fopen(path, "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, key, klen) || line[klen] != ':')
			continue;
		v = line + klen + 1;
		v += strspn(v, " \t");
		v[strcspn(v, "\n")] = 0;
		snprintf(buf, len, "%s", v);
		ret = 0;
		break;
	}
	fclose(f);
	return ret;
}

/* A numeric fdinfo value, or -1 if absent. */
static long long fdnum(int fd, const char *key)
{
	char buf[64];

	if (fdinfo(fd, key, buf, sizeof(buf)))
		return -1;
	return strtoll(buf, NULL, 0);
}

struct call {
	struct ib_uverbs_ioctl_hdr hdr;
	struct ib_uverbs_attr attrs[16];
};

/* The kernel refuses any method, core ones too, if driver_id differs. */
static void call_init(struct call *c, uint16_t object, uint16_t method)
{
	memset(c, 0, sizeof(*c));
	c->hdr.object_id = object;
	c->hdr.method_id = method;
	if (drv == DRV_MLX5)
		c->hdr.driver_id = RDMA_DRIVER_MLX5;
	else if (drv == DRV_RXE)
		c->hdr.driver_id = RDMA_DRIVER_RXE;
}

static struct ib_uverbs_attr *attr(struct call *c, uint16_t id, uint16_t len)
{
	struct ib_uverbs_attr *a = &c->attrs[c->hdr.num_attrs++];

	a->attr_id = id;
	a->len = len;
	a->flags = UVERBS_ATTR_F_MANDATORY;
	return a;
}

/* IDR and FD attributes carry the handle or FD itself. */
static void attr_handle(struct call *c, uint16_t id, uint64_t handle)
{
	attr(c, id, 0)->data = handle;
}

/* Inputs of up to 8 bytes travel inline. */
static void attr_in(struct call *c, uint16_t id, const void *p, uint16_t len)
{
	struct ib_uverbs_attr *a = attr(c, id, len);

	if (len <= sizeof(a->data))
		memcpy(&a->data, p, len);
	else
		a->data = (uintptr_t)p;
}

static void attr_out(struct call *c, uint16_t id, void *p, uint16_t len)
{
	attr(c, id, len)->data = (uintptr_t)p;
}

static int call_do(int fd, struct call *c)
{
	c->hdr.length = sizeof(c->hdr) + c->hdr.num_attrs * sizeof(c->attrs[0]);
	return ioctl(fd, RDMA_VERBS_IOCTL, c) ? -errno : 0;
}

/* @cq and @qp are handles, or -1 to leave the attribute out. */
static int query_events(int fd, int64_t cq, int64_t qp, struct uobj_events *ev)
{
	struct call c;

	memset(ev, 0xff, sizeof(*ev));
	call_init(&c, UVERBS_OBJECT_RESTORE, UVERBS_METHOD_CHECKPOINT_QUERY_EVENTS);
	if (cq >= 0)
		attr_handle(&c, UVERBS_ATTR_CHECKPOINT_QUERY_EVENTS_CQ_HANDLE, cq);
	if (qp >= 0)
		attr_handle(&c, UVERBS_ATTR_CHECKPOINT_QUERY_EVENTS_QP_HANDLE, qp);
	attr_out(&c, UVERBS_ATTR_CHECKPOINT_QUERY_EVENTS_RESP, ev, sizeof(*ev));
	return call_do(fd, &c);
}

static int query_default_async(int fd, uint32_t *id)
{
	struct call c;

	*id = UINT32_MAX;
	call_init(&c, UVERBS_OBJECT_RESTORE, UVERBS_METHOD_CHECKPOINT_QUERY_DEFAULT_ASYNC);
	attr_out(&c, UVERBS_ATTR_CHECKPOINT_QUERY_DEFAULT_ASYNC_ID, id, sizeof(*id));
	return call_do(fd, &c);
}

/* Returns the new async event FD, or -errno. */
static int async_alloc(int fd)
{
	struct call c;
	int ret;

	call_init(&c, UVERBS_OBJECT_ASYNC_EVENT, UVERBS_METHOD_ASYNC_EVENT_ALLOC);
	attr_handle(&c, UVERBS_ATTR_ASYNC_EVENT_ALLOC_FD_HANDLE, 0);
	ret = call_do(fd, &c);
	return ret ? ret : (int)c.attrs[0].data;
}

static size_t uctx_req(void *buf, int restore)
{
	struct mlx5_uctx_req m = { .total_num_bfregs = 4, .max_cqe_version = 1 };
	struct rxe_uctx_req r = { .flags = restore ? RXE_RESTORE_MODE : 0 };

	if (drv == DRV_MLX5) {
		memcpy(buf, &m, sizeof(m));
		return sizeof(m);
	}
	memcpy(buf, &r, sizeof(r));
	return sizeof(r);
}

struct write_get_context {
	struct ib_uverbs_cmd_hdr hdr;
	struct ib_uverbs_get_context core;
	uint8_t drv[sizeof(struct mlx5_uctx_req)];
};

static int write_get_context(int fd, int *async_fd)
{
	struct write_get_context cmd = {};
	struct {
		struct ib_uverbs_get_context_resp core;
		uint8_t drv[512];
	} resp = {};
	size_t len = offsetof(struct write_get_context, drv) + uctx_req(cmd.drv, 0);

	cmd.hdr.command = IB_USER_VERBS_CMD_GET_CONTEXT;
	cmd.hdr.in_words = len / 4;
	cmd.hdr.out_words = sizeof(resp) / 4;
	cmd.core.response = (uintptr_t)&resp;
	if (write(fd, &cmd, len) != (ssize_t)len)
		return -errno;
	*async_fd = resp.core.async_fd;
	return 0;
}

static int ioctl_get_context(int fd, int restore)
{
	uint8_t req[sizeof(struct mlx5_uctx_req)], resp[512];
	struct call c;

	call_init(&c, UVERBS_OBJECT_DEVICE, UVERBS_METHOD_GET_CONTEXT);
	attr_in(&c, UVERBS_ATTR_UHW_IN, req, uctx_req(req, restore));
	attr_out(&c, UVERBS_ATTR_UHW_OUT, resp, sizeof(resp));
	return call_do(fd, &c);
}

static int write_alloc_pd(int fd, uint32_t *handle)
{
	struct {
		struct ib_uverbs_cmd_hdr hdr;
		struct ib_uverbs_alloc_pd core;
	} cmd = {};
	struct {
		struct ib_uverbs_alloc_pd_resp core;
		uint8_t drv[64];
	} resp = {};

	cmd.hdr.command = IB_USER_VERBS_CMD_ALLOC_PD;
	cmd.hdr.in_words = sizeof(cmd) / 4;
	cmd.hdr.out_words = sizeof(resp) / 4;
	cmd.core.response = (uintptr_t)&resp;
	if (write(fd, &cmd, sizeof(cmd)) != sizeof(cmd))
		return -errno;
	*handle = resp.core.pd_handle;
	return 0;
}

/* Returns the channel FD, or -errno. */
static int write_comp_channel(int fd)
{
	struct {
		struct ib_uverbs_cmd_hdr hdr;
		struct ib_uverbs_create_comp_channel core;
	} cmd = {};
	struct ib_uverbs_create_comp_channel_resp resp = {};

	cmd.hdr.command = IB_USER_VERBS_CMD_CREATE_COMP_CHANNEL;
	cmd.hdr.in_words = sizeof(cmd) / 4;
	cmd.hdr.out_words = sizeof(resp) / 4;
	cmd.core.response = (uintptr_t)&resp;
	if (write(fd, &cmd, sizeof(cmd)) != sizeof(cmd))
		return -errno;
	return resp.fd;
}

/* A CQ with no channel, on @event_fd or, if -1, the default async file. */
static int create_cq(int fd, int event_fd, uint64_t user_handle, uint32_t *handle)
{
	uint32_t cqe = 15, vector = 0, resp_cqe;
	struct mlx5_cq_req m = { .cqe_size = 64 };
	uint8_t resp[64];
	struct call c;
	void *buf, *db;
	int ret;

	call_init(&c, UVERBS_OBJECT_CQ, UVERBS_METHOD_CQ_CREATE);
	attr_handle(&c, UVERBS_ATTR_CREATE_CQ_HANDLE, 0);
	attr_in(&c, UVERBS_ATTR_CREATE_CQ_CQE, &cqe, sizeof(cqe));
	attr_in(&c, UVERBS_ATTR_CREATE_CQ_USER_HANDLE, &user_handle, sizeof(user_handle));
	attr_in(&c, UVERBS_ATTR_CREATE_CQ_COMP_VECTOR, &vector, sizeof(vector));
	attr_out(&c, UVERBS_ATTR_CREATE_CQ_RESP_CQE, &resp_cqe, sizeof(resp_cqe));
	if (event_fd >= 0)
		attr_handle(&c, UVERBS_ATTR_CREATE_CQ_EVENT_FD, event_fd);
	if (drv == DRV_MLX5) {
		/* Left mapped: the CQ owns them until the context closes. */
		buf = mmap(NULL, 65536, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		db = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (buf == MAP_FAILED || db == MAP_FAILED)
			return -ENOMEM;
		m.buf_addr = (uintptr_t)buf;
		m.db_addr = (uintptr_t)db;
		attr_in(&c, UVERBS_ATTR_UHW_IN, &m, sizeof(m));
	}
	attr_out(&c, UVERBS_ATTR_UHW_OUT, resp, sizeof(resp));
	ret = call_do(fd, &c);
	*handle = c.attrs[0].data;
	return ret;
}

/* @channel and @event_fd are FDs, @reported and @queued counts, or -1. */
static int restore_cq(int fd, uint32_t handle, int channel, int event_fd,
		      int64_t reported, int64_t queued)
{
	uint32_t cqe = 16, vector = 0, resp_cqe, v;
	uint64_t user_handle = 0x5eed;
	struct call c;

	call_init(&c, UVERBS_OBJECT_RESTORE, UVERBS_METHOD_RESTORE_CQ);
	attr_in(&c, UVERBS_ATTR_RESTORE_CQ_HANDLE, &handle, sizeof(handle));
	attr_in(&c, UVERBS_ATTR_RESTORE_CQ_CQE, &cqe, sizeof(cqe));
	attr_in(&c, UVERBS_ATTR_RESTORE_CQ_USER_HANDLE, &user_handle, sizeof(user_handle));
	attr_in(&c, UVERBS_ATTR_RESTORE_CQ_COMP_VECTOR, &vector, sizeof(vector));
	attr_out(&c, UVERBS_ATTR_RESTORE_CQ_RESP_CQE, &resp_cqe, sizeof(resp_cqe));
	if (channel >= 0)
		attr_handle(&c, UVERBS_ATTR_RESTORE_CQ_COMP_CHANNEL, channel);
	if (event_fd >= 0)
		attr_handle(&c, UVERBS_ATTR_RESTORE_CQ_EVENT_FD, event_fd);
	if (reported >= 0) {
		v = reported;
		attr_in(&c, UVERBS_ATTR_RESTORE_CQ_COMP_EVENTS_REPORTED, &v, sizeof(v));
	}
	if (queued >= 0) {
		v = queued;
		attr_in(&c, UVERBS_ATTR_RESTORE_CQ_COMP_EVENTS_QUEUED, &v, sizeof(v));
	}
	return call_do(fd, &c);
}

static int restore_qp(int fd, uint32_t handle, uint32_t pd, uint32_t cq, int event_fd)
{
	struct ib_uverbs_qp_cap cap = {
		.max_send_wr = 1, .max_recv_wr = 1, .max_send_sge = 1, .max_recv_sge = 1,
	};
	uint64_t type = QPT_RC, state = QPS_RESET, user_handle = 0x5eed;
	struct call c;
	uint32_t qpn;

	call_init(&c, UVERBS_OBJECT_RESTORE, UVERBS_METHOD_RESTORE_QP);
	attr_in(&c, UVERBS_ATTR_RESTORE_QP_HANDLE, &handle, sizeof(handle));
	attr_handle(&c, UVERBS_ATTR_RESTORE_QP_PD_HANDLE, pd);
	attr_handle(&c, UVERBS_ATTR_RESTORE_QP_SEND_CQ_HANDLE, cq);
	attr_handle(&c, UVERBS_ATTR_RESTORE_QP_RECV_CQ_HANDLE, cq);
	attr_in(&c, UVERBS_ATTR_RESTORE_QP_TYPE, &type, sizeof(type));
	attr_in(&c, UVERBS_ATTR_RESTORE_QP_STATE, &state, sizeof(state));
	attr_in(&c, UVERBS_ATTR_RESTORE_QP_USER_HANDLE, &user_handle, sizeof(user_handle));
	attr_in(&c, UVERBS_ATTR_RESTORE_QP_CAP, &cap, sizeof(cap));
	attr_out(&c, UVERBS_ATTR_RESTORE_QP_RESP_QPN, &qpn, sizeof(qpn));
	if (event_fd >= 0)
		attr_handle(&c, UVERBS_ATTR_RESTORE_QP_EVENT_FD, event_fd);
	return call_do(fd, &c);
}

static int same_file(int fd, long long id, const char *type)
{
	char t[32] = "";

	fdinfo(fd, "type", t, sizeof(t));
	return fdnum(fd, "event_file_id") == id && !strcmp(t, type);
}

/* Returns @fd's event_file_id after checking its identity lines. */
static long long check_event_file(int fd, const char *what, const char *type,
				  int dflt, long long ctxn, long long index)
{
	char t[32] = "", name[64] = "";
	long long id = fdnum(fd, "event_file_id");

	fdinfo(fd, "type", t, sizeof(t));
	fdinfo(fd, "ibdev", name, sizeof(name));
	check(!strcmp(t, type), "%s: type %s", what, t);
	check(id > 0, "%s: event_file_id %lld", what, id);
	check(fdnum(fd, "ctxn") == ctxn && fdnum(fd, "ibdev_index") == index &&
	      !strcmp(name, ibdev), "%s: ctxn %lld ibdev_index %lld ibdev %s",
	      what, fdnum(fd, "ctxn"), fdnum(fd, "ibdev_index"), name);
	check(fdnum(fd, "default") == dflt, "%s: default %lld (want %d, -1 absent)",
	      what, fdnum(fd, "default"), dflt);
	check(fdnum(fd, "events_queued") == 0, "%s: events_queued %lld", what,
	      fdnum(fd, "events_queued"));
	return id;
}

/* Sends @fd over a socketpair and returns the received FD, or -1. */
static int pass_fd(int fd)
{
	union {
		struct cmsghdr h;
		char buf[CMSG_SPACE(sizeof(int))];
	} u = {};
	char b = 0;
	struct iovec iov = { .iov_base = &b, .iov_len = 1 };
	struct msghdr m = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = u.buf, .msg_controllen = sizeof(u.buf),
	};
	struct cmsghdr *cm = CMSG_FIRSTHDR(&m);
	int sv[2], out = -1;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv))
		return -1;
	cm->cmsg_level = SOL_SOCKET;
	cm->cmsg_type = SCM_RIGHTS;
	cm->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cm), &fd, sizeof(fd));
	if (sendmsg(sv[0], &m, 0) == 1) {
		memset(u.buf, 0, sizeof(u.buf));
		m.msg_controllen = sizeof(u.buf);
		cm = recvmsg(sv[1], &m, 0) == 1 ? CMSG_FIRSTHDR(&m) : NULL;
		if (cm && cm->cmsg_type == SCM_RIGHTS)
			memcpy(&out, CMSG_DATA(cm), sizeof(out));
	}
	close(sv[0]);
	close(sv[1]);
	return out;
}

static int readable(int fd, int ms)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };

	return poll(&p, 1, ms) == 1;
}

static int to_init(struct ibv_qp *qp)
{
	struct ibv_qp_attr a = {
		.qp_state = IBV_QPS_INIT, .port_num = 1,
		.qp_access_flags = IBV_ACCESS_LOCAL_WRITE,
	};

	return ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
			     IBV_QP_ACCESS_FLAGS);
}

static int connect_self(struct ibv_context *ctx, struct ibv_qp *qp, int sgid_index)
{
	struct ibv_qp_attr a;
	union ibv_gid gid;

	if (ibv_query_gid(ctx, 1, sgid_index, &gid) || to_init(qp))
		return -1;
	memset(&a, 0, sizeof(a));
	a.qp_state = IBV_QPS_RTR;
	a.path_mtu = IBV_MTU_1024;
	a.dest_qp_num = qp->qp_num;
	a.max_dest_rd_atomic = 1;
	a.min_rnr_timer = 12;
	a.ah_attr.is_global = 1;
	a.ah_attr.grh.dgid = gid;
	a.ah_attr.grh.sgid_index = sgid_index;
	a.ah_attr.grh.hop_limit = 64;
	a.ah_attr.port_num = 1;
	if (ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
			  IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
			  IBV_QP_MIN_RNR_TIMER))
		return -1;
	memset(&a, 0, sizeof(a));
	a.qp_state = IBV_QPS_RTS;
	a.timeout = 14;
	a.retry_cnt = 7;
	a.rnr_retry = 7;
	a.max_rd_atomic = 1;
	return ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
			     IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
}

static void comp_event_counters(struct ibv_context *ctx, struct ibv_pd *pd,
				struct ibv_comp_channel *ch, struct ibv_cq *cq,
				int sgid_index)
{
	struct ibv_qp_init_attr qia = {
		.send_cq = cq, .recv_cq = cq, .qp_type = IBV_QPT_RC,
		.cap = { .max_send_wr = 1, .max_recv_wr = 1, .max_send_sge = 1, .max_recv_sge = 1 },
	};
	struct ibv_recv_wr rwr = { .wr_id = 1 }, *brw;
	struct ibv_send_wr swr = { .wr_id = 2, .opcode = IBV_WR_SEND,
				   .send_flags = IBV_SEND_SIGNALED }, *bsw;
	struct uobj_events ev;
	struct ibv_cq *ecq;
	struct ibv_wc wc[2];
	struct ibv_qp *qp;
	int got = 0, i, ret;
	void *ectx;

	qp = ibv_create_qp(pd, &qia);
	if (!qp || connect_self(ctx, qp, sgid_index)) {
		skip("completion event: self-loop RC QP on GID %d: %s", sgid_index,
		     strerror(errno));
		goto out;
	}
	if (ibv_req_notify_cq(cq, 0) || ibv_post_recv(qp, &rwr, &brw) ||
	    ibv_post_send(qp, &swr, &bsw)) {
		check(0, "completion event: arm and post: %s", strerror(errno));
		goto out;
	}
	if (!check(readable(ch->fd, 5000), "completion event: channel readable"))
		goto out;
	check(fdnum(ch->fd, "events_queued") == 1, "channel: events_queued %lld with one unread",
	      fdnum(ch->fd, "events_queued"));
	ret = query_events(ctx->cmd_fd, cq->handle, -1, &ev);
	check(!ret && ev.comp_events_queued == 1 && ev.comp_events_reported == 0,
	      "CQ with an unread event: comp_events_queued %u comp_events_reported %u",
	      ev.comp_events_queued, ev.comp_events_reported);
	if (!check(!ibv_get_cq_event(ch, &ecq, &ectx) && ecq == cq, "ibv_get_cq_event"))
		goto out;
	ret = query_events(ctx->cmd_fd, cq->handle, -1, &ev);
	check(!ret && ev.comp_events_queued == 0 && ev.comp_events_reported == 1,
	      "CQ after the read: comp_events_queued %u comp_events_reported %u",
	      ev.comp_events_queued, ev.comp_events_reported);
	ibv_ack_cq_events(cq, 1);
	ret = query_events(ctx->cmd_fd, cq->handle, -1, &ev);
	check(!ret && ev.comp_events_reported == 1,
	      "CQ after ibv_ack_cq_events: comp_events_reported %u", ev.comp_events_reported);
	for (i = 0; got < 2 && i < 1000; i++) {
		int n = ibv_poll_cq(cq, 2 - got, wc + got);

		if (n < 0)
			break;
		got += n;
		if (got < 2)
			usleep(1000);
	}
	check(got == 2, "self-loop SEND: %d of 2 completions", got);
out:
	if (qp)
		ibv_destroy_qp(qp);
}

static void async_event_counters(struct ibv_context *ctx, struct ibv_pd *pd,
				 struct ibv_cq *cq)
{
	struct ibv_srq_init_attr sia = { .attr = { .max_wr = 4, .max_sge = 1 } };
	struct ibv_qp_init_attr qia = {
		.qp_context = (void *)0xa5, .send_cq = cq, .recv_cq = cq,
		.qp_type = IBV_QPT_RC,
		.cap = { .max_send_wr = 1, .max_send_sge = 1 },
	};
	struct ibv_qp_attr a = { .qp_state = IBV_QPS_ERR };
	struct ibv_async_event e;
	struct uobj_events ev;
	struct ibv_srq *srq;
	struct ibv_qp *qp = NULL;
	int ret;

	srq = ibv_create_srq(pd, &sia);
	qia.srq = srq;
	if (srq)
		qp = ibv_create_qp(pd, &qia);
	if (!qp || to_init(qp) || ibv_modify_qp(qp, &a, IBV_QP_STATE)) {
		skip("async event: QP on an SRQ to error: %s", strerror(errno));
		goto out;
	}
	if (!readable(ctx->async_fd, 2000)) {
		skip("async event: no LAST_WQE_REACHED after moving the QP to error");
		goto out;
	}
	check(fdnum(ctx->async_fd, "events_queued") == 1,
	      "default async file: events_queued %lld with one unread",
	      fdnum(ctx->async_fd, "events_queued"));
	ret = query_events(ctx->cmd_fd, -1, qp->handle, &ev);
	check(!ret && ev.async_events_queued == 1 && ev.async_events_reported == 0,
	      "QP with an unread event: async_events_queued %u async_events_reported %u",
	      ev.async_events_queued, ev.async_events_reported);
	if (!check(!ibv_get_async_event(ctx, &e), "ibv_get_async_event"))
		goto out;
	check(e.event_type == IBV_EVENT_QP_LAST_WQE_REACHED && e.element.qp == qp,
	      "async event: %s on the QP", ibv_event_type_str(e.event_type));
	ret = query_events(ctx->cmd_fd, -1, qp->handle, &ev);
	check(!ret && ev.async_events_queued == 0 && ev.async_events_reported == 1,
	      "QP after the read: async_events_queued %u async_events_reported %u",
	      ev.async_events_queued, ev.async_events_reported);
	ibv_ack_async_event(&e);
	ret = query_events(ctx->cmd_fd, -1, qp->handle, &ev);
	check(!ret && ev.async_events_reported == 1,
	      "QP after ibv_ack_async_event: async_events_reported %u",
	      ev.async_events_reported);
out:
	if (qp)
		ibv_destroy_qp(qp);
	if (srq)
		ibv_destroy_srq(srq);
}

/* A CQ created by hand on @fd's context, with @event_fd, uses file @id. */
static void cq_on_async_file(int fd, int event_fd, long long id, const char *which)
{
	struct uobj_events ev;
	uint32_t handle;
	int ret;

	ret = create_cq(fd, event_fd, 0x9abc, &handle);
	if (!check(!ret, "CQ on %s async file: create: %s", which, strerror(-ret)))
		return;
	ret = query_events(fd, handle, -1, &ev);
	check(!ret && ev.user_handle == 0x9abc && ev.async_file_id == id &&
	      !ev.comp_channel_id,
	      "CQ on %s async file: rc %d user_handle 0x%llx async_file_id %u (want %lld) comp_channel_id %u",
	      which, ret, (unsigned long long)ev.user_handle, ev.async_file_id, id,
	      ev.comp_channel_id);
}

/* Contexts this probe opens itself on @cdev. */
static void own_contexts(const char *cdev, struct ibv_comp_channel *ch,
			 long long ctxn, long long index)
{
	int wfd, ifd, nfd, afd = -1, a1, a2, ret;
	long long id1, id2;
	uint32_t id;

	wfd = open(cdev, O_RDWR | O_CLOEXEC);
	ret = wfd < 0 ? -errno : write_get_context(wfd, &afd);
	if (!check(!ret, "write GET_CONTEXT: %s", strerror(-ret)))
		return;
	check(fdnum(wfd, "ctxn") != ctxn && fdnum(wfd, "ibdev_index") == index,
	      "second context: ctxn %lld (first %lld), same ibdev_index",
	      fdnum(wfd, "ctxn"), ctxn);
	id1 = check_event_file(afd, "write GET_CONTEXT's async_fd", "async_event", 1,
			       fdnum(wfd, "ctxn"), index);
	check(id1 == 1, "IDs restart for each uverbs file: %lld", id1);
	ret = query_default_async(wfd, &id);
	check(!ret && id == id1, "QUERY_DEFAULT_ASYNC, write path: %u", id);
	close(afd);
	ret = query_default_async(wfd, &id);
	check(!ret && id == id1, "QUERY_DEFAULT_ASYNC after closing the default's only FD: %u", id);
	ret = restore_cq(wfd, 0xfff0, ch->fd, -1, -1, -1);
	check(ret == -EBADF, "RESTORE_CQ with a channel of another context: %s",
	      strerror(-ret));
	close(wfd);

	ifd = open(cdev, O_RDWR | O_CLOEXEC);
	ret = ifd < 0 ? -errno : ioctl_get_context(ifd, 0);
	if (!check(!ret, "ioctl GET_CONTEXT: %s", strerror(-ret)))
		return;
	ret = query_default_async(ifd, &id);
	check(!ret && id == 0, "QUERY_DEFAULT_ASYNC, no async file: %u", id);
	a1 = async_alloc(ifd);
	a2 = a1 < 0 ? a1 : async_alloc(ifd);
	if (check(a1 >= 0 && a2 >= 0, "ASYNC_EVENT_ALLOC twice: %s",
		  a1 < 0 || a2 < 0 ? strerror(a1 < 0 ? -a1 : -a2) : "ok")) {
		id1 = check_event_file(a1, "first ASYNC_EVENT_ALLOC", "async_event", 1,
				       fdnum(ifd, "ctxn"), index);
		id2 = check_event_file(a2, "second ASYNC_EVENT_ALLOC", "async_event", 0,
				       fdnum(ifd, "ctxn"), index);
		check(id1 != id2, "two async files: IDs %lld and %lld", id1, id2);
		ret = query_default_async(ifd, &id);
		check(!ret && id == id1, "QUERY_DEFAULT_ASYNC names the first: %u", id);
		cq_on_async_file(ifd, a2, id2, "the second");
		cq_on_async_file(ifd, -1, id1, "no EVENT_FD, so the default");
		close(a1);
		close(a2);
	}
	close(ifd);

	nfd = open(cdev, O_RDWR | O_CLOEXEC);
	ret = query_default_async(nfd, &id);
	check(ret < 0, "QUERY_DEFAULT_ASYNC with no context refused: %s", strerror(-ret));
	close(nfd);
}

/* rxe opens a restore-mode context without a migration. */
static void restore_context(const char *cdev)
{
	uint32_t pd = 0, cq = 0;
	int fd, afd, ch, ret;

	fd = open(cdev, O_RDWR | O_CLOEXEC);
	ret = fd < 0 ? -errno : ioctl_get_context(fd, 1);
	if (!check(!ret, "restore-mode ioctl GET_CONTEXT: %s", strerror(-ret)))
		return;
	ret = write_alloc_pd(fd, &pd);
	if (!ret)
		ret = create_cq(fd, -1, 0, &cq);
	if (!check(!ret, "PD and CQ on the restore-mode context: %s", strerror(-ret)))
		goto out;

	/* @pd's handle is taken, so a call that gets past the checks fails with EBUSY. */
	ret = restore_cq(fd, pd, -1, -1, -1, -1);
	check(ret == -EINVAL, "RESTORE_CQ without EVENT_FD, no default: %s", strerror(-ret));
	ret = restore_qp(fd, pd, pd, cq, -1);
	check(ret == -EINVAL, "RESTORE_QP without EVENT_FD, no default: %s", strerror(-ret));

	afd = async_alloc(fd);
	if (!check(afd >= 0, "ASYNC_EVENT_ALLOC: %s",
		   afd < 0 ? strerror(-afd) : "ok"))
		goto out;
	ret = restore_cq(fd, pd, -1, -1, -1, -1);
	check(ret == -EBUSY, "RESTORE_CQ without EVENT_FD, with a default: %s",
	      strerror(-ret));
	ret = restore_qp(fd, pd, pd, cq, -1);
	check(ret == -EBUSY, "RESTORE_QP without EVENT_FD, with a default: %s",
	      strerror(-ret));
	ret = restore_cq(fd, pd, -1, -1, 3, 1);
	check(ret == -EINVAL, "RESTORE_CQ with COMP_EVENTS_QUEUED and no channel: %s",
	      strerror(-ret));
	ch = write_comp_channel(fd);
	if (check(ch >= 0, "completion channel on the restore-mode context: %s",
		  ch < 0 ? strerror(-ch) : "ok")) {
		ret = restore_cq(fd, pd, ch, -1, 3, 1);
		check(ret == -EBUSY, "RESTORE_CQ with COMP_EVENTS_QUEUED and a channel: %s",
		      strerror(-ret));
		close(ch);
	}
	close(afd);
out:
	close(fd);
}

static int detect_driver(void)
{
	char path[PATH_MAX], link[PATH_MAX];
	ssize_t n;

	snprintf(path, sizeof(path), "/sys/class/infiniband/%s/device/driver", ibdev);
	n = readlink(path, link, sizeof(link) - 1);
	if (n > 0) {
		link[n] = 0;
		if (strstr(link, "mlx5_core"))
			return DRV_MLX5;
	}
	snprintf(path, sizeof(path), "/sys/class/infiniband/%s/parent", ibdev);
	return access(path, F_OK) ? DRV_OTHER : DRV_RXE;
}

int main(int argc, char **argv)
{
	struct ibv_context *ctx = NULL;
	struct ibv_comp_channel *ch;
	struct ibv_device **list;
	struct ibv_qp_init_attr qia = {
		.qp_type = IBV_QPT_RC,
		.cap = { .max_send_wr = 1, .max_recv_wr = 1, .max_send_sge = 1, .max_recv_sge = 1 },
	};
	long long ctxn, index, def_id, ch_id, ex_id;
	char name[64] = "", cdev[PATH_MAX] = "";
	struct uobj_events ev;
	struct ibv_cq *cq;
	struct ibv_pd *pd;
	struct ibv_qp *qp;
	int i, n, ex, fd, ret, status;
	uint32_t id;
	pid_t pid;

	if (argc < 2 || argc > 3)
		return fprintf(stderr, "usage: %s <ibdev> [sgid_index]\n", argv[0]), 2;
	ibdev = argv[1];
	drv = detect_driver();
	if (drv == DRV_OTHER)
		return fprintf(stderr, "%s: mlx5 and rxe only\n", ibdev), 2;

	list = ibv_get_device_list(&n);
	for (i = 0; list && i < n; i++)
		if (!strcmp(ibv_get_device_name(list[i]), ibdev)) {
			ctx = ibv_open_device(list[i]);
			snprintf(cdev, sizeof(cdev), "/dev/infiniband/%s", list[i]->dev_name);
		}
	if (!ctx)
		return fprintf(stderr, "cannot open %s\n", ibdev), 2;

	/* fdinfo identity */
	ctxn = fdnum(ctx->cmd_fd, "ctxn");
	index = fdnum(ctx->cmd_fd, "ibdev_index");
	fdinfo(ctx->cmd_fd, "ibdev", name, sizeof(name));
	if (!check(ctxn >= 0 && index >= 0 && !strcmp(name, ibdev),
		   "uverbs FD: ctxn %lld ibdev_index %lld ibdev %s", ctxn, index, name))
		return 1;
	def_id = check_event_file(ctx->async_fd, "default async file", "async_event", 1,
				  ctxn, index);
	ch = ibv_create_comp_channel(ctx);
	if (!ch)
		return fprintf(stderr, "ibv_create_comp_channel: %s\n", strerror(errno)), 2;
	ch_id = check_event_file(ch->fd, "completion channel", "comp_channel", -1, ctxn, index);
	ex = async_alloc(ctx->cmd_fd);
	if (!check(ex >= 0, "ASYNC_EVENT_ALLOC: %s", ex < 0 ? strerror(-ex) : "ok"))
		return 1;
	ex_id = check_event_file(ex, "second async file", "async_event", 0, ctxn, index);
	check(def_id != ch_id && def_id != ex_id && ch_id != ex_id,
	      "event_file_id distinct: %lld %lld %lld", def_id, ch_id, ex_id);

	fd = dup(ch->fd);
	check(same_file(fd, ch_id, "comp_channel"), "channel after dup()");
	close(fd);
	pid = fork();
	if (pid == 0)
		_exit(!(same_file(ch->fd, ch_id, "comp_channel") &&
			same_file(ex, ex_id, "async_event")));
	check(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
	      !WEXITSTATUS(status), "channel and async file in a forked child");
	fd = pass_fd(ch->fd);
	check(same_file(fd, ch_id, "comp_channel"), "channel after SCM_RIGHTS");
	if (fd >= 0)
		close(fd);
	fd = pass_fd(ex);
	check(same_file(fd, ex_id, "async_event"), "async file after SCM_RIGHTS");
	if (fd >= 0)
		close(fd);

	/* CHECKPOINT_QUERY_EVENTS and CHECKPOINT_QUERY_DEFAULT_ASYNC */
	pd = ibv_alloc_pd(ctx);
	cq = pd ? ibv_create_cq(ctx, 16, (void *)0x1234, ch, 0) : NULL;
	qia.send_cq = qia.recv_cq = cq;
	qia.qp_context = (void *)0x5678;
	qp = cq ? ibv_create_qp(pd, &qia) : NULL;
	if (!qp)
		return fprintf(stderr, "PD, CQ, QP: %s\n", strerror(errno)), 2;
	/* libibverbs passes its own object pointer as user_handle. */
	ret = query_events(ctx->cmd_fd, cq->handle, -1, &ev);
	check(!ret && ev.user_handle == (uintptr_t)cq && ev.comp_channel_id == ch_id &&
	      ev.async_file_id == def_id,
	      "CQ on the channel: rc %d user_handle 0x%llx comp_channel_id %u async_file_id %u",
	      ret, (unsigned long long)ev.user_handle, ev.comp_channel_id, ev.async_file_id);
	check(!ret && !ev.async_events_reported && !ev.comp_events_reported &&
	      !ev.async_events_queued && !ev.comp_events_queued, "CQ on the channel: counts zero");
	ret = query_events(ctx->cmd_fd, -1, qp->handle, &ev);
	check(!ret && ev.user_handle == (uintptr_t)qp && ev.async_file_id == def_id &&
	      !ev.comp_channel_id,
	      "QP: rc %d user_handle 0x%llx async_file_id %u comp_channel_id %u", ret,
	      (unsigned long long)ev.user_handle, ev.async_file_id, ev.comp_channel_id);
	ret = query_default_async(ctx->cmd_fd, &id);
	check(!ret && id == def_id, "QUERY_DEFAULT_ASYNC: %u", id);
	ret = query_events(ctx->cmd_fd, -1, -1, &ev);
	check(ret == -EINVAL, "QUERY_EVENTS with no handle: %s", strerror(-ret));
	ret = query_events(ctx->cmd_fd, cq->handle, qp->handle, &ev);
	check(ret == -EINVAL, "QUERY_EVENTS with both handles: %s", strerror(-ret));
	ret = restore_cq(ctx->cmd_fd, 0xfff0, -1, -1, 1, -1);
	check(ret == -EPERM, "RESTORE_CQ seeding a counter outside restore mode: %s",
	      strerror(-ret));

	/* Counters */
	if (argc == 3)
		comp_event_counters(ctx, pd, ch, cq, atoi(argv[2]));
	else
		skip("completion event: no sgid_index given");
	async_event_counters(ctx, pd, cq);

	/* Contexts opened here */
	own_contexts(cdev, ch, ctxn, index);
	if (drv == DRV_RXE)
		restore_context(cdev);
	else
		skip("restore-mode context: rxe only without a migration");

	ibv_destroy_qp(qp);
	ibv_destroy_cq(cq);
	ibv_dealloc_pd(pd);
	ibv_destroy_comp_channel(ch);
	close(ex);
	ibv_close_device(ctx);
	ibv_free_device_list(list);
	printf("%s\n", failed ? "FAIL" : "PASS");
	return failed;
}

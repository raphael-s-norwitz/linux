// SPDX-License-Identifier: GPL-2.0
/*
 * Which QP context fields show that an RC send is still pending?
 *
 *   qp_sq_state_probe <ibdev> <sgid_index>
 *
 * A dump waits for every RC QP's sends to be acknowledged by comparing
 * next_send_psn with last_acked_psn, but an RNR NAK rewinds
 * next_send_psn while the requester waits for the RNR timer, so a send
 * held by RNR NAKs reads as idle. This probe reads VFMIG_QUERY_QP's
 * RESP_SQ_PSN and RESP_SQ_STATE for self-loop RC QPs (mlx5 only):
 *
 *   fresh  in RTS, nothing sent
 *   idle   one send, completed against a posted receive
 *   rnr    one send with no receive posted, retried for ever (rnr_retry
 *          7, RNR timer 491 ms), sampled 20 times 50 ms apart
 *   freed  the rnr QP after the receive is posted and the send completes
 *
 * and, for each candidate rule, whether it holds in every rnr sample and
 * in none of the idle states. It changes nothing on the device beyond its
 * own QPs. Exit 0 if some rule separates them, 1 if none does, 2 on
 * setup errors.
 */
#define _GNU_SOURCE
#include "../../../../include/uapi/rdma/ib_user_ioctl_cmds.h"
#include "../../../../include/uapi/rdma/mlx5_user_ioctl_cmds.h"
#include "../../../../include/uapi/rdma/mlx5-abi.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <infiniband/verbs.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/rdma_user_ioctl_cmds.h>

#define RNR_SAMPLES 20
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

struct sample {
	struct mlx5_ib_vfmig_qp_sq_psn psn;
	struct mlx5_ib_vfmig_qp_sq_state st;
};

static struct ibv_context *ctx;
static struct ibv_pd *pd;
static int sgid_index;

static int query(struct ibv_qp *qp, struct sample *s)
{
	struct mlx5_ib_restore_qp_req blob;
	uint32_t create_flags;
	uint64_t user_handle;
	struct {
		struct ib_uverbs_ioctl_hdr hdr;
		struct ib_uverbs_attr attrs[6];
	} c = {};
	struct {
		uint16_t id;
		uint16_t len;
		void *data;
	} outs[] = {
		{ MLX5_IB_ATTR_VFMIG_QUERY_QP_RESP_BLOB, sizeof(blob), &blob },
		{ MLX5_IB_ATTR_VFMIG_QUERY_QP_RESP_USER_HANDLE, sizeof(user_handle), &user_handle },
		{ MLX5_IB_ATTR_VFMIG_QUERY_QP_RESP_CREATE_FLAGS, sizeof(create_flags),
		  &create_flags },
		{ MLX5_IB_ATTR_VFMIG_QUERY_QP_RESP_SQ_PSN, sizeof(s->psn), &s->psn },
		{ MLX5_IB_ATTR_VFMIG_QUERY_QP_RESP_SQ_STATE, sizeof(s->st), &s->st },
	};
	unsigned int i, n = 0;

	c.hdr.object_id = MLX5_IB_OBJECT_VFMIG;
	c.hdr.method_id = MLX5_IB_METHOD_VFMIG_QUERY_QP;
	c.hdr.driver_id = RDMA_DRIVER_MLX5;
	c.attrs[n].attr_id = MLX5_IB_ATTR_VFMIG_QUERY_QP_HANDLE;
	c.attrs[n].flags = UVERBS_ATTR_F_MANDATORY;
	c.attrs[n++].data = qp->handle;
	for (i = 0; i < ARRAY_SIZE(outs); i++, n++) {
		c.attrs[n].attr_id = outs[i].id;
		c.attrs[n].len = outs[i].len;
		c.attrs[n].flags = UVERBS_ATTR_F_MANDATORY;
		c.attrs[n].data = (uintptr_t)outs[i].data;
	}
	c.hdr.num_attrs = n;
	c.hdr.length = sizeof(c.hdr) + n * sizeof(c.attrs[0]);
	return ioctl(ctx->cmd_fd, RDMA_VERBS_IOCTL, &c) ? -errno : 0;
}

static struct ibv_qp *self_loop(struct ibv_cq *cq)
{
	struct ibv_qp_init_attr qia = {
		.send_cq = cq, .recv_cq = cq, .qp_type = IBV_QPT_RC,
		.cap = { .max_send_wr = 1, .max_recv_wr = 1, .max_send_sge = 1, .max_recv_sge = 1 },
	};
	struct ibv_qp_attr a;
	union ibv_gid gid;
	struct ibv_qp *qp;

	if (ibv_query_gid(ctx, 1, sgid_index, &gid))
		return NULL;
	qp = ibv_create_qp(pd, &qia);
	if (!qp)
		return NULL;

	memset(&a, 0, sizeof(a));
	a.qp_state = IBV_QPS_INIT;
	a.port_num = 1;
	a.qp_access_flags = IBV_ACCESS_LOCAL_WRITE;
	if (ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
			  IBV_QP_ACCESS_FLAGS))
		goto err;

	memset(&a, 0, sizeof(a));
	a.qp_state = IBV_QPS_RTR;
	a.path_mtu = IBV_MTU_1024;
	a.dest_qp_num = qp->qp_num;
	a.max_dest_rd_atomic = 1;
	a.min_rnr_timer = 31;
	a.ah_attr.is_global = 1;
	a.ah_attr.grh.dgid = gid;
	a.ah_attr.grh.sgid_index = sgid_index;
	a.ah_attr.grh.hop_limit = 64;
	a.ah_attr.port_num = 1;
	if (ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
			  IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
			  IBV_QP_MIN_RNR_TIMER))
		goto err;

	memset(&a, 0, sizeof(a));
	a.qp_state = IBV_QPS_RTS;
	a.timeout = 14;
	a.retry_cnt = 7;
	a.rnr_retry = 7;
	a.max_rd_atomic = 1;
	if (ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
			  IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC))
		goto err;
	return qp;
err:
	ibv_destroy_qp(qp);
	return NULL;
}

static int post_send(struct ibv_qp *qp)
{
	struct ibv_send_wr wr = { .wr_id = 1, .opcode = IBV_WR_SEND,
				  .send_flags = IBV_SEND_SIGNALED }, *bad;

	return ibv_post_send(qp, &wr, &bad);
}

static int post_recv(struct ibv_qp *qp)
{
	struct ibv_recv_wr wr = { .wr_id = 2 }, *bad;

	return ibv_post_recv(qp, &wr, &bad);
}

/* Poll @cq until @want completions arrive, for up to 10 s. */
static int reap(struct ibv_cq *cq, int want)
{
	struct timespec nap = { .tv_nsec = 10 * 1000 * 1000 };
	struct ibv_wc wc;
	int got = 0, n, i;

	for (i = 0; got < want && i < 1000; i++) {
		n = ibv_poll_cq(cq, 1, &wc);
		if (n < 0)
			return -1;
		if (n) {
			if (wc.status != IBV_WC_SUCCESS) {
				fprintf(stderr, "completion: %s\n", ibv_wc_status_str(wc.status));
				return -1;
			}
			got++;
			continue;
		}
		nanosleep(&nap, NULL);
	}
	return got == want ? 0 : -1;
}

static void show(const char *what, const struct sample *s)
{
	printf("  %-8s next_send_psn %#08x last_acked_psn %#08x ssn %#08x wqebb hw %#06x sw %#06x retry %u/%u rnr %u/%u\n",
	       what,
	       s->psn.next_send_psn, s->psn.last_acked_psn, s->st.ssn,
	       s->st.hw_sq_wqebb_counter, s->st.sw_sq_wqebb_counter,
	       s->st.cur_retry_count, s->st.retry_count,
	       s->st.cur_rnr_retry, s->st.rnr_retry);
}

static bool psn_pending(const struct sample *s)
{
	return ((s->psn.next_send_psn - 1 - s->psn.last_acked_psn) & 0xffffff) != 0;
}

static bool wqebb_pending(const struct sample *s)
{
	return s->st.hw_sq_wqebb_counter != s->st.sw_sq_wqebb_counter;
}

static bool rnr_spent(const struct sample *s)
{
	return s->st.cur_rnr_retry != s->st.rnr_retry;
}

static bool retry_spent(const struct sample *s)
{
	return s->st.cur_retry_count != s->st.retry_count;
}

/* Assumes ssn counts the send WQEs the requester has completed. */
static bool ssn_behind(const struct sample *s)
{
	return (s->st.ssn & 0xffff) != s->st.sw_sq_wqebb_counter;
}

static const struct {
	const char *name;
	bool (*pending)(const struct sample *s);
} rules[] = {
	{ "PSN outstanding (today's drain)", psn_pending },
	{ "hw_sq_wqebb_counter != sw_sq_wqebb_counter", wqebb_pending },
	{ "cur_rnr_retry != rnr_retry", rnr_spent },
	{ "cur_retry_count != retry_count", retry_spent },
	{ "ssn != sw_sq_wqebb_counter", ssn_behind },
};

int main(int argc, char **argv)
{
	struct timespec gap = { .tv_nsec = 50 * 1000 * 1000 };
	struct sample fresh, idle, freed, rnr[RNR_SAMPLES];
	struct ibv_qp *qf = NULL, *qi = NULL, *qr = NULL;
	struct ibv_cq *cf = NULL, *ci = NULL, *cr = NULL;
	struct ibv_device **list;
	struct ibv_wc wc;
	unsigned int r, i;
	int n, ret, found = 0;

	if (argc != 3)
		return fprintf(stderr, "usage: %s <ibdev> <sgid_index>\n", argv[0]), 2;
	sgid_index = atoi(argv[2]);
	list = ibv_get_device_list(&n);
	for (i = 0; list && (int)i < n; i++)
		if (!strcmp(ibv_get_device_name(list[i]), argv[1]))
			ctx = ibv_open_device(list[i]);
	if (!ctx)
		return fprintf(stderr, "cannot open %s\n", argv[1]), 2;
	pd = ibv_alloc_pd(ctx);
	cf = ibv_create_cq(ctx, 4, NULL, NULL, 0);
	ci = ibv_create_cq(ctx, 4, NULL, NULL, 0);
	cr = ibv_create_cq(ctx, 4, NULL, NULL, 0);
	if (!pd || !cf || !ci || !cr)
		return fprintf(stderr, "pd/cq: %s\n", strerror(errno)), 2;
	qf = self_loop(cf);
	qi = self_loop(ci);
	qr = self_loop(cr);
	if (!qf || !qi || !qr)
		return fprintf(stderr, "self-loop QPs on GID %d: %s\n", sgid_index,
			       strerror(errno)), 2;

	ret = query(qf, &fresh);
	if (ret)
		return fprintf(stderr, "VFMIG_QUERY_QP: %s (kernel without RESP_SQ_STATE?)\n",
			       strerror(-ret)), 2;
	if (post_recv(qi) || post_send(qi) || reap(ci, 2))
		return fprintf(stderr, "idle QP: send did not complete\n"), 2;
	if (query(qi, &idle))
		return 2;
	if (post_send(qr))
		return fprintf(stderr, "rnr QP: post_send: %s\n", strerror(errno)), 2;
	for (i = 0; i < RNR_SAMPLES; i++) {
		nanosleep(&gap, NULL);
		if (query(qr, &rnr[i]))
			return 2;
	}
	if (ibv_poll_cq(cr, 1, &wc) != 0)
		return fprintf(stderr, "rnr QP: the send completed with no receive posted\n"), 2;
	if (post_recv(qr) || reap(cr, 2))
		return fprintf(stderr, "rnr QP: send did not complete once the receive was posted\n"), 2;
	if (query(qr, &freed))
		return 2;

	printf("== QP context fields\n");
	show("fresh", &fresh);
	show("idle", &idle);
	for (i = 0; i < RNR_SAMPLES; i++) {
		char name[16];

		snprintf(name, sizeof(name), "rnr %u", i);
		show(name, &rnr[i]);
	}
	show("freed", &freed);

	printf("\n== rules: pending in every rnr sample, and in no idle state?\n");
	for (r = 0; r < ARRAY_SIZE(rules); r++) {
		unsigned int hits = 0;
		bool false_alarm = rules[r].pending(&fresh) || rules[r].pending(&idle) ||
				   rules[r].pending(&freed);

		for (i = 0; i < RNR_SAMPLES; i++)
			hits += rules[r].pending(&rnr[i]);
		printf("  %-4s %-44s rnr %2u/%u%s\n",
		       hits == RNR_SAMPLES && !false_alarm ? "YES" : "no", rules[r].name, hits,
		       RNR_SAMPLES, false_alarm ? ", also on an idle QP" : "");
		if (hits == RNR_SAMPLES && !false_alarm)
			found = 1;
	}

	ibv_destroy_qp(qf);
	ibv_destroy_qp(qi);
	ibv_destroy_qp(qr);
	ibv_destroy_cq(cf);
	ibv_destroy_cq(ci);
	ibv_destroy_cq(cr);
	ibv_dealloc_pd(pd);
	ibv_close_device(ctx);
	return found ? 0 : 1;
}

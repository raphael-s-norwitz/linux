// SPDX-License-Identifier: GPL-2.0
/*
 * Holds CQs in each arm state for cq_arm_state.sh.
 *
 *   cq_arm_holder <ibdev> <status_path> <sgid_index>
 *
 * Creates four CQs with no completion channel and leaves them in:
 *
 *   A  armed for any completion (ibv_req_notify_cq(cq, 0)), no completion
 *   B  armed for solicited completions (ibv_req_notify_cq(cq, 1))
 *   C  armed, then fired by one zero-length SEND on a self-loop RC QP
 *      through GID <sgid_index>; its completions are polled and it is not
 *      re-armed
 *   D  never armed
 *
 * The QP and its PD are destroyed again, so a dump holds only the context
 * and the four CQs. Writes "READY A=0x<cqn> B=... C=... D=..." to the
 * status file, then waits:
 *
 *   SIGUSR1  poll every CQ once and write "OK" or "FAIL: ..."
 *   SIGTERM  exit
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <infiniband/mlx5dv.h>
#include <infiniband/verbs.h>

#define NCQ 4

static struct ibv_context *ctx;
static struct ibv_cq *cqs[NCQ];
static const char *status_path;
static volatile sig_atomic_t want_check, terminate;

static void status(const char *fmt, ...)
{
	va_list ap;
	FILE *f = fopen(status_path, "w");

	if (!f)
		return;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

static int connect_self(struct ibv_qp *qp, int sgid_index)
{
	struct ibv_qp_attr a;
	union ibv_gid gid;

	if (ibv_query_gid(ctx, 1, sgid_index, &gid))
		return fprintf(stderr, "ibv_query_gid(%d): %s\n", sgid_index, strerror(errno)), -1;

	memset(&a, 0, sizeof(a));
	a.qp_state = IBV_QPS_INIT;
	a.port_num = 1;
	a.qp_access_flags = IBV_ACCESS_LOCAL_WRITE;
	if (ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
			  IBV_QP_ACCESS_FLAGS))
		return fprintf(stderr, "INIT: %s\n", strerror(errno)), -1;

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
		return fprintf(stderr, "RTR: %s\n", strerror(errno)), -1;

	memset(&a, 0, sizeof(a));
	a.qp_state = IBV_QPS_RTS;
	a.timeout = 14;
	a.retry_cnt = 7;
	a.rnr_retry = 7;
	a.max_rd_atomic = 1;
	if (ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
			  IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC))
		return fprintf(stderr, "RTS: %s\n", strerror(errno)), -1;
	return 0;
}

/* Arm @cq, then fire it with one self-SEND whose two completions land on it. */
static int fire(struct ibv_cq *cq, int sgid_index)
{
	struct ibv_recv_wr rwr = { .wr_id = 1 }, *brw;
	struct ibv_send_wr swr = { .wr_id = 2, .opcode = IBV_WR_SEND,
				   .send_flags = IBV_SEND_SIGNALED }, *bsw;
	struct ibv_qp_init_attr qia = {
		.send_cq = cq, .recv_cq = cq, .qp_type = IBV_QPT_RC,
		.cap = { .max_send_wr = 1, .max_recv_wr = 1, .max_send_sge = 1, .max_recv_sge = 1 },
	};
	struct timespec t0, t;
	struct ibv_wc wc[2];
	struct ibv_pd *pd;
	struct ibv_qp *qp;
	int got = 0, n, ret = -1;

	pd = ibv_alloc_pd(ctx);
	qp = pd ? ibv_create_qp(pd, &qia) : NULL;
	if (!qp) {
		fprintf(stderr, "pd/qp: %s\n", strerror(errno));
		goto out;
	}
	if (connect_self(qp, sgid_index))
		goto out;
	if (ibv_req_notify_cq(cq, 0)) {
		fprintf(stderr, "arm C: %s\n", strerror(errno));
		goto out;
	}
	if (ibv_post_recv(qp, &rwr, &brw) || ibv_post_send(qp, &swr, &bsw)) {
		fprintf(stderr, "post: %s\n", strerror(errno));
		goto out;
	}
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (got < 2) {
		n = ibv_poll_cq(cq, 2 - got, wc + got);
		if (n < 0) {
			fprintf(stderr, "poll C: %d\n", n);
			goto out;
		}
		got += n;
		clock_gettime(CLOCK_MONOTONIC, &t);
		if (got < 2 && t.tv_sec - t0.tv_sec > 10) {
			fprintf(stderr, "%d of 2 completions after 10s\n", got);
			goto out;
		}
	}
	if (wc[0].status != IBV_WC_SUCCESS || wc[1].status != IBV_WC_SUCCESS) {
		fprintf(stderr, "self-SEND: %s / %s\n", ibv_wc_status_str(wc[0].status),
			ibv_wc_status_str(wc[1].status));
		goto out;
	}
	ret = 0;
out:
	if (qp)
		ibv_destroy_qp(qp);
	if (pd)
		ibv_dealloc_pd(pd);
	return ret;
}

static int cqn(struct ibv_cq *cq, uint32_t *out)
{
	struct mlx5dv_cq dv;
	struct mlx5dv_obj obj = { .cq = { .in = cq, .out = &dv } };

	if (mlx5dv_init_obj(&obj, MLX5DV_OBJ_CQ))
		return -1;
	*out = dv.cqn;
	return 0;
}

static void on_sig(int sig)
{
	if (sig == SIGUSR1)
		want_check = 1;
	else
		terminate = 1;
}

int main(int argc, char **argv)
{
	struct sigaction sa = { .sa_handler = on_sig };
	struct ibv_device **list;
	sigset_t block, wait;
	uint32_t cqns[NCQ];
	struct ibv_wc wc;
	int i, n, bad;

	if (argc != 4)
		return fprintf(stderr, "usage: %s <ibdev> <status> <sgid_index>\n", argv[0]), 2;
	status_path = argv[2];

	list = ibv_get_device_list(&n);
	for (i = 0; list && i < n; i++)
		if (!strcmp(ibv_get_device_name(list[i]), argv[1]))
			ctx = ibv_open_device(list[i]);
	if (!ctx)
		return fprintf(stderr, "cannot open %s\n", argv[1]), 2;
	for (i = 0; i < NCQ; i++) {
		cqs[i] = ibv_create_cq(ctx, 16, NULL, NULL, 0);
		if (!cqs[i] || cqn(cqs[i], &cqns[i]))
			return fprintf(stderr, "CQ %c: %s\n", 'A' + i, strerror(errno)), 2;
	}
	if (fire(cqs[2], atoi(argv[3]))) {
		status("FAIL: could not fire CQ C");
		return 2;
	}
	if (ibv_req_notify_cq(cqs[0], 0) || ibv_req_notify_cq(cqs[1], 1))
		return fprintf(stderr, "arm A/B: %s\n", strerror(errno)), 2;

	/* Signals stay blocked except inside sigsuspend(), so none is lost between checks. */
	sigemptyset(&block);
	sigaddset(&block, SIGUSR1);
	sigaddset(&block, SIGTERM);
	sigaddset(&block, SIGINT);
	sigprocmask(SIG_BLOCK, &block, &wait);
	sigdelset(&wait, SIGUSR1);
	sigdelset(&wait, SIGTERM);
	sigdelset(&wait, SIGINT);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	printf("READY pid=%d A=0x%x B=0x%x C=0x%x D=0x%x\n", getpid(),
	       cqns[0], cqns[1], cqns[2], cqns[3]);
	fflush(stdout);
	status("READY A=0x%x B=0x%x C=0x%x D=0x%x", cqns[0], cqns[1], cqns[2], cqns[3]);

	while (!terminate) {
		if (want_check) {
			want_check = 0;
			for (i = 0, bad = -1; i < NCQ && bad < 0; i++)
				if (ibv_poll_cq(cqs[i], 1, &wc) != 0)
					bad = i;
			if (bad >= 0)
				status("FAIL: CQ %c not empty or poll failed", 'A' + bad);
			else
				status("OK");
		}
		if (!terminate && !want_check)
			sigsuspend(&wait);
	}
	for (i = 0; i < NCQ; i++)
		ibv_destroy_cq(cqs[i]);
	ibv_close_device(ctx);
	return 0;
}

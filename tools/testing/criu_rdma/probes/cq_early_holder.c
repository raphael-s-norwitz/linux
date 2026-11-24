// SPDX-License-Identifier: GPL-2.0
/*
 * Holds a CQ whose next completion comes after a migration's LOAD, for
 * cq_early_replay.sh.
 *
 *   cq_early_holder <ibdev> <status_path> <sgid_index> [timeout]
 *
 * Creates two CQs with no completion channel and arms both:
 *
 *   E  the send and receive CQ of a self-loop RC QP that sends once with
 *      no receive posted. The QP answers itself with RNR NAKs and, with
 *      rnr_retry 7, retries for ever. Once an RX fence drops its loopback
 *      traffic, it waits for ACKs instead, and after retry_cnt 7 ACK
 *      timeouts (4.096 us << [timeout], default 16) it fails with a
 *      "transport retry counter exceeded" completion on E.
 *   Q  no traffic
 *
 * Writes "READY E=0x<cqn> Q=0x<cqn> QP=0x<qpn>" to the status file, then
 * waits:
 *
 *   SIGUSR1  poll both CQs once and write "OK E=<status>" if E holds the
 *            failed send and Q is empty, else "FAIL: ..."
 *   SIGUSR2  post the receive the send is waiting for and poll E for up
 *            to 10 s, writing "ALIVE <completions>" if the send then
 *            completes, else "DEAD <completions>"
 *   SIGTERM  exit
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <infiniband/mlx5dv.h>
#include <infiniband/verbs.h>

static struct ibv_context *ctx;
static const char *status_path;
static volatile sig_atomic_t want_check, want_kick, terminate;

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

static int connect_self(struct ibv_qp *qp, int sgid_index, int timeout)
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
	a.min_rnr_timer = 31;
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
	a.timeout = timeout;
	a.retry_cnt = 7;
	a.rnr_retry = 7;
	a.max_rd_atomic = 1;
	if (ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
			  IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC))
		return fprintf(stderr, "RTS: %s\n", strerror(errno)), -1;
	return 0;
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
	else if (sig == SIGUSR2)
		want_kick = 1;
	else
		terminate = 1;
}

/* Is the send still being retried? Give it the receive it waits for. */
static void kick(struct ibv_qp *qp, struct ibv_cq *e)
{
	struct ibv_recv_wr rwr = { .wr_id = 2 }, *brw;
	struct timespec t0, t, nap = { .tv_nsec = 10 * 1000 * 1000 };
	char seen[256] = "";
	bool sent = false;
	struct ibv_wc wc;
	int n;

	if (ibv_post_recv(qp, &rwr, &brw)) {
		status("DEAD post_recv: %s", strerror(errno));
		return;
	}
	clock_gettime(CLOCK_MONOTONIC, &t0);
	t = t0;
	do {
		n = ibv_poll_cq(e, 1, &wc);
		if (n < 0) {
			status("DEAD poll failed %s", seen);
			return;
		}
		if (n) {
			snprintf(seen + strlen(seen), sizeof(seen) - strlen(seen), " %s=%s",
				 wc.wr_id == 1 ? "send" : "recv", ibv_wc_status_str(wc.status));
			sent |= wc.wr_id == 1;
			continue;
		}
		nanosleep(&nap, NULL);
		clock_gettime(CLOCK_MONOTONIC, &t);
	} while (!sent && t.tv_sec - t0.tv_sec < 10);
	status("%s%s", sent ? "ALIVE" : "DEAD", seen[0] ? seen : " no completion");
}

static void check(struct ibv_cq *e, struct ibv_cq *q)
{
	struct ibv_wc wc;
	int n;

	n = ibv_poll_cq(q, 1, &wc);
	if (n != 0) {
		status("FAIL: Q %s", n < 0 ? "poll failed" : "not empty");
		return;
	}
	n = ibv_poll_cq(e, 1, &wc);
	if (n < 0) {
		status("FAIL: E poll failed");
		return;
	}
	if (n == 0) {
		status("FAIL: E empty, the send has not completed");
		return;
	}
	if (wc.wr_id != 1 || wc.status == IBV_WC_SUCCESS) {
		status("FAIL: E wr_id %llu status %s", (unsigned long long)wc.wr_id,
		       ibv_wc_status_str(wc.status));
		return;
	}
	status("OK E=%s", ibv_wc_status_str(wc.status));
}

int main(int argc, char **argv)
{
	struct sigaction sa = { .sa_handler = on_sig };
	struct ibv_send_wr swr = { .wr_id = 1, .opcode = IBV_WR_SEND,
				   .send_flags = IBV_SEND_SIGNALED }, *bsw;
	struct ibv_qp_init_attr qia = {
		.qp_type = IBV_QPT_RC,
		.cap = { .max_send_wr = 1, .max_recv_wr = 1, .max_send_sge = 1, .max_recv_sge = 1 },
	};
	uint32_t ecqn, qcqn;
	struct ibv_device **list;
	struct ibv_cq *e, *q;
	struct ibv_wc wc;
	sigset_t block, wait;
	struct ibv_pd *pd;
	struct ibv_qp *qp;
	int i, n, timeout;

	if (argc != 4 && argc != 5)
		return fprintf(stderr, "usage: %s <ibdev> <status> <sgid_index> [timeout]\n",
			       argv[0]), 2;
	status_path = argv[2];
	timeout = argc == 5 ? atoi(argv[4]) : 16;

	list = ibv_get_device_list(&n);
	for (i = 0; list && i < n; i++)
		if (!strcmp(ibv_get_device_name(list[i]), argv[1]))
			ctx = ibv_open_device(list[i]);
	if (!ctx)
		return fprintf(stderr, "cannot open %s\n", argv[1]), 2;
	e = ibv_create_cq(ctx, 16, NULL, NULL, 0);
	q = ibv_create_cq(ctx, 16, NULL, NULL, 0);
	if (!e || !q || cqn(e, &ecqn) || cqn(q, &qcqn))
		return fprintf(stderr, "CQs: %s\n", strerror(errno)), 2;
	pd = ibv_alloc_pd(ctx);
	qia.send_cq = e;
	qia.recv_cq = e;
	qp = pd ? ibv_create_qp(pd, &qia) : NULL;
	if (!qp)
		return fprintf(stderr, "pd/qp: %s\n", strerror(errno)), 2;
	if (connect_self(qp, atoi(argv[3]), timeout))
		return 2;
	if (ibv_req_notify_cq(e, 0) || ibv_req_notify_cq(q, 0))
		return fprintf(stderr, "arm: %s\n", strerror(errno)), 2;
	if (ibv_post_send(qp, &swr, &bsw))
		return fprintf(stderr, "post: %s\n", strerror(errno)), 2;
	if (ibv_poll_cq(e, 1, &wc) != 0) {
		status("FAIL: E completed before the dump: %s", ibv_wc_status_str(wc.status));
		return 2;
	}

	/* Signals stay blocked except inside sigsuspend(), so none is lost between checks. */
	sigemptyset(&block);
	sigaddset(&block, SIGUSR1);
	sigaddset(&block, SIGUSR2);
	sigaddset(&block, SIGTERM);
	sigaddset(&block, SIGINT);
	sigprocmask(SIG_BLOCK, &block, &wait);
	sigdelset(&wait, SIGUSR1);
	sigdelset(&wait, SIGUSR2);
	sigdelset(&wait, SIGTERM);
	sigdelset(&wait, SIGINT);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	printf("READY pid=%d E=0x%x Q=0x%x QP=0x%x\n", getpid(), ecqn, qcqn, qp->qp_num);
	fflush(stdout);
	status("READY E=0x%x Q=0x%x QP=0x%x", ecqn, qcqn, qp->qp_num);

	while (!terminate) {
		if (want_check) {
			want_check = 0;
			check(e, q);
		}
		if (want_kick) {
			want_kick = 0;
			kick(qp, e);
		}
		if (!terminate && !want_check && !want_kick)
			sigsuspend(&wait);
	}
	ibv_destroy_qp(qp);
	ibv_dealloc_pd(pd);
	ibv_destroy_cq(e);
	ibv_destroy_cq(q);
	ibv_close_device(ctx);
	return 0;
}

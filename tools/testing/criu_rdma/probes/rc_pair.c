// SPDX-License-Identifier: GPL-2.0
/*
 * RC QP pairs between two RDMA devices in one process, driven from stdin.
 *
 *   rc_pair <ibdev_a> <gid_index_a> <ibdev_b> <gid_index_b>
 *
 * Commands, one per line; each answers with one line on stdout:
 *   connect <n>      create pair n (0..15): QP on A <-> QP on B, both RTS
 *   send <n> AB|BA   post a receive on the target, a zero-length SEND on
 *                    the source, and poll both CQs for up to 5 s
 *   state <n>        QP states of pair n (RTS=3, ERR=6)
 *   gid A|B <idx>    use GID <idx> for that side's later connects
 *   quit
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <infiniband/verbs.h>

#define NPAIRS 16

struct side {
	struct ibv_context *ctx;
	struct ibv_pd *pd;
	struct ibv_cq *cq;
	int gidx;
	union ibv_gid gid;
};

static struct side sa, sb;
static struct ibv_qp *qa[NPAIRS], *qb[NPAIRS];

static int open_side(struct side *s, const char *name, int gidx)
{
	struct ibv_device **list;
	int i, n;

	list = ibv_get_device_list(&n);
	for (i = 0; list && i < n; i++)
		if (!strcmp(ibv_get_device_name(list[i]), name))
			s->ctx = ibv_open_device(list[i]);
	if (!s->ctx)
		return fprintf(stderr, "cannot open %s\n", name), -1;
	s->pd = ibv_alloc_pd(s->ctx);
	s->cq = s->pd ? ibv_create_cq(s->ctx, 64, NULL, NULL, 0) : NULL;
	s->gidx = gidx;
	if (!s->cq || ibv_query_gid(s->ctx, 1, gidx, &s->gid))
		return fprintf(stderr, "%s: pd/cq/gid: %s\n", name, strerror(errno)), -1;
	return 0;
}

static int to_rts(struct ibv_qp *qp, struct side *me, struct side *peer, uint32_t dqpn)
{
	struct ibv_qp_attr a;

	memset(&a, 0, sizeof(a));
	a.qp_state = IBV_QPS_INIT;
	a.port_num = 1;
	a.qp_access_flags = IBV_ACCESS_LOCAL_WRITE;
	if (ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS))
		return -1;
	memset(&a, 0, sizeof(a));
	a.qp_state = IBV_QPS_RTR;
	a.path_mtu = IBV_MTU_1024;
	a.dest_qp_num = dqpn;
	a.max_dest_rd_atomic = 1;
	a.min_rnr_timer = 12;
	a.ah_attr.is_global = 1;
	a.ah_attr.grh.dgid = peer->gid;
	a.ah_attr.grh.sgid_index = me->gidx;
	a.ah_attr.grh.hop_limit = 64;
	a.ah_attr.port_num = 1;
	if (ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
			  IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER))
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

static struct ibv_qp *mkqp(struct side *s)
{
	struct ibv_qp_init_attr qia = {
		.send_cq = s->cq, .recv_cq = s->cq, .qp_type = IBV_QPT_RC,
		.cap = { .max_send_wr = 8, .max_recv_wr = 8, .max_send_sge = 1, .max_recv_sge = 1 },
	};

	return ibv_create_qp(s->pd, &qia);
}

static int qp_state(struct ibv_qp *qp)
{
	struct ibv_qp_attr a = {};
	struct ibv_qp_init_attr ia = {};

	if (ibv_query_qp(qp, &a, IBV_QP_STATE, &ia))
		return -errno;
	return a.qp_state;
}

/* Poll @cq for the completion with @wr_id; other completions are reported and dropped. */
static const char *wait_wc(struct ibv_cq *cq, uint64_t wr_id, struct timespec *t0)
{
	static char buf[64];
	struct ibv_wc wc;
	struct timespec t;

	for (;;) {
		if (ibv_poll_cq(cq, 1, &wc) == 1) {
			if (wc.wr_id != wr_id)
				continue;
			if (wc.status == IBV_WC_SUCCESS)
				return "ok";
			snprintf(buf, sizeof(buf), "%s", ibv_wc_status_str(wc.status));
			return buf;
		}
		clock_gettime(CLOCK_MONOTONIC, &t);
		if (t.tv_sec - t0->tv_sec >= 5)
			return "none";
	}
}

static void do_send(int n, int ab)
{
	static uint64_t seq = 1;
	struct ibv_qp *src = ab ? qa[n] : qb[n], *dst = ab ? qb[n] : qa[n];
	struct ibv_cq *scq = ab ? sa.cq : sb.cq, *dcq = ab ? sb.cq : sa.cq;
	uint64_t rid = seq++, sid = seq++;
	struct ibv_recv_wr rwr = { .wr_id = rid }, *brw;
	struct ibv_send_wr swr = { .wr_id = sid, .opcode = IBV_WR_SEND,
				   .send_flags = IBV_SEND_SIGNALED }, *bsw;
	const char *s, *r;
	struct timespec t0;

	if (ibv_post_recv(dst, &rwr, &brw)) {
		printf("send %d %s: post_recv: %s\n", n, ab ? "AB" : "BA", strerror(errno));
		return;
	}
	if (ibv_post_send(src, &swr, &bsw)) {
		printf("send %d %s: post_send: %s\n", n, ab ? "AB" : "BA", strerror(errno));
		return;
	}
	clock_gettime(CLOCK_MONOTONIC, &t0);
	s = wait_wc(scq, sid, &t0);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	r = wait_wc(dcq, rid, &t0);
	printf("send %d %s: sender=%s receiver=%s states A=%d B=%d\n", n, ab ? "AB" : "BA",
	       s, r, qp_state(qa[n]), qp_state(qb[n]));
}

int main(int argc, char **argv)
{
	char line[128], arg[8];
	int n;

	if (argc != 5)
		return fprintf(stderr, "usage: %s <ibdev_a> <gidx_a> <ibdev_b> <gidx_b>\n", argv[0]), 2;
	if (open_side(&sa, argv[1], atoi(argv[2])) || open_side(&sb, argv[3], atoi(argv[4])))
		return 2;
	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("ready\n");

	while (fgets(line, sizeof(line), stdin)) {
		if (sscanf(line, "connect %d", &n) == 1 && n >= 0 && n < NPAIRS) {
			qa[n] = mkqp(&sa);
			qb[n] = mkqp(&sb);
			if (!qa[n] || !qb[n] || to_rts(qa[n], &sa, &sb, qb[n]->qp_num) ||
			    to_rts(qb[n], &sb, &sa, qa[n]->qp_num))
				printf("connect %d: failed: %s\n", n, strerror(errno));
			else
				printf("connect %d: ok qpn A=0x%x B=0x%x\n", n, qa[n]->qp_num, qb[n]->qp_num);
		} else if (sscanf(line, "send %d %2s", &n, arg) == 2 && n >= 0 && n < NPAIRS && qa[n]) {
			do_send(n, !strcmp(arg, "AB"));
		} else if (sscanf(line, "state %d", &n) == 1 && n >= 0 && n < NPAIRS && qa[n]) {
			printf("state %d: A=%d B=%d\n", n, qp_state(qa[n]), qp_state(qb[n]));
		} else if (sscanf(line, "gid %2s %d", arg, &n) == 2) {
			struct side *s = arg[0] == 'A' ? &sa : &sb;

			s->gidx = n;
			if (ibv_query_gid(s->ctx, 1, n, &s->gid))
				printf("gid %s %d: %s\n", arg, n, strerror(errno));
			else
				printf("gid %s %d: ok\n", arg, n);
		} else if (!strncmp(line, "quit", 4)) {
			break;
		} else {
			printf("bad command: %s", line);
		}
	}
	return 0;
}

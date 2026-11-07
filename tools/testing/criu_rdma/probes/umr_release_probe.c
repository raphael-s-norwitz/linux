// SPDX-License-Identifier: GPL-2.0
/*
 * Check MLX5_VFMIG_RX_FENCE_F_IB_RELEASE on one VF:
 *   1. reg+dereg a 4 KiB MR, so mlx5_ib creates its UMR QP and CQ and
 *      leaves one idle pooled mkey;
 *   2. raise the RX fence with F_IB_RELEASE; mlx5_ib's QP and CQ count
 *      must drop back to the baseline taken before step 1;
 *   3. reg+dereg again; the UMR QP and CQ must come back and the MR work;
 *   4. lift the fence.
 *
 * Usage: umr_release_probe <pf-bdf> <vf-id> <vf-ibdev>
 * Build: make -C tools/testing/criu_rdma probes
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <infiniband/verbs.h>
#include "../../../../include/uapi/linux/mlx5_vfmig.h"

static int count_kernel(const char *ibdev, const char *kind)
{
	char cmd[256], line[1024];
	int n = 0;
	FILE *p;

	/* The UMR QP is a driver-type QP: listed only with -dd, by link. */
	if (!strcmp(kind, "qp"))
		snprintf(cmd, sizeof(cmd), "rdma -dd res show qp link %s", ibdev);
	else
		snprintf(cmd, sizeof(cmd), "rdma res show cq dev %s", ibdev);
	p = popen(cmd, "r");
	if (!p)
		return -1;
	while (fgets(line, sizeof(line), p)) {
		if (strstr(line, "[mlx5_ib]")) {
			n++;
			printf("    %s", line);
		}
	}
	pclose(p);
	return n;
}

static int report(const char *ibdev, const char *when, int *qp, int *cq)
{
	printf("  %s:\n", when);
	*qp = count_kernel(ibdev, "qp");
	*cq = count_kernel(ibdev, "cq");
	printf("  %s: %d mlx5_ib QP(s), %d mlx5_ib CQ(s)\n", when, *qp, *cq);
	return *qp < 0 || *cq < 0;
}

static int reg_dereg(struct ibv_pd *pd, void *buf)
{
	struct ibv_mr *mr;

	mr = ibv_reg_mr(pd, buf, 4096, IBV_ACCESS_LOCAL_WRITE |
			IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
	if (!mr) {
		fprintf(stderr, "ibv_reg_mr: %s\n", strerror(errno));
		return 1;
	}
	printf("  reg_mr lkey 0x%x\n", mr->lkey);
	if (ibv_dereg_mr(mr)) {
		fprintf(stderr, "ibv_dereg_mr failed\n");
		return 1;
	}
	return 0;
}

static int fence(int fd, unsigned int vf, struct mlx5_vfmig_rx_fence *f,
		 unsigned int op, unsigned int flags)
{
	f->vf_id = vf;
	f->op = op;
	f->flags = flags;
	f->reserved = 0;
	if (ioctl(fd, MLX5_VFMIG_IOC_RX_FENCE, f)) {
		fprintf(stderr, "RX_FENCE %s: %s\n",
			op == MLX5_VFMIG_RX_FENCE_RAISE ? "raise" : "lift",
			strerror(errno));
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct mlx5_vfmig_rx_fence f = {};
	struct ibv_device **list;
	struct ibv_context *ctx = NULL;
	struct ibv_pd *pd;
	int qp, cq, qp0, cq0, fd, i, fail = 0;
	unsigned int vf;
	char path[128];
	void *buf;

	if (argc != 4) {
		fprintf(stderr, "usage: %s <pf-bdf> <vf-id> <vf-ibdev>\n", argv[0]);
		return 2;
	}
	vf = strtoul(argv[2], NULL, 0);

	list = ibv_get_device_list(NULL);
	for (i = 0; list && list[i]; i++)
		if (!strcmp(ibv_get_device_name(list[i]), argv[3]))
			ctx = ibv_open_device(list[i]);
	if (!ctx) {
		fprintf(stderr, "cannot open %s\n", argv[3]);
		return 1;
	}
	pd = ibv_alloc_pd(ctx);
	buf = aligned_alloc(4096, 4096);
	if (!pd || !buf) {
		fprintf(stderr, "alloc_pd/buf failed\n");
		return 1;
	}
	snprintf(path, sizeof(path), "/dev/mlx5_vfmig/%s", argv[1]);
	fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return 1;
	}

	/*
	 * GSI has mlx5_ib CQs too, so count against a baseline. The UMR QP
	 * may not be listed at all; the QP count is only reported.
	 */
	if (report(argv[3], "baseline", &qp0, &cq0))
		return 1;

	printf("step 1: first MR on the VF\n");
	if (reg_dereg(pd, buf) || report(argv[3], "after first MR", &qp, &cq))
		return 1;
	if (cq != cq0 + 1) {
		printf("FAIL: expected one more mlx5_ib CQ after the first MR\n");
		fail = 1;
	}

	printf("step 2: raise the RX fence with F_IB_RELEASE\n");
	if (fence(fd, vf, &f, MLX5_VFMIG_RX_FENCE_RAISE,
		  MLX5_VFMIG_RX_FENCE_F_IB_RELEASE))
		return 1;
	printf("  returned flags 0x%x (table 0x%x group 0x%x)\n",
	       f.flags, f.table_id, f.group_id);
	if (!(f.flags & MLX5_VFMIG_RX_FENCE_F_IB_RELEASE)) {
		printf("FAIL: kernel cleared F_IB_RELEASE\n");
		fail = 1;
	}
	if (report(argv[3], "after release", &qp, &cq))
		return 1;
	if (cq != cq0) {
		printf("FAIL: UMR CQ still present after release\n");
		fail = 1;
	}

	printf("step 3: MR after the release\n");
	if (reg_dereg(pd, buf) || report(argv[3], "after second MR", &qp, &cq))
		fail = 1;
	else if (cq != cq0 + 1) {
		printf("FAIL: UMR CQ not recreated\n");
		fail = 1;
	}

	printf("step 4: lift the fence\n");
	f.flags = 0;
	if (fence(fd, vf, &f, MLX5_VFMIG_RX_FENCE_LIFT, 0))
		fail = 1;

	printf("step 5: LIFT with F_IB_RELEASE must be refused\n");
	f.vf_id = vf;
	f.op = MLX5_VFMIG_RX_FENCE_LIFT;
	f.flags = MLX5_VFMIG_RX_FENCE_F_IB_RELEASE;
	if (!ioctl(fd, MLX5_VFMIG_IOC_RX_FENCE, &f) || errno != EINVAL) {
		printf("FAIL: LIFT with F_IB_RELEASE not refused with EINVAL\n");
		fail = 1;
	}

	close(fd);
	ibv_dealloc_pd(pd);
	ibv_close_device(ctx);
	ibv_free_device_list(list);
	printf("%s\n", fail ? "FAIL" : "PASS");
	return fail;
}

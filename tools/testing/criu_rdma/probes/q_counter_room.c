// SPDX-License-Identifier: GPL-2.0
/*
 * Count the queue counters a VF can still allocate: ALLOC_Q_COUNTER through
 * DEVX until firmware refuses, print the number, then free them all. On a
 * VF that leaks one counter per migration the number drops by one per hop;
 * with mlx5_ib adopting the source's counter it stays the same.
 *
 * Usage: q_counter_room <ibdev>	(needs CAP_NET_RAW for DEVX)
 * Build: make -C tools/testing/criu_rdma probes
 */
#include <endian.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <infiniband/mlx5dv.h>
#include <infiniband/verbs.h>

#define MLX5_CMD_OP_ALLOC_Q_COUNTER	0x771
#define MAX_COUNTERS			1024

static struct ibv_context *open_ibdev(const char *name)
{
	struct mlx5dv_context_attr attr = {
		.flags = MLX5DV_CONTEXT_FLAGS_DEVX,
	};
	struct ibv_device **list;
	struct ibv_context *ctx = NULL;
	int i, n;

	list = ibv_get_device_list(&n);
	if (!list)
		return NULL;
	for (i = 0; i < n; i++)
		if (!strcmp(ibv_get_device_name(list[i]), name))
			ctx = mlx5dv_open_device(list[i], &attr);
	ibv_free_device_list(list);
	return ctx;
}

int main(int argc, char **argv)
{
	static struct mlx5dv_devx_obj *objs[MAX_COUNTERS];
	uint32_t in[4] = {}, out[4];
	struct ibv_context *ctx;
	int first = -1, last = -1;
	int n, i, err = 0;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <ibdev>\n", argv[0]);
		return 2;
	}
	ctx = open_ibdev(argv[1]);
	if (!ctx) {
		fprintf(stderr, "%s: cannot open a DEVX context: %s\n", argv[1],
			strerror(errno));
		return 1;
	}

	in[0] = htobe32(MLX5_CMD_OP_ALLOC_Q_COUNTER << 16);
	for (n = 0; n < MAX_COUNTERS; n++) {
		memset(out, 0, sizeof(out));
		objs[n] = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out,
						 sizeof(out));
		if (!objs[n]) {
			err = errno;
			break;
		}
		last = be32toh(out[2]) & 0xff;
		if (first < 0)
			first = last;
	}
	for (i = 0; i < n; i++)
		mlx5dv_devx_obj_destroy(objs[i]);
	ibv_close_device(ctx);

	if (n == MAX_COUNTERS) {
		fprintf(stderr, "%s: allocated %d counters without a refusal\n",
			argv[1], n);
		return 1;
	}
	if (!n) {
		fprintf(stderr, "%s: the first ALLOC_Q_COUNTER failed: %s\n",
			argv[1], strerror(err));
		return 1;
	}
	printf("%s: room for %d queue counters (ids 0x%x..0x%x), stopped by %s\n",
	       argv[1], n, first, last, strerror(err));
	return 0;
}

// SPDX-License-Identifier: GPL-2.0
/*
 *   vfmig_fence <pf_bdf> raise <vf> [eth]                 prints "<table_id> <group_id>"
 *   vfmig_fence <pf_bdf> lift <vf> <table> <group> [eth]
 *
 * "eth" passes MLX5_VFMIG_RX_FENCE_F_ETH_SUSPEND: raise also suspends the
 * VF's mlx5e, lift resumes it first. Whether raise suspended it goes to
 * stderr.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../../../../include/uapi/linux/mlx5_vfmig.h"

int main(int argc, char **argv)
{
	struct mlx5_vfmig_rx_fence f = {};
	char path[128];
	int fd, nargs;

	if (argc < 4)
		return fprintf(stderr, "usage: see source\n"), 2;
	snprintf(path, sizeof(path), "/dev/mlx5_vfmig/%s", argv[1]);
	fd = open(path, O_RDWR);
	if (fd < 0)
		return fprintf(stderr, "open %s: %s\n", path, strerror(errno)), 1;
	f.vf_id = strtoul(argv[3], NULL, 0);
	if (!strcmp(argv[2], "raise")) {
		f.op = MLX5_VFMIG_RX_FENCE_RAISE;
		nargs = 4;
	} else if (!strcmp(argv[2], "lift") && argc >= 6) {
		f.op = MLX5_VFMIG_RX_FENCE_LIFT;
		f.table_id = strtoul(argv[4], NULL, 0);
		f.group_id = strtoul(argv[5], NULL, 0);
		nargs = 6;
	} else {
		return fprintf(stderr, "usage: see source\n"), 2;
	}
	if (argc == nargs + 1 && !strcmp(argv[nargs], "eth"))
		f.flags = MLX5_VFMIG_RX_FENCE_F_ETH_SUSPEND;
	else if (argc != nargs)
		return fprintf(stderr, "usage: see source\n"), 2;
	if (ioctl(fd, MLX5_VFMIG_IOC_RX_FENCE, &f) < 0)
		return fprintf(stderr, "RX_FENCE: %s\n", strerror(errno)), 1;
	if (f.op == MLX5_VFMIG_RX_FENCE_RAISE) {
		printf("0x%x 0x%x\n", f.table_id, f.group_id);
		if (argc == nargs + 1)
			fprintf(stderr, "mlx5e %s\n",
				f.flags & MLX5_VFMIG_RX_FENCE_F_ETH_SUSPEND ?
				"suspended" : "not bound, nothing suspended");
	}
	return 0;
}

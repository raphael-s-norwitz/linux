# SPDX-License-Identifier: GPL-2.0
#
# Out-of-band validators for mlx5 vfmig. Build with:
#   make -C tools/testing/criu_rdma
#   make -C tools/testing/criu_rdma probes	# also the libibverbs probes

CC	?= gcc
CFLAGS	?= -O2 -g
CFLAGS	+= -Wall -Wextra

UAPI	:= ../../../include/uapi/linux/mlx5_vfmig.h

PROGS	:= tools/mlx5_vfmig probes/vfmig_fence
VERBS_PROGS := probes/rc_pair probes/umr_release_probe

all: $(PROGS)

probes: $(PROGS) $(VERBS_PROGS)

tools/mlx5_vfmig: tools/mlx5_vfmig.c $(UAPI)
	$(CC) $(CFLAGS) -o $@ $<

probes/vfmig_fence: probes/vfmig_fence.c $(UAPI)
	$(CC) $(CFLAGS) -o $@ $<

probes/rc_pair: probes/rc_pair.c
	$(CC) $(CFLAGS) -o $@ $< -libverbs

probes/umr_release_probe: probes/umr_release_probe.c $(UAPI)
	$(CC) $(CFLAGS) -o $@ $< -libverbs

clean:
	rm -f $(PROGS) $(VERBS_PROGS)

.PHONY: all probes clean

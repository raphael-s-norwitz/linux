# SPDX-License-Identifier: GPL-2.0
#
# Out-of-band validators for mlx5 vfmig. Build with:
#   make -C tools/testing/criu_rdma

CC	?= gcc
CFLAGS	?= -O2 -g
CFLAGS	+= -Wall -Wextra

PROGS	:= tools/mlx5_vfmig

all: $(PROGS)

tools/mlx5_vfmig: tools/mlx5_vfmig.c ../../../include/uapi/linux/mlx5_vfmig.h
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(PROGS)

.PHONY: all clean

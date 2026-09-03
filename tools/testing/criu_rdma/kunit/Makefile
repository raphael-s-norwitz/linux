# SPDX-License-Identifier: GPL-2.0
#
# Out-of-tree KUnit module for fixed-address IOVA reservation.
#
# This test is deliberately kept OUT of drivers/iommu so the in-tree
# subsystem stays free of project test wiring (no Kconfig, no Makefile
# edits). It builds as an external kernel module against a KUnit-enabled
# kernel tree:
#
#     make -C /path/to/linux M=$(pwd) modules
#
# or simply (KDIR defaults to the tree this file ships in):
#
#     make
#
# The target kernel only needs CONFIG_KUNIT and CONFIG_IOMMU_IOVA (both
# satisfied by an ordinary dma-iommu kernel with KUnit enabled); no
# in-tree Kconfig symbol is required.
#
# Run the suite by loading the module -- results appear in dmesg and under
# /sys/kernel/debug/kunit/iova_fixed/results:
#
#     sudo insmod iova_kunit.ko
#     sudo rmmod  iova_kunit
#
ifneq ($(KERNELRELEASE),)

obj-m += iova_kunit.o

else

# Default to the kernel source tree this file lives in (…/linux).
KDIR ?= $(abspath $(CURDIR)/../../../..)
PWD  := $(CURDIR)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

endif

# SPDX-License-Identifier: GPL-2.0
#
# Out-of-tree KUnit modules for fixed-address IOVA reservation:
#   iova_kunit  - pure allocator invariant on a synthetic iova_domain.
#   arena_kunit - DMA-API wrappers + dma_iova_link on a real DMA-IOMMU
#                 device (pass bdf=<DDDD:BB:DD.F>).
#
# These tests are deliberately kept OUT of drivers/iommu so the in-tree
# subsystem stays free of project test wiring (no Kconfig, no Makefile
# edits). They build as external kernel modules against a KUnit-enabled
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
# Run a suite by loading its module -- results appear in dmesg and under
# /sys/kernel/debug/kunit/<suite>/results:
#
#     sudo insmod iova_kunit.ko
#     sudo rmmod  iova_kunit
#
#     sudo insmod arena_kunit.ko bdf=0000:03:00.1
#     sudo rmmod  arena_kunit
#
ifneq ($(KERNELRELEASE),)

obj-m += iova_kunit.o
obj-m += arena_kunit.o

else

# Default to the kernel source tree this file lives in (…/linux).
KDIR ?= $(abspath $(CURDIR)/../../../..)
PWD  := $(CURDIR)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

endif

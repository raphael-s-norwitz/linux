// SPDX-License-Identifier: GPL-2.0-only
/*
 * Device-level KUnit for the fixed-address IOVA arena on a real IOMMU.
 *
 * iova_kunit.c proves the pure allocator invariant on a synthetic
 * iova_domain. This suite proves the next layer up -- the DMA-API
 * wrappers dma_iova_alloc_fixed()/dma_iova_free_fixed() plus dma_iova_link()
 * -- against a *real* DMA-IOMMU device, so the reserve/link/probe/flush
 * mechanics are validated on actual hardware page tables before any driver
 * (VFMIG) cuts its buffers over to the arena.
 *
 * It runs against any device that is on the DMA-IOMMU path; a VF works
 * well. Pass the device by BDF and (optionally) tune the window:
 *
 *     make -C tools/testing/criu_rdma/kunit
 *     sudo insmod arena_kunit.ko bdf=0000:03:00.1
 *     sudo insmod arena_kunit.ko bdf=0000:03:00.1 \
 *                 arena_base=0x100000000 arena_size=0x100000
 *
 * Results appear in dmesg and under
 * /sys/kernel/debug/kunit/arena_device/results.
 *
 * With no bdf= the whole suite skips (there is nothing hardware-independent
 * to assert here; that is iova_kunit's job).
 *
 * See tools/testing/criu_rdma/plans/dma_iova_reserve.md (Sections 15-16).
 */

#include <kunit/test.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/iommu-dma.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/sizes.h>

static char *bdf;
module_param(bdf, charp, 0444);
MODULE_PARM_DESC(bdf, "PCI BDF of a DMA-IOMMU device, e.g. 0000:03:00.1");

/*
 * A 4 GiB base mirrors the proposed VFMIG arena base; the exact value is
 * irrelevant to the mechanics, but it must land inside the device's IOMMU
 * aperture and on an otherwise-free hole. 2 MiB is large enough to force a
 * multi-page link while staying trivially inside any real aperture.
 */
static unsigned long long arena_base = 4ULL << 30;
module_param(arena_base, ullong, 0444);
MODULE_PARM_DESC(arena_base, "fixed IOVA base to reserve (default 4 GiB)");

static unsigned long arena_size = 2UL << 20;
module_param(arena_size, ulong, 0444);
MODULE_PARM_DESC(arena_size, "size of the reserved window (default 2 MiB)");

#define MAP_BURST	256

struct arena_ctx {
	struct pci_dev *pdev;
	struct device *dev;
};

static int arena_test_init(struct kunit *test)
{
	struct arena_ctx *ctx;
	unsigned int domain, bus, slot, func;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	test->priv = ctx;

	if (!bdf)
		return 0;	/* tests kunit_skip() themselves */

	if (sscanf(bdf, "%x:%x:%x.%x", &domain, &bus, &slot, &func) != 4)
		return 0;

	ctx->pdev = pci_get_domain_bus_and_slot(domain, bus,
						PCI_DEVFN(slot, func));
	if (!ctx->pdev)
		return 0;

	ctx->dev = &ctx->pdev->dev;
	return 0;
}

static void arena_test_exit(struct kunit *test)
{
	struct arena_ctx *ctx = test->priv;

	if (ctx && ctx->pdev)
		pci_dev_put(ctx->pdev);
}

/* Resolve the device or skip the test with a precise reason. */
static struct device *arena_dev(struct kunit *test)
{
	struct arena_ctx *ctx = test->priv;

	if (!bdf)
		kunit_skip(test, "pass bdf=<DDDD:BB:DD.F> of a DMA-IOMMU device");
	if (!ctx->dev)
		kunit_skip(test, "device '%s' not found", bdf);
	if (!use_dma_iommu(ctx->dev))
		kunit_skip(test, "device '%s' is not on the DMA-IOMMU path", bdf);
	return ctx->dev;
}

/*
 * Reserve the window, or skip if the environment cannot host it (aperture
 * or a pre-existing occupant). A genuine reservation must return 0.
 */
static void arena_reserve_or_skip(struct kunit *test, struct device *dev,
				  struct dma_iova_state *st)
{
	int ret = dma_iova_alloc_fixed(dev, st, arena_base, arena_size);

	if (ret == -EBUSY)
		kunit_skip(test,
			   "arena_base=0x%llx size=0x%lx is occupied; pick another base",
			   arena_base, arena_size);
	if (ret == -EINVAL)
		kunit_skip(test,
			   "arena_base=0x%llx size=0x%lx misaligned for this IOMMU granule",
			   arena_base, arena_size);
	KUNIT_ASSERT_EQ_MSG(test, ret, 0,
			    "dma_iova_alloc_fixed failed: %d", ret);
}

static bool in_arena(dma_addr_t a)
{
	return a >= arena_base && a < arena_base + arena_size;
}

/* Reserve succeeds; an overlapping second reserve is rejected with -EBUSY. */
static void test_arena_reserve_ebusy(struct kunit *test)
{
	struct device *dev = arena_dev(test);
	struct dma_iova_state st = {}, st2 = {};

	arena_reserve_or_skip(test, dev, &st);

	KUNIT_EXPECT_EQ_MSG(test,
			    dma_iova_alloc_fixed(dev, &st2, arena_base, arena_size),
			    -EBUSY,
			    "overlapping reserve was not rejected");

	dma_iova_free_fixed(dev, &st);
}

/* Ordinary streaming maps must never hand out an IOVA inside the reservation. */
static void test_arena_maps_avoid_range(struct kunit *test)
{
	struct device *dev = arena_dev(test);
	struct dma_iova_state st = {};
	dma_addr_t *addrs;
	struct page *p;
	int i, n = 0;

	p = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, p);

	addrs = kunit_kmalloc_array(test, MAP_BURST, sizeof(*addrs), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, addrs);

	arena_reserve_or_skip(test, dev, &st);

	for (i = 0; i < MAP_BURST; i++) {
		dma_addr_t a = dma_map_page(dev, p, 0, PAGE_SIZE,
					    DMA_BIDIRECTIONAL);

		if (dma_mapping_error(dev, a))
			break;
		addrs[n++] = a;
		KUNIT_EXPECT_FALSE_MSG(test, in_arena(a),
				       "streaming map returned reserved IOVA 0x%llx",
				       (u64)a);
	}
	KUNIT_EXPECT_GT_MSG(test, n, 0, "no streaming map succeeded");

	for (i = 0; i < n; i++)
		dma_unmap_page(dev, addrs[i], PAGE_SIZE, DMA_BIDIRECTIONAL);

	dma_iova_free_fixed(dev, &st);
	__free_page(p);
}

/*
 * Link a page at a chosen offset inside the reservation and confirm the
 * IOMMU page table resolves that exact IOVA to the page; unlink and confirm
 * the translation is gone.
 */
static void test_arena_link_probe(struct kunit *test)
{
	struct device *dev = arena_dev(test);
	struct iommu_domain *dom = iommu_get_domain_for_dev(dev);
	struct dma_iova_state st = {};
	size_t off = PAGE_SIZE;	/* not at the very base, to catch off math */
	dma_addr_t iova;
	struct page *p;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, dom);

	p = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, p);

	arena_reserve_or_skip(test, dev, &st);
	iova = arena_base + off;

	ret = dma_iova_link(dev, &st, page_to_phys(p), off, PAGE_SIZE,
			    DMA_BIDIRECTIONAL, 0);
	KUNIT_ASSERT_EQ_MSG(test, ret, 0, "dma_iova_link failed: %d", ret);
	KUNIT_ASSERT_EQ(test, dma_iova_sync(dev, &st, off, PAGE_SIZE), 0);

	KUNIT_EXPECT_EQ_MSG(test, iommu_iova_to_phys(dom, iova),
			    page_to_phys(p),
			    "IOVA 0x%llx did not resolve to the linked page",
			    (u64)iova);

	dma_iova_unlink(dev, &st, off, PAGE_SIZE, DMA_BIDIRECTIONAL, 0);
	KUNIT_EXPECT_EQ_MSG(test, iommu_iova_to_phys(dom, iova), (phys_addr_t)0,
			    "translation survived unlink at 0x%llx", (u64)iova);

	dma_iova_free_fixed(dev, &st);
	__free_page(p);
}

/*
 * The free is non-caching: after dma_iova_free_fixed() the identical base is
 * immediately reservable again (no lingering rcache/rbtree occupancy).
 */
static void test_arena_free_noncaching(struct kunit *test)
{
	struct device *dev = arena_dev(test);
	struct dma_iova_state st = {};

	arena_reserve_or_skip(test, dev, &st);
	dma_iova_free_fixed(dev, &st);

	KUNIT_EXPECT_EQ_MSG(test,
			    dma_iova_alloc_fixed(dev, &st, arena_base, arena_size),
			    0,
			    "re-reserve after free failed (free was caching)");
	dma_iova_free_fixed(dev, &st);
}

/*
 * Flush correctness: after unlink+free, re-linking the same offset to a
 * DIFFERENT page must resolve to the new page -- a missed IOTLB/flush-queue
 * drain would leave the stale translation behind.
 */
static void test_arena_relink_flush(struct kunit *test)
{
	struct device *dev = arena_dev(test);
	struct iommu_domain *dom = iommu_get_domain_for_dev(dev);
	struct dma_iova_state st = {};
	size_t off = PAGE_SIZE;
	dma_addr_t iova;
	struct page *a, *b;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, dom);

	a = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, a);
	b = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, b);
	KUNIT_ASSERT_NE(test, page_to_phys(a), page_to_phys(b));

	arena_reserve_or_skip(test, dev, &st);
	iova = arena_base + off;

	ret = dma_iova_link(dev, &st, page_to_phys(a), off, PAGE_SIZE,
			    DMA_BIDIRECTIONAL, 0);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test, dma_iova_sync(dev, &st, off, PAGE_SIZE), 0);
	KUNIT_EXPECT_EQ(test, iommu_iova_to_phys(dom, iova), page_to_phys(a));

	dma_iova_unlink(dev, &st, off, PAGE_SIZE, DMA_BIDIRECTIONAL, 0);

	ret = dma_iova_link(dev, &st, page_to_phys(b), off, PAGE_SIZE,
			    DMA_BIDIRECTIONAL, 0);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test, dma_iova_sync(dev, &st, off, PAGE_SIZE), 0);
	KUNIT_EXPECT_EQ_MSG(test, iommu_iova_to_phys(dom, iova), page_to_phys(b),
			    "re-link resolved to a stale page (flush missed)");

	dma_iova_unlink(dev, &st, off, PAGE_SIZE, DMA_BIDIRECTIONAL, 0);
	dma_iova_free_fixed(dev, &st);
	__free_page(b);
	__free_page(a);
}

static struct kunit_case arena_device_test_cases[] = {
	KUNIT_CASE(test_arena_reserve_ebusy),
	KUNIT_CASE(test_arena_maps_avoid_range),
	KUNIT_CASE(test_arena_link_probe),
	KUNIT_CASE(test_arena_free_noncaching),
	KUNIT_CASE(test_arena_relink_flush),
	{}
};

static struct kunit_suite arena_device_test_suite = {
	.name = "arena_device",
	.init = arena_test_init,
	.exit = arena_test_exit,
	.test_cases = arena_device_test_cases,
};

kunit_test_suites(&arena_device_test_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Device-level KUnit for fixed-address IOVA arena on real IOMMU");

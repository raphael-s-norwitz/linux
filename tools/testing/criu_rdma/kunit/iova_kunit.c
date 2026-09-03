// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for fixed-address IOVA reservation.
 *
 * These tests answer one design question: can a caller secure an exact,
 * pre-chosen IOVA range from an iova_domain and be certain the allocator
 * -- both the rbtree slow path (alloc_iova) and the per-CPU magazine
 * fast path (alloc_iova_fast) -- will never hand any of it back out?
 *
 * The subtlety is the rcache. free_iova_fast() caches a freed pfn in a
 * magazine but does NOT remove its rbtree node; the node is only erased
 * when a magazine is drained (depot trim / CPU hotplug / put_iova_domain).
 * So a cached pfn always still occupies its rbtree node, which makes the
 * rbtree the single authoritative occupancy map -- the rcache can only
 * ever return pfns that are already tree-occupied. The characterization
 * cases below prove that invariant, and show that today's reserve_iova()
 * is airtight against both paths but is NOT an exact allocator (it merges
 * over any overlap), which is exactly the gap alloc_iova_fixed() fills.
 *
 * See tools/testing/criu_rdma/plans/dma_iova_reserve.md (Part II).
 */
#include <kunit/test.h>
#include <linux/iova.h>
#include <linux/mm.h>
#include <linux/sizes.h>

/*
 * A 4 GiB base mirrors the proposed VFMIG_ARENA_IOVA_BASE; the exact
 * value is irrelevant to the invariant, but using it documents intent.
 * The window is small (1 MiB with 4 KiB pages) to keep the tests fast.
 */
#define ARENA_BASE	(4ULL << 30)
#define ARENA_PAGES	256UL
#define BASE_PFN	((unsigned long)(ARENA_BASE >> PAGE_SHIFT))
#define TOP_PFN		(BASE_PFN + ARENA_PAGES - 1)
/*
 * Allocate from just above the window so the top-down allocator must step
 * down across it -- forcing the "skip the reserved hole" behaviour.
 */
#define LIMIT_PFN	(TOP_PFN + 128)
/* Enough single-page allocations to march from LIMIT_PFN down past BASE_PFN. */
#define NALLOC		(ARENA_PAGES + 256UL)

struct iova_test_ctx {
	struct iova_domain iovad;
	bool domain_ready;
};

static bool overlaps_arena(unsigned long pfn_lo, unsigned long pfn_hi)
{
	return pfn_lo <= TOP_PFN && pfn_hi >= BASE_PFN;
}

static int iova_test_init(struct kunit *test)
{
	struct iova_test_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);

	KUNIT_ASSERT_EQ(test, iova_cache_get(), 0);
	init_iova_domain(&ctx->iovad, PAGE_SIZE, 1);
	KUNIT_ASSERT_EQ(test, iova_domain_init_rcaches(&ctx->iovad), 0);
	ctx->domain_ready = true;

	test->priv = ctx;
	return 0;
}

static void iova_test_exit(struct kunit *test)
{
	struct iova_test_ctx *ctx = test->priv;

	if (ctx && ctx->domain_ready)
		put_iova_domain(&ctx->iovad);
	iova_cache_put();
}

/*
 * reserve_iova() must fence the reserved window against the rbtree slow
 * path: allocations that would otherwise march straight through the
 * window are forced to skip it.
 */
static void test_reserve_fences_slow_path(struct kunit *test)
{
	struct iova_test_ctx *ctx = test->priv;
	struct iova *r;
	unsigned long i;

	r = reserve_iova(&ctx->iovad, BASE_PFN, TOP_PFN);
	KUNIT_ASSERT_NOT_NULL(test, r);
	KUNIT_EXPECT_EQ(test, r->pfn_lo, BASE_PFN);
	KUNIT_EXPECT_EQ(test, r->pfn_hi, TOP_PFN);

	for (i = 0; i < NALLOC; i++) {
		struct iova *it = alloc_iova(&ctx->iovad, 1, LIMIT_PFN, false);

		KUNIT_ASSERT_NOT_NULL(test, it);
		KUNIT_EXPECT_FALSE_MSG(test,
				       overlaps_arena(it->pfn_lo, it->pfn_hi),
				       "alloc_iova gave reserved pfn 0x%lx",
				       it->pfn_lo);
	}
}

/*
 * The crux invariant: a pfn freed via free_iova_fast() is cached in a
 * magazine but its rbtree node is NOT removed -- find_iova() still
 * resolves it. This is what makes the rbtree the authoritative occupancy
 * map and the rcache incapable of aliasing a range that has no node.
 */
static void test_cached_iova_retains_rbtree_node(struct kunit *test)
{
	struct iova_test_ctx *ctx = test->priv;
	unsigned long pfn;

	pfn = alloc_iova_fast(&ctx->iovad, 1, LIMIT_PFN, true);
	KUNIT_ASSERT_NE(test, pfn, 0UL);
	KUNIT_EXPECT_NOT_NULL(test, find_iova(&ctx->iovad, pfn));

	/* Fast-free: the pfn goes into a magazine, node stays in the tree. */
	free_iova_fast(&ctx->iovad, pfn, 1);
	KUNIT_EXPECT_NOT_NULL_MSG(test, find_iova(&ctx->iovad, pfn),
				  "cached pfn 0x%lx lost its rbtree node",
				  pfn);
}

/*
 * Because cached pfns keep their nodes, a reserved window is airtight
 * against the fast/rcache path too: churn the rcache hard with the window
 * reserved and confirm no fast allocation ever lands inside it -- neither
 * a fresh rbtree allocation nor a magazine reuse.
 */
static void test_reserve_fences_fast_path(struct kunit *test)
{
	struct iova_test_ctx *ctx = test->priv;
	unsigned long *pfns;
	unsigned long i;
	struct iova *r;

	r = reserve_iova(&ctx->iovad, BASE_PFN, TOP_PFN);
	KUNIT_ASSERT_NOT_NULL(test, r);

	pfns = kunit_kmalloc_array(test, NALLOC, sizeof(*pfns), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pfns);

	/* First burst: populate the tree (and cross the reserved hole). */
	for (i = 0; i < NALLOC; i++) {
		pfns[i] = alloc_iova_fast(&ctx->iovad, 1, LIMIT_PFN, true);
		KUNIT_ASSERT_NE(test, pfns[i], 0UL);
		KUNIT_EXPECT_FALSE_MSG(test, overlaps_arena(pfns[i], pfns[i]),
				       "alloc_iova_fast (rbtree) gave reserved pfn 0x%lx",
				       pfns[i]);
	}

	/* Free them all into the rcache. */
	for (i = 0; i < NALLOC; i++)
		free_iova_fast(&ctx->iovad, pfns[i], 1);

	/* Second burst now services from magazines -- still must skip the hole. */
	for (i = 0; i < NALLOC; i++) {
		unsigned long pfn = alloc_iova_fast(&ctx->iovad, 1, LIMIT_PFN, true);

		KUNIT_ASSERT_NE(test, pfn, 0UL);
		KUNIT_EXPECT_FALSE_MSG(test, overlaps_arena(pfn, pfn),
				       "alloc_iova_fast (rcache) gave reserved pfn 0x%lx",
				       pfn);
	}
}

/*
 * Document the gap alloc_iova_fixed() must close: reserve_iova() is not an
 * exact allocator. Reserving over an already-present (here: cached) range
 * does not create an independently-owned node -- it returns the existing
 * one after adjust/merge. A caller therefore cannot tell it got exactly
 * the range it asked for, nor free it without disturbing the other owner.
 */
static void test_reserve_merges_over_existing(struct kunit *test)
{
	struct iova_test_ctx *ctx = test->priv;
	struct iova *a, *r;
	unsigned long lo, hi;

	a = alloc_iova(&ctx->iovad, 8, LIMIT_PFN, false);
	KUNIT_ASSERT_NOT_NULL(test, a);
	lo = a->pfn_lo;
	hi = a->pfn_hi;

	/* Cache the range: node 'a' stays live in the tree. */
	free_iova_fast(&ctx->iovad, lo, 8);

	r = reserve_iova(&ctx->iovad, lo, hi);
	KUNIT_EXPECT_PTR_EQ_MSG(test, r, a,
				"reserve_iova returned a fresh node, not the merged one");
}

#ifdef IOVA_KUNIT_TEST_ALLOC_FIXED
/*
 * Contract for the proposed alloc_iova_fixed() primitive (see Part II of
 * tools/testing/criu_rdma/plans/dma_iova_reserve.md). Dormant until the
 * primitive lands in the same series -- define IOVA_KUNIT_TEST_ALLOC_FIXED
 * and declare the symbol in <linux/iova.h> to activate. These are the spec
 * the implementation must satisfy:
 *
 *   int alloc_iova_fixed(struct iova_domain *iovad,
 *			  unsigned long pfn_lo, unsigned long pfn_hi);
 *   returns 0 on exact reservation, -EBUSY on any overlap, -ENOMEM on OOM.
 */
static void test_fixed_exact_on_free_range(struct kunit *test)
{
	struct iova_test_ctx *ctx = test->priv;
	struct iova *found;

	KUNIT_EXPECT_EQ(test, alloc_iova_fixed(&ctx->iovad, BASE_PFN, TOP_PFN), 0);
	found = find_iova(&ctx->iovad, BASE_PFN);
	KUNIT_ASSERT_NOT_NULL(test, found);
	KUNIT_EXPECT_EQ(test, found->pfn_lo, BASE_PFN);
	KUNIT_EXPECT_EQ(test, found->pfn_hi, TOP_PFN);
}

static void test_fixed_ebusy_on_overlap(struct kunit *test)
{
	struct iova_test_ctx *ctx = test->priv;

	KUNIT_ASSERT_EQ(test, alloc_iova_fixed(&ctx->iovad, BASE_PFN, TOP_PFN), 0);
	/* Any overlap -- here a subset -- must be rejected outright. */
	KUNIT_EXPECT_EQ(test,
			alloc_iova_fixed(&ctx->iovad, BASE_PFN + 1, TOP_PFN - 1),
			-EBUSY);
}

static void test_fixed_ebusy_on_cached_overlap(struct kunit *test)
{
	struct iova_test_ctx *ctx = test->priv;
	struct iova *a;
	unsigned long lo, hi;

	a = alloc_iova(&ctx->iovad, 8, LIMIT_PFN, false);
	KUNIT_ASSERT_NOT_NULL(test, a);
	lo = a->pfn_lo;
	hi = a->pfn_hi;
	free_iova_fast(&ctx->iovad, lo, 8);	/* cached: node stays */

	KUNIT_EXPECT_EQ(test, alloc_iova_fixed(&ctx->iovad, lo, hi), -EBUSY);
}

static void test_fixed_free_roundtrip(struct kunit *test)
{
	struct iova_test_ctx *ctx = test->priv;
	struct iova *node;

	KUNIT_ASSERT_EQ(test, alloc_iova_fixed(&ctx->iovad, BASE_PFN, TOP_PFN), 0);
	node = find_iova(&ctx->iovad, BASE_PFN);
	KUNIT_ASSERT_NOT_NULL(test, node);
	__free_iova(&ctx->iovad, node);

	/* Independently owned: the exact range is reservable again. */
	KUNIT_EXPECT_EQ(test, alloc_iova_fixed(&ctx->iovad, BASE_PFN, TOP_PFN), 0);
}

/*
 * The positive fencing guarantee for the new primitive: once
 * alloc_iova_fixed() owns the range, the LIVE allocators must never hand
 * any of it out -- neither the rbtree slow path nor the rcache fast path.
 * This is the fixed-primitive analogue of test_reserve_fences_{slow,fast}
 * and the real "nothing can allocate over our reservation" assertion.
 * (There is deliberately no "reserve_iova fails on our range" test:
 * reserve_iova cannot fail -- it merges on overlap -- so the meaningful
 * negative is -EBUSY from alloc_iova_fixed plus this allocator fencing.)
 */
static void test_fixed_fences_allocators(struct kunit *test)
{
	struct iova_test_ctx *ctx = test->priv;
	unsigned long i;

	KUNIT_ASSERT_EQ(test, alloc_iova_fixed(&ctx->iovad, BASE_PFN, TOP_PFN), 0);

	for (i = 0; i < NALLOC; i++) {
		struct iova *it = alloc_iova(&ctx->iovad, 1, LIMIT_PFN, false);
		unsigned long pfn = alloc_iova_fast(&ctx->iovad, 1, LIMIT_PFN, true);

		KUNIT_ASSERT_NOT_NULL(test, it);
		KUNIT_ASSERT_NE(test, pfn, 0UL);
		KUNIT_EXPECT_FALSE_MSG(test, overlaps_arena(it->pfn_lo, it->pfn_hi),
				       "alloc_iova breached fixed reservation at 0x%lx",
				       it->pfn_lo);
		KUNIT_EXPECT_FALSE_MSG(test, overlaps_arena(pfn, pfn),
				       "alloc_iova_fast breached fixed reservation at 0x%lx",
				       pfn);
	}
}

/*
 * The free must truly RELEASE the range to the allocator (non-caching
 * path), not merely mark it not-busy: after free, an ordinary allocation
 * targeted at the window is allowed to land inside it again.
 */
static void test_fixed_free_releases_to_allocator(struct kunit *test)
{
	struct iova_test_ctx *ctx = test->priv;
	struct iova *node;
	unsigned long i;
	bool reused = false;

	KUNIT_ASSERT_EQ(test, alloc_iova_fixed(&ctx->iovad, BASE_PFN, TOP_PFN), 0);
	node = find_iova(&ctx->iovad, BASE_PFN);
	KUNIT_ASSERT_NOT_NULL(test, node);
	__free_iova(&ctx->iovad, node);

	/* With the fence gone, the top-down allocator can reoccupy the hole. */
	for (i = 0; i < NALLOC; i++) {
		struct iova *it = alloc_iova(&ctx->iovad, 1, LIMIT_PFN, false);

		KUNIT_ASSERT_NOT_NULL(test, it);
		if (overlaps_arena(it->pfn_lo, it->pfn_hi)) {
			reused = true;
			break;
		}
	}
	KUNIT_EXPECT_TRUE_MSG(test, reused,
			      "freed fixed range was never reused");
}
#endif /* IOVA_KUNIT_TEST_ALLOC_FIXED */

static struct kunit_case iova_fixed_test_cases[] = {
	KUNIT_CASE(test_reserve_fences_slow_path),
	KUNIT_CASE(test_cached_iova_retains_rbtree_node),
	KUNIT_CASE(test_reserve_fences_fast_path),
	KUNIT_CASE(test_reserve_merges_over_existing),
#ifdef IOVA_KUNIT_TEST_ALLOC_FIXED
	KUNIT_CASE(test_fixed_exact_on_free_range),
	KUNIT_CASE(test_fixed_ebusy_on_overlap),
	KUNIT_CASE(test_fixed_ebusy_on_cached_overlap),
	KUNIT_CASE(test_fixed_free_roundtrip),
	KUNIT_CASE(test_fixed_fences_allocators),
	KUNIT_CASE(test_fixed_free_releases_to_allocator),
#endif
	{}
};

static struct kunit_suite iova_fixed_test_suite = {
	.name = "iova_fixed",
	.init = iova_test_init,
	.exit = iova_test_exit,
	.test_cases = iova_fixed_test_cases,
};

kunit_test_suites(&iova_fixed_test_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit tests for fixed-address IOVA reservation");

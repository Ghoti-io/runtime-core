/**
 * @file
 *
 * The meter and the counting allocator, tested through the internal header.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../../src/b/account_internal.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

namespace {
struct Fixture {
  TrackingAllocator base;
  FakePages pages;
  GRCORE_Meter meter;
  GRCORE_Counting counting;
  Fixture() {
    grcore_meter_init(&meter);
    grcore_counting_init(&counting, &meter, base.get(), &pages.vtable);
  }
  const GRCORE_Allocator & a() { return counting.allocator; }
  void * m(size_t n) { return a().malloc_fn(a().ctx, n); }
  void f(void * p) { a().free_fn(a().ctx, p); }
};
} // namespace

TEST(Account, MallocFreeKeepInUsePeakAndBlocksExact) {
  Fixture x;
  void * p = x.m(100);
  void * q = x.m(28);
  ASSERT_NE(p, nullptr);
  ASSERT_NE(q, nullptr);
  EXPECT_EQ(grcore_meter_in_use(&x.meter), 128u);
  EXPECT_EQ(grcore_meter_blocks(&x.meter), 2u);
  EXPECT_EQ(grcore_meter_peak(&x.meter), 128u);
  x.f(p);
  EXPECT_EQ(grcore_meter_in_use(&x.meter), 28u);
  EXPECT_EQ(grcore_meter_blocks(&x.meter), 1u);
  EXPECT_EQ(grcore_meter_peak(&x.meter), 128u);
  x.f(q);
  EXPECT_EQ(grcore_meter_in_use(&x.meter), 0u);
  EXPECT_EQ(grcore_meter_blocks(&x.meter), 0u);
  EXPECT_EQ(x.base.live, 0);
}

TEST(Account, PayloadIsMaximallyAligned) {
  Fixture x;
  void * p = x.m(1);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % alignof(max_align_t), 0u);
  x.f(p);
}

TEST(Account, CallocZeroesAndCharges) {
  Fixture x;
  auto * p = static_cast<unsigned char *>(x.a().calloc_fn(x.a().ctx, 10, 12));
  ASSERT_NE(p, nullptr);
  for (int i = 0; i < 120; i++) {
    ASSERT_EQ(p[i], 0);
  }
  EXPECT_EQ(grcore_meter_in_use(&x.meter), 120u);
  EXPECT_EQ(grcore_meter_blocks(&x.meter), 1u);
  x.f(p);
  EXPECT_EQ(grcore_meter_in_use(&x.meter), 0u);
}

TEST(Account, CallocOverflowIsNullAndChargesNothing) {
  Fixture x;
  size_t big = std::numeric_limits<size_t>::max() / 2 + 1;
  EXPECT_EQ(x.a().calloc_fn(x.a().ctx, big, 2), nullptr);
  EXPECT_EQ(x.a().calloc_fn(x.a().ctx, std::numeric_limits<size_t>::max(), 1),
      nullptr);
  EXPECT_EQ(x.a().malloc_fn(x.a().ctx, std::numeric_limits<size_t>::max()),
      nullptr);
  EXPECT_EQ(grcore_meter_in_use(&x.meter), 0u);
  EXPECT_EQ(grcore_meter_blocks(&x.meter), 0u);
  EXPECT_EQ(x.base.live, 0);
}

TEST(Account, ZeroSizeIsNonNullAndOneBlock) {
  Fixture x;
  void * p = x.m(0);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(grcore_meter_in_use(&x.meter), 0u);
  EXPECT_EQ(grcore_meter_blocks(&x.meter), 1u);
  void * z = x.a().calloc_fn(x.a().ctx, 0, 8);
  ASSERT_NE(z, nullptr);
  EXPECT_EQ(grcore_meter_blocks(&x.meter), 2u);
  x.f(p);
  x.f(z);
  EXPECT_EQ(grcore_meter_blocks(&x.meter), 0u);
}

TEST(Account, ReallocGrowsShrinksAndKeepsTheBlockCount) {
  Fixture x;
  auto * p = static_cast<char *>(x.m(16));
  std::memcpy(p, "0123456789abcde", 16);
  p = static_cast<char *>(x.a().realloc_fn(x.a().ctx, p, 64));
  ASSERT_NE(p, nullptr);
  EXPECT_STREQ(p, "0123456789abcde");
  EXPECT_EQ(grcore_meter_in_use(&x.meter), 64u);
  EXPECT_EQ(grcore_meter_blocks(&x.meter), 1u);
  EXPECT_EQ(grcore_meter_peak(&x.meter), 64u);
  p = static_cast<char *>(x.a().realloc_fn(x.a().ctx, p, 8));
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(grcore_meter_in_use(&x.meter), 8u);
  EXPECT_EQ(grcore_meter_peak(&x.meter), 64u);
  p = static_cast<char *>(x.a().realloc_fn(x.a().ctx, p, 0));
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(grcore_meter_in_use(&x.meter), 0u);
  EXPECT_EQ(grcore_meter_blocks(&x.meter), 1u);
  x.f(p);
  EXPECT_EQ(grcore_meter_blocks(&x.meter), 0u);
  EXPECT_EQ(x.base.live, 0);
}

TEST(Account, ReallocOfNullAllocates) {
  Fixture x;
  void * p = x.a().realloc_fn(x.a().ctx, nullptr, 40);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(grcore_meter_in_use(&x.meter), 40u);
  EXPECT_EQ(grcore_meter_blocks(&x.meter), 1u);
  x.f(p);
}

TEST(Account, FailedBaseAllocationLeavesCountersUnchanged) {
  Fixture x;
  void * keep = x.m(10);
  ASSERT_NE(keep, nullptr);
  x.base.fail_at = x.base.calls + 1;
  EXPECT_EQ(x.m(5), nullptr);
  x.base.fail_at = x.base.calls + 1;
  EXPECT_EQ(x.a().calloc_fn(x.a().ctx, 1, 5), nullptr);
  x.base.fail_at = x.base.calls + 1;
  EXPECT_EQ(x.a().realloc_fn(x.a().ctx, keep, 500), nullptr);
  EXPECT_EQ(grcore_meter_in_use(&x.meter), 10u);
  EXPECT_EQ(grcore_meter_blocks(&x.meter), 1u);
  EXPECT_EQ(grcore_meter_peak(&x.meter), 10u);
  x.f(keep); // still valid after the failed realloc
  EXPECT_EQ(x.base.live, 0);
}

TEST(Account, FreeOfNullIsIgnored) {
  Fixture x;
  x.f(nullptr);
  EXPECT_EQ(grcore_meter_blocks(&x.meter), 0u);
}

TEST(Account, NullBasesMeanTheDefaults) {
  GRCORE_Meter meter;
  GRCORE_Counting c;
  grcore_meter_init(&meter);
  grcore_counting_init(&c, &meter, nullptr, nullptr);
  EXPECT_EQ(c.base_allocator, grcore_allocator_default());
  EXPECT_EQ(c.base_pages, grcore_page_provider_default());
}

TEST(Account, ManyThreadsLeaveTheMeterBalanced) {
  GRCORE_Group * g;
  ASSERT_EQ(grcore_group_create(nullptr, nullptr, &g), GRCORE_OK);
  const GRCORE_Allocator * a = grcore_group_allocator(g);
  const int threads = 4, rounds = 2000;
  std::vector<std::thread> ts;
  for (int t = 0; t < threads; t++) {
    ts.emplace_back([=] {
      for (int i = 0; i < rounds; i++) {
        void * p = a->malloc_fn(a->ctx, 100);
        void * q = a->calloc_fn(a->ctx, 4, 25);
        p = a->realloc_fn(a->ctx, p, 300);
        p = a->realloc_fn(a->ctx, p, 50);
        a->free_fn(a->ctx, q);
        a->free_fn(a->ctx, p);
      }
    });
  }
  for (auto & t : ts) {
    t.join();
  }
  EXPECT_EQ(grcore_group_memory_in_use(g), 0u);
  EXPECT_EQ(grcore_group_memory_blocks(g), 0u);
  // One thread alone holds at most 300 + 100 bytes at once.
  EXPECT_GE(grcore_group_memory_peak(g), 400u);
  EXPECT_LE(grcore_group_memory_peak(g), 400u * threads);
  EXPECT_EQ(grcore_group_destroy(g), GRCORE_OK);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

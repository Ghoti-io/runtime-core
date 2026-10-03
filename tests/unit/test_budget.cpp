/**
 * @file
 *
 * The budgets: fuel, memory (budget, reserve, refusals) and depth.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../../src/b/context_internal.h"

#include <thread>

namespace {

uint64_t word_of(const GRCORE_Context * c) {
  return __atomic_load_n(&c->request_word, __ATOMIC_ACQUIRE);
}
constexpr uint64_t kFuelBit = UINT64_C(1) << GRCORE_REQUEST_FUEL;
constexpr uint64_t kMemoryBit = UINT64_C(1) << GRCORE_REQUEST_MEMORY;

} // namespace

TEST(Fuel, IsExhaustedOnlyWhenMoreHasBeenUsedThanTheBudget) {
  RunWorld w(10);
  EXPECT_EQ(grcore_context_fuel_limit(w.ctx), 10u);
  EXPECT_EQ(grcore_context_fuel_remaining(w.ctx), 10u);
  EXPECT_FALSE(grcore_context_charge_fuel(w.ctx, 10));
  EXPECT_EQ(grcore_context_fuel_used(w.ctx), 10u);
  EXPECT_EQ(grcore_context_fuel_remaining(w.ctx), 0u);
  EXPECT_EQ(word_of(w.ctx), 0u);
  EXPECT_TRUE(grcore_context_charge_fuel(w.ctx, 1));
  EXPECT_EQ(word_of(w.ctx), kFuelBit);
  EXPECT_EQ(grcore_context_fuel_remaining(w.ctx), 0u); // not negative
  EXPECT_TRUE(grcore_context_request_pending(w.ctx, GRCORE_REQUEST_FUEL));
}

TEST(Fuel, TheCreationOptionIsKeptWhileTheCurrentBudgetChanges) {
  RunWorld w(10);
  ASSERT_EQ(grcore_context_set_fuel(w.ctx, 50), GRCORE_OK);
  EXPECT_EQ(grcore_context_fuel_limit(w.ctx), 50u);
  EXPECT_EQ(grcore_context_fuel(w.ctx), 10u); // what it was created with
}

TEST(Fuel, ChargingSaturatesAndAnUnlimitedBudgetNeverRunsOut) {
  RunWorld w; // unlimited
  EXPECT_EQ(grcore_context_fuel_remaining(w.ctx), GRCORE_UNLIMITED);
  EXPECT_FALSE(grcore_context_charge_fuel(w.ctx, UINT64_MAX));
  EXPECT_FALSE(grcore_context_charge_fuel(w.ctx, 5));
  EXPECT_EQ(grcore_context_fuel_used(w.ctx), UINT64_MAX);
  EXPECT_EQ(word_of(w.ctx), 0u);
  EXPECT_EQ(grcore_context_fuel_remaining(w.ctx), GRCORE_UNLIMITED);
  ASSERT_EQ(grcore_context_set_fuel(w.ctx, 100), GRCORE_OK);
  EXPECT_EQ(word_of(w.ctx), kFuelBit); // saturated use is over any real budget
}

TEST(Fuel, SettingTheBudgetRaisesAndLowersTheDerivedRequest) {
  RunWorld w(10);
  grcore_context_charge_fuel(w.ctx, 20);
  EXPECT_EQ(word_of(w.ctx), kFuelBit);
  ASSERT_EQ(grcore_context_set_fuel(w.ctx, 25), GRCORE_OK);
  EXPECT_EQ(word_of(w.ctx), 0u); // a level: it follows the budget
  EXPECT_EQ(grcore_context_fuel_remaining(w.ctx), 5u);
  ASSERT_EQ(grcore_context_set_fuel(w.ctx, 19), GRCORE_OK);
  EXPECT_EQ(word_of(w.ctx), kFuelBit);
}

TEST(Fuel, TheBudgetCanBeSetOnlyWhenNoGuestCodeIsExecuting) {
  RunWorld w(10);
  for (GRCORE_ContextConfig cfg : {GRCORE_CONFIG_RUNNING,
           GRCORE_CONFIG_PARKED_INSIDE}) {
    w.ctx->config = cfg;
    EXPECT_EQ(grcore_context_set_fuel(w.ctx, 99), GRCORE_ERR_INVALID);
    EXPECT_EQ(grcore_context_set_memory_bytes(w.ctx, 99), GRCORE_ERR_INVALID);
  }
  for (GRCORE_ContextConfig cfg : {GRCORE_CONFIG_PARKED_OUTSIDE,
           GRCORE_CONFIG_AT_POLL, GRCORE_CONFIG_PAUSED}) {
    w.ctx->config = cfg;
    EXPECT_EQ(grcore_context_set_fuel(w.ctx, 99), GRCORE_OK);
  }
  w.ctx->config = GRCORE_CONFIG_PARKED_OUTSIDE;
  EXPECT_EQ(grcore_context_fuel_limit(w.ctx), 99u);
  EXPECT_EQ(grcore_context_set_fuel(nullptr, 1), GRCORE_ERR_INVALID);
  GRCORE_Result other = GRCORE_OK;
  std::thread([&] { other = grcore_context_set_fuel(w.ctx, 5); }).join();
  EXPECT_EQ(other, GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_fuel_limit(w.ctx), 99u);
}

TEST(Fuel, NullContextReadsAsEmpty) {
  EXPECT_FALSE(grcore_context_charge_fuel(nullptr, 1));
  EXPECT_EQ(grcore_context_fuel_used(nullptr), 0u);
  EXPECT_EQ(grcore_context_fuel_remaining(nullptr), GRCORE_UNLIMITED);
  EXPECT_EQ(grcore_context_fuel_limit(nullptr), GRCORE_UNLIMITED);
  EXPECT_EQ(grcore_context_memory_limit(nullptr), GRCORE_UNLIMITED);
  EXPECT_EQ(grcore_context_memory_reserve(nullptr), GRCORE_DEFAULT_MEMORY_RESERVE);
  EXPECT_EQ(grcore_context_memory_refusals(nullptr), 0u);
  EXPECT_EQ(grcore_context_depth(nullptr, GRCORE_DEPTH_GUEST), 0u);
  EXPECT_EQ(grcore_context_enter_depth(nullptr, GRCORE_DEPTH_GUEST), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_leave_depth(nullptr, GRCORE_DEPTH_GUEST), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_set_memory_bytes(nullptr, 1), GRCORE_ERR_INVALID);
}

TEST(Memory, TheDefaultReserveIs262144AndIsReadBack) {
  EXPECT_EQ(GRCORE_DEFAULT_MEMORY_RESERVE, 262144u);
  RunWorld w(GRCORE_UNLIMITED, 1000);
  // RunWorld passes the default through its own argument default.
  EXPECT_EQ(grcore_context_memory_reserve(w.ctx), 262144u);
  EXPECT_EQ(grcore_context_memory_limit(w.ctx), 1000u);
  EXPECT_EQ(grcore_context_memory_bytes(w.ctx), 1000u);
}

TEST(Memory, AnAllocationThatCrossesTheBudgetIsServedAndRaisesTheRequest) {
  RunWorld w(GRCORE_UNLIMITED, 100, 50);
  const GRCORE_Allocator * a = grcore_context_allocator(w.ctx);
  void * p = a->malloc_fn(a->ctx, 100);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(word_of(w.ctx), 0u); // exactly at the budget is not over
  void * q = a->malloc_fn(a->ctx, 1);
  ASSERT_NE(q, nullptr);
  EXPECT_EQ(word_of(w.ctx), kMemoryBit);
  a->free_fn(a->ctx, q);
  // The bit is a level, refreshed at the next slow poll or budget change.
  grcore_context_refresh_derived(w.ctx);
  EXPECT_EQ(word_of(w.ctx), 0u);
  a->free_fn(a->ctx, p);
}

TEST(Memory, EveryAllocationPathChecksTheReserve) {
  RunWorld w(GRCORE_UNLIMITED, 100, 100);
  const GRCORE_Allocator * a = grcore_context_allocator(w.ctx);
  const GRCORE_PageProvider * pages = grcore_context_page_provider(w.ctx);
  EXPECT_EQ(a->malloc_fn(a->ctx, 201), nullptr);
  EXPECT_EQ(a->calloc_fn(a->ctx, 201, 1), nullptr);
  EXPECT_EQ(a->calloc_fn(a->ctx, 21, 10), nullptr);
  EXPECT_EQ(pages->map(pages->ctx, pages->page_size), nullptr); // 4096 > 200
  EXPECT_EQ(grcore_context_memory_refusals(w.ctx), 4u);
  EXPECT_EQ(grcore_context_memory_in_use(w.ctx), 0u);
  EXPECT_EQ(grcore_context_memory_blocks(w.ctx), 0u);
  // A realloc that grows past the reserve is refused and keeps the block.
  char * p = static_cast<char *>(a->malloc_fn(a->ctx, 150));
  ASSERT_NE(p, nullptr);
  p[0] = 'x';
  p[149] = 'y';
  EXPECT_EQ(a->realloc_fn(a->ctx, p, 250), nullptr);
  EXPECT_EQ(grcore_context_memory_refusals(w.ctx), 5u);
  EXPECT_EQ(grcore_context_memory_in_use(w.ctx), 150u);
  EXPECT_EQ(p[0], 'x');
  EXPECT_EQ(p[149], 'y');
  // Shrinking and growing within the reserve are fine.
  p = static_cast<char *>(a->realloc_fn(a->ctx, p, 200));
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(grcore_context_memory_in_use(w.ctx), 200u);
  p = static_cast<char *>(a->realloc_fn(a->ctx, p, 10));
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(grcore_context_memory_in_use(w.ctx), 10u);
  a->free_fn(a->ctx, p);
  EXPECT_EQ(grcore_context_memory_blocks(w.ctx), 0u);
}

TEST(Memory, PagesPastTheBudgetPostTheRequestToo) {
  const size_t page = grcore_page_provider_default()->page_size;
  RunWorld w(GRCORE_UNLIMITED, page, page);
  const GRCORE_PageProvider * pages = grcore_context_page_provider(w.ctx);
  void * one = pages->map(pages->ctx, page);
  ASSERT_NE(one, nullptr);
  EXPECT_EQ(word_of(w.ctx), 0u);
  void * two = pages->map(pages->ctx, page);
  ASSERT_NE(two, nullptr);
  EXPECT_EQ(word_of(w.ctx), kMemoryBit);
  EXPECT_EQ(pages->map(pages->ctx, page), nullptr);
  EXPECT_EQ(grcore_context_memory_refusals(w.ctx), 1u);
  pages->unmap(pages->ctx, two, page);
  pages->unmap(pages->ctx, one, page);
}

TEST(Memory, AZeroReserveMakesTheBudgetAHardCap) {
  RunWorld w(GRCORE_UNLIMITED, 100, 0);
  const GRCORE_Allocator * a = grcore_context_allocator(w.ctx);
  void * p = a->malloc_fn(a->ctx, 100);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(a->malloc_fn(a->ctx, 1), nullptr);
  EXPECT_EQ(word_of(w.ctx), 0u);
  a->free_fn(a->ctx, p);
}

TEST(Memory, AnUnlimitedBudgetNeverRefusesAndAHugeReserveDoesNotWrap) {
  RunWorld w(GRCORE_UNLIMITED, GRCORE_UNLIMITED, 0);
  const GRCORE_Allocator * a = grcore_context_allocator(w.ctx);
  void * p = a->malloc_fn(a->ctx, 1 << 20);
  ASSERT_NE(p, nullptr);
  a->free_fn(a->ctx, p);
  EXPECT_EQ(grcore_context_memory_refusals(w.ctx), 0u);
  RunWorld v(GRCORE_UNLIMITED, 100, UINT64_MAX); // limit + reserve would wrap
  const GRCORE_Allocator * b = grcore_context_allocator(v.ctx);
  void * q = b->malloc_fn(b->ctx, 1 << 20);
  EXPECT_NE(q, nullptr);
  b->free_fn(b->ctx, q);
}

TEST(Memory, SettingTheBudgetKeepsTheReserveAndRefreshesTheRequest) {
  RunWorld w(GRCORE_UNLIMITED, 100, 50);
  const GRCORE_Allocator * a = grcore_context_allocator(w.ctx);
  void * p = a->malloc_fn(a->ctx, 120);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(word_of(w.ctx), kMemoryBit);
  ASSERT_EQ(grcore_context_set_memory_bytes(w.ctx, 200), GRCORE_OK);
  EXPECT_EQ(word_of(w.ctx), 0u);
  EXPECT_EQ(grcore_context_memory_reserve(w.ctx), 50u);
  EXPECT_EQ(grcore_context_memory_limit(w.ctx), 200u);
  ASSERT_EQ(grcore_context_set_memory_bytes(w.ctx, 100), GRCORE_OK);
  EXPECT_EQ(word_of(w.ctx), kMemoryBit);
  a->free_fn(a->ctx, p);
}

TEST(Memory, TheGroupsAllocatorIsNotBudgeted) {
  RunWorld w(GRCORE_UNLIMITED, 10, 0);
  const GRCORE_Allocator * a = grcore_group_allocator(w.group);
  void * p = a->malloc_fn(a->ctx, 1 << 16);
  EXPECT_NE(p, nullptr);
  a->free_fn(a->ctx, p);
  EXPECT_EQ(word_of(w.ctx), 0u);
}

TEST(Depth, EnteringPastTheBudgetIsALimitErrorAndLeavesTheDepthUnchanged) {
  RunWorld w(GRCORE_UNLIMITED, GRCORE_UNLIMITED, GRCORE_DEFAULT_MEMORY_RESERVE, 3, 2);
  for (int i = 0; i < 3; i++) {
    EXPECT_EQ(grcore_context_enter_depth(w.ctx, GRCORE_DEPTH_GUEST), GRCORE_OK);
  }
  EXPECT_EQ(grcore_context_depth(w.ctx, GRCORE_DEPTH_GUEST), 3u);
  EXPECT_EQ(grcore_context_enter_depth(w.ctx, GRCORE_DEPTH_GUEST), GRCORE_ERR_LIMIT);
  EXPECT_EQ(grcore_context_depth(w.ctx, GRCORE_DEPTH_GUEST), 3u);
  // The two depths are independent.
  EXPECT_EQ(grcore_context_depth(w.ctx, GRCORE_DEPTH_NATIVE), 0u);
  EXPECT_EQ(grcore_context_enter_depth(w.ctx, GRCORE_DEPTH_NATIVE), GRCORE_OK);
  EXPECT_EQ(grcore_context_enter_depth(w.ctx, GRCORE_DEPTH_NATIVE), GRCORE_OK);
  EXPECT_EQ(grcore_context_enter_depth(w.ctx, GRCORE_DEPTH_NATIVE), GRCORE_ERR_LIMIT);
  // Leaving restores room.
  EXPECT_EQ(grcore_context_leave_depth(w.ctx, GRCORE_DEPTH_GUEST), GRCORE_OK);
  EXPECT_EQ(grcore_context_depth(w.ctx, GRCORE_DEPTH_GUEST), 2u);
  EXPECT_EQ(grcore_context_enter_depth(w.ctx, GRCORE_DEPTH_GUEST), GRCORE_OK);
  for (int i = 0; i < 3; i++) {
    EXPECT_EQ(grcore_context_leave_depth(w.ctx, GRCORE_DEPTH_GUEST), GRCORE_OK);
  }
  EXPECT_EQ(grcore_context_leave_depth(w.ctx, GRCORE_DEPTH_GUEST), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_depth(w.ctx, GRCORE_DEPTH_GUEST), 0u);
}

TEST(Depth, AZeroBudgetAdmitsNothingAndUnlimitedAdmitsEverything) {
  RunWorld zero(GRCORE_UNLIMITED, GRCORE_UNLIMITED, GRCORE_DEFAULT_MEMORY_RESERVE, 0, 0);
  EXPECT_EQ(grcore_context_enter_depth(zero.ctx, GRCORE_DEPTH_GUEST), GRCORE_ERR_LIMIT);
  EXPECT_EQ(grcore_context_enter_depth(zero.ctx, GRCORE_DEPTH_NATIVE), GRCORE_ERR_LIMIT);
  RunWorld open;
  for (int i = 0; i < 10000; i++) {
    ASSERT_EQ(grcore_context_enter_depth(open.ctx, GRCORE_DEPTH_NATIVE), GRCORE_OK);
  }
  EXPECT_EQ(grcore_context_depth(open.ctx, GRCORE_DEPTH_NATIVE), 10000u);
  for (int i = 0; i < 10000; i++) {
    ASSERT_EQ(grcore_context_leave_depth(open.ctx, GRCORE_DEPTH_NATIVE), GRCORE_OK);
  }
}

TEST(Depth, AnUnknownKindIsRefused) {
  RunWorld w;
  auto bad = static_cast<GRCORE_DepthKind>(5);
  EXPECT_EQ(grcore_context_enter_depth(w.ctx, bad), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_leave_depth(w.ctx, bad), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_depth(w.ctx, bad), 0u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

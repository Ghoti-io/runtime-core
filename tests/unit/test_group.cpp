/**
 * @file
 *
 * Context groups.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

TEST(Group, CreateAndDestroyWithDefaults) {
  GRCORE_Group * g = nullptr;
  ASSERT_EQ(grcore_group_create(nullptr, nullptr, &g), GRCORE_OK);
  ASSERT_NE(g, nullptr);
  EXPECT_EQ(grcore_group_context_count(g), 0u);
  EXPECT_NE(grcore_group_allocator(g), nullptr);
  EXPECT_NE(grcore_group_page_provider(g), nullptr);
  EXPECT_EQ(grcore_group_memory_in_use(g), 0u);
  EXPECT_EQ(grcore_group_destroy(g), GRCORE_OK);
}

TEST(Group, NullArgumentsAreRefused) {
  EXPECT_EQ(grcore_group_create(nullptr, nullptr, nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_group_destroy(nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_group_context_count(nullptr), 0u);
  EXPECT_EQ(grcore_group_allocator(nullptr), nullptr);
  EXPECT_EQ(grcore_group_page_provider(nullptr), nullptr);
  EXPECT_EQ(grcore_group_memory_in_use(nullptr), 0u);
  EXPECT_EQ(grcore_group_memory_peak(nullptr), 0u);
  EXPECT_EQ(grcore_group_memory_blocks(nullptr), 0u);
}

TEST(Group, DestroyWithLiveContextsIsRefusedAndLeavesTheGroupIntact) {
  GRCORE_Group * g;
  ASSERT_EQ(grcore_group_create(nullptr, nullptr, &g), GRCORE_OK);
  GRCORE_Context *a, *b;
  ASSERT_EQ(grcore_context_create(g, nullptr, &a), GRCORE_OK);
  ASSERT_EQ(grcore_context_create(g, nullptr, &b), GRCORE_OK);
  EXPECT_EQ(grcore_group_context_count(g), 2u);
  EXPECT_EQ(grcore_group_destroy(g), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_group_context_count(g), 2u);
  EXPECT_EQ(grcore_context_destroy(a), GRCORE_OK);
  EXPECT_EQ(grcore_group_destroy(g), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_group_context_count(g), 1u);
  EXPECT_EQ(grcore_context_destroy(b), GRCORE_OK);
  EXPECT_EQ(grcore_group_context_count(g), 0u);
  EXPECT_EQ(grcore_group_destroy(g), GRCORE_OK);
}

TEST(Group, UsesTheAllocatorItWasGivenAndFreesWithIt) {
  TrackingAllocator t;
  GRCORE_Group * g;
  ASSERT_EQ(grcore_group_create(t.get(), nullptr, &g), GRCORE_OK);
  EXPECT_GT(t.live, 0);
  EXPECT_EQ(grcore_group_destroy(g), GRCORE_OK);
  EXPECT_EQ(t.live, 0);
}

TEST(Group, CreateFailureWritesNothing) {
  TrackingAllocator t;
  t.fail_at = 1;
  GRCORE_Group * g = reinterpret_cast<GRCORE_Group *>(1);
  EXPECT_EQ(grcore_group_create(t.get(), nullptr, &g), GRCORE_ERR_OOM);
  EXPECT_EQ(g, reinterpret_cast<GRCORE_Group *>(1));
  EXPECT_EQ(t.live, 0);
}

TEST(Group, ItsAllocatorChargesTheGroupNotAContext) {
  GRCORE_Group * g;
  ASSERT_EQ(grcore_group_create(nullptr, nullptr, &g), GRCORE_OK);
  GRCORE_Context * c;
  ASSERT_EQ(grcore_context_create(g, nullptr, &c), GRCORE_OK);
  const GRCORE_Allocator * a = grcore_group_allocator(g);
  void * p = a->malloc_fn(a->ctx, 256);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(grcore_group_memory_in_use(g), 256u);
  EXPECT_EQ(grcore_group_memory_blocks(g), 1u);
  EXPECT_EQ(grcore_context_memory_in_use(c), 0u);
  a->free_fn(a->ctx, p);
  EXPECT_EQ(grcore_group_memory_in_use(g), 0u);
  EXPECT_EQ(grcore_group_memory_peak(g), 256u);
  EXPECT_EQ(grcore_context_destroy(c), GRCORE_OK);
  EXPECT_EQ(grcore_group_destroy(g), GRCORE_OK);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

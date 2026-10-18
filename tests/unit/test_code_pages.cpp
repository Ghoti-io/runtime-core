/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Runtime-core.
 *
 * Ghoti.io Runtime-core is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License version
 * 3 as published by the Free Software Foundation.
 *
 * Ghoti.io Runtime-core is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Budgeted executable pages retained through the last unmap.
 */

#include "test_helpers.h"

TEST(CodePages, BudgetCountsMappingsAndKeepsProviderAliveUntilFinalUnmap) {
  const size_t page_size = grcore_page_provider_default()->page_size;
  GRCORE_CodePages * pages;
  ASSERT_EQ(grcore_code_pages_create(nullptr, page_size,
      &pages), GRCORE_OK);
  const GRCORE_PageProvider * provider =
      grcore_code_pages_provider(pages);
  ASSERT_NE(provider, nullptr);

  void * mapping = provider->map(provider->ctx, page_size);
  ASSERT_NE(mapping, nullptr);
  EXPECT_EQ(provider->map(provider->ctx, page_size), nullptr);
  EXPECT_EQ(grcore_code_pages_in_use(pages), page_size);
  EXPECT_EQ(grcore_code_pages_peak(pages), page_size);
  EXPECT_GE(grcore_code_pages_accounted_bytes(pages), page_size);
  EXPECT_EQ(grcore_page_protect(provider, mapping, page_size,
      GRCORE_PAGE_READ_EXECUTE), GRCORE_OK);

  grcore_code_pages_release(pages);
  provider->unmap(provider->ctx, mapping, page_size);
}

TEST(CodePages, SharedModuleOwnerSurvivesOwnerAndMappingReleaseOrder) {
  const size_t page_size = grcore_page_provider_default()->page_size;
  GRCORE_CodePages * module_owner;
  ASSERT_EQ(grcore_code_pages_create(nullptr, page_size, &module_owner), GRCORE_OK);
  GRCORE_CodePages * cache_owner = grcore_code_pages_retain(module_owner);
  ASSERT_NE(cache_owner, nullptr);
  const GRCORE_PageProvider * provider =
      grcore_code_pages_provider(cache_owner);
  ASSERT_NE(provider, nullptr);

  void * mapping = provider->map(provider->ctx, page_size);
  ASSERT_NE(mapping, nullptr);
  grcore_code_pages_release(module_owner);
  EXPECT_EQ(grcore_code_pages_in_use(cache_owner), page_size);
  EXPECT_EQ(provider->map(provider->ctx, page_size), nullptr);

  grcore_code_pages_release(cache_owner);
  /* The mapping's provider reference remains valid after both owners release. */
  provider->unmap(provider->ctx, mapping, page_size);
}

TEST(CodePages, ZeroCapAndMalformedSizesRefuseWithoutCharging) {
  GRCORE_CodePages * pages;
  ASSERT_EQ(grcore_code_pages_create(nullptr, 0, &pages),
      GRCORE_OK);
  const GRCORE_PageProvider * provider =
      grcore_code_pages_provider(pages);
  EXPECT_EQ(provider->map(provider->ctx, provider->page_size), nullptr);
  EXPECT_EQ(provider->map(provider->ctx, provider->page_size + 1), nullptr);
  EXPECT_EQ(grcore_code_pages_in_use(pages), 0u);
  grcore_code_pages_release(pages);
}

TEST(CodePages, FailedCreatePreservesOutputAndReleasesItsOwnedGroup) {
  for (size_t failure = 1; failure <= 2; ++failure) {
    TrackingAllocator allocator;
    allocator.fail_at = failure;
    auto * sentinel = reinterpret_cast<GRCORE_CodePages *>(1);
    GRCORE_CodePages * pages = sentinel;
    EXPECT_EQ(grcore_code_pages_create(allocator.get(),
        grcore_page_provider_default()->page_size, &pages), GRCORE_ERR_OOM);
    EXPECT_EQ(pages, sentinel) << "the output is written only on success";
    EXPECT_EQ(allocator.live, 0) << "the partially created group is released";
  }

  TrackingAllocator allocator;
  GRCORE_CodePages * pages = nullptr;
  ASSERT_EQ(grcore_code_pages_create(allocator.get(),
      grcore_page_provider_default()->page_size, &pages), GRCORE_OK);
  EXPECT_GT(allocator.live, 0);
  grcore_code_pages_release(pages);
  EXPECT_EQ(allocator.live, 0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

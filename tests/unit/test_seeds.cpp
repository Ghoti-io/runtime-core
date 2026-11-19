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

#include <ghoti.io/runtime-core/runtime-core.h>

#include <gtest/gtest.h>

#include <set>
#include <vector>

namespace {

struct G {
  GRCORE_Group * g = nullptr;
  G() { EXPECT_EQ(grcore_group_create(nullptr, nullptr, &g), GRCORE_OK); }
  ~G() { EXPECT_EQ(grcore_group_destroy(g), GRCORE_OK); }
  GRCORE_Context * make() {
    GRCORE_Context * c = nullptr;
    EXPECT_EQ(grcore_context_create(g, nullptr, &c), GRCORE_OK);
    return c;
  }
};

}  // namespace

TEST(Seeds, Splitmix64MatchesThePublishedVectorsForSeedZero) {
  // The standard generator started at 0: its first two outputs.
  EXPECT_EQ(grcore_splitmix64(0), UINT64_C(0xE220A8397B1DCDAF));
  EXPECT_EQ(grcore_splitmix64(UINT64_C(0x9E3779B97F4A7C15)), UINT64_C(0x6E789E6AA1B965F4));
}

TEST(Seeds, ASuppliedMasterGivesTheSameContextSeedsInTheSameOrderEveryTime) {
  uint64_t first[3], second[3];
  for (uint64_t * out : {first, second}) {
    G w;
    ASSERT_EQ(grcore_group_seed(w.g, 0), GRCORE_OK);
    EXPECT_EQ(grcore_group_master_seed(w.g), 0u);
    GRCORE_Context * c[3];
    for (int i = 0; i < 3; i++) {
      c[i] = w.make();
      out[i] = grcore_context_seed(c[i]);
    }
    for (int i = 0; i < 3; i++) {
      EXPECT_EQ(grcore_context_destroy(c[i]), GRCORE_OK);
    }
  }
  for (int i = 0; i < 3; i++) {
    EXPECT_EQ(first[i], second[i]);
  }
  // Pinned: draws 0 and 1 of the sequence started at master 0.
  EXPECT_EQ(first[0], UINT64_C(0xE220A8397B1DCDAF));
  EXPECT_EQ(first[1], UINT64_C(0x6E789E6AA1B965F4));
}

TEST(Seeds, ADifferentMasterGivesDifferentSeedsAndNoTwoContextsShareOne) {
  G a, b;
  ASSERT_EQ(grcore_group_seed(a.g, 1), GRCORE_OK);
  ASSERT_EQ(grcore_group_seed(b.g, 2), GRCORE_OK);
  std::set<uint64_t> seen;
  std::vector<GRCORE_Context *> cs;
  for (int i = 0; i < 50; i++) {
    for (G * w : {&a, &b}) {
      cs.push_back(w->make());
      EXPECT_TRUE(seen.insert(grcore_context_seed(cs.back())).second) << "a seed repeated";
    }
  }
  // Control: the same master twice does repeat (the set sees it).
  G c;
  ASSERT_EQ(grcore_group_seed(c.g, 1), GRCORE_OK);
  GRCORE_Context * again = c.make();
  EXPECT_FALSE(seen.insert(grcore_context_seed(again)).second);
  EXPECT_EQ(grcore_context_destroy(again), GRCORE_OK);
  for (size_t i = 0; i < cs.size(); i++) {
    EXPECT_EQ(grcore_context_destroy(cs[i]), GRCORE_OK);
  }
}

TEST(Seeds, AContextsDrawsArePureAndEachIndexDiffers) {
  G w;
  ASSERT_EQ(grcore_group_seed(w.g, 7), GRCORE_OK);
  GRCORE_Context * c = w.make();
  EXPECT_EQ(grcore_context_seed_draw(c, 0), grcore_splitmix64(grcore_context_seed(c)));
  EXPECT_EQ(grcore_context_seed_draw(c, 5), grcore_context_seed_draw(c, 5));
  std::set<uint64_t> d;
  for (uint64_t i = 0; i < 100; i++) {
    EXPECT_TRUE(d.insert(grcore_context_seed_draw(c, i)).second);
  }
  EXPECT_EQ(grcore_context_destroy(c), GRCORE_OK);
  EXPECT_EQ(grcore_context_seed(nullptr), 0u);
  EXPECT_EQ(grcore_context_seed_draw(nullptr, 3), 0u);
  EXPECT_EQ(grcore_group_master_seed(nullptr), 0u);
}

TEST(Seeds, ASeedCannotChangeOnceAContextHasTakenOne) {
  G w;
  ASSERT_EQ(grcore_group_seed(w.g, 9), GRCORE_OK);
  GRCORE_Context * c = w.make();
  EXPECT_EQ(grcore_group_seed(w.g, 10), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_group_seed_random(w.g), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_group_master_seed(w.g), 9u);
  EXPECT_EQ(grcore_context_destroy(c), GRCORE_OK);
  // Destroying it does not give the draw back.
  EXPECT_EQ(grcore_group_seed(w.g, 10), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_group_seed(nullptr, 1), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_group_seed_random(nullptr), GRCORE_ERR_INVALID);
}

TEST(Seeds, AGroupThatWasNotGivenAMasterTookOneFromEntropy) {
  G a, b;
  EXPECT_NE(grcore_group_master_seed(a.g), grcore_group_master_seed(b.g))
      << "two groups from entropy share a 64-bit master";
  uint64_t before = grcore_group_master_seed(a.g);
  ASSERT_EQ(grcore_group_seed_random(a.g), GRCORE_OK);
  EXPECT_NE(grcore_group_master_seed(a.g), before);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

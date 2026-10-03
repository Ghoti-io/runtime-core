/**
 * @file
 *
 * The umbrella header compiled as C++, on its own.
 *
 * Including nothing else proves the umbrella is self-sufficient and that the
 * public headers are C++-consumable, which the C sources cannot show.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/runtime-core/runtime-core.h>

#include <gtest/gtest.h>

#include <string>

#define GRCORE_TEST_STR(x) #x
#define GRCORE_TEST_EXPAND(x) GRCORE_TEST_STR(x)

TEST(Umbrella, ReachesTheResultVersionAndAllocatorDeclarations) {
  GRCORE_Result result = GRCORE_OK;
  EXPECT_STREQ(grcore_result_string(result), "No error");
  EXPECT_NE(grcore_version_string(), nullptr);
  EXPECT_NE(grcore_allocator_default(), nullptr);
}

TEST(Umbrella, HeadersHaveExternCLinkageSoCppCanLinkTheSymbols) {
  /* A C++ translation unit that declared these without extern "C" would fail
   * to link; reaching this line at all is the check. */
  EXPECT_EQ(grcore_version_number(), GRCORE_VERSION_NUMBER);
}

TEST(Umbrella, EveryTypeRenameIsInEffectNotInsideAComment) {
  const char * renamed[] = {GRCORE_TEST_EXPAND(GRCORE_Allocator),
      GRCORE_TEST_EXPAND(GRCORE_Context), GRCORE_TEST_EXPAND(GRCORE_Port),
      GRCORE_TEST_EXPAND(GRCORE_PollCall), GRCORE_TEST_EXPAND(GRCORE_Verdict),
      GRCORE_TEST_EXPAND(GRCORE_Location), GRCORE_TEST_EXPAND(GRCORE_Outcome)};
  for (const char * r : renamed) {
    EXPECT_NE(std::string(r).find("ghotiio_"), std::string::npos) << r;
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

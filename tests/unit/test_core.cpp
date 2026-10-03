/**
 * @file
 *
 * The result vocabulary, the version, and the allocator.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/runtime-core/libver.h>

#include <set>
#include <string>

/* CONVENTIONS.md section 5: GRCORE_RESULT_COUNT closes the enum so a test can
 * check the string table is complete. A new result without a string falls
 * through to "Unknown error" and is caught here. */
TEST(Result, EveryCodeHasItsOwnString) {
  std::set<std::string> seen;
  for (int i = 0; i < GRCORE_RESULT_COUNT; i++) {
    const char * s = grcore_result_string(static_cast<GRCORE_Result>(i));
    ASSERT_NE(s, nullptr);
    EXPECT_STRNE(s, "Unknown error") << "result " << i;
    EXPECT_TRUE(seen.insert(s).second) << "result " << i << " shares a string";
  }
}

TEST(Result, OutOfRangeIsUnknownNotUndefined) {
  EXPECT_STREQ(grcore_result_string(GRCORE_RESULT_COUNT), "Unknown error");
  EXPECT_STREQ(grcore_result_string(static_cast<GRCORE_Result>(-1)),
      "Unknown error");
}

TEST(Result, ZeroIsSuccess) {
  EXPECT_EQ(GRCORE_OK, 0);
  EXPECT_STREQ(grcore_result_string(GRCORE_OK), "No error");
}

/* CONVENTIONS.md section 5 fixes the vocabulary. This library adds exactly
 * one constant, ERR_GUEST (section 13), so a second addition fails here
 * rather than being noticed later. */
TEST(Result, TheVocabularyIsTheSuitesPlusErrGuestAndNothingElse) {
  EXPECT_EQ(GRCORE_RESULT_COUNT, 10);
}

/* The nine shared constants keep the numbers they have in every other
 * library, because ERR_GUEST is appended before the count and not inserted. */
TEST(Result, SharedConstantsKeepTheirNumbers) {
  EXPECT_EQ(GRCORE_OK, 0);
  EXPECT_EQ(GRCORE_ERR_IO, 1);
  EXPECT_EQ(GRCORE_ERR_FORMAT, 2);
  EXPECT_EQ(GRCORE_ERR_UNSUPPORTED, 3);
  EXPECT_EQ(GRCORE_ERR_LIMIT, 4);
  EXPECT_EQ(GRCORE_ERR_CORRUPT, 5);
  EXPECT_EQ(GRCORE_ERR_OOM, 6);
  EXPECT_EQ(GRCORE_ERR_INVALID, 7);
  EXPECT_EQ(GRCORE_ERR_INTERNAL, 8);
  EXPECT_EQ(GRCORE_ERR_GUEST, 9);
}

TEST(Result, ErrGuestIsDescribedAsTheGuestsFailure) {
  EXPECT_STREQ(grcore_result_string(GRCORE_ERR_GUEST), "Guest code failed");
}

TEST(Version, StringMatchesTheGeneratedHeader) {
  EXPECT_STREQ(grcore_version_string(), GRCORE_VERSION_STRING);
  EXPECT_EQ(grcore_version_number(), GRCORE_VERSION_NUMBER);
  EXPECT_EQ(GRCORE_VERSION_NUMBER,
      GRCORE_MAKE_VERSION(GRCORE_VERSION_MAJOR, GRCORE_VERSION_MINOR,
          GRCORE_VERSION_PATCH));
}

TEST(Version, PackingIsOneBytePerComponent) {
  EXPECT_EQ(GRCORE_MAKE_VERSION(1, 2, 3), 0x010203u);
  EXPECT_LT(GRCORE_MAKE_VERSION(1, 2, 3), GRCORE_MAKE_VERSION(1, 3, 0));
}

TEST(Allocator, DefaultIsCutils) {
  const GRCORE_Allocator * allocator = grcore_allocator_default();
  ASSERT_NE(allocator, nullptr);
  EXPECT_EQ(allocator, gcu_allocator_default());
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

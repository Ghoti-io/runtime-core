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

TEST(Umbrella, ReachesTheThreeAHeaders) {
  // One symbol from each of engine.h, stack.h and frame.h, through the
  // umbrella alone.
  GRCORE_ConservativeDecoder d = {0xF0, 4, 1};
  uint64_t address = 0;
  ASSERT_TRUE(grcore_decoder_decode(&d, 0x30, &address));
  EXPECT_EQ(address, 4u);
  EXPECT_EQ(grcore_stack_frame_count(nullptr), 0u);
  GRCORE_FrameWalk walk;
  EXPECT_EQ(grcore_frame_walk_begin(nullptr, &walk), GRCORE_ERR_INVALID);
}

TEST(Umbrella, ReachesTheNewAHeadersAndTheRootsHeaderOfB) {
  // One symbol from each of activation.h, unwind.h, budget_scope.h and
  // b/roots.h, through the umbrella alone.
  EXPECT_EQ(grcore_activation_count(nullptr), 0u);
  EXPECT_EQ(grcore_unwind_all(nullptr, nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_budget_scope_count(nullptr), 0u);
  EXPECT_EQ(grcore_context_root_source_count(nullptr), 0u);
  // And the additions to budget.h and poll.h.
  EXPECT_EQ(grcore_context_fuel_scope_depth(nullptr), 0u);
  EXPECT_EQ(grcore_context_nested_depth(nullptr), 0u);
}

TEST(Umbrella, ReachesRefcountedCode) {
  // code.h through the umbrella alone, from C++.
  EXPECT_EQ(grcore_code_refcount(nullptr), 0u);
  EXPECT_EQ(grcore_code_payload(nullptr), nullptr);
  EXPECT_EQ(grcore_code_retain(nullptr), nullptr);
  grcore_code_release(nullptr);
  GRCORE_Code * code = nullptr;
  EXPECT_EQ(grcore_code_create(nullptr, nullptr, nullptr, &code),
      GRCORE_ERR_INVALID);
}

TEST(Umbrella, ReachesSnapshots) {
  // b/snapshot.h through the umbrella alone, from C++.
  EXPECT_EQ(grcore_snapshot_refcount(nullptr), 0u);
  EXPECT_EQ(grcore_snapshot_retain(nullptr), nullptr);
  EXPECT_EQ(grcore_context_snapshot(nullptr, nullptr, nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_restore(nullptr, nullptr, nullptr), GRCORE_ERR_INVALID);
}

TEST(Umbrella, ReachesTheProfilerAndTheRootSourceReader) {
  // b/profile.h and the per-source reader of b/roots.h, through the umbrella
  // alone, from C++.
  EXPECT_EQ(grcore_profiler_of(nullptr), nullptr);
  EXPECT_EQ(grcore_profiler_request(nullptr), GRCORE_ERR_INVALID);
  EXPECT_FALSE(grcore_profiler_timer_running(nullptr));
  GRCORE_Profiler * profiler = nullptr;
  EXPECT_EQ(grcore_profiler_attach(nullptr, 0, &profiler), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_root_source(nullptr, 0, nullptr, nullptr),
      GRCORE_ERR_INVALID);
}

TEST(Umbrella, ReachesTheNativeFrameReaderAndWriter) {
  EXPECT_EQ(grcore_deopt_read(nullptr, nullptr, nullptr, 0), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_deopt_write_back(nullptr, nullptr, nullptr, 0),
      GRCORE_ERR_INVALID);
}

TEST(Umbrella, ReachesTheCodeMetadataFormatAndTheLayoutDescriptor) {
  // This file includes the umbrella and nothing else, so a header the
  // umbrella omitted would not compile here.
  const GRCORE_JitLayout * layout = grcore_jit_layout();
  ASSERT_NE(layout, nullptr);
  EXPECT_EQ(layout->request_word_bytes, 8u);
  EXPECT_EQ(grcore_codemeta_find(nullptr, 0), nullptr);
  EXPECT_EQ(grcore_codemeta_validate(nullptr, 0, nullptr), GRCORE_ERR_INVALID);
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
      GRCORE_TEST_EXPAND(GRCORE_Location), GRCORE_TEST_EXPAND(GRCORE_Outcome),
      GRCORE_TEST_EXPAND(GRCORE_Stack), GRCORE_TEST_EXPAND(GRCORE_FrameRef),
      GRCORE_TEST_EXPAND(GRCORE_AbstractFrame),
      GRCORE_TEST_EXPAND(GRCORE_EngineDescriptor),
      GRCORE_TEST_EXPAND(GRCORE_PollIdentity),
      GRCORE_TEST_EXPAND(GRCORE_ActivationRef),
      GRCORE_TEST_EXPAND(GRCORE_ActivationInfo),
      GRCORE_TEST_EXPAND(GRCORE_BudgetScope),
      GRCORE_TEST_EXPAND(GRCORE_RootSource),
      GRCORE_TEST_EXPAND(GRCORE_RootVisitor),
      GRCORE_TEST_EXPAND(GRCORE_ConservativeRange),
      GRCORE_TEST_EXPAND(GRCORE_CSegment), GRCORE_TEST_EXPAND(GRCORE_Snapshot),
      GRCORE_TEST_EXPAND(GRCORE_RestoreEnv)};
  for (const char * r : renamed) {
    EXPECT_NE(std::string(r).find("ghotiio_"), std::string::npos) << r;
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

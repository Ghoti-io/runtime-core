/**
 * @file
 *
 * The JIT layout descriptor: emitted code finds the request word by offset.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstring>

namespace {

const GRCORE_Key kSvc = {"svc", GRCORE_CARDINALITY_MANY, GRCORE_PHASE_NONE,
    nullptr, nullptr};

/* What a poll's fast path does when compiled: load through the offset. */
uint64_t load_request_word(const GRCORE_Context * c) {
  const GRCORE_JitLayout * l = grcore_jit_layout();
  uint64_t word;
  std::memcpy(&word,
      reinterpret_cast<const unsigned char *>(c) + l->request_word_offset,
      sizeof word);
  return word;
}

} // namespace

TEST(Layout, TheDescriptorIsAnEightByteAlignedWordInsideTheContext) {
  const GRCORE_JitLayout * l = grcore_jit_layout();
  ASSERT_NE(l, nullptr);
  EXPECT_EQ(l->request_word_bytes, 8u);
  EXPECT_EQ(l->request_word_offset % 8, 0u);
  EXPECT_TRUE(l->request_nonzero_means_slow);
  // Inside the context: it is under the context's own size, which a test
  // can only bound from below, so read through it on a real context.
  RunWorld w;
  EXPECT_EQ(load_request_word(w.ctx), 0u);
}

TEST(Layout, TheWordReadThroughTheOffsetIsZeroFreshAndNonZeroAfterAPost) {
  RunWorld w;
  EXPECT_EQ(load_request_word(w.ctx), 0u);
  GRCORE_RequestKind kind = 0;
  ASSERT_EQ(grcore_context_request_kind(w.ctx, &kSvc, &kind), GRCORE_OK);
  GRCORE_Port * port;
  ASSERT_EQ(grcore_context_port(w.ctx, &port), GRCORE_OK);
  ASSERT_EQ(grcore_port_post(port, kind), GRCORE_OK);
  EXPECT_EQ(load_request_word(w.ctx), uint64_t{1} << kind);
  grcore_port_release(port);
  ASSERT_EQ(grcore_context_clear_request(w.ctx, kind), GRCORE_OK);
  EXPECT_EQ(load_request_word(w.ctx), 0u);
}

TEST(Layout, TerminateShowsThroughTheOffsetToo) {
  RunWorld w;
  ASSERT_EQ(grcore_context_terminate(w.ctx), GRCORE_OK);
  EXPECT_NE(load_request_word(w.ctx), 0u);
}

TEST(Layout, TheDescriptorIsTheSameStructOnEveryCall) {
  const GRCORE_JitLayout * a = grcore_jit_layout();
  const GRCORE_JitLayout * b = grcore_jit_layout();
  EXPECT_EQ(a, b);
  GRCORE_JitLayout copy = *a;
  EXPECT_EQ(copy.request_word_offset, b->request_word_offset);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

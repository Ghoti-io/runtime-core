/**
 * @file
 *
 * A root source states its own size (b/roots.h), so the struct can grow at
 * the end. No member has been added after `enumerate` yet, so there is no
 * older layout to copy; what is tested is the rule that holds the door open:
 * the initialiser writes the size, a size below the struct or off its
 * alignment is refused, a forgotten (zero) size is refused and not read, and a
 * source from a newer header is accepted with its unknown tail ignored. Each
 * refusal has a full-size control, so a registration that refuses everything
 * cannot pass.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace {

int g_enumerations = 0;

void counting_enumerate(
    GRCORE_Context *, void *, const GRCORE_RootVisitor *) {
  g_enumerations++;
}

GRCORE_RootSource make_source() {
  return GRCORE_ROOT_SOURCE_INIT("counting", counting_enumerate);
}

void enumerate_once(GRCORE_Context * c) {
  GRCORE_RootVisitor v;
  v.user = nullptr;
  v.slot = nullptr;
  v.range = nullptr;
  ASSERT_EQ(grcore_context_enumerate_roots(c, &v), GRCORE_OK);
}

} // namespace

TEST(RootSourceSize, TheInitialiserWritesTheSizeOfTheStructItWasCompiledAgainst) {
  static const GRCORE_RootSource s =
      GRCORE_ROOT_SOURCE_INIT("s", counting_enumerate);
  EXPECT_EQ(s.size, sizeof(GRCORE_RootSource));
  EXPECT_GE(s.size, GRCORE_ROOT_SOURCE_MIN_SIZE);
  EXPECT_TRUE(grcore_root_source_valid(&s));
  EXPECT_FALSE(grcore_root_source_valid(nullptr));
}

TEST(RootSourceSize, ASizeBelowTheMinimumOrOffAlignmentIsRefusedNotRead) {
  int token = 0;
  // Zero is the forgotten size: a source written with `{}` or by assignment
  // to a zeroed struct.
  const size_t bad[] = {0, 1, GRCORE_ROOT_SOURCE_MIN_SIZE - sizeof(void *),
      GRCORE_ROOT_SOURCE_MIN_SIZE - 1, GRCORE_ROOT_SOURCE_MIN_SIZE + 1,
      sizeof(GRCORE_RootSource) + 1};
  for (size_t size : bad) {
    GRCORE_RootSource s = make_source();
    s.size = size;
    EXPECT_FALSE(grcore_root_source_valid(&s)) << "size " << size;
    RunWorld w;
    g_enumerations = 0;
    EXPECT_EQ(grcore_context_add_root_source(w.ctx, &s, &token),
        GRCORE_ERR_INVALID)
        << "size " << size;
    EXPECT_EQ(grcore_context_root_source_count(w.ctx), 0u);
    enumerate_once(w.ctx);
    EXPECT_EQ(g_enumerations, 0);
  }
  // Control: at its size the same source is added and enumerated.
  GRCORE_RootSource s = make_source();
  RunWorld w;
  g_enumerations = 0;
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &s, &token), GRCORE_OK);
  enumerate_once(w.ctx);
  EXPECT_EQ(g_enumerations, 1);
}

TEST(RootSourceSize, ASourceFromANewerHeaderIsAcceptedAndItsUnknownTailIgnored) {
  // Decision: accepted, as for a key: a member this library does not know is
  // one whose absence is a defined degradation.
  int token = 0;
  const size_t extra = 2 * sizeof(void *);
  auto * block = static_cast<unsigned char *>(
      std::malloc(sizeof(GRCORE_RootSource) + extra));
  GRCORE_RootSource proto = make_source();
  std::memcpy(block, &proto, sizeof proto);
  std::memset(block + sizeof(GRCORE_RootSource), 0xA5, extra);
  auto * s = reinterpret_cast<GRCORE_RootSource *>(block);
  s->size = sizeof(GRCORE_RootSource) + extra;
  EXPECT_TRUE(grcore_root_source_valid(s));
  {
    RunWorld w;
    g_enumerations = 0;
    ASSERT_EQ(grcore_context_add_root_source(w.ctx, s, &token), GRCORE_OK);
    enumerate_once(w.ctx);
    EXPECT_EQ(g_enumerations, 1);
    EXPECT_EQ(grcore_context_remove_root_source(w.ctx, s, &token), GRCORE_OK);
  }
  std::free(block);
}

// RC-28: a root source names the codec that decodes its precise slots.

namespace {
const GRCORE_ValueCodec kBoxed = {UINT64_C(0xFFFF) << 48, UINT64_C(0xFFFC) << 48,
    UINT64_C(0x0000FFFFFFFFFFFF), 0, 0};
const GRCORE_ValueCodec kLowTag = {7, 1, ~UINT64_C(7), 0, 0};
void no_roots(GRCORE_Context *, void *, const GRCORE_RootVisitor *) {}
} // namespace

TEST(RootSourceCodec, ASourceWithoutOneHasNoneAndOneWithOneReadsBackThatOne) {
  static const GRCORE_RootSource plain = GRCORE_ROOT_SOURCE_INIT("plain", no_roots);
  static const GRCORE_RootSource boxed =
      GRCORE_ROOT_SOURCE_INIT_CODEC("boxed", no_roots, &kBoxed);
  EXPECT_EQ(grcore_root_source_codec(&plain), nullptr);
  EXPECT_EQ(grcore_root_source_codec(&boxed), &kBoxed);
  EXPECT_EQ(grcore_root_source_codec(nullptr), nullptr);
  // Control: two sources with different codecs coexist on one context and each
  // reads back its own (the two-engine case).
  static const GRCORE_RootSource low =
      GRCORE_ROOT_SOURCE_INIT_CODEC("low", no_roots, &kLowTag);
  RunWorld w;
  int a = 0, b = 0;
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &boxed, &a), GRCORE_OK);
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &low, &b), GRCORE_OK);
  const GRCORE_RootSource * got = nullptr;
  ASSERT_EQ(grcore_context_root_source(w.ctx, 0, &got, nullptr), GRCORE_OK);
  EXPECT_EQ(grcore_root_source_codec(got), &kBoxed);
  ASSERT_EQ(grcore_context_root_source(w.ctx, 1, &got, nullptr), GRCORE_OK);
  EXPECT_EQ(grcore_root_source_codec(got), &kLowTag);
}

TEST(RootSourceCodec, AMalformedCodecIsRefusedAndNothingIsAdded) {
  const GRCORE_ValueCodec bad[] = {
      {0, 1, ~UINT64_C(0), 0, 0},   // tag_value outside tag_mask
      {7, 8, ~UINT64_C(0), 0, 0},   // likewise
      {0, 0, ~UINT64_C(0), 64, 0},  // shift not below 64
  };
  for (const GRCORE_ValueCodec & c : bad) {
    EXPECT_FALSE(grcore_value_codec_valid(&c));
    GRCORE_RootSource s = GRCORE_ROOT_SOURCE_INIT_CODEC("bad", no_roots, &c);
    EXPECT_FALSE(grcore_root_source_valid(&s));
    RunWorld w;
    int token = 0;
    EXPECT_EQ(grcore_context_add_root_source(w.ctx, &s, &token), GRCORE_ERR_INVALID);
    EXPECT_EQ(grcore_context_root_source_count(w.ctx), 0u);
  }
  EXPECT_FALSE(grcore_value_codec_valid(nullptr));
  EXPECT_TRUE(grcore_value_codec_valid(&kBoxed));
}

TEST(RootSourceCodec, ASourceCompiledBeforeTheMemberExistedHasNoCodecAndStaysValid) {
  // A source whose `size` ends at `enumerate` must not have `codec` read: the
  // bytes after it are not its own. Plant a codec there and see it ignored.
  struct Old {
    size_t size;
    const char * name;
    void (*enumerate)(GRCORE_Context *, void *, const GRCORE_RootVisitor *);
    const GRCORE_ValueCodec * codec; // the bytes past the old struct
  };
  Old old = {GRCORE_ROOT_SOURCE_MIN_SIZE, "old", no_roots, &kBoxed};
  auto * s = reinterpret_cast<const GRCORE_RootSource *>(&old);
  EXPECT_TRUE(grcore_root_source_valid(s));
  EXPECT_EQ(grcore_root_source_codec(s), nullptr);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

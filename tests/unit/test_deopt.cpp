/**
 * @file
 *
 * Reading and writing a native frame by its metadata, over a hand-built site
 * and a stack-allocated frame.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstdint>
#include <cstring>

namespace {

// A frame of eight words; "rbp" points just past the end, as a native frame
// base does: word k is at base - 8 * (8 - k), that is offsets -64 .. -8.
struct Frame {
  uint64_t words[8];
  void * base() { return words + 8; }
  const void * base() const { return words + 8; }
  static int64_t offset(int word) { return -8 * (8 - word); }
};

const uint64_t kMark = 0xAAAAAAAAAAAAAAAAull;

void fill(Frame & f) {
  for (int i = 0; i < 8; i++) {
    f.words[i] = 0x1000 + static_cast<uint64_t>(i);
  }
}

GRCORE_CodeLocation slot(int word, GRCORE_SlotKind kind) {
  return {GRCORE_LOC_FRAME_SLOT, kind, Frame::offset(word)};
}

GRCORE_CodeSite site_of(const GRCORE_CodeLocation * locs, size_t n) {
  GRCORE_CodeSite s{};
  s.frame_state = locs;
  s.frame_state_count = n;
  return s;
}

} // namespace

TEST(Deopt, AMixedSiteReadsExactlyTheRightWords) {
  Frame f;
  fill(f);
  GRCORE_CodeLocation locs[] = {
      slot(2, GRCORE_SLOT_VALUE),
      {GRCORE_LOC_CONSTANT, GRCORE_SLOT_RAW, 77},
      {GRCORE_LOC_DEAD, GRCORE_SLOT_VALUE, 0},
      slot(7, GRCORE_SLOT_RAW),
      slot(0, GRCORE_SLOT_VALUE),
      {GRCORE_LOC_CONSTANT, GRCORE_SLOT_RAW, -1},
  };
  GRCORE_CodeSite s = site_of(locs, 6);
  uint64_t out[6];
  std::memset(out, 0xEE, sizeof out);
  ASSERT_EQ(grcore_deopt_read(&s, f.base(), out, 6), GRCORE_OK);
  EXPECT_EQ(out[0], 0x1002u);
  EXPECT_EQ(out[1], 77u);
  EXPECT_EQ(out[2], 0u);
  EXPECT_EQ(out[3], 0x1007u);
  EXPECT_EQ(out[4], 0x1000u);
  EXPECT_EQ(out[5], UINT64_MAX);
}

TEST(Deopt, AnOffByEightLocationReadsTheWrongWord) {
  // If the implementation ignored the offset this would still pass for the
  // right location; the second site must read a different word.
  Frame f;
  fill(f);
  GRCORE_CodeLocation right[] = {slot(3, GRCORE_SLOT_VALUE)};
  GRCORE_CodeLocation wrong[] = {
      {GRCORE_LOC_FRAME_SLOT, GRCORE_SLOT_VALUE, Frame::offset(3) + 8}};
  GRCORE_CodeSite a = site_of(right, 1), b = site_of(wrong, 1);
  uint64_t x = 0, y = 0;
  ASSERT_EQ(grcore_deopt_read(&a, f.base(), &x, 1), GRCORE_OK);
  ASSERT_EQ(grcore_deopt_read(&b, f.base(), &y, 1), GRCORE_OK);
  EXPECT_EQ(x, 0x1003u);
  EXPECT_EQ(y, 0x1004u);
  EXPECT_NE(x, y);
}

TEST(Deopt, WriteBackUpdatesValueSlotsOnly) {
  Frame f;
  fill(f);
  GRCORE_CodeLocation locs[] = {
      slot(1, GRCORE_SLOT_VALUE),
      slot(3, GRCORE_SLOT_RAW),
      {GRCORE_LOC_CONSTANT, GRCORE_SLOT_VALUE, 5},
      {GRCORE_LOC_DEAD, GRCORE_SLOT_VALUE, 0},
      slot(5, GRCORE_SLOT_VALUE),
  };
  GRCORE_CodeSite s = site_of(locs, 5);
  uint64_t in[5] = {kMark, kMark + 1, kMark + 2, kMark + 3, kMark + 4};
  ASSERT_EQ(grcore_deopt_write_back(&s, f.base(), in, 5), GRCORE_OK);
  EXPECT_EQ(f.words[1], kMark);          // VALUE: written
  EXPECT_EQ(f.words[5], kMark + 4);      // VALUE: written
  EXPECT_EQ(f.words[3], 0x1003u);        // RAW: untouched
  for (int untouched : {0, 2, 4, 6, 7}) { // words no location names (or RAW)
    EXPECT_EQ(f.words[untouched], 0x1000u + static_cast<uint64_t>(untouched))
        << "word " << untouched;
  }
}

TEST(Deopt, ReadAfterWriteBackRoundTripsTheValueSlots) {
  Frame f;
  fill(f);
  GRCORE_CodeLocation locs[] = {slot(0, GRCORE_SLOT_VALUE),
      slot(6, GRCORE_SLOT_VALUE)};
  GRCORE_CodeSite s = site_of(locs, 2);
  uint64_t in[2] = {11, 22}, out[2] = {0, 0};
  ASSERT_EQ(grcore_deopt_write_back(&s, f.base(), in, 2), GRCORE_OK);
  ASSERT_EQ(grcore_deopt_read(&s, f.base(), out, 2), GRCORE_OK);
  EXPECT_EQ(out[0], 11u);
  EXPECT_EQ(out[1], 22u);
}

TEST(Deopt, RefusalsWriteNothing) {
  Frame f;
  fill(f);
  GRCORE_CodeLocation locs[] = {slot(1, GRCORE_SLOT_VALUE),
      slot(2, GRCORE_SLOT_VALUE)};
  GRCORE_CodeSite s = site_of(locs, 2);
  uint64_t out[3] = {kMark, kMark, kMark};
  EXPECT_EQ(grcore_deopt_read(&s, f.base(), out, 3), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_deopt_read(&s, f.base(), out, 1), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_deopt_read(nullptr, f.base(), out, 2), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_deopt_read(&s, nullptr, out, 2), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_deopt_read(&s, f.base(), nullptr, 2), GRCORE_ERR_INVALID);
  EXPECT_EQ(out[0], kMark);
  EXPECT_EQ(out[1], kMark);
  EXPECT_EQ(out[2], kMark);
  EXPECT_EQ(grcore_deopt_write_back(&s, f.base(), out, 3), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_deopt_write_back(&s, f.base(), out, 1), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_deopt_write_back(nullptr, f.base(), out, 2),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_deopt_write_back(&s, nullptr, out, 2), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_deopt_write_back(&s, f.base(), nullptr, 2),
      GRCORE_ERR_INVALID);
  for (int i = 0; i < 8; i++) {
    EXPECT_EQ(f.words[i], 0x1000u + static_cast<uint64_t>(i));
  }
}

TEST(Deopt, AnEmptySiteIsValid) {
  Frame f;
  fill(f);
  GRCORE_CodeSite s = site_of(nullptr, 0);
  uint64_t out[1] = {kMark};
  EXPECT_EQ(grcore_deopt_read(&s, f.base(), out, 0), GRCORE_OK);
  EXPECT_EQ(grcore_deopt_write_back(&s, f.base(), out, 0), GRCORE_OK);
  EXPECT_EQ(out[0], kMark);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

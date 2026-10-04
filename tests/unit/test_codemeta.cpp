/**
 * @file
 *
 * The code-metadata format (AD-17): validation, lookup and the umbrella.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <algorithm>
#include <cstring>
#include <random>
#include <string>

namespace {

/* A table under construction: owns the arrays the sites point into. */
struct Built {
  struct Site {
    uint32_t offset;
    GRCORE_CodeSiteKind kind;
    GRCORE_PollIdentity identity;
    std::vector<GRCORE_CodeLocation> live;
    std::vector<GRCORE_DerivedPointer> derived;
    std::vector<GRCORE_CodeLocation> state;
  };
  std::vector<Site> parts;
  std::vector<GRCORE_CodeSite> sites;
  GRCORE_CodeMeta meta{};

  void finish(uint32_t frame_bytes, uint32_t code_bytes) {
    sites.clear();
    for (auto & p : parts) {
      GRCORE_CodeSite s{};
      s.code_offset = p.offset;
      s.kind = p.kind;
      s.identity = p.identity;
      s.live = p.live.empty() ? nullptr : p.live.data();
      s.live_count = p.live.size();
      s.derived = p.derived.empty() ? nullptr : p.derived.data();
      s.derived_count = p.derived.size();
      s.frame_state = p.state.empty() ? nullptr : p.state.data();
      s.frame_state_count = p.state.size();
      sites.push_back(s);
    }
    meta.version = GRCORE_CODEMETA_FORMAT_VERSION;
    meta.frame_bytes = frame_bytes;
    meta.code_bytes = code_bytes;
    meta.site_count = sites.size();
    meta.sites = sites.empty() ? nullptr : sites.data();
  }
};

GRCORE_CodeLocation slot(int64_t offset, GRCORE_SlotKind kind) {
  return {GRCORE_LOC_FRAME_SLOT, kind, offset};
}

/* Three sites of one function with two interpreter slots, one frame of 64. */
Built valid() {
  Built b;
  for (uint32_t i = 0; i < 3; i++) {
    Built::Site s;
    s.offset = 10 + i * 10;
    s.kind = i == 2 ? GRCORE_SITE_GUARD : GRCORE_SITE_GC_POINT_CALL;
    s.identity = {7, i * 4};
    s.live = {slot(-8, GRCORE_SLOT_VALUE), slot(-16, GRCORE_SLOT_VALUE)};
    s.derived = {{-24, -8, 16}};
    s.state = {slot(-8, GRCORE_SLOT_VALUE),
        {GRCORE_LOC_CONSTANT, GRCORE_SLOT_RAW, 99}};
    b.parts.push_back(s);
  }
  b.finish(64, 100);
  return b;
}

const char * refuse(const Built & b) {
  const char * why = nullptr;
  EXPECT_EQ(grcore_codemeta_validate(&b.meta, b.meta.code_bytes, &why),
      GRCORE_ERR_CORRUPT);
  EXPECT_NE(why, nullptr);
  return why;
}

} // namespace

TEST(CodeMeta, AValidTableValidatesAndFindsEverySiteAndNoOffsetBetween) {
  Built b = valid();
  const char * why = "untouched";
  ASSERT_EQ(grcore_codemeta_validate(&b.meta, 100, &why), GRCORE_OK);
  EXPECT_STREQ(why, "untouched"); // written only on corruption
  for (uint32_t off = 0; off < 100; off++) {
    const GRCORE_CodeSite * s = grcore_codemeta_find(&b.meta, off);
    bool is_site = off == 10 || off == 20 || off == 30;
    if (is_site) {
      ASSERT_NE(s, nullptr) << off;
      EXPECT_EQ(s->code_offset, off);
    } else {
      EXPECT_EQ(s, nullptr) << off;
    }
  }
  EXPECT_EQ(grcore_codemeta_find(&b.meta, 100000), nullptr);
  EXPECT_EQ(grcore_codemeta_find(nullptr, 10), nullptr);
}

TEST(CodeMeta, ANullReasonIsAccepted) {
  Built b = valid();
  EXPECT_EQ(grcore_codemeta_validate(&b.meta, 100, nullptr), GRCORE_OK);
  b.meta.version = 2;
  EXPECT_EQ(grcore_codemeta_validate(&b.meta, 100, nullptr),
      GRCORE_ERR_CORRUPT);
  EXPECT_EQ(grcore_codemeta_validate(nullptr, 100, nullptr),
      GRCORE_ERR_INVALID);
}

TEST(CodeMeta, AnEmptyTableIsValid) {
  Built b;
  b.finish(0, 0);
  EXPECT_EQ(grcore_codemeta_validate(&b.meta, 0, nullptr), GRCORE_OK);
  EXPECT_EQ(grcore_codemeta_find(&b.meta, 0), nullptr);
}

TEST(CodeMeta, AWrongVersionIsCorrupt) {
  Built b = valid();
  b.meta.version = GRCORE_CODEMETA_FORMAT_VERSION + 1;
  EXPECT_NE(std::strstr(refuse(b), "version"), nullptr);
}

TEST(CodeMeta, ACodeSizeThatIsNotTheCodesIsCorrupt) {
  Built b = valid();
  const char * why = nullptr;
  EXPECT_EQ(grcore_codemeta_validate(&b.meta, 101, &why), GRCORE_ERR_CORRUPT);
  EXPECT_NE(why, nullptr);
}

TEST(CodeMeta, OffsetsThatDoNotIncreaseStrictlyAreCorrupt) {
  Built b = valid();
  b.parts[1].offset = 10; // equal to the one before
  b.finish(64, 100);
  EXPECT_NE(std::strstr(refuse(b), "increasing"), nullptr);
  b.parts[1].offset = 5; // before it
  b.finish(64, 100);
  EXPECT_NE(std::strstr(refuse(b), "increasing"), nullptr);
}

TEST(CodeMeta, AnOffsetPastTheCodeIsCorrupt) {
  Built b = valid();
  b.parts[2].offset = 100; // the first byte past the code
  b.finish(64, 100);
  EXPECT_NE(std::strstr(refuse(b), "past the code"), nullptr);
}

TEST(CodeMeta, ASlotThatIsNotEightByteAlignedIsCorrupt) {
  Built b = valid();
  b.parts[0].live[0].value = -12;
  b.finish(64, 100);
  EXPECT_NE(std::strstr(refuse(b), "misaligned"), nullptr);
}

TEST(CodeMeta, ASlotOutsideTheFrameIsCorrupt) {
  for (int64_t bad : {int64_t{0}, int64_t{8}, int64_t{-72}, int64_t{-1000}}) {
    Built b = valid();
    b.parts[1].state[0].value = bad;
    b.finish(64, 100);
    EXPECT_NE(std::strstr(refuse(b), "outside the frame"), nullptr) << bad;
  }
  Built edge = valid(); // -64 is the last slot of a 64-byte frame
  edge.parts[1].state[0].value = -64;
  edge.finish(64, 100);
  EXPECT_EQ(grcore_codemeta_validate(&edge.meta, 100, nullptr), GRCORE_OK);
}

TEST(CodeMeta, ADerivedPointerWhoseBaseIsNotLiveIsCorrupt) {
  Built b = valid();
  b.parts[2].derived[0].base_slot = -32; // in the frame, but not live
  b.finish(64, 100);
  EXPECT_NE(std::strstr(refuse(b), "base is not a live"), nullptr);
}

TEST(CodeMeta, ADerivedPointerRoundTrips) {
  Built b = valid();
  const GRCORE_CodeSite * s = grcore_codemeta_find(&b.meta, 20);
  ASSERT_NE(s, nullptr);
  ASSERT_EQ(s->derived_count, 1u);
  EXPECT_EQ(s->derived[0].slot, -24);
  EXPECT_EQ(s->derived[0].base_slot, -8);
  EXPECT_EQ(s->derived[0].delta, 16);
  // The value a reader reconstructs: base word plus delta.
  uint64_t frame[8] = {0, 0, 0, 0x1000, 0, 0, 0, 0}; // slot -8 is index 7
  frame[7] = 0x4000;
  EXPECT_EQ(frame[7] + static_cast<uint64_t>(s->derived[0].delta), 0x4010u);
}

TEST(CodeMeta, ADerivedPointersBaseThatIsOnlyALiveRawSlotIsCorrupt) {
  Built b = valid();
  b.parts[1].live[0].slot_kind = GRCORE_SLOT_RAW; // slot -8, the base
  b.finish(64, 100);
  EXPECT_NE(std::strstr(refuse(b), "base is not a live"), nullptr);
}

TEST(CodeMeta, AnInvalidSlotKindIsCorruptInAStackMapAndInAFrameState) {
  Built b = valid();
  b.parts[0].live[1].slot_kind = static_cast<GRCORE_SlotKind>(9);
  b.finish(64, 100);
  EXPECT_NE(std::strstr(refuse(b), "slot kind"), nullptr);
  b = valid();
  b.parts[2].state[1].slot_kind = static_cast<GRCORE_SlotKind>(9);
  b.finish(64, 100);
  EXPECT_NE(std::strstr(refuse(b), "slot kind"), nullptr);
}

TEST(CodeMeta, ADerivedPointersOwnAndBaseSlotsAreRangeChecked) {
  for (int64_t bad : {int64_t{-12}, int64_t{0}, int64_t{-72}, int64_t{8}}) {
    Built own = valid();
    own.parts[1].derived[0].slot = bad;
    own.finish(64, 100);
    EXPECT_NE(std::strstr(refuse(own), "derived pointer slot"), nullptr) << bad;
    Built base = valid();
    base.parts[1].derived[0].base_slot = bad;
    base.finish(64, 100);
    EXPECT_NE(std::strstr(refuse(base), "derived pointer slot"), nullptr) << bad;
  }
}

TEST(CodeMeta, TwoSitesOfOneFunctionMustAgreeOnTheSlotCount) {
  Built b = valid();
  b.parts[2].state.push_back({GRCORE_LOC_DEAD, GRCORE_SLOT_RAW, 0});
  b.finish(64, 100);
  EXPECT_NE(std::strstr(refuse(b), "slot count"), nullptr);
  // A different function may have a different count.
  b.parts[2].identity.function = 8;
  b.finish(64, 100);
  EXPECT_EQ(grcore_codemeta_validate(&b.meta, 100, nullptr), GRCORE_OK);
}

TEST(CodeMeta, WideSitesAndManyFunctionsAreValidatedAndTheirFlawsStillFound) {
  // 40 sites of 300 live references and 300 derived pointers each, whose bases
  // are found among the live slots by the fast path; and 5,000 sites of as
  // many functions, two of them the same function far apart.
  Built wide;
  for (uint32_t i = 0; i < 40; i++) {
    Built::Site s;
    s.offset = 10 + i * 10;
    s.kind = GRCORE_SITE_GC_POINT_CALL;
    s.identity = {7, i};
    for (int64_t k = 0; k < 300; k++) {
      s.live.push_back(slot(-8 * (k + 1), GRCORE_SLOT_VALUE));
      s.derived.push_back({-8 * (300 + k + 1), -8 * (300 - k), 16});
    }
    wide.parts.push_back(s);
  }
  wide.finish(8 * 600, 10 * 40 + 20);
  EXPECT_EQ(grcore_codemeta_validate(&wide.meta, wide.meta.code_bytes, nullptr), GRCORE_OK);
  // The last site's last derived pointer is based on a slot that is only RAW.
  wide.parts.back().live[0].slot_kind = GRCORE_SLOT_RAW; // slot -8, the base of the last
  wide.finish(8 * 600, 10 * 40 + 20);
  const char * why = refuse(wide);
  EXPECT_NE(std::string(why).find("derived pointer's base"), std::string::npos) << why;
  // A marking left over from the refused site must not make the next table
  // look valid or invalid: validate the good one again.
  wide.parts.back().live[0].slot_kind = GRCORE_SLOT_VALUE;
  wide.finish(8 * 600, 10 * 40 + 20);
  EXPECT_EQ(grcore_codemeta_validate(&wide.meta, wide.meta.code_bytes, nullptr), GRCORE_OK);

  Built many;
  constexpr uint32_t kMany = 5000;
  for (uint32_t i = 0; i < kMany; i++) {
    Built::Site s;
    s.offset = 4 + i * 4;
    s.kind = GRCORE_SITE_GUARD;
    s.identity = {1000 + i, 0};
    s.state = {{GRCORE_LOC_CONSTANT, GRCORE_SLOT_RAW, 1}};
    many.parts.push_back(s);
  }
  many.finish(64, 4 * kMany + 8);
  EXPECT_EQ(grcore_codemeta_validate(&many.meta, many.meta.code_bytes, nullptr), GRCORE_OK);
  // The last site now names the first site's function with another slot count.
  many.parts.back().identity.function = 1000;
  many.parts.back().state.push_back({GRCORE_LOC_CONSTANT, GRCORE_SLOT_RAW, 2});
  many.finish(64, 4 * kMany + 8);
  why = refuse(many);
  EXPECT_NE(std::string(why).find("disagree on the slot count"), std::string::npos) << why;
}

TEST(CodeMeta, ANullArrayWithANonZeroCountIsCorrupt) {
  Built b = valid();
  b.finish(64, 100);
  b.sites[1].live = nullptr;
  EXPECT_NE(std::strstr(refuse(b), "NULL"), nullptr);
  b = valid();
  b.sites[1].derived = nullptr;
  EXPECT_NE(std::strstr(refuse(b), "NULL"), nullptr);
  b = valid();
  b.sites[1].frame_state = nullptr;
  EXPECT_NE(std::strstr(refuse(b), "NULL"), nullptr);
  b = valid();
  b.meta.sites = nullptr;
  EXPECT_NE(std::strstr(refuse(b), "NULL"), nullptr);
}

TEST(CodeMeta, AnUnknownKindIsCorrupt) {
  Built b = valid();
  b.parts[0].kind = GRCORE_SITE_KIND_COUNT;
  b.finish(64, 100);
  EXPECT_NE(std::strstr(refuse(b), "site kind"), nullptr);
  b = valid();
  b.parts[0].state[1].kind = GRCORE_LOC_KIND_COUNT;
  b.finish(64, 100);
  EXPECT_NE(std::strstr(refuse(b), "location kind"), nullptr);
  b = valid();
  b.parts[0].live[0].kind = GRCORE_LOC_CONSTANT;
  b.finish(64, 100);
  EXPECT_NE(std::strstr(refuse(b), "not a frame slot"), nullptr);
}

TEST(CodeMeta, ARandomisedBuildValidateFindProperty) {
  std::mt19937_64 rng(0x6a17c0de);
  for (int round = 0; round < 300; round++) {
    uint32_t frame_bytes = 8 * (1 + rng() % 32);
    uint32_t code_bytes = 64 + static_cast<uint32_t>(rng() % 4096);
    size_t n = rng() % 40;
    std::vector<uint32_t> offsets;
    for (size_t i = 0; i < n; i++) {
      offsets.push_back(static_cast<uint32_t>(rng() % code_bytes));
    }
    std::sort(offsets.begin(), offsets.end());
    offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
    size_t interp = rng() % 6;
    Built b;
    for (uint32_t off : offsets) {
      Built::Site s;
      s.offset = off;
      s.kind = static_cast<GRCORE_CodeSiteKind>(rng() % GRCORE_SITE_KIND_COUNT);
      s.identity = {rng() % 3, rng() % 100};
      size_t slots = frame_bytes / 8;
      for (size_t k = 0; k < slots; k++) {
        if (rng() % 3 == 0) {
          s.live.push_back(
              slot(-8 * static_cast<int64_t>(k + 1), GRCORE_SLOT_VALUE));
        }
      }
      for (size_t k = 0; k < slots; k++) {
        if (!s.live.empty() && rng() % 5 == 0) {
          s.derived.push_back({-8 * static_cast<int64_t>(k + 1),
              s.live[rng() % s.live.size()].value,
              static_cast<int64_t>(rng() % 64)});
        }
      }
      b.parts.push_back(s);
    }
    // One slot count per function identity: pick it from the function.
    for (auto & p : b.parts) {
      size_t count = (p.identity.function + interp) % 6;
      for (size_t k = 0; k < count; k++) {
        p.state.push_back(rng() % 2 ? slot(-8, GRCORE_SLOT_VALUE)
                                    : GRCORE_CodeLocation{GRCORE_LOC_DEAD,
                                          GRCORE_SLOT_RAW, 0});
      }
    }
    b.finish(frame_bytes, code_bytes);
    ASSERT_EQ(grcore_codemeta_validate(&b.meta, code_bytes, nullptr),
        GRCORE_OK)
        << round;
    size_t found = 0;
    for (uint32_t off = 0; off < code_bytes; off++) {
      const GRCORE_CodeSite * s = grcore_codemeta_find(&b.meta, off);
      bool is_site = std::binary_search(offsets.begin(), offsets.end(), off);
      ASSERT_EQ(s != nullptr, is_site) << round << " " << off;
      if (s != nullptr) {
        found++;
        ASSERT_EQ(s->code_offset, off);
      }
    }
    ASSERT_EQ(found, offsets.size());
    // Breaking one offset makes the table corrupt.
    if (offsets.size() > 1) {
      b.sites[1].code_offset = b.sites[0].code_offset;
      ASSERT_EQ(grcore_codemeta_validate(&b.meta, code_bytes, nullptr),
          GRCORE_ERR_CORRUPT);
    }
  }
}

TEST(CodeMeta, TheUmbrellaIncludesTheHeaderFromCxx) {
  GRCORE_CodeMeta m{};
  EXPECT_EQ(m.site_count, 0u);
  EXPECT_EQ(GRCORE_CODEMETA_FORMAT_VERSION, 1u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

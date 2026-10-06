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
  return {GRCORE_LOC_FRAME_SLOT, kind, Frame::offset(word), GRCORE_REPR_BITS};
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
      {GRCORE_LOC_CONSTANT, GRCORE_SLOT_RAW, 77, GRCORE_REPR_BITS},
      {GRCORE_LOC_DEAD, GRCORE_SLOT_VALUE, 0, GRCORE_REPR_BITS},
      slot(7, GRCORE_SLOT_RAW),
      slot(0, GRCORE_SLOT_VALUE),
      {GRCORE_LOC_CONSTANT, GRCORE_SLOT_RAW, -1, GRCORE_REPR_BITS},
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
      {GRCORE_LOC_FRAME_SLOT, GRCORE_SLOT_VALUE, Frame::offset(3) + 8, GRCORE_REPR_BITS}};
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
      {GRCORE_LOC_CONSTANT, GRCORE_SLOT_VALUE, 5, GRCORE_REPR_BITS},
      {GRCORE_LOC_DEAD, GRCORE_SLOT_VALUE, 0, GRCORE_REPR_BITS},
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

// ---- Representations (AD-27) ------------------------------------------------

namespace {

GRCORE_CodeLocation tagged(int word, GRCORE_Representation r,
    GRCORE_SlotKind kind = GRCORE_SLOT_RAW) {
  GRCORE_CodeLocation l = slot(word, kind);
  l.representation = r;
  return l;
}

const uint64_t kI32 = 0xFFFFFFFBull;            // -5
const uint64_t kI64 = 0x123456789ABCDEF0ull;
const uint64_t kF32 = 0x3FC00000ull;            // 1.5f
const uint64_t kF64 = 0x400921FB54442D18ull;    // pi

// A context with the converting engine registered, its log installed, and a
// pool sized for the "reservation".
struct ConvWorld : RunWorld {
  GRCORE_EngineId engine = 0;
  ConvLog log;
  ConvWorld() {
    log.pool.reserve(64);
    log.pool_limit = 64;
    g_conv = &log;
    EXPECT_EQ(grcore_engine_register(ctx, &kConv, &engine), GRCORE_OK);
  }
  ~ConvWorld() { g_conv = nullptr; }
};

// Words 0..3 are I32, I64, F32, F64; word 4 is BITS.
struct Five {
  Frame f;
  GRCORE_CodeLocation locs[5];
  GRCORE_CodeSite site;
  Five() {
    fill(f);
    f.words[0] = kI32;
    f.words[1] = kI64;
    f.words[2] = kF32;
    f.words[3] = kF64;
    f.words[4] = 0x77;
    locs[0] = tagged(0, GRCORE_REPR_I32);
    locs[1] = tagged(1, GRCORE_REPR_I64);
    locs[2] = tagged(2, GRCORE_REPR_F32);
    locs[3] = tagged(3, GRCORE_REPR_F64);
    locs[4] = tagged(4, GRCORE_REPR_BITS);
    site = site_of(locs, 5);
  }
};

} // namespace

TEST(DeoptRepr, EveryRepresentationRoundTripsAndIsSeenTaggedBeforeAnyRebuild) {
  ConvWorld w;
  Five m;
  GRCORE_DeoptSlot raw[5];
  ASSERT_EQ(grcore_deopt_read_tagged(&m.site, m.f.base(), raw, 5), GRCORE_OK);
  const GRCORE_Representation want[5] = {GRCORE_REPR_I32, GRCORE_REPR_I64,
      GRCORE_REPR_F32, GRCORE_REPR_F64, GRCORE_REPR_BITS};
  const uint64_t words[5] = {kI32, kI64, kF32, kF64, 0x77};
  for (int i = 0; i < 5; i++) {
    EXPECT_EQ(raw[i].representation, want[i]) << i;
    EXPECT_EQ(raw[i].word, words[i]) << i;
  }
  EXPECT_EQ(w.log.converts, 0); // reading converts nothing

  EXPECT_EQ(grcore_deopt_converting_count(&m.site), 4u);
  GRCORE_DeoptReservation * res = nullptr;
  ASSERT_EQ(grcore_deopt_reserve(w.ctx, 4, &res), GRCORE_OK);
  uint64_t slots[5];
  ASSERT_EQ(grcore_deopt_rebuild(w.ctx, w.engine, &m.site, m.f.base(), res,
                slots, 5),
      GRCORE_OK);
  EXPECT_EQ(w.log.converts, 4);
  EXPECT_EQ(slots[0] >> kConvTagShift, 1u);
  EXPECT_EQ(slots[1] >> kConvTagShift, 2u);
  EXPECT_EQ(slots[2] >> kConvTagShift, 3u);
  EXPECT_EQ(slots[3] >> kConvTagShift, 4u);
  EXPECT_EQ(slots[4], 0x77u); // BITS: verbatim

  // Scribble the native frame, then write back: the raw words return.
  Frame before = m.f;
  for (int i = 0; i < 4; i++) {
    m.f.words[i] = kMark;
  }
  GRCORE_DeoptOutcome outcome = GRCORE_DEOPT_EXIT_AT_SITE;
  ASSERT_EQ(grcore_deopt_write_back_checked(w.ctx, w.engine, &m.site,
                m.f.base(), slots, 5, &outcome, nullptr),
      GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_DEOPT_WRITTEN);
  for (int i = 0; i < 4; i++) {
    EXPECT_EQ(m.f.words[i], before.words[i]) << i;
  }
  EXPECT_EQ(m.f.words[4], 0x77u); // BITS is not written back
  grcore_deopt_release(w.ctx, res);
}

TEST(DeoptRepr, AZeroRepresentationReadsAsBitsAndIsCopiedVerbatim) {
  ConvWorld w;
  Frame f;
  fill(f);
  // Built the way a producer from before the field existed built it.
  GRCORE_CodeLocation locs[2] = {
      {GRCORE_LOC_FRAME_SLOT, GRCORE_SLOT_RAW, Frame::offset(3), {}},
      {GRCORE_LOC_CONSTANT, GRCORE_SLOT_RAW, 9, {}}};
  EXPECT_EQ(locs[0].representation, GRCORE_REPR_BITS);
  GRCORE_CodeSite s = site_of(locs, 2);
  GRCORE_DeoptSlot raw[2];
  ASSERT_EQ(grcore_deopt_read_tagged(&s, f.base(), raw, 2), GRCORE_OK);
  EXPECT_EQ(raw[0].representation, GRCORE_REPR_BITS);
  EXPECT_EQ(raw[0].word, 0x1003u);
  EXPECT_EQ(raw[1].word, 9u);
  GRCORE_DeoptReservation * res = nullptr;
  ASSERT_EQ(grcore_deopt_reserve(w.ctx, 0, &res), GRCORE_OK);
  uint64_t slots[2];
  ASSERT_EQ(grcore_deopt_rebuild(w.ctx, w.engine, &s, f.base(), res, slots, 2),
      GRCORE_OK);
  EXPECT_EQ(slots[0], 0x1003u);
  EXPECT_EQ(slots[1], 9u);
  EXPECT_EQ(w.log.converts, 0);
  grcore_deopt_release(w.ctx, res);
}

TEST(DeoptRepr, BindingRefusesAMissingConversionAndAcceptsAnEngineThatHasIt) {
  RunWorld w;
  GRCORE_EngineId conv = 0, plain = 0;
  ASSERT_EQ(grcore_engine_register(w.ctx, &kConv, &conv), GRCORE_OK);
  ASSERT_EQ(grcore_engine_register(w.ctx, &kGamma, &plain), GRCORE_OK);
  Five m;
  GRCORE_CodeSite sites[1] = {m.site};
  sites[0].code_offset = 4;
  GRCORE_CodeMeta meta{};
  meta.version = GRCORE_CODEMETA_FORMAT_VERSION;
  meta.frame_bytes = 64;
  meta.code_bytes = 16;
  meta.site_count = 1;
  meta.sites = sites;
  const char * why = nullptr;
  EXPECT_EQ(grcore_deopt_bind(w.ctx, conv, &meta, &why), GRCORE_OK);
  why = nullptr;
  EXPECT_EQ(grcore_deopt_bind(w.ctx, plain, &meta, &why), GRCORE_ERR_UNSUPPORTED);
  ASSERT_NE(why, nullptr);
  EXPECT_NE(std::strstr(why, "I32"), nullptr) << why;

  // BITS-only code binds to the engine without conversions.
  GRCORE_CodeLocation bits[2] = {slot(0, GRCORE_SLOT_RAW), slot(1, GRCORE_SLOT_VALUE)};
  sites[0] = site_of(bits, 2);
  sites[0].code_offset = 4;
  EXPECT_EQ(grcore_deopt_bind(w.ctx, plain, &meta, &why), GRCORE_OK);
  EXPECT_EQ(grcore_deopt_bind(w.ctx, 99, &meta, &why), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_deopt_bind(w.ctx, conv, nullptr, &why), GRCORE_ERR_INVALID);
}

TEST(DeoptRepr, ADescriptorWithOnlyHalfTheConversionDoesNotBind) {
  static const GRCORE_EngineDescriptor half = GRCORE_ENGINE_DESCRIPTOR_INIT("half",
      nullptr, nullptr, nullptr, GRCORE_ScopeInterface{nullptr, nullptr, nullptr},
      GRCORE_ConservativeDecoder{0, 0, 0}, nullptr, nullptr, conv_convert,
      nullptr);
  RunWorld w;
  GRCORE_EngineId id = 0;
  ASSERT_EQ(grcore_engine_register(w.ctx, &half, &id), GRCORE_OK);
  GRCORE_CodeLocation locs[1] = {tagged(0, GRCORE_REPR_F64)};
  GRCORE_CodeSite sites[1] = {site_of(locs, 1)};
  sites[0].code_offset = 4;
  GRCORE_CodeMeta meta{GRCORE_CODEMETA_FORMAT_VERSION, 64, 16, 1, sites};
  const char * why = nullptr;
  EXPECT_EQ(grcore_deopt_bind(w.ctx, id, &meta, &why), GRCORE_ERR_UNSUPPORTED);
  ASSERT_NE(why, nullptr);
  EXPECT_NE(std::strstr(why, "F64"), nullptr) << why;
}

TEST(DeoptRepr, AValueThatFailsTheTypeTestWritesNothingAndExitsAtTheSite) {
  ConvWorld w;
  Five m;
  GRCORE_DeoptReservation * res = nullptr;
  ASSERT_EQ(grcore_deopt_reserve(w.ctx, 4, &res), GRCORE_OK);
  uint64_t slots[5];
  ASSERT_EQ(grcore_deopt_rebuild(w.ctx, w.engine, &m.site, m.f.base(), res,
                slots, 5),
      GRCORE_OK);
  Frame untouched = m.f;
  for (int i = 0; i < 4; i++) {
    m.f.words[i] = kMark + static_cast<uint64_t>(i);
  }
  untouched = m.f;

  // Control: the right values write.
  GRCORE_DeoptOutcome outcome = GRCORE_DEOPT_EXIT_AT_SITE;
  size_t misfit = 99;
  ASSERT_EQ(grcore_deopt_write_back_checked(w.ctx, w.engine, &m.site,
                m.f.base(), slots, 5, &outcome, &misfit),
      GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_DEOPT_WRITTEN);
  EXPECT_EQ(m.f.words[0], kI32);
  for (int i = 0; i < 4; i++) {
    m.f.words[i] = kMark + static_cast<uint64_t>(i);
  }

  // A float-tagged value in the integer-64 slot: representable (the bits are
  // there) and still not an integer.
  uint64_t bad[5] = {slots[0], slots[2], slots[2], slots[3], slots[4]};
  ASSERT_EQ(grcore_deopt_write_back_checked(w.ctx, w.engine, &m.site,
                m.f.base(), bad, 5, &outcome, &misfit),
      GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_DEOPT_EXIT_AT_SITE);
  EXPECT_EQ(misfit, 1u);
  for (int i = 0; i < 8; i++) { // not even the slot before the bad one
    EXPECT_EQ(m.f.words[i], untouched.words[i]) << i;
  }

  // An integer-32 whose payload is too wide for 32 bits.
  uint64_t wide[5] = {(UINT64_C(1) << kConvTagShift) | 0x100000000ull, slots[1],
      slots[2], slots[3], slots[4]};
  ASSERT_EQ(grcore_deopt_write_back_checked(w.ctx, w.engine, &m.site,
                m.f.base(), wide, 5, &outcome, &misfit),
      GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_DEOPT_EXIT_AT_SITE);
  EXPECT_EQ(misfit, 0u);
  for (int i = 0; i < 8; i++) {
    EXPECT_EQ(m.f.words[i], untouched.words[i]) << i;
  }
  grcore_deopt_release(w.ctx, res);
}

TEST(DeoptRepr, ACollectionBetweenConversionsSeesNullsAndConvertedValuesOnly) {
  ConvWorld w;
  Five m;
  GRCORE_DeoptReservation * res = nullptr;
  ASSERT_EQ(grcore_deopt_reserve(w.ctx, 4, &res), GRCORE_OK);
  uint64_t slots[5];
  std::memset(slots, 0xEE, sizeof slots); // what a skipped phase 1 would leave
  int call = 0, failures = 0;
  w.log.on_convert = [&](GRCORE_Context * ctx) {
    // A forced collection: enumerate the context's roots now.
    std::vector<uint64_t> cells;
    GRCORE_RootVisitor v;
    v.user = &cells;
    v.slot = [](void * user, uint64_t * p) {
      static_cast<std::vector<uint64_t> *>(user)->push_back(*p);
    };
    v.range = nullptr;
    ASSERT_EQ(grcore_context_enumerate_roots(ctx, &v), GRCORE_OK);
    // The converted cells are the only roots (there is no guest stack here):
    // all four are visible, the first `call` converted and the rest null.
    if (cells.size() != 4u) {
      failures++;
    } else {
      for (size_t i = 0; i < 4; i++) {
        bool converted = i < static_cast<size_t>(call);
        if (converted != (cells[i] != 0)) {
          failures++;
        }
      }
    }
    // The slots being rebuilt are null at every point: never a raw word,
    // never the stale 0xEE.
    for (int i = 0; i < 4; i++) {
      if (slots[i] != 0) {
        failures++;
      }
    }
    call++;
  };
  ASSERT_EQ(grcore_deopt_rebuild(w.ctx, w.engine, &m.site, m.f.base(), res,
                slots, 5),
      GRCORE_OK);
  EXPECT_EQ(call, 4);
  EXPECT_EQ(failures, 0);
  EXPECT_EQ(slots[4], 0x77u);
  EXPECT_NE(slots[0], 0u);

  // Afterwards the cells are not roots any more: the slots are.
  size_t seen = 0;
  GRCORE_RootVisitor v;
  v.user = &seen;
  v.slot = [](void * user, uint64_t *) { ++*static_cast<size_t *>(user); };
  v.range = nullptr;
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  EXPECT_EQ(seen, 0u);
  grcore_deopt_release(w.ctx, res);
}

TEST(DeoptRepr, AShortReservationIsRefusedBeforeAnythingIsWritten) {
  ConvWorld w;
  Five m;
  GRCORE_DeoptReservation * res = nullptr;
  ASSERT_EQ(grcore_deopt_reserve(w.ctx, 3, &res), GRCORE_OK); // one short
  EXPECT_EQ(grcore_deopt_reservation_capacity(res), 3u);
  uint64_t slots[5];
  std::memset(slots, 0xEE, sizeof slots);
  EXPECT_EQ(grcore_deopt_rebuild(w.ctx, w.engine, &m.site, m.f.base(), res,
                slots, 5),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(w.log.converts, 0);
  for (uint64_t x : slots) {
    EXPECT_EQ(x, 0xEEEEEEEEEEEEEEEEull);
  }
  grcore_deopt_release(w.ctx, res);

  // The same site with exactly enough succeeds (the control).
  ASSERT_EQ(grcore_deopt_reserve(w.ctx, 4, &res), GRCORE_OK);
  EXPECT_EQ(grcore_deopt_rebuild(w.ctx, w.engine, &m.site, m.f.base(), res,
                slots, 5),
      GRCORE_OK);
  grcore_deopt_release(w.ctx, res);
}

TEST(DeoptRepr, RebuildAndWriteBackRefuseAnEngineWithoutTheConversion) {
  RunWorld w;
  GRCORE_EngineId plain = 0;
  ASSERT_EQ(grcore_engine_register(w.ctx, &kGamma, &plain), GRCORE_OK);
  Five m;
  GRCORE_DeoptReservation * res = nullptr;
  ASSERT_EQ(grcore_deopt_reserve(w.ctx, 4, &res), GRCORE_OK);
  uint64_t slots[5] = {0, 0, 0, 0, 0};
  EXPECT_EQ(grcore_deopt_rebuild(w.ctx, plain, &m.site, m.f.base(), res, slots,
                5),
      GRCORE_ERR_UNSUPPORTED);
  GRCORE_DeoptOutcome outcome;
  EXPECT_EQ(grcore_deopt_write_back_checked(w.ctx, plain, &m.site, m.f.base(),
                slots, 5, &outcome, nullptr),
      GRCORE_ERR_UNSUPPORTED);
  grcore_deopt_release(w.ctx, res);
}

TEST(DeoptRepr, APlantedWrongTagIsCaughtByTheRoundTrip) {
  // The table says F64 where the compiler's value is an I32. The test's check
  // is what an engine's own code relies on: the value it is handed for slot 0
  // is an I32 (tag 1), and what it hands back is accepted. Both fail for the
  // lie, and the right table is the control that passes them.
  ConvWorld w;
  Five right;
  Five wrong;
  wrong.locs[0] = tagged(0, GRCORE_REPR_F64);
  GRCORE_DeoptReservation * res = nullptr;
  ASSERT_EQ(grcore_deopt_reserve(w.ctx, 4, &res), GRCORE_OK);
  const uint64_t i32_five = (UINT64_C(1) << kConvTagShift) | 5;

  auto handed_an_i32 = [&](Five & m) {
    uint64_t slots[5];
    EXPECT_EQ(grcore_deopt_rebuild(w.ctx, w.engine, &m.site, m.f.base(), res,
                  slots, 5),
        GRCORE_OK);
    return slots[0] >> kConvTagShift == 1u;
  };
  auto accepts_an_i32 = [&](Five & m) {
    uint64_t slots[5] = {i32_five, 0, 0, 0, 0};
    uint64_t other[5];
    EXPECT_EQ(grcore_deopt_rebuild(w.ctx, w.engine, &m.site, m.f.base(), res,
                  other, 5),
        GRCORE_OK);
    slots[1] = other[1];
    slots[2] = other[2];
    slots[3] = other[3];
    slots[4] = other[4];
    GRCORE_DeoptOutcome o = GRCORE_DEOPT_EXIT_AT_SITE;
    EXPECT_EQ(grcore_deopt_write_back_checked(w.ctx, w.engine, &m.site,
                  m.f.base(), slots, 5, &o, nullptr),
        GRCORE_OK);
    return o == GRCORE_DEOPT_WRITTEN;
  };
  EXPECT_TRUE(handed_an_i32(right));
  EXPECT_TRUE(accepts_an_i32(right));
  EXPECT_FALSE(handed_an_i32(wrong));
  EXPECT_FALSE(accepts_an_i32(wrong));
  grcore_deopt_release(w.ctx, res);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

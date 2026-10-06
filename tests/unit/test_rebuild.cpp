/**
 * @file
 *
 * Rebuilding a chain of compiled frames into their guest frames in one piece
 * (AD-27, AD-28): `grcore_compiled_rebuild`, and the reservation each compiled
 * call extends.
 *
 * The JIT is not in these: the frames are built by hand (`HandStack` over
 * `HandCode`), with a guest frame pushed for each, as every compiled call
 * pushes one.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../../src/a/guest_internal.h"

#include <set>

namespace {

const char kOutsideBytes[64] = {};
const uintptr_t kOutside = reinterpret_cast<uintptr_t>(kOutsideBytes);

struct CodeHolder {
  HandCode code;
  explicit CodeHolder(bool dead_slot) : code(false, dead_slot, false) {}
};

/* A world with hand-built code registered for either the plain engine (which
 * converts nothing) or the converting one. Site 2 of the frame state, the word
 * at -24, has representation `rep`. */
struct RW : CodeHolder, StackWorld {
  GRCORE_EngineId conv = 0;
  GRCORE_EngineId engine = 0;
  ConvLog log;
  explicit RW(GRCORE_Representation rep = GRCORE_REPR_BITS, bool use_conv = false,
      bool dead_slot = false, const GRCORE_Allocator * allocator = nullptr)
      : CodeHolder(dead_slot),
        StackWorld(GRCORE_UNLIMITED, GRCORE_UNLIMITED, GRCORE_DEFAULT_MEMORY_RESERVE,
            GRCORE_UNLIMITED, allocator) {
    code.state[2].representation = rep;
    g_conv = &log;
    log.pool_limit = 4096;
    EXPECT_EQ(grcore_engine_register(ctx, &kConv, &conv), GRCORE_OK);
    engine = use_conv ? conv : alpha;
    EXPECT_EQ(code.add_to(ctx, engine), GRCORE_OK);
  }
  ~RW() { g_conv = nullptr; }
};

GRCORE_ActivationRef enter(GRCORE_Stack * s, GRCORE_ActivationKind kind,
    GRCORE_EngineId engine = 0) {
  GRCORE_ActivationRef ref;
  EXPECT_EQ(grcore_activation_enter(s, kind, engine, false, nullptr, &ref), GRCORE_OK);
  return ref;
}

/* n compiled frames over n guest frames. Frame d counted from the innermost is
 * stopped at site d, so it is function 100 + d and its guest frame is the
 * (n-1-d)-th from the outermost. Guest slots hold sentinels. */
struct Chain {
  HandStack st;
  size_t n;
  size_t slots;
  GRCORE_ActivationRef rec;
  std::vector<GRCORE_FrameRef> guest; // outermost first
  static uint64_t sentinel(size_t j, size_t i) { return 0xAAAA0000u + j * 16 + i; }
  Chain(RW & w, size_t frames, size_t slot_count = 3, size_t extra_above = 0,
      size_t entered_with = 1)
      : st(frames), n(frames), slots(slot_count) {
    auto push = [&](uint64_t fn, size_t j) {
      GRCORE_FrameRef f;
      EXPECT_EQ(grcore_stack_push(w.stack, w.engine, slots, &f), GRCORE_OK);
      for (size_t i = 0; i < slots; i++) {
        grcore_stack_slot_set(w.stack, f, i, sentinel(j, i));
      }
      grcore_stack_set_identity(w.stack, f, GRCORE_PollIdentity{fn, 0});
      guest.push_back(f);
    };
    size_t j = 0;
    for (; j < entered_with; j++) {
      push(100 + (n - 1 - j), j);
    }
    rec = enter(w.stack, GRCORE_ACTIVATION_JIT);
    for (; j < n; j++) {
      push(100 + (n - 1 - j), j);
    }
    for (size_t e = 0; e < extra_above; e++, j++) {
      push(999, j);
    }
    std::vector<uintptr_t> bases = st.build(w.code, 0, n, 0, kOutside);
    EXPECT_EQ(grcore_activation_set_compiled(w.stack, rec, bases[0], w.code.at(0)),
        GRCORE_OK);
  }
  uint64_t slot(GRCORE_Stack * s, size_t j, size_t i) const {
    uint64_t v = 0;
    EXPECT_EQ(grcore_stack_slot_get(s, guest[j], i, &v), GRCORE_OK);
    return v;
  }
  /* Whether every guest slot still holds its sentinel: nothing was written. */
  bool untouched(GRCORE_Stack * s) const {
    for (size_t j = 0; j < guest.size(); j++) {
      for (size_t i = 0; i < slots; i++) {
        if (slot(s, j, i) != sentinel(j, i)) {
          return false;
        }
      }
    }
    return true;
  }
};

} // namespace

TEST(ChainRebuild, EveryFrameOfAFiftyDeepRunIsRebuiltIntoItsOwnGuestFrame) {
  RW w;
  Chain c(w, 50);
  size_t rebuilt = 0;
  ASSERT_EQ(grcore_compiled_rebuild(w.ctx, nullptr, SIZE_MAX, &rebuilt), GRCORE_OK);
  EXPECT_EQ(rebuilt, 50u);
  for (size_t j = 0; j < 50; j++) {
    SCOPED_TRACE(j);
    size_t d = 49 - j; // depth from the innermost
    EXPECT_EQ(c.slot(w.stack, j, 0), HandStack::ref_of(d));
    EXPECT_EQ(c.slot(w.stack, j, 1), HandStack::trap_of(d)) << "raw words are copied as they are";
    EXPECT_EQ(c.slot(w.stack, j, 2), 7 + d);
  }
  // The frames are rebuilt, so the record carries none: a walk finds nothing.
  GRCORE_ActivationInfo info;
  ASSERT_EQ(grcore_activation_info(w.stack, c.rec, &info), GRCORE_OK);
  EXPECT_EQ(info.frame_base, 0u);
  EXPECT_EQ(info.return_address, 0u);
  GRCORE_CompiledWalk walk;
  GRCORE_CompiledFrame f;
  ASSERT_EQ(grcore_compiled_walk_begin(w.ctx, &walk), GRCORE_OK);
  EXPECT_EQ(grcore_compiled_walk_next(&walk, &f), GRCORE_CWALK_END);
  // The record stays open: the native frames are still on the stack.
  EXPECT_EQ(grcore_activation_count(w.stack), 1u);
  grcore_unwind_all(w.stack, nullptr);
}

TEST(ChainRebuild, TheRebuildStartsFromTheWalkStartCellToo) {
  // Compiled code does not set the record: it stores the cell, and the rebuild
  // starts from it.
  RW w;
  Chain c(w, 3);
  GRCORE_ActivationInfo info;
  ASSERT_EQ(grcore_activation_info(w.stack, c.rec, &info), GRCORE_OK);
  ASSERT_EQ(grcore_activation_set_compiled(w.stack, c.rec, 0, 0), GRCORE_OK);
  uintptr_t * cell = reinterpret_cast<uintptr_t *>(
      reinterpret_cast<unsigned char *>(w.ctx) + grcore_jit_layout()->walk_cell_offset);
  cell[0] = info.frame_base;
  cell[1] = info.return_address;
  size_t rebuilt = 0;
  ASSERT_EQ(grcore_compiled_rebuild(w.ctx, nullptr, SIZE_MAX, &rebuilt), GRCORE_OK);
  EXPECT_EQ(rebuilt, 3u);
  EXPECT_EQ(cell[0], 0u);
  EXPECT_EQ(c.slot(w.stack, 0, 0), HandStack::ref_of(2));
  grcore_unwind_all(w.stack, nullptr);
}

TEST(ChainRebuild, AGuestFrameAboveTheInnermostCompiledFrameIsLeftAsItIs) {
  RW w;
  Chain c(w, 4, 3, /*extra_above=*/1);
  size_t rebuilt = 0;
  ASSERT_EQ(grcore_compiled_rebuild(w.ctx, nullptr, SIZE_MAX, &rebuilt), GRCORE_OK);
  EXPECT_EQ(rebuilt, 4u);
  for (size_t j = 0; j < 4; j++) {
    EXPECT_EQ(c.slot(w.stack, j, 0), HandStack::ref_of(3 - j));
  }
  for (size_t i = 0; i < 3; i++) {
    EXPECT_EQ(c.slot(w.stack, 4, i), Chain::sentinel(4, i)) << "the callee not yet entered";
  }
  grcore_unwind_all(w.stack, nullptr);
}

TEST(ChainRebuild, ADeadLocationLeavesTheGuestFramesOwnWord) {
  RW w(GRCORE_REPR_BITS, false, /*dead_slot=*/true);
  Chain c(w, 5, /*slot_count=*/4);
  ASSERT_EQ(grcore_compiled_rebuild(w.ctx, nullptr, SIZE_MAX, nullptr), GRCORE_OK);
  for (size_t j = 0; j < 5; j++) {
    EXPECT_EQ(c.slot(w.stack, j, 0), HandStack::ref_of(4 - j));
    EXPECT_EQ(c.slot(w.stack, j, 3), Chain::sentinel(j, 3)) << "the frame keeps its own word";
  }
  grcore_unwind_all(w.stack, nullptr);
}

TEST(ChainRebuild, NoCompiledStateRebuildsNothingAndSucceeds) {
  RW w;
  size_t rebuilt = 99;
  EXPECT_EQ(grcore_compiled_rebuild(w.ctx, nullptr, SIZE_MAX, &rebuilt), GRCORE_OK);
  EXPECT_EQ(rebuilt, 0u);
  EXPECT_EQ(grcore_compiled_rebuild(nullptr, nullptr, SIZE_MAX, nullptr), GRCORE_ERR_INVALID);
}

TEST(ChainRebuild, AnUnwindRebuildsOnlyTheFramesThatSurviveIt) {
  RW w;
  Chain c(w, 10);
  size_t rebuilt = 0;
  // The ten guest frames are indexed from the outermost: keeping four means the
  // outer four are rebuilt and the inner six, which an unwind pops, are not.
  ASSERT_EQ(grcore_compiled_rebuild(w.ctx, nullptr, 4, &rebuilt), GRCORE_OK);
  EXPECT_EQ(rebuilt, 4u);
  for (size_t j = 0; j < 10; j++) {
    if (j < 4) {
      EXPECT_EQ(c.slot(w.stack, j, 0), HandStack::ref_of(9 - j)) << j;
    } else {
      for (size_t i = 0; i < 3; i++) {
        EXPECT_EQ(c.slot(w.stack, j, i), Chain::sentinel(j, i)) << j << "," << i;
      }
    }
  }
  grcore_unwind_all(w.stack, nullptr);
}

TEST(ChainRebuild, ConvertingLocationsAreConvertedInOnePieceFromTheReservation) {
  RW w(GRCORE_REPR_I32, /*use_conv=*/true);
  Chain c(w, 50);
  GRCORE_DeoptReservation * res = nullptr;
  ASSERT_EQ(grcore_deopt_reserve(w.ctx, 50, &res), GRCORE_OK);
  size_t rebuilt = 0;
  ASSERT_EQ(grcore_compiled_rebuild(w.ctx, res, SIZE_MAX, &rebuilt), GRCORE_OK);
  EXPECT_EQ(rebuilt, 50u);
  EXPECT_EQ(w.log.converts, 50);
  for (size_t j = 0; j < 50; j++) {
    size_t d = 49 - j;
    EXPECT_EQ(c.slot(w.stack, j, 2), (uint64_t{1} << kConvTagShift) | (7 + d)) << j;
    EXPECT_EQ(c.slot(w.stack, j, 0), HandStack::ref_of(d));
  }
  ASSERT_EQ(grcore_deopt_release(w.ctx, res), GRCORE_OK);
  grcore_unwind_all(w.stack, nullptr);
}

TEST(ChainRebuild, AReservationOneCellShortIsRefusedUpFrontAndNothingIsWritten) {
  RW w(GRCORE_REPR_I32, true);
  Chain c(w, 50);
  GRCORE_DeoptReservation * res = nullptr;
  ASSERT_EQ(grcore_deopt_reserve(w.ctx, 49, &res), GRCORE_OK);
  EXPECT_EQ(grcore_compiled_rebuild(w.ctx, res, SIZE_MAX, nullptr), GRCORE_ERR_INVALID);
  EXPECT_TRUE(c.untouched(w.stack));
  EXPECT_EQ(w.log.converts, 0);
  EXPECT_EQ(grcore_compiled_rebuild(w.ctx, nullptr, SIZE_MAX, nullptr), GRCORE_ERR_INVALID);
  EXPECT_TRUE(c.untouched(w.stack));
  // The walk is still there to be rebuilt once the reservation is made good.
  ASSERT_EQ(grcore_deopt_reservation_extend(w.ctx, res, 1), GRCORE_OK);
  EXPECT_EQ(grcore_deopt_reservation_capacity(res), 50u);
  EXPECT_EQ(grcore_compiled_rebuild(w.ctx, res, SIZE_MAX, nullptr), GRCORE_OK);
  ASSERT_EQ(grcore_deopt_release(w.ctx, res), GRCORE_OK);
  grcore_unwind_all(w.stack, nullptr);
}

TEST(ChainRebuild, AnEngineThatCannotConvertRefusesTheChainBeforeWritingAnything) {
  RW w(GRCORE_REPR_I32, /*use_conv=*/false);
  Chain c(w, 6);
  GRCORE_DeoptReservation * res = nullptr;
  ASSERT_EQ(grcore_deopt_reserve(w.ctx, 6, &res), GRCORE_OK);
  EXPECT_EQ(grcore_compiled_rebuild(w.ctx, res, SIZE_MAX, nullptr), GRCORE_ERR_UNSUPPORTED);
  EXPECT_TRUE(c.untouched(w.stack));
  ASSERT_EQ(grcore_deopt_release(w.ctx, res), GRCORE_OK);
  grcore_unwind_all(w.stack, nullptr);
}

TEST(ChainRebuild, EveryMismatchBetweenTheChainAndTheGuestStackIsRefusedAndWritesNothing) {
  struct Case {
    const char * name;
    std::function<void(RW &, Chain &)> mutate;
    GRCORE_Result expect;
  };
  const Case cases[] = {
      {"a guest frame of another function",
          [](RW & w, Chain & c) {
            grcore_stack_set_identity(w.stack, c.guest[2], GRCORE_PollIdentity{12345, 0});
          },
          GRCORE_ERR_INVALID},
      {"the chain ends in a corrupt return word",
          [](RW &, Chain & c) { c.st.words[2 * 8 + 7] = kOutside; },
          GRCORE_ERR_CORRUPT},
      {"the last frame has no marker",
          [](RW &, Chain & c) { c.st.words[5 * 8 + 6] = 0; },
          GRCORE_ERR_CORRUPT},
  };
  for (const Case & k : cases) {
    SCOPED_TRACE(k.name);
    RW w;
    Chain c(w, 6);
    k.mutate(w, c);
    EXPECT_EQ(grcore_compiled_rebuild(w.ctx, nullptr, SIZE_MAX, nullptr), k.expect);
    // Only what the mutation itself changed may differ from the sentinels.
    for (size_t j = 0; j < 6; j++) {
      for (size_t i = 0; i < 3; i++) {
        EXPECT_EQ(c.slot(w.stack, j, i), Chain::sentinel(j, i)) << j << "," << i;
      }
    }
    GRCORE_ActivationInfo info;
    ASSERT_EQ(grcore_activation_info(w.stack, c.rec, &info), GRCORE_OK);
    EXPECT_NE(info.frame_base, 0u) << "a refusal leaves the record as it was";
    grcore_unwind_all(w.stack, nullptr);
  }
}

TEST(ChainRebuild, AGuestFrameWithTheWrongSlotCountIsRefused) {
  RW w;
  Chain c(w, 4, /*slot_count=*/2); // the frame state describes three
  EXPECT_EQ(grcore_compiled_rebuild(w.ctx, nullptr, SIZE_MAX, nullptr), GRCORE_ERR_INVALID);
  EXPECT_TRUE(c.untouched(w.stack));
  grcore_unwind_all(w.stack, nullptr);
}

TEST(ChainRebuild, ARecordEnteredWithNoGuestFrameHasNothingToRebuildInto) {
  RW w;
  HandStack st(3);
  std::vector<uintptr_t> bases = st.build(w.code, 0, 3, 0, kOutside);
  GRCORE_ActivationRef rec = enter(w.stack, GRCORE_ACTIVATION_JIT);
  ASSERT_EQ(grcore_activation_set_compiled(w.stack, rec, bases[0], w.code.at(0)), GRCORE_OK);
  EXPECT_EQ(grcore_compiled_rebuild(w.ctx, nullptr, SIZE_MAX, nullptr), GRCORE_ERR_INVALID);
  ASSERT_EQ(grcore_activation_leave(w.stack, rec), GRCORE_OK);
}

TEST(ChainRebuild, OnlyTheInnermostRunIsRebuiltAndTheOneUnderANativeIsKept) {
  RW w;
  // An outer run of three over a native and then an inner run of two.
  HandStack st(5);
  auto push = [&](uint64_t fn) {
    GRCORE_FrameRef f;
    EXPECT_EQ(grcore_stack_push(w.stack, w.engine, 3, &f), GRCORE_OK);
    grcore_stack_slot_set(w.stack, f, 0, 5);
    grcore_stack_set_identity(w.stack, f, GRCORE_PollIdentity{fn, 0});
    return f;
  };
  push(104);
  GRCORE_ActivationRef a = enter(w.stack, GRCORE_ACTIVATION_JIT);
  push(103);
  push(102); // the outer run's three: 104, 103, 102 over sites 4..2? built below
  std::vector<uintptr_t> outer = st.build(w.code, 16, 3, 2, kOutside);
  ASSERT_EQ(grcore_activation_set_compiled(w.stack, a, outer[0], w.code.at(2)), GRCORE_OK);
  GRCORE_ActivationRef n = enter(w.stack, GRCORE_ACTIVATION_NATIVE);
  GRCORE_FrameRef g0 = push(101);
  GRCORE_ActivationRef b = enter(w.stack, GRCORE_ACTIVATION_JIT);
  push(100);
  std::vector<uintptr_t> inner = st.build(w.code, 0, 2, 0, kOutside);
  ASSERT_EQ(grcore_activation_set_compiled(w.stack, b, inner[0], w.code.at(0)), GRCORE_OK);
  (void)g0;
  size_t rebuilt = 0;
  // The inner record entered with five guest frames: its run's outermost frame
  // (function 101) is guest frame 4, and its innermost (100) guest frame 5.
  ASSERT_EQ(grcore_compiled_rebuild(w.ctx, nullptr, SIZE_MAX, &rebuilt), GRCORE_OK);
  EXPECT_EQ(rebuilt, 2u);
  GRCORE_ActivationInfo info;
  ASSERT_EQ(grcore_activation_info(w.stack, b, &info), GRCORE_OK);
  EXPECT_EQ(info.frame_base, 0u);
  ASSERT_EQ(grcore_activation_info(w.stack, a, &info), GRCORE_OK);
  EXPECT_EQ(info.frame_base, outer[0]) << "the run under the native is kept";
  grcore_unwind_all(w.stack, nullptr);
  (void)n;
}

TEST(ReservationExtend, EachCallExtendsByTheCalleesMaximumAndGivesItBack) {
  RW w;
  GRCORE_DeoptReservation * res = nullptr;
  ASSERT_EQ(grcore_deopt_reserve(w.ctx, 2, &res), GRCORE_OK);
  EXPECT_EQ(grcore_deopt_reservation_capacity(res), 2u);
  for (size_t depth = 1; depth <= 50; depth++) {
    ASSERT_EQ(grcore_deopt_reservation_extend(w.ctx, res, 3), GRCORE_OK);
    EXPECT_EQ(grcore_deopt_reservation_capacity(res), 2 + 3 * depth);
  }
  for (size_t depth = 50; depth > 0; depth--) {
    grcore_deopt_reservation_retract(res, 3);
    EXPECT_EQ(grcore_deopt_reservation_capacity(res), 2 + 3 * (depth - 1));
  }
  // Zero is accepted; retracting more than was added stops at zero; NULL is
  // ignored.
  EXPECT_EQ(grcore_deopt_reservation_extend(w.ctx, res, 0), GRCORE_OK);
  grcore_deopt_reservation_retract(res, 1000);
  EXPECT_EQ(grcore_deopt_reservation_capacity(res), 0u);
  grcore_deopt_reservation_retract(nullptr, 1);
  EXPECT_EQ(grcore_deopt_reservation_extend(nullptr, res, 1), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_deopt_reservation_extend(w.ctx, nullptr, 1), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_deopt_reservation_extend(w.ctx, res, SIZE_MAX), GRCORE_ERR_INVALID);
  ASSERT_EQ(grcore_deopt_release(w.ctx, res), GRCORE_OK);
}

TEST(ReservationExtend, ARefusedAllocationLeavesTheReservationAsItWas) {
  TrackingAllocator tracking;
  RW w(GRCORE_REPR_BITS, false, false, &tracking.vtable);
  GRCORE_DeoptReservation * res = nullptr;
  ASSERT_EQ(grcore_deopt_reserve(w.ctx, 4, &res), GRCORE_OK);
  // The one allocation the extension makes is refused: it reports an error and
  // the capacity is what it was.
  tracking.calls = 0;
  tracking.fail_at = 1;
  GRCORE_Result r = grcore_deopt_reservation_extend(w.ctx, res, 100);
  tracking.fail_at = 0;
  EXPECT_TRUE(r == GRCORE_ERR_OOM || r == GRCORE_ERR_LIMIT) << r;
  EXPECT_EQ(grcore_deopt_reservation_capacity(res), 4u);
  ASSERT_EQ(grcore_deopt_reservation_extend(w.ctx, res, 100), GRCORE_OK);
  EXPECT_EQ(grcore_deopt_reservation_capacity(res), 104u);
  ASSERT_EQ(grcore_deopt_release(w.ctx, res), GRCORE_OK);
}

TEST(ReservationExtend, AnExtendedCellIsAGoodRootOnlyWhileARebuildIsFillingIt) {
  // The array may move when it is extended; the collector reads only the cells a
  // rebuild is filling, and none between rebuilds.
  RW w(GRCORE_REPR_I32, true);
  GRCORE_DeoptReservation * res = nullptr;
  ASSERT_EQ(grcore_deopt_reserve(w.ctx, 1, &res), GRCORE_OK);
  ASSERT_EQ(grcore_deopt_reservation_extend(w.ctx, res, 2000), GRCORE_OK);
  size_t seen = 0;
  GRCORE_RootVisitor v = {};
  v.user = &seen;
  v.slot = [](void * user, uint64_t *) { ++*static_cast<size_t *>(user); };
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  EXPECT_EQ(seen, 0u);
  ASSERT_EQ(grcore_deopt_release(w.ctx, res), GRCORE_OK);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

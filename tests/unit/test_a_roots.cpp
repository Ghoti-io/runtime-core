/**
 * @file
 *
 * A's root source (AD-17, AD-18): precise VALUE slots of every frame, the
 * engines' hooks, and conservative ranges for the C segments that activation
 * records name.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../../src/a/guest_internal.h"

#include <thread>
#include <vector>

namespace {

struct Seen {
  std::vector<uint64_t *> slots;
  std::vector<uint64_t> values;
  std::vector<GRCORE_ConservativeRange> ranges;
  int slot_calls = 0;
  uint64_t rewrite_add = 0;
};

GRCORE_RootVisitor visitor_for(Seen * seen) {
  GRCORE_RootVisitor v;
  v.user = seen;
  v.slot = [](void * user, uint64_t * slot) {
    auto * s = static_cast<Seen *>(user);
    s->slot_calls++;
    s->slots.push_back(slot);
    s->values.push_back(*slot);
    *slot += s->rewrite_add;
  };
  v.range = [](void * user, const GRCORE_ConservativeRange * r) {
    static_cast<Seen *>(user)->ranges.push_back(*r);
  };
  return v;
}

/* Frames of two engines: alpha has RAW slots at even indices and VALUE at odd
 * ones; beta's slots are all VALUE. Slot i of a frame is v * 10 + i. */
GRCORE_FrameRef push_numbered(
    GRCORE_Stack * s, GRCORE_EngineId e, size_t slots, uint64_t v) {
  GRCORE_FrameRef f = {0};
  EXPECT_EQ(grcore_stack_push(s, e, slots, &f), GRCORE_OK);
  for (size_t i = 0; i < slots; i++) {
    EXPECT_EQ(grcore_stack_slot_set(s, f, i, v * 10 + i), GRCORE_OK);
  }
  return f;
}

} // namespace

TEST(ARoots, TheFirstEngineRegistrationAddsOneRootSourceAndNoMoreAreAdded) {
  RunWorld w;
  EXPECT_EQ(grcore_context_root_source_count(w.ctx), 0u);
  GRCORE_EngineId a, b;
  ASSERT_EQ(grcore_engine_register(w.ctx, &kAlpha, &a), GRCORE_OK);
  EXPECT_EQ(grcore_context_root_source_count(w.ctx), 1u);
  ASSERT_EQ(grcore_engine_register(w.ctx, &kBeta, &b), GRCORE_OK);
  EXPECT_EQ(grcore_context_root_source_count(w.ctx), 1u);
  // A refused registration adds none.
  RunWorld other;
  GRCORE_EngineId id;
  EXPECT_EQ(grcore_engine_register(other.ctx, nullptr, &id), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_root_source_count(other.ctx), 0u);
}

TEST(ARoots, EveryValueSlotIsVisitedOnceInnermostFrameFirstAndRawSlotsAreNot) {
  StackWorld w;
  // Outermost to innermost: alpha (3 slots), beta (2), alpha (2).
  GRCORE_FrameRef f1 = push_numbered(w.stack, w.alpha, 3, 1);
  GRCORE_FrameRef f2 = push_numbered(w.stack, w.beta, 2, 2);
  GRCORE_FrameRef f3 = push_numbered(w.stack, w.alpha, 2, 3);
  Seen seen;
  GRCORE_RootVisitor v = visitor_for(&seen);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  // f3 (alpha): slot 1 is VALUE. f2 (beta): both. f1 (alpha): slot 1.
  EXPECT_EQ(seen.values, (std::vector<uint64_t>{31, 20, 21, 11}));
  ASSERT_EQ(seen.slots.size(), 4u);
  EXPECT_EQ(seen.slots[0], grcore_stack_slots(w.stack, f3) + 1);
  EXPECT_EQ(seen.slots[1], grcore_stack_slots(w.stack, f2) + 0);
  EXPECT_EQ(seen.slots[2], grcore_stack_slots(w.stack, f2) + 1);
  EXPECT_EQ(seen.slots[3], grcore_stack_slots(w.stack, f1) + 1);
  EXPECT_TRUE(seen.ranges.empty()); // no activation recorded a segment
}

TEST(ARoots, AVisitorMayRewriteAVALUESlotAndLaterReadsSeeTheNewValue) {
  StackWorld w;
  GRCORE_FrameRef f = push_numbered(w.stack, w.alpha, 3, 4);
  Seen seen;
  seen.rewrite_add = 1000;
  GRCORE_RootVisitor v = visitor_for(&seen);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  uint64_t raw = 0, value = 0;
  ASSERT_EQ(grcore_stack_slot_get(w.stack, f, 0, &raw), GRCORE_OK);
  ASSERT_EQ(grcore_stack_slot_get(w.stack, f, 1, &value), GRCORE_OK);
  EXPECT_EQ(raw, 40u);      // RAW: untouched
  EXPECT_EQ(value, 1041u);  // VALUE: rewritten
  Seen again;
  GRCORE_RootVisitor v2 = visitor_for(&again);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v2), GRCORE_OK);
  EXPECT_EQ(again.values, std::vector<uint64_t>{1041});
  // The abstract frame sees the rewritten value too.
  ASSERT_EQ(grcore_stack_slot_get(w.stack, f, 1, &value), GRCORE_OK);
  EXPECT_EQ(value, 1041u);
}

TEST(ARoots, AnEngineWithNoSlotKindsAndNoHooksContributesNothing) {
  RunWorld w;
  GRCORE_EngineId g;
  ASSERT_EQ(grcore_engine_register(w.ctx, &kGamma, &g), GRCORE_OK);
  GRCORE_Stack * s = grcore_context_stack(w.ctx);
  push_numbered(s, g, 4, 1);
  Seen seen;
  GRCORE_RootVisitor v = visitor_for(&seen);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  EXPECT_TRUE(seen.values.empty());
}

TEST(ARoots, TheEnginesRootsHookIsCalledPerFrameAndReportsWhatItAdds) {
  HookWorld w;
  // delta: slot 0 VALUE, slot 1 RAW; its hook reports slot 1 of function 99.
  for (uint64_t i = 1; i <= 3; i++) {
    GRCORE_FrameRef f = push_numbered(w.stack, w.delta, 2, i);
    grcore_stack_set_identity(w.stack, f, GRCORE_PollIdentity{i == 2 ? 99 : i, 0});
  }
  Seen seen;
  GRCORE_RootVisitor v = visitor_for(&seen);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  // Innermost frame first: slot 0 of frame 3, then the hook (nothing, its
  // function is 3); frame 2's slot 0 and the hook's slot 1; frame 1's slot 0.
  EXPECT_EQ(seen.values, (std::vector<uint64_t>{30, 20, 21, 10}));
  EXPECT_EQ(w.log.root_functions, (std::vector<uint64_t>{3, 99, 1}));
  // Innermost first, and the hook's frame carries no location.
  EXPECT_EQ(w.log.root_depths, (std::vector<size_t>{0, 1, 2}));
  for (const GRCORE_Location & l : w.log.root_locations) {
    EXPECT_EQ(l.file, nullptr);
    EXPECT_EQ(l.line, 0);
  }
  EXPECT_EQ(w.log.contexts[0], w.ctx);
}

TEST(ARoots, ACSegmentBecomesOneRangeWithTheEnginesDecoderAndEngineZeroGetsIdentity) {
  StackWorld w;
  GRCORE_CSegment beta_seg = {0x7000, 0x7800}, plain_seg = {0x9000, 0x9100};
  GRCORE_ActivationRef host, with_beta, plain, no_segment;
  ASSERT_EQ(grcore_activation_enter(
                w.stack, GRCORE_ACTIVATION_HOST, 0, false, &plain_seg, &host),
      GRCORE_OK);
  ASSERT_EQ(grcore_activation_enter(w.stack, GRCORE_ACTIVATION_NATIVE, w.beta,
                false, &beta_seg, &with_beta),
      GRCORE_OK);
  ASSERT_EQ(grcore_activation_enter(
                w.stack, GRCORE_ACTIVATION_INTERPRETER, w.alpha, false, nullptr,
                &no_segment),
      GRCORE_OK);
  (void)plain;
  Seen seen;
  GRCORE_RootVisitor v = visitor_for(&seen);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  // Only the two with segments, innermost first.
  ASSERT_EQ(seen.ranges.size(), 2u);
  EXPECT_EQ(seen.ranges[0].lo, 0x7000u);
  EXPECT_EQ(seen.ranges[0].hi, 0x7800u);
  EXPECT_EQ(seen.ranges[0].mask, 0xFFFF0u); // beta's decoder
  EXPECT_EQ(seen.ranges[0].shift, 4u);
  EXPECT_EQ(seen.ranges[0].base, 0x1000u);
  EXPECT_EQ(seen.ranges[1].lo, 0x9000u);
  EXPECT_EQ(seen.ranges[1].hi, 0x9100u);
  EXPECT_EQ(seen.ranges[1].mask, UINT64_MAX); // engine zero: identity
  EXPECT_EQ(seen.ranges[1].shift, 0u);
  EXPECT_EQ(seen.ranges[1].base, 0u);
  // The range's decoder reads a word the way the engine does.
  GRCORE_ConservativeDecoder d = {seen.ranges[0].mask, seen.ranges[0].shift,
      seen.ranges[0].base};
  uint64_t address = 0;
  ASSERT_TRUE(grcore_decoder_decode(&d, 0x00ABC5, &address));
  EXPECT_EQ(address, 0xABCu + 0x1000u);
  // A segment is gone from the roots once its activation is left.
  grcore_activation_leave(w.stack, no_segment);
  grcore_activation_leave(w.stack, with_beta);
  grcore_activation_leave(w.stack, host);
  Seen after;
  GRCORE_RootVisitor v2 = visitor_for(&after);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v2), GRCORE_OK);
  EXPECT_TRUE(after.ranges.empty());
}

TEST(ARoots, PreciseSlotsComeBeforeRangesAndFramesAreAllReportedWithSegmentsPresent) {
  StackWorld w;
  GRCORE_CSegment seg = {0x100, 0x200};
  GRCORE_ActivationRef a;
  push_numbered(w.stack, w.beta, 2, 1);
  ASSERT_EQ(grcore_activation_enter(
                w.stack, GRCORE_ACTIVATION_NATIVE, 0, false, &seg, &a),
      GRCORE_OK);
  push_numbered(w.stack, w.beta, 2, 2);
  std::vector<std::string> order;
  GRCORE_RootVisitor v = {};
  v.user = &order;
  v.slot = [](void * u, uint64_t *) {
    static_cast<std::vector<std::string> *>(u)->push_back("slot");
  };
  v.range = [](void * u, const GRCORE_ConservativeRange *) {
    static_cast<std::vector<std::string> *>(u)->push_back("range");
  };
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  EXPECT_EQ(order, (std::vector<std::string>{"slot", "slot", "slot", "slot", "range"}));
}

TEST(ARoots, ASourceThatHasNoFramesAndNoRecordsReportsNothing) {
  StackWorld w;
  Seen seen;
  GRCORE_RootVisitor v = visitor_for(&seen);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  EXPECT_TRUE(seen.values.empty());
  EXPECT_TRUE(seen.ranges.empty());
  // A frame with no slots is fine.
  GRCORE_FrameRef f;
  ASSERT_EQ(grcore_stack_push(w.stack, w.beta, 0, &f), GRCORE_OK);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  EXPECT_TRUE(seen.values.empty());
}

TEST(ARoots, EnumeratingFromANonOwnerIsInvalidAndVisitsNothing) {
  StackWorld w;
  push_numbered(w.stack, w.beta, 2, 1);
  Seen seen;
  GRCORE_RootVisitor v = visitor_for(&seen);
  GRCORE_Result r = GRCORE_OK;
  std::thread([&] { r = grcore_context_enumerate_roots(w.ctx, &v); }).join();
  EXPECT_EQ(r, GRCORE_ERR_INVALID);
  EXPECT_EQ(seen.slot_calls, 0);
}

TEST(ARoots, ASourceFromAnotherLibraryIsEnumeratedAfterAs) {
  StackWorld w;
  push_numbered(w.stack, w.beta, 1, 1);
  static const GRCORE_RootSource extra = {"extra",
      [](GRCORE_Context *, void * value, const GRCORE_RootVisitor * visitor) {
        visitor->slot(visitor->user, static_cast<uint64_t *>(value));
      }};
  uint64_t held = 777;
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &extra, &held), GRCORE_OK);
  EXPECT_EQ(grcore_context_root_source_count(w.ctx), 2u);
  Seen seen;
  GRCORE_RootVisitor v = visitor_for(&seen);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  EXPECT_EQ(seen.values, (std::vector<uint64_t>{10, 777}));
}

TEST(ARoots, EnumerationFollowsTheStackThroughGrowthAndTheAlwaysMoveMode) {
  StackWorld w;
  grcore_stack_set_always_move(w.stack, true);
  for (uint64_t i = 1; i <= 40; i++) {
    push_numbered(w.stack, w.beta, 3, i);
  }
  ASSERT_GT(grcore_stack_move_count(w.stack), 0u);
  Seen seen;
  GRCORE_RootVisitor v = visitor_for(&seen);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  ASSERT_EQ(seen.values.size(), 120u);
  EXPECT_EQ(seen.values[0], 400u);  // the innermost frame, slot 0
  EXPECT_EQ(seen.values[119], 12u); // the outermost frame, slot 2
}

TEST(ARoots, ARootSourceIsReachableWhilePausedAndThroughOnlyBTypes) {
  StackWorld w(3);
  FrameGuest g;
  g.engine = w.beta;
  g.n = 20;
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, frame_guest_entry, &g, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  Seen seen;
  GRCORE_RootVisitor v = visitor_for(&seen);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  // Beta's slots are all VALUE; the guest's frames have two slots each.
  EXPECT_EQ(seen.values.size(), 2u * grcore_stack_frame_count(w.stack));
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

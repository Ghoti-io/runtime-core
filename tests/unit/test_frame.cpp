/**
 * @file
 *
 * The abstract frame, the frame walk and the scope interface.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstring>
#include <thread>
#include <vector>

namespace {

/* Pushes three frames of two engines (alpha 3 slots, beta 2, alpha 1), the
 * outer two with call-site identities, and returns the refs. The innermost
 * polls under a fuel budget that has run out, so the run pauses there. */
struct ThreeFrames {
  GRCORE_FrameRef outer{0};
  GRCORE_FrameRef middle{0};
  GRCORE_FrameRef inner{0};
};

GRCORE_Step three_frames_entry(GRCORE_Context * c, void * state) {
  auto * t = static_cast<ThreeFrames *>(state);
  GRCORE_Stack * s = grcore_context_stack(c);
  if (grcore_stack_frame_count(s) == 0) {
    grcore_stack_push(s, 1, 3, &t->outer);
    grcore_stack_slot_set(s, t->outer, 0, 10);
    grcore_stack_slot_set(s, t->outer, 1, 11);
    grcore_stack_slot_set(s, t->outer, 2, 12);
    grcore_stack_set_identity(s, t->outer, GRCORE_PollIdentity{1, 5});
    grcore_stack_push(s, 2, 2, &t->middle);
    grcore_stack_slot_set(s, t->middle, 0, 0xABCD0);
    grcore_stack_slot_set(s, t->middle, 1, 0x20);
    grcore_stack_set_identity(s, t->middle, GRCORE_PollIdentity{2, 9});
    grcore_stack_push(s, 1, 1, &t->inner);
    grcore_stack_slot_set(s, t->inner, 0, 99);
  }
  grcore_context_charge_fuel(c, 1);
  GRCORE_Verdict v = grcore_stack_poll(c, 3, 4);
  return v == GRCORE_VERDICT_PAUSE ? GRCORE_STEP_PAUSED : GRCORE_STEP_FINISHED;
}

struct Paused : StackWorld {
  ThreeFrames frames;
  Paused() : StackWorld(0) {
    GRCORE_Outcome outcome;
    EXPECT_EQ(grcore_run(ctx, three_frames_entry, &frames, &outcome), GRCORE_OK);
    EXPECT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  }
};

std::vector<GRCORE_AbstractFrame> walk_all(const GRCORE_Context * c) {
  std::vector<GRCORE_AbstractFrame> out;
  GRCORE_FrameWalk walk;
  EXPECT_EQ(grcore_frame_walk_begin(c, &walk), GRCORE_OK);
  GRCORE_AbstractFrame f;
  while (grcore_frame_walk_next(&walk, &f)) {
    out.push_back(f);
  }
  return out;
}

} // namespace

TEST(FrameWalk, ReadsEveryFrameInnermostFirstWithEachEnginesDescriptor) {
  Paused w;
  std::vector<GRCORE_AbstractFrame> frames = walk_all(w.ctx);
  ASSERT_EQ(frames.size(), 3u);

  EXPECT_EQ(frames[0].depth, 0u);
  EXPECT_EQ(frames[0].engine, w.alpha);
  EXPECT_EQ(frames[0].descriptor, &kAlpha);
  EXPECT_EQ(frames[0].identity.function, 3u);
  EXPECT_EQ(frames[0].identity.offset, 4u);
  EXPECT_STREQ(frames[0].location.file, kAlphaFile);
  EXPECT_EQ(frames[0].location.line, 3004);
  EXPECT_EQ(frames[0].slot_count, 1u);
  EXPECT_EQ(frames[0].frame.offset, w.frames.inner.offset);
  EXPECT_EQ(frames[0].context, w.ctx);

  EXPECT_EQ(frames[1].depth, 1u);
  EXPECT_EQ(frames[1].engine, w.beta);
  EXPECT_EQ(frames[1].descriptor, &kBeta);
  EXPECT_EQ(frames[1].identity.function, 2u);
  EXPECT_EQ(frames[1].identity.offset, 9u);
  EXPECT_STREQ(frames[1].location.file, kBetaFile);
  EXPECT_EQ(frames[1].location.line, 9);
  EXPECT_EQ(frames[1].slot_count, 2u);

  EXPECT_EQ(frames[2].depth, 2u);
  EXPECT_EQ(frames[2].descriptor, &kAlpha);
  EXPECT_EQ(frames[2].identity.function, 1u);
  EXPECT_EQ(frames[2].identity.offset, 5u);
  EXPECT_EQ(frames[2].location.line, 1005);
  EXPECT_EQ(frames[2].slot_count, 3u);
}

TEST(FrameWalk, TheInnermostFrameLocationIsThePauseLocation) {
  Paused w;
  std::vector<GRCORE_AbstractFrame> frames = walk_all(w.ctx);
  ASSERT_FALSE(frames.empty());
  GRCORE_Location pause = grcore_context_pause_location(w.ctx);
  EXPECT_STREQ(frames[0].location.file, pause.file);
  EXPECT_EQ(frames[0].location.line, pause.line);
}

TEST(FrameWalk, SlotsComeWithTheKindEachEngineDeclaredAndTheirValues) {
  Paused w;
  std::vector<GRCORE_AbstractFrame> frames = walk_all(w.ctx);
  ASSERT_EQ(frames.size(), 3u);
  GRCORE_SlotKind kind;
  uint64_t value;
  // alpha: even slots RAW, odd VALUE.
  ASSERT_EQ(grcore_frame_slot(&frames[2], 0, &kind, &value), GRCORE_OK);
  EXPECT_EQ(kind, GRCORE_SLOT_RAW);
  EXPECT_EQ(value, 10u);
  ASSERT_EQ(grcore_frame_slot(&frames[2], 1, &kind, &value), GRCORE_OK);
  EXPECT_EQ(kind, GRCORE_SLOT_VALUE);
  EXPECT_EQ(value, 11u);
  ASSERT_EQ(grcore_frame_slot(&frames[2], 2, &kind, &value), GRCORE_OK);
  EXPECT_EQ(kind, GRCORE_SLOT_RAW);
  EXPECT_EQ(value, 12u);
  // beta: all VALUE.
  ASSERT_EQ(grcore_frame_slot(&frames[1], 0, &kind, &value), GRCORE_OK);
  EXPECT_EQ(kind, GRCORE_SLOT_VALUE);
  EXPECT_EQ(value, 0xABCD0u);
  ASSERT_EQ(grcore_frame_slot(&frames[0], 0, &kind, &value), GRCORE_OK);
  EXPECT_EQ(kind, GRCORE_SLOT_RAW);
  EXPECT_EQ(value, 99u);
  // Either output may be omitted.
  EXPECT_EQ(grcore_frame_slot(&frames[0], 0, nullptr, &value), GRCORE_OK);
  EXPECT_EQ(grcore_frame_slot(&frames[0], 0, &kind, nullptr), GRCORE_OK);
}

TEST(FrameWalk, SlotAccessOutOfRangeOrWithBadArgumentsIsRefusedAndWritesNothing) {
  Paused w;
  std::vector<GRCORE_AbstractFrame> frames = walk_all(w.ctx);
  GRCORE_SlotKind kind = GRCORE_SLOT_VALUE;
  uint64_t value = 5;
  EXPECT_EQ(grcore_frame_slot(&frames[0], 1, &kind, &value), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_frame_slot(&frames[0], SIZE_MAX, &kind, &value),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_frame_slot(nullptr, 0, &kind, &value), GRCORE_ERR_INVALID);
  EXPECT_EQ(kind, GRCORE_SLOT_VALUE);
  EXPECT_EQ(value, 5u);
  GRCORE_AbstractFrame forged = frames[0];
  forged.frame.offset += 8; // names no frame
  EXPECT_EQ(grcore_frame_slot(&forged, 0, &kind, &value), GRCORE_ERR_INVALID);
  GRCORE_AbstractFrame blank = {};
  EXPECT_EQ(grcore_frame_slot(&blank, 0, &kind, &value), GRCORE_ERR_INVALID);
}

TEST(FrameWalk, TheWalkEndsAndStaysEnded) {
  Paused w;
  GRCORE_FrameWalk walk;
  ASSERT_EQ(grcore_frame_walk_begin(w.ctx, &walk), GRCORE_OK);
  GRCORE_AbstractFrame f;
  EXPECT_TRUE(grcore_frame_walk_next(&walk, &f));
  EXPECT_TRUE(grcore_frame_walk_next(&walk, &f));
  EXPECT_TRUE(grcore_frame_walk_next(&walk, &f));
  GRCORE_AbstractFrame last = f;
  EXPECT_FALSE(grcore_frame_walk_next(&walk, &f));
  EXPECT_FALSE(grcore_frame_walk_next(&walk, &f));
  EXPECT_EQ(f.depth, last.depth); // untouched when there is no frame
  EXPECT_FALSE(grcore_frame_walk_next(nullptr, &f));
  EXPECT_FALSE(grcore_frame_walk_next(&walk, nullptr));
}

TEST(FrameWalk, AContextWithNoFramesOrNoEngineWalksAsEmpty) {
  {
    RunWorld w(0);
    GRCORE_Outcome outcome;
    Fn fn{[&](GRCORE_Context * c) {
      grcore_context_charge_fuel(c, 1);
      return GRCORE_POLL(c) == GRCORE_VERDICT_PAUSE ? GRCORE_STEP_PAUSED
                                                   : GRCORE_STEP_FINISHED;
    }};
    ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
    ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
    EXPECT_TRUE(walk_all(w.ctx).empty()); // no engine, no stack
  }
  {
    StackWorld w(0);
    GRCORE_Outcome outcome;
    Fn fn{[&](GRCORE_Context * c) {
      grcore_context_charge_fuel(c, 1);
      return grcore_stack_poll(c, 1, 1) == GRCORE_VERDICT_PAUSE
          ? GRCORE_STEP_PAUSED
          : GRCORE_STEP_FINISHED;
    }};
    ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
    ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
    EXPECT_TRUE(walk_all(w.ctx).empty()); // an engine, no frames
  }
}

TEST(FrameWalk, IsRefusedUnlessTheContextIsReadableAndHeld) {
  StackWorld w(0);
  GRCORE_FrameWalk walk;
  walk.depth = 77;
  // Parked, before any run.
  EXPECT_EQ(grcore_frame_walk_begin(w.ctx, &walk), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_frame_walk_begin(nullptr, &walk), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_frame_walk_begin(w.ctx, nullptr), GRCORE_ERR_INVALID);
  // Running.
  GRCORE_Result during = GRCORE_OK;
  GRCORE_Outcome outcome;
  ThreeFrames frames;
  Fn fn{[&](GRCORE_Context * c) {
    during = grcore_frame_walk_begin(c, &walk);
    return three_frames_entry(c, &frames);
  }};
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_EQ(during, GRCORE_ERR_INVALID);
  EXPECT_EQ(walk.depth, 77u); // never written on a refusal
  ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  // Paused, but on a thread that does not hold it.
  GRCORE_Result other = GRCORE_OK;
  std::thread([&] { other = grcore_frame_walk_begin(w.ctx, &walk); }).join();
  EXPECT_EQ(other, GRCORE_ERR_INVALID);
  // Paused and held: allowed.
  EXPECT_EQ(grcore_frame_walk_begin(w.ctx, &walk), GRCORE_OK);
}

TEST(FrameWalk, IsAllowedFromAHandlerAtPollAndSeesTheSameFrames) {
  StackWorld w(0);
  std::vector<std::string> seen;
  GRCORE_ContextState state_in_handler = GRCORE_CONTEXT_PARKED;
  Probe probe("observer");
  probe.extra = [&](GRCORE_Context * c, GRCORE_PollCall *) {
    state_in_handler = grcore_context_state(c);
    for (const auto & f : walk_all(c)) {
      seen.push_back(f.descriptor->name);
    }
  };
  ASSERT_EQ(grcore_context_register(w.ctx, &kObserveKey, &probe), GRCORE_OK);
  ThreeFrames frames;
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, three_frames_entry, &frames, &outcome), GRCORE_OK);
  EXPECT_EQ(state_in_handler, GRCORE_CONTEXT_AT_POLL);
  ASSERT_EQ(seen.size(), 3u);
  EXPECT_EQ(seen[0], "alpha");
  EXPECT_EQ(seen[1], "beta");
  EXPECT_EQ(seen[2], "alpha");
}

TEST(FrameWalk, AFrameKeptPastTheResumeCannotBeReadThrough) {
  Paused w;
  std::vector<GRCORE_AbstractFrame> frames = walk_all(w.ctx);
  ASSERT_EQ(frames.size(), 3u);
  ASSERT_EQ(grcore_context_set_fuel(w.ctx, GRCORE_UNLIMITED), GRCORE_OK);
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_resume(w.ctx, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  // Parked now: not readable.
  uint64_t value = 3;
  EXPECT_EQ(grcore_frame_slot(&frames[0], 0, nullptr, &value), GRCORE_ERR_INVALID);
  EXPECT_EQ(value, 3u);
  EXPECT_EQ(grcore_frame_scope_count(&frames[2]), 0u);
  GRCORE_ScopeInfo scope;
  EXPECT_EQ(grcore_frame_scope(&frames[2], 0, &scope), GRCORE_ERR_INVALID);
  GRCORE_Variable v;
  EXPECT_EQ(grcore_frame_variable(&frames[2], 0, 0, &v), GRCORE_ERR_INVALID);
  char buf[8];
  size_t len;
  EXPECT_EQ(grcore_frame_inspect(&frames[0], 0, buf, 8, &len), GRCORE_ERR_INVALID);
  GRCORE_FrameWalk walk = {w.ctx, w.frames.inner, 0};
  GRCORE_AbstractFrame f;
  EXPECT_FALSE(grcore_frame_walk_next(&walk, &f));
}

TEST(FrameScopes, AnEngineWithScopesReportsEachScopesKindNameAndVariables) {
  Paused w;
  std::vector<GRCORE_AbstractFrame> frames = walk_all(w.ctx);
  const GRCORE_AbstractFrame & outer = frames[2]; // alpha, three slots
  ASSERT_EQ(grcore_frame_scope_count(&outer), 2u);
  GRCORE_ScopeInfo scope;
  ASSERT_EQ(grcore_frame_scope(&outer, 0, &scope), GRCORE_OK);
  EXPECT_EQ(scope.kind, GRCORE_SCOPE_LOCAL);
  EXPECT_STREQ(scope.name, "locals");
  EXPECT_EQ(scope.variable_count, 3u);
  ASSERT_EQ(grcore_frame_scope(&outer, 1, &scope), GRCORE_OK);
  EXPECT_EQ(scope.kind, GRCORE_SCOPE_CLOSURE);
  EXPECT_STREQ(scope.name, "captured");
  EXPECT_EQ(scope.variable_count, 1u);

  GRCORE_Variable v;
  for (size_t i = 0; i < 3; i++) {
    ASSERT_EQ(grcore_frame_variable(&outer, 0, i, &v), GRCORE_OK);
    EXPECT_STREQ(v.name, kAlphaVarNames[i]);
    EXPECT_EQ(v.value, 10u + i);
    EXPECT_EQ(v.kind, i % 2 == 0 ? GRCORE_SLOT_RAW : GRCORE_SLOT_VALUE);
  }
  ASSERT_EQ(grcore_frame_variable(&outer, 1, 0, &v), GRCORE_OK);
  EXPECT_STREQ(v.name, "captured0");
  EXPECT_EQ(v.value, 10u);
  EXPECT_EQ(v.kind, GRCORE_SLOT_VALUE);
}

TEST(FrameScopes, AVariableReadFollowsTheSlotsCurrentValue) {
  Paused w;
  std::vector<GRCORE_AbstractFrame> frames = walk_all(w.ctx);
  ASSERT_EQ(grcore_stack_slot_set(w.stack, w.frames.outer, 1, 4242), GRCORE_OK);
  GRCORE_Variable v;
  ASSERT_EQ(grcore_frame_variable(&frames[2], 0, 1, &v), GRCORE_OK);
  EXPECT_EQ(v.value, 4242u);
}

TEST(FrameScopes, OutOfRangeScopesAndVariablesAreRefusedAndWriteNothing) {
  Paused w;
  std::vector<GRCORE_AbstractFrame> frames = walk_all(w.ctx);
  const GRCORE_AbstractFrame & outer = frames[2];
  GRCORE_ScopeInfo scope = {GRCORE_SCOPE_GLOBAL, "keep", 77};
  GRCORE_Variable v = {"keep", GRCORE_SLOT_VALUE, 77};
  EXPECT_EQ(grcore_frame_scope(&outer, 2, &scope), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_frame_scope(&outer, SIZE_MAX, &scope), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_frame_scope(&outer, 0, nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_frame_scope(nullptr, 0, &scope), GRCORE_ERR_INVALID);
  EXPECT_EQ(scope.variable_count, 77u);
  EXPECT_EQ(grcore_frame_variable(&outer, 0, 3, &v), GRCORE_ERR_INVALID); // past the end
  EXPECT_EQ(grcore_frame_variable(&outer, 1, 1, &v), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_frame_variable(&outer, 2, 0, &v), GRCORE_ERR_INVALID); // no such scope
  EXPECT_EQ(grcore_frame_variable(&outer, 0, 0, nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_frame_variable(nullptr, 0, 0, &v), GRCORE_ERR_INVALID);
  EXPECT_STREQ(v.name, "keep");
  EXPECT_EQ(v.value, 77u);
}

TEST(FrameScopes, AnEngineRegisteredWithAnEmptyInterfaceHasNoScopes) {
  Paused w;
  std::vector<GRCORE_AbstractFrame> frames = walk_all(w.ctx);
  const GRCORE_AbstractFrame & beta = frames[1];
  EXPECT_EQ(grcore_frame_scope_count(&beta), 0u);
  GRCORE_ScopeInfo scope;
  GRCORE_Variable v;
  EXPECT_EQ(grcore_frame_scope(&beta, 0, &scope), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_frame_variable(&beta, 0, 0, &v), GRCORE_ERR_INVALID);
}

TEST(FrameScopes, AnEnginesRefusalComesBackAndWritesNothing) {
  static const GRCORE_EngineDescriptor liar = {"liar", nullptr, nullptr, nullptr,
      GRCORE_ScopeInterface{
          [](const GRCORE_AbstractFrame *) -> size_t { return 1; },
          [](const GRCORE_AbstractFrame *, size_t,
              GRCORE_ScopeInfo * out) -> GRCORE_Result {
            *out = GRCORE_ScopeInfo{GRCORE_SCOPE_LOCAL, "l", 5};
            return GRCORE_OK;
          },
          [](const GRCORE_AbstractFrame *, size_t, size_t,
              GRCORE_Variable *) -> GRCORE_Result { return GRCORE_ERR_INVALID; }},
      GRCORE_ConservativeDecoder{0, 0, 0}};
  RunWorld w(0);
  GRCORE_EngineId e;
  ASSERT_EQ(grcore_engine_register(w.ctx, &liar, &e), GRCORE_OK);
  Fn fn{[&](GRCORE_Context * c) {
    GRCORE_FrameRef f;
    grcore_stack_push(grcore_context_stack(c), e, 1, &f);
    grcore_context_charge_fuel(c, 1);
    return grcore_stack_poll(c, 1, 1) == GRCORE_VERDICT_PAUSE
        ? GRCORE_STEP_PAUSED
        : GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  std::vector<GRCORE_AbstractFrame> frames = walk_all(w.ctx);
  ASSERT_EQ(frames.size(), 1u);
  GRCORE_Variable v = {"keep", GRCORE_SLOT_VALUE, 77};
  EXPECT_EQ(grcore_frame_variable(&frames[0], 0, 0, &v), GRCORE_ERR_INVALID);
  EXPECT_STREQ(v.name, "keep");
}

TEST(FrameInspect, RendersASlotThroughItsEnginesInspectorOrInHex) {
  RunWorld w(0);
  GRCORE_EngineId alpha, gamma;
  ASSERT_EQ(grcore_engine_register(w.ctx, &kAlpha, &alpha), GRCORE_OK);
  ASSERT_EQ(grcore_engine_register(w.ctx, &kGamma, &gamma), GRCORE_OK);
  Fn fn{[&](GRCORE_Context * c) {
    GRCORE_Stack * s = grcore_context_stack(c);
    GRCORE_FrameRef f;
    grcore_stack_push(s, gamma, 1, &f);
    grcore_stack_slot_set(s, f, 0, 255);
    grcore_stack_push(s, alpha, 2, &f);
    grcore_stack_slot_set(s, f, 1, 31);
    grcore_context_charge_fuel(c, 1);
    return grcore_stack_poll(c, 1, 1) == GRCORE_VERDICT_PAUSE
        ? GRCORE_STEP_PAUSED
        : GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  std::vector<GRCORE_AbstractFrame> frames = walk_all(w.ctx);
  ASSERT_EQ(frames.size(), 2u);
  char buf[32];
  size_t len = 0;
  ASSERT_EQ(grcore_frame_inspect(&frames[0], 1, buf, sizeof buf, &len), GRCORE_OK);
  EXPECT_STREQ(buf, "a#31");
  ASSERT_EQ(grcore_frame_inspect(&frames[1], 0, buf, sizeof buf, &len), GRCORE_OK);
  EXPECT_STREQ(buf, "0xff");
  EXPECT_EQ(grcore_frame_inspect(&frames[1], 1, buf, sizeof buf, &len),
      GRCORE_ERR_INVALID); // no such slot
}

TEST(FrameWalk, TheWalkOfADeepStackIsCompleteAndTheDepthsAreContiguous) {
  StackWorld w(40);
  FrameGuest g;
  g.engine = w.alpha;
  g.n = 100;
  ASSERT_EQ(grcore_stack_set_always_move(w.stack, true), GRCORE_OK);
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, frame_guest_entry, &g, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  std::vector<GRCORE_AbstractFrame> frames = walk_all(w.ctx);
  ASSERT_EQ(frames.size(), 41u);
  for (size_t i = 0; i < frames.size(); i++) {
    EXPECT_EQ(frames[i].depth, i);
    uint64_t v = 0;
    ASSERT_EQ(grcore_frame_slot(&frames[i], 0, nullptr, &v), GRCORE_OK);
    EXPECT_EQ(v, 41u - i); // slot 0 of the frame pushed k-th holds k
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

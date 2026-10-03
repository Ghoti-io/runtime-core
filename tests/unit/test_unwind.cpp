/**
 * @file
 *
 * The unwinder (AD-5, AD-18): frames popped innermost first with their hooks,
 * deeper activations left, deeper scopes closed, depths restored; and a
 * context that migrates while paused inside nested scopes and activations.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../../src/a/guest_internal.h"

#include <thread>
#include <vector>

namespace {

GRCORE_FrameRef push_value(GRCORE_Stack * s, GRCORE_EngineId e, uint64_t v,
    size_t slots = 2, uint64_t function = 1) {
  GRCORE_FrameRef f = {0};
  EXPECT_EQ(grcore_stack_push(s, e, slots, &f), GRCORE_OK);
  EXPECT_EQ(grcore_stack_slot_set(s, f, 0, v), GRCORE_OK);
  grcore_stack_set_identity(s, f, GRCORE_PollIdentity{function, 0});
  return f;
}

GRCORE_ActivationRef enter(GRCORE_Stack * s, GRCORE_ActivationKind kind,
    GRCORE_EngineId e = 0) {
  GRCORE_ActivationRef r = {0};
  EXPECT_EQ(grcore_activation_enter(s, kind, e, false, nullptr, &r), GRCORE_OK);
  return r;
}

GRCORE_BudgetScope open_scope(GRCORE_Stack * s, uint64_t fuel = 100) {
  GRCORE_BudgetScope scope = {0};
  EXPECT_EQ(grcore_budget_scope_open(s, fuel, GRCORE_SCOPE_POLICY_UNWIND, &scope),
      GRCORE_OK);
  return scope;
}

/* The matrix's stack: 2 frames below activation A0; a third frame, then A1
 * (native) and two more frames, then a scope opened inside A1. */
struct Built {
  GRCORE_ActivationRef a0{0}, a1{0};
  GRCORE_BudgetScope scope{0};
};

Built build(HookWorld & w) {
  Built b;
  push_value(w.stack, w.delta, 1);
  push_value(w.stack, w.delta, 2);
  b.a0 = enter(w.stack, GRCORE_ACTIVATION_INTERPRETER, w.delta);
  push_value(w.stack, w.delta, 3);
  b.a1 = enter(w.stack, GRCORE_ACTIVATION_NATIVE);
  push_value(w.stack, w.delta, 4);
  push_value(w.stack, w.delta, 5);
  b.scope = open_scope(w.stack);
  return b;
}

uint64_t native(const HookWorld & w) {
  return grcore_context_depth(w.ctx, GRCORE_DEPTH_NATIVE);
}
uint64_t guest(const HookWorld & w) {
  return grcore_context_depth(w.ctx, GRCORE_DEPTH_GUEST);
}

} // namespace

TEST(Unwind, ToAnActivationPopsFramesInnermostFirstLeavesDeeperOnesAndClosesScopes) {
  HookWorld w;
  Built b = build(w);
  ASSERT_EQ(grcore_stack_frame_count(w.stack), 5u);
  ASSERT_EQ(native(w), 1u);
  ASSERT_EQ(grcore_context_fuel_scope_depth(w.ctx), 1u);

  size_t popped = 99;
  ASSERT_EQ(grcore_unwind_to_activation(w.stack, b.a0, &popped), GRCORE_OK);
  EXPECT_EQ(popped, 3u);
  // Hooks ran innermost first, each with its frame still on the stack, and
  // the abstract frame each saw was the innermost then.
  EXPECT_EQ(w.log.unwound, (std::vector<uint64_t>{5, 4, 3}));
  EXPECT_EQ(w.log.unwound_depth, (std::vector<size_t>{0, 0, 0}));
  EXPECT_EQ(w.log.on_stack, (std::vector<bool>{true, true, true}));
  EXPECT_EQ(w.log.contexts[0], w.ctx);
  // The deeper activation and the scope inside it are gone; the target stays.
  EXPECT_EQ(grcore_activation_count(w.stack), 1u);
  GRCORE_ActivationRef top;
  ASSERT_EQ(grcore_activation_top(w.stack, &top), GRCORE_OK);
  EXPECT_EQ(top.id, b.a0.id);
  EXPECT_EQ(grcore_budget_scope_count(w.stack), 0u);
  EXPECT_EQ(grcore_context_fuel_scope_depth(w.ctx), 0u);
  // Depths are restored: native back to zero, guest to the two frames below.
  EXPECT_EQ(native(w), 0u);
  EXPECT_EQ(guest(w), 2u);
  EXPECT_EQ(grcore_stack_frame_count(w.stack), 2u);
  uint64_t v = 0;
  ASSERT_EQ(grcore_stack_slot_get(w.stack, grcore_stack_top(w.stack), 0, &v), GRCORE_OK);
  EXPECT_EQ(v, 2u);
  // The target can be left normally now.
  EXPECT_EQ(grcore_activation_leave(w.stack, b.a0), GRCORE_OK);
}

TEST(Unwind, ToTheDeeperActivationKeepsItAndClosesOnlyTheScopeInsideIt) {
  HookWorld w;
  Built b = build(w);
  size_t popped = 99;
  ASSERT_EQ(grcore_unwind_to_activation(w.stack, b.a1, &popped), GRCORE_OK);
  EXPECT_EQ(popped, 2u);
  EXPECT_EQ(w.log.unwound, (std::vector<uint64_t>{5, 4}));
  EXPECT_EQ(grcore_activation_count(w.stack), 2u);
  EXPECT_EQ(native(w), 1u); // the target's own native depth stays
  EXPECT_EQ(grcore_budget_scope_count(w.stack), 0u);
  EXPECT_EQ(grcore_context_fuel_scope_depth(w.ctx), 0u);
  EXPECT_EQ(grcore_stack_frame_count(w.stack), 3u);
  EXPECT_EQ(grcore_activation_leave(w.stack, b.a1), GRCORE_OK);
}

TEST(Unwind, AScopeOpenedBeforeTheTargetSurvivesTheUnwind) {
  HookWorld w;
  GRCORE_BudgetScope outer = open_scope(w.stack, 500);
  push_value(w.stack, w.delta, 1);
  GRCORE_ActivationRef a = enter(w.stack, GRCORE_ACTIVATION_INTERPRETER, w.delta);
  push_value(w.stack, w.delta, 2);
  GRCORE_BudgetScope inner = open_scope(w.stack, 50);
  (void)inner;
  push_value(w.stack, w.delta, 3);
  ASSERT_EQ(grcore_unwind_to_activation(w.stack, a, nullptr), GRCORE_OK);
  EXPECT_EQ(grcore_budget_scope_count(w.stack), 1u);
  EXPECT_EQ(grcore_context_fuel_scope_depth(w.ctx), 1u);
  EXPECT_EQ(grcore_context_fuel_scope_top(w.ctx), outer.id);
  EXPECT_EQ(grcore_stack_frame_count(w.stack), 1u);
  ASSERT_EQ(grcore_activation_leave(w.stack, a), GRCORE_OK);
  EXPECT_EQ(grcore_budget_scope_close(w.stack, outer), GRCORE_ERR_INVALID); // frame 1 remains
  ASSERT_EQ(grcore_stack_pop(w.stack), GRCORE_OK);
  EXPECT_EQ(grcore_budget_scope_close(w.stack, outer), GRCORE_OK);
}

TEST(Unwind, ABadReferenceChangesNothing) {
  HookWorld w;
  Built b = build(w);
  size_t popped = 77;
  EXPECT_EQ(grcore_unwind_to_activation(w.stack, GRCORE_ActivationRef{0}, &popped),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_unwind_to_activation(w.stack, GRCORE_ActivationRef{9999}, &popped),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_unwind_to_activation(nullptr, b.a0, &popped), GRCORE_ERR_INVALID);
  EXPECT_EQ(popped, 77u);
  EXPECT_EQ(grcore_stack_frame_count(w.stack), 5u);
  EXPECT_EQ(grcore_activation_count(w.stack), 2u);
  EXPECT_EQ(native(w), 1u);
  EXPECT_TRUE(w.log.unwound.empty());
  // A reference to an activation that was left is stale.
  ASSERT_EQ(grcore_unwind_to_activation(w.stack, b.a0, nullptr), GRCORE_OK);
  ASSERT_EQ(grcore_activation_leave(w.stack, b.a0), GRCORE_OK);
  EXPECT_EQ(grcore_unwind_to_activation(w.stack, b.a0, nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_unwind_to_activation(w.stack, b.a1, nullptr), GRCORE_ERR_INVALID);
}

TEST(Unwind, ANonOwnerCannotUnwind) {
  HookWorld w;
  Built b = build(w);
  GRCORE_Result r1 = GRCORE_OK, r2 = GRCORE_OK;
  std::thread([&] {
    r1 = grcore_unwind_to_activation(w.stack, b.a0, nullptr);
    r2 = grcore_unwind_all(w.stack, nullptr);
  }).join();
  EXPECT_EQ(r1, GRCORE_ERR_INVALID);
  EXPECT_EQ(r2, GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_stack_frame_count(w.stack), 5u);
}

TEST(Unwind, AllEmptiesTheStackAndEveryDepthWhateverTheMix) {
  HookWorld w;
  Built b = build(w);
  (void)b;
  // Frames of an engine with no hooks mix in.
  push_value(w.stack, w.alpha, 6);
  GRCORE_ActivationRef re = enter(w.stack, GRCORE_ACTIVATION_REENTRY);
  (void)re;
  push_value(w.stack, w.beta, 7);
  open_scope(w.stack, 5);
  // And a fuel scope opened straight through B, which A knows nothing of.
  uint64_t direct = 0;
  ASSERT_EQ(grcore_context_fuel_scope_open(
                w.ctx, 9, GRCORE_SCOPE_POLICY_UNWIND, &direct),
      GRCORE_OK);
  ASSERT_EQ(grcore_context_nested_depth(w.ctx), 1u);
  size_t popped = 0;
  ASSERT_EQ(grcore_unwind_all(w.stack, &popped), GRCORE_OK);
  EXPECT_EQ(popped, 7u);
  EXPECT_EQ(w.log.unwound, (std::vector<uint64_t>{5, 4, 3, 2, 1}));
  EXPECT_EQ(grcore_stack_frame_count(w.stack), 0u);
  EXPECT_EQ(grcore_activation_count(w.stack), 0u);
  EXPECT_EQ(grcore_budget_scope_count(w.stack), 0u);
  EXPECT_EQ(grcore_context_fuel_scope_depth(w.ctx), 0u);
  EXPECT_EQ(guest(w), 0u);
  EXPECT_EQ(native(w), 0u);
  EXPECT_EQ(grcore_context_nested_depth(w.ctx), 0u);
  // The stack is as good as new.
  push_value(w.stack, w.delta, 1);
  EXPECT_EQ(guest(w), 1u);
  // Unwinding an empty stack succeeds and pops nothing.
  ASSERT_EQ(grcore_unwind_all(w.stack, &popped), GRCORE_OK);
  EXPECT_EQ(popped, 1u);
  ASSERT_EQ(grcore_unwind_all(w.stack, &popped), GRCORE_OK);
  EXPECT_EQ(popped, 0u);
  EXPECT_EQ(grcore_unwind_all(nullptr, &popped), GRCORE_ERR_INVALID);
}

TEST(Unwind, AllAfterAnUnwoundRunLeavesNothingBehind) {
  HookWorld w;
  uint64_t base = 0;
  Fn fn{[&](GRCORE_Context * c) {
    GRCORE_Stack * s = grcore_context_stack(c);
    base = grcore_context_fuel_used(c);
    push_value(s, w.delta, 10);
    enter(s, GRCORE_ACTIVATION_JIT);
    push_value(s, w.delta, 11);
    open_scope(s, 1000);
    grcore_context_terminate(c);
    EXPECT_EQ(grcore_stack_poll(c, 1, 1), GRCORE_VERDICT_UNWIND);
    return GRCORE_STEP_UNWOUND;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_ERR_LIMIT);
  (void)base;
  // The run is over but its frames, records and scope are still here.
  EXPECT_EQ(grcore_stack_frame_count(w.stack), 2u);
  EXPECT_EQ(native(w), 1u);
  size_t popped = 0;
  ASSERT_EQ(grcore_unwind_all(w.stack, &popped), GRCORE_OK);
  EXPECT_EQ(popped, 2u);
  EXPECT_EQ(w.log.unwound, (std::vector<uint64_t>{11, 10}));
  EXPECT_EQ(grcore_activation_count(w.stack), 0u);
  EXPECT_EQ(grcore_context_fuel_scope_depth(w.ctx), 0u);
  EXPECT_EQ(guest(w), 0u);
  EXPECT_EQ(native(w), 0u);
}

TEST(Unwind, AllAfterAFinishedRunThatLeftFramesBehindCleansThemToo) {
  HookWorld w;
  Fn fn{[&](GRCORE_Context * c) {
    GRCORE_Stack * s = grcore_context_stack(c);
    push_value(s, w.delta, 20);
    push_value(s, w.delta, 21);
    push_value(s, w.delta, 22);
    enter(s, GRCORE_ACTIVATION_NATIVE);
    return GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  EXPECT_EQ(guest(w), 3u); // stale frames, a used depth, for the next run
  ASSERT_EQ(grcore_unwind_all(w.stack, nullptr), GRCORE_OK);
  EXPECT_EQ(w.log.unwound, (std::vector<uint64_t>{22, 21, 20}));
  EXPECT_EQ(guest(w), 0u);
  EXPECT_EQ(native(w), 0u);
  EXPECT_EQ(grcore_stack_frame_count(w.stack), 0u);
}

TEST(Unwind, TheHookIsNeverRunForAnEngineThatDeclaresNone) {
  HookWorld w;
  push_value(w.stack, w.alpha, 1); // alpha declares no unwind hook
  push_value(w.stack, w.delta, 2);
  push_value(w.stack, w.beta, 3);  // nor does beta
  size_t popped = 0;
  ASSERT_EQ(grcore_unwind_all(w.stack, &popped), GRCORE_OK);
  EXPECT_EQ(popped, 3u);
  EXPECT_EQ(w.log.unwound, std::vector<uint64_t>{2}); // only delta's frame
}

namespace {

/* Pushes frames and records, pausing on the way, so that a context is
 * paused inside nested scopes and activations. Its position is on the stack,
 * and in `phase`, `i` and the references below. */
struct NestedGuest {
  GRCORE_EngineId engine = 0;
  int phase = 0;
  uint64_t i = 0;
  uint64_t n = 30;
  uint64_t sum = 0;
  GRCORE_ActivationRef host{0}, interp{0}, nat{0};
  GRCORE_BudgetScope outer{0}, inner{0};
};

GRCORE_Step nested_entry(GRCORE_Context * c, void * state) {
  auto * g = static_cast<NestedGuest *>(state);
  GRCORE_Stack * s = grcore_context_stack(c);
  if (g->phase == 0) {
    g->host = enter(s, GRCORE_ACTIVATION_HOST);
    g->interp = enter(s, GRCORE_ACTIVATION_INTERPRETER, g->engine);
    g->outer = open_scope(s, GRCORE_UNLIMITED);
    push_value(s, g->engine, 100);
    push_value(s, g->engine, 101);
    g->nat = enter(s, GRCORE_ACTIVATION_NATIVE);
    g->inner = open_scope(s, GRCORE_UNLIMITED);
    push_value(s, g->engine, 102);
    g->phase = 1;
  }
  if (g->phase == 1) {
    while (g->i < g->n) {
      grcore_context_charge_fuel(c, 1);
      GRCORE_Verdict v = grcore_stack_poll(c, 1, g->i);
      if (v == GRCORE_VERDICT_PAUSE) {
        return GRCORE_STEP_PAUSED;
      }
      if (v == GRCORE_VERDICT_UNWIND) {
        return GRCORE_STEP_UNWOUND;
      }
      g->sum += g->i;
      g->i++;
    }
    g->phase = 2;
  }
  EXPECT_EQ(grcore_stack_pop(s), GRCORE_OK);
  EXPECT_EQ(grcore_budget_scope_close(s, g->inner), GRCORE_OK);
  EXPECT_EQ(grcore_activation_leave(s, g->nat), GRCORE_OK);
  EXPECT_EQ(grcore_stack_pop(s), GRCORE_OK);
  EXPECT_EQ(grcore_stack_pop(s), GRCORE_OK);
  EXPECT_EQ(grcore_budget_scope_close(s, g->outer), GRCORE_OK);
  EXPECT_EQ(grcore_activation_leave(s, g->interp), GRCORE_OK);
  EXPECT_EQ(grcore_activation_leave(s, g->host), GRCORE_OK);
  return GRCORE_STEP_FINISHED;
}

} // namespace

TEST(Migrate, APausedContextInsideNestedScopesAndActivationsResumesOnAnotherThread) {
  // The uninterrupted answer.
  uint64_t expected = 0;
  {
    HookWorld w;
    NestedGuest g;
    g.engine = w.delta;
    GRCORE_Outcome outcome;
    ASSERT_EQ(grcore_run(w.ctx, nested_entry, &g, &outcome), GRCORE_OK);
    ASSERT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
    expected = g.sum;
    EXPECT_EQ(grcore_stack_frame_count(w.stack), 0u);
  }
  EXPECT_EQ(expected, 29u * 30u / 2u);

  HookWorld w(5);
  NestedGuest g;
  g.engine = w.delta;
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, nested_entry, &g, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  ASSERT_EQ(grcore_activation_count(w.stack), 3u);
  ASSERT_EQ(grcore_budget_scope_count(w.stack), 2u);
  ASSERT_EQ(grcore_stack_frame_count(w.stack), 3u);
  ASSERT_EQ(native(w), 1u);
  ASSERT_EQ(grcore_context_release(w.ctx), GRCORE_OK);

  GRCORE_Result r = GRCORE_ERR_INTERNAL;
  GRCORE_Outcome there = GRCORE_OUTCOME_PAUSED;
  size_t seen_activations = 0, seen_scopes = 0, seen_frames = 0;
  int pauses = 0;
  std::thread([&] {
    r = grcore_context_acquire(w.ctx);
    if (r != GRCORE_OK) {
      return;
    }
    GRCORE_Stack * s = grcore_context_stack(w.ctx);
    seen_activations = grcore_activation_count(s);
    seen_scopes = grcore_budget_scope_count(s);
    seen_frames = grcore_stack_frame_count(s);
    while (there == GRCORE_OUTCOME_PAUSED && pauses < 100) {
      pauses++;
      grcore_context_set_fuel(w.ctx, grcore_context_fuel_limit(w.ctx) + 7);
      r = grcore_resume(w.ctx, &there);
      if (r != GRCORE_OK) {
        break;
      }
    }
    grcore_context_release(w.ctx);
  }).join();
  ASSERT_EQ(r, GRCORE_OK);
  EXPECT_EQ(seen_activations, 3u);
  EXPECT_EQ(seen_scopes, 2u);
  EXPECT_EQ(seen_frames, 3u);
  EXPECT_EQ(there, GRCORE_OUTCOME_FINISHED);
  ASSERT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
  EXPECT_EQ(g.sum, expected); // the same output as an uninterrupted run
  EXPECT_EQ(grcore_activation_count(w.stack), 0u);
  EXPECT_EQ(grcore_budget_scope_count(w.stack), 0u);
  EXPECT_EQ(grcore_context_fuel_scope_depth(w.ctx), 0u);
  EXPECT_EQ(native(w), 0u);
  EXPECT_EQ(guest(w), 0u);
  EXPECT_GT(pauses, 1);
}

TEST(Migrate, UnwindingAndClosingScopesWorkOnTheThreadTheContextMovedTo) {
  TrackingAllocator t;
  {
    HookWorld w(5, GRCORE_UNLIMITED, GRCORE_DEFAULT_MEMORY_RESERVE,
        GRCORE_UNLIMITED, t.get());
    NestedGuest g;
    g.engine = w.delta;
    GRCORE_Outcome outcome;
    ASSERT_EQ(grcore_run(w.ctx, nested_entry, &g, &outcome), GRCORE_OK);
    ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
    ASSERT_EQ(grcore_context_release(w.ctx), GRCORE_OK);

    size_t popped_inner = 0, popped_outer = 0, popped_all = 0;
    GRCORE_Result r_inner = GRCORE_ERR_INTERNAL, r_outer = GRCORE_ERR_INTERNAL,
                  r_all = GRCORE_ERR_INTERNAL;
    size_t scopes_after_inner = 99, activations_after_inner = 99,
           frames_after_inner = 99, activations_after_outer = 99,
           frames_after_outer = 99;
    uint64_t native_after_inner = 99, native_after_outer = 99;
    std::thread([&] {
      if (grcore_context_acquire(w.ctx) != GRCORE_OK) {
        return;
      }
      GRCORE_Stack * s = grcore_context_stack(w.ctx);
      // The inner scope began after the native activation, so the activation
      // stays; only the one frame above the scope's base is popped.
      r_inner = grcore_budget_scope_unwind(s, g.inner, &popped_inner);
      scopes_after_inner = grcore_budget_scope_count(s);
      activations_after_inner = grcore_activation_count(s);
      frames_after_inner = grcore_stack_frame_count(s);
      native_after_inner = native(w);
      // The outer scope began before it, so unwinding to it leaves it.
      r_outer = grcore_budget_scope_unwind(s, g.outer, &popped_outer);
      activations_after_outer = grcore_activation_count(s);
      frames_after_outer = grcore_stack_frame_count(s);
      native_after_outer = native(w);
      r_all = grcore_unwind_all(s, &popped_all);
      grcore_context_release(w.ctx);
    }).join();
    EXPECT_EQ(r_inner, GRCORE_OK);
    EXPECT_EQ(popped_inner, 1u);
    EXPECT_EQ(scopes_after_inner, 1u);      // the outer scope remains
    EXPECT_EQ(activations_after_inner, 3u);
    EXPECT_EQ(frames_after_inner, 2u);
    EXPECT_EQ(native_after_inner, 1u);
    EXPECT_EQ(r_outer, GRCORE_OK);
    EXPECT_EQ(popped_outer, 2u);
    EXPECT_EQ(activations_after_outer, 2u); // host and interpreter remain
    EXPECT_EQ(frames_after_outer, 0u);
    EXPECT_EQ(native_after_outer, 0u);
    EXPECT_EQ(r_all, GRCORE_OK);
    EXPECT_EQ(popped_all, 0u);
    ASSERT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
    EXPECT_EQ(grcore_activation_count(w.stack), 0u);
    EXPECT_EQ(grcore_stack_frame_count(w.stack), 0u);
    EXPECT_EQ(guest(w), 0u);
    EXPECT_EQ(w.log.unwound, (std::vector<uint64_t>{102, 101, 100}));
  }
  EXPECT_EQ(t.live, 0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

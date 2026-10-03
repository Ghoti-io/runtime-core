/**
 * @file
 *
 * Activation records (AD-17, AD-21): LIFO enter and leave, the native depth
 * budget, nesting, storage, and the refusals.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../../src/a/guest_internal.h"

#include <thread>
#include <vector>

namespace {

GRCORE_ActivationRef enter_ok(GRCORE_Stack * s, GRCORE_ActivationKind kind,
    GRCORE_EngineId e = 0, bool nested = false,
    const GRCORE_CSegment * seg = nullptr) {
  GRCORE_ActivationRef r = {0};
  EXPECT_EQ(grcore_activation_enter(s, kind, e, nested, seg, &r), GRCORE_OK);
  EXPECT_NE(r.id, 0u);
  return r;
}

uint64_t native(const StackWorld & w) {
  return grcore_context_depth(w.ctx, GRCORE_DEPTH_NATIVE);
}

} // namespace

TEST(Activation, EnterAndLeaveInOrderFollowTheCountTheInfoAndTheNativeDepth) {
  StackWorld w;
  EXPECT_EQ(grcore_activation_count(w.stack), 0u);
  GRCORE_ActivationRef host = enter_ok(w.stack, GRCORE_ACTIVATION_HOST);
  GRCORE_ActivationRef interp =
      enter_ok(w.stack, GRCORE_ACTIVATION_INTERPRETER, w.alpha);
  // Neither a host nor an interpreter record enters the native depth.
  EXPECT_EQ(native(w), 0u);
  GRCORE_ActivationRef nat = enter_ok(w.stack, GRCORE_ACTIVATION_NATIVE);
  EXPECT_EQ(native(w), 1u);
  EXPECT_EQ(grcore_activation_count(w.stack), 3u);

  GRCORE_ActivationInfo info;
  ASSERT_EQ(grcore_activation_at(w.stack, 0, &info), GRCORE_OK);
  EXPECT_EQ(info.kind, GRCORE_ACTIVATION_HOST);
  EXPECT_EQ(info.engine, 0u);
  EXPECT_FALSE(info.nested);
  ASSERT_EQ(grcore_activation_at(w.stack, 1, &info), GRCORE_OK);
  EXPECT_EQ(info.kind, GRCORE_ACTIVATION_INTERPRETER);
  EXPECT_EQ(info.engine, w.alpha);
  ASSERT_EQ(grcore_activation_at(w.stack, 2, &info), GRCORE_OK);
  EXPECT_EQ(info.kind, GRCORE_ACTIVATION_NATIVE);
  GRCORE_ActivationRef top;
  ASSERT_EQ(grcore_activation_top(w.stack, &top), GRCORE_OK);
  EXPECT_EQ(top.id, nat.id);
  ASSERT_EQ(grcore_activation_info(w.stack, interp, &info), GRCORE_OK);
  EXPECT_EQ(info.kind, GRCORE_ACTIVATION_INTERPRETER);

  EXPECT_EQ(grcore_activation_leave(w.stack, nat), GRCORE_OK);
  EXPECT_EQ(native(w), 0u);
  EXPECT_EQ(grcore_activation_leave(w.stack, interp), GRCORE_OK);
  EXPECT_EQ(grcore_activation_leave(w.stack, host), GRCORE_OK);
  EXPECT_EQ(grcore_activation_count(w.stack), 0u);
  EXPECT_EQ(grcore_activation_top(w.stack, &top), GRCORE_ERR_INVALID);
}

TEST(Activation, JitNativeAndReentryEnterTheNativeDepthAndLeaveGivesItBack) {
  StackWorld w;
  GRCORE_ActivationKind kinds[] = {GRCORE_ACTIVATION_JIT,
      GRCORE_ACTIVATION_NATIVE, GRCORE_ACTIVATION_REENTRY};
  std::vector<GRCORE_ActivationRef> refs;
  for (size_t i = 0; i < 3; i++) {
    refs.push_back(enter_ok(w.stack, kinds[i]));
    EXPECT_EQ(native(w), i + 1);
  }
  for (size_t i = 3; i > 0; i--) {
    EXPECT_EQ(grcore_activation_leave(w.stack, refs[i - 1]), GRCORE_OK);
    EXPECT_EQ(native(w), i - 1);
  }
}

TEST(Activation, AReentryIsAlwaysNestedAndNestedCountsInB) {
  StackWorld w;
  GRCORE_ActivationRef re = enter_ok(w.stack, GRCORE_ACTIVATION_REENTRY, 0, false);
  GRCORE_ActivationInfo info;
  ASSERT_EQ(grcore_activation_info(w.stack, re, &info), GRCORE_OK);
  EXPECT_TRUE(info.nested);
  EXPECT_EQ(grcore_context_nested_depth(w.ctx), 1u);
  GRCORE_ActivationRef plain = enter_ok(w.stack, GRCORE_ACTIVATION_INTERPRETER, w.alpha);
  EXPECT_EQ(grcore_context_nested_depth(w.ctx), 1u);
  GRCORE_ActivationRef nest =
      enter_ok(w.stack, GRCORE_ACTIVATION_INTERPRETER, w.alpha, true);
  EXPECT_EQ(grcore_context_nested_depth(w.ctx), 2u);
  ASSERT_EQ(grcore_activation_leave(w.stack, nest), GRCORE_OK);
  EXPECT_EQ(grcore_context_nested_depth(w.ctx), 1u);
  ASSERT_EQ(grcore_activation_leave(w.stack, plain), GRCORE_OK);
  ASSERT_EQ(grcore_activation_leave(w.stack, re), GRCORE_OK);
  EXPECT_EQ(grcore_context_nested_depth(w.ctx), 0u);
}

TEST(Activation, ANativeDepthBudgetRefusesTheThirdEnterAndChangesNothing) {
  StackWorld w(GRCORE_UNLIMITED, GRCORE_UNLIMITED,
      GRCORE_DEFAULT_MEMORY_RESERVE, GRCORE_UNLIMITED, nullptr, 2);
  GRCORE_ActivationRef a = enter_ok(w.stack, GRCORE_ACTIVATION_JIT);
  GRCORE_ActivationRef b = enter_ok(w.stack, GRCORE_ACTIVATION_NATIVE);
  GRCORE_ActivationRef c = {55};
  EXPECT_EQ(grcore_activation_enter(w.stack, GRCORE_ACTIVATION_REENTRY, 0, true,
                nullptr, &c),
      GRCORE_ERR_LIMIT);
  EXPECT_EQ(c.id, 55u); // not written
  EXPECT_EQ(grcore_activation_count(w.stack), 2u);
  EXPECT_EQ(native(w), 2u);
  EXPECT_EQ(grcore_context_nested_depth(w.ctx), 0u); // the refused one was nested
  // Kinds that do not enter the native depth are not refused by it.
  GRCORE_ActivationRef h = enter_ok(w.stack, GRCORE_ACTIVATION_HOST);
  GRCORE_ActivationRef i = enter_ok(w.stack, GRCORE_ACTIVATION_INTERPRETER, w.alpha);
  // Leaving gives room back.
  ASSERT_EQ(grcore_activation_leave(w.stack, i), GRCORE_OK);
  ASSERT_EQ(grcore_activation_leave(w.stack, h), GRCORE_OK);
  ASSERT_EQ(grcore_activation_leave(w.stack, b), GRCORE_OK);
  GRCORE_ActivationRef again = enter_ok(w.stack, GRCORE_ACTIVATION_JIT);
  EXPECT_EQ(native(w), 2u);
  ASSERT_EQ(grcore_activation_leave(w.stack, again), GRCORE_OK);
  ASSERT_EQ(grcore_activation_leave(w.stack, a), GRCORE_OK);
}

TEST(Activation, ACSegmentIsRecordedAndAnEmptyOneIsRefused) {
  StackWorld w;
  GRCORE_CSegment seg = {0x1000, 0x2000};
  GRCORE_ActivationRef r = enter_ok(w.stack, GRCORE_ACTIVATION_NATIVE, w.beta, false, &seg);
  GRCORE_ActivationInfo info;
  ASSERT_EQ(grcore_activation_info(w.stack, r, &info), GRCORE_OK);
  EXPECT_EQ(info.segment_lo, 0x1000u);
  EXPECT_EQ(info.segment_hi, 0x2000u);
  EXPECT_EQ(info.engine, w.beta);
  GRCORE_ActivationRef none = enter_ok(w.stack, GRCORE_ACTIVATION_HOST);
  ASSERT_EQ(grcore_activation_info(w.stack, none, &info), GRCORE_OK);
  EXPECT_EQ(info.segment_lo, 0u);
  EXPECT_EQ(info.segment_hi, 0u);
  grcore_activation_leave(w.stack, none);
  grcore_activation_leave(w.stack, r);

  GRCORE_CSegment empty = {0x2000, 0x2000}, backwards = {0x3000, 0x2000};
  GRCORE_ActivationRef out = {9};
  EXPECT_EQ(grcore_activation_enter(w.stack, GRCORE_ACTIVATION_NATIVE, 0, false,
                &empty, &out),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_activation_enter(w.stack, GRCORE_ACTIVATION_NATIVE, 0, false,
                &backwards, &out),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(out.id, 9u);
  EXPECT_EQ(native(w), 0u);
}

TEST(Activation, ARecordRemembersTheFrameCountItBeganWith) {
  StackWorld w;
  GRCORE_FrameRef f;
  ASSERT_EQ(grcore_stack_push(w.stack, w.alpha, 1, &f), GRCORE_OK);
  ASSERT_EQ(grcore_stack_push(w.stack, w.alpha, 1, &f), GRCORE_OK);
  GRCORE_ActivationRef r = enter_ok(w.stack, GRCORE_ACTIVATION_INTERPRETER, w.alpha);
  GRCORE_ActivationInfo info;
  ASSERT_EQ(grcore_activation_info(w.stack, r, &info), GRCORE_OK);
  EXPECT_EQ(info.base_frame_count, 2u);
  // Growth moves the buffer; the record holds a count, not an address.
  grcore_stack_set_always_move(w.stack, true);
  for (int i = 0; i < 20; i++) {
    ASSERT_EQ(grcore_stack_push(w.stack, w.alpha, 3, &f), GRCORE_OK);
  }
  ASSERT_EQ(grcore_activation_info(w.stack, r, &info), GRCORE_OK);
  EXPECT_EQ(info.base_frame_count, 2u);
  for (int i = 0; i < 20; i++) {
    ASSERT_EQ(grcore_stack_pop(w.stack), GRCORE_OK);
  }
  EXPECT_EQ(grcore_activation_leave(w.stack, r), GRCORE_OK);
}

TEST(Activation, LeaveRefusesAnythingButTheInnermostRecordAtItsBase) {
  StackWorld w;
  GRCORE_ActivationRef outer = enter_ok(w.stack, GRCORE_ACTIVATION_HOST);
  GRCORE_ActivationRef inner = enter_ok(w.stack, GRCORE_ACTIVATION_NATIVE);
  // Out of order.
  EXPECT_EQ(grcore_activation_leave(w.stack, outer), GRCORE_ERR_INVALID);
  // Forged and null references.
  EXPECT_EQ(grcore_activation_leave(w.stack, GRCORE_ActivationRef{0}), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_activation_leave(w.stack, GRCORE_ActivationRef{9999}), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_activation_leave(nullptr, inner), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_activation_count(w.stack), 2u);
  EXPECT_EQ(native(w), 1u);

  // A frame pushed since and not popped.
  GRCORE_FrameRef f;
  ASSERT_EQ(grcore_stack_push(w.stack, w.alpha, 1, &f), GRCORE_OK);
  EXPECT_EQ(grcore_activation_leave(w.stack, inner), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_activation_count(w.stack), 2u);
  EXPECT_EQ(native(w), 1u);
  ASSERT_EQ(grcore_stack_pop(w.stack), GRCORE_OK);

  ASSERT_EQ(grcore_activation_leave(w.stack, inner), GRCORE_OK);
  // A stale reference: the record is gone, and its id is never reused.
  EXPECT_EQ(grcore_activation_leave(w.stack, inner), GRCORE_ERR_INVALID);
  GRCORE_ActivationRef next = enter_ok(w.stack, GRCORE_ACTIVATION_NATIVE);
  EXPECT_NE(next.id, inner.id);
  EXPECT_EQ(grcore_activation_leave(w.stack, inner), GRCORE_ERR_INVALID);
  ASSERT_EQ(grcore_activation_leave(w.stack, next), GRCORE_OK);
  ASSERT_EQ(grcore_activation_leave(w.stack, outer), GRCORE_OK);
}

TEST(Activation, LeaveRefusesWhenFramesBelowTheBaseWerePopped) {
  StackWorld w;
  GRCORE_FrameRef f;
  ASSERT_EQ(grcore_stack_push(w.stack, w.alpha, 1, &f), GRCORE_OK);
  GRCORE_ActivationRef r = enter_ok(w.stack, GRCORE_ACTIVATION_INTERPRETER, w.alpha);
  ASSERT_EQ(grcore_stack_pop(w.stack), GRCORE_OK); // the caller's frame, not ours
  EXPECT_EQ(grcore_activation_leave(w.stack, r), GRCORE_ERR_INVALID);
  ASSERT_EQ(grcore_stack_push(w.stack, w.alpha, 1, &f), GRCORE_OK);
  EXPECT_EQ(grcore_activation_leave(w.stack, r), GRCORE_OK);
}

TEST(Activation, LeaveRefusesWhileABudgetScopeOpenedInsideIsStillOpen) {
  StackWorld w;
  GRCORE_ActivationRef r = enter_ok(w.stack, GRCORE_ACTIVATION_INTERPRETER, w.alpha);
  GRCORE_BudgetScope scope;
  ASSERT_EQ(grcore_budget_scope_open(
                w.stack, 100, GRCORE_SCOPE_POLICY_UNWIND, &scope),
      GRCORE_OK);
  EXPECT_EQ(grcore_activation_leave(w.stack, r), GRCORE_ERR_INVALID);
  ASSERT_EQ(grcore_budget_scope_close(w.stack, scope), GRCORE_OK);
  EXPECT_EQ(grcore_activation_leave(w.stack, r), GRCORE_OK);
  // A scope opened BEFORE the activation does not block its leave.
  ASSERT_EQ(grcore_budget_scope_open(
                w.stack, 100, GRCORE_SCOPE_POLICY_UNWIND, &scope),
      GRCORE_OK);
  GRCORE_ActivationRef r2 = enter_ok(w.stack, GRCORE_ACTIVATION_NATIVE);
  EXPECT_EQ(grcore_activation_leave(w.stack, r2), GRCORE_OK);
  EXPECT_EQ(grcore_budget_scope_close(w.stack, scope), GRCORE_OK);
}

TEST(Activation, ArgumentsAreValidated) {
  StackWorld w;
  GRCORE_ActivationRef out = {5};
  EXPECT_EQ(grcore_activation_enter(nullptr, GRCORE_ACTIVATION_HOST, 0, false, nullptr, &out),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_activation_enter(w.stack, GRCORE_ACTIVATION_HOST, 0, false, nullptr, nullptr),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_activation_enter(w.stack, static_cast<GRCORE_ActivationKind>(7), 0,
                false, nullptr, &out),
      GRCORE_ERR_INVALID);
  // An engine id that was never registered.
  EXPECT_EQ(grcore_activation_enter(w.stack, GRCORE_ACTIVATION_HOST, 3, false, nullptr, &out),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(out.id, 5u);
  EXPECT_EQ(grcore_activation_count(w.stack), 0u);
  GRCORE_ActivationInfo info;
  EXPECT_EQ(grcore_activation_at(w.stack, 0, &info), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_activation_at(nullptr, 0, &info), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_activation_at(w.stack, 0, nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_activation_info(w.stack, GRCORE_ActivationRef{0}, &info), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_activation_info(nullptr, GRCORE_ActivationRef{1}, &info), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_activation_top(nullptr, &out), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_activation_count(nullptr), 0u);
}

TEST(Activation, ANonOwnerCannotEnterOrLeave) {
  StackWorld w;
  GRCORE_ActivationRef mine = enter_ok(w.stack, GRCORE_ACTIVATION_HOST);
  GRCORE_Result enter = GRCORE_OK, leave = GRCORE_OK;
  GRCORE_ActivationRef out = {0};
  std::thread([&] {
    enter = grcore_activation_enter(
        w.stack, GRCORE_ACTIVATION_HOST, 0, false, nullptr, &out);
    leave = grcore_activation_leave(w.stack, mine);
  }).join();
  EXPECT_EQ(enter, GRCORE_ERR_INVALID);
  EXPECT_EQ(leave, GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_activation_count(w.stack), 1u);
  EXPECT_EQ(grcore_activation_leave(w.stack, mine), GRCORE_OK);
}

TEST(Activation, ManyRecordsGrowTheArrayAndKeepTheirContents) {
  StackWorld w;
  std::vector<GRCORE_ActivationRef> refs;
  for (int i = 0; i < 50; i++) {
    refs.push_back(enter_ok(w.stack, i % 2 ? GRCORE_ACTIVATION_NATIVE
                                           : GRCORE_ACTIVATION_INTERPRETER,
        w.alpha));
  }
  EXPECT_EQ(native(w), 25u);
  for (size_t i = 0; i < 50; i++) {
    GRCORE_ActivationInfo info;
    ASSERT_EQ(grcore_activation_at(w.stack, i, &info), GRCORE_OK);
    EXPECT_EQ(info.kind, i % 2 ? GRCORE_ACTIVATION_NATIVE
                               : GRCORE_ACTIVATION_INTERPRETER);
  }
  for (size_t i = 50; i > 0; i--) {
    ASSERT_EQ(grcore_activation_leave(w.stack, refs[i - 1]), GRCORE_OK);
  }
  EXPECT_EQ(native(w), 0u);
}

TEST(Activation, EveryAllocationFailureOfEnterIsOomAndChangesNothing) {
  bool succeeded = false;
  int failures = 0;
  for (long n = 1; n < 10 && !succeeded; n++) {
    TrackingAllocator t;
    {
      StackWorld w(GRCORE_UNLIMITED, GRCORE_UNLIMITED,
          GRCORE_DEFAULT_MEMORY_RESERVE, GRCORE_UNLIMITED, t.get());
      GRCORE_ActivationRef out = {31};
      t.fail_at = t.calls + n;
      GRCORE_Result r = grcore_activation_enter(
          w.stack, GRCORE_ACTIVATION_REENTRY, 0, true, nullptr, &out);
      t.fail_at = 0;
      if (r == GRCORE_OK) {
        succeeded = true;
        EXPECT_EQ(grcore_activation_count(w.stack), 1u);
      } else {
        failures++;
        EXPECT_EQ(r, GRCORE_ERR_OOM) << "n=" << n;
        EXPECT_EQ(out.id, 31u) << "n=" << n;
        EXPECT_EQ(grcore_activation_count(w.stack), 0u) << "n=" << n;
        EXPECT_EQ(native(w), 0u) << "n=" << n;
        EXPECT_EQ(grcore_context_nested_depth(w.ctx), 0u) << "n=" << n;
      }
    }
    EXPECT_EQ(t.live, 0) << "n=" << n;
  }
  EXPECT_TRUE(succeeded);
  EXPECT_GE(failures, 1);
}

TEST(Activation, ATightMemoryBudgetRefusesEnterAsALimitAndChangesNothing) {
  StackWorld w(GRCORE_UNLIMITED, GRCORE_UNLIMITED, 0);
  ASSERT_EQ(grcore_context_set_memory_bytes(w.ctx, grcore_context_memory_in_use(w.ctx)),
      GRCORE_OK);
  uint64_t refusals = grcore_context_memory_refusals(w.ctx);
  GRCORE_ActivationRef out = {31};
  EXPECT_EQ(grcore_activation_enter(
                w.stack, GRCORE_ACTIVATION_NATIVE, 0, false, nullptr, &out),
      GRCORE_ERR_LIMIT);
  EXPECT_GT(grcore_context_memory_refusals(w.ctx), refusals);
  EXPECT_EQ(out.id, 31u);
  EXPECT_EQ(grcore_activation_count(w.stack), 0u);
  EXPECT_EQ(native(w), 0u);
  ASSERT_EQ(grcore_context_set_memory_bytes(w.ctx, GRCORE_UNLIMITED), GRCORE_OK);
  GRCORE_ActivationRef ok = enter_ok(w.stack, GRCORE_ACTIVATION_NATIVE);
  EXPECT_EQ(grcore_activation_leave(w.stack, ok), GRCORE_OK);
}

TEST(Activation, ANestedActivationTurnsAFuelPauseIntoALimitUnwindUntilItIsLeft) {
  StackWorld w(10);
  GRCORE_Verdict inner = GRCORE_VERDICT_CONTINUE, outer = GRCORE_VERDICT_CONTINUE;
  Fn fn{[&](GRCORE_Context * c) {
    GRCORE_Stack * s = grcore_context_stack(c);
    GRCORE_FrameRef f;
    EXPECT_EQ(grcore_stack_push(s, w.alpha, 1, &f), GRCORE_OK);
    GRCORE_ActivationRef re;
    EXPECT_EQ(grcore_activation_enter(
                  s, GRCORE_ACTIVATION_REENTRY, 0, false, nullptr, &re),
        GRCORE_OK);
    grcore_context_charge_fuel(c, 11);
    inner = grcore_stack_poll(c, 1, 1);
    EXPECT_EQ(grcore_context_unwind_result(c), GRCORE_ERR_LIMIT);
    EXPECT_EQ(grcore_activation_leave(s, re), GRCORE_OK);
    outer = grcore_stack_poll(c, 1, 2);
    return outer == GRCORE_VERDICT_PAUSE ? GRCORE_STEP_PAUSED
                                         : GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_EQ(inner, GRCORE_VERDICT_UNWIND);
  EXPECT_EQ(outer, GRCORE_VERDICT_PAUSE);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  // An INTERPRETER record with `nested` set does the same.
  GRCORE_Stack * s = w.stack;
  GRCORE_ActivationRef nest;
  ASSERT_EQ(grcore_activation_enter(
                s, GRCORE_ACTIVATION_INTERPRETER, w.alpha, true, nullptr, &nest),
      GRCORE_OK);
  EXPECT_EQ(grcore_context_nested_depth(w.ctx), 1u);
  EXPECT_EQ(grcore_activation_leave(s, nest), GRCORE_OK);
}

TEST(Activation, ADestroyedContextWithRecordsStillOpenFreesEverything) {
  TrackingAllocator t;
  {
    StackWorld w(GRCORE_UNLIMITED, GRCORE_UNLIMITED,
        GRCORE_DEFAULT_MEMORY_RESERVE, GRCORE_UNLIMITED, t.get());
    for (int i = 0; i < 20; i++) {
      enter_ok(w.stack, GRCORE_ACTIVATION_NATIVE);
    }
  }
  EXPECT_EQ(t.live, 0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

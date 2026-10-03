/**
 * @file
 *
 * Budget scopes in A (AD-21): where on the stack a scope began, unwinding to
 * its boundary, and the page, sidebar and nav pane scenario end to end.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../../src/a/guest_internal.h"

#include <thread>
#include <vector>

namespace {

std::vector<const GRCORE_Key *> pause_keys(const GRCORE_Context * c) {
  std::vector<const GRCORE_Key *> keys;
  for (size_t i = 0; i < grcore_context_pause_key_count(c); i++) {
    keys.push_back(grcore_context_pause_key(c, i));
  }
  return keys;
}

const GRCORE_Key * core(unsigned kind) { return grcore_core_key(kind); }

GRCORE_BudgetScope open_scope(GRCORE_Stack * s, uint64_t fuel,
    GRCORE_ScopePolicy policy = GRCORE_SCOPE_POLICY_UNWIND) {
  GRCORE_BudgetScope scope = {0};
  EXPECT_EQ(grcore_budget_scope_open(s, fuel, policy, &scope), GRCORE_OK);
  EXPECT_NE(scope.id, 0u);
  return scope;
}

GRCORE_FrameRef push(GRCORE_Stack * s, GRCORE_EngineId e, uint64_t v) {
  GRCORE_FrameRef f = {0};
  EXPECT_EQ(grcore_stack_push(s, e, 2, &f), GRCORE_OK);
  grcore_stack_slot_set(s, f, 0, v);
  return f;
}

uint64_t used_of(GRCORE_Context * c, GRCORE_BudgetScope s) {
  uint64_t v = 12345;
  EXPECT_EQ(grcore_context_fuel_scope_used(c, s.id, &v), GRCORE_OK);
  return v;
}

} // namespace

TEST(BudgetScope, OpenRemembersTheBaseAndCloseRequiresTheStackBackThere) {
  StackWorld w;
  EXPECT_EQ(grcore_budget_scope_count(w.stack), 0u);
  push(w.stack, w.alpha, 1);
  GRCORE_BudgetScope s = open_scope(w.stack, 100);
  EXPECT_EQ(grcore_budget_scope_count(w.stack), 1u);
  GRCORE_BudgetScope inner;
  ASSERT_EQ(grcore_budget_scope_innermost(w.stack, &inner), GRCORE_OK);
  EXPECT_EQ(inner.id, s.id);
  EXPECT_EQ(grcore_context_fuel_scope_top(w.ctx), s.id); // B counts it
  push(w.stack, w.alpha, 2);
  // Frames above the base: refused, nothing changes.
  EXPECT_EQ(grcore_budget_scope_close(w.stack, s), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_budget_scope_count(w.stack), 1u);
  EXPECT_EQ(grcore_context_fuel_scope_depth(w.ctx), 1u);
  ASSERT_EQ(grcore_stack_pop(w.stack), GRCORE_OK);
  ASSERT_EQ(grcore_budget_scope_close(w.stack, s), GRCORE_OK);
  EXPECT_EQ(grcore_budget_scope_count(w.stack), 0u);
  EXPECT_EQ(grcore_context_fuel_scope_depth(w.ctx), 0u);
  EXPECT_EQ(grcore_budget_scope_innermost(w.stack, &inner), GRCORE_ERR_INVALID);
}

TEST(BudgetScope, MisuseIsInvalidAndChangesNothing) {
  StackWorld w;
  GRCORE_BudgetScope out = {55};
  EXPECT_EQ(grcore_budget_scope_open(nullptr, 1, GRCORE_SCOPE_POLICY_UNWIND, &out),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_budget_scope_open(w.stack, 1, GRCORE_SCOPE_POLICY_UNWIND, nullptr),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_budget_scope_open(
                w.stack, 1, static_cast<GRCORE_ScopePolicy>(4), &out),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(out.id, 55u);
  EXPECT_EQ(grcore_budget_scope_count(w.stack), 0u);
  EXPECT_EQ(grcore_budget_scope_close(w.stack, GRCORE_BudgetScope{0}), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_budget_scope_close(w.stack, GRCORE_BudgetScope{7}), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_budget_scope_close(nullptr, GRCORE_BudgetScope{7}), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_budget_scope_unwind(w.stack, GRCORE_BudgetScope{7}, nullptr),
      GRCORE_ERR_INVALID);
  EXPECT_FALSE(grcore_budget_scope_exhausted(w.stack, GRCORE_BudgetScope{7}));
  EXPECT_FALSE(grcore_budget_scope_exhausted(nullptr, GRCORE_BudgetScope{7}));
  EXPECT_EQ(grcore_budget_scope_count(nullptr), 0u);

  GRCORE_BudgetScope outer = open_scope(w.stack, 100);
  GRCORE_BudgetScope inner = open_scope(w.stack, 100);
  // Not the innermost.
  EXPECT_EQ(grcore_budget_scope_close(w.stack, outer), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_budget_scope_count(w.stack), 2u);
  ASSERT_EQ(grcore_budget_scope_close(w.stack, inner), GRCORE_OK);
  // Stale.
  EXPECT_EQ(grcore_budget_scope_close(w.stack, inner), GRCORE_ERR_INVALID);
  ASSERT_EQ(grcore_budget_scope_close(w.stack, outer), GRCORE_OK);
}

TEST(BudgetScope, ANonOwnerCannotOpenCloseOrUnwind) {
  StackWorld w;
  GRCORE_BudgetScope mine = open_scope(w.stack, 100);
  GRCORE_Result open = GRCORE_OK, close = GRCORE_OK, unwind = GRCORE_OK;
  GRCORE_BudgetScope out = {0};
  std::thread([&] {
    open = grcore_budget_scope_open(w.stack, 5, GRCORE_SCOPE_POLICY_UNWIND, &out);
    close = grcore_budget_scope_close(w.stack, mine);
    unwind = grcore_budget_scope_unwind(w.stack, mine, nullptr);
  }).join();
  EXPECT_EQ(open, GRCORE_ERR_INVALID);
  EXPECT_EQ(close, GRCORE_ERR_INVALID);
  EXPECT_EQ(unwind, GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_budget_scope_count(w.stack), 1u);
}

TEST(BudgetScope, CloseRefusesWhileAnActivationOpenedInsideIsStillOpen) {
  StackWorld w;
  GRCORE_BudgetScope s = open_scope(w.stack, 100);
  GRCORE_ActivationRef a;
  ASSERT_EQ(grcore_activation_enter(
                w.stack, GRCORE_ACTIVATION_NATIVE, 0, false, nullptr, &a),
      GRCORE_OK);
  EXPECT_EQ(grcore_budget_scope_close(w.stack, s), GRCORE_ERR_INVALID);
  ASSERT_EQ(grcore_activation_leave(w.stack, a), GRCORE_OK);
  EXPECT_EQ(grcore_budget_scope_close(w.stack, s), GRCORE_OK);
}

TEST(BudgetScope, CloseRefusesAScopeOpenedStraightThroughBLeftOpenInsideIt) {
  StackWorld w;
  GRCORE_BudgetScope s = open_scope(w.stack, 100);
  uint64_t direct = 0;
  ASSERT_EQ(grcore_context_fuel_scope_open(
                w.ctx, 5, GRCORE_SCOPE_POLICY_UNWIND, &direct),
      GRCORE_OK);
  EXPECT_EQ(grcore_budget_scope_close(w.stack, s), GRCORE_ERR_INVALID);
  ASSERT_EQ(grcore_context_fuel_scope_close(w.ctx, direct), GRCORE_OK);
  EXPECT_EQ(grcore_budget_scope_close(w.stack, s), GRCORE_OK);
  // Unwinding to the scope closes the stray one with it.
  s = open_scope(w.stack, 100);
  ASSERT_EQ(grcore_context_fuel_scope_open(
                w.ctx, 5, GRCORE_SCOPE_POLICY_UNWIND, &direct),
      GRCORE_OK);
  ASSERT_EQ(grcore_budget_scope_unwind(w.stack, s, nullptr), GRCORE_OK);
  EXPECT_EQ(grcore_context_fuel_scope_depth(w.ctx), 0u);
  EXPECT_EQ(grcore_budget_scope_count(w.stack), 0u);
}

TEST(BudgetScope, AnExhaustedScopeUnwindsToItsBoundaryAndRunFinishesOk) {
  HookWorld w(1000);
  GRCORE_Verdict verdict = GRCORE_VERDICT_CONTINUE;
  GRCORE_Result reason = GRCORE_OK;
  std::vector<const GRCORE_Key *> keys;
  bool exhausted = false;
  size_t popped = 99;
  Fn fn{[&](GRCORE_Context * c) {
    GRCORE_Stack * s = grcore_context_stack(c);
    push(s, w.delta, 1); // the caller, below the boundary
    GRCORE_BudgetScope scope = open_scope(s, 10);
    push(s, w.delta, 2);
    push(s, w.delta, 3);
    grcore_context_charge_fuel(c, 11);
    verdict = grcore_stack_poll(c, 7, 7);
    reason = grcore_context_unwind_result(c);
    keys = pause_keys(c);
    exhausted = grcore_budget_scope_exhausted(s, scope);
    EXPECT_EQ(grcore_budget_scope_unwind(s, scope, &popped), GRCORE_OK);
    // The caller carries on: its frame is intact, the scope is closed.
    EXPECT_EQ(grcore_stack_frame_count(s), 1u);
    EXPECT_EQ(grcore_budget_scope_count(s), 0u);
    EXPECT_FALSE(grcore_budget_scope_exhausted(s, scope));
    EXPECT_EQ(grcore_stack_poll(c, 1, 2), GRCORE_VERDICT_CONTINUE);
    EXPECT_EQ(grcore_stack_pop(s), GRCORE_OK);
    return GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome = GRCORE_OUTCOME_PAUSED;
  EXPECT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  EXPECT_EQ(verdict, GRCORE_VERDICT_UNWIND);
  EXPECT_EQ(reason, GRCORE_ERR_LIMIT);
  EXPECT_EQ(keys, std::vector<const GRCORE_Key *>{core(GRCORE_REQUEST_FUEL)});
  EXPECT_TRUE(exhausted);
  EXPECT_EQ(popped, 2u);
  EXPECT_EQ(w.log.unwound, (std::vector<uint64_t>{3, 2}));
}

TEST(BudgetScope, AScopeIsNotTheCauseWhenTerminateOrTheCeilingDecidedToo) {
  StackWorld w(20);
  int stage = 0;
  GRCORE_BudgetScope scope = {0};
  bool by_ceiling = true, by_terminate = true;
  Fn fn{[&](GRCORE_Context * c) {
    GRCORE_Stack * s = grcore_context_stack(c);
    if (stage == 0) {
      push(s, w.alpha, 1);
      scope = open_scope(s, 10);
      grcore_context_charge_fuel(c, 30); // the ceiling and the scope both
      stage = 1;
      EXPECT_EQ(grcore_stack_poll(c, 1, 1), GRCORE_VERDICT_PAUSE);
      by_ceiling = grcore_budget_scope_exhausted(s, scope);
      return GRCORE_STEP_PAUSED;
    }
    EXPECT_EQ(grcore_context_terminate(c), GRCORE_OK);
    EXPECT_EQ(grcore_stack_poll(c, 1, 2), GRCORE_VERDICT_UNWIND);
    by_terminate = grcore_budget_scope_exhausted(s, scope);
    return GRCORE_STEP_UNWOUND;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  EXPECT_FALSE(by_ceiling);
  ASSERT_EQ(grcore_context_set_fuel(w.ctx, 1000), GRCORE_OK);
  EXPECT_EQ(grcore_resume(w.ctx, &outcome), GRCORE_ERR_LIMIT);
  EXPECT_FALSE(by_terminate);
}

TEST(BudgetScope, ThePausePolicyPausesAtTheScopeAndTheHostRaisesItsBudget) {
  StackWorld w(1000);
  GRCORE_BudgetScope scope = {0};
  int stage = 0;
  Fn fn{[&](GRCORE_Context * c) {
    GRCORE_Stack * s = grcore_context_stack(c);
    if (stage == 0) {
      push(s, w.alpha, 1);
      scope = open_scope(s, 10, GRCORE_SCOPE_POLICY_PAUSE);
      grcore_context_charge_fuel(c, 11);
      stage = 1;
      EXPECT_EQ(grcore_stack_poll(c, 1, 1), GRCORE_VERDICT_PAUSE);
      EXPECT_FALSE(grcore_budget_scope_exhausted(s, scope));
      return GRCORE_STEP_PAUSED;
    }
    EXPECT_EQ(grcore_stack_poll(c, 1, 2), GRCORE_VERDICT_CONTINUE);
    EXPECT_EQ(grcore_budget_scope_close(s, scope), GRCORE_OK);
    EXPECT_EQ(grcore_stack_pop(s), GRCORE_OK);
    return GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  EXPECT_EQ(pause_keys(w.ctx), std::vector<const GRCORE_Key *>{core(GRCORE_REQUEST_FUEL)});
  ASSERT_EQ(grcore_context_fuel_scope_set_budget(w.ctx, scope.id, 50), GRCORE_OK);
  ASSERT_EQ(grcore_resume(w.ctx, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
}

TEST(BudgetScope, UnwindingToAnOuterScopeLeavesDeeperActivationsAndClosesDeeperScopes) {
  HookWorld w;
  GRCORE_FrameRef f;
  (void)f;
  push(w.stack, w.delta, 1);
  GRCORE_BudgetScope outer = open_scope(w.stack, 100);
  push(w.stack, w.delta, 2);
  GRCORE_ActivationRef a;
  ASSERT_EQ(grcore_activation_enter(
                w.stack, GRCORE_ACTIVATION_JIT, 0, false, nullptr, &a),
      GRCORE_OK);
  push(w.stack, w.delta, 3);
  GRCORE_BudgetScope inner = open_scope(w.stack, 10);
  (void)inner;
  push(w.stack, w.delta, 4);
  ASSERT_EQ(grcore_context_depth(w.ctx, GRCORE_DEPTH_NATIVE), 1u);
  size_t popped = 0;
  ASSERT_EQ(grcore_budget_scope_unwind(w.stack, outer, &popped), GRCORE_OK);
  EXPECT_EQ(popped, 3u);
  EXPECT_EQ(w.log.unwound, (std::vector<uint64_t>{4, 3, 2}));
  EXPECT_EQ(grcore_stack_frame_count(w.stack), 1u);
  EXPECT_EQ(grcore_activation_count(w.stack), 0u);
  EXPECT_EQ(grcore_budget_scope_count(w.stack), 0u);
  EXPECT_EQ(grcore_context_fuel_scope_depth(w.ctx), 0u);
  EXPECT_EQ(grcore_context_depth(w.ctx, GRCORE_DEPTH_NATIVE), 0u);
  EXPECT_EQ(grcore_context_depth(w.ctx, GRCORE_DEPTH_GUEST), 1u);
}

TEST(BudgetScope, PageSidebarAndNavPaneWithFramesNavUnwindsAndTheOthersKeepTheirBudgets) {
  HookWorld w;
  uint64_t page_used = 0, sidebar_used = 0, nav_used = 0;
  uint64_t page_remaining = 0;
  int nav_steps = 0;
  size_t nav_popped = 0;
  Fn fn{[&](GRCORE_Context * c) {
    GRCORE_Stack * s = grcore_context_stack(c);
    push(s, w.delta, 10); // the page's own call
    GRCORE_BudgetScope page = open_scope(s, 1000);
    grcore_context_charge_fuel(c, 20);

    push(s, w.delta, 11); // the sidebar template call
    GRCORE_BudgetScope sidebar = open_scope(s, 500);
    grcore_context_charge_fuel(c, 30);
    EXPECT_EQ(grcore_stack_poll(c, 11, 1), GRCORE_VERDICT_CONTINUE);
    sidebar_used = used_of(c, sidebar);
    EXPECT_EQ(grcore_budget_scope_close(s, sidebar), GRCORE_OK);
    EXPECT_EQ(grcore_stack_pop(s), GRCORE_OK);

    push(s, w.delta, 12); // the nav pane, which never stops
    GRCORE_BudgetScope nav = open_scope(s, 100);
    for (;;) {
      push(s, w.delta, 13); // each iteration calls deeper
      grcore_context_charge_fuel(c, 7);
      nav_steps++;
      GRCORE_Verdict v = grcore_stack_poll(c, 12, static_cast<uint64_t>(nav_steps));
      if (v == GRCORE_VERDICT_UNWIND) {
        break;
      }
      EXPECT_EQ(grcore_stack_pop(s), GRCORE_OK);
      if (nav_steps > 1000) {
        ADD_FAILURE() << "the nav pane was never stopped";
        break;
      }
    }
    EXPECT_TRUE(grcore_budget_scope_exhausted(s, nav));
    nav_used = used_of(c, nav);
    EXPECT_EQ(grcore_budget_scope_unwind(s, nav, &nav_popped), GRCORE_OK);
    EXPECT_EQ(grcore_stack_pop(s), GRCORE_OK); // the nav call's own frame

    grcore_context_charge_fuel(c, 20);
    EXPECT_EQ(grcore_stack_poll(c, 10, 2), GRCORE_VERDICT_CONTINUE);
    page_used = used_of(c, page);
    uint64_t r = 0;
    EXPECT_EQ(grcore_context_fuel_scope_remaining(c, page.id, &r), GRCORE_OK);
    page_remaining = r;
    EXPECT_EQ(grcore_budget_scope_close(s, page), GRCORE_OK);
    EXPECT_EQ(grcore_stack_pop(s), GRCORE_OK);
    return GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  EXPECT_EQ(nav_steps, 15);
  EXPECT_EQ(nav_used, 105u);
  EXPECT_EQ(nav_popped, 1u); // the innermost call, above the nav pane's boundary
  EXPECT_EQ(sidebar_used, 30u);
  EXPECT_EQ(page_used, 40u);
  EXPECT_EQ(page_remaining, 960u);
  EXPECT_EQ(grcore_stack_frame_count(w.stack), 0u);
  EXPECT_EQ(grcore_context_depth(w.ctx, GRCORE_DEPTH_GUEST), 0u);
}

TEST(BudgetScope, TenThousandChildCallsPauseAtTheCeilingWithABoundedNumberOpened) {
  StackWorld w(100000);
  int opened = 0;
  Fn fn{[&](GRCORE_Context * c) {
    GRCORE_Stack * s = grcore_context_stack(c);
    for (int i = 0; i < 10000; i++) {
      push(s, w.alpha, 1);
      GRCORE_BudgetScope scope = open_scope(s, 1000);
      opened++;
      grcore_context_charge_fuel(c, 20);
      if (grcore_stack_poll(c, 1, 1) == GRCORE_VERDICT_PAUSE) {
        return GRCORE_STEP_PAUSED;
      }
      EXPECT_EQ(grcore_budget_scope_close(s, scope), GRCORE_OK);
      EXPECT_EQ(grcore_stack_pop(s), GRCORE_OK);
    }
    return GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  EXPECT_EQ(pause_keys(w.ctx), std::vector<const GRCORE_Key *>{core(GRCORE_REQUEST_FUEL)});
  EXPECT_EQ(opened, 5001);
  EXPECT_EQ(grcore_budget_scope_count(w.stack), 1u);
  EXPECT_EQ(grcore_stack_frame_count(w.stack), 1u);
}

TEST(BudgetScope, EveryAllocationFailureOfOpenIsOomAndOpensNothing) {
  bool succeeded = false;
  int failures = 0;
  for (long n = 1; n < 10 && !succeeded; n++) {
    TrackingAllocator t;
    {
      StackWorld w(GRCORE_UNLIMITED, GRCORE_UNLIMITED,
          GRCORE_DEFAULT_MEMORY_RESERVE, GRCORE_UNLIMITED, t.get());
      GRCORE_BudgetScope out = {31};
      t.fail_at = t.calls + n;
      GRCORE_Result r =
          grcore_budget_scope_open(w.stack, 5, GRCORE_SCOPE_POLICY_UNWIND, &out);
      t.fail_at = 0;
      if (r == GRCORE_OK) {
        succeeded = true;
        EXPECT_EQ(grcore_budget_scope_count(w.stack), 1u);
        EXPECT_EQ(grcore_context_fuel_scope_depth(w.ctx), 1u);
      } else {
        failures++;
        EXPECT_EQ(r, GRCORE_ERR_OOM) << "n=" << n;
        EXPECT_EQ(out.id, 31u) << "n=" << n;
        // Neither A's record nor B's scope exists: they succeed or fail together.
        EXPECT_EQ(grcore_budget_scope_count(w.stack), 0u) << "n=" << n;
        EXPECT_EQ(grcore_context_fuel_scope_depth(w.ctx), 0u) << "n=" << n;
      }
    }
    EXPECT_EQ(t.live, 0) << "n=" << n;
  }
  EXPECT_TRUE(succeeded);
  EXPECT_GE(failures, 2); // A's array and B's table
}

TEST(BudgetScope, ATightMemoryBudgetRefusesOpenAsALimit) {
  StackWorld w(GRCORE_UNLIMITED, GRCORE_UNLIMITED, 0);
  ASSERT_EQ(grcore_context_set_memory_bytes(w.ctx, grcore_context_memory_in_use(w.ctx)),
      GRCORE_OK);
  GRCORE_BudgetScope out = {31};
  uint64_t refusals = grcore_context_memory_refusals(w.ctx);
  EXPECT_EQ(grcore_budget_scope_open(w.stack, 5, GRCORE_SCOPE_POLICY_UNWIND, &out),
      GRCORE_ERR_LIMIT);
  EXPECT_GT(grcore_context_memory_refusals(w.ctx), refusals);
  EXPECT_EQ(out.id, 31u);
  EXPECT_EQ(grcore_budget_scope_count(w.stack), 0u);
  EXPECT_EQ(grcore_context_fuel_scope_depth(w.ctx), 0u);
}

TEST(BudgetScope, ADestroyedContextWithScopesStillOpenFreesEverything) {
  TrackingAllocator t;
  {
    StackWorld w(GRCORE_UNLIMITED, GRCORE_UNLIMITED,
        GRCORE_DEFAULT_MEMORY_RESERVE, GRCORE_UNLIMITED, t.get());
    for (int i = 0; i < 20; i++) {
      open_scope(w.stack, 10);
    }
  }
  EXPECT_EQ(t.live, 0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

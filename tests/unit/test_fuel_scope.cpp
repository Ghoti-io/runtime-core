/**
 * @file
 *
 * Fuel scopes in B (AD-21): an exclusive budget under the inclusive ceiling,
 * the scoped unwind that closing a scope ends, and the verdicts when the
 * ceiling and a scope are exhausted together.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../../src/b/context_internal.h"

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

uint64_t used_of(GRCORE_Context * c, uint64_t id) {
  uint64_t v = 12345;
  EXPECT_EQ(grcore_context_fuel_scope_used(c, id, &v), GRCORE_OK);
  return v;
}
uint64_t remaining_of(GRCORE_Context * c, uint64_t id) {
  uint64_t v = 12345;
  EXPECT_EQ(grcore_context_fuel_scope_remaining(c, id, &v), GRCORE_OK);
  return v;
}

uint64_t open_scope(GRCORE_Context * c, uint64_t budget,
    GRCORE_ScopePolicy policy = GRCORE_SCOPE_POLICY_UNWIND) {
  uint64_t id = 0;
  EXPECT_EQ(grcore_context_fuel_scope_open(c, budget, policy, &id), GRCORE_OK);
  return id;
}

} // namespace

TEST(FuelScope, OpenGivesDistinctNonzeroIdsAndCloseMustBeInnermostFirst) {
  RunWorld w;
  EXPECT_EQ(grcore_context_fuel_scope_depth(w.ctx), 0u);
  EXPECT_EQ(grcore_context_fuel_scope_top(w.ctx), 0u);
  uint64_t a = open_scope(w.ctx, 100);
  uint64_t b = open_scope(w.ctx, 100);
  EXPECT_NE(a, 0u);
  EXPECT_NE(b, 0u);
  EXPECT_NE(a, b);
  EXPECT_EQ(grcore_context_fuel_scope_depth(w.ctx), 2u);
  EXPECT_EQ(grcore_context_fuel_scope_top(w.ctx), b);
  // Not the innermost: refused, nothing changes.
  EXPECT_EQ(grcore_context_fuel_scope_close(w.ctx, a), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_fuel_scope_depth(w.ctx), 2u);
  EXPECT_EQ(grcore_context_fuel_scope_close(w.ctx, b), GRCORE_OK);
  // A closed scope's id is stale for good, even when its depth is reused.
  EXPECT_EQ(grcore_context_fuel_scope_close(w.ctx, b), GRCORE_ERR_INVALID);
  uint64_t c = open_scope(w.ctx, 100);
  EXPECT_NE(c, b);
  EXPECT_EQ(grcore_context_fuel_scope_close(w.ctx, b), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_fuel_scope_close(w.ctx, c), GRCORE_OK);
  EXPECT_EQ(grcore_context_fuel_scope_close(w.ctx, a), GRCORE_OK);
  EXPECT_EQ(grcore_context_fuel_scope_depth(w.ctx), 0u);
}

TEST(FuelScope, MisuseIsInvalidAndWritesNothing) {
  RunWorld w;
  uint64_t id = 77;
  EXPECT_EQ(grcore_context_fuel_scope_open(nullptr, 1, GRCORE_SCOPE_POLICY_UNWIND, &id),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_fuel_scope_open(w.ctx, 1, GRCORE_SCOPE_POLICY_UNWIND, nullptr),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_fuel_scope_open(
                w.ctx, 1, static_cast<GRCORE_ScopePolicy>(9), &id),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(id, 77u);
  EXPECT_EQ(grcore_context_fuel_scope_depth(w.ctx), 0u);
  EXPECT_EQ(grcore_context_fuel_scope_close(w.ctx, 0), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_fuel_scope_close(nullptr, 1), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_fuel_scope_close(w.ctx, 1), GRCORE_ERR_INVALID);
  uint64_t v = 5;
  EXPECT_EQ(grcore_context_fuel_scope_used(w.ctx, 1, &v), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_fuel_scope_remaining(w.ctx, 1, &v), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_fuel_scope_set_budget(w.ctx, 1, 5), GRCORE_ERR_INVALID);
  EXPECT_EQ(v, 5u);
  uint64_t a = open_scope(w.ctx, 10);
  EXPECT_EQ(grcore_context_fuel_scope_used(w.ctx, a, nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_fuel_scope_depth(nullptr), 0u);
  EXPECT_EQ(grcore_context_fuel_scope_top(nullptr), 0u);
  EXPECT_EQ(grcore_context_fuel_scope_unwinding(nullptr), 0u);
}

TEST(FuelScope, ANonOwnerCannotOpenOrCloseOne) {
  RunWorld w;
  uint64_t a = open_scope(w.ctx, 10);
  GRCORE_Result open = GRCORE_OK, close = GRCORE_OK;
  uint64_t id = 0;
  std::thread([&] {
    open = grcore_context_fuel_scope_open(
        w.ctx, 10, GRCORE_SCOPE_POLICY_UNWIND, &id);
    close = grcore_context_fuel_scope_close(w.ctx, a);
  }).join();
  EXPECT_EQ(open, GRCORE_ERR_INVALID);
  EXPECT_EQ(close, GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_fuel_scope_depth(w.ctx), 1u);
}

TEST(FuelScope, AChildsChargeCountsTowardTheCeilingAndNotTheParent) {
  RunWorld w(1000);
  uint64_t parent = open_scope(w.ctx, 100);
  grcore_context_charge_fuel(w.ctx, 40); // the parent's own work
  uint64_t child = open_scope(w.ctx, 100);
  grcore_context_charge_fuel(w.ctx, 60); // the child's
  EXPECT_EQ(used_of(w.ctx, child), 60u);
  EXPECT_EQ(remaining_of(w.ctx, child), 40u);
  // The parent's clock stopped while the child ran.
  EXPECT_EQ(used_of(w.ctx, parent), 40u);
  EXPECT_EQ(remaining_of(w.ctx, parent), 60u);
  // Both counted against the one ceiling.
  EXPECT_EQ(grcore_context_fuel_used(w.ctx), 100u);
  EXPECT_EQ(grcore_context_fuel_remaining(w.ctx), 900u);
  ASSERT_EQ(grcore_context_fuel_scope_close(w.ctx, child), GRCORE_OK);
  grcore_context_charge_fuel(w.ctx, 10);
  EXPECT_EQ(used_of(w.ctx, parent), 50u);
  EXPECT_EQ(grcore_context_fuel_used(w.ctx), 110u);
  // Charges with no scope open count only against the ceiling.
  ASSERT_EQ(grcore_context_fuel_scope_close(w.ctx, parent), GRCORE_OK);
  grcore_context_charge_fuel(w.ctx, 5);
  EXPECT_EQ(grcore_context_fuel_used(w.ctx), 115u);
}

TEST(FuelScope, ChargeReportsExhaustionOfTheScopeWhenTheCeilingIsNot) {
  RunWorld w(1000);
  uint64_t a = open_scope(w.ctx, 10);
  EXPECT_FALSE(grcore_context_charge_fuel(w.ctx, 10)); // exactly the budget
  EXPECT_TRUE(grcore_context_charge_fuel(w.ctx, 1));   // one past it
  EXPECT_EQ(remaining_of(w.ctx, a), 0u);
  EXPECT_TRUE(grcore_context_request_pending(w.ctx, GRCORE_REQUEST_FUEL));
  // Closing the exhausted scope clears the level the scope had raised.
  ASSERT_EQ(grcore_context_fuel_scope_close(w.ctx, a), GRCORE_OK);
  EXPECT_FALSE(grcore_context_request_pending(w.ctx, GRCORE_REQUEST_FUEL));
}

TEST(FuelScope, AnUnlimitedScopeIsNeverExhausted) {
  RunWorld w(GRCORE_UNLIMITED);
  uint64_t a = open_scope(w.ctx, GRCORE_UNLIMITED);
  EXPECT_FALSE(grcore_context_charge_fuel(w.ctx, UINT64_MAX / 2));
  EXPECT_EQ(remaining_of(w.ctx, a), GRCORE_UNLIMITED);
  EXPECT_FALSE(grcore_context_request_pending(w.ctx, GRCORE_REQUEST_FUEL));
}

TEST(FuelScope, ExhaustedScopeUnwindsThroughTheFuelKeyAloneAndClosingItLetsRunFinish) {
  RunWorld w(1000);
  uint64_t id = 0;
  GRCORE_Verdict verdict = GRCORE_VERDICT_CONTINUE;
  GRCORE_Result reason = GRCORE_OK, after_close = GRCORE_ERR_INTERNAL;
  std::vector<const GRCORE_Key *> keys;
  uint64_t unwinding = 0, unwinding_after = 99;
  Fn fn{[&](GRCORE_Context * c) {
    id = open_scope(c, 10);
    grcore_context_charge_fuel(c, 11);
    verdict = GRCORE_POLL(c);
    reason = grcore_context_unwind_result(c);
    keys = pause_keys(c);
    unwinding = grcore_context_fuel_scope_unwinding(c);
    EXPECT_EQ(grcore_context_fuel_scope_close(c, id), GRCORE_OK);
    unwinding_after = grcore_context_fuel_scope_unwinding(c);
    after_close = grcore_context_unwind_result(c);
    EXPECT_EQ(grcore_context_pause_key_count(c), 0u);
    EXPECT_EQ(grcore_context_pause_location(c).file, nullptr);
    return GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome = GRCORE_OUTCOME_PAUSED;
  EXPECT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  EXPECT_EQ(verdict, GRCORE_VERDICT_UNWIND);
  EXPECT_EQ(reason, GRCORE_ERR_LIMIT);
  EXPECT_EQ(keys, std::vector<const GRCORE_Key *>{core(GRCORE_REQUEST_FUEL)});
  EXPECT_EQ(unwinding, id);
  EXPECT_EQ(unwinding_after, 0u);
  EXPECT_EQ(after_close, GRCORE_OK);
  EXPECT_EQ(grcore_context_fuel_used(w.ctx), 11u); // the ceiling counted it
  EXPECT_EQ(grcore_context_fuel_scope_depth(w.ctx), 0u);
  EXPECT_EQ(grcore_context_state(w.ctx), GRCORE_CONTEXT_PARKED);
  // After the run too, the cleared unwind leaves no stale reason behind.
  EXPECT_EQ(grcore_context_pause_key_count(w.ctx), 0u);
  EXPECT_EQ(grcore_context_pause_location(w.ctx).file, nullptr);
}

TEST(FuelScope, ARunThatFinishesWithAScopedUnwindStillOpenIsAnEntryLie) {
  RunWorld w(1000);
  Fn fn{[&](GRCORE_Context * c) {
    open_scope(c, 10);
    grcore_context_charge_fuel(c, 11);
    EXPECT_EQ(GRCORE_POLL(c), GRCORE_VERDICT_UNWIND);
    return GRCORE_STEP_FINISHED; // never closed the scope
  }};
  GRCORE_Outcome outcome = GRCORE_OUTCOME_PAUSED;
  EXPECT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_ERR_INTERNAL);
}

TEST(FuelScope, AScopedUnwindTheEntryReportsAsUnwoundEndsTheRunWithALimitError) {
  RunWorld w(1000);
  Fn fn{[&](GRCORE_Context * c) {
    open_scope(c, 10);
    grcore_context_charge_fuel(c, 11);
    EXPECT_EQ(GRCORE_POLL(c), GRCORE_VERDICT_UNWIND);
    return GRCORE_STEP_UNWOUND; // the engine chose to end the run
  }};
  GRCORE_Outcome outcome = GRCORE_OUTCOME_PAUSED;
  EXPECT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_ERR_LIMIT);
}

TEST(FuelScope, SiblingScopesEachKeepTheirFullBudget) {
  RunWorld w(1000);
  Fn fn{[&](GRCORE_Context * c) {
    uint64_t first = open_scope(c, 10);
    grcore_context_charge_fuel(c, 11);
    EXPECT_EQ(GRCORE_POLL(c), GRCORE_VERDICT_UNWIND);
    EXPECT_EQ(grcore_context_fuel_scope_close(c, first), GRCORE_OK);
    uint64_t second = open_scope(c, 10);
    EXPECT_EQ(remaining_of(c, second), 10u);
    grcore_context_charge_fuel(c, 10);
    EXPECT_EQ(GRCORE_POLL(c), GRCORE_VERDICT_CONTINUE); // exactly the budget
    EXPECT_EQ(grcore_context_fuel_scope_close(c, second), GRCORE_OK);
    return GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  EXPECT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  EXPECT_EQ(grcore_context_fuel_used(w.ctx), 21u);
}

TEST(FuelScope, TheCeilingBeatsAScopeAndRaisingItLetsTheScopeUnwindNext) {
  RunWorld w(20);
  uint64_t id = 0;
  int stage = 0;
  GRCORE_Verdict second = GRCORE_VERDICT_CONTINUE;
  Fn fn{[&](GRCORE_Context * c) {
    if (stage == 0) {
      id = open_scope(c, 10);
      grcore_context_charge_fuel(c, 30); // both exhausted
      stage = 1;
      if (GRCORE_POLL(c) == GRCORE_VERDICT_PAUSE) {
        return GRCORE_STEP_PAUSED;
      }
      ADD_FAILURE() << "both exhausted must pause";
      return GRCORE_STEP_FINISHED;
    }
    second = GRCORE_POLL(c);
    EXPECT_EQ(grcore_context_fuel_scope_close(c, id), GRCORE_OK);
    return GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  EXPECT_EQ(pause_keys(w.ctx), std::vector<const GRCORE_Key *>{core(GRCORE_REQUEST_FUEL)});
  EXPECT_EQ(grcore_context_fuel_scope_unwinding(w.ctx), 0u); // a pause, not a scoped unwind
  EXPECT_EQ(grcore_context_fuel_scope_depth(w.ctx), 1u);
  // The host raises the ceiling; the scope is still over, so it unwinds.
  ASSERT_EQ(grcore_context_set_fuel(w.ctx, 1000), GRCORE_OK);
  ASSERT_EQ(grcore_resume(w.ctx, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  EXPECT_EQ(second, GRCORE_VERDICT_UNWIND);
}

TEST(FuelScope, ThePausePolicyPausesAtTheScopeAndTheHostCanRaiseItsBudget) {
  RunWorld w(1000);
  uint64_t id = 0;
  int stage = 0;
  GRCORE_Verdict after = GRCORE_VERDICT_UNWIND;
  Fn fn{[&](GRCORE_Context * c) {
    if (stage == 0) {
      id = open_scope(c, 10, GRCORE_SCOPE_POLICY_PAUSE);
      grcore_context_charge_fuel(c, 11);
      stage = 1;
      return GRCORE_POLL(c) == GRCORE_VERDICT_PAUSE ? GRCORE_STEP_PAUSED
                                                   : GRCORE_STEP_FINISHED;
    }
    after = GRCORE_POLL(c);
    EXPECT_EQ(grcore_context_fuel_scope_close(c, id), GRCORE_OK);
    return GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  EXPECT_EQ(pause_keys(w.ctx), std::vector<const GRCORE_Key *>{core(GRCORE_REQUEST_FUEL)});
  EXPECT_EQ(grcore_context_fuel_scope_unwinding(w.ctx), 0u);
  // The use stays, so raising the budget by k buys exactly k more.
  ASSERT_EQ(grcore_context_fuel_scope_set_budget(w.ctx, id, 100), GRCORE_OK);
  EXPECT_EQ(used_of(w.ctx, id), 11u);
  EXPECT_EQ(remaining_of(w.ctx, id), 89u);
  ASSERT_EQ(grcore_resume(w.ctx, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  EXPECT_EQ(after, GRCORE_VERDICT_CONTINUE);
}

TEST(FuelScope, ASetBudgetIsRefusedWhileRunningAndForAnUnknownScope) {
  RunWorld w(1000);
  Fn fn{[&](GRCORE_Context * c) {
    uint64_t id = open_scope(c, 10);
    EXPECT_EQ(grcore_context_fuel_scope_set_budget(c, id, 50), GRCORE_ERR_INVALID);
    EXPECT_EQ(remaining_of(c, id), 10u);
    EXPECT_EQ(grcore_context_fuel_scope_close(c, id), GRCORE_OK);
    return GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  uint64_t id = open_scope(w.ctx, 10);
  EXPECT_EQ(grcore_context_fuel_scope_set_budget(w.ctx, id + 1, 50), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_fuel_scope_set_budget(w.ctx, id, 50), GRCORE_OK);
  EXPECT_EQ(remaining_of(w.ctx, id), 50u);
}

TEST(FuelScope, TerminateAndAnExhaustedScopeUnwindTogetherSoItIsNotScoped) {
  RunWorld w(1000);
  uint64_t id = 0;
  std::vector<const GRCORE_Key *> keys;
  uint64_t unwinding = 99;
  GRCORE_Result after_close = GRCORE_OK;
  Fn fn{[&](GRCORE_Context * c) {
    id = open_scope(c, 10);
    grcore_context_charge_fuel(c, 11);
    EXPECT_EQ(grcore_context_terminate(c), GRCORE_OK);
    EXPECT_EQ(GRCORE_POLL(c), GRCORE_VERDICT_UNWIND);
    keys = pause_keys(c);
    unwinding = grcore_context_fuel_scope_unwinding(c);
    EXPECT_EQ(grcore_context_fuel_scope_close(c, id), GRCORE_OK);
    after_close = grcore_context_unwind_result(c);
    // Terminate is still pending: the next poll unwinds again.
    EXPECT_EQ(GRCORE_POLL(c), GRCORE_VERDICT_UNWIND);
    return GRCORE_STEP_UNWOUND;
  }};
  GRCORE_Outcome outcome;
  EXPECT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_ERR_LIMIT);
  EXPECT_EQ(keys, (std::vector<const GRCORE_Key *>{
                      core(GRCORE_REQUEST_TERMINATE), core(GRCORE_REQUEST_FUEL)}));
  EXPECT_EQ(unwinding, 0u);
  EXPECT_EQ(after_close, GRCORE_ERR_LIMIT); // closing did not clear a terminal unwind
}

TEST(FuelScope, ASimultaneousPauseRequestLosesToTheScopedUnwindSoTheKeyIsFuelAlone) {
  RunWorld w(1000);
  GRCORE_Port * port;
  ASSERT_EQ(grcore_context_port(w.ctx, &port), GRCORE_OK);
  ASSERT_EQ(grcore_port_post(port, GRCORE_REQUEST_TIME), GRCORE_OK);
  std::vector<const GRCORE_Key *> keys;
  uint64_t id = 0, unwinding = 0;
  Fn fn{[&](GRCORE_Context * c) {
    id = open_scope(c, 10);
    grcore_context_charge_fuel(c, 11);
    EXPECT_EQ(GRCORE_POLL(c), GRCORE_VERDICT_UNWIND);
    keys = pause_keys(c);
    unwinding = grcore_context_fuel_scope_unwinding(c);
    EXPECT_EQ(grcore_context_fuel_scope_close(c, id), GRCORE_OK);
    return GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  EXPECT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_EQ(keys, std::vector<const GRCORE_Key *>{core(GRCORE_REQUEST_FUEL)});
  EXPECT_EQ(unwinding, id);
  grcore_port_release(port);
}

TEST(FuelScope, TheRuntimePollReportsAScopeLimitAndRefusesAPausePolicyScope) {
  RunWorld w(1000);
  Fn fn{[&](GRCORE_Context * c) {
    uint64_t a = open_scope(c, 10, GRCORE_SCOPE_POLICY_UNWIND);
    EXPECT_EQ(grcore_runtime_poll(c, 5, GRCORE_HERE), GRCORE_OK);
    EXPECT_EQ(grcore_runtime_poll(c, 6, GRCORE_HERE), GRCORE_ERR_LIMIT);
    EXPECT_EQ(grcore_context_fuel_scope_unwinding(c), a); // a scoped unwind
    EXPECT_EQ(grcore_context_fuel_scope_close(c, a), GRCORE_OK);
    // A pause-policy scope cannot pause a native: a pause is promoted to a
    // terminal limit unwind, which closing the scope does not end.
    uint64_t b = open_scope(c, 10, GRCORE_SCOPE_POLICY_PAUSE);
    EXPECT_EQ(grcore_runtime_poll(c, 11, GRCORE_HERE), GRCORE_ERR_LIMIT);
    EXPECT_EQ(grcore_context_fuel_scope_unwinding(c), 0u);
    EXPECT_EQ(grcore_context_fuel_scope_close(c, b), GRCORE_OK);
    return GRCORE_STEP_UNWOUND;
  }};
  GRCORE_Outcome outcome;
  EXPECT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_ERR_LIMIT);
}

TEST(FuelScope, PageSidebarAndNavPaneNavUnwindsAtItsBoundaryAndTheOthersFinish) {
  RunWorld w;
  uint64_t page_used = 0, sidebar_used = 0, nav_used = 0, page_remaining = 0,
           sidebar_remaining = 0;
  int nav_steps = 0;
  Fn fn{[&](GRCORE_Context * c) {
    uint64_t page = open_scope(c, 1000);
    grcore_context_charge_fuel(c, 20);
    uint64_t sidebar = open_scope(c, 500);
    grcore_context_charge_fuel(c, 30);
    sidebar_used = used_of(c, sidebar);
    sidebar_remaining = remaining_of(c, sidebar);
    EXPECT_EQ(GRCORE_POLL(c), GRCORE_VERDICT_CONTINUE);
    EXPECT_EQ(grcore_context_fuel_scope_close(c, sidebar), GRCORE_OK);
    uint64_t nav = open_scope(c, 100);
    for (;;) { // the nav pane loops forever
      grcore_context_charge_fuel(c, 7);
      nav_steps++;
      if (GRCORE_POLL(c) == GRCORE_VERDICT_UNWIND) {
        break;
      }
      if (nav_steps > 1000) {
        ADD_FAILURE() << "the nav pane was never stopped";
        break;
      }
    }
    EXPECT_EQ(grcore_context_fuel_scope_unwinding(c), nav);
    nav_used = used_of(c, nav);
    EXPECT_EQ(grcore_context_fuel_scope_close(c, nav), GRCORE_OK);
    grcore_context_charge_fuel(c, 20);
    EXPECT_EQ(GRCORE_POLL(c), GRCORE_VERDICT_CONTINUE);
    page_used = used_of(c, page);
    page_remaining = remaining_of(c, page);
    EXPECT_EQ(grcore_context_fuel_scope_close(c, page), GRCORE_OK);
    return GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  EXPECT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  EXPECT_EQ(nav_steps, 15); // 14 * 7 = 98 is within 100, 15 * 7 = 105 is not
  EXPECT_EQ(nav_used, 105u);
  EXPECT_EQ(sidebar_used, 30u); // only its own work
  EXPECT_EQ(sidebar_remaining, 470u);
  EXPECT_EQ(page_used, 40u);    // the nav pane's 105 is not the page's
  EXPECT_EQ(page_remaining, 960u);
  EXPECT_EQ(grcore_context_fuel_used(w.ctx), 20u + 30u + 105u + 20u);
}

TEST(FuelScope, TenThousandChildCallsStopAtTheCeilingWithABoundedNumberOpened) {
  RunWorld w(100000);
  int opened = 0;
  Fn fn{[&](GRCORE_Context * c) {
    for (int i = 0; i < 10000; i++) {
      uint64_t id = open_scope(c, 1000);
      opened++;
      grcore_context_charge_fuel(c, 20);
      if (GRCORE_POLL(c) == GRCORE_VERDICT_PAUSE) {
        return GRCORE_STEP_PAUSED; // the child stays open, as at any pause
      }
      EXPECT_EQ(grcore_context_fuel_scope_close(c, id), GRCORE_OK);
    }
    return GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  EXPECT_EQ(pause_keys(w.ctx), std::vector<const GRCORE_Key *>{core(GRCORE_REQUEST_FUEL)});
  EXPECT_EQ(opened, 5001); // 5000 * 20 = 100000 is within the ceiling
  EXPECT_EQ(grcore_context_fuel_scope_depth(w.ctx), 1u);
}

TEST(FuelScope, MoreScopesThanTheFirstCapacityKeepEachOnesUse) {
  RunWorld w;
  std::vector<uint64_t> ids;
  for (int i = 0; i < 40; i++) {
    ids.push_back(open_scope(w.ctx, 1000));
    grcore_context_charge_fuel(w.ctx, static_cast<uint64_t>(i + 1));
  }
  EXPECT_EQ(grcore_context_fuel_scope_depth(w.ctx), 40u);
  for (int i = 0; i < 40; i++) {
    EXPECT_EQ(used_of(w.ctx, ids[i]), static_cast<uint64_t>(i + 1)) << i;
    if (i > 0) {
      EXPECT_GT(ids[i], ids[i - 1]);
    }
  }
  for (int i = 39; i >= 0; i--) {
    EXPECT_EQ(grcore_context_fuel_scope_close(w.ctx, ids[i]), GRCORE_OK);
  }
}

TEST(FuelScope, EveryAllocationFailureOfOpenIsOomAndLeavesNothingBehind) {
  bool succeeded = false;
  int failures = 0;
  for (long n = 1; n < 10 && !succeeded; n++) {
    TrackingAllocator t;
    {
      RunWorld w(GRCORE_UNLIMITED, GRCORE_UNLIMITED,
          GRCORE_DEFAULT_MEMORY_RESERVE, GRCORE_UNLIMITED, GRCORE_UNLIMITED,
          t.get());
      uint64_t id = 31;
      t.fail_at = t.calls + n;
      GRCORE_Result r = grcore_context_fuel_scope_open(
          w.ctx, 5, GRCORE_SCOPE_POLICY_UNWIND, &id);
      t.fail_at = 0;
      if (r == GRCORE_OK) {
        succeeded = true;
        EXPECT_EQ(grcore_context_fuel_scope_depth(w.ctx), 1u);
      } else {
        failures++;
        EXPECT_EQ(r, GRCORE_ERR_OOM) << "n=" << n;
        EXPECT_EQ(id, 31u) << "n=" << n;
        EXPECT_EQ(grcore_context_fuel_scope_depth(w.ctx), 0u) << "n=" << n;
        EXPECT_EQ(grcore_context_memory_blocks(w.ctx), 0u) << "n=" << n;
      }
    }
    EXPECT_EQ(t.live, 0) << "n=" << n;
  }
  EXPECT_TRUE(succeeded);
  EXPECT_GE(failures, 1);
}

TEST(FuelScope, AnOpenTheMemoryBudgetRefusesIsALimit) {
  RunWorld w(GRCORE_UNLIMITED, 0, 0);
  uint64_t id = 31;
  uint64_t refusals = grcore_context_memory_refusals(w.ctx);
  EXPECT_EQ(grcore_context_fuel_scope_open(
                w.ctx, 5, GRCORE_SCOPE_POLICY_UNWIND, &id),
      GRCORE_ERR_LIMIT);
  EXPECT_GT(grcore_context_memory_refusals(w.ctx), refusals);
  EXPECT_EQ(id, 31u);
  EXPECT_EQ(grcore_context_fuel_scope_depth(w.ctx), 0u);
  ASSERT_EQ(grcore_context_set_memory_bytes(w.ctx, GRCORE_UNLIMITED), GRCORE_OK);
  EXPECT_EQ(grcore_context_fuel_scope_open(
                w.ctx, 5, GRCORE_SCOPE_POLICY_UNWIND, &id),
      GRCORE_OK);
}

TEST(FuelScope, AContextDestroyedWithScopesStillOpenFreesEverything) {
  TrackingAllocator t;
  {
    RunWorld w(GRCORE_UNLIMITED, GRCORE_UNLIMITED,
        GRCORE_DEFAULT_MEMORY_RESERVE, GRCORE_UNLIMITED, GRCORE_UNLIMITED,
        t.get());
    for (int i = 0; i < 20; i++) {
      open_scope(w.ctx, 10);
    }
  }
  EXPECT_EQ(t.live, 0);
}

TEST(FuelScope, ANewRunStartsWithoutAStaleScopedUnwind) {
  RunWorld w(1000);
  Fn first{[&](GRCORE_Context * c) {
    open_scope(c, 1);
    grcore_context_charge_fuel(c, 5);
    EXPECT_EQ(GRCORE_POLL(c), GRCORE_VERDICT_UNWIND);
    EXPECT_NE(grcore_context_fuel_scope_unwinding(c), 0u);
    return GRCORE_STEP_UNWOUND; // abandons the scope without closing it
  }};
  GRCORE_Outcome outcome;
  EXPECT_EQ(grcore_run(w.ctx, fn_entry, &first, &outcome), GRCORE_ERR_LIMIT);
  Fn second{[&](GRCORE_Context * c) {
    EXPECT_EQ(grcore_context_fuel_scope_unwinding(c), 0u);
    return GRCORE_STEP_FINISHED;
  }};
  EXPECT_EQ(grcore_run(w.ctx, fn_entry, &second, &outcome), GRCORE_OK);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

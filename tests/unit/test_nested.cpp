/**
 * @file
 *
 * The nesting count in B (AD-5): a pause verdict reached inside a nested
 * activation becomes a limit unwind, and a terminate request stays pending
 * until the outermost `run` returns.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

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

} // namespace

TEST(Nested, EnterLeaveAndDepthAreCountedAndLeaveAtZeroIsRefused) {
  RunWorld w;
  EXPECT_EQ(grcore_context_nested_depth(w.ctx), 0u);
  EXPECT_EQ(grcore_context_nested_leave(w.ctx), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_nested_enter(w.ctx), GRCORE_OK);
  EXPECT_EQ(grcore_context_nested_enter(w.ctx), GRCORE_OK);
  EXPECT_EQ(grcore_context_nested_depth(w.ctx), 2u);
  EXPECT_EQ(grcore_context_nested_leave(w.ctx), GRCORE_OK);
  EXPECT_EQ(grcore_context_nested_leave(w.ctx), GRCORE_OK);
  EXPECT_EQ(grcore_context_nested_leave(w.ctx), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_nested_depth(w.ctx), 0u);
  EXPECT_EQ(grcore_context_nested_enter(nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_nested_leave(nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_nested_depth(nullptr), 0u);
}

TEST(Nested, ANonOwnerCannotChangeTheCount) {
  RunWorld w;
  ASSERT_EQ(grcore_context_nested_enter(w.ctx), GRCORE_OK);
  GRCORE_Result enter = GRCORE_OK, leave = GRCORE_OK;
  std::thread([&] {
    enter = grcore_context_nested_enter(w.ctx);
    leave = grcore_context_nested_leave(w.ctx);
  }).join();
  EXPECT_EQ(enter, GRCORE_ERR_INVALID);
  EXPECT_EQ(leave, GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_nested_depth(w.ctx), 1u);
}

TEST(Nested, AFuelPauseInsideANestedActivationIsALimitUnwindAndTheOuterPollPausesAfterLeave) {
  RunWorld w(10);
  GRCORE_Verdict inner = GRCORE_VERDICT_CONTINUE, outer = GRCORE_VERDICT_CONTINUE;
  GRCORE_Result reason = GRCORE_OK;
  std::vector<const GRCORE_Key *> keys;
  Fn fn{[&](GRCORE_Context * c) {
    EXPECT_EQ(grcore_context_nested_enter(c), GRCORE_OK);
    grcore_context_charge_fuel(c, 11);
    inner = GRCORE_POLL(c);
    reason = grcore_context_unwind_result(c);
    keys = pause_keys(c);
    EXPECT_EQ(grcore_context_nested_leave(c), GRCORE_OK);
    // Back in the outer activation, the same exhausted fuel pauses as usual.
    outer = GRCORE_POLL(c);
    return outer == GRCORE_VERDICT_PAUSE ? GRCORE_STEP_PAUSED
                                         : GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_EQ(inner, GRCORE_VERDICT_UNWIND);
  EXPECT_EQ(reason, GRCORE_ERR_LIMIT);
  // The keys that asked for the pause stay readable as the reason.
  EXPECT_EQ(keys, std::vector<const GRCORE_Key *>{grcore_core_key(GRCORE_REQUEST_FUEL)});
  EXPECT_EQ(outer, GRCORE_VERDICT_PAUSE);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
}

TEST(Nested, ATimeRequestIsRefusedAPauseInsideANestedActivationToo) {
  RunWorld w;
  GRCORE_Port * port;
  ASSERT_EQ(grcore_context_port(w.ctx, &port), GRCORE_OK);
  ASSERT_EQ(grcore_port_post(port, GRCORE_REQUEST_TIME), GRCORE_OK);
  GRCORE_Verdict inner = GRCORE_VERDICT_CONTINUE;
  Fn fn{[&](GRCORE_Context * c) {
    grcore_context_nested_enter(c);
    inner = GRCORE_POLL(c);
    grcore_context_nested_leave(c);
    return GRCORE_STEP_UNWOUND;
  }};
  GRCORE_Outcome outcome;
  EXPECT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_ERR_LIMIT);
  EXPECT_EQ(inner, GRCORE_VERDICT_UNWIND);
  grcore_port_release(port);
}

TEST(Nested, TerminateInANestedActivationUnwindsThereAndAgainAtTheOuterPoll) {
  RunWorld w;
  GRCORE_Verdict inner = GRCORE_VERDICT_CONTINUE, outer = GRCORE_VERDICT_CONTINUE;
  Fn fn{[&](GRCORE_Context * c) {
    EXPECT_EQ(grcore_context_terminate(c), GRCORE_OK);
    grcore_context_nested_enter(c);
    inner = GRCORE_POLL(c);
    grcore_context_nested_leave(c);
    EXPECT_TRUE(grcore_context_request_pending(c, GRCORE_REQUEST_TERMINATE));
    outer = GRCORE_POLL(c);
    return GRCORE_STEP_UNWOUND;
  }};
  GRCORE_Outcome outcome;
  EXPECT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_ERR_LIMIT);
  EXPECT_EQ(inner, GRCORE_VERDICT_UNWIND);
  EXPECT_EQ(outer, GRCORE_VERDICT_UNWIND);
  // Cleared only when the outermost run returned.
  EXPECT_FALSE(grcore_context_request_pending(w.ctx, GRCORE_REQUEST_TERMINATE));
}

TEST(Nested, WithNoNestedActivationAPauseStillPauses) {
  RunWorld w(10);
  Fn fn{[&](GRCORE_Context * c) {
    grcore_context_charge_fuel(c, 11);
    return GRCORE_POLL(c) == GRCORE_VERDICT_PAUSE ? GRCORE_STEP_PAUSED
                                                  : GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
}

TEST(Nested, EveryLevelOfNestingBlocksAPauseUntilTheLastIsLeft) {
  RunWorld w(10);
  std::vector<GRCORE_Verdict> seen;
  Fn fn{[&](GRCORE_Context * c) {
    grcore_context_charge_fuel(c, 11);
    grcore_context_nested_enter(c);
    grcore_context_nested_enter(c);
    seen.push_back(GRCORE_POLL(c));
    grcore_context_nested_leave(c);
    seen.push_back(GRCORE_POLL(c));
    grcore_context_nested_leave(c);
    seen.push_back(GRCORE_POLL(c));
    return GRCORE_STEP_PAUSED;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_EQ(seen, (std::vector<GRCORE_Verdict>{GRCORE_VERDICT_UNWIND,
                      GRCORE_VERDICT_UNWIND, GRCORE_VERDICT_PAUSE}));
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

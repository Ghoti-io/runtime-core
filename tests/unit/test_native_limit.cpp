/**
 * @file
 *
 * The native stack budget in bytes and the limit word compiled code compares
 * its stack pointer with (AD-28): the option, the word, and where it is set.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstring>

namespace {

/* A group and a context made with a native stack budget. */
struct LimitWorld {
  GRCORE_Group * group = nullptr;
  GRCORE_Context * ctx = nullptr;
  explicit LimitWorld(uint64_t bytes, uint64_t fuel = GRCORE_UNLIMITED) {
    GRCORE_Options * o = nullptr;
    EXPECT_EQ(grcore_options_create(nullptr, &o), GRCORE_OK);
    EXPECT_EQ(grcore_options_set_native_stack_bytes(o, bytes), GRCORE_OK);
    EXPECT_EQ(grcore_options_set_fuel(o, fuel), GRCORE_OK);
    EXPECT_EQ(grcore_group_create(nullptr, nullptr, &group), GRCORE_OK);
    EXPECT_EQ(grcore_context_create(group, o, &ctx), GRCORE_OK);
    grcore_options_destroy(o);
  }
  ~LimitWorld() {
    EXPECT_EQ(grcore_context_destroy(ctx), GRCORE_OK);
    EXPECT_EQ(grcore_group_destroy(group), GRCORE_OK);
  }
};

/* What compiled code reads: the word at the layout's offset. */
uintptr_t limit_word(GRCORE_Context * c) {
  uintptr_t w;
  std::memcpy(&w,
      reinterpret_cast<const unsigned char *>(c) +
          grcore_jit_layout()->native_limit_offset,
      sizeof w);
  return w;
}

struct LimitProbe {
  uintptr_t limit_in_entry = 0;
  uintptr_t local_in_entry = 0;
  int calls = 0;
  bool pause_first = false;
};

GRCORE_Step probe_entry(GRCORE_Context * c, void * state) {
  auto * p = static_cast<LimitProbe *>(state);
  volatile char here = 0;
  p->local_in_entry = reinterpret_cast<uintptr_t>(&here);
  p->limit_in_entry = limit_word(c);
  p->calls++;
  if (p->pause_first && p->calls == 1) {
    grcore_context_charge_fuel(c, 1);
    GRCORE_Verdict v = grcore_stack_poll(c, 1, 1);
    return v == GRCORE_VERDICT_PAUSE ? GRCORE_STEP_PAUSED : GRCORE_STEP_FINISHED;
  }
  return GRCORE_STEP_FINISHED;
}

} // namespace

TEST(NativeStackBytes, TheOptionDefaultsToUnlimitedAndRoundTrips) {
  GRCORE_Options * o = nullptr;
  ASSERT_EQ(grcore_options_create(nullptr, &o), GRCORE_OK);
  EXPECT_EQ(grcore_options_get_native_stack_bytes(o), GRCORE_UNLIMITED);
  EXPECT_EQ(grcore_options_set_native_stack_bytes(o, 65536), GRCORE_OK);
  EXPECT_EQ(grcore_options_get_native_stack_bytes(o), 65536u);
  grcore_options_destroy(o);
  EXPECT_EQ(grcore_options_set_native_stack_bytes(nullptr, 1), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_options_get_native_stack_bytes(nullptr), GRCORE_UNLIMITED);
}

TEST(NativeStackBytes, ALimitIsTheStackPointerLessTheBudgetAndZeroWhenThereIsNoBudget) {
  {
    LimitWorld w(4096);
    EXPECT_EQ(grcore_context_native_stack_bytes(w.ctx), 4096u);
    EXPECT_EQ(grcore_context_native_limit(w.ctx), 0u) << "nothing has set it yet";
    ASSERT_EQ(grcore_context_native_limit_set(w.ctx, 100000), GRCORE_OK);
    EXPECT_EQ(grcore_context_native_limit(w.ctx), 100000u - 4096u);
    EXPECT_EQ(limit_word(w.ctx), 100000u - 4096u) << "the word compiled code reads";
    // A budget larger than the stack pointer cannot be below zero.
    ASSERT_EQ(grcore_context_native_limit_set(w.ctx, 4000), GRCORE_OK);
    EXPECT_EQ(grcore_context_native_limit(w.ctx), 0u);
  }
  {
    LimitWorld w(GRCORE_UNLIMITED);
    ASSERT_EQ(grcore_context_native_limit_set(w.ctx, 100000), GRCORE_OK);
    EXPECT_EQ(grcore_context_native_limit(w.ctx), 0u);
  }
  EXPECT_EQ(grcore_context_native_limit_set(nullptr, 1), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_native_limit(nullptr), 0u);
  EXPECT_EQ(grcore_context_native_stack_bytes(nullptr), GRCORE_UNLIMITED);
}

TEST(NativeStackBytes, RunSetsTheLimitFromItsOwnStackAndClearsItWhenItEnds) {
  LimitWorld w(1 << 20);
  LimitProbe p;
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, probe_entry, &p, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  ASSERT_NE(p.limit_in_entry, 0u);
  // The limit is run's stack pointer less the budget; the entry's frame is near
  // run's (a sanitizer's frames are larger, so which is lower is not fixed).
  uintptr_t expect = p.local_in_entry - (1u << 20);
  uintptr_t gap = p.limit_in_entry > expect ? p.limit_in_entry - expect : expect - p.limit_in_entry;
  EXPECT_LT(gap, 16384u);
  EXPECT_EQ(grcore_context_native_limit(w.ctx), 0u)
      << "it names a stack nobody runs on once run returns";
}

TEST(NativeStackBytes, ResumeSetsItAgainFromWhereTheResumeIs) {
  LimitWorld w(1 << 20, /*fuel=*/0);
  LimitProbe p;
  p.pause_first = true;
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, probe_entry, &p, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  EXPECT_EQ(grcore_context_native_limit(w.ctx), 0u);
  uintptr_t first = p.limit_in_entry;
  p.limit_in_entry = 0;
  ASSERT_EQ(grcore_context_set_fuel(w.ctx, GRCORE_UNLIMITED), GRCORE_OK);
  ASSERT_EQ(grcore_resume(w.ctx, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  EXPECT_NE(p.limit_in_entry, 0u);
  EXPECT_NEAR(static_cast<double>(p.limit_in_entry), static_cast<double>(first), 16384.0);
}

TEST(NativeStackBytes, ARunWithNoBudgetLeavesTheLimitZero) {
  LimitWorld w(GRCORE_UNLIMITED);
  LimitProbe p;
  p.limit_in_entry = 1;
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, probe_entry, &p, &outcome), GRCORE_OK);
  EXPECT_EQ(p.limit_in_entry, 0u);
}

TEST(NativeStackBytes, AReentryFindsTheRunsLimitAndOnlySetsOneWhereThereIsNone) {
  LimitWorld w(1 << 20);
  GRCORE_EngineId engine = 0;
  ASSERT_EQ(grcore_engine_register(w.ctx, &kAlpha, &engine), GRCORE_OK);
  GRCORE_Stack * stack = grcore_context_stack(w.ctx);
  // Outside a run the word is unset, so a re-entry sets it from where it is.
  GRCORE_ActivationRef a;
  ASSERT_EQ(grcore_activation_enter(stack, GRCORE_ACTIVATION_REENTRY, engine,
                false, nullptr, &a), GRCORE_OK);
  uintptr_t set = grcore_context_native_limit(w.ctx);
  EXPECT_NE(set, 0u);
  // A second, nested one leaves it alone: the budget is a total.
  GRCORE_ActivationRef b;
  ASSERT_EQ(grcore_activation_enter(stack, GRCORE_ACTIVATION_REENTRY, engine,
                false, nullptr, &b), GRCORE_OK);
  EXPECT_EQ(grcore_context_native_limit(w.ctx), set);
  ASSERT_EQ(grcore_activation_leave(stack, b), GRCORE_OK);
  ASSERT_EQ(grcore_activation_leave(stack, a), GRCORE_OK);
  // And a JIT or native record never sets one.
  ASSERT_EQ(grcore_context_native_limit_set(w.ctx, 0), GRCORE_OK);
  GRCORE_ActivationRef c;
  ASSERT_EQ(grcore_activation_enter(stack, GRCORE_ACTIVATION_NATIVE, engine,
                false, nullptr, &c), GRCORE_OK);
  EXPECT_EQ(grcore_context_native_limit(w.ctx), 0u);
  ASSERT_EQ(grcore_activation_leave(stack, c), GRCORE_OK);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

/**
 * @file
 *
 * `run` and `resume`: the outcomes, the refusals, an entry that misreports,
 * what a resume clears, a watchdog stopping a runaway guest, and migration of
 * a paused context to another thread.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../../src/b/context_internal.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace {

GRCORE_Step finishes(GRCORE_Context *, void *) { return GRCORE_STEP_FINISHED; }

int g_entered = 0;
GRCORE_Step counts_entry(GRCORE_Context *, void *) {
  g_entered++;
  return GRCORE_STEP_FINISHED;
}

} // namespace

TEST(Run, AFinishingEntryReturnsOkFinishedAndLeavesTheContextParkedOutside) {
  RunWorld w;
  void * seen_state = nullptr;
  GRCORE_ContextState seen = GRCORE_CONTEXT_PARKED;
  int marker = 0;
  Fn fn{[&](GRCORE_Context * c) {
    seen = grcore_context_state(c);
    seen_state = &marker;
    return GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome = GRCORE_OUTCOME_PAUSED;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  EXPECT_EQ(seen, GRCORE_CONTEXT_RUNNING);
  EXPECT_EQ(w.ctx->config, GRCORE_CONFIG_PARKED_OUTSIDE);
  EXPECT_EQ(grcore_context_unwind_result(w.ctx), GRCORE_OK);
  // Reusable.
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_EQ(seen_state, &marker);
}

TEST(Run, ResumeCallsTheSameEntryWithTheSameState) {
  RunWorld w(5);
  CountingGuest g;
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, counting_entry, &g, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  ASSERT_EQ(g.pos, 5u);
  ASSERT_EQ(grcore_context_set_fuel(w.ctx, 12), GRCORE_OK);
  ASSERT_EQ(grcore_resume(w.ctx, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  // Continued from the saved position, not from zero. The step that paused
  // charged again when the entry was re-entered, which is this guest's own
  // doing: it charges at the top of the loop.
  EXPECT_EQ(g.pos, 11u);
}

TEST(Run, RefusesWhatItCannotRunAndChangesNothing) {
  RunWorld w;
  GRCORE_Outcome outcome = GRCORE_OUTCOME_PAUSED;
  g_entered = 0;
  EXPECT_EQ(grcore_run(nullptr, counts_entry, nullptr, &outcome), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_run(w.ctx, nullptr, nullptr, &outcome), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_run(w.ctx, counts_entry, nullptr, nullptr), GRCORE_ERR_INVALID);
  GRCORE_Result other = GRCORE_OK;
  std::thread([&] { other = grcore_run(w.ctx, counts_entry, nullptr, &outcome); }).join();
  EXPECT_EQ(other, GRCORE_ERR_INVALID);
  // Every configuration but "parked, outside run".
  for (GRCORE_ContextConfig cfg : {GRCORE_CONFIG_PARKED_INSIDE,
           GRCORE_CONFIG_RUNNING, GRCORE_CONFIG_AT_POLL, GRCORE_CONFIG_PAUSED}) {
    w.ctx->config = cfg;
    EXPECT_EQ(grcore_run(w.ctx, counts_entry, nullptr, &outcome),
        GRCORE_ERR_INVALID) << cfg;
    EXPECT_EQ(w.ctx->config, cfg);
  }
  w.ctx->config = GRCORE_CONFIG_PARKED_OUTSIDE;
  EXPECT_EQ(g_entered, 0);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_PAUSED); // never written
}

TEST(Run, ResumeRefusesWhatIsNotAPausedContextAndChangesNothing) {
  RunWorld w;
  GRCORE_Outcome outcome = GRCORE_OUTCOME_FINISHED;
  EXPECT_EQ(grcore_resume(nullptr, &outcome), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_resume(w.ctx, nullptr), GRCORE_ERR_INVALID);
  for (GRCORE_ContextConfig cfg : {GRCORE_CONFIG_PARKED_OUTSIDE,
           GRCORE_CONFIG_PARKED_INSIDE, GRCORE_CONFIG_RUNNING,
           GRCORE_CONFIG_AT_POLL}) {
    w.ctx->config = cfg;
    EXPECT_EQ(grcore_resume(w.ctx, &outcome), GRCORE_ERR_INVALID) << cfg;
    EXPECT_EQ(w.ctx->config, cfg);
  }
  // Paused, but nothing was ever run: there is no entry to call.
  w.ctx->config = GRCORE_CONFIG_PAUSED;
  EXPECT_EQ(grcore_resume(w.ctx, &outcome), GRCORE_ERR_INVALID);
  EXPECT_EQ(w.ctx->config, GRCORE_CONFIG_PAUSED);
  w.ctx->config = GRCORE_CONFIG_PARKED_OUTSIDE;
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
}

TEST(Run, ResumeFromAnotherThreadWithoutAcquiringIsRefused) {
  RunWorld w(5);
  CountingGuest g;
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, counting_entry, &g, &outcome), GRCORE_OK);
  GRCORE_Result other = GRCORE_OK;
  std::thread([&] { other = grcore_resume(w.ctx, &outcome); }).join();
  EXPECT_EQ(other, GRCORE_ERR_INVALID);
  EXPECT_EQ(w.ctx->config, GRCORE_CONFIG_PAUSED);
}

TEST(Run, AnUnwindReturnsTheErrorNotAnOutcomeAndParksTheContext) {
  RunWorld w;
  GRCORE_Outcome outcome = GRCORE_OUTCOME_PAUSED;
  ASSERT_EQ(grcore_context_terminate(w.ctx), GRCORE_OK);
  CountingGuest g;
  EXPECT_EQ(grcore_run(w.ctx, counting_entry, &g, &outcome), GRCORE_ERR_LIMIT);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  EXPECT_EQ(g.pos, 0u);
  EXPECT_EQ(w.ctx->config, GRCORE_CONFIG_PARKED_OUTSIDE);
  EXPECT_EQ(grcore_context_pause_key_count(w.ctx), 1u);
  EXPECT_EQ(grcore_context_pause_location(w.ctx).line, kCountingPollLine);
  // A fresh run is not affected by the last one's unwind.
  ASSERT_EQ(grcore_run(w.ctx, counting_entry, &g, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  EXPECT_EQ(grcore_context_unwind_result(w.ctx), GRCORE_OK);
  EXPECT_EQ(grcore_context_pause_key_count(w.ctx), 0u);
  EXPECT_EQ(grcore_context_pause_location(w.ctx).file, nullptr);
}

TEST(Run, ResumeClearsTimeAndInterruptButNotFuelOrServiceKinds) {
  RunWorld w(5);
  GRCORE_RequestKind svc;
  ASSERT_EQ(grcore_context_request_kind(w.ctx, &kYieldKey, &svc), GRCORE_OK);
  GRCORE_Port * p;
  ASSERT_EQ(grcore_context_port(w.ctx, &p), GRCORE_OK);
  ASSERT_EQ(grcore_port_post(p, GRCORE_REQUEST_TIME), GRCORE_OK);
  ASSERT_EQ(grcore_port_post(p, GRCORE_REQUEST_INTERRUPT), GRCORE_OK);
  ASSERT_EQ(grcore_port_post(p, svc), GRCORE_OK);
  CountingGuest g;
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, counting_entry, &g, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  EXPECT_EQ(grcore_context_pause_key_count(w.ctx), 2u); // time, interrupt
  bool time_after = true, interrupt_after = true, svc_after = false,
       fuel_after = false;
  Probe observer{"o"};
  observer.extra = [&](GRCORE_Context * c, GRCORE_PollCall *) {
    if (observer.calls > 1) {
      return; // the first poll after the resume is the one that matters
    }
    time_after = grcore_context_request_pending(c, GRCORE_REQUEST_TIME);
    interrupt_after = grcore_context_request_pending(c, GRCORE_REQUEST_INTERRUPT);
    svc_after = grcore_context_request_pending(c, svc);
    fuel_after = grcore_context_request_pending(c, GRCORE_REQUEST_FUEL);
  };
  ASSERT_EQ(grcore_context_register(w.ctx, &kObserveKey, &observer), GRCORE_OK);
  ASSERT_EQ(grcore_resume(w.ctx, &outcome), GRCORE_OK);
  EXPECT_FALSE(time_after);
  EXPECT_FALSE(interrupt_after);
  EXPECT_TRUE(svc_after);
  EXPECT_FALSE(fuel_after); // fuel not yet exhausted: 5 units, 1 step in
  grcore_port_release(p);
}

TEST(Run, TheKeysAndTheLocationOfAPauseAreValidUntilTheNextResume) {
  RunWorld w(3);
  CountingGuest g;
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, counting_entry, &g, &outcome), GRCORE_OK);
  EXPECT_EQ(grcore_context_pause_key_count(w.ctx), 1u);
  EXPECT_NE(grcore_context_pause_location(w.ctx).file, nullptr);
  EXPECT_EQ(grcore_context_pause_key(w.ctx, 1), nullptr);
  EXPECT_EQ(grcore_context_pause_key(nullptr, 0), nullptr);
  EXPECT_EQ(grcore_context_pause_key_count(nullptr), 0u);
  EXPECT_EQ(grcore_context_pause_location(nullptr).file, nullptr);
  EXPECT_EQ(grcore_context_unwind_result(nullptr), GRCORE_OK);
  ASSERT_EQ(grcore_context_set_fuel(w.ctx, GRCORE_UNLIMITED), GRCORE_OK);
  ASSERT_EQ(grcore_resume(w.ctx, &outcome), GRCORE_OK);
  EXPECT_EQ(grcore_context_pause_key_count(w.ctx), 0u);
  EXPECT_EQ(grcore_context_pause_location(w.ctx).file, nullptr);
}

TEST(Run, AnEntryThatPausesWithoutAPauseVerdictIsInternalAndLeavesTheContextParked) {
  RunWorld w;
  Fn fn{[](GRCORE_Context *) { return GRCORE_STEP_PAUSED; }};
  GRCORE_Outcome outcome = GRCORE_OUTCOME_FINISHED;
  EXPECT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_ERR_INTERNAL);
  EXPECT_EQ(w.ctx->config, GRCORE_CONFIG_PARKED_OUTSIDE);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  // And it can be used again, and migrated.
  ASSERT_EQ(grcore_run(w.ctx, finishes, nullptr, &outcome), GRCORE_OK);
  EXPECT_EQ(grcore_context_release(w.ctx), GRCORE_OK);
  EXPECT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
}

TEST(Run, EveryWayAnEntryCanMisreportIsInternal) {
  struct Lie {
    const char * name;
    std::function<GRCORE_Step(GRCORE_Context *)> body;
  };
  std::vector<Lie> lies = {
      {"unwound with no poll",
          [](GRCORE_Context *) { return GRCORE_STEP_UNWOUND; }},
      {"finished after a pause verdict",
          [](GRCORE_Context * c) {
            grcore_context_charge_fuel(c, 1000);
            GRCORE_POLL(c);
            return GRCORE_STEP_FINISHED;
          }},
      {"unwound after a pause verdict",
          [](GRCORE_Context * c) {
            grcore_context_charge_fuel(c, 1000);
            GRCORE_POLL(c);
            return GRCORE_STEP_UNWOUND;
          }},
      {"paused after an unwind verdict",
          [](GRCORE_Context * c) {
            grcore_context_terminate(c);
            GRCORE_POLL(c);
            return GRCORE_STEP_PAUSED;
          }},
      {"finished after an unwind verdict",
          [](GRCORE_Context * c) {
            grcore_context_terminate(c);
            GRCORE_POLL(c);
            return GRCORE_STEP_FINISHED;
          }},
      {"finished inside a host call",
          [](GRCORE_Context * c) {
            grcore_context_park(c);
            return GRCORE_STEP_FINISHED;
          }},
      {"a step that does not exist",
          [](GRCORE_Context *) { return static_cast<GRCORE_Step>(7); }},
  };
  for (auto & lie : lies) {
    RunWorld w(10);
    Fn fn{lie.body};
    GRCORE_Outcome outcome = GRCORE_OUTCOME_FINISHED;
    EXPECT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_ERR_INTERNAL)
        << lie.name;
    EXPECT_EQ(w.ctx->config, GRCORE_CONFIG_PARKED_OUTSIDE) << lie.name;
    EXPECT_FALSE(grcore_context_request_pending(w.ctx, GRCORE_REQUEST_TERMINATE))
        << lie.name;
    EXPECT_EQ(grcore_context_state(w.ctx), GRCORE_CONTEXT_PARKED) << lie.name;
  }
}

TEST(Run, AResumedEntryThatLiesIsInternalToo) {
  RunWorld w(5);
  int calls = 0;
  Fn fn{[&](GRCORE_Context * c) {
    if (calls++ == 0) {
      grcore_context_charge_fuel(c, 100);
      GRCORE_POLL(c);
      return GRCORE_STEP_PAUSED;
    }
    return GRCORE_STEP_PAUSED; // lies: no poll this time
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_EQ(grcore_resume(w.ctx, &outcome), GRCORE_ERR_INTERNAL);
  EXPECT_EQ(w.ctx->config, GRCORE_CONFIG_PARKED_OUTSIDE);
}

TEST(Run, AnEntryMayBracketAHostCallAndStillFinish) {
  RunWorld w;
  Fn fn{[](GRCORE_Context * c) {
    EXPECT_EQ(grcore_context_park(c), GRCORE_OK);
    EXPECT_EQ(grcore_context_state(c), GRCORE_CONTEXT_PARKED);
    EXPECT_EQ(grcore_context_release(c), GRCORE_ERR_INVALID); // cannot migrate
    EXPECT_EQ(grcore_context_unpark(c), GRCORE_OK);
    return GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  EXPECT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
}

TEST(Run, AWatchdogThreadStopsAGuestThatNeverStopsOnItsOwn) {
  RunWorld w;
  GRCORE_Port * p;
  ASSERT_EQ(grcore_context_port(w.ctx, &p), GRCORE_OK);
  std::atomic<uint64_t> iterations{0};
  Fn fn{[&](GRCORE_Context * c) {
    for (;;) {
      GRCORE_Verdict v = GRCORE_POLL(c);
      if (v == GRCORE_VERDICT_UNWIND) {
        return GRCORE_STEP_UNWOUND;
      }
      iterations++;
      // Valgrind runs threads one at a time; a guest that never yields would
      // keep the watchdog from ever being scheduled.
      std::this_thread::yield();
    }
  }};
  std::thread watchdog([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(grcore_port_post(p, GRCORE_REQUEST_TERMINATE), GRCORE_OK);
  });
  GRCORE_Outcome outcome;
  EXPECT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_ERR_LIMIT);
  watchdog.join();
  EXPECT_GT(iterations.load(), 0u);
  grcore_port_release(p);
}

TEST(Run, AWatchdogWakesAGuestBlockedInAWaitAndItUnwinds) {
  RunWorld w;
  GRCORE_Port * p;
  ASSERT_EQ(grcore_context_port(w.ctx, &p), GRCORE_OK);
  Fn fn{[&](GRCORE_Context * c) {
    bool woken = false;
    EXPECT_EQ(grcore_context_wait(c, GRCORE_UNLIMITED, &woken), GRCORE_OK);
    EXPECT_TRUE(woken);
    return GRCORE_POLL(c) == GRCORE_VERDICT_UNWIND ? GRCORE_STEP_UNWOUND
                                                   : GRCORE_STEP_FINISHED;
  }};
  std::thread watchdog([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(grcore_port_post(p, GRCORE_REQUEST_TERMINATE), GRCORE_OK);
  });
  GRCORE_Outcome outcome;
  EXPECT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_ERR_LIMIT);
  watchdog.join();
  grcore_port_release(p);
}

TEST(Migrate, APausedContextResumesOnAnotherThreadWithTheSameOutputAsAStraightRun) {
  CountingGuest straight;
  {
    RunWorld w;
    GRCORE_Outcome o;
    ASSERT_EQ(grcore_run(w.ctx, counting_entry, &straight, &o), GRCORE_OK);
    ASSERT_EQ(o, GRCORE_OUTCOME_FINISHED);
  }
  RunWorld w(30);
  CountingGuest g;
  // Built before the guest runs, so that nothing but the library's hand-off
  // orders what the run writes against what the migrant reads.
  GRCORE_Result r = GRCORE_ERR_INTERNAL;
  GRCORE_Outcome there = GRCORE_OUTCOME_PAUSED;
  bool readable = false;
  GRCORE_Result fuel_result = GRCORE_ERR_INTERNAL;
  MigrantThread migrant(w.ctx, [&] {
    readable = grcore_context_guest_state_readable(w.ctx);
    fuel_result = grcore_context_set_fuel(w.ctx, GRCORE_UNLIMITED);
    r = grcore_resume(w.ctx, &there);
    grcore_context_release(w.ctx);
  });
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, counting_entry, &g, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  ASSERT_EQ(grcore_context_release(w.ctx), GRCORE_OK);
  EXPECT_FALSE(grcore_context_is_owner(w.ctx));
  migrant.go();
  ASSERT_TRUE(migrant.take_back());
  migrant.join();
  ASSERT_EQ(migrant.acquire_result(), GRCORE_OK);
  EXPECT_TRUE(readable);
  EXPECT_EQ(fuel_result, GRCORE_OK);
  EXPECT_EQ(r, GRCORE_OK);
  EXPECT_EQ(there, GRCORE_OUTCOME_FINISHED);
  EXPECT_EQ(g.sum, straight.sum);
  EXPECT_EQ(g.pos, straight.pos);
}

TEST(Migrate, AContextPingPongsBetweenThreadsAcrossManyPauses) {
  RunWorld w(7);
  CountingGuest g;
  g.n = 200;
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, counting_entry, &g, &outcome), GRCORE_OK);
  int hops = 0;
  while (outcome == GRCORE_OUTCOME_PAUSED) {
    GRCORE_Result step[3] = {GRCORE_ERR_INTERNAL, GRCORE_ERR_INTERNAL,
        GRCORE_ERR_INTERNAL};
    MigrantThread migrant(w.ctx, [&] {
      step[0] = grcore_context_set_fuel(w.ctx, grcore_context_fuel_used(w.ctx) + 7);
      step[1] = grcore_resume(w.ctx, &outcome);
      step[2] = grcore_context_release(w.ctx);
    });
    ASSERT_EQ(grcore_context_release(w.ctx), GRCORE_OK);
    migrant.go();
    ASSERT_TRUE(migrant.take_back());
    migrant.join();
    ASSERT_EQ(migrant.acquire_result(), GRCORE_OK);
    for (GRCORE_Result s : step) {
      ASSERT_EQ(s, GRCORE_OK);
    }
    hops++;
  }
  EXPECT_GT(hops, 20);
  EXPECT_EQ(g.sum, sum_below(200));
}

TEST(Migrate, ATerminateFromAnotherThreadAfterTheHandOffUnwindsOnResume) {
  RunWorld w(10);
  GRCORE_Port * p;
  ASSERT_EQ(grcore_context_port(w.ctx, &p), GRCORE_OK);
  CountingGuest g;
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, counting_entry, &g, &outcome), GRCORE_OK);
  ASSERT_EQ(grcore_context_release(w.ctx), GRCORE_OK);
  std::thread([&] { EXPECT_EQ(grcore_port_post(p, GRCORE_REQUEST_TERMINATE), GRCORE_OK); })
      .join();
  GRCORE_Result r = GRCORE_OK;
  std::thread([&] {
    ASSERT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
    r = grcore_resume(w.ctx, &outcome);
    ASSERT_EQ(grcore_context_release(w.ctx), GRCORE_OK);
  }).join();
  EXPECT_EQ(r, GRCORE_ERR_LIMIT);
  ASSERT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
  grcore_port_release(p);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

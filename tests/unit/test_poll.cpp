/**
 * @file
 *
 * The poll: the fast path, the four phases, verdicts and the keys that issue
 * them, the runtime poll for natives, and the memory path.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../../src/b/context_internal.h"

#include <cstring>
#include <string>
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

/* Runs `n` polls inside one run and returns what each said. */
std::vector<GRCORE_Verdict> polls_in_a_run(
    GRCORE_Context * c, int n, GRCORE_Outcome * outcome = nullptr) {
  std::vector<GRCORE_Verdict> seen;
  Fn fn{[&](GRCORE_Context * ctx) {
    for (int i = 0; i < n; i++) {
      GRCORE_Verdict v = GRCORE_POLL(ctx);
      seen.push_back(v);
      if (v == GRCORE_VERDICT_PAUSE) {
        return GRCORE_STEP_PAUSED;
      }
      if (v == GRCORE_VERDICT_UNWIND) {
        return GRCORE_STEP_UNWOUND;
      }
    }
    return GRCORE_STEP_FINISHED;
  }};
  static Fn keep; // run keeps the pointer for resume
  keep = fn;
  GRCORE_Outcome o;
  grcore_run(c, fn_entry, &keep, outcome != nullptr ? outcome : &o);
  return seen;
}

} // namespace

TEST(Poll, WithNothingPendingNoHandlerRunsAndTheStateStaysRunning) {
  RunWorld w;
  Probe d{"d"}, a{"a"}, o{"o"}, y{"y"};
  ASSERT_EQ(grcore_context_register(w.ctx, &kDecideKey, &d), GRCORE_OK);
  ASSERT_EQ(grcore_context_register(w.ctx, &kActKey, &a), GRCORE_OK);
  ASSERT_EQ(grcore_context_register(w.ctx, &kObserveKey, &o), GRCORE_OK);
  ASSERT_EQ(grcore_context_register(w.ctx, &kYieldKey, &y), GRCORE_OK);
  std::vector<GRCORE_ContextState> states;
  Fn fn{[&](GRCORE_Context * c) {
    for (int i = 0; i < 5; i++) {
      EXPECT_EQ(GRCORE_POLL(c), GRCORE_VERDICT_CONTINUE);
      states.push_back(grcore_context_state(c));
    }
    return GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_EQ(d.calls + a.calls + o.calls + y.calls, 0);
  for (auto s : states) {
    EXPECT_EQ(s, GRCORE_CONTEXT_RUNNING);
  }
}

TEST(Poll, ExhaustedFuelPausesWithTheFuelKeyAndTheLocationOfThePoll) {
  RunWorld w(10);
  CountingGuest g;
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, counting_entry, &g, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  EXPECT_EQ(grcore_context_state(w.ctx), GRCORE_CONTEXT_PAUSED);
  EXPECT_EQ(g.pos, 10u); // ten steps paid for, the eleventh pauses
  ASSERT_EQ(pause_keys(w.ctx).size(), 1u);
  EXPECT_EQ(pause_keys(w.ctx)[0], core(GRCORE_REQUEST_FUEL));
  GRCORE_Location loc = grcore_context_pause_location(w.ctx);
  EXPECT_STREQ(loc.file, kCountingPollFile);
  EXPECT_EQ(loc.line, kCountingPollLine);
  EXPECT_TRUE(grcore_context_guest_state_readable(w.ctx));
}

TEST(Poll, ExactlyAsMuchFuelAsTheBudgetIsNotExhausted) {
  RunWorld w(100);
  CountingGuest g; // 100 steps at one unit each
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, counting_entry, &g, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  EXPECT_EQ(g.sum, sum_below(100));
}

TEST(Poll, RaisingTheBudgetAndResumingRunsToTheEnd) {
  RunWorld w(10);
  CountingGuest g;
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, counting_entry, &g, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  ASSERT_EQ(grcore_context_set_fuel(w.ctx, 1000), GRCORE_OK);
  ASSERT_EQ(grcore_resume(w.ctx, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  EXPECT_EQ(g.sum, sum_below(100));
  EXPECT_EQ(grcore_context_state(w.ctx), GRCORE_CONTEXT_PARKED);
  EXPECT_EQ(w.ctx->config, GRCORE_CONFIG_PARKED_OUTSIDE);
}

TEST(Poll, ResumingWithoutRaisingPausesAgainWithTheSameKey) {
  RunWorld w(10);
  CountingGuest g;
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, counting_entry, &g, &outcome), GRCORE_OK);
  for (int i = 0; i < 3; i++) {
    ASSERT_EQ(grcore_resume(w.ctx, &outcome), GRCORE_OK);
    EXPECT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
    ASSERT_EQ(pause_keys(w.ctx).size(), 1u);
    EXPECT_EQ(pause_keys(w.ctx)[0], core(GRCORE_REQUEST_FUEL));
    EXPECT_EQ(g.pos, 10u); // no progress without fuel
  }
}

TEST(Poll, TerminatingAPausedRunUnwindsWithALimitErrorAndTheReasonIsReadable) {
  RunWorld w(10);
  CountingGuest g;
  GRCORE_Outcome outcome = GRCORE_OUTCOME_FINISHED;
  ASSERT_EQ(grcore_run(w.ctx, counting_entry, &g, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  ASSERT_EQ(grcore_context_terminate(w.ctx), GRCORE_OK);
  outcome = GRCORE_OUTCOME_PAUSED;
  EXPECT_EQ(grcore_resume(w.ctx, &outcome), GRCORE_ERR_LIMIT);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_PAUSED); // not written on failure
  EXPECT_EQ(grcore_context_unwind_result(w.ctx), GRCORE_ERR_LIMIT);
  // Terminate and fuel both held; terminate alone is the unwind's reason.
  ASSERT_EQ(pause_keys(w.ctx).size(), 1u);
  EXPECT_EQ(pause_keys(w.ctx)[0], core(GRCORE_REQUEST_TERMINATE));
  // Parked outside run, so the context can be reused or migrated.
  EXPECT_EQ(w.ctx->config, GRCORE_CONFIG_PARKED_OUTSIDE);
  EXPECT_EQ(grcore_context_state(w.ctx), GRCORE_CONTEXT_PARKED);
  // Terminate stayed pending until run returned, then was cleared.
  EXPECT_FALSE(grcore_context_request_pending(w.ctx, GRCORE_REQUEST_TERMINATE));
  EXPECT_EQ(grcore_context_release(w.ctx), GRCORE_OK);
  EXPECT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
}

TEST(Poll, ATimeRequestFromAnotherThreadPausesWithTheTimeKeyAndResumeClearsIt) {
  RunWorld w;
  GRCORE_Port * p;
  ASSERT_EQ(grcore_context_port(w.ctx, &p), GRCORE_OK);
  std::thread([&] { ASSERT_EQ(grcore_port_post(p, GRCORE_REQUEST_TIME), GRCORE_OK); })
      .join();
  CountingGuest g;
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, counting_entry, &g, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  EXPECT_EQ(pause_keys(w.ctx), std::vector<const GRCORE_Key *>{core(GRCORE_REQUEST_TIME)});
  EXPECT_TRUE(grcore_context_request_pending(w.ctx, GRCORE_REQUEST_TIME));
  ASSERT_EQ(grcore_resume(w.ctx, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  EXPECT_FALSE(grcore_context_request_pending(w.ctx, GRCORE_REQUEST_TIME));
  EXPECT_EQ(g.sum, sum_below(100));
  grcore_port_release(p);
}

TEST(Poll, AnInterruptPausesLikeTime) {
  RunWorld w;
  GRCORE_Port * p;
  ASSERT_EQ(grcore_context_port(w.ctx, &p), GRCORE_OK);
  ASSERT_EQ(grcore_port_post(p, GRCORE_REQUEST_INTERRUPT), GRCORE_OK);
  CountingGuest g;
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, counting_entry, &g, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  EXPECT_EQ(pause_keys(w.ctx),
      std::vector<const GRCORE_Key *>{core(GRCORE_REQUEST_INTERRUPT)});
  ASSERT_EQ(grcore_resume(w.ctx, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  grcore_port_release(p);
}

TEST(Poll, TerminateUnwindsFromTheRuntimePollToo) {
  RunWorld w;
  ASSERT_EQ(grcore_context_terminate(w.ctx), GRCORE_OK);
  GRCORE_Result got = GRCORE_OK;
  bool pending_after_unwind = false;
  Fn fn{[&](GRCORE_Context * c) {
    got = grcore_runtime_poll(c, 1, GRCORE_HERE);
    pending_after_unwind =
        grcore_context_request_pending(c, GRCORE_REQUEST_TERMINATE);
    return GRCORE_STEP_UNWOUND;
  }};
  GRCORE_Outcome outcome;
  EXPECT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_ERR_LIMIT);
  EXPECT_EQ(got, GRCORE_ERR_LIMIT);
  EXPECT_TRUE(pending_after_unwind); // pending until run returns
  EXPECT_FALSE(grcore_context_request_pending(w.ctx, GRCORE_REQUEST_TERMINATE));
}

TEST(Poll, TheStrongestVerdictWinsAndOnlyItsVotersAreReported) {
  RunWorld w(0); // fuel is exhausted at once: a built-in pause vote as well
  Probe pause{"pause"}, unwind{"unwind"}, quiet{"quiet"};
  pause.vote = GRCORE_VERDICT_PAUSE;
  unwind.vote = GRCORE_VERDICT_UNWIND;
  ASSERT_EQ(grcore_context_register(w.ctx, &kDecideKey, &pause), GRCORE_OK);
  ASSERT_EQ(grcore_context_register(w.ctx, &kDecideKey, &quiet), GRCORE_OK);
  ASSERT_EQ(grcore_context_register(w.ctx, &kDecideKey, &unwind), GRCORE_OK);
  CountingGuest g;
  g.fuel = 1;
  GRCORE_Outcome outcome;
  EXPECT_EQ(grcore_run(w.ctx, counting_entry, &g, &outcome), GRCORE_ERR_LIMIT);
  auto keys = pause_keys(w.ctx);
  ASSERT_EQ(keys.size(), 1u);
  EXPECT_EQ(keys[0], &kDecideKey); // the unwind voter, not the pausers
  EXPECT_EQ(grcore_context_unwind_result(w.ctx), GRCORE_ERR_LIMIT);
  EXPECT_EQ(pause.calls, 1);
  EXPECT_EQ(unwind.calls, 1);
}

TEST(Poll, ThePausingKeysAreInCanonicalOrderBuiltInsByKindThenRegistration) {
  static const GRCORE_Key kA = GRCORE_KEY_INIT("A", GRCORE_CARDINALITY_MANY,
      GRCORE_PHASE_DECIDE, nullptr, probe_handler, nullptr, nullptr, nullptr);
  static const GRCORE_Key kB = GRCORE_KEY_INIT("B", GRCORE_CARDINALITY_MANY,
      GRCORE_PHASE_DECIDE, nullptr, probe_handler, nullptr, nullptr, nullptr);
  RunWorld w(0);
  Probe a{"a"}, b{"b"};
  a.vote = b.vote = GRCORE_VERDICT_PAUSE;
  ASSERT_EQ(grcore_context_register(w.ctx, &kB, &b), GRCORE_OK);
  ASSERT_EQ(grcore_context_register(w.ctx, &kA, &a), GRCORE_OK);
  GRCORE_Port * p;
  ASSERT_EQ(grcore_context_port(w.ctx, &p), GRCORE_OK);
  ASSERT_EQ(grcore_port_post(p, GRCORE_REQUEST_INTERRUPT), GRCORE_OK);
  ASSERT_EQ(grcore_port_post(p, GRCORE_REQUEST_TIME), GRCORE_OK);
  CountingGuest g;
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, counting_entry, &g, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  std::vector<const GRCORE_Key *> want = {core(GRCORE_REQUEST_TIME),
      core(GRCORE_REQUEST_INTERRUPT), core(GRCORE_REQUEST_FUEL), &kB, &kA};
  EXPECT_EQ(pause_keys(w.ctx), want);
  EXPECT_EQ(grcore_context_pause_key(w.ctx, 5), nullptr);
  grcore_port_release(p);
}

TEST(Poll, TheLowestRegistrationIndexAmongUnwindersChoosesTheResult) {
  static const GRCORE_Key kA = GRCORE_KEY_INIT("A", GRCORE_CARDINALITY_MANY,
      GRCORE_PHASE_DECIDE, nullptr, probe_handler, nullptr, nullptr, nullptr);
  for (int swap = 0; swap < 2; swap++) {
    RunWorld w;
    Probe guest{"guest"}, limit{"limit"};
    guest.vote = limit.vote = GRCORE_VERDICT_UNWIND;
    guest.unwind_result = GRCORE_ERR_GUEST;
    limit.unwind_result = GRCORE_ERR_LIMIT;
    Probe * first = swap ? &limit : &guest;
    Probe * second = swap ? &guest : &limit;
    ASSERT_EQ(grcore_context_register(w.ctx, &kA, first), GRCORE_OK);
    ASSERT_EQ(grcore_context_register(w.ctx, &kA, second), GRCORE_OK);
    CountingGuest g;
    GRCORE_Port * p;
    ASSERT_EQ(grcore_context_port(w.ctx, &p), GRCORE_OK);
    ASSERT_EQ(grcore_port_post(p, GRCORE_REQUEST_TIME), GRCORE_OK); // slow path
    GRCORE_Outcome outcome;
    GRCORE_Result r = grcore_run(w.ctx, counting_entry, &g, &outcome);
    EXPECT_EQ(r, first->unwind_result) << "swap=" << swap;
    EXPECT_EQ(grcore_context_unwind_result(w.ctx), first->unwind_result);
    EXPECT_EQ(pause_keys(w.ctx).size(), 2u);
    grcore_port_release(p);
  }
}

TEST(Poll, AGuestUnwindResultIsErrGuest) {
  RunWorld w;
  Probe trap{"trap"};
  trap.vote = GRCORE_VERDICT_UNWIND;
  trap.unwind_result = GRCORE_ERR_GUEST;
  ASSERT_EQ(grcore_context_register(w.ctx, &kDecideKey, &trap), GRCORE_OK);
  GRCORE_Port * p;
  ASSERT_EQ(grcore_context_port(w.ctx, &p), GRCORE_OK);
  ASSERT_EQ(grcore_port_post(p, GRCORE_REQUEST_TIME), GRCORE_OK);
  CountingGuest g;
  GRCORE_Outcome outcome;
  EXPECT_EQ(grcore_run(w.ctx, counting_entry, &g, &outcome), GRCORE_ERR_GUEST);
  EXPECT_EQ(grcore_context_unwind_result(w.ctx), GRCORE_ERR_GUEST);
  grcore_port_release(p);
}

TEST(Poll, TerminateBeatsAGuestUnwinderBecauseBuiltInsComeFirst) {
  RunWorld w;
  Probe trap{"trap"};
  trap.vote = GRCORE_VERDICT_UNWIND;
  trap.unwind_result = GRCORE_ERR_GUEST;
  ASSERT_EQ(grcore_context_register(w.ctx, &kDecideKey, &trap), GRCORE_OK);
  ASSERT_EQ(grcore_context_terminate(w.ctx), GRCORE_OK);
  CountingGuest g;
  GRCORE_Outcome outcome;
  EXPECT_EQ(grcore_run(w.ctx, counting_entry, &g, &outcome), GRCORE_ERR_LIMIT);
  EXPECT_EQ(pause_keys(w.ctx).size(), 2u);
}

TEST(Poll, VotingIsValidOnlyInDecideAndYield) {
  RunWorld w;
  GRCORE_Result vote_in[5];
  GRCORE_Result unwind_in[5];
  GRCORE_Phase phase_in[5];
  auto record = [&](GRCORE_Context *, GRCORE_PollCall * call) {
    int ph = grcore_pollcall_phase(call);
    vote_in[ph] = grcore_pollcall_vote(call, GRCORE_VERDICT_CONTINUE);
    unwind_in[ph] = grcore_pollcall_set_unwind_result(call, GRCORE_ERR_GUEST);
    phase_in[ph] = grcore_pollcall_phase(call);
  };
  Probe d{"d"}, a{"a"}, o{"o"}, y{"y"};
  d.extra = a.extra = o.extra = y.extra = record;
  ASSERT_EQ(grcore_context_register(w.ctx, &kDecideKey, &d), GRCORE_OK);
  ASSERT_EQ(grcore_context_register(w.ctx, &kActKey, &a), GRCORE_OK);
  ASSERT_EQ(grcore_context_register(w.ctx, &kObserveKey, &o), GRCORE_OK);
  ASSERT_EQ(grcore_context_register(w.ctx, &kYieldKey, &y), GRCORE_OK);
  GRCORE_Port * p;
  ASSERT_EQ(grcore_context_port(w.ctx, &p), GRCORE_OK);
  ASSERT_EQ(grcore_port_post(p, GRCORE_REQUEST_TIME), GRCORE_OK);
  // A time request would make the poll pause and skip YIELD. Swap it for a
  // service kind, which no built-in reads, so the poll continues.
  ASSERT_EQ(grcore_context_clear_request(w.ctx, GRCORE_REQUEST_TIME), GRCORE_OK);
  GRCORE_RequestKind kind;
  ASSERT_EQ(grcore_context_request_kind(w.ctx, &kDecideKey, &kind), GRCORE_OK);
  ASSERT_EQ(grcore_port_post(p, kind), GRCORE_OK);
  auto verdicts = polls_in_a_run(w.ctx, 1);
  ASSERT_EQ(verdicts, std::vector<GRCORE_Verdict>{GRCORE_VERDICT_CONTINUE});
  EXPECT_EQ(vote_in[GRCORE_PHASE_DECIDE], GRCORE_OK);
  EXPECT_EQ(vote_in[GRCORE_PHASE_ACT], GRCORE_ERR_INVALID);
  EXPECT_EQ(vote_in[GRCORE_PHASE_OBSERVE], GRCORE_ERR_INVALID);
  EXPECT_EQ(vote_in[GRCORE_PHASE_YIELD], GRCORE_OK);
  EXPECT_EQ(unwind_in[GRCORE_PHASE_DECIDE], GRCORE_OK);
  EXPECT_EQ(unwind_in[GRCORE_PHASE_ACT], GRCORE_ERR_INVALID);
  EXPECT_EQ(unwind_in[GRCORE_PHASE_OBSERVE], GRCORE_ERR_INVALID);
  EXPECT_EQ(unwind_in[GRCORE_PHASE_YIELD], GRCORE_OK);
  EXPECT_EQ(phase_in[GRCORE_PHASE_DECIDE], GRCORE_PHASE_DECIDE);
  EXPECT_EQ(phase_in[GRCORE_PHASE_YIELD], GRCORE_PHASE_YIELD);
  // Argument checks.
  EXPECT_EQ(grcore_pollcall_vote(nullptr, GRCORE_VERDICT_PAUSE), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_pollcall_set_unwind_result(nullptr, GRCORE_ERR_GUEST),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_pollcall_phase(nullptr), GRCORE_PHASE_NONE);
  EXPECT_EQ(grcore_pollcall_verdict(nullptr), GRCORE_VERDICT_CONTINUE);
  EXPECT_FALSE(grcore_pollcall_pending(nullptr, GRCORE_REQUEST_TIME));
  EXPECT_FALSE(grcore_pollcall_reclaim_requested(nullptr));
  grcore_port_release(p);
}

TEST(Poll, BadVerdictsAndUnwindResultsAreRefused) {
  RunWorld w;
  GRCORE_Result bad_verdict = GRCORE_OK, bad_result = GRCORE_OK,
                ok_result = GRCORE_ERR_INTERNAL;
  Probe d{"d"};
  d.extra = [&](GRCORE_Context *, GRCORE_PollCall * call) {
    bad_verdict = grcore_pollcall_vote(call, static_cast<GRCORE_Verdict>(7));
    bad_result = grcore_pollcall_set_unwind_result(call, GRCORE_ERR_OOM);
    ok_result = grcore_pollcall_set_unwind_result(call, GRCORE_ERR_LIMIT);
  };
  ASSERT_EQ(grcore_context_register(w.ctx, &kDecideKey, &d), GRCORE_OK);
  GRCORE_Port * p;
  ASSERT_EQ(grcore_context_port(w.ctx, &p), GRCORE_OK);
  GRCORE_RequestKind kind;
  ASSERT_EQ(grcore_context_request_kind(w.ctx, &kDecideKey, &kind), GRCORE_OK);
  ASSERT_EQ(grcore_port_post(p, kind), GRCORE_OK);
  polls_in_a_run(w.ctx, 1);
  EXPECT_EQ(bad_verdict, GRCORE_ERR_INVALID);
  EXPECT_EQ(bad_result, GRCORE_ERR_INVALID);
  EXPECT_EQ(ok_result, GRCORE_OK);
  grcore_port_release(p);
}

TEST(Poll, PhasesRunInTheFixedOrderAndSeeTheRightVerdict) {
  RunWorld w;
  std::vector<std::string> log;
  GRCORE_Verdict decide_saw = GRCORE_VERDICT_PAUSE, act_saw = GRCORE_VERDICT_PAUSE,
                 observe_saw = GRCORE_VERDICT_PAUSE, yield_saw = GRCORE_VERDICT_PAUSE;
  Probe y{"YIELD"}, o{"OBSERVE"}, a{"ACT"}, d{"DECIDE"};
  y.log = o.log = a.log = d.log = &log;
  d.extra = [&](GRCORE_Context *, GRCORE_PollCall * c) { decide_saw = grcore_pollcall_verdict(c); };
  a.extra = [&](GRCORE_Context *, GRCORE_PollCall * c) { act_saw = grcore_pollcall_verdict(c); };
  o.extra = [&](GRCORE_Context *, GRCORE_PollCall * c) { observe_saw = grcore_pollcall_verdict(c); };
  y.extra = [&](GRCORE_Context *, GRCORE_PollCall * c) { yield_saw = grcore_pollcall_verdict(c); };
  // Registered out of phase order on purpose.
  ASSERT_EQ(grcore_context_register(w.ctx, &kYieldKey, &y), GRCORE_OK);
  ASSERT_EQ(grcore_context_register(w.ctx, &kObserveKey, &o), GRCORE_OK);
  ASSERT_EQ(grcore_context_register(w.ctx, &kActKey, &a), GRCORE_OK);
  ASSERT_EQ(grcore_context_register(w.ctx, &kDecideKey, &d), GRCORE_OK);
  GRCORE_Port* p;
  ASSERT_EQ(grcore_context_port(w.ctx, &p), GRCORE_OK);
  GRCORE_RequestKind kind;
  ASSERT_EQ(grcore_context_request_kind(w.ctx, &kDecideKey, &kind), GRCORE_OK);
  ASSERT_EQ(grcore_port_post(p, kind), GRCORE_OK); // stays pending: no verdict
  EXPECT_EQ(polls_in_a_run(w.ctx, 2).size(), 2u);
  std::vector<std::string> want = {"DECIDE", "ACT", "OBSERVE", "YIELD",
      "DECIDE", "ACT", "OBSERVE", "YIELD"};
  EXPECT_EQ(log, want);
  EXPECT_EQ(decide_saw, GRCORE_VERDICT_CONTINUE);
  EXPECT_EQ(act_saw, GRCORE_VERDICT_CONTINUE);
  EXPECT_EQ(observe_saw, GRCORE_VERDICT_CONTINUE);
  EXPECT_EQ(yield_saw, GRCORE_VERDICT_CONTINUE);
  grcore_port_release(p);
}

TEST(Poll, ActAndObserveSeeAPauseButYieldDoesNotRunForOne) {
  RunWorld w(0);
  Probe a{"a"}, o{"o"}, y{"y"};
  GRCORE_Verdict act_saw = GRCORE_VERDICT_CONTINUE, observe_saw = GRCORE_VERDICT_CONTINUE;
  a.extra = [&](GRCORE_Context *, GRCORE_PollCall * c) { act_saw = grcore_pollcall_verdict(c); };
  o.extra = [&](GRCORE_Context *, GRCORE_PollCall * c) { observe_saw = grcore_pollcall_verdict(c); };
  ASSERT_EQ(grcore_context_register(w.ctx, &kActKey, &a), GRCORE_OK);
  ASSERT_EQ(grcore_context_register(w.ctx, &kObserveKey, &o), GRCORE_OK);
  ASSERT_EQ(grcore_context_register(w.ctx, &kYieldKey, &y), GRCORE_OK);
  CountingGuest g;
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, counting_entry, &g, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  EXPECT_EQ(act_saw, GRCORE_VERDICT_PAUSE);
  EXPECT_EQ(observe_saw, GRCORE_VERDICT_PAUSE);
  EXPECT_EQ(a.calls, 1);
  EXPECT_EQ(o.calls, 1);
  EXPECT_EQ(y.calls, 0);
}

TEST(Poll, AYieldHandlerMayRaiseTheVerdictToAPauseAndIsReportedAsItsKey) {
  RunWorld w;
  Probe y{"y"}, quiet{"quiet"};
  y.vote = GRCORE_VERDICT_PAUSE;
  ASSERT_EQ(grcore_context_register(w.ctx, &kYieldKey, &quiet), GRCORE_OK);
  ASSERT_EQ(grcore_context_register(w.ctx, &kYieldKey, &y), GRCORE_OK);
  GRCORE_Port * p;
  ASSERT_EQ(grcore_context_port(w.ctx, &p), GRCORE_OK);
  GRCORE_RequestKind kind;
  ASSERT_EQ(grcore_context_request_kind(w.ctx, &kYieldKey, &kind), GRCORE_OK);
  ASSERT_EQ(grcore_port_post(p, kind), GRCORE_OK);
  GRCORE_Outcome outcome;
  auto v = polls_in_a_run(w.ctx, 3, &outcome);
  EXPECT_EQ(v, std::vector<GRCORE_Verdict>{GRCORE_VERDICT_PAUSE});
  EXPECT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  EXPECT_EQ(pause_keys(w.ctx), std::vector<const GRCORE_Key *>{&kYieldKey});
  EXPECT_EQ(quiet.calls, 1);
  EXPECT_EQ(y.calls, 1);
  grcore_port_release(p);
}

TEST(Poll, AYieldHandlerMayUnwindWithAGuestError) {
  RunWorld w;
  Probe y{"y"};
  y.vote = GRCORE_VERDICT_UNWIND;
  y.unwind_result = GRCORE_ERR_GUEST;
  ASSERT_EQ(grcore_context_register(w.ctx, &kYieldKey, &y), GRCORE_OK);
  GRCORE_Port * p;
  ASSERT_EQ(grcore_context_port(w.ctx, &p), GRCORE_OK);
  GRCORE_RequestKind kind;
  ASSERT_EQ(grcore_context_request_kind(w.ctx, &kYieldKey, &kind), GRCORE_OK);
  ASSERT_EQ(grcore_port_post(p, kind), GRCORE_OK);
  GRCORE_Outcome outcome;
  polls_in_a_run(w.ctx, 3, &outcome);
  EXPECT_EQ(grcore_context_unwind_result(w.ctx), GRCORE_ERR_GUEST);
  grcore_port_release(p);
}

TEST(Poll, AnOverflowKindPendingTakesTheSlowPathSoHandlersRun) {
  RunWorld w;
  std::vector<GRCORE_RequestKind> kinds;
  for (int i = 0; i < 70; i++) {
    GRCORE_RequestKind k;
    ASSERT_EQ(grcore_context_request_kind(w.ctx, &kDecideKey, &k), GRCORE_OK);
    kinds.push_back(k);
  }
  Probe o{"o"};
  ASSERT_EQ(grcore_context_register(w.ctx, &kObserveKey, &o), GRCORE_OK);
  GRCORE_Port * p;
  ASSERT_EQ(grcore_context_port(w.ctx, &p), GRCORE_OK);
  polls_in_a_run(w.ctx, 3);
  EXPECT_EQ(o.calls, 0);
  ASSERT_EQ(grcore_port_post(p, 66), GRCORE_OK);
  polls_in_a_run(w.ctx, 3);
  EXPECT_EQ(o.calls, 3); // level-triggered: every poll while it is pending
  ASSERT_EQ(grcore_context_clear_request(w.ctx, 66), GRCORE_OK);
  polls_in_a_run(w.ctx, 3);
  EXPECT_EQ(o.calls, 3);
  grcore_port_release(p);
}

TEST(Poll, PauseAllowedIsTrueOnlyForAnOrdinaryPollOutsideEveryNestedActivation) {
  RunWorld w;
  Probe y{"y"};
  std::vector<bool> allowed;
  y.extra = [&](GRCORE_Context *, GRCORE_PollCall * c) {
    allowed.push_back(grcore_pollcall_pause_allowed(c));
  };
  ASSERT_EQ(grcore_context_register(w.ctx, &kYieldKey, &y), GRCORE_OK);
  GRCORE_RequestKind kind;
  ASSERT_EQ(grcore_context_request_kind(w.ctx, &kYieldKey, &kind), GRCORE_OK);
  GRCORE_Port * p;
  ASSERT_EQ(grcore_context_port(w.ctx, &p), GRCORE_OK);
  ASSERT_EQ(grcore_port_post(p, kind), GRCORE_OK);
  Fn fn{[&](GRCORE_Context * c) {
    EXPECT_EQ(GRCORE_POLL(c), GRCORE_VERDICT_CONTINUE);                 // true
    EXPECT_EQ(grcore_runtime_poll(c, 0, GRCORE_HERE), GRCORE_OK);       // false
    EXPECT_EQ(grcore_context_nested_enter(c), GRCORE_OK);
    EXPECT_EQ(GRCORE_POLL(c), GRCORE_VERDICT_CONTINUE);                 // false
    EXPECT_EQ(grcore_context_nested_leave(c), GRCORE_OK);
    EXPECT_EQ(GRCORE_POLL(c), GRCORE_VERDICT_CONTINUE);                 // true
    return GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_EQ(allowed, (std::vector<bool>{true, false, false, true}));
  EXPECT_FALSE(grcore_pollcall_pause_allowed(nullptr));
  grcore_port_release(p);
}

TEST(Poll, AHandlerSeesItsOwnValueAndTheRequestsAsThePollSeesThem) {
  RunWorld w;
  Probe d{"d"};
  bool saw_time = false, saw_interrupt = true;
  d.extra = [&](GRCORE_Context *, GRCORE_PollCall * c) {
    saw_time = grcore_pollcall_pending(c, GRCORE_REQUEST_TIME);
    saw_interrupt = grcore_pollcall_pending(c, GRCORE_REQUEST_INTERRUPT);
  };
  ASSERT_EQ(grcore_context_register(w.ctx, &kDecideKey, &d), GRCORE_OK);
  GRCORE_Port * p;
  ASSERT_EQ(grcore_context_port(w.ctx, &p), GRCORE_OK);
  ASSERT_EQ(grcore_port_post(p, GRCORE_REQUEST_TIME), GRCORE_OK);
  polls_in_a_run(w.ctx, 1);
  EXPECT_EQ(d.calls, 1);
  EXPECT_TRUE(saw_time);
  EXPECT_FALSE(saw_interrupt);
  grcore_port_release(p);
}

TEST(Poll, AHandlerThatRegistersAtPollGrowsTheTablesWithoutDisturbingThePoll) {
  RunWorld w(0);
  // The ACT handler registers more DECIDE voters than the table has room for,
  // so the registration table and the poll's scratch block both grow while a
  // poll is in progress.
  static Probe added[40];
  int registered = 0;
  Probe a{"a"};
  a.extra = [&](GRCORE_Context * c, GRCORE_PollCall *) {
    for (int i = 0; i < 10; i++) {
      added[registered].vote = GRCORE_VERDICT_PAUSE;
      if (grcore_context_register(c, &kDecideKey, &added[registered]) ==
          GRCORE_OK) {
        registered++;
      }
    }
  };
  Probe first{"first"};
  first.vote = GRCORE_VERDICT_PAUSE;
  ASSERT_EQ(grcore_context_register(w.ctx, &kDecideKey, &first), GRCORE_OK);
  ASSERT_EQ(grcore_context_register(w.ctx, &kActKey, &a), GRCORE_OK);
  CountingGuest g;
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, counting_entry, &g, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  EXPECT_EQ(registered, 10);
  // That poll's key list was settled before ACT: the fuel key and `first`.
  EXPECT_EQ(pause_keys(w.ctx),
      (std::vector<const GRCORE_Key *>{core(GRCORE_REQUEST_FUEL), &kDecideKey}));
  EXPECT_EQ(added[0].calls, 0);
  // The next poll runs the new handlers, in registration order after `first`.
  ASSERT_EQ(grcore_resume(w.ctx, &outcome), GRCORE_OK);
  EXPECT_EQ(added[0].calls, 1);
  EXPECT_EQ(added[9].calls, 1);
  EXPECT_EQ(pause_keys(w.ctx).size(), 2u + 10u);
}

TEST(Poll, EveryAllocationFailureWhileRegisteringLeavesTheTablesConsistent) {
  bool succeeded = false;
  int failures = 0;
  for (long n = 1; n < 100 && !succeeded; n++) {
    TrackingAllocator t;
    RunWorld w(0, GRCORE_UNLIMITED, GRCORE_DEFAULT_MEMORY_RESERVE,
        GRCORE_UNLIMITED, GRCORE_UNLIMITED, t.get());
    std::vector<Probe> probes(20);
    for (auto & p : probes) {
      p.vote = GRCORE_VERDICT_PAUSE;
    }
    t.fail_at = t.calls + n;
    size_t registered = 0;
    for (auto & p : probes) {
      GRCORE_Result r = grcore_context_register(w.ctx, &kDecideKey, &p);
      if (r != GRCORE_OK) {
        EXPECT_EQ(r, GRCORE_ERR_OOM) << "n=" << n;
        failures++;
        break;
      }
      registered++;
    }
    EXPECT_EQ(grcore_context_registration_count(w.ctx), registered) << "n=" << n;
    succeeded = registered == probes.size();
    t.fail_at = 0;
    // Whatever was registered must be pollable: a vote slot for every one.
    CountingGuest g;
    GRCORE_Outcome outcome;
    ASSERT_EQ(grcore_run(w.ctx, counting_entry, &g, &outcome), GRCORE_OK);
    EXPECT_EQ(grcore_context_pause_key_count(w.ctx), 1u + registered) << "n=" << n;
  }
  EXPECT_TRUE(succeeded);
  EXPECT_GE(failures, 2); // the first growth and the second
}

TEST(Poll, PollingOutsideRunningUnwindsWithInvalidAndChangesNoState) {
  RunWorld w;
  ASSERT_EQ(grcore_context_terminate(w.ctx), GRCORE_OK); // forces the slow path
  EXPECT_EQ(w.ctx->config, GRCORE_CONFIG_PARKED_OUTSIDE);
  EXPECT_EQ(GRCORE_POLL(w.ctx), GRCORE_VERDICT_UNWIND);
  EXPECT_EQ(grcore_context_unwind_result(w.ctx), GRCORE_ERR_INVALID);
  EXPECT_EQ(w.ctx->config, GRCORE_CONFIG_PARKED_OUTSIDE);
  EXPECT_EQ(grcore_context_pause_key_count(w.ctx), 0u);
  EXPECT_EQ(grcore_runtime_poll(w.ctx, 0, GRCORE_HERE), GRCORE_ERR_INVALID);
  // Paused is not running either.
  w.ctx->config = GRCORE_CONFIG_PAUSED;
  EXPECT_EQ(GRCORE_POLL(w.ctx), GRCORE_VERDICT_UNWIND);
  EXPECT_EQ(w.ctx->config, GRCORE_CONFIG_PAUSED);
  w.ctx->config = GRCORE_CONFIG_PARKED_OUTSIDE;
  // A thread that does not own the context gets the same, and writes nothing.
  GRCORE_Verdict other = GRCORE_VERDICT_CONTINUE;
  GRCORE_Result other_runtime = GRCORE_OK;
  std::thread([&] {
    other = GRCORE_POLL(w.ctx);
    other_runtime = grcore_runtime_poll(w.ctx, 0, GRCORE_HERE);
  }).join();
  EXPECT_EQ(other, GRCORE_VERDICT_UNWIND);
  EXPECT_EQ(other_runtime, GRCORE_ERR_INVALID);
  EXPECT_EQ(w.ctx->config, GRCORE_CONFIG_PARKED_OUTSIDE);
  grcore_context_clear_terminate(w.ctx);
}

TEST(Poll, TheRuntimePollRefusesAPauseAndUnwindsWithALimitError) {
  RunWorld w(5);
  GRCORE_Result got = GRCORE_OK;
  GRCORE_ContextState after = GRCORE_CONTEXT_PAUSED;
  Fn fn{[&](GRCORE_Context * c) {
    got = grcore_runtime_poll(c, 6, GRCORE_HERE);
    after = grcore_context_state(c);
    return GRCORE_STEP_UNWOUND;
  }};
  GRCORE_Outcome outcome;
  EXPECT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_ERR_LIMIT);
  EXPECT_EQ(got, GRCORE_ERR_LIMIT);
  EXPECT_EQ(after, GRCORE_CONTEXT_RUNNING); // never PAUSED
  EXPECT_EQ(grcore_context_unwind_result(w.ctx), GRCORE_ERR_LIMIT);
  // The reason is the key that asked to pause.
  EXPECT_EQ(pause_keys(w.ctx), std::vector<const GRCORE_Key *>{core(GRCORE_REQUEST_FUEL)});
  EXPECT_EQ(grcore_context_fuel_used(w.ctx), 6u);
}

TEST(Poll, TheRuntimePollFromANonOwnerWritesNothing) {
  RunWorld w(100);
  GRCORE_Result got = GRCORE_OK;
  std::thread([&] { got = grcore_runtime_poll(w.ctx, 50, GRCORE_HERE); }).join();
  EXPECT_EQ(got, GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_fuel_used(w.ctx), 0u);
}

TEST(PollMemory, BudgetChangesThatClearTheBitAlsoClearTheReclaimMark) {
  RunWorld w(GRCORE_UNLIMITED, 1000, 500);
  const GRCORE_Allocator * a = grcore_context_allocator(w.ctx);
  void * big = a->malloc_fn(a->ctx, 1200);
  ASSERT_NE(big, nullptr);
  CountingGuest g;
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, counting_entry, &g, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  ASSERT_TRUE(w.ctx->reclaim_tried);
  ASSERT_EQ(grcore_context_set_memory_bytes(w.ctx, 2000), GRCORE_OK);
  EXPECT_FALSE(w.ctx->reclaim_tried);
  // A later overage gets its reclaim chance again.
  ASSERT_EQ(grcore_context_set_memory_bytes(w.ctx, 1000), GRCORE_OK);
  std::vector<bool> reclaim;
  Probe act{"act"};
  act.extra = [&](GRCORE_Context *, GRCORE_PollCall * c) {
    reclaim.push_back(grcore_pollcall_reclaim_requested(c));
  };
  ASSERT_EQ(grcore_context_register(w.ctx, &kActKey, &act), GRCORE_OK);
  ASSERT_EQ(grcore_resume(w.ctx, &outcome), GRCORE_OK);
  ASSERT_FALSE(reclaim.empty());
  EXPECT_TRUE(reclaim[0]);
  a->free_fn(a->ctx, big);
}

TEST(Poll, TheRuntimePollChargesWorkAndContinuesWhileThereIsFuel) {
  RunWorld w(100);
  std::vector<GRCORE_Result> got;
  Fn fn{[&](GRCORE_Context * c) {
    got.push_back(grcore_runtime_poll(c, 60, GRCORE_HERE));
    got.push_back(grcore_runtime_poll(c, 40, GRCORE_HERE));
    got.push_back(grcore_runtime_poll(c, 1, GRCORE_HERE));
    return GRCORE_STEP_UNWOUND;
  }};
  GRCORE_Outcome outcome;
  EXPECT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_ERR_LIMIT);
  EXPECT_EQ(got, (std::vector<GRCORE_Result>{GRCORE_OK, GRCORE_OK, GRCORE_ERR_LIMIT}));
  EXPECT_EQ(grcore_runtime_poll(nullptr, 1, GRCORE_HERE), GRCORE_ERR_INVALID);
}

TEST(Poll, AYieldHandlerThatPausesDuringARuntimePollIsRefusedToo) {
  RunWorld w;
  Probe y{"y"};
  y.vote = GRCORE_VERDICT_PAUSE;
  ASSERT_EQ(grcore_context_register(w.ctx, &kYieldKey, &y), GRCORE_OK);
  GRCORE_Port * p;
  ASSERT_EQ(grcore_context_port(w.ctx, &p), GRCORE_OK);
  GRCORE_RequestKind kind;
  ASSERT_EQ(grcore_context_request_kind(w.ctx, &kYieldKey, &kind), GRCORE_OK);
  ASSERT_EQ(grcore_port_post(p, kind), GRCORE_OK);
  GRCORE_Result got = GRCORE_OK;
  Fn fn{[&](GRCORE_Context * c) {
    got = grcore_runtime_poll(c, 0, GRCORE_HERE);
    return GRCORE_STEP_UNWOUND;
  }};
  GRCORE_Outcome outcome;
  EXPECT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_ERR_LIMIT);
  EXPECT_EQ(got, GRCORE_ERR_LIMIT);
  EXPECT_EQ(pause_keys(w.ctx), std::vector<const GRCORE_Key *>{&kYieldKey});
  grcore_port_release(p);
}

TEST(PollMemory, OverBudgetWithinTheReserveIsServedAndTheFirstPollOnlyAsksToReclaim) {
  RunWorld w(GRCORE_UNLIMITED, 1000, 500);
  const GRCORE_Allocator * a = grcore_context_allocator(w.ctx);
  void * small = a->malloc_fn(a->ctx, 900);
  ASSERT_NE(small, nullptr);
  EXPECT_FALSE(grcore_context_request_pending(w.ctx, GRCORE_REQUEST_MEMORY));
  void * over = a->malloc_fn(a->ctx, 300); // 1200: past the budget, in reserve
  ASSERT_NE(over, nullptr);
  EXPECT_TRUE(grcore_context_request_pending(w.ctx, GRCORE_REQUEST_MEMORY));

  Probe act{"act"};
  std::vector<bool> reclaim;
  act.extra = [&](GRCORE_Context *, GRCORE_PollCall * c) {
    reclaim.push_back(grcore_pollcall_reclaim_requested(c));
  };
  ASSERT_EQ(grcore_context_register(w.ctx, &kActKey, &act), GRCORE_OK);
  Probe obs{"obs"};
  obs.extra = [&](GRCORE_Context *, GRCORE_PollCall * c) {
    EXPECT_FALSE(grcore_pollcall_reclaim_requested(c)); // ACT only
  };
  ASSERT_EQ(grcore_context_register(w.ctx, &kObserveKey, &obs), GRCORE_OK);

  GRCORE_Outcome outcome;
  auto v = polls_in_a_run(w.ctx, 3, &outcome);
  // First poll: reclaim asked, verdict deferred. Second: still over, pause.
  ASSERT_EQ(v.size(), 2u);
  EXPECT_EQ(v[0], GRCORE_VERDICT_CONTINUE);
  EXPECT_EQ(v[1], GRCORE_VERDICT_PAUSE);
  EXPECT_EQ(reclaim, (std::vector<bool>{true, false}));
  EXPECT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  EXPECT_EQ(pause_keys(w.ctx), std::vector<const GRCORE_Key *>{core(GRCORE_REQUEST_MEMORY)});
  a->free_fn(a->ctx, over);
  a->free_fn(a->ctx, small);
}

TEST(PollMemory, AReclaimThatBringsMemoryBackUnderTheBudgetMeansNoPause) {
  RunWorld w(GRCORE_UNLIMITED, 1000, 500);
  const GRCORE_Allocator * a = grcore_context_allocator(w.ctx);
  void * big = a->malloc_fn(a->ctx, 1200);
  ASSERT_NE(big, nullptr);
  Probe collector{"gc"};
  collector.extra = [&](GRCORE_Context *, GRCORE_PollCall * c) {
    if (grcore_pollcall_reclaim_requested(c) && big != nullptr) {
      a->free_fn(a->ctx, big); // the "collector" frees memory in ACT
      big = nullptr;
    }
  };
  ASSERT_EQ(grcore_context_register(w.ctx, &kActKey, &collector), GRCORE_OK);
  GRCORE_Outcome outcome;
  auto v = polls_in_a_run(w.ctx, 4, &outcome);
  EXPECT_EQ(v, (std::vector<GRCORE_Verdict>(4, GRCORE_VERDICT_CONTINUE)));
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  EXPECT_FALSE(grcore_context_request_pending(w.ctx, GRCORE_REQUEST_MEMORY));
  EXPECT_EQ(collector.calls, 1); // the bit dropped, so the slow path stopped
  EXPECT_FALSE(w.ctx->reclaim_tried);
}

TEST(PollMemory, RaisingTheBudgetAfterAMemoryPauseLetsTheRunContinue) {
  RunWorld w(GRCORE_UNLIMITED, 1000, 500);
  const GRCORE_Allocator * a = grcore_context_allocator(w.ctx);
  void * big = a->malloc_fn(a->ctx, 1200);
  ASSERT_NE(big, nullptr);
  CountingGuest g;
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, counting_entry, &g, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  EXPECT_EQ(g.pos, 1u); // one step was allowed while the collector tried
  ASSERT_EQ(grcore_resume(w.ctx, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_PAUSED); // still over
  ASSERT_EQ(grcore_context_set_memory_bytes(w.ctx, 2000), GRCORE_OK);
  EXPECT_FALSE(grcore_context_request_pending(w.ctx, GRCORE_REQUEST_MEMORY));
  ASSERT_EQ(grcore_resume(w.ctx, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  EXPECT_EQ(g.sum, sum_below(100));
  a->free_fn(a->ctx, big);
}

TEST(PollMemory, PastTheReserveTheAllocatorRefusesAndNothingIsCharged) {
  RunWorld w(GRCORE_UNLIMITED, 1000, 500);
  const GRCORE_Allocator * a = grcore_context_allocator(w.ctx);
  void * p = a->malloc_fn(a->ctx, 1400);
  ASSERT_NE(p, nullptr);
  uint64_t in_use = grcore_context_memory_in_use(w.ctx);
  uint64_t blocks = grcore_context_memory_blocks(w.ctx);
  EXPECT_EQ(grcore_context_memory_refusals(w.ctx), 0u);
  EXPECT_EQ(a->malloc_fn(a->ctx, 101), nullptr);
  EXPECT_EQ(grcore_context_memory_refusals(w.ctx), 1u);
  EXPECT_EQ(grcore_context_memory_in_use(w.ctx), in_use);
  EXPECT_EQ(grcore_context_memory_blocks(w.ctx), blocks);
  void * edge = a->malloc_fn(a->ctx, 100); // exactly limit + reserve
  EXPECT_NE(edge, nullptr);
  a->free_fn(a->ctx, edge);
  a->free_fn(a->ctx, p);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

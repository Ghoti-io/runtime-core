/**
 * @file
 *
 * The phase-shuffle test mode (AD-5, AD-16): handlers that commute give the
 * same verdict and key list under every seed, and a handler whose vote
 * depends on the order handlers ran in is caught.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../../src/b/context_internal.h"

#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kSeeds = 64;

/* One poll in a context whose fuel is exhausted (so the slow path runs) and
 * whose handlers are `decide`, `act` and `observe`. Returns the pause keys'
 * names, in the order reported. */
struct Outcome {
  GRCORE_Verdict verdict = GRCORE_VERDICT_CONTINUE;
  std::vector<std::string> keys;
  std::vector<std::string> decide_order;
  std::vector<std::string> observe_order;
  std::vector<std::string> act_order;
};

std::string name_of(const GRCORE_Key * k) { return k->name; }

Outcome poll_once(uint64_t seed, bool shuffle,
    const std::vector<std::pair<std::string, GRCORE_Verdict>> & deciders) {
  Outcome out;
  RunWorld w(0);
  std::vector<Probe> d, a, o;
  d.reserve(deciders.size());
  for (auto & x : deciders) {
    d.emplace_back(x.first);
    d.back().vote = x.second;
    d.back().log = &out.decide_order;
  }
  for (auto & p : d) {
    EXPECT_EQ(grcore_context_register(w.ctx, &kDecideKey, &p), GRCORE_OK);
  }
  for (const char * n : {"a1", "a2", "a3"}) {
    a.emplace_back(n);
    a.back().log = &out.act_order;
  }
  for (const char * n : {"o1", "o2", "o3", "o4"}) {
    o.emplace_back(n);
    o.back().log = &out.observe_order;
  }
  for (auto & p : a) {
    EXPECT_EQ(grcore_context_register(w.ctx, &kActKey, &p), GRCORE_OK);
  }
  for (auto & p : o) {
    EXPECT_EQ(grcore_context_register(w.ctx, &kObserveKey, &p), GRCORE_OK);
  }
  EXPECT_EQ(grcore_context_set_phase_shuffle(w.ctx, shuffle, seed), GRCORE_OK);
  Fn fn{[&](GRCORE_Context * c) {
    out.verdict = GRCORE_POLL(c);
    return out.verdict == GRCORE_VERDICT_PAUSE    ? GRCORE_STEP_PAUSED
        : out.verdict == GRCORE_VERDICT_UNWIND ? GRCORE_STEP_UNWOUND
                                               : GRCORE_STEP_FINISHED;
  }};
  grcore_context_charge_fuel(w.ctx, 1);
  GRCORE_Outcome outcome;
  EXPECT_NE(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_ERR_INTERNAL);
  for (size_t i = 0; i < grcore_context_pause_key_count(w.ctx); i++) {
    const GRCORE_Key * k = grcore_context_pause_key(w.ctx, i);
    out.keys.push_back(k == &kDecideKey ? "decide" : name_of(k));
  }
  return out;
}

/* A key per voter, so the names of the keys that voted can be told apart. */
const GRCORE_Key kV0 = {"v0", GRCORE_CARDINALITY_MANY, GRCORE_PHASE_DECIDE, nullptr, probe_handler, nullptr, nullptr, nullptr};
const GRCORE_Key kV1 = {"v1", GRCORE_CARDINALITY_MANY, GRCORE_PHASE_DECIDE, nullptr, probe_handler, nullptr, nullptr, nullptr};
const GRCORE_Key kV2 = {"v2", GRCORE_CARDINALITY_MANY, GRCORE_PHASE_DECIDE, nullptr, probe_handler, nullptr, nullptr, nullptr};
const GRCORE_Key kV3 = {"v3", GRCORE_CARDINALITY_MANY, GRCORE_PHASE_DECIDE, nullptr, probe_handler, nullptr, nullptr, nullptr};
const GRCORE_Key * const kVoters[] = {&kV0, &kV1, &kV2, &kV3};

/* Like poll_once, but the DECIDE voters are registered under their own keys,
 * so the reported key list says which of them voted. `plant` is called for
 * each voter's probe and may make its vote depend on the run. */
Outcome keyed_poll(uint64_t seed, bool shuffle,
    const std::vector<GRCORE_Verdict> & votes,
    const std::function<void(size_t, Probe &)> & plant = nullptr) {
  Outcome out;
  RunWorld w(0);
  std::vector<Probe> d;
  d.reserve(votes.size());
  for (size_t i = 0; i < votes.size(); i++) {
    d.emplace_back(kVoters[i]->name);
    d.back().vote = votes[i];
    d.back().log = &out.decide_order;
    if (plant) {
      plant(i, d.back());
    }
    EXPECT_EQ(grcore_context_register(w.ctx, kVoters[i], &d.back()), GRCORE_OK);
  }
  std::vector<Probe> o;
  for (const char * n : {"o1", "o2", "o3", "o4"}) {
    o.emplace_back(n);
    o.back().log = &out.observe_order;
  }
  for (auto & p : o) {
    EXPECT_EQ(grcore_context_register(w.ctx, &kObserveKey, &p), GRCORE_OK);
  }
  EXPECT_EQ(grcore_context_set_phase_shuffle(w.ctx, shuffle, seed), GRCORE_OK);
  Fn fn{[&](GRCORE_Context * c) {
    out.verdict = GRCORE_POLL(c);
    return out.verdict == GRCORE_VERDICT_PAUSE    ? GRCORE_STEP_PAUSED
        : out.verdict == GRCORE_VERDICT_UNWIND ? GRCORE_STEP_UNWOUND
                                               : GRCORE_STEP_FINISHED;
  }};
  grcore_context_charge_fuel(w.ctx, 1);
  GRCORE_Outcome outcome;
  EXPECT_NE(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_ERR_INTERNAL);
  for (size_t i = 0; i < grcore_context_pause_key_count(w.ctx); i++) {
    out.keys.push_back(name_of(grcore_context_pause_key(w.ctx, i)));
  }
  return out;
}

} // namespace

TEST(Shuffle, CommutingHandlersGiveTheSameVerdictAndKeysUnderEverySeed) {
  std::vector<GRCORE_Verdict> votes = {GRCORE_VERDICT_PAUSE,
      GRCORE_VERDICT_CONTINUE, GRCORE_VERDICT_PAUSE, GRCORE_VERDICT_PAUSE};
  Outcome base = keyed_poll(0, false, votes);
  EXPECT_EQ(base.verdict, GRCORE_VERDICT_PAUSE);
  EXPECT_EQ(base.keys, (std::vector<std::string>{"fuel", "v0", "v2", "v3"}));
  std::set<std::vector<std::string>> decide_orders, observe_orders;
  for (int seed = 1; seed <= kSeeds; seed++) {
    Outcome o = keyed_poll(static_cast<uint64_t>(seed), true, votes);
    EXPECT_EQ(o.verdict, base.verdict) << "seed " << seed;
    EXPECT_EQ(o.keys, base.keys) << "seed " << seed;
    EXPECT_EQ(o.decide_order.size(), 4u);
    EXPECT_EQ(o.observe_order.size(), 4u);
    decide_orders.insert(o.decide_order);
    observe_orders.insert(o.observe_order);
  }
  // The mode really shuffled: 64 seeds over 24 orders did not give one.
  EXPECT_GT(decide_orders.size(), 5u);
  EXPECT_GT(observe_orders.size(), 5u);
}

TEST(Shuffle, WithTheModeOffHandlersRunInRegistrationOrder) {
  std::vector<GRCORE_Verdict> votes(4, GRCORE_VERDICT_CONTINUE);
  for (uint64_t seed : {0, 1, 99}) {
    Outcome o = keyed_poll(seed, false, votes);
    EXPECT_EQ(o.decide_order, (std::vector<std::string>{"v0", "v1", "v2", "v3"}));
    EXPECT_EQ(o.observe_order, (std::vector<std::string>{"o1", "o2", "o3", "o4"}));
  }
}

TEST(Shuffle, ActHandlersKeepRegistrationOrderEvenWithTheModeOn) {
  for (int seed = 1; seed <= kSeeds; seed++) {
    Outcome o = poll_once(static_cast<uint64_t>(seed), true,
        {{"x", GRCORE_VERDICT_PAUSE}});
    EXPECT_EQ(o.act_order, (std::vector<std::string>{"a1", "a2", "a3"}))
        << "seed " << seed;
  }
}

TEST(Shuffle, TheSameSeedGivesTheSameOrders) {
  std::vector<GRCORE_Verdict> votes(4, GRCORE_VERDICT_CONTINUE);
  Outcome a = keyed_poll(12345, true, votes);
  Outcome b = keyed_poll(12345, true, votes);
  EXPECT_EQ(a.decide_order, b.decide_order);
  EXPECT_EQ(a.observe_order, b.observe_order);
}

TEST(Shuffle, TheOrderChangesFromPollToPollWithinOneRun) {
  RunWorld w(GRCORE_UNLIMITED);
  std::vector<std::string> order;
  std::vector<Probe> o;
  o.reserve(5);
  for (const char * n : {"o1", "o2", "o3", "o4", "o5"}) {
    o.emplace_back(n);
    o.back().log = &order;
  }
  for (auto & p : o) {
    ASSERT_EQ(grcore_context_register(w.ctx, &kObserveKey, &p), GRCORE_OK);
  }
  GRCORE_Port * port;
  ASSERT_EQ(grcore_context_port(w.ctx, &port), GRCORE_OK);
  GRCORE_RequestKind kind;
  ASSERT_EQ(grcore_context_request_kind(w.ctx, &kObserveKey, &kind), GRCORE_OK);
  ASSERT_EQ(grcore_port_post(port, kind), GRCORE_OK);
  ASSERT_EQ(grcore_context_set_phase_shuffle(w.ctx, true, 7), GRCORE_OK);
  Fn fn{[&](GRCORE_Context * c) {
    for (int i = 0; i < 6; i++) {
      GRCORE_POLL(c);
    }
    return GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  ASSERT_EQ(order.size(), 30u);
  std::set<std::vector<std::string>> polls;
  for (size_t i = 0; i < 6; i++) {
    polls.insert(std::vector<std::string>(order.begin() + i * 5, order.begin() + i * 5 + 5));
  }
  EXPECT_GT(polls.size(), 1u);
  grcore_port_release(port);
}

TEST(Shuffle, AnOrderDependentHandlerIsSeenToGiveTwoDifferentKeyLists) {
  // Each voter pauses only if it ran first. That is exactly a handler whose
  // vote depends on the others, which AD-5 forbids and this mode finds.
  std::vector<GRCORE_Verdict> votes(4, GRCORE_VERDICT_CONTINUE);
  std::set<std::vector<std::string>> key_lists;
  for (int seed = 1; seed <= kSeeds; seed++) {
    int ran = 0;
    Outcome o = keyed_poll(static_cast<uint64_t>(seed), true, votes,
        [&](size_t, Probe & p) {
          p.extra = [&ran, &p](GRCORE_Context *, GRCORE_PollCall *) {
            p.vote = ran++ == 0 ? GRCORE_VERDICT_PAUSE : GRCORE_VERDICT_CONTINUE;
          };
        });
    key_lists.insert(o.keys);
  }
  EXPECT_GE(key_lists.size(), 2u);
}

TEST(Shuffle, TheSamePlantedHandlerLooksCommutingWithTheModeOff) {
  // The control: without the shuffle the planted defect is invisible, which is
  // why the mode exists.
  std::vector<GRCORE_Verdict> votes(4, GRCORE_VERDICT_CONTINUE);
  std::set<std::vector<std::string>> key_lists;
  for (int seed = 1; seed <= kSeeds; seed++) {
    int ran = 0;
    Outcome o = keyed_poll(static_cast<uint64_t>(seed), false, votes,
        [&](size_t, Probe & p) {
          p.extra = [&ran, &p](GRCORE_Context *, GRCORE_PollCall *) {
            p.vote = ran++ == 0 ? GRCORE_VERDICT_PAUSE : GRCORE_VERDICT_CONTINUE;
          };
        });
    key_lists.insert(o.keys);
  }
  EXPECT_EQ(key_lists.size(), 1u);
}

TEST(Shuffle, UnwindVotersAndTheirResultAreOrderIndependentToo) {
  std::vector<GRCORE_Verdict> votes = {GRCORE_VERDICT_UNWIND,
      GRCORE_VERDICT_PAUSE, GRCORE_VERDICT_UNWIND, GRCORE_VERDICT_CONTINUE};
  Outcome base = keyed_poll(0, false, votes,
      [](size_t i, Probe & p) {
        p.unwind_result = i == 0 ? GRCORE_ERR_GUEST : GRCORE_ERR_LIMIT;
      });
  EXPECT_EQ(base.verdict, GRCORE_VERDICT_UNWIND);
  EXPECT_EQ(base.keys, (std::vector<std::string>{"v0", "v2"}));
  for (int seed = 1; seed <= kSeeds; seed++) {
    Outcome o = keyed_poll(static_cast<uint64_t>(seed), true, votes,
        [](size_t i, Probe & p) {
          p.unwind_result = i == 0 ? GRCORE_ERR_GUEST : GRCORE_ERR_LIMIT;
        });
    EXPECT_EQ(o.keys, base.keys) << "seed " << seed;
  }
}

TEST(Shuffle, SettingTheModeIsRefusedForNonOwnersAndNull) {
  RunWorld w;
  EXPECT_EQ(grcore_context_set_phase_shuffle(nullptr, true, 1), GRCORE_ERR_INVALID);
  GRCORE_Result other = GRCORE_OK;
  std::thread([&] { other = grcore_context_set_phase_shuffle(w.ctx, true, 1); }).join();
  EXPECT_EQ(other, GRCORE_ERR_INVALID);
  EXPECT_FALSE(w.ctx->shuffle);
}


namespace {

/* A request kind owned by a service that clears it in its own handler (as the
 * profiler, the debugger and the JIT do), and observers that ask whether it
 * was pending. `extra_kinds` defines that many kinds first, so the service's
 * kind can land in the overflow set (63 and up). */
struct ClearWorld {
  struct Seen {
    bool pending = false;
    size_t slot = 0; ///< Position in the poll's run order.
  };
  Outcome out;
  std::vector<Seen> seen;
  size_t clearer_slot = 0;
  GRCORE_RequestKind kind = 0;
  bool live_pending_after = true;
  bool via_live_read = false; ///< Observers use the context read, not the call.
  size_t order = 0;
};

ClearWorld clear_poll(uint64_t seed, int extra_kinds, bool via_live_read) {
  ClearWorld cw;
  cw.via_live_read = via_live_read;
  cw.seen.resize(3);
  RunWorld w;
  for (int i = 0; i < extra_kinds; i++) {
    GRCORE_RequestKind k;
    EXPECT_EQ(grcore_context_request_kind(w.ctx, &kObserveKey, &k), GRCORE_OK);
  }
  static const GRCORE_Key kOwner = {"owner", GRCORE_CARDINALITY_ONE,
      GRCORE_PHASE_OBSERVE, nullptr, probe_handler, nullptr, nullptr, nullptr};
  EXPECT_EQ(grcore_context_request_kind(w.ctx, &kOwner, &cw.kind), GRCORE_OK);
  Probe owner("owner");
  owner.extra = [&](GRCORE_Context * c, GRCORE_PollCall *) {
    cw.clearer_slot = cw.order++;
    EXPECT_EQ(grcore_context_clear_request(c, cw.kind), GRCORE_OK);
  };
  std::vector<Probe> watchers;
  watchers.reserve(3);
  for (size_t i = 0; i < 3; i++) {
    watchers.emplace_back("w" + std::to_string(i));
    watchers.back().extra = [&cw, i](GRCORE_Context * c, GRCORE_PollCall * call) {
      cw.seen[i].slot = cw.order++;
      cw.seen[i].pending = cw.via_live_read
          ? grcore_context_request_pending(c, cw.kind)
          : grcore_pollcall_pending(call, cw.kind);
    };
  }
  EXPECT_EQ(grcore_context_register(w.ctx, &kOwner, &owner), GRCORE_OK);
  for (auto & p : watchers) {
    EXPECT_EQ(grcore_context_register(w.ctx, &kObserveKey, &p), GRCORE_OK);
  }
  GRCORE_Port * port;
  EXPECT_EQ(grcore_context_port(w.ctx, &port), GRCORE_OK);
  EXPECT_EQ(grcore_port_post(port, cw.kind), GRCORE_OK);
  EXPECT_EQ(grcore_context_set_phase_shuffle(w.ctx, true, seed), GRCORE_OK);
  Fn fn{[&](GRCORE_Context * c) {
    GRCORE_POLL(c);
    return GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  EXPECT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  cw.live_pending_after = grcore_context_request_pending(w.ctx, cw.kind);
  grcore_port_release(port);
  return cw;
}

void expect_every_observer_told_pending(int extra_kinds) {
  bool some_after = false, some_before = false;
  for (int seed = 1; seed <= kSeeds; seed++) {
    ClearWorld cw = clear_poll(static_cast<uint64_t>(seed), extra_kinds, false);
    for (size_t i = 0; i < 3; i++) {
      EXPECT_TRUE(cw.seen[i].pending) << "seed " << seed << " observer " << i;
      (cw.seen[i].slot > cw.clearer_slot ? some_after : some_before) = true;
    }
    // The request is still the service's to clear: it is gone afterwards.
    EXPECT_FALSE(cw.live_pending_after) << "seed " << seed;
  }
  // The control that the mode did put observers on both sides of the clear.
  EXPECT_TRUE(some_after);
  EXPECT_TRUE(some_before);
}

} // namespace

TEST(Shuffle, AServiceClearingItsOwnRequestDoesNotHideItFromTheObserversAfterIt) {
  expect_every_observer_told_pending(0);
}

TEST(Shuffle, TheSameHoldsForAKindInTheOverflowSet) {
  expect_every_observer_told_pending(70);
}

TEST(Shuffle, ReadingTheLiveRequestInsteadOfTheCallIsOrderDependent) {
  // The control: this is what the call's answer used to be, and the mode
  // catches it as two different answers for one poll.
  std::set<std::vector<bool>> answers;
  for (int seed = 1; seed <= kSeeds; seed++) {
    ClearWorld cw = clear_poll(static_cast<uint64_t>(seed), 0, true);
    answers.insert({cw.seen[0].pending, cw.seen[1].pending, cw.seen[2].pending});
  }
  EXPECT_GE(answers.size(), 2u);
}

TEST(Shuffle, ARequestPostedDuringAPollIsToldAtTheNextOneNotThisOne) {
  RunWorld w;
  static const GRCORE_Key kEarly = {"early", GRCORE_CARDINALITY_ONE,
      GRCORE_PHASE_OBSERVE, nullptr, probe_handler, nullptr, nullptr, nullptr};
  static const GRCORE_Key kLate = {"late", GRCORE_CARDINALITY_ONE,
      GRCORE_PHASE_OBSERVE, nullptr, nullptr, nullptr, nullptr, nullptr};
  GRCORE_RequestKind early, late;
  ASSERT_EQ(grcore_context_request_kind(w.ctx, &kEarly, &early), GRCORE_OK);
  ASSERT_EQ(grcore_context_request_kind(w.ctx, &kLate, &late), GRCORE_OK);
  GRCORE_Port * port;
  ASSERT_EQ(grcore_context_port(w.ctx, &port), GRCORE_OK);
  std::vector<int> told;
  Probe p("early");
  p.extra = [&](GRCORE_Context *, GRCORE_PollCall * call) {
    told.push_back(grcore_pollcall_pending(call, late) ? 1 : 0);
    if (told.size() == 1) {
      EXPECT_EQ(grcore_port_post(port, late), GRCORE_OK);
      EXPECT_FALSE(grcore_pollcall_pending(call, late)); // not this poll's
    }
  };
  ASSERT_EQ(grcore_context_register(w.ctx, &kEarly, &p), GRCORE_OK);
  ASSERT_EQ(grcore_port_post(port, early), GRCORE_OK);
  Fn fn{[&](GRCORE_Context * c) {
    GRCORE_POLL(c);
    GRCORE_POLL(c);
    return GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_EQ(told, (std::vector<int>{0, 1}));
  grcore_port_release(port);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

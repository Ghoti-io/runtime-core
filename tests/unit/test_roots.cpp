/**
 * @file
 *
 * Root sources: the registry in B (AD-18). A's own source is tested in
 * test_a_roots.cpp.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../../src/b/context_internal.h"

#include <string>
#include <thread>
#include <vector>

namespace {

/* A source that reports what it was given: a named slot and a range, and
 * logs which source ran. */
struct Held {
  std::string name;
  uint64_t slot = 0;
  std::vector<std::string> * log = nullptr;
};

void held_enumerate(
    GRCORE_Context *, void * value, const GRCORE_RootVisitor * visitor) {
  auto * h = static_cast<Held *>(value);
  if (h->log != nullptr) {
    h->log->push_back(h->name);
  }
  if (visitor->slot != nullptr) {
    visitor->slot(visitor->user, &h->slot);
  }
  if (visitor->range != nullptr) {
    GRCORE_ConservativeRange r = {0x1000, 0x2000, 0xFFFF, 0, 0x10};
    visitor->range(visitor->user, &r);
  }
}

const GRCORE_RootSource kSourceA = {"a", held_enumerate};
const GRCORE_RootSource kSourceB = {"b", held_enumerate};

struct Seen {
  std::vector<uint64_t *> slots;
  std::vector<GRCORE_ConservativeRange> ranges;
};

GRCORE_RootVisitor visitor_for(Seen * seen) {
  GRCORE_RootVisitor v;
  v.user = seen;
  v.slot = [](void * user, uint64_t * slot) {
    static_cast<Seen *>(user)->slots.push_back(slot);
  };
  v.range = [](void * user, const GRCORE_ConservativeRange * r) {
    static_cast<Seen *>(user)->ranges.push_back(*r);
  };
  return v;
}

} // namespace

TEST(Roots, AddCountEnumerateAndRemoveInRegistrationOrder) {
  RunWorld w;
  std::vector<std::string> log;
  Held a{"a", 11, &log}, b{"b", 22, &log};
  EXPECT_EQ(grcore_context_root_source_count(w.ctx), 0u);
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kSourceA, &a), GRCORE_OK);
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kSourceB, &b), GRCORE_OK);
  EXPECT_EQ(grcore_context_root_source_count(w.ctx), 2u);

  Seen seen;
  GRCORE_RootVisitor v = visitor_for(&seen);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  EXPECT_EQ(log, (std::vector<std::string>{"a", "b"}));
  ASSERT_EQ(seen.slots.size(), 2u);
  EXPECT_EQ(seen.slots[0], &a.slot);
  EXPECT_EQ(seen.slots[1], &b.slot);
  ASSERT_EQ(seen.ranges.size(), 2u);
  EXPECT_EQ(seen.ranges[0].lo, 0x1000u);
  EXPECT_EQ(seen.ranges[0].hi, 0x2000u);
  EXPECT_EQ(seen.ranges[0].mask, 0xFFFFu);
  EXPECT_EQ(seen.ranges[0].shift, 0u);
  EXPECT_EQ(seen.ranges[0].base, 0x10u);

  // Remove takes out the named one only.
  ASSERT_EQ(grcore_context_remove_root_source(w.ctx, &kSourceA, &a), GRCORE_OK);
  EXPECT_EQ(grcore_context_root_source_count(w.ctx), 1u);
  log.clear();
  seen = Seen{};
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  EXPECT_EQ(log, std::vector<std::string>{"b"});
}

TEST(Roots, ASourceCanBeReadByIndexAndEnumeratedOnItsOwn) {
  RunWorld w;
  Held a{"a", 11, nullptr}, b{"b", 22, nullptr};
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kSourceA, &a), GRCORE_OK);
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kSourceB, &b), GRCORE_OK);
  const GRCORE_RootSource * source = nullptr;
  void * value = nullptr;
  ASSERT_EQ(grcore_context_root_source(w.ctx, 1, &source, &value), GRCORE_OK);
  EXPECT_EQ(source, &kSourceB);
  EXPECT_EQ(value, &b);
  EXPECT_STREQ(source->name, "b");
  // Either output may be omitted.
  EXPECT_EQ(grcore_context_root_source(w.ctx, 0, nullptr, &value), GRCORE_OK);
  EXPECT_EQ(value, &a);
  EXPECT_EQ(grcore_context_root_source(w.ctx, 0, &source, nullptr), GRCORE_OK);
  EXPECT_EQ(source, &kSourceA);
  // Calling the source's own enumerate reports only its roots.
  Seen seen;
  GRCORE_RootVisitor v = visitor_for(&seen);
  source->enumerate(w.ctx, &a, &v);
  ASSERT_EQ(seen.slots.size(), 1u);
  EXPECT_EQ(seen.slots[0], &a.slot);
}

TEST(Roots, ReadingASourceOutOfRangeOrFromAnotherThreadIsRefusedAndWritesNothing) {
  RunWorld w;
  Held a{"a", 11, nullptr};
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kSourceA, &a), GRCORE_OK);
  const GRCORE_RootSource * source = &kSourceB;
  void * value = &a;
  EXPECT_EQ(grcore_context_root_source(w.ctx, 1, &source, &value),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_root_source(nullptr, 0, &source, &value),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(source, &kSourceB);
  EXPECT_EQ(value, &a);
  GRCORE_Result r = GRCORE_OK;
  std::thread([&] { r = grcore_context_root_source(w.ctx, 0, &source, &value); })
      .join();
  EXPECT_EQ(r, GRCORE_ERR_INVALID);
  EXPECT_EQ(source, &kSourceB);
}

TEST(Roots, AVisitorMayRewriteAPreciseSlotAndLaterReadsSeeIt) {
  RunWorld w;
  Held a{"a", 5, nullptr};
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kSourceA, &a), GRCORE_OK);
  GRCORE_RootVisitor v = {};
  v.slot = [](void *, uint64_t * slot) { *slot += 1000; };
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  EXPECT_EQ(a.slot, 1005u);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  EXPECT_EQ(a.slot, 2005u);
}

TEST(Roots, ASourceSkipsWhatTheVisitorDoesNotWant) {
  RunWorld w;
  Held a{"a", 5, nullptr};
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kSourceA, &a), GRCORE_OK);
  GRCORE_RootVisitor only_slots = {};
  int slots = 0;
  only_slots.user = &slots;
  only_slots.slot = [](void * u, uint64_t *) { ++*static_cast<int *>(u); };
  EXPECT_EQ(grcore_context_enumerate_roots(w.ctx, &only_slots), GRCORE_OK);
  EXPECT_EQ(slots, 1);
  GRCORE_RootVisitor none = {};
  EXPECT_EQ(grcore_context_enumerate_roots(w.ctx, &none), GRCORE_OK);
}

TEST(Roots, TheSameSourceWithAnotherValueIsADifferentRegistration) {
  RunWorld w;
  Held a1{"a1"}, a2{"a2"};
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kSourceA, &a1), GRCORE_OK);
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kSourceA, &a2), GRCORE_OK);
  EXPECT_EQ(grcore_context_root_source_count(w.ctx), 2u);
  ASSERT_EQ(grcore_context_remove_root_source(w.ctx, &kSourceA, &a1), GRCORE_OK);
  Seen seen;
  GRCORE_RootVisitor v = visitor_for(&seen);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  ASSERT_EQ(seen.slots.size(), 1u);
  EXPECT_EQ(seen.slots[0], &a2.slot);
}

TEST(Roots, RefusalsAreInvalidAndChangeNothing) {
  RunWorld w;
  Held a{"a"};
  const GRCORE_RootSource no_enumerate = {"none", nullptr};
  EXPECT_EQ(grcore_context_add_root_source(nullptr, &kSourceA, &a), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_add_root_source(w.ctx, nullptr, &a), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_add_root_source(w.ctx, &no_enumerate, &a), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_root_source_count(w.ctx), 0u);

  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kSourceA, &a), GRCORE_OK);
  // The same source and value again.
  EXPECT_EQ(grcore_context_add_root_source(w.ctx, &kSourceA, &a), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_root_source_count(w.ctx), 1u);

  // Removing what is not there, or with NULLs.
  Held other{"other"};
  EXPECT_EQ(grcore_context_remove_root_source(w.ctx, &kSourceA, &other), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_remove_root_source(w.ctx, &kSourceB, &a), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_remove_root_source(w.ctx, nullptr, &a), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_remove_root_source(nullptr, &kSourceA, &a), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_root_source_count(w.ctx), 1u);

  GRCORE_RootVisitor v = {};
  EXPECT_EQ(grcore_context_enumerate_roots(w.ctx, nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_enumerate_roots(nullptr, &v), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_root_source_count(nullptr), 0u);
}

TEST(Roots, ANonOwnerCannotAddRemoveOrEnumerate) {
  RunWorld w;
  Held a{"a"}, b{"b"};
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kSourceA, &a), GRCORE_OK);
  GRCORE_Result add = GRCORE_OK, remove = GRCORE_OK, enumerate = GRCORE_OK;
  std::thread([&] {
    GRCORE_RootVisitor v = {};
    add = grcore_context_add_root_source(w.ctx, &kSourceB, &b);
    remove = grcore_context_remove_root_source(w.ctx, &kSourceA, &a);
    enumerate = grcore_context_enumerate_roots(w.ctx, &v);
  }).join();
  EXPECT_EQ(add, GRCORE_ERR_INVALID);
  EXPECT_EQ(remove, GRCORE_ERR_INVALID);
  EXPECT_EQ(enumerate, GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_root_source_count(w.ctx), 1u);
}

TEST(Roots, TheTableGrowsPastItsFirstCapacityAndKeepsTheOrder) {
  RunWorld w;
  std::vector<std::string> log;
  std::vector<Held> held(20);
  for (size_t i = 0; i < held.size(); i++) {
    held[i].name = std::to_string(i);
    held[i].log = &log;
    ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kSourceA, &held[i]), GRCORE_OK);
  }
  EXPECT_EQ(grcore_context_root_source_count(w.ctx), 20u);
  GRCORE_RootVisitor v = {};
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  ASSERT_EQ(log.size(), 20u);
  for (size_t i = 0; i < 20; i++) {
    EXPECT_EQ(log[i], std::to_string(i));
  }
  // Removing from the middle keeps the rest in order.
  ASSERT_EQ(grcore_context_remove_root_source(w.ctx, &kSourceA, &held[7]), GRCORE_OK);
  log.clear();
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  ASSERT_EQ(log.size(), 19u);
  EXPECT_EQ(log[6], "6");
  EXPECT_EQ(log[7], "8");
}

TEST(Roots, EveryAllocationFailureOfAddIsRefusedAndLeavesTheTableUnchanged) {
  bool succeeded = false;
  int failures = 0;
  for (long n = 1; n < 10 && !succeeded; n++) {
    TrackingAllocator t;
    {
      RunWorld w(GRCORE_UNLIMITED, GRCORE_UNLIMITED,
          GRCORE_DEFAULT_MEMORY_RESERVE, GRCORE_UNLIMITED, GRCORE_UNLIMITED,
          t.get());
      Held a{"a"};
      t.fail_at = t.calls + n;
      GRCORE_Result r = grcore_context_add_root_source(w.ctx, &kSourceA, &a);
      t.fail_at = 0;
      if (r == GRCORE_OK) {
        succeeded = true;
        EXPECT_EQ(grcore_context_root_source_count(w.ctx), 1u);
      } else {
        failures++;
        EXPECT_EQ(r, GRCORE_ERR_OOM) << "n=" << n;
        EXPECT_EQ(grcore_context_root_source_count(w.ctx), 0u) << "n=" << n;
        EXPECT_EQ(grcore_context_memory_blocks(w.ctx), 0u) << "n=" << n;
      }
    }
    EXPECT_EQ(t.live, 0) << "n=" << n;
  }
  EXPECT_TRUE(succeeded);
  EXPECT_GE(failures, 1);
}

TEST(Roots, AGrowthTheBudgetRefusesIsALimitAndLeavesTheOldTableWorking) {
  RunWorld w(GRCORE_UNLIMITED, GRCORE_UNLIMITED, 0); // no reserve
  std::vector<Held> held(5);
  for (size_t i = 0; i < 4; i++) {
    ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kSourceA, &held[i]), GRCORE_OK);
  }
  // The table is full at its first capacity of four. Pin the budget to what is
  // in use, so the doubled table has no room.
  ASSERT_EQ(grcore_context_set_memory_bytes(w.ctx, grcore_context_memory_in_use(w.ctx)),
      GRCORE_OK);
  uint64_t refusals = grcore_context_memory_refusals(w.ctx);
  EXPECT_EQ(grcore_context_add_root_source(w.ctx, &kSourceA, &held[4]),
      GRCORE_ERR_LIMIT);
  EXPECT_GT(grcore_context_memory_refusals(w.ctx), refusals);
  EXPECT_EQ(grcore_context_root_source_count(w.ctx), 4u);
  int calls = 0;
  GRCORE_RootVisitor v = {};
  v.user = &calls;
  v.slot = [](void * u, uint64_t *) { ++*static_cast<int *>(u); };
  EXPECT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  EXPECT_EQ(calls, 4);
  // Raising the budget lets the same add succeed.
  ASSERT_EQ(grcore_context_set_memory_bytes(w.ctx, GRCORE_UNLIMITED), GRCORE_OK);
  EXPECT_EQ(grcore_context_add_root_source(w.ctx, &kSourceA, &held[4]), GRCORE_OK);
}

TEST(Roots, EnumerationWorksInEveryStateTheOwnerCanBeIn) {
  RunWorld w(3);
  Held a{"a", 1};
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kSourceA, &a), GRCORE_OK);
  int calls = 0;
  GRCORE_RootVisitor v = {};
  v.user = &calls;
  v.slot = [](void * u, uint64_t *) { ++*static_cast<int *>(u); };
  Fn fn{[&](GRCORE_Context * c) {
    EXPECT_EQ(grcore_context_enumerate_roots(c, &v), GRCORE_OK); // running
    grcore_context_charge_fuel(c, 10);
    return GRCORE_POLL(c) == GRCORE_VERDICT_PAUSE ? GRCORE_STEP_PAUSED
                                                  : GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  EXPECT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK); // paused
  EXPECT_EQ(calls, 2);
  grcore_context_set_fuel(w.ctx, GRCORE_UNLIMITED);
  ASSERT_EQ(grcore_resume(w.ctx, &outcome), GRCORE_OK); // enumerates again, running
  EXPECT_EQ(calls, 3);
  EXPECT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK); // parked
  EXPECT_EQ(calls, 4);
}

TEST(Roots, AContextDestroyedWithSourcesStillOpenFreesEverything) {
  TrackingAllocator t;
  {
    RunWorld w(GRCORE_UNLIMITED, GRCORE_UNLIMITED,
        GRCORE_DEFAULT_MEMORY_RESERVE, GRCORE_UNLIMITED, GRCORE_UNLIMITED,
        t.get());
    std::vector<Held> held(9);
    for (auto & h : held) {
      ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kSourceA, &h), GRCORE_OK);
    }
  }
  EXPECT_EQ(t.live, 0);
}

TEST(Roots, RemovingTheLastSourceLeavesNothingCharged) {
  RunWorld w;
  Held a{"a"};
  uint64_t before = grcore_context_memory_blocks(w.ctx);
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kSourceA, &a), GRCORE_OK);
  EXPECT_GT(grcore_context_memory_blocks(w.ctx), before);
  ASSERT_EQ(grcore_context_remove_root_source(w.ctx, &kSourceA, &a), GRCORE_OK);
  EXPECT_EQ(grcore_context_memory_blocks(w.ctx), before);
  // And a source can be added again after the table was released.
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kSourceA, &a), GRCORE_OK);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

/**
 * @file
 *
 * The sampling profiler (b/profile.h): samples taken by posting, attribution
 * to the innermost frame and to every distinct location, the fixed table, the
 * commuting OBSERVE handler, the timer and its teardown, and the refusals.
 *
 * The engine is "alpha" from test_helpers.h: a poll identity (f, o) is the
 * line f * 1000 + o of alpha.src, so a test names the place it wants.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

namespace {

constexpr int line(uint64_t f, uint64_t o) { return int(f * 1000 + o); }

struct Report {
  std::vector<GRCORE_ProfileEntry> entries;
  GRCORE_ProfileTotals totals{};
};

Report read(const GRCORE_Profiler * p, size_t room = 64) {
  Report r;
  r.entries.resize(room);
  size_t n = 0;
  EXPECT_EQ(grcore_profiler_report(p, r.entries.data(), room, &n, &r.totals),
      GRCORE_OK);
  r.entries.resize(n);
  return r;
}

const GRCORE_ProfileEntry * find(const Report & r, int l) {
  for (const GRCORE_ProfileEntry & e : r.entries) {
    if (e.line == l && std::strcmp(e.file, kAlphaFile) == 0) {
      return &e;
    }
  }
  return nullptr;
}

/* A guest on the guest stack. Each step pushes `depth` frames of function 1
 * (the outer ones with call-site identity (1, 5)), a callee of function 2 on
 * top if `callee`, posts the sample request if `request`, and polls at
 * (2, `offset`) or (1, `offset`). */
struct Guest {
  GRCORE_EngineId engine = 0;
  uint64_t steps = 1;
  uint64_t depth = 1;
  bool callee = false;
  bool request = true;
  GRCORE_Profiler * profiler = nullptr;
  std::function<uint64_t(uint64_t)> offset = [](uint64_t) { return 7u; };
  std::function<void(uint64_t)> before_poll;
  uint64_t done = 0;
};

GRCORE_Step guest_entry(GRCORE_Context * c, void * state) {
  auto * g = static_cast<Guest *>(state);
  GRCORE_Stack * s = grcore_context_stack(c);
  GRCORE_EngineId alpha = g->engine;
  for (; g->done < g->steps; g->done++) {
    for (uint64_t d = 0; d < g->depth; d++) {
      GRCORE_FrameRef f;
      EXPECT_EQ(grcore_stack_push(s, alpha, 2, &f), GRCORE_OK);
      if (d + 1 < g->depth || g->callee) {
        grcore_stack_set_identity(s, f, GRCORE_PollIdentity{1, 5});
      }
    }
    if (g->callee) {
      GRCORE_FrameRef f;
      EXPECT_EQ(grcore_stack_push(s, alpha, 2, &f), GRCORE_OK);
    }
    if (g->before_poll) {
      g->before_poll(g->done);
    }
    if (g->request) {
      EXPECT_EQ(grcore_profiler_request(g->profiler), GRCORE_OK);
    }
    GRCORE_Verdict v = grcore_stack_poll(
        c, g->callee ? 2 : 1, g->offset(g->done));
    for (uint64_t d = 0; d < g->depth + (g->callee ? 1 : 0); d++) {
      grcore_stack_pop(s);
    }
    if (v != GRCORE_VERDICT_CONTINUE) {
      return GRCORE_STEP_UNWOUND;
    }
  }
  return GRCORE_STEP_FINISHED;
}

GRCORE_Result run_guest(GRCORE_Context * c, Guest * g) {
  GRCORE_Outcome outcome;
  GRCORE_Result r = grcore_run(c, guest_entry, g, &outcome);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  return r;
}

} // namespace

TEST(Profiler, ARequestPostedBeforeEachPollGivesExactlyOneSampleAtTheInnermostFrame) {
  StackWorld w;
  GRCORE_Profiler * p = nullptr;
  ASSERT_EQ(grcore_profiler_attach(w.ctx, 0, &p), GRCORE_OK);
  Guest g;
  g.engine = w.alpha;
  g.steps = 25;
  g.profiler = p;
  ASSERT_EQ(run_guest(w.ctx, &g), GRCORE_OK);
  Report r = read(p);
  EXPECT_EQ(r.totals.samples, 25u);
  EXPECT_EQ(r.totals.no_frame, 0u);
  EXPECT_EQ(r.totals.dropped, 0u);
  ASSERT_EQ(r.entries.size(), 1u);
  EXPECT_EQ(r.entries[0].line, line(1, 7));
  EXPECT_EQ(r.entries[0].self, 25u);
  EXPECT_EQ(r.entries[0].inclusive, 25u);
}

TEST(Profiler, NoSampleIsTakenWithoutARequestEvenWhenEveryPollIsSlow) {
  StackWorld w;
  GRCORE_Profiler * p = nullptr;
  ASSERT_EQ(grcore_profiler_attach(w.ctx, 0, &p), GRCORE_OK);
  // Another service's kind keeps the poll on its slow path, so the profiler's
  // handler runs on every poll and has to decide there is nothing to do.
  Probe observe("observe");
  ASSERT_EQ(grcore_context_register(w.ctx, &kObserveKey, &observe), GRCORE_OK);
  GRCORE_RequestKind other = 0;
  ASSERT_EQ(grcore_context_request_kind(w.ctx, &kObserveKey, &other), GRCORE_OK);
  GRCORE_Port * port = nullptr;
  ASSERT_EQ(grcore_context_port(w.ctx, &port), GRCORE_OK);
  Guest g;
  g.engine = w.alpha;
  g.steps = 10;
  g.request = false;
  g.before_poll = [&](uint64_t) { grcore_port_post(port, other); };
  ASSERT_EQ(run_guest(w.ctx, &g), GRCORE_OK);
  grcore_port_release(port);
  EXPECT_EQ(observe.calls, 10);
  Report r = read(p);
  EXPECT_EQ(r.totals.samples, 0u);
  EXPECT_TRUE(r.entries.empty());
}

TEST(Profiler, ASampleClearsItsOwnRequestSoTheNextPollTakesNoSample) {
  StackWorld w;
  GRCORE_Profiler * p = nullptr;
  ASSERT_EQ(grcore_profiler_attach(w.ctx, 0, &p), GRCORE_OK);
  Guest g;
  g.engine = w.alpha;
  g.steps = 1;
  g.profiler = p;
  ASSERT_EQ(run_guest(w.ctx, &g), GRCORE_OK);
  EXPECT_FALSE(grcore_context_request_pending(w.ctx, grcore_profiler_kind(p)));
  // Two more polls with no new request add nothing.
  Guest quiet;
  quiet.engine = w.alpha;
  quiet.steps = 2;
  quiet.request = false;
  ASSERT_EQ(run_guest(w.ctx, &quiet), GRCORE_OK);
  EXPECT_EQ(read(p).totals.samples, 1u);
}

TEST(Profiler, SelfGoesToTheInnermostFrameAndInclusiveToEveryLocationOnTheStack) {
  StackWorld w;
  GRCORE_Profiler * p = nullptr;
  ASSERT_EQ(grcore_profiler_attach(w.ctx, 0, &p), GRCORE_OK);
  Guest g;
  g.engine = w.alpha;
  g.steps = 4;
  g.callee = true; // one caller frame at (1,5), a callee polling at (2,7)
  g.profiler = p;
  ASSERT_EQ(run_guest(w.ctx, &g), GRCORE_OK);
  Report r = read(p);
  const GRCORE_ProfileEntry * callee = find(r, line(2, 7));
  const GRCORE_ProfileEntry * caller = find(r, line(1, 5));
  ASSERT_NE(callee, nullptr);
  ASSERT_NE(caller, nullptr);
  EXPECT_EQ(callee->self, 4u);
  EXPECT_EQ(callee->inclusive, 4u);
  EXPECT_EQ(caller->self, 0u);
  EXPECT_EQ(caller->inclusive, 4u);
  EXPECT_EQ(r.entries.size(), 2u);
  EXPECT_EQ(r.entries[0].line, line(2, 7)); // hottest first: self 4 before 0
}

TEST(Profiler, ARecursiveFunctionCountsOncePerSampleInclusively) {
  StackWorld w;
  GRCORE_Profiler * p = nullptr;
  ASSERT_EQ(grcore_profiler_attach(w.ctx, 0, &p), GRCORE_OK);
  Guest g;
  g.engine = w.alpha;
  g.steps = 3;
  g.depth = 6; // five frames at call site (1,5) and the innermost at (1,7)
  g.profiler = p;
  ASSERT_EQ(run_guest(w.ctx, &g), GRCORE_OK);
  Report r = read(p);
  const GRCORE_ProfileEntry * recursive = find(r, line(1, 5));
  ASSERT_NE(recursive, nullptr);
  EXPECT_EQ(recursive->inclusive, 3u); // not 15
  EXPECT_EQ(recursive->self, 0u);
  const GRCORE_ProfileEntry * inner = find(r, line(1, 7));
  ASSERT_NE(inner, nullptr);
  EXPECT_EQ(inner->self, 3u);
  EXPECT_EQ(inner->inclusive, 3u);
}

TEST(Profiler, ALocationIsKeyedByTheTextOfItsFileNotByThePointer) {
  struct Twin {
    static GRCORE_Location locate(const GRCORE_Context *, uint64_t function,
        uint64_t offset) {
      // Two different pointers to equal text, alternating by function.
      static char a[] = "twin.src";
      static char b[] = "twin.src";
      return GRCORE_Location{function % 2 == 0 ? a : b, int(offset)};
    }
  };
  static const GRCORE_EngineDescriptor twin = GRCORE_ENGINE_DESCRIPTOR_INIT("twin", nullptr, Twin::locate,
      nullptr, GRCORE_ScopeInterface{nullptr, nullptr, nullptr},
      GRCORE_ConservativeDecoder{0, 0, 0}, nullptr, nullptr);
  RunWorld w;
  GRCORE_EngineId id = 0;
  ASSERT_EQ(grcore_engine_register(w.ctx, &twin, &id), GRCORE_OK);
  GRCORE_Profiler * p = nullptr;
  ASSERT_EQ(grcore_profiler_attach(w.ctx, 0, &p), GRCORE_OK);
  Fn fn;
  fn.body = [&](GRCORE_Context * c) {
    GRCORE_Stack * s = grcore_context_stack(c);
    GRCORE_FrameRef f;
    grcore_stack_push(s, id, 1, &f);
    for (uint64_t function = 0; function < 4; function++) {
      grcore_profiler_request(p);
      grcore_stack_poll(c, function, 3);
    }
    grcore_stack_pop(s);
    return GRCORE_STEP_FINISHED;
  };
  GRCORE_Outcome o;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &o), GRCORE_OK);
  Report r = read(p);
  ASSERT_EQ(r.entries.size(), 1u);
  EXPECT_EQ(r.entries[0].self, 4u);
}

TEST(Profiler, APollWithAnEmptyGuestStackIsCountedAsNoFrameAndAddsNoEntry) {
  RunWorld w;
  GRCORE_Profiler * p = nullptr;
  ASSERT_EQ(grcore_profiler_attach(w.ctx, 0, &p), GRCORE_OK);
  Fn fn;
  fn.body = [&](GRCORE_Context * c) {
    EXPECT_EQ(grcore_profiler_request(p), GRCORE_OK);
    EXPECT_EQ(GRCORE_POLL(c), GRCORE_VERDICT_CONTINUE);
    return GRCORE_STEP_FINISHED;
  };
  GRCORE_Outcome o;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &o), GRCORE_OK);
  Report r = read(p);
  EXPECT_EQ(r.totals.samples, 1u);
  EXPECT_EQ(r.totals.no_frame, 1u);
  EXPECT_TRUE(r.entries.empty());
}

TEST(Profiler, ATableThatIsFullCountsTheExtraLocationsAsDroppedAndKeepsTheTotals) {
  StackWorld w;
  GRCORE_Profiler * p = nullptr;
  ASSERT_EQ(grcore_profiler_attach(w.ctx, 4, &p), GRCORE_OK);
  Guest g;
  g.engine = w.alpha;
  g.steps = 10;
  g.profiler = p;
  g.offset = [](uint64_t i) { return 100 + i; }; // ten distinct locations
  ASSERT_EQ(run_guest(w.ctx, &g), GRCORE_OK);
  Report r = read(p);
  EXPECT_EQ(r.totals.samples, 10u);
  EXPECT_EQ(r.totals.locations, 4u);
  EXPECT_EQ(r.totals.dropped, 6u);
  uint64_t self = 0;
  for (const GRCORE_ProfileEntry & e : r.entries) {
    self += e.self;
  }
  // The four that fit are the first four seen, each hit once.
  EXPECT_EQ(self, 4u);
  EXPECT_EQ(r.entries.size(), 4u);
}

TEST(Profiler, TheHandlerChargesNoFuelAndTheTableIsTheOnlyBlockItEverTook) {
  StackWorld w;
  GRCORE_Profiler * p = nullptr;
  ASSERT_EQ(grcore_profiler_attach(w.ctx, 8, &p), GRCORE_OK);
  // Warm the guest stack first, so its own growth is not in the measure.
  Guest warm;
  warm.engine = w.alpha;
  warm.steps = 2;
  warm.depth = 3;
  warm.request = false;
  ASSERT_EQ(run_guest(w.ctx, &warm), GRCORE_OK);
  uint64_t fuel_before = grcore_context_fuel_used(w.ctx);
  uint64_t blocks_before = grcore_context_memory_blocks(w.ctx);
  Guest g;
  g.engine = w.alpha;
  g.steps = 200;
  g.depth = 3;
  g.profiler = p;
  g.offset = [](uint64_t i) { return i % 20; }; // more locations than room
  ASSERT_EQ(run_guest(w.ctx, &g), GRCORE_OK);
  EXPECT_EQ(grcore_context_fuel_used(w.ctx), fuel_before);
  EXPECT_EQ(grcore_context_memory_blocks(w.ctx), blocks_before);
  Report r = read(p);
  EXPECT_EQ(r.totals.samples, 200u);
  EXPECT_GT(r.totals.dropped, 0u);
}

TEST(Profiler, TheHandlerSurvivesAnAllocatorThatFailsEverythingAfterAttach) {
  // The strongest form of "allocates nothing": the group's allocator refuses
  // every call once the profiler is attached and the stack has its room. The
  // run must still sample, with the same counts.
  struct Refusing {
    GRCORE_Allocator vtable;
    bool refuse = false;
    long refused = 0;
    Refusing() {
      vtable.ctx = this;
      vtable.malloc_fn = [](void * c, size_t n) -> void * {
        auto * r = static_cast<Refusing *>(c);
        if (r->refuse) {
          r->refused++;
          return nullptr;
        }
        return std::malloc(n ? n : 1);
      };
      vtable.calloc_fn = [](void * c, size_t n, size_t m) -> void * {
        auto * r = static_cast<Refusing *>(c);
        if (r->refuse) {
          r->refused++;
          return nullptr;
        }
        return std::calloc(n ? n : 1, m ? m : 1);
      };
      vtable.realloc_fn = [](void * c, void * p, size_t n) -> void * {
        auto * r = static_cast<Refusing *>(c);
        if (r->refuse) {
          r->refused++;
          return nullptr;
        }
        return std::realloc(p, n ? n : 1);
      };
      vtable.free_fn = [](void *, void * p) { std::free(p); };
    }
  } refusing;
  StackWorld w(GRCORE_UNLIMITED, GRCORE_UNLIMITED,
      GRCORE_DEFAULT_MEMORY_RESERVE, GRCORE_UNLIMITED, &refusing.vtable);
  GRCORE_Profiler * p = nullptr;
  ASSERT_EQ(grcore_profiler_attach(w.ctx, 4, &p), GRCORE_OK);
  Fn fn;
  fn.body = [&](GRCORE_Context * c) {
    GRCORE_Stack * s = grcore_context_stack(c);
    GRCORE_FrameRef f;
    EXPECT_EQ(grcore_stack_push(s, w.alpha, 2, &f), GRCORE_OK);
    refusing.refuse = true; // from here on, any allocation fails
    for (uint64_t i = 0; i < 50; i++) {
      grcore_profiler_request(p);
      grcore_stack_poll(c, 1, i % 9);
    }
    refusing.refuse = false;
    grcore_stack_pop(s);
    return GRCORE_STEP_FINISHED;
  };
  GRCORE_Outcome o;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &o), GRCORE_OK);
  EXPECT_EQ(refusing.refused, 0);
  EXPECT_EQ(read(p).totals.samples, 50u);
}

TEST(Profiler, AttachFailsCleanlyAtEveryAllocationAndLeavesNothingBehind) {
  for (long n = 1; n <= 12; n++) {
    TrackingAllocator tracker;
    {
      RunWorld w(GRCORE_UNLIMITED, GRCORE_UNLIMITED,
          GRCORE_DEFAULT_MEMORY_RESERVE, GRCORE_UNLIMITED, GRCORE_UNLIMITED,
          tracker.get());
      long base = tracker.calls;
      tracker.fail_at = base + n;
      GRCORE_Profiler * p = nullptr;
      GRCORE_Result r = grcore_profiler_attach(w.ctx, 16, &p);
      tracker.fail_at = 0;
      if (r != GRCORE_OK) {
        EXPECT_EQ(r, GRCORE_ERR_OOM) << "n=" << n;
        EXPECT_EQ(p, nullptr);
        EXPECT_EQ(grcore_profiler_of(w.ctx), nullptr);
        // A later attach still works.
        EXPECT_EQ(grcore_profiler_attach(w.ctx, 16, &p), GRCORE_OK);
      }
      EXPECT_EQ(grcore_profiler_of(w.ctx), p);
    }
    EXPECT_EQ(tracker.live, 0) << "n=" << n;
  }
}

TEST(Profiler, PhaseShuffleGivesTheSameCountsForEverySeed) {
  std::vector<std::vector<uint64_t>> seen;
  for (uint64_t seed = 1; seed <= 24; seed++) {
    StackWorld w;
    GRCORE_Profiler * p = nullptr;
    ASSERT_EQ(grcore_profiler_attach(w.ctx, 0, &p), GRCORE_OK);
    Probe d1("d1"), d2("d2"), o1("o1"), o2("o2");
    ASSERT_EQ(grcore_context_register(w.ctx, &kDecideKey, &d1), GRCORE_OK);
    ASSERT_EQ(grcore_context_register(w.ctx, &kDecideKey, &d2), GRCORE_OK);
    ASSERT_EQ(grcore_context_register(w.ctx, &kObserveKey, &o1), GRCORE_OK);
    ASSERT_EQ(grcore_context_register(w.ctx, &kObserveKey, &o2), GRCORE_OK);
    ASSERT_EQ(grcore_context_set_phase_shuffle(w.ctx, true, seed), GRCORE_OK);
    Guest g;
  g.engine = w.alpha;
    g.steps = 40;
    g.depth = 2;
    g.callee = true;
    g.profiler = p;
    g.offset = [](uint64_t i) { return i % 5; };
    ASSERT_EQ(run_guest(w.ctx, &g), GRCORE_OK);
    Report r = read(p);
    std::vector<uint64_t> flat{r.totals.samples, r.totals.no_frame,
        r.totals.dropped, r.entries.size()};
    for (const GRCORE_ProfileEntry & e : r.entries) {
      flat.push_back(uint64_t(e.line));
      flat.push_back(e.self);
      flat.push_back(e.inclusive);
    }
    seen.push_back(flat);
    EXPECT_EQ(o1.calls, 40);
    EXPECT_EQ(o2.calls, 40);
  }
  ASSERT_FALSE(seen.empty());
  EXPECT_EQ(seen[0][0], 40u);
  for (const auto & s : seen) {
    EXPECT_EQ(s, seen[0]);
  }
}

TEST(Profiler, AReportIsSortedBySelfThenInclusiveThenFileAndLine) {
  StackWorld w;
  GRCORE_Profiler * p = nullptr;
  ASSERT_EQ(grcore_profiler_attach(w.ctx, 0, &p), GRCORE_OK);
  // Offsets chosen so self counts are 3, 2, 2, 1; the two 2s tie on self and
  // inclusive and are ordered by line.
  const uint64_t plan[] = {9, 4, 9, 6, 4, 9, 2, 6};
  Guest g;
  g.engine = w.alpha;
  g.steps = 8;
  g.profiler = p;
  g.offset = [&](uint64_t i) { return plan[i]; };
  ASSERT_EQ(run_guest(w.ctx, &g), GRCORE_OK);
  Report r = read(p);
  ASSERT_EQ(r.entries.size(), 4u);
  EXPECT_EQ(r.entries[0].line, line(1, 9));
  EXPECT_EQ(r.entries[1].line, line(1, 4));
  EXPECT_EQ(r.entries[2].line, line(1, 6));
  EXPECT_EQ(r.entries[3].line, line(1, 2));
  // A smaller array gets the first of that order and nothing more.
  Report top2 = read(p, 2);
  ASSERT_EQ(top2.entries.size(), 2u);
  EXPECT_EQ(top2.entries[0].line, line(1, 9));
  EXPECT_EQ(top2.entries[1].line, line(1, 4));
  EXPECT_EQ(top2.totals.locations, 4u);
  // Totals alone need no array.
  GRCORE_ProfileTotals t{};
  EXPECT_EQ(grcore_profiler_report(p, nullptr, 0, nullptr, &t), GRCORE_OK);
  EXPECT_EQ(t.samples, 8u);
}

TEST(Profiler, ResetEmptiesTheTableSoItsLocationsAreFreeAgain) {
  StackWorld w;
  GRCORE_Profiler * p = nullptr;
  ASSERT_EQ(grcore_profiler_attach(w.ctx, 2, &p), GRCORE_OK);
  Guest g;
  g.engine = w.alpha;
  g.steps = 5;
  g.profiler = p;
  g.offset = [](uint64_t i) { return i; };
  ASSERT_EQ(run_guest(w.ctx, &g), GRCORE_OK);
  EXPECT_GT(read(p).totals.dropped, 0u);
  ASSERT_EQ(grcore_profiler_reset(p), GRCORE_OK);
  Report cleared = read(p);
  EXPECT_EQ(cleared.totals.samples, 0u);
  EXPECT_EQ(cleared.totals.dropped, 0u);
  EXPECT_EQ(cleared.totals.locations, 0u);
  EXPECT_TRUE(cleared.entries.empty());
  Guest again;
  again.engine = w.alpha;
  again.steps = 2;
  again.profiler = p;
  again.offset = [](uint64_t i) { return 50 + i; };
  ASSERT_EQ(run_guest(w.ctx, &again), GRCORE_OK);
  Report r = read(p);
  EXPECT_EQ(r.totals.locations, 2u);
  EXPECT_EQ(r.totals.dropped, 0u);
}

TEST(Profiler, RefusalsChangeNothing) {
  RunWorld w;
  GRCORE_Profiler * p = nullptr;
  EXPECT_EQ(grcore_profiler_attach(nullptr, 0, &p), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_profiler_attach(w.ctx, 0, nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_profiler_attach(w.ctx, GRCORE_PROFILER_MAX_CAPACITY + 1, &p),
      GRCORE_ERR_LIMIT);
  EXPECT_EQ(p, nullptr);
  EXPECT_EQ(grcore_profiler_of(w.ctx), nullptr);
  ASSERT_EQ(grcore_profiler_attach(w.ctx, 0, &p), GRCORE_OK);
  size_t registrations = grcore_context_registration_count(w.ctx);
  GRCORE_Profiler * second = nullptr;
  EXPECT_EQ(grcore_profiler_attach(w.ctx, 0, &second), GRCORE_ERR_INVALID);
  EXPECT_EQ(second, nullptr);
  EXPECT_EQ(grcore_context_registration_count(w.ctx), registrations);
  EXPECT_EQ(grcore_profiler_of(w.ctx), p);
  EXPECT_EQ(grcore_profiler_of(nullptr), nullptr);
  EXPECT_EQ(grcore_profiler_request(nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_profiler_reset(nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_profiler_report(nullptr, nullptr, 0, nullptr, nullptr),
      GRCORE_ERR_INVALID);
  GRCORE_ProfileEntry e;
  EXPECT_EQ(grcore_profiler_report(p, nullptr, 1, nullptr, nullptr),
      GRCORE_ERR_INVALID);
  (void)e;
  EXPECT_FALSE(grcore_profiler_timer_running(nullptr));
}

TEST(Profiler, ReadingFromInsideAPollIsRefused) {
  StackWorld w;
  GRCORE_Profiler * p = nullptr;
  ASSERT_EQ(grcore_profiler_attach(w.ctx, 0, &p), GRCORE_OK);
  Probe observe("observe");
  GRCORE_Result inside_report = GRCORE_OK, inside_reset = GRCORE_OK;
  observe.extra = [&](GRCORE_Context *, GRCORE_PollCall *) {
    inside_report = grcore_profiler_report(p, nullptr, 0, nullptr, nullptr);
    inside_reset = grcore_profiler_reset(p);
  };
  ASSERT_EQ(grcore_context_register(w.ctx, &kObserveKey, &observe), GRCORE_OK);
  Guest g;
  g.engine = w.alpha;
  g.steps = 1;
  g.profiler = p;
  ASSERT_EQ(run_guest(w.ctx, &g), GRCORE_OK);
  EXPECT_EQ(inside_report, GRCORE_ERR_INVALID);
  EXPECT_EQ(inside_reset, GRCORE_ERR_INVALID);
  EXPECT_EQ(read(p).totals.samples, 1u);
}

TEST(Profiler, ReadingFromAnotherThreadIsRefused) {
  RunWorld w;
  GRCORE_Profiler * p = nullptr;
  ASSERT_EQ(grcore_profiler_attach(w.ctx, 0, &p), GRCORE_OK);
  GRCORE_Result r = GRCORE_OK, t = GRCORE_OK;
  std::thread([&] {
    r = grcore_profiler_report(p, nullptr, 0, nullptr, nullptr);
    t = grcore_profiler_timer_start(p, 1000);
  }).join();
  EXPECT_EQ(r, GRCORE_ERR_INVALID);
  EXPECT_EQ(t, GRCORE_ERR_INVALID);
  EXPECT_FALSE(grcore_profiler_timer_running(p));
}

/* ---- The timer ---------------------------------------------------------- */

TEST(ProfilerTimer, ZeroIntervalAndASecondStartAreRefusedAndStopWithoutStartIsANoOp) {
  RunWorld w;
  GRCORE_Profiler * p = nullptr;
  ASSERT_EQ(grcore_profiler_attach(w.ctx, 0, &p), GRCORE_OK);
  EXPECT_EQ(grcore_profiler_timer_stop(p), GRCORE_OK);
  EXPECT_EQ(grcore_profiler_timer_start(p, 0), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_profiler_timer_start(p, GRCORE_PROFILER_MAX_INTERVAL_US + 1),
      GRCORE_ERR_LIMIT);
  EXPECT_FALSE(grcore_profiler_timer_running(p));
  ASSERT_EQ(grcore_profiler_timer_start(p, 100000), GRCORE_OK);
  EXPECT_TRUE(grcore_profiler_timer_running(p));
  EXPECT_EQ(grcore_profiler_timer_start(p, 5), GRCORE_ERR_INVALID);
  EXPECT_TRUE(grcore_profiler_timer_running(p));
  EXPECT_EQ(grcore_profiler_timer_stop(p), GRCORE_OK);
  EXPECT_FALSE(grcore_profiler_timer_running(p));
  EXPECT_EQ(grcore_profiler_timer_stop(p), GRCORE_OK);
  // It can be started again after a stop.
  ASSERT_EQ(grcore_profiler_timer_start(p, 100000), GRCORE_OK);
}

TEST(ProfilerTimer, ARunningTimerSamplesAGuestThatPollsAndStopsWhenAskedTo) {
  StackWorld w;
  GRCORE_Profiler * p = nullptr;
  ASSERT_EQ(grcore_profiler_attach(w.ctx, 0, &p), GRCORE_OK);
  ASSERT_EQ(grcore_profiler_timer_start(p, 500), GRCORE_OK);
  Guest g;
  g.engine = w.alpha;
  g.steps = 400;
  g.request = false;
  g.before_poll = [](uint64_t) {
    std::this_thread::sleep_for(std::chrono::microseconds(250));
  };
  ASSERT_EQ(run_guest(w.ctx, &g), GRCORE_OK);
  ASSERT_EQ(grcore_profiler_timer_stop(p), GRCORE_OK);
  Report r = read(p);
  // 400 polls over at least 100 ms of a 0.5 ms tick: far more than a handful
  // unless the timer is dead; samples are bounded by the polls.
  EXPECT_GE(r.totals.samples, 20u);
  EXPECT_LE(r.totals.samples, 400u);
  ASSERT_EQ(r.entries.size(), 1u);
  EXPECT_EQ(r.entries[0].self, r.totals.samples);
  // Stopped means stopped: no more samples arrive.
  uint64_t before = r.totals.samples;
  Guest more;
  more.engine = w.alpha;
  more.steps = 50;
  more.request = false;
  ASSERT_EQ(run_guest(w.ctx, &more), GRCORE_OK);
  EXPECT_EQ(read(p).totals.samples, before);
}

TEST(ProfilerTimer, DestroyingTheContextWhileTheTimerRunsIsClean) {
  for (int i = 0; i < 20; i++) {
    TrackingAllocator tracker;
    {
      RunWorld w(GRCORE_UNLIMITED, GRCORE_UNLIMITED,
          GRCORE_DEFAULT_MEMORY_RESERVE, GRCORE_UNLIMITED, GRCORE_UNLIMITED,
          tracker.get());
      GRCORE_Profiler * p = nullptr;
      ASSERT_EQ(grcore_profiler_attach(w.ctx, 0, &p), GRCORE_OK);
      ASSERT_EQ(grcore_profiler_timer_start(p, 1 + i), GRCORE_OK);
      std::this_thread::sleep_for(std::chrono::microseconds(100 * i));
      // ~RunWorld destroys the context with the timer thread running.
    }
    EXPECT_EQ(tracker.live, 0);
  }
}

#ifndef _WIN32
namespace {
volatile sig_atomic_t g_signal_seen = 0;
volatile pthread_t g_signal_thread;
void on_signal(int) {
  g_signal_thread = pthread_self();
  g_signal_seen = 1;
}
} // namespace

TEST(ProfilerTimer, AProcessSignalIsNeverDeliveredToTheTimerThread) {
  // The timer starts while SIGUSR1 is unblocked here. Only then is this thread
  // blocked, so the one place left that could run the handler is the timer
  // thread, unless it starts with every signal blocked.
  struct sigaction act, old_act;
  memset(&act, 0, sizeof act);
  act.sa_handler = on_signal;
  sigemptyset(&act.sa_mask);
  ASSERT_EQ(sigaction(SIGUSR1, &act, &old_act), 0);
  sigset_t usr1, previous;
  sigemptyset(&usr1);
  sigaddset(&usr1, SIGUSR1);
  g_signal_seen = 0;
  {
    RunWorld w;
    GRCORE_Profiler * p = nullptr;
    ASSERT_EQ(grcore_profiler_attach(w.ctx, 0, &p), GRCORE_OK);
    ASSERT_EQ(grcore_profiler_timer_start(p, 1000), GRCORE_OK);
    ASSERT_EQ(pthread_sigmask(SIG_BLOCK, &usr1, &previous), 0);
    ASSERT_EQ(kill(getpid(), SIGUSR1), 0);
    for (int i = 0; i < 100 && !g_signal_seen; i++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_EQ(g_signal_seen, 0) << "the handler ran on the timer thread";
    // The thread that blocked it gets it when it unblocks.
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    for (int i = 0; i < 100 && !g_signal_seen; i++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_EQ(g_signal_seen, 1);
    EXPECT_TRUE(pthread_equal(g_signal_thread, pthread_self()));
  }
  pthread_sigmask(SIG_SETMASK, &previous, nullptr);
  sigaction(SIGUSR1, &old_act, nullptr);
}
#endif

TEST(ProfilerTimer, ARequestFromAnotherThreadWhileTheProfilerLivesIsAccepted) {
  RunWorld w;
  GRCORE_Profiler * p = nullptr;
  ASSERT_EQ(grcore_profiler_attach(w.ctx, 0, &p), GRCORE_OK);
  GRCORE_Result r = GRCORE_ERR_INTERNAL;
  std::thread([&] { r = grcore_profiler_request(p); }).join();
  EXPECT_EQ(r, GRCORE_OK);
  EXPECT_TRUE(grcore_context_request_pending(w.ctx, grcore_profiler_kind(p)));
}

/* ---- Snapshots ---------------------------------------------------------- */

TEST(ProfilerSnapshot, AContextWithAProfilerStillSnapshotsAndTheRestoredOneHasNone) {
  StackWorld src;
  GRCORE_Profiler * p = nullptr;
  ASSERT_EQ(grcore_profiler_attach(src.ctx, 0, &p), GRCORE_OK);
  Guest g;
  g.engine = src.alpha;
  g.steps = 3;
  g.profiler = p;
  ASSERT_EQ(run_guest(src.ctx, &g), GRCORE_OK);
  GRCORE_Snapshot * snapshot = nullptr;
  ASSERT_EQ(grcore_context_snapshot(src.ctx, nullptr, &snapshot), GRCORE_OK);
  // No blob is the profiler's: the key has no snapshot hook.
  for (size_t i = 0; i < grcore_snapshot_blob_count(snapshot); i++) {
    const char * name = grcore_snapshot_blob_name(snapshot, i);
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(std::strstr(name, "profiler"), nullptr) << name;
  }
  StackWorld dst;
  EXPECT_EQ(grcore_context_restore(dst.ctx, snapshot, nullptr), GRCORE_OK);
  EXPECT_EQ(grcore_profiler_of(dst.ctx), nullptr);
  // The source is unchanged and still has its profile.
  EXPECT_EQ(grcore_profiler_of(src.ctx), p);
  EXPECT_EQ(read(p).totals.samples, 3u);
  grcore_snapshot_release(snapshot);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

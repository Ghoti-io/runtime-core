/**
 * @file
 *
 * Shared helpers for the Ghoti.io Runtime-core unit tests.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRCORE_TEST_HELPERS_H
#define GHOTI_IO_GRCORE_TEST_HELPERS_H

#include <gtest/gtest.h>

#include <ghoti.io/runtime-core/runtime-core.h>

#include <cstdlib>
#include <functional>
#include <string>
#include <utility>
#include <vector>

/* A base allocator that counts, tracks live blocks, and can fail the Nth
 * allocation call (malloc, calloc or realloc, counted from 1). The tests that
 * sweep N from 1 upward use it to reach every allocation-failure arm. */
struct TrackingAllocator {
  GRCORE_Allocator vtable;
  long calls = 0;
  long fail_at = 0; // 0: never fail
  long live = 0;    // successful allocations not yet freed

  TrackingAllocator(const TrackingAllocator &) = delete;
  TrackingAllocator & operator=(const TrackingAllocator &) = delete;
  TrackingAllocator() {
    vtable.ctx = this;
    vtable.malloc_fn = [](void * c, size_t n) -> void * {
      auto * t = static_cast<TrackingAllocator *>(c);
      if (t->fails()) {
        return nullptr;
      }
      void * p = std::malloc(n ? n : 1);
      if (p != nullptr) {
        t->live++;
      }
      return p;
    };
    vtable.calloc_fn = [](void * c, size_t n, size_t m) -> void * {
      auto * t = static_cast<TrackingAllocator *>(c);
      if (t->fails()) {
        return nullptr;
      }
      void * p = std::calloc(n ? n : 1, m ? m : 1);
      if (p != nullptr) {
        t->live++;
      }
      return p;
    };
    vtable.realloc_fn = [](void * c, void * p, size_t n) -> void * {
      auto * t = static_cast<TrackingAllocator *>(c);
      if (t->fails()) {
        return nullptr;
      }
      void * q = std::realloc(p, n ? n : 1);
      if (p == nullptr && q != nullptr) {
        t->live++;
      }
      return q;
    };
    vtable.free_fn = [](void * c, void * p) {
      if (p != nullptr) {
        static_cast<TrackingAllocator *>(c)->live--;
      }
      std::free(p);
    };
  }
  bool fails() {
    calls++;
    return fail_at != 0 && calls == fail_at;
  }
  const GRCORE_Allocator * get() const { return &vtable; }
};

/* A page provider over the default one that counts and can be told to fail. */
struct FakePages {
  GRCORE_PageProvider vtable;
  bool fail = false;
  long live = 0;

  FakePages(const FakePages &) = delete;
  FakePages & operator=(const FakePages &) = delete;
  FakePages() {
    vtable.ctx = this;
    vtable.page_size = grcore_page_provider_default()->page_size;
    vtable.map = [](void * c, size_t n) -> void * {
      auto * f = static_cast<FakePages *>(c);
      if (f->fail) {
        return nullptr;
      }
      const GRCORE_PageProvider * d = grcore_page_provider_default();
      void * p = d->map(d->ctx, n);
      if (p != nullptr) {
        f->live++;
      }
      return p;
    };
    vtable.unmap = [](void * c, void * p, size_t n) {
      const GRCORE_PageProvider * d = grcore_page_provider_default();
      d->unmap(d->ctx, p, n);
      static_cast<FakePages *>(c)->live--;
    };
  }
};

/* ---- Running guest code ------------------------------------------------ */

/* An entry function whose body is a lambda, so a test can write the "guest"
 * where it asserts about it. The state it passes to `run` is the Fn. */
struct Fn {
  std::function<GRCORE_Step(GRCORE_Context *)> body;
};
inline GRCORE_Step fn_entry(GRCORE_Context * c, void * state) {
  return static_cast<Fn *>(state)->body(c);
}

/* An engine-free guest: adds the integers below `n`, charging `fuel` per
 * step and polling once per step. Its position lives in its own state, which
 * is what lets a pause drop every C frame and `resume` pick up where it left
 * off. */
struct CountingGuest {
  uint64_t n = 100;
  uint64_t fuel = 1;
  uint64_t pos = 0;
  uint64_t sum = 0;
};
/* Where the poll in counting_entry is. kCountingPollLine is two lines above
 * the call; keep them together. */
inline const char * const kCountingPollFile = __FILE__;
inline const int kCountingPollLine = __LINE__ + 5;
inline GRCORE_Step counting_entry(GRCORE_Context * c, void * state) {
  auto * g = static_cast<CountingGuest *>(state);
  while (g->pos < g->n) {
    grcore_context_charge_fuel(c, g->fuel);
    GRCORE_Verdict v = grcore_poll(c, GRCORE_Location{__FILE__, __LINE__});
    if (v == GRCORE_VERDICT_PAUSE) {
      return GRCORE_STEP_PAUSED;
    }
    if (v == GRCORE_VERDICT_UNWIND) {
      return GRCORE_STEP_UNWOUND;
    }
    g->sum += g->pos;
    g->pos++;
  }
  return GRCORE_STEP_FINISHED;
}
inline uint64_t sum_below(uint64_t n) {
  uint64_t s = 0;
  for (uint64_t i = 0; i < n; i++) {
    s += i;
  }
  return s;
}

/* A group and one context, with the budgets the test wants. */
struct RunWorld {
  GRCORE_Group * group = nullptr;
  GRCORE_Context * ctx = nullptr;
  explicit RunWorld(uint64_t fuel = GRCORE_UNLIMITED,
      uint64_t memory = GRCORE_UNLIMITED,
      uint64_t reserve = GRCORE_DEFAULT_MEMORY_RESERVE,
      uint64_t guest_depth = GRCORE_UNLIMITED,
      uint64_t native_depth = GRCORE_UNLIMITED,
      const GRCORE_Allocator * allocator = nullptr) {
    GRCORE_Options * o;
    EXPECT_EQ(grcore_options_create(nullptr, &o), GRCORE_OK);
    grcore_options_set_fuel(o, fuel);
    grcore_options_set_memory_bytes(o, memory);
    grcore_options_set_memory_reserve(o, reserve);
    grcore_options_set_guest_depth(o, guest_depth);
    grcore_options_set_native_depth(o, native_depth);
    EXPECT_EQ(grcore_group_create(allocator, nullptr, &group), GRCORE_OK);
    EXPECT_EQ(grcore_context_create(group, o, &ctx), GRCORE_OK);
    grcore_options_destroy(o);
  }
  RunWorld(const RunWorld &) = delete;
  RunWorld & operator=(const RunWorld &) = delete;
  ~RunWorld() {
    if (ctx != nullptr) {
      if (!grcore_context_is_owner(ctx)) {
        EXPECT_EQ(grcore_context_acquire(ctx), GRCORE_OK);
      }
      EXPECT_EQ(grcore_context_destroy(ctx), GRCORE_OK);
    }
    EXPECT_EQ(grcore_group_destroy(group), GRCORE_OK);
  }
};

/* ---- Poll handlers ----------------------------------------------------- */

/* What a test registers under the probe keys. It records that it ran, and
 * votes if the phase lets it. */
struct Probe {
  Probe() = default;
  explicit Probe(std::string n) : name(std::move(n)) {}
  std::string name;
  GRCORE_Verdict vote = GRCORE_VERDICT_CONTINUE;
  GRCORE_Result unwind_result = GRCORE_OK; // OK: leave the default
  std::vector<std::string> * log = nullptr;
  int calls = 0;
  std::function<void(GRCORE_Context *, GRCORE_PollCall *)> extra;
};
inline void probe_handler(GRCORE_Context * c, void * value, GRCORE_PollCall * call) {
  auto * p = static_cast<Probe *>(value);
  p->calls++;
  if (p->log != nullptr) {
    p->log->push_back(p->name);
  }
  if (p->extra) {
    p->extra(c, call);
  }
  if (p->vote != GRCORE_VERDICT_CONTINUE) {
    grcore_pollcall_vote(call, p->vote);
  }
  if (p->unwind_result != GRCORE_OK) {
    grcore_pollcall_set_unwind_result(call, p->unwind_result);
  }
}
inline const GRCORE_Key kDecideKey = {"decide", GRCORE_CARDINALITY_MANY,
    GRCORE_PHASE_DECIDE, nullptr, probe_handler};
inline const GRCORE_Key kActKey = {"act", GRCORE_CARDINALITY_MANY,
    GRCORE_PHASE_ACT, nullptr, probe_handler};
inline const GRCORE_Key kObserveKey = {"observe", GRCORE_CARDINALITY_MANY,
    GRCORE_PHASE_OBSERVE, nullptr, probe_handler};
inline const GRCORE_Key kYieldKey = {"yield", GRCORE_CARDINALITY_MANY,
    GRCORE_PHASE_YIELD, nullptr, probe_handler};

#endif /* GHOTI_IO_GRCORE_TEST_HELPERS_H */

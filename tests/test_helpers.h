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

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>
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
  GRCORE_PageProvider vtable =
      GRCORE_PAGE_PROVIDER_INIT(nullptr, 0, nullptr, nullptr, nullptr);
  bool fail = false;
  bool fail_protect = false;
  long protects = 0;
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
    vtable.protect = [](void * c, void * p, size_t n,
                         GRCORE_PageAccess a) -> bool {
      auto * f = static_cast<FakePages *>(c);
      f->protects++;
      if (f->fail_protect) {
        return false;
      }
      const GRCORE_PageProvider * d = grcore_page_provider_default();
      return d->protect(d->ctx, p, n, a);
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
/* Where the poll in counting_entry is. kCountingPollLine is five lines above
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
inline const GRCORE_Key kDecideKey = GRCORE_KEY_INIT("decide", GRCORE_CARDINALITY_MANY,
    GRCORE_PHASE_DECIDE, nullptr, probe_handler, nullptr, nullptr, nullptr);
inline const GRCORE_Key kActKey = GRCORE_KEY_INIT("act", GRCORE_CARDINALITY_MANY,
    GRCORE_PHASE_ACT, nullptr, probe_handler, nullptr, nullptr, nullptr);
inline const GRCORE_Key kObserveKey = GRCORE_KEY_INIT("observe", GRCORE_CARDINALITY_MANY,
    GRCORE_PHASE_OBSERVE, nullptr, probe_handler, nullptr, nullptr, nullptr);
inline const GRCORE_Key kYieldKey = GRCORE_KEY_INIT("yield", GRCORE_CARDINALITY_MANY,
    GRCORE_PHASE_YIELD, nullptr, probe_handler, nullptr, nullptr, nullptr);

/* ---- Engines and frames ----------------------------------------------- */

/* Two engine-free engines, which is what A's tests need: something that
 * declares slot kinds, a locator, an inspector and scopes, and a second one
 * that declares them differently, so a test can tell a consumer that reads
 * through the descriptor from one that guessed.
 *
 * "alpha": slot i is RAW for even i and VALUE for odd. A poll identity
 * (f, o) is the line f * 1000 + o in "alpha.src". A value prints as "a#N".
 * Every frame has two scopes: "locals" (a LOCAL scope with one variable per
 * slot, v0, v1, ...) and "captured" (a CLOSURE scope with one variable, the
 * frame's slot 0, as a VALUE).
 *
 * "beta": every slot is a VALUE. (f, o) is line o in "beta.wasm". A value
 * prints as "b:HEX". No scopes. Its decoder takes bits 4..19 and adds 0x1000.
 *
 * "gamma": declares nothing but a name, so every default shows. */
inline const char * const kAlphaFile = "alpha.src";
inline const char * const kBetaFile = "beta.wasm";

inline GRCORE_SlotKind alpha_slot_kind(
    const GRCORE_AbstractFrame *, size_t index) {
  return index % 2 == 0 ? GRCORE_SLOT_RAW : GRCORE_SLOT_VALUE;
}
inline GRCORE_Location alpha_locate(
    const GRCORE_Context *, uint64_t function, uint64_t offset) {
  return GRCORE_Location{kAlphaFile, static_cast<int>(function * 1000 + offset)};
}
inline size_t alpha_inspect(const GRCORE_Context *, GRCORE_SlotKind,
    uint64_t value, char * buffer, size_t size) {
  int n = std::snprintf(buffer, size, "a#%llu",
      static_cast<unsigned long long>(value));
  return static_cast<size_t>(n);
}
inline size_t alpha_scope_count(const GRCORE_AbstractFrame *) { return 2; }
inline const char * const kAlphaVarNames[] = {
    "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7"};
inline GRCORE_Result alpha_scope(const GRCORE_AbstractFrame * frame,
    size_t index, GRCORE_ScopeInfo * out) {
  if (index == 0) {
    *out = GRCORE_ScopeInfo{GRCORE_SCOPE_LOCAL, "locals", frame->slot_count};
    return GRCORE_OK;
  }
  if (index == 1) {
    *out = GRCORE_ScopeInfo{GRCORE_SCOPE_CLOSURE, "captured", 1};
    return GRCORE_OK;
  }
  return GRCORE_ERR_INVALID;
}
inline GRCORE_Result alpha_variable(const GRCORE_AbstractFrame * frame,
    size_t scope, size_t index, GRCORE_Variable * out) {
  GRCORE_Stack * stack = grcore_context_stack(frame->context);
  uint64_t value;
  if (scope == 0 && index < frame->slot_count && index < 8) {
    if (grcore_stack_slot_get(stack, frame->frame, index, &value) != GRCORE_OK) {
      return GRCORE_ERR_INVALID;
    }
    *out = GRCORE_Variable{kAlphaVarNames[index],
        alpha_slot_kind(frame, index), value};
    return GRCORE_OK;
  }
  if (scope == 1 && index == 0) {
    if (grcore_stack_slot_get(stack, frame->frame, 0, &value) != GRCORE_OK) {
      return GRCORE_ERR_INVALID;
    }
    *out = GRCORE_Variable{"captured0", GRCORE_SLOT_VALUE, value};
    return GRCORE_OK;
  }
  return GRCORE_ERR_INVALID;
}
inline const GRCORE_EngineDescriptor kAlpha = GRCORE_ENGINE_DESCRIPTOR_INIT("alpha", alpha_slot_kind,
    alpha_locate, alpha_inspect,
    GRCORE_ScopeInterface{alpha_scope_count, alpha_scope, alpha_variable},
    GRCORE_ConservativeDecoder{0, 0, 0}, nullptr, nullptr, nullptr, nullptr);

inline GRCORE_SlotKind beta_slot_kind(
    const GRCORE_AbstractFrame *, size_t) {
  return GRCORE_SLOT_VALUE;
}
inline GRCORE_Location beta_locate(
    const GRCORE_Context *, uint64_t, uint64_t offset) {
  return GRCORE_Location{kBetaFile, static_cast<int>(offset)};
}
inline size_t beta_inspect(const GRCORE_Context *, GRCORE_SlotKind,
    uint64_t value, char * buffer, size_t size) {
  int n = std::snprintf(buffer, size, "b:%llx",
      static_cast<unsigned long long>(value));
  return static_cast<size_t>(n);
}
inline const GRCORE_EngineDescriptor kBeta = GRCORE_ENGINE_DESCRIPTOR_INIT("beta", beta_slot_kind,
    beta_locate, beta_inspect, GRCORE_ScopeInterface{nullptr, nullptr, nullptr},
    GRCORE_ConservativeDecoder{0xFFFF0, 4, 0x1000}, nullptr, nullptr, nullptr, nullptr);

inline const GRCORE_EngineDescriptor kGamma = GRCORE_ENGINE_DESCRIPTOR_INIT("gamma", nullptr, nullptr,
    nullptr, GRCORE_ScopeInterface{nullptr, nullptr, nullptr},
    GRCORE_ConservativeDecoder{0, 0, 0}, nullptr, nullptr, nullptr, nullptr);

/* A RunWorld with the two engines registered. */
struct StackWorld : RunWorld {
  GRCORE_EngineId alpha = 0;
  GRCORE_EngineId beta = 0;
  GRCORE_Stack * stack = nullptr;
  explicit StackWorld(uint64_t fuel = GRCORE_UNLIMITED,
      uint64_t memory = GRCORE_UNLIMITED,
      uint64_t reserve = GRCORE_DEFAULT_MEMORY_RESERVE,
      uint64_t guest_depth = GRCORE_UNLIMITED,
      const GRCORE_Allocator * allocator = nullptr,
      uint64_t native_depth = GRCORE_UNLIMITED)
      : RunWorld(fuel, memory, reserve, guest_depth, native_depth,
            allocator) {
    EXPECT_EQ(grcore_engine_register(ctx, &kAlpha, &alpha), GRCORE_OK);
    EXPECT_EQ(grcore_engine_register(ctx, &kBeta, &beta), GRCORE_OK);
    stack = grcore_context_stack(ctx);
  }
};

/* "delta": an engine with hooks. Slot 0 of every frame is a VALUE and the
 * rest are RAW. Its `roots` hook reports the frame's slot 1 as a precise root
 * when the frame's function is 99 (a "boxed" frame whose RAW slot is really a
 * reference), and always logs the call. Its `unwind` hook logs the frame's slot
 * 0, its depth, and whether the frame was still on the stack. Its decoder takes
 * bits 8..15 and adds 0x40. */
struct HookLog {
  std::vector<uint64_t> unwound;       // slot 0 of each frame unwound, in order
  std::vector<size_t> unwound_depth;   // the abstract frame's depth each time
  std::vector<bool> on_stack;          // frame valid while the hook ran
  std::vector<uint64_t> root_functions; // function of each frame `roots` saw
  std::vector<size_t> root_depths;      // the abstract frame's depth each time
  std::vector<GRCORE_Location> root_locations; // and its location
  std::vector<const GRCORE_Context *> contexts; // where hooks were called
};
inline HookLog * g_hook_log = nullptr;

inline GRCORE_SlotKind delta_slot_kind(
    const GRCORE_AbstractFrame *, size_t index) {
  return index == 0 ? GRCORE_SLOT_VALUE : GRCORE_SLOT_RAW;
}
inline void delta_roots(GRCORE_Context * context,
    const GRCORE_AbstractFrame * frame, const GRCORE_RootVisitor * visitor) {
  if (g_hook_log != nullptr) {
    g_hook_log->root_functions.push_back(frame->identity.function);
    g_hook_log->root_depths.push_back(frame->depth);
    g_hook_log->root_locations.push_back(frame->location);
    g_hook_log->contexts.push_back(context);
  }
  if (frame->identity.function == 99 && frame->slot_count > 1 &&
      visitor->slot != nullptr) {
    uint64_t * slots = grcore_stack_slots(grcore_context_stack(context), frame->frame);
    visitor->slot(visitor->user, &slots[1]);
  }
}
inline void delta_unwind(
    GRCORE_Context * context, const GRCORE_AbstractFrame * frame) {
  if (g_hook_log == nullptr) {
    return;
  }
  GRCORE_Stack * stack = grcore_context_stack(context);
  uint64_t v = 0;
  grcore_stack_slot_get(stack, frame->frame, 0, &v);
  g_hook_log->unwound.push_back(v);
  g_hook_log->unwound_depth.push_back(frame->depth);
  g_hook_log->on_stack.push_back(grcore_stack_frame_valid(stack, frame->frame));
  g_hook_log->contexts.push_back(context);
}
inline const GRCORE_EngineDescriptor kDelta = GRCORE_ENGINE_DESCRIPTOR_INIT("delta", delta_slot_kind, nullptr,
    nullptr, GRCORE_ScopeInterface{nullptr, nullptr, nullptr},
    GRCORE_ConservativeDecoder{0xFF00, 8, 0x40}, delta_roots, delta_unwind, nullptr, nullptr);

/* A StackWorld that also has the hooked engine, and a log the hooks write. */
struct HookWorld : StackWorld {
  GRCORE_EngineId delta = 0;
  HookLog log;
  explicit HookWorld(uint64_t fuel = GRCORE_UNLIMITED,
      uint64_t memory = GRCORE_UNLIMITED,
      uint64_t reserve = GRCORE_DEFAULT_MEMORY_RESERVE,
      uint64_t guest_depth = GRCORE_UNLIMITED,
      const GRCORE_Allocator * allocator = nullptr,
      uint64_t native_depth = GRCORE_UNLIMITED)
      : StackWorld(fuel, memory, reserve, guest_depth, allocator, native_depth) {
    EXPECT_EQ(grcore_engine_register(ctx, &kDelta, &delta), GRCORE_OK);
    g_hook_log = &log;
  }
  ~HookWorld() { g_hook_log = nullptr; }
};

/* An engine-free guest whose position lives entirely on the guest stack. It
 * pushes `n` alpha frames, slot 0 of frame i holding i, charging one unit of
 * fuel and polling (function 1, offset = the frame count) after each push;
 * then it pops them all, summing slot 0, charging and polling after each pop.
 * A pause drops every C frame, and the entry picks up from the stack plus one
 * flag (which half it is in), so `sum` is the same however often it is paused or whichever thread resumes it.
 */
struct FrameGuest {
  uint64_t n = 10;
  GRCORE_EngineId engine = 0;
  uint64_t sum = 0;
  GRCORE_Result failure = GRCORE_OK; // why a push stopped it, if one did
  bool popping = false;              // the one thing besides the stack it keeps
};
inline GRCORE_Step frame_guest_entry(GRCORE_Context * c, void * state) {
  auto * g = static_cast<FrameGuest *>(state);
  GRCORE_Stack * stack = grcore_context_stack(c);
  while (!g->popping && grcore_stack_frame_count(stack) < g->n) {
    GRCORE_FrameRef f;
    g->failure = grcore_stack_push(stack, g->engine, 2, &f);
    if (g->failure != GRCORE_OK) {
      return GRCORE_STEP_FINISHED; // the guest gives up; the host reads why
    }
    grcore_stack_slot_set(stack, f, 0, grcore_stack_frame_count(stack));
    grcore_context_charge_fuel(c, 1);
    GRCORE_Verdict v = grcore_stack_poll(c, 1, grcore_stack_frame_count(stack));
    if (v == GRCORE_VERDICT_PAUSE) {
      return GRCORE_STEP_PAUSED;
    }
    if (v == GRCORE_VERDICT_UNWIND) {
      return GRCORE_STEP_UNWOUND;
    }
  }
  g->popping = true;
  while (grcore_stack_frame_count(stack) > 0) {
    uint64_t value = 0;
    grcore_stack_slot_get(stack, grcore_stack_top(stack), 0, &value);
    g->sum += value;
    grcore_stack_pop(stack);
    grcore_context_charge_fuel(c, 1);
    GRCORE_Verdict v = grcore_stack_poll(c, 2, grcore_stack_frame_count(stack));
    if (v == GRCORE_VERDICT_PAUSE) {
      return GRCORE_STEP_PAUSED;
    }
    if (v == GRCORE_VERDICT_UNWIND) {
      return GRCORE_STEP_UNWOUND;
    }
  }
  return GRCORE_STEP_FINISHED;
}

/* A thread that takes a context over by the library's own hand-off and by
 * nothing else. std::thread's start and join order memory by themselves, so a
 * test that hands a context over by starting a thread after releasing it and
 * joining before reading proves nothing about grcore_context_release and
 * grcore_context_acquire: weaken either and ThreadSanitizer still sees no
 * race. This one is built before the owner writes the state the migrant will
 * read, is told to go through a relaxed flag (which orders nothing), spins on
 * grcore_context_acquire, runs the body, and is taken back from by the owner
 * spinning on the same call.
 * Only the release store and the acquire exchange order the context's memory.
 *
 * The body must end by releasing the context and must not touch anything the
 * owner reads afterwards once it has done so. */
class MigrantThread {
 public:
  MigrantThread(GRCORE_Context * context, std::function<void()> body)
      : context_(context), thread_([this, body = std::move(body)] {
          while (!go_.load(std::memory_order_relaxed)) {
            if (quit_.load(std::memory_order_relaxed) || expired()) {
              return;
            }
            std::this_thread::yield();
          }
          GRCORE_Result got = grcore_context_acquire(context_);
          while (got != GRCORE_OK && !expired() &&
              !quit_.load(std::memory_order_relaxed)) {
            std::this_thread::yield();
            got = grcore_context_acquire(context_);
          }
          acquire_result_.store(got, std::memory_order_relaxed);
          if (got != GRCORE_OK) {
            done_.store(true, std::memory_order_relaxed);
            return;
          }
          taken_.store(true, std::memory_order_relaxed);
          body();
          done_.store(true, std::memory_order_relaxed);
        }) {}
  MigrantThread(const MigrantThread &) = delete;
  MigrantThread & operator=(const MigrantThread &) = delete;
  /* The result of the migrant's own acquire: GRCORE_OK if it took the
   * context, whatever the last refusal was if the deadline passed first. Read
   * it after take_back(). */
  GRCORE_Result acquire_result() const {
    return acquire_result_.load(std::memory_order_relaxed);
  }
  ~MigrantThread() {
    quit_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) {
      thread_.join();
    }
  }
  /* The owner has released the context: tell the migrant to take it. */
  void go() { go_.store(true, std::memory_order_relaxed); }
  /* Spin on the library's acquire until the migrant has released the context
   * and this thread owns it again; false if the body finished without
   * releasing it. Only then may the owner read what the migrant wrote. */
  bool take_back() {
    // The context is unowned between the owner's release and the migrant's
    // acquire, so an acquire here before the migrant has its turn would
    // succeed and prove nothing. The flag orders nothing either.
    while (!taken_.load(std::memory_order_relaxed)) {
      if (done_.load(std::memory_order_relaxed) || expired()) {
        return timed_out();
      }
      std::this_thread::yield();
    }
    for (;;) {
      bool finished = done_.load(std::memory_order_relaxed);
      if (grcore_context_acquire(context_) == GRCORE_OK) {
        return true;
      }
      if (finished || expired()) {
        return timed_out();
      }
      std::this_thread::yield();
    }
  }
  /* Reap the thread after take_back(); the join is housekeeping. */
  void join() { thread_.join(); }

 private:
  /* A hand-off that never completes must fail the test, not hang it. */
  static constexpr std::chrono::seconds kDeadline{30};
  bool expired() const {
    return std::chrono::steady_clock::now() >= deadline_;
  }
  bool timed_out() const {
    if (expired()) {
      ADD_FAILURE() << "MigrantThread: the hand-off did not complete within "
                    << kDeadline.count() << " s";
    }
    return false;
  }
  GRCORE_Context * context_;
  std::chrono::steady_clock::time_point deadline_ =
      std::chrono::steady_clock::now() + kDeadline;
  std::atomic<GRCORE_Result> acquire_result_{GRCORE_ERR_INTERNAL};
  std::atomic<bool> go_{false};
  std::atomic<bool> taken_{false};
  std::atomic<bool> quit_{false};
  std::atomic<bool> done_{false};
  std::thread thread_;
};

/* "conv": an engine that converts every AD-27 representation, and a log.
 *
 * Its values are tagged words: the top byte says what the value is (1 an
 * integer-32, 2 an integer-64, 3 a float-32, 4 a float-64) and the low 56 bits
 * are the payload. The 64-bit ones do not fit in a payload, so they are boxed
 * in a pool the test sized beforehand (the "reservation": `convert` allocates
 * nothing, it only takes the next entry and counts it). `reverse` is the type
 * test: a value fits a representation only if its tag is that
 * representation's, so an integer is not accepted where a float is expected
 * even though its bits could be read as one.
 *
 * `on_convert` runs inside every `convert`: a test hangs a forced collection
 * (a root enumeration) there, to see what a collector would see in the middle
 * of a rebuild. */
struct ConvLog {
  std::vector<uint64_t> pool;       // boxed 64-bit raws; reserved by the test
  size_t pool_limit = 0;            // entries the reservation allows
  int converts = 0;                 // calls of convert
  int reverses = 0;                 // calls of reverse
  int pool_overruns = 0;            // convert needed an entry past the limit
  std::function<void(GRCORE_Context *)> on_convert;
};
inline ConvLog * g_conv = nullptr;

constexpr uint64_t kConvTagShift = 56;
constexpr uint64_t kConvPayload = (UINT64_C(1) << kConvTagShift) - 1;
inline uint64_t conv_tag_of(GRCORE_Representation r) {
  switch (r) {
  case GRCORE_REPR_I32: return 1;
  case GRCORE_REPR_I64: return 2;
  case GRCORE_REPR_F32: return 3;
  case GRCORE_REPR_F64: return 4;
  default: return 0;
  }
}
inline void conv_convert(GRCORE_Context * context,
    GRCORE_Representation rep, uint64_t raw, uint64_t * out_root) {
  ConvLog & log = *g_conv;
  log.converts++;
  if (log.on_convert) {
    log.on_convert(context);
  }
  uint64_t payload = raw & 0xFFFFFFFFull;
  if (rep == GRCORE_REPR_I64 || rep == GRCORE_REPR_F64) {
    if (log.pool.size() >= log.pool_limit) {
      log.pool_overruns++;
      *out_root = 0;
      return;
    }
    payload = log.pool.size();
    log.pool.push_back(raw);
  }
  *out_root = (conv_tag_of(rep) << kConvTagShift) | payload;
}
inline bool conv_reverse(GRCORE_Context *, GRCORE_Representation rep,
    uint64_t value, uint64_t * out_raw) {
  ConvLog & log = *g_conv;
  log.reverses++;
  if ((value >> kConvTagShift) != conv_tag_of(rep) || conv_tag_of(rep) == 0) {
    return false;
  }
  uint64_t payload = value & kConvPayload;
  if (rep == GRCORE_REPR_I64 || rep == GRCORE_REPR_F64) {
    if (payload >= log.pool.size()) {
      return false;
    }
    *out_raw = log.pool[payload];
    return true;
  }
  if (payload > 0xFFFFFFFFull) {
    return false;
  }
  *out_raw = payload;
  return true;
}
inline GRCORE_SlotKind conv_slot_kind(const GRCORE_AbstractFrame *, size_t) {
  return GRCORE_SLOT_VALUE;
}
inline const GRCORE_EngineDescriptor kConv = GRCORE_ENGINE_DESCRIPTOR_INIT(
    "conv", conv_slot_kind, nullptr, nullptr,
    GRCORE_ScopeInterface{nullptr, nullptr, nullptr},
    GRCORE_ConservativeDecoder{0, 0, 0}, nullptr, nullptr, conv_convert,
    conv_reverse);

/* ---- Hand-built compiled code and frames (AD-28) ----------------------- */

/* A region standing in for compiled code, with a stack map for each of its
 * sites. Site i is at offset 16 * (i + 1); its identity is (100 + i, i); the
 * frame it describes has this layout below the frame base:
 *
 *   -8   a reference                (VALUE in the stack map, and in the state)
 *   -16  raw bits                   (RAW; looks like a reference when it is)
 *   -24  a raw 32-bit integer       (RAW, representation I32)
 *   -32  and further down           (in no map at all)
 *
 * `released` counts how often the code's release callback has run. */
struct HandCode {
  static constexpr size_t kSites = 64;
  static constexpr size_t kFrameBytes = 48;
  std::vector<unsigned char> bytes;
  GRCORE_CodeLocation live[1];
  GRCORE_CodeLocation state[3];
  std::vector<GRCORE_CodeSite> sites;
  GRCORE_CodeMeta meta;
  GRCORE_Code * code = nullptr; // the handle; the creator's reference is below
  bool creator = true;          // whether this fixture still holds that reference
  /* The count lives on the heap and the release callback owns a share of it,
   * so a context that outlives the fixture (a registry releasing at destroy)
   * still has somewhere valid to count. */
  std::shared_ptr<int> counter = std::make_shared<int>(0);
  int & released = *counter;

  HandCode() : bytes(16 * (kSites + 2)) {
    live[0] = {GRCORE_LOC_FRAME_SLOT, GRCORE_SLOT_VALUE, -8, GRCORE_REPR_BITS};
    state[0] = {GRCORE_LOC_FRAME_SLOT, GRCORE_SLOT_VALUE, -8, GRCORE_REPR_BITS};
    state[1] = {GRCORE_LOC_FRAME_SLOT, GRCORE_SLOT_RAW, -16, GRCORE_REPR_BITS};
    state[2] = {GRCORE_LOC_FRAME_SLOT, GRCORE_SLOT_RAW, -24, GRCORE_REPR_I32};
    for (size_t i = 0; i < kSites; i++) {
      GRCORE_CodeSite site{};
      site.code_offset = static_cast<uint32_t>(16 * (i + 1));
      site.kind = GRCORE_SITE_GC_POINT_CALL;
      site.identity = GRCORE_PollIdentity{100 + i, i};
      site.live = live;
      site.live_count = 1;
      site.frame_state = state;
      site.frame_state_count = 3;
      sites.push_back(site);
    }
    meta = {};
    meta.version = GRCORE_CODEMETA_FORMAT_VERSION;
    meta.frame_bytes = kFrameBytes;
    meta.code_bytes = static_cast<uint32_t>(bytes.size());
    meta.site_count = sites.size();
    meta.sites = sites.data();
    EXPECT_EQ(grcore_code_create(nullptr, new std::shared_ptr<int>(counter),
                  [](void * payload) {
                    auto * share = static_cast<std::shared_ptr<int> *>(payload);
                    ++**share;
                    delete share;
                  },
                  &code),
        GRCORE_OK);
  }
  HandCode(const HandCode &) = delete;
  HandCode & operator=(const HandCode &) = delete;
  ~HandCode() { give_up(); }
  /* Drops the fixture's own reference, so that what the registry and the slots
   * hold is all there is. `code` stays a valid handle while any of them does. */
  void give_up() {
    if (creator) {
      creator = false;
      grcore_code_release(code);
    }
  }
  uintptr_t start() const { return reinterpret_cast<uintptr_t>(bytes.data()); }
  size_t size() const { return bytes.size(); }
  /* The return address of a call at site i. */
  uintptr_t at(size_t site) const { return start() + 16 * (site + 1); }
  GRCORE_Result add_to(GRCORE_Context * c, GRCORE_EngineId engine) {
    return grcore_code_register(c, engine, code, start(), size(), &meta);
  }
};

/* Words standing in for a native stack, with frames built in them. A frame is
 * eight words: words 0..5 are what lies below the base, word 6 is the base
 * (holding the caller's base) and word 7 the return address. Frames are laid
 * out from low to high, innermost first, so each caller is above its callee. */
struct HandStack {
  static constexpr size_t kFrameWords = 8;
  std::vector<uint64_t> words;
  explicit HandStack(size_t frames, size_t spare_words = 0)
      : words(frames * kFrameWords + spare_words, 0) {}
  uintptr_t base_at(size_t word) { return reinterpret_cast<uintptr_t>(&words[word + 6]); }
  uintptr_t lo() { return reinterpret_cast<uintptr_t>(words.data()); }
  uintptr_t hi() { return reinterpret_cast<uintptr_t>(words.data() + words.size()); }
  /* The reference and the look-alike a frame holds, in a way that tells the
   * frames apart: `tag` is the frame's number. */
  static uint64_t ref_of(size_t tag) { return 0x100000 + tag * 0x10; }
  static uint64_t trap_of(size_t tag) { return 0x900000 + tag * 0x10; }
  /* Builds `n` frames starting at word `first`, the k-th stopped at site
   * `site0 + k` of `code`. The last frame's return address is `entry` and its
   * base word `last_caller`. Returns the bases, innermost first. */
  std::vector<uintptr_t> build(HandCode & code, size_t first, size_t n,
      size_t site0, uintptr_t entry, uintptr_t last_caller = 0) {
    std::vector<uintptr_t> bases;
    for (size_t k = 0; k < n; k++) {
      size_t w = first + k * kFrameWords;
      size_t tag = site0 + k;
      words[w + 5] = ref_of(tag);
      words[w + 4] = trap_of(tag);       // raw, and a value no frame holds as a reference
      words[w + 3] = 7 + tag;            // an I32
      words[w + 2] = trap_of(tag) + 1;   // outside every map
      words[w + 1] = trap_of(tag) + 2;
      words[w + 0] = trap_of(tag) + 3;
      bases.push_back(base_at(w));
    }
    for (size_t k = 0; k < n; k++) {
      size_t w = first + k * kFrameWords;
      bool last = k + 1 == n;
      words[w + 6] = last ? last_caller : bases[k + 1];
      words[w + 7] = last ? entry : code.at(site0 + k + 1);
    }
    return bases;
  }
};

#endif /* GHOTI_IO_GRCORE_TEST_HELPERS_H */

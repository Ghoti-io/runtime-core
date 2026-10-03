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

#include <cstdio>
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
inline const GRCORE_Key kDecideKey = {"decide", GRCORE_CARDINALITY_MANY,
    GRCORE_PHASE_DECIDE, nullptr, probe_handler};
inline const GRCORE_Key kActKey = {"act", GRCORE_CARDINALITY_MANY,
    GRCORE_PHASE_ACT, nullptr, probe_handler};
inline const GRCORE_Key kObserveKey = {"observe", GRCORE_CARDINALITY_MANY,
    GRCORE_PHASE_OBSERVE, nullptr, probe_handler};
inline const GRCORE_Key kYieldKey = {"yield", GRCORE_CARDINALITY_MANY,
    GRCORE_PHASE_YIELD, nullptr, probe_handler};

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
inline const GRCORE_EngineDescriptor kAlpha = {"alpha", alpha_slot_kind,
    alpha_locate, alpha_inspect,
    GRCORE_ScopeInterface{alpha_scope_count, alpha_scope, alpha_variable},
    GRCORE_ConservativeDecoder{0, 0, 0}, nullptr, nullptr};

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
inline const GRCORE_EngineDescriptor kBeta = {"beta", beta_slot_kind,
    beta_locate, beta_inspect, GRCORE_ScopeInterface{nullptr, nullptr, nullptr},
    GRCORE_ConservativeDecoder{0xFFFF0, 4, 0x1000}, nullptr, nullptr};

inline const GRCORE_EngineDescriptor kGamma = {"gamma", nullptr, nullptr,
    nullptr, GRCORE_ScopeInterface{nullptr, nullptr, nullptr},
    GRCORE_ConservativeDecoder{0, 0, 0}, nullptr, nullptr};

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
inline const GRCORE_EngineDescriptor kDelta = {"delta", delta_slot_kind, nullptr,
    nullptr, GRCORE_ScopeInterface{nullptr, nullptr, nullptr},
    GRCORE_ConservativeDecoder{0xFF00, 8, 0x40}, delta_roots, delta_unwind};

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

#endif /* GHOTI_IO_GRCORE_TEST_HELPERS_H */

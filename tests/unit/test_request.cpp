/**
 * @file
 *
 * Requests and ports: kinds, the request word, the overflow set, port
 * lifetime and reference counting, posting from other threads, and the wake.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../../src/b/context_internal.h"
#include "../../src/b/group_internal.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace {

const GRCORE_Key kSvc = GRCORE_KEY_INIT("svc", GRCORE_CARDINALITY_MANY, GRCORE_PHASE_NONE,
    nullptr, nullptr, nullptr, nullptr, nullptr);
const GRCORE_Key kOther = GRCORE_KEY_INIT("other", GRCORE_CARDINALITY_MANY,
    GRCORE_PHASE_NONE, nullptr, nullptr, nullptr, nullptr, nullptr);

uint64_t word_of(const GRCORE_Context * c) {
  return __atomic_load_n(&c->request_word, __ATOMIC_ACQUIRE);
}

constexpr uint64_t kOverflowBit = UINT64_C(1) << 63;

GRCORE_RequestKind define(GRCORE_Context * c, const GRCORE_Key * k) {
  GRCORE_RequestKind kind = 0;
  EXPECT_EQ(grcore_context_request_kind(c, k, &kind), GRCORE_OK);
  return kind;
}

} // namespace

TEST(Request, ServiceKindsStartAtFiveAndAreAttributedToTheirKeys) {
  RunWorld w;
  EXPECT_EQ(GRCORE_REQUEST_FIRST_KEYED, 5);
  GRCORE_RequestKind a = define(w.ctx, &kSvc);
  GRCORE_RequestKind b = define(w.ctx, &kSvc); // the same key again
  GRCORE_RequestKind c = define(w.ctx, &kOther);
  EXPECT_EQ(a, 5u);
  EXPECT_EQ(b, 6u);
  EXPECT_EQ(c, 7u);
  EXPECT_EQ(grcore_context_request_kind_key(w.ctx, a), &kSvc);
  EXPECT_EQ(grcore_context_request_kind_key(w.ctx, c), &kOther);
  EXPECT_EQ(grcore_context_request_kind_key(w.ctx, 8), nullptr);
  EXPECT_EQ(grcore_context_request_kind_key(w.ctx, GRCORE_REQUEST_FUEL), nullptr);
  EXPECT_EQ(grcore_context_request_kind_key(nullptr, a), nullptr);
}

TEST(Request, DefiningAKindIsRefusedWhenItCannotBeDone) {
  RunWorld w;
  GRCORE_RequestKind kind = 99;
  EXPECT_EQ(grcore_context_request_kind(nullptr, &kSvc, &kind), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_request_kind(w.ctx, nullptr, &kind), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_request_kind(w.ctx, &kSvc, nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(kind, 99u); // output untouched
  w.ctx->config = GRCORE_CONFIG_RUNNING;
  EXPECT_EQ(grcore_context_request_kind(w.ctx, &kSvc, &kind), GRCORE_ERR_INVALID);
  w.ctx->config = GRCORE_CONFIG_PARKED_OUTSIDE;
  GRCORE_Result other = GRCORE_OK;
  std::thread([&] { other = grcore_context_request_kind(w.ctx, &kSvc, &kind); })
      .join();
  EXPECT_EQ(other, GRCORE_ERR_INVALID);
  EXPECT_EQ(w.ctx->kind_count, 0u);
  // Allowed parked inside, at-poll and paused.
  for (GRCORE_ContextConfig cfg : {GRCORE_CONFIG_PARKED_INSIDE,
           GRCORE_CONFIG_AT_POLL, GRCORE_CONFIG_PAUSED}) {
    w.ctx->config = cfg;
    EXPECT_EQ(grcore_context_request_kind(w.ctx, &kSvc, &kind), GRCORE_OK);
  }
  EXPECT_EQ(w.ctx->kind_count, 3u);
  w.ctx->config = GRCORE_CONFIG_PARKED_OUTSIDE;
}

TEST(Request, PortIsCreatedOnceAndReferenceCounted) {
  RunWorld w;
  EXPECT_EQ(grcore_group_port_count(w.group), 0u);
  GRCORE_Port *p1, *p2;
  ASSERT_EQ(grcore_context_port(w.ctx, &p1), GRCORE_OK);
  ASSERT_EQ(grcore_context_port(w.ctx, &p2), GRCORE_OK);
  EXPECT_EQ(p1, p2);
  EXPECT_EQ(grcore_group_port_count(w.group), 1u);
  EXPECT_EQ(grcore_port_retain(p1), p1);
  grcore_port_release(p1);
  grcore_port_release(p2);
  // The context's own reference and one more are still held.
  EXPECT_EQ(grcore_group_port_count(w.group), 1u);
  grcore_port_release(p1); // the retain's
  EXPECT_EQ(grcore_group_port_count(w.group), 1u); // the context's
  EXPECT_EQ(grcore_port_retain(nullptr), nullptr);
  grcore_port_release(nullptr); // ignored
  // The port is charged to the group, not the context.
  EXPECT_GT(grcore_group_memory_blocks(w.group), 0u);
  EXPECT_EQ(grcore_context_memory_blocks(w.ctx), 0u);
}

TEST(Request, PortRefusals) {
  RunWorld w;
  GRCORE_Port * p = nullptr;
  EXPECT_EQ(grcore_context_port(nullptr, &p), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_port(w.ctx, nullptr), GRCORE_ERR_INVALID);
  GRCORE_Result other = GRCORE_OK;
  std::thread([&] { other = grcore_context_port(w.ctx, &p); }).join();
  EXPECT_EQ(other, GRCORE_ERR_INVALID);
  EXPECT_EQ(p, nullptr);
  EXPECT_EQ(grcore_group_port_count(w.group), 0u);
  EXPECT_EQ(grcore_port_post(nullptr, GRCORE_REQUEST_TIME), GRCORE_ERR_INVALID);
}

TEST(Request, DestroyingTheContextFreesAPortNobodyHolds) {
  RunWorld w;
  GRCORE_Port * p;
  ASSERT_EQ(grcore_context_port(w.ctx, &p), GRCORE_OK);
  grcore_port_release(p);
  uint64_t before = grcore_group_memory_blocks(w.group);
  EXPECT_GT(before, 0u);
  ASSERT_EQ(grcore_context_destroy(w.ctx), GRCORE_OK);
  w.ctx = nullptr;
  EXPECT_EQ(grcore_group_port_count(w.group), 0u);
  EXPECT_EQ(grcore_group_memory_blocks(w.group), 0u);
  EXPECT_EQ(grcore_group_memory_in_use(w.group), 0u);
}

TEST(Request, APortOutlivesItsContextAndRefusesPosts) {
  RunWorld w;
  GRCORE_RequestKind k = define(w.ctx, &kSvc);
  GRCORE_Port * p;
  ASSERT_EQ(grcore_context_port(w.ctx, &p), GRCORE_OK);
  ASSERT_EQ(grcore_port_post(p, k), GRCORE_OK);
  ASSERT_EQ(grcore_context_destroy(w.ctx), GRCORE_OK);
  w.ctx = nullptr;
  EXPECT_EQ(grcore_group_context_count(w.group), 0u);
  EXPECT_EQ(grcore_group_port_count(w.group), 1u);
  // Posting is refused, and the memory is still valid (ASan checks this).
  EXPECT_EQ(grcore_port_post(p, GRCORE_REQUEST_TIME), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_port_post(p, k), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_port_retain(p), p);
  grcore_port_release(p);
  EXPECT_EQ(grcore_group_port_count(w.group), 1u);
  // The group cannot be destroyed while the port lives.
  EXPECT_EQ(grcore_group_destroy(w.group), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_group_port_count(w.group), 1u);
  grcore_port_release(p); // the last one
  EXPECT_EQ(grcore_group_port_count(w.group), 0u);
  EXPECT_EQ(grcore_group_memory_blocks(w.group), 0u);
}

TEST(Request, GroupDestroyWithALivePortIsRefusedAndLeavesTheGroupIntact) {
  GRCORE_Group * g;
  GRCORE_Context * c;
  ASSERT_EQ(grcore_group_create(nullptr, nullptr, &g), GRCORE_OK);
  ASSERT_EQ(grcore_context_create(g, nullptr, &c), GRCORE_OK);
  GRCORE_Port * p;
  ASSERT_EQ(grcore_context_port(c, &p), GRCORE_OK);
  ASSERT_EQ(grcore_context_destroy(c), GRCORE_OK);
  EXPECT_EQ(grcore_group_destroy(g), GRCORE_ERR_INVALID);
  // Intact: still usable.
  GRCORE_Context * c2;
  ASSERT_EQ(grcore_context_create(g, nullptr, &c2), GRCORE_OK);
  ASSERT_EQ(grcore_context_destroy(c2), GRCORE_OK);
  grcore_port_release(p);
  EXPECT_EQ(grcore_group_destroy(g), GRCORE_OK);
}

TEST(Request, PostSetsTheBitAndTheOwnerReadsAndClearsIt) {
  RunWorld w;
  GRCORE_RequestKind k = define(w.ctx, &kSvc);
  GRCORE_Port * p;
  ASSERT_EQ(grcore_context_port(w.ctx, &p), GRCORE_OK);
  EXPECT_EQ(word_of(w.ctx), 0u);
  for (GRCORE_RequestKind kind :
      {(GRCORE_RequestKind)GRCORE_REQUEST_TERMINATE,
          (GRCORE_RequestKind)GRCORE_REQUEST_TIME,
          (GRCORE_RequestKind)GRCORE_REQUEST_INTERRUPT, k}) {
    EXPECT_FALSE(grcore_context_request_pending(w.ctx, kind));
    EXPECT_EQ(grcore_port_post(p, kind), GRCORE_OK);
    EXPECT_EQ(grcore_port_post(p, kind), GRCORE_OK); // twice is not an error
    EXPECT_TRUE(grcore_context_request_pending(w.ctx, kind));
    EXPECT_NE(word_of(w.ctx) & (UINT64_C(1) << kind), 0u);
  }
  EXPECT_EQ(grcore_context_clear_request(w.ctx, GRCORE_REQUEST_TIME), GRCORE_OK);
  EXPECT_FALSE(grcore_context_request_pending(w.ctx, GRCORE_REQUEST_TIME));
  EXPECT_EQ(grcore_context_clear_request(w.ctx, k), GRCORE_OK);
  EXPECT_FALSE(grcore_context_request_pending(w.ctx, k));
  EXPECT_EQ(grcore_context_clear_request(w.ctx, k), GRCORE_OK); // not pending
  EXPECT_TRUE(grcore_context_request_pending(w.ctx, GRCORE_REQUEST_INTERRUPT));
  grcore_port_release(p);
}

TEST(Request, DerivedAndUndefinedKindsCannotBePostedAndNothingIsSet) {
  RunWorld w;
  define(w.ctx, &kSvc); // kind 5 exists; 6 does not
  GRCORE_Port * p;
  ASSERT_EQ(grcore_context_port(w.ctx, &p), GRCORE_OK);
  EXPECT_EQ(grcore_port_post(p, GRCORE_REQUEST_FUEL), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_port_post(p, GRCORE_REQUEST_MEMORY), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_port_post(p, 6), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_port_post(p, 62), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_port_post(p, 63), GRCORE_ERR_INVALID); // overflow, undefined
  EXPECT_EQ(grcore_port_post(p, UINT32_MAX), GRCORE_ERR_INVALID);
  EXPECT_EQ(word_of(w.ctx), 0u);
  grcore_port_release(p);
}

TEST(Request, ClearRefusesWhatOnlyTheLibraryMayClear) {
  RunWorld w;
  EXPECT_EQ(grcore_context_clear_request(w.ctx, GRCORE_REQUEST_TERMINATE),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_clear_request(w.ctx, GRCORE_REQUEST_FUEL),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_clear_request(w.ctx, GRCORE_REQUEST_MEMORY),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_clear_request(w.ctx, 9), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_clear_request(nullptr, GRCORE_REQUEST_TIME),
      GRCORE_ERR_INVALID);
  GRCORE_Result other = GRCORE_OK;
  std::thread([&] {
    other = grcore_context_clear_request(w.ctx, GRCORE_REQUEST_TIME);
  }).join();
  EXPECT_EQ(other, GRCORE_ERR_INVALID);
}

TEST(Request, TerminateByTheOwnerSetsTheBitAndIsRefusedOffOwner) {
  RunWorld w;
  GRCORE_Result other = GRCORE_OK;
  std::thread([&] { other = grcore_context_terminate(w.ctx); }).join();
  EXPECT_EQ(other, GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_terminate(nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(word_of(w.ctx), 0u);
  EXPECT_EQ(grcore_context_terminate(w.ctx), GRCORE_OK);
  EXPECT_TRUE(grcore_context_request_pending(w.ctx, GRCORE_REQUEST_TERMINATE));
  grcore_context_clear_terminate(w.ctx);
  EXPECT_EQ(word_of(w.ctx), 0u);
}

TEST(Request, SeventyKindsOverflowTheWordAndTheOverflowSetIsSignalled) {
  RunWorld w;
  std::vector<GRCORE_RequestKind> kinds;
  for (int i = 0; i < 70; i++) {
    kinds.push_back(define(w.ctx, &kSvc));
  }
  ASSERT_EQ(kinds.front(), 5u);
  ASSERT_EQ(kinds.back(), 74u);
  GRCORE_Port * p;
  ASSERT_EQ(grcore_context_port(w.ctx, &p), GRCORE_OK);

  EXPECT_EQ(grcore_port_post(p, 66), GRCORE_OK);
  EXPECT_EQ(word_of(w.ctx), kOverflowBit); // only the signal, not a kind bit
  EXPECT_TRUE(grcore_context_request_pending(w.ctx, 66));
  EXPECT_FALSE(grcore_context_request_pending(w.ctx, 65));
  EXPECT_FALSE(grcore_context_request_pending(w.ctx, 63));
  EXPECT_FALSE(grcore_context_request_pending(w.ctx, 74));

  // Two overflow kinds in different words of the set.
  EXPECT_EQ(grcore_port_post(p, 74), GRCORE_OK);
  EXPECT_EQ(grcore_port_post(p, 63), GRCORE_OK);
  EXPECT_TRUE(grcore_context_request_pending(w.ctx, 63));
  EXPECT_EQ(grcore_context_clear_request(w.ctx, 66), GRCORE_OK);
  EXPECT_FALSE(grcore_context_request_pending(w.ctx, 66));
  EXPECT_NE(word_of(w.ctx) & kOverflowBit, 0u); // others still set
  EXPECT_EQ(grcore_context_clear_request(w.ctx, 66), GRCORE_OK);
  EXPECT_EQ(grcore_context_clear_request(w.ctx, 63), GRCORE_OK);
  EXPECT_NE(word_of(w.ctx) & kOverflowBit, 0u);
  EXPECT_EQ(grcore_context_clear_request(w.ctx, 74), GRCORE_OK);
  EXPECT_EQ(word_of(w.ctx), 0u); // the signal drops with the last one

  // The word's own highest service kind, 62, is a bit and not overflow.
  EXPECT_EQ(grcore_port_post(p, 62), GRCORE_OK);
  EXPECT_EQ(word_of(w.ctx), UINT64_C(1) << 62);
  grcore_port_release(p);
}

TEST(Request, PendingForAnOverflowKindIsFalseWithNoPort) {
  RunWorld w;
  for (int i = 0; i < 70; i++) {
    define(w.ctx, &kSvc);
  }
  EXPECT_FALSE(grcore_context_request_pending(w.ctx, 66));
  EXPECT_EQ(grcore_context_clear_request(w.ctx, 66), GRCORE_OK);
}

TEST(Request, ManyThreadsPostingConcurrentlyLoseNothing) {
  RunWorld w;
  std::vector<GRCORE_RequestKind> kinds;
  for (int i = 0; i < 70; i++) {
    kinds.push_back(define(w.ctx, &kSvc));
  }
  GRCORE_Port * p;
  ASSERT_EQ(grcore_context_port(w.ctx, &p), GRCORE_OK);
  const int threads = 4;
  const int rounds = 400;
  std::atomic<int> ok{0};
  std::atomic<int> done{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < threads; t++) {
    ts.emplace_back([&, t] {
      GRCORE_Port * mine = grcore_port_retain(p);
      for (int r = 0; r < rounds; r++) {
        // Word kinds and overflow kinds, spread over the threads.
        GRCORE_RequestKind k = kinds[static_cast<size_t>((t * 17 + r) % 70)];
        if (grcore_port_post(mine, k) == GRCORE_OK) {
          ok++;
        }
      }
      grcore_port_release(mine);
      done++;
    });
  }
  // The owner reads and clears while they post.
  std::vector<int> seen(70, 0);
  while (done.load() < threads) {
    // Valgrind runs threads one at a time; without a yield this loop would
    // spend its whole slice spinning.
    std::this_thread::yield();
    for (size_t i = 0; i < kinds.size(); i++) {
      if (grcore_context_request_pending(w.ctx, kinds[i])) {
        seen[i]++;
        grcore_context_clear_request(w.ctx, kinds[i]);
      }
    }
  }
  for (auto & t : ts) {
    t.join();
  }
  EXPECT_EQ(ok.load(), threads * rounds);
  // Every kind that was posted was seen at least once, or is pending now.
  for (size_t i = 0; i < kinds.size(); i++) {
    bool posted = false;
    for (int t = 0; t < threads && !posted; t++) {
      for (int r = 0; r < rounds && !posted; r++) {
        posted = static_cast<size_t>((t * 17 + r) % 70) == i;
      }
    }
    if (posted) {
      EXPECT_TRUE(seen[i] > 0 || grcore_context_request_pending(w.ctx, kinds[i]))
          << "kind index " << i;
    }
  }
  grcore_port_release(p);
}

TEST(Request, PostersRacingTheContextsDestructionGetOkOrInvalidAndNothingElse) {
  RunWorld w;
  GRCORE_RequestKind k = define(w.ctx, &kSvc);
  GRCORE_Port * p;
  ASSERT_EQ(grcore_context_port(w.ctx, &p), GRCORE_OK);
  std::atomic<bool> go{false};
  std::atomic<int> bad{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < 3; t++) {
    ts.emplace_back([&] {
      GRCORE_Port * mine = grcore_port_retain(p);
      while (!go.load()) {
        std::this_thread::yield();
      }
      for (int r = 0; r < 4000; r++) {
        GRCORE_Result res = grcore_port_post(mine, r % 2 ? k : static_cast<GRCORE_RequestKind>(GRCORE_REQUEST_TIME));
        if (res != GRCORE_OK && res != GRCORE_ERR_INVALID) {
          bad++;
        }
      }
      grcore_port_release(mine);
    });
  }
  go = true;
  std::this_thread::sleep_for(std::chrono::microseconds(200));
  ASSERT_EQ(grcore_context_destroy(w.ctx), GRCORE_OK);
  w.ctx = nullptr;
  for (auto & t : ts) {
    t.join();
  }
  EXPECT_EQ(bad.load(), 0);
  grcore_port_release(p);
  EXPECT_EQ(grcore_group_port_count(w.group), 0u);
}

TEST(Wait, ReturnsWokenWhenAnotherThreadPostsAndLeavesTheContextRunning) {
  RunWorld w;
  GRCORE_Port * p;
  ASSERT_EQ(grcore_context_port(w.ctx, &p), GRCORE_OK);
  bool woken = false;
  GRCORE_Result r = GRCORE_ERR_INTERNAL;
  GRCORE_ContextState during = GRCORE_CONTEXT_PAUSED;
  Fn fn{[&](GRCORE_Context * c) {
    std::thread poster([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      EXPECT_EQ(grcore_port_post(p, GRCORE_REQUEST_TIME), GRCORE_OK);
    });
    r = grcore_context_wait(c, GRCORE_UNLIMITED, &woken);
    during = grcore_context_state(c);
    poster.join();
    return GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_EQ(r, GRCORE_OK);
  EXPECT_TRUE(woken);
  EXPECT_EQ(during, GRCORE_CONTEXT_RUNNING);
  // The wake does not consume the request.
  EXPECT_TRUE(grcore_context_request_pending(w.ctx, GRCORE_REQUEST_TIME));
  grcore_port_release(p);
}

TEST(Wait, TimeoutReturnsNotWokenAndRunning) {
  RunWorld w;
  bool woken = true;
  GRCORE_ContextState during = GRCORE_CONTEXT_PAUSED;
  Fn fn{[&](GRCORE_Context * c) {
    auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(grcore_context_wait(c, 5u * 1000u * 1000u, &woken), GRCORE_OK);
    auto took = std::chrono::steady_clock::now() - start;
    EXPECT_GE(took, std::chrono::milliseconds(4));
    EXPECT_LT(took, std::chrono::seconds(2));
    // 999999999 ns forces the carry from nanoseconds into seconds.
    start = std::chrono::steady_clock::now();
    bool again = true;
    EXPECT_EQ(grcore_context_wait(c, 999999999u, &again), GRCORE_OK);
    took = std::chrono::steady_clock::now() - start;
    EXPECT_FALSE(again);
    EXPECT_GE(took, std::chrono::milliseconds(900));
    EXPECT_LT(took, std::chrono::seconds(2));
    during = grcore_context_state(c);
    return GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_FALSE(woken);
  EXPECT_EQ(during, GRCORE_CONTEXT_RUNNING);
}

TEST(Wait, AStaleDerivedBitDoesNotEndTheWait) {
  RunWorld w(GRCORE_UNLIMITED, 1000, 500);
  const GRCORE_Allocator * a = grcore_context_allocator(w.ctx);
  void * big = a->malloc_fn(a->ctx, 1200); // memory request raised
  grcore_context_charge_fuel(w.ctx, 1);
  ASSERT_EQ(grcore_context_set_fuel(w.ctx, 0), GRCORE_OK); // fuel raised
  ASSERT_EQ(grcore_context_set_fuel(w.ctx, GRCORE_UNLIMITED), GRCORE_OK);
  a->free_fn(a->ctx, big); // bit now stale: nothing refreshed it
  EXPECT_NE(__atomic_load_n(&w.ctx->request_word, __ATOMIC_ACQUIRE), 0u);
  bool woken = true;
  Fn fn{[&](GRCORE_Context * c) {
    EXPECT_EQ(grcore_context_wait(c, 5u * 1000u * 1000u, &woken), GRCORE_OK);
    return GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_FALSE(woken);
}

TEST(Wait, ReturnsAtOnceWhenARequestIsAlreadyPending) {
  RunWorld w;
  ASSERT_EQ(grcore_context_terminate(w.ctx), GRCORE_OK);
  bool woken = false;
  Fn fn{[&](GRCORE_Context * c) {
    EXPECT_EQ(grcore_context_wait(c, GRCORE_UNLIMITED, &woken), GRCORE_OK);
    return GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_TRUE(woken);
}

TEST(Wait, IsRefusedOutsideRunningAndWithNullArguments) {
  RunWorld w;
  bool woken = true;
  EXPECT_EQ(grcore_context_wait(w.ctx, 1, &woken), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_wait(nullptr, 1, &woken), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_wait(w.ctx, 1, nullptr), GRCORE_ERR_INVALID);
  EXPECT_TRUE(woken); // untouched
  EXPECT_EQ(w.ctx->config, GRCORE_CONFIG_PARKED_OUTSIDE);
  EXPECT_EQ(grcore_group_port_count(w.group), 0u); // no port was made
}

TEST(Request, EveryAllocationFailureInKindsAndPortSetupIsOomAndLeaksNothing) {
  bool succeeded = false;
  int failures = 0;
  for (long n = 1; n < 200 && !succeeded; n++) {
    TrackingAllocator t;
    GRCORE_Group * g;
    ASSERT_EQ(grcore_group_create(t.get(), nullptr, &g), GRCORE_OK);
    GRCORE_Context * c;
    ASSERT_EQ(grcore_context_create(g, nullptr, &c), GRCORE_OK);
    t.fail_at = t.calls + n;
    GRCORE_Result r = GRCORE_OK;
    GRCORE_Port * port = nullptr;
    uint32_t defined = 0;
    for (int i = 0; i < 70 && r == GRCORE_OK; i++) {
      GRCORE_RequestKind kind;
      r = grcore_context_request_kind(c, &kSvc, &kind);
      if (r == GRCORE_OK) {
        defined++;
      }
    }
    if (r == GRCORE_OK) {
      r = grcore_context_port(c, &port);
    }
    if (r == GRCORE_OK) {
      r = grcore_port_post(port, 66); // grows the overflow set
    }
    if (r == GRCORE_OK) {
      r = grcore_port_post(port, 74);
    }
    if (r == GRCORE_OK) {
      succeeded = true;
    } else {
      failures++;
      EXPECT_EQ(r, GRCORE_ERR_OOM) << "n=" << n;
      // A refusal leaves the kind table as it was.
      EXPECT_EQ(c->kind_count, defined) << "n=" << n;
    }
    t.fail_at = 0;
    grcore_port_release(port);
    EXPECT_EQ(grcore_context_destroy(c), GRCORE_OK);
    EXPECT_EQ(grcore_group_port_count(g), 0u) << "n=" << n;
    EXPECT_EQ(grcore_group_memory_blocks(g), 0u) << "n=" << n;
    EXPECT_EQ(grcore_group_destroy(g), GRCORE_OK);
    EXPECT_EQ(t.live, 0) << "n=" << n;
  }
  EXPECT_TRUE(succeeded);
  EXPECT_GE(failures, 5); // kind table growths, the port, the overflow set
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

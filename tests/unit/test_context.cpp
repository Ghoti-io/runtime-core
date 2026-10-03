/**
 * @file
 *
 * The context: creation, lifecycle, ownership, keys and teardown.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../../src/b/context_internal.h"

#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

std::vector<std::string> g_log;
int g_blocks_at_last_destroy = -1;

void log_destroy(GRCORE_Context *, void * value) {
  g_log.push_back(static_cast<const char *>(value));
}

const GRCORE_Key kOne = {"one", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_NONE,
    log_destroy};
const GRCORE_Key kMany = {"many", GRCORE_CARDINALITY_MANY,
    GRCORE_PHASE_DECIDE, log_destroy};
const GRCORE_Key kQuiet = {"quiet", GRCORE_CARDINALITY_MANY,
    GRCORE_PHASE_YIELD, nullptr};

struct World {
  GRCORE_Group * group = nullptr;
  GRCORE_Context * ctx = nullptr;
  explicit World(const GRCORE_Options * o = nullptr,
      const GRCORE_Allocator * a = nullptr) {
    EXPECT_EQ(grcore_group_create(a, nullptr, &group), GRCORE_OK);
    EXPECT_EQ(grcore_context_create(group, o, &ctx), GRCORE_OK);
    g_log.clear();
  }
  ~World() {
    if (ctx != nullptr) {
      if (!grcore_context_is_owner(ctx)) {
        EXPECT_EQ(grcore_context_acquire(ctx), GRCORE_OK);
      }
      ctx->config = GRCORE_CONFIG_PARKED_OUTSIDE;
      EXPECT_EQ(grcore_context_destroy(ctx), GRCORE_OK);
    }
    EXPECT_EQ(grcore_group_destroy(group), GRCORE_OK);
  }
};

// Call from another thread and collect the result.
template <typename F>
GRCORE_Result on_other_thread(F f) {
  GRCORE_Result r = GRCORE_OK;
  std::thread t([&] { r = f(); });
  t.join();
  return r;
}

} // namespace

TEST(Context, CreateGivesAParkedOwnedUnlimitedContext) {
  World w;
  EXPECT_EQ(grcore_context_state(w.ctx), GRCORE_CONTEXT_PARKED);
  EXPECT_EQ(w.ctx->config, GRCORE_CONFIG_PARKED_OUTSIDE);
  EXPECT_TRUE(grcore_context_is_owner(w.ctx));
  EXPECT_EQ(grcore_context_fuel(w.ctx), GRCORE_UNLIMITED);
  EXPECT_EQ(grcore_context_memory_bytes(w.ctx), GRCORE_UNLIMITED);
  EXPECT_EQ(grcore_context_guest_depth(w.ctx), GRCORE_UNLIMITED);
  EXPECT_EQ(grcore_context_native_depth(w.ctx), GRCORE_UNLIMITED);
  EXPECT_EQ(grcore_group_context_count(w.group), 1u);
  EXPECT_EQ(grcore_context_group(w.ctx), w.group);
  EXPECT_EQ(grcore_context_registration_count(w.ctx), 0u);
  EXPECT_NE(grcore_context_allocator(w.ctx), nullptr);
  EXPECT_NE(grcore_context_page_provider(w.ctx), nullptr);
}

TEST(Context, NullArgumentsAreRefused) {
  GRCORE_Group * g;
  ASSERT_EQ(grcore_group_create(nullptr, nullptr, &g), GRCORE_OK);
  GRCORE_Context * c;
  EXPECT_EQ(grcore_context_create(nullptr, nullptr, &c), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_create(g, nullptr, nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_destroy(nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_acquire(nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_release(nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_park(nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_unpark(nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_register(nullptr, &kOne, g), GRCORE_ERR_INVALID);
  EXPECT_FALSE(grcore_context_is_owner(nullptr));
  EXPECT_FALSE(grcore_context_guest_state_readable(nullptr));
  EXPECT_EQ(grcore_context_slot(nullptr, &kOne), nullptr);
  EXPECT_EQ(grcore_context_fuel(nullptr), GRCORE_UNLIMITED);
  EXPECT_EQ(grcore_context_memory_in_use(nullptr), 0u);
  EXPECT_EQ(grcore_context_memory_peak(nullptr), 0u);
  EXPECT_EQ(grcore_context_memory_blocks(nullptr), 0u);
  EXPECT_EQ(grcore_context_group(nullptr), nullptr);
  EXPECT_EQ(grcore_context_allocator(nullptr), nullptr);
  EXPECT_EQ(grcore_context_page_provider(nullptr), nullptr);
  EXPECT_EQ(grcore_context_options(nullptr), nullptr);
  EXPECT_EQ(grcore_context_registration_count(nullptr), 0u);
  EXPECT_EQ(grcore_context_registration(nullptr, 0, nullptr, nullptr),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_group_destroy(g), GRCORE_OK);
}

TEST(Context, ConfiguredOptionsAreCopiedNotShared) {
  GRCORE_Options * o;
  ASSERT_EQ(grcore_options_create(nullptr, &o), GRCORE_OK);
  grcore_options_set_fuel(o, 1000);
  grcore_options_set_memory_bytes(o, 1 << 20);
  grcore_options_set_guest_depth(o, 64);
  grcore_options_set_native_depth(o, 32);
  ASSERT_EQ(grcore_options_set_keyed(o, &kOne, "cfg", 3), GRCORE_OK);
  World w(o);
  grcore_options_set_fuel(o, 7);
  grcore_options_set_keyed(o, &kOne, "changed", 7);
  grcore_options_destroy(o); // the context outlives the caller's object
  EXPECT_EQ(grcore_context_fuel(w.ctx), 1000u);
  EXPECT_EQ(grcore_context_memory_bytes(w.ctx), 1u << 20);
  EXPECT_EQ(grcore_context_guest_depth(w.ctx), 64u);
  EXPECT_EQ(grcore_context_native_depth(w.ctx), 32u);
  const void * p;
  size_t n;
  ASSERT_EQ(grcore_options_get_keyed(grcore_context_options(w.ctx), &kOne, &p, &n),
      GRCORE_OK);
  EXPECT_EQ(std::string(static_cast<const char *>(p), n), "cfg");
}

TEST(Context, CardinalityOneRefusesASecondAndKeepsTheFirst) {
  World w;
  char a[] = "a", b[] = "b";
  EXPECT_EQ(grcore_context_register(w.ctx, &kOne, a), GRCORE_OK);
  EXPECT_EQ(grcore_context_register(w.ctx, &kOne, b), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_slot(w.ctx, &kOne), a);
  EXPECT_EQ(grcore_context_registration_count(w.ctx), 1u);
}

TEST(Context, CardinalityManyKeepsBothInOrder) {
  World w;
  char a[] = "a", b[] = "b";
  EXPECT_EQ(grcore_context_register(w.ctx, &kMany, a), GRCORE_OK);
  EXPECT_EQ(grcore_context_register(w.ctx, &kMany, b), GRCORE_OK);
  EXPECT_EQ(grcore_context_registration_count(w.ctx), 2u);
  EXPECT_EQ(grcore_context_slot(w.ctx, &kMany), a); // the first
  const GRCORE_Key * k;
  void * v;
  ASSERT_EQ(grcore_context_registration(w.ctx, 1, &k, &v), GRCORE_OK);
  EXPECT_EQ(k, &kMany);
  EXPECT_EQ(v, b);
  EXPECT_EQ(grcore_context_registration(w.ctx, 2, &k, &v), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_registration(w.ctx, 0, nullptr, nullptr), GRCORE_OK);
}

TEST(Context, KeysAreIdentifiedByAddressNotName) {
  World w;
  static const GRCORE_Key twin = {"one", GRCORE_CARDINALITY_ONE,
      GRCORE_PHASE_NONE, nullptr};
  char a[] = "a", b[] = "b";
  EXPECT_EQ(grcore_context_register(w.ctx, &kOne, a), GRCORE_OK);
  EXPECT_EQ(grcore_context_register(w.ctx, &twin, b), GRCORE_OK);
  EXPECT_EQ(grcore_context_slot(w.ctx, &twin), b);
  EXPECT_EQ(grcore_context_slot(w.ctx, &kMany), nullptr);
}

TEST(Context, RegisterRefusalsLeaveTheTableUnchanged) {
  World w;
  char a[] = "a";
  EXPECT_EQ(grcore_context_register(w.ctx, &kMany, nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_register(w.ctx, nullptr, a), GRCORE_ERR_INVALID);
  GRCORE_Key bad_card = {"x", static_cast<GRCORE_Cardinality>(9),
      GRCORE_PHASE_NONE, nullptr};
  GRCORE_Key bad_phase = {"x", GRCORE_CARDINALITY_MANY,
      static_cast<GRCORE_Phase>(9), nullptr};
  EXPECT_EQ(grcore_context_register(w.ctx, &bad_card, a), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_register(w.ctx, &bad_phase, a), GRCORE_ERR_INVALID);
  ASSERT_EQ(grcore_context_transition(w.ctx, GRCORE_CONFIG_RUNNING), GRCORE_OK);
  EXPECT_EQ(grcore_context_register(w.ctx, &kMany, a), GRCORE_ERR_INVALID);
  ASSERT_EQ(grcore_context_transition(w.ctx, GRCORE_CONFIG_PARKED_OUTSIDE),
      GRCORE_OK);
  EXPECT_EQ(grcore_context_registration_count(w.ctx), 0u);
}

TEST(Context, RegisterFromAnotherThreadIsRefused) {
  World w;
  char a[] = "a";
  EXPECT_EQ(on_other_thread([&] {
    return grcore_context_register(w.ctx, &kMany, a);
  }), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_registration_count(w.ctx), 0u);
}

TEST(Context, RegisterIsAllowedWhileParkedAtPollOrPaused) {
  World w;
  char a[] = "a";
  w.ctx->config = GRCORE_CONFIG_PARKED_INSIDE;
  EXPECT_EQ(grcore_context_register(w.ctx, &kMany, a), GRCORE_OK);
  w.ctx->config = GRCORE_CONFIG_AT_POLL;
  EXPECT_EQ(grcore_context_register(w.ctx, &kMany, a), GRCORE_OK);
  w.ctx->config = GRCORE_CONFIG_PAUSED;
  EXPECT_EQ(grcore_context_register(w.ctx, &kMany, a), GRCORE_OK);
}

TEST(Context, RegisterOutOfMemoryLeavesTheTableUnchanged) {
  TrackingAllocator t;
  World w(nullptr, t.get());
  char a[] = "a";
  t.fail_at = t.calls + 1;
  EXPECT_EQ(grcore_context_register(w.ctx, &kMany, a), GRCORE_ERR_OOM);
  EXPECT_EQ(grcore_context_registration_count(w.ctx), 0u);
  EXPECT_EQ(grcore_context_slot(w.ctx, &kMany), nullptr);
  t.fail_at = 0;
  // Grow past the first capacity, then fail the growth.
  for (int i = 0; i < 4; i++) {
    ASSERT_EQ(grcore_context_register(w.ctx, &kQuiet, a), GRCORE_OK);
  }
  t.fail_at = t.calls + 1;
  EXPECT_EQ(grcore_context_register(w.ctx, &kQuiet, a), GRCORE_ERR_OOM);
  EXPECT_EQ(grcore_context_registration_count(w.ctx), 4u);
  t.fail_at = 0;
  EXPECT_EQ(grcore_context_register(w.ctx, &kQuiet, a), GRCORE_OK);
  EXPECT_EQ(grcore_context_registration_count(w.ctx), 5u);
}

TEST(Context, TeardownRunsDestructorsInReverseRegistrationOrder) {
  World w;
  static char a1[] = "A1", b[] = "B", a2[] = "A2";
  ASSERT_EQ(grcore_context_register(w.ctx, &kMany, a1), GRCORE_OK);
  ASSERT_EQ(grcore_context_register(w.ctx, &kOne, b), GRCORE_OK);
  ASSERT_EQ(grcore_context_register(w.ctx, &kMany, a2), GRCORE_OK);
  ASSERT_EQ(grcore_context_register(w.ctx, &kQuiet, a2), GRCORE_OK); // no hook
  GRCORE_Context * c = w.ctx;
  w.ctx = nullptr;
  ASSERT_EQ(grcore_context_destroy(c), GRCORE_OK);
  EXPECT_EQ(g_log, (std::vector<std::string>{"A2", "B", "A1"}));
  EXPECT_EQ(grcore_group_context_count(w.group), 0u);
}

namespace {
void free_through_context(GRCORE_Context * c, void * value) {
  const GRCORE_Allocator * a = grcore_context_allocator(c);
  a->free_fn(a->ctx, value);
  g_blocks_at_last_destroy = static_cast<int>(grcore_context_memory_blocks(c));
}
const GRCORE_Key kHeld = {"held", GRCORE_CARDINALITY_MANY, GRCORE_PHASE_NONE,
    free_through_context};
} // namespace

TEST(Context, MeterBlocksEqualWhatRegistrantsStillHoldAtTheEnd) {
  TrackingAllocator t;
  World w(nullptr, t.get());
  const GRCORE_Allocator * a = grcore_context_allocator(w.ctx);
  for (int i = 0; i < 3; i++) {
    void * p = a->malloc_fn(a->ctx, 100);
    ASSERT_NE(p, nullptr);
    ASSERT_EQ(grcore_context_register(w.ctx, &kHeld, p), GRCORE_OK);
  }
  EXPECT_EQ(grcore_context_memory_blocks(w.ctx), 3u);
  EXPECT_EQ(grcore_context_memory_in_use(w.ctx), 300u);
  GRCORE_Context * c = w.ctx;
  w.ctx = nullptr;
  g_blocks_at_last_destroy = -1;
  ASSERT_EQ(grcore_context_destroy(c), GRCORE_OK);
  EXPECT_EQ(g_blocks_at_last_destroy, 0); // the last destructor saw none left
  EXPECT_EQ(grcore_group_context_count(w.group), 0u);
  // Everything, the registry included, went back to the base allocator.
  EXPECT_EQ(t.live, 1); // the group itself, freed by ~World
}

TEST(Context, MemoryInUseIsNonzeroWhileARegistrantHoldsABlockAndZeroAfter) {
  World w;
  const GRCORE_Allocator * a = grcore_context_allocator(w.ctx);
  void * p = a->malloc_fn(a->ctx, 10);
  ASSERT_EQ(grcore_context_register(w.ctx, &kHeld, p), GRCORE_OK);
  EXPECT_EQ(grcore_context_memory_in_use(w.ctx), 10u);
  EXPECT_EQ(grcore_context_memory_blocks(w.ctx), 1u);
  GRCORE_Context * c = w.ctx;
  w.ctx = nullptr;
  g_blocks_at_last_destroy = -1;
  ASSERT_EQ(grcore_context_destroy(c), GRCORE_OK); // its destructor frees p
  EXPECT_EQ(g_blocks_at_last_destroy, 0);
}

TEST(Context, LifecycleEdgesAreExactlyTheEightLegalOnes) {
  const GRCORE_ContextConfig P = GRCORE_CONFIG_PARKED_OUTSIDE;
  const GRCORE_ContextConfig I = GRCORE_CONFIG_PARKED_INSIDE;
  const GRCORE_ContextConfig R = GRCORE_CONFIG_RUNNING;
  const GRCORE_ContextConfig T = GRCORE_CONFIG_AT_POLL;
  const GRCORE_ContextConfig S = GRCORE_CONFIG_PAUSED;
  const std::vector<std::pair<GRCORE_ContextConfig, GRCORE_ContextConfig>>
      legal = {{P, R}, {R, T}, {T, R}, {T, S}, {S, R}, {R, I}, {I, R}, {R, P}};
  ASSERT_EQ(legal.size(), 8u);
  World w;
  int accepted = 0;
  for (int from = 0; from < GRCORE_CONFIG_COUNT; from++) {
    for (int to = 0; to < GRCORE_CONFIG_COUNT; to++) {
      auto f = static_cast<GRCORE_ContextConfig>(from);
      auto t = static_cast<GRCORE_ContextConfig>(to);
      bool want = false;
      for (auto & e : legal) {
        want = want || (e.first == f && e.second == t);
      }
      w.ctx->config = f;
      GRCORE_Result r = grcore_context_transition(w.ctx, t);
      if (want) {
        accepted++;
        EXPECT_EQ(r, GRCORE_OK) << from << "->" << to;
        EXPECT_EQ(w.ctx->config, t) << from << "->" << to;
      } else {
        EXPECT_EQ(r, GRCORE_ERR_INVALID) << from << "->" << to;
        EXPECT_EQ(w.ctx->config, f) << from << "->" << to;
      }
    }
  }
  EXPECT_EQ(accepted, 8);
  w.ctx->config = P;
  EXPECT_EQ(grcore_context_transition(w.ctx, GRCORE_CONFIG_COUNT),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_transition(nullptr, R), GRCORE_ERR_INVALID);
}

TEST(Context, TransitionFromANonOwnerIsRefused) {
  World w;
  EXPECT_EQ(on_other_thread([&] {
    return grcore_context_transition(w.ctx, GRCORE_CONFIG_RUNNING);
  }), GRCORE_ERR_INVALID);
  EXPECT_EQ(w.ctx->config, GRCORE_CONFIG_PARKED_OUTSIDE);
}

TEST(Context, PublicStateMapsBothParkedConfigurationsToParked) {
  World w;
  const struct {
    GRCORE_ContextConfig config;
    GRCORE_ContextState state;
  } map[] = {
      {GRCORE_CONFIG_PARKED_OUTSIDE, GRCORE_CONTEXT_PARKED},
      {GRCORE_CONFIG_PARKED_INSIDE, GRCORE_CONTEXT_PARKED},
      {GRCORE_CONFIG_RUNNING, GRCORE_CONTEXT_RUNNING},
      {GRCORE_CONFIG_AT_POLL, GRCORE_CONTEXT_AT_POLL},
      {GRCORE_CONFIG_PAUSED, GRCORE_CONTEXT_PAUSED},
  };
  for (auto & m : map) {
    w.ctx->config = m.config;
    EXPECT_EQ(grcore_context_state(w.ctx), m.state);
  }
}

TEST(Context, ParkAndUnparkBracketAHostCall) {
  World w;
  EXPECT_EQ(grcore_context_park(w.ctx), GRCORE_ERR_INVALID); // not running
  EXPECT_EQ(grcore_context_unpark(w.ctx), GRCORE_ERR_INVALID);
  EXPECT_EQ(w.ctx->config, GRCORE_CONFIG_PARKED_OUTSIDE); // not "run begins"
  ASSERT_EQ(grcore_context_transition(w.ctx, GRCORE_CONFIG_RUNNING), GRCORE_OK);
  ASSERT_EQ(grcore_context_park(w.ctx), GRCORE_OK);
  EXPECT_EQ(grcore_context_state(w.ctx), GRCORE_CONTEXT_PARKED);
  EXPECT_EQ(grcore_context_park(w.ctx), GRCORE_ERR_INVALID);
  ASSERT_EQ(grcore_context_unpark(w.ctx), GRCORE_OK);
  EXPECT_EQ(grcore_context_state(w.ctx), GRCORE_CONTEXT_RUNNING);
}

TEST(Context, ReleaseIsAllowedOnlyWhenParkedOutsideOrPaused) {
  World w;
  const GRCORE_ContextConfig all[] = {GRCORE_CONFIG_PARKED_OUTSIDE,
      GRCORE_CONFIG_PARKED_INSIDE, GRCORE_CONFIG_RUNNING,
      GRCORE_CONFIG_AT_POLL, GRCORE_CONFIG_PAUSED};
  for (auto c : all) {
    w.ctx->config = c;
    bool can = c == GRCORE_CONFIG_PARKED_OUTSIDE || c == GRCORE_CONFIG_PAUSED;
    GRCORE_Result r = grcore_context_release(w.ctx);
    EXPECT_EQ(r, can ? GRCORE_OK : GRCORE_ERR_INVALID) << c;
    EXPECT_EQ(grcore_context_is_owner(w.ctx), !can) << c;
    if (can) {
      EXPECT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
    }
  }
}

TEST(Context, ASecondOwnerIsRefusedUntilTheFirstReleases) {
  World w;
  EXPECT_EQ(grcore_context_acquire(w.ctx), GRCORE_ERR_INVALID); // by the owner
  EXPECT_EQ(on_other_thread([&] { return grcore_context_acquire(w.ctx); }),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(on_other_thread([&] { return grcore_context_release(w.ctx); }),
      GRCORE_ERR_INVALID); // a non-owner cannot release
  EXPECT_TRUE(grcore_context_is_owner(w.ctx));
  ASSERT_EQ(grcore_context_release(w.ctx), GRCORE_OK);
  EXPECT_FALSE(grcore_context_is_owner(w.ctx));
  EXPECT_EQ(grcore_context_release(w.ctx), GRCORE_ERR_INVALID); // already
  EXPECT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
  EXPECT_TRUE(grcore_context_is_owner(w.ctx));
}

TEST(Context, DestroyNeedsTheOwnerAndAParkedOutsideOrPausedContext) {
  World w;
  EXPECT_EQ(on_other_thread([&] { return grcore_context_destroy(w.ctx); }),
      GRCORE_ERR_INVALID);
  for (auto c : {GRCORE_CONFIG_PARKED_INSIDE, GRCORE_CONFIG_RUNNING,
           GRCORE_CONFIG_AT_POLL}) {
    w.ctx->config = c;
    EXPECT_EQ(grcore_context_destroy(w.ctx), GRCORE_ERR_INVALID) << c;
  }
  EXPECT_EQ(grcore_group_context_count(w.group), 1u);
  // An abandoned paused context must not pin its group.
  w.ctx->config = GRCORE_CONFIG_PAUSED;
  GRCORE_Context * c = w.ctx;
  w.ctx = nullptr;
  EXPECT_EQ(grcore_context_destroy(c), GRCORE_OK);
  EXPECT_EQ(grcore_group_context_count(w.group), 0u);
}

TEST(Context, UnparkFromANonOwnerIsRefused) {
  World w;
  ASSERT_EQ(grcore_context_transition(w.ctx, GRCORE_CONFIG_RUNNING), GRCORE_OK);
  ASSERT_EQ(grcore_context_park(w.ctx), GRCORE_OK);
  EXPECT_EQ(on_other_thread([&] { return grcore_context_unpark(w.ctx); }),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(w.ctx->config, GRCORE_CONFIG_PARKED_INSIDE);
  ASSERT_EQ(grcore_context_unpark(w.ctx), GRCORE_OK);
  ASSERT_EQ(grcore_context_transition(w.ctx, GRCORE_CONFIG_PARKED_OUTSIDE),
      GRCORE_OK);
}

TEST(Context, ThreadIdsAreNonzeroDistinctAndNeverReused) {
  uintptr_t mine = grcore_thread_id();
  EXPECT_NE(mine, 0u);
  EXPECT_EQ(grcore_thread_id(), mine);
  std::set<uintptr_t> seen{mine};
  for (int i = 0; i < 50; i++) {
    uintptr_t id = 0;
    std::thread t([&] { id = grcore_thread_id(); });
    t.join();
    EXPECT_NE(id, 0u);
    EXPECT_TRUE(seen.insert(id).second); // sequential threads, no reuse
  }
}

namespace {
struct TearingDown {
  GRCORE_Context * ctx;
  std::vector<GRCORE_Result> results;
  void * slot_seen = reinterpret_cast<void *>(1);
  size_t count_seen = 99;
};
TearingDown * g_td = nullptr;
const GRCORE_Key kProbe = {"probe", GRCORE_CARDINALITY_MANY,
    GRCORE_PHASE_NONE, [](GRCORE_Context * c, void * value) {
      static char extra[] = "x";
      TearingDown * td = g_td;
      td->results.push_back(grcore_context_register(c, &kMany, extra));
      td->results.push_back(grcore_context_release(c));
      td->results.push_back(grcore_context_acquire(c));
      td->results.push_back(grcore_context_destroy(c));
      td->results.push_back(
          grcore_context_transition(c, GRCORE_CONFIG_RUNNING));
      // This registration is already out of the table; so is every later one.
      td->slot_seen = grcore_context_slot(c, &kOne);
      td->count_seen = grcore_context_registration_count(c);
      (void)value;
    }};
} // namespace

TEST(Context, ADestructorCannotChangeTheContextOrSeeDestroyedValues) {
  World w;
  static char first[] = "first", later[] = "later";
  ASSERT_EQ(grcore_context_register(w.ctx, &kOne, first), GRCORE_OK);
  ASSERT_EQ(grcore_context_register(w.ctx, &kProbe, later), GRCORE_OK);
  TearingDown td{w.ctx, {}};
  g_td = &td;
  GRCORE_Context * c = w.ctx;
  w.ctx = nullptr;
  ASSERT_EQ(grcore_context_destroy(c), GRCORE_OK);
  g_td = nullptr;
  ASSERT_EQ(td.results.size(), 5u);
  for (auto r : td.results) {
    EXPECT_EQ(r, GRCORE_ERR_INVALID);
  }
  // kOne was registered before the probe, so it is still live and visible;
  // the probe itself is no longer counted.
  EXPECT_EQ(td.slot_seen, first);
  EXPECT_EQ(td.count_seen, 1u);
  EXPECT_EQ(grcore_group_context_count(w.group), 0u);
}

TEST(Context, GuestStateIsReadableOnlyAtPollOrPausedByTheOwner) {
  World w;
  const GRCORE_ContextConfig all[] = {GRCORE_CONFIG_PARKED_OUTSIDE,
      GRCORE_CONFIG_PARKED_INSIDE, GRCORE_CONFIG_RUNNING,
      GRCORE_CONFIG_AT_POLL, GRCORE_CONFIG_PAUSED};
  for (auto c : all) {
    w.ctx->config = c;
    bool want = c == GRCORE_CONFIG_AT_POLL || c == GRCORE_CONFIG_PAUSED;
    EXPECT_EQ(grcore_context_guest_state_readable(w.ctx), want) << c;
    bool other = true;
    std::thread t([&] { other = grcore_context_guest_state_readable(w.ctx); });
    t.join();
    EXPECT_FALSE(other) << c;
  }
  w.ctx->config = GRCORE_CONFIG_PAUSED;
  ASSERT_EQ(grcore_context_release(w.ctx), GRCORE_OK);
  EXPECT_FALSE(grcore_context_guest_state_readable(w.ctx)); // unowned
  ASSERT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
}

// The hand-off uses only the library's own release and acquire. The plain
// variable below is written by one owner and read by the next, so under
// ThreadSanitizer this test fails if those two calls do not order memory.
TEST(Context, OwnershipHandOffIsRaceFree) {
  World w;
  int plain = 0;
  std::atomic<bool> worker_has_it{false};
  ASSERT_EQ(grcore_context_release(w.ctx), GRCORE_OK);
  std::thread t([&] {
    while (grcore_context_acquire(w.ctx) != GRCORE_OK) {
      std::this_thread::yield();
    }
    worker_has_it.store(true);
    plain = 41;
    static char v[] = "v";
    EXPECT_EQ(grcore_context_register(w.ctx, &kMany, v), GRCORE_OK);
    plain++;
    EXPECT_EQ(grcore_context_release(w.ctx), GRCORE_OK);
  });
  while (!worker_has_it.load()) {
    std::this_thread::yield();
  }
  while (grcore_context_acquire(w.ctx) != GRCORE_OK) {
    std::this_thread::yield();
  }
  EXPECT_EQ(plain, 42);
  EXPECT_NE(grcore_context_slot(w.ctx, &kMany), nullptr);
  t.join();
  EXPECT_TRUE(grcore_context_is_owner(w.ctx));
}

TEST(Context, ManyContextsInOneGroupFromManyThreads) {
  GRCORE_Group * g;
  ASSERT_EQ(grcore_group_create(nullptr, nullptr, &g), GRCORE_OK);
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; i++) {
    threads.emplace_back([g] {
      for (int j = 0; j < 200; j++) {
        GRCORE_Context * c;
        ASSERT_EQ(grcore_context_create(g, nullptr, &c), GRCORE_OK);
        const GRCORE_Allocator * a = grcore_context_allocator(c);
        a->free_fn(a->ctx, a->malloc_fn(a->ctx, 8));
        ASSERT_EQ(grcore_context_destroy(c), GRCORE_OK);
      }
    });
  }
  for (auto & t : threads) {
    t.join();
  }
  EXPECT_EQ(grcore_group_context_count(g), 0u);
  EXPECT_EQ(grcore_group_destroy(g), GRCORE_OK);
}

TEST(Context, EveryAllocationFailureOfCreateIsOomAndLeaksNothing) {
  GRCORE_Options * o;
  ASSERT_EQ(grcore_options_create(nullptr, &o), GRCORE_OK);
  ASSERT_EQ(grcore_options_set_keyed(o, &kOne, "xyz", 3), GRCORE_OK);
  ASSERT_EQ(grcore_options_set_keyed(o, &kMany, "uvw", 3), GRCORE_OK);
  bool succeeded = false;
  int failures = 0;
  for (long n = 1; n < 100 && !succeeded; n++) {
    TrackingAllocator t;
    GRCORE_Group * g;
    ASSERT_EQ(grcore_group_create(t.get(), nullptr, &g), GRCORE_OK);
    long after_group = t.calls;
    t.fail_at = after_group + n;
    GRCORE_Context * c = reinterpret_cast<GRCORE_Context *>(1);
    GRCORE_Result r = grcore_context_create(g, o, &c);
    if (r == GRCORE_OK) {
      succeeded = true;
      EXPECT_EQ(grcore_group_context_count(g), 1u);
      EXPECT_EQ(grcore_context_destroy(c), GRCORE_OK);
    } else {
      failures++;
      EXPECT_EQ(r, GRCORE_ERR_OOM) << "n=" << n;
      EXPECT_EQ(c, reinterpret_cast<GRCORE_Context *>(1)) << "n=" << n;
    }
    EXPECT_EQ(grcore_group_context_count(g), 0u) << "n=" << n;
    EXPECT_EQ(grcore_group_destroy(g), GRCORE_OK);
    EXPECT_EQ(t.live, 0) << "n=" << n;
  }
  EXPECT_TRUE(succeeded);
  EXPECT_GE(failures, 3); // the struct, the options, and two keyed entries
  grcore_options_destroy(o);
}

TEST(Context, AContextUsesTheGroupsPageProviderAndAllocator) {
  TrackingAllocator t;
  FakePages pages;
  GRCORE_Group * g;
  ASSERT_EQ(grcore_group_create(t.get(), &pages.vtable, &g), GRCORE_OK);
  GRCORE_Context * c;
  ASSERT_EQ(grcore_context_create(g, nullptr, &c), GRCORE_OK);
  const GRCORE_PageProvider * p = grcore_context_page_provider(c);
  void * m = p->map(p->ctx, p->page_size);
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(pages.live, 1);
  p->unmap(p->ctx, m, p->page_size);
  EXPECT_EQ(pages.live, 0);
  const GRCORE_Allocator * a = grcore_context_allocator(c);
  void * b = a->malloc_fn(a->ctx, 5);
  long before = t.live;
  a->free_fn(a->ctx, b);
  EXPECT_EQ(t.live, before - 1);
  EXPECT_EQ(grcore_context_destroy(c), GRCORE_OK);
  EXPECT_EQ(grcore_group_destroy(g), GRCORE_OK);
  EXPECT_EQ(t.live, 0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

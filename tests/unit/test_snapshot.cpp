/**
 * @file
 *
 * Context snapshots (b/snapshot.h): the object, the walk over the keys that
 * make one, the restore, and every refusal.
 *
 * The keys under test are toys, so that what each hook was asked to do is
 * recorded and an assertion can say it: a snapshot is only as good as the
 * order and the atomicity of the calls the keys see.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

/* What a toy key holds, and what was done to it. */
struct Toy {
  std::string name;
  std::vector<uint64_t> values;
  std::string tag;
  void * scratch = nullptr;           // allocated by APPLY, through the context
  size_t scratch_bytes = 16;
  GRCORE_Context * context = nullptr;
  int check = 0, apply = 0, prepare = 0, commit = 0, abandon = 0;
  GRCORE_Result fail_snapshot = GRCORE_OK;
  GRCORE_Result fail_check = GRCORE_OK;
  GRCORE_Result fail_apply = GRCORE_OK;
  GRCORE_Result fail_prepare = GRCORE_OK;
  const void * seen_env = nullptr;
  std::vector<std::string> * log = nullptr;
};

void note(Toy * t, const char * what) {
  if (t->log != nullptr) {
    t->log->push_back(std::string(what) + ":" + t->name);
  }
}

GRCORE_Result toy_snapshot(
    GRCORE_Context *, void * value, GRCORE_SnapshotWriter * w) {
  auto * t = static_cast<Toy *>(value);
  note(t, "snapshot");
  if (t->fail_snapshot != GRCORE_OK) {
    return t->fail_snapshot;
  }
  GRCORE_Result r = grcore_snapshot_writer_u64(w, t->values.size());
  for (size_t i = 0; r == GRCORE_OK && i < t->values.size(); i++) {
    r = grcore_snapshot_writer_u64(w, t->values[i]);
  }
  if (r == GRCORE_OK) {
    r = grcore_snapshot_writer_string(w, t->tag.c_str());
  }
  return r;
}

void toy_release_scratch(Toy * t) {
  if (t->scratch != nullptr) {
    const GRCORE_Allocator * a = grcore_context_allocator(t->context);
    a->free_fn(a->ctx, t->scratch);
    t->scratch = nullptr;
  }
}

GRCORE_Result toy_restore(GRCORE_Context * c, void * value,
    GRCORE_SnapshotReader * r, void * env, GRCORE_RestoreMode mode) {
  auto * t = static_cast<Toy *>(value);
  t->seen_env = env;
  uint64_t n = 0;
  GRCORE_Result res = grcore_snapshot_reader_u64(r, &n);
  std::vector<uint64_t> values;
  for (uint64_t i = 0; res == GRCORE_OK && i < n; i++) {
    uint64_t v = 0;
    res = grcore_snapshot_reader_u64(r, &v);
    values.push_back(v);
  }
  const char * tag = nullptr;
  if (res == GRCORE_OK) {
    res = grcore_snapshot_reader_string(r, &tag, nullptr);
  }
  if (res != GRCORE_OK) {
    return res;
  }
  if (mode == GRCORE_RESTORE_CHECK) {
    t->check++;
    note(t, "check");
    return t->fail_check;
  }
  note(t, "apply");
  t->apply++;
  if (t->fail_apply != GRCORE_OK) {
    return t->fail_apply;
  }
  const GRCORE_Allocator * a = grcore_context_allocator(c);
  void * scratch = a->malloc_fn(a->ctx, t->scratch_bytes);
  if (scratch == nullptr) {
    return grcore_context_memory_refusals(c) > 0 ? GRCORE_ERR_LIMIT
                                                 : GRCORE_ERR_OOM;
  }
  t->scratch = scratch;
  t->values = values;
  t->tag = tag;
  return GRCORE_OK;
}

GRCORE_Result toy_settle(
    GRCORE_Context *, void * value, void * env, GRCORE_SettleMode mode) {
  auto * t = static_cast<Toy *>(value);
  t->seen_env = env;
  switch (mode) {
    case GRCORE_SETTLE_PREPARE:
      t->prepare++;
      note(t, "prepare");
      return t->fail_prepare;
    case GRCORE_SETTLE_COMMIT:
      t->commit++;
      note(t, "commit");
      return GRCORE_OK;
    case GRCORE_SETTLE_ABANDON:
      t->abandon++;
      note(t, "abandon");
      toy_release_scratch(t);
      t->values.clear();
      t->tag.clear();
      return GRCORE_OK;
  }
  return GRCORE_OK;
}

void toy_destroy(GRCORE_Context *, void * value) {
  toy_release_scratch(static_cast<Toy *>(value));
}

#define TOY_KEY(var, nm)                                                    \
  const GRCORE_Key var = GRCORE_KEY_INIT(nm, GRCORE_CARDINALITY_ONE, GRCORE_PHASE_NONE,    \
      toy_destroy, nullptr, toy_snapshot, toy_restore, toy_settle)
TOY_KEY(kToyA, "toy-a");
TOY_KEY(kToyB, "toy-b");
TOY_KEY(kToyTwinOfA, "toy-a");

/* A key that holds a value and takes no part in snapshots. */
const GRCORE_Key kPlain = GRCORE_KEY_INIT("plain", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_NONE,
    nullptr, nullptr, nullptr, nullptr, nullptr);

/* A world with two toys registered, in the order a test gives. */
struct ToyWorld {
  /* The toys come first so that they are destroyed last: the context's
   * teardown runs their key's destructor. */
  Toy a, b;
  RunWorld w;
  explicit ToyWorld(bool a_first = true, const GRCORE_Allocator * alloc = nullptr)
      : w(GRCORE_UNLIMITED, GRCORE_UNLIMITED, GRCORE_DEFAULT_MEMORY_RESERVE,
            GRCORE_UNLIMITED, GRCORE_UNLIMITED, alloc) {
    a.name = "a";
    b.name = "b";
    a.context = b.context = w.ctx;
    if (a_first) {
      EXPECT_EQ(grcore_context_register(w.ctx, &kToyA, &a), GRCORE_OK);
      EXPECT_EQ(grcore_context_register(w.ctx, &kToyB, &b), GRCORE_OK);
    } else {
      EXPECT_EQ(grcore_context_register(w.ctx, &kToyB, &b), GRCORE_OK);
      EXPECT_EQ(grcore_context_register(w.ctx, &kToyA, &a), GRCORE_OK);
    }
  }
  void fill() {
    a.values = {1, 2, 3};
    a.tag = "alpha";
    b.values = {42};
    b.tag = "";
  }
};

struct Taken {
  GRCORE_Snapshot * s = nullptr;
  ~Taken() { grcore_snapshot_release(s); }
};

} // namespace

TEST(Snapshot, EveryHookedKeyWritesOneBlobInRegistrationOrder) {
  ToyWorld src;
  src.fill();
  Taken t;
  ASSERT_EQ(grcore_context_snapshot(src.w.ctx, nullptr, &t.s), GRCORE_OK);
  ASSERT_EQ(grcore_snapshot_blob_count(t.s), 2u);
  EXPECT_STREQ(grcore_snapshot_blob_name(t.s, 0), "toy-a");
  EXPECT_STREQ(grcore_snapshot_blob_name(t.s, 1), "toy-b");
  EXPECT_EQ(grcore_snapshot_blob_name(t.s, 2), nullptr);
  EXPECT_FALSE(grcore_snapshot_was_paused(t.s));
  EXPECT_EQ(grcore_snapshot_refcount(t.s), 1u);
  const void * data = nullptr;
  size_t size = 0;
  ASSERT_EQ(grcore_snapshot_blob(t.s, "toy-b", &data, &size), GRCORE_OK);
  EXPECT_EQ(size, 8u + 8u + 8u + 1u); // count, one value, string length, NUL
  EXPECT_EQ(grcore_snapshot_blob(t.s, "toy-c", &data, &size), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_snapshot_size(t.s),
      (8u + 3 * 8u + 8u + 6u) + (8u + 8u + 8u + 1u));
}

TEST(Snapshot, AKeyWithNoHooksIsNotPartOfTheSnapshotAndTheHostRegistersItAgain) {
  ToyWorld src;
  int plain = 5;
  ASSERT_EQ(grcore_context_register(src.w.ctx, &kPlain, &plain), GRCORE_OK);
  src.fill();
  Taken t;
  ASSERT_EQ(grcore_context_snapshot(src.w.ctx, nullptr, &t.s), GRCORE_OK);
  EXPECT_EQ(grcore_snapshot_blob_count(t.s), 2u);
  // The destination has no plain key and needs none: it is not a blob.
  ToyWorld dst;
  EXPECT_EQ(grcore_context_restore(dst.w.ctx, t.s, nullptr), GRCORE_OK);
  EXPECT_EQ(dst.a.values, (std::vector<uint64_t>{1, 2, 3}));
}

TEST(Snapshot, RestoreRebuildsEachKeyAndRunsTheCallsInTheDocumentedOrder) {
  ToyWorld src;
  src.fill();
  Taken t;
  ASSERT_EQ(grcore_context_snapshot(src.w.ctx, nullptr, &t.s), GRCORE_OK);
  // The destination registers in the other order: APPLY follows the
  // snapshot's, SETTLE the destination's.
  std::vector<std::string> log;
  ToyWorld dst(false);
  dst.a.log = dst.b.log = &log;
  ASSERT_EQ(grcore_context_restore(dst.w.ctx, t.s, nullptr), GRCORE_OK);
  EXPECT_EQ(log, (std::vector<std::string>{"check:a", "check:b", "apply:a",
                     "apply:b", "prepare:b", "prepare:a", "commit:b",
                     "commit:a"}));
  EXPECT_EQ(dst.a.values, (std::vector<uint64_t>{1, 2, 3}));
  EXPECT_EQ(dst.a.tag, "alpha");
  EXPECT_EQ(dst.b.values, (std::vector<uint64_t>{42}));
  EXPECT_EQ(dst.a.abandon + dst.b.abandon, 0);
}

TEST(Snapshot, ASnapshotMayBeRestoredTwiceAndOutlivesTheContextItCameFrom) {
  Taken t;
  {
    ToyWorld src;
    src.fill();
    ASSERT_EQ(grcore_context_snapshot(src.w.ctx, nullptr, &t.s), GRCORE_OK);
  }
  ToyWorld one, two;
  EXPECT_EQ(grcore_context_restore(one.w.ctx, t.s, nullptr), GRCORE_OK);
  EXPECT_EQ(grcore_context_restore(two.w.ctx, t.s, nullptr), GRCORE_OK);
  EXPECT_EQ(one.a.values, two.a.values);
  EXPECT_EQ(one.b.values, two.b.values);
  // Releasing early does not touch what was restored.
  grcore_snapshot_release(t.s);
  t.s = nullptr;
  EXPECT_EQ(one.a.tag, "alpha");
}

TEST(Snapshot, RetainAndReleaseBalanceAndNullIsANoOp) {
  ToyWorld src;
  Taken t;
  ASSERT_EQ(grcore_context_snapshot(src.w.ctx, nullptr, &t.s), GRCORE_OK);
  EXPECT_EQ(grcore_snapshot_retain(t.s), t.s);
  EXPECT_EQ(grcore_snapshot_refcount(t.s), 2u);
  grcore_snapshot_release(t.s);
  EXPECT_EQ(grcore_snapshot_refcount(t.s), 1u);
  EXPECT_EQ(grcore_snapshot_retain(nullptr), nullptr);
  grcore_snapshot_release(nullptr);
  EXPECT_EQ(grcore_snapshot_refcount(nullptr), 0u);
  EXPECT_EQ(grcore_snapshot_size(nullptr), 0u);
  EXPECT_EQ(grcore_snapshot_blob_count(nullptr), 0u);
  EXPECT_EQ(grcore_snapshot_blob_name(nullptr, 0), nullptr);
  EXPECT_FALSE(grcore_snapshot_was_paused(nullptr));
}

TEST(Snapshot, TheEnvironmentLookupIsAskedByKeyNameAndItsAnswerReachesBothHooks) {
  ToyWorld src;
  Taken t;
  ASSERT_EQ(grcore_context_snapshot(src.w.ctx, nullptr, &t.s), GRCORE_OK);
  ToyWorld dst;
  static int env_a = 1, env_b = 2;
  GRCORE_RestoreEnv env = GRCORE_RESTORE_ENV_INIT(nullptr, nullptr, nullptr, nullptr, nullptr);
  env.lookup = [](void *, const char * name) -> void * {
    return std::strcmp(name, "toy-a") == 0 ? &env_a
        : std::strcmp(name, "toy-b") == 0  ? &env_b
                                           : nullptr;
  };
  ASSERT_EQ(grcore_context_restore(dst.w.ctx, t.s, &env), GRCORE_OK);
  EXPECT_EQ(dst.a.seen_env, &env_a);
  EXPECT_EQ(dst.b.seen_env, &env_b);
}

/* ---- Refusing to take ---------------------------------------------------- */

TEST(Snapshot, TakeIsRefusedWhileRunningAndAtPollAndChangesNothing) {
  Toy a;
  Probe probe("p");
  RunWorld w;
  a.name = "a";
  a.context = w.ctx;
  ASSERT_EQ(grcore_context_register(w.ctx, &kToyA, &a), GRCORE_OK);
  GRCORE_Result while_running = GRCORE_OK, at_poll = GRCORE_OK;
  GRCORE_Snapshot * sentinel = reinterpret_cast<GRCORE_Snapshot *>(0x1234);
  GRCORE_Snapshot * out_running = sentinel;
  GRCORE_Snapshot * out_poll = sentinel;
  probe.extra = [&](GRCORE_Context * c, GRCORE_PollCall *) {
    at_poll = grcore_context_snapshot(c, nullptr, &out_poll);
  };
  ASSERT_EQ(grcore_context_register(w.ctx, &kObserveKey, &probe), GRCORE_OK);
  Fn fn;
  fn.body = [&](GRCORE_Context * c) {
    while_running = grcore_context_snapshot(c, nullptr, &out_running);
    grcore_context_charge_fuel(c, 1);
    GRCORE_Verdict v = grcore_poll(c, GRCORE_Location{__FILE__, __LINE__});
    return v == GRCORE_VERDICT_PAUSE ? GRCORE_STEP_PAUSED : GRCORE_STEP_FINISHED;
  };
  GRCORE_Outcome outcome;
  // The observe handler only runs on a slow poll: make one pending.
  ASSERT_EQ(grcore_context_set_fuel(w.ctx, 0), GRCORE_OK);
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_EQ(while_running, GRCORE_ERR_INVALID);
  EXPECT_EQ(out_running, sentinel);
  EXPECT_EQ(at_poll, GRCORE_ERR_INVALID);
  EXPECT_EQ(out_poll, sentinel);
  EXPECT_EQ(a.apply + a.check, 0);
}

TEST(Snapshot, TakeIsRefusedWithAFuelScopeOpenAndWorksAfterItCloses) {
  ToyWorld w;
  uint64_t id = 0;
  ASSERT_EQ(grcore_context_fuel_scope_open(
                w.w.ctx, 100, GRCORE_SCOPE_POLICY_UNWIND, &id),
      GRCORE_OK);
  GRCORE_Snapshot * out = nullptr;
  EXPECT_EQ(grcore_context_snapshot(w.w.ctx, nullptr, &out), GRCORE_ERR_INVALID);
  EXPECT_EQ(out, nullptr);
  ASSERT_EQ(grcore_context_fuel_scope_close(w.w.ctx, id), GRCORE_OK);
  EXPECT_EQ(grcore_context_snapshot(w.w.ctx, nullptr, &out), GRCORE_OK);
  grcore_snapshot_release(out);
}

TEST(Snapshot, TakeIsRefusedFromAnotherThread) {
  ToyWorld w;
  GRCORE_Result r = GRCORE_OK;
  std::thread([&] {
    GRCORE_Snapshot * out = nullptr;
    r = grcore_context_snapshot(w.w.ctx, nullptr, &out);
  }).join();
  EXPECT_EQ(r, GRCORE_ERR_INVALID);
}

TEST(Snapshot, NullArgumentsAreRefused) {
  ToyWorld w;
  GRCORE_Snapshot * out = nullptr;
  EXPECT_EQ(grcore_context_snapshot(nullptr, nullptr, &out), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_snapshot(w.w.ctx, nullptr, nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_restore(nullptr, nullptr, nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_restore(w.w.ctx, nullptr, nullptr), GRCORE_ERR_INVALID);
}

TEST(Snapshot, AKeyThatRefusesRefusesTheWholeSnapshotAndLeavesNothingAllocated) {
  for (GRCORE_Result why : {GRCORE_ERR_INVALID, GRCORE_ERR_OOM, GRCORE_ERR_LIMIT}) {
    TrackingAllocator mem;
    ToyWorld w;
    w.fill();
    w.b.fail_snapshot = why;
    GRCORE_Snapshot * out = nullptr;
    EXPECT_EQ(grcore_context_snapshot(w.w.ctx, mem.get(), &out), why);
    EXPECT_EQ(out, nullptr);
    EXPECT_EQ(mem.live, 0) << "a refused snapshot must free what it made";
  }
}

TEST(Snapshot, IncompleteHooksNullNamesManyCardinalityAndDuplicateNamesAreRefused) {
  static const GRCORE_Key only_snapshot = GRCORE_KEY_INIT("half", GRCORE_CARDINALITY_ONE,
      GRCORE_PHASE_NONE, nullptr, nullptr, toy_snapshot, nullptr, nullptr);
  static const GRCORE_Key no_settle = GRCORE_KEY_INIT("nosettle", GRCORE_CARDINALITY_ONE,
      GRCORE_PHASE_NONE, nullptr, nullptr, toy_snapshot, toy_restore, nullptr);
  static const GRCORE_Key no_name = GRCORE_KEY_INIT(nullptr, GRCORE_CARDINALITY_ONE,
      GRCORE_PHASE_NONE, nullptr, nullptr, toy_snapshot, toy_restore, toy_settle);
  static const GRCORE_Key many = GRCORE_KEY_INIT("many", GRCORE_CARDINALITY_MANY,
      GRCORE_PHASE_NONE, nullptr, nullptr, toy_snapshot, toy_restore, toy_settle);
  for (const GRCORE_Key * k : {&only_snapshot, &no_settle, &no_name, &many}) {
    Toy t;
    RunWorld w;
    t.context = w.ctx;
    ASSERT_EQ(grcore_context_register(w.ctx, k, &t), GRCORE_OK);
    GRCORE_Snapshot * out = nullptr;
    EXPECT_EQ(grcore_context_snapshot(w.ctx, nullptr, &out), GRCORE_ERR_INVALID)
        << (k->name != nullptr ? k->name : "(no name)");
    EXPECT_EQ(out, nullptr);
  }
  Toy a, twin;
  RunWorld w;
  a.context = twin.context = w.ctx;
  ASSERT_EQ(grcore_context_register(w.ctx, &kToyA, &a), GRCORE_OK);
  ASSERT_EQ(grcore_context_register(w.ctx, &kToyTwinOfA, &twin), GRCORE_OK);
  GRCORE_Snapshot * out = nullptr;
  EXPECT_EQ(grcore_context_snapshot(w.ctx, nullptr, &out), GRCORE_ERR_INVALID)
      << "two blobs of one name";
}

/* ---- Refusing to restore, and atomicity ------------------------------------ */

TEST(Snapshot, ABlobWithNoKeyAndAKeyWithNoBlobAreBothRefusedBeforeAnyChange) {
  ToyWorld src;
  src.fill();
  Taken t;
  ASSERT_EQ(grcore_context_snapshot(src.w.ctx, nullptr, &t.s), GRCORE_OK);
  {
    RunWorld w; // no keys at all
    EXPECT_EQ(grcore_context_restore(w.ctx, t.s, nullptr), GRCORE_ERR_INVALID);
  }
  {
    Toy a;
    RunWorld w; // only one of the two
    a.context = w.ctx;
    ASSERT_EQ(grcore_context_register(w.ctx, &kToyA, &a), GRCORE_OK);
    EXPECT_EQ(grcore_context_restore(w.ctx, t.s, nullptr), GRCORE_ERR_INVALID);
    EXPECT_EQ(a.check + a.apply, 0);
  }
  {
    // A destination with an extra hooked key: it would be left fresh in a
    // context that is otherwise not.
    Toy e;
    ToyWorld w;
    static const GRCORE_Key extra = GRCORE_KEY_INIT("toy-extra", GRCORE_CARDINALITY_ONE,
        GRCORE_PHASE_NONE, nullptr, nullptr, toy_snapshot, toy_restore, toy_settle);
    e.context = w.w.ctx;
    ASSERT_EQ(grcore_context_register(w.w.ctx, &extra, &e), GRCORE_OK);
    EXPECT_EQ(grcore_context_restore(w.w.ctx, t.s, nullptr), GRCORE_ERR_INVALID);
    EXPECT_EQ(w.a.check + w.a.apply + w.b.check + w.b.apply, 0);
  }
}

TEST(Snapshot, ACheckThatRefusesStopsBeforeAnyKeyIsApplied) {
  ToyWorld src;
  src.fill();
  Taken t;
  ASSERT_EQ(grcore_context_snapshot(src.w.ctx, nullptr, &t.s), GRCORE_OK);
  ToyWorld dst;
  dst.b.fail_check = GRCORE_ERR_INVALID;
  EXPECT_EQ(grcore_context_restore(dst.w.ctx, t.s, nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(dst.a.check, 1);
  EXPECT_EQ(dst.a.apply + dst.b.apply, 0) << "CHECK is the pass that changes nothing";
  EXPECT_TRUE(dst.a.values.empty());
}

TEST(Snapshot, AnApplyThatFailsAbandonsTheKeysAppliedBeforeItNewestFirst) {
  ToyWorld src;
  src.fill();
  Taken t;
  ASSERT_EQ(grcore_context_snapshot(src.w.ctx, nullptr, &t.s), GRCORE_OK);
  for (GRCORE_Result why : {GRCORE_ERR_LIMIT, GRCORE_ERR_OOM}) {
    std::vector<std::string> log;
    ToyWorld dst;
    dst.a.log = dst.b.log = &log;
    dst.b.fail_apply = why;
    EXPECT_EQ(grcore_context_restore(dst.w.ctx, t.s, nullptr), why);
    EXPECT_EQ(log, (std::vector<std::string>{"check:a", "check:b", "apply:a",
                       "apply:b", "abandon:a"}));
    EXPECT_TRUE(dst.a.values.empty());
    EXPECT_EQ(dst.a.scratch, nullptr);
    EXPECT_EQ(dst.a.commit + dst.b.commit, 0);
    // The destination is still a fresh context: a restore now succeeds.
    dst.b.fail_apply = GRCORE_OK;
    EXPECT_EQ(grcore_context_restore(dst.w.ctx, t.s, nullptr), GRCORE_OK);
    EXPECT_EQ(dst.a.values, (std::vector<uint64_t>{1, 2, 3}));
  }
}

TEST(Snapshot, APrepareThatFailsAbandonsEveryAppliedKeyAndCommitsNone) {
  ToyWorld src;
  src.fill();
  Taken t;
  ASSERT_EQ(grcore_context_snapshot(src.w.ctx, nullptr, &t.s), GRCORE_OK);
  std::vector<std::string> log;
  ToyWorld dst;
  dst.a.log = dst.b.log = &log;
  dst.b.fail_prepare = GRCORE_ERR_INVALID;
  EXPECT_EQ(grcore_context_restore(dst.w.ctx, t.s, nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(log, (std::vector<std::string>{"check:a", "check:b", "apply:a",
                     "apply:b", "prepare:a", "prepare:b", "abandon:b",
                     "abandon:a"}));
  EXPECT_EQ(dst.a.commit + dst.b.commit, 0);
  EXPECT_EQ(dst.a.scratch, nullptr);
  EXPECT_EQ(dst.b.scratch, nullptr);
}

TEST(Snapshot, RestoreIsRefusedIntoAContextThatIsNotParkedOutsideRunOrIsNotOurs) {
  ToyWorld src;
  src.fill();
  Taken t;
  ASSERT_EQ(grcore_context_snapshot(src.w.ctx, nullptr, &t.s), GRCORE_OK);
  ToyWorld dst;
  GRCORE_Result r = GRCORE_OK;
  std::thread([&] { r = grcore_context_restore(dst.w.ctx, t.s, nullptr); }).join();
  EXPECT_EQ(r, GRCORE_ERR_INVALID);
  EXPECT_EQ(dst.a.check, 0);
  // Open scope: not fresh.
  uint64_t id = 0;
  ASSERT_EQ(grcore_context_fuel_scope_open(
                dst.w.ctx, 100, GRCORE_SCOPE_POLICY_UNWIND, &id),
      GRCORE_OK);
  EXPECT_EQ(grcore_context_restore(dst.w.ctx, t.s, nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(dst.a.check, 0);
}

/* ---- Paused contexts ------------------------------------------------------ */

namespace {

/* A guest whose position lives in a toy-like key, so a snapshot carries it. */
struct GuestHolder {
  CountingGuest g;
};
GRCORE_Result guest_snapshot(
    GRCORE_Context *, void * value, GRCORE_SnapshotWriter * w) {
  auto * h = static_cast<GuestHolder *>(value);
  GRCORE_Result r = grcore_snapshot_writer_u64(w, h->g.n);
  if (r == GRCORE_OK) r = grcore_snapshot_writer_u64(w, h->g.pos);
  if (r == GRCORE_OK) r = grcore_snapshot_writer_u64(w, h->g.sum);
  return r;
}
GRCORE_Result guest_restore(GRCORE_Context *, void * value,
    GRCORE_SnapshotReader * r, void *, GRCORE_RestoreMode mode) {
  auto * h = static_cast<GuestHolder *>(value);
  uint64_t n, pos, sum;
  GRCORE_Result res = grcore_snapshot_reader_u64(r, &n);
  if (res == GRCORE_OK) res = grcore_snapshot_reader_u64(r, &pos);
  if (res == GRCORE_OK) res = grcore_snapshot_reader_u64(r, &sum);
  if (res == GRCORE_OK && mode == GRCORE_RESTORE_APPLY) {
    h->g.n = n;
    h->g.pos = pos;
    h->g.sum = sum;
  }
  return res;
}
GRCORE_Result guest_settle(
    GRCORE_Context *, void * value, void *, GRCORE_SettleMode mode) {
  if (mode == GRCORE_SETTLE_ABANDON) {
    static_cast<GuestHolder *>(value)->g = CountingGuest();
  }
  return GRCORE_OK;
}
const GRCORE_Key kGuestKey = GRCORE_KEY_INIT("counting-guest", GRCORE_CARDINALITY_ONE,
    GRCORE_PHASE_NONE, nullptr, nullptr, guest_snapshot, guest_restore,
    guest_settle);

} // namespace

TEST(Snapshot, APausedContextRestoresPausedAndResumesToTheUninterruptedAnswer) {
  GuestHolder src_guest;
  src_guest.g.n = 200;
  RunWorld src(50);
  ASSERT_EQ(grcore_context_register(src.ctx, &kGuestKey, &src_guest), GRCORE_OK);
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(src.ctx, counting_entry, &src_guest.g, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  ASSERT_GT(src_guest.g.pos, 0u);
  ASSERT_LT(src_guest.g.pos, 200u);
  Taken t;
  ASSERT_EQ(grcore_context_snapshot(src.ctx, nullptr, &t.s), GRCORE_OK);
  EXPECT_TRUE(grcore_snapshot_was_paused(t.s));

  GuestHolder dst_guest;
  RunWorld dst; // its own budgets: unlimited fuel here
  ASSERT_EQ(grcore_context_register(dst.ctx, &kGuestKey, &dst_guest), GRCORE_OK);
  GRCORE_RestoreEnv env = GRCORE_RESTORE_ENV_INIT(nullptr, nullptr, nullptr, nullptr, nullptr);
  env.entry = counting_entry;
  env.entry_state = &dst_guest.g;
  env.pause_file = kCountingPollFile;
  ASSERT_EQ(grcore_context_restore(dst.ctx, t.s, &env), GRCORE_OK);
  EXPECT_EQ(grcore_context_state(dst.ctx), GRCORE_CONTEXT_PAUSED);
  EXPECT_EQ(grcore_context_fuel_used(dst.ctx), 0u) << "fuel used starts at zero";
  GRCORE_Location where = grcore_context_pause_location(dst.ctx);
  EXPECT_STREQ(where.file, kCountingPollFile);
  EXPECT_EQ(where.line, grcore_context_pause_location(src.ctx).line);
  ASSERT_EQ(grcore_resume(dst.ctx, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  EXPECT_EQ(dst_guest.g.sum, sum_below(200));
  // The source is untouched and can finish too, to the same answer.
  ASSERT_EQ(grcore_context_set_fuel(src.ctx, GRCORE_UNLIMITED), GRCORE_OK);
  ASSERT_EQ(grcore_resume(src.ctx, &outcome), GRCORE_OK);
  EXPECT_EQ(src_guest.g.sum, dst_guest.g.sum);
}

TEST(Snapshot, APausedSnapshotNeedsAnEntryToResumeWith) {
  GuestHolder g;
  g.g.n = 100;
  RunWorld src(10);
  ASSERT_EQ(grcore_context_register(src.ctx, &kGuestKey, &g), GRCORE_OK);
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(src.ctx, counting_entry, &g.g, &outcome), GRCORE_OK);
  Taken t;
  ASSERT_EQ(grcore_context_snapshot(src.ctx, nullptr, &t.s), GRCORE_OK);
  GuestHolder d;
  RunWorld dst;
  ASSERT_EQ(grcore_context_register(dst.ctx, &kGuestKey, &d), GRCORE_OK);
  EXPECT_EQ(grcore_context_restore(dst.ctx, t.s, nullptr), GRCORE_ERR_INVALID);
  GRCORE_RestoreEnv no_entry = GRCORE_RESTORE_ENV_INIT(nullptr, nullptr, nullptr, nullptr, nullptr);
  EXPECT_EQ(grcore_context_restore(dst.ctx, t.s, &no_entry), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_state(dst.ctx), GRCORE_CONTEXT_PARKED);
  EXPECT_EQ(d.g.pos, 0u);
}

TEST(Snapshot, AParkedContextRestoresParkedAndRunsAsAFreshOneWould) {
  GuestHolder src_guest;
  src_guest.g.n = 60;
  RunWorld src;
  ASSERT_EQ(grcore_context_register(src.ctx, &kGuestKey, &src_guest), GRCORE_OK);
  Taken t;
  ASSERT_EQ(grcore_context_snapshot(src.ctx, nullptr, &t.s), GRCORE_OK);
  EXPECT_FALSE(grcore_snapshot_was_paused(t.s));
  GuestHolder dst_guest;
  RunWorld dst;
  ASSERT_EQ(grcore_context_register(dst.ctx, &kGuestKey, &dst_guest), GRCORE_OK);
  ASSERT_EQ(grcore_context_restore(dst.ctx, t.s, nullptr), GRCORE_OK);
  EXPECT_EQ(grcore_context_state(dst.ctx), GRCORE_CONTEXT_PARKED);
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(dst.ctx, counting_entry, &dst_guest.g, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  EXPECT_EQ(dst_guest.g.sum, sum_below(60));
}

TEST(Snapshot, TheDestinationsBudgetsAreItsOwnNotTheSnapshots) {
  GuestHolder src_guest;
  src_guest.g.n = 100;
  RunWorld src(30);
  ASSERT_EQ(grcore_context_register(src.ctx, &kGuestKey, &src_guest), GRCORE_OK);
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(src.ctx, counting_entry, &src_guest.g, &outcome), GRCORE_OK);
  Taken t;
  ASSERT_EQ(grcore_context_snapshot(src.ctx, nullptr, &t.s), GRCORE_OK);
  GuestHolder dst_guest;
  RunWorld dst(5); // a smaller budget than the source had
  ASSERT_EQ(grcore_context_register(dst.ctx, &kGuestKey, &dst_guest), GRCORE_OK);
  GRCORE_RestoreEnv env = GRCORE_RESTORE_ENV_INIT(nullptr, nullptr, nullptr, nullptr, nullptr);
  env.entry = counting_entry;
  env.entry_state = &dst_guest.g;
  ASSERT_EQ(grcore_context_restore(dst.ctx, t.s, &env), GRCORE_OK);
  EXPECT_EQ(grcore_context_fuel_limit(dst.ctx), 5u);
  ASSERT_EQ(grcore_resume(dst.ctx, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_PAUSED) << "the destination's 5 units ran out";
}

/* ---- The writer and the reader --------------------------------------------- */

TEST(Snapshot, AReaderThatRunsPastTheEndFailsAndStaysFailed) {
  struct Reads {
    static GRCORE_Result restore(GRCORE_Context *, void *,
        GRCORE_SnapshotReader * r, void * env, GRCORE_RestoreMode) {
      auto * out = static_cast<std::vector<GRCORE_Result> *>(env);
      uint64_t v;
      out->push_back(grcore_snapshot_reader_u64(r, &v));
      out->push_back(grcore_snapshot_reader_u64(r, &v)); // past the end
      char b;
      out->push_back(grcore_snapshot_reader_read(r, &b, 1));
      out->push_back(static_cast<GRCORE_Result>(grcore_snapshot_reader_remaining(r)));
      return GRCORE_OK;
    }
    static GRCORE_Result snapshot(
        GRCORE_Context *, void *, GRCORE_SnapshotWriter * w) {
      return grcore_snapshot_writer_u64(w, 7);
    }
    static GRCORE_Result settle(
        GRCORE_Context *, void *, void *, GRCORE_SettleMode) {
      return GRCORE_OK;
    }
  };
  static const GRCORE_Key k = GRCORE_KEY_INIT("reads", GRCORE_CARDINALITY_ONE,
      GRCORE_PHASE_NONE, nullptr, nullptr, Reads::snapshot, Reads::restore,
      Reads::settle);
  int token = 0;
  RunWorld a;
  ASSERT_EQ(grcore_context_register(a.ctx, &k, &token), GRCORE_OK);
  Taken t;
  ASSERT_EQ(grcore_context_snapshot(a.ctx, nullptr, &t.s), GRCORE_OK);
  RunWorld b;
  ASSERT_EQ(grcore_context_register(b.ctx, &k, &token), GRCORE_OK);
  std::vector<GRCORE_Result> seen;
  GRCORE_RestoreEnv env = GRCORE_RESTORE_ENV_INIT(nullptr, nullptr, nullptr, nullptr, nullptr);
  env.user = &seen;
  env.lookup = [](void * u, const char *) -> void * { return u; };
  ASSERT_EQ(grcore_context_restore(b.ctx, t.s, &env), GRCORE_OK);
  // CHECK and APPLY each read the same blob from its start.
  ASSERT_EQ(seen.size(), 8u);
  for (size_t i : {0u, 4u}) {
    EXPECT_EQ(seen[i], GRCORE_OK);
    EXPECT_EQ(seen[i + 1], GRCORE_ERR_CORRUPT);
    EXPECT_EQ(seen[i + 2], GRCORE_ERR_CORRUPT) << "a failed reader stays failed";
    EXPECT_EQ(seen[i + 3], static_cast<GRCORE_Result>(0));
  }
}

TEST(Snapshot, AStringMissingItsTerminatorIsCorruptNotARead) {
  struct Bad {
    static GRCORE_Result snapshot(
        GRCORE_Context *, void *, GRCORE_SnapshotWriter * w) {
      // A length of 3 followed by four bytes none of which is NUL.
      GRCORE_Result r = grcore_snapshot_writer_u64(w, 3);
      return r == GRCORE_OK ? grcore_snapshot_writer_write(w, "abcd", 4) : r;
    }
    static GRCORE_Result restore(GRCORE_Context *, void *,
        GRCORE_SnapshotReader * r, void * env, GRCORE_RestoreMode) {
      const char * text;
      *static_cast<GRCORE_Result *>(env) =
          grcore_snapshot_reader_string(r, &text, nullptr);
      return GRCORE_OK;
    }
    static GRCORE_Result settle(
        GRCORE_Context *, void *, void *, GRCORE_SettleMode) {
      return GRCORE_OK;
    }
  };
  static const GRCORE_Key k = GRCORE_KEY_INIT("bad-string", GRCORE_CARDINALITY_ONE,
      GRCORE_PHASE_NONE, nullptr, nullptr, Bad::snapshot, Bad::restore,
      Bad::settle);
  int token = 0;
  RunWorld a, b;
  ASSERT_EQ(grcore_context_register(a.ctx, &k, &token), GRCORE_OK);
  ASSERT_EQ(grcore_context_register(b.ctx, &k, &token), GRCORE_OK);
  Taken t;
  ASSERT_EQ(grcore_context_snapshot(a.ctx, nullptr, &t.s), GRCORE_OK);
  GRCORE_Result seen = GRCORE_OK;
  GRCORE_RestoreEnv env = GRCORE_RESTORE_ENV_INIT(nullptr, nullptr, nullptr, nullptr, nullptr);
  env.user = &seen;
  env.lookup = [](void * u, const char *) -> void * { return u; };
  ASSERT_EQ(grcore_context_restore(b.ctx, t.s, &env), GRCORE_OK);
  EXPECT_EQ(seen, GRCORE_ERR_CORRUPT);
}

TEST(Snapshot, AWriterRefusesNullDataWithBytesAndTheBlobStaysWhole) {
  struct W {
    static GRCORE_Result snapshot(
        GRCORE_Context *, void * value, GRCORE_SnapshotWriter * w) {
      auto * r = static_cast<GRCORE_Result *>(value);
      *r = grcore_snapshot_writer_write(w, nullptr, 3);
      EXPECT_EQ(grcore_snapshot_writer_write(w, nullptr, 0), GRCORE_OK);
      EXPECT_EQ(grcore_snapshot_writer_size(w), 0u);
      EXPECT_NE(grcore_snapshot_writer_allocator(w), nullptr);
      return GRCORE_OK;
    }
    static GRCORE_Result restore(GRCORE_Context *, void *,
        GRCORE_SnapshotReader *, void *, GRCORE_RestoreMode) {
      return GRCORE_OK;
    }
    static GRCORE_Result settle(
        GRCORE_Context *, void *, void *, GRCORE_SettleMode) {
      return GRCORE_OK;
    }
  };
  static const GRCORE_Key k = GRCORE_KEY_INIT("w", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_NONE,
      nullptr, nullptr, W::snapshot, W::restore, W::settle);
  GRCORE_Result seen = GRCORE_OK;
  RunWorld a;
  ASSERT_EQ(grcore_context_register(a.ctx, &k, &seen), GRCORE_OK);
  Taken t;
  ASSERT_EQ(grcore_context_snapshot(a.ctx, nullptr, &t.s), GRCORE_OK);
  EXPECT_EQ(seen, GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_snapshot_size(t.s), 0u);
}

TEST(Snapshot, AWriterPastTheCapIsALimitErrorAndTheBlobStaysWhole) {
  struct W {
    static GRCORE_Result snapshot(
        GRCORE_Context *, void * value, GRCORE_SnapshotWriter * w) {
      auto * out = static_cast<GRCORE_Result *>(value);
      char c = 0;
      /* The pointer is never read: the size is refused first. */
      out[0] = grcore_snapshot_writer_write(
          w, &c, (size_t)GRCORE_SNAPSHOT_MAX_BYTES + 1u);
      out[1] = grcore_snapshot_writer_write(w, &c, 1);
      out[2] = grcore_snapshot_writer_write(
          w, &c, (size_t)GRCORE_SNAPSHOT_MAX_BYTES); // total + size past it
      EXPECT_EQ(grcore_snapshot_writer_size(w), 1u);
      return GRCORE_OK;
    }
    static GRCORE_Result restore(GRCORE_Context *, void *,
        GRCORE_SnapshotReader *, void *, GRCORE_RestoreMode) {
      return GRCORE_OK;
    }
    static GRCORE_Result settle(
        GRCORE_Context *, void *, void *, GRCORE_SettleMode) {
      return GRCORE_OK;
    }
  };
  static const GRCORE_Key k = GRCORE_KEY_INIT("cap", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_NONE,
      nullptr, nullptr, W::snapshot, W::restore, W::settle);
  GRCORE_Result seen[3] = {GRCORE_OK, GRCORE_OK, GRCORE_OK};
  RunWorld a;
  ASSERT_EQ(grcore_context_register(a.ctx, &k, seen), GRCORE_OK);
  Taken t;
  ASSERT_EQ(grcore_context_snapshot(a.ctx, nullptr, &t.s), GRCORE_OK);
  EXPECT_EQ(seen[0], GRCORE_ERR_LIMIT);
  EXPECT_EQ(seen[1], GRCORE_OK);
  EXPECT_EQ(seen[2], GRCORE_ERR_LIMIT);
  EXPECT_EQ(grcore_snapshot_size(t.s), 1u);
}

TEST(Snapshot, ADestinationKeyWithHooksAndNoNameIsRefusedNotDereferenced) {
  ToyWorld src;
  src.fill();
  Taken t;
  ASSERT_EQ(grcore_context_snapshot(src.w.ctx, nullptr, &t.s), GRCORE_OK);
  Toy n;
  ToyWorld w;
  static const GRCORE_Key nameless = GRCORE_KEY_INIT(nullptr, GRCORE_CARDINALITY_ONE,
      GRCORE_PHASE_NONE, nullptr, nullptr, toy_snapshot, toy_restore, toy_settle);
  n.context = w.w.ctx;
  ASSERT_EQ(grcore_context_register(w.w.ctx, &nameless, &n), GRCORE_OK);
  EXPECT_EQ(grcore_context_restore(w.w.ctx, t.s, nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(w.a.check + w.a.apply + w.b.check + w.b.apply, 0);
}

/* ---- Allocation failure ------------------------------------------------------ */

TEST(Snapshot, EveryAllocationFailureDuringTakeIsACleanErrorWithNothingLeaked) {
  long total = 0;
  {
    TrackingAllocator probe;
    ToyWorld w;
    w.fill();
    GRCORE_Snapshot * s = nullptr;
    ASSERT_EQ(grcore_context_snapshot(w.w.ctx, probe.get(), &s), GRCORE_OK);
    grcore_snapshot_release(s);
    total = probe.calls;
  }
  ASSERT_GT(total, 3);
  for (long n = 1; n <= total; n++) {
    TrackingAllocator mem;
    mem.fail_at = n;
    ToyWorld w;
    w.fill();
    GRCORE_Snapshot * out = nullptr;
    EXPECT_EQ(grcore_context_snapshot(w.w.ctx, mem.get(), &out), GRCORE_ERR_OOM)
        << "failing allocation " << n;
    EXPECT_EQ(out, nullptr);
    EXPECT_EQ(mem.live, 0) << "leak at allocation " << n;
  }
}

TEST(Snapshot, EveryAllocationFailureDuringRestoreLeavesTheDestinationFreshAndLeaksNothing) {
  ToyWorld src;
  src.fill();
  Taken t;
  ASSERT_EQ(grcore_context_snapshot(src.w.ctx, nullptr, &t.s), GRCORE_OK);
  long total = 0;
  {
    TrackingAllocator probe;
    ToyWorld dst(true, probe.get());
    long before = probe.calls;
    ASSERT_EQ(grcore_context_restore(dst.w.ctx, t.s, nullptr), GRCORE_OK);
    total = probe.calls - before;
  }
  ASSERT_GE(total, 2);
  for (long k = 1; k <= total; k++) {
    TrackingAllocator mem;
    {
      ToyWorld dst(true, mem.get());
      mem.fail_at = mem.calls + k;
      GRCORE_Result r = grcore_context_restore(dst.w.ctx, t.s, nullptr);
      EXPECT_EQ(r, GRCORE_ERR_OOM) << "failing restore allocation " << k;
      EXPECT_EQ(dst.a.scratch, nullptr);
      EXPECT_EQ(dst.b.scratch, nullptr);
      EXPECT_TRUE(dst.a.values.empty());
      EXPECT_EQ(dst.a.commit, 0);
      mem.fail_at = 0;
      // Still fresh, so a second attempt works.
      EXPECT_EQ(grcore_context_restore(dst.w.ctx, t.s, nullptr), GRCORE_OK);
    }
    EXPECT_EQ(mem.live, 0) << "leak when restore allocation " << k << " failed";
  }
}

TEST(Snapshot, ADestinationOverItsMemoryBudgetFailsWithLimitAndStaysFresh) {
  ToyWorld src;
  src.fill();
  Taken t;
  ASSERT_EQ(grcore_context_snapshot(src.w.ctx, nullptr, &t.s), GRCORE_OK);
  Toy a, b;
  RunWorld w(GRCORE_UNLIMITED, 64, 0); // 64 bytes, no reserve
  a.name = "a";
  b.name = "b";
  a.context = b.context = w.ctx;
  a.scratch_bytes = b.scratch_bytes = 4096;
  ASSERT_EQ(grcore_context_register(w.ctx, &kToyA, &a), GRCORE_OK);
  ASSERT_EQ(grcore_context_register(w.ctx, &kToyB, &b), GRCORE_OK);
  EXPECT_EQ(grcore_context_restore(w.ctx, t.s, nullptr), GRCORE_ERR_LIMIT);
  EXPECT_EQ(a.scratch, nullptr);
  EXPECT_TRUE(a.values.empty());
  EXPECT_EQ(a.abandon, 0) << "the key that failed released its own";
  EXPECT_EQ(grcore_context_state(w.ctx), GRCORE_CONTEXT_PARKED);
}

/* ---- Threads ----------------------------------------------------------------- */

TEST(Snapshot, OneSnapshotRestoredConcurrentlyIntoContextsOnManyThreads) {
  ToyWorld src;
  src.fill();
  Taken t;
  ASSERT_EQ(grcore_context_snapshot(src.w.ctx, nullptr, &t.s), GRCORE_OK);
  std::atomic<int> good{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; i++) {
    threads.emplace_back([&] {
      for (int round = 0; round < 50; round++) {
        grcore_snapshot_retain(t.s);
        ToyWorld dst;
        if (grcore_context_restore(dst.w.ctx, t.s, nullptr) == GRCORE_OK &&
            dst.a.values == std::vector<uint64_t>{1, 2, 3} &&
            dst.a.tag == "alpha") {
          good++;
        }
        grcore_snapshot_release(t.s);
      }
    });
  }
  for (auto & th : threads) {
    th.join();
  }
  EXPECT_EQ(good.load(), 8 * 50);
  EXPECT_EQ(grcore_snapshot_refcount(t.s), 1u);
}

TEST(Snapshot, ThePausedContextMigratesAfterTheSnapshotAndTheSnapshotIsUnaffected) {
  GuestHolder g;
  g.g.n = 120;
  RunWorld src(40);
  ASSERT_EQ(grcore_context_register(src.ctx, &kGuestKey, &g), GRCORE_OK);
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(src.ctx, counting_entry, &g.g, &outcome), GRCORE_OK);
  Taken t;
  ASSERT_EQ(grcore_context_snapshot(src.ctx, nullptr, &t.s), GRCORE_OK);
  // Restore on another thread while this one keeps the source.
  uint64_t sum = 0;
  std::thread([&] {
    GuestHolder d;
    RunWorld dst;
    EXPECT_EQ(grcore_context_register(dst.ctx, &kGuestKey, &d), GRCORE_OK);
    GRCORE_RestoreEnv env = GRCORE_RESTORE_ENV_INIT(nullptr, nullptr, nullptr, nullptr, nullptr);
    env.entry = counting_entry;
    env.entry_state = &d.g;
    EXPECT_EQ(grcore_context_restore(dst.ctx, t.s, &env), GRCORE_OK);
    GRCORE_Outcome o;
    EXPECT_EQ(grcore_resume(dst.ctx, &o), GRCORE_OK);
    sum = d.g.sum;
  }).join();
  EXPECT_EQ(sum, sum_below(120));
}

/* ---- Robustness: a nameless key, and the writer's cap ------------------------ */

TEST(Snapshot, ANamelessHookedDestinationKeyIsRefusedNotDereferenced) {
  ToyWorld src;
  src.fill();
  Taken t;
  ASSERT_EQ(grcore_context_snapshot(src.w.ctx, nullptr, &t.s), GRCORE_OK);
  static const GRCORE_Key nameless = GRCORE_KEY_INIT(nullptr, GRCORE_CARDINALITY_ONE,
      GRCORE_PHASE_NONE, nullptr, nullptr, toy_snapshot, toy_restore, toy_settle);
  Toy n;
  ToyWorld dst;
  n.context = dst.w.ctx;
  ASSERT_EQ(grcore_context_register(dst.w.ctx, &nameless, &n), GRCORE_OK);
  EXPECT_EQ(grcore_context_restore(dst.w.ctx, t.s, nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(dst.a.check + dst.a.apply, 0);
}

TEST(Snapshot, TheWriterRefusesAWriteOverTheCapAndOneThatWouldOverflowTheTotal) {
  struct Cap {
    static GRCORE_Result snapshot(
        GRCORE_Context *, void * value, GRCORE_SnapshotWriter * w) {
      auto * out = static_cast<std::vector<GRCORE_Result> *>(value);
      const char dummy = 0;
      out->push_back(grcore_snapshot_writer_write(
          w, &dummy, GRCORE_SNAPSHOT_MAX_BYTES + 1)); // larger than the cap
      out->push_back(grcore_snapshot_writer_write(w, &dummy, 1));
      // total + size would pass the cap: size alone is under it.
      out->push_back(grcore_snapshot_writer_write(
          w, &dummy, GRCORE_SNAPSHOT_MAX_BYTES));
      return GRCORE_OK;
    }
    static GRCORE_Result restore(GRCORE_Context *, void *,
        GRCORE_SnapshotReader *, void *, GRCORE_RestoreMode) {
      return GRCORE_OK;
    }
    static GRCORE_Result settle(
        GRCORE_Context *, void *, void *, GRCORE_SettleMode) {
      return GRCORE_OK;
    }
  };
  static const GRCORE_Key k = GRCORE_KEY_INIT("cap", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_NONE,
      nullptr, nullptr, Cap::snapshot, Cap::restore, Cap::settle);
  std::vector<GRCORE_Result> seen;
  RunWorld a;
  ASSERT_EQ(grcore_context_register(a.ctx, &k, &seen), GRCORE_OK);
  Taken t;
  ASSERT_EQ(grcore_context_snapshot(a.ctx, nullptr, &t.s), GRCORE_OK);
  ASSERT_EQ(seen.size(), 3u);
  EXPECT_EQ(seen[0], GRCORE_ERR_LIMIT);
  EXPECT_EQ(seen[1], GRCORE_OK);
  EXPECT_EQ(seen[2], GRCORE_ERR_LIMIT);
  EXPECT_EQ(grcore_snapshot_size(t.s), 1u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

/**
 * @file
 *
 * A restore environment states its own size (b/snapshot.h), so the struct can
 * grow at the end. The cases are those of test_key_size.cpp, for the members
 * the environment gained after the first generation (`entry`, `entry_state`
 * and `pause_file`, which come after `lookup`).
 *
 * Every case is an environment as an older or a newer header would have built
 * it, and each has a control: the same environment at its full size, which
 * must behave the other way. The older-layout copy is exactly as large as its
 * size says, on the heap, so a read past the end is a finding under ASan or
 * Valgrind and not a silent zero; the members that must not be read are armed,
 * so that a core that ignores `size` fails a plain run.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace {

// A key that snapshots one number and records the environment its hooks see.
struct Holder {
  uint64_t value = 0;
  const void * seen = nullptr;
};
GRCORE_Result holder_snapshot(
    GRCORE_Context *, void * v, GRCORE_SnapshotWriter * w) {
  return grcore_snapshot_writer_u64(w, static_cast<Holder *>(v)->value);
}
GRCORE_Result holder_restore(GRCORE_Context *, void * v,
    GRCORE_SnapshotReader * r, void * env, GRCORE_RestoreMode mode) {
  auto * h = static_cast<Holder *>(v);
  uint64_t n = 0;
  GRCORE_Result res = grcore_snapshot_reader_u64(r, &n);
  if (res == GRCORE_OK && mode == GRCORE_RESTORE_APPLY) {
    h->value = n;
    h->seen = env;
  }
  return res;
}
GRCORE_Result holder_settle(
    GRCORE_Context *, void *, void *, GRCORE_SettleMode) {
  return GRCORE_OK;
}
const GRCORE_Key kHolderKey = GRCORE_KEY_INIT("holder", GRCORE_CARDINALITY_ONE,
    GRCORE_PHASE_NONE, nullptr, nullptr, holder_snapshot, holder_restore,
    holder_settle);

void * lookup_marker(void * user, const char *) { return user; }

// The environment as the first generation had it: `user` and `lookup`, and
// nothing after. A copy of the layout, so the bytes after it are not the
// test's.
struct OldEnv {
  size_t size;
  void * user;
  void * (*lookup)(void *, const char *);
};
static_assert(sizeof(OldEnv) == GRCORE_RESTORE_ENV_MIN_SIZE,
    "the old layout is the minimum layout");
static_assert(offsetof(OldEnv, lookup) == offsetof(GRCORE_RestoreEnv, lookup),
    "the old layout is a prefix of the current one");

const GRCORE_RestoreEnv * old_env_on_heap(void * user) {
  auto * e = static_cast<OldEnv *>(std::malloc(sizeof(OldEnv)));
  e->size = sizeof(OldEnv);
  e->user = user;
  e->lookup = lookup_marker;
  return reinterpret_cast<const GRCORE_RestoreEnv *>(e);
}

// An environment whose every member is armed: the control, at full size.
GRCORE_RestoreEnv armed_env(void * user, CountingGuest * guest) {
  return GRCORE_RESTORE_ENV_INIT(user, lookup_marker, counting_entry, guest,
      kCountingPollFile);
}

// A parked context holding a holder with the number `value`, snapshotted.
struct Source {
  Holder holder;
  RunWorld world;
  GRCORE_Snapshot * snapshot = nullptr;
  explicit Source(uint64_t value) {
    holder.value = value;
    EXPECT_EQ(grcore_context_register(world.ctx, &kHolderKey, &holder), GRCORE_OK);
    EXPECT_EQ(grcore_context_snapshot(world.ctx, nullptr, &snapshot), GRCORE_OK);
  }
  ~Source() { grcore_snapshot_release(snapshot); }
};

// A paused context (a guest stopped by a fuel budget), snapshotted.
struct PausedSource {
  Holder holder;
  CountingGuest guest;
  RunWorld world;
  GRCORE_Snapshot * snapshot = nullptr;
  PausedSource() : world(10) {
    guest.n = 100;
    EXPECT_EQ(grcore_context_register(world.ctx, &kHolderKey, &holder), GRCORE_OK);
    GRCORE_Outcome outcome;
    EXPECT_EQ(grcore_run(world.ctx, counting_entry, &guest, &outcome), GRCORE_OK);
    EXPECT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
    EXPECT_EQ(grcore_context_snapshot(world.ctx, nullptr, &snapshot), GRCORE_OK);
    EXPECT_TRUE(grcore_snapshot_was_paused(snapshot));
  }
  ~PausedSource() { grcore_snapshot_release(snapshot); }
};

struct Destination {
  Holder holder;
  RunWorld world;
  Destination() {
    EXPECT_EQ(grcore_context_register(world.ctx, &kHolderKey, &holder), GRCORE_OK);
  }
};

} // namespace

TEST(RestoreEnvSize, TheInitialiserWritesTheSizeOfTheStructItWasCompiledAgainst) {
  static const GRCORE_RestoreEnv e = GRCORE_RESTORE_ENV_INIT(
      nullptr, nullptr, nullptr, nullptr, nullptr);
  EXPECT_EQ(e.size, sizeof(GRCORE_RestoreEnv));
  EXPECT_GE(e.size, GRCORE_RESTORE_ENV_MIN_SIZE);
  EXPECT_TRUE(grcore_restore_env_valid(&e));
  EXPECT_FALSE(grcore_restore_env_valid(nullptr));
}

TEST(RestoreEnvSize, AnOlderShorterEnvironmentIsUsedForItsLookupAndItsAbsentMembersAreNeverRead) {
  Source src(77);
  static int marker;
  const GRCORE_RestoreEnv * old = old_env_on_heap(&marker);
  {
    Destination dst;
    ASSERT_EQ(grcore_context_restore(dst.world.ctx, src.snapshot, old), GRCORE_OK);
    EXPECT_EQ(dst.holder.value, 77u);
    EXPECT_EQ(dst.holder.seen, &marker) << "lookup is in the first generation";
  }
  std::free(const_cast<GRCORE_RestoreEnv *>(old));
}

TEST(RestoreEnvSize, MembersBeyondTheStatedSizeAreNotReadAndAtFullSizeAre) {
  // The plain-run form of the test above: the members are real and armed, and
  // only `size` keeps them unread.
  PausedSource src;
  {
    CountingGuest unused;
    GRCORE_RestoreEnv shortened = armed_env(nullptr, &unused);
    shortened.size = GRCORE_RESTORE_ENV_MIN_SIZE;
    Destination dst;
    EXPECT_EQ(grcore_context_restore(dst.world.ctx, src.snapshot, &shortened),
        GRCORE_ERR_INVALID)
        << "a paused snapshot needs `entry`, which this size does not reach";
    EXPECT_EQ(grcore_context_state(dst.world.ctx), GRCORE_CONTEXT_PARKED);
    EXPECT_EQ(dst.holder.value, 0u);
  }
  // Control: the same bytes at full size restore, and resume with the entry.
  {
    CountingGuest resumed;
    GRCORE_RestoreEnv whole = armed_env(nullptr, &resumed);
    Destination dst;
    ASSERT_EQ(grcore_context_restore(dst.world.ctx, src.snapshot, &whole), GRCORE_OK);
    EXPECT_EQ(grcore_context_state(dst.world.ctx), GRCORE_CONTEXT_PAUSED);
    EXPECT_STREQ(grcore_context_pause_location(dst.world.ctx).file,
        kCountingPollFile);
  }
}

TEST(RestoreEnvSize, EachTrailingMemberIsReadOnlyWhereTheSizeReachesItsEnd) {
  PausedSource src;
  struct Cut {
    size_t size;
    bool restores;
    bool has_file;
  };
  const Cut cuts[] = {
      {GRCORE_RESTORE_ENV_MIN_SIZE, false, false},
      {offsetof(GRCORE_RestoreEnv, entry_state), true, false},
      {offsetof(GRCORE_RestoreEnv, pause_file), true, false},
      {sizeof(GRCORE_RestoreEnv), true, true},
  };
  for (const Cut & cut : cuts) {
    CountingGuest guest;
    GRCORE_RestoreEnv e = armed_env(nullptr, &guest);
    e.size = cut.size;
    Destination dst;
    GRCORE_Result r = grcore_context_restore(dst.world.ctx, src.snapshot, &e);
    if (!cut.restores) {
      EXPECT_EQ(r, GRCORE_ERR_INVALID) << "size " << cut.size;
      continue;
    }
    ASSERT_EQ(r, GRCORE_OK) << "size " << cut.size;
    const char * file = grcore_context_pause_location(dst.world.ctx).file;
    if (cut.has_file) {
      EXPECT_STREQ(file, kCountingPollFile) << "size " << cut.size;
    } else {
      EXPECT_EQ(file, nullptr) << "size " << cut.size;
    }
    if (cut.size >= offsetof(GRCORE_RestoreEnv, pause_file)) {
      // `entry_state` reached the context: the guest it names resumes.
      GRCORE_Outcome outcome;
      ASSERT_EQ(grcore_resume(dst.world.ctx, &outcome), GRCORE_OK);
      EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
      EXPECT_GT(guest.pos, 0u) << "size " << cut.size;
    }
  }
}

TEST(RestoreEnvSize, ASizeBelowTheMinimumOrOffAlignmentIsRefusedNotRead) {
  Source src(5);
  const size_t bad[] = {0, 1, GRCORE_RESTORE_ENV_MIN_SIZE - sizeof(void *),
      GRCORE_RESTORE_ENV_MIN_SIZE - 1, GRCORE_RESTORE_ENV_MIN_SIZE + 1,
      sizeof(GRCORE_RestoreEnv) + 1,
      sizeof(GRCORE_RestoreEnv) + sizeof(void *) / 2};
  for (size_t size : bad) {
    CountingGuest guest;
    GRCORE_RestoreEnv e = armed_env(nullptr, &guest);
    e.size = size;
    EXPECT_FALSE(grcore_restore_env_valid(&e)) << "size " << size;
    Destination dst;
    EXPECT_EQ(grcore_context_restore(dst.world.ctx, src.snapshot, &e),
        GRCORE_ERR_INVALID)
        << "size " << size;
    EXPECT_EQ(dst.holder.value, 0u) << "the destination is as it was";
  }
  // Controls: the smallest size and the full size are accepted, so the list
  // above is refused for its size and not for anything else about it. NULL
  // stays "no environment".
  for (size_t size : {GRCORE_RESTORE_ENV_MIN_SIZE, sizeof(GRCORE_RestoreEnv)}) {
    CountingGuest guest;
    GRCORE_RestoreEnv e = armed_env(nullptr, &guest);
    e.size = size;
    Destination dst;
    EXPECT_EQ(grcore_context_restore(dst.world.ctx, src.snapshot, &e), GRCORE_OK)
        << "size " << size;
    EXPECT_EQ(dst.holder.value, 5u);
  }
  Destination dst;
  EXPECT_EQ(grcore_context_restore(dst.world.ctx, src.snapshot, nullptr), GRCORE_OK);
}

TEST(RestoreEnvSize, AnEnvironmentFromANewerHeaderIsAcceptedAndItsUnknownTailIgnored) {
  // Decision: accepted, as for a key: a member this library does not know is
  // one whose absence is a defined degradation.
  Source src(9);
  const size_t extra = 2 * sizeof(void *);
  auto * block = static_cast<unsigned char *>(
      std::malloc(sizeof(GRCORE_RestoreEnv) + extra));
  static int marker;
  CountingGuest guest;
  GRCORE_RestoreEnv proto = armed_env(&marker, &guest);
  std::memcpy(block, &proto, sizeof proto);
  std::memset(block + sizeof(GRCORE_RestoreEnv), 0xA5, extra);
  auto * e = reinterpret_cast<GRCORE_RestoreEnv *>(block);
  e->size = sizeof(GRCORE_RestoreEnv) + extra;
  EXPECT_TRUE(grcore_restore_env_valid(e));
  Destination dst;
  ASSERT_EQ(grcore_context_restore(dst.world.ctx, src.snapshot, e), GRCORE_OK);
  EXPECT_EQ(dst.holder.value, 9u);
  EXPECT_EQ(dst.holder.seen, &marker) << "the members it knows are used";
  std::free(block);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

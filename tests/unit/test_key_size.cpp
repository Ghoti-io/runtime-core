/**
 * @file
 *
 * A key states its own size (key.h), so the struct can grow at the end.
 *
 * Every case is a key as an older or a newer header would have built it, and
 * each has a control: the same key at its full size, which must behave the
 * other way, so that a test that passes because the hook was never wired
 * cannot pass. The hooks that must not be read are tripwires that fail the
 * test when called; the older-layout copies are exactly as large as their
 * size says, on the heap, so a read past the end is a finding under ASan or
 * Valgrind and not a silent zero.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace {

int g_poll_calls = 0;
int g_snapshot_calls = 0;
int g_restore_calls = 0;
int g_settle_calls = 0;
int g_destroy_calls = 0;

void count_destroy(GRCORE_Context *, void *) { g_destroy_calls++; }
void tripwire_poll(GRCORE_Context *, void *, GRCORE_PollCall *) {
  g_poll_calls++;
}
GRCORE_Result tripwire_snapshot(
    GRCORE_Context *, void *, GRCORE_SnapshotWriter *) {
  g_snapshot_calls++;
  return GRCORE_OK;
}
GRCORE_Result tripwire_restore(GRCORE_Context *, void *,
    GRCORE_SnapshotReader *, void *, GRCORE_RestoreMode) {
  g_restore_calls++;
  return GRCORE_OK;
}
GRCORE_Result tripwire_settle(
    GRCORE_Context *, void *, void *, GRCORE_SettleMode) {
  g_settle_calls++;
  return GRCORE_OK;
}

void reset_counts() {
  g_poll_calls = g_snapshot_calls = g_restore_calls = g_settle_calls =
      g_destroy_calls = 0;
}

// The key as the first generation had it: everything through `destroy`, and
// nothing after. It is a copy of the layout, not the real struct, so that the
// bytes after it really are not the test's.
struct OldKey {
  size_t size;
  const char * name;
  GRCORE_Cardinality cardinality;
  GRCORE_Phase phase;
  void (*destroy)(GRCORE_Context *, void *);
};
static_assert(sizeof(OldKey) == GRCORE_KEY_MIN_SIZE,
    "the old layout is the minimum layout");
static_assert(offsetof(OldKey, destroy) == offsetof(GRCORE_Key, destroy),
    "the old layout is a prefix of the current one");
static_assert(offsetof(OldKey, name) == offsetof(GRCORE_Key, name), "prefix");

// A key whose tail is tripwires, built at full size: the control.
GRCORE_Key tripwire_key(const char * name, GRCORE_Phase phase) {
  return GRCORE_KEY_INIT(name, GRCORE_CARDINALITY_ONE, phase, count_destroy,
      tripwire_poll, tripwire_snapshot, tripwire_restore, tripwire_settle);
}

// An exactly-sized heap block holding an OldKey. Reading one byte past it is
// a heap overflow to ASan and an invalid read to Valgrind.
const GRCORE_Key * old_key_on_heap(GRCORE_Phase phase) {
  auto * k = static_cast<OldKey *>(std::malloc(sizeof(OldKey)));
  k->size = sizeof(OldKey);
  k->name = "old";
  k->cardinality = GRCORE_CARDINALITY_ONE;
  k->phase = phase;
  k->destroy = count_destroy;
  return reinterpret_cast<const GRCORE_Key *>(k);
}

// Runs one poll with a request pending, so that the slow path runs every
// phase. The guest asks for a pause and gives up at once.
void poll_once(GRCORE_Context * c) {
  GRCORE_Port * port = nullptr;
  ASSERT_EQ(grcore_context_port(c, &port), GRCORE_OK);
  ASSERT_EQ(grcore_port_post(port, GRCORE_REQUEST_INTERRUPT), GRCORE_OK);
  grcore_port_release(port);
  Fn fn{[](GRCORE_Context * ctx) {
    return GRCORE_POLL(ctx) == GRCORE_VERDICT_CONTINUE ? GRCORE_STEP_FINISHED
                                                       : GRCORE_STEP_UNWOUND;
  }};
  GRCORE_Outcome outcome;
  // The interrupt unwinds the run with its own result; only the handlers
  // that ran matter here.
  (void)grcore_run(c, fn_entry, &fn, &outcome);
}

size_t blob_count_of(GRCORE_Context * c) {
  GRCORE_Snapshot * s = nullptr;
  EXPECT_EQ(grcore_context_snapshot(c, nullptr, &s), GRCORE_OK);
  size_t n = s != nullptr ? grcore_snapshot_blob_count(s) : 99;
  grcore_snapshot_release(s);
  return n;
}

} // namespace

TEST(KeySize, TheInitialiserWritesTheSizeOfTheStructItWasCompiledAgainst) {
  static const GRCORE_Key k = GRCORE_KEY_INIT("k", GRCORE_CARDINALITY_ONE,
      GRCORE_PHASE_NONE, nullptr, nullptr, nullptr, nullptr, nullptr);
  EXPECT_EQ(k.size, sizeof(GRCORE_Key));
  EXPECT_GE(k.size, GRCORE_KEY_MIN_SIZE);
  EXPECT_TRUE(grcore_key_valid(&k));
  EXPECT_FALSE(grcore_key_valid(nullptr));
}

TEST(KeySize, AnOlderShorterKeyRegistersAndItsAbsentHooksAreNeverRead) {
  reset_counts();
  const GRCORE_Key * old = old_key_on_heap(GRCORE_PHASE_OBSERVE);
  {
    RunWorld w;
    ASSERT_EQ(grcore_context_register(w.ctx, old, w.ctx), GRCORE_OK);
    GRCORE_RequestKind kind;
    EXPECT_EQ(grcore_context_request_kind(w.ctx, old, &kind), GRCORE_OK);
    // Not part of a snapshot: `snapshot` is absent, so it reads as NULL.
    EXPECT_EQ(blob_count_of(w.ctx), 0u);
    // Not polled: `poll` is absent, so it reads as NULL, and the phase scan
    // that looks at every key does not fault.
    poll_once(w.ctx);
    EXPECT_EQ(g_destroy_calls, 0);
  }
  EXPECT_EQ(g_destroy_calls, 1) << "the first-generation destroy still runs";
  std::free(const_cast<GRCORE_Key *>(old));
}

TEST(KeySize, TripwiresBeyondTheStatedSizeAreNotCalledAndAtFullSizeAre) {
  // The plain-run form of the test above: the tail is real and armed, and
  // only `size` keeps it unread. A core that ignored `size` fails here
  // without a sanitizer.
  reset_counts();
  {
    GRCORE_Key shortened = tripwire_key("short", GRCORE_PHASE_OBSERVE);
    shortened.size = GRCORE_KEY_MIN_SIZE;
    RunWorld w;
    ASSERT_EQ(grcore_context_register(w.ctx, &shortened, w.ctx), GRCORE_OK);
    EXPECT_EQ(blob_count_of(w.ctx), 0u);
    poll_once(w.ctx);
    EXPECT_EQ(g_poll_calls, 0);
    EXPECT_EQ(g_snapshot_calls, 0);
    EXPECT_EQ(g_settle_calls, 0);
  }
  EXPECT_EQ(g_destroy_calls, 1);
  // Control: the same bytes at full size are read.
  reset_counts();
  {
    GRCORE_Key whole = tripwire_key("whole", GRCORE_PHASE_OBSERVE);
    RunWorld w;
    ASSERT_EQ(grcore_context_register(w.ctx, &whole, w.ctx), GRCORE_OK);
    EXPECT_EQ(blob_count_of(w.ctx), 1u);
    poll_once(w.ctx);
    EXPECT_EQ(g_poll_calls, 1);
    EXPECT_EQ(g_snapshot_calls, 1);
  }
}

TEST(KeySize, EachTrailingMemberIsReadOnlyWhereTheSizeReachesItsEnd) {
  struct Cut {
    size_t size;
    int polls;
    size_t blobs;
  };
  const Cut cuts[] = {
      {offsetof(GRCORE_Key, poll), 0, 0},
      {offsetof(GRCORE_Key, poll) + sizeof(void *), 1, 0},
      {offsetof(GRCORE_Key, snapshot) + sizeof(void *), 1, 0},
      {offsetof(GRCORE_Key, restore) + sizeof(void *), 1, 0},
      {sizeof(GRCORE_Key), 1, 1},
  };
  for (const Cut & cut : cuts) {
    reset_counts();
    GRCORE_Key k = tripwire_key("cut", GRCORE_PHASE_OBSERVE);
    k.size = cut.size;
    // A snapshot hook without its restore and settle is refused, as ever; a
    // cut that drops them leaves a key that has no snapshot hook, or an
    // incomplete one.
    RunWorld w;
    ASSERT_EQ(grcore_context_register(w.ctx, &k, w.ctx), GRCORE_OK)
        << "size " << cut.size;
    GRCORE_Snapshot * s = nullptr;
    GRCORE_Result r = grcore_context_snapshot(w.ctx, nullptr, &s);
    if (cut.blobs == 1) {
      EXPECT_EQ(r, GRCORE_OK);
      EXPECT_EQ(grcore_snapshot_blob_count(s), 1u);
      grcore_snapshot_release(s);
    } else if (cut.size > offsetof(GRCORE_Key, snapshot)) {
      // snapshot present, restore or settle absent: the incomplete set.
      EXPECT_EQ(r, GRCORE_ERR_INVALID) << "size " << cut.size;
    } else {
      EXPECT_EQ(r, GRCORE_OK);
      grcore_snapshot_release(s);
    }
    poll_once(w.ctx);
    EXPECT_EQ(g_poll_calls, cut.polls) << "size " << cut.size;
  }
}

TEST(KeySize, ASizeBelowTheMinimumOrOffAlignmentIsRefusedNotRead) {
  const size_t bad[] = {0, 1, GRCORE_KEY_MIN_SIZE - sizeof(void *),
      GRCORE_KEY_MIN_SIZE - 1, GRCORE_KEY_MIN_SIZE + 1,
      sizeof(GRCORE_Key) + 1, sizeof(GRCORE_Key) + sizeof(void *) / 2};
  for (size_t size : bad) {
    GRCORE_Key k = tripwire_key("bad", GRCORE_PHASE_OBSERVE);
    k.size = size;
    EXPECT_FALSE(grcore_key_valid(&k)) << "size " << size;
    RunWorld w;
    EXPECT_EQ(grcore_context_register(w.ctx, &k, w.ctx), GRCORE_ERR_INVALID)
        << "size " << size;
    GRCORE_RequestKind kind;
    EXPECT_EQ(grcore_context_request_kind(w.ctx, &k, &kind),
        GRCORE_ERR_INVALID)
        << "size " << size;
    EXPECT_EQ(grcore_context_registration_count(w.ctx), 0u);
  }
  // Controls: the smallest size and the full size are accepted, so the list
  // above is refused for its size and not for anything else about the key.
  for (size_t size : {GRCORE_KEY_MIN_SIZE, sizeof(GRCORE_Key)}) {
    GRCORE_Key k = tripwire_key("ok", GRCORE_PHASE_OBSERVE);
    k.size = size;
    RunWorld w;
    EXPECT_EQ(grcore_context_register(w.ctx, &k, w.ctx), GRCORE_OK)
        << "size " << size;
  }
}

TEST(KeySize, AKeyFromANewerHeaderIsAcceptedAndItsUnknownTailIgnored) {
  // Decision: accepted. A member this core does not know is one whose
  // absence is a defined degradation (a hook not run), so refusing the key
  // would only make a newer library unusable on an older core for nothing.
  reset_counts();
  const size_t extra = 2 * sizeof(void *);
  auto * block = static_cast<unsigned char *>(
      std::malloc(sizeof(GRCORE_Key) + extra));
  GRCORE_Key proto = tripwire_key("newer", GRCORE_PHASE_OBSERVE);
  std::memcpy(block, &proto, sizeof proto);
  // The unknown members hold garbage that would crash if called or followed.
  std::memset(block + sizeof(GRCORE_Key), 0xA5, extra);
  auto * key = reinterpret_cast<GRCORE_Key *>(block);
  key->size = sizeof(GRCORE_Key) + extra;
  EXPECT_TRUE(grcore_key_valid(key));
  {
    RunWorld w;
    ASSERT_EQ(grcore_context_register(w.ctx, key, w.ctx), GRCORE_OK);
    EXPECT_EQ(blob_count_of(w.ctx), 1u) << "the members it knows are used";
    poll_once(w.ctx);
    EXPECT_EQ(g_poll_calls, 1);
  }
  EXPECT_EQ(g_destroy_calls, 1);
  std::free(block);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

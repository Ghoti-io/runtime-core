/**
 * @file
 *
 * The cross-heap handle table (b/handles.h, CAP-14): tokens, owners and
 * holders, roots, release, stale tokens, tombstones, limits and threads.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../../src/b/context_internal.h"
#include "../../src/b/group_internal.h"
#include "../../src/b/handles_internal.h"

#include <ghoti.io/runtime-core/b/handles.h>
#include <ghoti.io/runtime-core/b/snapshot.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <set>

namespace {

/* A group and any number of contexts, each destroyed by the thread that made
 * it (a context is owned by its creator). */
struct World {
  GRCORE_Group * group = nullptr;
  GRCORE_HandleTable * table = nullptr;
  std::vector<GRCORE_Context *> contexts;

  explicit World(const GRCORE_Allocator * allocator = nullptr) {
    EXPECT_EQ(grcore_group_create(allocator, nullptr, &group), GRCORE_OK);
    table = grcore_group_handles(group);
  }
  World(const World &) = delete;
  World & operator=(const World &) = delete;
  GRCORE_Context * make() {
    GRCORE_Context * c = nullptr;
    EXPECT_EQ(grcore_context_create(group, nullptr, &c), GRCORE_OK);
    contexts.push_back(c);
    return c;
  }
  void kill(GRCORE_Context * c) {
    EXPECT_EQ(grcore_context_destroy(c), GRCORE_OK);
    contexts.erase(std::find(contexts.begin(), contexts.end(), c));
  }
  GRCORE_HandleHolder enroll(GRCORE_Context * c) {
    GRCORE_HandleHolder h = GRCORE_HANDLE_HOLDER_INIT;
    EXPECT_EQ(grcore_handles_enroll(table, c, &h), GRCORE_OK);
    return h;
  }
  GRCORE_HandleToken pin(GRCORE_Context * owner, GRCORE_HandleHolder holder,
      uint64_t value) {
    GRCORE_HandleToken t = GRCORE_HANDLE_TOKEN_INIT;
    EXPECT_EQ(grcore_handles_pin(table, owner, holder, value, &t), GRCORE_OK);
    return t;
  }
  ~World() {
    while (!contexts.empty()) {
      EXPECT_EQ(grcore_context_destroy(contexts.back()), GRCORE_OK);
      contexts.pop_back();
    }
    // Every context is gone: the table is empty and charges nothing.
    EXPECT_EQ(grcore_handles_count(table), 0u);
    EXPECT_EQ(grcore_handles_tombstone_count(table), 0u);
    EXPECT_EQ(grcore_group_memory_in_use(group), 0u);
    EXPECT_EQ(grcore_group_memory_blocks(group), 0u);
    EXPECT_EQ(grcore_group_destroy(group), GRCORE_OK);
  }
};

/* A stand-in for a moving collector: reports every precise root of a context
 * and, if asked, rewrites each word. */
std::vector<uint64_t> roots_of(GRCORE_Context * c, int64_t add = 0) {
  struct State {
    std::vector<uint64_t> words;
    int64_t add;
  } state{{}, add};
  GRCORE_RootVisitor v;
  v.user = &state;
  v.slot = [](void * user, uint64_t * slot) {
    auto * s = static_cast<State *>(user);
    s->words.push_back(*slot);
    *slot = static_cast<uint64_t>(static_cast<int64_t>(*slot) + s->add);
  };
  v.range = nullptr;
  EXPECT_EQ(grcore_context_enumerate_roots(c, &v), GRCORE_OK);
  std::sort(state.words.begin(), state.words.end());
  return state.words;
}

uint64_t resolve(World & w, GRCORE_Context * owner, GRCORE_HandleToken t) {
  uint64_t v = ~0ull;
  EXPECT_EQ(grcore_handles_resolve(w.table, t, owner, &v), GRCORE_HANDLE_LIVE);
  return v;
}

} // namespace

TEST(Handles, AFreshGroupHasAnEmptyTableWithTheDefaultLimits) {
  World w;
  ASSERT_NE(w.table, nullptr);
  EXPECT_EQ(grcore_handles_count(w.table), 0u);
  EXPECT_EQ(grcore_handles_limit_per_holder(w.table),
      GRCORE_HANDLES_DEFAULT_PER_HOLDER);
  EXPECT_EQ(grcore_handles_limit_total(w.table), GRCORE_HANDLES_DEFAULT_TOTAL);
  EXPECT_EQ(grcore_group_memory_in_use(w.group), 0u);
  EXPECT_EQ(grcore_group_handles(nullptr), nullptr);
  EXPECT_EQ(grcore_handles_count(nullptr), 0u);
  EXPECT_EQ(grcore_handles_limit_total(nullptr), 0u);
}

TEST(Handles, PinResolveInspectAndReleaseRoundTrip) {
  World w;
  GRCORE_Context * owner = w.make();
  GRCORE_Context * holder = w.make();
  GRCORE_HandleHolder h = w.enroll(holder);
  GRCORE_HandleToken t = w.pin(owner, h, 0xABCD1234u);
  EXPECT_NE(t.generation, 0u);
  EXPECT_NE(t.tag, 0u);
  EXPECT_EQ(grcore_handles_count(w.table), 1u);
  EXPECT_EQ(grcore_handles_owned_count(w.table, owner), 1u);
  EXPECT_EQ(grcore_handles_held_count(w.table, holder), 1u);
  EXPECT_EQ(grcore_handles_held_count(w.table, owner), 0u);

  EXPECT_EQ(resolve(w, owner, t), 0xABCD1234u);
  uint64_t o = 0, hid = 0;
  EXPECT_EQ(grcore_handles_inspect(w.table, t, holder, &o, &hid),
      GRCORE_HANDLE_LIVE);
  EXPECT_NE(o, 0u);
  EXPECT_NE(hid, 0u);
  EXPECT_NE(o, hid);
  uint64_t o2 = 0;
  EXPECT_EQ(grcore_handles_inspect(w.table, t, owner, &o2, nullptr),
      GRCORE_HANDLE_LIVE);
  EXPECT_EQ(o2, o);

  EXPECT_EQ(grcore_handles_release(w.table, t, holder), GRCORE_HANDLE_LIVE);
  EXPECT_EQ(grcore_handles_count(w.table), 0u);
  EXPECT_EQ(grcore_handles_owned_count(w.table, owner), 0u);
  EXPECT_EQ(grcore_handles_held_count(w.table, holder), 0u);
  // The token is stale for both parties now, and a second release changes
  // nothing.
  EXPECT_EQ(grcore_handles_inspect(w.table, t, holder, nullptr, nullptr),
      GRCORE_HANDLE_STALE);
  EXPECT_EQ(grcore_handles_resolve(w.table, t, owner, nullptr),
      GRCORE_HANDLE_STALE);
  EXPECT_EQ(grcore_handles_release(w.table, t, holder), GRCORE_HANDLE_STALE);
}

TEST(Handles, AnOwnerCanRevokeAndTheHolderSeesAStaleToken) {
  World w;
  GRCORE_Context * owner = w.make();
  GRCORE_Context * holder = w.make();
  GRCORE_HandleToken t = w.pin(owner, w.enroll(holder), 7);
  EXPECT_EQ(grcore_handles_release(w.table, t, owner), GRCORE_HANDLE_LIVE);
  EXPECT_EQ(grcore_handles_inspect(w.table, t, holder, nullptr, nullptr),
      GRCORE_HANDLE_STALE);
  EXPECT_EQ(grcore_handles_owned_count(w.table, owner), 0u);
}

TEST(Handles, AGenerationIsNeverReusedEvenWhenTheIndexIs) {
  World w;
  GRCORE_Context * owner = w.make();
  GRCORE_HandleHolder h = w.enroll(owner);
  std::set<uint64_t> generations;
  GRCORE_HandleToken previous = GRCORE_HANDLE_TOKEN_INIT;
  for (int i = 0; i < 2000; i++) {
    GRCORE_HandleToken t = w.pin(owner, h, static_cast<uint64_t>(i));
    EXPECT_TRUE(generations.insert(t.generation).second) << i;
    if (i > 0) {
      // The one free index is taken again, and the old token stays dead.
      EXPECT_EQ(t.index, previous.index);
      EXPECT_GT(t.generation, previous.generation);
      EXPECT_EQ(grcore_handles_inspect(w.table, previous, owner, nullptr, nullptr),
          GRCORE_HANDLE_STALE);
      EXPECT_EQ(grcore_handles_resolve(w.table, previous, owner, nullptr),
          GRCORE_HANDLE_STALE);
    }
    EXPECT_EQ(resolve(w, owner, t), static_cast<uint64_t>(i));
    ASSERT_EQ(grcore_handles_release(w.table, t, owner), GRCORE_HANDLE_LIVE);
    previous = t;
  }
}

TEST(Handles, ForgedAndForeignTokensAreStale) {
  World w;
  GRCORE_Context * owner = w.make();
  GRCORE_HandleHolder h = w.enroll(owner);
  GRCORE_HandleToken t = w.pin(owner, h, 1);
  auto status = [&](GRCORE_HandleToken k) {
    return grcore_handles_inspect(w.table, k, owner, nullptr, nullptr);
  };
  ASSERT_EQ(status(t), GRCORE_HANDLE_LIVE);
  GRCORE_HandleToken none = GRCORE_HANDLE_TOKEN_INIT;
  EXPECT_EQ(status(none), GRCORE_HANDLE_STALE);
  GRCORE_HandleToken k = t;
  k.generation += 1;
  EXPECT_EQ(status(k), GRCORE_HANDLE_STALE);
  k = t;
  k.generation -= 1;
  EXPECT_EQ(status(k), GRCORE_HANDLE_STALE);
  k = t;
  k.tag ^= 1u;
  EXPECT_EQ(status(k), GRCORE_HANDLE_STALE);
  k = t;
  k.index += 1;
  EXPECT_EQ(status(k), GRCORE_HANDLE_STALE);
  k = t;
  k.index = 0x7ffffff0u;
  EXPECT_EQ(status(k), GRCORE_HANDLE_STALE);
  // A token of another table, even at the same index and generation.
  World other;
  GRCORE_Context * oc = other.make();
  GRCORE_HandleToken ot = other.pin(oc, other.enroll(oc), 1);
  EXPECT_NE(ot.tag, t.tag);
  EXPECT_EQ(status(ot), GRCORE_HANDLE_STALE);
  EXPECT_EQ(grcore_handles_release(w.table, ot, owner), GRCORE_HANDLE_STALE);
  EXPECT_EQ(status(t), GRCORE_HANDLE_LIVE);
  EXPECT_EQ(grcore_handles_release(w.table, t, owner), GRCORE_HANDLE_LIVE);
}

TEST(Handles, ATokenIsRefusedFromAnyContextButItsHolderAndOwner) {
  World w;
  GRCORE_Context * owner = w.make();
  GRCORE_Context * holder = w.make();
  GRCORE_Context * third = w.make();
  w.enroll(third);
  GRCORE_HandleToken t = w.pin(owner, w.enroll(holder), 99);
  uint64_t o = 5, hid = 5, v = 5;
  EXPECT_EQ(grcore_handles_inspect(w.table, t, third, &o, &hid),
      GRCORE_HANDLE_FOREIGN);
  EXPECT_EQ(o, 5u); // nothing about the entry is reported
  EXPECT_EQ(hid, 5u);
  EXPECT_EQ(grcore_handles_resolve(w.table, t, third, &v), GRCORE_HANDLE_FOREIGN);
  EXPECT_EQ(v, 5u);
  EXPECT_EQ(grcore_handles_release(w.table, t, third), GRCORE_HANDLE_FOREIGN);
  // A context that never took part is refused the same way.
  GRCORE_Context * stranger = w.make();
  EXPECT_EQ(grcore_handles_inspect(w.table, t, stranger, nullptr, nullptr),
      GRCORE_HANDLE_FOREIGN);
  // The holder cannot read the value: only the owner resolves.
  EXPECT_EQ(grcore_handles_resolve(w.table, t, holder, &v), GRCORE_HANDLE_FOREIGN);
  EXPECT_EQ(v, 5u);
  EXPECT_EQ(grcore_handles_count(w.table), 1u);
  EXPECT_EQ(grcore_handles_release(w.table, t, holder), GRCORE_HANDLE_LIVE);
}

TEST(Handles, BadCallsAreRefusedAndChangeNothing) {
  World w;
  GRCORE_Context * c = w.make();
  GRCORE_HandleHolder h = w.enroll(c);
  GRCORE_HandleToken t = GRCORE_HANDLE_TOKEN_INIT;
  EXPECT_EQ(grcore_handles_pin(nullptr, c, h, 1, &t), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_handles_pin(w.table, nullptr, h, 1, &t), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_handles_pin(w.table, c, h, 1, nullptr), GRCORE_ERR_INVALID);
  GRCORE_HandleHolder none = GRCORE_HANDLE_HOLDER_INIT;
  EXPECT_EQ(grcore_handles_pin(w.table, c, none, 1, &t), GRCORE_ERR_INVALID);
  GRCORE_HandleHolder bad = h;
  bad.tag ^= 1u;
  EXPECT_EQ(grcore_handles_pin(w.table, c, bad, 1, &t), GRCORE_ERR_INVALID);
  bad = h;
  bad.serial += 1;
  EXPECT_EQ(grcore_handles_pin(w.table, c, bad, 1, &t), GRCORE_ERR_INVALID);
  bad = h;
  bad.slot = 1000;
  EXPECT_EQ(grcore_handles_pin(w.table, c, bad, 1, &t), GRCORE_ERR_INVALID);
  EXPECT_EQ(t.generation, 0u); // never written on a refusal
  EXPECT_EQ(grcore_handles_enroll(w.table, c, nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_handles_enroll(nullptr, c, &h), GRCORE_ERR_INVALID);
  GRCORE_HandleToken some = w.pin(c, h, 1);
  EXPECT_EQ(grcore_handles_resolve(nullptr, some, c, nullptr),
      GRCORE_HANDLE_BAD_CALL);
  EXPECT_EQ(grcore_handles_resolve(w.table, some, nullptr, nullptr),
      GRCORE_HANDLE_BAD_CALL);
  EXPECT_EQ(grcore_handles_inspect(w.table, some, nullptr, nullptr, nullptr),
      GRCORE_HANDLE_BAD_CALL);
  EXPECT_EQ(grcore_handles_release(w.table, some, nullptr), GRCORE_HANDLE_BAD_CALL);
  // A context of another group.
  World other;
  GRCORE_Context * oc = other.make();
  EXPECT_EQ(grcore_handles_pin(w.table, oc, h, 1, &t), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_handles_enroll(w.table, oc, &h), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_handles_resolve(w.table, some, oc, nullptr),
      GRCORE_HANDLE_BAD_CALL);
  EXPECT_EQ(grcore_handles_held_count(w.table, oc), 0u);
  EXPECT_EQ(grcore_handles_count(w.table), 1u);
  EXPECT_EQ(grcore_handles_release(w.table, some, c), GRCORE_HANDLE_LIVE);
}

TEST(Handles, ACallFromAThreadThatDoesNotOwnTheContextIsRefused) {
  World w;
  GRCORE_Context * owner = w.make();
  GRCORE_HandleHolder h = w.enroll(owner);
  GRCORE_HandleToken t = w.pin(owner, h, 3);
  GRCORE_Result pin = GRCORE_OK;
  GRCORE_HandleStatus resolve_status = GRCORE_HANDLE_LIVE,
                      inspect_status = GRCORE_HANDLE_LIVE,
                      release_status = GRCORE_HANDLE_LIVE;
  GRCORE_HandleHolder enrolled;
  GRCORE_Result enroll = GRCORE_OK;
  size_t owned = 99, held = 99;
  std::thread([&] {
    GRCORE_HandleToken k;
    pin = grcore_handles_pin(w.table, owner, h, 1, &k);
    enroll = grcore_handles_enroll(w.table, owner, &enrolled);
    resolve_status = grcore_handles_resolve(w.table, t, owner, nullptr);
    inspect_status = grcore_handles_inspect(w.table, t, owner, nullptr, nullptr);
    release_status = grcore_handles_release(w.table, t, owner);
    owned = grcore_handles_owned_count(w.table, owner);
    held = grcore_handles_held_count(w.table, owner);
  }).join();
  EXPECT_EQ(pin, GRCORE_ERR_INVALID);
  EXPECT_EQ(enroll, GRCORE_ERR_INVALID);
  EXPECT_EQ(resolve_status, GRCORE_HANDLE_BAD_CALL);
  EXPECT_EQ(inspect_status, GRCORE_HANDLE_BAD_CALL);
  EXPECT_EQ(release_status, GRCORE_HANDLE_BAD_CALL);
  EXPECT_EQ(owned, 0u);
  EXPECT_EQ(held, 0u);
  EXPECT_EQ(grcore_handles_count(w.table), 1u);
  EXPECT_EQ(grcore_handles_release(w.table, t, owner), GRCORE_HANDLE_LIVE);
}

TEST(Handles, EnrollIsIdempotentAndAHolderNameIsNeverReused) {
  World w;
  GRCORE_Context * c = w.make();
  GRCORE_HandleHolder a = w.enroll(c);
  GRCORE_HandleHolder b = w.enroll(c);
  EXPECT_EQ(a.serial, b.serial);
  EXPECT_EQ(a.slot, b.slot);
  EXPECT_TRUE(grcore_handles_holder_alive(w.table, a));
  std::set<uint64_t> serials{a.serial};
  for (int i = 0; i < 50; i++) {
    GRCORE_Context * d = w.make();
    GRCORE_HandleHolder h = w.enroll(d);
    EXPECT_TRUE(serials.insert(h.serial).second);
    w.kill(d);
    EXPECT_FALSE(grcore_handles_holder_alive(w.table, h)) << i;
  }
  EXPECT_TRUE(grcore_handles_holder_alive(w.table, a));
  GRCORE_HandleHolder none = GRCORE_HANDLE_HOLDER_INIT;
  EXPECT_FALSE(grcore_handles_holder_alive(w.table, none));
  EXPECT_FALSE(grcore_handles_holder_alive(nullptr, a));
}

TEST(Handles, AValueHeldThroughTheTableIsARootOfItsOwner) {
  World w;
  GRCORE_Context * owner = w.make();
  GRCORE_Context * other = w.make();
  GRCORE_Context * holder = w.make();
  GRCORE_HandleHolder h = w.enroll(holder);
  EXPECT_EQ(grcore_context_root_source_count(owner), 0u);
  EXPECT_TRUE(roots_of(owner).empty());
  GRCORE_HandleToken a = w.pin(owner, h, 100);
  GRCORE_HandleToken b = w.pin(owner, h, 200);
  GRCORE_HandleToken c = w.pin(other, h, 300);
  // One source per owner, whatever the number of entries.
  EXPECT_EQ(grcore_context_root_source_count(owner), 1u);
  EXPECT_EQ(grcore_context_root_source_count(other), 1u);
  EXPECT_EQ(grcore_context_root_source_count(holder), 0u);
  EXPECT_EQ(roots_of(owner), (std::vector<uint64_t>{100, 200}));
  EXPECT_EQ(roots_of(other), (std::vector<uint64_t>{300}));
  EXPECT_TRUE(roots_of(holder).empty());

  // A moving collector rewrites the words it is shown; the owner then reads
  // the moved value.
  roots_of(owner, 1000);
  EXPECT_EQ(resolve(w, owner, a), 1100u);
  EXPECT_EQ(resolve(w, owner, b), 1200u);
  EXPECT_EQ(resolve(w, other, c), 300u);

  // A released entry is no longer a root, and its word is gone.
  ASSERT_EQ(grcore_handles_release(w.table, a, holder), GRCORE_HANDLE_LIVE);
  EXPECT_EQ(roots_of(owner), (std::vector<uint64_t>{1200}));
  ASSERT_EQ(grcore_handles_release(w.table, b, holder), GRCORE_HANDLE_LIVE);
  EXPECT_TRUE(roots_of(owner).empty());
  // The source stays registered until the context ends, and reports nothing.
  EXPECT_EQ(grcore_context_root_source_count(owner), 1u);
  GRCORE_HandleToken d = w.pin(owner, h, 5);
  EXPECT_EQ(grcore_context_root_source_count(owner), 1u);
  EXPECT_EQ(roots_of(owner), (std::vector<uint64_t>{5}));
  grcore_handles_release(w.table, d, holder);
  grcore_handles_release(w.table, c, holder);
}

TEST(Handles, ARootVisitorWithoutASlotCallbackSeesNothingAndNothingBreaks) {
  World w;
  GRCORE_Context * c = w.make();
  w.pin(c, w.enroll(c), 1);
  GRCORE_RootVisitor v;
  v.user = nullptr;
  v.slot = nullptr;
  v.range = nullptr;
  EXPECT_EQ(grcore_context_enumerate_roots(c, &v), GRCORE_OK);
}

TEST(Handles, ADeadOwnersEntriesBecomeTombstones) {
  World w;
  GRCORE_Context * owner = w.make();
  GRCORE_Context * holder = w.make();
  GRCORE_HandleHolder h = w.enroll(holder);
  GRCORE_HandleToken a = w.pin(owner, h, 1);
  GRCORE_HandleToken b = w.pin(owner, h, 2);
  GRCORE_HandleToken live = w.pin(holder, h, 3);
  uint64_t o = 0, hid = 0;
  ASSERT_EQ(grcore_handles_inspect(w.table, a, holder, &o, &hid),
      GRCORE_HANDLE_LIVE);
  w.kill(owner);

  // The tombstone keeps no word: nothing of the dead heap is left in the table.
  EXPECT_EQ(w.table->entries[a.index].value, 0u);
  EXPECT_EQ(w.table->entries[b.index].value, 0u);
  EXPECT_EQ(grcore_handles_count(w.table), 3u);
  EXPECT_EQ(grcore_handles_tombstone_count(w.table), 2u);
  EXPECT_EQ(grcore_handles_held_count(w.table, holder), 3u);
  uint64_t o2 = 0, h2 = 0;
  EXPECT_EQ(grcore_handles_inspect(w.table, a, holder, &o2, &h2),
      GRCORE_HANDLE_TOMBSTONE);
  EXPECT_EQ(o2, o); // the dead owner is still named
  EXPECT_EQ(h2, hid);
  EXPECT_EQ(grcore_handles_inspect(w.table, b, holder, nullptr, nullptr),
      GRCORE_HANDLE_TOMBSTONE);
  // The holder cannot resolve a tombstone (nobody can), and an entry whose
  // owner is alive is unaffected.
  uint64_t v = 5;
  EXPECT_EQ(grcore_handles_resolve(w.table, a, holder, &v), GRCORE_HANDLE_FOREIGN);
  EXPECT_EQ(v, 5u);
  EXPECT_EQ(resolve(w, holder, live), 3u);
  // A tombstone still counts against its holder until it is released.
  EXPECT_EQ(grcore_handles_release(w.table, a, holder), GRCORE_HANDLE_TOMBSTONE);
  EXPECT_EQ(grcore_handles_tombstone_count(w.table), 1u);
  EXPECT_EQ(grcore_handles_inspect(w.table, a, holder, nullptr, nullptr),
      GRCORE_HANDLE_STALE);
  EXPECT_EQ(grcore_handles_release(w.table, a, holder), GRCORE_HANDLE_STALE);
  EXPECT_EQ(grcore_handles_release(w.table, b, holder), GRCORE_HANDLE_TOMBSTONE);
  EXPECT_EQ(grcore_handles_tombstone_count(w.table), 0u);
  EXPECT_EQ(grcore_handles_release(w.table, live, holder), GRCORE_HANDLE_LIVE);
}

TEST(Handles, ADeadHoldersEntriesAreReleasedAndTheOwnersRootsGoWithThem) {
  World w;
  GRCORE_Context * owner = w.make();
  GRCORE_Context * holder = w.make();
  GRCORE_Context * keeper = w.make();
  GRCORE_HandleHolder h = w.enroll(holder);
  GRCORE_HandleHolder k = w.enroll(keeper);
  GRCORE_HandleToken a = w.pin(owner, h, 10);
  GRCORE_HandleToken b = w.pin(owner, h, 20);
  GRCORE_HandleToken kept = w.pin(owner, k, 30);
  EXPECT_EQ(roots_of(owner), (std::vector<uint64_t>{10, 20, 30}));
  w.kill(holder);
  EXPECT_EQ(grcore_handles_count(w.table), 1u);
  EXPECT_EQ(grcore_handles_owned_count(w.table, owner), 1u);
  EXPECT_EQ(roots_of(owner), (std::vector<uint64_t>{30}));
  EXPECT_EQ(grcore_handles_inspect(w.table, a, owner, nullptr, nullptr),
      GRCORE_HANDLE_STALE);
  EXPECT_EQ(grcore_handles_resolve(w.table, b, owner, nullptr),
      GRCORE_HANDLE_STALE);
  EXPECT_EQ(resolve(w, owner, kept), 30u);
  // A pin for the dead holder is refused, not dereferenced.
  GRCORE_HandleToken t = GRCORE_HANDLE_TOKEN_INIT;
  EXPECT_EQ(grcore_handles_pin(w.table, owner, h, 1, &t), GRCORE_ERR_INVALID);
  EXPECT_FALSE(grcore_handles_holder_alive(w.table, h));
  EXPECT_EQ(grcore_handles_release(w.table, kept, keeper), GRCORE_HANDLE_LIVE);
}

TEST(Handles, AContextThatBothOwnsAndHoldsEndsCleanly) {
  World w;
  GRCORE_Context * a = w.make();
  GRCORE_Context * b = w.make();
  GRCORE_HandleHolder ha = w.enroll(a), hb = w.enroll(b);
  GRCORE_HandleToken self = w.pin(a, ha, 1);   // a owns, a holds
  GRCORE_HandleToken ab = w.pin(a, hb, 2);     // a owns, b holds
  GRCORE_HandleToken ba = w.pin(b, ha, 3);     // b owns, a holds
  EXPECT_EQ(resolve(w, a, self), 1u);
  w.kill(a);
  // self: released with its holder. ab: tombstone for b. ba: released.
  EXPECT_EQ(grcore_handles_count(w.table), 1u);
  EXPECT_EQ(grcore_handles_tombstone_count(w.table), 1u);
  EXPECT_EQ(grcore_handles_inspect(w.table, ab, b, nullptr, nullptr),
      GRCORE_HANDLE_TOMBSTONE);
  EXPECT_EQ(grcore_handles_owned_count(w.table, b), 0u);
  EXPECT_TRUE(roots_of(b).empty());
  EXPECT_EQ(grcore_handles_release(w.table, ab, b), GRCORE_HANDLE_TOMBSTONE);
  (void)ba;
}

TEST(Handles, LimitsBindPerHolderAndInTotalAndAReleaseGivesTheRoomBack) {
  World w;
  GRCORE_Context * owner = w.make();
  GRCORE_Context * h1c = w.make();
  GRCORE_Context * h2c = w.make();
  GRCORE_HandleHolder h1 = w.enroll(h1c), h2 = w.enroll(h2c);
  EXPECT_EQ(grcore_handles_set_limits(w.table, 2, 3), GRCORE_OK);
  EXPECT_EQ(grcore_handles_limit_per_holder(w.table), 2u);
  EXPECT_EQ(grcore_handles_limit_total(w.table), 3u);
  GRCORE_HandleToken a = w.pin(owner, h1, 1);
  GRCORE_HandleToken b = w.pin(owner, h1, 2);
  GRCORE_HandleToken t = GRCORE_HANDLE_TOKEN_INIT;
  EXPECT_EQ(grcore_handles_pin(w.table, owner, h1, 3, &t), GRCORE_ERR_LIMIT);
  EXPECT_EQ(t.generation, 0u);
  GRCORE_HandleToken c = w.pin(owner, h2, 4); // another holder has room
  EXPECT_EQ(grcore_handles_pin(w.table, owner, h2, 5, &t), GRCORE_ERR_LIMIT);
  EXPECT_EQ(grcore_handles_count(w.table), 3u);
  EXPECT_EQ(roots_of(owner), (std::vector<uint64_t>{1, 2, 4})); // refusal: no root
  ASSERT_EQ(grcore_handles_release(w.table, a, h1c), GRCORE_HANDLE_LIVE);
  EXPECT_EQ(grcore_handles_pin(w.table, owner, h1, 6, &t), GRCORE_OK);
  EXPECT_EQ(resolve(w, owner, t), 6u);
  // Lowering the limits evicts nothing.
  EXPECT_EQ(grcore_handles_set_limits(w.table, 1, 1), GRCORE_OK);
  EXPECT_EQ(grcore_handles_count(w.table), 3u);
  EXPECT_EQ(resolve(w, owner, b), 2u);
  (void)c;
}

TEST(Handles, ATombstoneStillCountsAgainstItsHolder) {
  World w;
  GRCORE_Context * owner = w.make();
  GRCORE_Context * holder = w.make();
  GRCORE_HandleHolder h = w.enroll(holder);
  ASSERT_EQ(grcore_handles_set_limits(w.table, 1, 10), GRCORE_OK);
  GRCORE_HandleToken a = w.pin(owner, h, 1);
  w.kill(owner);
  GRCORE_Context * owner2 = w.make();
  GRCORE_HandleToken t;
  EXPECT_EQ(grcore_handles_pin(w.table, owner2, h, 2, &t), GRCORE_ERR_LIMIT);
  ASSERT_EQ(grcore_handles_release(w.table, a, holder), GRCORE_HANDLE_TOMBSTONE);
  EXPECT_EQ(grcore_handles_pin(w.table, owner2, h, 2, &t), GRCORE_OK);
}

TEST(Handles, SetLimitsRefusesZeroAndTooLargeAndAcceptsUnlimited) {
  World w;
  EXPECT_EQ(grcore_handles_set_limits(nullptr, 1, 1), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_handles_set_limits(w.table, 0, 5), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_handles_set_limits(w.table, 5, 0), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_handles_set_limits(w.table, 5, GRCORE_HANDLES_MAX_ENTRIES + 1),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_handles_limit_per_holder(w.table),
      GRCORE_HANDLES_DEFAULT_PER_HOLDER); // unchanged by the refusals
  EXPECT_EQ(grcore_handles_set_limits(w.table, GRCORE_HANDLES_UNLIMITED,
                GRCORE_HANDLES_UNLIMITED),
      GRCORE_OK);
  EXPECT_EQ(grcore_handles_limit_per_holder(w.table), GRCORE_HANDLES_UNLIMITED);
  EXPECT_EQ(grcore_handles_limit_total(w.table), GRCORE_HANDLES_MAX_ENTRIES);
}

TEST(Handles, ARunOutOfGenerationsIsALimitAndChangesNothing) {
  World w;
  GRCORE_Context * c = w.make();
  GRCORE_HandleHolder h = w.enroll(c);
  GRCORE_HandleToken a = w.pin(c, h, 1);
  w.table->next_generation = UINT64_MAX; // as if 2^64 - 2 had been issued
  GRCORE_HandleToken t = GRCORE_HANDLE_TOKEN_INIT;
  EXPECT_EQ(grcore_handles_pin(w.table, c, h, 2, &t), GRCORE_ERR_LIMIT);
  EXPECT_EQ(t.generation, 0u);
  EXPECT_EQ(grcore_handles_count(w.table), 1u);
  EXPECT_EQ(grcore_handles_release(w.table, a, c), GRCORE_HANDLE_LIVE);
}

TEST(Handles, ManyEntriesGrowTheTableAndTheGroupMeterReturnsToZero) {
  World w;
  GRCORE_Context * owner = w.make();
  GRCORE_Context * holder = w.make();
  GRCORE_HandleHolder h = w.enroll(holder);
  ASSERT_EQ(grcore_handles_set_limits(w.table, 100000, 100000), GRCORE_OK);
  uint64_t baseline = grcore_group_memory_in_use(w.group);
  std::vector<GRCORE_HandleToken> tokens;
  for (uint64_t i = 0; i < 5000; i++) {
    tokens.push_back(w.pin(owner, h, i * 3));
  }
  EXPECT_GT(grcore_group_memory_in_use(w.group), baseline);
  EXPECT_EQ(grcore_handles_count(w.table), 5000u);
  std::vector<uint64_t> expected;
  for (uint64_t i = 0; i < 5000; i++) {
    expected.push_back(i * 3);
  }
  EXPECT_EQ(roots_of(owner), expected);
  // Release in a scrambled order: both lists stay consistent.
  for (size_t i = 0; i < tokens.size(); i += 2) {
    ASSERT_EQ(grcore_handles_release(w.table, tokens[i], holder), GRCORE_HANDLE_LIVE);
  }
  EXPECT_EQ(grcore_handles_owned_count(w.table, owner), 2500u);
  EXPECT_EQ(roots_of(owner).size(), 2500u);
  for (size_t i = 1; i < tokens.size(); i += 2) {
    EXPECT_EQ(resolve(w, owner, tokens[i]), static_cast<uint64_t>(i) * 3);
  }
  // The rest go with the holder.
  w.kill(holder);
  EXPECT_EQ(grcore_handles_count(w.table), 0u);
}

TEST(Handles, AllocationFailureInPinLeavesTheTableConsistentAtEveryPoint) {
  int refused = 0, succeeded = 0;
  for (long fail = 1; fail <= 12; fail++) {
    TrackingAllocator t;
    {
      World w(t.get());
      GRCORE_Context * owner = w.make();
      GRCORE_Context * holder = w.make();
      t.calls = 0;
      t.fail_at = fail;
      GRCORE_HandleHolder h = GRCORE_HANDLE_HOLDER_INIT;
      GRCORE_Result r = grcore_handles_enroll(w.table, holder, &h);
      GRCORE_HandleToken tok = GRCORE_HANDLE_TOKEN_INIT;
      GRCORE_Result p = GRCORE_ERR_INVALID;
      if (r == GRCORE_OK) {
        p = grcore_handles_pin(w.table, owner, h, 77, &tok);
      } else {
        EXPECT_EQ(r, GRCORE_ERR_OOM);
      }
      t.fail_at = 0;
      if (p == GRCORE_OK) {
        succeeded++;
        EXPECT_EQ(resolve(w, owner, tok), 77u);
        EXPECT_EQ(grcore_handles_count(w.table), 1u);
      } else {
        refused++;
        if (r == GRCORE_OK) {
          EXPECT_TRUE(p == GRCORE_ERR_OOM || p == GRCORE_ERR_LIMIT) << p;
        }
        EXPECT_EQ(tok.generation, 0u);
        EXPECT_EQ(grcore_handles_count(w.table), 0u);
        EXPECT_TRUE(roots_of(owner).empty());
        // The refused call can simply be made again.
        GRCORE_HandleHolder h2 = w.enroll(holder);
        GRCORE_HandleToken again = w.pin(owner, h2, 78);
        EXPECT_EQ(resolve(w, owner, again), 78u);
      }
    } // World's destructor asserts the group is empty and charged nothing.
    EXPECT_EQ(t.live, 0) << "leak at failure " << fail;
  }
  // The sweep reached a failing allocation in the enroll, in the pin, and
  // then ran past the last one.
  EXPECT_GE(refused, 3);
  EXPECT_GE(succeeded, 1);
}

namespace {
GRCORE_Result gate_calls = GRCORE_OK;
}

TEST(Handles, ARootSourceTheContextRefusesFailsThePinAndLeavesNothingBehind) {
  // The context's own memory budget refuses the root-source table.
  World w;
  GRCORE_Options * o;
  ASSERT_EQ(grcore_options_create(nullptr, &o), GRCORE_OK);
  grcore_options_set_memory_bytes(o, 1);
  grcore_options_set_memory_reserve(o, 0);
  GRCORE_Context * tight = nullptr;
  ASSERT_EQ(grcore_context_create(w.group, o, &tight), GRCORE_OK);
  grcore_options_destroy(o);
  w.contexts.push_back(tight);
  GRCORE_Context * holder = w.make();
  GRCORE_HandleHolder h = w.enroll(holder);
  GRCORE_HandleToken t = GRCORE_HANDLE_TOKEN_INIT;
  gate_calls = grcore_handles_pin(w.table, tight, h, 1, &t);
  EXPECT_EQ(gate_calls, GRCORE_ERR_LIMIT);
  EXPECT_EQ(grcore_handles_count(w.table), 0u);
  EXPECT_EQ(grcore_context_root_source_count(tight), 0u);
  EXPECT_EQ(grcore_handles_owned_count(w.table, tight), 0u);
  EXPECT_EQ(t.generation, 0u);
  // Its party was not kept: the group's meter holds only the holder's.
  EXPECT_EQ(w.table->party_count, 1u);
}

TEST(Handles, ARestoreIntoAContextThatOwnsEntriesIsRefused) {
  World w;
  GRCORE_Context* src = w.make();
  GRCORE_Context* dst = w.make();
  GRCORE_Snapshot* snap = nullptr;
  ASSERT_EQ(grcore_context_snapshot(src, nullptr, &snap), GRCORE_OK);
  GRCORE_HandleToken t = w.pin(dst, w.enroll(src), 1);
  EXPECT_EQ(grcore_context_restore(dst, snap, nullptr), GRCORE_ERR_INVALID);
  // Releasing the entry makes the same restore legal again.
  ASSERT_EQ(grcore_handles_release(w.table, t, dst), GRCORE_HANDLE_LIVE);
  EXPECT_EQ(grcore_context_restore(dst, snap, nullptr), GRCORE_OK);
  // The context that only holds a token may be restored into.
  GRCORE_HandleToken held = w.pin(src, w.enroll(dst), 2);
  EXPECT_EQ(grcore_context_restore(dst, snap, nullptr), GRCORE_OK);
  EXPECT_EQ(grcore_handles_release(w.table, held, dst), GRCORE_HANDLE_LIVE);
  grcore_snapshot_release(snap);
}

namespace {

/* A key whose destructor tries to use the table while its context is being
 * destroyed: the table refuses (the context is tearing down), and the
 * context's destruction still releases what it held. */
struct DestructorProbe {
  GRCORE_HandleTable * table = nullptr;
  GRCORE_HandleToken token = GRCORE_HANDLE_TOKEN_INIT;
  GRCORE_HandleHolder holder = GRCORE_HANDLE_HOLDER_INIT;
  GRCORE_HandleStatus release_status = GRCORE_HANDLE_LIVE;
  GRCORE_Result pin_result = GRCORE_OK;
  GRCORE_Result enroll_result = GRCORE_OK;
};

void probe_destroy(GRCORE_Context * c, void * value) {
  auto * p = static_cast<DestructorProbe *>(value);
  p->release_status = grcore_handles_release(p->table, p->token, c);
  GRCORE_HandleToken t;
  p->pin_result = grcore_handles_pin(p->table, c, p->holder, 1, &t);
  GRCORE_HandleHolder h;
  p->enroll_result = grcore_handles_enroll(p->table, c, &h);
}

const GRCORE_Key kProbeKey = GRCORE_KEY_INIT(
    "test.handles.probe", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_NONE, probe_destroy,
    nullptr, nullptr, nullptr, nullptr);

} // namespace

TEST(Handles, AKeyDestructorFindsTheTableRefusingAContextThatIsTearingDown) {
  World w;
  GRCORE_Context * owner = w.make();
  GRCORE_Context * doomed = w.make();
  DestructorProbe probe;
  probe.table = w.table;
  probe.holder = w.enroll(doomed);
  probe.token = w.pin(owner, probe.holder, 9);
  ASSERT_EQ(grcore_context_register(doomed, &kProbeKey, &probe), GRCORE_OK);
  w.kill(doomed);
  EXPECT_EQ(probe.release_status, GRCORE_HANDLE_BAD_CALL);
  EXPECT_EQ(probe.pin_result, GRCORE_ERR_INVALID);
  EXPECT_EQ(probe.enroll_result, GRCORE_ERR_INVALID);
  // The destruction released what it held all the same.
  EXPECT_EQ(grcore_handles_count(w.table), 0u);
  EXPECT_TRUE(roots_of(owner).empty());
}

TEST(Handles, ThreadsPinForAHolderOnAnotherThreadWhileItReleasesAndDies) {
  World w;
  constexpr int kTokens = 400;
  GRCORE_Context * owner = w.make(); // main thread
  GRCORE_HandleHolder holder_name = GRCORE_HANDLE_HOLDER_INIT;
  std::mutex m;
  std::condition_variable cv;
  std::vector<GRCORE_HandleToken> queue;
  bool enrolled = false, producing_done = false;
  std::atomic<int> released{0};

  std::thread holder_thread([&] {
    GRCORE_Context * mine = nullptr;
    ASSERT_EQ(grcore_context_create(w.group, nullptr, &mine), GRCORE_OK);
    {
      std::lock_guard<std::mutex> g(m);
      ASSERT_EQ(grcore_handles_enroll(w.table, mine, &holder_name), GRCORE_OK);
      enrolled = true;
    }
    cv.notify_all();
    for (;;) {
      GRCORE_HandleToken t;
      {
        std::unique_lock<std::mutex> g(m);
        cv.wait(g, [&] { return !queue.empty() || producing_done; });
        if (queue.empty()) {
          break;
        }
        t = queue.back();
        queue.pop_back();
      }
      GRCORE_HandleStatus s =
          grcore_handles_inspect(w.table, t, mine, nullptr, nullptr);
      EXPECT_EQ(s, GRCORE_HANDLE_LIVE);
      EXPECT_EQ(grcore_handles_release(w.table, t, mine), GRCORE_HANDLE_LIVE);
      released++;
    }
    // Dies with whatever is left held (nothing is, but pins race the death).
    EXPECT_EQ(grcore_context_destroy(mine), GRCORE_OK);
  });

  {
    std::unique_lock<std::mutex> g(m);
    cv.wait(g, [&] { return enrolled; });
  }
  for (int i = 0; i < kTokens; i++) {
    GRCORE_HandleToken t;
    ASSERT_EQ(grcore_handles_pin(w.table, owner, holder_name,
                  static_cast<uint64_t>(i), &t),
        GRCORE_OK);
    // The collector may run on this thread at any time.
    if (i % 16 == 0) {
      roots_of(owner);
    }
    {
      std::lock_guard<std::mutex> g(m);
      queue.push_back(t);
    }
    cv.notify_one();
  }
  {
    std::lock_guard<std::mutex> g(m);
    producing_done = true;
  }
  cv.notify_all();
  holder_thread.join();
  EXPECT_EQ(released.load(), kTokens);
  EXPECT_EQ(grcore_handles_count(w.table), 0u);
  EXPECT_FALSE(grcore_handles_holder_alive(w.table, holder_name));
  GRCORE_HandleToken t;
  EXPECT_EQ(grcore_handles_pin(w.table, owner, holder_name, 1, &t),
      GRCORE_ERR_INVALID);
}

TEST(Handles, AnOwnerDyingOnItsThreadWhileTheHolderUsesTheTokensMakesTombstones) {
  World w;
  GRCORE_Context * holder = w.make();
  GRCORE_HandleHolder h = w.enroll(holder);
  constexpr int kTokens = 200;
  std::vector<GRCORE_HandleToken> tokens(kTokens);
  std::mutex m;
  std::condition_variable cv;
  bool pinned = false, may_die = false;
  std::thread owner_thread([&] {
    GRCORE_Context * mine = nullptr;
    ASSERT_EQ(grcore_context_create(w.group, nullptr, &mine), GRCORE_OK);
    for (int i = 0; i < kTokens; i++) {
      ASSERT_EQ(grcore_handles_pin(w.table, mine, h, static_cast<uint64_t>(i),
                    &tokens[i]),
          GRCORE_OK);
    }
    {
      std::lock_guard<std::mutex> g(m);
      pinned = true;
    }
    cv.notify_all();
    {
      std::unique_lock<std::mutex> g(m);
      cv.wait(g, [&] { return may_die; });
    }
    EXPECT_EQ(grcore_context_destroy(mine), GRCORE_OK);
  });
  {
    std::unique_lock<std::mutex> g(m);
    cv.wait(g, [&] { return pinned; });
  }
  // The holder inspects continuously while the owner is destroyed.
  {
    std::lock_guard<std::mutex> g(m);
    may_die = true;
  }
  cv.notify_all();
  size_t tombstones_seen = 0;
  for (int pass = 0; pass < 2000 && tombstones_seen < kTokens; pass++) {
    tombstones_seen = 0;
    for (auto & t : tokens) {
      GRCORE_HandleStatus s =
          grcore_handles_inspect(w.table, t, holder, nullptr, nullptr);
      ASSERT_TRUE(s == GRCORE_HANDLE_LIVE || s == GRCORE_HANDLE_TOMBSTONE);
      tombstones_seen += s == GRCORE_HANDLE_TOMBSTONE;
    }
    std::this_thread::yield();
  }
  owner_thread.join();
  EXPECT_EQ(grcore_handles_tombstone_count(w.table), static_cast<size_t>(kTokens));
  for (auto & t : tokens) {
    ASSERT_EQ(grcore_handles_release(w.table, t, holder), GRCORE_HANDLE_TOMBSTONE);
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

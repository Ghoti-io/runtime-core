/**
 * @file
 *
 * The registry of compiled code, the entry slots and the retired list (AD-28):
 * lookup by address range, and a code lifetime that outlasts every frame that
 * can return into it.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <algorithm>
#include <random>
#include <thread>

namespace {

/* A world with an engine, and the compiled-code fixture not yet registered. */
struct RWorld : StackWorld {
  HandCode code;
  explicit RWorld(const GRCORE_Allocator * allocator = nullptr)
      : StackWorld(GRCORE_UNLIMITED, GRCORE_UNLIMITED,
            GRCORE_DEFAULT_MEMORY_RESERVE, GRCORE_UNLIMITED, allocator) {}
};

GRCORE_ActivationRef enter(GRCORE_Stack * s, GRCORE_ActivationKind kind) {
  GRCORE_ActivationRef ref;
  EXPECT_EQ(grcore_activation_enter(s, kind, 0, false, nullptr, &ref), GRCORE_OK);
  return ref;
}

} // namespace

/* ---- The registry ------------------------------------------------------- */

TEST(Registry, LookupIsExactAtTheStartTheLastByteAndOnePastIt) {
  RWorld w;
  ASSERT_EQ(w.code.add_to(w.ctx, w.alpha), GRCORE_OK);
  GRCORE_CodeRange r;
  uintptr_t start = w.code.start(), end = start + w.code.size();
  ASSERT_TRUE(grcore_code_lookup(w.ctx, start, &r));
  EXPECT_EQ(r.start, start);
  EXPECT_EQ(r.end, end);
  EXPECT_EQ(r.engine, w.alpha);
  EXPECT_EQ(r.code, w.code.code);
  EXPECT_EQ(r.meta, &w.code.meta);
  EXPECT_FALSE(r.retired);
  EXPECT_TRUE(grcore_code_lookup(w.ctx, end - 1, &r));
  EXPECT_TRUE(grcore_code_lookup(w.ctx, start + 1, nullptr));
  EXPECT_FALSE(grcore_code_lookup(w.ctx, end, &r));
  EXPECT_FALSE(grcore_code_lookup(w.ctx, start - 1, &r));
  EXPECT_FALSE(grcore_code_lookup(w.ctx, 0, &r));
  EXPECT_FALSE(grcore_code_lookup(w.ctx, UINTPTR_MAX, &r));
  EXPECT_FALSE(grcore_code_lookup(nullptr, start, &r));
  EXPECT_EQ(grcore_code_registered_count(w.ctx), 1u);
}

TEST(Registry, AnEmptyRegistryAndAContextWithNoEngineFindNothing) {
  RunWorld bare;
  GRCORE_CodeRange r;
  EXPECT_FALSE(grcore_code_lookup(bare.ctx, 0x1000, &r));
  EXPECT_EQ(grcore_code_registered_count(bare.ctx), 0u);
  EXPECT_EQ(grcore_code_retired_count(bare.ctx), 0u);
  HandCode code;
  // With no engine there is no A state to keep the registry in.
  EXPECT_EQ(grcore_code_register(bare.ctx, 1, code.code, code.start(),
                code.size(), &code.meta),
      GRCORE_ERR_INVALID);
  GRCORE_EntrySlot * slot = nullptr;
  EXPECT_EQ(grcore_entry_slot_create(bare.ctx, &slot), GRCORE_ERR_INVALID);
  EXPECT_EQ(slot, nullptr);
  RWorld w;
  EXPECT_FALSE(grcore_code_lookup(w.ctx, 0x1000, &r));
  EXPECT_EQ(grcore_code_unregister(w.ctx, 0x1000), GRCORE_ERR_INVALID);
}

TEST(Registry, OverlapIsRefusedInEveryShapeAndChangesNothing) {
  RWorld w;
  HandCode other;
  ASSERT_EQ(w.code.add_to(w.ctx, w.alpha), GRCORE_OK);
  uintptr_t start = w.code.start();
  size_t size = w.code.size();
  GRCORE_CodeMeta m = other.meta;
  size_t before = grcore_code_refcount(other.code);
  struct Case { const char * name; uintptr_t start; size_t size; };
  const Case bad[] = {
      {"identical", start, size},
      {"inside", start + 16, 16},
      {"one byte at the start", start - 15, 16},
      {"one byte at the end", start + size - 1, 16},
      {"contains it", start - 16, size + 32},
      {"starts at the same address, longer", start, size + 64},
      {"ends at the same address, longer", start - 64, size + 64},
  };
  for (const Case & c : bad) {
    m.code_bytes = static_cast<uint32_t>(c.size);
    // The table must fit the code or the refusal would be for another reason:
    // sites past the end are trimmed to the ones that fit.
    m.site_count = std::min<size_t>(m.site_count, (c.size - 16) / 16);
    EXPECT_EQ(grcore_code_register(w.ctx, w.alpha, other.code, c.start, c.size, &m),
        GRCORE_ERR_INVALID)
        << c.name;
    EXPECT_EQ(grcore_code_registered_count(w.ctx), 1u) << c.name;
    EXPECT_EQ(grcore_code_refcount(other.code), before) << c.name;
  }
  // Touching is not overlapping, on either side.
  m = other.meta;
  m.code_bytes = 64;
  m.site_count = 3;
  EXPECT_EQ(grcore_code_register(w.ctx, w.alpha, other.code, start + size, 64, &m),
      GRCORE_OK);
  EXPECT_EQ(grcore_code_register(w.ctx, w.alpha, other.code, start - 64, 64, &m),
      GRCORE_OK);
  EXPECT_EQ(grcore_code_registered_count(w.ctx), 3u);
  GRCORE_CodeRange r;
  ASSERT_TRUE(grcore_code_lookup(w.ctx, start - 1, &r));
  EXPECT_EQ(r.start, start - 64);
  ASSERT_TRUE(grcore_code_lookup(w.ctx, start + size, &r));
  EXPECT_EQ(r.start, start + size);
  ASSERT_TRUE(grcore_code_lookup(w.ctx, start, &r));
  EXPECT_EQ(r.start, start);
}

TEST(Registry, ArgumentsAreCheckedAndEachRefusalLeavesNothingBehind) {
  RWorld w;
  HandCode & c = w.code;
  size_t before = grcore_code_refcount(c.code);
  EXPECT_EQ(grcore_code_register(w.ctx, w.alpha, nullptr, c.start(), c.size(), &c.meta),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_code_register(w.ctx, w.alpha, c.code, c.start(), c.size(), nullptr),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_code_register(nullptr, w.alpha, c.code, c.start(), c.size(), &c.meta),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_code_register(w.ctx, w.alpha, c.code, c.start(), 0, &c.meta),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_code_register(w.ctx, w.alpha, c.code, UINTPTR_MAX - 8, c.size(), &c.meta),
      GRCORE_ERR_INVALID); // wraps
  EXPECT_EQ(grcore_code_register(w.ctx, 0, c.code, c.start(), c.size(), &c.meta),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_code_register(w.ctx, 99, c.code, c.start(), c.size(), &c.meta),
      GRCORE_ERR_INVALID);
  // A table of another size than the range is corrupt, not merely wrong.
  GRCORE_CodeMeta wrong = c.meta;
  wrong.code_bytes += 16;
  EXPECT_EQ(grcore_code_register(w.ctx, w.alpha, c.code, c.start(), c.size(), &wrong),
      GRCORE_ERR_CORRUPT);
  GRCORE_CodeMeta bad_version = c.meta;
  bad_version.version = 2;
  EXPECT_EQ(grcore_code_register(w.ctx, w.alpha, c.code, c.start(), c.size(), &bad_version),
      GRCORE_ERR_CORRUPT);
  EXPECT_EQ(grcore_code_registered_count(w.ctx), 0u);
  EXPECT_EQ(grcore_code_refcount(c.code), before);
  // And a non-owner may not.
  std::thread([&] {
    EXPECT_EQ(c.add_to(w.ctx, w.alpha), GRCORE_ERR_INVALID);
    EXPECT_EQ(grcore_code_unregister(w.ctx, c.start()), GRCORE_ERR_INVALID);
  }).join();
  EXPECT_EQ(grcore_code_registered_count(w.ctx), 0u);
  ASSERT_EQ(c.add_to(w.ctx, w.alpha), GRCORE_OK);
}

TEST(Registry, TheRegistryHoldsOneCountedReferenceAndUnregisteringWithNoFrameReleasesIt) {
  RWorld w;
  size_t base = grcore_code_refcount(w.code.code);
  ASSERT_EQ(w.code.add_to(w.ctx, w.alpha), GRCORE_OK);
  EXPECT_EQ(grcore_code_refcount(w.code.code), base + 1);
  w.code.give_up();
  EXPECT_EQ(w.code.released, 0);
  // No JIT record is open, so the reference goes at once, and the range with it.
  GRCORE_CodeRange r;
  uintptr_t start = w.code.start();
  EXPECT_EQ(grcore_code_unregister(w.ctx, start), GRCORE_OK);
  EXPECT_EQ(w.code.released, 1);
  EXPECT_FALSE(grcore_code_lookup(w.ctx, start, &r));
  EXPECT_EQ(grcore_code_registered_count(w.ctx), 0u);
  EXPECT_EQ(grcore_code_retired_count(w.ctx), 0u);
  EXPECT_EQ(grcore_code_unregister(w.ctx, start), GRCORE_ERR_INVALID);
}

TEST(Registry, UnregisteringUnderALiveJitRecordKeepsTheRangeFoundUntilItLeaves) {
  RWorld w;
  ASSERT_EQ(w.code.add_to(w.ctx, w.alpha), GRCORE_OK);
  w.code.give_up();
  GRCORE_ActivationRef jit = enter(w.stack, GRCORE_ACTIVATION_JIT);
  uintptr_t start = w.code.start();
  ASSERT_EQ(grcore_code_unregister(w.ctx, start), GRCORE_OK);
  EXPECT_EQ(w.code.released, 0);
  EXPECT_EQ(grcore_code_registered_count(w.ctx), 0u);
  EXPECT_EQ(grcore_code_retired_count(w.ctx), 1u);
  // A frame may still return into it, so it is found, and says it is retired.
  GRCORE_CodeRange r;
  ASSERT_TRUE(grcore_code_lookup(w.ctx, start + 20, &r));
  EXPECT_TRUE(r.retired);
  // It cannot be unregistered twice, and its memory is not free for new code.
  EXPECT_EQ(grcore_code_unregister(w.ctx, start), GRCORE_ERR_INVALID);
  HandCode other;
  EXPECT_EQ(grcore_code_register(w.ctx, w.alpha, other.code, start, w.code.size(),
                &w.code.meta),
      GRCORE_ERR_INVALID);
  ASSERT_EQ(grcore_activation_leave(w.stack, jit), GRCORE_OK);
  EXPECT_EQ(w.code.released, 1);
  EXPECT_FALSE(grcore_code_lookup(w.ctx, start, &r));
  EXPECT_EQ(grcore_code_retired_count(w.ctx), 0u);
  // Now the address can be taken again.
  EXPECT_EQ(grcore_code_register(w.ctx, w.alpha, other.code, start, w.code.size(),
                &w.code.meta),
      GRCORE_OK);
}

TEST(Registry, ManyRangesRegisteredOutOfOrderAreEachFoundAndNoOtherAddressIs) {
  RWorld w;
  std::vector<std::unique_ptr<HandCode>> codes;
  for (int i = 0; i < 40; i++) {
    codes.push_back(std::make_unique<HandCode>());
  }
  std::vector<size_t> order(codes.size());
  for (size_t i = 0; i < order.size(); i++) {
    order[i] = i;
  }
  std::mt19937 rng(7);
  std::shuffle(order.begin(), order.end(), rng);
  for (size_t i : order) {
    ASSERT_EQ(codes[i]->add_to(w.ctx, w.alpha), GRCORE_OK);
  }
  EXPECT_EQ(grcore_code_registered_count(w.ctx), codes.size());
  for (auto & c : codes) {
    GRCORE_CodeRange r;
    ASSERT_TRUE(grcore_code_lookup(w.ctx, c->start(), &r));
    EXPECT_EQ(r.code, c->code);
    ASSERT_TRUE(grcore_code_lookup(w.ctx, c->start() + c->size() - 1, &r));
    EXPECT_EQ(r.code, c->code);
    ASSERT_TRUE(grcore_code_lookup(w.ctx, c->at(5), &r));
    EXPECT_EQ(r.code, c->code);
  }
  // Unregister every other one: the rest are still found, the others miss.
  for (size_t i = 0; i < codes.size(); i += 2) {
    ASSERT_EQ(grcore_code_unregister(w.ctx, codes[i]->start()), GRCORE_OK);
  }
  for (size_t i = 0; i < codes.size(); i++) {
    GRCORE_CodeRange r;
    EXPECT_EQ(grcore_code_lookup(w.ctx, codes[i]->at(1), &r), i % 2 == 1) << i;
  }
}

TEST(Registry, ARefusedAllocationLeavesTheRegistryAsItWas) {
  TrackingAllocator alloc;
  {
    RWorld w(alloc.get());
    HandCode second;
    ASSERT_EQ(w.code.add_to(w.ctx, w.alpha), GRCORE_OK);
    size_t refs = grcore_code_refcount(second.code);
    bool refused = false, succeeded = false;
    for (long n = 1; n < 20 && !succeeded; n++) {
      alloc.fail_at = alloc.calls + n;
      GRCORE_Result r = second.add_to(w.ctx, w.alpha);
      alloc.fail_at = 0;
      if (r == GRCORE_OK) {
        succeeded = true;
        break;
      }
      refused = true;
      EXPECT_TRUE(r == GRCORE_ERR_OOM || r == GRCORE_ERR_LIMIT);
      EXPECT_EQ(grcore_code_registered_count(w.ctx), 1u);
      EXPECT_EQ(grcore_code_refcount(second.code), refs);
    }
    // The first registration already grew the array, so a second may need no
    // allocation at all: the sweep is on the path, not on this particular count.
    EXPECT_TRUE(refused || succeeded);
    GRCORE_EntrySlot * slot = nullptr;
    bool slot_refused = false;
    for (long n = 1; n < 20; n++) {
      alloc.fail_at = alloc.calls + n;
      GRCORE_Result r = grcore_entry_slot_create(w.ctx, &slot);
      alloc.fail_at = 0;
      if (r == GRCORE_OK) {
        break;
      }
      slot_refused = true;
      EXPECT_EQ(slot, nullptr);
    }
    EXPECT_TRUE(slot_refused);
    ASSERT_NE(slot, nullptr);
    // A slot that cannot get its node is left as it was.
    alloc.fail_at = alloc.calls + 1;
    EXPECT_EQ(grcore_entry_slot_set(w.ctx, slot, w.code.code, w.code.at(0)),
        GRCORE_ERR_OOM);
    alloc.fail_at = 0;
    EXPECT_EQ(slot->entry, 0u);
    EXPECT_EQ(grcore_entry_slot_code(slot), nullptr);
  }
  EXPECT_EQ(alloc.live, 0) << "everything the registry allocated is freed at destroy";
}

/* ---- Entry slots -------------------------------------------------------- */

TEST(EntrySlot, AWordCompiledCodeCanLoadThatStaysPutAndZeroMeansNone) {
  RWorld w;
  std::vector<GRCORE_EntrySlot *> slots;
  for (int i = 0; i < 200; i++) {
    GRCORE_EntrySlot * s = nullptr;
    ASSERT_EQ(grcore_entry_slot_create(w.ctx, &s), GRCORE_OK);
    EXPECT_EQ(s->entry, 0u);
    slots.push_back(s);
  }
  // The first slot's address is the same after two hundred more were made.
  GRCORE_EntrySlot * first = slots[0];
  uintptr_t * word = &first->entry;
  ASSERT_EQ(grcore_entry_slot_set(w.ctx, first, w.code.code, w.code.at(3)), GRCORE_OK);
  EXPECT_EQ(*word, w.code.at(3));
  EXPECT_EQ(grcore_entry_slot_code(first), w.code.code);
  EXPECT_EQ(grcore_code_refcount(w.code.code), 2u);
  ASSERT_EQ(grcore_entry_slot_clear(w.ctx, first), GRCORE_OK);
  EXPECT_EQ(*word, 0u);
  EXPECT_EQ(grcore_entry_slot_code(first), nullptr);
  // Clearing an empty slot is fine; the creator's reference is all that is left.
  EXPECT_EQ(grcore_entry_slot_clear(w.ctx, first), GRCORE_OK);
  EXPECT_EQ(grcore_code_refcount(w.code.code), 1u);
  EXPECT_EQ(w.code.released, 0);
}

TEST(EntrySlot, ArgumentsAreCheckedAndAnotherContextsSlotIsRefused) {
  RWorld w, other;
  GRCORE_EntrySlot * mine = nullptr, * theirs = nullptr;
  ASSERT_EQ(grcore_entry_slot_create(w.ctx, &mine), GRCORE_OK);
  ASSERT_EQ(grcore_entry_slot_create(other.ctx, &theirs), GRCORE_OK);
  EXPECT_EQ(grcore_entry_slot_create(w.ctx, nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_entry_slot_set(w.ctx, mine, nullptr, 8), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_entry_slot_set(w.ctx, mine, w.code.code, 0), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_entry_slot_set(w.ctx, nullptr, w.code.code, 8), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_entry_slot_set(w.ctx, theirs, w.code.code, 8), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_entry_slot_clear(w.ctx, theirs), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_entry_slot_clear(nullptr, mine), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_entry_slot_code(nullptr), nullptr);
  EXPECT_EQ(grcore_code_refcount(w.code.code), 1u);
  std::thread([&] {
    EXPECT_EQ(grcore_entry_slot_set(w.ctx, mine, w.code.code, 8), GRCORE_ERR_INVALID);
  }).join();
  EXPECT_EQ(mine->entry, 0u);
}

TEST(EntrySlot, ReplacingCodeRetiresTheOldReferenceAndTakesANewOne) {
  RWorld w;
  HandCode second;
  GRCORE_EntrySlot * s = nullptr;
  ASSERT_EQ(grcore_entry_slot_create(w.ctx, &s), GRCORE_OK);
  ASSERT_EQ(grcore_entry_slot_set(w.ctx, s, w.code.code, w.code.at(0)), GRCORE_OK);
  w.code.give_up();
  // No JIT record: the replaced code is released at once.
  ASSERT_EQ(grcore_entry_slot_set(w.ctx, s, second.code, second.at(0)), GRCORE_OK);
  second.give_up();
  EXPECT_EQ(w.code.released, 1);
  EXPECT_EQ(second.released, 0);
  EXPECT_EQ(s->entry, second.at(0));
  // With one open, it waits.
  HandCode third;
  GRCORE_ActivationRef jit = enter(w.stack, GRCORE_ACTIVATION_JIT);
  ASSERT_EQ(grcore_entry_slot_set(w.ctx, s, third.code, third.at(0)), GRCORE_OK);
  third.give_up();
  EXPECT_EQ(second.released, 0);
  EXPECT_EQ(grcore_code_retired_count(w.ctx), 1u);
  ASSERT_EQ(grcore_activation_leave(w.stack, jit), GRCORE_OK);
  EXPECT_EQ(second.released, 1);
  EXPECT_EQ(third.released, 0);
  EXPECT_EQ(grcore_code_retired_count(w.ctx), 0u);
}

/* ---- Code lifetime ------------------------------------------------------ */

TEST(Retire, ASlotClearedUnderALiveJitRecordIsReleasedWhenTheLastOneLeavesAndNotBefore) {
  RWorld w;
  GRCORE_EntrySlot * s = nullptr;
  ASSERT_EQ(grcore_entry_slot_create(w.ctx, &s), GRCORE_OK);
  ASSERT_EQ(grcore_entry_slot_set(w.ctx, s, w.code.code, w.code.at(0)), GRCORE_OK);
  w.code.give_up();
  GRCORE_ActivationRef outer = enter(w.stack, GRCORE_ACTIVATION_JIT);
  GRCORE_ActivationRef native = enter(w.stack, GRCORE_ACTIVATION_NATIVE);
  GRCORE_ActivationRef inner = enter(w.stack, GRCORE_ACTIVATION_JIT);
  ASSERT_EQ(grcore_entry_slot_clear(w.ctx, s), GRCORE_OK);
  EXPECT_EQ(s->entry, 0u) << "no later call goes through a cleared slot";
  EXPECT_EQ(w.code.released, 0);
  EXPECT_EQ(grcore_code_retired_count(w.ctx), 1u);
  // A record that is not a JIT one is not a compiled frame: leaving it frees nothing.
  ASSERT_EQ(grcore_activation_leave(w.stack, inner), GRCORE_OK);
  EXPECT_EQ(w.code.released, 0) << "one JIT record is still open";
  ASSERT_EQ(grcore_activation_leave(w.stack, native), GRCORE_OK);
  EXPECT_EQ(w.code.released, 0);
  ASSERT_EQ(grcore_activation_leave(w.stack, outer), GRCORE_OK);
  EXPECT_EQ(w.code.released, 1) << "released by the last JIT record leaving";
  EXPECT_EQ(grcore_code_retired_count(w.ctx), 0u);
}

TEST(Retire, ClearingWithNoJitRecordLiveReleasesAtOnceEvenUnderOtherRecords) {
  RWorld w;
  GRCORE_EntrySlot * s = nullptr;
  ASSERT_EQ(grcore_entry_slot_create(w.ctx, &s), GRCORE_OK);
  ASSERT_EQ(grcore_entry_slot_set(w.ctx, s, w.code.code, w.code.at(0)), GRCORE_OK);
  w.code.give_up();
  GRCORE_ActivationRef host = enter(w.stack, GRCORE_ACTIVATION_HOST);
  GRCORE_ActivationRef interp = enter(w.stack, GRCORE_ACTIVATION_INTERPRETER);
  ASSERT_EQ(grcore_entry_slot_clear(w.ctx, s), GRCORE_OK);
  EXPECT_EQ(w.code.released, 1);
  EXPECT_EQ(grcore_code_retired_count(w.ctx), 0u);
  grcore_activation_leave(w.stack, interp);
  grcore_activation_leave(w.stack, host);
}

TEST(Retire, ClearingASlotAllocatesNothingSoItCannotFailForWantOfMemory) {
  TrackingAllocator alloc;
  RWorld w(alloc.get());
  GRCORE_EntrySlot * s = nullptr;
  ASSERT_EQ(grcore_entry_slot_create(w.ctx, &s), GRCORE_OK);
  ASSERT_EQ(grcore_entry_slot_set(w.ctx, s, w.code.code, w.code.at(0)), GRCORE_OK);
  w.code.give_up();
  GRCORE_ActivationRef jit = enter(w.stack, GRCORE_ACTIVATION_JIT);
  long calls = alloc.calls;
  alloc.fail_at = alloc.calls + 1; // the first allocation from here would fail
  EXPECT_EQ(grcore_entry_slot_clear(w.ctx, s), GRCORE_OK);
  EXPECT_EQ(alloc.calls, calls);
  alloc.fail_at = 0;
  EXPECT_EQ(grcore_activation_leave(w.stack, jit), GRCORE_OK);
  EXPECT_EQ(w.code.released, 1);
}

TEST(Retire, CodeNamedByARangeAndASlotGoesOnlyWhenBothHaveLetGo) {
  RWorld w;
  ASSERT_EQ(w.code.add_to(w.ctx, w.alpha), GRCORE_OK);
  GRCORE_EntrySlot * s = nullptr;
  ASSERT_EQ(grcore_entry_slot_create(w.ctx, &s), GRCORE_OK);
  ASSERT_EQ(grcore_entry_slot_set(w.ctx, s, w.code.code, w.code.at(0)), GRCORE_OK);
  w.code.give_up();
  GRCORE_ActivationRef jit = enter(w.stack, GRCORE_ACTIVATION_JIT);
  ASSERT_EQ(grcore_entry_slot_clear(w.ctx, s), GRCORE_OK);
  ASSERT_EQ(grcore_activation_leave(w.stack, jit), GRCORE_OK);
  EXPECT_EQ(w.code.released, 0) << "the registry still names it";
  ASSERT_EQ(grcore_code_unregister(w.ctx, w.code.start()), GRCORE_OK);
  EXPECT_EQ(w.code.released, 1);
}

TEST(Retire, AnUnwindThatDropsTheJitRecordReleasesWhatWaitedOnIt) {
  RWorld w;
  GRCORE_EntrySlot * s = nullptr;
  ASSERT_EQ(grcore_entry_slot_create(w.ctx, &s), GRCORE_OK);
  ASSERT_EQ(grcore_entry_slot_set(w.ctx, s, w.code.code, w.code.at(0)), GRCORE_OK);
  w.code.give_up();
  GRCORE_ActivationRef host = enter(w.stack, GRCORE_ACTIVATION_HOST);
  enter(w.stack, GRCORE_ACTIVATION_JIT);
  enter(w.stack, GRCORE_ACTIVATION_NATIVE);
  ASSERT_EQ(grcore_entry_slot_clear(w.ctx, s), GRCORE_OK);
  EXPECT_EQ(w.code.released, 0);
  size_t popped = 0;
  ASSERT_EQ(grcore_unwind_to_activation(w.stack, host, &popped), GRCORE_OK);
  EXPECT_EQ(grcore_activation_count(w.stack), 1u);
  EXPECT_EQ(w.code.released, 1);
}

TEST(Retire, DestroyingTheContextReleasesEverythingWhateverIsOpen) {
  TrackingAllocator alloc;
  HandCode retired_slot, retired_range, live_range, live_slot;
  {
    RWorld w(alloc.get());
    HandCode & a = retired_slot;
    HandCode & b = retired_range;
    HandCode & c = live_range;
    HandCode & d = live_slot;
    ASSERT_EQ(b.add_to(w.ctx, w.alpha), GRCORE_OK);
    ASSERT_EQ(c.add_to(w.ctx, w.alpha), GRCORE_OK);
    GRCORE_EntrySlot * s1 = nullptr, * s2 = nullptr;
    ASSERT_EQ(grcore_entry_slot_create(w.ctx, &s1), GRCORE_OK);
    ASSERT_EQ(grcore_entry_slot_create(w.ctx, &s2), GRCORE_OK);
    ASSERT_EQ(grcore_entry_slot_set(w.ctx, s1, a.code, a.at(0)), GRCORE_OK);
    ASSERT_EQ(grcore_entry_slot_set(w.ctx, s2, d.code, d.at(0)), GRCORE_OK);
    for (HandCode * h : {&a, &b, &c, &d}) {
      h->give_up();
    }
    enter(w.stack, GRCORE_ACTIVATION_JIT); // still open when the context goes
    ASSERT_EQ(grcore_entry_slot_clear(w.ctx, s1), GRCORE_OK);
    ASSERT_EQ(grcore_code_unregister(w.ctx, b.start()), GRCORE_OK);
    EXPECT_EQ(a.released + b.released + c.released + d.released, 0);
  }
  EXPECT_EQ(retired_slot.released, 1);
  EXPECT_EQ(retired_range.released, 1);
  EXPECT_EQ(live_range.released, 1);
  EXPECT_EQ(live_slot.released, 1);
  EXPECT_EQ(alloc.live, 0);
}

TEST(Retire, AnotherThreadsReferenceMeansTheLastDropIsTheOneThatReleases) {
  RWorld w;
  GRCORE_EntrySlot * s = nullptr;
  ASSERT_EQ(grcore_entry_slot_create(w.ctx, &s), GRCORE_OK);
  ASSERT_EQ(grcore_entry_slot_set(w.ctx, s, w.code.code, w.code.at(0)), GRCORE_OK);
  GRCORE_Code * extra = grcore_code_retain(w.code.code);
  w.code.give_up();
  std::atomic<bool> go{false};
  std::thread other([&] {
    while (!go.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    grcore_code_release(extra); // the last one: runs the release here
  });
  GRCORE_ActivationRef jit = enter(w.stack, GRCORE_ACTIVATION_JIT);
  ASSERT_EQ(grcore_entry_slot_clear(w.ctx, s), GRCORE_OK);
  ASSERT_EQ(grcore_activation_leave(w.stack, jit), GRCORE_OK);
  EXPECT_EQ(w.code.released, 0) << "the other thread still holds one";
  go.store(true, std::memory_order_release);
  other.join();
  EXPECT_EQ(w.code.released, 1);
}

TEST(Retire, AContextHandedToAnotherThreadReleasesItsRetiredCodeThere) {
  RWorld w;
  GRCORE_EntrySlot * s = nullptr;
  ASSERT_EQ(grcore_entry_slot_create(w.ctx, &s), GRCORE_OK);
  ASSERT_EQ(grcore_entry_slot_set(w.ctx, s, w.code.code, w.code.at(0)), GRCORE_OK);
  w.code.give_up();
  GRCORE_ActivationRef jit = enter(w.stack, GRCORE_ACTIVATION_JIT);
  ASSERT_EQ(grcore_entry_slot_clear(w.ctx, s), GRCORE_OK);
  ASSERT_EQ(grcore_context_release(w.ctx), GRCORE_OK);
  std::thread other([&] {
    while (grcore_context_acquire(w.ctx) != GRCORE_OK) {
      std::this_thread::yield();
    }
    EXPECT_EQ(w.code.released, 0);
    EXPECT_EQ(grcore_activation_leave(w.stack, jit), GRCORE_OK);
    EXPECT_EQ(w.code.released, 1);
    EXPECT_EQ(grcore_context_release(w.ctx), GRCORE_OK);
  });
  other.join();
  ASSERT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
  EXPECT_EQ(w.code.released, 1);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

/**
 * @file
 *
 * An engine descriptor states its own size (a/engine.h), so the struct can grow
 * at the end. The cases are those of test_key_size.cpp, for the hooks a
 * descriptor gained after the first generation: `roots` and `unwind`.
 *
 * Every case is a descriptor as an older or a newer header would have built
 * it, and each has a control: the same descriptor at its full size, which must
 * behave the other way, so that a test that passes because the hook was never
 * wired cannot pass. The hooks that must not be read are tripwires that count
 * their calls; the older-layout copies are exactly as large as their size
 * says, on the heap, so a read past the end is a finding under ASan or
 * Valgrind and not a silent zero.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace {

int g_roots_calls = 0;
int g_unwind_calls = 0;

GRCORE_SlotKind value_slot(const GRCORE_AbstractFrame *, size_t) {
  return GRCORE_SLOT_VALUE;
}
void tripwire_roots(GRCORE_Context *, const GRCORE_AbstractFrame *,
    const GRCORE_RootVisitor *) {
  g_roots_calls++;
}
void tripwire_unwind(GRCORE_Context *, const GRCORE_AbstractFrame *) {
  g_unwind_calls++;
}

void reset_counts() { g_roots_calls = g_unwind_calls = 0; }

// The descriptor as the first generation had it: everything through
// `decoder`, and nothing after. It is a copy of the layout, not the real
// struct, so that the bytes after it really are not the test's.
struct OldDescriptor {
  size_t size;
  const char * name;
  GRCORE_SlotKind (*slot_kind)(const GRCORE_AbstractFrame *, size_t);
  GRCORE_Location (*locate)(const GRCORE_Context *, uint64_t, uint64_t);
  size_t (*inspect)(
      const GRCORE_Context *, GRCORE_SlotKind, uint64_t, char *, size_t);
  GRCORE_ScopeInterface scopes;
  GRCORE_ConservativeDecoder decoder;
};
static_assert(sizeof(OldDescriptor) == GRCORE_ENGINE_DESCRIPTOR_MIN_SIZE,
    "the old layout is the minimum layout");
static_assert(offsetof(OldDescriptor, decoder) ==
        offsetof(GRCORE_EngineDescriptor, decoder),
    "the old layout is a prefix of the current one");
static_assert(offsetof(OldDescriptor, name) ==
        offsetof(GRCORE_EngineDescriptor, name),
    "prefix");

// A descriptor whose tail is tripwires, built at full size: the control.
GRCORE_EngineDescriptor tripwire_descriptor(const char * name) {
  return GRCORE_ENGINE_DESCRIPTOR_INIT(name, value_slot, nullptr, nullptr,
      GRCORE_ScopeInterface{nullptr, nullptr, nullptr},
      GRCORE_ConservativeDecoder{0, 0, 0}, tripwire_roots, tripwire_unwind);
}

// An exactly-sized heap block holding an OldDescriptor. Reading one byte past
// it is a heap overflow to ASan and an invalid read to Valgrind.
const GRCORE_EngineDescriptor * old_descriptor_on_heap() {
  auto * d = static_cast<OldDescriptor *>(std::malloc(sizeof(OldDescriptor)));
  std::memset(d, 0, sizeof *d);
  d->size = sizeof(OldDescriptor);
  d->name = "old";
  d->slot_kind = value_slot;
  return reinterpret_cast<const GRCORE_EngineDescriptor *>(d);
}

// Pushes one frame of the engine, enumerates the context's roots, and unwinds
// the stack: the three places a descriptor's trailing hooks are read. Returns
// how many precise roots the enumeration reported.
size_t drive(RunWorld & w, GRCORE_EngineId id) {
  GRCORE_Stack * stack = grcore_context_stack(w.ctx);
  GRCORE_FrameRef f;
  EXPECT_EQ(grcore_stack_push(stack, id, 1, &f), GRCORE_OK);
  size_t slots = 0;
  GRCORE_RootVisitor v;
  v.user = &slots;
  v.slot = [](void * user, uint64_t *) { ++*static_cast<size_t *>(user); };
  v.range = nullptr;
  EXPECT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  size_t popped = 0;
  EXPECT_EQ(grcore_unwind_all(stack, &popped), GRCORE_OK);
  EXPECT_EQ(popped, 1u);
  return slots;
}

} // namespace

TEST(EngineSize, TheInitialiserWritesTheSizeOfTheStructItWasCompiledAgainst) {
  static const GRCORE_EngineDescriptor d = GRCORE_ENGINE_DESCRIPTOR_INIT("d",
      nullptr, nullptr, nullptr,
      GRCORE_ScopeInterface{nullptr, nullptr, nullptr},
      GRCORE_ConservativeDecoder{0, 0, 0}, nullptr, nullptr);
  EXPECT_EQ(d.size, sizeof(GRCORE_EngineDescriptor));
  EXPECT_GE(d.size, GRCORE_ENGINE_DESCRIPTOR_MIN_SIZE);
  EXPECT_TRUE(grcore_engine_descriptor_valid(&d));
  EXPECT_FALSE(grcore_engine_descriptor_valid(nullptr));
}

TEST(EngineSize, AnOlderShorterDescriptorRegistersAndItsAbsentHooksAreNeverRead) {
  const GRCORE_EngineDescriptor * old = old_descriptor_on_heap();
  {
    RunWorld w;
    GRCORE_EngineId id = 0;
    ASSERT_EQ(grcore_engine_register(w.ctx, old, &id), GRCORE_OK);
    // The frame's value slot is reported by the stack's own source; `roots`
    // is absent, so it reads as NULL, and `unwind` likewise on the way out.
    EXPECT_EQ(drive(w, id), 1u);
  }
  std::free(const_cast<GRCORE_EngineDescriptor *>(old));
}

TEST(EngineSize, TripwiresBeyondTheStatedSizeAreNotCalledAndAtFullSizeAre) {
  // The plain-run form of the test above: the tail is real and armed, and
  // only `size` keeps it unread. A core that ignored `size` fails here
  // without a sanitizer.
  reset_counts();
  {
    GRCORE_EngineDescriptor shortened = tripwire_descriptor("short");
    shortened.size = GRCORE_ENGINE_DESCRIPTOR_MIN_SIZE;
    RunWorld w;
    GRCORE_EngineId id = 0;
    ASSERT_EQ(grcore_engine_register(w.ctx, &shortened, &id), GRCORE_OK);
    EXPECT_EQ(drive(w, id), 1u);
    EXPECT_EQ(g_roots_calls, 0);
    EXPECT_EQ(g_unwind_calls, 0);
  }
  // Control: the same bytes at full size are read.
  reset_counts();
  {
    GRCORE_EngineDescriptor whole = tripwire_descriptor("whole");
    RunWorld w;
    GRCORE_EngineId id = 0;
    ASSERT_EQ(grcore_engine_register(w.ctx, &whole, &id), GRCORE_OK);
    EXPECT_EQ(drive(w, id), 1u);
    EXPECT_EQ(g_roots_calls, 1);
    EXPECT_EQ(g_unwind_calls, 1);
  }
}

TEST(EngineSize, EachTrailingMemberIsReadOnlyWhereTheSizeReachesItsEnd) {
  struct Cut {
    size_t size;
    int roots;
    int unwinds;
  };
  const Cut cuts[] = {
      {offsetof(GRCORE_EngineDescriptor, roots), 0, 0},
      {offsetof(GRCORE_EngineDescriptor, roots) + sizeof(void *), 1, 0},
      {sizeof(GRCORE_EngineDescriptor), 1, 1},
  };
  for (const Cut & cut : cuts) {
    reset_counts();
    GRCORE_EngineDescriptor d = tripwire_descriptor("cut");
    d.size = cut.size;
    RunWorld w;
    GRCORE_EngineId id = 0;
    ASSERT_EQ(grcore_engine_register(w.ctx, &d, &id), GRCORE_OK)
        << "size " << cut.size;
    drive(w, id);
    EXPECT_EQ(g_roots_calls, cut.roots) << "size " << cut.size;
    EXPECT_EQ(g_unwind_calls, cut.unwinds) << "size " << cut.size;
  }
}

TEST(EngineSize, ASizeBelowTheMinimumOrOffAlignmentIsRefusedNotRead) {
  const size_t bad[] = {0, 1, GRCORE_ENGINE_DESCRIPTOR_MIN_SIZE - sizeof(void *),
      GRCORE_ENGINE_DESCRIPTOR_MIN_SIZE - 1,
      GRCORE_ENGINE_DESCRIPTOR_MIN_SIZE + 1, sizeof(GRCORE_EngineDescriptor) + 1,
      sizeof(GRCORE_EngineDescriptor) + sizeof(void *) / 2};
  for (size_t size : bad) {
    GRCORE_EngineDescriptor d = tripwire_descriptor("bad");
    d.size = size;
    EXPECT_FALSE(grcore_engine_descriptor_valid(&d)) << "size " << size;
    RunWorld w;
    GRCORE_EngineId id = 0;
    EXPECT_EQ(grcore_engine_register(w.ctx, &d, &id), GRCORE_ERR_INVALID)
        << "size " << size;
    EXPECT_EQ(id, 0u);
    EXPECT_EQ(grcore_engine_count(w.ctx), 0u);
  }
  // Controls: the smallest size and the full size are accepted, so the list
  // above is refused for its size and not for anything else about it.
  for (size_t size :
      {GRCORE_ENGINE_DESCRIPTOR_MIN_SIZE, sizeof(GRCORE_EngineDescriptor)}) {
    GRCORE_EngineDescriptor d = tripwire_descriptor("ok");
    d.size = size;
    RunWorld w;
    GRCORE_EngineId id = 0;
    EXPECT_EQ(grcore_engine_register(w.ctx, &d, &id), GRCORE_OK)
        << "size " << size;
  }
}

TEST(EngineSize, ADescriptorFromANewerHeaderIsAcceptedAndItsUnknownTailIgnored) {
  // Decision: accepted, as for a key. A member this library does not know is
  // one whose absence is a defined degradation (a hook not run), so refusing
  // the descriptor would only make a newer engine unusable on an older core.
  reset_counts();
  const size_t extra = 2 * sizeof(void *);
  auto * block = static_cast<unsigned char *>(
      std::malloc(sizeof(GRCORE_EngineDescriptor) + extra));
  GRCORE_EngineDescriptor proto = tripwire_descriptor("newer");
  std::memcpy(block, &proto, sizeof proto);
  // The unknown members hold garbage that would crash if called or followed.
  std::memset(block + sizeof(GRCORE_EngineDescriptor), 0xA5, extra);
  auto * d = reinterpret_cast<GRCORE_EngineDescriptor *>(block);
  d->size = sizeof(GRCORE_EngineDescriptor) + extra;
  EXPECT_TRUE(grcore_engine_descriptor_valid(d));
  {
    RunWorld w;
    GRCORE_EngineId id = 0;
    ASSERT_EQ(grcore_engine_register(w.ctx, d, &id), GRCORE_OK);
    EXPECT_EQ(drive(w, id), 1u);
    EXPECT_EQ(g_roots_calls, 1) << "the members it knows are used";
    EXPECT_EQ(g_unwind_calls, 1);
  }
  std::free(block);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

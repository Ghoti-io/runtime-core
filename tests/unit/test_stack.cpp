/**
 * @file
 *
 * The guest stack: frames, links, growth, budgets, poll identity and
 * migration.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../../src/a/guest_internal.h"

#include <cstring>
#include <thread>
#include <vector>

namespace {

GRCORE_FrameRef push_ok(GRCORE_Stack * s, GRCORE_EngineId e, size_t slots) {
  GRCORE_FrameRef f = {0};
  EXPECT_EQ(grcore_stack_push(s, e, slots, &f), GRCORE_OK);
  return f;
}

} // namespace

TEST(Stack, PushZeroesTheSlotsAndLinksTheCallerAndPopRestores) {
  StackWorld w;
  EXPECT_EQ(grcore_stack_top(w.stack).offset, 0u);
  EXPECT_EQ(grcore_stack_frame_count(w.stack), 0u);
  GRCORE_FrameRef f1 = push_ok(w.stack, w.alpha, 3);
  EXPECT_EQ(f1.offset, 8u); // the first frame sits at offset 8
  EXPECT_EQ(grcore_stack_top(w.stack).offset, f1.offset);
  EXPECT_EQ(grcore_stack_caller(w.stack, f1).offset, 0u);
  size_t count = 99;
  ASSERT_EQ(grcore_stack_slot_count(w.stack, f1, &count), GRCORE_OK);
  EXPECT_EQ(count, 3u);
  for (size_t i = 0; i < 3; i++) {
    uint64_t v = 1;
    ASSERT_EQ(grcore_stack_slot_get(w.stack, f1, i, &v), GRCORE_OK);
    EXPECT_EQ(v, 0u);
  }
  GRCORE_FrameRef f2 = push_ok(w.stack, w.beta, 2);
  EXPECT_EQ(f2.offset, 8u + 40u + 3u * 8u);
  EXPECT_EQ(grcore_stack_top(w.stack).offset, f2.offset);
  EXPECT_EQ(grcore_stack_caller(w.stack, f2).offset, f1.offset);
  EXPECT_EQ(grcore_stack_frame_count(w.stack), 2u);
  EXPECT_EQ(grcore_stack_bytes_used(w.stack), 40u + 24u + 40u + 16u);
  EXPECT_EQ(grcore_context_depth(w.ctx, GRCORE_DEPTH_GUEST), 2u);
  GRCORE_EngineId e = 0;
  ASSERT_EQ(grcore_stack_engine(w.stack, f1, &e), GRCORE_OK);
  EXPECT_EQ(e, w.alpha);
  ASSERT_EQ(grcore_stack_engine(w.stack, f2, &e), GRCORE_OK);
  EXPECT_EQ(e, w.beta);

  ASSERT_EQ(grcore_stack_pop(w.stack), GRCORE_OK);
  EXPECT_EQ(grcore_stack_top(w.stack).offset, f1.offset);
  EXPECT_EQ(grcore_context_depth(w.ctx, GRCORE_DEPTH_GUEST), 1u);
  EXPECT_FALSE(grcore_stack_frame_valid(w.stack, f2));
  ASSERT_EQ(grcore_stack_pop(w.stack), GRCORE_OK);
  EXPECT_EQ(grcore_stack_top(w.stack).offset, 0u);
  EXPECT_EQ(grcore_stack_frame_count(w.stack), 0u);
  EXPECT_EQ(grcore_stack_bytes_used(w.stack), 0u);
  EXPECT_EQ(grcore_context_depth(w.ctx, GRCORE_DEPTH_GUEST), 0u);
  EXPECT_EQ(grcore_stack_pop(w.stack), GRCORE_ERR_INVALID); // nothing to pop
}

TEST(Stack, AReusedOffsetIsZeroedAgain) {
  StackWorld w;
  GRCORE_FrameRef f = push_ok(w.stack, w.alpha, 2);
  ASSERT_EQ(grcore_stack_slot_set(w.stack, f, 1, 0xDEAD), GRCORE_OK);
  ASSERT_EQ(grcore_stack_pop(w.stack), GRCORE_OK);
  GRCORE_FrameRef g = push_ok(w.stack, w.alpha, 2);
  EXPECT_EQ(g.offset, f.offset);
  uint64_t v = 1;
  ASSERT_EQ(grcore_stack_slot_get(w.stack, g, 1, &v), GRCORE_OK);
  EXPECT_EQ(v, 0u);
}

TEST(Stack, AFrameWithNoSlotsIsAFrame) {
  StackWorld w;
  GRCORE_FrameRef f = push_ok(w.stack, w.alpha, 0);
  size_t count = 9;
  ASSERT_EQ(grcore_stack_slot_count(w.stack, f, &count), GRCORE_OK);
  EXPECT_EQ(count, 0u);
  EXPECT_EQ(grcore_stack_slots(w.stack, f), nullptr);
  uint64_t v;
  EXPECT_EQ(grcore_stack_slot_get(w.stack, f, 0, &v), GRCORE_ERR_INVALID);
  GRCORE_FrameRef g = push_ok(w.stack, w.alpha, 1);
  EXPECT_EQ(grcore_stack_caller(w.stack, g).offset, f.offset);
  EXPECT_EQ(grcore_stack_pop(w.stack), GRCORE_OK);
  EXPECT_EQ(grcore_stack_pop(w.stack), GRCORE_OK);
}

TEST(Stack, PushRefusalsLeaveTheDepthAndTheStackUnchanged) {
  StackWorld w;
  GRCORE_FrameRef f = {1234};
  EXPECT_EQ(grcore_stack_push(w.stack, 0, 1, &f), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_stack_push(w.stack, 3, 1, &f), GRCORE_ERR_INVALID); // unregistered
  EXPECT_EQ(grcore_stack_push(nullptr, w.alpha, 1, &f), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_stack_push(w.stack, w.alpha, 1, nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_stack_push(w.stack, w.alpha, GRCORE_FRAME_MAX_SLOTS + 1, &f),
      GRCORE_ERR_INVALID);
  GRCORE_Result other = GRCORE_OK;
  std::thread([&] { other = grcore_stack_push(w.stack, w.alpha, 1, &f); }).join();
  EXPECT_EQ(other, GRCORE_ERR_INVALID);
  EXPECT_EQ(f.offset, 1234u); // never written on a refusal
  EXPECT_EQ(grcore_context_depth(w.ctx, GRCORE_DEPTH_GUEST), 0u);
  EXPECT_EQ(grcore_stack_frame_count(w.stack), 0u);
  EXPECT_EQ(grcore_stack_bytes_capacity(w.stack), 0u); // no buffer was made
}

TEST(Stack, ThereIsNoStackBeforeAnEngineIsRegistered) {
  RunWorld w;
  GRCORE_Stack * none = grcore_context_stack(w.ctx);
  EXPECT_EQ(none, nullptr);
  GRCORE_FrameRef f = {5};
  EXPECT_EQ(grcore_stack_push(none, 1, 1, &f), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_stack_pop(none), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_stack_top(none).offset, 0u);
  EXPECT_EQ(grcore_stack_frame_count(none), 0u);
  EXPECT_EQ(grcore_stack_bytes_used(none), 0u);
  EXPECT_EQ(grcore_stack_bytes_capacity(none), 0u);
  EXPECT_EQ(grcore_stack_move_count(none), 0u);
  EXPECT_FALSE(grcore_stack_frame_valid(none, f));
  EXPECT_EQ(grcore_stack_reserve(none, 10), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_stack_set_always_move(none, true), GRCORE_ERR_INVALID);
}

TEST(Stack, GrowthMovesTheBufferAndKeepsOffsetsSlotsAndLinks) {
  StackWorld w;
  std::vector<GRCORE_FrameRef> frames;
  GRCORE_FrameRef first = push_ok(w.stack, w.alpha, 6);
  frames.push_back(first);
  ASSERT_EQ(grcore_stack_slot_set(w.stack, first, 5, 555), GRCORE_OK);
  size_t capacity = grcore_stack_bytes_capacity(w.stack);
  EXPECT_EQ(capacity, GRCORE_STACK_INITIAL_BYTES);
  EXPECT_EQ(grcore_stack_move_count(w.stack), 0u);
  uint64_t * before = grcore_stack_slots(w.stack, first);
  ASSERT_NE(before, nullptr);
  // Push until the first buffer is full.
  while (grcore_stack_move_count(w.stack) == 0) {
    GRCORE_FrameRef f = push_ok(w.stack, w.alpha, 6);
    ASSERT_EQ(grcore_stack_slot_set(w.stack, f, 0, frames.size()), GRCORE_OK);
    frames.push_back(f);
    ASSERT_LT(frames.size(), 100u);
  }
  EXPECT_EQ(grcore_stack_bytes_capacity(w.stack), 2 * capacity); // it doubles
  // The old buffer was still live when the new one was made, so the address is
  // certainly different; the slot pointer saved before is not used again.
  uint64_t * after = grcore_stack_slots(w.stack, first);
  ASSERT_NE(after, nullptr);
  EXPECT_NE(after, before);
  uint64_t v = 0;
  ASSERT_EQ(grcore_stack_slot_get(w.stack, first, 5, &v), GRCORE_OK);
  EXPECT_EQ(v, 555u);
  for (size_t i = 1; i < frames.size(); i++) {
    EXPECT_TRUE(grcore_stack_frame_valid(w.stack, frames[i]));
    EXPECT_EQ(grcore_stack_caller(w.stack, frames[i]).offset, frames[i - 1].offset);
    ASSERT_EQ(grcore_stack_slot_get(w.stack, frames[i], 0, &v), GRCORE_OK);
    EXPECT_EQ(v, i);
  }
  EXPECT_EQ(grcore_stack_frame_count(w.stack), frames.size());
}

TEST(Stack, AnOddlySizedFrameLargerThanADoublingGrowsToFitIt) {
  StackWorld w;
  push_ok(w.stack, w.alpha, 1);
  GRCORE_FrameRef big = push_ok(w.stack, w.alpha, 1000);
  EXPECT_GE(grcore_stack_bytes_capacity(w.stack), 40u + 8000u);
  ASSERT_EQ(grcore_stack_slot_set(w.stack, big, 999, 77), GRCORE_OK);
  uint64_t v = 0;
  ASSERT_EQ(grcore_stack_slot_get(w.stack, big, 999, &v), GRCORE_OK);
  EXPECT_EQ(v, 77u);
}

TEST(Stack, AlwaysMoveModeChangesTheBufferOnEveryPush) {
  StackWorld w;
  ASSERT_EQ(grcore_stack_set_always_move(w.stack, true), GRCORE_OK);
  std::vector<GRCORE_FrameRef> frames;
  GRCORE_FrameRef f = push_ok(w.stack, w.alpha, 2);
  frames.push_back(f);
  ASSERT_EQ(grcore_stack_slot_set(w.stack, f, 1, 100), GRCORE_OK);
  uint64_t moves = grcore_stack_move_count(w.stack);
  for (int i = 0; i < 20; i++) {
    // Take the address, push, and read the address again. The saved one is
    // never dereferenced; only compared as a number.
    uintptr_t saved =
        reinterpret_cast<uintptr_t>(grcore_stack_slots(w.stack, frames[0]));
    GRCORE_FrameRef g = push_ok(w.stack, w.alpha, 2);
    ASSERT_EQ(grcore_stack_slot_set(w.stack, g, 1, 200 + i), GRCORE_OK);
    frames.push_back(g);
    uintptr_t now =
        reinterpret_cast<uintptr_t>(grcore_stack_slots(w.stack, frames[0]));
    EXPECT_NE(saved, now) << "push " << i;
    EXPECT_EQ(grcore_stack_move_count(w.stack), moves + i + 1);
  }
  // Every read is still correct after twenty moves.
  uint64_t v = 0;
  ASSERT_EQ(grcore_stack_slot_get(w.stack, frames[0], 1, &v), GRCORE_OK);
  EXPECT_EQ(v, 100u);
  for (size_t i = 1; i < frames.size(); i++) {
    ASSERT_EQ(grcore_stack_slot_get(w.stack, frames[i], 1, &v), GRCORE_OK);
    EXPECT_EQ(v, 199u + i);
    EXPECT_EQ(grcore_stack_caller(w.stack, frames[i]).offset, frames[i - 1].offset);
  }
  // Turning it off stops the moves.
  ASSERT_EQ(grcore_stack_set_always_move(w.stack, false), GRCORE_OK);
  moves = grcore_stack_move_count(w.stack);
  push_ok(w.stack, w.alpha, 1);
  EXPECT_EQ(grcore_stack_move_count(w.stack), moves);
}

TEST(Stack, ReserveMakesRoomSoPushesThatFitDoNotMove) {
  StackWorld w;
  GRCORE_FrameRef f = push_ok(w.stack, w.alpha, 2);
  ASSERT_EQ(grcore_stack_reserve(w.stack, 10000), GRCORE_OK);
  EXPECT_GE(grcore_stack_bytes_capacity(w.stack), 10000u);
  uint64_t moves = grcore_stack_move_count(w.stack);
  EXPECT_EQ(moves, 1u); // reserve is itself a move
  uint64_t * slots = grcore_stack_slots(w.stack, f);
  for (int i = 0; i < 100; i++) {
    push_ok(w.stack, w.alpha, 2);
  }
  EXPECT_EQ(grcore_stack_move_count(w.stack), moves);
  EXPECT_EQ(grcore_stack_slots(w.stack, f), slots);
  // Room already there: reserve does nothing.
  ASSERT_EQ(grcore_stack_reserve(w.stack, 10), GRCORE_OK);
  EXPECT_EQ(grcore_stack_move_count(w.stack), moves);
}

TEST(Stack, ReserveOnAnEmptyStackMakesTheFirstBuffer) {
  StackWorld w;
  ASSERT_EQ(grcore_stack_reserve(w.stack, 4000), GRCORE_OK);
  EXPECT_GE(grcore_stack_bytes_capacity(w.stack), 4000u);
  EXPECT_EQ(grcore_stack_move_count(w.stack), 0u); // nothing to move
  EXPECT_EQ(grcore_stack_bytes_used(w.stack), 0u);
  EXPECT_EQ(grcore_stack_reserve(w.stack, SIZE_MAX), GRCORE_ERR_OOM);
}

TEST(Stack, TheGuestDepthBudgetCountsFrames) {
  StackWorld w(GRCORE_UNLIMITED, GRCORE_UNLIMITED,
      GRCORE_DEFAULT_MEMORY_RESERVE, 4);
  for (int i = 0; i < 4; i++) {
    push_ok(w.stack, w.alpha, 1);
  }
  GRCORE_FrameRef f = {42};
  EXPECT_EQ(grcore_stack_push(w.stack, w.alpha, 1, &f), GRCORE_ERR_LIMIT);
  EXPECT_EQ(f.offset, 42u);
  EXPECT_EQ(grcore_context_depth(w.ctx, GRCORE_DEPTH_GUEST), 4u);
  EXPECT_EQ(grcore_stack_frame_count(w.stack), 4u);
  // A pop leaves the depth, so one more fits.
  ASSERT_EQ(grcore_stack_pop(w.stack), GRCORE_OK);
  EXPECT_EQ(grcore_context_depth(w.ctx, GRCORE_DEPTH_GUEST), 3u);
  push_ok(w.stack, w.alpha, 1);
  EXPECT_EQ(grcore_context_depth(w.ctx, GRCORE_DEPTH_GUEST), 4u);
}

TEST(Stack, TheMemoryBudgetRefusesGrowthWithLimitAndRaisingItAllowsTheRetry) {
  // Limit and reserve small: the first 512-byte buffer fits, the doubled one,
  // allocated while the old one is still live, does not.
  StackWorld w(GRCORE_UNLIMITED, 1500, 0);
  int pushed = 0;
  GRCORE_Result r = GRCORE_OK;
  GRCORE_FrameRef f;
  while (pushed < 200 && (r = grcore_stack_push(w.stack, w.alpha, 10, &f)) == GRCORE_OK) {
    pushed++;
  }
  ASSERT_EQ(r, GRCORE_ERR_LIMIT);
  ASSERT_GT(pushed, 2);
  EXPECT_LT(pushed, 200);
  EXPECT_GE(grcore_context_memory_refusals(w.ctx), 1u);
  uint64_t refusals = grcore_context_memory_refusals(w.ctx);
  // The failed push changed nothing.
  EXPECT_EQ(grcore_stack_frame_count(w.stack), static_cast<size_t>(pushed));
  EXPECT_EQ(grcore_context_depth(w.ctx, GRCORE_DEPTH_GUEST),
      static_cast<uint64_t>(pushed));
  EXPECT_EQ(grcore_stack_push(w.stack, w.alpha, 10, &f), GRCORE_ERR_LIMIT);
  EXPECT_GT(grcore_context_memory_refusals(w.ctx), refusals);
  // Raise the limit and the same push succeeds.
  ASSERT_EQ(grcore_context_set_memory_bytes(w.ctx, 100000), GRCORE_OK);
  ASSERT_EQ(grcore_stack_push(w.stack, w.alpha, 10, &f), GRCORE_OK);
  EXPECT_EQ(grcore_stack_frame_count(w.stack), static_cast<size_t>(pushed) + 1);
}

TEST(Stack, AReserveRefusedForTheBudgetIsALimitToo) {
  StackWorld w(GRCORE_UNLIMITED, 1000, 0);
  uint64_t refusals = grcore_context_memory_refusals(w.ctx);
  EXPECT_EQ(grcore_stack_reserve(w.stack, 100000), GRCORE_ERR_LIMIT);
  EXPECT_GT(grcore_context_memory_refusals(w.ctx), refusals);
  EXPECT_EQ(grcore_stack_bytes_capacity(w.stack), 0u);
}

TEST(Stack, EveryAllocationFailureOfPushIsOomAndLeaksNothing) {
  // Sweep N over the first push (which makes the buffer) and over a push that
  // must grow it.
  for (int grow = 0; grow < 2; grow++) {
    bool succeeded = false;
    int failures = 0;
    for (long n = 1; n < 20 && !succeeded; n++) {
      TrackingAllocator t;
      {
        StackWorld w(GRCORE_UNLIMITED, GRCORE_UNLIMITED,
            GRCORE_DEFAULT_MEMORY_RESERVE, GRCORE_UNLIMITED, t.get());
        if (grow) {
          push_ok(w.stack, w.alpha, 40); // 360 bytes of the 512
        }
        size_t frames = grcore_stack_frame_count(w.stack);
        size_t capacity = grcore_stack_bytes_capacity(w.stack);
        t.fail_at = t.calls + n;
        GRCORE_FrameRef f = {9};
        GRCORE_Result r = grcore_stack_push(w.stack, w.alpha, 40, &f);
        t.fail_at = 0;
        if (r == GRCORE_OK) {
          succeeded = true;
        } else {
          failures++;
          EXPECT_EQ(r, GRCORE_ERR_OOM) << "grow=" << grow << " n=" << n;
          EXPECT_EQ(f.offset, 9u);
          EXPECT_EQ(grcore_stack_frame_count(w.stack), frames);
          EXPECT_EQ(grcore_stack_bytes_capacity(w.stack), capacity);
          EXPECT_EQ(grcore_context_depth(w.ctx, GRCORE_DEPTH_GUEST), frames);
          // The stack is still usable.
          EXPECT_EQ(grcore_stack_push(w.stack, w.alpha, 40, &f), GRCORE_OK);
        }
      }
      EXPECT_EQ(t.live, 0) << "grow=" << grow << " n=" << n;
    }
    EXPECT_TRUE(succeeded);
    EXPECT_GE(failures, 1);
  }
}

TEST(Stack, SlotAccessChecksTheIndexAndTheFrame) {
  StackWorld w;
  GRCORE_FrameRef f = push_ok(w.stack, w.alpha, 3);
  ASSERT_EQ(grcore_stack_slot_set(w.stack, f, 2, 0x1122334455667788ull), GRCORE_OK);
  uint64_t v = 0;
  ASSERT_EQ(grcore_stack_slot_get(w.stack, f, 2, &v), GRCORE_OK);
  EXPECT_EQ(v, 0x1122334455667788ull);
  // Out of range: refused, and the slots next to the frame are not touched.
  GRCORE_FrameRef g = push_ok(w.stack, w.alpha, 1);
  ASSERT_EQ(grcore_stack_slot_set(w.stack, g, 0, 31337), GRCORE_OK);
  v = 5;
  EXPECT_EQ(grcore_stack_slot_get(w.stack, f, 3, &v), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_stack_slot_get(w.stack, f, SIZE_MAX, &v), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_stack_slot_set(w.stack, f, 3, 1), GRCORE_ERR_INVALID);
  EXPECT_EQ(v, 5u);
  ASSERT_EQ(grcore_stack_slot_get(w.stack, g, 0, &v), GRCORE_OK);
  EXPECT_EQ(v, 31337u);
  EXPECT_EQ(grcore_stack_slot_get(w.stack, f, 0, nullptr), GRCORE_ERR_INVALID);
  GRCORE_Result other = GRCORE_OK;
  std::thread([&] { other = grcore_stack_slot_set(w.stack, f, 0, 1); }).join();
  EXPECT_EQ(other, GRCORE_ERR_INVALID);
  std::thread([&] { other = grcore_stack_slot_get(w.stack, f, 0, &v); }).join();
  EXPECT_EQ(other, GRCORE_ERR_INVALID);
}

TEST(Stack, AForgedFrameReferenceIsRefusedEverywhere) {
  StackWorld w;
  GRCORE_FrameRef f = push_ok(w.stack, w.alpha, 4);
  GRCORE_FrameRef g = push_ok(w.stack, w.beta, 4);
  // Nothing, a number that was never an offset, the middle of a frame, the
  // middle of a slot, one past the end and far beyond.
  const size_t forged[] = {0, 1, 7, 9, f.offset + 8, f.offset + 40,
      f.offset + 44, g.offset + 8, g.offset + 40, g.offset + 72,
      grcore_stack_bytes_used(w.stack) + 8, 1u << 20, SIZE_MAX};
  for (size_t offset : forged) {
    GRCORE_FrameRef bad = {offset};
    uint64_t v = 7;
    size_t count = 7;
    GRCORE_EngineId e = 7;
    GRCORE_PollIdentity id = {7, 7};
    EXPECT_FALSE(grcore_stack_frame_valid(w.stack, bad)) << offset;
    EXPECT_EQ(grcore_stack_slot_get(w.stack, bad, 0, &v), GRCORE_ERR_INVALID) << offset;
    EXPECT_EQ(grcore_stack_slot_set(w.stack, bad, 0, 1), GRCORE_ERR_INVALID) << offset;
    EXPECT_EQ(grcore_stack_slot_count(w.stack, bad, &count), GRCORE_ERR_INVALID) << offset;
    EXPECT_EQ(grcore_stack_engine(w.stack, bad, &e), GRCORE_ERR_INVALID) << offset;
    EXPECT_EQ(grcore_stack_identity(w.stack, bad, &id), GRCORE_ERR_INVALID) << offset;
    EXPECT_EQ(grcore_stack_set_identity(w.stack, bad, id), GRCORE_ERR_INVALID) << offset;
    EXPECT_EQ(grcore_stack_slots(w.stack, bad), nullptr) << offset;
    EXPECT_EQ(grcore_stack_caller(w.stack, bad).offset, 0u) << offset;
    EXPECT_EQ(v, 7u);
    EXPECT_EQ(count, 7u);
    EXPECT_EQ(e, 7u);
    EXPECT_EQ(id.function, 7u);
  }
  // The real ones still work.
  EXPECT_TRUE(grcore_stack_frame_valid(w.stack, f));
  EXPECT_TRUE(grcore_stack_frame_valid(w.stack, g));
}

TEST(Stack, AHeaderCopiedIntoTheSlotsIsNotAFrame) {
  StackWorld w;
  GRCORE_FrameRef f = push_ok(w.stack, w.alpha, 8);
  // A real header, byte for byte, at an aligned place inside the slots: the
  // tag it carries is for another offset, so a reference there is refused.
  uint64_t * slots = grcore_stack_slots(w.stack, f);
  std::memcpy(slots, slots - 5, GRCORE_FRAME_HEADER);
  GRCORE_FrameRef inside = {f.offset + GRCORE_FRAME_HEADER};
  EXPECT_FALSE(grcore_stack_frame_valid(w.stack, inside));
}

TEST(Stack, ASavedReferenceToAPoppedFrameIsRefused) {
  StackWorld w;
  push_ok(w.stack, w.alpha, 2);
  GRCORE_FrameRef g = push_ok(w.stack, w.alpha, 2);
  ASSERT_EQ(grcore_stack_pop(w.stack), GRCORE_OK);
  uint64_t v;
  EXPECT_FALSE(grcore_stack_frame_valid(w.stack, g));
  EXPECT_EQ(grcore_stack_slot_get(w.stack, g, 0, &v), GRCORE_ERR_INVALID);
}

TEST(Stack, SlotsPointsAtTheFramesSlotsAndWritesThroughItAreReadBack) {
  StackWorld w;
  GRCORE_FrameRef f = push_ok(w.stack, w.alpha, 3);
  uint64_t * slots = grcore_stack_slots(w.stack, f);
  ASSERT_NE(slots, nullptr);
  slots[0] = 10;
  slots[2] = 30;
  uint64_t v = 0;
  ASSERT_EQ(grcore_stack_slot_get(w.stack, f, 2, &v), GRCORE_OK);
  EXPECT_EQ(v, 30u);
  ASSERT_EQ(grcore_stack_slot_set(w.stack, f, 1, 20), GRCORE_OK);
  EXPECT_EQ(slots[1], 20u);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(slots) % alignof(uint64_t), 0u);
  std::thread([&] { EXPECT_EQ(grcore_stack_slots(w.stack, f), nullptr); }).join();
}

TEST(Stack, IdentityIsZeroUntilSetAndSurvivesGrowth) {
  StackWorld w;
  GRCORE_FrameRef f = push_ok(w.stack, w.alpha, 1);
  GRCORE_PollIdentity id = {1, 1};
  ASSERT_EQ(grcore_stack_identity(w.stack, f, &id), GRCORE_OK);
  EXPECT_EQ(id.function, 0u);
  EXPECT_EQ(id.offset, 0u);
  ASSERT_EQ(grcore_stack_set_identity(w.stack, f, GRCORE_PollIdentity{12, 34}),
      GRCORE_OK);
  ASSERT_EQ(grcore_stack_set_identity(w.stack, f, GRCORE_PollIdentity{12, 35}),
      GRCORE_OK);
  ASSERT_EQ(grcore_stack_reserve(w.stack, 100000), GRCORE_OK);
  ASSERT_EQ(grcore_stack_identity(w.stack, f, &id), GRCORE_OK);
  EXPECT_EQ(id.function, 12u);
  EXPECT_EQ(id.offset, 35u);
  // The identity is a frame's own: its slots and links were not touched.
  uint64_t v = 5;
  ASSERT_EQ(grcore_stack_slot_get(w.stack, f, 0, &v), GRCORE_OK);
  EXPECT_EQ(v, 0u);
}

TEST(StackPoll, FuelExhaustionRecordsTheIdentityAndLocatesThePause) {
  StackWorld w(2);
  GRCORE_EngineId alpha = w.alpha;
  Fn fn{[&](GRCORE_Context * c) {
    GRCORE_Stack * s = grcore_context_stack(c);
    GRCORE_FrameRef f;
    EXPECT_EQ(grcore_stack_push(s, alpha, 1, &f), GRCORE_OK);
    for (uint64_t pc = 0; pc < 10; pc++) {
      grcore_context_charge_fuel(c, 1);
      GRCORE_Verdict v = grcore_stack_poll(c, 7, pc);
      if (v == GRCORE_VERDICT_PAUSE) {
        return GRCORE_STEP_PAUSED;
      }
    }
    return GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  // Fuel 2 means the third charge is over budget: the poll at pc 2 paused.
  GRCORE_Location where = grcore_context_pause_location(w.ctx);
  EXPECT_STREQ(where.file, kAlphaFile);
  EXPECT_EQ(where.line, 7 * 1000 + 2);
  GRCORE_PollIdentity id;
  ASSERT_EQ(grcore_context_poll_identity(w.ctx, &id), GRCORE_OK);
  EXPECT_EQ(id.function, 7u);
  EXPECT_EQ(id.offset, 2u);
  // The identity is in the frame itself, for the owner in any state.
  GRCORE_PollIdentity in_frame;
  ASSERT_EQ(grcore_stack_identity(
                w.stack, grcore_stack_top(w.stack), &in_frame),
      GRCORE_OK);
  EXPECT_EQ(in_frame.offset, 2u);
}

TEST(StackPoll, TheDescriptorIsNotAskedWhereAPollIsUnlessSomethingIsPending) {
  static int locates = 0;
  static const GRCORE_EngineDescriptor counting = {"counting", nullptr,
      [](const GRCORE_Context *, uint64_t, uint64_t) {
        locates++;
        return GRCORE_Location{"x", 1};
      },
      nullptr, GRCORE_ScopeInterface{nullptr, nullptr, nullptr},
      GRCORE_ConservativeDecoder{0, 0, 0}};
  RunWorld w;
  GRCORE_EngineId e;
  ASSERT_EQ(grcore_engine_register(w.ctx, &counting, &e), GRCORE_OK);
  locates = 0;
  Fn fn{[&](GRCORE_Context * c) {
    GRCORE_FrameRef f;
    grcore_stack_push(grcore_context_stack(c), e, 0, &f);
    for (int i = 0; i < 1000; i++) {
      EXPECT_EQ(grcore_stack_poll(c, 1, static_cast<uint64_t>(i)),
          GRCORE_VERDICT_CONTINUE);
    }
    return GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_EQ(locates, 0);
}

TEST(StackPoll, ThePollWorksOnTheTopFrameOfTwoEngines) {
  StackWorld w(1);
  GRCORE_EngineId beta = w.beta;
  GRCORE_EngineId alpha = w.alpha;
  Fn fn{[&](GRCORE_Context * c) {
    GRCORE_Stack * s = grcore_context_stack(c);
    GRCORE_FrameRef f;
    grcore_stack_push(s, alpha, 0, &f);
    grcore_stack_push(s, beta, 0, &f);
    grcore_context_charge_fuel(c, 5);
    return grcore_stack_poll(c, 3, 77) == GRCORE_VERDICT_PAUSE
        ? GRCORE_STEP_PAUSED
        : GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  // The top frame is beta's, so beta's locator named it.
  EXPECT_STREQ(grcore_context_pause_location(w.ctx).file, kBetaFile);
  EXPECT_EQ(grcore_context_pause_location(w.ctx).line, 77);
}

TEST(StackPoll, WithoutFramesItPollsLikeGrcorePollWithNoLocationAndNoIdentity) {
  for (int engines = 0; engines < 2; engines++) {
    RunWorld w(0);
    if (engines) {
      GRCORE_EngineId e;
      ASSERT_EQ(grcore_engine_register(w.ctx, &kAlpha, &e), GRCORE_OK);
    }
    Fn fn{[&](GRCORE_Context * c) {
      grcore_context_charge_fuel(c, 1);
      return grcore_stack_poll(c, 5, 6) == GRCORE_VERDICT_PAUSE
          ? GRCORE_STEP_PAUSED
          : GRCORE_STEP_FINISHED;
    }};
    GRCORE_Outcome outcome;
    ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
    ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED) << engines;
    EXPECT_EQ(grcore_context_pause_location(w.ctx).file, nullptr);
    EXPECT_EQ(grcore_context_pause_location(w.ctx).line, 0);
    EXPECT_EQ(grcore_context_pause_key_count(w.ctx), 1u);
    GRCORE_PollIdentity id;
    EXPECT_EQ(grcore_context_poll_identity(w.ctx, &id), GRCORE_ERR_INVALID);
  }
}

TEST(StackPoll, PollIdentityIsReadableOnlyInReadableStates) {
  StackWorld w(1);
  FrameGuest g;
  g.engine = w.alpha;
  g.n = 5;
  GRCORE_PollIdentity id = {9, 9};
  EXPECT_EQ(grcore_context_poll_identity(w.ctx, &id), GRCORE_ERR_INVALID); // parked
  EXPECT_EQ(grcore_context_poll_identity(nullptr, &id), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_context_poll_identity(w.ctx, nullptr), GRCORE_ERR_INVALID);
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, frame_guest_entry, &g, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  ASSERT_EQ(grcore_context_poll_identity(w.ctx, &id), GRCORE_OK);
  EXPECT_EQ(id.function, 1u);
  EXPECT_EQ(id.offset, 2u);
  GRCORE_Result other = GRCORE_OK;
  std::thread([&] {
    GRCORE_PollIdentity i2;
    other = grcore_context_poll_identity(w.ctx, &i2);
  }).join();
  EXPECT_EQ(other, GRCORE_ERR_INVALID); // not the holder
  // While running it is refused.
  ASSERT_EQ(grcore_context_set_fuel(w.ctx, GRCORE_UNLIMITED), GRCORE_OK);
  GRCORE_Result during = GRCORE_OK;
  Fn fn{[&](GRCORE_Context * c) {
    GRCORE_PollIdentity i2;
    during = grcore_context_poll_identity(c, &i2);
    return GRCORE_STEP_FINISHED;
  }};
  GRCORE_Context * c = w.ctx;
  // Finish the paused run first, then run a fresh entry.
  ASSERT_EQ(grcore_resume(c, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  ASSERT_EQ(grcore_run(c, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_EQ(during, GRCORE_ERR_INVALID);
}

TEST(StackPoll, TheIdentityAFramePausedAtReadsBackAfterTheResumeToo) {
  StackWorld w(3);
  FrameGuest g;
  g.engine = w.alpha;
  g.n = 6;
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, frame_guest_entry, &g, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  GRCORE_PollIdentity paused;
  ASSERT_EQ(grcore_context_poll_identity(w.ctx, &paused), GRCORE_OK);
  EXPECT_EQ(paused.offset, 4u); // the fourth push was the first over budget
  ASSERT_EQ(grcore_context_set_fuel(w.ctx, GRCORE_UNLIMITED), GRCORE_OK);
  ASSERT_EQ(grcore_resume(w.ctx, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  EXPECT_EQ(g.sum, 21u);
}

TEST(Stack, AFrameGuestSumsTheSameWhateverTheBudget) {
  for (uint64_t fuel : {uint64_t{1}, uint64_t{4}, uint64_t{100}}) {
    StackWorld w(fuel);
    FrameGuest g;
    g.engine = w.alpha;
    g.n = 20;
    GRCORE_Outcome outcome;
    ASSERT_EQ(grcore_run(w.ctx, frame_guest_entry, &g, &outcome), GRCORE_OK);
    int pauses = 0;
    while (outcome == GRCORE_OUTCOME_PAUSED) {
      pauses++;
      ASSERT_EQ(grcore_context_set_fuel(w.ctx, grcore_context_fuel_limit(w.ctx) + fuel),
          GRCORE_OK);
      ASSERT_EQ(grcore_resume(w.ctx, &outcome), GRCORE_OK);
      ASSERT_LT(pauses, 1000);
    }
    EXPECT_EQ(g.sum, 210u) << fuel;
    EXPECT_EQ(grcore_stack_frame_count(w.stack), 0u);
    EXPECT_EQ(grcore_context_depth(w.ctx, GRCORE_DEPTH_GUEST), 0u);
    if (fuel == 1) {
      EXPECT_GT(pauses, 10);
    }
  }
}

TEST(Stack, ADepthLimitHitInsideTheGuestIsALimitErrorAndTheFramesStay) {
  StackWorld w(GRCORE_UNLIMITED, GRCORE_UNLIMITED,
      GRCORE_DEFAULT_MEMORY_RESERVE, 5);
  FrameGuest g;
  g.engine = w.alpha;
  g.n = 20;
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, frame_guest_entry, &g, &outcome), GRCORE_OK);
  EXPECT_EQ(g.failure, GRCORE_ERR_LIMIT);
  EXPECT_EQ(grcore_stack_frame_count(w.stack), 5u);
  EXPECT_EQ(grcore_context_depth(w.ctx, GRCORE_DEPTH_GUEST), 5u);
}

TEST(Stack, APausedContextWithFramesMigratesAndResumesOnAnotherThread) {
  StackWorld w(5);
  FrameGuest g;
  g.engine = w.alpha;
  g.n = 30;
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, frame_guest_entry, &g, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  size_t frames = grcore_stack_frame_count(w.stack);
  ASSERT_GT(frames, 0u);
  ASSERT_EQ(grcore_context_release(w.ctx), GRCORE_OK);

  GRCORE_Result r = GRCORE_ERR_INTERNAL;
  GRCORE_Outcome there = GRCORE_OUTCOME_PAUSED;
  size_t seen_frames = 0;
  uint64_t seen_slot = 0;
  int pauses = 0;
  std::thread([&] {
    r = grcore_context_acquire(w.ctx);
    if (r != GRCORE_OK) {
      return;
    }
    GRCORE_Stack * s = grcore_context_stack(w.ctx);
    seen_frames = grcore_stack_frame_count(s);
    grcore_stack_slot_get(s, grcore_stack_top(s), 0, &seen_slot);
    there = GRCORE_OUTCOME_PAUSED;
    while (there == GRCORE_OUTCOME_PAUSED && pauses < 100) {
      pauses++;
      grcore_context_set_fuel(w.ctx, grcore_context_fuel_limit(w.ctx) + 7);
      r = grcore_resume(w.ctx, &there);
      if (r != GRCORE_OK) {
        break;
      }
    }
    // Pops happen on this thread too: the stack went with the context.
    grcore_context_release(w.ctx);
  }).join();
  ASSERT_EQ(r, GRCORE_OK);
  EXPECT_EQ(there, GRCORE_OUTCOME_FINISHED);
  EXPECT_EQ(seen_frames, frames); // intact on arrival
  EXPECT_EQ(seen_slot, frames);   // the top frame's own counter
  ASSERT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
  // The same output as an uninterrupted run.
  EXPECT_EQ(g.sum, 30u * 31u / 2u);
  EXPECT_EQ(grcore_stack_frame_count(w.stack), 0u);
  EXPECT_EQ(grcore_context_depth(w.ctx, GRCORE_DEPTH_GUEST), 0u);
}

TEST(Stack, AnAbandonedPausedContextWithFramesDestroysClean) {
  TrackingAllocator t;
  {
    StackWorld w(3, GRCORE_UNLIMITED, GRCORE_DEFAULT_MEMORY_RESERVE,
        GRCORE_UNLIMITED, t.get());
    FrameGuest g;
    g.engine = w.alpha;
    g.n = 50;
    GRCORE_Outcome outcome;
    ASSERT_EQ(grcore_run(w.ctx, frame_guest_entry, &g, &outcome), GRCORE_OK);
    ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
    ASSERT_GT(grcore_stack_frame_count(w.stack), 0u);
  }
  EXPECT_EQ(t.live, 0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

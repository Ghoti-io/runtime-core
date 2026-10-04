/**
 * @file
 *
 * Reference-counted compiled code: the count, the release callback, threads,
 * and allocation failure.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <atomic>
#include <thread>
#include <vector>

namespace {

struct Payload {
  int released = 0;
  int value = 41;
};

void release_payload(void * p) {
  static_cast<Payload *>(p)->released++;
}

} // namespace

TEST(Code, CreateThenReleaseCallsReleaseOnceWithThePayload) {
  Payload p;
  GRCORE_Code * c = nullptr;
  ASSERT_EQ(grcore_code_create(nullptr, &p, release_payload, &c), GRCORE_OK);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(grcore_code_refcount(c), 1u);
  EXPECT_EQ(grcore_code_payload(c), &p);
  EXPECT_EQ(p.released, 0);
  grcore_code_release(c);
  EXPECT_EQ(p.released, 1);
}

TEST(Code, RetainAndReleaseBalance) {
  Payload p;
  GRCORE_Code * c = nullptr;
  ASSERT_EQ(grcore_code_create(nullptr, &p, release_payload, &c), GRCORE_OK);
  EXPECT_EQ(grcore_code_retain(c), c);
  EXPECT_EQ(grcore_code_retain(c), c);
  EXPECT_EQ(grcore_code_refcount(c), 3u);
  grcore_code_release(c);
  grcore_code_release(c);
  EXPECT_EQ(p.released, 0);
  EXPECT_EQ(grcore_code_refcount(c), 1u);
  grcore_code_release(c);
  EXPECT_EQ(p.released, 1);
}

TEST(Code, ThreadsRetainingAndReleasingASharedHandleBalance) {
  Payload p;
  GRCORE_Code * c = nullptr;
  ASSERT_EQ(grcore_code_create(nullptr, &p, release_payload, &c), GRCORE_OK);
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; t++) {
    threads.emplace_back([c] {
      for (int i = 0; i < 20000; i++) {
        grcore_code_retain(c);
        grcore_code_release(c);
      }
    });
  }
  for (auto & t : threads) {
    t.join();
  }
  EXPECT_EQ(grcore_code_refcount(c), 1u);
  EXPECT_EQ(p.released, 0);
  grcore_code_release(c);
  EXPECT_EQ(p.released, 1);
}

TEST(Code, TheLastReleaseFromAnotherThreadSeesTheOwnersWrites) {
  // The owner writes, hands a reference to a thread, and drops its own; the
  // thread's release is the last and runs the callback, which must see the
  // write (acquire-release on the count; TSan would flag it otherwise).
  static Payload p;
  p = Payload();
  GRCORE_Code * c = nullptr;
  ASSERT_EQ(grcore_code_create(nullptr, &p,
                [](void * q) { static_cast<Payload *>(q)->released = static_cast<Payload *>(q)->value; },
                &c),
      GRCORE_OK);
  p.value = 7;
  grcore_code_retain(c);
  std::thread t([c] { grcore_code_release(c); });
  grcore_code_release(c);
  t.join();
  EXPECT_EQ(p.released, 7);
}

TEST(Code, ARefusedCreateWritesNothing) {
  Payload p;
  GRCORE_Code * sentinel = reinterpret_cast<GRCORE_Code *>(0x1234);
  GRCORE_Code * c = sentinel;
  EXPECT_EQ(grcore_code_create(nullptr, &p, nullptr, &c), GRCORE_ERR_INVALID);
  EXPECT_EQ(c, sentinel);
  EXPECT_EQ(grcore_code_create(nullptr, &p, release_payload, nullptr),
      GRCORE_ERR_INVALID);
}

TEST(Code, NullHandlesAreNoOps) {
  grcore_code_release(nullptr);
  EXPECT_EQ(grcore_code_retain(nullptr), nullptr);
  EXPECT_EQ(grcore_code_payload(nullptr), nullptr);
  EXPECT_EQ(grcore_code_refcount(nullptr), 0u);
}

TEST(Code, AllocationFailureSweep) {
  for (long n = 1; n <= 3; n++) {
    TrackingAllocator a;
    a.fail_at = n;
    Payload p;
    GRCORE_Code * sentinel = reinterpret_cast<GRCORE_Code *>(0x1234);
    GRCORE_Code * c = sentinel;
    GRCORE_Result r = grcore_code_create(a.get(), &p, release_payload, &c);
    if (n == 1) {
      EXPECT_EQ(r, GRCORE_ERR_OOM);
      EXPECT_EQ(c, sentinel);
      EXPECT_EQ(a.live, 0);
    } else {
      ASSERT_EQ(r, GRCORE_OK);
      // The handle keeps its own copy of the allocator: free through it after
      // the caller's pointer would have been dropped.
      grcore_code_release(c);
      EXPECT_EQ(a.live, 0);
      EXPECT_EQ(p.released, 1);
    }
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

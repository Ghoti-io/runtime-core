/**
 * @file
 *
 * The Windows child helper (tests/test_helpers.h, `run_in_child`): a child that hangs is given up on at its
 * timeout and terminated, one that aborts is an abort with its stderr, one that returns has status zero.
 * Windows only: elsewhere the tests fork, and there is nothing here to run.
 */

#include "test_helpers.h"

#ifdef _WIN32
#include <chrono>

TEST(ChildHelper, AChildThatHangsIsTerminatedAtTheTimeoutAndReportedAsTimedOutNotAborted) {
  // The body sleeps for a minute at most (so that a helper that never gives up still ends this test, late).
  const auto begin = std::chrono::steady_clock::now();
  const grcore_test::ChildOutcome o = grcore_test::run_in_child(
      [] {
        for (int i = 0; i < 600; i++) {
          Sleep(100);
        }
        std::_Exit(0);
      },
      3000, /*expect_timeout=*/true);
  const auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
  EXPECT_TRUE(o.timed_out);
  EXPECT_FALSE(o.ran);
  EXPECT_FALSE(o.aborted);
  EXPECT_LT(elapsed_ms, 30000) << "the wait ended at its timeout, not when the child did (a minute) or the 120 s default";
  EXPECT_GE(elapsed_ms, 2500) << "and not before it";
  ASSERT_NE(o.pid, 0ul);
  EXPECT_EQ(o.status, 1ul) << "the child's exit code is the one it was terminated with (259, STILL_ACTIVE, if it still runs)";
  HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(o.pid));
  if (h != nullptr) {
    EXPECT_EQ(WaitForSingleObject(h, 0), static_cast<DWORD>(WAIT_OBJECT_0)) << "the child process is gone";
    CloseHandle(h);
  }
}

TEST(ChildHelper, AChildThatAbortsIsAnAbortWithItsStderrAndNotATimeout) {
  const grcore_test::ChildOutcome o = grcore_test::run_in_child([] {
    std::fprintf(stderr, "child-marker-before-abort\n");
    std::fflush(stderr);
    std::abort();
  });
  EXPECT_TRUE(o.aborted) << "status " << o.status;
  EXPECT_TRUE(o.ran);
  EXPECT_FALSE(o.timed_out);
  EXPECT_NE(o.err.find("child-marker-before-abort"), std::string::npos) << o.err;
}

TEST(ChildHelper, AChildThatReturnsHasStatusZeroAndRan) {
  const grcore_test::ChildOutcome o = grcore_test::run_in_child([] {});
  EXPECT_TRUE(o.ran);
  EXPECT_EQ(o.status, 0ul);
  EXPECT_FALSE(o.aborted);
  EXPECT_FALSE(o.timed_out);
}
#endif

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

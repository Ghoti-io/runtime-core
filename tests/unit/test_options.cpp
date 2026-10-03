/**
 * @file
 *
 * Context options.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../../src/b/options_internal.h"

#include <cstring>
#include <string>

namespace {
const GRCORE_Key kA = {"a", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_NONE, nullptr};
const GRCORE_Key kB = {"b", GRCORE_CARDINALITY_MANY, GRCORE_PHASE_OBSERVE,
    nullptr};

std::string bytes_of(const GRCORE_Options * o, const GRCORE_Key * k) {
  const void * p = reinterpret_cast<const void *>(1);
  size_t n = 99;
  EXPECT_EQ(grcore_options_get_keyed(o, k, &p, &n), GRCORE_OK);
  if (p == nullptr) {
    EXPECT_EQ(n, 0u);
    return "<absent>";
  }
  return std::string(static_cast<const char *>(p), n);
}
} // namespace

TEST(Options, UnlimitedIsDistinctFromTheLargestRealValue) {
  GRCORE_Options * o;
  ASSERT_EQ(grcore_options_create(nullptr, &o), GRCORE_OK);
  uint64_t before = grcore_options_get_fuel(o);
  ASSERT_EQ(grcore_options_set_fuel(o, UINT64_MAX - 1), GRCORE_OK);
  EXPECT_EQ(grcore_options_get_fuel(o), UINT64_MAX - 1);
  EXPECT_NE(grcore_options_get_fuel(o), before);
  EXPECT_NE(grcore_options_get_fuel(o), GRCORE_UNLIMITED);
  grcore_options_destroy(o);
}

TEST(Options, DefaultsAreUnlimited) {
  GRCORE_Options * o;
  ASSERT_EQ(grcore_options_create(nullptr, &o), GRCORE_OK);
  EXPECT_EQ(grcore_options_get_fuel(o), GRCORE_UNLIMITED);
  EXPECT_EQ(grcore_options_get_memory_bytes(o), GRCORE_UNLIMITED);
  EXPECT_EQ(grcore_options_get_guest_depth(o), GRCORE_UNLIMITED);
  EXPECT_EQ(grcore_options_get_native_depth(o), GRCORE_UNLIMITED);
  EXPECT_EQ(bytes_of(o, &kA), "<absent>");
  grcore_options_destroy(o);
}

TEST(Options, SettersAndGettersRoundTripAndAreIndependent) {
  GRCORE_Options * o;
  ASSERT_EQ(grcore_options_create(nullptr, &o), GRCORE_OK);
  EXPECT_EQ(grcore_options_set_fuel(o, 1), GRCORE_OK);
  EXPECT_EQ(grcore_options_set_memory_bytes(o, 2), GRCORE_OK);
  EXPECT_EQ(grcore_options_set_guest_depth(o, 3), GRCORE_OK);
  EXPECT_EQ(grcore_options_set_native_depth(o, 4), GRCORE_OK);
  EXPECT_EQ(grcore_options_get_fuel(o), 1u);
  EXPECT_EQ(grcore_options_get_memory_bytes(o), 2u);
  EXPECT_EQ(grcore_options_get_guest_depth(o), 3u);
  EXPECT_EQ(grcore_options_get_native_depth(o), 4u);
  EXPECT_EQ(grcore_options_set_fuel(o, 0), GRCORE_OK); // zero is a real value
  EXPECT_EQ(grcore_options_get_fuel(o), 0u);
  EXPECT_EQ(grcore_options_set_fuel(o, GRCORE_UNLIMITED), GRCORE_OK);
  EXPECT_EQ(grcore_options_get_fuel(o), GRCORE_UNLIMITED);
  grcore_options_destroy(o);
}

TEST(Options, NullArgumentsAreRefused) {
  GRCORE_Options * o;
  EXPECT_EQ(grcore_options_create(nullptr, nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_options_set_fuel(nullptr, 1), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_options_get_fuel(nullptr), GRCORE_UNLIMITED);
  ASSERT_EQ(grcore_options_create(nullptr, &o), GRCORE_OK);
  const void * p = nullptr;
  size_t n = 0;
  EXPECT_EQ(grcore_options_set_keyed(o, nullptr, "x", 1), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_options_set_keyed(o, &kA, nullptr, 1), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_options_get_keyed(o, &kA, nullptr, &n), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_options_get_keyed(o, &kA, &p, nullptr), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_options_get_keyed(o, nullptr, &p, &n), GRCORE_ERR_INVALID);
  grcore_options_destroy(o);
  grcore_options_destroy(nullptr);
}

TEST(Options, KeyedBytesAreCopiedReplacedAndRemoved) {
  GRCORE_Options * o;
  ASSERT_EQ(grcore_options_create(nullptr, &o), GRCORE_OK);
  char buf[] = "hello";
  ASSERT_EQ(grcore_options_set_keyed(o, &kA, buf, 5), GRCORE_OK);
  std::memset(buf, 'X', 5); // the caller's buffer is not the stored one
  EXPECT_EQ(bytes_of(o, &kA), "hello");
  ASSERT_EQ(grcore_options_set_keyed(o, &kB, "bee", 3), GRCORE_OK);
  ASSERT_EQ(grcore_options_set_keyed(o, &kA, "longer text", 11), GRCORE_OK);
  EXPECT_EQ(bytes_of(o, &kA), "longer text");
  EXPECT_EQ(bytes_of(o, &kB), "bee");
  ASSERT_EQ(grcore_options_set_keyed(o, &kA, nullptr, 0), GRCORE_OK);
  EXPECT_EQ(bytes_of(o, &kA), "<absent>");
  EXPECT_EQ(bytes_of(o, &kB), "bee");
  ASSERT_EQ(grcore_options_set_keyed(o, &kA, nullptr, 0), GRCORE_OK); // absent
  grcore_options_destroy(o);
}

TEST(Options, GetKeyedWritesNothingOnRefusal) {
  GRCORE_Options * o;
  ASSERT_EQ(grcore_options_create(nullptr, &o), GRCORE_OK);
  const void * p = reinterpret_cast<const void *>(7);
  size_t n = 77;
  EXPECT_EQ(grcore_options_get_keyed(o, nullptr, &p, &n), GRCORE_ERR_INVALID);
  EXPECT_EQ(p, reinterpret_cast<const void *>(7));
  EXPECT_EQ(n, 77u);
  grcore_options_destroy(o);
}

TEST(Options, CloneIsDeepAndIndependent) {
  GRCORE_Options *o, *c;
  ASSERT_EQ(grcore_options_create(nullptr, &o), GRCORE_OK);
  grcore_options_set_fuel(o, 10);
  grcore_options_set_native_depth(o, 5);
  ASSERT_EQ(grcore_options_set_keyed(o, &kA, "one", 3), GRCORE_OK);
  ASSERT_EQ(grcore_options_clone(o, nullptr, &c), GRCORE_OK);
  grcore_options_set_fuel(o, 99);
  ASSERT_EQ(grcore_options_set_keyed(o, &kA, "two", 3), GRCORE_OK);
  EXPECT_EQ(grcore_options_get_fuel(c), 10u);
  EXPECT_EQ(grcore_options_get_native_depth(c), 5u);
  EXPECT_EQ(bytes_of(c, &kA), "one");
  grcore_options_destroy(o);
  EXPECT_EQ(bytes_of(c, &kA), "one");
  grcore_options_destroy(c);
}

TEST(Options, CloneOfNullIsTheDefaults) {
  GRCORE_Options * c;
  ASSERT_EQ(grcore_options_clone(nullptr, nullptr, &c), GRCORE_OK);
  EXPECT_EQ(grcore_options_get_fuel(c), GRCORE_UNLIMITED);
  grcore_options_destroy(c);
}

TEST(Options, EveryAllocationFailureIsRefusedAndLeaksNothing) {
  // Create, two keyed sets (one replacing), a clone: sweep the Nth failure.
  bool reached_the_end = false;
  for (long n = 1; n < 50 && !reached_the_end; n++) {
    TrackingAllocator t;
    t.fail_at = n;
    GRCORE_Options *o = nullptr, *c = nullptr;
    GRCORE_Result r = grcore_options_create(t.get(), &o);
    if (r == GRCORE_OK) {
      r = grcore_options_set_keyed(o, &kA, "aa", 2);
      if (r == GRCORE_OK) {
        r = grcore_options_set_keyed(o, &kB, "bb", 2);
      }
      if (r == GRCORE_OK) {
        r = grcore_options_set_keyed(o, &kA, "aaaa", 4);
      }
      if (r == GRCORE_OK) {
        r = grcore_options_clone(o, t.get(), &c);
      }
    }
    if (r == GRCORE_OK) {
      reached_the_end = true;
      EXPECT_EQ(bytes_of(c, &kB), "bb");
    } else {
      EXPECT_EQ(r, GRCORE_ERR_OOM) << "n=" << n;
      if (o != nullptr) {
        // A refused set leaves the earlier entries intact.
        // Whatever was stored before the refusal is exactly as it was.
        std::string a = bytes_of(o, &kA);
        EXPECT_TRUE(a == "<absent>" || a == "aa" || a == "aaaa") << a;
        std::string b = bytes_of(o, &kB);
        EXPECT_TRUE(b == "<absent>" || b == "bb") << b;
      }
    }
    grcore_options_destroy(c);
    grcore_options_destroy(o);
    EXPECT_EQ(t.live, 0) << "n=" << n;
  }
  EXPECT_TRUE(reached_the_end);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

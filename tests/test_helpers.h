/**
 * @file
 *
 * Shared helpers for the Ghoti.io Runtime-core unit tests.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRCORE_TEST_HELPERS_H
#define GHOTI_IO_GRCORE_TEST_HELPERS_H

#include <gtest/gtest.h>

#include <ghoti.io/runtime-core/runtime-core.h>

#include <cstdlib>

/* A base allocator that counts, tracks live blocks, and can fail the Nth
 * allocation call (malloc, calloc or realloc, counted from 1). The tests that
 * sweep N from 1 upward use it to reach every allocation-failure arm. */
struct TrackingAllocator {
  GRCORE_Allocator vtable;
  long calls = 0;
  long fail_at = 0; // 0: never fail
  long live = 0;    // successful allocations not yet freed

  TrackingAllocator(const TrackingAllocator &) = delete;
  TrackingAllocator & operator=(const TrackingAllocator &) = delete;
  TrackingAllocator() {
    vtable.ctx = this;
    vtable.malloc_fn = [](void * c, size_t n) -> void * {
      auto * t = static_cast<TrackingAllocator *>(c);
      if (t->fails()) {
        return nullptr;
      }
      void * p = std::malloc(n ? n : 1);
      if (p != nullptr) {
        t->live++;
      }
      return p;
    };
    vtable.calloc_fn = [](void * c, size_t n, size_t m) -> void * {
      auto * t = static_cast<TrackingAllocator *>(c);
      if (t->fails()) {
        return nullptr;
      }
      void * p = std::calloc(n ? n : 1, m ? m : 1);
      if (p != nullptr) {
        t->live++;
      }
      return p;
    };
    vtable.realloc_fn = [](void * c, void * p, size_t n) -> void * {
      auto * t = static_cast<TrackingAllocator *>(c);
      if (t->fails()) {
        return nullptr;
      }
      void * q = std::realloc(p, n ? n : 1);
      if (p == nullptr && q != nullptr) {
        t->live++;
      }
      return q;
    };
    vtable.free_fn = [](void * c, void * p) {
      if (p != nullptr) {
        static_cast<TrackingAllocator *>(c)->live--;
      }
      std::free(p);
    };
  }
  bool fails() {
    calls++;
    return fail_at != 0 && calls == fail_at;
  }
  const GRCORE_Allocator * get() const { return &vtable; }
};

/* A page provider over the default one that counts and can be told to fail. */
struct FakePages {
  GRCORE_PageProvider vtable;
  bool fail = false;
  long live = 0;

  FakePages(const FakePages &) = delete;
  FakePages & operator=(const FakePages &) = delete;
  FakePages() {
    vtable.ctx = this;
    vtable.page_size = grcore_page_provider_default()->page_size;
    vtable.map = [](void * c, size_t n) -> void * {
      auto * f = static_cast<FakePages *>(c);
      if (f->fail) {
        return nullptr;
      }
      const GRCORE_PageProvider * d = grcore_page_provider_default();
      void * p = d->map(d->ctx, n);
      if (p != nullptr) {
        f->live++;
      }
      return p;
    };
    vtable.unmap = [](void * c, void * p, size_t n) {
      const GRCORE_PageProvider * d = grcore_page_provider_default();
      d->unmap(d->ctx, p, n);
      static_cast<FakePages *>(c)->live--;
    };
  }
};

#endif /* GHOTI_IO_GRCORE_TEST_HELPERS_H */

/**
 * @file
 *
 * The default page provider and the counting wrapper over it.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <csignal>
#include <cstring>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

TEST(Page, DefaultIsAPowerOfTwoGranuleWithAllThreeCalls) {
  const GRCORE_PageProvider * p = grcore_page_provider_default();
  ASSERT_NE(p, nullptr);
  EXPECT_GE(p->page_size, 512u);
  EXPECT_EQ(p->page_size & (p->page_size - 1), 0u);
  EXPECT_NE(p->map, nullptr);
  EXPECT_NE(p->unmap, nullptr);
  EXPECT_NE(p->protect, nullptr);
}

TEST(Page, DefaultMapsZeroFilledWritablePages) {
  const GRCORE_PageProvider * p = grcore_page_provider_default();
  size_t size = p->page_size * 2;
  auto * m = static_cast<unsigned char *>(p->map(p->ctx, size));
  ASSERT_NE(m, nullptr);
  for (size_t i = 0; i < size; i++) {
    ASSERT_EQ(m[i], 0) << i;
  }
  std::memset(m, 0xAB, size);
  p->unmap(p->ctx, m, size);
}

TEST(Page, DefaultRefusesZeroAndPartialPages) {
  const GRCORE_PageProvider * p = grcore_page_provider_default();
  EXPECT_EQ(p->map(p->ctx, 0), nullptr);
  EXPECT_EQ(p->map(p->ctx, p->page_size + 1), nullptr);
  EXPECT_EQ(p->map(p->ctx, p->page_size - 1), nullptr);
}

TEST(Page, UnmapOfNullIsIgnored) {
  const GRCORE_PageProvider * p = grcore_page_provider_default();
  p->unmap(p->ctx, nullptr, p->page_size);
}

TEST(Page, ContextProviderChargesBytesAndBlocks) {
  GRCORE_Group * g;
  ASSERT_EQ(grcore_group_create(nullptr, nullptr, &g), GRCORE_OK);
  GRCORE_Context * c;
  ASSERT_EQ(grcore_context_create(g, nullptr, &c), GRCORE_OK);
  const GRCORE_PageProvider * p = grcore_context_page_provider(c);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->page_size, grcore_page_provider_default()->page_size);

  size_t one = p->page_size;
  void * a = p->map(p->ctx, one);
  void * b = p->map(p->ctx, one * 3);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(grcore_context_memory_in_use(c), one * 4);
  EXPECT_EQ(grcore_context_memory_blocks(c), 2u);
  EXPECT_EQ(grcore_context_memory_peak(c), one * 4);
  p->unmap(p->ctx, b, one * 3);
  EXPECT_EQ(grcore_context_memory_in_use(c), one);
  EXPECT_EQ(grcore_context_memory_peak(c), one * 4);
  p->unmap(p->ctx, a, one);
  EXPECT_EQ(grcore_context_memory_in_use(c), 0u);
  EXPECT_EQ(grcore_context_memory_blocks(c), 0u);
  EXPECT_EQ(grcore_context_destroy(c), GRCORE_OK);
  EXPECT_EQ(grcore_group_destroy(g), GRCORE_OK);
}

TEST(Page, BadSizesChargeNothing) {
  GRCORE_Group * g;
  ASSERT_EQ(grcore_group_create(nullptr, nullptr, &g), GRCORE_OK);
  const GRCORE_PageProvider * p = grcore_group_page_provider(g);
  EXPECT_EQ(p->map(p->ctx, 0), nullptr);
  EXPECT_EQ(p->map(p->ctx, p->page_size + 1), nullptr);
  EXPECT_EQ(grcore_group_memory_in_use(g), 0u);
  EXPECT_EQ(grcore_group_memory_blocks(g), 0u);
  EXPECT_EQ(grcore_group_destroy(g), GRCORE_OK);
}

TEST(Page, BaseFailureLeavesTheCountersAlone) {
  FakePages base;
  GRCORE_Group * g;
  ASSERT_EQ(grcore_group_create(nullptr, &base.vtable, &g), GRCORE_OK);
  const GRCORE_PageProvider * p = grcore_group_page_provider(g);
  base.fail = true;
  EXPECT_EQ(p->map(p->ctx, p->page_size), nullptr);
  EXPECT_EQ(grcore_group_memory_in_use(g), 0u);
  EXPECT_EQ(grcore_group_memory_blocks(g), 0u);
  EXPECT_EQ(grcore_group_memory_peak(g), 0u);
  base.fail = false;
  void * m = p->map(p->ctx, p->page_size);
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(base.live, 1);
  p->unmap(p->ctx, m, p->page_size);
  EXPECT_EQ(base.live, 0);
  EXPECT_EQ(grcore_group_destroy(g), GRCORE_OK);
}

TEST(Page, CountingUnmapOfNullChargesNothing) {
  GRCORE_Group * g;
  ASSERT_EQ(grcore_group_create(nullptr, nullptr, &g), GRCORE_OK);
  GRCORE_Context * c;
  ASSERT_EQ(grcore_context_create(g, nullptr, &c), GRCORE_OK);
  const GRCORE_PageProvider * cp = grcore_context_page_provider(c);
  const GRCORE_PageProvider * gp = grcore_group_page_provider(g);
  void * held = cp->map(cp->ctx, cp->page_size);
  ASSERT_NE(held, nullptr);
  cp->unmap(cp->ctx, nullptr, cp->page_size);
  gp->unmap(gp->ctx, nullptr, gp->page_size);
  EXPECT_EQ(grcore_context_memory_in_use(c), cp->page_size);
  EXPECT_EQ(grcore_context_memory_blocks(c), 1u);
  EXPECT_EQ(grcore_group_memory_in_use(g), 0u);
  EXPECT_EQ(grcore_group_memory_blocks(g), 0u);
  cp->unmap(cp->ctx, held, cp->page_size);
  EXPECT_EQ(grcore_context_destroy(c), GRCORE_OK);
  EXPECT_EQ(grcore_group_destroy(g), GRCORE_OK);
}

TEST(Page, GroupRefusesAnUnusableCustomProvider) {
  FakePages good;
  GRCORE_PageProvider p = good.vtable;
  GRCORE_Group * g = reinterpret_cast<GRCORE_Group *>(1);
  p.page_size = 0;
  EXPECT_EQ(grcore_group_create(nullptr, &p, &g), GRCORE_ERR_INVALID);
  p = good.vtable;
  p.map = nullptr;
  EXPECT_EQ(grcore_group_create(nullptr, &p, &g), GRCORE_ERR_INVALID);
  p = good.vtable;
  p.unmap = nullptr;
  EXPECT_EQ(grcore_group_create(nullptr, &p, &g), GRCORE_ERR_INVALID);
  EXPECT_EQ(g, reinterpret_cast<GRCORE_Group *>(1));
  EXPECT_EQ(grcore_group_create(nullptr, &good.vtable, &g), GRCORE_OK);
  EXPECT_EQ(grcore_group_destroy(g), GRCORE_OK);
}

TEST(Page, ACustomProviderWithItsOwnGranuleIsHonoured) {
  FakePages base;
  base.vtable.page_size = 4 * grcore_page_provider_default()->page_size;
  GRCORE_Group * g;
  ASSERT_EQ(grcore_group_create(nullptr, &base.vtable, &g), GRCORE_OK);
  const GRCORE_PageProvider * p = grcore_group_page_provider(g);
  EXPECT_EQ(p->page_size, base.vtable.page_size);
  EXPECT_EQ(p->map(p->ctx, grcore_page_provider_default()->page_size), nullptr);
  EXPECT_EQ(grcore_group_destroy(g), GRCORE_OK);
}

/* ---- protect ------------------------------------------------------------ */

/* Runs `body` in a forked child and returns the signal that killed it, or 0
 * when it exited normally. A write to a read-execute page must be SIGSEGV. */
template <typename F>
static int child_signal(F body) {
  pid_t pid = fork();
  if (pid == 0) {
    body();
    _exit(0);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFSIGNALED(status) ? WTERMSIG(status) : 0;
}

static void exercise_protect(const GRCORE_PageProvider * p) {
  size_t size = p->page_size;
  auto * m = static_cast<unsigned char *>(p->map(p->ctx, size));
  ASSERT_NE(m, nullptr);
  m[0] = 0xC3; /* ret */
  ASSERT_EQ(grcore_page_protect(p, m, size, GRCORE_PAGE_READ_EXECUTE),
      GRCORE_OK);
  EXPECT_EQ(m[0], 0xC3); /* still readable */
  EXPECT_EQ(child_signal([&] { m[1] = 1; }), SIGSEGV);
  ASSERT_EQ(grcore_page_protect(p, m, size, GRCORE_PAGE_READ_WRITE),
      GRCORE_OK);
  m[1] = 7;
  EXPECT_EQ(m[1], 7);
  EXPECT_EQ(child_signal([&] { m[2] = 1; }), 0);
  p->unmap(p->ctx, m, size);
}

TEST(Page, DefaultFlipsAMappingToReadExecuteAndBack) {
  exercise_protect(grcore_page_provider_default());
}

TEST(Page, CountingProviderFlipsAMappingAndChargesNothing) {
  GRCORE_Group * g;
  ASSERT_EQ(grcore_group_create(nullptr, nullptr, &g), GRCORE_OK);
  GRCORE_Context * c;
  ASSERT_EQ(grcore_context_create(g, nullptr, &c), GRCORE_OK);
  const GRCORE_PageProvider * p = grcore_context_page_provider(c);
  ASSERT_NE(p->protect, nullptr);
  void * m = p->map(p->ctx, p->page_size);
  ASSERT_NE(m, nullptr);
  uint64_t in_use = grcore_context_memory_in_use(c);
  uint64_t blocks = grcore_context_memory_blocks(c);
  uint64_t peak = grcore_context_memory_peak(c);
  p->unmap(p->ctx, m, p->page_size);
  exercise_protect(p);
  EXPECT_EQ(grcore_context_memory_in_use(c), 0u);
  m = p->map(p->ctx, p->page_size);
  ASSERT_EQ(grcore_page_protect(p, m, p->page_size, GRCORE_PAGE_READ_EXECUTE),
      GRCORE_OK);
  EXPECT_EQ(grcore_context_memory_in_use(c), in_use);
  EXPECT_EQ(grcore_context_memory_blocks(c), blocks);
  EXPECT_EQ(grcore_context_memory_peak(c), peak);
  ASSERT_EQ(grcore_page_protect(p, m, p->page_size, GRCORE_PAGE_READ_WRITE),
      GRCORE_OK);
  p->unmap(p->ctx, m, p->page_size);
  EXPECT_EQ(grcore_context_destroy(c), GRCORE_OK);
  EXPECT_EQ(grcore_group_destroy(g), GRCORE_OK);
}

TEST(Page, ProtectIsInvalidWithoutAProtectMember) {
  GRCORE_PageProvider none = *grcore_page_provider_default();
  none.protect = nullptr;
  void * m = none.map(none.ctx, none.page_size);
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(grcore_page_protect(&none, m, none.page_size,
                GRCORE_PAGE_READ_EXECUTE),
      GRCORE_ERR_INVALID);
  /* A counting wrapper over a provider that cannot protect cannot either. */
  GRCORE_Group * g;
  ASSERT_EQ(grcore_group_create(nullptr, &none, &g), GRCORE_OK);
  const GRCORE_PageProvider * gp = grcore_group_page_provider(g);
  EXPECT_EQ(gp->protect, nullptr);
  EXPECT_EQ(grcore_page_protect(gp, m, none.page_size, GRCORE_PAGE_READ_WRITE),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_group_destroy(g), GRCORE_OK);
  none.unmap(none.ctx, m, none.page_size);
}

TEST(Page, ProtectRefusesBadArgumentsAndLeavesTheMappingAlone) {
  const GRCORE_PageProvider * p = grcore_page_provider_default();
  auto * m = static_cast<unsigned char *>(p->map(p->ctx, p->page_size * 2));
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(grcore_page_protect(nullptr, m, p->page_size,
                GRCORE_PAGE_READ_EXECUTE),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_page_protect(p, nullptr, p->page_size,
                GRCORE_PAGE_READ_EXECUTE),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_page_protect(p, m + 1, p->page_size,
                GRCORE_PAGE_READ_EXECUTE),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_page_protect(p, m, p->page_size + 1,
                GRCORE_PAGE_READ_EXECUTE),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_page_protect(p, m, 0, GRCORE_PAGE_READ_EXECUTE),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_page_protect(p, m, p->page_size,
                static_cast<GRCORE_PageAccess>(2)),
      GRCORE_ERR_INVALID);
  m[0] = 1; /* still writable: nothing above changed it */
  p->unmap(p->ctx, m, p->page_size * 2);
}

TEST(Page, ProtectReportsAProviderFailureAsIo) {
  FakePages base;
  base.fail_protect = true;
  GRCORE_Group * g;
  ASSERT_EQ(grcore_group_create(nullptr, &base.vtable, &g), GRCORE_OK);
  const GRCORE_PageProvider * p = grcore_group_page_provider(g);
  void * m = p->map(p->ctx, p->page_size);
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(grcore_page_protect(p, m, p->page_size, GRCORE_PAGE_READ_EXECUTE),
      GRCORE_ERR_IO);
  EXPECT_EQ(base.protects, 1);
  p->unmap(p->ctx, m, p->page_size);
  EXPECT_EQ(grcore_group_destroy(g), GRCORE_OK);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

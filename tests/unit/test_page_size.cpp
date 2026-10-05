/**
 * @file
 *
 * A page provider states its own size (b/page.h), so the struct can grow at
 * the end. The cases are those of test_key_size.cpp, for the one member a
 * provider gained after the first generation: `protect`.
 *
 * Every case is a provider as an older or a newer header would have built it,
 * and each has a control: the same provider at its full size, which must
 * behave the other way. The older-layout copy is exactly as large as its size
 * says, on the heap, so a read past the end is a finding under ASan or
 * Valgrind and not a silent zero; the `protect` that must not be read is a
 * tripwire that counts its calls.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace {

int g_protect_calls = 0;

void * default_map(void *, size_t size) {
  const GRCORE_PageProvider * d = grcore_page_provider_default();
  return d->map(d->ctx, size);
}
void default_unmap(void *, void * ptr, size_t size) {
  const GRCORE_PageProvider * d = grcore_page_provider_default();
  d->unmap(d->ctx, ptr, size);
}
bool tripwire_protect(void *, void *, size_t, GRCORE_PageAccess) {
  g_protect_calls++;
  return true;
}

// The provider as the first generation had it: everything through `unmap`,
// and nothing after. A copy of the layout, so the bytes after it are not the
// test's.
struct OldProvider {
  size_t size;
  void * ctx;
  size_t page_size;
  void * (*map)(void *, size_t);
  void (*unmap)(void *, void *, size_t);
};
static_assert(sizeof(OldProvider) == GRCORE_PAGE_PROVIDER_MIN_SIZE,
    "the old layout is the minimum layout");
static_assert(offsetof(OldProvider, unmap) ==
        offsetof(GRCORE_PageProvider, unmap),
    "the old layout is a prefix of the current one");

GRCORE_PageProvider tripwire_provider() {
  return GRCORE_PAGE_PROVIDER_INIT(nullptr,
      grcore_page_provider_default()->page_size, default_map, default_unmap,
      tripwire_protect);
}

const GRCORE_PageProvider * old_provider_on_heap() {
  auto * p = static_cast<OldProvider *>(std::malloc(sizeof(OldProvider)));
  p->size = sizeof(OldProvider);
  p->ctx = nullptr;
  p->page_size = grcore_page_provider_default()->page_size;
  p->map = default_map;
  p->unmap = default_unmap;
  return reinterpret_cast<const GRCORE_PageProvider *>(p);
}

// A group over `provider`, a context in it, and one page mapped and unmapped
// through the context's counting provider, then asked to change protection:
// returns what `grcore_page_protect` said about the context's provider.
GRCORE_Result drive(const GRCORE_PageProvider * provider, bool * mapped) {
  GRCORE_Group * g = nullptr;
  GRCORE_Context * c = nullptr;
  EXPECT_EQ(grcore_group_create(nullptr, provider, &g), GRCORE_OK);
  EXPECT_EQ(grcore_context_create(g, nullptr, &c), GRCORE_OK);
  const GRCORE_PageProvider * counting = grcore_context_page_provider(c);
  void * m = counting->map(counting->ctx, counting->page_size);
  *mapped = m != nullptr;
  GRCORE_Result r = GRCORE_ERR_INVALID;
  if (m != nullptr) {
    EXPECT_EQ(grcore_context_memory_in_use(c), counting->page_size);
    r = grcore_page_protect(
        counting, m, counting->page_size, GRCORE_PAGE_READ_EXECUTE);
    if (r == GRCORE_OK) {
      EXPECT_EQ(grcore_page_protect(counting, m, counting->page_size,
                    GRCORE_PAGE_READ_WRITE),
          GRCORE_OK);
    }
    counting->unmap(counting->ctx, m, counting->page_size);
    EXPECT_EQ(grcore_context_memory_in_use(c), 0u);
  }
  EXPECT_EQ(grcore_context_destroy(c), GRCORE_OK);
  EXPECT_EQ(grcore_group_destroy(g), GRCORE_OK);
  return r;
}

} // namespace

TEST(PageSize, TheInitialiserWritesTheSizeOfTheStructItWasCompiledAgainst) {
  static const GRCORE_PageProvider p =
      GRCORE_PAGE_PROVIDER_INIT(nullptr, 4096, nullptr, nullptr, nullptr);
  EXPECT_EQ(p.size, sizeof(GRCORE_PageProvider));
  EXPECT_GE(p.size, GRCORE_PAGE_PROVIDER_MIN_SIZE);
  EXPECT_TRUE(grcore_page_provider_valid(&p));
  EXPECT_FALSE(grcore_page_provider_valid(nullptr));
  EXPECT_TRUE(grcore_page_provider_valid(grcore_page_provider_default()));
}

TEST(PageSize, AnOlderShorterProviderIsUsableAndItsAbsentProtectIsNeverRead) {
  const GRCORE_PageProvider * old = old_provider_on_heap();
  bool mapped = false;
  // The group takes the provider, the counting provider over it maps and
  // charges, and `protect` is absent in both: unsupported, not a fault.
  EXPECT_EQ(drive(old, &mapped), GRCORE_ERR_INVALID);
  EXPECT_TRUE(mapped);
  void * m = default_map(nullptr, old->page_size);
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(grcore_page_protect(old, m, old->page_size, GRCORE_PAGE_READ_EXECUTE),
      GRCORE_ERR_INVALID);
  default_unmap(nullptr, m, old->page_size);
  std::free(const_cast<GRCORE_PageProvider *>(old));
}

TEST(PageSize, ATripwireBeyondTheStatedSizeIsNotCalledAndAtFullSizeIs) {
  // The plain-run form of the test above: the member is real and armed, and
  // only `size` keeps it unread.
  g_protect_calls = 0;
  {
    GRCORE_PageProvider shortened = tripwire_provider();
    shortened.size = GRCORE_PAGE_PROVIDER_MIN_SIZE;
    bool mapped = false;
    EXPECT_EQ(drive(&shortened, &mapped), GRCORE_ERR_INVALID);
    EXPECT_TRUE(mapped);
    EXPECT_EQ(g_protect_calls, 0);
  }
  // Control: the same bytes at full size are read, and the counting provider
  // forwards the call.
  g_protect_calls = 0;
  {
    GRCORE_PageProvider whole = tripwire_provider();
    bool mapped = false;
    EXPECT_EQ(drive(&whole, &mapped), GRCORE_OK);
    EXPECT_TRUE(mapped);
    EXPECT_EQ(g_protect_calls, 2);
  }
}

TEST(PageSize, ASizeBelowTheMinimumOrOffAlignmentIsRefusedNotRead) {
  g_protect_calls = 0;
  const size_t bad[] = {0, 1, GRCORE_PAGE_PROVIDER_MIN_SIZE - sizeof(void *),
      GRCORE_PAGE_PROVIDER_MIN_SIZE - 1, GRCORE_PAGE_PROVIDER_MIN_SIZE + 1,
      sizeof(GRCORE_PageProvider) + 1,
      sizeof(GRCORE_PageProvider) + sizeof(void *) / 2};
  for (size_t size : bad) {
    GRCORE_PageProvider p = tripwire_provider();
    p.size = size;
    EXPECT_FALSE(grcore_page_provider_valid(&p)) << "size " << size;
    GRCORE_Group * g = reinterpret_cast<GRCORE_Group *>(1);
    EXPECT_EQ(grcore_group_create(nullptr, &p, &g), GRCORE_ERR_INVALID)
        << "size " << size;
    EXPECT_EQ(g, reinterpret_cast<GRCORE_Group *>(1)) << "written only on success";
    void * m = default_map(nullptr, p.page_size);
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(grcore_page_protect(&p, m, p.page_size, GRCORE_PAGE_READ_EXECUTE),
        GRCORE_ERR_INVALID)
        << "size " << size;
    default_unmap(nullptr, m, p.page_size);
  }
  EXPECT_EQ(g_protect_calls, 0);
  // Controls: the smallest size and the full size are accepted, so the list
  // above is refused for its size and not for anything else about it.
  for (size_t size : {GRCORE_PAGE_PROVIDER_MIN_SIZE, sizeof(GRCORE_PageProvider)}) {
    GRCORE_PageProvider p = tripwire_provider();
    p.size = size;
    GRCORE_Group * g = nullptr;
    EXPECT_EQ(grcore_group_create(nullptr, &p, &g), GRCORE_OK)
        << "size " << size;
    EXPECT_EQ(grcore_group_destroy(g), GRCORE_OK);
  }
}

TEST(PageSize, AProviderFromANewerHeaderIsAcceptedAndItsUnknownTailIgnored) {
  // Decision: accepted, as for a key. A member this library does not know is
  // one whose absence is a defined degradation, so refusing the provider
  // would only make a newer host unusable on an older core.
  g_protect_calls = 0;
  const size_t extra = 2 * sizeof(void *);
  auto * block = static_cast<unsigned char *>(
      std::malloc(sizeof(GRCORE_PageProvider) + extra));
  GRCORE_PageProvider proto = tripwire_provider();
  std::memcpy(block, &proto, sizeof proto);
  std::memset(block + sizeof(GRCORE_PageProvider), 0xA5, extra);
  auto * p = reinterpret_cast<GRCORE_PageProvider *>(block);
  p->size = sizeof(GRCORE_PageProvider) + extra;
  EXPECT_TRUE(grcore_page_provider_valid(p));
  bool mapped = false;
  EXPECT_EQ(drive(p, &mapped), GRCORE_OK);
  EXPECT_EQ(g_protect_calls, 2) << "the members it knows are used";
  std::free(block);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

# Budgeted module code pages

`GRCORE_CodePages` gives a compiled-code owner a page provider backed by its
own runtime-core group and capped independently at a fixed byte limit. The
group belongs to the code store, not to an instance. Each successful mapping
counts against both the group's meter and the code store's cap. The cap is
charged in mapped page bytes, matching the amount that must be reclaimed when
generated code is destroyed.

The code store owns its independent group. A successful page map holds an
additional object reference until its matching unmap. Code owners should also
retain the object while they hold a provider pointer; the map reference carries
it across the compiled mapping's lifetime. The final release frees the code
store and then destroys its group.

`grcore_code_pages_create(allocator, byte_cap, out_pages)` accepts a custom
allocator for that owned group; `NULL` selects runtime-core's default. The
allocator must outlive the last mapped page. Creation writes `out_pages` only
on success. A group-allocation or code-page-object failure releases any
partial owner and leaves the caller's output unchanged.

## Why the cap wraps the group provider

Using the process default provider directly would leave executable mappings
outside runtime-core's accounting. The code store creates a runtime-core group,
whose counting provider wraps the process page provider, then adds a module
cap. This preserves core accounting and lets a module share code across
instances in different context groups without charging an arbitrary instance.

The map path reserves bytes atomically before asking the backing provider for
pages. Concurrent compile requests therefore cannot pass the cap together.
Backing allocation failure refunds the reservation. `protect` forwards to the
backing provider and is absent when the backing provider does not support it;
the JIT then refuses compilation before publishing code.

## Lifetime order

The module owns one code-page object shared by all cache keys. Each cache holds
a reference to it. Every executable mapping holds another reference until
unmap, so dropping the module and cache references cannot destroy the group
provider while code is still mapped. The compiled artifact release callback
keeps another reference through `grjit_code_destroy`, because that call frees
JIT metadata after unmapping its code pages. Teardown clears context entry
slots and unregisters code ranges, then drops cache code references and the
module's code-page reference. Runtime-core's active code references carry any
returning frame through final artifact destruction.

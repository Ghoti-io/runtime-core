# Design

**Status:** In progress. Describes what exists (the scaffold and B's first
half: groups, contexts, options, keys and memory accounting); the rest is
design, taken from the runtime stack's architecture spine (AD-1 to AD-26).

## What this library is

`runtime-core` is the kernel of a microkernel: B (the execution context) and A
(the frame protocol), and no language and no service. Services (the heap, the
debugger, a profiler) register on B's poll by key, engines push frames through
A, and codegen emits against A's formats. The kernel never calls any of them
(AD-1). That is what keeps A from becoming shaped like one language: if the
kernel knew Tang's value type, the second engine would have to bend to it.

## Why one library, with two layers

A and B ship together, with separate headers (AD-3). The interface between
them is not known yet: what A needs from B will be settled by the first
engine, and splitting into two libraries now would freeze that interface in a
version number before anything has used it. The layering is real, though, and
it is enforced rather than remembered: an `a/` header may include `b/`, and
never the reverse (`check-direction`). When a real second consumer wants B
without A, the directory boundary is where the library splits.

**Rejected: two libraries from the start.** It would make the A-to-B interface
a released one before it had a client, and every change to it a coordinated
bump of two versions.

**Rejected: one flat header directory.** A single layer of headers cannot say
which part is frozen and which is free to change, and cannot be checked for
direction.

## Why labels, and why by directory

Each boundary has its own stability rule (AD-14): B is the C embedding API and
is frozen at the first tag; A's engine-facing headers, the stack-map, deopt and
barrier-descriptor formats and the IR are `free`, so a consumer requires the
exact version it was built against. A single rule would either freeze the
internals that must still move or let the embedding API break.

The label is a `@stability` tag in each header and `check-labels` fails a
header without one. It also fails a header whose label disagrees with its
directory (top level and `b/` stable, `a/` free). The directory is the
contract; a label that could be chosen freely would be a label that could be
chosen wrongly.

**Rejected: a labels table in a document.** A table that lists the headers is
not read by anything, and a new header is added to it by someone remembering.

## Why the edge gate reads two places

The spine forbids `core` depending on any service, on codegen, on an engine,
or on ctang (AD-2). `check-edges` checks every `#include` under `src/` and
`include/`, and the `NEEDED` list of every built shared object. Neither alone
is enough: the `#include` lines can be clean while a `.so` still links a
library through a build flag, and a link line can be clean while a header
reaches for a forbidden one. It is an allowlist (cutil and this library), so a
new forbidden library needs no edit to be caught.

**Rejected: checking the pkg-config manifest.** `Requires:` says what the
`.pc` file claims, and the gate exists because that claim can be wrong.

## Why every gate has a self-test

A gate that has never been seen to fail may measure nothing. `check-gates`
runs each real script against `tests/gates`: a planted defect that must fail
and name its offence, a control that must pass, and an empty tree that must
fail rather than report success over zero files. The link-line check builds a
real shared object against a stub forbidden library, so `readelf` reads a
genuine `NEEDED` entry rather than a fixture's idea of one.

**Rejected: testing the gates by hand when they are written.** It proves the
gate worked once, and says nothing when a later edit makes it vacuous.

## The result vocabulary and ERR_GUEST

The suite's nine result constants are unchanged and keep their numbers.
`GRCORE_ERR_GUEST` is appended before the count: it is returned when guest
code ends in an uncaught exception or a Wasm trap (AD-13), and the reason is
read from the context. Folding it into `ERR_INTERNAL` would make "the guest's
program threw" and "the runtime's invariant failed" one result, and a host
that must tell them apart could not. The departure is recorded in
CONVENTIONS.md section 13.

## B, part 1: groups, contexts, options, keys, accounting

The first half of B is the object a host holds and the rules about who may
touch it. It has no `run`, no poll and no requests yet; those attach to it in
later stories, which is why the lifecycle's internal transition function
exists now and is tested exhaustively.

**Groups and contexts.** A context belongs to one group from creation (AD-7)
and a group refuses to be destroyed while it has contexts. The count is
atomic because contexts of one group are created and destroyed on different
threads. A group takes a base allocator and page provider and frees with
them; the group and each context own counting wrappers over that same base
allocator and provider. Destroying a group needs quiescence: no context of it
may be created or destroyed concurrently.

**Options, not `_Limits`.** The four budgets (fuel, memory bytes, guest depth,
native depth) are set through an opaque options object (AD-13). An unset
budget is `GRCORE_UNLIMITED`, the largest `uint64_t`, so that no real budget
can be mistaken for it. Creating a context deep-copies the options, so
changing them afterwards cannot change a context that exists. Nothing is
enforced yet; the story that enforces a budget also decides what it charges.

**Rejected: a `_Limits` struct.** A field added to a struct breaks every
caller that built one. The spine reserves `_Limits` for parsers and
validators, whose limits are inputs to a pure function.

**Keys.** A key is a static object, identified by its address, carrying a
cardinality, a phase and a destructor (AD-19). A context stores
`(key, value)` pairs in registration order and knows nothing about what they
are (AD-1). A NULL value is refused, so a NULL lookup always means absent.
Teardown runs destructors in reverse registration order, so that a later
registrant, which may depend on an earlier one, is released first (AD-20).
Lookup is a linear scan: a context holds a handful of registrations, and the
scan beats a hash on that size and needs no extra allocation.

**Lifecycle.** The four public states hide a fifth configuration: "parked"
means either not inside `run`, or inside a host call bracketed by park and
unpark, and only the first can migrate (AD-20). The context stores the five
configurations; the public state folds the two parked ones. There are exactly
eight legal edges, held in one table behind one checked function, which the
tests walk over all 25 pairs.

**Ownership.** One owning thread at a time (AD-6): the creator, until
`release`. The owner is an atomic word; `release` stores zero with release
ordering and `acquire` compare-and-swaps with acquire ordering. Everything
else about a context is plain data guarded by that protocol, so a hand-off
using only those two calls is race-free and ThreadSanitizer checks it (the
test writes a plain variable on one side and reads it on the other).

**Rejected: a mutex per context.** A context is single-threaded by design, so
a lock would protect nothing the ownership protocol does not, would cost on
every access and would hide a violated protocol instead of refusing it.

**Rejected: an ownership token the caller holds** (a handle passed to every
call). It would make a thread's identity something the host must carry and
could forge or lose. The thread id is assigned lazily from a global atomic
counter on a thread's first call, so it needs no platform thread API and is
never reused: a thread that exits while owning a context leaves it owned by an
id no later thread can take. A host should release or destroy a context before
its thread ends. A paused context the host abandons can still be destroyed by
its owner.

**Accounting.** B does all memory accounting (AD-20). A meter is three atomic
counters: bytes in use, the high-water mark, and live blocks. A counting
allocator wraps a base allocator, and a counting page provider wraps a base
page provider; each charges one meter. A group has one pair, and each context
has its own, so services and engines that allocate through the context's
allocator are metered exactly. The allocator prefixes each block with a header
the size of `max_align_t` that records its size; a page mapping counts as one
block. A base allocation that fails changes no counter. The context's own
struct and its registration table come from the base allocator uncharged: they
are the runtime's, not the guest's. Context-independent code is charged to
the group (AD-13); how a context's usage rolls up into its group's is the
nested-budget story's decision.

**Page provider.** `runtime-heap` and `runtime-jit` obtain pages only through
the provider the context hands them (AD-13), so their memory is counted
without either library knowing how. The default is anonymous `mmap`,
zero-filled and read-write. A size that is zero or not a page multiple returns
NULL. Changing protection (needed for JIT, never writable and executable at
once) is not in this interface yet.

## Benchmarks

Every library ships a benchmark harness from its first commit (AD-26). This
one holds a calibration case, fixed integer work that touches no library
code, so that a figure from a real case can be read against the machine it
was taken on, and three real cases: creating and destroying a context, a
counting malloc/free pair, and a keyed slot lookup. No budgets are recorded:
the spine records them once a first measurement of a real case exists, and
the poll, the frame walk and request posting are the next cases to add.

## Continuous integration

`core` gets compress-style CI from the start (AD-16): both compilers,
ASan+UBSan, ThreadSanitizer, Valgrind, the gates and their self-test, a
coverage floor of 80, and MSYS2 on Windows. ThreadSanitizer now checks the
ownership hand-off between threads; the requests that are the one
cross-thread operation come later.

# Design

**Status:** In progress. Describes what exists (the scaffold, all of B:
groups, contexts, options, keys, memory accounting, requests and ports, the
poll, `run` and `resume`, and the budgets; and the first half of A: the guest
stack, engine descriptors, the abstract frame, the frame walk and scopes); the
rest of A (activation records, root sources, the unwinder) is design, taken
from the runtime stack's architecture spine (AD-1 to AD-26).

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

## B, part 2: requests, the poll, `run`, budgets and migration

The second half of B makes a context do something and lets the outside stop
it. It adds four headers under `b/` (`request.h`, `poll.h`, `run.h` and
`budget.h`), all labelled `stable`, and edits `key.h`, `options.h` and
`group.h`.

**Requests are the one cross-thread operation (AD-4).** Another thread never
touches a context: it posts a *request kind* through a *port*. Five kinds are
the core's: terminate, time, interrupt, fuel and memory. Fuel and memory are
*derived*: they are recomputed from the budgets at every slow poll, so they
are levels that cannot be lost and cannot be posted. Time and interrupt are
*edges* the host acknowledges, and `resume` clears them. Terminate is sticky
until `run` returns (AD-21). A service defines its own kinds against its own
key (`grcore_context_request_kind`), which is what makes every bit of the
request word attributable (AD-19), and it clears them itself.

**The request word and the overflow set.** Kinds below 63 are bits of one
atomic 64-bit word, which is all the poll's fast path reads: one load and one
branch, no mutex. A kind at or above 63 lives in a bit set beside the port,
guarded by the port's mutex, and is announced by the word's top bit. So any
number of kinds is allowed, memory permitting, and the fast path is the same
instruction sequence however many exist; only a service that asks about an
overflow kind pays for the lock.

**Ports.** A port is reference-counted and owned by the group, and it is
charged to the group's meter, because it can outlive the context it points at.
It holds a mutex, a condition variable and a context pointer that is cleared
under the mutex when the context is destroyed. A post takes the mutex, sets
the bit and signals; if the context is gone it is refused with `ERR_INVALID`
and touches nothing, so a late post against a dead context is harmless and the
last release frees the port. A context that is waiting (`grcore_context_wait`)
sleeps on that same condition variable, and a post sets its bit under the same
mutex, so a wake cannot be missed. The group refuses to be destroyed while a
port is live. The mutex is the reason this library now links `-pthread`.

**Rejected: a mutex or a condition variable on the poll.** The poll runs at
every loop back-edge and function entry. A lock there would cost on a path
that is nearly always empty, and the JIT cannot emit one inline.

**Rejected: a posted request that mutates guest state.** A foreign thread
writing into a running context would race the guest. A request only sets a
flag; the owner reads it at a poll and decides.

**Rejected: one request word per kind.** It would make the fast path a loop,
or a wider load, and the number of kinds would be fixed by the layout.

**The poll and its four phases (AD-5).** A poll with a request pending takes
the slow path, which `core` owns: DECIDE, ACT, OBSERVE, YIELD, in that order.
A service registers a handler under a key whose phase says which. DECIDE
handlers *vote* (continue, pause or unwind; the strongest wins), and the votes
are levels: they are made again at every poll. ACT carries the verdict out (a
collector runs here), OBSERVE is read-only, and YIELD runs only when the
verdict is continue and may still raise it. A handler is given an opaque
`GRCORE_PollCall`, never an API that allocates, so DECIDE and OBSERVE cannot
reach a collection point.

**Verdicts do not depend on handler order.** A vote is stored against the
handler's *registration index* (the built-in kinds first, then each
registration), not appended in the order handlers ran. The winning key list
and the unwind result are read off that table afterwards in index order, so
what a pause reports is "built-in keys by kind, then registration order"
whatever order DECIDE ran in. The phase-shuffle mode runs DECIDE and OBSERVE
in a seeded random order to prove it: a handler whose vote depends on who ran
before it produces two different key lists over 64 seeds, and a test plants
exactly that and watches it be caught. The same planted handler looks fine
with the mode off, which is why the mode exists.

**Rejected: handler-registration order for verdicts.** The first service to
register would then win every tie, and two services could change each other's
behaviour just by being loaded in a different order.

**Memory defers its verdict by one poll.** The allocator that takes the
context past its budget is still served, from a bounded reserve
(`GRCORE_DEFAULT_MEMORY_RESERVE`, 262144 bytes), and raises the memory
request. Because the collector is an ACT handler and ACT follows DECIDE, the
first poll over budget tells ACT handlers to reclaim and votes nothing; if the
context is still over afterwards, the next poll votes to pause. Going back
under the budget clears the mark. Past the reserve the allocator returns NULL
and charges nothing (the caller's `ERR_OOM` or `ERR_LIMIT`); the refusal is
counted. The check and the charge are not one atomic step, so only the owning
thread's own allocations are exact, which is all a context needs.

**`run` and `resume`; why a pause is not a longjmp.** `run` takes an *entry
function* the engine supplies and returns `OK` with an outcome of finished or
paused; an unwind returns `ERR_LIMIT` or `ERR_GUEST` and the reason is read
from the context (AD-13). A pause leaves no host C frame above `run`: the
entry sees a pause verdict, saves its position in its own state (a real
engine keeps it on the guest stack) and returns `STEP_PAUSED`, and `resume`
calls the same entry again. That is what makes it legal to release a paused
context and acquire it on another thread (AD-20), which the tests do many
times over. `run` checks what the entry reports against what the poll
decided: an entry that returns paused without a pause verdict, or finished
after one, is a bug that would otherwise leave the context in a state no later
call can interpret, so it is `ERR_INTERNAL` and the context is left parked
outside `run`.

**Rejected: pause as a C `longjmp` or a second native stack.** It pins the
thread, so the context could not migrate, and a stack that outlives its
frames is the option (c) the spine reserves behind A's guest-stack
abstraction for a later, deliberate step.

**The runtime poll (AD-21).** A native whose work the guest controls (a
regex, a sort, a copy) calls `grcore_runtime_poll` every bounded amount of
work. It charges that work as fuel, then polls with pause forbidden: a pause
verdict becomes an unwind with `ERR_LIMIT`, because a native cannot return to
the host from the middle of C code. The keys that wanted the pause stay
readable as the reason. Terminate unwinds from here too and stays pending
until `run` returns.

**Fuel, depth.** Fuel is exhausted when *more* has been used than the budget,
so a budget of N pays for exactly N units. Use is kept when the budget is
raised, so raising by k buys k more. Depth is enforced where it is entered
(`ERR_LIMIT`, depth unchanged).

**The fast path checks no state.** A poll with nothing pending returns at once
after one load, so a poll made outside `run` is only noticed when something is
pending; the slow path then refuses it. The owner of an engine calls the poll
only from inside its entry function, and a check on every call would cost the
branch the JIT's inline test must not pay. The poll also does not check for a
NULL context.

**A new field on `GRCORE_Key`.** The poll handler is a trailing `poll` field.
C zero-fills an aggregate initialiser that omits it, so a key written before
the field existed keeps its meaning (a NULL handler is never run), but a C++
compile with `-Wextra` flags the omission. The key is still a public
fixed-layout struct that callers define statically; making it an accessor-built
object is deferred until before the first release.

## A, part 1: the guest stack, engine descriptors, the abstract frame and scopes

The first half of A adds three headers under `a/` (`engine.h`, `stack.h` and
`frame.h`), all labelled `free`: a consumer requires the exact version it was
built against (AD-14). They may include `b/` and the top-level headers, and
nothing in `b/` includes them; only the umbrella does. Before this a paused
context had nowhere to keep its position, no consumer could read a frame
without knowing the engine, and a poll could not say where in the guest it
was.

**The guest stack is a byte buffer of frames, named by offsets (AD-8, AD-17).**
A call from guest code to guest code pushes a frame, so an interpreter never
recurses in C to make it, and a pause is just a return: the whole position is
data on the stack. A frame is a 40-byte header (the caller's offset, the poll
identity, the frame size, the engine id, the slot count, a tag) followed by 64-bit slots. A
consumer holds a `GRCORE_FrameRef`, an offset, never an address, because the
stack grows by allocating a bigger buffer, copying and freeing the old one.
`grcore_stack_slots` hands out an address anyway, for an interpreter's inner
loop, and the header says in one sentence when it dies: at the next push, pop,
reserve or poll, each of which is a GC point. The interface is opaque
(`GRCORE_Stack`, `GRCORE_FrameRef`, accessors), so a per-context native stack
segment (option (c), reserved by AD-8) can later sit behind the same calls.

Growth copies instead of calling `realloc` on purpose. `realloc` may extend in
place, in which case a bug that holds a slot address across a push would pass
every test and fail in production the first time the allocator moves the
block. A copy always allocates the new buffer before freeing the old one, so a
move always changes the address, and `grcore_stack_set_always_move` makes
every push a move so that a test does not have to wait for growth to find the
bug. A tag in each header, derived from the frame's offset, makes a
reference that was never a frame (a stale offset, a number someone typed, the
middle of a slot) fail the check with high probability. It is a guard against
accidents, not against a hostile caller, and the header says so.

**The stack is per-context state registered by key (AD-1, AD-19).** The first
`grcore_engine_register` creates it and registers it under a cardinality-one
key of A's whose destructor frees it, so B created a context without knowing A
exists and B's headers do not include A's. It is created at engine
registration because registration is refused while the context is running, and
the stack must exist before the first push. Everything is allocated through
the context's counting allocator, so it is charged to the context: a growth
the memory budget refuses returns `ERR_LIMIT`, any other refusal `ERR_OOM`,
and in both the stack and the depth are as they were. A push enters the
guest-depth budget and a pop leaves it, so the depth limit counts frames, as
ctang's does (AD-16, AD-21). The stack migrates with the context, so a context
paused with frames can be released on one thread and resumed on another, and
the migration test checks the output against an uninterrupted run.

**An engine descriptor says what a frame means (AD-18).** Every frame header
names its engine, and the engine registered one static descriptor: its name,
the kind of each slot (`RAW`, or an engine `VALUE`), a locator that names a
poll identity as a source location, an inspector that prints a value, a scope
interface, and a conservative decoder (mask, shift, base) for its value
encoding. A callback that is NULL means the engine has nothing to say, and the
reader falls back (hexadecimal for an uninspected value). Fields that later
stories need (root enumeration, unwinding, deoptimization) are trailing: C
zero-fills a descriptor written before they existed. They are not in the struct
yet because a field with no reader is a promise nobody has tested.

**One abstract frame, one walk (AD-18, AD-20).** `grcore_frame_walk_*` yields
a `GRCORE_AbstractFrame`: context, engine, descriptor, poll identity, the
location the descriptor gives that identity, slot count and depth. Slots,
scopes and variables are read through accessors that ask the descriptor, so the
debugger, the collector, the frame-level differential and the recorder all read
one thing. Reading is two-tier: the engine that owns the running context uses
the stack accessors in any state; every other consumer uses the walk, which is
refused unless the context is at-poll or paused and the caller holds it. The
accessors repeat the check, but an abstract frame is valid only until the
context next runs; after that, results are unspecified.

**The poll names where it is (AD-18).** `grcore_stack_poll(context, function,
offset)` records the identity in the top frame and polls. The descriptor's
`locate` is called only when a request is pending, so the fast path is
`grcore_poll`'s own load and branch plus a keyed lookup and two stores. With no
frame it polls as `grcore_poll` does, with the location `{NULL, 0}` and no
identity. An engine records the call-site identity on a caller's frame with
`grcore_stack_set_identity` before it pushes the callee, so a walk can say where
each outer frame is.

Alternatives considered and rejected:

- **Frame pointers instead of offsets.** A pointer into a buffer that grows is
  a dangling pointer in waiting, and AD-17 forbids raw pointers across a GC
  point. Offsets are the only link that survives a copy, and they leave option
  (c) open.
- **`realloc` for growth.** See above: it hides the very bug the offsets exist
  to prevent.
- **One descriptor callback per consumer** (one for the collector, one for the
  debugger, one for the differential). It would make the descriptor grow with
  every consumer. One abstract frame with a few reads serves all of them, and a
  new consumer needs no new callback.
- **Engine-specific frame readers.** Then every consumer would know every
  engine, which is the coupling AD-18 exists to prevent: the second engine
  would have to be taught to each of them.
- **The stack as a field of the context.** B would have to name A's type, and
  B's headers would include A's. A keyed slot costs a linear lookup on a cold
  path and keeps the direction one way, which the direction gate enforces.
- **A heap-allocated frame per call.** An allocation per call, a free per
  return, a pointer per link, and frames scattered where a walk and a copy
  would have to chase them. A contiguous buffer is cache-friendly, is freed
  with one call, and is what a native segment would also be.

## Benchmarks

Every library ships a benchmark harness from its first commit (AD-26). This
one holds a calibration case, fixed integer work that touches no library
code, so that a figure from a real case can be read against the machine it
was taken on, and nine real cases: creating and destroying a context, a
counting malloc/free pair, a keyed slot lookup, the poll's fast path, the
poll's slow path through one handler in each phase, a post to a port, a push
and pop of a guest frame, the engine-aware poll's fast path, and a walk of
sixteen frames at a pause (reported per frame read). No budgets are recorded:
the spine records them once a first measurement of a real case exists.

## Continuous integration

`core` gets compress-style CI from the start (AD-16): both compilers,
ASan+UBSan, ThreadSanitizer, Valgrind, the gates and their self-test, a
coverage floor of 80, and MSYS2 on Windows. ThreadSanitizer now checks the
ownership hand-off between threads, concurrent posters, a post racing the
context's destruction, a watchdog stopping a running guest, and a paused
context resumed on another thread.

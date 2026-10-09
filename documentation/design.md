# Design

**Status:** In progress. Describes what exists: the scaffold; all of B (groups,
contexts, options, keys, memory accounting, requests and ports, the poll, `run`
and `resume`, the budgets with their fuel scopes, and root sources); and all of
A (the guest stack, engine descriptors, the abstract frame, the frame walk and
scopes, activation records, the unwinder and budget scopes), and context
snapshots (the snapshot object and the hooks on keys, and the guest stack's own
hooks), and the sampling profiler (an OBSERVE service with an optional timer). What is not here is the collector, the JIT and the debugger, each a
library of its own, taken from the runtime stack's architecture spine (AD-1 to
AD-26).

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
NULL. A provider also has a trailing `protect` member, `grcore_page_protect`
being the checked call: it flips a mapping between read-write and read-execute
(never both, AD-13), which is what a JIT does once after filling its pages.
NULL means unsupported and is `ERR_INVALID`; a provider that reports failure is
`ERR_IO`. The default uses `mprotect` (`VirtualProtect` on Windows, which has
run under wine and not yet on a Windows machine), and the counting provider
forwards the call
unchanged and charges nothing, since protection moves no bytes. The member is
an addition to a `stable` header, made because a JIT cannot exist without it;
nothing was released, and a provider states its own `size`, so one written
before the member existed has it absent (see "The other caller-filled structs
state their size", below).

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

**The clock a timeout is measured on.** A timed wait is
`pthread_cond_timedwait`, which takes an absolute time on the clock the
condition variable was made with. On POSIX that is `CLOCK_MONOTONIC`, so a
timeout does not move when the wall clock does. winpthreads (MinGW) accepts
only `CLOCK_REALTIME` for a condition and answers `EINVAL` for
`CLOCK_MONOTONIC`; port creation used to read that as out of memory and so
failed outright. On Windows the condition and its deadline are therefore both
on `CLOCK_REALTIME` (`src/b/cond_clock_internal.h` is the one place the choice
is made), and a timeout that is pending when the system clock is stepped is
shortened or lengthened by the step. The profiler's timer thread is the other
user and has the same property.

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

**What is pending does not depend on handler order either.** A service clears
its own request in its handler (the profiler, the JIT's tier-up, the debugger do),
and a handler that ran after it used to be told the request was gone, so who saw a
request depended on who ran first. The poll now copies the request word and the
overflow set when it starts (the copy is sized when a kind is defined, so a poll
still never allocates), and `grcore_pollcall_pending` and the built-in keys read the
copy: every handler of one poll is told the same thing, a post that arrives while
the poll runs is told at the next poll, and `grcore_context_request_pending` stays
the live read for the owner outside a poll. The shuffle tests plant a handler that
reads the live state and catch it as two different answers over 64 seeds; the
lang-tang profile test, which had been written around the one order that worked, is
now run under sixteen seeds and must give one report.

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

**A key states its size, so the struct can grow (AD-14, AD-19).** A
`GRCORE_Key` is a public fixed-layout struct that every library defines as a
static initialiser, and it has grown twice (`poll`, then the three snapshot
hooks), each time forcing an edit of every static key in every library. After
the first release a new trailing field would also be a binary break: a key
compiled before the field existed cannot be told from one with garbage in it.
So the first member is `size`, the `sizeof(GRCORE_Key)` of the header the key
was built against, and the initialiser macro `GRCORE_KEY_INIT(...)`
(`{ sizeof(GRCORE_Key), ... }`, a constant expression in C17 and C++20) writes
it, so a definition cannot forget it. The core reads a member after `destroy`
only through accessors that check `size` covers the member's end
(`GRCORE_KEY_HAS`) and take an absent one as NULL; `name`, `cardinality`,
`phase` and `destroy` are the first-generation layout, `GRCORE_KEY_MIN_SIZE`,
and are always read.

Registration (`grcore_context_register` and `grcore_context_request_kind`)
calls `grcore_key_valid` and refuses with `ERR_INVALID`, before anything is
read or stored, a key whose `size` is below `GRCORE_KEY_MIN_SIZE` (which
includes zero, the forgotten size) or is not a multiple of the struct's
alignment (no `sizeof` can be). A `size` larger than the core's struct is
*accepted* and the members past the ones this core knows are ignored: a newer
library on an older core should lose a hook, not the whole key, so every future
member must be one whose absence is a defined degradation (a hook not run),
never a requirement. A definition written in the old positional form starts
with a string where there is now a `size_t` and does not compile, which is the
intended one-time break; the five libraries' keys, tests, examples and the
planted-defect patch were converted in the same change.

Rejected: a *trailing* size or version (code built before it was added never
wrote it, so it is garbage to the reader and indistinguishable from a real
value); a version number (it is bumped by hand, says nothing about which
fields exist, and two numbers must be compared where one size answers
directly); a pointer to a vtable (a second static object per key, an
indirection on the poll's per-key loop, and the key's identity, its address,
would be the pointer's holder and not the data); designated initialisers only
(nothing makes a caller write `.size`, C++20 requires declaration order and
forbids mixing positional and designated forms, and an omitted size is found
only at run time); and keeping the trailing, zero-filled scheme (the status
quo, which cannot be told from garbage and breaks every definition for the
`-Wextra` warning on each new field). The tests (`test_key_size.cpp`) build
older-layout keys as a heap block exactly as large as their `size`, so a read
past the end is a finding under ASan and Valgrind, and arm every member after
the stated size with a tripwire, so a core that ignores `size` fails a plain
run.

**The other caller-filled structs state their size.** `GRCORE_Key`'s rule (above)
applies to every public struct that a host or another library *defines* and the
core *reads*, and that has grown or can grow at the end:
`GRCORE_EngineDescriptor`, `GRCORE_PageProvider`, `GRCORE_RestoreEnv` and
`GRCORE_RootSource`. Each has a leading `size` written by an initialiser macro
(`GRCORE_ENGINE_DESCRIPTOR_INIT`, `GRCORE_PAGE_PROVIDER_INIT`,
`GRCORE_RESTORE_ENV_INIT`, `GRCORE_ROOT_SOURCE_INIT`), a `..._MIN_SIZE` (the
first-generation layout: through `decoder`, `unmap`, `lookup`, `enumerate`), a
`..._HAS(p, member)` that says whether `size` covers a member, and a `..._valid`
function, which registration, group creation and restore call before anything
is read. A member after the minimum is read only where `size` covers it, an
absent one being NULL (a descriptor's `roots` and `unwind`, a provider's
`protect`, an environment's `entry`, `entry_state` and `pause_file`); a `size`
below the minimum, zero included, or off the struct's alignment is refused with
`ERR_INVALID`; a larger `size` is accepted and the unknown tail ignored. The
reasons and the rejected alternatives are the key's. Three points are specific:
a provider and an environment are often filled by assignment to a zeroed
struct, so their macro is also the starting point of that (`size` is part of the
initialiser, and a `{}` left at zero is refused, not read); the core's own
counting provider is built at full size, and forwards `protect` only if the
provider under it states one; and `GRCORE_RootSource` has no member after its
first generation, so its tests cover the rule (initialiser, refusal, a newer
size) but have no older layout to copy. `GRCORE_RestoreEnv` is the one of the
four that has already grown (`entry`, `entry_state`, `pause_file` after
`lookup`), which is why it is here and not left to zero-filling. Not given a
size: the contexts, options, groups and ports (opaque, built by setters,
AD-13), the root *visitor* (built by the collector and read by sources, and
without a member after the first generation), and A's `free` formats, whose
reader and writer require the same release (AD-14): the code-metadata tables
(which `runtime-jit` writes, and which carry a format version of their own) and
the layout descriptor (which the core writes). The tests
(`test_engine_size.cpp`, `test_page_size.cpp`, `test_restore_env_size.cpp`,
`test_root_source_size.cpp`) follow `test_key_size.cpp`: older-layout copies as
heap blocks exactly as large as their `size`, armed tripwires past the stated
size, a full-size control for every refusal, and a larger-size case.

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
and in both the stack and the depth are as they were. The exception is the
bookkeeping of compiled code (the code registry, the entry slots, the retire list
and the deopt reservation): that is the engine's and not the program's, so it is
allocated through the group's allocator and is not on the context's budget (the
compiled-code pages themselves are the engine's to map through the group's page
provider, for the same reason). A push enters the
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
stories need are trailing, and the descriptor states its own `size`, so one
written before they existed has them absent. They were not in the struct until a story read them, because a field
with no reader is a promise nobody has tested; root enumeration and unwinding
are in it now (part 2), and deoptimization is not: the stack-map and deopt
format is a table in `a/codemeta.h`, not a descriptor field.

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

## A, part 2: activation records, root sources, the unwinder and budget scopes

The second half of A adds three headers under `a/` (`activation.h`, `unwind.h`
and `budget_scope.h`, all `free`) and one under `b/` (`roots.h`, `stable`), and
adds to `budget.h` and `poll.h`. Before this nothing recorded how control
crossed between host, interpreter, JIT and C; nothing let a collector find a
context's roots; nothing took the stack apart when a run ended early; and fuel
was one flat budget, so a runaway template call could not be stopped at its own
boundary (AD-5, AD-17, AD-18, AD-21).

**Activation records (AD-17).** Every crossing records a record on the stack's
array, entered and left in strict LIFO order: HOST, INTERPRETER, JIT, NATIVE and
REENTRY. A record is named by a serial that is never reused, so a stale or
forged reference is refused where a stale frame offset could name a later
frame. It holds counts and a segment, never an address into the guest stack, so
growth cannot invalidate it. A JIT, NATIVE or REENTRY record enters the native
depth budget and its leave gives it back (AD-21); HOST and INTERPRETER do not,
because an interpreter makes no C call to enter a guest frame (AD-8). A leave
is refused unless the stack is back at the frame count the record began with
and no scope opened inside it is still open, so the two LIFO disciplines
cannot cross. A record stays open across a pause, which is what lets a
context paused inside nested scopes and activations migrate and resume.

**The nesting count, and why it lives in B (AD-5).** A nested activation (a
REENTRY always, an INTERPRETER when the caller says so) cannot pause to the
host, because there is a C frame of the host's, or of an opaque function's,
above `run`. B owns the poll, so B owns the count: `grcore_context_nested_enter`
and `leave`, called by A's records. While it is above zero the poll treats
`allow_pause` as false, exactly as the runtime poll already did, so a pause
verdict becomes a limit unwind and the keys that asked for the pause stay
readable. Terminate needed no change: it is an unwind vote that stays pending
until the outermost `run` returns, so the nested poll unwinds, and so does the
outer one after the nested unwind and its leave. Nothing evaluates while
paused and no YIELD run is made yet (those are later stories); the count and
the rule are here so they will have somewhere to stand.

**Asking whether a pause is possible.** A YIELD handler that votes a pause only
to give the host a look (the debugger) must not vote one the poll will refuse:
core turns a refused pause into a limit unwind, which would end the program the
handler meant only to inspect. The runtime poll and a nested activation both
refuse, and the handler cannot see either from its arguments, so
`grcore_pollcall_pause_allowed` reports what the poll already decided. It is a
read of one flag in the poll's record and changes no behaviour.

**Root sources (AD-11, AD-18).** B holds a table of `(source, value)` pairs and
a function that asks each in turn. A source reports two things as plain data: a
pointer to a 64-bit value slot (a visitor may write through it, which is how a
moving collector will update a slot in place) and a conservative range
(`lo`, `hi`, `mask`, `shift`, `base`). `gc` reaches them only through B's types
(AD-2). A's source is registered with B by the first engine registration, before
the keyed registration of the stack, and removed again if that fails, so a
refusal leaves no source pointing at a freed stack. It reports every VALUE slot
of every frame, innermost frame first, then calls the frame's engine `roots`
hook for what a slot kind cannot say (an open upvalue, a value held in a side
table), then one range for each activation that recorded a C segment, with its
engine's decoder; engine zero has none, and its words are read as the
addresses they are. JIT frames are not scanned conservatively and a record
without a segment contributes nothing (AD-17).

`grcore_context_root_source(index)` reads one entry of that table, because the
all-at-once enumeration hands every root to one visitor and cannot say which
source a root came from. A consumer that must say so (the heap's retention
query names the source and a slot's ordinal in its enumeration) calls the
source's own `enumerate` with a visitor of its own, which is what the
enumeration does for each entry. It adds no new way to reach a root.

**The unwinder (AD-5, AD-18).** `grcore_unwind_to_activation` and
`grcore_unwind_all` pop frames innermost first, calling the engine's `unwind`
hook while the frame is still on the stack, leave the deeper activations (which
gives back their native depth and nesting) and close the deeper scopes. They run
no guest code: a limit unwind cannot be caught and runs no `finally`. They edit
data; the C frames above `run` unwind by returning, which is why nothing crosses
a host frame. `unwind_all` is the end of a run, after an unwind and after a
finish alike, and so it also settles the case story 4 deferred: frames left on
the stack when a run finishes leave a stale used depth for the next run.

**Fuel scopes (AD-21), counted in B.** A scope has an *exclusive* budget: a
charge goes to the innermost scope only, so the parent's clock stops while a
child runs, and every charge also counts against the existing fuel limit, which
is the *inclusive* request ceiling. The poll turns exhaustion into a verdict by
the same FUEL request bit as before; the handler tells the cases apart. When the
ceiling is exhausted, or the scope's policy is PAUSE, the verdict is a pause,
so the host can raise the ceiling or the scope's budget. When only a scope is,
and its policy is UNWIND, the verdict is an unwind with `ERR_LIMIT` and the
`fuel` key as its only voter. The poll records that as a *scoped* unwind, naming
the scope. The engine then unwinds its frames to the scope's boundary and
closes the scope; closing it clears the scoped unwind, so `run` does not hold a
terminal one and a FINISHED return afterwards is legal. An unwind with another
voter (terminate), or a pause refused because the activation is nested, is not
scoped and closing a scope does not clear it. Because the votes are levels, the
ceiling beats the scope: with both exhausted the poll pauses, and once the host
raises the ceiling the next poll finds the scope still exhausted and unwinds.

**Budget scopes in A (AD-21).** B counts; A adds what only A can know: where on
the guest stack and among the activation records the scope began. Open reserves
room for A's record first, then opens B's scope, so the two succeed or fail
together. Close needs the scope innermost, the stack back at its frame count
and no activation opened inside it still open. `grcore_budget_scope_unwind` pops
to the base, leaves the activations opened since, closes the scopes inside it
and then closes the scope. A fuel scope opened straight through B is not A's to
track; closing or unwinding an A scope closes any left open inside it, and
`unwind_all` closes them all.

**Two new trailing fields on the descriptor.** `roots` and `unwind` follow the
decoder. Like the key's `poll` field, a descriptor written before they existed
keeps its meaning (the descriptor's `size` stops before them, and an absent hook
is never called); see "The other caller-filled structs state their size".

**Nothing raw survives (AD-17).** Records and scopes are arrays that grow by
copy through the context's counting allocator, as the stack does, so they are
charged to the context: a growth the memory budget refuses is `ERR_LIMIT`, any
other refusal `ERR_OOM`, and nothing has changed. A hook's abstract frame has
no `location` (a collector calls `roots` on every frame at every collection,
and `locate` is engine code), and a hook must not push, pop or poll.

Alternatives considered and rejected:

- **Scopes counted in A.** B does all the accounting (AD-20), the poll that
  decides is B's, and the ceiling those charges must also count against is
  B's. A would have had to charge B's counter and keep its own, and the two
  could disagree. The scope is B's; only its position is A's.
- **Activation records on the guest stack.** They would be frames, and the
  frame walk, the depth budget and every consumer would have to tell them from
  guest frames. They are not guest frames: JIT and C activations are not on the
  guest stack at all, and a record must be able to outlive the frames above it.
  A separate array, with the frame count as a field, keeps both simple.
- **A root-source key instead of a registry.** A key finds one slot of one
  kind, and the collector would have had to know every library's key to find
  the roots, which is the knowledge AD-2 forbids. A table of sources is one
  call the collector makes whatever is registered.
- **A new request kind for scope exhaustion.** The FUEL bit already means "fuel
  needs a verdict", and a new kind would change the core kind count, the
  fast-path word and every existing test. The cost of reusing the bit is that
  the `fuel` key reports both a ceiling and a scope, which is accurate: it is
  fuel either way.
- **Unwinding by `longjmp`.** It would cross host C frames (AD-13), skip any
  cleanup between, and pin the thread. The unwinder edits data and the C frames
  return.
- **Having `run` unwind to the scope's boundary.** Only the engine knows its
  frames and what its caller does about a failed call (a limit-error value, an
  error-list entry), so the engine unwinds and `run` learns it is over when the
  scope closes.

## A, part 3: the code-metadata format and the JIT layout descriptor

The first JIT (`runtime-jit`) emits against formats that live here, so the
reader of a compiled frame (the collector's root source, a pause-time rebuild)
exists before any one writer does (AD-14, AD-17). Both headers are `free`.

**`a/codemeta.h`: stack maps and deopt records.** A *site* is a place in the
emitted code, named by a code offset that is the return address of a call or
of a poll's slow call, and the start of the exit stub of a guard. It is one
of five GC-point kinds (AD-17: poll, allocation slow path, call out of guest
code, frame push, nested entry) or a guard. A site carries the poll identity
`(function, offset)` of AD-18, the stack map (the live references as
frame-slot locations with their slot kind), derived pointers as `(slot, base
slot, delta)` triples (AD-12), and the deopt frame state: one location per
interpreter slot, a frame slot, a 64-bit constant or dead. The format says
*frame base*, never a register: the x86-64 baseline's is `rbp` and the arm64
baseline's is `x29` (the table is the same for both, byte for byte), and a
backend that keeps its frame elsewhere fills the same table. The frame lies below the
base, a slot is named by the byte offset of its lowest byte (`-8`, `-16`,
...), and a table's `frame_bytes` bounds them. A table is immutable and owned
by its builder; `grcore_codemeta_validate` is the one parser of it. It checks
the version, offsets (strictly increasing, inside the code), slot alignment
and range, that a derived pointer's base is one of the site's live references,
and that two sites of one function agree on the interpreter slot count, and it
reads nothing outside the table. Its cost is sites times distinct function
identities (the identity check scans earlier sites rather than allocate); the
tables a code generator makes name one function. `grcore_codemeta_find` is a
binary search for a site at exactly one offset. The descriptor in `engine.h`
still has no deoptimization field: the metadata is a table the engine hands to
whoever reads it, not a hook, and the reader is `a/deopt.h`.

**`a/layout.h`: what emitted code may know about a context.** Compiled code is
shared between contexts and reaches state through a context register (AD-22),
so the offsets it may bake in must be stated, not inferred from a shared
header (AD-19). `grcore_jit_layout` returns a static struct with one entry:
the byte offset and width of the request word the poll's fast path loads, and
the rule that non-zero means slow. It is computed in `src/b/` from the real
`offsetof`, and a test reads the word through the offset on a live context.
There is no keyed-slot offset, on purpose: keyed state is found by key. A code
generator reads the descriptor when it compiles and stores the offset it used,
so code is valid for exactly the build that produced it.

**`a/code.h`: reference-counted compiled code.** AD-13 says the core owns the
count of compiled code, and AD-22 says compiled code is shared between
contexts, so a retain and a release can race on one handle from two threads.
`GRCORE_Code` is an opaque handle over a payload the code generator gives it
and a release function; creation sets the count to one, `retain` is a relaxed
increment and the `release` that reaches zero is acquire-release, so the thread
that frees the payload sees every write the other holders made to it (the same
discipline as `GRCORE_Port`). The count is the one place the library lets two
threads touch an object without an owner, which is why the header says so.
The handle copies the allocator it was created with, so the caller's
allocator pointer need not outlive it. The rejected alternative is a count
inside the code generator's own object: every generator would write the same
atomic, and an engine that holds code from two generators would have two
counting disciplines.

**`a/deopt.h`: reading and writing a native frame by its metadata.** The
frame-state half of a site says where each interpreter slot can be read, and
`grcore_deopt_read` is the one function that does it: for a frame slot the word
at `frame_base + offset`, for a constant the immediate, for a dead slot zero.
`grcore_deopt_write_back` is its inverse for the reference slots only: it stores
into a frame word named by a `FRAME_SLOT` location of kind `VALUE` and leaves raw
words, constants, dead slots and every unnamed word alone. The pair exists so
that an engine's poll can make the interpreter's own frame current from a
compiled one (and the debugger and collector then see an ordinary frame),
and then let a collector that updated a reference in that frame in place be
honoured by the compiled code that continues. They are pure and trust a table
that passed `grcore_codemeta_validate`. The rejected alternative is leaving the
reader to each code generator or engine: the format is the core's (AD-14), and
a reader written beside each writer is how a writer and a reader come to agree
only with each other.

**Representation-tagged frame state (AD-27, CAP-9 of the calls spec).**
A frame-state location carries a representation after its three older members:
`BITS` (zero), `I32`, `I64`, `F32` or `F64` (`GRCORE_Representation`, declared
in `a/engine.h` because the descriptor's callbacks take it, and used by
`a/codemeta.h`). Zero is `BITS`, so a location a producer built from before the
field existed, or zero-initialised, means "copy the word verbatim", which is what
pc, sp and fuel always meant; every existing producer and consumer therefore
behaves as it did. A representation is never a new `GRCORE_SlotKind`: whether a
word is a reference is the collector's question and is answered by the slot
kind alone, and a raw `I32` must never be answerable as "a reference" by any
reader of that enum. The validator refuses a representation out of range, a
converting one on a `VALUE` location (a raw value is not a reference), on a
`DEAD` location (nothing to convert) and in a stack map (a raw value never
appears there as a reference), and `grcore_codemeta_validate_at` says which
site and which entry. The format version stays 1, by decision: the library is
unreleased, so no table outside the tree exists to misread, and every producer
in the tree was updated with the struct, which grew at its end.

The engine descriptor gains `convert` and `reverse` at its end, read only where
`size` covers them (an older descriptor reads as having neither, so it converts
nothing and binds only `BITS` code). `grcore_deopt_bind` is the binding check:
code with a converting location is refused with `GRCORE_ERR_UNSUPPORTED`, the
reason naming the representation, unless both callbacks exist. A missing
conversion is an error and never a default, because the plausible default (copy
the bits) is exactly a raw word reaching a slot the engine reads as a value.
Reading shows a raw slot with its representation (`grcore_deopt_read_tagged`)
and converts nothing, so inspection, DECIDE and OBSERVE handlers never cause a
conversion.

A rebuild (`grcore_deopt_rebuild`) is two-phase and draws on a reservation
(`grcore_deopt_reserve`) taken when compiled code is entered, sized by the
compiler for the most converting locations live at any site. It reads every
non-converting word and writes null into every converting slot; converts each
raw value, through `convert`, into a cell of the reservation, which is
registered as a root source so a collection sees it; and only then copies the
cells into the slots. Every refusal (a short reservation, a missing callback, a
wrong count) happens before the first write, so after that the rebuild cannot
fail, allocates nothing and does not collect, and a collector sees at any
moment null or a converted value, never a raw word and never a half-written
frame. Write-back (`grcore_deopt_write_back_checked`) installs each converting
slot through `reverse`, which is the engine's type test: being representable
is not enough (an integer slot does not accept a float whose bits would read as
one). All values are tested before any word is written, and a value that fails
leaves the frame untouched and reports `GRCORE_DEOPT_EXIT_AT_SITE`, which the
engine maps to its existing exit-at-this-site path. The old
`grcore_deopt_write_back` is unchanged (reference slots only). A reservation must be released
(`grcore_deopt_release`, owner thread only) before its context is destroyed:
destruction frees the root-source table but cannot know the reservation. The
rebuilt `slots` are the guest frame's own array, which the collector already
scans; the reservation holds a value only while it is converted.

Rejected: a representation as another `GRCORE_SlotKind` (the collector's enum
would then carry cases it must never act on); converting at every poll (a poll
with nothing pending would pay for values nobody reads); a default conversion
that copies the bits (above); allocating the converted values from the engine's
heap during the rebuild (a rebuild could then fail or collect, with the frame
half raw); and bumping the format version (nothing outside the tree to protect,
and the version tests would then measure nothing).

## A, part 4: the precise walk of compiled frames (AD-17, AD-28, CAP-2)

Compiled code calling compiled code leaves frames on the native stack that no
guest stack records, and the collector, the debugger and the frame differential
must still see every one of them precisely at every GC point. Before this part
a frame base was a caller-supplied pointer, an activation record held neither
a frame base nor a return address, nothing mapped a return address to compiled
code, and nothing kept code alive while a frame returned into it. The new
headers are `a/registry.h` and `a/compiled.h`; `a/activation.h`, `a/frame.h`
and A's root source gain what they need. All are `free`; the metadata format
version stays 1 and `BITS`-only and interpreter-only behaviour is unchanged
(a record that carries no compiled state is a record that never did).

**The frame layout contract.** The one thing the core trusts about a compiled
frame, for every backend's internal calling convention: the word at the frame's
base holds the caller's frame base, and the word after it the return address
(`rbp` on x86-64, `x29` with the saved link on arm64). The frame lies below its
base, as `a/codemeta.h` already says. A frame is found from a return address
inside registered code; its stack map is `grcore_codemeta_find` at the offset
of that address. A frame's extent, which a conservative scan must leave out, is
from its base less the code's `frame_bytes` up through the two words. Nothing
unwinds natively through a compiled frame, and nothing else about a frame (its
saved registers, its alignment padding) is read.

**The registry** (`a/registry.h`) is per context: a sorted array of
`[start, end)` ranges, each with the engine whose frames the code runs, the
code's counted reference and its table, searched by bisection (O(log n) from a
return address to the code). Ranges do not overlap, and registering one that
does is refused with nothing changed. The table is validated against the range's
size at registration. It lives with A's guest state, so a context needs an
engine before it can hold code. Only the owner thread writes it; a collector
reads it at a poll on that thread.

**Entry slots.** A slot is one word, `entry`, that compiled code loads to call
a function: the address of its compiled entry, or zero. The slot's memory does
not move until the context is destroyed, so compiled code may embed its address.
Setting a slot takes a counted reference to the code; clearing it zeroes the
word first, so no later call goes through it.

**Code lifetime and the retired list.** The registry and each slot hold a
counted reference. Clearing a slot, replacing its code or unregistering a range
does not release that reference while a compiled frame may return into the code;
it moves to the *retired list*, which is released when the context has no open
`GRCORE_ACTIVATION_JIT` record (a counter kept by `grcore_activation_enter` and
the one place a record is dropped, so an unwind releases it too). With none
open the reference is released at once. A retired range is still found by
`grcore_code_lookup` (it says it is retired), because a frame that returns into
it must still be walked, and its addresses are not given to new code until it is
released. Clearing a slot allocates nothing: the node that carries the retired
reference is made when the slot takes the code, so discarding code cannot fail
for want of memory. Everything is released at context destruction whatever is
open.

**The record's compiled state.** An activation record may carry a frame base
and a return address (`grcore_activation_set_compiled`; `enter` keeps its
signature, and the fields are appended to `GRCORE_ActivationInfo`): the base of
the innermost compiled frame under that record and the address in its code that
it is stopped at. Compiled code stores them before a native call or a poll's
slow path. Zero in both clears it, which a record that pauses or whose frames
were rebuilt must do; the walk reads native memory and so is valid only while
those frames are on the stack. A record with no segment has nothing to bound
its frames, so the walk trusts its base and checks only alignment and order;
the caller keeps the state clear whenever its frames are not live. Only a JIT
record may carry the state, because only those count toward releasing retired
code. A compiled frame's `meta` and `site` are valid only while its code stays
registered, and entry slots live until the context is destroyed.

**The walk** (`a/compiled.h`) starts at the innermost record that carries
compiled state. A frame's code is found from its return address, its site from
the offset in that code; the caller's base and return address are the two words
at the frame's base. A return address in registered code continues the run; the
run ends only at the chain-end marker (part 5), and the walk goes on to the next
record outward that carries state (a compiled run under a nested activation). It checks that each caller base is word-aligned, above its
callee's and, when the record has a C segment, inside it; that the innermost
return address is in code at a site; and that runs ascend. **A broken chain is
never skipped.** `grcore_compiled_walk_next` returns `GRCORE_CWALK_BROKEN` with a
reason and stays broken, after yielding every good frame before the break. The
frame walk stops and says so (`grcore_frame_walk_broken`). Root enumeration has
no result channel, so it prints the reason and aborts: a skipped frame is a
missed root, which a collector turns into a live object freed, and stopping is
cheaper than that.

**Roots.** A's root source reports each VALUE slot of each compiled frame once,
as the address of the native slot, so a moving collector writes through it; only
what the site's stack map names is reported, so a raw word that looks like a
reference, an `I32` or an unmapped word never is, and nothing in a compiled
frame is scanned conservatively. Compiled frames are placed among the guest
frames, and paired with the guest frame each stands for (part 5); one with none
comes before the guest frames that were on the stack when its record was entered
(`base_frames`). A conservative range (a record's C
segment) is reported with every compiled frame's extent cut out of it (from the
base less the code's `frame_bytes` through the two saved words), so a segment
that is drawn too wide, or that spans its own record's frames, still scans only
the C words between them.

**The abstract frame.** The frame walk interleaves compiled frames with the
interpreter frames in the same order. A compiled frame carries its engine and
descriptor (from the registry), the identity and location of its site, its
native base and its site, and no guest frame: its `frame` reference is null and
the `stack.h` accessors refuse it. `grcore_frame_slot` and the new
`grcore_frame_slot_tagged` read its slots by the site's frame state, as they are
in the native frame with their representation, converting nothing (a counting
conversion in the tests stays at zero); a dead slot reads as raw zero. It has no
scopes to ask the engine about. Pairing a compiled frame with the guest frame a
compiled call pushes is part 5's.

Rejected: **a shadow stack of frame records**, pushed by every compiled call
(a store per call on the hot path of the very feature that exists to make calls
cheap, and a second source of truth that can drift from the real stack);
**scanning compiled frames conservatively** (AD-17 forbids it, a moving
collector could not update what it cannot prove is a reference, and an `I32` or
a spilled raw word would pin garbage); **a global registry shared by all
contexts** (a shared structure written on every compile and read at every poll
needs a lock or a hazard scheme, whereas under AD-22 each context finding code
by its own registry makes the owner thread the only writer); and **patching the
return address** to a stub that pops a side record (which breaks a return
predictor on every call and returns, and means a native unwinder, a profiler or a
debugger reading the stack sees addresses in no registered code). The walk costs
two registry bisections (the frame's code, and its caller's, to see whether the
run goes on), one search for the site and two loads per compiled frame, and
only at a GC point.

**Derived pointers and a collector that moves (AD-12, CAP-10).** The pass keeps
a site's derived pointers consistent with their base. For each derived pointer
it takes `delta = derived - base` from the two frame words as they are, parks
the delta in the derived pointer's own slot, lets the visitor update the base in
place, and then stores `base + delta` into the slot. The derived pointer is not
reported to the visitor: it points into the middle of an object, which a
collector's precise walk may not be handed. Nothing else reads the slot while
the walk runs, so parking the delta in it needs no memory and no limit on how
many derived pointers a site has. With a visitor that moves nothing, which is
every collector today, the slot is written with the value it already had. The
delta is taken from the words and not from the metadata's `delta`, so the pass
preserves what the code holds, and a site whose code holds a different offset
than it declared is not made worse by a move. A visitor with no `slot` function
leaves the slot alone. runtime-heap's relocation torture (a test-only mode that
moves every unpinned object at every collection) is what exercises this pass;
`CompiledRoots.ADerivedPointerFollowsItsBaseAndKeepsItsDelta` exercises it with a
visitor that moves every reference, and was seen to fail with the recomputation
removed.

The decision behind it is runtime-heap's relocation torture, a test-only build
mode that moves every unpinned object at every collection; its `design.md`
("Relocation torture") records the mode and the alternatives rejected for it.
This pass is the half of it that lives here.

A derived slot is the pass's own while the walk runs, because the delta is
parked in it, so `grcore_codemeta_validate` refuses a site whose derived slot is
also a live reference (the visitor would be shown the delta as one) or is named
by two derived entries (it would be subtracted twice). A chain, one entry's slot
being another's base, is refused by the checks of the base (it must be live) and
of the slot (it must not be), so it needs no check of its own. Two entries may
share a base.

Rejected: **reporting the derived pointer as a root of its own** (a precise walk
would find an address that is not the start of an object, and a moving collector
cannot update it without knowing its base); **recomputing from the metadata's
`delta`** (the declared offset can differ from the held one, and the held one is
what the code uses); and **a side array of deltas** (allocation in a pass that
runs inside a collection, which must not fail).

## A, part 5: calls between compiled frames (AD-28, CAP-1 to CAP-4)

Part 4 made a chain of compiled frames visible. This part is what a compiled
call needs from the core to be correct: a place the walk starts from that
compiled code can write, a run that ends definitively, a compiled frame and its
guest frame that are one frame, a rebuild of the whole chain, a native stack
measured in bytes, an entry slot that remembers a callee that cannot be
compiled, and a measurement of what the retired list holds. All of it is `free`,
appended to the headers, and `BITS`-only and interpreter-only behaviour is
unchanged. `runtime-jit`'s `design.md` ("Calls between compiled functions") is
the other half: the convention, the call sequence and the hooks.

**Where the walk starts: a cell in the context.** Before any call that can reach
a GC point, compiled code stores its frame base and the return address of the
call in two words of the *context*, at an offset `a/layout.h` states
(`walk_cell_offset`). The record array grows by copy and so moves, and compiled
code cannot name a record, so the value cannot go in the record directly; the
context never moves and compiled code already holds it. Core moves the cell into
the innermost record at the next activation entry (so a native that records an
activation leaves the compiled run below it described by the record below) and
at every walk (`grcore_compiled_walk_begin`), and leaving a JIT record clears it,
so a later walk cannot read frames that were left. A cell that is set while the
innermost record is not a JIT one cannot have been set by compiled code, and is
a broken walk and never silently dropped. Rejected: **a pointer to the record**
(dangling after the array grows), **a thread-local** (code is shared between
contexts, a paused context resumes on another thread, and TSan sees a race that
is not one), and **a global** (one context's run is another's frames).

**A run ends only at a marker.** Story 2 ended a run at any return address in no
registered code, which is also what a corrupt word looks like, so a corrupt
return word silently dropped every outer frame's roots. The entry adapter of a
callable function sets its frame register to `GRCORE_COMPILED_CHAIN_END` before
it calls the first compiled function, so that function's saved caller base is
the marker, and the walk ends a run on it and on nothing else: a return address
in no registered code, met without it, is a broken chain, and enumeration aborts
with the reason. The marker is odd, so it is never a frame base (a base is
word-aligned) and is not a value a stack holds by accident. Rejected: **a
registered entry stub with a site kind of its own** (a metadata format change
for every reader, and a second registry lookup per run), **zero as the marker**
(what zeroed or overwritten memory looks like, which is the case this closes),
and **counting frames** (a count is as corruptible as the word it replaces).
`HandStack::build` leaves the marker, and `EveryBrokenChainIsAnErrorAndNeverASkipOrAnEnd`
grew the corrupt-return, missing-marker and look-alike-base cases.

**A compiled frame and its guest frame are one frame.** Every guest call pushes
the callee's guest frame, compiled or not, so each compiled frame has a guest
frame of its own, stale until a rebuild (compiled code keeps its values in its
native frame). Reporting both showed each activation twice and reported the
guest frame's stale VALUE slots as roots. A frame of a run of `n` frames entered
with `base_frames` guest frames stands for the guest frame at index
`base_frames - 1 + (n - 1 - depth)` from the outermost: the run's outermost frame
is the function the interpreter entered. The compiled walk counts a run on a copy
of itself, by the same step it walks with, so the count and the walk cannot
disagree, and each frame reports `run_depth` and `run_length`. The pairing is a
claim about the stack, so it is checked (the guest frame must be the same engine
and function as the site's identity) and where it does not hold both are
reported as before, which can retain too much and never too little. A paired
frame is shown once, by its identity and its stack map, with the guest frame in a
new `guest_frame` member (`frame` stays null: the `stack.h` accessors would read
the stale slots), and the root pass skips the guest frame's slots but still runs
its engine's `roots` hook, since what that reports is not a slot. A guest frame
above the innermost compiled frame (a callee pushed and not yet entered) is an
ordinary frame.

**One rebuild for a whole chain.** `grcore_compiled_rebuild` rebuilds every
compiled frame of the innermost run (down to the innermost activation record)
into its existing guest frame, so nothing is inserted and no offset moves. It
checks everything before it writes anything: that the run walks and ends
cleanly, that each frame's guest frame is there, of the same engine and function
and with as many slots as the frame state, that the reservation holds every
converting location of the chain, and that the engine can convert them; a
refusal changes nothing. It then converts in AD-27's two phases across the whole
chain (null in every converting slot of every frame, convert every raw value into
a cell, write the slots), so no frame is seen half raw, and a `DEAD` location
leaves the guest frame's own word (a rebuild may put anything there, and a frame
that keeps a header word the compiled code does not know is better left with it).
`keep_frames` is how an unwind rebuilds only what survives: a frame whose guest
frame is at or above that index is not converted, because it is about to be
popped. On success the record's state is cleared and the record is marked
*rebuilt*. The reservation, which the one-frame rebuild took at a fixed size, is
extended by each compiled call (`grcore_deopt_reservation_extend`) by the callee's
maximum and given back when the call returns (`retract`, which cannot fail); the
memory is taken at the call, where a refusal can still be answered by exiting at
the call site, and the rebuild that a guard fifty frames down starts cannot fail
for want of a cell. A chain's rebuild needs the *sum* of its frames' converting
locations at once, which is why a per-frame reservation (what story 1 had) is not
enough.

*A defect in story 2, found here.* `grcore_activation_leave` required the guest
stack to be as it was when the record was entered, which catches an engine that
forgot to pop. After a chain rebuild that can never hold: each compiled call
pushed a guest frame above the record's base, the rebuild fills them in, and the
interpreter finishes them after the native frames return, so an engine could not
leave the record at all and so never release retired code. A *rebuilt* JIT record
may now be left with its guest frames in place, and never with fewer than it began
with; a record whose rebuild was refused keeps the strict rule.

**The native stack, in bytes.** Native depth was a count of activations, which
says nothing about the stack compiled code and its helpers have used. A budget in
bytes is an option (`grcore_options_set_native_stack_bytes`); `run` and `resume`
set a limit word in the context from their own stack pointer less the budget (a
resume may be on another thread, so another stack), a re-entry that finds none
sets it, and `run` clears it when it ends because it names a stack nobody is
running on. The re-entry that set it clears it when it is left, for the same reason:
the word otherwise outlives the frame it was measured in, and a later compiled call
made from an engine that never entered through `run` would be measured against it. Each callable function compares `rsp`, less its own frame, with the
word in its prologue (the offset is in `a/layout.h`), and below it deoptimizes the
chain at the call site; only the guest-depth budget gives a verdict, so verdicts
agree across tiers. The budget is a total for compiled code and the C code under
it (the push hook, a collector), so it must leave room for them. Rejected:
**counting frames** (a function's frame is not a constant, and a native that
recurses counts for nothing), **a guard page and a fault handler** (a fault is not
a place to rebuild fifty frames from, and a signal handler on a thread the
embedder owns is not ours to install), and **a per-function constant** (the
function that overflows is the one that did not know how much stack its callees
would use).

**An entry slot can be marked refused.** A call finds three kinds of slot word
with one compare against one: above it, compiled code to call; zero, empty, so
the engine is asked to compile; `GRCORE_ENTRY_REFUSED`, which an engine puts in a
slot whose function cannot be compiled (`grcore_entry_slot_refuse`, which retires
any code the slot held and cannot fail), so that a callee that cannot be compiled
is an exit that costs one compare and no hook. A later set (a tier-up) or clear
replaces the mark; setting an entry that is not above it is refused, so no code
address can be mistaken for it.

**What the retired list holds, measured.** Retired code is released only when no
JIT activation is open (AD-28), so one compiled run that stays open while its
slots are replaced retains every replaced function. `grcore_code_retired_peak`
records the high-water mark of the list (retired ranges and retired slot code
together). `Calls.RepeatedReplacementUnderOneLongLivedActivation...` in
runtime-jit replaces one function 200 times under one open JIT record: the peak is
400 references (a retired slot reference and a retired range for each, which
name the same code), 832,640 bytes are held (4,163 per replaced function: its
page, and bookkeeping), and everything is released when the record is left. The
growth is linear in the replacements made during the longest-lived activation, a
function costs one page, and replacements happen once per tier-up or deopt of a
function (a function that is discarded after eight deopts, as lang-tang's is, is
replaced at most eight times), so no bound or epoch is added; the figure, and the
statistic that makes it re-measurable, are the evidence, and a long-lived embedder
that disagrees has the number to argue with.

**Tail calls ask nothing new of the core (AD-28, CAP-8).** A compiled tail call
replaces the caller's native frame and its guest frame (`runtime-jit`'s `design.md`,
"Tail calls"), and the core's API already covers the guest half: the engine's `tail`
hook extends the reservation by the callee's maximum, makes room
(`grcore_stack_reserve`, for a callee whose frame is larger), gives back the
caller's extension (`grcore_deopt_reservation_retract`), pops the caller's guest
frame and pushes the callee's. Everything that can fail is before the pop, so the pop
and push cannot, which is why there is no "replace frame" call: it would be reserve,
pop and push, and one more thing for every engine to call. What the walk needs holds
without a change, by reasoning that the tests check from outside:

- *After the jump the callee's frame holds the replaced caller's saved base and
  return address* in the two words at its base, so `grcore_compiled_walk_next` reads
  the callee as a callee of the original caller. A run still ends at the marker or at
  the caller, a chain after a widening or narrowing tail call is walked once per frame
  with its own identity (`Tail.ACollectionInAFiftyDeepChain...` in runtime-jit: fifty
  frames, the outermost saved base the marker), and the pairing is still positional
  because the hook replaced the guest frame as the jump replaced the native one:
  compiled frames and guest frames correspond one for one.
- *The walk-start cell holds a dead frame after the jump* and is not cleared. It is
  read only after a store that precedes every call that can reach a GC point, the
  native-stack stub stores it itself, and the entry adapter clears it when it leaves,
  so no walk reads it stale. Clearing it at the jump would be two stores per tail call
  for a value nothing reads first.
- *The guest depth does not change*, so the depth budget gives the verdict the
  interpreter's own tail call gives (a million-deep tail recursion fits a limit of
  one hundred, and a non-tail one hits it at the same depth in both tiers); *a rebuild*
  of a chain a tail call went through rebuilds the callers' and the callee's frames,
  nothing else, from a reservation that holds the callee's cells and not the replaced
  caller's (`Tail.EveryTailCallReplacesTheReservationExtension...`).
- *An unwind or a record's `leave`* sees the guest frame count it began with or more:
  a tail call never lowers it, so the rule that a rebuilt JIT record may be left with
  guest frames above its base and never with fewer is untouched.

**Natives called from compiled code ask nothing new of the core either (AD-28, AD-17,
CAP-7).** `runtime-jit` calls an engine's native directly and stores the walk-start cell
first (`runtime-jit`'s `design.md`, "Calls to natives"); what the core has to give, it
already does, and the story that added the call found no need to change it:

- *The cell, and the record a native opens.* A native that opens an activation record
  (`GRCORE_ACTIVATION_NATIVE` for a leaf-ish one that only wants its own frame pinned,
  `GRCORE_ACTIVATION_REENTRY` for one that runs guest code) moves the cell into the record
  below at `grcore_activation_enter`, so a compiled run under the native stays described
  by the JIT record it belongs to, and a nested run of its own sets the cell afresh. A
  native that opens none is walked from the cell directly, which is what a collection in a
  leaf native needs. The library opens no record on a native's behalf: that would be a
  core call on every native call, and most natives need neither.
- *The segment is the native's frame, and no helper makes it.* The fixture's `NativeScope`
  gives `[stack pointer of the native - 256, base of the innermost compiled frame)`: the
  low end is read from `rsp` in the native itself, the high end from the walk-start cell,
  so the range takes in the native's own frame *and the stack arguments the call made*
  (which the compiled frame's extent does not cover and a native's frame does not either),
  and `visit_range` cuts the compiled frames out of it. A helper that made this would be
  two intrinsics and a read of a layout word, and would hide the rule that matters, which
  is that a native pins what its frame holds and must not rely on an argument being
  updated; so none was added. The contract is in `runtime-jit`'s `natives.h`.
- *Depth.* A `JIT`, `NATIVE` or `REENTRY` record enters the native-depth budget. An interpreted
  nesting costs one unit a level (the REENTRY record); a compiled nesting costs two (REENTRY and JIT), and a
  compiled outer run costs one more than an interpreted one. So the same budget refuses a program that nests
  compiled natives at about half the depth the interpreted run reaches (budget 4: three runs interpreted, two
  compiled), and the same *interpreted* nesting needs a budget one larger under a compiled outer run
  (`Natives.ARefusedRecordMakes...` and `...ARefusedJitRecord...` measure both). That is AD-21's accounting and
  not a difference between tiers' verdicts in kind: the native sees a refused `grcore_activation_enter` and
  returns the status that unwinds, in both, and an engine whose wrapper wants equal depths must not count the
  JIT record (story 9's to decide).
- *A record that was rebuilt cannot be left with fewer guest frames than it began with.*
  An unwind that finds no scope in a compiled run pops the frames above the run's entry
  frame in the `deopt` hook, but the entry frame itself, which the record counted when it
  began, must stay until `grcore_activation_leave` has run, and the caller of the run
  pops it afterwards. The rule is `grcore_activation_leave`'s, unchanged; an engine that
  pops the entry frame in the hook is refused at the leave, so the fixture's hook leaves it
  and `run_compiled` pops it after the record is left.
- *The limit word.* A nested compiled run must not call `grcore_context_native_limit_here`
  again: the limit is the outer run's, set from the stack pointer the outer run began at,
  and the nested run's deeper frames are measured against it. (The nested re-entry's own
  `grcore_activation_enter` sets it only if it finds it unset, as before.)

## B, part 3: context snapshots

A snapshot (`b/snapshot.h`) is the first thing in this library that outlives
the context it describes. It is the spine's reserved seam for CAP-11: a context
started from a frozen, pre-initialised image gives the output of one
initialised from scratch (AD-12, AD-19, AD-20).

**What a snapshot is.** A list of named byte blobs, one per registered key that
has snapshot hooks, written by the keys; the core does not know what is in
them. It stores no host pointer and no address (a reference a key writes is an
index, a host function is a name), is immutable once made, counts references
with an atomic (retain relaxed, the release that reaches zero acquire-release,
as `GRCORE_Code` does), keeps its own copy of the allocator it was made with
(it is not the context's, since a snapshot may outlive the context), and may be
restored any number of times, into contexts on any thread, concurrently. It is
an in-memory object. A byte format or a file is **not done** (the spine defers
it); the blobs' contents are deliberately in native byte order and host sizes,
and `reader_string` hands out pointers into the snapshot, because nothing here
is parsed from outside the process.

**When one may be taken (AD-20).** By the owning thread only, with the context
paused or parked outside `run`, with no fuel scope open and no nested
activation; the guest stack's own hook adds "no activation record and no budget
scope record". Every refusal is `ERR_INVALID` and
changes nothing: the keys' shape is checked before anything is allocated (a key
with `snapshot` but no `restore` or `settle`, no name, more than one
registration or a name another hooked key has), and a hook that refuses frees the
partial snapshot. Reading a running or at-poll context is never allowed, which
is AD-20's rule for reading guest state and not a restriction added here.

**Restore is create-then-fill, and atomic.** The host builds the destination as
it would for a fresh run, registers the same keys, and calls
`grcore_context_restore`. The structure of the call, and not the care of each
hook, is what makes it atomic:

1. The key set must be the snapshot's exactly: every blob has a destination key
   of its name, and the destination has no hooked key without a blob. (A key
   without hooks is not part of any snapshot and the host registers it again.)
2. **CHECK**: every key, in snapshot order, is asked whether the blob fits,
   and must change nothing. A mismatch (an engine table, a program, a heap
   codec, a root layout) is refused here, before any change.
3. **APPLY**: every key builds its state, in snapshot order. A failure (the
   destination's budget, an allocation) undoes the keys applied before it,
   newest first, through their ABANDON settle.
4. **PREPARE**, in the destination's registration order: each key proves that
   COMMIT cannot fail and allocates whatever it needs; a failure abandons
   everything.
5. **COMMIT**, same order, cannot fail; then a paused snapshot makes the
   destination paused.

`settle` is a separate hook, run after every APPLY, because state that refers
across keys (the heap writes root slots, which live in the frames and the
engine's own tables) can only be finished once every key has built its part;
no outcome depends on the registration order (PREPARE and COMMIT follow it only
to be deterministic, after every APPLY), so two keys need not know which was
registered first. The three modes of `settle` exist because a
commit that can fail after another key has committed has nothing to roll back
to: PREPARE is where it may fail, and COMMIT is final.

**What the core itself captures.** Only whether the context was paused, and the
line it paused at. Restoring a paused snapshot leaves the destination paused, so
`grcore_resume` continues it, with the entry function and its state the host
supplies in the restore environment (a function address cannot be in a
snapshot); the pause location's file is borrowed from the host the same way.
Not captured, supplied again as for a fresh run: the group, the options and
budgets (fuel used starts at zero, the limits are the destination's), the page
provider and allocator, the request port, the keys that have no hooks, and the
keys that paused it (a restored context reports none).

**The guest stack's snapshot hooks (A).** The stack's own key (`runtime-core.guest`)
has hooks, so every engine's frames travel with a snapshot without the engine
writing them. The blob is the engine table by name and the frames byte for byte.
Frames are position independent (links are offsets and a header's tag is a
function of its own offset), so they land at the same offsets in the
destination and every link stays true, and the poll identity in each header
travels with them. The one thing in a frame that is not data is a `VALUE` slot,
which holds a reference into somebody's heap: it is written as **zero** (the
engine's `slot_kind` says which), and the owner of the reference writes it back
at settle, which is why the heap's root enumeration must give the same count at
take and at settle. A `RAW` slot is written as it is.

Take is refused (`ERR_INVALID`) with an activation record or a budget scope
record open: those are the host and native frames and the template boundaries
that AD-20 says have no place in a frozen context. Restore refuses, in CHECK
and before anything changes, a destination whose engine table is not the
snapshot's (same names, same order: a header names its engine by position), a
destination that already has a frame, an activation or a scope, and a blob that
is not a well-formed stack (a frame whose tag, link or size is wrong; the
counts are checked against the frames themselves, so a blob that lies about its
size cannot make the copy overrun). The destination's *own* depth and memory
budgets are what limit it: each restored frame enters the depth count, and a
snapshot deeper than the destination allows is `ERR_LIMIT` with the depth
counted back and the stack as it was; the buffer is grown through the context's
counting allocator, so a budget or an allocation failure ends the same way.

*Rejected:* a relocating restore that rebuilt the stack frame by frame through
`grcore_stack_push`. It would renumber nothing here, but it would run the
depth and growth logic once per frame, and a mismatch in the middle would leave
a half-pushed stack for the key to unpick; copying a validated buffer and
setting three counters has one place to fail.

**Rejected alternatives.**

- *Copying the context's memory.* The page provider picks addresses, so a byte
  copy needs the same addresses, ties a snapshot to one process layout and
  cannot be shared; and it carries every function pointer into a "frozen"
  object, which the seam forbids (AD-12).
- *A registry of serialisers in the core.* The core would then know what a heap
  and an engine are (AD-1). Keys already name who owns what; the hooks are one
  more field of the thing that already identifies them.
- *Hooks that restore in one pass.* The heap restores before the engine has
  rebuilt its frames if it was registered first, and the order a host registers
  in must not decide what a restore means; two passes (apply, settle) remove the
  dependence. A rollback needs a PREPARE that cannot be skipped, so settle has
  three modes rather than a second hook.
- *A snapshot that refuses nothing.* A blob with no key, or a key with no
  blob, is a context that would be partly restored; it is refused as the
  mismatch it is.

## B, part 4: the sampling profiler

`b/profile.h` answers CAP-12: where a guest's time goes. It is one keyed
registration, phase OBSERVE (AD-5, AD-19), with one request kind that belongs to
that key, and it is the one service the core adds to the poll.

**A sample.** Anything that posts the kind makes the *next poll* take one sample.
The OBSERVE handler sees its own kind pending, clears it (the debugger and the
JIT clear theirs the same way), and walks the frames innermost first through
`grcore_frame_walk_begin` and `_next`, which is legal because `poll_slow` leaves
the context at-poll for the whole poll. The innermost frame's location gets a
*self* hit; every distinct location in the walk gets an *inclusive* hit, so a
recursive function counts once per sample. A poll with an empty guest stack is a
sample counted in `no_frame` (a subset of `samples`) that adds no entry.

The profiler knows no engine. It reads a frame only through the engine
descriptor's `locate`, so one profiler serves Tang, a later Wasm engine and the
JIT, and because a poll identity is the same on every tier (AD-18) the profile
of an interpreted run and the profile of a JIT run of one program agree. It
includes nothing from `runtime-heap`, `runtime-jit`, an engine or the debugger.

**What the handler may do.** It votes nothing, charges no fuel, changes nothing
the guest can see and allocates nothing. The location table is a fixed capacity
(512 by default) set at attach and taken then through the context's counting
allocator as one block each for the entries and the hash index; a location that
does not fit is counted in `dropped` and never allocated for. Locations are keyed
by the *text* of the file and the line, not by pointer, because an engine may
hand out two pointers to equal strings; the profiler keeps the first pointer it
saw, which `GRCORE_Location` already says must outlive the context's use of it.
It commutes with every other OBSERVE and DECIDE handler, which the phase-shuffle
test checks over many seeds.

**Triggers.** `grcore_profiler_request` posts the kind through the context's port
from any thread. `grcore_profiler_timer_start(interval_us)` runs a thread that
calls it every interval. The timer holds only a retained port and the kind and
touches no guest state (AD-4, AD-6); the key's `destroy` stops it and joins it
before the profiler is freed, so a context destroyed while the timer runs is safe
(the TSan target exercises this). A zero interval, one under `GRCORE_PROFILER_MIN_INTERVAL_US` (100 us, refused rather than clamped: a period of a microsecond spins a core) and a second start are refused.
The sample rate is the host's: a tick that finds a sample already pending merges
with it, so a guest that polls rarely is sampled at its poll rate.

This is the one place the core starts a thread of its own. It is an opt-in
convenience over `grcore_port_post`, which any host thread could call, and it is
the only part of the library that sleeps.

**Safepoint bias, stated.** A sample is taken at the next poll after the request,
so time between polls is charged to the poll that ends it: in a loop, to the
loop's back-edge poll. The bias is the same on every tier. It is the price of
reading a guest stack only where it is consistent (AD-4: another thread acts on a
context only by a request), and it means a profile says "where the guest was when
it next could be asked", not "where the CPU was".

**Reading.** `grcore_profiler_report` copies entries (file, line, self,
inclusive) into a caller array, ordered by self descending, then inclusive
descending, then file text and line, so a report is deterministic; it fills a
smaller array with the first entries of that order by insertion, with no scratch
memory. It and `grcore_profiler_reset` (which empties the table, so its locations
are free again) are the owner's and are refused inside a poll. Neither allocates.

**Snapshots.** The key has no `snapshot` hook, so the profiler is part of no
snapshot: a context with one snapshots and restores, and the restored context has
none, as the host attaches again on the destination.

**The fast path.** Nothing here is on the poll's fast path: a context with no
profiler, or no sample pending, runs exactly the code it ran before. The poll
benchmark agrees (`poll-fast` 1.51 ns and `stack-poll` 2.65 ns with the profiler
compiled in, 1.58 and 2.68 without: best of 7 repeats each, one build of each; the
difference is within the noise of the machine, a few percent, and the profiler
puts nothing on the path). A sample itself costs
about 100 ns at depth one and about 30 ns per further frame (`profile-sample-1`,
`profile-sample-32`), post included, so a 1 kHz timer costs on the order of a
ten-thousandth of the guest's time.

*Rejected:*

- *SIGPROF with a program-counter sample.* A signal handler cannot walk a guest
  stack that may be mid-push, and AD-4 allows another thread to act on a context
  only by a request.
- *A sampler thread that reads the guest stack.* The stack belongs to the owner
  (AD-6); a thread reading it races every push.
- *Counting at every poll.* That is instrumentation, not sampling, and it adds to
  the fast path every guest pays.
- *Allocating a new entry in the handler.* A handler is read-only and is given no
  allocating API; a fixed table, with a counted overflow, keeps that true and
  makes a full table a measurement rather than a failure.

## Benchmarks

Every library ships a benchmark harness from its first commit (AD-26). This
one holds a calibration case, fixed integer work that touches no library
code, so that a figure from a real case can be read against the machine it
was taken on, and seventeen real cases, sixteen operations and one whole validation,
`codemeta-validate`. The operations are: creating and destroying a context, a
counting malloc/free pair, a keyed slot lookup, the poll's fast path, the
poll's slow path through four handlers (one in each phase), a post to a port, a push
and pop of a guest frame, the engine-aware poll's fast path, a walk of
sixteen frames at a pause (reported per frame read), an activation record
entered and left, a fuel scope opened, charged and closed, the enumeration of the
roots of sixteen frames (reported per frame), a snapshot taken of a context
holding one 8 KiB blob (and released), the restore of it, and one profiler sample
at depth one and at depth thirty-two (a request posted, then the poll that takes
it). `codemeta-validate` is the seventeenth: it validates a code-metadata
table of 100 sites with 1,000 live slots and 1,000 derived pointers each, and
one of 20,000 sites of 20,000 functions; its unit is one validation of both.

No budget is asserted, so nothing fails if a figure moves; the spine says a
budget is recorded once a first measurement of a real case exists, and these are
those measurements. They were taken with `make bench` on an Intel Core 7 150U
(12 threads), gcc 14.2.0 `-O2 -g`, release, `-std=c17`, best of seven repeats,
on 2026-10-05, against runtime-core `e1f0bd6` with this change. The machine was
not idle: other sessions' builds held the load average between 4.6 and 6.3
throughout, which is why the calibration case reads 1.73 ns a step here where
runtime-jit and lang-tang recorded 1.1 to 1.4 on the same machine, and why the
median of a few cases (keyed lookup, roots) sits well above the best. Read a
figure against the calibration beside it, and re-measure on a quiet machine
before treating any of them as a budget.

| Case | Best of seven | Unit |
| --- | --- | --- |
| calibration | 1.73 ns | one xorshift step |
| ctx-create | 104 ns | a context created and destroyed in a group |
| count-malloc | 30.8 ns | a counting malloc and free pair |
| keyed-lookup | 4.0 ns | `grcore_context_slot` for a registered key |
| poll-fast | 1.54 ns | the poll with nothing pending |
| poll-slow-4 | 104 ns | the poll through four handlers, one in each phase |
| port-post | 17.1 ns | a post to a port |
| stack-pushpop | 26.6 ns | a guest frame pushed and popped |
| stack-poll | 7.87 ns | the engine-aware poll's fast path |
| frame-walk | 31.0 ns | one frame read, in a walk of sixteen at a pause |
| activation | 27.2 ns | an activation record entered and left |
| fuel-scope | 51.5 ns | a fuel scope opened, charged and closed |
| roots-16 | 12.9 ns | one frame, in an enumeration of the roots of sixteen |
| snapshot-take | 318 ns | a snapshot of a context holding one 8 KiB blob, and its release |
| snapshot-restore | 267 ns | the restore of that snapshot |
| profile-sample-1 | 169 ns | one sample at depth one |
| profile-sample-32 | 1.66 us | one sample at depth thirty-two |
| codemeta-validate | 1.82 ms | one validation of both tables above |

## Continuous integration

`core` gets compress-style CI from the start (AD-16): both compilers,
ASan+UBSan, ThreadSanitizer, Valgrind, the gates and their self-test, a
coverage floor of 80, and MSYS2 on Windows. ThreadSanitizer now checks the
ownership hand-off between threads, concurrent posters, a post racing the
context's destruction, a watchdog stopping a running guest, and a paused
context resumed on another thread.

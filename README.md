# Ghoti.io Runtime-core

The core of the Ghoti.io language runtime stack, in C. It holds two things and
nothing else: **B**, the execution context (policy, budgets, capabilities, the
context group, requests, the poll and the context's lifecycle), and **A**, the
frame protocol (the abstract frame, scopes, the guest stack, activation
records and root sources). Engines such as `lang-tang` push frames through A;
services such as the heap and the debugger attach to B; hosts create contexts
and run them. This library knows no engine and no service.

Nothing is released. So far there is the scaffold (the build, the version, the
result vocabulary, the allocator, and the gates and benchmark harness), all of
B: groups, contexts, options, keys, the lifecycle and ownership rules,
memory accounting, requests and ports, the four-phase poll, `run` and `resume`,
the budgets with their fuel scopes, and root sources; and all of A: the guest
stack, engine descriptors, the abstract frame, the frame walk and scopes,
activation records (with the compiled-frame state), the unwinder, budget
scopes, the code registry and the walk of compiled frames.

## Example

```c
#include <ghoti.io/runtime-core/runtime-core.h>

#include <stdio.h>

int main(void) {
  printf("runtime-core %s\n", grcore_version_string());
  printf("%s\n", grcore_result_string(GRCORE_ERR_GUEST));
  return 0;
}
```

Compile it against an installed copy with
`cc example.c $(pkg-config --cflags --libs ghoti.io-runtime-core-0)`.

## Building

`runtime-core` depends on `cutil` and on nothing else. Libraries find each
other through pkg-config only, so build the suite first from the workspace
root (`./bootstrap.sh`), then:

```bash
export PKG_CONFIG_PATH="$PWD/.local/share/pkgconfig"
make -C libs/runtime-core test PREFIX="$PWD/.local"
```

Pass `PREFIX=` to every `make`, including a throwaway one: the rpath is added
only when it is set. `make help` lists the targets. The ones particular to
this library:

| Target | Does |
| --- | --- |
| `test` | build, `check-symbols`, `check-aliasing` (gcc only), `check-stamps`, the gates below, the examples, the unit tests, and one smoke run of the benchmark |
| `examples` | build each program under `examples/` and run it; a failing example fails `test` |
| `check-labels` | fail if a public header has no `@stability stable` or `free` label, or the wrong one for its directory |
| `check-direction` | fail if a `b/` or top-level header includes an `a/` header (or the umbrella) |
| `check-edges` | fail on any `#include` or shared-object dependency on a Ghoti library other than `cutil` |
| `check-gates` | run each gate against a planted defect and a control, and against an empty tree, and fail unless each behaves; the gates are the three above, `check-symbols`, `check-stamps`, `check-version` and `check-wiring` |
| `check-version` | build the generated header at 0.2.3 and fail unless the header, the packed number a consumer compares and the library's file name all say so, and the header carries its SPDX line |
| `check-wiring` | fail if a gate is not in `TEST_GATES` or its target no longer runs its script |
| `check-hook` | run `.githooks/commit-msg` over messages with assistant attribution and with human co-authors, and fail on any it strips or keeps wrongly |
| `bench` | run the benchmark harness in full; it prints a calibration result first |
| `test-asan`, `test-tsan`, `test-valgrind-quiet` | the same tests under ASan+UBSan, ThreadSanitizer and Valgrind |
| `coverage` | instrumented run and line report; `COVERAGE_MIN=80` fails below the floor |

## The API

The public headers live in `include/ghoti.io/runtime-core/`, in three places
with three rules:

| Location | Holds | Label |
| --- | --- | --- |
| top level | shared basics: macros, version, allocator, results | `stable` |
| `b/` | B, the execution context | `stable` |
| `a/` | A, the frame protocol | `free` |

`stable` means frozen at the major version once the library is released.
`free` means a consumer requires the exact version it was built against. The
label is the `@stability` tag in each header's `@file` block. The include
direction is one way: an `a/` header may include `b/` and top-level headers,
and nothing else may include `a/`. Only the umbrella, `runtime-core.h`,
includes both.

Today the API is the basics: `GRCORE_Result` (the suite's vocabulary plus
`GRCORE_ERR_GUEST`, for guest code that ended in an uncaught exception or a
trap), `grcore_result_string`, `grcore_version_string`,
`grcore_version_number`, and `GRCORE_Allocator` (cutil's allocator under a
local name), and, in `b/` and `a/`:

| Header | Holds |
| --- | --- |
| `b/group.h` | `GRCORE_Group`: created by the host, owns the allocator and page provider its contexts draw on; refuses to be destroyed while it has contexts |
| `b/context.h` | `GRCORE_Context`: four states (parked, running, at-poll, paused), one owning thread, `acquire`/`release` to migrate, keyed registration, a counting allocator and page provider, memory getters |
| `b/options.h` | `GRCORE_Options`: opaque, set through setters; four budgets that are `GRCORE_UNLIMITED` until set, plus keyed byte options |
| `b/key.h` | `GRCORE_Key`: a static object with a leading `size`, a cardinality, a phase, a destructor, a poll handler and three optional snapshot hooks (`snapshot`, `restore`, `settle`); identity is its address. Define it with `GRCORE_KEY_INIT(name, cardinality, phase, destroy, poll, snapshot, restore, settle)`, which writes `size`; the core reads a trailing member only if `size` reaches it |
| `b/page.h` | `GRCORE_PageProvider` (a leading `size`; define it with `GRCORE_PAGE_PROVIDER_INIT(ctx, page_size, map, unmap, protect)`): the pages `runtime-heap` and `runtime-jit` ask for, and `grcore_page_protect`, which flips a mapping between read-write and read-execute |
| `b/request.h` | request kinds, and `GRCORE_Port`: the only way another thread acts on a context, reference-counted and valid after its context is gone |
| `b/poll.h` | `grcore_poll`, the runtime poll for natives, the four phases and their verdicts, `grcore_pollcall_pause_allowed` (whether a pause vote would return to the host, which a debugger asks before voting one), and the phase-shuffle test mode |
| `b/run.h` | `grcore_run`, `grcore_resume`, `grcore_context_wait`, and the pause's keys, location and unwind reason |
| `b/budget.h` | fuel, memory (budget, reserve, refusals) and depth enforcement; fuel scopes, an exclusive budget under the request's ceiling |
| `b/snapshot.h` | `GRCORE_Snapshot`: an immutable, reference-counted image of a paused or idle context, made of one named blob per key that has hooks; `grcore_context_snapshot` and `grcore_context_restore`, which is atomic and takes a `GRCORE_RestoreEnv` (a leading `size`, defined with `GRCORE_RESTORE_ENV_INIT(user, lookup, entry, entry_state, pause_file)`) (a destination that cannot take it is left as it was); a bounded writer and reader for a hook's bytes |
| `b/profile.h` | `GRCORE_Profiler`: a sampling profiler, one OBSERVE registration and one request kind; a request makes the next poll walk the frames and count the locations (self and inclusive) into a fixed table; an optional timer thread; no allocation in the handler; biased to safepoints |
| `b/roots.h` | root sources (`GRCORE_RootSource`, a leading `size`, defined with `GRCORE_ROOT_SOURCE_INIT(name, enumerate)`): how a collector finds a context's roots (precise slots and conservative ranges) through B's types alone, enumerated all at once or one source at a time (`grcore_context_root_source`, for a consumer that names where a root came from) |
| `a/engine.h` | `GRCORE_EngineDescriptor`: an engine's slot kinds, locator, inspector, scope interface, conservative decoder and `roots`, `unwind`, `convert` and `reverse` hooks, registered per context; a leading `size`, written by `GRCORE_ENGINE_DESCRIPTOR_INIT(name, slot_kind, locate, inspect, scopes, decoder, roots, unwind, convert, reverse)`; `free` |
| `a/stack.h` | `GRCORE_Stack`: the context's guest stack of frames named by offset, growing by copy, with the depth budget counting frames; `grcore_stack_poll` records a poll identity; `free` |
| `a/frame.h` | `GRCORE_AbstractFrame` and the frame walk: the one way any consumer reads a frame, its slots and its scopes, in readable states only; `free` |
| `a/activation.h` | activation records: every crossing between host, interpreter, JIT code and C, entered and left in LIFO order, with the native depth budget and the nesting that forbids a pause; `free` |
| `a/unwind.h` | the unwinder: pops frames innermost first with their engine's hook, leaves deeper activations, closes deeper scopes; `free` |
| `a/budget_scope.h` | budget scopes: a call boundary with its own fuel budget, unwound to when it runs out; `free` |
| `a/layout.h` | the JIT layout descriptor: the offset of the request word a compiled poll loads; `free` |
| `a/code.h` | `GRCORE_Code`: reference-counted compiled code, an opaque handle with a payload and a release callback; the count is atomic because compiled code is shared between contexts (AD-22); `free` |
| `a/deopt.h` | `grcore_deopt_read` and `grcore_deopt_write_back`: a native frame read into, and its reference slots written back from, an array of interpreter slots by a site's frame state; `free` |
| `a/registry.h` | the per-context registry of compiled code by address range, entry slots (one word compiled code loads to call), and the retired list: code is released when no JIT activation record is open (AD-28); `free` |
| `a/compiled.h` | the precise walk of compiled frames from an activation record's frame base and return address, by the frame layout contract (the base word holds the caller's base, the next the return address); a broken chain is an error and never skipped; `free` |
| `a/codemeta.h` | the code-metadata format: stack maps and deopt records for compiled code, with a validator and a lookup; `free` |

```c
GRCORE_Group * group;
GRCORE_Context * context;
grcore_group_create(NULL, NULL, &group);
grcore_context_create(group, NULL, &context);   /* parked, owned by this thread */
/* ... register keyed state, hand the context to a worker with release/acquire ... */
grcore_context_destroy(context);                /* destructors in reverse order */
grcore_group_destroy(group);
```

A context is used by its owning thread only. Hand it over with
`grcore_context_release` (allowed only when it is parked outside `run`, or
paused) and `grcore_context_acquire` on the other thread. The one exception is
a port: any thread may post a request through one, which is how a watchdog
stops a runaway guest.

Running guest code is done with an *entry function* an engine supplies. A
pause cannot keep C frames, so the entry saves its position in its own state
and `resume` calls it again:

```c
static GRCORE_Step entry(GRCORE_Context * context, void * state) {
  Loop * loop = state;
  while (loop->next < loop->end) {
    grcore_context_charge_fuel(context, 1);
    GRCORE_Verdict verdict = GRCORE_POLL(context);
    if (verdict == GRCORE_VERDICT_PAUSE)  return GRCORE_STEP_PAUSED;
    if (verdict == GRCORE_VERDICT_UNWIND) return GRCORE_STEP_UNWOUND;
    /* ... one step of work ... */
    loop->next++;
  }
  return GRCORE_STEP_FINISHED;
}

GRCORE_Outcome outcome;
grcore_run(context, entry, &loop, &outcome);   /* OK, finished or paused */
if (outcome == GRCORE_OUTCOME_PAUSED) {
  /* which key paused it, and where: grcore_context_pause_key(context, 0),
   * grcore_context_pause_location(context) */
  grcore_context_set_fuel(context, GRCORE_UNLIMITED);
  grcore_resume(context, &outcome);
}
```

A paused guest's frames are read the same way whatever engine pushed them:

```c
GRCORE_FrameWalk walk;
GRCORE_AbstractFrame frame;
grcore_frame_walk_begin(context, &walk);        /* at-poll or paused, held */
while (grcore_frame_walk_next(&walk, &frame)) { /* innermost first */
  /* frame.descriptor->name, frame.identity, frame.location, ... */
  grcore_frame_slot(&frame, 0, &kind, &value);
}
```

[examples/README.md](examples/README.md) indexes the runnable examples by what
they show, and `make examples` builds and runs each.

## Status

Version 0.0.0, unreleased. The design and the alternatives it rejected are in
[documentation/design.md](documentation/design.md). Patches are not being
accepted at this time; see [CONTRIBUTING.md](CONTRIBUTING.md).

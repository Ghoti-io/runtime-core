# Ghoti.io Runtime-core

The core of the Ghoti.io language runtime stack, in C. It holds two things and
nothing else: **B**, the execution context (policy, budgets, capabilities, the
context group, requests, the poll and the context's lifecycle), and **A**, the
frame protocol (the abstract frame, scopes, the guest stack, activation
records and root sources). Engines such as `lang-tang` push frames through A;
services such as the heap and the debugger attach to B; hosts create contexts
and run them. This library knows no engine and no service.

Nothing is released. So far there is the scaffold (the build, the version, the
result vocabulary, the allocator, and the gates and benchmark harness) and the
all of B: groups, contexts, options, keys, the lifecycle and ownership rules,
memory accounting, requests and ports, the four-phase poll, `run` and `resume`,
and the budgets. The frames (A) come next.

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
| `check-gates` | run each gate against a planted defect and a control, and against an empty tree, and fail unless each behaves |
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
local name), and, in `b/`:

| Header | Holds |
| --- | --- |
| `b/group.h` | `GRCORE_Group`: created by the host, owns the allocator and page provider its contexts draw on; refuses to be destroyed while it has contexts |
| `b/context.h` | `GRCORE_Context`: four states (parked, running, at-poll, paused), one owning thread, `acquire`/`release` to migrate, keyed registration, a counting allocator and page provider, memory getters |
| `b/options.h` | `GRCORE_Options`: opaque, set through setters; four budgets that are `GRCORE_UNLIMITED` until set, plus keyed byte options |
| `b/key.h` | `GRCORE_Key`: a static object with a cardinality, a phase and a destructor; identity is its address |
| `b/page.h` | `GRCORE_PageProvider`: the pages `runtime-heap` and `runtime-jit` will ask for |
| `b/request.h` | request kinds, and `GRCORE_Port`: the only way another thread acts on a context, reference-counted and valid after its context is gone |
| `b/poll.h` | `grcore_poll`, the runtime poll for natives, the four phases and their verdicts, and the phase-shuffle test mode |
| `b/run.h` | `grcore_run`, `grcore_resume`, `grcore_context_wait`, and the pause's keys, location and unwind reason |
| `b/budget.h` | fuel, memory (budget, reserve, refusals) and depth enforcement |

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

[examples/README.md](examples/README.md) indexes the runnable examples by what
they show, and `make examples` builds and runs each.

## Status

Version 0.0.0, unreleased. The design and the alternatives it rejected are in
[documentation/design.md](documentation/design.md). Patches are not being
accepted at this time; see [CONTRIBUTING.md](CONTRIBUTING.md).

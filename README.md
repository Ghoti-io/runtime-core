# Ghoti.io Runtime-core

The core of the Ghoti.io language runtime stack, in C. It holds two things and
nothing else: **B**, the execution context (policy, budgets, capabilities, the
context group, requests, the poll and the context's lifecycle), and **A**, the
frame protocol (the abstract frame, scopes, the guest stack, activation
records and root sources). Engines such as `lang-tang` push frames through A;
services such as the heap and the debugger attach to B; hosts create contexts
and run them. This library knows no engine and no service.

Nothing is released. This is the scaffold: the build, the version, the result
vocabulary, the allocator, and the gates and benchmark harness that the
contexts and frames will be built under. Contexts and frames come next.

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
| `test` | build, `check-symbols`, `check-aliasing` (gcc only), `check-stamps`, the gates below, the unit tests, and one smoke run of the benchmark |
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
local name).

## Status

Version 0.0.0, unreleased. The design and the alternatives it rejected are in
[documentation/design.md](documentation/design.md). Patches are not being
accepted at this time; see [CONTRIBUTING.md](CONTRIBUTING.md).

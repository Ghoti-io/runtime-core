#!/bin/sh
#
# Fail if a version other than 0.0.0 does not come out right.
#
# Every input to the version is zero today, so swapping the minor and the
# patch (or dropping one) changes no output and nothing notices. This asks the
# Makefile for a build at 0.2.3 - MINOR_VERSION is "minor.patch" there - and
# checks each place the number is written: the generated header's three
# integers and its string, the packed number a consumer compares against, the
# shared library's file name, and the SPDX header the generator must carry.
#
# Usage: check-version.sh <makefile> <project-root>
#   The Makefile is a parameter so that check-gates.sh can run this against a
#   copy with the defect planted in it.

set -u

MAKEFILE="${1:?usage: check-version.sh <makefile> <project-root>}"
ROOT="${2:?usage: check-version.sh <makefile> <project-root>}"
CC="${CC:-cc}"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

gen="$work/generated"
header="$gen/ghoti.io/runtime-core/libver_gen.h"

if ! make -s --no-print-directory -f "$MAKEFILE" -C "$ROOT" SKIP_DEP_CHECK=1 \
    MINOR_VERSION=2.3 GEN_DIR="$gen" "$header" >"$work/make.log" 2>&1; then
  printf 'check-version: could not generate the header:\n' >&2
  cat "$work/make.log" >&2
  exit 1
fi
if [ ! -s "$header" ]; then
  printf 'check-version: %s was not written; this gate is measuring nothing\n' \
    "$header" >&2
  exit 1
fi

status=0
need() {
  if ! grep -qE "$1" "$header"; then
    printf 'check-version: %s lacks a line matching %s\n' "$header" "$2" >&2
    sed 's/^/  | /' "$header" >&2
    status=1
  fi
}
need '^#define GHOTIIO_RUNTIME_CORE_VERSION_MAJOR 0$' 'MAJOR 0'
need '^#define GHOTIIO_RUNTIME_CORE_VERSION_MINOR 2$' 'MINOR 2'
need '^#define GHOTIIO_RUNTIME_CORE_VERSION_PATCH 3$' 'PATCH 3'
need '^#define GHOTIIO_RUNTIME_CORE_VERSION "0\.2\.3"$' 'VERSION "0.2.3"'
need '^ \* SPDX-License-Identifier: LGPL-3\.0-only$' 'an SPDX identifier'

# What a consumer sees: the packed number, compared at compile time.
cat > "$work/probe.c" <<'PROBE'
#include <ghoti.io/runtime-core/libver.h>
_Static_assert(GRCORE_VERSION_NUMBER == GRCORE_MAKE_VERSION(0, 2, 3),
    "the packed version is not 0.2.3");
_Static_assert(GRCORE_VERSION_MINOR == 2 && GRCORE_VERSION_PATCH == 3,
    "minor and patch are not 2 and 3");
int main(void) { return 0; }
PROBE
if ! $CC -std=c17 -I "$ROOT/include" -I "$gen" -c "$work/probe.c" \
    -o "$work/probe.o" >"$work/cc.log" 2>&1; then
  printf 'check-version: the packed version is not 0.2.3 for a consumer:\n' >&2
  cat "$work/cc.log" >&2
  status=1
fi

# The shared library's file name carries minor and patch after the soname.
if [ "$(uname -s)" = Linux ]; then
target="$(make -s --no-print-directory -f "$MAKEFILE" -C "$ROOT" SKIP_DEP_CHECK=1 \
  MINOR_VERSION=2.3 --eval 'print-target: ; @echo $(TARGET)' print-target 2>/dev/null)"
case "$target" in
  *.so.0.2.3 | *.0.2.3.dylib | *-0.2.3.dll) ;;
  *)
    printf 'check-version: the shared library would be named "%s", not ...0.2.3\n' \
      "$target" >&2
    status=1 ;;
esac
fi

if [ "$status" -eq 0 ]; then
  printf 'check-version: a 0.2.3 build writes 0.2.3 into the header, the packed number and the library name\n'
fi
exit "$status"

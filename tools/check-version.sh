#!/bin/sh
#
# Fail if generated, packed, or linked version data drifts from the Makefile.
#
# The Makefile supplies the major version; this asks it for a build at MAJOR.2.3
# (MINOR_VERSION is "minor.patch" there) and checks each output: the generated
# header's three integers and string, the packed number a consumer compares
# against, the shared library's file name, and the SPDX header the generator's
# template must carry.
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

major="$(make -s --no-print-directory -f "$MAKEFILE" -C "$ROOT" SKIP_DEP_CHECK=1 \
  --eval 'print-major: ; @echo $(MAJOR_VERSION)' print-major 2>/dev/null)"
case "$major" in
  ''|*[!0-9]*) printf 'check-version: Makefile has no numeric MAJOR_VERSION\n' >&2; exit 1 ;;
esac

status=0
need() {
  if ! grep -qE "$1" "$header"; then
    printf 'check-version: %s lacks a line matching %s\n' "$header" "$2" >&2
    sed 's/^/  | /' "$header" >&2
    status=1
  fi
}
need "^#define GHOTIIO_RUNTIME_CORE_VERSION_MAJOR $major$" "MAJOR $major"
need '^#define GHOTIIO_RUNTIME_CORE_VERSION_MINOR 2$' 'MINOR 2'
need '^#define GHOTIIO_RUNTIME_CORE_VERSION_PATCH 3$' 'PATCH 3'
need "^#define GHOTIIO_RUNTIME_CORE_VERSION \"$major\.2\.3\"$" "VERSION \"$major.2.3\""
need '^ \* SPDX-License-Identifier: LGPL-3\.0-only$' 'an SPDX identifier'

# What a consumer sees: the packed number, compared at compile time.
cat > "$work/probe.c" <<PROBE
#include <ghoti.io/runtime-core/libver.h>
_Static_assert(GRCORE_VERSION_NUMBER == GRCORE_MAKE_VERSION($major, 2, 3),
    "the packed version does not match the generated version");
_Static_assert(GRCORE_VERSION_MAJOR == $major && GRCORE_VERSION_MINOR == 2 &&
    GRCORE_VERSION_PATCH == 3, "version components do not match");
int main(void) { return 0; }
PROBE
if ! $CC -std=c17 -I "$ROOT/include" -I "$gen" -c "$work/probe.c" \
    -o "$work/probe.o" >"$work/cc.log" 2>&1; then
  printf 'check-version: the packed version does not match the generated version for a consumer:\n' >&2
  cat "$work/cc.log" >&2
  status=1
fi

# The shared library's file name carries minor and patch after the soname.
if [ "$(uname -s)" = Linux ]; then
target="$(make -s --no-print-directory -f "$MAKEFILE" -C "$ROOT" SKIP_DEP_CHECK=1 \
  MINOR_VERSION=2.3 --eval 'print-target: ; @echo $(TARGET)' print-target 2>/dev/null)"
case "$target" in
  *.so."$major".2.3 | *."$major".2.3.dylib | *-"$major".2.3.dll) ;;
  *)
    printf 'check-version: the shared library would be named "%s", not ...%s.2.3\n' \
      "$target" "$major" >&2
    status=1 ;;
esac
fi

if [ "$status" -eq 0 ]; then
  printf 'check-version: a %s.2.3 build writes its version into the header, the packed number and the library name\n' "$major"
fi
exit "$status"

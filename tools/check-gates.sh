#!/bin/sh
#
# Prove that each layering gate fails on the defect it exists to catch.
#
# A gate that has never been seen to fail may be measuring nothing. So this
# runs the real scripts - not copies, not mocks - against tests/gates:
#
#   control/   a tree that is correct; the gate must pass it, so that a gate
#              which rejects everything cannot pass this self-test
#   planted-*  a tree with one defect; the gate must exit non-zero AND name
#              the offending file, line or edge, so that a gate which fails
#              for the wrong reason (a typo, a missing tool) does not count
#   (empty)    an empty directory; the gate must fail rather than report
#              success over a population of zero
#
# The link-line check cannot use a committed fixture, because what it reads is
# a built shared object's NEEDED list. So it builds two real ones in a
# temporary directory, against stub libraries: one that links a forbidden
# library and one that links only cutil's stub.
#
# Usage: check-gates.sh   (from the project root)

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
FIX="$ROOT/tests/gates"
CC="${CC:-cc}"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

failures=0
checks=0

fail() {
  printf 'check-gates: FAIL: %s\n' "$*" >&2
  failures=$((failures + 1))
}

# expect_pass <label> <command...>
expect_pass() {
  label="$1"; shift
  checks=$((checks + 1))
  if out="$("$@" 2>&1)"; then
    printf '  ok   %s passes\n' "$label"
  else
    fail "$label should pass but failed:
$out"
  fi
}

# expect_fail <label> <needle> <command...>
# The needle is what the failure message must name.
expect_fail() {
  label="$1"; needle="$2"; shift 2
  checks=$((checks + 1))
  out="$("$@" 2>&1)"
  rc=$?
  if [ "$rc" -eq 0 ]; then
    fail "$label should fail but exited 0:
$out"
  elif ! printf '%s' "$out" | grep -qF -- "$needle"; then
    fail "$label failed, but without naming '$needle':
$out"
  else
    printf '  ok   %s fails, naming %s\n' "$label" "$needle"
  fi
}

mkdir "$work/empty"

printf 'check-labels\n'
L="$HERE/check-labels.sh"
expect_pass 'labels/control' "$L" "$FIX/labels/control"
expect_fail 'labels/planted-missing' 'nolabel.h' "$L" "$FIX/labels/planted-missing"
expect_fail 'labels/planted-wrong-directory' 'a/frame.h is labelled stable' \
  "$L" "$FIX/labels/planted-wrong-directory"
expect_fail 'labels/planted-bogus' '"experimental"' "$L" "$FIX/labels/planted-bogus"
expect_fail 'labels/planted-prose-only' 'top.h has no @stability' \
  "$L" "$FIX/labels/planted-prose-only"
expect_fail 'labels/planted-b-free' 'b/ctx.h is labelled free' \
  "$L" "$FIX/labels/planted-b-free"
expect_fail 'labels/planted-two-labels' 'more than one @stability' \
  "$L" "$FIX/labels/planted-two-labels"
expect_fail 'labels/empty' 'measuring nothing' "$L" "$work/empty"

printf 'check-direction\n'
D="$HERE/check-direction.sh"
expect_pass 'direction/control' "$D" "$FIX/direction/control"
expect_fail 'direction/planted-b-includes-a' 'b/ctx.h:' \
  "$D" "$FIX/direction/planted-b-includes-a"
expect_fail 'direction/planted-top-includes-a' 'top.h:' \
  "$D" "$FIX/direction/planted-top-includes-a"
expect_fail 'direction/planted-b-includes-umbrella' 'b/ctx.h:' \
  "$D" "$FIX/direction/planted-b-includes-umbrella"
expect_fail 'direction/planted-b-includes-a-relative' 'b/ctx.h:' \
  "$D" "$FIX/direction/planted-b-includes-a-relative"
expect_fail 'direction/planted-b-includes-umbrella-relative' 'b/ctx.h:' \
  "$D" "$FIX/direction/planted-b-includes-umbrella-relative"
expect_fail 'direction/planted-top-includes-umbrella-relative' 'top.h:' \
  "$D" "$FIX/direction/planted-top-includes-umbrella-relative"
expect_fail 'direction/empty' 'measuring nothing' "$D" "$work/empty"

printf 'check-edges --includes\n'
E="$HERE/check-edges.sh"
expect_pass 'edges/control' "$E" --includes "$FIX/edges/control"
expect_fail 'edges/planted-src-heap' 'runtime-core -> runtime-heap' \
  "$E" --includes "$FIX/edges/planted-src-heap"
expect_fail 'edges/planted-header-debug' 'runtime-core -> runtime-debug' \
  "$E" --includes "$FIX/edges/planted-header-debug"
expect_fail 'edges/planted-ctang' 'runtime-core -> tang' \
  "$E" --includes "$FIX/edges/planted-ctang"
expect_fail 'edges/planted-engine' 'runtime-core -> lang-tang' \
  "$E" --includes "$FIX/edges/planted-engine"
expect_fail 'edges/empty (includes)' 'measuring nothing' \
  "$E" --includes "$work/empty"

expect_fail 'edges/planted-relative' 'runtime-core -> runtime-heap' \
  "$E" --includes "$FIX/edges/planted-relative"
expect_fail 'edges/planted-example' 'runtime-core -> runtime-heap' \
  "$E" --includes "$FIX/edges/planted-example"

printf 'check-edges --links\n'
# The .dll arm (objdump -p is the reader there) has run under wine, cross-built
# (suite/tools/xwin in the workspace); it has not run on a Windows machine.
case "$(uname -s)" in
  MINGW* | MSYS*) SHEXT=dll; SHFLAGS="-shared" ;;
  Darwin) SHEXT=dylib; SHFLAGS="-dynamiclib" ;;
  *) SHEXT=so; SHFLAGS="-shared -fPIC" ;;
esac
stubs="$work/stubs"
mkdir -p "$stubs" "$work/planted" "$work/control" "$work/dev"
printf 'int stub_heap(void) { return 1; }\n' > "$work/heap.c"
printf 'int stub_cutil(void) { return 2; }\n' > "$work/cutil.c"
printf 'int stub_heap(void);\nint planted(void) { return stub_heap(); }\n' \
  > "$work/planted.c"
printf 'int stub_cutil(void);\nint control(void) { return stub_cutil(); }\n' \
  > "$work/control.c"

built=1
# shellcheck disable=SC2086
{
  $CC $SHFLAGS -o "$stubs/libghoti.io-runtime-heap-0.$SHEXT" "$work/heap.c" &&
  $CC $SHFLAGS -o "$stubs/libghoti.io-cutil-0.$SHEXT" "$work/cutil.c" &&
  $CC $SHFLAGS -o "$work/planted/libplanted.$SHEXT" "$work/planted.c" \
    -L"$stubs" -Wl,--no-as-needed -l:libghoti.io-runtime-heap-0.$SHEXT &&
  $CC $SHFLAGS -o "$stubs/libghoti.io-cutil-dev.$SHEXT" "$work/cutil.c" &&
  $CC $SHFLAGS -o "$work/dev/libdev.$SHEXT" "$work/control.c" \
    -L"$stubs" -Wl,--no-as-needed -l:libghoti.io-cutil-dev.$SHEXT &&
  $CC $SHFLAGS -o "$work/control/libcontrol.$SHEXT" "$work/control.c" \
    -L"$stubs" -Wl,--no-as-needed -l:libghoti.io-cutil-0.$SHEXT
} >"$work/build.log" 2>&1 || built=0
if [ "$built" -eq 0 ]; then
  fail "could not build the link-line fixtures:
$(cat "$work/build.log")"
else
  expect_pass 'links/control' "$E" --links "$work/control"
  expect_pass 'links/cutil with a BRANCH suffix (-dev)' "$E" --links "$work/dev"
  expect_fail 'links/planted-heap' 'runtime-core -> runtime-heap' \
    "$E" --links "$work/planted"
  expect_fail 'links/planted-heap names the object' 'libplanted' \
    "$E" --links "$work/planted"
fi
expect_fail 'links/empty' 'measuring nothing' "$E" --links "$work/empty"

# A name that merely begins with an allowed one is another library, not that
# library with a branch suffix.
extra="$work/extra"
mkdir -p "$extra" "$work/extra-stubs"
printf 'int stub_extra(void) { return 4; }\n' > "$work/extra-stub.c"
printf 'int stub_extra(void);\nint extended(void) { return stub_extra(); }\n' > "$work/extended.c"
# shellcheck disable=SC2086
if $CC $SHFLAGS -o "$work/extra-stubs/libghoti.io-cutil-extra-0.$SHEXT" "$work/extra-stub.c" &&
  $CC $SHFLAGS -o "$extra/libextended.$SHEXT" "$work/extended.c" \
    -L"$work/extra-stubs" -Wl,--no-as-needed -l:libghoti.io-cutil-extra-0.$SHEXT \
    >"$work/extra-build.log" 2>&1; then
  expect_fail 'links/planted-name-extending-an-allowed-one' 'runtime-core -> cutil-extra' \
    "$E" --links "$extra"
else
  fail "could not build the extended-name fixture"
fi

printf 'check-stamps\n'
S="$HERE/check-stamps.py"
expect_pass 'stamps/control' python3 "$S" "$FIX/stamps/control.mk"
expect_fail 'stamps/planted-no-stamp' 'names no flag stamp' \
  python3 "$S" "$FIX/stamps/planted-no-stamp.mk"
expect_fail 'stamps/planted-unrecorded-flag' 'EXTRA_CFLAGS' \
  python3 "$S" "$FIX/stamps/planted-unrecorded-flag.mk"
expect_fail 'stamps/planted-link-unrecorded' 'LINK_EXTRA' \
  python3 "$S" "$FIX/stamps/planted-link-unrecorded.mk"
expect_fail 'stamps/planted-no-printf' 'does not printf' \
  python3 "$S" "$FIX/stamps/planted-no-printf.mk"
expect_fail 'stamps/planted-no-stamps' 'no flag stamps at all' \
  python3 "$S" "$FIX/stamps/planted-no-stamps.mk"
expect_fail 'stamps/planted-no-compile' 'no compile rules' \
  python3 "$S" "$FIX/stamps/planted-no-compile.mk"
expect_fail 'stamps/planted-no-link' 'no link lines' \
  python3 "$S" "$FIX/stamps/planted-no-link.mk"

printf 'check-version\n'
V="$HERE/check-version.sh"
MK="$ROOT/Makefile"
expect_pass 'version/control' "$V" "$MK" "$ROOT"
sed -e 's/VERSION_MINOR \$(VERSION_MINOR_ONLY)/VERSION_MINOR $(VERSION_PATCH_ONLY)/' \
    -e 's/VERSION_PATCH \$(VERSION_PATCH_ONLY)/VERSION_PATCH $(VERSION_MINOR_ONLY)/' \
    "$MK" > "$work/swapped.mk"
expect_fail 'version/planted-minor-and-patch-swapped' 'MINOR 2' \
  "$V" "$work/swapped.mk" "$ROOT"
sed -e 's/VERSION_PATCH \$(VERSION_PATCH_ONLY)/VERSION_PATCH 0/' \
    "$MK" > "$work/nopatch.mk"
expect_fail 'version/planted-patch-dropped' 'PATCH 3' \
  "$V" "$work/nopatch.mk" "$ROOT"
grep -v "SPDX-License-Identifier" "$MK" > "$work/nospdx.mk"
expect_fail 'version/planted-no-spdx' 'an SPDX identifier' \
  "$V" "$work/nospdx.mk" "$ROOT"
expect_fail 'version/no-makefile' 'could not generate' \
  "$V" "$work/absent.mk" "$ROOT"

printf 'check-wiring\n'
W="$HERE/check-wiring.py"
expect_pass 'wiring/control' python3 "$W" "$FIX/wiring/control.mk"
expect_pass 'wiring/the real Makefile' python3 "$W" "$ROOT/Makefile"
expect_fail 'wiring/planted-gate-dropped' 'check-gates is not in TEST_GATES' \
  python3 "$W" "$FIX/wiring/planted-gate-dropped.mk"
expect_fail 'wiring/planted-test-ignores-gates' 'does not depend on $(TEST_GATES)' \
  python3 "$W" "$FIX/wiring/planted-test-ignores-gates.mk"
expect_fail 'wiring/planted-recipe-empty' 'check-labels recipe never runs' \
  python3 "$W" "$FIX/wiring/planted-recipe-empty.mk"
expect_fail 'wiring/planted-edges-no-links' 'check-edges.sh --links' \
  python3 "$W" "$FIX/wiring/planted-edges-no-links.mk"
expect_fail 'wiring/planted-no-target' 'check-gates has no target' \
  python3 "$W" "$FIX/wiring/planted-no-target.mk"
expect_fail 'wiring/planted-no-list' 'measuring nothing' \
  python3 "$W" "$FIX/wiring/planted-no-list.mk"

# check-symbols reads a built shared object's dynamic symbol table, which is
# nm -D and so Linux only (the Makefile skips it elsewhere too).
if [ "$(uname -s)" = Linux ]; then
  printf 'check-symbols\n'
  Y="$HERE/check-symbols.sh"
  tok=ghotiio_runtime_core_0
  sym="$work/sym"
  mkdir -p "$sym"
  printf 'int %s_grcore_ctx_make(void) { return 1; }\n' "$tok" > "$sym/good.c"
  printf 'int grcore_leaked(void) { return 1; }\n' > "$sym/bad.c"
  printf 'int %s_grcore_missing(void);\nint %s_grcore_user(void) { return %s_grcore_missing(); }\n' \
    "$tok" "$tok" "$tok" > "$sym/split.c"
  printf 'static int hidden(void) { return 1; }\n' > "$sym/empty.c"
  built=1
  {
    $CC -shared -fPIC -o "$sym/good.so" "$sym/good.c" &&
    $CC -shared -fPIC -o "$sym/bad.so" "$sym/bad.c" &&
    $CC -shared -fPIC -o "$sym/split.so" "$sym/split.c" &&
    $CC -shared -fPIC -o "$sym/empty.so" "$sym/empty.c"
  } >"$sym/build.log" 2>&1 || built=0
  if [ "$built" -eq 0 ]; then
    fail "could not build the symbol fixtures:
$(cat "$sym/build.log")"
  else
    expect_pass 'symbols/control' "$Y" "$sym/good.so" "$tok" "$FIX/symbols/control"
    expect_fail 'symbols/planted-unnamespaced-export' 'grcore_leaked' \
      "$Y" "$sym/bad.so" "$tok" "$FIX/symbols/control"
    expect_fail 'symbols/planted-split-symbol' 'split symbol' \
      "$Y" "$sym/split.so" "$tok" "$FIX/symbols/control"
    expect_fail 'symbols/planted-no-api' 'grcore_ctx_make' \
      "$Y" "$sym/good.so" "$tok" "$FIX/symbols/planted-no-api"
    expect_fail 'symbols/planted-no-macros' 'ctx_internal.h' \
      "$Y" "$sym/good.so" "$tok" "$FIX/symbols/planted-no-macros"
    expect_fail 'symbols/planted-bad-guard' 'MY_OWN_GUARD_H' \
      "$Y" "$sym/good.so" "$tok" "$FIX/symbols/planted-bad-guard"
    expect_fail 'symbols/planted-dup-guard' 'sharing an include guard' \
      "$Y" "$sym/good.so" "$tok" "$FIX/symbols/planted-dup-guard"
    expect_fail 'symbols/exports-nothing' 'measuring nothing' \
      "$Y" "$sym/empty.so" "$tok" "$FIX/symbols/control"
    expect_fail 'symbols/no-library' 'measuring nothing' \
      "$Y" "$sym/absent.so" "$tok" "$FIX/symbols/control"
  fi
fi


if [ "$failures" -ne 0 ]; then
  printf 'check-gates: %d of %d checks failed\n' "$failures" "$checks" >&2
  exit 1
fi
printf 'check-gates: all %d checks behaved: each gate fails on its planted defect, passes its control, and fails on an empty population\n' \
  "$checks"

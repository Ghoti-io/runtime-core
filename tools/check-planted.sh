#!/bin/sh
#
# Prove that the tests of the cross-heap handle table fail on the defect each
# exists to catch.
#
# A test that has never been seen to fail may be measuring nothing. This builds
# a throwaway copy of the library under build/planted/tree, applies ONE patch at
# a time from tests/planted/, builds the test it names, and requires that test
# to FAIL; then takes the patch out again, rebuilds, and requires the same test
# to PASS (the control: a test that fails for any reason at all, a crash or a
# missing library, would otherwise count as having caught the defect).
#
# A patch starts with comment lines that this script reads:
#
#   # test: <the test executable, e.g. testHandles>
#   # filter: <the gtest filter naming the test that must notice it>
#   # arm: release | tsan
#
# and then is a unified diff against the library's tree (`patch -p1`, fuzz 0).
# The `tsan` arm builds with ThreadSanitizer (the tree `make test-tsan` builds)
# and is for the defects only it can see: a missing lock.
#
# The work is a copy, so the working tree is never touched.
#
# Usage: tools/check-planted.sh [--arm release|tsan|all] [NN ...]
#   PREFIX  passed to make (the rpath, and where cutil is found); optional
#   JOBS    parallel compiles (default 4)
#   NN      run only the patches numbered so (e.g. 03 17)
#
# Needs `patch` and a C and C++ toolchain; exits 1 if it finds no patch to
# run, because a gate over an empty population passes anything.

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
JOBS="${JOBS:-4}"
arm_wanted=all
only=""

while [ $# -gt 0 ]; do
  case "$1" in
    --arm) arm_wanted="${2:?--arm needs release, tsan or all}"; shift 2 ;;
    *) only="$only $1"; shift ;;
  esac
done

if ! command -v patch >/dev/null 2>&1; then
  printf 'check-planted: patch is not installed; this gate cannot run\n' >&2
  exit 1
fi

case "$(uname -s)" in
  Linux) os=linux ;;
  *) printf 'check-planted: this gate runs on Linux only\n' >&2; exit 1 ;;
esac

WORK="$ROOT/build/planted/tree"
mkdir -p "$ROOT/build/planted"
if [ -d "$WORK" ]; then
  rm -r "$WORK"
fi
mkdir -p "$WORK"
for item in Makefile include src tests pkgconfig; do
  if [ -e "$ROOT/$item" ]; then
    cp -a "$ROOT/$item" "$WORK/$item"
  fi
done
# The patches are read from the real tree; the copy needs no gate fixtures.
rm -r "$WORK/tests/gates" "$WORK/tests/planted" 2>/dev/null

failures=0
ran=0

fail() {
  printf 'check-planted: FAIL: %s\n' "$*" >&2
  failures=$((failures + 1))
}

# build <arm> <test>   (quiet unless it fails)
build() {
  arm="$1"; test="$2"
  if [ "$arm" = tsan ]; then dir="release-tsan"; else dir="release"; fi
  target="build/$os/$dir/apps/$test"
  if [ -n "${PREFIX:-}" ]; then
    out="$(make -C "$WORK" -j"$JOBS" PREFIX="$PREFIX" "$target" 2>&1)"
  else
    out="$(make -C "$WORK" -j"$JOBS" "$target" 2>&1)"
  fi
  rc=$?
  [ "$rc" -eq 0 ] || printf '%s\n' "$out" | tail -25 >&2
  return "$rc"
}

# run <arm> <test> <filter>   exit status of the test
run() {
  arm="$1"; test="$2"; filter="$3"
  if [ "$arm" = tsan ]; then
    exe="$WORK/build/$os/release-tsan/apps/$test"
    LD_PRELOAD= TSAN_OPTIONS="halt_on_error=1:history_size=7" \
      "$exe" --gtest_filter="$filter" >"$WORK/last.log" 2>&1
  else
    exe="$WORK/build/$os/release/apps/$test"
    "$exe" --gtest_filter="$filter" >"$WORK/last.log" 2>&1
  fi
}

# The tests that ran must be more than zero: a filter that matches nothing
# exits 0 and would pass the control while measuring nothing.
ran_some() {
  grep -q '^\[  PASSED  \] [1-9]' "$WORK/last.log"
}

# header <patch> <key>
header() {
  sed -n "s/^# $2: //p" "$1" | head -1
}

controls_done=""

for patch_file in "$ROOT"/tests/planted/*.patch; do
  [ -e "$patch_file" ] || continue
  name="$(basename "$patch_file" .patch)"
  num="${name%%-*}"
  if [ -n "$only" ]; then
    case " $only " in *" $num "*) ;; *) continue ;; esac
  fi
  test="$(header "$patch_file" test)"
  filter="$(header "$patch_file" filter)"
  arm="$(header "$patch_file" arm)"
  if [ -z "$test" ] || [ -z "$filter" ] || [ -z "$arm" ]; then
    fail "$name: the patch header lacks test, filter or arm"
    continue
  fi
  if [ "$arm_wanted" != all ] && [ "$arm_wanted" != "$arm" ]; then
    continue
  fi

  # The control, once per arm, test and filter: unpatched, the named tests pass.
  case " $controls_done " in
    *" $arm/$test/$filter "*) ;;
    *)
      if ! build "$arm" "$test"; then
        fail "$name: the unpatched tree does not build"
        continue
      fi
      if ! run "$arm" "$test" "$filter" || ! ran_some; then
        fail "$name: the control fails: $test --gtest_filter=$filter does not pass unpatched"
        tail -15 "$WORK/last.log" >&2
        continue
      fi
      controls_done="$controls_done $arm/$test/$filter"
      ;;
  esac

  if ! (cd "$WORK" && patch -p1 --fuzz=0 -s <"$patch_file"); then
    fail "$name: the patch no longer applies (regenerate it against the current sources)"
    # patch may have written part of it
    (cd "$WORK" && patch -R -p1 --fuzz=0 -s -f <"$patch_file") >/dev/null 2>&1
    continue
  fi
  if ! build "$arm" "$test"; then
    fail "$name: the patched tree does not build, which proves nothing"
    (cd "$WORK" && patch -R -p1 --fuzz=0 -s <"$patch_file")
    continue
  fi
  ran=$((ran + 1))
  if run "$arm" "$test" "$filter"; then
    fail "$name: $test --gtest_filter=$filter PASSED with the defect planted"
  else
    printf '  ok   %s fails on its defect\n' "$name"
  fi
  if ! (cd "$WORK" && patch -R -p1 --fuzz=0 -s <"$patch_file"); then
    fail "$name: the patch could not be taken out; stopping"
    exit 1
  fi
  # Rebuilt and rerun, so that the next patch starts from a tree that passes.
  if ! build "$arm" "$test" || ! run "$arm" "$test" "$filter" || ! ran_some; then
    fail "$name: the control after removing the patch does not pass"
  fi
done

if [ "$ran" -eq 0 ]; then
  printf 'check-planted: no patch was run; this gate is measuring nothing\n' >&2
  exit 1
fi
if [ "$failures" -ne 0 ]; then
  printf 'check-planted: %d problem(s) in %d patch(es)\n' "$failures" "$ran" >&2
  exit 1
fi
printf 'check-planted: %d planted defect(s) each caught, each control passing\n' "$ran"

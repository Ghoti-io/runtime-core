#!/bin/sh
#
# Fail if a header includes against the A/B direction.
#
# AD-3: A (the frame protocol, a/) may include B (the execution context, b/)
# and the top-level shared basics. B and the top level never include A. The
# umbrella runtime-core.h is the only header allowed to include both, because
# that is what an umbrella is.
#
# What counts as "including A": a path through a/, and also the umbrella
# itself, which pulls A in by the back door. An include of runtime-core.h from
# b/ would pass a check that only looked for "a/".
#
# Usage: check-direction.sh <root>
#   <root> holds include/ghoti.io/runtime-core/. Fails on an empty population.

set -eu

ROOT="${1:?usage: check-direction.sh <root>}"
BASE="$ROOT/include/ghoti.io/runtime-core"

if [ ! -d "$BASE" ]; then
  printf 'check-direction: %s does not exist; this gate is measuring nothing\n' \
    "$BASE" >&2
  exit 1
fi

# Everything that is not A, and is not the umbrella.
subjects="$(find "$BASE" -type f -name '*.h' ! -path "$BASE/a/*" \
  ! -path "$BASE/runtime-core.h" | sort)"
if [ -z "$subjects" ]; then
  printf 'check-direction: no B or top-level headers under %s; this gate is measuring nothing\n' \
    "$BASE" >&2
  exit 1
fi

status=0
count=0
for h in $subjects; do
  count=$((count + 1))
  # file:line:text for every include that reaches A. Four spellings: the
  # installed path, the umbrella, and the two relative forms.
  hits="$(grep -nE '^[[:space:]]*#[[:space:]]*include[[:space:]]*([<"](ghoti\.io/)?runtime-core/(a/|runtime-core\.h)|"(\.\.?/)*a/)' "$h" || true)"
  if [ -n "$hits" ]; then
    printf '%s\n' "$hits" | while IFS= read -r line; do
      n="${line%%:*}"
      text="${line#*:}"
      printf 'check-direction: %s:%s includes A from outside A: %s\n' \
        "$h" "$n" "$text" >&2
    done
    status=1
  fi
done

if [ "$status" -ne 0 ]; then
  exit 1
fi
printf 'check-direction: %d B and top-level headers, none includes A\n' "$count"

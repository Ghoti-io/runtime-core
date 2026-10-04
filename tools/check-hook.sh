#!/bin/sh
#
# Fail if .githooks/commit-msg strips what it must keep or keeps what it must
# strip. The hook removes machine attribution and preserves human co-authors;
# its assistant names once matched anywhere inside a word, so a co-author
# called Raider or Devine lost their trailer.
#
# Usage: check-hook.sh <hook>

set -u

HOOK="${1:?usage: check-hook.sh <hook>}"
[ -f "$HOOK" ] || { printf 'check-hook: %s does not exist\n' "$HOOK" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM
failures=0
checks=0

# run_hook <name> <message...>: runs the hook over a message, prints the result
run_hook() {
  f="$work/msg"
  printf '%s\n' "$@" > "$f"
  sh "$HOOK" "$f" || return 1
  cat "$f"
}

expect_kept() { # <needle> <message...>
  needle="$1"; shift
  checks=$((checks + 1))
  if ! run_hook "$@" | grep -qF -- "$needle"; then
    printf 'check-hook: FAIL: "%s" was stripped\n' "$needle" >&2
    failures=$((failures + 1))
  fi
}
expect_stripped() { # <needle> <message...>
  needle="$1"; shift
  checks=$((checks + 1))
  if run_hook "$@" | grep -qF -- "$needle"; then
    printf 'check-hook: FAIL: "%s" was kept\n' "$needle" >&2
    failures=$((failures + 1))
  fi
}

expect_stripped 'noreply@anthropic.com' 'Subject' '' 'Body.' '' \
  'Co-Authored-By: Claude Opus <noreply@anthropic.com>'
expect_kept 'Body.' 'Subject' '' 'Body.' '' \
  'Co-Authored-By: Claude Opus <noreply@anthropic.com>'
expect_stripped 'Generated with' 'Subject' '' 'Body.' '' \
  'Generated with [Claude Code](https://claude.com/claude-code)'
expect_stripped 'cursor_session' 'Subject' '' 'Body.' '' 'cursor_session: abc123'
expect_stripped 'Session-Id' 'Subject' '' 'Body.' '' 'Session-Id: abc (claude)'
expect_stripped 'Copilot' 'Subject' '' 'Body.' '' 'Co-authored-by: Copilot <c@example.com>'
# Human co-authors, whose names contain an assistant's name inside a word.
expect_kept 'Ann Devine' 'Subject' '' 'Body.' '' 'Co-authored-by: Ann Devine <ann@example.com>'
expect_kept 'Raider Jones' 'Subject' '' 'Body.' '' 'Co-authored-by: Raider Jones <r@example.com>'
expect_kept 'Precursor' 'Subject' '' 'Body.' '' 'Signed-off-by: A. Precursor <a@example.com>'
expect_kept 'Pat Smith' 'Subject' '' 'Body.' '' 'Co-authored-by: Pat Smith <pat@example.com>'
# A message that is only attribution is not emptied.
checks=$((checks + 1))
out="$(run_hook 'Co-Authored-By: Claude <noreply@anthropic.com>')"
[ -n "$out" ] || { printf 'check-hook: FAIL: an all-attribution message was emptied\n' >&2; failures=$((failures + 1)); }

if [ "$failures" -ne 0 ]; then
  printf 'check-hook: %d of %d checks failed\n' "$failures" "$checks" >&2
  exit 1
fi
printf 'check-hook: all %d checks behaved\n' "$checks"

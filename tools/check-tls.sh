#!/bin/sh
# Fail if a built shared library uses a thread-local model that a dlopen of it
# cannot rely on, or (on x86-64, i386 and aarch64) does not use TLS
# descriptors for its thread-locals.
#
#   - initial-exec (or local-exec) in a shared object claims a slot in the
#     static TLS block. A dlopen takes it from glibc's surplus and fails when
#     that is exhausted; musl and the BSDs have less or none. Hosts load
#     engines, and defiant-core loads modules, with dlopen. The linker marks
#     such an object with DF_STATIC_TLS and leaves TPOFF relocations in it.
#   - the library's one thread-local is read by the owner check on every
#     guarded call, and the Makefile compiles it with descriptors
#     (-mtls-dialect=gnu2; aarch64's default) so that a read costs a short
#     resolver rather than __tls_get_addr. A build that lost the flag still
#     works, and only this check sees it.
#
# Usage: check-tls.sh <shared-library>
# Fails on an empty population: a library with no TLS relocation at all means
# the check read nothing it could judge.

set -u

LIB="${1:?usage: check-tls.sh <shared-library>}"

if [ ! -f "$LIB" ]; then
  printf 'check-tls: %s does not exist; this gate is measuring nothing\n' "$LIB" >&2
  exit 1
fi

machine="$(readelf -h "$LIB" 2>/dev/null | awk -F: '/Machine:/ {gsub(/^[ \t]+/, "", $2); print $2}')"
if [ -z "$machine" ]; then
  printf 'check-tls: %s is not an ELF object readelf can read; this gate is measuring nothing\n' "$LIB" >&2
  exit 1
fi

relocs="$(readelf -rW "$LIB" 2>/dev/null)"
tls="$(printf '%s\n' "$relocs" | grep -E 'R_[A-Z0-9_]*(TLS|TPOFF|DTPMOD|DTPOFF|TPREL)' || true)"
if [ -z "$tls" ]; then
  printf 'check-tls: %s has no TLS relocation; this gate is measuring nothing\n' "$LIB" >&2
  exit 1
fi

status=0
if readelf -dW "$LIB" 2>/dev/null | grep -E '\(FLAGS\)' | grep -q STATIC_TLS; then
  printf 'check-tls: %s is marked STATIC_TLS: it was built with initial-exec TLS, which a dlopen cannot rely on\n' "$LIB" >&2
  status=1
fi
static_relocs="$(printf '%s\n' "$tls" | grep -E 'TPOFF|TPREL|TLS_IE|GOTTPOFF' | grep -vE 'DTPOFF|DTPREL' || true)"
if [ -n "$static_relocs" ]; then
  printf 'check-tls: %s has initial-exec TLS relocations:\n%s\n' "$LIB" "$static_relocs" >&2
  status=1
fi

case "$machine" in
  *X86-64* | *80386* | *AArch64*)
    if ! printf '%s\n' "$tls" | grep -q 'TLSDESC'; then
      printf 'check-tls: %s (%s) reaches its thread-locals without TLS descriptors (-mtls-dialect=gnu2 lost?):\n%s\n' \
        "$LIB" "$machine" "$tls" >&2
      status=1
    fi
    ;;
esac

if [ "$status" -eq 0 ]; then
  printf 'check-tls: %s (%s) reaches its thread-locals through %d dlopen-safe relocation(s)\n' \
    "$LIB" "$machine" "$(printf '%s\n' "$tls" | wc -l)"
fi
exit "$status"

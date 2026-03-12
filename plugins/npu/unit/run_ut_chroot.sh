#!/bin/bash
# Run NPU plugin unit tests inside a chroot environment to prevent
# corruption of the host /dev.
#
# Usage: run_ut_chroot.sh <test-binary>

set -e

die() { echo "ERROR: $*" >&2; exit 1; }

[ $# -ge 1 ] || die "Usage: $0 <test-binary>"
[ -x "$1" ] || die "Test binary not found or not executable: $1"

TEST_BIN="$(realpath "$1")"
BUILD_DIR="$(pwd)"

# Re-exec inside a private mount namespace if not already there.
if [ -z "$_NPU_UT_IN_NS" ]; then
    export _NPU_UT_IN_NS=1
    exec unshare --mount --propagation private "$0" "$@"
fi

ROOT=$(mktemp -d /tmp/npu-ut-root.XXXXXX)
trap 'rm -rf "$ROOT" 2>/dev/null || true' EXIT

mount -t tmpfs tmpfs "$ROOT" -o mode=755,size=128m

mkdir -p "$ROOT"/{dev,tmp,run,proc}

# /dev on its own tmpfs so tests can safely umount+rmdir /dev
mount -t tmpfs tmpfs "$ROOT/dev" -o mode=755
mknod "$ROOT/dev/null" c 1 3 && chmod 666 "$ROOT/dev/null"
mknod "$ROOT/dev/zero" c 1 5 && chmod 666 "$ROOT/dev/zero"
mknod "$ROOT/dev/full" c 1 7 && chmod 666 "$ROOT/dev/full"

mount -t proc proc "$ROOT/proc" 2>/dev/null || true

# Bind-mount shared library directories so the dynamically linked
# test binary can resolve its dependencies inside the chroot.
for d in $(ldd "$TEST_BIN" 2>/dev/null \
           | grep -o '/[^ ]*' \
           | xargs -I{} dirname {} \
           | sort -u); do
    [ -d "$d" ] || continue
    mkdir -p "$ROOT$d"
    mount --bind "$d" "$ROOT$d" 2>/dev/null || true
done

# Bind-mount build directory so gcov can persist .gcda files
mkdir -p "$ROOT$BUILD_DIR"
mount --bind "$BUILD_DIR" "$ROOT$BUILD_DIR"

exec chroot "$ROOT" "$TEST_BIN"

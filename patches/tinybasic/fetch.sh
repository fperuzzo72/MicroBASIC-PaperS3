#!/usr/bin/env bash
# Fetches Stefan Lenz's IoT BASIC and lays its portable interpreter core into
# editor/lib/TinyBasic/, where PlatformIO picks it up as a library.
#
# The source is deliberately NOT vendored into this repository. Upstream's
# licensing is ambiguous (root LICENSE says BSD-3-Clause as of 2025, while
# every source header still carries a GPL v3 notice), and this project has no
# need to resolve that: GPL obligations trigger on distribution, and fetching
# at build time keeps third-party source out of our tree entirely. Same
# approach already used for the readers in patches/cpr-vcodex/.
#
# A full-history mirror is kept at ../_backups/stefan-tinybasic.bundle in case
# upstream disappears; point UPSTREAM at it to build offline.
set -euo pipefail

cd "$(dirname "$0")/../.."

UPSTREAM="${UPSTREAM:-https://github.com/slviajero/tinybasic.git}"
# Pinned so a build is reproducible and an upstream change can never silently
# alter the interpreter under us. Bump deliberately, re-running the patches.
PIN="99c6b631aebeede8badb73833e4207b4d89c8ed0"   # 2026-06-18

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
DEST="editor/lib/TinyBasic"

echo "==> cloning $UPSTREAM"
git clone --quiet "$UPSTREAM" "$WORK/src"
git -C "$WORK/src" checkout --quiet "$PIN"

# The Posix variant's basic.c is the portable interpreter core -- plain C with
# no Arduino dependencies (verified: it compiles standalone on the host). Its
# runtime.c is only *one* implementation of the runtime contract; ours lives in
# editor/src/tb_runtime.cpp, so we take everything except that.
echo "==> installing interpreter core into $DEST"
rm -rf "$DEST"
mkdir -p "$DEST"
for f in basic.c basic.h hardware.h language.h common.h runtime.h; do
  cp "$WORK/src/Basic2/Posix/$f" "$DEST/$f"
done

echo "==> applying patches"
for p in patches/tinybasic/[0-9]*.py; do
  python3 "$p"
done

echo "==> done. $DEST now holds:"
ls -la "$DEST"

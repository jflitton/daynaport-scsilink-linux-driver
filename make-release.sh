#!/bin/sh
# make-release.sh - build a distributable source tarball for one kernel target.
#   RUN ON THE MAC.
#
# Each kernel target lives in its own directory (linux-2.0, linux-7.0, ...).
# This packages one target's sources together with the shared, version-
# independent files (lib/*.h, COPYING) flattened into a single self-contained
# tarball a user can unpack and build with a purely local include path.
#
#   ./make-release.sh linux-2.0          # version 0.1 (default)
#   ./make-release.sh linux-2.0 0.3      # explicit driver version
#
# Produces scsilink-<VERSION>-kernel-<KVER>.tar.gz that unpacks to
# scsilink-<VERSION>-kernel-<KVER>/ (e.g. scsilink-0.3-kernel-2.0/).  The kernel
# target is in the name so 2.0 and 7.0 releases at the same driver version don't
# collide.
# Upload it as the asset on the GitHub release for the matching tag; don't
# commit it into the repo.
#
# gzip (not xz/bzip2) so a Linux 2.0 box's stock tar+gzip can `tar xzf` it.

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

VERDIR=${1%/}                   # tolerate a trailing slash from tab-completion
VERSION="${2:-0.1}"

if [ -z "$VERDIR" ] || [ ! -d "$VERDIR" ]; then
    echo "usage: $0 <kernel-target-dir> [version]   e.g. $0 linux-2.0 0.3" >&2
    exit 1
fi

KVER=${VERDIR#linux-}           # linux-2.0 -> 2.0
DIST="scsilink-$VERSION-kernel-$KVER"

# Per-target sources, plus shared files vendored in and flattened.
TARGET_FILES="scsilink.c Makefile install.sh README.md CHANGES"
SHARED_FILES="lib/daynaport.h COPYING"

rm -rf "$DIST" "$DIST.tar.gz"
mkdir "$DIST"

for f in $TARGET_FILES; do
    [ -f "$VERDIR/$f" ] || { echo "ERROR: missing $VERDIR/$f" >&2; exit 1; }
    cp "$VERDIR/$f" "$DIST/"
done
for f in $SHARED_FILES; do
    [ -f "$f" ] || { echo "ERROR: missing shared file $f" >&2; exit 1; }
    cp "$f" "$DIST/"
done

# The repo keeps shared headers in lib/ and the target Makefile finds them with
# -I../lib.  In the flattened tarball they sit next to the source, so rewrite the
# include path to the local dir.  (#include "daynaport.h" would also resolve via
# the quote-include search, but a clean -I. avoids a dangling ../lib.)
sed -i '' 's#-I\.\./lib#-I.#g' "$DIST/Makefile"

# Archive format.  Pre-2.4 targets must use a tar a period GNU tar can read:
#   --format=ustar     macOS tar (bsdtar) defaults to pax interchange format,
#                      which writes a typeflag-'x' extended header per file (for
#                      hi-res mtime).  The era's GNU tar can't read 'x' and errors
#                      "unknown file type 'x'".  ustar uses plain octal mtime.
#   COPYFILE_DISABLE=1 stop macOS from adding ._* AppleDouble / xattr entries.
# Modern targets (2.4+) take a normal tar.
case "$KVER" in
    2.0|2.2)
        COPYFILE_DISABLE=1 tar --format=ustar -czf "$DIST.tar.gz" "$DIST" ;;
    *)
        tar -czf "$DIST.tar.gz" "$DIST" ;;
esac
rm -rf "$DIST"

echo ">> built $DIST.tar.gz"
tar tzf "$DIST.tar.gz"

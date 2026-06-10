#!/bin/sh
# make-release.sh - build the distributable source tarball.  RUN ON THE MAC.
#
# Produces scsilink-<VERSION>.tar.gz that unpacks to scsilink-<VERSION>/ with
# just the files a user needs to build + install.  Upload that as the asset on
# the GitHub release for the matching tag; don't commit it into the repo.
#
#   ./make-release.sh            # version 0.1 (default)
#   ./make-release.sh 0.2        # other version
#
# gzip (not xz/bzip2) so a Linux 2.0 box's stock tar+gzip can `tar xzf` it.

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

VERSION="${1:-0.1}"
DIST="scsilink-$VERSION"
FILES="scsilink.c Makefile install.sh README.md COPYING CHANGES"

rm -rf "$DIST" "$DIST.tar.gz"
mkdir "$DIST"
for f in $FILES; do cp "$f" "$DIST/"; done

# Archive format matters for a Linux 2.0 target:
#   --format=ustar     macOS tar (bsdtar) defaults to pax interchange format,
#                      which writes a typeflag-'x' extended header per file (for
#                      hi-res mtime).  The era's GNU tar can't read 'x' and errors
#                      "unknown file type 'x'".  ustar uses plain octal mtime.
#   COPYFILE_DISABLE=1 stop macOS from adding ._* AppleDouble / xattr entries.
COPYFILE_DISABLE=1 tar --format=ustar -czf "$DIST.tar.gz" "$DIST"
rm -rf "$DIST"

echo ">> built $DIST.tar.gz"
tar tzf "$DIST.tar.gz"

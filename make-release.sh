#!/bin/sh
# make-release.sh - build a distributable source tarball of the whole repo.
#   RUN ON THE MAC.
#
# A release is the entire repository at the current commit (HEAD), compressed
# into one archive named with that commit's date (e.g. 2026-06-15).  Every kernel
# target and the shared lib/ ship together in their normal layout, so a user
# unpacks the tree and builds the target they need from its subdirectory -- the
# per-target Makefiles already find the shared header via -I../lib, so nothing is
# flattened or rewritten.
#
#   ./make-release.sh
#
# Produces scsilink_<DATE>.tar.gz that unpacks to scsilink_<DATE>/.  Upload it as
# the asset on the GitHub release for the matching tag; don't commit it into the
# repo.
#
# Built from the committed tree (git archive HEAD): no build products, editor
# cruft, or uncommitted edits leak in -- commit before releasing.
#
# Re-archived as gzip + ustar (not pax/xz/bzip2) so even a Linux 2.0 box's stock
# tar+gzip can `tar xzf` it: macOS bsdtar defaults to pax extended headers and
# git archive prepends a pax global header, neither of which the era's GNU tar
# can read.

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

# Commit date of HEAD, YYYY-MM-DD.  Reproducible -- the same commit always yields
# the same name (a build-time date would drift every day you re-run).
DATE=$(git log -1 --date=format:'%Y-%m-%d' --format=%cd)
DIST="scsilink_$DATE"

rm -rf "$DIST" "$DIST.tar.gz"

# Stage the committed tree under one date-named dir, then re-archive as plain
# ustar so any tar -- modern or vintage -- can read it.
mkdir "$DIST"
git archive HEAD | tar -x -f - -C "$DIST"
COPYFILE_DISABLE=1 tar --format=ustar -czf "$DIST.tar.gz" "$DIST"
rm -rf "$DIST"

echo ">> built $DIST.tar.gz"
tar tzf "$DIST.tar.gz"

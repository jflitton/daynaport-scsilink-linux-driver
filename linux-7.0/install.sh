#!/bin/sh
# install.sh - build, install, and (optionally) wire boot-loading for the
# DaynaPORT SCSI/Link driver.  RUN AS ROOT on the target (Linux 7.x).
#
# It drives the Makefile (build + modules_install + depmod), then enables
# autoload at boot via /etc/modules-load.d/scsilink.conf.
#
#   ./install.sh                 build against the running kernel's build tree
#   KDIR=/path ./install.sh      build against a different kernel build tree
#
# Re-runnable: safe to run again after editing scsilink.c.

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

KDIR="${KDIR:-/lib/modules/$(uname -r)/build}"
[ -d "$KDIR" ] || {
    echo "ERROR: no kernel build tree at $KDIR (set KDIR=...)" >&2; exit 1; }

echo ">> building (KDIR=$KDIR)"
make KDIR="$KDIR"

echo ">> installing module + running depmod"
make KDIR="$KDIR" install

# The module carries MODULE_ALIAS_SCSI_DEVICE(TYPE_PROCESSOR), so udev autoloads
# it when the adapter is enumerated.  The adapter is usually present at boot, so
# also list it for modules-load.d as a belt-and-suspenders.
CONF=/etc/modules-load.d/scsilink.conf
if [ -d /etc/modules-load.d ]; then
    if [ -f "$CONF" ] && grep -q '^scsilink$' "$CONF"; then
        echo ">> $CONF already loads scsilink"
    else
        echo scsilink > "$CONF"
        echo ">> wrote $CONF (loads at boot)"
    fi
else
    cat <<EOF
>> Module installed.  Load it now with:
       modprobe scsilink
   To load at boot, add 'scsilink' to your distro's module list.
EOF
fi

echo ">> done.  modprobe scsilink, then configure the interface as any NIC."

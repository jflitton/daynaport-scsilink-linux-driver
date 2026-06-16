#!/bin/sh
# install.sh - build, install, and (optionally) wire boot-loading for the
# DaynaPORT SCSI/Link driver.  RUN AS ROOT on the target (Linux 2.0.x).
#
# It just drives the Makefile (build + install + depmod), then adds a load line
# to Slackware's /etc/rc.d/rc.modules if that file exists; on other distros it
# prints how to load the module instead.
#
#   ./install.sh                 build against /usr/src/linux and install
#   KERNEL=/path ./install.sh    build against a different kernel source tree
#
# Re-runnable: safe to run again after editing scsilink.c.

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

KERNEL="${KERNEL:-/usr/src/linux}"
[ -d "$KERNEL/include/linux" ] || {
    echo "ERROR: no kernel source at $KERNEL (set KERNEL=...)" >&2; exit 1; }

echo ">> building (KERNEL=$KERNEL)"
make KERNEL="$KERNEL"

echo ">> installing module + running depmod"
make KERNEL="$KERNEL" install

RCMOD=/etc/rc.d/rc.modules
# Matches Slackware's own rc.modules idiom (it loads lp/slip/ppp the same way);
# depmod (run by `make install`) lets modprobe resolve it by name.  rc.modules is
# sourced from rc.S, before rc.M runs rc.inet1, so the driver is up first.
LOADLINE="/sbin/modprobe scsilink"

if [ -f "$RCMOD" ]; then
    if grep -q scsilink "$RCMOD"; then
        echo ">> $RCMOD already loads scsilink"
    else
        echo "$LOADLINE" >> "$RCMOD"
        echo ">> added load line to $RCMOD (loads at boot, before rc.inet1)"
    fi
else
    cat <<EOF
>> Module installed.  Load it now with:
       modprobe scsilink      (or: insmod scsilink.o)
   To load at boot, add that line to your distro's module list, e.g.
       Slackware : /etc/rc.d/rc.modules
       Debian    : /etc/modules
       Red Hat   : /etc/rc.d/rc.local
   It must come up before the network scripts configure ethN.
EOF
fi

echo ">> done.  Configure ethN as for any NIC (Slackware: /etc/rc.d/rc.inet1)."

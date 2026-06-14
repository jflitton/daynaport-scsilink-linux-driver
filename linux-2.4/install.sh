#!/bin/sh
# install.sh - build, install, and (optionally) wire boot-loading for the
# DaynaPORT SCSI/Link driver.  RUN AS ROOT on the target (Linux 2.4.x).
#
# It just drives the Makefile (build + install + depmod), then adds the module
# to Debian's /etc/modules if that file exists (Sarge ships 2.4.27); on other
# distros it prints how to load the module instead.
#
#   ./install.sh                 build against the running kernel's tree, install
#   KERNEL=/path ./install.sh    build against a different kernel source tree
#
# Re-runnable: safe to run again after editing scsilink.c.

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

# Match the Makefile's default: the 2.4 build symlink, else /usr/src/linux.
if [ -z "$KERNEL" ]; then
    if [ -d "/lib/modules/$(uname -r)/build" ]; then
        KERNEL="/lib/modules/$(uname -r)/build"
    else
        KERNEL="/usr/src/linux"
    fi
fi
[ -d "$KERNEL/include/linux" ] || {
    echo "ERROR: no kernel source at $KERNEL (set KERNEL=...)" >&2; exit 1; }

echo ">> building (KERNEL=$KERNEL)"
make KERNEL="$KERNEL"

echo ">> installing module + running depmod"
make KERNEL="$KERNEL" install

MODFILE=/etc/modules
# Debian loads modules listed in /etc/modules at boot (via /etc/init.d/modutils),
# before networking is configured -- so the driver is up before ethN is brought up.
if [ -f "$MODFILE" ]; then
    if grep -q '^scsilink$' "$MODFILE"; then
        echo ">> $MODFILE already loads scsilink"
    else
        echo "scsilink" >> "$MODFILE"
        echo ">> added scsilink to $MODFILE (loads at boot, before networking)"
    fi
else
    cat <<EOF
>> Module installed.  Load it now with:
       modprobe scsilink      (or: insmod scsilink.o)
   To load at boot, add it to your distro's module list, e.g.
       Debian    : /etc/modules
       Slackware : /etc/rc.d/rc.modules
       Red Hat   : /etc/rc.d/rc.local
   It must come up before the network scripts configure ethN.
EOF
fi

echo ">> done.  Configure ethN as for any NIC (Debian: /etc/network/interfaces)."

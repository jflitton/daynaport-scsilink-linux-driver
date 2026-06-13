# DaynaPORT SCSI/Link — Linux 2.0.x driver

The **Linux 2.0.x** build of the DaynaPORT SCSI/Link Ethernet driver. Builds
out-of-tree as a loadable module (`scsilink.o`) against any 2.0.x kernel source.
See the [project README](../README.md) for the overview and the shared protocol
constants + RX parser in [`lib/`](../lib).

Tested on 486SLC @ 33MHz / Adaptec AHA-1542 / BlueSCSI V2 release v2026.04.27.

## Install

Download the latest release tarball from the [releases](https://github.com/jflitton/daynaport-scsilink-linux-driver/releases) page or clone this repo.

Needs the target's configured kernel source (default `/usr/src/linux`, matching
the running kernel) and the period gcc (2.7.x). As root:

```sh
./install.sh          # build, install to /lib/modules/`uname -r`/net, depmod
```

`install.sh` also adds a load line to Slackware's `/etc/rc.d/rc.modules` if it
finds it; on other distros it prints how to load at boot.

Or run the steps by hand:

```sh
make                                 # build scsilink.o against /usr/src/linux
make KERNEL=/usr/src/linux-2.0.40    # ...or against another kernel tree
make install                         # install + depmod
modprobe scsilink                    # load now (or: insmod scsilink.o)
```

Then bring the interface up as you would any NIC (on Slackware, set the address
in `/etc/rc.d/rc.inet1`):

```sh
ifconfig eth0 192.168.1.50 netmask 255.255.255.0 up
route add default gw 192.168.1.1
```

## Performance
Download performance is about 70kB/sec on a 486/33, limited by the fact that receives
aren't interrupt-driven -- we have to poll for them.  Upload performance is 3-4x better
as we do have the benefit of interrupts when packets are sent.

The RX poll cadence can be tuned at load time (milliseconds) without rebuilding:

```sh
insmod scsilink.o poll_ms=80 poll0_ms=20 fast_hold=16   # these are the defaults
```

`poll_ms` is the idle interval, `poll0_ms` the interval while data is flowing, and
`fast_hold` how many empty polls to stay at the fast rate before relaxing to idle.

## Files

| File         | Purpose                                                        |
|--------------|----------------------------------------------------------------|
| `scsilink.c` | the driver                                                     |
| `Makefile`   | out-of-tree module build (kernel-dir + MODVERSIONS autodetect) |
| `install.sh` | build + install + optional boot wiring                         |
| `COPYING`    | GNU General Public License, version 2                          |
| `CHANGES`    | release history                                                |

## License

Copyright (C) 2026 Jeff Flitton <jeff@flitton.dev>. GPL v2 — see [`COPYING`](COPYING).

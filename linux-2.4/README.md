# DaynaPORT SCSI/Link — Linux 2.4.x driver

The **Linux 2.4.x** build of the DaynaPORT SCSI/Link Ethernet driver. Builds
out-of-tree as a loadable module (`scsilink.o`) against any 2.4.x kernel source.
See the [project README](../README.md) for the overview and the shared protocol
constants + RX parser in [`lib/`](../lib).

Intended for Debian 3.1 (Sarge), which ships a 2.4.27 kernel — e.g. on a 68k Mac
such as the Quadra 605. **Status: builds against the 2.4.27 headers; not yet
tested on hardware.**

## Install

Download the latest release tarball from the [releases](https://github.com/jflitton/daynaport-scsilink-linux-driver/releases) page or clone this repo.

Needs the target's configured kernel source and the gcc the kernel was built with
(Sarge: gcc 3.3, sometimes 2.95). On Debian, install the matching kernel headers
(`apt-get install kernel-headers-$(uname -r)`); the build defaults to
`/lib/modules/$(uname -r)/build`, falling back to `/usr/src/linux`. As root:

```sh
./install.sh          # build, install to /lib/modules/`uname -r`/net, depmod
```

`install.sh` adds `scsilink` to Debian's `/etc/modules` if it finds it (so it
loads at boot, before the network comes up); on other distros it prints how to
load at boot.

Or run the steps by hand:

```sh
make                                 # build against the running kernel's tree
make KERNEL=/usr/src/linux-2.4.27    # ...or against another kernel tree
make install                         # install + depmod
modprobe scsilink                    # load now (or: insmod scsilink.o)
```

Then bring the interface up as you would any NIC:

```sh
ifconfig eth0 192.168.1.50 netmask 255.255.255.0 up
route add default gw 192.168.1.1
```

On Debian, make it persistent in `/etc/network/interfaces`.

## Tuning

Receive is polled (the adapter has no RX interrupt), so the RX poll cadence and
READ request size are tunable at load time without rebuilding:

```sh
insmod scsilink.o poll_ms=80 poll0_ms=20 fast_hold=16 rx_req_len=4096   # defaults
```

`poll_ms` is the idle interval, `poll0_ms` the interval while data is flowing, and
`fast_hold` how many empty polls to stay at the fast rate before relaxing to idle
(all in milliseconds). `rx_req_len` is the byte count requested per READ — the
device may cap or ignore it, and it is clamped to 2048–16384; the default 4096
already covers BlueSCSI's max 2-frame batch, so raising it changes nothing on
BlueSCSI or ZuluSCSI.

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

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

Then bring the interface up as you would any NIC.

## Performance Tuning

The knobs below tune the RX poll cadence and READ request size (see
[How it works](../README.md#how-it-works) for the device behavior they tune).
On 2.0 they are set at load time only — module parameters are not writable at
runtime:

```sh
insmod scsilink.o poll_ms=80 poll0_ms=20 fast_hold=16 rx_req_len=4096   # defaults shown
```

| Param        | Meaning                                                          |
|--------------|------------------------------------------------------------------|
| `poll_ms`    | idle interval — between empty READs when no data is waiting (ms)  |
| `poll0_ms`   | fast interval — between empty READs during the post-activity hold (ms) |
| `fast_hold`  | empty polls to stay at the fast rate before relaxing to idle      |
| `rx_req_len` | bytes requested per READ; the device may cap or ignore it (2048–16384) |
| `debug`      | log per-READ RX stats every 256 reads (0=off, the default)        |

Measured on my 486SLC @ 33 MHz test rig: about 250 kBytes/s down, 330 kBytes/s up.
Receive is slower because it has to poll while transmit does not (see
[How it works](../README.md#how-it-works)); on this slow a host the polled-read
ceiling shows up as the gap between the two. The defaults are already near-optimal
here — the knobs are mainly for characterization.

## Files

| File         | Purpose                                                        |
|--------------|----------------------------------------------------------------|
| `scsilink.c` | the driver (`#include`s the shared `lib/daynaport.h`)          |
| `Makefile`   | out-of-tree module build (kernel-dir + MODVERSIONS autodetect) |
| `install.sh` | build + install + optional boot wiring                         |
| `CHANGES`    | release history                                                |

## License

Copyright (C) 2026 Jeff Flitton <jeff@flitton.dev>. GPL v2 — see
[`COPYING`](../COPYING).

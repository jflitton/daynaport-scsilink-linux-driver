# DaynaPORT SCSI/Link — Linux 2.4.x driver

The **Linux 2.4.x** build of the DaynaPORT SCSI/Link Ethernet driver. Builds
out-of-tree as a loadable module (`scsilink.o`) against any 2.4.x kernel source.
See the [project README](../README.md) for the overview and the shared protocol
constants + RX parser in [`lib/`](../lib).

Tested on a Pentium III 600 / Symbios Logic 53c875J / ZuluSCSI Blaster with FW v2026.06.12,
on kernel 2.4.31 (Slackware 10.2).

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

Then bring the interface up as you would any NIC.

## Parameters

The knobs below tune the RX poll cadence, READ request size, and TX/RX arbitration
(see [How it works](../README.md#how-it-works) for the device behavior they tune).
On 2.4 they are set at load time only — module parameters are not writable at
runtime:

```sh
insmod scsilink.o poll_ms=80 poll0_ms=20 fast_hold=16 rx_req_len=4096 tx_burst=16   # defaults shown
```

| Param        | Meaning                                                          |
|--------------|------------------------------------------------------------------|
| `poll_ms`    | idle interval — between empty READs when no data is waiting (ms)  |
| `poll0_ms`   | fast interval — between empty READs during the post-activity hold (ms) |
| `fast_hold`  | empty polls to stay at the fast rate before relaxing to idle      |
| `rx_req_len` | bytes requested per READ; the device may cap or ignore it (2048–16384) |
| `tx_burst`   | max frames to send before yielding to one RX poll (1–16)         |
| `debug`      | log per-READ RX stats every 256 reads (0=off, the default)        |

The defaults are already near-optimal — the knobs are mainly for characterization
or much slower hosts. `tx_burst` trades upload throughput (larger — back-to-back
WRITEs amortize per-command overhead) against RX fairness (smaller — inbound drains
before the adapter's RX FIFO overflows); 16 is a good default.

## Performance

On the test rig, TX runs ~1.2 MB/s with RX at comparable throughput over the
polled READ path.

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

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

There is no RX interrupt, so receive is polled. Three knobs control the RX
cadence, `rx_req_len` sizes each READ request, and `tx_burst` balances TX against
RX on the device's single command slot. They are set at load time (Linux 2.4
module parameters are not writable at runtime):

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

A READ that returns frames is followed immediately by the next, so a live
download polls back-to-back at the speed of the SCSI round-trip; the cadence
knobs govern only *empty* polling — how often to probe when no data is waiting.
The receive ceiling is therefore set by the per-READ round-trip and the adapter,
not the host poll rate, so the defaults are already near-optimal and the knobs
are most useful for characterization or on much slower hosts.

`rx_req_len` is headroom for targets that honor a larger request. ZuluSCSI and
BlueSCSI are both based on SCSI2SD, whose firmware hard-caps a batch at 2 frames,
so raising it past the default changes nothing on those two — the default 4096
already covers that 2-frame max batch.

`tx_burst` arbitrates the device's one-command-at-a-time channel: the engine
sends at most this many queued frames before forcing an RX poll. Since the device
cannot transmit and receive at once, a large burst favors upload throughput
(back-to-back WRITEs amortize per-command SCSI overhead) while a small one favors
RX fairness (inbound ACKs and replies drain sooner instead of overflowing the
adapter's small RX FIFO while the host holds the bus writing). The default of 16
tends to be optimal.

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

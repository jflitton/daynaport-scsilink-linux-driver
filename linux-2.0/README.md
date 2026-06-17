# DaynaPORT SCSI/Link — Linux 2.0.x driver

The **Linux 2.0.x** build of the DaynaPORT SCSI/Link Ethernet driver. Builds
out-of-tree as a loadable module (`scsilink.o`) against any 2.0.x kernel source.

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

Then configure the interface up as you would any NIC

## Performance
Download runs about 250kB/sec, upload about 330kB/sec on a 486SLC @ 33Mhz.
The gap is the SCSI side, not the link. Nothing signals inbound frames, 
so receive has to poll for them with READ(6) commands, spending bus time
probing even when no frame is waiting. Transmit never polls — the stack 
hands us a frame when there's one to send, so a WRITE only happens when there's
real work. That polling asymmetry is the gap.

The RX poll cadence and READ request size can be tuned at load time without
rebuilding:

```sh
insmod scsilink.o poll_ms=80 poll0_ms=20 fast_hold=16 rx_req_len=4096   # these are the defaults
```

A READ that returns frames is followed immediately by the next, so a live
download polls back-to-back at the speed of the SCSI round-trip; the cadence
knobs govern only *empty* polling. `poll_ms` is the idle interval (between empty
READs when no data is waiting), `poll0_ms` the fast interval during the brief
post-activity hold, and `fast_hold` how many empty polls to stay fast before
relaxing to idle (all in milliseconds). `rx_req_len` is the byte count requested
per READ — the device may cap or ignore it, and it is clamped to 2048–16384; the
default 4096 already covers ZuluSCSI/BlueSCSI's max 2-frame batch, so raising it
changes nothing.

`debug=1` logs per-READ RX yield and rate every 256 READs (off by default) — handy
for telling a poll-limited rig from a read-latency-limited one.

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

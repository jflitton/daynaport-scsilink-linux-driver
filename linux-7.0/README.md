# DaynaPORT SCSI/Link — Linux 7.x driver

The **Linux 7.x** build of the DaynaPORT SCSI/Link Ethernet driver. Builds
out-of-tree as a loadable module (`scsilink.ko`) via kbuild against a configured
7.x kernel tree. See the [project README](../README.md) for the overview and the
shared protocol constants + RX parser in [`lib/`](../lib).

Tested on x86-64 (AMD A10-7700K) / Adaptec AHA-2940UW / ZuluSCSI Blaster with FW v2026.06.12,
on kernel 7.0.12 (Fedora 44).

## Install

Needs the running kernel's configured build tree — on most distros the
`kernel-devel` / `linux-headers` package, symlinked at
`/lib/modules/$(uname -r)/build` — plus `gcc` and `make`. As root:

```sh
./install.sh                      # build, modules_install, depmod, boot autoload
KDIR=/path/to/linux ./install.sh  # ...or build against another kernel tree
```

`install.sh` builds `scsilink.ko`, installs it under
`/lib/modules/$(uname -r)/updates/`, runs `depmod`, and writes
`/etc/modules-load.d/scsilink.conf` so it loads at boot. The module also carries
`MODULE_ALIAS_SCSI_DEVICE(TYPE_PROCESSOR)`, so udev autoloads it when the adapter
is enumerated.

Or by hand:

```sh
make                  # build scsilink.ko against /lib/modules/`uname -r`/build
make KDIR=/usr/src/linux   # ...or against another kernel tree
make install          # modules_install + depmod (as root)
modprobe scsilink     # load now
```

> **Secure Boot:** `make install` tries to sign the module with the kernel's
> signing key, which the distro `kernel-devel` package does not ship — so you'll
> see a harmless `sign-file: ... signing_key.pem: No such file` and the module is
> left *unsigned*. That only matters if Secure Boot enforces module signatures:
> either disable Secure Boot, or enroll your own MOK key and sign with it. With
> Secure Boot off, the unsigned module loads normally.

## Performance Tuning

There is no RX interrupt, so receive is polled. Three knobs control the RX
cadence, `rx_req_len` sizes each READ request, and `tx_burst` balances TX against
RX on the device's single command slot. They are all **writable at runtime** as
well as settable at load (the poll loop reads them live each cycle):

```sh
modprobe scsilink poll_ms=80 poll0_ms=20 fast_hold=16 rx_req_len=4096 tx_burst=16   # at load (defaults shown)
echo 5 > /sys/module/scsilink/parameters/poll0_ms       # live; takes effect next poll
```

| Param        | Meaning                                                          |
|--------------|------------------------------------------------------------------|
| `poll_ms`    | idle interval — between READs when no data is waiting (ms)        |
| `poll0_ms`   | fast interval — while data is flowing, and during the hold (ms)   |
| `fast_hold`  | empty polls to stay at the fast rate before relaxing to idle      |
| `rx_req_len` | bytes requested per READ; the device may cap or ignore it (2048–16384) |
| `tx_burst`   | max frames to send before yielding to one RX poll (1–16)         |

Measured on the test rig: ~8.25 Mbit/s TX, ~7.75 Mbit/s RX. For this system,
the defaults are already near-optimal — a `poll0_ms` sweep showed RX is
flat from ~5–20 ms and actually *degrades* below that (polling harder just floods
the adapter with empty READs), because the receive ceiling is set by the per-READ
round-trip and the adapter, not the host poll rate. The knobs are most useful for
characterization and on much slower hosts.

`rx_req_len` is headroom for targets that honor a larger request. ZuluSCSI and
BlueSCSI are both based on SCSI2SD, whose firmware hard-caps a batch at 2 frames,
so raising it past the default changes nothing on those two — the default 4096
already covers that 2-frame max batch.

`tx_burst` arbitrates the device's one-command-at-a-time channel: the poll loop
sends at most this many frames before forcing an RX poll. Since the device cannot
transmit and receive at once, a large burst favors upload throughput (back-to-back
WRITEs amortize per-command SCSI overhead) while a small one favors RX fairness
(inbound ACKs and replies drain sooner instead of overflowing the adapter's small
RX FIFO while the host holds the bus writing). In my test, the default of 16 looks
to be about right.

## Files

| File         | Purpose                                               |
|--------------|-------------------------------------------------------|
| `scsilink.c` | the driver (`#include`s the shared `lib/daynaport.h`) |
| `Makefile`   | out-of-tree kbuild module (`obj-m`, `-I../lib`)       |
| `install.sh` | build + install                                       |
| `CHANGES`    | release history                                       |

## License

Copyright (C) 2026 Jeff Flitton <jeff@flitton.dev>. GPL v2 — see
[`COPYING`](../COPYING).

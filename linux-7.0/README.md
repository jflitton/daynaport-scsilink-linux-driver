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

## Parameters

The knobs below tune the RX poll cadence, READ request size, and TX/RX arbitration
(see [How it works](../README.md#how-it-works) for the device behavior they tune).
On 7.x they are all **writable at runtime** as well as settable at load — the poll
loop re-reads them live each cycle:

```sh
modprobe scsilink poll_ms=80 poll0_ms=20 fast_hold=16 rx_req_len=4096 tx_burst=16   # at load (defaults shown)
echo 5 > /sys/module/scsilink/parameters/poll0_ms       # live; takes effect next poll
```

| Param        | Meaning                                                          |
|--------------|------------------------------------------------------------------|
| `poll_ms`    | idle interval — between READs when no data is waiting (ms)        |
| `poll0_ms`   | fast interval — between empty READs during the post-activity hold (ms) |
| `fast_hold`  | empty polls to stay at the fast rate before relaxing to idle      |
| `rx_req_len` | bytes requested per READ; the device may cap or ignore it (2048–16384) |
| `tx_burst`   | max frames to send before yielding to one RX poll (1–16)         |
| `debug`      | log per-READ RX stats every 256 reads (0=off, the default)        |

The defaults are already near-optimal — the knobs are mainly for characterization
or much slower hosts. Driving `poll0_ms` toward continuous *empty* polling actually
*hurts*: hammering the adapter with empty READs starves its frame handling.
`tx_burst` trades upload throughput (larger — back-to-back WRITEs amortize
per-command overhead) against RX fairness (smaller — inbound drains before the
adapter's RX FIFO overflows); 16 is a good default.

## Performance
Download runs about 995kB/sec, upload about 1.18MB/sec on this rig. With productive
READs issued back-to-back, download runs close to the upload rate, trailing it only
by the slightly heavier READ round-trip over a WRITE on this adapter.

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

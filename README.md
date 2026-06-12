# daynaport-scsilink-linux-driver

A loadable Ethernet driver for the **DaynaPORT SCSI/Link** SCSI-to-Ethernet
adapter, as emulated by [BlueSCSI V2](https://bluescsi.com/),
[ZuluSCSI](https://zuluscsi.com/), and [PiSCSI](https://github.com/PiSCSI/piscsi).
It binds to the SCSI processor device the adapter presents and exposes it as a
standard Ethernet interface (`eth0`) — letting a vintage (or modern) machine use
a BlueSCSI/ZuluSCSI/PiSCSI DaynaPORT emulation as a SCSI-attached NIC. Each
driver builds out-of-tree as a loadable module against its kernel's source.

The Linux SCSI and networking subsystems were rewritten many times across the
2.0 → 7.x span, so a single source file can't span them. Instead the repo holds
**one self-contained driver per kernel era**, each in its own directory, sharing
only what is genuinely version-independent (the DaynaPORT protocol logic).

## Supported kernels

| Kernel target | Directory                | Status                              | Driver version |
|---------------|--------------------------|-------------------------------------|----------------|
| Linux 2.0.x   | [`linux-2.0/`](linux-2.0/) | Working — tested on i386 / AHA-1542 / BlueSCSI V2 | 0.3 |
| Linux 7.x     | `linux-7.0/` *(planned)*   | Not started                         | —              |

Intermediary targets (2.2, 2.4, 2.6, 3.x, …) are planned to bridge the 2.0
original and the 7.0 port.

## Repository layout

```
.
├── README.md          # this file
├── COPYING            # GPLv2 — shared, vendored into each release tarball
├── lib/               # shared, version-independent C
│   └── daynaport.h    # protocol constants + callback-based RX frame parser
├── reference/         # protocol documentation (version-independent)
│   ├── daynaport.md   # DaynaPORT opcode / framing reference
│   └── SLINKCMD.TXT   # Dayna's original SCSI/Link command set
├── make-release.sh    # package one target into a self-contained tarball
└── linux-2.0/         # the Linux 2.0.x driver (build/install instructions inside)
    ├── scsilink.c     # #includes the shared lib/daynaport.h
    ├── Makefile
    ├── install.sh
    ├── README.md
    └── CHANGES
```

The protocol logic — opcode/framing constants **and** the RX record parser —
lives once in [`lib/daynaport.h`](lib/daynaport.h). It is kernel-API-free and
C89-clean, so the same header compiles under gcc 2.7.x for the 2.0 driver and a
modern toolchain for later ports; each driver supplies its own per-frame delivery
callback that hands packets up the era's network stack.

## Build & install

See the README inside the target directory for that kernel — e.g.
[`linux-2.0/README.md`](linux-2.0/README.md). In short:

```sh
cd linux-2.0
./install.sh          # build, install, depmod (as root)
```

Pre-built release tarballs are on the
[releases](https://github.com/jflitton/daynaport-scsilink-linux-driver/releases)
page; each is self-contained (the shared header and `COPYING` are flattened in
alongside the driver source).

## License

GPLv2 — see [`COPYING`](COPYING).

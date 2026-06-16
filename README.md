# daynaport-scsilink-linux-driver

A loadable Ethernet driver for the **DaynaPORT SCSI/Link** SCSI-to-Ethernet
adapter, as emulated by [ZuluSCSI](https://zuluscsi.com/), [BlueSCSI V2](https://bluescsi.com/), 
and [PiSCSI](https://github.com/PiSCSI/piscsi).

It binds to the SCSI processor device the adapter presents and exposes it as a
standard Ethernet interface (e.g. `eth0`).  Each
driver builds out-of-tree as a loadable module against its kernel's source.

The Linux SCSI and networking subsystems were rewritten many times across the
2.0 → 7.x span, so a single source file can't span them. Instead the repo holds
**one self-contained driver per kernel era**, each in its own directory, sharing
only what is genuinely version-independent (the DaynaPORT protocol logic).

## Supported kernels

| Kernel target | Directory                |
|---------------|--------------------------|
| Linux 2.0.x   | [`linux-2.0/`](linux-2.0/) |
| Linux 7.x     | [`linux-7.0/`](linux-7.0/) |

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
├── linux-2.0/         # the Linux 2.0.x driver
│   ├── scsilink.c
│   ├── Makefile
│   ├── install.sh
│   ├── README.md
│   └── CHANGES
└── linux-7.0/         # the Linux 7.x driver
    ├── scsilink.c
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

## License

GPLv2 — see [`COPYING`](COPYING).

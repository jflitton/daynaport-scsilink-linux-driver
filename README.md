# daynaport-scsilink-linux-driver

A loadable Ethernet driver for the **DaynaPORT SCSI/Link** SCSI-to-Ethernet
adapter on **Linux 2.0.x**, as emulated by [BlueSCSI V2](https://bluescsi.com/)
and [PiSCSI](https://github.com/PiSCSI/piscsi). It binds to the SCSI processor device the adapter presents and
exposes it as a standard Ethernet interface (`eth0`). Builds out-of-tree as a
loadable module (`scsilink.o`) against any 2.0.x kernel source.

Tested on i386 / Adaptec AHA-1542 / BlueSCSI V2.

## Install

Downloads from the [releases](https://github.com/jflitton/daynaport-scsilink-linux-driver/releases) page

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
Download performance is limited to about 40kB/sec, due to the fact that receives aren't
interrupt-driven, we have to poll for them.  Upload performance is 3-4x better as we
do have the benefit of interrupts when packets are sent.

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

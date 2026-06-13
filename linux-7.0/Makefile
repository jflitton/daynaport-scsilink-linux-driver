# Makefile - out-of-tree kbuild for the DaynaPORT SCSI/Link driver on Linux 7.x.
#
# Builds scsilink.ko as a loadable module against a configured Linux 7.x kernel
# build tree.  It touches no file inside that tree, so the same source works
# across the 7.x series (rebuild per kernel, as modules always need).
#
#   make                          build scsilink.ko against the running kernel
#   make KDIR=/path/to/linux      build against a kernel tree other than the default
#   make install                  modules_install under /lib/modules/`uname -r` + depmod
#   make clean                    remove build products
#
# The shared, version-independent protocol header lives one level up in lib/
# (lib/daynaport.h); -I$(src)/../lib puts it on the include path.  The driver
# pulls it in with #include "daynaport.h".

obj-m += scsilink.o
ccflags-y += -I$(src)/../lib

# Where the configured kernel build tree lives.  The 2.4+ convention is the
# /lib/modules/<release>/build symlink created by the running kernel's package.
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

.PHONY: all install clean

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

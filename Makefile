# Makefile - out-of-tree build for the DaynaPORT SCSI/Link driver.
#
# Builds scsilink.o as a loadable module against ANY configured Linux 2.0.x
# kernel source tree -- it touches no file inside that tree, so the same source
# works across the whole 2.0 series (rebuild per kernel, as modules always need).
#
#   make                      build scsilink.o
#   make KERNEL=/path/to/src  build against a kernel tree other than the default
#   make install              install under /lib/modules/`uname -r`/net + depmod
#   make clean                remove build products
#
# The kernel tree must be configured and have generated headers present
# (i.e. `make config && make dep` has been run there), so that
# include/linux/{version.h,autoconf.h} exist.

# Where the configured kernel source lives.  (2.0 has no /lib/modules/*/build
# symlink -- that is a 2.4+ convention -- so default to the classic path.)
# NB: plain `ifeq` default, NOT `?=` -- GNU make 3.76 (the Slackware 3.x / 2.0
# era make) does not apply `?=` here and leaves KERNEL empty.  `make KERNEL=...`
# still overrides, since a set value makes the ifeq false.
ifeq ($(KERNEL),)
KERNEL    = /usr/src/linux
endif
AUTOCONF  = $(KERNEL)/include/linux/autoconf.h

# gcc, not make's built-in `cc` default (still overridable: make CC=...)
CC        = gcc

# CPU tuning flag.  A module should be tuned like the kernel it loads into, and
# the authoritative source for that is the kernel's own config -- so derive the
# flag from the CONFIG_M* symbol the kernel was built with.  (This mirrors what
# period out-of-tree packages such as pcmcia-cs did: read the kernel config to
# match its flags, rather than hardcode one or guess from /proc/cpuinfo -- what
# matters for a module is the kernel's build, not the raw CPU.)  Falls back to a
# safe per-arch default if the config can't be read.  Override anytime with
# `make CPU=-m68060` (or `make CPU=` for none).
#
# x86 note: gcc 2.7.x (the 2.0-era compiler) has no Pentium tuning, so the whole
# 486/586/686 family was compiled -m486 -- that is why -m486 is THE customary
# x86 flag of this era, and -m486 code still runs all the way down to a 386.
# m68k note: unlike x86 there is no universal flag (030/040/060 differ in MMU/
# FPU and kernels are built per-CPU), so deriving from the config matters more
# here; the fallback -m68040 suits the Mac Quadras most likely to run this.
ifndef CPU
CPU := $(shell \
    if   grep -qs  'CONFIG_M386 1'           $(AUTOCONF); then echo -m386;   \
    elif grep -Eqs 'CONFIG_M(486|586|686) 1' $(AUTOCONF); then echo -m486;   \
    elif grep -qs  'CONFIG_M68060 1'         $(AUTOCONF); then echo -m68060; \
    elif grep -qs  'CONFIG_M68040 1'         $(AUTOCONF); then echo -m68040; \
    elif grep -qs  'CONFIG_M68030 1'         $(AUTOCONF); then echo -m68030; \
    elif grep -qs  'CONFIG_M68020 1'         $(AUTOCONF); then echo -m68020; \
    elif uname -m | grep -q '68k';           then echo -m68040; \
    elif uname -m | grep -q '86';            then echo -m486;   \
    fi)
endif

MODULE    = scsilink.o
SOURCE    = scsilink.c

# The driver includes "scsi.h"/"hosts.h" with quotes; those live in the kernel's
# drivers/scsi directory, not on the normal include path -- so add it explicitly.
INCLUDES  = -I$(KERNEL)/include -I$(KERNEL)/drivers/scsi

WARN      = -Wall -Wstrict-prototypes
OPT       = -O2 -fomit-frame-pointer -fno-strength-reduce

# If the target kernel was built with CONFIG_MODVERSIONS, its exported symbols
# carry version checksums; a module must be compiled with -DMODVERSIONS and pull
# in the generated modversions.h or insmod will reject it on symbol mismatch.
# Detect it from autoconf.h (absent/!set -> empty -> plain build).
MODVERS  := $(shell grep -s 'CONFIG_MODVERSIONS 1' $(AUTOCONF))
ifneq ($(MODVERS),)
MODFLAGS  = -DMODVERSIONS -include $(KERNEL)/include/linux/modversions.h
endif

CFLAGS    = -D__KERNEL__ -DMODULE $(MODFLAGS) $(INCLUDES) $(WARN) $(OPT) $(CPU)

# Install location for the running kernel.
RELEASE  := $(shell uname -r)
MODDIR    = /lib/modules/$(RELEASE)/net

.PHONY: all install clean

all: $(MODULE)

$(MODULE): $(SOURCE)
	@[ -d $(KERNEL)/include/linux ] || \
	  { echo "ERROR: no kernel source at $(KERNEL) -- set KERNEL=..."; exit 1; }
	$(CC) $(CFLAGS) -c $(SOURCE) -o $(MODULE)

install: $(MODULE)
	mkdir -p $(MODDIR)
	cp $(MODULE) $(MODDIR)/$(MODULE)
	/sbin/depmod -a

clean:
	rm -f $(MODULE) *.o

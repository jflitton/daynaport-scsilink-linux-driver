# Makefile - host-side unit tests for the shared DaynaPORT protocol core.
#
# daynaport.h is deliberately kernel-API-free and C89-clean, so its pure
# functions (CDB assembly, the RX record parser, the adaptive-poll cadence) can
# be unit-tested in plain userspace -- no kernel build tree, no hardware, no rig.
# This is intentionally NOT wired into the driver builds or make-release.sh; it
# is a developer check you run from this directory.
#
#   make test     build and run the tests (nonzero exit if any check fails)
#   make clean    remove the test binary
#
# Runs on the host (Mac or Linux); override the compiler with `make CC=gcc`.

CC      = cc
CFLAGS  = -std=c89 -pedantic -Wall -Wextra -Wstrict-prototypes -I.
TEST    = daynaport_test

.PHONY: test clean

test: $(TEST)
	./$(TEST)

$(TEST): daynaport_test.c daynaport.h
	$(CC) $(CFLAGS) -o $@ daynaport_test.c

clean:
	rm -f $(TEST)

TARGET = ge_patch_unofficial
OBJS = main.o ge_list.o gu.o version.o exports.o

# GePatch Unofficial: a fork of TheFlow's GePatch v0.2. The PRX carries its version and the commit it
# was built from (version.c); a "-dirty" suffix means the build had uncommitted changes.
VERSION = v0.2-unofficial.1
COMMIT := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)$(shell git diff --quiet HEAD -- . 2>/dev/null || echo -dirty)

CFLAGS = -O3 -Os -G0 -Wall -fshort-wchar -fno-pic -mno-check-zero-division
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

BUILD_PRX = 1
PRX_EXPORTS = exports.exp

USE_KERNEL_LIBC = 1
USE_KERNEL_LIBS = 1

LIBS = -lpspsystemctrl_kernel

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak

# The list walker runs over every GE command of every frame: optimise it for speed, not size.
ge_list.o: CFLAGS += -O2

# Rebuilt every time, so the embedded commit is always the one built from.
version.o: CFLAGS += -DGEPATCH_VERSION='"$(VERSION)"' -DGEPATCH_COMMIT='"$(COMMIT)"'
version.o: FORCE
.PHONY: FORCE
FORCE:

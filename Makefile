# pdp11-diskimage -- host tools for 2.9BSD-family (s5fs) disk images.
#
# Default build is dependency-free.  `make FUSE=1` adds the `mount`/`umount`
# subcommands (needs libfuse3-dev); everything else is unchanged.
CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -pedantic
CPPFLAGS ?=

BIN     := bin
SRC     := src

# core multi-tool: `s5fs <command>` (no external dependencies)
BASE := s5fs.c cmd_mkfs.c cmd_mktree.c cmd_tar.c cmd_restore.c cmd_dump.c \
        cmd_fsck.c cmd_fs.c cmd_shell.c cmd_fsdb.c cmd_manifest.c cmd_scavenge.c \
        cmd_vhd.c cmd_util.c s5fs_core.c s5fs_rw.c tree.c fsread.c device.c s5endian.c
HDRS := $(SRC)/cmds.h $(SRC)/s5fs_core.h $(SRC)/s5fs_rw.h $(SRC)/tree.h \
        $(SRC)/fsread.h $(SRC)/fsutil.h $(SRC)/pdp11fs.h $(SRC)/device.h \
        $(SRC)/s5endian.h

FUSE ?= 0
ifeq ($(FUSE),1)
  CPPFLAGS += -DHAVE_FUSE -D_FILE_OFFSET_BITS=64 $(shell pkg-config --cflags fuse3)
  FUSELIBS := $(shell pkg-config --libs fuse3)
  BASE     += cmd_mount.c
endif

OBJSRC := $(addprefix $(SRC)/, $(BASE))

all: $(BIN)/s5fs

$(BIN)/s5fs: $(OBJSRC) $(HDRS) | $(BIN)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(OBJSRC) $(FUSELIBS)

$(BIN):
	mkdir -p $(BIN)

test: $(BIN)/s5fs
	@sh tests/run.sh

clean:
	rm -rf $(BIN)

.PHONY: all clean test

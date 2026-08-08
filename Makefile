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
        cmd_analysis.c cmd_vhd.c cmd_util.c s5fs_core.c s5fs_rw.c tree.c fsread.c \
        device.c s5endian.c
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

# The suite again under AddressSanitizer + UBSan.  Some checks -- notably the
# crafted dump tape -- exercise memory safety, and an out-of-bounds READ does
# not fault on a normal build, so it is only detectable here.
#
# abort_on_error is the point: by default ASan prints and exits 1, which a test
# checking "did it crash?" reads as success.  Aborting turns a sanitizer finding
# into a signal, so the existing crash checks catch it.
#
# detect_leaks is OFF deliberately, not by oversight.  tree.c builds the whole
# in-memory filesystem tree and never frees it: it is the working set until
# tree_serialize() finishes, and the process exits immediately after, so
# freeing it would be ceremony.  LeakSanitizer reports every node (951 bytes
# over a small tar), which would drown the findings that matter.  If you want
# to audit allocation lifetimes, run with detect_leaks=1 by hand and expect
# tree.c's nodes.
SANFLAGS := -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
test-san: $(OBJSRC) $(HDRS) | $(BIN)
	$(CC) $(SANFLAGS) $(CPPFLAGS) -o $(BIN)/s5fs-san $(OBJSRC) $(FUSELIBS)
	@S5=$(BIN)/s5fs-san \
	 ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
	 UBSAN_OPTIONS=halt_on_error=1:abort_on_error=1 \
	 sh tests/run.sh

# Optional corruption fuzzer (needs python3).  NOT part of `make test` and CI
# does not run it -- the suite stays sh + coreutils.  Build sanitized first,
# or a finding is invisible: an out-of-bounds READ does not fault otherwise.
fuzz: $(OBJSRC) $(HDRS) | $(BIN)
	$(CC) $(SANFLAGS) $(CPPFLAGS) -o $(BIN)/s5fs-fuzz $(OBJSRC) $(FUSELIBS)
	@python3 tests/fuzz.py --bin $(BIN)/s5fs-fuzz --iters $(ITERS)

ITERS ?= 100

clean:
	rm -rf $(BIN)

.PHONY: all clean test test-san fuzz

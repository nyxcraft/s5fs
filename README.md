# s5fs -- 2.9BSD-family disk-image tools

A single host multi-tool for building and inspecting the traditional PDP-11
Unix filesystem (retroactively **s5fs**) as SIMH `.dsk` images -- the final link
in the "rebuild 2.9BSD from source to a bootable image" pipeline this toolchain
serves.  It is release-agnostic: one filesystem writer covers **V7, 2.8, 2.9,
and 2.10** (see the compatibility note below), because they share the s5fs
on-disk format byte-for-byte.

Everything sits on one faithful C99 port of 2.9BSD's filesystem writer, so the
images are the ones the era's own `fsck`/kernel expect -- the same byte-fidelity
bar the cross `as`/`cc` are held to.

## Commands

    s5fs mkfs      create an (empty) filesystem image
    s5fs mktree    build an image from a host directory tree
    s5fs tar       image <-> tar archive:  tar c (image->archive), tar x (archive->image, .gz/.bz2/.Z ok)
    s5fs restore   restore a 2.9BSD dump tape into an image
    s5fs dump      write a 2.9BSD dump tape from an image (the reverse of restore)
    s5fs fsck      check/repair an image (-p repairs, -l lists the tree)
    s5fs icheck    block + free-list check (-s salvages the free list)
    s5fs dcheck    directory link-count check
    s5fs clri      clear (zero) inodes by number
    s5fs fsdb      interactive filesystem debugger (-w to edit)
    s5fs manifest  fingerprint an image (path/mode/owner/size/cksum per file)
    s5fs verify    diff an image against a manifest
    s5fs boot      install a primary bootstrap into block 0
    s5fs vhd       wrap/unwrap a fixed-VHD container (wrap/unwrap/info)

    -- file access without a mount (portable; the only Windows-friendly path) --
    s5fs ls        list a directory (-l long, -a all)
    s5fs cat       print file contents to stdout
    s5fs get       copy a file OUT of the image to the host
    s5fs put       copy a host file INTO the image
    s5fs cp        copy a file within the image
    s5fs mv        rename/move a file within the image
    s5fs rm        remove file(s)
    s5fs mkdir     create directory(ies) (-p for parents)
    s5fs rmdir     remove empty directory(ies)
    s5fs chmod     change permission bits (octal)
    s5fs chown     change owner (uid[:gid])
    s5fs chgrp     change group (gid)
    s5fs shell     interactive explorer (cd/ls/get/put/rm/...)

    s5fs devices   list known disk types (`devices <name>` shows its partitions)
    s5fs mount     FUSE-mount an image (-w for read-write)  (`make FUSE=1`)
    s5fs umount    unmount a FUSE mount

`fsck` is the unified checker (blocks + link counts), as 4.x/2.9 `fsck` was;
`icheck`/`dcheck`/`clri` are the classic split tools over the same engine.
`mount` needs `make FUSE=1` (libfuse3); the default build is dependency-free.
Every command that opens an image also accepts a partition selector for
whole-disk images -- `-d <device> -P <letter>` or a raw `-o <start[:len]>` (see
"Partitions and whole-disk images").

## File access without a mount

FUSE is Linux/macOS-only, so the batch file commands and the interactive shell
are the portable way to work inside an image -- and the only option on Windows.
All of them take the image as the first operand and share one mutation engine
(`s5fs_rw`, the same code the FUSE mount uses), so each command leaves the image
consistent (it passes `s5fs fsck` afterwards).

    s5fs ls    [-l] [-a] image [path]      # list a directory (or a file)
    s5fs cat   image path...               # print file contents
    s5fs get   image imgpath [hostpath]    # copy a file out (hostpath "-" = stdout)
    s5fs put   image hostpath [imgpath]    # copy a host file in (imgpath may be a dir)
    s5fs cp    image src dst               # copy; '@' prefix = a HOST path
    s5fs mv    image src dst               # rename/move within the image
    s5fs rm    image path...               # remove file(s)
    s5fs mkdir [-p] image dir...           # create directory(ies)
    s5fs rmdir image dir...                # remove empty directory(ies)
    s5fs chmod image mode path...          # set permission bits (octal)
    s5fs chown image uid[:gid] path...     # set owner (numeric; :gid optional)
    s5fs chgrp image gid path...           # set group (numeric)

Paths are inside the image; `cp` additionally reaches the **host** filesystem
when a path is prefixed with `@`, so one command copies in either direction (or
both, or neither):

    s5fs cp img @./hello.c /src/hello.c    # host  -> image  (like put)
    s5fs cp img /etc/passwd @./passwd      # image -> host   (like get)
    s5fs cp img /a /b                      # image -> image
    s5fs cp img @a @b                      # host  -> host

(`@` is Windows-safe: real host paths -- including `C:\...` -- never start with
it.  When the destination is a directory, the source basename is appended.)

`put`/`get` preserve the file's permission bits, and `put` keeps the host
file's mtime.  Example:

    s5fs mkfs -d rl02 disk.dsk
    s5fs put  disk.dsk ./hello.c /src/hello.c
    s5fs ls -l disk.dsk /src
    s5fs get  disk.dsk /src/hello.c -      # to stdout

### Interactive explorer

    s5fs shell [-r] [-B 512|1024|2048] [-A pdp11|le|be] image

A small REPL with a current directory inside the image (read-write unless `-r`):

    $ s5fs shell disk.dsk
    s5fs shell: disk.dsk (pdp11, 1024-byte blocks, read-write)
    /> mkdir src
    /> cd src
    /src> put ../hello.c
    /src> ls -l
    -rw-r--r--  1    0    0      812 2026-07-08 14:40 hello.c
    /src> cat hello.c
    ...
    /src> quit

Commands: `ls [-l] [-a]`, `cd`, `pwd`, `cat`, `get`, `put`, `cp`, `mv`, `rm`,
`mkdir`, `rmdir`, `chmod`, `chown`, `chgrp`, `stat`, `help`, `quit`/`exit`.

## Mounting (FUSE)

    s5fs mount [-B 512|1024|2048] [-A pdp11|le|be] [-w] [-f] image mountpoint
    s5fs umount mountpoint

Mounts an image with FUSE so you can browse and copy files with normal tools.
Read-only by default; `-w` enables read-write: `create`, `mkdir`, `write`,
`truncate`, `unlink`, `rmdir`, `rename` (incl. cross-directory dir moves, with
`..`/link-count fixup), `chmod`, `chown`, `utimens`.  `-f` runs in the
foreground.  It is a thin front-end over the same `s5fs_rw` engine the file
commands use, so its writes allocate in place over the full direct +
single/double/triple-indirect map (files of any size), and after edits
`s5fs fsck` confirms the image is still consistent.

## Layout

    src/pdp11fs.h    on-disk s5fs format for an LP64 host: constants + explicit
                     PDP-11 encoders (16-bit LE, 32-bit middle-endian long,
                     3-byte l3 address).  No host struct overlays disk bytes.
    src/s5fs_core.[ch] the writer core, ported line-for-line from cmd/mkfs.c 2.5:
                     superblock, chained free list w/ rotational interleave,
                     allocator, inode/directory/single-indirect writers.
    src/device.[ch]  PDP-11 disk geometry (SIMH + BSD cross-checked) + an
                     optional, advisory-only driver-availability note.
    src/s5endian.[ch] pluggable byte order (pdp11 / le / be) -- the only thing
                     that differs across s5fs hosts.
    src/cmd_mkfs.c   `mkfs`   subcommand
    src/cmd_mktree.c `mktree` subcommand -- host directory -> populated image
    src/cmd_tar.c    `tar`    subcommand -- archive <-> image (c/x)
    src/cmd_restore.c `restore` subcommand -- dump tape -> image
    src/cmd_dump.c   `dump`   subcommand -- image -> dump tape (flat or SIMH .tap)
    src/cmd_vhd.c    `vhd`    subcommand -- fixed-VHD footer wrap/unwrap/info
    src/fsread.[ch]  read-only reader (inode/bmap/readdir/readfile) used by the
                     export paths (`tar c`, `dump`), the file commands, and mount.
    src/s5fs_rw.[ch] the shared read/write engine: path lookup, directory-slot
                     add/remove, inode alloc/free, in-place block map (full
                     1/2/3 indirect), and the mkdir/rm/rename/put/... ops.  Used
                     by cmd_fs.c, cmd_shell.c, AND the FUSE mount -- one copy.
    src/cmd_fs.c     `ls`/`cat`/`get`/`put`/`cp`/`mv`/`rm`/`mkdir`/`rmdir`/`chmod`
    src/cmd_shell.c  `shell` -- the interactive explorer REPL (paths)
    src/cmd_fsdb.c   `fsdb`  -- the interactive debugger (raw inodes/blocks)
    src/cmd_manifest.c `manifest`/`verify` -- mtree-style fingerprint + diff
    src/fsutil.h     small presentation helpers (mode string, path resolve)
    src/cmd_fsck.c   `fsck`   subcommand -- an independent reader/checker.
    src/cmd_mount.c  `mount`/`umount` -- a thin FUSE front-end over s5fs_rw
                     (`make FUSE=1`; cmd_util.c has the no-FUSE stubs).
    src/cmd_util.c   `devices`, `boot`, and `mount`/`umount` stubs.
    src/s5fs.c       the git-style dispatcher (the only main()).

## Build

    make            # -> bin/s5fs   (cc -std=c99 -Wall -Wextra -pedantic)
    make FUSE=1     # also builds the `mount`/`umount` subcommands (libfuse3)
    make test       # self-contained regression suite (tests/run.sh)

`make test` builds its own fixtures (no external 2.9 data) and checks the whole
surface -- put/get, cp/mv/rm/mkdir, tar and dump round-trips, `.tap` framing,
partition isolation on a whole-disk RP06, VHD wrap/unwrap, byte order, and the
shell -- asserting `fsck` is clean after every mutating flow.

## mkfs

    s5fs mkfs [-B 512|1024|2048] [-a pdp11|le|be] [-F v7|sysv]
              [-d device | -b blocks | -s sectors]
              [-r release] [-m m] [-n n] [-t mtime] [-i ninode] image

`-B` is the filesystem block size (default 1024, the UCB_NKB config; 512 is the
non-UCB / V7 config; 2048 is the System V `Fs4b` size for non-PDP targets).
`-F` picks the superblock flavor: `v7` (default) or `sysv` (see Compatibility).
Size comes from a known device (`-d`, see `s5fs
devices`), filesystem blocks (`-b`), or 512-byte SIMH sectors (`-s`).  `-m`/`-n`
are the free-list interleave (default 5/10, as mkfs), `-t` pins the timestamp
for a reproducible image, `-i` forces a minimum inode count.

    s5fs mkfs -d rl02 disk.dsk    # an RL02 (10 MB) 1K-block filesystem
    s5fs mkfs -s 20480 disk.dsk   # the same, by explicit sector count

**The image is kernel-agnostic.**  A device only fixes the size; geometry is
identical across releases.  `-r` changes nothing in the image -- it only turns
on a gentle note if the chosen disk's controller wasn't usually in that
release's kernels (e.g. `-d ra81 -r 2.9`).  With no `-r`, nothing is warned; the
note never blocks, since a kernel may carry a backported driver.

## mktree

    s5fs mktree [-B 512|1024|2048] [-a pdp11|le|be]
                [-d device | -b blocks | -s sectors] [-t mtime] rootdir image

Builds a *populated* image: creates a fresh filesystem sized for the device,
then copies the host directory `rootdir` into it -- directories become s5fs
directories, regular files are stored with the full direct/single/double/triple
-indirect map, all owned by root with the host permission bits.  This is how a
built root tree becomes a `.dsk`.

    s5fs mktree -d rp06 ./root disk.dsk     # lay ./root onto an RP06 image
    s5fs fsck -l disk.dsk                    # verify + list what landed

`-D` builds `/dev` from a spec (`name c|b major minor [mode]`, see
`examples/dev.spec`); a `lost+found` is always created at the root.  Note the
major numbers must match the target kernel's `cdevsw`/`bdevsw`.  Not yet
handled (and reported when skipped): symlinks (not an s5fs feature) and
hard-link coalescing (each link currently becomes its own inode).

## tar

    s5fs tar x [-B 512|1024|2048] [-a pdp11|le|be]
               [-d device | -b blocks | -s sectors] [-t mtime] archive image   (extract)
    s5fs tar c [-B 512|1024|2048] [-A pdp11|le|be] image archive                     (create)

`tar x` builds an image from a tar archive (ustar/GNU); `tar c` is the reverse,
writing a tar archive from an image.  Metadata (mode incl. setuid/setgid/sticky,
uid, gid, mtime) round-trips through the archive, so a faithful root tarball
reproduces its ownership and dates; hard links (`1`) share one inode, device
nodes (`3`/`4`) carry the major/minor.  Symlinks and fifos have no s5fs
equivalent and are skipped (reported).  On extract, compressed archives are
auto-detected by magic and decompressed via `gzip`/`bzip2` (runtime tools, not
build deps).  The output of `tar c` is read by standard `tar`, and `tar c |
tar x` round-trips an image to a byte-identical tree.

    s5fs tar x -d rp06 root.tar.gz  disk.dsk     # archive -> image
    s5fs tar c disk.dsk root.tar                 # image   -> archive

On extract, entries may arrive in any order: `tar x` (and `restore`) build an
in-memory tree (`tree.c`) that materializes parent directories on demand, then
serialize it in one pass -- assign inodes, compute link counts, write.

## restore / dump

    s5fs restore [-B 512|1024|2048] [-d device | -b blocks | -s sectors] dumpfile image
    s5fs dump    [-B 512|1024|2048] [-A pdp11|le|be] [-T] image dumpfile

`restore` reads a 2.9BSD `dump(8)` tape into an image (the job of `restor(8)`);
`dump` is the reverse, writing a tape from an image (the job of `dump(8)`).  A
*faithful* pair: they keep the original inode numbers and metadata (mode / uid /
gid / nlink / atime / mtime / ctime, device numbers) and copy directory data
verbatim, so the namespace, link counts, timestamps, and file contents are
reproduced exactly.  `dump` emits the full record set -- `TS_TAPE`, the
`TS_CLRI` / `TS_BITS` inode maps, one `TS_INODE` (+ `TS_ADDR` continuations for
indirect blocks) per inode, and `TS_END` -- with correct per-record checksums.

    s5fs restore -d rl02 rootdump root.dsk        # tape  -> image
    s5fs dump    root.dsk root.dump               # image -> tape
    s5fs fsck -l root.dsk

Both directions interoperate with the real 2.9BSD tools: a tape from `s5fs dump`
is read by native `restor(8)` (`restor tf` validates it; `restor rf` rebuilds a
filesystem from it that our `fsck` then verifies), and native `dump(8)` tapes
restore through `s5fs restore`.

The dump format (MAGIC 60011, `struct spcl` records, s5fs inode) is stable
across V7 -> 4.1 (incl. 2.8/2.9/2.10); it reads/writes a PDP-11 dump with
`-B`-sized records.  4.2BSD introduced a new, incompatible format (MAGIC 60012,
FFS inode, fixed 1024-byte records) alongside FFS -- out of scope, like FFS.

`dump -T` writes the tape in **SIMH `.tap` format** (each `NTREC`-block tape
record wrapped with a 4-byte little-endian length before and after, ended by
two tape marks) instead of a flat record stream.  That's the container a SIMH
`TM11`/`TS11` drive expects, so you can `attach ts0 root.tap` inside a running
2.9BSD and restore from it with the machine's own `restor(8)` -- the way a real
tape restore was done.  (Without `-T` the output is the flat stream that
`s5fs restore` and `apout`-hosted `restor` read.)

## Container formats

Images are **raw** by default -- a flat 512-byte-sector dump, which is SIMH's
native disk format and what every other tool (`dd`, QEMU, ...) understands.  Two
optional containers are supported:

- **SIMH `.tap`** for tapes, via `dump -T` (above) -- record framing for a
  simulated tape drive.
- **fixed VHD** for disks, via `s5fs vhd`:

      s5fs vhd wrap   disk.dsk [disk.vhd]   # append the 512-byte VHD footer
      s5fs vhd unwrap disk.vhd [disk.dsk]   # strip it back to raw
      s5fs vhd info   disk.vhd              # show type/size/geometry/creator

  A fixed VHD is just the raw image plus a 512-byte footer (cookie `conectix`,
  CHS geometry, size, `type=fixed`, checksum) that SIMH / QEMU / Hyper-V
  auto-detect.  Because the footer sits *after* the filesystem, every other
  `s5fs` command reads -- and even edits -- a wrapped image transparently, so
  you only wrap once (for hand-off) and unwrap if a tool wants a bare image.
  `wrap`/`unwrap` are byte-exact inverses; with no second argument they act in
  place.  Dynamic/differencing VHD (sparse, needs a block-allocation table) is
  out of scope -- raw covers space just fine for these small disks.

## boot / booting in SIMH

    s5fs boot image bootfile

Installs a primary bootstrap into block 0 (block 0 isn't part of the
filesystem).  `bootfile` is a raw boot block (the 2.9BSD `mdec/*uboot` files) or
a `.o` (the 16-byte a.out header is stripped, as mdec's `dd bs=8w skip=1` does).

A restored root boots for real.  The full pipeline, verified end to end:

    s5fs restore -d rl02 rootdump root.dsk        # dump -> s5fs image
    s5fs boot root.dsk .../sys/mdec/rluboot        # install the RL bootstrap
    # SIMH:  set cpu 11/70 2M / set rl0 RL02 / attach rl0 root.dsk / boot rl0
    #        at the ": " prompt type  rl(0,0)rlunix

boots `rluboot` (block 0) -> `/boot` -> the `rlunix` kernel -> the 2.9BSD
single-user shell (`Berkeley UNIX (Rev. 2.9.2)`), which runs commands normally.

## Device spec file

The built-in device table (`s5fs devices`) can be extended without recompiling.
`s5fs` reads an INI file from `$S5FS_DEVICES`, else `~/.config/s5fs/devices`;
entries add new disks or override built-ins by name.  Capacities are 512-byte
blocks; `since`/`until` are optional (default `v7`..`2.10`, i.e. never warn):

    # ~/.config/s5fs/devices
    [eagle]
    blocks = 1002000
    desc   = Fujitsu M2351 Eagle (SMD)

    [rl02]                  ; override the built-in
    blocks = 20000
    desc   = RL02, last cyl reserved for bad144

    [eagle]                 ; ..with a site partition layout (see below)
    blocks     = 1002000
    partitions = a:0:20000 b:20000:20000 c:40000:962000 h:0:1002000

Unknown keys are ignored, so the format has room to grow.  See
`examples/devices.ini`.

## Partitions and whole-disk images

Most images are a single filesystem: block 0 is the boot block, block 1 the
superblock, then inodes and data.  The small removable drives (RK05, RL01/RL02)
are always used this way -- their driver has no partition table, so the whole
disk *is* one filesystem, and that's what a plain image is.

Bigger drives are sliced into partitions `a..h`.  Crucially, in this era **the
layout is compiled into the kernel driver, not written on the disk** -- there is
no label.  The partition you touch is selected by the device node's minor
number (`minor & 7`), which the driver maps to a `{start, length}` window via
its `*_sizes[]` table.  A whole-disk image therefore just places each
filesystem at the offset the kernel expects, with the boot block at absolute 0:

    disk:  [ boot | root fs (a) .......| swap (b) | /usr fs (c) ................ ]
    block:  0      1                     ^b start   ^c start

`s5fs` reproduces the driver's tables (from `sys/GENERIC/ioconf.c`) for the
standard drives, and any command that opens an image takes a partition selector:

    -d <device> -P <letter>     the named partition of that drive (needs -d)
    -o <START[:LEN]>            a raw window, START/LEN in 512-byte blocks

Under the hood this is a single base-block offset added in the open path -- the
same arithmetic the kernel driver does -- so every command operates *inside* the
chosen partition.  See a drive's map with `s5fs devices <name>`:

    $ s5fs devices rp06
    rp06 -- RP06 pack (Massbus)
    partitions (512-byte blocks):
      part      start     length       size
       a            0       9614      4.7 MB
       b         9614       8778      4.3 MB   (swap)
       c        18392     153406     74.9 MB   (/usr)
       h            0     340670    166.3 MB   (whole disk)

Build a bootable multi-partition disk by laying each filesystem into its window
(the create commands grow the whole-disk file and never disturb other
partitions), then installing the boot block:

    s5fs restore -d rp06 -P a rootdump   disk.dsk   # root  -> partition a
    s5fs mktree  -d rp06 -P c ./usr-tree disk.dsk   # /usr  -> partition c
    s5fs boot    disk.dsk mdec/rpuboot              # bootstrap at block 0
    # inspect / edit / check any partition in place:
    s5fs ls   -d rp06 -P a disk.dsk /
    s5fs fsck -d rp06 -P c disk.dsk
    s5fs mount -d rp06 -P a disk.dsk /mnt           # (FUSE build)

Because the offsets must match the kernel, use the standard table for a stock
kernel; for a hand-edited kernel, give `s5fs` your custom layout via the
`partitions =` INI key (above) -- the disk conforms to the kernel, never the
reverse.

## fsck

    s5fs fsck [-B 512|1024|2048] [-A pdp11|le|be] [-l] [-p] image

Decodes the superblock, walks every allocated inode's block tree (direct +
single/double/triple indirect), traverses the chained free list, confirms the
volume partitions exactly once into reserved / used / free (what `icheck`
does), and tallies directory references against each inode's `di_nlink` (what
`dcheck` does).  On top of those it runs the classic higher fsck phases:

- **inode sanity** -- inodes with an invalid file type; directory entries that
  point at an unallocated inode (dangling) or an out-of-range inode; a "."
  entry that doesn't reference its own directory;
- **connectivity** -- a DFS from root marks every reachable inode; anything
  allocated but *un*reachable is an orphan (found by reachability, not link
  count, since an orphan directory's own "." keeps its count nonzero).

`-p` repairs: fix link counts, salvage the free list, **zero dangling entries**,
and **reconnect orphans into `/lost+found`** (an orphan directory also gets its
".." repointed), then recompute link counts from the repaired tree.  `-l` lists
the directory tree (ncheck-style).  Byte order is auto-detected (`-A` overrides).
The checker shares no logic with the writer, so it is a real cross-check of
`s5fs mkfs`/`mktree`, and reads any s5fs image in the family below.

### fsdb -- interactive debugger

    s5fs fsdb [-w] [-B ..] [-A ..] [-d dev -P part | -o blk] image

Where the shell works in the namespace (paths), `fsdb` works at the raw
inode/block level -- the forensic / repair / learn-the-format tool:

    fsdb> sb                 superblock summary
    fsdb> inode 2            decode an inode (type, mode, times, block pointers)
    fsdb> dir 2              raw directory entries (slot / inode / name)
    fsdb> map 91             an inode's logical->physical block map
    fsdb> block 27           hexdump a filesystem block
    fsdb> cat 91 ; path /bin/sh ; links 21
    fsdb> set 91 uid 0       (-w) patch mode|nlink|uid|gid|size
    fsdb> poke 27 0 2e       (-w) write raw hex bytes at block/offset

Read-only unless `-w`.  It reuses the same reader and writer core as the rest of
the tools, so what you see and change is exactly what they see.

## Manifest / verify

    s5fs manifest [-B ..] [-A ..] [-d dev -P part | -o blk] image
    s5fs verify   [-B ..] [-A ..] [-d dev -P part | -o blk] image manifest

`manifest` prints an mtree-style fingerprint -- one line per path with its type,
permission bits, uid, gid, size, mtime, and a CRC32 of the file's contents (a
device node records its major.minor instead):

    f 755 0 2 105246 482562190 97ecb203 /unix
    d 775 3 3 1632 438221056 - /bin
    c 666 0 0 0 417829221 17.0 /dev/tty

`verify` walks a second image and reports what differs -- `!` changed, `+` extra
(in image, not manifest), `-` missing -- and exits nonzero if anything did.  It
compares type / mode / owner for everything, plus **size + content checksum for
files** and **major.minor for devices**; it deliberately ignores mtime and does
not checksum directories (whose on-disk size and entry order legitimately vary),
so a content-faithful transformation passes clean:

    s5fs manifest reference.dsk > ref.mtree
    s5fs restore -d rl02 rootdump rebuilt.dsk   # (or any transform)
    s5fs verify rebuilt.dsk ref.mtree           # exit 0 == every file identical

This is the tool for regression-checking image transforms and for proving a
rebuilt world byte-matches a reference: a `dump | restore` round-trip of the
real 2.9 root verifies clean, while a single changed mode, byte, added, or
removed file is reported.

## Byte order

s5fs has the same *structure* on every host; only multi-byte fields encode
differently, so the codec is pluggable (`src/s5endian.c`) across three regimes:

    pdp11   16-bit LE, 32-bit middle-endian (word-swapped), PDP l3 3-byte
            -- V7 / 2.xBSD on the PDP-11                        (the default)
    le      16-bit/32-bit little-endian, LE 3-byte
            -- VAX (3BSD/4.0/4.1) and any little-endian SysV host
    be      16-bit/32-bit big-endian, BE 3-byte
            -- big-endian SysV hosts (m68k, ...)

`mkfs -a <arch>` picks the order; `fsck` auto-detects it from the superblock
(override with `-A`).  Aliases are accepted for the canonical `le`/`be`:
`vax`, `x86`, `i386`, `little` -> `le`; `m68k`, `68k`, `sun`, `big` -> `be`.
Adding a new architecture is a device-table entry, never a format change.

## Compatibility

The writer/reader cover the whole standalone s5fs family:

- **V7** (PDP-11): `-B 512 -a pdp11` (NADDR 13).
- **2.8 / 2.9 / 2.10** (PDP-11): default `-B 1024 -a pdp11` (NADDR 7); non-UCB
  builds use `-B 512`.
- **3BSD / 4.0 / 4.1** (VAX): `-B 512 -a le` (NADDR 13) -- same s5fs structure,
  little-endian.
- **System V** (VAX / 3B / x86 / 68k): `-B 512|1024|2048 -a le`|`be` (NADDR 13);
  2048 (`Fs4b`) is the largest s5fs block size, raising the ceiling to 32 GiB.
  Add `-F sysv` so `mkfs` also stamps the System V superblock magic
  (`0xfd187e20`) + block-size type at the tail, so a SysV kernel auto-detects
  the volume and its block size; `s5fs` reads its own SysV images back (the
  totals move to the SysV offsets).  This is written to the documented
  `struct filsys` layout but is **not verified against a real System V kernel**
  (none is available here); the default flavor is plain V7/2.9.

Out of scope: **V6** (an older, incompatible filesystem -- 2-byte block
addresses) and **2.11BSD** (its filesystem diverged) -- and 4.2BSD onward,
which moved to FFS.

## Limits

The traditional filesystem packs each block address into an inode as **3 bytes**
(the PDP-11 `l3` format), so a filesystem holds at most **2^24 = 16,777,216
blocks**.  `mkfs` (and the other create paths) refuse a larger size rather than
silently truncating.  With the three supported block sizes that ceiling is:

| block size | max filesystem | who uses it |
|---|---|---|
| **512**  |  8 GiB | V7 |
| **1024** | 16 GiB | 2.x BSD (UCB) |
| **2048** | 32 GiB | System V (`Fs4b`) |

2048 is the family maximum -- the s5fs superblock's block-size code only defines
up to 2048; anything larger is FFS, a different filesystem.  Secondary limits:
at most ~1M inodes (the i-list start `s_isize` is a 16-bit field, so <= 65533
i-list blocks), and any single file is capped at **2 GiB** (`di_size` is a
signed 32-bit `off_t`).

Note there is no on-disk block-size marker in this (V7-derived) superblock, so
a reader must be told the size (`-B`), exactly as the era's compile-time `BSIZE`
worked; 512/1024 are the PDP-11 world, 2048 is there for SysV-era targets on
other CPUs (paired with `-a le`/`-a be`).

## Validation

- **Byte decode** -- superblock, reserved inodes, and root directory decode to
  expected values.
- **`s5fs fsck`** -- block accounting comes back clean on `mkfs` output and
  flags injected corruption (out-of-range/missing/double-allocated blocks).
- **dump round-trip** -- `restore rootdump | dump | restore` reproduces a
  byte-identical file tree (every inode, all data, mode/uid/gid, and the real
  1980s timestamps), fsck-clean.
- **native-tool interop** -- a tape from `s5fs dump` is accepted by native
  2.9BSD `restor(8)` under `apout`: `restor tf` validates it, and `restor rf`
  rebuilds a filesystem whose tree is byte-identical to the reference (proving
  our `TS_CLRI`/`TS_BITS` maps and per-record checksums, which our own
  `restore` skips).
- **SIMH boot** -- a restored + boot-installed root boots to a shell.
- **partitions** -- a whole-disk RP06 image built with `mkfs -P a` + `mkfs -P c`
  (or `restore -P`/`mktree -P`) keeps each partition isolated (a file in `a` is
  absent from `c`), each `fsck`s clean independently, and `-P a` mounts/reads the
  real 2.9 root; INI-defined tables (`s5fs devices eagle`) work too.

Still planned: a self-hosted `icheck`/`fsck` built with our own cross-toolchain
and run under `apsim`, for a fully third-party oracle.

## Roadmap

- optional: preserve host directory/device-node times in `mktree` (regular
  files already do)
- optional: a one-shot `cp` between two disk images (host `@` syntax already in
  place; multi-image addressing is the remaining piece)

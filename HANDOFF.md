# s5fs — engineering handoff

You are picking up an essentially **feature-complete** host toolkit for creating,
inspecting, editing, repairing, and converting the traditional PDP-11 Unix
filesystem — retroactively called **s5fs** (the System-V / V7 filesystem) — as
SIMH-style disk images. This document is the orientation an incoming engineer
(human or agent) needs before touching the code. `README.md` is the user manual;
this is the *builder's* manual: architecture, invariants, how correctness was
established, what's deliberately out of scope, and where the bodies are buried.

Read this top-to-bottom once, then keep §5 (invariants) and §9 (gotchas) nearby.

**`docs/` has the depth this document only summarizes** — one file per
subsystem, each ending in a maintainer checklist. Start with
[`docs/design.md`](docs/design.md); the format itself is
[`docs/on-disk-format.md`](docs/on-disk-format.md), the free list and the limits
of recovery are [`docs/allocator.md`](docs/allocator.md), and the evidence
behind §8 is [`docs/validation.md`](docs/validation.md). The full index is in
`README.md`.

---

## 1. What this is, in one breath

A single dependency-free C99 binary, `s5fs`, dispatched git-style
(`s5fs <command> …`). One faithful port of 2.9BSD's `mkfs` filesystem writer sits
at the bottom; every front-end (mkfs, tar, dump/restore, tree-from-host, the
file commands, the FUSE mount) builds on that one allocator and inode/directory
code, so **there is exactly one mutation code path** and it produces images the
era's own `fsck` and kernel accept byte-for-byte.

It is **release-agnostic**: the same writer covers V7, 2.8, 2.9, and 2.10,
because they share the s5fs on-disk format byte-for-byte. It is also
**host-agnostic**: all multi-byte on-disk fields go through a pluggable
byte-order codec (PDP-11 middle-endian, little-endian VAX/x86, big-endian
m68k), so the tool runs and the images are portable across any modern host.

---

## 2. How this repo came to be (and its sibling)

This repo was **split out of a larger monorepo**, `~/pdp11-bsd29-toolchain`
(a full 2.9BSD cross-toolchain: as, cc/c0/c1, ld, ar, nm, …). The filesystem
work lived there under `diskimage/` and was extracted with
`git subtree split --prefix=diskimage`, which reparented `diskimage/src → src`
etc. and dropped every commit that didn't touch `diskimage/`. The result was
pulled into this fresh repo. **History is preserved** — the 11 commits here are
the real ones, authorship and dates intact.

Consequences you should know:

- **Commit messages still carry the `diskimage:` prefix.** That's the honest
  history from when this was a subdirectory. Don't be confused by it; there is
  no `diskimage/` directory here. Reword only if the owner asks (it rewrites
  hashes).
- **The monorepo is untouched and still has this code** on its
  `diskimage-toolkit` branch. If you need to sync a fix back, that's the place.
- **The validation oracle lives in the monorepo, not here** (see §8). This repo
  is source-only and builds/tests standalone; the *historical* 2.9 data,
  native tool binaries, and the apout/apsim emulators used to prove byte
  fidelity are all in the monorepo (and some of that data is copyright-encumbered
  and was never committable — see §8).

---

## 3. Build, test, run

Dependency-free by default — just a C99 compiler and make:

```sh
make            # builds bin/s5fs  (CFLAGS: -std=c99 -O2 -Wall -Wextra -pedantic)
make test       # builds, then runs tests/run.sh  (self-contained, ~27 checks)
make clean
make FUSE=1     # additionally compiles the `mount`/`umount` subcommands
                # (needs libfuse3-dev + pkg-config; everything else unchanged)
```

`bin/` is gitignored. The default build has **zero external dependencies**;
FUSE is the only optional extra and only affects two subcommands.

The test suite (`tests/run.sh`) builds all its own fixtures — it needs no
external 2.9 data — and every mutating flow ends by asserting `s5fs fsck` prints
`clean`. It covers both layers and their seam: file ops, partitions, VHD, `.tap`
framing, all three byte orders, 2048-byte/SysV superblocks, the oversize guard,
fsdb, manifest/verify, scavenge, the analysis bundle, and the multi-mount shell.
**Keep it green; add a check for every new behavior.**

---

## 4. Architecture

### The layer cake (bottom to top)

```
  s5endian.[ch]     pluggable byte-order codecs (put/get 16/24/32)   ← no fs knowledge
  pdp11fs.h         on-disk STRUCTURE: field offsets, sizes, constants ← no byte order
        │
  fsread.[ch]       READ side: decode superblock (auto-detect BO), iget,
                    namei, readdir, bmap (direct+1/2/3 indirect), readfile
  s5fs_core.[ch]    WRITE side: the ported mkfs — superblock, chained free
                    list w/ rotational interleave, inode/dir/indirect writers,
                    block alloc/free, s5fs_mount() to load an existing image
        │
  s5fs_rw.[ch]      THE MUTATION ENGINE: one RW handle = {FSR read + S5FS write},
                    two page-cache-coherent fds. mkdir/rmdir/unlink/rename/creat/
                    chmod/chown/utimes/pwrite/truncate/put_fd/copy. All CLI file
                    ops, the shell, and FUSE call THESE — nothing reimplements
                    directory or block logic.
  tree.[ch]         in-memory fs tree → tree_serialize(): for front-ends that
                    receive entries in arbitrary order (tar, dump) and can't
                    build in one forward pass
        │
  cmd_*.c           the subcommands (front-ends). Thin: parse args, drive the
                    engine. s5fs.c is the ONLY main() and just routes.
```

Golden rule: **read goes through `fsread`, write/allocate goes through
`s5fs_core`, and everything user-facing that mutates goes through `s5fs_rw`.**
If you find yourself hand-rolling directory-entry or block-map math in a
`cmd_*.c`, stop — the primitive you want already exists a layer down.

### File-by-file map

| File | Role |
|---|---|
| `s5fs.c` | dispatcher + `main()`; the subcommand table & usage |
| `cmds.h` | every `cmd_*` entry-point prototype |
| `pdp11fs.h` | on-disk structural constants & field offsets (byte-order-free) |
| `s5endian.[ch]` | the three codecs (`S5_PDP11`/`S5_LE`/`S5_BE`) + name parsing |
| `fsread.[ch]` | read-only reader (FSR) |
| `s5fs_core.[ch]` | writer core (S5FS); the ported `mkfs` |
| `s5fs_rw.[ch]` | shared read+write mutation engine (RW) |
| `tree.[ch]` | arbitrary-order → serialized image |
| `device.[ch]` | PDP-11 disk geometry + compiled-in partition tables + advisory driver check |
| `fsutil.h` | presentation/`cp` helpers (implemented in `cmd_fs.c`) |
| `cmd_mkfs.c` | `mkfs` |
| `cmd_mktree.c` | `mktree` (live host directory walk → image) |
| `cmd_tar.c` | `tar c` (image→tar) / `tar x` (tar→image, handles .gz/.bz2/.Z) |
| `cmd_restore.c` | `restore` (2.9 dump tape → image) |
| `cmd_dump.c` | `dump` (image → 2.9 dump tape; `-T` for SIMH `.tap` framing) |
| `cmd_fsck.c` | `fsck` (phases 1–3, `-p` repair, `-l` list) **plus** `icheck`, `dcheck`, `clri` |
| `cmd_fsdb.c` | `fsdb` interactive debugger (`-w` to edit) |
| `cmd_manifest.c` | `manifest` + `verify` (mtree-style fingerprint/diff) |
| `cmd_scavenge.c` | `scavenge` (deleted-name recovery + signature carving) |
| `cmd_analysis.c` | `ncheck`, `quot`, `du`, `df`, `labelit` |
| `cmd_util.c` | `boot` (install block-0 bootstrap) + `devices` |
| `cmd_vhd.c` | `vhd` fixed-VHD wrap/unwrap/info |
| `cmd_fs.c` | the file commands: `ls cat get put cp mv rm mkdir rmdir chmod chown chgrp` + `fs_copy`/helpers |
| `cmd_shell.c` | `shell` — interactive **multi-mount VFS** (see §6) |
| `cmd_mount.c` | `mount`/`umount` FUSE wrapper (only compiled with `FUSE=1`) |

---

## 5. Design invariants — do not break these

1. **Never overlay a host struct on disk bytes.** Every multi-byte field is
   read/written through an `s5_codec` (`get16/put16/get24/put24/get32/put32`).
   This is what makes the tool byte-order-portable and 64-bit-clean. Adding a
   field means adding an offset in `pdp11fs.h` and codec calls — not a `struct`.
2. **One mutation path.** New file-manipulation features extend `s5fs_rw.c`, and
   the CLI/shell/FUSE all call it. Don't fork the logic.
3. **Consistency after every op.** High-level `rw_*` ops flush the superblock
   (`rw_sync`) before returning, so the image on disk is valid after each call.
   Preserve that; the test suite asserts `fsck clean` after mutations.
4. **Block addresses are 24-bit (3-byte l3-packed).** Hence the hard ceiling of
   `P11_MAXFSBLKS = 2^24` blocks, enforced in `s5fs_begin`. Don't remove the guard.
5. **Block size ∈ {512, 1024, 2048} only** (`P11_BSIZE_OK`). 2048 is the s5fs
   family maximum (SysV Fs4b); larger blocks are FFS, a *different* filesystem.
   NADDR is **13** for 512/2048 and **7** for 1024 (the UCB_NKB profile). Default
   bsize is **1024**.
6. **Times are preserved, not stamped.** Inodes carry atime/mtime/ctime; a value
   of 0 means "stamp with the superblock time (now)", any nonzero value is
   written through verbatim. Restore/tar-x/mktree must keep 1980s dates intact —
   there is a regression here (see §9); don't reintroduce "stamp everything now".
7. **Partitions are a base-block offset, full stop.** There is no on-disk label.
   A partition is `minor(dev)&7` indexing the kernel driver's compiled-in
   `*_sizes[]` table, which we mirror in `device.c`. `base` (a byte offset)
   threads through `fsread`, `s5fs_core`, and `s5fs_rw`. Whole-disk operations on
   a partition must be **grow-only** for the file (never `ftruncate` a whole-disk
   image down to one partition's size — that bug was fixed once already).
8. **`-pedantic` clean, C99, no GCC-isms.** No `typeof`, no VLAs-as-struct-members,
   include `<unistd.h>` where you use `getopt`, match `%lo`/`%ld` to the actual
   type. The build must stay warning-free under `-Wall -Wextra -pedantic`.

---

## 6. Two subsystems worth understanding before you touch them

**The multi-mount shell (`cmd_shell.c`).** The interactive explorer is a small
VFS, not a single-image browser. You `mount <img> <at>` any number of images at
arbitrary virtual paths; `route(abspath)` picks the **longest-prefix** mount, so
overlapping mounts shadow correctly (deeper wins). `cp`/`mv` resolve each side
independently, so you can copy **between different images** in one command, and a
leading `@` on a path escapes to the **host** filesystem. The launch image
auto-mounts at `/`. Virtual (unmounted) intermediate directories are navigable
with `cd`. If you extend it, respect `route()`/`parentof()`/`rpath()` — don't
special-case single-mount.

**dump/restore + containers (`cmd_dump.c`, `cmd_vhd.c`).** `dump` emits the real
2.9BSD `dump(8)`/`restor` tape format (MAGIC 60011, `spcl` records, checksum =
`CHECKSUM(84446) - sum16`), byte-order-aware. `-T` wraps it in SIMH `.tap` framing
(each `NTREC`-block record bracketed by a 4-byte LE length, two tape marks at
end). Round-trips are validated against the **native** `restor(8)` running under
the emulator. `vhd` appends a 512-byte fixed-VHD footer (cookie `conectix`, CHS
per the spec algorithm, ones-complement checksum); **dynamic VHD is out of scope.**
When wrapping, always `lseek(SEEK_END)` before writing the footer (append, don't
overwrite — that bug was fixed once).

---

## 7. On-disk format cheat-sheet

The authoritative reference is `pdp11fs.h` (heavily commented). Essentials:

- Block 0 = boot, block 1 = superblock (`struct filsys`), i-list starts block 2.
- Inode 1 = bad-block list, inode 2 = root directory.
- dinode = 64 bytes; 13 (or via profile) 3-byte block addrs at offset 12;
  atime/mtime/ctime at 52/56/60.
- Directory entry = 16 bytes: 2-byte inode number + 14-byte name.
- Free blocks are a **chained free list** (superblock caches `NICFREE=50`
  addresses; the last points to a block holding the next 50…). The list is
  **rotationally interleaved**, so files are *not* laid out contiguously — this
  is why undelete is limited (§9).
- **SysV superblock dialect** (`mkfs -F sysv`): shares the common part
  (offsets 0–417) with V7, then inserts `s_dinfo[4]`@418 (pushing tfree→426,
  tinode→430) and adds `s_state`@500, `s_magic`@504 (`0xfd187e20`),
  `s_type`@508 (1/2/3 = 512/1024/2048). All of that lands in the V7 layout's
  zeroed tail, so a SysV image still reads fine as a plain image.

---

## 8. How correctness was established (and how to re-verify)

This toolkit was held to a **byte-fidelity** bar, the same one the cross
`as`/`cc` in the monorepo meet. Don't settle for "fsck is happy" when you can
prove exactness. The evidence base:

- **Native-tool oracle (in the monorepo).** The historical 2.9BSD `mkfs`,
  `restor`, `fsck`, etc. are run under **apout/apsim** (PDP-11 user-mode
  emulators) and diffed against our output. Full dump/restore round-trips were
  proven byte-identical, and our tapes validate + rebuild under the *native*
  `restor(8)`. Elsewhere in the toolchain, cross-built binaries (e.g. rogue 3.6)
  came out **bit-for-bit identical** to the shipped originals — that's the
  standard to aim for.
- **`fsck clean` after every mutation** — enforced by the test suite.
- **Superblock/format round-trips** — SysV magic, byte order, 2048-byte blocks,
  `.tap` un-framing == flat dump, VHD footer checksum, all asserted.

**Caveat for a future agent:** the emulators, the native tool binaries, the
`mdec` boot blocks, and the original release trees (`~/bsd/2.9`, `~/rogue*`)
live in / around the monorepo. Some of that is **copyright-encumbered**: it is
usable locally for validation but was **never committed** and must not be. Treat
`~/bsd/2.9` and `~/rogue*` as **read-only originals**. This `s5fs` repo is clean,
original C — keep it that way; don't vendor copyrighted data into it.

---

## 9. Gotchas & already-fixed bugs (don't reintroduce)

- **Stale-buffer directory over-read.** `fsr_readfile` clamps to the file/dir
  size, but naive loops over all `NDIRECT` slots read leftover buffer bytes past
  the real directory size, inventing ghost entries (once produced 977 dir lines
  vs 247 real). **Always bound directory iteration by the returned byte count**
  (`e*P11_DIRENTSZ < got`), not by slot capacity. This bit both `cmd_analysis.c`
  and `cmd_scavenge.c`.
- **Whole-disk `ftruncate` shrink.** `mkfs -P` on a partition of a whole-disk
  image must not truncate the file down to the partition size. `s5fs_begin` is
  base-aware and grow-only when `base != 0`. (Caught via `strace` — double
  ftruncate.)
- **VHD footer overwrite.** See §6 — append at `SEEK_END`.
- **Orphan *directories* aren't linkcount-0.** A directory always has `nlink ≥ 1`
  from its own `.`, so fsck Phase 3 detects orphaned directories by
  **reachability DFS from root**, not by `nlink == 0`. Keep it.
- **Times stamped "now" (LIVE ISSUE / deferred).** mktree currently does *not*
  preserve host directory / device-node mtimes in all paths — the atime/mtime/
  ctime plumbing exists and works for files, but finishing full time-preservation
  in `mktree` for dirs/devnodes was explicitly deferred, not done. If you touch
  mktree, this is the first thing to finish.
- **`scavenge` is honest, not magic.** Because the free list is chained *through*
  freed blocks (~1 in 50 gets overwritten by the chain itself) and files aren't
  contiguous, real undelete is impossible. `scavenge` recovers **deleted names**
  (ghost directory entries) and does **signature carving** (a.out/ar/tar/text);
  its `build_used` follows the free-list chain and excludes chain blocks from
  "text" carving. It was renamed from `recover` precisely so nobody expects
  `undelete`. Keep the naming and the documented limits honest.

---

## 10. Scope boundaries (deliberate — don't "fix" these)

- **s5fs family only:** V7 / 2.8 / 2.9 / 2.10 (and other s5fs hosts via the
  codec). **V6 and 2.11 are out of scope.** FFS/UFS is a different filesystem.
- **Max filesystem:** 2^24 blocks → 8 GiB @512 / 16 GiB @1024 / 32 GiB @2048.
- **Max file:** 2 GiB (`di_size` is a signed 32-bit byte count).
- **Fixed VHD only** (raw + footer). Dynamic/differencing VHD is out of scope.
- **A previously-considered "rebuild the whole OS from source to a bootable
  disk" end-goal was explicitly DROPPED.** This toolkit is the finished
  deliverable in its own right; do **not** propose or resurrect the
  source→disk-rebuild pipeline as the reason this exists.

---

## 11. Conventions

- **Commits:** directly on `main`. **No `Co-Authored-By` trailers** — the history
  was rewritten once to strip them, so adding one back reintroduces what that
  rewrite removed.
- **Nothing gets pushed to any remote** without an explicit ask.
- **No external dependencies** in the default build. FUSE is the sole opt-in.
- **Style:** BSD KNF / style(9) with extra vertical breathing room, enforced by
  the repo's `.clang-format` (settings identical to vax11-xdev's). Tabs, terse
  comments that explain *why*, a block comment at the top of each file stating
  that file's contract. Keep every new subcommand thin and route it through the
  shared engine.
- **Format your own edits, not the tree.** `clang-format` cannot see that a
  construct is a table, so it collapses the columns that *are* the
  documentation. The data tables — `subcmds[]` in `s5fs.c`, `codecs[]` in
  `s5endian.c`, the disk/partition tables in `device.c`, the field offsets in
  `pdp11fs.h` — are fenced with `/* clang-format off */` and must stay fenced.
  Add a table, fence it.
- **Tests are not optional.** New behavior → new check in `tests/run.sh`, and it
  must end `fsck clean` if it mutates.

---

## 12. Suggested first moves for the incoming engineer

1. `make && make test` — confirm 27/27 green on your host.
2. Read in this order: `pdp11fs.h` → `s5endian.h` → `fsread.h` → `s5fs_core.h`
   → `s5fs_rw.h` → `s5fs.c`. That's the whole design in ~6 headers.
3. Skim `tests/run.sh` — it's the fastest tour of what the tool actually does.
4. Only then open a `cmd_*.c`. They're thin; the interesting logic is underneath.

If you're extending: new *file operation* → `s5fs_rw.c` + wire into `cmd_fs.c`
and `cmd_shell.c`; new *format/container* → new `cmd_*.c` front-end over the
existing reader/writer; new *host byte order* → a codec in `s5endian.c` (+ maybe
a `device.c` entry). You should almost never need to touch the free-list or
inode-writer internals in `s5fs_core.c`.

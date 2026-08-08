# s5fs — design

This is the design of the whole toolkit: what it is, the one idea that shapes all
of it, and how the subsystems fit together. For how to *use* it, see the
[user guide](user-guide.md). Each subsystem has its own document for the depth
this one only points at.

---

## 1. What this is

`s5fs` is a **host-side toolkit for the traditional Unix filesystem** — the V7 /
System V filesystem, retroactively called **s5fs**. On a modern LP64 host it
creates, inspects, edits, repairs, and converts s5fs volumes as SIMH-style disk
images, and it does so faithfully enough that the era's own `fsck` and kernel
accept the result byte-for-byte.

One dependency-free C99 binary, dispatched git-style (`s5fs <command> …`), with
36 subcommands over a single shared engine. The default build has **zero
external dependencies**; FUSE is the only opt-in, and it affects exactly two
subcommands.

Two properties define the whole design:

- **Release-agnostic.** V7, 2.8BSD, 2.9BSD, and 2.10BSD share the s5fs on-disk
  format byte-for-byte, so one writer covers all of them. There is no
  per-release code path anywhere in the tree.
- **Host-agnostic.** Every multi-byte on-disk field goes through a pluggable
  byte-order codec, so the tool runs — and the images it makes are correct — on
  any modern host, for any of the three CPU regimes that ran this filesystem.

---

## 2. The one idea: exactly one mutation path

The thing that keeps this toolkit honest is a rule about *code*, not about
format: **there is exactly one path that mutates an image.**

One faithful port of 2.9BSD's `mkfs` sits at the bottom and owns the allocator,
the inode writer, and the directory writer. Above it, one mutation engine
(`s5fs_rw`) owns every high-level operation — mkdir, unlink, rename, truncate,
pwrite, chmod. Above *that*, every front-end that can change an image — the
batch file commands, the interactive shell, the FUSE mount, `tar x`, `restore`,
`mktree` — calls those same primitives.

The payoff is that a front-end cannot invent a subtly different filesystem.
`tar x` and the FUSE mount lay out directories identically because they are
running the same `rw_mkdir`. A correctness fix lands once and every subcommand
inherits it. It is also why the test suite can assert `fsck clean` after every
mutating flow and have that mean something: there is only one thing to check.

The corresponding rule for a maintainer: **if you are writing directory-entry or
block-map arithmetic inside a `cmd_*.c`, stop.** The primitive already exists a
layer down.

---

## 3. The layer cake

```
  s5endian.[ch]   pluggable byte-order codecs (put/get 16/24/32)  ← no fs knowledge
  pdp11fs.h       on-disk STRUCTURE: field offsets, sizes, consts ← no byte order
        │
  fsread.[ch]     READ side: decode superblock (auto-detect byte order), iget,
                  namei, readdir, bmap (direct + 1/2/3 indirect), readfile
  s5fs_core.[ch]  WRITE side: the ported mkfs — superblock, chained free list
                  with rotational interleave, inode/dir/indirect writers, block
                  alloc/free, s5fs_mount() to load an existing image
        │
  s5fs_rw.[ch]    THE MUTATION ENGINE: one RW handle = {FSR read + S5FS write},
                  two page-cache-coherent fds
  tree.[ch]       in-memory fs tree → tree_serialize(), for front-ends that
                  receive entries in arbitrary order (tar, dump)
        │
  cmd_*.c         the subcommands. Thin: parse args, drive the engine.
                  s5fs.c is the ONLY main() and just routes.
```

The two bottom layers are deliberately split along an axis that is easy to get
wrong. `pdp11fs.h` knows **where** every field sits and **how wide** it is, and
knows nothing about byte order. `s5endian.[ch]` knows how to encode an integer
and nothing about filesystems. Neither can be corrupted by a change to the
other, and adding a field is adding an offset plus codec calls — never a
`struct`.

The read and write sides are also deliberately independent. `fsread` shares no
logic with `s5fs_core` beyond the structural constants and the codecs, which is
what makes `s5fs fsck` a genuine cross-check of `s5fs mkfs` rather than a
restatement of it. See [`fsck.md`](fsck.md).

### File map

| File | Role |
|---|---|
| `s5fs.c` | dispatcher + `main()`; the subcommand table & usage |
| `cmds.h` | every `cmd_*` entry-point prototype |
| `pdp11fs.h` | on-disk structural constants & field offsets (byte-order-free) |
| `s5endian.[ch]` | the three codecs (`S5_PDP11`/`S5_LE`/`S5_BE`) + name parsing |
| `fsread.[ch]` | read-only reader (FSR) |
| `s5fs_core.[ch]` | writer core (S5FS); the ported `mkfs` |
| `s5fs_rw.[ch]` | shared read+write mutation engine (RW) |
| `tree.[ch]` | arbitrary-order entries → serialized image |
| `device.[ch]` | disk geometry, compiled-in partition tables, advisory driver check |
| `fsutil.h` | presentation / `cp` helpers (implemented in `cmd_fs.c`) |
| `cmd_mkfs.c` | `mkfs` |
| `cmd_mktree.c` | `mktree` (live host directory walk → image) |
| `cmd_tar.c` | `tar c` / `tar x` (handles `.gz`/`.bz2`/`.Z`) |
| `cmd_restore.c` | `restore` (2.9 dump tape → image) |
| `cmd_dump.c` | `dump` (image → 2.9 dump tape; `-T` for SIMH `.tap`) |
| `cmd_fsck.c` | `fsck` phases 1–3, `-p` repair, `-l` list; plus `icheck`, `dcheck`, `clri` |
| `cmd_fsdb.c` | `fsdb` interactive debugger (`-w` to edit) |
| `cmd_manifest.c` | `manifest` + `verify` (mtree-style fingerprint/diff) |
| `cmd_scavenge.c` | `scavenge` (deleted names + signature carving) |
| `cmd_analysis.c` | `ncheck`, `quot`, `du`, `df`, `labelit` |
| `cmd_util.c` | `boot` (block-0 bootstrap) + `devices` |
| `cmd_vhd.c` | `vhd` fixed-VHD wrap/unwrap/info |
| `cmd_fs.c` | the file commands + `fs_copy`/helpers |
| `cmd_shell.c` | `shell` — interactive multi-mount VFS |
| `cmd_mount.c` | `mount`/`umount` FUSE wrapper (only with `FUSE=1`) |

---

## 4. The subsystems

Each of these has its own document.

| Subsystem | Document | What it owns |
|---|---|---|
| On-disk format | [`on-disk-format.md`](on-disk-format.md) | superblock, dinode, dirents, the SysV dialect |
| Byte order | [`byte-order.md`](byte-order.md) | the three codecs, middle-endian, `l3` |
| Allocator | [`allocator.md`](allocator.md) | chained free list, rotational interleave |
| Mutation engine | [`mutation-engine.md`](mutation-engine.md) | the RW handle, block maps, the file ops |
| Interactive shell | [`shell.md`](shell.md) | the multi-mount VFS, routing, cross-image `cp` |
| Checker | [`fsck.md`](fsck.md) | the three phases, `-p` repair |
| Containers | [`containers.md`](containers.md) | dump tapes, SIMH `.tap`, fixed VHD |
| Partitions | [`partitions.md`](partitions.md) | geometry, device tables, base offsets |
| Correctness | [`validation.md`](validation.md) | the standard and the evidence |

---

## 5. Design invariants

These are load-bearing. Breaking one does not produce a compile error; it
produces an image that looks fine and is wrong.

1. **Never overlay a host struct on disk bytes.** Every multi-byte field is
   read and written through an `s5_codec`. This is what makes the tool
   byte-order-portable and 64-bit-clean.
2. **One mutation path.** New file-manipulation features extend `s5fs_rw.c`;
   the CLI, shell, and FUSE all call it. Don't fork the logic.
3. **Consistency after every op.** High-level `rw_*` operations flush the
   superblock before returning, so the on-disk image is valid after each call.
4. **Block addresses are 24-bit.** Hence a hard ceiling of 2²⁴ blocks, enforced
   in `s5fs_begin`. The guard refuses rather than silently truncating.
5. **Block size ∈ {512, 1024, 2048} only.** 2048 is the s5fs family maximum;
   larger blocks are FFS, a different filesystem. `NADDR` is **13** for 512 and
   2048, and **7** for 1024 (the UCB_NKB profile).
6. **Times are preserved, not stamped.** A zero time means "stamp with the
   superblock's time"; any nonzero value is written through verbatim. 1980s
   dates must survive a restore, a `tar x`, and a `mktree`.
7. **Partitions are a base-block offset, full stop.** There is no on-disk
   label. Whole-disk operations on a partition must be **grow-only**.
8. **`-pedantic` clean, C99, no GCC-isms.** The build stays warning-free under
   `-Wall -Wextra -pedantic`.

---

## 6. Scope boundaries

Deliberate, and not defects to be fixed:

- **s5fs family only** — V7, 2.8, 2.9, 2.10, and other s5fs hosts via the
  codecs. **V6 and 2.11 are out of scope.** FFS/UFS is a different filesystem.
- **Max filesystem** — 2²⁴ blocks: 8 GiB @512, 16 GiB @1024, 32 GiB @2048.
- **Max file** — 2 GiB (`di_size` is a signed 32-bit byte count).
- **Fixed VHD only.** Dynamic and differencing VHD are out of scope.
- **No source→bootable-disk rebuild pipeline.** A previously considered
  "rebuild the whole OS from source" end-goal was explicitly dropped. This
  toolkit is the finished deliverable on its own terms.

---

## 7. Testing

`make test` runs `tests/run.sh` — 27 checks, self-contained (it builds all its
own fixtures and needs no external historical data) and dependency-free (`sh`
plus coreutils).

The character of the suite: **every mutating flow ends by asserting `s5fs fsck`
prints `clean`.** Beyond that it covers both layers and their seam — file
operations, partitions, VHD, `.tap` framing, all three byte orders, 2048-byte
blocks and the SysV superblock, the oversize guard, `fsdb`, `manifest`/`verify`,
`scavenge`, the analysis bundle, and the multi-mount shell.

Two round-trips carry most of the weight, because they are end-to-end and
compare bytes rather than opinions: `tar c | tar x | tar c` must produce an
identical archive, and `dump | restore | tar c` must match that same archive.

New behavior gets a check. If it mutates, the check ends `fsck clean`.

---

## 8. For a maintainer

- **The primitive already exists a layer down.** Hand-rolled directory or
  block-map math in a `cmd_*.c` is the smell that matters most here.
- **The block-map ladder lives once**, in `fsr_lbn_route()`. It was open-coded
  in four readers and three had the double-indirect range wrong, silently
  corrupting every file past ~8 MB. Call the shared one.
- **Bound directory iteration by the returned byte count**, never by slot
  capacity. `fsr_readfile` clamps to the real size, but a naive loop over all
  `NDIRECT` slots reads stale buffer bytes and invents ghost entries. This bug
  has appeared twice (`cmd_analysis.c`, `cmd_scavenge.c`).
- **A zero time means "now"; a nonzero time is verbatim.** Do not reintroduce
  "stamp everything now" — it destroys the 1980s dates that make a restored
  tree faithful.
- **Grow-only when `base != 0`.** Never `ftruncate` a whole-disk image down to
  one partition's size.
- **Orphan directories are not link-count-zero.** A directory always has
  `nlink ≥ 1` from its own `.`, so orphans are found by reachability, not by
  link count.
- **Don't vendor copyrighted data into this repo.** The validation oracle —
  emulators, native tool binaries, original release trees — lives outside it and
  is deliberately not committable. See [`validation.md`](validation.md).

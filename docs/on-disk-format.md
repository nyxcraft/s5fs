# The s5fs on-disk format

The reference for what actually sits on the platter. `src/pdp11fs.h` is the
authoritative source — this document is its narrative form, explaining what the
fields mean and which ones bite.

Everything here is **byte-order-independent**: these are offsets and widths.
How a multi-byte field is *encoded* is [`byte-order.md`](byte-order.md). The
split is deliberate and worth preserving — see [`design.md`](design.md) §3.

Constants originate from the 2.9BSD headers (`<sys/param.h>`, `<sys/ino.h>`,
`<sys/inode.h>`, `<sys/filsys.h>`, `<sys/dir.h>`, `<sys/fblk.h>`, and the
UCB_NKB config) and were confirmed identical in V7 and 4.0/4.1.

---

## 1. Volume layout

```
  block 0        boot block (never touched by the filesystem itself)
  block 1        superblock (struct filsys)
  blocks 2..     the i-list — s_isize - 2 blocks of inodes
  blocks         data
   s_isize..
   s_fsize-1
```

Two inode numbers are reserved:

| inode | meaning |
|---|---|
| **1** | the bad-block list. Legitimately unreferenced by any directory. |
| **2** | the root directory. |

Inode numbering is 1-based and there is no inode 0 — a directory entry whose
inode field is 0 is an **empty slot**, which is how deletion works and why
[`scavenge`](../src/cmd_scavenge.c) can still find deleted names.

### Block-size profiles

There is **no on-disk marker for the block size** in this V7-derived
superblock. A reader has to be told, exactly as the era's compile-time `BSIZE`
worked. Everything else derives from it:

| | 512 | 1024 | 2048 |
|---|---|---|---|
| inodes per block (`inopb`) | 8 | 16 | 32 |
| block addrs per indirect (`nindir`) | 128 | 256 | 512 |
| dir entries per block (`ndirect`) | 32 | 64 | 128 |
| **`naddr`** (di_addr slots) | **13** | **7** | **13** |
| direct slots (`laddr = naddr - 3`) | 10 | 4 | 10 |
| era | V7 | 2.xBSD (UCB) | System V `Fs4b` |

**`naddr` is 7 at 1024, not 13.** That is the UCB_NKB profile, and it is the
single most surprising number in the format: the 1024-byte 2.xBSD filesystem
has *fewer* address slots than the 512-byte V7 one, trading direct blocks for a
smaller inode footprint. Getting this wrong produces a filesystem that reads
correctly for small files and corrupts at the first indirect block.

---

## 2. The superblock (`struct filsys`, block 1)

| offset | field | width | meaning |
|---|---|---|---|
| 0 | `s_isize` | 2 | first data block = 2 + i-list blocks |
| 2 | `s_fsize` | 4 | total blocks in the volume |
| 6 | `s_nfree` | 2 | valid entries in `s_free` |
| 8 | `s_free[50]` | 200 | the free-block cache |
| 208 | `s_ninode` | 2 | valid entries in `s_inode` |
| 210 | `s_inode[100]` | 200 | the free-inode cache |
| 410 | `s_flock`,`s_ilock`,`s_fmod`,`s_ronly` | 4 | in-core flags; zero on disk |
| 414 | `s_time` | 4 | last superblock update |
| 418 | `s_tfree` | 4 | total free blocks |
| 422 | `s_tinode` | 2 | total free inodes |
| 424 | `s_dinfo[2]` | 4 | the interleave `m`, `n` |
| 428 | `s_fsmnt[12]` | 12 | mount-point name / volume label |
| 440 | `s_lasti` | 2 | inode-search hint |
| 442 | `s_nbehind` | 2 | inode-search hint; ends at 444 |

`s_isize` being a 16-bit field is where the **≈1M inode ceiling** comes from:
at most 65533 i-list blocks.

`s_lasti` and `s_nbehind` are kernel search hints and carry no consistency
meaning; nothing here depends on them.

### The System V dialect

`mkfs -F sysv` writes a second superblock flavor. The **common part (0–417) is
byte-identical**; SysV then inserts `s_dinfo[4]` at 418, which pushes the
totals later, and adds a magic and block-size type near the end of the block:

| offset | field | width | meaning |
|---|---|---|---|
| 418 | `s_dinfo[4]` | 8 | device info |
| 426 | `s_tfree` | 4 | total free blocks |
| 430 | `s_tinode` | 2 | total free inodes |
| 432 | `s_fname[6]` | 6 | filesystem name |
| 438 | `s_fpack[6]` | 6 | pack name |
| 500 | `s_state` | 4 | clean marker: `0x7c269d38 - s_time` |
| 504 | `s_magic` | 4 | `0xfd187e20` |
| 508 | `s_type` | 4 | 1 / 2 / 3 = 512 / 1024 / 2048 |

The point of the design: **all of that lands in the V7 layout's zeroed tail**,
so a SysV-flavored image still reads correctly as a plain image. The fields
that matter for traversal — `isize`, `fsize`, the inode count, the directories,
the free list — are dialect-independent. Only the totals move, and they are
advisory.

`s_type` is the one place the block size *is* recorded, but only in this
dialect, so it cannot be relied on generally.

---

## 3. The inode (`struct dinode`, 64 bytes)

| offset | field | width | meaning |
|---|---|---|---|
| 0 | `di_mode` | 2 | type + permission bits |
| 2 | `di_nlink` | 2 | link count |
| 4 | `di_uid` | 2 | owner |
| 6 | `di_gid` | 2 | group |
| 8 | `di_size` | 4 | size in bytes |
| 12 | `di_addr[40]` | 40 | up to 13 × 3-byte block numbers |
| 52 | `di_atime` | 4 | access time |
| 56 | `di_mtime` | 4 | modification time |
| 60 | `di_ctime` | 4 | inode-change time |

`di_addr` is **40 bytes holding 3-byte packed block numbers** — 13 × 3 = 39,
with a byte of slack. At the 1024 profile only the first 7 slots (21 bytes) are
used and the rest are zero.

`di_size` is a *signed* 32-bit byte count, which is the **2 GiB single-file
limit**.

Inode `n` (1-based) lives at:

```
  block  = (n + 2*inopb - 1) / inopb
  offset = ((n + 2*inopb - 1) % inopb) * 64
```

which places inode 1 at block 2 offset 0 — the arithmetic folds the i-list's
two-block start offset into the division. It is written this way to match the
original `itod`/`itoo` macros exactly.

### Mode bits

| bits | meaning |
|---|---|
| `0170000` | type mask |
| `0040000` | directory |
| `0020000` | character special |
| `0060000` | block special |
| `0100000` | regular |
| `0004000` / `0002000` / `0001000` | setuid / setgid / sticky |
| `0000400` / `0000200` / `0000100` | read / write / execute (owner) |

A **device node** stores its major/minor packed as `(major << 8) | minor` in
`di_addr[0]` and has no data blocks.

### Times

The writer treats **zero as "stamp with the superblock's time"** and any
nonzero value as verbatim. That one rule is what lets `restore`, `tar x`, and
`mktree` preserve real 1980s dates instead of rewriting history to the moment
the image was built. It is an invariant, not a convenience — see
[`design.md`](design.md) §5.

---

## 4. The block map

`di_addr` slots are read as: `laddr` direct, then single, double, and triple
indirect. An indirect block is a flat array of 4-byte block numbers,
`nindir` of them.

| profile | direct | single | double | triple |
|---|---|---|---|---|
| 512 (`laddr` 10) | 10 | +128 | +16 384 | +2 097 152 |
| 1024 (`laddr` 4) | 4 | +256 | +65 536 | +16 777 216 |
| 2048 (`laddr` 10) | 10 | +512 | +262 144 | +134 217 728 |

Two writers build these maps, and the difference is deliberate:

- **`s5fs_iput`** — the ported `mkfs` inode writer. Direct slots plus *one*
  level of single indirection, which is all `mkfs` ever needed for the root and
  `lost+found`. It is kept exactly as the original so the empty-filesystem path
  reproduces native `mkfs` byte-for-byte.
- **`s5fs_setblocks`** — the general writer used by every front-end. Full
  direct + single/double/triple, allocating indirect blocks as it goes, so real
  files of any size can be stored.

Do not merge them. The first one's fidelity is the thing being protected.

---

## 5. Directories

A directory is an ordinary file whose contents are fixed 16-byte records:

```
  offset 0   2 bytes   inode number (0 = free slot)
  offset 2  14 bytes   name, NUL-padded, NOT NUL-terminated when 14 chars
```

Consequences worth stating plainly:

- **Names are capped at 14 characters** and a 14-character name has no
  terminator. Always copy into a 15-byte buffer and terminate by hand.
- **Deletion zeroes only the inode field.** The name bytes stay. That is
  exactly what `scavenge` reads.
- Entry 0 is `.` and entry 1 is `..` by construction. The root's `..` points to
  itself.
- A directory's link count is 2 plus one per child subdirectory — so a
  directory is **never** link-count-zero, even when orphaned. See
  [`fsck.md`](fsck.md) §4.

### The stale-buffer trap

`fsr_readfile` correctly clamps its read to the file's size. A loop that
iterates all `ndirect` slots of the last block anyway will read **leftover
buffer bytes past the real directory size and invent ghost entries** — this
once produced 977 directory lines where 247 were real.

**Bound directory iteration by the returned byte count** (`e * 16 < got`), never
by slot capacity. This bug has been introduced twice, in `cmd_analysis.c` and
`cmd_scavenge.c`. It will be introduced again.

---

## 6. Free blocks

Free space is a **chained list**, not a bitmap. The superblock caches 50
addresses; when they run out, the last one points at a block holding the next
50, and so on. The list is rotationally interleaved rather than sequential.

That design has consequences well beyond allocation — it is why files are not
contiguous and why undelete is impossible — so it has its own document:
[`allocator.md`](allocator.md).

The on-disk chain block is trivial:

```
  offset 0   2 bytes   count
  offset 2   200 bytes daddr_t[50]
```

---

## 7. For a maintainer

- **`naddr` is 7 at 1024 and 13 at 512/2048.** Not a typo. Small files will
  hide a mistake here.
- **Add a field by adding an offset here plus codec calls** — never by declaring
  a `struct` over disk bytes. That is invariant #1.
- **14-character names have no terminator.**
- **Bound directory loops by bytes returned**, not by `ndirect` (§5).
- **Zero time means "now".** Nonzero passes through verbatim.
- **The SysV dialect lives in the V7 tail**, so it is additive; don't branch
  traversal on it.
- **`s5fs_iput` is frozen for fidelity.** New block-mapping work belongs in
  `s5fs_setblocks`.

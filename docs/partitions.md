# Disks and partitions

How a filesystem is placed inside a larger file, how disk geometry is selected,
and why partitioning here looks nothing like a modern partition table.

Code: `src/device.[ch]`; the base offset threads through `fsread`, `s5fs_core`,
and `s5fs_rw`.

---

## 1. A partition is a base offset. That is the entire model.

There is **no on-disk label** on these disks. Nothing on the platter says where
partition `a` ends or that partition `c` exists.

Instead, the kernel driver had the layout **compiled into it** — a `*_sizes[]`
table in `sys/GENERIC/ioconf.c` — and `minor(dev) & 7` indexed it. The disk had
to conform to the kernel, not the other way round. Change kernels, and the same
platter is partitioned differently, with no warning and no way to detect the
mismatch.

`device.c` mirrors those tables:

```c
typedef struct {
        char     letter;   /* 'a'..'h'                        */
        uint32_t start;    /* first block, in 512-byte blocks */
        uint32_t len;      /* length in 512-byte blocks       */
} disk_part;
```

**Overlap is normal and correct.** Partitions `a`, `b`, and `c` typically tile
the disk while `g` or `h` alias the whole thing. A modern check that rejects
overlapping partitions would reject every real 2.9BSD disk.

Everything below the CLI reduces the selection to one number: `base`, a byte
offset. `fsread`, `s5fs_core`, and `s5fs_rw` all take it, add it to every block
offset, and are otherwise unaware that a partition is involved.

---

## 2. Selecting one

Three ways, in precedence order:

```sh
s5fs mkfs -o 20000:40000 disk.dsk    # explicit START[:LEN], 512-byte blocks
s5fs mkfs -d rp06 -P a  disk.dsk     # partition 'a' of an RP06
s5fs mkfs           bare.dsk         # base 0, the whole file
```

`-o` wins if given, then `-d`/`-P`, otherwise the whole image. `-o` exists for
disks whose table you do not have, images you are reverse-engineering, and
fragments.

Two isolated filesystems in one whole-disk image:

```sh
s5fs mkfs -d rp06 -P a disk.dsk
s5fs mkfs -d rp06 -P c disk.dsk
s5fs fsck -d rp06 -P a disk.dsk      # each checks independently
s5fs fsck -d rp06 -P c disk.dsk
```

The test suite asserts exactly this: a file written into `a` is absent from `c`,
the whole-disk file is the full RP06 size, and both partitions `fsck` clean.

---

## 3. The grow-only rule

**A whole-disk operation on a partition must never shrink the file.**

`s5fs_begin` sizes the image so that unallocated blocks read as zero. For a
bare image (`base == 0`) it `ftruncate`s to exactly the filesystem size. For a
partition (`base != 0`) that would be catastrophic — the file is a *whole disk*
the caller already sized, and truncating it to one partition's length destroys
every partition after this one:

```c
if (fs->base == 0) {
        ftruncate(fd, (off_t)nblocks * fs->bsize);          /* exact */
} else {
        off_t need = fs->base + (off_t)nblocks * fs->bsize;
        if (fstat(fd, &st) == 0 && st.st_size < need)
                ftruncate(fd, need);                        /* grow only */
}
```

This was a real bug — `mkfs -P` truncating a whole-disk image down — and it was
found with `strace`, by noticing a double `ftruncate`. It is invariant #7 for a
reason.

---

## 4. The built-in table

Capacities are in 512-byte blocks, cross-checked between the 2.9BSD drivers and
SIMH's `pdp11_*.c` geometry.

| device | blocks | notes |
|---|---|---|
| `rk05` | 4 872 | RK11 cartridge |
| `rl01` / `rl02` | 10 240 / 20 480 | RL11 cartridges |
| `rk06` / `rk07` | 27 126 / 53 790 | RK611 |
| `rp03` | 83 180 | RP11 |
| `rm03` | 131 680 | Massbus |
| `rp04` / `rp05` | 171 798 | Massbus |
| `rm80` | 242 606 | Massbus |
| `rp06` | 340 670 | Massbus |
| `rm05` | 500 384 | Massbus |
| `rp07` | 1 008 000 | Massbus |
| `rx50` … `ra92` | 800 … 2 940 951 | MSCP family (2.10-era) |

`s5fs devices` prints the effective table; `s5fs devices <name>` prints one
disk's partitions.

### Geometry is hardware; drivers are software

A device selection only fixes the **total size**, and geometry is a property of
the drive, identical across releases. So the per-release `since`/`until` tags in
the table are **advisory only**: they exist to print a gentle heads-up if you
volunteer a `--target` release and pick a disk whose driver was not usually in
that release's kernels.

It is never an error, never blocks, and with no `--target` it never fires at
all. PDP-11 driver provenance is genuinely messy — backports, site ports,
one-off Emulex controllers — so this is a hint, not a rule. Keep it that way; a
hard check here would reject legitimate historical configurations.

---

## 5. User-defined disks

The built-in table can be extended or overridden by an INI file, found via
`$S5FS_DEVICES` or `~/.config/s5fs/devices`. `examples/devices.ini` is the
template:

```ini
[eagle]
blocks     = 1002000
desc       = Fujitsu M2351 Eagle (SMD, ~490 MB)
partitions = a:0:20000 b:20000:20000 c:40000:962000 h:0:1002000
```

| key | meaning |
|---|---|
| `blocks` | **required** — capacity in 512-byte blocks |
| `desc` | shown by `s5fs devices` |
| `since` / `until` | advisory range (`v7`\|`2.8`\|`2.9`\|`2.10`) |
| `partitions` | `letter:start:length …` in 512-byte blocks |

Unknown keys are ignored on purpose, so adding a geometry field later cannot
break someone's existing file. A device with no `blocks` is skipped with a
warning rather than aborting the run.

**Match your kernel's table.** Since the disk carries no label, an INI entry
that disagrees with the kernel you intend to boot produces a filesystem the
kernel will read at the wrong offset.

---

## 6. Device nodes

Related, and a different flavor of the same "must match the kernel" problem.
`mktree -D` takes a spec file describing `/dev`:

```
# name  type  major  minor  [octal-mode]
console c 0 0 0600
null    c 2 2 0666
rl0     b 5 0 0600
```

A device node has no data blocks; its major and minor are packed into
`di_addr[0]` as `(major << 8) | minor`.

**The major numbers must match the target kernel's `cdevsw[]`/`bdevsw[]`.**
Those are configuration-specific — read the kernel's `c.c`. The values in
`examples/dev.spec` are illustrative of a small 2.9BSD `/dev`, not
authoritative.

---

## 7. For a maintainer

- **`base` is a byte offset and nothing more.** Don't grow it into a partition
  abstraction; the layers below it should stay unaware.
- **Grow-only when `base != 0`.** Never `ftruncate` a whole-disk image down
  (§3).
- **Overlapping partitions are legal.** Don't validate them away.
- **The driver advisory is advisory.** It must never become an error or a
  blocker.
- **Unknown INI keys are ignored deliberately** — that is the forward
  compatibility story.
- **Capacities are in 512-byte blocks everywhere in this file**, regardless of
  the filesystem block size. Mixing the two units is the easy mistake.

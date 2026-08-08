# s5fs — user guide

`s5fs` creates, inspects, edits, repairs, and converts disk images holding the
traditional Unix filesystem — the V7 / System V filesystem, called **s5fs**. It
runs on your host, needs no kernel support, and never has to mount anything.

For how it works, see the [design document](design.md).

---

## 1. Synopsis

```
s5fs <command> [args ...]
```

Run any command with no arguments for its own usage. `s5fs help` lists them all.

Most commands take the same image-selection options:

| option | meaning |
|---|---|
| `-B 512\|1024\|2048` | filesystem block size (default **1024**) |
| `-A pdp11\|le\|be` | on-disk byte order (default: **auto-detect** on read) |
| `-d DEV` | disk type, e.g. `rl02`, `rp06` (`s5fs devices` lists them) |
| `-P LETTER` | partition `a`..`h` of that disk type |
| `-o BLK[:LEN]` | explicit base offset in 512-byte blocks, instead of `-d`/`-P` |

There is **no on-disk marker for the block size**, exactly as the era's
compile-time `BSIZE` worked — so if an image is not 1024, you must say `-B`.
Byte order *is* auto-detected, because the superblock's `isize`/`fsize` pair only
decodes sanely under the right codec.

---

## 2. The commands

**Create and populate**

| command | what it does |
|---|---|
| `mkfs` | create a filesystem image |
| `mktree` | build an image from a host directory tree |
| `tar c` / `tar x` | image → tar archive, and tar → image (reads `.gz`/`.bz2`/`.Z`) |
| `restore` | restore a 2.9BSD dump tape into an image (`-f` past a bad checksum) |
| `dump` | write a 2.9BSD dump tape from an image (`-T` for SIMH `.tap`) |
| `boot` | install a bootstrap into block 0 |
| `vhd` | wrap / unwrap / describe a fixed-VHD container |

**File access** — no mount required

| command | what it does |
|---|---|
| `ls` `cat` `get` `put` `cp` `mv` `rm` | the obvious things |
| `mkdir` `rmdir` `chmod` `chown` `chgrp` | the other obvious things |
| `shell` | interactive explorer with a mount table (§5) |
| `mount` / `umount` | FUSE-mount an image (needs `make FUSE=1`) |

**Check and repair**

| command | what it does |
|---|---|
| `fsck` | check an image; `-p` repairs, `-l` lists the tree |
| `icheck` | block and free-list check; `-s` salvages the free list |
| `dcheck` | directory link-count check |
| `clri` | clear (zero) inodes by number |
| `fsdb` | interactive filesystem debugger; `-w` to edit |

**Analyze**

| command | what it does |
|---|---|
| `manifest` / `verify` | fingerprint an image, and diff one against a manifest |
| `scavenge` | recover deleted-file remnants (§7) |
| `ncheck` | inode → path; `-s` audits setuid/setgid and device nodes |
| `quot` | blocks and files per owner |
| `du` | disk usage per directory subtree |
| `df` | block and inode usage summary |
| `labelit` | read or set the volume label |
| `devices` | list known disk types and their partitions |

---

## 3. Making a filesystem

```sh
s5fs mkfs -d rl02 root.dsk           # size it from a known disk type
s5fs mkfs -b 20000 small.dsk         # or give a block count directly
s5fs mkfs -B 2048 -a le big.dsk      # 2048-byte blocks, little-endian
s5fs mkfs -F sysv -a le sysv.dsk     # also write the System V superblock magic
```

Useful `mkfs` options:

| option | meaning |
|---|---|
| `-d DEV` / `-b N` / `-s N` | size from a disk type, a block count, or 512-byte sectors |
| `-B N` | block size (512 / 1024 / 2048) |
| `-a BO` | byte order to write (`pdp11` default, `le`, `be`) |
| `-F sysv` | add the System V superblock magic and type |
| `-i N` | minimum inode count (otherwise the historical size heuristic) |
| `-t SECS` | timestamp to stamp, instead of now |
| `-m` / `-n` | free-list rotational interleave (defaults 5 and 10) |

`mkfs` **refuses** a filesystem larger than 2²⁴ blocks rather than silently
truncating the block addresses. See [Limits](#8-limits).

---

## 4. Getting files in and out

Nothing here needs root, a loop device, or a kernel that understands s5fs.

```sh
s5fs put  img.dsk ./hello /bin/hello      # host  → image
s5fs get  img.dsk /etc/passwd ./passwd    # image → host
s5fs cat  img.dsk /etc/motd
s5fs ls -l img.dsk /bin
s5fs cp   img.dsk /bin/sh /bin/sh.bak     # within the image
s5fs cp   img.dsk @./local /tmp/remote    # '@' = a HOST path
s5fs rm   img.dsk /tmp/junk
s5fs mkdir -p img.dsk /usr/local/bin
```

The `@` prefix marks a host path, so `cp` moves data in either direction.

To build a whole image from a host tree, or to move a tree between images:

```sh
s5fs mktree -d rp06 ./rootfs new.dsk      # host tree → fresh image
s5fs tar c img.dsk backup.tar             # image → tar
s5fs tar x -d rl02 backup.tar new.dsk     # tar → fresh image
```

`mktree` preserves host times for **files and directories** alike. The entries
it synthesizes rather than copies — `lost+found`, and a `/dev` built from `-D` —
are stamped with the filesystem's time, which `-t` sets; they correspond to
nothing on the host, so there is no time to preserve. Host device nodes are not
stored at all (they are skipped and counted in the summary).

---

## 5. The interactive shell

`s5fs shell` is not a single-image browser — it is a small VFS with a mount
table:

```sh
s5fs shell root.dsk                       # auto-mounts at /
```

```
mount usr.dsk /usr            mount another image anywhere
mount -r old.dsk /old         read-only
umount /usr                   drop a mount
mounts                        show the table
cd  pwd  ls [-l] [-a]  cat  stat
get imgpath [host]            put host [imgpath]
cp [@]src [@]dst              mv  rm  mkdir  rmdir  chmod  chown  chgrp
help  quit
```

Every path is routed to the mount serving it — **the deepest mountpoint that
prefixes the path wins** — so overlapping mounts shadow the way you would
expect, and `cp` resolves each side independently:

```
mount a.dsk /a
mount b.dsk /b
cp /a/etc/passwd /b/tmp/passwd        # copies BETWEEN two different images
cp @./hostfile /a/tmp/x               # '@' escapes to the host filesystem
```

A write to a mount you mounted `-r` returns `Read-only file system`. Virtual
intermediate directories — path components no mount claims — are navigable with
`cd`.

---

## 6. Checking and repairing

```sh
s5fs fsck img.dsk                 # check; prints "clean" if it is
s5fs fsck -l img.dsk              # ... and list the tree
s5fs fsck -p img.dsk              # repair what it can
```

`fsck` runs three phases: block accounting (every block belongs to exactly one
of reserved / inode-referenced / free), inode and directory sanity (dangling
entries, bad `.`, link counts), and connectivity (anything allocated but
unreachable from the root is an orphan). `-p` zeroes directory entries pointing
at free inodes, reconnects orphans into `/lost+found`, and rebuilds the free
list.

`icheck`, `dcheck`, and `clri` are the narrower historical tools, kept
separately for when you want just one of those jobs.

For hand inspection and surgical edits:

```sh
s5fs fsdb img.dsk                 # read-only
s5fs fsdb -w img.dsk              # allow edits
```

`fsdb` understands `sb`, `inode N`, `path /some/file`, `block N`, and — with
`-w` — `set N FIELD VALUE`. Always re-run `fsck` after editing.

To detect drift over time:

```sh
s5fs manifest img.dsk > baseline.txt
s5fs verify img.dsk baseline.txt      # nonzero exit if anything changed
```

---

## 7. Scavenging deleted files

```sh
s5fs scavenge img.dsk                 # report
s5fs scavenge -x outdir img.dsk       # extract what it can carve
```

**This is not undelete, and it cannot be.** The free list is chained *through*
the freed blocks themselves, so roughly one freed block in fifty is overwritten
by the chain, and files were never laid out contiguously to begin with. What
`scavenge` actually does is recover **deleted names** — ghost directory entries
whose inode field was cleared but whose name bytes survive — and **carve by
signature** (`a.out`, `ar`, `tar`, and plain text). Expect names and fragments,
not files. The command was renamed from `recover` precisely so the name would
stop over-promising.

---

## 8. Limits

| | limit | why |
|---|---|---|
| filesystem | 2²⁴ blocks — 8 GiB @512, 16 GiB @1024, 32 GiB @2048 | block addresses are 3 bytes |
| single file | 2 GiB | `di_size` is a signed 32-bit byte count |
| inodes | **65535** | inode numbers are 16-bit; a larger tree is refused |
| file name | 14 characters | the directory entry is 2 + 14 bytes — a longer name is **refused**, not truncated |
| block size | 512, 1024, 2048 | the family maximum is `Fs4b` |

Out of scope by design: V6 and 2.11BSD, FFS/UFS, and dynamic or differencing
VHD.

---

## 9. Exit status

Zero on success. Non-zero on a malformed or unreadable image, a failed
operation, a `verify` that found differences, or an `fsck` that found errors it
was not asked to repair.

---

## 10. Examples

```sh
# Build a bootable-shaped root from a host tree and check it
s5fs mktree -d rl02 ./rootfs root.dsk
s5fs boot root.dsk ./mdec/rlboot
s5fs fsck root.dsk

# Two filesystems in one whole-disk image, checked independently
s5fs mkfs -d rp06 -P a disk.dsk
s5fs mkfs -d rp06 -P c disk.dsk
s5fs fsck -d rp06 -P a disk.dsk
s5fs fsck -d rp06 -P c disk.dsk

# Write a tape a real 2.9BSD restor(8) will read, framed for a SIMH drive
s5fs dump -T root.dsk root.tap

# Wrap for a hypervisor, and unwrap again
s5fs vhd wrap root.dsk root.vhd
s5fs vhd unwrap root.vhd root.back
```

Continue to the [design document](design.md) for the architecture, or to
[`on-disk-format.md`](on-disk-format.md) for the format itself.

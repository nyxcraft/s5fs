# The mutation engine

Everything that changes an image goes through `src/s5fs_rw.[ch]`. This is
invariant #2 — *one mutation path* — made concrete: the batch file commands, the
interactive shell, and the FUSE mount are three front-ends over one
implementation of mkdir, unlink, rename, truncate, and the rest.

The directory-slot, inode-allocation, and block-map logic here was originally
proven inside the FUSE read-write layer, then factored out precisely so the
non-FUSE commands could run the same proven code rather than a second copy.

---

## 1. The RW handle: two fds, one image

```c
typedef struct {
        FSR  r;         /* read side  (always open)        */
        S5FS w;         /* write side (valid if writable)  */
        int  writable;
} RW;
```

An `RW` opens the same file **twice** — once read-only through the reader, once
`O_RDWR` through the writer:

```c
int rw_open(RW *h, const char *path, uint32_t bsize, int forced_bo,
            int writable, int64_t base)
{
        if (fsr_open(&h->r, path, bsize, forced_bo, base) < 0)
                return -1;
        if (writable) {
                int wfd = open(path, O_RDWR);
                if (wfd < 0 || s5fs_mount(&h->w, wfd, h->r.bsize, h->r.bo, base) < 0)
                        ... unwind ...
                h->writable = 1;
        }
        return 0;
}
```

Two things make this work rather than being a hazard:

- **The two fds are page-cache coherent.** On any Unix, two descriptors on the
  same file share the page cache, so the read side observes the write side's
  writes immediately. No flush-and-reopen dance is needed, and none exists.
- **The writer inherits the reader's decoded parameters** — `h->r.bsize` and
  `h->r.bo`. Byte order is auto-detected exactly once, by the reader, and the
  writer is then told. There is no second detection to disagree with the first.

The alternative — one fd, with the reader and writer sharing it — would have
meant one of the two layers giving up its own file position and buffering
model. Two coherent fds is the cheaper correctness.

Read-only is a first-class mode: `writable = 0` leaves `w` untouched, and every
mutating entry point checks the flag. That is what the shell's `mount -r`
rides on.

---

## 2. Consistency after every operation

Invariant #3. Every high-level `rw_*` operation calls `rw_sync` before
returning:

```c
void rw_sync(RW *h) { if (h->writable) s5fs_finish(&h->w); }
```

`s5fs_finish` serializes the in-core superblock — free list, counts, timestamp
— back to block 1. So after `rw_mkdir` returns, the image on disk is a valid
filesystem, not a half-updated one waiting for a close.

This costs one block write per operation and buys the property the test suite
leans on: **`fsck` can be run after any single operation and must say `clean`.**
Preserve it.

The one deliberate exception is documented at the call site: a bulk loop that
writes many blocks syncs **once at the end** rather than per block. Single-shot
callers sync themselves.

---

## 3. The block map

`rw_bmap` maps a logical block number to a physical one, optionally allocating:

```c
uint32_t rw_bmap(RW *h, uint8_t *ds, uint32_t lbn, int alloc, int *dirty);
```

`ds` is the caller's copy of the raw 64-byte dinode; `dirty` is set when the map
changed and the inode needs writing back. Passing the dinode in — rather than
re-reading it per call — is what lets a write loop touch the inode once.

The map is the format's: `laddr` direct slots, then single, double, and triple
indirect, with `ind_map` recursing a level at a time and allocating indirect
blocks on demand when `alloc` is set. `per_at(levels)` gives the data blocks
covered per entry at each level (`nindir^(levels-1)`).

Inode location is the same arithmetic as everywhere else, exposed for the FUSE
random-write path:

```c
void rw_ino_loc(RW *h, uint32_t ino, uint32_t *blk, uint32_t *off)
{
        *blk = (ino + 2 * h->w.inopb - 1) / h->w.inopb;
        *off = ((ino + 2 * h->w.inopb - 1) % h->w.inopb) * P11_DINODESZ;
}
```

---

## 4. The operations

All paths are **absolute within the image** (`/bin/sh`). All return `0` or a
negative errno, so a FUSE front-end can return the value directly and a CLI
front-end can `strerror(-rc)` it.

| operation | notes |
|---|---|
| `rw_mkdir(path, perm)` | creates `.` and `..`, bumps the parent's link count |
| `rw_rmdir(path)` | refuses a non-empty directory |
| `rw_unlink(path)` | zeroes the directory slot's inode field; frees blocks at link 0 |
| `rw_rename(from, to)` | within one image; **`EINVAL`** if `to` is inside `from` (§5) |
| `rw_creat(path, perm, &ino)` | returns the new inode number |
| `rw_chmod` / `rw_chown` / `rw_utimes` | metadata only |
| `rw_pwrite(ino, buf, size, off)` | allocating random write |
| `rw_truncate(ino, len)` | grows or shrinks; frees the tail |
| `rw_put_fd(path, srcfd, perm, mtime)` | stream a host fd into the image |
| `rw_copy(src, dst)` | within one image |
| `rw_copy_between(sh, src, dh, dst)` | **across two images** |

`rw_copy_between` is the primitive that makes the shell's cross-image `cp` work
(see [`shell.md`](shell.md)). It takes two `RW` handles and streams through,
rather than requiring a host temporary file.

`rw_utimes` and the `mtime` argument to `rw_put_fd` exist so front-ends can
honor invariant #6 — preserve times rather than stamping them. A caller that
knows the original time must pass it.

---

## 5. Two refusals worth knowing

**A name longer than 14 bytes is refused, not truncated** (`ENAMETOOLONG`), and
checked *before* anything is allocated so a failure leaks no inode. The format
cannot represent the name; truncating produced two entries with the same
on-disk name, both unreachable under the caller's name.

**A directory cannot be renamed into its own subtree** (`EINVAL`, as POSIX
requires). `is_ancestor` walks `..` from the destination to the root. Without
it the subtree kept its internal links but lost every reference from the root —
one successful-looking `mv` silently detached it, and `fsck -p` reconnecting the
result left a directory cycle behind.

## 6. Deletion, precisely

Worth spelling out because two subsystems depend on the details:

1. The directory entry's **2-byte inode field is zeroed**. The 14 name bytes
   are left alone — which is what [`scavenge`](allocator.md) reads.
2. The inode's link count drops. If it reaches zero, the block map is walked
   and every block is returned via `s5fs_bfree`, and the inode is cleared.
3. Because `s5fs_bfree` spills the superblock cache into a freed block every
   50th call, deletion **destroys the contents of roughly one freed block in
   fifty**. See [`allocator.md`](allocator.md) §2.

Step 1 is why deleted names survive. Step 3 is why deleted *files* do not.

---

## 7. Extending it

New file operation:

1. Implement `rw_<op>` in `s5fs_rw.c`, ending in `rw_sync`.
2. Declare it in `s5fs_rw.h`.
3. Wire it into `cmd_fs.c` (batch), `cmd_shell.c` (interactive), and
   `cmd_mount.c` (FUSE) as appropriate.
4. Add a check to `tests/run.sh` that ends `fsck clean`.

If step 1 finds itself re-deriving directory-entry offsets or indirect-block
arithmetic, the primitive already exists — `rw_bmap`, `rw_ino_loc`,
`s5fs_alloc`, `s5fs_setblocks`.

---

## 8. For a maintainer

- **Two fds, page-cache coherent.** Don't "optimize" this into one fd, and
  don't add a flush-and-reopen; the coherence is what the design rests on.
- **Byte order is detected once, by the reader.** The writer is told. Never
  detect twice.
- **Every high-level op ends in `rw_sync`.** A bulk loop may sync once at the
  end, but a single-shot caller must not skip it.
- **Return `-errno`, not `-1`.** The FUSE layer passes the value straight
  through.
- **Paths are absolute within the image.** Host paths never reach this layer —
  the `@` escape is resolved by the front-end.
- **Preserve times.** If a caller knows the original mtime, it must pass it;
  defaulting to "now" is the regression that keeps trying to come back.
- **Check the name before allocating.** `name_ok()` runs ahead of `ialloc_scan`
  in every op that creates an entry, so a refused name costs nothing.

# The allocator: chained free list and rotational interleave

Free space in s5fs is a **chained list threaded through the free blocks
themselves**, laid out in a rotational permutation. It is not a bitmap, it is
not an extent map, and it is not sorted. Almost everything surprising about
this filesystem — why files aren't contiguous, why undelete is impossible, why
`fsck` has to walk a chain rather than compare bitmaps — falls out of this one
design.

The code is `src/s5fs_core.c`, ported line-for-line from 2.9BSD `mkfs.c`
(SCCS 2.5).

---

## 1. The structure

The superblock caches up to **50** (`NICFREE`) block addresses. When they run
out, the *last* one taken is not a data block at all — it is a chain block
holding the next 50 addresses, and so on down the volume.

```
  superblock                chain block               chain block
  ┌──────────────┐          ┌──────────────┐          ┌──────────────┐
  │ s_nfree = 50 │          │ nfree = 50   │          │ nfree = 50   │
  │ s_free[0] ───┼────────► │ free[0] ─────┼────────► │ free[0] ──►…  │
  │ s_free[1..49]│          │ free[1..49]  │          │ free[1..49]  │
  └──────────────┘          └──────────────┘          └──────────────┘
     49 real free              49 real free              49 real free
     blocks + a link           blocks + a link           blocks + a link
```

`s_free[0]` is always the link. The chain terminates when that link is **block
0** — which is why `mkfs` frees block 0 first, before any real block: it sinks
to the bottom of the list and becomes the end marker.

On-disk a chain block is just a count and an array:

```
  offset 0    2 bytes    nfree
  offset 2  200 bytes    daddr_t[50]
```

---

## 2. Allocate and free

Both operations are stack-like, on the superblock's cache:

```c
int32_t s5fs_alloc(S5FS *fs)
{
        fs->s_tfree--;
        bno = fs->s_free[--fs->s_nfree];
        if (bno == 0)                    /* hit the terminator: volume full */
                return fail("out of free space");
        if (fs->s_nfree <= 0)            /* that block WAS the chain block */
                s5fs_get_fblk(fs, bno);  /* read the next 50 out of it */
        return bno;
}

void s5fs_bfree(S5FS *fs, int32_t bno)
{
        if (bno != 0)
                fs->s_tfree++;
        if (fs->s_nfree >= P11_NICFREE) {
                s5fs_put_fblk(fs, bno);  /* spill the full cache INTO bno */
                fs->s_nfree = 0;
        }
        fs->s_free[fs->s_nfree++] = bno;
}
```

Read those two carefully, because the whole document follows from them:

- **Allocation is LIFO.** The last block freed is the first block handed out.
- **Emptying the cache consumes the chain block.** When `s_nfree` reaches zero
  the block just popped is itself the link; its 50 addresses are read into the
  cache and the block is then handed to the caller as ordinary data.
- **Freeing periodically destroys data.** When the cache is full, the block
  being freed is *overwritten* with the cache contents to become the new chain
  block. Roughly **one freed block in fifty** has its contents destroyed at the
  moment of deletion.

That last point is not a bug and cannot be fixed — it is how the format stores
its free list. It is also the reason `scavenge` is honest about what it can do
(§5).

---

## 3. The rotational interleave

A 1980s disk had real rotational latency. Handing out block *n+1* right after
block *n* was the worst case: by the time the driver issued the next request,
the head had already passed the sector, costing a full revolution. So `mkfs`
lays the free list out **pre-scrambled**, such that consecutively allocated
blocks are physically spaced far enough apart that the next one arrives under
the head just in time.

Two superblock parameters describe it (`s_dinfo`, at offset 424): **`m`** the
interleave gap and **`n`** the number of blocks in a rotation group. Defaults
are `m = 5`, `n = 10`, clamped as the original does — `n` out of range becomes
500, and an `m` out of range becomes 3.

The permutation is built by stepping through a rotation group `m` at a time,
skipping positions already taken:

```c
i = 0;
for (j = 0; j < fn; j++) {
        while (flg[i]) i = (i + 1) % fn;   /* skip taken slots */
        fs->adr[j] = i + 1;
        flg[i]++;
        i = (i + fm) % fn;                 /* step by the gap */
}
```

With the defaults that yields `adr = [1, 6, 2, 7, 3, 8, 4, 9, 5, 10]`.

The volume is then freed **from the top down**, one rotation group at a time,
visiting each group in permuted order:

```c
s5fs_bfree(fs, 0);                    /* terminator, first so it sinks lowest */
d = fsize - 1;
while (d % fn) d++;                   /* round up to a group boundary */
for (; d > 0; d -= fn)
        for (i = 0; i < fn; i++) {
                f = d - fs->adr[i];
                if (f < fsize && f >= isize)
                        s5fs_bfree(fs, f);
        }
```

Because freeing is LIFO and this runs high-to-low, **allocation begins at the
low end of the volume** and walks upward — while, within each group of 10, the
blocks come out spread across the rotation rather than in sequence.

The practical upshot for a modern reader: **a file's blocks are not
contiguous and not monotonic.** Anything that assumes otherwise — a carver, a
"read ahead N blocks" optimization, a contiguity check — is wrong on a real
image even when it happens to work on a freshly written one.

---

## 4. Mounting an existing filesystem

`s5fs_mount` is the inverse of `s5fs_finish`: it reads block 1, decodes the
superblock into the `S5FS` handle, and leaves the allocator ready to run
against the *existing* free list. After that, `s5fs_alloc` and `s5fs_bfree`
behave exactly as they do during `mkfs`.

`s5fs_finish` serializes the decoded superblock back to block 1. Every
high-level operation in [`mutation-engine.md`](mutation-engine.md) calls it
before returning, so the image on disk is consistent after each call rather than
only at close time.

The inode side is simpler than the block side: `s5fs_ialloc` just hands out the
next number (`++fs->ino`). The superblock's 100-entry free-inode cache
(`s_inode`) is a kernel search accelerator; nothing here depends on it for
correctness, and `fsck` recomputes the totals.

---

## 5. Why undelete is impossible

This is the question the design forces, so it is worth answering once,
concretely. To recover a deleted file you would need to know **which blocks it
held** and to find **their contents intact**. Both fail:

- The inode is cleared on unlink, so the block map is gone.
- Blocks were never contiguous (§3), so you cannot infer the map from a
  starting block.
- Roughly 1 in 50 freed blocks was overwritten by the free-list chain at the
  moment of deletion (§2), so even a correct guess hits destroyed data
  regularly.

What *does* survive is the **name**: unlink zeroes only the directory entry's
2-byte inode field, leaving the 14 name bytes in place. And file *content* can
sometimes be identified by **signature** — `a.out`, `ar`, `tar`, plain text.

So `scavenge` recovers deleted names and carves by signature, and its
`build_used` walks the free-list chain specifically to **exclude chain blocks
from text carving** — those blocks contain the list, not user data, and
reporting them as recovered text would be noise.

The command was renamed from `recover` to `scavenge` for exactly this reason.
Keep the name and keep the documented limits honest; a user who expects
`undelete` will be misled by anything more optimistic.

---

## 6. For a maintainer

- **`s_free[0]` is the link, and block 0 terminates the chain.** Free block 0
  first, before any real block.
- **Allocation is LIFO**, so free order determines allocation order. Changing
  the free-list construction changes the on-disk layout of every file written
  afterwards.
- **Freeing destroys data every 50th block.** Any "recover" feature has to be
  designed around this, not in spite of it.
- **The interleave permutation is `mkfs`'s, verbatim.** It is part of what makes
  output byte-identical to the native tool; don't "improve" it.
- **You should almost never need to touch this file.** New file operations
  belong in `s5fs_rw.c`; new formats belong in a `cmd_*.c`. Changes here move
  bytes in every image the tool has ever produced.

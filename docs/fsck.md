# The checker

`s5fs fsck` verifies that an image is a consistent filesystem, and with `-p`
repairs what can be repaired. `icheck`, `dcheck`, and `clri` are the same
machinery exposed as the narrower historical tools.

The code is `src/cmd_fsck.c`.

---

## 1. Why it shares no code with the writer

This is the design decision that makes the checker worth anything.

`cmd_fsck.c` has **its own reader** — its own `rdblk`, its own inode decode, its
own `bmap`, its own directory walk. It shares nothing with `s5fs_core.c` except
the structural constants in `pdp11fs.h` and the byte-order codecs in
`s5endian.h`.

That duplication is deliberate. A checker built on the writer's block-mapping
code cannot detect a bug in the writer's block-mapping code — it would compute
the same wrong answer twice and call it consistent. Because these are
independent implementations of the same specification, **`s5fs fsck` is a
genuine cross-check of `s5fs mkfs`**, and "fsck clean" after a mutation is
evidence rather than a tautology.

Do not refactor them together. The redundancy *is* the feature.

---

## 2. The core claim

Everything the block check does is in service of one assertion:

> Every block in `[0, s_fsize)` belongs to **exactly one** of: reserved
> (boot, superblock, i-list), referenced by exactly one inode, or free.

Not zero of them (a lost block), not two of them (a block both free and
allocated, or multiply referenced — the corruption that silently destroys two
files at once).

The bookkeeping is one byte per block:

```c
uint8_t *use;   /* 0x7f = reference count (saturating) | 0x80 = on the free list */
```

The low seven bits count references and saturate rather than wrapping, so a
badly corrupted image reports "multiply referenced" instead of counting back
around to a plausible 1. The high bit is set by the free-list walk. At the end,
a block with both bits set is `free AND used`; a block with neither is lost.

---

## 3. The phases

`fsck` runs both check groups (`CHK_BLOCKS | CHK_LINKS`); `icheck` runs blocks
only, `dcheck` links only.

### Phase 1 — blocks

Walk every allocated inode's full block tree (direct + single/double/triple
indirect) and mark each block, including the indirect blocks themselves. Two
failures are reported here:

- **out of range** — a block number outside `[s_isize, s_fsize)`, i.e. pointing
  into the reserved area or off the end of the volume;
- **multiply referenced** — a block already marked by another inode.

Then walk the free-list chain from the superblock, marking `0x80`, reporting a
free block that is out of range or already marked used.

Finally, sweep the volume: a block that is neither used nor free is **lost**,
and the recomputed totals are compared with `s_tfree`/`s_tinode`.

### Phase 2 — inodes and directories

Each allocated inode's mode is checked against the legal type set. Then every
directory is walked, and for each entry:

- an inode number **beyond the i-list** is an error;
- an entry pointing at an **unallocated inode** is a *dangling reference*;
- `.` that does not point at its own directory is an error;
- a running per-inode reference tally is accumulated.

Afterwards the tally is compared with each inode's `di_nlink`. A mismatch is
reported both ways round — "free but *n* directories reference it" and
"`di_nlink=n` but *m* directories reference it" — because the two mean different
things about what went wrong.

### Phase 3 — connectivity

Depth-first from the root inode, marking everything reachable. Anything
allocated but unreachable is an **orphan**.

Inode 1 is marked reachable unconditionally: the bad-block list is legitimately
referenced by no directory, and flagging it every run would train the reader to
ignore orphan reports.

---

## 4. The orphan-directory subtlety

This is the trap in Phase 3, and it has to be stated because the obvious
implementation is wrong.

**A directory can never be found by looking for `nlink == 0`.** Every directory
contains `.`, which is a link to itself, so an orphaned directory still has
`nlink ≥ 1` — and a directory with subdirectories has more. It is unreferenced
by any *parent*, but its link count looks perfectly healthy.

So orphan detection must be **reachability from the root**, not a link-count
test. That is why Phase 3 exists as a separate DFS rather than being folded into
Phase 2's counting. Keep it that way.

---

## 5. Repair (`-p`)

Repair is conservative: it fixes what has an unambiguous correct answer and
reports the rest.

| problem | repair |
|---|---|
| directory entry → free or out-of-range inode | zero the entry's inode field |
| `di_nlink` disagrees with the directory count | write the counted value |
| orphaned inode | reconnect into `/lost+found` |
| lost blocks / bad totals | rebuild the free list and totals |

Zeroing the dangling entry is done first, because it changes the counts that
the link-count fix then writes — doing it in the other order would install
counts that include references about to be deleted.

Reconnection needs somewhere to put things, which is why `mkfs` preallocates
`/lost+found` with empty directory blocks at creation time: the reconnect path
must be able to add entries without allocating a block in a filesystem whose
free list is the thing currently under suspicion.

Always re-run `fsck` after `-p`, and after any `fsdb -w` edit.

---

## 6. The narrower tools

| command | equals |
|---|---|
| `icheck` | `CHK_BLOCKS` only; `-s` salvages the free list |
| `dcheck` | `CHK_LINKS` only |
| `clri` | zero an inode by number — a blunt instrument, no checking |
| `fsck -l` | the full check, plus a listing of the tree |

`clri` deliberately does not validate; it is the escape hatch for an inode so
corrupt that the checker cannot reason about it. Expect to run `fsck -p`
afterwards to clean up the references it just orphaned.

---

## 7. For a maintainer

- **Never share code with the writer.** The independence is what makes the
  check meaningful.
- **Orphan directories are not link-count-zero.** Reachability, always (§4).
- **Reference counts saturate.** Don't switch to a wrapping counter to save a
  byte.
- **Inode 1 is legitimately unreferenced.** Keep it whitelisted.
- **Repair order matters**: zero dangling entries *before* rewriting link
  counts.
- **`fsck` prints `clean` on the last line** when it finds nothing. The test
  suite matches on exactly that, so don't add trailing output after it.

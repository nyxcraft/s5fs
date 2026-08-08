# Correctness: the standard and the evidence

This toolkit is held to a **byte-fidelity** bar, not a "seems to work" bar. The
distinction matters because a filesystem writer has an unusually large space of
outputs that pass every check you would naturally write and are still wrong —
images that `fsck` clean, mount, list correctly, and differ from what the era's
own tools produce.

This document records what was actually proven, how, and what a future
maintainer can and cannot re-run.

---

## 1. The standard

> Don't settle for "fsck is happy" when you can prove exactness.

Three levels of evidence, strongest first:

1. **Byte-identical to a native tool's output**, diffed against the historical
   binary running under emulation.
2. **Accepted by a native tool** — our output consumed by the era's own program,
   which validates structures our own reader skips.
3. **Self-consistent** — round-trips, `fsck clean`, format assertions.

Level 3 is what `tests/run.sh` automates and what you get for free on any host.
Levels 1 and 2 require the oracle described in §4, which lives outside this
repo.

The comparison point is the rest of the toolchain this work came from: cross-built
binaries there came out **bit-for-bit identical** to the shipped originals. That
is the standard being aimed at, and it is achievable for a filesystem writer
because `mkfs`'s output is fully determined by its inputs.

---

## 2. What was proven

### Byte decode

Superblock, reserved inodes, and the root directory decode to expected values
across block sizes and all three byte orders.

### The writer against native `mkfs`

`s5fs_core.c` is a line-for-line port of 2.9BSD `mkfs.c` (SCCS 2.5) — the layout
arithmetic, the rotational free-list interleave, the allocator, and the
inode/directory writers. It is kept that way deliberately, which is why
`s5fs_iput` still builds only direct + one single-indirect level: that is what
the original did, and the empty-filesystem path has to reproduce native `mkfs`
byte-for-byte. General block mapping lives in a *separate* function
(`s5fs_setblocks`) precisely so the fidelity of the original path is not
disturbed. See [`on-disk-format.md`](on-disk-format.md) §4.

### dump/restore round-trips

`restore rootdump | dump | restore` reproduces a byte-identical file tree —
every inode, all data, mode/uid/gid, and the real 1980s timestamps — and the
result is `fsck` clean.

### Native-tool interop (level 2)

A tape from `s5fs dump` is accepted by the historical 2.9BSD `restor(8)` running
under `apout`:

- `restor tf` validates it;
- `restor rf` rebuilds a filesystem whose tree is byte-identical to the
  reference.

This is the strongest single result here, because it exercises structures **our
own `restore` never reads** — the `TS_CLRI` and `TS_BITS` inode maps and the
per-record checksums. Our reader skipping them means our own round-trip could
not have caught an error in them. The native tool could, and did not.

This is why `dump` must keep writing those maps even though `restore` ignores
them. See [`containers.md`](containers.md) §1.

### Boot

A restored, boot-installed root boots to a shell under SIMH.

### Partitions

A whole-disk RP06 built with `mkfs -P a` + `mkfs -P c` keeps each partition
isolated, each `fsck`s clean independently, and `-P a` mounts and reads the real
2.9 root. INI-defined tables work the same way.

---

## 3. The automated suite

`make test` — 27 checks, self-contained and dependency-free. It builds all its
own fixtures, so it needs **no historical data** and runs anywhere.

What it covers: `mkfs`, put/get byte-identity, mkdir/cp/mv/rm, the `@` host
round-trip, `tar c|x` tree round-trip, `dump|restore` tree round-trip, `.tap`
framing, partition isolation and whole-disk sizing, VHD wrap/info/read/unwrap,
little-endian images, 2048-byte blocks, the oversize guard, the SysV superblock
(present with `-F sysv`, absent by default, surviving a read-write round-trip),
`fsdb` inspect and edit, `manifest`/`verify`, `scavenge`, the analysis bundle,
and the multi-mount shell including cross-image `cp`.

Two properties give it its teeth:

- **Every mutating flow ends by asserting `s5fs fsck` prints `clean`.**
- **The round-trips compare bytes, not descriptions.** `tar c | tar x | tar c`
  must produce an identical archive, and `dump | restore | tar c` must match
  that same archive. A writer bug that is self-consistent still fails these if
  it loses information.

Keep it green, and add a check for every new behavior. If the behavior mutates
an image, the check ends `fsck clean`.

---

## 4. The oracle, and why it is not in this repo

The historical 2.9BSD `mkfs`, `restor`, and `fsck` binaries; the `apout`/`apsim`
PDP-11 user-mode emulators; the `mdec` boot blocks; and the original release
trees are what levels 1 and 2 depend on. **None of them are in this repo, and
none of them may be.**

- Some of that material is **copyright-encumbered**. It is usable locally for
  validation and was never committable.
- This repo is clean, original C. Keeping it that way is a deliberate property,
  not an oversight.

Treat the original release trees as **read-only originals**. Do not vendor them,
do not copy fixtures out of them into `tests/`, and do not add a test that
silently depends on one existing.

The material lives in and around the sibling monorepo this work was split out
of (`~/pdp11-bsd29-toolchain`, branch `diskimage-toolkit`), which is also where
a fix should be synced back to if one is made here.

---

## 5. Re-verifying

On any host, with no historical data:

```sh
make && make test          # the whole level-3 suite
make test-san              # the same suite under AddressSanitizer + UBSan
```

`test-san` is not redundant. Several checks feed the tools deliberately hostile
input — a crafted dump tape, a looping free list, a directory cycle — and an
**out-of-bounds read does not fault on a normal build**. The crafted-tape check
passes a plain build even with the overflow present; only the sanitizer turns it
into a failure. `abort_on_error` is set so a sanitizer finding becomes a signal
rather than a quiet exit 1 that a crash check reads as success.

With the oracle available, the two checks worth re-running by hand are the ones
the suite cannot do:

1. **`mkfs` diff** — build an image with `s5fs mkfs`, build the same geometry
   with native `mkfs` under `apout`, and `cmp` them.
2. **`restor` interop** — write a tape with `s5fs dump`, then `restor tf` to
   validate and `restor rf` to rebuild, and compare the rebuilt tree.

Still planned, and never done: a self-hosted `icheck`/`fsck` built with the
cross-toolchain and run under `apsim`, giving a fully third-party oracle for the
checker itself. Today the checker's independence rests on it sharing no code
with the writer ([`fsck.md`](fsck.md) §1), which is strong but is not a third
party.

---

## 6. For a maintainer

- **"fsck clean" is level 3, not proof.** It is necessary and not sufficient;
  when an exact comparison is available, make it.
- **Keep `s5fs_iput` frozen.** Its fidelity to native `mkfs` is a result, and
  general block mapping already has its own function.
- **Keep writing `TS_CLRI`/`TS_BITS`.** They are the part of our output that
  only a native tool can check.
- **Never vendor copyrighted data into this repo**, including as a test
  fixture.
- **The suite must stay self-contained.** A test that needs historical data is a
  test that does not run on a contributor's machine.
- **Round-trips must compare bytes.** A round-trip that compares summaries can
  be passed by a writer that loses information symmetrically.

# Containers: dump tapes, SIMH `.tap`, and fixed VHD

Three wrappers that carry an s5fs filesystem somewhere else: the 2.9BSD
`dump(8)` tape format, the SIMH `.tap` tape framing that wraps it, and the fixed
VHD footer that lets a modern hypervisor or SIMH attach a raw image.

Code: `src/cmd_dump.c`, `src/cmd_restore.c`, `src/cmd_vhd.c`.

---

## 1. The dump tape format

`s5fs dump` writes the real historical format from `dumprestor.h` — not an
approximation of it. The structure of a tape:

```
  TS_TAPE     header
  TS_CLRI     map of inodes to clear
  TS_BITS     map of inodes present in this dump
  TS_INODE    one per allocated inode, followed by its data blocks
   (+ TS_ADDR   continuation records for indirect blocks)
  TS_END
```

Constants:

| name | value |
|---|---|
| `MAGIC` | 60011 |
| `CHECKSUM` | 84446 |
| `TS_TAPE` / `TS_INODE` / `TS_BITS` / `TS_ADDR` / `TS_END` / `TS_CLRI` | 1 / 2 / 3 / 4 / 5 / 6 |
| `NTREC` | 10 blocks per tape record |

The record header (`struct spcl`) carries the type, the dump and prior-dump
dates, a volume number, the inode number, the magic, a checksum, an embedded
copy of the 64-byte dinode at offset 22, and a block count and address map.

### The checksum

Every record carries a 16-bit checksum computed so that the whole record sums
to a fixed constant:

```c
put16(spcl + C_CHECKSUM, 0);                        /* zero the field first */
for (i = 0; i < words; i++)
        s += get16(spcl + 2 * i);
put16(spcl + C_CHECKSUM, (CHECKSUM - s) & 0xffff);  /* 84446 - sum */
```

Zeroing the field before summing is not optional — the checksum covers itself,
so it has to be a known value while the sum is taken. This is the classic shape
of the era's tape checksums, and it is what native `restor(8)` validates.

### Byte order

**The dump inherits the image's byte order.** A PDP-11 image produces a PDP-11
tape; the `spcl` fields and the embedded dinode go out through the same codec
the image was read with. Metadata and data copy through verbatim, so
`dump | restore` is a faithful round-trip and a tape from a PDP-11 image is
readable by a real PDP-11.

### What restore skips

Our own `s5fs restore` does not consume the `TS_CLRI` and `TS_BITS` inode maps —
it rebuilds the filesystem from the `TS_INODE` records directly. But `dump`
still **writes** them correctly, because native `restor(8)` does read them.
That asymmetry is intentional and is exactly what the native-tool validation
proves (see [`validation.md`](validation.md)).

---

## 2. SIMH `.tap` framing

`dump -T` wraps the flat tape in SIMH's `.tap` container so it can be attached
to an emulated TM/TS drive.

The framing: group `NTREC` blocks into one tape record, and bracket each record
with a **4-byte little-endian length, before and after**:

```
  [len][ ... NTREC * BSIZE bytes ... ][len]  [len][ ... ][len]  ...  [0][0]
```

The length appears twice so the tape can be read in either direction, which is
what a real drive supports. Two zero-length records (eight zero bytes) mark
end-of-tape.

The record size is not arbitrary: `NTREC * BSIZE` is exactly what the historical
`dump` wrote in one `write()` and exactly what `restor(8)` asks for with
`read(mt, tbf, NTREC*BSIZE)`. Matching it is what makes the tape mount and read
on an emulated drive rather than merely parse.

The test suite asserts the framed tape is larger than the flat one and ends in
eight zero bytes.

---

## 3. Fixed VHD

A fixed VHD is a raw sector image with a **512-byte footer appended at the end**.
Nothing about the data changes. That placement is the whole reason this is
cheap: because the footer sits *after* the filesystem, every other command in
this toolkit reads a wrapped image transparently — `fsck`, `ls`, and `put` do
not know or care that it is there.

The test suite checks exactly that: `fsck` on a `.vhd` comes back clean.

Footer fields, all **big-endian regardless of the image's own byte order** (the
VHD spec is big-endian; the filesystem inside it is not):

| offset | field |
|---|---|
| 0 | cookie `conectix` |
| 8 | features (reserved bit, `0x00000002`) |
| … | version, data offset, timestamp, creator |
| … | original and current size, CHS geometry |
| … | disk type = fixed |
| 64 | ones-complement checksum |
| 68 | unique id (left zero; SIMH ignores it) |

CHS geometry follows the spec's Appendix algorithm mapping total sectors to
cylinders/heads/sectors. The checksum is the ones' complement of the sum of
every other byte, with the checksum field itself zero during the sum — same
discipline as the dump record checksum.

```sh
s5fs vhd wrap   src [dst]    # append the footer (in place if no dst)
s5fs vhd unwrap src [dst]    # strip it
s5fs vhd info   file         # print the footer fields
```

### The append trap

**Always `lseek(SEEK_END)` before writing the footer.** Writing at the current
offset overwrites the last 512 bytes of the filesystem with the footer and
produces a file that is exactly the right length, carries a valid `conectix`
cookie, and has lost a block of user data. It looks correct to every check that
does not read that specific block.

This bug was introduced once and fixed once. `wrap` also refuses an image that
already has a footer, so double-wrapping cannot silently nest.

**Dynamic and differencing VHD are out of scope.** They are a genuinely
different format — a block allocation table and sparse data blocks — not a
footer, and nothing in this toolkit would be reused implementing them.

---

## 4. For a maintainer

- **Zero the checksum field before summing it**, in both the dump record and the
  VHD footer.
- **The dump inherits the image's byte order.** Don't normalize tapes to one
  order; a PDP-11 tape must be a PDP-11 tape.
- **Keep writing `TS_CLRI`/`TS_BITS`** even though our `restore` ignores them.
  Native `restor(8)` needs them, and that interop is a validation anchor.
- **`NTREC * BSIZE` is the tape record size.** It matches what the era's tools
  read; changing it makes tapes that parse but won't mount.
- **VHD footers append at `SEEK_END`.** Never at the current offset.
- **A wrapped VHD must stay readable by every other subcommand.** If a change
  makes `fsck img.vhd` fail, the change is wrong.

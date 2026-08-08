# Byte order

s5fs has the **same on-disk structure on every machine that ran it**. Only the
encoding of fields wider than one byte follows the CPU. That single observation
is what lets one writer emit images for three different hardware regimes, and
it is why byte order is a pluggable codec here rather than a `#ifdef`.

Structure lives in [`on-disk-format.md`](on-disk-format.md); this document is
encoding only. The code is `src/s5endian.[ch]` — about 120 lines, and nothing
above it may assume an order.

---

## 1. The three regimes

| codec | 16-bit | 32-bit | 3-byte | machines |
|---|---|---|---|---|
| **`S5_PDP11`** | LE | **middle-endian** | PDP `l3` | Research V7, 2.xBSD on the PDP-11 |
| **`S5_LE`** | LE | LE | LE | VAX (3BSD/4.0/4.1), and any little-endian SysV host |
| **`S5_BE`** | BE | BE | BE | big-endian SysV hosts (m68k, the Interdata port) |

`S5_PDP11` is deliberately the zero value, so a zero-initialized `s5fs_opts`
produces a PDP-11 image — the common case — without anyone having to remember
to set it.

Names are parsed leniently, because people describe these machines
inconsistently:

| codec | accepted |
|---|---|
| `S5_PDP11` | `pdp11`, `pdp`, `pdp-11` |
| `S5_LE` | `le`, `little`, `vax`, `x86`, `i386` |
| `S5_BE` | `be`, `big`, `m68k`, `68k`, `mc68000`, `sun` |

---

## 2. The exact encodings

Take `0xAABBCCDD` as a 32-bit field, and a 24-bit block address `v` with
`hi = v>>16`, `mid = v>>8`, `lo = v`:

| codec | 32-bit bytes | 3-byte bytes |
|---|---|---|
| `S5_PDP11` | `BB AA DD CC` | `hi lo mid` |
| `S5_LE` | `DD CC BB AA` | `lo mid hi` |
| `S5_BE` | `AA BB CC DD` | `hi mid lo` |

### Why the PDP-11 32-bit order looks scrambled

The PDP-11 has no 32-bit register. A `long` is **two 16-bit words**, each stored
little-endian, with the **high word first**. So the machine writes
`BB AA` (the high word, LE) then `DD CC` (the low word, LE). This is
"middle-endian" or "word-swapped", and it is neither of the two orders anyone
reaches for by reflex — which is exactly why it has to be a codec and not an
assumption.

### Why the 3-byte order is stranger still

Block addresses are packed into three bytes by the `l3tol`/`ltol3` routines,
and the PDP-11 layout — `hi lo mid` — is not a truncation of *any* of the 32-bit
orders. It is its own convention. The encodings here were derived from the two
shipped `l3tol.c` implementations (PDP-11 and VAX) plus each CPU's long
representation; the big-endian variant matches the historical `#ifdef interdata`
branch of the same file.

The practical warning: **you cannot derive the 3-byte encoding from the 32-bit
one.** Both are tabulated above because both had to be looked up.

---

## 3. The codec interface

```c
typedef struct {
        const char *name;
        void     (*put16)(uint8_t *, uint16_t);
        uint16_t (*get16)(const uint8_t *);
        void     (*put32)(uint8_t *, uint32_t);
        uint32_t (*get32)(const uint8_t *);
        void     (*put24)(uint8_t *, uint32_t);   /* 3-byte block address */
        uint32_t (*get24)(const uint8_t *);
} s5_codec;
```

Six functions, no state. Every layer that touches disk bytes holds a
`const s5_codec *bo` and calls through it: `fs->bo->put16(dp + P11_DI_MODE,
mode)`.

This is what invariant #1 — *never overlay a host struct on disk bytes* —
actually buys. Because no host `struct` is ever laid over the image, the
toolkit is simultaneously byte-order-portable, alignment-safe, and 64-bit-clean,
and none of those three required separate work.

---

## 4. Auto-detection

Writers are told the byte order (`mkfs -a`); readers work it out. `detect_bo`
tries each codec in turn and accepts the first whose superblock decodes to a
*self-consistent* volume:

```
  isize >= 2  &&  isize < fsize  &&  fsize > 0  &&  fsize <= (file size / bsize)
```

The check is strong in practice because the wrong codec scrambles `s_fsize`
into something that almost never lands under the actual file size while also
staying above a plausible `s_isize`. The file's own length is the anchor — that
is the piece an attacker of the heuristic (or an unlucky image) would have to
match by accident.

`-A pdp11|le|be` forces a codec and skips detection, which is what you want for
a truncated image, a fragment, or a partition whose length you are still
working out.

Note the asymmetry: **the block size is not detected, and cannot be.** There is
no on-disk block-size marker in the V7-derived superblock — only the optional
SysV `s_type` records it. Byte order is guessable because it has redundancy to
check against; block size has none.

---

## 5. Adding a host

A new machine is a new codec, not a new code path:

1. Add an enum value in `s5endian.h` before `S5_NENDIAN`.
2. Add the six functions and the table entry in `s5endian.c`.
3. Add its names to `s5_endian_parse`.
4. Possibly add a `device.c` entry if it brings new disk geometry.

Nothing above `s5endian.c` changes. If a change to support a new host requires
touching `fsread.c` or `s5fs_core.c`, the layering has been violated somewhere
and that is the bug to fix first.

---

## 6. For a maintainer

- **`S5_PDP11` must stay 0.** A zero-initialized options struct meaning
  "PDP-11" is relied on.
- **The 3-byte order is independent of the 32-bit order.** Don't derive one
  from the other; use the table in §2.
- **Nothing above the codec may assume an order** — no `htons`, no casting a
  buffer to a `uint32_t *`, no memcpy of a multi-byte field.
- **Detection needs the file length.** Keep the `fsize <= fileblocks` term; it
  is what makes the heuristic reliable.
- **Block size is not detectable.** Don't add a heuristic for it — say `-B`.

/*
 * s5endian.h -- pluggable byte order for s5fs on-disk fields.
 *
 * s5fs has the SAME on-disk structure on every host it ran on; only the
 * encoding of fields wider than one byte follows the CPU.  Three regimes cover
 * the machines that used this filesystem:
 *
 *   S5_PDP11  16-bit LE, 32-bit MIDDLE-endian (word-swapped), PDP l3 3-byte
 *             -- Research V7 / 2.xBSD on the PDP-11
 *   S5_LE     16-bit LE, 32-bit LE,            LE  3-byte
 *             -- VAX (3BSD/4.0/4.1), and any little-endian SysV host (x86, ...)
 *   S5_BE     16-bit BE, 32-bit BE,            BE  3-byte
 *             -- big-endian SysV hosts (m68k, and the historical Interdata port)
 *
 * The 32-bit and 3-byte encodings were derived from the two shipped l3tol.c
 * (PDP-11 vs VAX) plus each CPU's long representation; the big-endian variant
 * matches the historical `#ifdef interdata` branch of the same code.
 *
 * Everything routes through these codecs; no code below s5fs_core/cmd_fsck ever
 * assumes a byte order, so adding a host means (at most) a new device entry.
 */
#ifndef S5ENDIAN_H
#define S5ENDIAN_H

#include <stdint.h>

typedef enum {
	S5_PDP11 = 0,	/* keep 0: the zero-initialised default */
	S5_LE,
	S5_BE,
	S5_NENDIAN
} s5_endian;

typedef struct {
	const char *name;
	void     (*put16)(uint8_t *, uint16_t);
	uint16_t (*get16)(const uint8_t *);
	void     (*put32)(uint8_t *, uint32_t);
	uint32_t (*get32)(const uint8_t *);
	void     (*put24)(uint8_t *, uint32_t);	/* 3-byte packed block address */
	uint32_t (*get24)(const uint8_t *);
} s5_codec;

/* Codec for a regime, or NULL if `e` is out of range. */
const s5_codec *s5_codec_for(s5_endian e);

/* Parse "pdp11"/"vax"/"le"/"be"/"m68k"/... ; returns S5_NENDIAN if unknown. */
s5_endian s5_endian_parse(const char *s);
const char *s5_endian_name(s5_endian e);

#endif /* S5ENDIAN_H */

/*
 * s5endian.c -- the three s5fs byte-order codecs.  See s5endian.h.
 *
 * Reference values (0xAABBCCDD as a 32-bit field; a 24-bit block address as
 * hi=v>>16, mid=v>>8, lo=v):
 *   PDP-11 32-bit -> BB AA DD CC     3-byte -> hi lo mid   (word-swapped long)
 *   LE     32-bit -> DD CC BB AA     3-byte -> lo mid hi
 *   BE     32-bit -> AA BB CC DD     3-byte -> hi mid lo
 * 16-bit is LE for PDP-11 and LE, BE for BE.
 */

#include "s5endian.h"

#include <string.h>
#include <strings.h>

/* ---- little-endian 16-bit (shared by PDP-11 and LE) ---- */
static void
le16_put(uint8_t *b, uint16_t v)
{
	b[0] = (uint8_t)v;
	b[1] = (uint8_t)(v >> 8);
}

static uint16_t
le16_get(const uint8_t *b)
{
	return (uint16_t)(b[0] | (b[1] << 8));
}

/* ---- big-endian 16-bit ---- */
static void
be16_put(uint8_t *b, uint16_t v)
{
	b[0] = (uint8_t)(v >> 8);
	b[1] = (uint8_t)v;
}

static uint16_t
be16_get(const uint8_t *b)
{
	return (uint16_t)((b[0] << 8) | b[1]);
}

/* ---- PDP-11 middle-endian 32-bit + PDP l3 3-byte ---- */
static void
pdp32_put(uint8_t *b, uint32_t v)
{
	b[0] = (uint8_t)(v >> 16);
	b[1] = (uint8_t)(v >> 24); /* high word, LE */
	b[2] = (uint8_t)(v);
	b[3] = (uint8_t)(v >> 8); /* low  word, LE */
}

static uint32_t
pdp32_get(const uint8_t *b)
{
	return ((uint32_t)b[1] << 24) | ((uint32_t)b[0] << 16) |
	       ((uint32_t)b[3] << 8) | (uint32_t)b[2];
}

static void
pdp24_put(uint8_t *b, uint32_t v)
{
	b[0] = (uint8_t)(v >> 16);
	b[1] = (uint8_t)(v);
	b[2] = (uint8_t)(v >> 8);
}

static uint32_t
pdp24_get(const uint8_t *b)
{
	return ((uint32_t)b[0] << 16) | ((uint32_t)b[2] << 8) | (uint32_t)b[1];
}

/* ---- straight little-endian 32-bit + 3-byte ---- */
static void
le32_put(uint8_t *b, uint32_t v)
{
	b[0] = (uint8_t)v;
	b[1] = (uint8_t)(v >> 8);
	b[2] = (uint8_t)(v >> 16);
	b[3] = (uint8_t)(v >> 24);
}

static uint32_t
le32_get(const uint8_t *b)
{
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
	       ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void
le24_put(uint8_t *b, uint32_t v)
{
	b[0] = (uint8_t)v;
	b[1] = (uint8_t)(v >> 8);
	b[2] = (uint8_t)(v >> 16);
}

static uint32_t
le24_get(const uint8_t *b)
{
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16);
}

/* ---- straight big-endian 32-bit + 3-byte ---- */
static void
be32_put(uint8_t *b, uint32_t v)
{
	b[0] = (uint8_t)(v >> 24);
	b[1] = (uint8_t)(v >> 16);
	b[2] = (uint8_t)(v >> 8);
	b[3] = (uint8_t)v;
}

static uint32_t
be32_get(const uint8_t *b)
{
	return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
	       ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

static void
be24_put(uint8_t *b, uint32_t v)
{
	b[0] = (uint8_t)(v >> 16);
	b[1] = (uint8_t)(v >> 8);
	b[2] = (uint8_t)v;
}

static uint32_t
be24_get(const uint8_t *b)
{
	return ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | (uint32_t)b[2];
}

/* clang-format off */	/* codec table: columns pair put/get per width */
static const s5_codec codecs[S5_NENDIAN] = {
	[S5_PDP11] = { "pdp11", le16_put, le16_get, pdp32_put, pdp32_get, pdp24_put, pdp24_get },
	[S5_LE]    = { "le",    le16_put, le16_get, le32_put,  le32_get,  le24_put,  le24_get  },
	[S5_BE]    = { "be",    be16_put, be16_get, be32_put,  be32_get,  be24_put,  be24_get  },
};
/* clang-format on */

const s5_codec *
s5_codec_for(s5_endian e)
{
	if (e < 0 || e >= S5_NENDIAN)
		return NULL;
	return &codecs[e];
}

const char *
s5_endian_name(s5_endian e)
{
	const s5_codec *c = s5_codec_for(e);
	return c ? c->name : "?";
}

s5_endian
s5_endian_parse(const char *s)
{
	if (!s)
		return S5_NENDIAN;
	if (!strcasecmp(s, "pdp11") || !strcasecmp(s, "pdp") || !strcasecmp(s, "pdp-11"))
		return S5_PDP11;
	if (!strcasecmp(s, "le") || !strcasecmp(s, "little") || !strcasecmp(s, "vax") ||
	    !strcasecmp(s, "x86") || !strcasecmp(s, "i386"))
		return S5_LE;
	if (!strcasecmp(s, "be") || !strcasecmp(s, "big") || !strcasecmp(s, "m68k") ||
	    !strcasecmp(s, "68k") || !strcasecmp(s, "mc68000") || !strcasecmp(s, "sun"))
		return S5_BE;
	return S5_NENDIAN;
}

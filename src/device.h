/*
 * device.h -- PDP-11 disk geometry table + an OPTIONAL, advisory-only
 * driver-availability check.
 *
 * The disk image is s5fs and kernel-agnostic: a device selection only fixes
 * the total size, and geometry is hardware (identical across releases).  The
 * per-release "since/until" tags exist solely to print a *gentle* heads-up
 * when the caller volunteers a --target release and picks a disk whose driver
 * wasn't usually in that release's kernels.  It is never an error and never
 * blocks: PDP-11 driver provenance is messy (backports, site ports), so this
 * is a hint, not a rule.  With no --target, nothing is ever warned.
 *
 * Capacities are 512-byte blocks, cross-checked between the 2.9BSD drivers and
 * SIMH's PDP11/pdp11_*.c geometry.
 */
#ifndef DEVICE_H
#define DEVICE_H

#include <stdint.h>

/* s5fs-family releases, ordered oldest->newest.  Advisory only. */
typedef enum {
	REL_NONE = -1,	/* caller gave no target -> never warn */
	REL_V7   = 0,
	REL_28,
	REL_29,
	REL_210
} bsd_rel;

/* One partition of a drive, as the kernel driver's compiled-in *_sizes[] table
 * defines it: a fixed window of the platter.  There is no on-disk label -- the
 * disk must match the kernel, so these mirror sys/GENERIC/ioconf.c.  Overlap is
 * normal (a+b+c tile the disk while g/h alias the whole thing). */
typedef struct {
	char     letter;	/* 'a'..'h'                          */
	uint32_t start;		/* first block, in 512-byte blocks   */
	uint32_t len;		/* length in 512-byte blocks         */
} disk_part;

typedef struct {
	const char *name;	/* --device token, e.g. "rl02"       */
	const char *desc;	/* human description incl. controller */
	uint32_t    blocks;	/* capacity in 512-byte blocks        */
	bsd_rel     since;	/* driver usually present from here.. */
	bsd_rel     until;	/* ..through here (advisory)          */
	const disk_part *parts;	/* partition table, or NULL if none  */
	int         nparts;
} disk_dev;

/* Look up a device by name (case-insensitive); NULL if unknown. */
const disk_dev *device_find(const char *name);

/* Find partition `letter` (a..h) of `dev`; fills *start,*len (512-byte blocks).
 * Returns 0 on success, -1 if the device has no such partition. */
int device_partition(const disk_dev *dev, char letter, uint32_t *start, uint32_t *len);

/* Resolve a device/partition/offset selection to a byte base + length.
 *  - ospec "START[:LEN]" (512-byte blocks) wins if non-NULL;
 *  - else letter!=0 selects that partition of device `devname`;
 *  - else base 0 / len 0 (the whole image).
 * Returns 0 (fills *base_bytes and *len512, len512 0 = to end), or -1 with a
 * message on stderr (unknown device, missing partition, bad -o). */
int device_resolve_part(const char *devname, char letter, const char *ospec,
                        long long *base_bytes, uint32_t *len512);

/* Print the device table to stdout (name==NULL) or one device's partitions. */
void device_list(void);
void device_show_parts(const char *name);

/* Parse "v7" / "2.8" / "2.9" / "2.10" (tolerant of a "bsd" suffix).
 * Returns REL_NONE if unrecognised. */
bsd_rel release_parse(const char *s);
const char *release_name(bsd_rel r);

/* If `target` is a real release and `dev` falls outside its usual driver
 * range, print a soft advisory to stderr.  No-op when target==REL_NONE.
 * Always returns (never exits); returns 1 if it warned, else 0. */
int device_advise(const disk_dev *dev, bsd_rel target);

#endif /* DEVICE_H */

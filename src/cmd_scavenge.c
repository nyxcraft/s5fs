/*
 * cmd_scavenge.c -- `s5fs scavenge`: pick through the free space for remnants
 * of deleted files.
 *
 *   s5fs scavenge [opts] image            report what can be found
 *   s5fs scavenge [opts] -x DIR image     also carve recoverable data into DIR
 *
 * Deliberately NOT called "undelete": traditional Unix rm makes general
 * undelete impossible -- it zeroes the directory entry's inode number and, on
 * the last link, zeroes the inode itself (so the block list is gone) and frees
 * the data blocks.  fsck Phase 3 already rescues inodes still allocated
 * (orphans); scavenge gathers the traces rm leaves behind:
 *
 *   1. deleted names  -- rm clears an entry's inode number but leaves the
 *      14-byte NAME, so directory blocks keep a ghost of every removed name.
 *   2. content by signature -- unreferenced blocks are scanned for file starts
 *      (a.out, ar, tar, text); each is evidence of a deleted file, and -x
 *      carves out what survives.
 *
 * Honest limits: the block list is gone; s5fs interleaves blocks (not
 * contiguous); and the free list is chained THROUGH freed blocks, so ~1 in 50
 * is overwritten with links.  So a single-block file (or a deleted a.out start)
 * comes back exact, multi-block text recovers block-by-block, and a large
 * binary yields only its head -- never a guaranteed whole-file undelete.
 */

#define _POSIX_C_SOURCE 200809L

#include "fsread.h"
#include "device.h"
#include "cmds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static FSR      R;
static uint8_t *used;			/* blocks referenced by a live inode */
static uint32_t nino;

enum { SIG_AOUT, SIG_AR, SIG_TAR, SIG_TEXT, SIG_DIR, SIG_ZERO, SIG_OTHER, SIG_N };
static const char *signame[SIG_N] = { "a.out", "ar", "tar", "text", "dir", "zero", "other" };

static void mark_ind(uint32_t bno, int level)
{
	uint8_t buf[P11_MAXBSIZE];
	uint32_t i;
	if (bno == 0 || bno >= R.fsize) return;
	used[bno] = 1;
	if (level == 0 || fsr_bread(&R, bno, buf) < 0) return;
	for (i = 0; i < R.nindir; i++)
		mark_ind(R.bo->get32(buf + 4 * i), level - 1);
}

/* mark every block that is filesystem metadata (a live inode's data/indirect
 * blocks, or a free-list chain block) -- what's left over is candidate data */
static void build_used(void)
{
	uint32_t ino, i;
	uint8_t sb[P11_MAXBSIZE];
	int32_t nfree, fr[P11_NICFREE];
	used = calloc(R.fsize, 1);
	nino = (R.isize - 2) * R.inopb;
	if (!used) { fprintf(stderr, "s5fs scavenge: out of memory\n"); exit(1); }
	for (ino = 1; ino <= nino; ino++) {
		fsr_inode in;
		if (fsr_iget(&R, ino, &in) < 0) break;
		if ((in.mode & P11_IFMT) != P11_IFREG && (in.mode & P11_IFMT) != P11_IFDIR)
			continue;
		for (i = 0; i < R.laddr; i++)
			if (in.addr[i] > 0 && (uint32_t)in.addr[i] < R.fsize) used[in.addr[i]] = 1;
		mark_ind((uint32_t)in.addr[R.laddr], 1);
		mark_ind((uint32_t)in.addr[R.laddr + 1], 2);
		mark_ind((uint32_t)in.addr[R.laddr + 2], 3);
	}
	/* Follow the chained free list and mark the blocks that HOLD the chain --
	 * s5fs keeps the list inside freed data blocks, so those are overwritten
	 * metadata, not recoverable data.  (The other free blocks stay candidates.) */
	if (fsr_bread(&R, P11_SUPERBLK, sb) < 0) return;
	nfree = (int16_t)R.bo->get16(sb + P11_SB_NFREE);
	for (i = 0; i < P11_NICFREE; i++)
		fr[i] = (int32_t)R.bo->get32(sb + P11_SB_FREE + 4 * i);
	for (;;) {
		int32_t bno;
		if (nfree <= 0) break;
		bno = fr[--nfree];
		if (bno == 0 || (uint32_t)bno < R.isize || (uint32_t)bno >= R.fsize) break;
		if (nfree == 0) {			/* this block chains the list */
			uint8_t fb[P11_MAXBSIZE];
			uint32_t k;
			used[bno] = 1;
			if (fsr_bread(&R, (uint32_t)bno, fb) < 0) break;
			nfree = (int16_t)R.bo->get16(fb + P11_FB_NFREE);
			for (k = 0; k < P11_NICFREE; k++)
				fr[k] = (int32_t)R.bo->get32(fb + P11_FB_FREE + 4 * k);
		}
	}
}

/* ---- deleted directory names ---- */
static int printable_name(const uint8_t *p)
{
	int i;
	if (p[0] == 0) return 0;
	for (i = 0; i < P11_DIRSIZ && p[i]; i++)
		if (p[i] < 32 || p[i] > 126) return 0;
	return 1;
}

static uint32_t deleted_names(void)
{
	uint32_t ino, found = 0;
	for (ino = 1; ino <= nino; ino++) {
		fsr_inode in;
		uint32_t nblk, b;
		uint8_t buf[P11_MAXBSIZE];
		if (fsr_iget(&R, ino, &in) < 0) break;
		if ((in.mode & P11_IFMT) != P11_IFDIR) continue;
		nblk = ((uint32_t)in.size + R.bsize - 1) / R.bsize;
		for (b = 0; b < nblk; b++) {
			uint32_t e;
			if (fsr_readfile(&R, &in, buf, R.bsize, (long)b * R.bsize) <= 0) continue;
			for (e = 0; e < R.ndirect; e++) {
				uint8_t *d = buf + e * P11_DIRENTSZ;
				char nm[P11_DIRSIZ + 1];
				if (R.bo->get16(d) != 0) continue;	/* live entry */
				if (!printable_name(d + 2)) continue;	/* never-used slot */
				memcpy(nm, d + 2, P11_DIRSIZ); nm[P11_DIRSIZ] = '\0';
				if (found == 0) printf("Deleted names (inode number cleared, name survived):\n");
				printf("  '%s'  in directory inode %u\n", nm, ino);
				found++;
			}
		}
	}
	if (!found) printf("Deleted names: none found.\n");
	return found;
}

/* ---- classify an unreferenced block by its leading bytes ---- */
static int classify(const uint8_t *b)
{
	uint32_t i, pr = 0, zero = 0;
	uint16_t w0 = R.bo->get16(b);
	if (w0 == 0407 || w0 == 0410 || w0 == 0411) return SIG_AOUT;
	if (w0 == 0177545) return SIG_AR;			/* PDP-11 ar magic */
	if (R.bsize >= 512 && memcmp(b + 257, "ustar", 5) == 0) return SIG_TAR;
	if (R.bo->get16(b + 2) == 0 && b[2 + 2] == '.' && b[P11_DIRENTSZ + 2 + 2] == '.') {
		/* slot0 name ".", slot1 name ".." -> a freed directory block */
		if (b[2 + 3] == 0) return SIG_DIR;
	}
	{
		uint32_t np = 0;			/* non-printable, non-zero bytes */
		for (i = 0; i < R.bsize; i++) {
			if (b[i] == 0) zero++;
			else if ((b[i] >= 32 && b[i] < 127) || b[i] == 9 || b[i] == 10 || b[i] == 13) pr++;
			else np++;
		}
		if (zero == R.bsize) return SIG_ZERO;
		/* text if the non-zero content is essentially all printable (trailing
		 * zero padding of a short file is ignored) and there's real text */
		if (pr >= 16 && np * 20 < pr) return SIG_TEXT;
	}
	return SIG_OTHER;
}

/* carve `len` bytes from consecutive physical blocks (contiguous assumption) */
static long carve(uint32_t bno, long len, const char *outdir, const char *base)
{
	char path[1200];
	uint8_t b[P11_MAXBSIZE];
	FILE *f;
	long done = 0;
	snprintf(path, sizeof path, "%s/%s-blk%u", outdir, base, bno);
	f = fopen(path, "wb");
	if (!f) { fprintf(stderr, "s5fs scavenge: %s: cannot create\n", path); return -1; }
	while (done < len) {
		uint32_t cur = bno + (uint32_t)(done / R.bsize);
		long want = len - done; if (want > (long)R.bsize) want = R.bsize;
		if (cur >= R.fsize || fsr_bread(&R, cur, b) < 0) break;
		if (fwrite(b, 1, want, f) != (size_t)want) break;
		done += want;
	}
	fclose(f);
	printf("  carved %s  (%ld bytes from block %u)\n", path, done, bno);
	return done;
}

int cmd_scavenge(int argc, char **argv)
{
	uint32_t bsize = 0, plen = 0, bno, tally[SIG_N];
	int forced = -1, c, i;
	const char *dev = NULL, *ospec = NULL, *outdir = NULL;
	char part = 0;
	long long base = 0;

	while ((c = getopt(argc, argv, "B:A:d:P:o:x:")) != -1) {
		switch (c) {
		case 'B': bsize = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'A': { s5_endian e = s5_endian_parse(optarg);
		            if (e == S5_NENDIAN) { fprintf(stderr, "scavenge: bad -A\n"); return 2; }
		            forced = (int)e; break; }
		case 'd': dev = optarg; break;
		case 'P': part = optarg[0]; break;
		case 'o': ospec = optarg; break;
		case 'x': outdir = optarg; break;
		default: fprintf(stderr, "usage: s5fs scavenge [-B ..] [-A ..] [-d dev -P part | -o blk] [-x DIR] image\n"); return 2;
		}
	}
	if (device_resolve_part(dev, part, ospec, &base, &plen) < 0) return 2;
	if (optind != argc - 1) { fprintf(stderr, "usage: s5fs scavenge [...] [-x DIR] image\n"); return 2; }
	if (fsr_open(&R, argv[optind], bsize, forced, base) < 0) {
		fprintf(stderr, "s5fs scavenge: %s: not a readable s5fs image (try -B/-A)\n", argv[optind]);
		return 1;
	}
	if (outdir && mkdir(outdir, 0755) < 0 && access(outdir, W_OK) < 0) {
		fprintf(stderr, "s5fs scavenge: %s: cannot use output dir\n", outdir);
		fsr_close(&R); return 1;
	}

	build_used();
	deleted_names();

	for (i = 0; i < SIG_N; i++) tally[i] = 0;
	printf("\nUnreferenced-block content scan:\n");
	for (bno = R.isize; bno < R.fsize; bno++) {
		uint8_t b[P11_MAXBSIZE];
		int sig;
		if (used[bno]) continue;
		if (fsr_bread(&R, bno, b) < 0) continue;
		sig = classify(b);
		tally[sig]++;
		if (sig == SIG_AOUT) {
			long len = 16 + R.bo->get16(b + 2) + R.bo->get16(b + 4) + R.bo->get16(b + 8);
			printf("  block %u: a.out (magic 0%o, text=%u data=%u syms=%u -> ~%ld bytes)\n",
			       bno, R.bo->get16(b), R.bo->get16(b + 2), R.bo->get16(b + 4), R.bo->get16(b + 8), len);
			if (outdir) carve(bno, len, outdir, "aout");
		} else if (sig == SIG_AR) {
			printf("  block %u: ar archive (extract with `ar` once recovered)\n", bno);
		} else if (sig == SIG_TAR) {
			printf("  block %u: tar archive\n", bno);
			if (outdir) carve(bno, (long)(R.fsize - bno) * R.bsize > 65536 ? 65536 : 1024, outdir, "tar");
		}
	}
	printf("  totals:");
	for (i = 0; i < SIG_N; i++) if (tally[i]) printf("  %s=%u", signame[i], tally[i]);
	printf("\n");

	if (outdir) {
		uint32_t nt = 0;
		printf("\nCarving text runs into %s (each block that reads as text)...\n", outdir);
		for (bno = R.isize; bno < R.fsize; bno++) {
			uint8_t b[P11_MAXBSIZE];
			if (used[bno]) continue;
			if (fsr_bread(&R, bno, b) < 0) continue;
			if (classify(b) == SIG_TEXT) { carve(bno, R.bsize, outdir, "text"); if (++nt >= 200) { printf("  (stopping at 200 text blocks)\n"); break; } }
		}
		printf("\nNote: recovery is inherently partial on s5fs.  The inode (block\n"
		       "list) is gone, blocks are rotationally interleaved (not contiguous),\n"
		       "and the free list is chained THROUGH freed blocks -- so roughly one\n"
		       "freed block in fifty is overwritten with free-list links.  Single-block\n"
		       "files usually survive intact; multi-block carves are a best-effort\n"
		       "contiguous guess (compare against the reported header size).\n");
	} else {
		printf("\n(run with `-x DIR` to carve a.out/tar/text starts into DIR)\n");
	}

	fsr_close(&R);
	return 0;
}

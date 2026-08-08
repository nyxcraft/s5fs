/*
 * cmd_analysis.c -- classic read-mostly analysis tools over an s5fs image:
 *
 *   s5fs ncheck [-s] [-i N,N] image     inode -> path (-s: setuid/setgid/dev audit)
 *   s5fs quot   image                   blocks + files per owner
 *   s5fs du     [-a] image [path]       disk usage per directory subtree
 *   s5fs df     image                   block + inode usage summary
 *   s5fs labelit image [name [pack]]    read/set the volume label
 *
 * All but labelit are read-only; they reuse the fsread reader.  Block counts
 * are in filesystem blocks (the image's -B size).
 */

#define _POSIX_C_SOURCE 200809L

#include "fsread.h"
#include "fsutil.h"
#include "device.h"
#include "cmds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

/* leading -B/-A + -d/-P/-o -> open FSR; returns index of first non-option, -1 err */
static int
a_open(int argc, char **argv, FSR *r, const char *usage)
{
	uint32_t bsize = 0, plen = 0;
	int forced = -1, c;
	const char *dev = NULL, *ospec = NULL;
	char part = 0;
	long long base = 0;
	optind = 1;
	while ((c = getopt(argc, argv, "B:A:d:P:o:sai:")) != -1) {
		switch (c) {
		case 'B':
			bsize = (uint32_t)strtoul(optarg, NULL, 0);
			break;
		case 'A': {
			s5_endian e = s5_endian_parse(optarg);
			if (e == S5_NENDIAN) {
				fprintf(stderr, "%s: bad -A\n", argv[0]);
				return -1;
			}
			forced = (int)e;
			break;
		}
		case 'd':
			dev = optarg;
			break;
		case 'P':
			part = optarg[0];
			break;
		case 'o':
			ospec = optarg;
			break;
		case 's':
		case 'a':
		case 'i':
			break; /* consumed by the caller below */
		default:
			fprintf(stderr, "%s\n", usage);
			return -1;
		}
	}
	if (device_resolve_part(dev, part, ospec, &base, &plen) < 0)
		return -1;
	if (optind >= argc) {
		fprintf(stderr, "%s\n", usage);
		return -1;
	}
	if (fsr_open(r, argv[optind], bsize, forced, base) < 0) {
		fprintf(stderr, "%s: %s: not a readable s5fs image (try -B/-A)\n", argv[0], argv[optind]);
		return -1;
	}
	return optind;
}

/* ================= ncheck ================= */
struct nctx {
	FSR *r;
	int sflag;
	const char *ilist;
};

static int
wanted_inode(const char *list, uint32_t ino)
{
	const char *p = list;
	char buf[16];
	while (*p) {
		int n = 0;
		while (*p && *p != ',') {
			if (n < 15)
				buf[n++] = *p;
			p++;
		}
		buf[n] = '\0';
		if ((uint32_t)strtoul(buf, NULL, 0) == ino)
			return 1;
		if (*p == ',')
			p++;
	}
	return 0;
}

/* One-shot walk guard.  A directory cycle is representable on disk (a corrupt
 * image, or fsck -p reconnecting an orphaned loop), and without this the
 * recursion below runs until the stack is exhausted. */
static fsr_walkset g_ws;

static void
ncheck_walk(struct nctx *c, uint32_t ino, const char *path)
{
	fsr_inode in;
	uint8_t buf[P11_MAXBSIZE];
	uint32_t nblk, b;
	int show = 1;
	if (fsr_iget(c->r, ino, &in) < 0)
		return;
	if (c->sflag)
		show = (in.mode & 04000) || (in.mode & 02000) ||
		       (in.mode & P11_IFMT) == P11_IFCHR || (in.mode & P11_IFMT) == P11_IFBLK;
	if (c->ilist)
		show = wanted_inode(c->ilist, ino);
	if (show && ino != P11_ROOTINO) {
		char m[11];
		fs_modestr(in.mode, m);
		printf("%5u  %s  %s\n", ino, m, path);
	}
	if ((in.mode & P11_IFMT) != P11_IFDIR)
		return;
	nblk = ((uint32_t)in.size + c->r->bsize - 1) / c->r->bsize;
	for (b = 0; b < nblk; b++) {
		uint32_t e;
		long got = fsr_readfile(c->r, &in, buf, c->r->bsize, (long)b * c->r->bsize);
		if (got <= 0)
			continue;
		for (e = 0; e * P11_DIRENTSZ < (uint32_t)got; e++) {
			uint8_t *d = buf + e * P11_DIRENTSZ;
			uint32_t di = c->r->bo->get16(d);
			char nm[P11_DIRSIZ + 1], child[1200];
			if (di == 0)
				continue;
			memcpy(nm, d + 2, P11_DIRSIZ);
			nm[P11_DIRSIZ] = '\0';
			if (!strcmp(nm, ".") || !strcmp(nm, ".."))
				continue;
			snprintf(child, sizeof child, "%s%s%s", path, strcmp(path, "/") ? "/" : "", nm);
			if (fsr_walk_enter(&g_ws, di))
				ncheck_walk(c, di, child);
		}
	}
}

int
cmd_ncheck(int argc, char **argv)
{
	FSR r;
	struct nctx c;
	int idx, i;
	c.sflag = 0;
	c.ilist = NULL;
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-s"))
			c.sflag = 1;
		else if (!strcmp(argv[i], "-i") && i + 1 < argc)
			c.ilist = argv[++i];
	}
	idx = a_open(argc, argv, &r, "usage: s5fs ncheck [-s] [-i N,N] [-B ..] [-A ..] [-d dev -P part] image");
	if (idx < 0)
		return 2;
	c.r = &r;
	if (c.sflag)
		printf("setuid / setgid / device files:\n");
	if (fsr_walkset_init(&g_ws, &r) < 0) {
		fprintf(stderr, "ncheck: out of memory\n");
		fsr_close(&r);
		return 1;
	}
	ncheck_walk(&c, P11_ROOTINO, "/");
	fsr_walkset_free(&g_ws);
	fsr_close(&r);
	return 0;
}

/* ================= quot ================= */
struct qrow {
	int uid;
	long kb, nf;
};

int
cmd_quot(int argc, char **argv)
{
	FSR r;
	int idx;
	uint32_t ino, nino;
	long *kb, *nf;
	int u;
	struct qrow *rows;
	int nr = 0;
	idx = a_open(argc, argv, &r, "usage: s5fs quot [-B ..] [-A ..] [-d dev -P part] image");
	if (idx < 0)
		return 2;
	kb = calloc(65536, sizeof *kb);
	nf = calloc(65536, sizeof *nf);
	rows = malloc(65536 * sizeof *rows);
	if (!kb || !nf || !rows) {
		fprintf(stderr, "quot: out of memory\n");
		return 1;
	}
	nino = (r.isize - 2) * r.inopb;
	for (ino = 1; ino <= nino; ino++) {
		fsr_inode in;
		int t;
		if (fsr_iget(&r, ino, &in) < 0)
			break;
		if (in.mode == 0)
			continue;
		t = in.mode & P11_IFMT;
		if (t != P11_IFREG && t != P11_IFDIR)
			continue;
		kb[(uint16_t)in.uid] += ((long)in.size + 1023) / 1024;
		nf[(uint16_t)in.uid]++;
	}
	for (u = 0; u < 65536; u++)
		if (nf[u]) {
			rows[nr].uid = u;
			rows[nr].kb = kb[u];
			rows[nr].nf = nf[u];
			nr++;
		}
	/* simple insertion sort by kb desc (few distinct uids) */
	{
		int a, b;
		for (a = 1; a < nr; a++) {
			struct qrow tmp = rows[a];
			b = a - 1;
			while (b >= 0 && rows[b].kb < tmp.kb) {
				rows[b + 1] = rows[b];
				b--;
			}
			rows[b + 1] = tmp;
		}
	}
	printf("%8s %8s  %s\n", "kbytes", "files", "uid");
	for (u = 0; u < nr; u++)
		printf("%8ld %8ld  %d\n", rows[u].kb, rows[u].nf, rows[u].uid);
	fsr_close(&r);
	return 0;
}

/* ================= du ================= */
static int du_all;

static long
du_walk(FSR *r, uint32_t ino, const char *path)
{
	fsr_inode in;
	uint8_t buf[P11_MAXBSIZE];
	uint32_t nblk, b;
	long total;
	if (fsr_iget(r, ino, &in) < 0)
		return 0;
	total = ((uint32_t)in.size + r->bsize - 1) / r->bsize; /* this node's own blocks */
	if ((in.mode & P11_IFMT) != P11_IFDIR) {
		if (du_all)
			printf("%8ld  %s\n", total, path);
		return total;
	}
	nblk = ((uint32_t)in.size + r->bsize - 1) / r->bsize;
	for (b = 0; b < nblk; b++) {
		uint32_t e;
		long got = fsr_readfile(r, &in, buf, r->bsize, (long)b * r->bsize);
		if (got <= 0)
			continue;
		for (e = 0; e * P11_DIRENTSZ < (uint32_t)got; e++) {
			uint8_t *d = buf + e * P11_DIRENTSZ;
			uint32_t di = r->bo->get16(d);
			char nm[P11_DIRSIZ + 1], child[1200];
			if (di == 0)
				continue;
			memcpy(nm, d + 2, P11_DIRSIZ);
			nm[P11_DIRSIZ] = '\0';
			if (!strcmp(nm, ".") || !strcmp(nm, ".."))
				continue;
			snprintf(child, sizeof child, "%s%s%s", path, strcmp(path, "/") ? "/" : "", nm);
			if (fsr_walk_enter(&g_ws, di))
				total += du_walk(r, di, child);
		}
	}
	printf("%8ld  %s\n", total, path); /* du prints dirs, post-order */
	return total;
}

int
cmd_du(int argc, char **argv)
{
	FSR r;
	int idx;
	const char *path = "/";
	uint32_t ino;
	int i;
	du_all = 0;
	for (i = 1; i < argc; i++)
		if (!strcmp(argv[i], "-a"))
			du_all = 1;
	idx = a_open(argc, argv, &r, "usage: s5fs du [-a] [-B ..] [-A ..] [-d dev -P part] image [path]");
	if (idx < 0)
		return 2;
	if (idx + 1 < argc && argv[idx + 1][0] == '/')
		path = argv[idx + 1];
	ino = fsr_namei(&r, path);
	if (!ino) {
		fprintf(stderr, "du: %s: not found\n", path);
		fsr_close(&r);
		return 1;
	}
	if (fsr_walkset_init(&g_ws, &r) < 0) {
		fprintf(stderr, "du: out of memory\n");
		fsr_close(&r);
		return 1;
	}
	du_walk(&r, ino, path);
	fsr_walkset_free(&g_ws);
	fsr_close(&r);
	return 0;
}

/* ================= df ================= */
int
cmd_df(int argc, char **argv)
{
	FSR r;
	int idx;
	uint8_t sb[P11_MAXBSIZE];
	uint32_t ino, nino, iused = 0, dtot, dfree = 0, i;
	int32_t nfree, fr[P11_NICFREE];
	idx = a_open(argc, argv, &r, "usage: s5fs df [-B ..] [-A ..] [-d dev -P part] image");
	if (idx < 0)
		return 2;
	if (fsr_bread(&r, P11_SUPERBLK, sb) < 0) {
		fprintf(stderr, "df: read error\n");
		return 1;
	}
	dtot = r.fsize - r.isize; /* data blocks */
	nino = (r.isize - 2) * r.inopb;
	for (ino = 1; ino <= nino; ino++) {
		fsr_inode in;
		if (fsr_iget(&r, ino, &in) < 0)
			break;
		if (in.mode != 0)
			iused++;
	}
	nfree = (int16_t)r.bo->get16(sb + P11_SB_NFREE); /* walk the free list */
	nfree = P11_CLAMP_NFREE(nfree);
	for (i = 0; i < P11_NICFREE; i++)
		fr[i] = (int32_t)r.bo->get32(sb + P11_SB_FREE + 4 * i);
	for (;;) {
		int32_t bno;
		if (nfree <= 0)
			break;
		/* A corrupt chain can point back at itself; the list cannot
		 * legitimately hold more entries than the volume has blocks. */
		if (dfree >= dtot) { /* a chain that points back at itself */
			fprintf(stderr, "df: free list loops; free count is unreliable\n");
			dfree = dtot; /* clamp: `used` is unsigned and would wrap */
			break;
		}
		bno = fr[--nfree];
		if (bno == 0 || (uint32_t)bno < r.isize || (uint32_t)bno >= r.fsize)
			break;
		dfree++;
		if (nfree == 0) {
			uint8_t fb[P11_MAXBSIZE];
			uint32_t k;
			if (fsr_bread(&r, (uint32_t)bno, fb) < 0)
				break;
			nfree = (int16_t)r.bo->get16(fb + P11_FB_NFREE);
			nfree = P11_CLAMP_NFREE(nfree);
			for (k = 0; k < P11_NICFREE; k++)
				fr[k] = (int32_t)r.bo->get32(fb + P11_FB_FREE + 4 * k);
		}
	}
	printf("%-14s %9s %9s %9s %5s  %9s %8s %8s\n",
	       "filesystem", "blocks", "used", "free", "cap", "inodes", "iused", "ifree");
	printf("%-14s %9u %9u %9u %4u%%  %9u %8u %8u\n",
	       argv[idx], dtot, dtot - dfree, dfree,
	       dtot ? (dtot - dfree) * 100 / dtot : 0,
	       nino, iused, nino - iused);
	printf("(%u-byte blocks)\n", r.bsize);
	fsr_close(&r);
	return 0;
}

/* ================= labelit ================= */
int
cmd_labelit(int argc, char **argv)
{
	FSR r;
	int idx;
	uint8_t sb[P11_MAXBSIZE];
	const s5_codec *bo;
	uint32_t bsz;
	long long base;
	off_t sboff;
	int fd, sysv;
	const char *name = NULL, *pack = NULL;

	idx = a_open(argc, argv, &r, "usage: s5fs labelit [-B ..] [-A ..] [-d dev -P part] image [name [pack]]");
	if (idx < 0)
		return 2;
	bo = r.bo;
	bsz = r.bsize;
	base = r.base;
	sboff = (off_t)base + (off_t)P11_SUPERBLK * bsz;
	if (idx + 1 < argc)
		name = argv[idx + 1];
	if (idx + 2 < argc)
		pack = argv[idx + 2];
	fsr_close(&r);

	fd = open(argv[idx], name ? O_RDWR : O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "labelit: %s: cannot open\n", argv[idx]);
		return 1;
	}
	if (lseek(fd, sboff, SEEK_SET) < 0 || read(fd, sb, bsz) != (ssize_t)bsz) {
		fprintf(stderr, "labelit: read error\n");
		close(fd);
		return 1;
	}
	sysv = (bo->get32(sb + P11_SB_MAGIC) == (uint32_t)P11_FS_MAGIC);

	if (!name) { /* read */
		char b1[16], b2[16];
		if (sysv) {
			memcpy(b1, sb + P11_SB_SVFNAME, 6);
			b1[6] = '\0';
			memcpy(b2, sb + P11_SB_SVFPACK, 6);
			b2[6] = '\0';
			printf("fsname '%s'  pack '%s'  (System V labels)\n", b1, b2);
		}
		else {
			memcpy(b1, sb + P11_SB_FSMNT, 12);
			b1[12] = '\0';
			printf("fsname '%s'  (V7 s_fsmnt)\n", b1);
		}
		close(fd);
		return 0;
	}
	/* write */
	if (sysv) {
		memset(sb + P11_SB_SVFNAME, 0, 6);
		memcpy(sb + P11_SB_SVFNAME, name, strlen(name) > 6 ? 6 : strlen(name));
		if (pack) {
			memset(sb + P11_SB_SVFPACK, 0, 6);
			memcpy(sb + P11_SB_SVFPACK, pack, strlen(pack) > 6 ? 6 : strlen(pack));
		}
	}
	else {
		memset(sb + P11_SB_FSMNT, 0, 12);
		memcpy(sb + P11_SB_FSMNT, name, strlen(name) > 12 ? 12 : strlen(name));
		if (pack)
			fprintf(stderr, "labelit: note: V7 superblock has one name field; pack ignored\n");
	}
	if (lseek(fd, sboff, SEEK_SET) < 0 || write(fd, sb, bsz) != (ssize_t)bsz) {
		fprintf(stderr, "labelit: write error\n");
		close(fd);
		return 1;
	}
	close(fd);
	printf("%s: label set to '%s'%s%s\n", argv[idx], name, pack ? " pack " : "", pack ? pack : "");
	return 0;
}

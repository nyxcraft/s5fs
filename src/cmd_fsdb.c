/*
 * cmd_fsdb.c -- an interactive s5fs debugger, the classic `fsdb`.
 *
 *   s5fs fsdb [-w] [-B 512|1024|2048] [-A pdp11|le|be] [-d dev -P part | -o blk] image
 *
 * Where the shell works in the *namespace* (paths), fsdb works at the raw
 * inode/block level -- inspect any inode, dump any block, follow the block map,
 * and (with -w) patch inode fields or poke raw bytes.  It's the forensic /
 * repair / learn-the-format tool.  Read-only unless -w.
 *
 * Commands (numbers are decimal unless noted):
 *   sb                     superblock summary
 *   inode N | i N          decode inode N
 *   dir N                  list directory inode N's entries (slot / inode / name)
 *   map N                  logical->physical block map of inode N
 *   block N | b N          hexdump filesystem block N
 *   cat N                  write inode N's file contents to stdout
 *   path /a/b              resolve a path to an inode number
 *   links N                count directory references to inode N
 *   set N field value      (-w) patch mode|nlink|uid|gid|size of inode N
 *   poke BLK OFF HH..      (-w) write raw hex bytes at block BLK, offset OFF
 *   help ; quit | exit
 */

#define _POSIX_C_SOURCE 200809L

#include "s5fs_rw.h"
#include "fsutil.h"
#include "device.h"
#include "cmds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static RW H;

static const char *typestr(uint16_t mode)
{
	switch (mode & P11_IFMT) {
	case P11_IFDIR: return "directory";
	case P11_IFREG: return "regular file";
	case P11_IFCHR: return "character device";
	case P11_IFBLK: return "block device";
	default:        return "unknown/free";
	}
}

/* read-only logical->physical map (mirrors fsread's bmap; no allocation) */
static uint32_t fdb_bmap(const int32_t *addr, uint32_t lbn)
{
	uint8_t buf[P11_MAXBSIZE];
	uint32_t per = H.r.nindir, phys;
	if (lbn < H.r.laddr)
		return (uint32_t)addr[lbn];
	lbn -= H.r.laddr;
	if (lbn < per) {
		phys = (uint32_t)addr[H.r.laddr];
		if (phys && fsr_bread(&H.r, phys, buf) == 0) phys = H.r.bo->get32(buf + 4 * lbn); else phys = 0;
	} else if (lbn < per * per) {
		lbn -= per;
		phys = (uint32_t)addr[H.r.laddr + 1];
		if (phys && fsr_bread(&H.r, phys, buf) == 0) phys = H.r.bo->get32(buf + 4 * (lbn / per)); else phys = 0;
		if (phys && fsr_bread(&H.r, phys, buf) == 0) phys = H.r.bo->get32(buf + 4 * (lbn % per)); else phys = 0;
	} else {
		lbn -= per * per;
		phys = (uint32_t)addr[H.r.laddr + 2];
		if (phys && fsr_bread(&H.r, phys, buf) == 0) phys = H.r.bo->get32(buf + 4 * (lbn / (per * per))); else phys = 0;
		if (phys && fsr_bread(&H.r, phys, buf) == 0) phys = H.r.bo->get32(buf + 4 * ((lbn / per) % per)); else phys = 0;
		if (phys && fsr_bread(&H.r, phys, buf) == 0) phys = H.r.bo->get32(buf + 4 * (lbn % per)); else phys = 0;
	}
	return phys;
}

static void do_sb(void)
{
	uint8_t sb[P11_MAXBSIZE];
	const s5_codec *bo = H.r.bo;
	uint32_t isize, fsize;
	if (fsr_bread(&H.r, P11_SUPERBLK, sb) < 0) { printf("cannot read superblock\n"); return; }
	isize = bo->get16(sb + P11_SB_ISIZE);
	fsize = bo->get32(sb + P11_SB_FSIZE);
	printf("byte order : %s        block size : %u\n", bo->name, H.r.bsize);
	printf("s_isize    : %u  (first data block; i-list = blocks 2..%u = %u inodes)\n",
	       isize, isize - 1, (isize - 2) * H.r.inopb);
	printf("s_fsize    : %u  (%.1f MB)\n", fsize, fsize * (double)H.r.bsize / 1048576.0);
	printf("s_nfree    : %d      s_ninode : %d\n",
	       (int16_t)bo->get16(sb + P11_SB_NFREE), (int16_t)bo->get16(sb + P11_SB_NINODE));
	if (bo->get32(sb + P11_SB_MAGIC) == (uint32_t)P11_FS_MAGIC)
		printf("s_magic    : fd187e20 (System V)  s_type=%u  tfree=%d  tinode=%d\n",
		       bo->get32(sb + P11_SB_TYPE), (int32_t)bo->get32(sb + P11_SB_SVTFREE),
		       (int16_t)bo->get16(sb + P11_SB_SVTINODE));
	else
		printf("s_tfree    : %d      s_tinode : %d      m/n : %d/%d\n",
		       (int32_t)bo->get32(sb + P11_SB_TFREE), (int16_t)bo->get16(sb + P11_SB_TINODE),
		       (int16_t)bo->get16(sb + P11_SB_DINFO), (int16_t)bo->get16(sb + P11_SB_DINFO + 2));
}

static void do_inode(uint32_t ino)
{
	fsr_inode in;
	char m[11];
	time_t t;
	uint32_t i;
	if (fsr_iget(&H.r, ino, &in) < 0) { printf("inode %u: out of range\n", ino); return; }
	fs_modestr(in.mode, m);
	printf("inode %u  (i-list block %u)\n", ino, (ino + 2 * H.r.inopb - 1) / H.r.inopb);
	printf("  type   %s\n", typestr(in.mode));
	printf("  mode   %s (0%o)\n", m, in.mode & 07777);
	printf("  nlink  %d    uid %d    gid %d\n", (uint16_t)in.nlink, (uint16_t)in.uid, (uint16_t)in.gid);
	printf("  size   %d bytes\n", in.size);
	t = in.mtime; printf("  mtime  %s", ctime(&t));
	t = in.atime; printf("  atime  %s", ctime(&t));
	t = in.ctime; printf("  ctime  %s", ctime(&t));
	if ((in.mode & P11_IFMT) == P11_IFCHR || (in.mode & P11_IFMT) == P11_IFBLK) {
		printf("  device major %u minor %u  (addr[0]=0%o)\n",
		       (in.addr[0] >> 8) & 0377, in.addr[0] & 0377, (unsigned)in.addr[0]);
		return;
	}
	printf("  direct:");
	for (i = 0; i < H.r.laddr; i++) printf(" %d", in.addr[i]);
	printf("\n  single/double/triple indirect: %d %d %d\n",
	       in.addr[H.r.laddr], in.addr[H.r.laddr + 1], in.addr[H.r.laddr + 2]);
}

static void do_dir(uint32_t ino)
{
	fsr_inode in;
	uint8_t buf[P11_MAXBSIZE];
	uint32_t nblk, b, e, used = 0, freecnt = 0;
	if (fsr_iget(&H.r, ino, &in) < 0) { printf("inode %u: out of range\n", ino); return; }
	if ((in.mode & P11_IFMT) != P11_IFDIR) { printf("inode %u is not a directory\n", ino); return; }
	printf("directory inode %u (%d bytes)\n  slot  inode  name\n", ino, in.size);
	nblk = ((uint32_t)in.size + H.r.bsize - 1) / H.r.bsize;
	for (b = 0; b < nblk; b++) {
		uint32_t phys = fdb_bmap(in.addr, b);
		if (!phys || fsr_bread(&H.r, phys, buf) < 0) continue;
		for (e = 0; e < H.r.ndirect; e++) {
			uint8_t *d = buf + e * P11_DIRENTSZ;
			uint16_t di = H.r.bo->get16(d);
			char nm[P11_DIRSIZ + 1];
			if ((b * H.r.ndirect + e) * P11_DIRENTSZ >= (uint32_t)in.size) break;
			if (di == 0) { freecnt++; continue; }
			memcpy(nm, d + 2, P11_DIRSIZ); nm[P11_DIRSIZ] = '\0';
			printf("  %4u  %5u  %s\n", b * H.r.ndirect + e, di, nm);
			used++;
		}
	}
	printf("  (%u entries, %u free slots)\n", used, freecnt);
}

static void do_map(uint32_t ino)
{
	fsr_inode in;
	uint32_t nblk, b, run0 = 0, prev = 0;
	if (fsr_iget(&H.r, ino, &in) < 0) { printf("inode %u: out of range\n", ino); return; }
	nblk = ((uint32_t)in.size + H.r.bsize - 1) / H.r.bsize;
	printf("inode %u block map (%u logical blocks):\n", ino, nblk);
	for (b = 0; b < nblk; b++) {
		uint32_t phys = fdb_bmap(in.addr, b);
		printf("  %5u -> %-8u%s\n", b, phys, phys == 0 ? "  (hole)" : "");
		(void)run0; (void)prev;
	}
}

static void do_block(uint32_t bno)
{
	uint8_t buf[P11_MAXBSIZE];
	uint32_t i, j;
	if (fsr_bread(&H.r, bno, buf) < 0) { printf("block %u: read error\n", bno); return; }
	printf("block %u:\n", bno);
	for (i = 0; i < H.r.bsize; i += 16) {
		printf("  %04x  ", i);
		for (j = 0; j < 16; j++) { printf("%02x ", buf[i + j]); if (j == 7) printf(" "); }
		printf(" |");
		for (j = 0; j < 16; j++) { int ch = buf[i + j]; putchar(ch >= 32 && ch < 127 ? ch : '.'); }
		printf("|\n");
	}
}

static void do_cat(uint32_t ino)
{
	fsr_inode in;
	long off = 0;
	if (fsr_iget(&H.r, ino, &in) < 0) { printf("inode %u: out of range\n", ino); return; }
	if ((in.mode & P11_IFMT) == P11_IFDIR) { printf("inode %u is a directory\n", ino); return; }
	while (off < in.size) {
		uint8_t b[8192];
		long want = in.size - off; if (want > (long)sizeof b) want = sizeof b;
		if (fsr_readfile(&H.r, &in, b, want, off) != want) break;
		fwrite(b, 1, want, stdout);
		off += want;
	}
}

struct lc { uint32_t target, count; };
static int lc_cb(void *a, uint32_t ino, const char *name)
{
	struct lc *l = a; (void)name;
	if (ino == l->target) l->count++;
	return 0;
}
static void do_links(uint32_t target)
{
	uint32_t nino = (H.r.isize - 2) * H.r.inopb, ino;
	struct lc l; l.target = target; l.count = 0;
	for (ino = 1; ino <= nino; ino++) {
		fsr_inode in;
		if (fsr_iget(&H.r, ino, &in) < 0) break;
		if ((in.mode & P11_IFMT) == P11_IFDIR)
			fsr_readdir(&H.r, &in, lc_cb, &l);
	}
	printf("inode %u is referenced by %u director%s\n",
	       target, l.count, l.count == 1 ? "y" : "ies");
}

/* -w: patch a decoded field of an inode */
static void do_set(uint32_t ino, const char *field, const char *valstr)
{
	uint8_t pb[P11_MAXBSIZE];
	uint32_t blk, off;
	uint8_t *ds;
	long val = strtol(valstr, NULL, !strcmp(field, "mode") ? 8 : 10);
	if (!H.writable) { printf("read-only; reopen with -w\n"); return; }
	rw_ino_loc(&H, ino, &blk, &off);
	s5fs_rdblk(&H.w, blk, pb);
	ds = pb + off;
	if      (!strcmp(field, "mode"))  H.w.bo->put16(ds + P11_DI_MODE, (uint16_t)val);
	else if (!strcmp(field, "nlink")) H.w.bo->put16(ds + P11_DI_NLINK, (uint16_t)val);
	else if (!strcmp(field, "uid"))   H.w.bo->put16(ds + P11_DI_UID, (uint16_t)val);
	else if (!strcmp(field, "gid"))   H.w.bo->put16(ds + P11_DI_GID, (uint16_t)val);
	else if (!strcmp(field, "size"))  H.w.bo->put32(ds + P11_DI_SIZE, (uint32_t)val);
	else { printf("unknown field '%s' (mode|nlink|uid|gid|size)\n", field); return; }
	s5fs_wtblk(&H.w, blk, pb);
	printf("inode %u %s set\n", ino, field);
}

/* -w: write raw hex bytes into a block */
static void do_poke(int argc, char **tok)
{
	uint8_t buf[P11_MAXBSIZE];
	uint32_t bno, o, i;
	if (!H.writable) { printf("read-only; reopen with -w\n"); return; }
	if (argc < 4) { printf("usage: poke BLK OFF HH [HH...]\n"); return; }
	bno = (uint32_t)strtoul(tok[1], NULL, 0);
	o   = (uint32_t)strtoul(tok[2], NULL, 0);
	if (fsr_bread(&H.r, bno, buf) < 0) { printf("block %u: read error\n", bno); return; }
	for (i = 3; i < (uint32_t)argc && o < H.r.bsize; i++, o++)
		buf[o] = (uint8_t)strtoul(tok[i], NULL, 16);
	s5fs_wtblk(&H.w, bno, buf);
	printf("poked %d byte(s) into block %s\n", argc - 3, tok[1]);
}

static void help(void)
{
	printf(
	  "  sb                     superblock summary\n"
	  "  inode N | i N          decode inode N\n"
	  "  dir N                  list directory inode N (slot / inode / name)\n"
	  "  map N                  logical->physical block map of inode N\n"
	  "  block N | b N          hexdump filesystem block N\n"
	  "  cat N                  write inode N's contents to stdout\n"
	  "  path /a/b              resolve a path to an inode number\n"
	  "  links N                count directory references to inode N\n"
	  "  set N field value      (-w) patch mode|nlink|uid|gid|size (mode is octal)\n"
	  "  poke BLK OFF HH..      (-w) write raw hex bytes at block/offset\n"
	  "  help ; quit | exit\n");
}

int cmd_fsdb(int argc, char **argv)
{
	uint32_t bsize = 0, plen = 0;
	int forced = -1, wr = 0, c, interactive;
	const char *dev = NULL, *ospec = NULL;
	char part = 0, line[2048];
	long long base = 0;

	optind = 1;
	while ((c = getopt(argc, argv, "wB:A:d:P:o:")) != -1) {
		switch (c) {
		case 'w': wr = 1; break;
		case 'B': bsize = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'A': { s5_endian e = s5_endian_parse(optarg);
		            if (e == S5_NENDIAN) { fprintf(stderr, "fsdb: bad -A\n"); return 2; }
		            forced = (int)e; break; }
		case 'd': dev = optarg; break;
		case 'P': part = optarg[0]; break;
		case 'o': ospec = optarg; break;
		default: fprintf(stderr, "usage: s5fs fsdb [-w] [-B ..] [-A ..] [-d dev -P part | -o blk] image\n"); return 2;
		}
	}
	if (optind != argc - 1) { fprintf(stderr, "usage: s5fs fsdb [-w] [-B ..] [-A ..] [-d dev -P part | -o blk] image\n"); return 2; }
	if (device_resolve_part(dev, part, ospec, &base, &plen) < 0) return 2;
	if (rw_open(&H, argv[optind], bsize, forced, wr, base) < 0) {
		if (!wr || rw_open(&H, argv[optind], bsize, forced, 0, base) < 0) {
			fprintf(stderr, "fsdb: %s: not a readable s5fs image (try -B/-A)\n", argv[optind]);
			return 1;
		}
		wr = 0;
	}
	interactive = isatty(STDIN_FILENO);
	printf("s5fs fsdb: %s (%s, %u-byte blocks, %s)\n", argv[optind],
	       H.r.bo->name, H.r.bsize, H.writable ? "read-write" : "read-only");
	printf("type 'help' for commands.\n");

	for (;;) {
		char *tok[64]; int n = 0;
		if (interactive) { printf("fsdb> "); fflush(stdout); }
		if (!fgets(line, sizeof line, stdin)) { printf("\n"); break; }
		{ char *save, *t;
		  for (t = strtok_r(line, " \t\r\n", &save); t && n < 63; t = strtok_r(NULL, " \t\r\n", &save)) tok[n++] = t; }
		if (n == 0) continue;
		tok[n] = NULL;

		if (!strcmp(tok[0], "quit") || !strcmp(tok[0], "exit")) break;
		else if (!strcmp(tok[0], "help") || !strcmp(tok[0], "?")) help();
		else if (!strcmp(tok[0], "sb")) do_sb();
		else if ((!strcmp(tok[0], "inode") || !strcmp(tok[0], "i")) && n >= 2) do_inode((uint32_t)strtoul(tok[1], NULL, 0));
		else if (!strcmp(tok[0], "dir") && n >= 2)  do_dir((uint32_t)strtoul(tok[1], NULL, 0));
		else if (!strcmp(tok[0], "map") && n >= 2)  do_map((uint32_t)strtoul(tok[1], NULL, 0));
		else if ((!strcmp(tok[0], "block") || !strcmp(tok[0], "b")) && n >= 2) do_block((uint32_t)strtoul(tok[1], NULL, 0));
		else if (!strcmp(tok[0], "cat") && n >= 2)  do_cat((uint32_t)strtoul(tok[1], NULL, 0));
		else if (!strcmp(tok[0], "path") && n >= 2) { uint32_t x = rw_namei(&H, tok[1]);
		         if (x) printf("%s -> inode %u\n", tok[1], x); else printf("%s: not found\n", tok[1]); }
		else if (!strcmp(tok[0], "links") && n >= 2) do_links((uint32_t)strtoul(tok[1], NULL, 0));
		else if (!strcmp(tok[0], "set") && n >= 4)  do_set((uint32_t)strtoul(tok[1], NULL, 0), tok[2], tok[3]);
		else if (!strcmp(tok[0], "poke"))           do_poke(n, tok);
		else printf("%s: unknown or missing args (try 'help')\n", tok[0]);
	}
	rw_close(&H);
	return 0;
}

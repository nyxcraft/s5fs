/*
 * cmd_shell.c -- an interactive explorer for an s5fs image (no mount needed).
 *
 *   s5fs shell [-B 512|1024] [-A pdp11|le|be] [-r] image
 *
 * A tiny REPL with a current directory inside the image.  Commands:
 *
 *   ls [-l] [-a] [path]     list a directory
 *   cd [path]               change directory ("cd" -> "/")
 *   pwd                     print the current image directory
 *   cat file...             print file contents
 *   get imgpath [hostpath]  copy a file out to the host
 *   put hostpath [imgpath]  copy a host file in
 *   cp src dst              copy within the image
 *   mv src dst              rename/move within the image
 *   rm file...              remove file(s)
 *   mkdir dir...            create directory(ies)
 *   rmdir dir...            remove empty directory(ies)
 *   chmod mode path...      change permission bits
 *   stat path               show inode details
 *   help                    this list
 *   quit / exit             leave
 *
 * Reuses the s5fs_rw engine, so it edits exactly like the FUSE mount and the
 * batch commands.  Opens read-write by default (-r for read-only browsing).
 */

#define _POSIX_C_SOURCE 200809L

#include "s5fs_rw.h"
#include "fsutil.h"
#include "device.h"
#include "cmds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>

static char cwd[2048] = "/";

/* absolute image path for `arg` relative to cwd, into a static buffer */
static const char *rel(const char *arg)
{
	static char out[2048];
	fs_resolve(cwd, arg ? arg : ".", out, sizeof out);
	return out;
}

static void ls_long(const char *name, const fsr_inode *in)
{
	char m[11], ts[20]; time_t tt = in->mtime; struct tm *tm = localtime(&tt);
	fs_modestr(in->mode, m);
	if (tm) strftime(ts, sizeof ts, "%Y-%m-%d %H:%M", tm); else snprintf(ts, sizeof ts, "?");
	if ((in->mode & P11_IFMT) == P11_IFCHR || (in->mode & P11_IFMT) == P11_IFBLK)
		printf("%s %2d %4d %4d %3u,%3u %s %s\n", m, (uint16_t)in->nlink,
		       (uint16_t)in->uid, (uint16_t)in->gid,
		       (in->addr[0] >> 8) & 0377, in->addr[0] & 0377, ts, name);
	else
		printf("%s %2d %4d %4d %8d %s %s\n", m, (uint16_t)in->nlink,
		       (uint16_t)in->uid, (uint16_t)in->gid, in->size, ts, name);
}

static void do_ls(RW *h, int argc, char **argv)
{
	int longf = 0, all = 0, i;
	const char *path = cwd;
	fsr_inode in; uint32_t ino;

	for (i = 1; i < argc; i++) {
		if (argv[i][0] == '-') {
			if (strchr(argv[i], 'l')) longf = 1;
			if (strchr(argv[i], 'a')) all = 1;
		} else path = rel(argv[i]);
	}
	ino = rw_namei(h, path);
	if (!ino || rw_iget(h, ino, &in) < 0) { printf("ls: %s: not found\n", path); return; }
	if ((in.mode & P11_IFMT) != P11_IFDIR) {		/* a single file */
		const char *base = strrchr(path, '/');
		base = (base && base[1]) ? base + 1 : path;
		if (longf) ls_long(base, &in); else printf("%s\n", base);
		return;
	}
	{
		uint8_t buf[P11_MAXBSIZE];
		uint32_t nblk = ((uint32_t)in.size + h->r.bsize - 1) / h->r.bsize, b, e;
		for (b = 0; b < nblk; b++) {
			/* re-read each dir block through the reader's bmap via readfile */
			long got = fsr_readfile(&h->r, &in, buf, h->r.bsize, (long)b * h->r.bsize);
			for (e = 0; e * P11_DIRENTSZ < (uint32_t)got; e++) {
				uint8_t *d = buf + e * P11_DIRENTSZ;
				uint16_t di = h->r.bo->get16(d);
				char name[P11_DIRSIZ + 1]; fsr_inode ein;
				if (di == 0) continue;
				memcpy(name, d + 2, P11_DIRSIZ); name[P11_DIRSIZ] = '\0';
				if (!all && name[0] == '.') continue;
				if (!longf) { printf("%s\n", name); continue; }
				if (rw_iget(h, di, &ein) < 0) continue;
				ls_long(name, &ein);
			}
		}
	}
}

static void do_cd(RW *h, const char *arg)
{
	const char *p = arg ? rel(arg) : "/";
	char save[2048];
	fsr_inode in; uint32_t ino = rw_namei(h, p);
	if (!ino || rw_iget(h, ino, &in) < 0) { printf("cd: %s: not found\n", p); return; }
	if ((in.mode & P11_IFMT) != P11_IFDIR) { printf("cd: %s: not a directory\n", p); return; }
	snprintf(save, sizeof save, "%s", p);
	snprintf(cwd, sizeof cwd, "%s", save);
}

static void do_cat(RW *h, int argc, char **argv)
{
	int i;
	for (i = 1; i < argc; i++) {
		const char *p = rel(argv[i]);
		fsr_inode in; long off = 0; uint32_t ino = rw_namei(h, p);
		if (!ino || rw_iget(h, ino, &in) < 0) { printf("cat: %s: not found\n", p); continue; }
		if ((in.mode & P11_IFMT) == P11_IFDIR) { printf("cat: %s: is a directory\n", p); continue; }
		while (off < in.size) {
			uint8_t buf[8192];
			long want = in.size - off; if (want > (long)sizeof buf) want = sizeof buf;
			if (fsr_readfile(&h->r, &in, buf, want, off) != want) break;
			fwrite(buf, 1, want, stdout); off += want;
		}
	}
}

static void do_get(RW *h, int argc, char **argv)
{
	const char *imgp, *hostp; char hbuf[2048];
	fsr_inode in; uint32_t ino; long off = 0; int ofd;
	if (argc < 2) { printf("usage: get imgpath [hostpath]\n"); return; }
	imgp = rel(argv[1]);
	if (argc >= 3) hostp = argv[2];
	else { const char *b = strrchr(imgp, '/'); snprintf(hbuf, sizeof hbuf, "%s", b ? b + 1 : imgp); hostp = hbuf; }
	ino = rw_namei(h, imgp);
	if (!ino || rw_iget(h, ino, &in) < 0) { printf("get: %s: not found\n", imgp); return; }
	if ((in.mode & P11_IFMT) == P11_IFDIR) { printf("get: %s: is a directory\n", imgp); return; }
	ofd = open(hostp, O_WRONLY | O_CREAT | O_TRUNC, in.mode & 0777);
	if (ofd < 0) { printf("get: %s: %s\n", hostp, strerror(errno)); return; }
	while (off < in.size) {
		uint8_t buf[65536];
		long want = in.size - off; if (want > (long)sizeof buf) want = sizeof buf;
		if (fsr_readfile(&h->r, &in, buf, want, off) != want) break;
		if (write(ofd, buf, want) != want) break;
		off += want;
	}
	close(ofd);
	printf("%s -> %s (%d bytes)\n", imgp, hostp, in.size);
}

static void do_put(RW *h, int argc, char **argv)
{
	const char *hostp; char ibuf[2048]; int ifd, rc;
	struct stat s; unsigned perm = 0644; int32_t mt = 0;
	if (argc < 2) { printf("usage: put hostpath [imgpath]\n"); return; }
	hostp = argv[1];
	ifd = open(hostp, O_RDONLY);
	if (ifd < 0) { printf("put: %s: %s\n", hostp, strerror(errno)); return; }
	if (fstat(ifd, &s) == 0) { perm = s.st_mode & 0777; mt = (int32_t)s.st_mtime; }
	if (argc >= 3) snprintf(ibuf, sizeof ibuf, "%s", rel(argv[2]));
	else {
		const char *b = strrchr(hostp, '/');
		char one[1024]; snprintf(one, sizeof one, "%s", b ? b + 1 : hostp);
		snprintf(ibuf, sizeof ibuf, "%s", rel(one));
	}
	rc = rw_put_fd(h, ibuf, ifd, perm, mt);
	close(ifd);
	if (rc < 0) { printf("put: %s: %s\n", ibuf, strerror(-rc)); return; }
	printf("%s -> %s\n", hostp, ibuf);
}

static void do_stat(RW *h, const char *arg)
{
	const char *p = rel(arg);
	fsr_inode in; uint32_t ino = rw_namei(h, p); char m[11]; time_t mt;
	if (!ino || rw_iget(h, ino, &in) < 0) { printf("stat: %s: not found\n", p); return; }
	fs_modestr(in.mode, m); mt = in.mtime;
	printf("  path:  %s\n", p);
	printf("  inode: %u\n", ino);
	printf("  mode:  %s (0%o)\n", m, in.mode & 07777);
	printf("  links: %d   uid: %d   gid: %d\n", (uint16_t)in.nlink, (uint16_t)in.uid, (uint16_t)in.gid);
	printf("  size:  %d bytes\n", in.size);
	printf("  mtime: %s", ctime(&mt));
}

static void help(void)
{
	printf(
	  "commands:\n"
	  "  ls [-l] [-a] [path]     list directory\n"
	  "  cd [path]               change directory (cd -> /)\n"
	  "  pwd                     print current directory\n"
	  "  cat file...             print file(s)\n"
	  "  get imgpath [hostpath]  copy a file out to the host\n"
	  "  put hostpath [imgpath]  copy a host file in\n"
	  "  cp [@]src [@]dst        copy (prefix a HOST path with '@')\n"
	  "  mv src dst              rename/move within the image\n"
	  "  rm file...              remove file(s)\n"
	  "  mkdir dir...            create directory(ies)\n"
	  "  rmdir dir...            remove empty directory(ies)\n"
	  "  chmod mode path...      change permission bits (octal)\n"
	  "  chown uid[:gid] path... change owner (numeric)\n"
	  "  chgrp gid path...       change group (numeric)\n"
	  "  stat path               show inode details\n"
	  "  help                    this list\n"
	  "  quit | exit             leave\n");
}

int cmd_shell(int argc, char **argv)
{
	RW h; uint32_t bsize = 0, plen = 0; int forced = -1, ro = 0, c, interactive;
	const char *dev = NULL, *ospec = NULL; char part = 0; long long base = 0;
	char line[4096];

	optind = 1;
	while ((c = getopt(argc, argv, "rB:A:d:P:o:")) != -1) {
		switch (c) {
		case 'r': ro = 1; break;
		case 'B': bsize = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'A': { s5_endian e = s5_endian_parse(optarg);
		            if (e == S5_NENDIAN) { fprintf(stderr, "shell: bad -A\n"); return 2; }
		            forced = (int)e; break; }
		case 'd': dev = optarg; break;
		case 'P': part = optarg[0]; break;
		case 'o': ospec = optarg; break;
		default: fprintf(stderr, "usage: s5fs shell [-r] [-B ..] [-A ..] [-d dev -P part | -o blk] image\n"); return 2;
		}
	}
	if (optind != argc - 1) { fprintf(stderr, "usage: s5fs shell [-r] [-B ..] [-A ..] [-d dev -P part | -o blk] image\n"); return 2; }
	if (device_resolve_part(dev, part, ospec, &base, &plen) < 0) return 2;

	if (rw_open(&h, argv[optind], bsize, forced, !ro, base) < 0) {
		/* fall back to read-only if the image can't be opened writable */
		if (ro || rw_open(&h, argv[optind], bsize, forced, 0, base) < 0) {
			fprintf(stderr, "shell: %s: not a readable s5fs image (try -B/-A, or -d/-P)\n", argv[optind]);
			return 1;
		}
		ro = 1;
	}
	interactive = isatty(STDIN_FILENO);
	printf("s5fs shell: %s (%s, %u-byte blocks, %s)\n", argv[optind],
	       h.r.bo->name, h.r.bsize, h.writable ? "read-write" : "read-only");
	printf("type 'help' for commands.\n");

	for (;;) {
		char *tok[64]; int n = 0, ro_blocked;
		if (interactive) { printf("%s> ", cwd); fflush(stdout); }
		if (!fgets(line, sizeof line, stdin)) { printf("\n"); break; }
		{
			char *save, *t;
			for (t = strtok_r(line, " \t\r\n", &save); t && n < 63; t = strtok_r(NULL, " \t\r\n", &save))
				tok[n++] = t;
		}
		if (n == 0) continue;
		tok[n] = NULL;

		ro_blocked = !h.writable;
		if (!strcmp(tok[0], "quit") || !strcmp(tok[0], "exit")) break;
		else if (!strcmp(tok[0], "help") || !strcmp(tok[0], "?")) help();
		else if (!strcmp(tok[0], "pwd")) printf("%s\n", cwd);
		else if (!strcmp(tok[0], "ls")) do_ls(&h, n, tok);
		else if (!strcmp(tok[0], "cd")) do_cd(&h, n >= 2 ? tok[1] : NULL);
		else if (!strcmp(tok[0], "cat")) do_cat(&h, n, tok);
		else if (!strcmp(tok[0], "get")) do_get(&h, n, tok);
		else if (!strcmp(tok[0], "stat")) { if (n >= 2) do_stat(&h, tok[1]); else printf("usage: stat path\n"); }
		else if (ro_blocked && (!strcmp(tok[0], "put") ||
		         !strcmp(tok[0], "mv") || !strcmp(tok[0], "rm") || !strcmp(tok[0], "mkdir") ||
		         !strcmp(tok[0], "rmdir") || !strcmp(tok[0], "chmod") ||
		         !strcmp(tok[0], "chown") || !strcmp(tok[0], "chgrp")))
			printf("%s: image is open read-only\n", tok[0]);
		else if (!strcmp(tok[0], "put")) do_put(&h, n, tok);
		else if (!strcmp(tok[0], "cp")) {
			if (n < 3) printf("usage: cp [@]src [@]dst   (@ = host path)\n");
			else {
				const char *s = tok[1], *d = tok[2]; int sh = fs_host_path(&s), dh = fs_host_path(&d);
				char sb[2048], db[2048]; int rc;
				if (!sh) { snprintf(sb, sizeof sb, "%s", rel(s)); s = sb; }
				if (!dh) { snprintf(db, sizeof db, "%s", rel(d)); d = db; }
				rc = fs_copy(&h, s, sh, d, dh);
				if (rc < 0) printf("cp: %s\n", strerror(-rc));
			}
		}
		else if (!strcmp(tok[0], "mv")) {
			if (n < 3) printf("usage: mv src dst\n");
			else { char s[2048]; int rc; snprintf(s, sizeof s, "%s", rel(tok[1])); rc = rw_rename(&h, s, rel(tok[2]));
			       if (rc < 0) printf("mv: %s\n", strerror(-rc)); }
		}
		else if (!strcmp(tok[0], "rm")) {
			int i; for (i = 1; i < n; i++) { char p[2048]; int rc; snprintf(p, sizeof p, "%s", rel(tok[i])); rc = rw_unlink(&h, p);
			       if (rc < 0) printf("rm: %s: %s\n", tok[i], strerror(-rc)); }
		}
		else if (!strcmp(tok[0], "mkdir")) {
			int i; for (i = 1; i < n; i++) { char p[2048]; int rc; snprintf(p, sizeof p, "%s", rel(tok[i])); rc = rw_mkdir(&h, p, 0755);
			       if (rc < 0) printf("mkdir: %s: %s\n", tok[i], strerror(-rc)); }
		}
		else if (!strcmp(tok[0], "rmdir")) {
			int i; for (i = 1; i < n; i++) { char p[2048]; int rc; snprintf(p, sizeof p, "%s", rel(tok[i])); rc = rw_rmdir(&h, p);
			       if (rc < 0) printf("rmdir: %s: %s\n", tok[i], strerror(-rc)); }
		}
		else if (!strcmp(tok[0], "chmod")) {
			if (n < 3) printf("usage: chmod mode path...\n");
			else { unsigned perm = (unsigned)strtoul(tok[1], NULL, 8); int i;
			       for (i = 2; i < n; i++) { char p[2048]; int rc; snprintf(p, sizeof p, "%s", rel(tok[i])); rc = rw_chmod(&h, p, perm);
			              if (rc < 0) printf("chmod: %s: %s\n", tok[i], strerror(-rc)); } }
		}
		else if (!strcmp(tok[0], "chown")) {
			if (n < 3) printf("usage: chown uid[:gid] path...\n");
			else { int uid, gid, i; fs_parse_owner(tok[1], &uid, &gid);
			       for (i = 2; i < n; i++) { char p[2048]; int rc; snprintf(p, sizeof p, "%s", rel(tok[i])); rc = rw_chown(&h, p, uid, gid);
			              if (rc < 0) printf("chown: %s: %s\n", tok[i], strerror(-rc)); } }
		}
		else if (!strcmp(tok[0], "chgrp")) {
			if (n < 3) printf("usage: chgrp gid path...\n");
			else { int gid = (int)strtol(tok[1], NULL, 10), i;
			       for (i = 2; i < n; i++) { char p[2048]; int rc; snprintf(p, sizeof p, "%s", rel(tok[i])); rc = rw_chown(&h, p, -1, gid);
			              if (rc < 0) printf("chgrp: %s: %s\n", tok[i], strerror(-rc)); } }
		}
		else printf("%s: unknown command (try 'help')\n", tok[0]);
	}
	rw_close(&h);
	return 0;
}

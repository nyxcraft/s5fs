/*
 * cmd_util.c -- small s5fs subcommands: `devices`, and the `mount`/`umount`
 * placeholders for the planned FUSE support.
 */

#define _POSIX_C_SOURCE 200809L

#include "cmds.h"
#include "device.h"
#include "pdp11fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

int cmd_devices(int argc, char **argv)
{
	if (argc >= 2)			/* `s5fs devices <name>` -> its partition map */
		device_show_parts(argv[1]);
	else
		device_list();
	return 0;
}

/*
 * s5fs boot -- install a primary bootstrap into block 0 of an image.
 *
 * The 2.9BSD mdec "*uboot" files are already the raw boot code (a.out assembled,
 * stripped, header removed).  If given a .o instead (a.out magic 0407/0410/0411
 * in the first word), the 16-byte exec header is skipped, matching mdec's
 * `dd bs=8w skip=1`.  Block 0 is the boot block and is not part of the
 * filesystem, so this is independent of the fs contents.
 *
 * usage: s5fs boot image bootfile
 */
int cmd_boot(int argc, char **argv)
{
	uint8_t buf[P11_MAXBSIZE];
	const char *image, *bootf;
	int bf, img;
	ssize_t n;
	size_t off = 0;

	if (argc != 3) { fprintf(stderr, "usage: s5fs boot image bootfile\n"); return 2; }
	image = argv[1];
	bootf = argv[2];

	bf = open(bootf, O_RDONLY);
	if (bf < 0) { fprintf(stderr, "s5fs boot: %s: %s\n", bootf, strerror(errno)); return 1; }
	memset(buf, 0, sizeof buf);
	n = read(bf, buf, sizeof buf);
	close(bf);
	if (n <= 0) { fprintf(stderr, "s5fs boot: %s: empty or unreadable\n", bootf); return 1; }

	/* skip a 16-byte a.out header if this is a .o rather than raw boot code */
	{
		uint16_t magic = (uint16_t)(buf[0] | (buf[1] << 8));
		if (magic == 0407 || magic == 0410 || magic == 0411) {
			off = 16;
			if ((size_t)n <= off) { fprintf(stderr, "s5fs boot: bootfile too small\n"); return 1; }
		}
	}
	if ((size_t)n - off > 512)
		fprintf(stderr, "s5fs boot: warning: boot code is %zu bytes (>512); "
		        "the ROM loads only the first sector\n", (size_t)n - off);

	img = open(image, O_RDWR);
	if (img < 0) { fprintf(stderr, "s5fs boot: %s: %s\n", image, strerror(errno)); return 1; }
	if (lseek(img, 0, SEEK_SET) < 0 ||
	    write(img, buf + off, (size_t)n - off) != (ssize_t)((size_t)n - off)) {
		fprintf(stderr, "s5fs boot: %s: write error\n", image);
		close(img); return 1;
	}
	close(img);
	printf("%s: installed %zu-byte boot block from %s\n", image, (size_t)n - off, bootf);
	return 0;
}

/* When built with FUSE (make FUSE=1), cmd_mount.c provides these instead. */
#ifndef HAVE_FUSE
int cmd_mount(int argc, char **argv)
{
	(void)argc; (void)argv;
	fprintf(stderr,
	    "s5fs mount: this build has no FUSE support -- rebuild with 'make FUSE=1'\n"
	    "(needs libfuse3-dev).  Meanwhile inspect an image with 's5fs fsck -l'.\n");
	return 2;
}

int cmd_umount(int argc, char **argv)
{
	(void)argc; (void)argv;
	fprintf(stderr, "s5fs umount: this build has no FUSE support (make FUSE=1).\n");
	return 2;
}
#endif /* !HAVE_FUSE */

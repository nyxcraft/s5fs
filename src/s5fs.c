/*
 * s5fs -- a single multi-tool for 2.9BSD-family (s5fs) disk images.
 *
 * Git-style dispatch: `s5fs <command> [args...]`.  All subcommands share the
 * s5fs writer core, the fsck reader, and the device table; this file is the
 * only main() and just routes to the chosen subcommand.
 *
 *   s5fs mkfs    create a filesystem image
 *   s5fs fsck    check an image (and, with -l, list its tree)
 *   s5fs devices list known PDP-11 disk types
 *   s5fs mount   FUSE-mount an image        (planned)
 *   s5fs umount  unmount a FUSE mount       (planned)
 */

#include "cmds.h"

#include <stdio.h>
#include <string.h>

static const struct subcmd {
	const char *name;
	int       (*fn)(int, char **);
	const char *help;
} subcmds[] = {
	{ "mkfs",    cmd_mkfs,    "create an s5fs filesystem image"          },
	{ "mktree",  cmd_mktree,  "build an image from a host directory tree" },
	{ "tar",     cmd_tar,     "image <-> tar archive (tar c / tar x)"    },
	{ "restore", cmd_restore, "restore a 2.9BSD dump tape into an image" },
	{ "dump",    cmd_dump,    "write a 2.9BSD dump tape from an image"   },
	{ "fsck",    cmd_fsck,    "check an image (-p repairs; -l lists)"    },
	{ "icheck",  cmd_icheck,  "block/free-list check (-s salvages)"      },
	{ "dcheck",  cmd_dcheck,  "directory link-count check"               },
	{ "clri",    cmd_clri,    "clear (zero) inodes by number"            },
	{ "fsdb",    cmd_fsdb,    "interactive filesystem debugger (-w to edit)" },
	{ "manifest",cmd_manifest,"fingerprint an image (path/mode/owner/cksum)" },
	{ "verify",  cmd_verify,  "diff an image against a manifest"         },
	{ "boot",    cmd_boot,    "install a boot block into block 0"        },
	{ "vhd",     cmd_vhd,     "wrap/unwrap a fixed-VHD container"        },
	{ "ls",      cmd_ls,      "list a directory (-l long, -a all)"       },
	{ "cat",     cmd_cat,     "print file contents to stdout"            },
	{ "get",     cmd_get,     "copy a file out of the image to the host" },
	{ "put",     cmd_put,     "copy a host file into the image"          },
	{ "cp",      cmd_cp,      "copy a file within the image"             },
	{ "mv",      cmd_mv,      "rename/move a file within the image"      },
	{ "rm",      cmd_rm,      "remove file(s) from the image"            },
	{ "mkdir",   cmd_mkdir,   "create directory(ies) (-p for parents)"   },
	{ "rmdir",   cmd_rmdir,   "remove empty directory(ies)"              },
	{ "chmod",   cmd_chmod,   "change permission bits (octal)"           },
	{ "chown",   cmd_chown,   "change owner: uid[:gid]"                  },
	{ "chgrp",   cmd_chgrp,   "change group: gid"                        },
	{ "shell",   cmd_shell,   "interactive explorer (cd/ls/get/put/...)" },
	{ "devices", cmd_devices, "list known PDP-11 disk types"             },
	{ "mount",   cmd_mount,   "FUSE-mount an image read-only (make FUSE=1)" },
	{ "umount",  cmd_umount,  "unmount a FUSE mount"                     },
};
#define NSUB ((int)(sizeof subcmds / sizeof subcmds[0]))

static int usage(int rc)
{
	int i;

	fprintf(stderr, "usage: s5fs <command> [args...]\n\ncommands:\n");
	for (i = 0; i < NSUB; i++)
		fprintf(stderr, "  %-9s %s\n", subcmds[i].name, subcmds[i].help);
	fprintf(stderr, "\nRun a command with no arguments for its own usage.\n");
	return rc;
}

int main(int argc, char **argv)
{
	const char *cmd;
	int i;

	if (argc < 2)
		return usage(2);
	cmd = argv[1];
	if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0)
		return usage(0);
	if (strcmp(cmd, "unmount") == 0)		/* friendly alias */
		return cmd_umount(argc - 1, argv + 1);

	for (i = 0; i < NSUB; i++)
		if (strcmp(cmd, subcmds[i].name) == 0)
			return subcmds[i].fn(argc - 1, argv + 1);

	fprintf(stderr, "s5fs: unknown command '%s'\n", cmd);
	return usage(2);
}

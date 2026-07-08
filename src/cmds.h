/*
 * cmds.h -- s5fs subcommand entry points.
 *
 * Each subcommand is invoked as cmd(argc, argv) with argv[0] set to the
 * subcommand's own name, so getopt() inside it behaves exactly as in a
 * standalone program.  The s5fs dispatcher (s5fs.c) is the only main().
 */
#ifndef CMDS_H
#define CMDS_H

int cmd_mkfs(int argc, char **argv);	/* create an s5fs image           */
int cmd_mktree(int argc, char **argv);	/* image from a host directory    */
int cmd_tar(int argc, char **argv);	/* image <-> tar archive (c/x)    */
int cmd_restore(int argc, char **argv); /* restore a dump(8) tape into an image */
int cmd_dump(int argc, char **argv);	/* write a dump(8) tape from an image */
int cmd_fsck(int argc, char **argv);	/* check / list an s5fs image     */
int cmd_icheck(int argc, char **argv);	/* block/free-list check (-s salvage) */
int cmd_dcheck(int argc, char **argv);	/* directory link-count check     */
int cmd_clri(int argc, char **argv);	/* clear an inode by number       */
int cmd_fsdb(int argc, char **argv);	/* interactive filesystem debugger */
int cmd_manifest(int argc, char **argv);/* mtree-style fingerprint of an image */
int cmd_verify(int argc, char **argv);	/* diff an image against a manifest    */
int cmd_scavenge(int argc, char **argv);/* scavenge deleted-file remnants      */
int cmd_boot(int argc, char **argv);	/* install a boot block (block 0) */
int cmd_vhd(int argc, char **argv);	/* wrap/unwrap a fixed-VHD container */

/* file manipulation without a mount (works on any host, incl. Windows) */
int cmd_ls(int argc, char **argv);	/* list a directory (or file)     */
int cmd_cat(int argc, char **argv);	/* print file contents            */
int cmd_get(int argc, char **argv);	/* copy a file image -> host      */
int cmd_put(int argc, char **argv);	/* copy a file host -> image      */
int cmd_cp(int argc, char **argv);	/* copy a file within the image   */
int cmd_mv(int argc, char **argv);	/* rename/move within the image   */
int cmd_rm(int argc, char **argv);	/* remove file(s)                 */
int cmd_mkdir(int argc, char **argv);	/* create directory(ies) (-p)     */
int cmd_rmdir(int argc, char **argv);	/* remove empty directory(ies)    */
int cmd_chmod(int argc, char **argv);	/* change permission bits         */
int cmd_chown(int argc, char **argv);	/* change owner (uid[:gid])       */
int cmd_chgrp(int argc, char **argv);	/* change group (gid)             */
int cmd_shell(int argc, char **argv);	/* interactive explorer (REPL)    */

int cmd_devices(int argc, char **argv);	/* list known PDP-11 disk types   */
int cmd_mount(int argc, char **argv);	/* FUSE mount (planned)           */
int cmd_umount(int argc, char **argv);	/* FUSE unmount (planned)         */

#endif /* CMDS_H */

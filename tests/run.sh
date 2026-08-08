#!/bin/sh
# s5fs regression tests -- self-contained (build our own fixtures, no external
# 2.9 data), dependency-free (sh + coreutils).  Run:  make test
#
# Exercises both layers and their seam: filesystem ops (mkfs/put/get/cp/mv/rm/
# mkdir/tar/dump/shell/fsck), the disk layer (partitions, VHD, .tap), and byte
# order.  Every mutating flow ends by asserting `s5fs fsck` is clean.

set -u
S5=${S5:-./bin/s5fs}
[ -x "$S5" ] || S5=../bin/s5fs
[ -x "$S5" ] || { echo "run.sh: cannot find bin/s5fs (build first)"; exit 2; }
S5=$(cd "$(dirname "$S5")" && pwd)/$(basename "$S5")

T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT
pass=0; fail=0
ok() { pass=$((pass + 1)); printf '  ok   %s\n' "$1"; }
no() { fail=$((fail + 1)); printf '  FAIL %s\n' "$1"; }
fsck_clean() { [ "$("$S5" fsck "$@" 2>/dev/null | tail -1)" = clean ]; }

# Run a command under a time limit; returns 124 if it had to be killed.
# macOS has no timeout(1), and a bare `timeout ...` there exits 127 -- which is
# neither 124 nor a crash, so every hang/crash check silently passed instead of
# running.  Prefer the real thing, fall back to a poll-and-kill.
if command -v timeout >/dev/null 2>&1; then
	tmo() { _s=$1; shift; timeout "$_s" "$@"; }
elif command -v gtimeout >/dev/null 2>&1; then
	tmo() { _s=$1; shift; gtimeout "$_s" "$@"; }
else
	tmo() {
		_s=$1; shift
		"$@" & _p=$!
		_i=0
		while kill -0 "$_p" 2>/dev/null; do
			[ "$_i" -ge "$_s" ] && {
				kill -9 "$_p" 2>/dev/null
				wait "$_p" 2>/dev/null
				return 124
			}
			_i=$((_i + 1))
			sleep 1
		done
		wait "$_p"
	}
fi

echo "s5fs regression tests ($S5)"

# 1. mkfs + fsck
"$S5" mkfs -d rl02 "$T/a.dsk" >/dev/null 2>&1
if fsck_clean "$T/a.dsk"; then ok "mkfs -> fsck clean"; else no "mkfs -> fsck clean"; fi

# 2. put / ls / get round-trip
printf 'the quick brown fox\n' > "$T/src"
"$S5" put "$T/a.dsk" "$T/src" /f >/dev/null 2>&1
"$S5" get "$T/a.dsk" /f "$T/out" >/dev/null 2>&1
if cmp -s "$T/src" "$T/out"; then ok "put/get byte-identical"; else no "put/get byte-identical"; fi
if "$S5" ls "$T/a.dsk" / 2>/dev/null | grep -q '^f$'; then ok "ls shows put file"; else no "ls shows put file"; fi

# 3. mkdir / cp / mv / rm  + fsck
"$S5" mkdir "$T/a.dsk" /d >/dev/null 2>&1
"$S5" cp "$T/a.dsk" /f /d/f2 >/dev/null 2>&1
"$S5" mv "$T/a.dsk" /d/f2 /d/f3 >/dev/null 2>&1
"$S5" rm "$T/a.dsk" /f >/dev/null 2>&1
if fsck_clean "$T/a.dsk"; then ok "mkdir/cp/mv/rm -> fsck clean"; else no "mkdir/cp/mv/rm -> fsck clean"; fi
if "$S5" cat "$T/a.dsk" /d/f3 2>/dev/null | grep -q fox; then ok "cp/mv preserved data"; else no "cp/mv preserved data"; fi

# 4. cp host<->image via '@' (both directions)
"$S5" cp "$T/a.dsk" @"$T/src" /viacp >/dev/null 2>&1
"$S5" cp "$T/a.dsk" /viacp @"$T/viaout" >/dev/null 2>&1
if cmp -s "$T/src" "$T/viaout"; then ok "cp @host round-trip"; else no "cp @host round-trip"; fi

# 5. tar c | tar x round-trip (tree identical)
"$S5" tar c "$T/a.dsk" "$T/t1.tar" >/dev/null 2>&1
"$S5" tar x -d rl02 "$T/t1.tar" "$T/b.dsk" >/dev/null 2>&1
"$S5" tar c "$T/b.dsk" "$T/t2.tar" >/dev/null 2>&1
if cmp -s "$T/t1.tar" "$T/t2.tar"; then ok "tar c|x tree round-trip"; else no "tar c|x tree round-trip"; fi

# 6. dump | restore round-trip (tree identical)
"$S5" dump "$T/a.dsk" "$T/d.dump" >/dev/null 2>&1
"$S5" restore -d rl02 "$T/d.dump" "$T/c.dsk" >/dev/null 2>&1
"$S5" tar c "$T/c.dsk" "$T/t3.tar" >/dev/null 2>&1
if cmp -s "$T/t1.tar" "$T/t3.tar"; then ok "dump|restore tree round-trip"; else no "dump|restore tree round-trip"; fi
if fsck_clean "$T/c.dsk"; then ok "restore -> fsck clean"; else no "restore -> fsck clean"; fi

# 7. dump -T (SIMH .tap): larger than flat, ends in two tape marks (8 zero bytes)
"$S5" dump -T "$T/a.dsk" "$T/t.tap" >/dev/null 2>&1
tapsz=$(wc -c < "$T/t.tap"); flatsz=$(wc -c < "$T/d.dump")
zeros=$(tail -c 8 "$T/t.tap" | od -An -tx1 | tr -d ' \n')
if [ "$tapsz" -gt "$flatsz" ] && [ "$zeros" = 0000000000000000 ]; then ok "dump -T .tap framing"; else no "dump -T .tap framing"; fi

# 8. partitions: two isolated filesystems in one whole-disk image
"$S5" mkfs -d rp06 -P a "$T/w.dsk" >/dev/null 2>&1
"$S5" mkfs -d rp06 -P c "$T/w.dsk" >/dev/null 2>&1
wsz=$(wc -c < "$T/w.dsk")
"$S5" cp -d rp06 -P a "$T/w.dsk" @"$T/src" /marker >/dev/null 2>&1
in_a=$("$S5" ls -d rp06 -P a "$T/w.dsk" / 2>/dev/null | grep -c '^marker$')
in_c=$("$S5" ls -d rp06 -P c "$T/w.dsk" / 2>/dev/null | grep -c '^marker$')
if [ "$wsz" -eq $((340670 * 512)) ]; then ok "whole-disk image full size"; else no "whole-disk image full size"; fi
if [ "$in_a" -eq 1 ] && [ "$in_c" -eq 0 ]; then ok "partition isolation (a has marker, c does not)"; else no "partition isolation"; fi
if fsck_clean -d rp06 -P a "$T/w.dsk" && fsck_clean -d rp06 -P c "$T/w.dsk"; then ok "both partitions fsck clean"; else no "both partitions fsck clean"; fi

# 9. fixed VHD: wrap grows +512, info=fixed, tools read it, unwrap == original
cp "$T/a.dsk" "$T/v.dsk"; cp "$T/a.dsk" "$T/v.orig"
"$S5" vhd wrap "$T/v.dsk" "$T/v.vhd" >/dev/null 2>&1
if [ "$(($(wc -c < "$T/v.vhd") - $(wc -c < "$T/v.orig")))" -eq 512 ]; then ok "vhd wrap +512 footer"; else no "vhd wrap +512 footer"; fi
if "$S5" vhd info "$T/v.vhd" 2>/dev/null | grep -q fixed; then ok "vhd info = fixed"; else no "vhd info = fixed"; fi
if fsck_clean "$T/v.vhd"; then ok "tools read wrapped VHD"; else no "tools read wrapped VHD"; fi
"$S5" vhd unwrap "$T/v.vhd" "$T/v.back" >/dev/null 2>&1
if cmp -s "$T/v.orig" "$T/v.back"; then ok "vhd unwrap == original"; else no "vhd unwrap == original"; fi

# 10. byte order: little-endian image is created and read back clean
"$S5" mkfs -a le -d rl02 "$T/le.dsk" >/dev/null 2>&1
if fsck_clean "$T/le.dsk"; then ok "byte order le -> fsck clean"; else no "byte order le -> fsck clean"; fi

# 10b. 2048-byte blocks: create, round-trip a multi-block file, fsck
"$S5" mkfs -B 2048 -d rp06 "$T/b2k.dsk" >/dev/null 2>&1
head -c 200000 /dev/zero 2>/dev/null | tr '\0' 'Z' > "$T/big" 2>/dev/null
"$S5" put -B 2048 "$T/b2k.dsk" "$T/big" /big >/dev/null 2>&1
"$S5" get -B 2048 "$T/b2k.dsk" /big "$T/big.out" >/dev/null 2>&1
if cmp -s "$T/big" "$T/big.out" && fsck_clean -B 2048 "$T/b2k.dsk"; then ok "2048-byte blocks round-trip + fsck"; else no "2048-byte blocks round-trip + fsck"; fi

# 10c. oversize filesystem is refused, not silently truncated
if ! "$S5" mkfs -b 16777217 "$T/huge.dsk" >/dev/null 2>&1; then ok "reject > 2^24 blocks"; else no "reject > 2^24 blocks"; fi

# 10d. System V superblock flavor: FsMAGIC written, survives an rw round-trip
"$S5" mkfs -F sysv -a le -d rp06 "$T/sv.dsk" >/dev/null 2>&1   # 1024-byte blocks -> superblock @1024
m1=$(od -An -tx1 -j 1528 -N4 "$T/sv.dsk" | tr -d ' \n')        # s_magic @ 1024+504; LE fd187e20 = 20 7e 18 fd
"$S5" put "$T/sv.dsk" "$T/src" /f >/dev/null 2>&1
m2=$(od -An -tx1 -j 1528 -N4 "$T/sv.dsk" | tr -d ' \n')
"$S5" mkfs -a le -d rp06 "$T/v7.dsk" >/dev/null 2>&1
mv7=$(od -An -tx1 -j 1528 -N4 "$T/v7.dsk" | tr -d ' \n')
if [ "$m1" = 207e18fd ] && [ "$m2" = 207e18fd ] && [ "$mv7" = 00000000 ] && fsck_clean "$T/sv.dsk"; then
	ok "SysV superblock (FsMAGIC + rw round-trip; absent by default)"
else no "SysV superblock (m1=$m1 m2=$m2 v7=$mv7)"; fi

# 10e. fsdb inspects (sb/inode) read-only and edits (-w set) with fsck clean
"$S5" mkfs -d rl02 "$T/fdb.dsk" >/dev/null 2>&1
"$S5" put "$T/fdb.dsk" "$T/src" /f >/dev/null 2>&1
fino=$(printf 'path /f\nquit\n' | "$S5" fsdb "$T/fdb.dsk" 2>/dev/null | grep -oE 'inode [0-9]+' | awk '{print $2}')
sbok=$(printf 'sb\nquit\n' | "$S5" fsdb "$T/fdb.dsk" 2>/dev/null | grep -c 's_isize')
printf "set %s uid 55\nquit\n" "$fino" | "$S5" fsdb -w "$T/fdb.dsk" >/dev/null 2>&1
uid=$("$S5" ls -l "$T/fdb.dsk" /f 2>/dev/null | awk '{print $3}')
if [ "$sbok" = 1 ] && [ -n "$fino" ] && [ "$uid" = 55 ] && fsck_clean "$T/fdb.dsk"; then
	ok "fsdb inspect + edit (set uid) -> fsck clean"
else no "fsdb inspect + edit (sb=$sbok ino=$fino uid=$uid)"; fi

# 10f. manifest/verify: an image matches its own manifest; a change is caught
"$S5" mkfs -d rl02 "$T/mf.dsk" >/dev/null 2>&1
"$S5" put "$T/mf.dsk" "$T/src" /f >/dev/null 2>&1
"$S5" mkdir "$T/mf.dsk" /sub >/dev/null 2>&1
"$S5" manifest "$T/mf.dsk" > "$T/m.txt" 2>/dev/null
"$S5" verify "$T/mf.dsk" "$T/m.txt" >/dev/null 2>&1; v0=$?
"$S5" chmod "$T/mf.dsk" 700 /f >/dev/null 2>&1
"$S5" verify "$T/mf.dsk" "$T/m.txt" >/dev/null 2>&1; v1=$?
if [ "$v0" -eq 0 ] && [ "$v1" -ne 0 ]; then ok "manifest/verify (clean; catches a change)"; else no "manifest/verify (v0=$v0 v1=$v1)"; fi

# 10g. scavenge: a deleted name survives, and an a.out start is carved exactly
printf '\010\001\100\000\020\000\000\000\000\000\000\000\000\000\000\000' > "$T/prog"
head -c 80 /dev/zero 2>/dev/null | tr '\0' 'Z' >> "$T/prog"   # magic 0410, text=64 data=16 -> 96 bytes
"$S5" mkfs -d rl02 "$T/rc.dsk" >/dev/null 2>&1
"$S5" put "$T/rc.dsk" "$T/prog" /GONEPROG >/dev/null 2>&1
"$S5" rm "$T/rc.dsk" /GONEPROG >/dev/null 2>&1
rname=$("$S5" scavenge "$T/rc.dsk" 2>/dev/null | grep -c GONEPROG)
raout=$("$S5" scavenge "$T/rc.dsk" 2>/dev/null | grep -c 'a.out')
"$S5" scavenge -x "$T/rout" "$T/rc.dsk" >/dev/null 2>&1
rcarve=no; head -c 96 "$T"/rout/aout-* 2>/dev/null | cmp -s - "$T/prog" && rcarve=yes
if [ "$rname" -ge 1 ] && [ "$raout" -ge 1 ] && [ "$rcarve" = yes ]; then
	ok "scavenge (deleted name + a.out carved exactly)"
else no "scavenge (name=$rname aout=$raout carve=$rcarve)"; fi

# 10h. analysis bundle: df / quot / ncheck -s / du / labelit
"$S5" mkfs -d rl02 "$T/an.dsk" >/dev/null 2>&1
"$S5" put "$T/an.dsk" "$T/src" /f >/dev/null 2>&1
"$S5" chmod "$T/an.dsk" 4755 /f >/dev/null 2>&1
dfok=$("$S5" df "$T/an.dsk" 2>/dev/null | grep -c blocks)
qok=$("$S5" quot "$T/an.dsk" 2>/dev/null | grep -c kbytes)
sok=$("$S5" ncheck -s "$T/an.dsk" 2>/dev/null | grep -c '/f$')
duok=$("$S5" du "$T/an.dsk" 2>/dev/null | grep -c ' /$')
"$S5" labelit "$T/an.dsk" myvol >/dev/null 2>&1
lok=$("$S5" labelit "$T/an.dsk" 2>/dev/null | grep -c myvol)
if [ "$dfok" -ge 1 ] && [ "$qok" -ge 1 ] && [ "$sok" -eq 1 ] && [ "$duok" -ge 1 ] && [ "$lok" -ge 1 ] && fsck_clean "$T/an.dsk"; then
	ok "analysis (df/quot/ncheck -s/du/labelit) + fsck clean"
else no "analysis (df=$dfok quot=$qok ncheck=$sok du=$duok label=$lok)"; fi

# 11. interactive shell drives the engine
printf 'mkdir sub\ncd sub\nput %s g\nls\nquit\n' "$T/src" | "$S5" shell "$T/a.dsk" >/dev/null 2>&1
if "$S5" cat "$T/a.dsk" /sub/g 2>/dev/null | grep -q fox && fsck_clean "$T/a.dsk"; then ok "shell session -> fsck clean"; else no "shell session -> fsck clean"; fi

# 11b. shell multi-mount: cp a file between two different images
"$S5" mkfs -d rl02 "$T/ma.dsk" >/dev/null 2>&1
"$S5" mkfs -d rl02 "$T/mb.dsk" >/dev/null 2>&1
"$S5" put "$T/ma.dsk" "$T/src" /x >/dev/null 2>&1
printf 'mount %s /a\nmount %s /b\ncp /a/x /b/y\nquit\n' "$T/ma.dsk" "$T/mb.dsk" | "$S5" shell >/dev/null 2>&1
if "$S5" cat "$T/mb.dsk" /y 2>/dev/null | cmp -s - "$T/src" && fsck_clean "$T/ma.dsk" && fsck_clean "$T/mb.dsk"; then
	ok "shell multi-mount cross-image cp"
else no "shell multi-mount cross-image cp"; fi

# 12. name longer than 14 chars is REFUSED, not silently truncated.  Truncating
#     produced two entries with the same on-disk name, both unreachable under
#     the name the caller gave, on an image fsck still called clean.
"$S5" mkfs -d rl02 "$T/nm.dsk" >/dev/null 2>&1
"$S5" put "$T/nm.dsk" "$T/src" /averylongfilename_AAA >/dev/null 2>&1; long1=$?
"$S5" put "$T/nm.dsk" "$T/src" /averylongfilename_BBB >/dev/null 2>&1
"$S5" put "$T/nm.dsk" "$T/src" /exactly14char >/dev/null 2>&1; short=$?
ndup=$("$S5" ls "$T/nm.dsk" / 2>/dev/null | sort | uniq -d | wc -l)
if [ "$long1" -ne 0 ] && [ "$short" -eq 0 ] && [ "$ndup" -eq 0 ] && fsck_clean "$T/nm.dsk"; then
	ok "over-long name refused (no duplicate entries)"
else no "over-long name refused (long=$long1 short=$short dups=$ndup)"; fi

# 12b. mktree / tar x skip an unrepresentable name loudly instead of truncating
# two names sharing their first 14 characters: truncation makes them collide
mkdir -p "$T/lt" && : > "$T/lt/short.c"
: > "$T/lt/this_is_a_long_name_one.c" && : > "$T/lt/this_is_a_long_name_two.c"
"$S5" mktree -d rl02 "$T/lt" "$T/lt.dsk" >/dev/null 2>&1
ltn=$("$S5" ls "$T/lt.dsk" / 2>/dev/null | sort | uniq -d | wc -l)
lts=$("$S5" ls "$T/lt.dsk" / 2>/dev/null | grep -c '^short.c$')
if [ "$ltn" -eq 0 ] && [ "$lts" -eq 1 ] && fsck_clean "$T/lt.dsk"; then
	ok "mktree skips over-long names (no duplicates)"
else no "mktree skips over-long names (dups=$ltn short=$lts)"; fi

# 13. renaming a directory into its own subtree is refused (POSIX EINVAL).
#     It used to succeed and silently detach the whole subtree from the root.
"$S5" mkfs -d rl02 "$T/mv.dsk" >/dev/null 2>&1
"$S5" mkdir "$T/mv.dsk" /a >/dev/null 2>&1
"$S5" mkdir "$T/mv.dsk" /a/b >/dev/null 2>&1
"$S5" mv "$T/mv.dsk" /a /a/b/c >/dev/null 2>&1; mvrc=$?
still=$("$S5" ls "$T/mv.dsk" / 2>/dev/null | grep -c '^a$')
if [ "$mvrc" -ne 0 ] && [ "$still" -eq 1 ] && fsck_clean "$T/mv.dsk"; then
	ok "rename into own subtree refused"
else no "rename into own subtree refused (rc=$mvrc still=$still)"; fi

# 14. a directory CYCLE must not crash the recursive walkers.  Build a real one
#     with fsdb: point /p/q/loop's directory entry back at /p.  Before the fix
#     this segfaulted du, ncheck, manifest, fsck -l and tar c (stack exhausted).
"$S5" mkfs -d rl02 "$T/cy.dsk" >/dev/null 2>&1
"$S5" mkdir "$T/cy.dsk" /p >/dev/null 2>&1
"$S5" mkdir "$T/cy.dsk" /p/q >/dev/null 2>&1
"$S5" cp "$T/cy.dsk" @"$T/src" /p/q/loop >/dev/null 2>&1
cyp=$(printf 'path /p\nquit\n'   | "$S5" fsdb "$T/cy.dsk" 2>/dev/null | grep -oE 'inode [0-9]+' | awk '{print $2}')
cyq=$(printf 'path /p/q\nquit\n' | "$S5" fsdb "$T/cy.dsk" 2>/dev/null | grep -oE 'inode [0-9]+' | awk '{print $2}')
cyb=$(printf 'map %s\nquit\n' "$cyq" | "$S5" fsdb "$T/cy.dsk" 2>/dev/null | awk '/-> /{print $3; exit}')
if [ -n "$cyp" ] && [ -n "$cyb" ]; then
	cylo=$(printf '%02x' $((cyp & 255))); cyhi=$(printf '%02x' $((cyp / 256)))
	printf 'poke %s 32 %s %s\nquit\n' "$cyb" "$cylo" "$cyhi" | "$S5" fsdb -w "$T/cy.dsk" >/dev/null 2>&1
	cyloop=$(printf 'dir %s\nquit\n' "$cyq" | "$S5" fsdb "$T/cy.dsk" 2>/dev/null | grep -c "  *2  *$cyp  *loop")
	cycrash=0
	for cmd in du ncheck manifest quot; do
		tmo 20 "$S5" $cmd "$T/cy.dsk" >/dev/null 2>&1
		[ $? -gt 128 ] && cycrash=$((cycrash + 1))
	done
	tmo 20 "$S5" tar c "$T/cy.dsk" "$T/cy.tar" >/dev/null 2>&1
	[ $? -gt 128 ] && cycrash=$((cycrash + 1))
	tmo 20 "$S5" fsck -l "$T/cy.dsk" >/dev/null 2>&1
	[ $? -gt 128 ] && cycrash=$((cycrash + 1))
	if [ "$cyloop" -eq 1 ] && [ "$cycrash" -eq 0 ]; then
		ok "directory cycle does not crash the walkers"
	else no "directory cycle (built=$cyloop crashed=$cycrash)"; fi
else no "directory cycle (could not build the fixture)"; fi

# 15. tar x honours 2048-byte blocks (it was the one command hardcoding 512/1024)
"$S5" tar c "$T/a.dsk" "$T/b2.tar" >/dev/null 2>&1
"$S5" tar x -B 2048 -d rp06 "$T/b2.tar" "$T/b2.dsk" >/dev/null 2>&1
if fsck_clean -B 2048 "$T/b2.dsk"; then ok "tar x -B 2048"; else no "tar x -B 2048"; fi

# 16. a file large enough to reach the DOUBLE-indirect tail must round-trip.
#     s5fs_setblocks lays single/double/triple out sequentially, but every
#     reader ended the double range at nindir^2 instead of nindir + nindir^2,
#     so the last nindir blocks of it were looked up in the TRIPLE indirect.
#     At 512-byte blocks that is byte 8393728 onward -- a file written by
#     mktree/tar x/restore read back corrupt from there, and fsck stayed clean.
#     Content must vary with position or a misrouted block reads the same.
mkdir -p "$T/bigtr" && seq 1 1500000 > "$T/bigtr/big"      # ~10.4 MB > the 8.45 MB boundary
"$S5" mktree -B 512 -b 40000 "$T/bigtr" "$T/bg.dsk" >/dev/null 2>&1
"$S5" get -B 512 "$T/bg.dsk" /big "$T/bg.out" >/dev/null 2>&1
"$S5" cat -B 512 "$T/bg.dsk" /big > "$T/bg.cat" 2>/dev/null
if cmp -s "$T/bigtr/big" "$T/bg.out" && cmp -s "$T/bigtr/big" "$T/bg.cat" &&
   fsck_clean -B 512 "$T/bg.dsk"; then
	ok "double-indirect tail round-trips (get + cat)"
else no "double-indirect tail round-trips"; fi

# 16b. and through the archive/tape front-ends, which use the same map
"$S5" tar c -B 512 "$T/bg.dsk" "$T/bg.tar" >/dev/null 2>&1
"$S5" tar x -B 512 -b 40000 "$T/bg.tar" "$T/bg2.dsk" >/dev/null 2>&1
"$S5" get -B 512 "$T/bg2.dsk" /big "$T/bg2.out" >/dev/null 2>&1
"$S5" dump -B 512 "$T/bg.dsk" "$T/bg.dmp" >/dev/null 2>&1
"$S5" restore -B 512 -b 40000 "$T/bg.dmp" "$T/bg3.dsk" >/dev/null 2>&1
"$S5" get -B 512 "$T/bg3.dsk" /big "$T/bg3.out" >/dev/null 2>&1
if cmp -s "$T/bigtr/big" "$T/bg2.out" && cmp -s "$T/bigtr/big" "$T/bg3.out"; then
	ok "large file survives tar and dump round-trips"
else no "large file survives tar and dump round-trips"; fi

# 17. inode numbers are 16-bit on disk, so more than 65535 of them cannot be
#     expressed: number 65536 wraps to 0 and itod(0) lands on block 1 -- the
#     SUPERBLOCK.  mktree used to write such a tree happily and report success
#     while destroying the filesystem (root unreadable, every inode orphaned).
#     mkfs -i drives the same guard without materialising 65k files.
"$S5" mkfs -b 200000 -i 70000 "$T/ino.dsk" >/dev/null 2>&1; inobad=$?
"$S5" mkfs -b 200000 -i 65000 "$T/ino2.dsk" >/dev/null 2>&1; inook=$?
if [ "$inobad" -ne 0 ] && [ "$inook" -eq 0 ] && fsck_clean "$T/ino2.dsk"; then
	ok "more than 65535 inodes refused"
else no "more than 65535 inodes refused (over=$inobad under=$inook)"; fi

# 18. a free-list chain block that points at ITSELF must not spin the tools.
#     fsck, icheck, df and scavenge all walk the chain, and all four looped
#     forever -- on exactly the damaged images those tools exist to inspect.
#     (fsdb rewrites the superblock on exit, so corrupt the chain block the
#     superblock already points at rather than the superblock itself.)
#     Hex is decoded in awk: "16#nn" is a bashism and this suite runs under sh.
"$S5" mkfs -a le -d rl02 "$T/fl.dsk" >/dev/null 2>&1
flb=$(printf 'block 1\nquit\n' | "$S5" fsdb "$T/fl.dsk" 2>/dev/null | awk '
	/^  0000 / {
		for (i = 0; i < 16; i++) V[sprintf("%x", i)] = i
		lo = V[substr($10, 1, 1)] * 16 + V[substr($10, 2, 1)]
		hi = V[substr($11, 1, 1)] * 16 + V[substr($11, 2, 1)]
		print hi * 256 + lo
		exit
	}')
if [ -n "$flb" ] && [ "$flb" -gt 0 ]; then
	fllo=$(printf '%02x' $((flb % 256))); flhi=$(printf '%02x' $((flb / 256)))
	printf 'poke %s 0 01 00\npoke %s 2 %s %s 00 00\nquit\n' "$flb" "$flb" "$fllo" "$flhi" |
		"$S5" fsdb -w "$T/fl.dsk" >/dev/null 2>&1
	flhang=0
	for cmd in scavenge df icheck fsck; do
		tmo 20 "$S5" $cmd "$T/fl.dsk" >/dev/null 2>&1
		[ $? -eq 124 ] && flhang=$((flhang + 1))
	done
	flsaw=$(tmo 20 "$S5" fsck "$T/fl.dsk" 2>&1 | grep -c 'free list loops')
	tmo 20 "$S5" fsck -p "$T/fl.dsk" >/dev/null 2>&1     # must be able to repair it
	if [ "$flhang" -eq 0 ] && [ "$flsaw" -ge 1 ] && fsck_clean "$T/fl.dsk"; then
		ok "looping free list is diagnosed and repaired"
	else no "looping free list (hung=$flhang diagnosed=$flsaw)"; fi
else no "looping free list (could not build the fixture)"; fi

# 19. chmod/chown/chgrp took their arguments through bare strtol, so anything
#     non-numeric silently became 0: `chmod rwx` stripped every permission bit
#     and `chown root` set uid 0, both reporting success.  They must refuse.
"$S5" mkfs -d rl02 "$T/vd.dsk" >/dev/null 2>&1
"$S5" put "$T/vd.dsk" "$T/src" /f >/dev/null 2>&1
"$S5" chmod "$T/vd.dsk" 644 /f >/dev/null 2>&1
"$S5" chmod "$T/vd.dsk" rwx /f >/dev/null 2>&1;      vbad1=$?
"$S5" chown "$T/vd.dsk" root /f >/dev/null 2>&1;     vbad2=$?
"$S5" chgrp "$T/vd.dsk" staff /f >/dev/null 2>&1;    vbad3=$?
vmode=$("$S5" ls -l "$T/vd.dsk" /f 2>/dev/null | awk '{print $1}')
"$S5" chmod "$T/vd.dsk" 700 /f >/dev/null 2>&1;      vok1=$?
"$S5" chown "$T/vd.dsk" 55:66 /f >/dev/null 2>&1;    vok2=$?
vuid=$("$S5" ls -l "$T/vd.dsk" /f 2>/dev/null | awk '{print $3}')
if [ "$vbad1" -ne 0 ] && [ "$vbad2" -ne 0 ] && [ "$vbad3" -ne 0 ] &&
   [ "$vmode" = "-rw-r--r--" ] && [ "$vok1" -eq 0 ] && [ "$vok2" -eq 0 ] &&
   [ "$vuid" = 55 ] && fsck_clean "$T/vd.dsk"; then
	ok "chmod/chown/chgrp reject non-numeric arguments"
else no "chmod/chown/chgrp reject non-numeric (rc=$vbad1/$vbad2/$vbad3 mode=$vmode uid=$vuid)"; fi

# 20. a crafted dump tape must not read past the record buffer.  c_count is
#     taken straight off the tape and used to index the c_addr flags INSIDE the
#     record; with c_count=65535 and an inflated di_size (so the b<nblk guard
#     does not stop it first) restore read ~64 KB past a 2048-byte stack array.
#     An over-read does not fault on a normal build, so this check only has
#     teeth under `make test-san` -- here it just proves restore still copes.
"$S5" mkfs -B 512 -b 4000 "$T/tp.dsk" >/dev/null 2>&1
"$S5" put -B 512 "$T/tp.dsk" "$T/src" /f >/dev/null 2>&1
"$S5" dump -B 512 "$T/tp.dsk" "$T/tp.dump" >/dev/null 2>&1
tpoff=""; tpi=0; tpn=$(( $(wc -c < "$T/tp.dump") / 512 ))
while [ "$tpi" -lt "$tpn" ]; do
	o=$((tpi * 512))
	ty=$(od -An -tu2 -j $o          -N2 "$T/tp.dump" | tr -d ' ')
	mg=$(od -An -tu2 -j $((o + 18)) -N2 "$T/tp.dump" | tr -d ' ')
	md=$(od -An -tu2 -j $((o + 22)) -N2 "$T/tp.dump" | tr -d ' ')
	if [ "$ty" = 2 ] && [ "$mg" = 60011 ] && [ $((md / 4096)) -eq 8 ]; then tpoff=$o; break; fi
	tpi=$((tpi + 1))
done
if [ -n "$tpoff" ]; then
	cp "$T/tp.dump" "$T/tp.bad"
	printf '\377\377' | dd of="$T/tp.bad" bs=1 seek=$((tpoff + 86)) conv=notrunc 2>/dev/null
	printf '\000\001\000\000' | dd of="$T/tp.bad" bs=1 seek=$((tpoff + 30)) conv=notrunc 2>/dev/null
	tmo 30 "$S5" restore -B 512 -b 4000 "$T/tp.bad" "$T/tp.out" >/dev/null 2>&1
	tprc=$?
	if [ "$tprc" -le 128 ] && [ "$tprc" -ne 124 ]; then
		ok "crafted dump tape does not overrun the record buffer"
	else no "crafted dump tape (exit $tprc)"; fi
else no "crafted dump tape (could not find a TS_INODE record)"; fi

# 21. the device spec file is user input and was parsed loosely: an empty
#     [] name, a non-numeric blocks value, partition letters outside a..h and
#     out-of-range start/length were all accepted silently.  Worst, an
#     unterminated "[section" was skipped and its keys then applied to the
#     PREVIOUS device -- so a typo silently rewrote a different disk.
cat > "$T/bad.ini" <<'INI'
[]
blocks = 100
[good]
blocks = 4000
partitions = a:0:2000 z:0:10 b:2000:2000
[victim]
blocks = 777
[unterminated
blocks = 5
INI
S5FS_DEVICES="$T/bad.ini" "$S5" devices >"$T/ini.out" 2>"$T/ini.err"
inivic=$(awk '$1 == "victim" {print $2}' "$T/ini.out")     # must keep its own 777
inibad=$(grep -c "not a..h\|empty device name\|missing ']'" "$T/ini.err")
iniprt=$("$S5" devices good 2>/dev/null | grep -cE '^   [ab] ')
S5FS_DEVICES="$T/bad.ini" "$S5" mkfs -d good "$T/ini.dsk" >/dev/null 2>&1; inimk=$?
if [ "$inivic" = 777 ] && [ "$inibad" -ge 3 ] && [ "$inimk" -eq 0 ] && fsck_clean "$T/ini.dsk"; then
	ok "device spec rejects malformed entries (and keys do not leak between sections)"
else no "device spec validation (victim=$inivic diags=$inibad mkfs=$inimk)"; fi

# 22. `boot` wrote the WHOLE bootfile at offset 0, up to 2048 bytes.  Block 0 is
#     one 512-byte sector and on a 512-byte filesystem the superblock starts at
#     byte 512, so an oversized bootfile overwrote the superblock and the head
#     of the i-list -- while printing a warning saying the excess merely would
#     not be *loaded*, and then reporting success.  It must clamp to one sector.
"$S5" mkfs -B 512 -b 4000 "$T/bt.dsk" >/dev/null 2>&1
"$S5" put -B 512 "$T/bt.dsk" "$T/src" /f >/dev/null 2>&1
head -c 2000 /dev/zero 2>/dev/null | tr '\0' 'B' > "$T/bigboot"
"$S5" boot "$T/bt.dsk" "$T/bigboot" >/dev/null 2>&1; btrc=$?
btcat=$("$S5" cat -B 512 "$T/bt.dsk" /f 2>/dev/null | grep -c fox)
btb0=$(od -An -c -N4 "$T/bt.dsk" | tr -d ' \n')          # block 0 still got the code
if [ "$btrc" -eq 0 ] && [ "$btcat" -eq 1 ] && [ "$btb0" = "BBBB" ] &&
   fsck_clean -B 512 "$T/bt.dsk"; then
	ok "boot clamps an oversized bootfile to block 0"
else no "boot clamps oversized bootfile (rc=$btrc cat=$btcat b0=$btb0)"; fi

# 23. s_nfree is read off the disk and then used directly as an index into a
#     50-entry free-block cache.  Corruption fuzzing found fsck/icheck indexing
#     free_[223] of an int32_t[50] (UBSan: "index 223 out of bounds") -- that
#     array is a local, so it is a real stack over-read.  Only test-san can see
#     it; on a plain build this check just proves the tools still cope.
"$S5" mkfs -B 512 -b 3000 "$T/nf.dsk" >/dev/null 2>&1
"$S5" put -B 512 "$T/nf.dsk" "$T/src" /f >/dev/null 2>&1
# s_nfree sits at byte 512+6; leave isize/fsize valid so the image still opens
printf '\310\000' | dd of="$T/nf.dsk" bs=1 seek=518 conv=notrunc 2>/dev/null
nfsaw=$(od -An -tu2 -j 518 -N2 "$T/nf.dsk" | tr -d ' ')
nfbad=0
for cmd in fsck icheck df scavenge du; do
	tmo 20 "$S5" $cmd -B 512 "$T/nf.dsk" >/dev/null 2>&1
	rc=$?
	{ [ $rc -gt 128 ] || [ $rc -eq 124 ]; } && nfbad=$((nfbad + 1))
done
tmo 20 "$S5" mkdir -B 512 "$T/nf.dsk" /zz >/dev/null 2>&1
[ $? -gt 128 ] && nfbad=$((nfbad + 1))
if [ "$nfsaw" = 200 ] && [ "$nfbad" -eq 0 ] && tmo 30 "$S5" fsck -p -B 512 "$T/nf.dsk" >/dev/null 2>&1 &&
   fsck_clean -B 512 "$T/nf.dsk"; then
	ok "out-of-range s_nfree does not walk off the free-block cache"
else no "out-of-range s_nfree (nfree=$nfsaw bad=$nfbad)"; fi

# 24. restore now verifies the per-record checksum that dump writes and native
#     restor(8) checks.  A damaged tape used to be parsed as though sound: the
#     counts and offsets from a corrupt record drove everything downstream.
"$S5" mkfs -B 512 -b 4000 "$T/ck.dsk" >/dev/null 2>&1
"$S5" put -B 512 "$T/ck.dsk" "$T/src" /f >/dev/null 2>&1
"$S5" dump -B 512 "$T/ck.dsk" "$T/ck.dump" >/dev/null 2>&1
"$S5" restore -B 512 -b 4000 "$T/ck.dump" "$T/ck.ok" >/dev/null 2>&1; ckok=$?
cp "$T/ck.dump" "$T/ck.bad"
printf '\052' | dd of="$T/ck.bad" bs=1 seek=$((5 * 512 + 40)) conv=notrunc 2>/dev/null
"$S5" restore -B 512 -b 4000 "$T/ck.bad" "$T/ck.b1" >/dev/null 2>&1;    ckbad=$?
cksaw=$("$S5" restore -B 512 -b 4000 "$T/ck.bad" "$T/ck.b1" 2>&1 | grep -c 'bad record checksum')
"$S5" restore -f -B 512 -b 4000 "$T/ck.bad" "$T/ck.b2" >/dev/null 2>&1; ckf=$?
ckfok=0; fsck_clean -B 512 "$T/ck.b2" && ckfok=1
if [ "$ckok" -eq 0 ] && [ "$ckbad" -ne 0 ] && [ "$cksaw" -ge 1 ] &&
   [ "$ckf" -ne 0 ] && [ "$ckfok" -eq 1 ] && fsck_clean -B 512 "$T/ck.ok"; then
	ok "restore verifies record checksums (-f salvages anyway)"
else no "restore checksums (ok=$ckok bad=$ckbad saw=$cksaw force=$ckf forceclean=$ckfok)"; fi

# 25. mktree preserved regular files' times but stamped every DIRECTORY with the
#     filesystem's creation time, so a tree built from a 1980s source came back
#     with its files dated correctly and every directory dated today.
#     Compared RELATIVELY, not against a fixed epoch: `touch -t` reads local
#     time, so a hardcoded value passes in one timezone and fails in another.
#     What matters is that host entries share the HOST's time and differ from
#     the filesystem's, which -t pins to a known value.
#     Manifest fields: type mode uid gid size mtime cksum path.
mkdir -p "$T/tt/old/deep"
echo hello > "$T/tt/old/f"
touch -t 198406151200 "$T/tt/old/deep" "$T/tt/old/f" "$T/tt/old" "$T/tt"
"$S5" mktree -d rl02 -t 500000000 "$T/tt" "$T/tt.dsk" >/dev/null 2>&1
ttm() { "$S5" manifest "$T/tt.dsk" 2>/dev/null | awk -v p="$1" '$8 == p {print $6}'; }
ttfile=$(ttm /old/f)
ttroot=$(ttm /); ttdir=$(ttm /old); ttdeep=$(ttm /old/deep); ttlf=$(ttm /lost+found)
if [ -n "$ttfile" ] && [ "$ttfile" -lt 600000000 ] &&
   [ "$ttroot" = "$ttfile" ] && [ "$ttdir" = "$ttfile" ] && [ "$ttdeep" = "$ttfile" ] &&
   [ "$ttlf" = 500000000 ] && [ "$ttfile" != 500000000 ] && fsck_clean "$T/tt.dsk"; then
	ok "mktree preserves host directory times"
else no "mktree preserves directory times (root=$ttroot dir=$ttdir deep=$ttdeep file=$ttfile lf=$ttlf)"; fi

echo "------------------------------------------------------------"
echo "PASS $pass   FAIL $fail"
[ "$fail" -eq 0 ]

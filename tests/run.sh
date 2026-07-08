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

# 11. interactive shell drives the engine
printf 'mkdir sub\ncd sub\nput %s g\nls\nquit\n' "$T/src" | "$S5" shell "$T/a.dsk" >/dev/null 2>&1
if "$S5" cat "$T/a.dsk" /sub/g 2>/dev/null | grep -q fox && fsck_clean "$T/a.dsk"; then ok "shell session -> fsck clean"; else no "shell session -> fsck clean"; fi

echo "------------------------------------------------------------"
echo "PASS $pass   FAIL $fail"
[ "$fail" -eq 0 ]

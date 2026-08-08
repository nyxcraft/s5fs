/*
 * device.c -- PDP-11/VAX disk geometry table.  See device.h.
 *
 * A built-in table of known DEC disks (capacities in 512-byte blocks, cross-
 * checked between the BSD drivers in sys/dev and SIMH's pdp11 disk models),
 * plus an optional user INI file that adds or overrides entries so people can
 * describe site controllers / custom SMD packs / homebrew SIMH devices without
 * recompiling.  The file is read from $S5FS_DEVICES, else ~/.config/s5fs/devices.
 *
 *   # capacities are 512-byte blocks
 *   [mysmd]
 *   blocks = 500384
 *   desc   = custom SMD pack
 *   since  = v7        ; optional advisory range (defaults v7..2.10)
 *   until  = 2.10
 *
 * The since/until tags are a deliberately loose advisory (see device.h): they
 * reflect roughly when each controller's driver appears in the V7->2.10 s5fs
 * family, not a guarantee about any particular kernel.
 */

#define _POSIX_C_SOURCE 200809L

#include "device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

/* Standard partition tables, straight from sys/GENERIC/ioconf.c, with the
 * cylinder offsets pre-multiplied by that driver's blocks-per-cylinder so the
 * numbers here are absolute 512-byte-block (start, length) windows.  Overlap is
 * intentional: a/b/c tile the platter, g/h alias the whole drive. */
#define NP(a) ((int)(sizeof(a) / sizeof(a)[0]))

/* clang-format off */	/* disk + partition tables: columns are the geometry */
static const disk_part hk_parts[] = {		/* RK06/RK07, 66 blk/cyl */
	{'a',     0,  5940}, {'b',  5940,  2376}, {'c',  8316, 45474},
	{'d',  8316, 18810}, {'g',     0, 27126}, {'h',     0, 53790},
};
static const disk_part rp_parts[] = {		/* RP03, 200 blk/cyl */
	{'a',     0, 10400}, {'b', 10400,  5200}, {'c', 15600, 67580}, {'h', 0, 83180},
};
static const disk_part hp_parts[] = {		/* RP04/05/06, 418 blk/cyl */
	{'a',      0,   9614}, {'b',   9614,   8778}, {'c',  18392, 153406},
	{'d', 171798, 168872}, {'e',  18392, 322278},
	{'g',      0, 171798}, {'h',      0, 340670},
};
static const disk_part rm_parts[] = {		/* RM02/03, 160 blk/cyl */
	{'a',     0,   4800}, {'b',  4800,   4800}, {'c',  9600, 122080},
	{'d',  9600,  62720}, {'e', 72320,  59360}, {'h',     0, 131680},
};
static const disk_part rm5_parts[] = {		/* RM05, 608 blk/cyl */
	{'a',      0,   9120}, {'b',   9120,   9120}, {'c',  18240, 234080},
	{'d', 252320, 248064}, {'e',  18240, 164160}, {'f', 182400, 152000},
	{'g', 334400, 165376}, {'h',      0, 500384},
};

static const disk_dev builtin[] = {
	/* name     description                                blocks   since    until */
	/* name     description                                blocks   since    until    parts  nparts */
	/* --- RK11 / RK05 (removable cartridge; whole disk = one fs) --- */
	{ "rk05", "RK05 cartridge (RK11)",                       4872, REL_V7,  REL_210, NULL, 0 },
	/* --- RL11 / RL01, RL02 (whole disk = one fs) --- */
	{ "rl01", "RL01 cartridge (RL11)",                      10240, REL_V7,  REL_210, NULL, 0 },
	{ "rl02", "RL02 cartridge (RL11)",                      20480, REL_V7,  REL_210, NULL, 0 },
	/* --- RK611 / RK06, RK07 --- */
	{ "rk06", "RK06 disk (RK611)",                          27126, REL_28,  REL_210, hk_parts, NP(hk_parts) },
	{ "rk07", "RK07 disk (RK611)",                          53790, REL_28,  REL_210, hk_parts, NP(hk_parts) },
	/* --- RP11: RP03 --- */
	{ "rp03", "RP03 pack (RP11)",                           83180, REL_V7,  REL_210, rp_parts, NP(rp_parts) },
	/* --- RH11/RH70 Massbus: RM02/03, RP04/05/06, RM05/80, RP07 --- */
	{ "rm03", "RM03 pack (Massbus)",                       131680, REL_V7,  REL_210, rm_parts,  NP(rm_parts)  },
	{ "rp04", "RP04 pack (Massbus)",                       171798, REL_V7,  REL_210, hp_parts,  NP(hp_parts)  },
	{ "rp05", "RP05 pack (Massbus)",                       171798, REL_V7,  REL_210, hp_parts,  NP(hp_parts)  },
	{ "rm80", "RM80 disk (Massbus)",                       242606, REL_V7,  REL_210, NULL, 0 },
	{ "rp06", "RP06 pack (Massbus)",                       340670, REL_V7,  REL_210, hp_parts,  NP(hp_parts)  },
	{ "rm05", "RM05 pack (Massbus)",                       500384, REL_V7,  REL_210, rm5_parts, NP(rm5_parts) },
	{ "rp07", "RP07 disk (Massbus)",                      1008000, REL_V7,  REL_210, NULL, 0 },
	/* --- MSCP (UDA50/RQDX): RD winchesters, RA packs, RX50 floppy --- */
	{ "rx50", "RX50 floppy (MSCP)",                           800, REL_210, REL_210, NULL, 0 },
	{ "rd51", "RD51 winchester (MSCP)",                     21600, REL_210, REL_210, NULL, 0 },
	{ "rd31", "RD31 winchester (MSCP)",                     41560, REL_210, REL_210, NULL, 0 },
	{ "rd52", "RD52 winchester (MSCP)",                     60480, REL_210, REL_210, NULL, 0 },
	{ "rd53", "RD53 winchester (MSCP)",                    138672, REL_210, REL_210, NULL, 0 },
	{ "rd54", "RD54 winchester (MSCP)",                    311200, REL_210, REL_210, NULL, 0 },
	{ "ra60", "RA60 pack (MSCP)",                          400176, REL_210, REL_210, NULL, 0 },
	{ "ra70", "RA70 disk (MSCP)",                          547041, REL_210, REL_210, NULL, 0 },
	{ "ra80", "RA80 disk (MSCP)",                          237212, REL_210, REL_210, NULL, 0 },
	{ "ra81", "RA81 disk (MSCP)",                          891072, REL_210, REL_210, NULL, 0 },
	{ "ra82", "RA82 disk (MSCP)",                         1216665, REL_210, REL_210, NULL, 0 },
	{ "ra90", "RA90 disk (MSCP)",                         2376153, REL_210, REL_210, NULL, 0 },
	{ "ra92", "RA92 disk (MSCP)",                         2940951, REL_210, REL_210, NULL, 0 },
};
/* clang-format on */
#define NBUILTIN ((int)(sizeof builtin / sizeof builtin[0]))

/* runtime table = built-ins, then user file merged on top */
static disk_dev *table;
static int ntable;
static int nuser;	  /* how many entries came from a file */
static char srcpath[512]; /* the user file we loaded, if any */
static int loaded;

static const char *const relnames[] = {"v7", "2.8", "2.9", "2.10"};

const char *
release_name(bsd_rel r)
{
	if (r < REL_V7 || r > REL_210)
		return "?";
	return relnames[r];
}

bsd_rel
release_parse(const char *s)
{
	if (!s)
		return REL_NONE;
	if (!strcasecmp(s, "v7") || !strcasecmp(s, "7"))
		return REL_V7;
	if (!strncmp(s, "2.8", 3))
		return REL_28;
	if (!strncmp(s, "2.9", 3))
		return REL_29;
	if (!strncmp(s, "2.10", 4))
		return REL_210;
	return REL_NONE;
}

/* ---- INI loading -------------------------------------------------- */

static char *
trim(char *s)
{
	char *e;
	while (*s && isspace((unsigned char)*s))
		s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = '\0';
	return s;
}

/* add `d` to the runtime table: override an existing entry by name, else append */
static void
merge(const disk_dev *d)
{
	int i;
	for (i = 0; i < ntable; i++)
		if (!strcasecmp(table[i].name, d->name)) {
			table[i] = *d;
			return;
		}
	table = realloc(table, (size_t)(ntable + 1) * sizeof *table);
	if (!table) {
		perror("s5fs: realloc");
		exit(1);
	}
	table[ntable++] = *d;
}

static void
commit(disk_dev *cur, int *have)
{
	if (!*have)
		return;
	*have = 0;
	if (cur->blocks == 0) {
		fprintf(stderr, "s5fs: device '%s' in spec has no 'blocks' -- ignored\n",
			cur->name ? cur->name : "?");
		return;
	}
	if (!cur->desc)
		cur->desc = "(user-defined)";
	merge(cur);
	nuser++; /* counts overrides and additions alike */
}

static void
load_user(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[512];
	disk_dev cur;
	int have = 0;

	if (!f)
		return;
	snprintf(srcpath, sizeof srcpath, "%s", path);
	memset(&cur, 0, sizeof cur);

	while (fgets(line, sizeof line, f)) {
		char *p = trim(line);
		if (*p == '\0' || *p == '#' || *p == ';')
			continue; /* blank / full-line comment */
		if (*p == '[') {  /* [devname] */
			char *end = strchr(p, ']');
			if (!end)
				continue;
			*end = '\0';
			commit(&cur, &have);
			memset(&cur, 0, sizeof cur);
			cur.name = strdup(trim(p + 1));
			cur.since = REL_V7; /* default: never warns */
			cur.until = REL_210;
			have = 1;
		}
		else { /* key = value */
			char *eq = strchr(p, '=');
			char *key, *val, *sp;
			bsd_rel r;
			if (!eq || !have)
				continue;
			*eq = '\0';
			key = trim(p);
			val = trim(eq + 1);
			if (!strcasecmp(key, "blocks")) {
				cur.blocks = (uint32_t)strtoul(val, NULL, 0);
			}
			else if (!strcasecmp(key, "desc")) {
				cur.desc = strdup(val);
			}
			else if (!strcasecmp(key, "since") || !strcasecmp(key, "until")) {
				for (sp = val; *sp && !isspace((unsigned char)*sp); sp++)
					;
				*sp = '\0'; /* first token (drop inline comment) */
				r = release_parse(val);
				if (r != REL_NONE) {
					if (!strcasecmp(key, "since"))
						cur.since = r;
					else
						cur.until = r;
				}
			}
			else if (!strcasecmp(key, "partitions")) {
				/* letter:start:len ... (space/comma separated, 512-blocks) */
				disk_part *pv = NULL;
				int pn = 0;
				char *tok, *tsv;
				for (tok = strtok_r(val, " \t,", &tsv); tok; tok = strtok_r(NULL, " \t,", &tsv)) {
					char L;
					unsigned long st, ln;
					if (sscanf(tok, "%c:%lu:%lu", &L, &st, &ln) != 3)
						continue;
					pv = realloc(pv, (size_t)(pn + 1) * sizeof *pv);
					if (!pv) {
						perror("s5fs: realloc");
						exit(1);
					}
					pv[pn].letter = L;
					pv[pn].start = (uint32_t)st;
					pv[pn].len = (uint32_t)ln;
					pn++;
				}
				cur.parts = pv;
				cur.nparts = pn;
			}
			/* unknown keys ignored -- forward-compat (sectors/tracks/...) */
		}
	}
	commit(&cur, &have);
	fclose(f);
}

static void
ensure_loaded(void)
{
	const char *env, *home;
	char path[512];

	if (loaded)
		return;
	loaded = 1;
	table = malloc(sizeof builtin);
	if (!table) {
		perror("s5fs: malloc");
		exit(1);
	}
	memcpy(table, builtin, sizeof builtin);
	ntable = NBUILTIN;

	env = getenv("S5FS_DEVICES");
	if (env && *env) {
		load_user(env);
	}
	else if ((home = getenv("HOME")) != NULL) {
		snprintf(path, sizeof path, "%s/.config/s5fs/devices", home);
		load_user(path);
	}
}

/* ---- public API --------------------------------------------------- */

const disk_dev *
device_find(const char *name)
{
	int i;
	ensure_loaded();
	for (i = 0; i < ntable; i++)
		if (!strcasecmp(name, table[i].name))
			return &table[i];
	return NULL;
}

int
device_partition(const disk_dev *dev, char letter, uint32_t *start, uint32_t *len)
{
	int i;
	if (!dev || !dev->parts)
		return -1;
	for (i = 0; i < dev->nparts; i++)
		if (dev->parts[i].letter == letter) {
			*start = dev->parts[i].start;
			*len = dev->parts[i].len;
			return 0;
		}
	return -1;
}

int
device_resolve_part(const char *devname, char letter, const char *ospec,
		    long long *base_bytes, uint32_t *len512)
{
	*base_bytes = 0;
	*len512 = 0;
	if (ospec) { /* raw START[:LEN] in 512-byte blocks */
		char *colon;
		unsigned long st = strtoul(ospec, &colon, 0), ln = 0;
		if (*colon == ':')
			ln = strtoul(colon + 1, NULL, 0);
		*base_bytes = (long long)st * 512;
		*len512 = (uint32_t)ln;
		return 0;
	}
	if (letter) { /* -P <letter> needs a device table */
		const disk_dev *dev;
		uint32_t s, l;
		if (!devname) {
			fprintf(stderr, "s5fs: -P <partition> needs -d <device>\n");
			return -1;
		}
		dev = device_find(devname);
		if (!dev) {
			fprintf(stderr, "s5fs: unknown device '%s'\n", devname);
			return -1;
		}
		if (device_partition(dev, letter, &s, &l) < 0) {
			fprintf(stderr, "s5fs: %s has no partition '%c' "
					"(try `s5fs devices %s`)\n",
				devname, letter, devname);
			return -1;
		}
		*base_bytes = (long long)s * 512;
		*len512 = l;
	}
	return 0; /* neither -> whole image (base 0) */
}

void
device_show_parts(const char *name)
{
	const disk_dev *dev = device_find(name);
	int i;
	if (!dev) {
		fprintf(stderr, "s5fs: unknown device '%s'\n", name);
		return;
	}
	if (!dev->parts || dev->nparts == 0) {
		printf("%s (%u blocks): no partition table -- the whole disk is one "
		       "filesystem (no -P needed)\n",
		       dev->name, dev->blocks);
		return;
	}
	printf("%s -- %s\npartitions (512-byte blocks):\n", dev->name, dev->desc);
	printf("  %-4s %10s %10s %10s\n", "part", "start", "length", "size");
	for (i = 0; i < dev->nparts; i++) {
		const disk_part *p = &dev->parts[i];
		printf("   %c   %10u %10u %8.1f MB\n",
		       p->letter, p->start, p->len, p->len / 2048.0);
	}
	printf("(overlap is normal: a/b/c tile the disk, g/h alias the whole drive)\n");
}

void
device_list(void)
{
	int i;
	ensure_loaded();
	printf("%-8s %10s  %-8s  %s\n", "device", "blocks", "drivers", "description");
	for (i = 0; i < ntable; i++) {
		char range[16];
		if (table[i].since == table[i].until)
			snprintf(range, sizeof range, "%s", release_name(table[i].since));
		else
			snprintf(range, sizeof range, "%s-%s",
				 release_name(table[i].since), release_name(table[i].until));
		printf("%-8s %10u  %-8s  %s\n",
		       table[i].name, table[i].blocks, range, table[i].desc);
	}
	if (srcpath[0])
		printf("(%d entr%s from %s; overrides win by name)\n",
		       nuser, nuser == 1 ? "y" : "ies", srcpath);
}

int
device_advise(const disk_dev *dev, bsd_rel target)
{
	if (target == REL_NONE || !dev)
		return 0;
	if (target < dev->since) {
		fprintf(stderr,
			"s5fs: note: %s's controller isn't usually in a %s kernel "
			"(it appears around %s). Fine if yours has the driver "
			"(e.g. backported) -- continuing.\n",
			dev->name, release_name(target), release_name(dev->since));
		return 1;
	}
	if (target > dev->until) {
		fprintf(stderr,
			"s5fs: note: %s was typically retired by %s. Fine if your %s "
			"kernel still has the driver -- continuing.\n",
			dev->name, release_name(dev->until), release_name(target));
		return 1;
	}
	return 0;
}

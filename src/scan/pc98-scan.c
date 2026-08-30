/*
 * pc98-scan — walk a Hoot letter\game_98 tree and write set.pc98 files.
 * Optional --rip runs hootrip archive-rip (batch) when configured.
 */
#include "pc98.h"
#include "engines/hoot_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

static void usage(void)
{
	fprintf(stderr,
		"pc98-scan — write set.pc98 into each Hoot game folder\n"
		"Usage: pc98-scan [--root DIR] [--xml DIR] [--rip] [--dry]\n"
		"  --root   music tree (default D:\\spel\\pc98\\pc98 music)\n"
		"  --xml    Hoot XML catalogue directory (hoot.xml + gamelists)\n"
		"  --rip    after writing sets, spawn hootrip archive-rip\n"
		"  --dry    count folders only, do not write\n");
}

int main(int argc, char **argv)
{
	const char *root = "D:\\spel\\pc98\\pc98 music";
	const char *xml = "";
	int rip = 0, dry = 0, i, n;
	pc98_cfg cfg;

	for (i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--root") == 0 && i + 1 < argc)
			root = argv[++i];
		else if (strcmp(argv[i], "--xml") == 0 && i + 1 < argc)
			xml = argv[++i];
		else if (strcmp(argv[i], "--rip") == 0)
			rip = 1;
		else if (strcmp(argv[i], "--dry") == 0)
			dry = 1;
		else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage();
			return 0;
		} else {
			fprintf(stderr, "unknown arg: %s\n", argv[i]);
			usage();
			return 1;
		}
	}

	pc98_cfg_defaults(&cfg);
	n = hoot_scan_tree(root, xml, dry ? 0 : 1);
	printf("pc98-scan: %d game folders%s under %s\n",
		n, dry ? " (dry)" : " (set.pc98 written)", root);
	if (n < 0)
		return 1;

	if (rip) {
		char cmd[2048];
		const char *exe = cfg.hootrip_path[0] ? cfg.hootrip_path : "hootrip.exe";
		if (!xml[0]) {
			fprintf(stderr, "pc98-scan: --rip needs --xml pointing at HootArchive\n");
			return 1;
		}
		snprintf(cmd, sizeof cmd,
			"\"%s\" --archive \"%s\" archive-rip --out cache --jobs 2",
			exe, xml);
		printf("pc98-scan: %s\n", cmd);
#ifdef _WIN32
		return system(cmd) == 0 ? 0 : 1;
#else
		return system(cmd) == 0 ? 0 : 1;
#endif
	}
	return 0;
}

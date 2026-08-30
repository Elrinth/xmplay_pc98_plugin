#include "pc98.h"
#include "engines/s98_engine.h"
#include "engines/pmd_engine.h"
#include "engines/fmp_engine.h"
#include "engines/hoot_engine.h"
#include "engines/bgmdrv_engine.h"
#include "engines/na_engine.h"
#include "engines/mfd_engine.h"
#include "engines/n3g_engine.h"
#include "engines/pai_engine.h"
#include "engines/msb_engine.h"
#include "engines/md_engine.h"
#include "engines/ntl_engine.h"
#include "engines/gntl_engine.h"
#include "engines/fmd_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define ACCESS _access
#else
#include <unistd.h>
#define ACCESS access
#endif

static int fail;

static void expect(int cond, const char *msg)
{
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", msg);
		fail++;
	}
}

static void test_setfile(void)
{
	const char *names[] = { "Opening", "\xe6\xbc\x94\xe5\xa5\x8f\xe5\x81\x9c\xe6\xad\xa2", "Town" };
	const int codes[] = { 1, 0, 2 };
	char path[256];
	uint8_t *data;
	long sz;
	FILE *fp;
	pc98_info inf;
#ifdef _WIN32
	snprintf(path, sizeof path, "%s\\xmp-pc98-set-test.pc98", getenv("TEMP") ? getenv("TEMP") : ".");
#else
	snprintf(path, sizeof path, "/tmp/xmp-pc98-set-test.pc98");
#endif
	expect(pc98_set_write(path, "dalk_98", "Dalk", 3, names, codes) == 0, "set write");
	fp = fopen(path, "rb");
	expect(fp != NULL, "set reopen");
	if (!fp) return;
	fseek(fp, 0, SEEK_END);
	sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	data = (uint8_t *)malloc((size_t)sz + 1);
	fread(data, 1, (size_t)sz, fp);
	fclose(fp);
	expect(hoot_probe_mem(data, (size_t)sz), "set probe");
	expect(pc98_analyze("set.pc98", data, (size_t)sz, NULL, &inf) == 0, "set analyze");
	expect(inf.kind == PC98_KIND_SET, "set kind");
	expect(inf.songs == 2, "stop track skipped"); /* Opening + Town */
	free(data);
#ifdef _WIN32
	DeleteFileA(path);
#else
	unlink(path);
#endif
}

static void test_rejects(void)
{
	uint8_t mid[8] = { 'M', 'T', 'h', 'd', 0, 0, 0, 6 };
	uint8_t vgm[8] = { 'V', 'g', 'm', ' ', 0, 0, 0, 0 };
	uint8_t bgmdrv[] = {
		0x01, 0xFF, 0x38, 0x00, 0x02, 0xFF, 0xCF, 0x00,
		0x03, 0xFF, 0x6A, 0x01, 0x04, 0xFF, 0xB2, 0x02
	};
	uint8_t c64mus[16] = {
		0x00, 0x10, 0x20, 0x00, 0x18, 0x00, 0x10, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	uint8_t uso[16] = {
		0x01, 0x00, 0x06, 0x00, 0x02, 0x00, 0x02, 0x00,
		0x00, 0x00, 0x54, 0x72, 0x00, 0x00, 0x6B, 0x43
	};
	expect(pc98_probe("song.mid", mid, sizeof mid) == PC98_KIND_NONE, "reject midi");
	expect(pc98_probe("tune.vgm", vgm, sizeof vgm) == PC98_KIND_NONE, "reject vgm");
	expect(pc98_probe("FAL01.MUS", bgmdrv, sizeof bgmdrv) == PC98_KIND_BGMDRV, "BGMDRV MUS is native");
	expect(fmp_probe_mem(bgmdrv, sizeof bgmdrv) == 0, "FMP must not claim BGMDRV MUS");
	expect(pc98_probe("tune.mus", c64mus, sizeof c64mus) == PC98_KIND_NONE, "reject C64-like MUS");
	expect(pc98_probe("M_01.USO", uso, sizeof uso) == PC98_KIND_SET,
		"short USO header falls back to Hoot");
	{
		uint8_t obj[8] = { 0x4C, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
		expect(pc98_probe("foo.o", obj, sizeof obj) == PC98_KIND_NONE,
			"COFF .o is not Packen NA");
	}
	expect(pc98_probe("song.fmd", uso, sizeof uso) == PC98_KIND_FMP ||
			pc98_probe("song.fmd", uso, sizeof uso) == PC98_KIND_NONE,
		"USO bytes without .uso name are not forced SET");
	{
		uint8_t pai[16] = {
			'P', 'A', 'I', ' ', '3', '.', '0', '0', 'M', 0,
			0, 0, 0x2C, 0, 0, 0
		};
		expect(pc98_probe("K01.PAI", pai, sizeof pai) == PC98_KIND_PAI,
			"PAI 3.00M magic");
		{
			uint8_t notpai[16] = { 0 };
			expect(pc98_probe("song.pai", notpai, sizeof notpai) == PC98_KIND_NONE,
				".pai name is not enough");
		}
	}
	{
		uint8_t ntl[32] = {
			0x00, 0x20, 0x00, 0x05,
			0x13, 0x00, 0x11, 0x14, 0x00, 0x12,
			0x15, 0x00, 0x13, 0x16, 0x00, 0x20,
			0x17, 0x00, 0x21, 0, 0, 0, 0, 0,
			0, 0, 0, 0, 0, 0, 0, 0
		};
		uint8_t gntl[32] = {
			0x00, 0x20, 0x00, 0x05,
			0x13, 0x00, 0xB1, 0x14, 0x00, 0x31,
			0x15, 0x00, 0x32, 0x16, 0x00, 0x33,
			0x17, 0x00, 0x34, 0, 0, 0, 0, 0,
			0, 0, 0, 0, 0, 0, 0, 0
		};
		uint8_t md[64];
		uint8_t gmd[8] = { 0xCB, 0xA1, 0xA1, 0xA5, 0xA5, 0xA5, 0x31, 0xA6 };
		memset(md, 0, sizeof md);
		md[0] = 0x30; md[2] = 0x30; md[4] = 0x30; md[6] = 0x30;
		md[8] = 0x01;
		expect(pc98_probe("F_ASA.NTL", ntl, sizeof ntl) == PC98_KIND_NTL,
			"NTL FM header");
		expect(fmp_probe_mem(ntl, sizeof ntl) == 0, "FMP must not claim NTL");
		expect(pc98_probe("G_ASA.NTL", gntl, sizeof gntl) == PC98_KIND_GNTL,
			"G_* MIDI NTL is claimed as GNTL");
		expect(fmp_probe_mem(gntl, sizeof gntl) == 0, "FMP must not claim G_NTL");
		expect(pc98_probe("OERS_000.MD", md, sizeof md) == PC98_KIND_OPNMD,
			"OPNDRV MD header");
		expect(fmp_probe_mem(md, sizeof md) == 0, "FMP must not claim OPNDRV MD");
		expect(pc98_probe("OERS_000.GMD", gmd, sizeof gmd) == PC98_KIND_NONE,
			"tiny GMD stub is not claimed");
		expect(fmp_probe_mem(gmd, sizeof gmd) == 0, "FMP must not claim packed GMD");
	}
}

static void test_fmp_header(void)
{
	const char *p = "D:\\spel\\pc98\\pc98 music\\S\\saintd_98\\WL_00.OPI";
	FILE *fp;
	uint8_t hdr[64];
	size_t n;
	if (ACCESS(p, 0) != 0) {
		printf("  FMP live header skipped (no saintd_98)\n");
		return;
	}
	fp = fopen(p, "rb");
	if (!fp) return;
	n = fread(hdr, 1, sizeof hdr, fp);
	fclose(fp);
	expect(fmp_probe_mem(hdr, n), "FMP OPI probe");
	expect(pc98_probe(p, hdr, n) == PC98_KIND_FMP, "pc98 FMP kind");
}

static void test_live_sidecars(void)
{
	const char *fal = "D:\\spel\\pc98\\pc98 music\\F\\faladia_98\\FAL01.MUS";
	const char *uso = "D:\\spel\\pc98\\pc98 music\\I\\idolwosg_98\\M_01.USO";
	const char *setp = "D:\\spel\\pc98\\pc98 music\\F\\faladia_98\\set.pc98";
	FILE *fp;
	uint8_t hdr[64];
	size_t n;
	pc98_info inf;
	if (ACCESS(fal, 0) != 0) {
		printf("  sidecar live skipped\n");
		return;
	}
	fp = fopen(fal, "rb");
	if (!fp) return;
	n = fread(hdr, 1, sizeof hdr, fp);
	fclose(fp);
	expect(pc98_probe(fal, hdr, n) == PC98_KIND_BGMDRV, "faladia MUS probe");
	expect(fmp_probe_mem(hdr, n) == 0, "faladia not FMP");
	expect(bgmdrv_probe_mem(hdr, n), "faladia bgmdrv probe");
	fp = fopen(uso, "rb");
	if (fp) {
		n = fread(hdr, 1, sizeof hdr, fp);
		fclose(fp);
		expect(pc98_probe(uso, hdr, n) == PC98_KIND_MFD, "idolwosg USO is MFD");
		expect(mfd_probe_mem(hdr, n), "idolwosg mfd probe");
	}
	fp = fopen(setp, "rb");
	if (fp) {
		uint8_t *raw;
		long sz;
		fseek(fp, 0, SEEK_END);
		sz = ftell(fp);
		fseek(fp, 0, SEEK_SET);
		raw = (uint8_t *)malloc((size_t)sz);
		if (raw && fread(raw, 1, (size_t)sz, fp) == (size_t)sz &&
				pc98_analyze(setp, raw, (size_t)sz, NULL, &inf) == 0)
			expect(inf.songs >= 2, "set.pc98 dummy expands to MUS list");
		free(raw);
		fclose(fp);
	}
}

static void test_fal03_render(void)
{
	const char *fal = "D:\\spel\\pc98\\pc98 music\\F\\faladia_98\\FAL03.MUS";
	FILE *fp;
	uint8_t *raw;
	long sz;
	pc98_player *p;
	pc98_cfg cfg;
	float buf[4096];
	int got, i;
	double peak = 0;
	if (ACCESS(fal, 0) != 0) {
		printf("  FAL03 render skipped (no faladia_98)\n");
		return;
	}
	fp = fopen(fal, "rb");
	if (!fp) return;
	fseek(fp, 0, SEEK_END);
	sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (sz <= 0 || sz > (long)PC98_MAX_FILE) { fclose(fp); return; }
	raw = (uint8_t *)malloc((size_t)sz);
	if (!raw || fread(raw, 1, (size_t)sz, fp) != (size_t)sz) {
		free(raw);
		fclose(fp);
		return;
	}
	fclose(fp);
	pc98_cfg_defaults(&cfg);
	expect(pc98_probe(fal, raw, (size_t)sz) == PC98_KIND_BGMDRV, "FAL03 kind");
	p = pc98_player_open(fal, raw, (size_t)sz, &cfg);
	expect(p != NULL, "FAL03 open");
	if (p) {
		expect(pc98_player_kind(p) == PC98_KIND_BGMDRV, "FAL03 player kind");
		expect(pc98_player_one_loop_ms(p, 0) >= 1000, "FAL03 length");
		got = pc98_player_process(p, buf, 2048);
		expect(got == 2048, "FAL03 process frames");
		for (i = 0; i < got * 2; ++i) {
			double a = buf[i] < 0 ? -buf[i] : buf[i];
			if (a > peak) peak = a;
		}
		expect(peak > 0.001, "FAL03 audio energy");
		pc98_player_close(p);
	}
	free(raw);
}

static void test_live_energy(const char *path, pc98_kind want, const char *tag)
{
	FILE *fp;
	uint8_t *raw;
	long sz;
	pc98_player *p;
	pc98_cfg cfg;
	float buf[16384];
	int got, i, pass;
	double peak = 0;
	if (ACCESS(path, 0) != 0) {
		printf("  %s skipped (missing)\n", tag);
		return;
	}
	fp = fopen(path, "rb");
	if (!fp) return;
	fseek(fp, 0, SEEK_END);
	sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (sz <= 0 || sz > (long)PC98_MAX_FILE) { fclose(fp); return; }
	raw = (uint8_t *)malloc((size_t)sz);
	if (!raw || fread(raw, 1, (size_t)sz, fp) != (size_t)sz) {
		free(raw);
		fclose(fp);
		return;
	}
	fclose(fp);
	pc98_cfg_defaults(&cfg);
	if (ACCESS("D:\\spel\\pc98\\mml_dot_nert\\quick-pmd_2.0\\BIN\\PLAYER2\\2608_BD.WAV", 0) == 0)
		snprintf(cfg.rhythm_path, sizeof cfg.rhythm_path,
			"D:\\spel\\pc98\\mml_dot_nert\\quick-pmd_2.0\\BIN\\PLAYER2");
	expect(pc98_probe(path, raw, (size_t)sz) == want, tag);
	p = pc98_player_open(path, raw, (size_t)sz, &cfg);
	expect(p != NULL, tag);
	if (p) {
		got = 0;
		for (pass = 0; pass < 8 && peak <= 0.0005; ++pass) {
			int n = pc98_player_process(p, buf, 8192);
			if (n > got) got = n;
			for (i = 0; i < n * 2; ++i) {
				double a = buf[i] < 0 ? -buf[i] : buf[i];
				if (a > peak) peak = a;
			}
		}
		expect(got > 0, tag);
		if (peak <= 0.0005)
			fprintf(stderr, "  %s peak=%g kind=%d loop_ms=%d\n", tag,
				peak, (int)pc98_player_kind(p), pc98_player_one_loop_ms(p, 0));
		expect(peak > 0.0005, tag);
		if (want == PC98_KIND_FMD || want == PC98_KIND_OPNMD) {
			int ms = pc98_player_one_loop_ms(p, 0);
			printf("  %s one_loop_ms=%d\n", tag, ms);
			expect(ms >= 4000 && ms <= 240000, "FMD/MD length not locked");
		}
		pc98_player_close(p);
	}
	free(raw);
}

#ifdef _WIN32
static void census_tree(const char *root)
{
	/* Light census: letter buckets + probe a sample of .M / .FMD / set files. */
	char spec[512];
	WIN32_FIND_DATAA letter, game;
	HANDLE hl, hg;
	int games = 0, pmd = 0, fmp = 0, other = 0;
	if (ACCESS(root, 0) != 0) {
		printf("  census skipped (no %s)\n", root);
		return;
	}
	snprintf(spec, sizeof spec, "%s\\*", root);
	hl = FindFirstFileA(spec, &letter);
	if (hl == INVALID_HANDLE_VALUE) return;
	do {
		char lpath[512], gspec[512];
		if (!(letter.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
		if (letter.cFileName[0] == '.') continue;
		if (strlen(letter.cFileName) > 2) continue;
		snprintf(lpath, sizeof lpath, "%s\\%s", root, letter.cFileName);
		snprintf(gspec, sizeof gspec, "%s\\*", lpath);
		hg = FindFirstFileA(gspec, &game);
		if (hg == INVALID_HANDLE_VALUE) continue;
		do {
			char gpath[512], fspec[512];
			WIN32_FIND_DATAA ff;
			HANDLE hf;
			int saw_pmd = 0, saw_fmp = 0;
			if (!(game.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
			if (game.cFileName[0] == '.') continue;
			games++;
			snprintf(gpath, sizeof gpath, "%s\\%s", lpath, game.cFileName);
			snprintf(fspec, sizeof fspec, "%s\\*", gpath);
			hf = FindFirstFileA(fspec, &ff);
			if (hf == INVALID_HANDLE_VALUE) continue;
			do {
				const char *n = ff.cFileName;
				size_t L = strlen(n);
				if (L < 3) continue;
				if ((n[L - 2] == '.' || n[L - 3] == '.') &&
					(n[L - 1] == 'M' || n[L - 1] == 'm') &&
					(L < 4 || n[L - 3] == '\\' || n[L - 2] == '.'))
					saw_pmd = 1;
				if (strstr(n, ".OPI") || strstr(n, ".opi") ||
						strstr(n, ".FMD") || strstr(n, ".fmd") ||
						strstr(n, ".MMD") || strstr(n, ".mmd") ||
						strstr(n, ".OVI") || strstr(n, ".ovi"))
					saw_fmp = 1;
			} while (FindNextFileA(hf, &ff));
			FindClose(hf);
			if (saw_pmd) pmd++;
			else if (saw_fmp) fmp++;
			else other++;
		} while (FindNextFileA(hg, &game));
		FindClose(hg);
	} while (FindNextFileA(hl, &letter));
	FindClose(hl);
	printf("  census %s: %d games, %d with PMD-like files, %d FMP-like, %d other\n",
		root, games, pmd, fmp, other);
	expect(games > 100, "census found Hoot tree");
}
#else
static void census_tree(const char *root)
{
	(void)root;
	printf("  census skipped (Win32 walk)\n");
}
#endif

/* XMPlay CheckFile only peeks 256 bytes — probe must still claim. */
static void test_xmplay_peek(const char *path, pc98_kind want, const char *tag)
{
	FILE *fp;
	uint8_t hdr[256];
	size_t n;
	if (ACCESS(path, 0) != 0) {
		printf("  %s peek skipped (missing)\n", tag);
		return;
	}
	fp = fopen(path, "rb");
	if (!fp) return;
	n = fread(hdr, 1, sizeof hdr, fp);
	fclose(fp);
	expect(n >= 16, tag);
	expect(pc98_probe(path, hdr, n) == want, tag);
}

int main(void)
{
	pc98_cfg cfg;
	pc98_cfg_defaults(&cfg);
	test_rejects();
	test_xmplay_peek("D:\\spel\\pc98\\pc98 music\\K\\kirisima_98\\KG03N.MSB",
		PC98_KIND_MSB, "KG03N.MSB CheckFile peek");
	test_xmplay_peek("D:\\spel\\pc98\\pc98 music\\K\\k_sekiga_98\\F_JINKEI.NTL",
		PC98_KIND_NTL, "F_JINKEI.NTL CheckFile peek");
	test_xmplay_peek("D:\\spel\\pc98\\pc98 music\\K\\k_sekiga_98\\G_JINKEI.NTL",
		PC98_KIND_GNTL, "G_JINKEI.NTL CheckFile peek");
	test_xmplay_peek("D:\\spel\\pc98\\pc98 music\\K\\k_sekiga_98\\G_GOOD_B.NTL",
		PC98_KIND_GNTL, "G_GOOD_B.NTL CheckFile peek");
	test_xmplay_peek("D:\\spel\\pc98\\pc98 music\\K\\k_sekiga_98\\G_KOSIN3.NTL",
		PC98_KIND_GNTL, "G_KOSIN3.NTL CheckFile peek");
	test_setfile();
	test_fmp_header();
	test_live_sidecars();
	test_fal03_render();
	test_live_energy("D:\\spel\\pc98\\pc98 music\\D\\dangtoys_98\\DT01.O",
		PC98_KIND_NA, "DT01.O energy");
	test_live_energy("D:\\spel\\pc98\\pc98 music\\N\\night_s_98\\NSA02.USO",
		PC98_KIND_MFD, "NSA02.USO energy");
	test_live_energy("D:\\spel\\pc98\\pc98 music\\N\\n3gpbl98\\OPENING.MDT",
		PC98_KIND_N3G, "OPENING.MDT energy");
	test_live_energy("D:\\spel\\pc98\\pc98 music\\N\\nightsft_98\\NS01.LSP",
		PC98_KIND_PMD, "NS01.LSP energy");
	test_live_energy("D:\\spel\\pc98\\pc98 music\\K\\k_chojin_98\\K01.PAI",
		PC98_KIND_PAI, "K01.PAI energy");
	test_live_energy("D:\\spel\\pc98\\pc98 music\\K\\kirisima_98\\KG01.MSB",
		PC98_KIND_MSB, "KG01.MSB energy");
	test_live_energy("D:\\spel\\pc98\\pc98 music\\K\\kirisima_98\\KG03N.MSB",
		PC98_KIND_MSB, "KG03N.MSB energy");
	test_live_energy("D:\\spel\\pc98\\pc98 music\\O\\oerstd98\\OERS_000.MD",
		PC98_KIND_OPNMD, "OERS_000.MD energy");
	test_live_energy("D:\\spel\\pc98\\pc98 music\\O\\oerstd98\\OERS_000.GMD",
		PC98_KIND_FMD, "OERS_000.GMD energy");
	test_live_energy("D:\\spel\\pc98\\pc98 music\\O\\oerstd98\\OERS_000.MMD",
		PC98_KIND_FMD, "OERS_000.MMD energy");
	test_live_energy("D:\\spel\\pc98\\pc98 music\\O\\oerstd98\\OERS_002.GMD",
		PC98_KIND_FMD, "OERS_002.GMD energy");
	test_live_energy("D:\\spel\\pc98\\pc98 music\\O\\oerstd98\\OERS_002.MD",
		PC98_KIND_OPNMD, "OERS_002.MD energy");
	{
		const char *md0 = "D:\\spel\\pc98\\pc98 music\\O\\oerstd98\\OERS_000.MD";
		const char *g2 = "D:\\spel\\pc98\\pc98 music\\O\\oerstd98\\OERS_002.GMD";
		FILE *fp;
		uint8_t *raw;
		long sz;
		pc98_player *p;
		pc98_cfg cfg;
		int ms;
		if (ACCESS(md0, 0) == 0 && (fp = fopen(md0, "rb"))) {
			fseek(fp, 0, SEEK_END);
			sz = ftell(fp);
			fseek(fp, 0, SEEK_SET);
			raw = (uint8_t *)malloc((size_t)sz);
			if (raw && fread(raw, 1, (size_t)sz, fp) == (size_t)sz) {
				pc98_cfg_defaults(&cfg);
				p = pc98_player_open(md0, raw, (size_t)sz, &cfg);
				expect(p != NULL, "OERS_000.MD open");
				if (p) {
					ms = pc98_player_one_loop_ms(p, 0);
					printf("  OERS_000.MD one_loop_ms=%d\n", ms);
					expect(ms >= 15500 && ms <= 17500, "OERS_000.MD ~16s not 4s");
					pc98_player_close(p);
				}
			}
			free(raw);
			fclose(fp);
		}
		{
			const char *md2 = "D:\\spel\\pc98\\pc98 music\\O\\oerstd98\\OERS_002.MD";
			if (ACCESS(md2, 0) == 0 && (fp = fopen(md2, "rb"))) {
				fseek(fp, 0, SEEK_END);
				sz = ftell(fp);
				fseek(fp, 0, SEEK_SET);
				raw = (uint8_t *)malloc((size_t)sz);
				if (raw && fread(raw, 1, (size_t)sz, fp) == (size_t)sz) {
					pc98_cfg_defaults(&cfg);
					if (ACCESS("D:\\spel\\pc98\\mml_dot_nert\\quick-pmd_2.0\\BIN\\PLAYER2\\2608_BD.WAV", 0) == 0)
						snprintf(cfg.rhythm_path, sizeof cfg.rhythm_path,
							"D:\\spel\\pc98\\mml_dot_nert\\quick-pmd_2.0\\BIN\\PLAYER2");
					p = pc98_player_open(md2, raw, (size_t)sz, &cfg);
					expect(p != NULL, "OERS_002.MD open");
					if (p) {
						float buf[8192];
						double e_on = 0, e_off = 0;
						int pass, i, n;
						ms = pc98_player_one_loop_ms(p, 0);
						printf("  OERS_002.MD one_loop_ms=%d\n", ms);
						expect(ms >= 50000 && ms <= 90000,
							"OERS_002.MD ~62s not first FB");
						for (pass = 0; pass < 6; ++pass) {
							n = pc98_player_process(p, buf, 4096);
							for (i = 0; i < n * 2; ++i)
								e_on += (double)buf[i] * buf[i];
						}
						pc98_player_close(p);
						cfg.mute_rhythm = 1;
						p = pc98_player_open(md2, raw, (size_t)sz, &cfg);
						expect(p != NULL, "OERS_002.MD mute rhy open");
						if (p) {
							for (pass = 0; pass < 6; ++pass) {
								n = pc98_player_process(p, buf, 4096);
								for (i = 0; i < n * 2; ++i)
									e_off += (double)buf[i] * buf[i];
							}
							printf("  OERS_002.MD rhy energy on=%.3f off=%.3f\n",
								e_on, e_off);
							expect(e_on > e_off * 1.02,
								"OERS_002.MD 86-board drums audible");
							pc98_player_close(p);
							p = NULL;
						}
					}
				}
				free(raw);
				fclose(fp);
			}
		}
		if (ACCESS(g2, 0) == 0 && (fp = fopen(g2, "rb"))) {
			fseek(fp, 0, SEEK_END);
			sz = ftell(fp);
			fseek(fp, 0, SEEK_SET);
			raw = (uint8_t *)malloc((size_t)sz);
			if (raw && fread(raw, 1, (size_t)sz, fp) == (size_t)sz) {
				pc98_cfg_defaults(&cfg);
				p = pc98_player_open(g2, raw, (size_t)sz, &cfg);
				expect(p != NULL, "OERS_002.GMD open");
				if (p) {
					ms = pc98_player_one_loop_ms(p, 0);
					printf("  OERS_002.GMD one_loop_ms=%d\n", ms);
					expect(ms >= 90000 && ms <= 150000, "OERS_002.GMD ~2min like SC-55 rip");
					pc98_player_close(p);
				}
			}
			free(raw);
			fclose(fp);
		}
	}
	test_live_energy("D:\\spel\\pc98\\pc98 music\\K\\k_sekiga_98\\F_ASA.NTL",
		PC98_KIND_NTL, "F_ASA.NTL energy");
	test_live_energy("D:\\spel\\pc98\\pc98 music\\K\\k_sekiga_98\\F_JINKEI.NTL",
		PC98_KIND_NTL, "F_JINKEI.NTL energy");
	test_live_energy("D:\\spel\\pc98\\pc98 music\\K\\k_sekiga_98\\G_KOSIN3.NTL",
		PC98_KIND_GNTL, "G_KOSIN3.NTL energy");
	test_live_energy("D:\\spel\\pc98\\pc98 music\\K\\k_sekiga_98\\G_GOOD_B.NTL",
		PC98_KIND_GNTL, "G_GOOD_B.NTL energy");
	{
		const char *k01 = "D:\\spel\\pc98\\pc98 music\\K\\k_chojin_98\\K01.PAI";
		FILE *fp;
		uint8_t *raw;
		long sz;
		pc98_player *p;
		pc98_cfg cfg;
		int ms;
		if (ACCESS(k01, 0) == 0 && (fp = fopen(k01, "rb"))) {
			fseek(fp, 0, SEEK_END);
			sz = ftell(fp);
			fseek(fp, 0, SEEK_SET);
			raw = (uint8_t *)malloc((size_t)sz);
			if (raw && fread(raw, 1, (size_t)sz, fp) == (size_t)sz) {
				pc98_cfg_defaults(&cfg);
				p = pc98_player_open(k01, raw, (size_t)sz, &cfg);
				expect(p != NULL, "K01 open");
				if (p) {
					ms = pc98_player_one_loop_ms(p, 0);
					printf("  K01.PAI one_loop_ms=%d\n", ms);
					expect(ms >= 15000 && ms <= 60000, "K01 length ~24s");
					expect(strcmp(pc98_player_filetype(p), "PAI") == 0, "K01 filetype");
					pc98_player_close(p);
				}
			}
			free(raw);
			fclose(fp);
		}
	}
	census_tree("D:\\spel\\pc98\\pc98 music");
	printf("test_pc98_render: %s (%d)\n", fail ? "FAIL" : "ok", fail);
	return fail ? 1 : 0;
}

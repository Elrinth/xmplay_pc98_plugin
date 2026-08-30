#include "pc98.h"
#include "pc98_util.h"
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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct pc98_player {
	pc98_kind kind;
	pc98_cfg cfg;
	void *eng;
	int song;
	int songs;
	int one_loop_ms[PC98_MAX_SONGS];
	char title[256];
	char artist[256];
	char game[256];
	char filetype[32];
	char engine[64];
};

static int banned_name(const char *filename)
{
	if (!filename) return 0;
	if (pc98_iends(filename, ".vgm") || pc98_iends(filename, ".vgz")) return 1;
	if (pc98_iends(filename, ".sap")) return 1;
	if (pc98_iends(filename, ".mid") || pc98_iends(filename, ".midi")) return 1;
	if (pc98_iends(filename, ".mml")) return 1;
	if (pc98_iends(filename, ".com") || pc98_iends(filename, ".exe")) return 1;
	if (pc98_iends(filename, ".sys") || pc98_iends(filename, ".tdf")) return 1;
	if (pc98_iends(filename, ".ins")) return 1;
	return 0;
}

pc98_kind pc98_probe(const char *filename, const uint8_t *data, size_t len)
{
	if (banned_name(filename))
		return PC98_KIND_NONE;
	if (s98_probe(data, len))
		return PC98_KIND_S98;
	if (na_probe_mem(data, len) &&
			(!filename || pc98_iends(filename, ".o")))
		return PC98_KIND_NA;
	if (mfd_probe_mem(data, len) &&
			(!filename || pc98_iends(filename, ".uso")))
		return PC98_KIND_MFD;
	if (n3g_probe_mem(data, len) &&
			(!filename || pc98_iends(filename, ".mdt")))
		return PC98_KIND_N3G;
	if (pai_probe_mem(data, len))
		return PC98_KIND_PAI;
	if (msb_probe_mem(data, len))
		return PC98_KIND_MSB;
	if (md_probe_mem(data, len))
		return PC98_KIND_OPNMD;
	if (ntl_probe_mem(data, len))
		return PC98_KIND_NTL;
	if (gntl_probe_mem(data, len))
		return PC98_KIND_GNTL;
	if (fmd_probe_mem(data, len))
		return PC98_KIND_FMD;
	if (bgmdrv_probe_mem(data, len))
		return PC98_KIND_BGMDRV;
	if (hoot_probe_file(filename, data, len))
		return PC98_KIND_SET;
	if (pmd_probe_mem(data, len))
		return PC98_KIND_PMD;
	if (fmp_probe_mem(data, len)) {
		/* FMP probe is offset-heuristic; never steal .mus/.uso. */
		if (filename && (pc98_iends(filename, ".mus") || pc98_iends(filename, ".uso") ||
				pc98_iends(filename, ".msb") || pc98_iends(filename, ".ntl") ||
				pc98_iends(filename, ".md") || pc98_iends(filename, ".gmd")))
			return PC98_KIND_NONE;
		return PC98_KIND_FMP;
	}
	return PC98_KIND_NONE;
}

int pc98_analyze(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out)
{
	pc98_kind k;
	if (!out) return -1;
	memset(out, 0, sizeof *out);
	k = pc98_probe(filename, data, len);
	if (k == PC98_KIND_S98) return s98_analyze(data, len, cfg, out);
	if (k == PC98_KIND_NA) return na_analyze_mem(filename, data, len, cfg, out);
	if (k == PC98_KIND_MFD) return mfd_analyze_mem(filename, data, len, cfg, out);
	if (k == PC98_KIND_N3G) return n3g_analyze_mem(filename, data, len, cfg, out);
	if (k == PC98_KIND_PAI) return pai_analyze_mem(filename, data, len, cfg, out);
	if (k == PC98_KIND_MSB) return msb_analyze_mem(filename, data, len, cfg, out);
	if (k == PC98_KIND_OPNMD) return md_analyze_mem(filename, data, len, cfg, out);
	if (k == PC98_KIND_NTL) return ntl_analyze_mem(filename, data, len, cfg, out);
	if (k == PC98_KIND_GNTL) return gntl_analyze_mem(filename, data, len, cfg, out);
	if (k == PC98_KIND_FMD) return fmd_analyze_mem(filename, data, len, cfg, out);
	if (k == PC98_KIND_BGMDRV) return bgmdrv_analyze_mem(filename, data, len, cfg, out);
	if (k == PC98_KIND_SET) return hoot_analyze_mem(filename, data, len, cfg, out);
	if (k == PC98_KIND_PMD) return pmd_analyze_mem(filename, data, len, cfg, out);
	if (k == PC98_KIND_FMP) return fmp_analyze_mem(filename, data, len, cfg, out);
	return -1;
}

static void fill_from_info(pc98_player *p, const pc98_info *inf)
{
	int i;
	p->songs = inf->songs > 0 ? inf->songs : 1;
	if (p->songs > PC98_MAX_SONGS) p->songs = PC98_MAX_SONGS;
	for (i = 0; i < p->songs; ++i)
		p->one_loop_ms[i] = inf->one_loop_ms[i];
	pc98_bounded(p->title, sizeof p->title, inf->title);
	pc98_bounded(p->artist, sizeof p->artist, inf->artist);
	pc98_bounded(p->game, sizeof p->game, inf->game);
	pc98_bounded(p->filetype, sizeof p->filetype, inf->filetype);
	pc98_bounded(p->engine, sizeof p->engine, inf->engine);
}

pc98_player *pc98_player_open(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg)
{
	pc98_player *p;
	pc98_info inf;
	pc98_kind k;
	if (!data || len < 4) return NULL;
	k = pc98_probe(filename, data, len);
	if (k == PC98_KIND_NONE) return NULL;
	p = (pc98_player *)calloc(1, sizeof *p);
	if (!p) return NULL;
	if (cfg) p->cfg = *cfg;
	else pc98_cfg_defaults(&p->cfg);
	pc98_cfg_clamp(&p->cfg);
	p->kind = k;
	p->song = 0;
	p->songs = 1;
	memset(&inf, 0, sizeof inf);
	if (k == PC98_KIND_S98) {
		p->eng = s98_open(data, len, &p->cfg);
		if (!p->eng) { free(p); return NULL; }
		s98_analyze(data, len, &p->cfg, &inf);
		fill_from_info(p, &inf);
		if (s98_title(p->eng)[0]) pc98_bounded(p->title, sizeof p->title, s98_title(p->eng));
		if (s98_artist(p->eng)[0]) pc98_bounded(p->artist, sizeof p->artist, s98_artist(p->eng));
		if (s98_game(p->eng)[0]) pc98_bounded(p->game, sizeof p->game, s98_game(p->eng));
		p->one_loop_ms[0] = s98_one_loop_ms(p->eng);
		pc98_bounded(p->filetype, sizeof p->filetype, "S98");
		pc98_bounded(p->engine, sizeof p->engine, "fmgen S98");
	} else if (k == PC98_KIND_PMD) {
		p->eng = pmd_open_mem(filename, data, len, &p->cfg);
		if (!p->eng) { free(p); return NULL; }
		pmd_analyze_mem(filename, data, len, &p->cfg, &inf);
		fill_from_info(p, &inf);
		if (pmd_title_h(p->eng)[0]) pc98_bounded(p->title, sizeof p->title, pmd_title_h(p->eng));
		if (pmd_artist_h(p->eng)[0]) pc98_bounded(p->artist, sizeof p->artist, pmd_artist_h(p->eng));
		p->one_loop_ms[0] = pmd_one_loop_ms_h(p->eng);
		pc98_bounded(p->filetype, sizeof p->filetype, "PMD");
		pc98_bounded(p->engine, sizeof p->engine, "pmdmini / PMDWin");
	} else if (k == PC98_KIND_FMP) {
		p->eng = fmp_open_mem(filename, data, len, &p->cfg);
		if (!p->eng) { free(p); return NULL; }
		fmp_analyze_mem(filename, data, len, &p->cfg, &inf);
		fill_from_info(p, &inf);
		if (fmp_title_h(p->eng)[0]) pc98_bounded(p->title, sizeof p->title, fmp_title_h(p->eng));
		p->one_loop_ms[0] = fmp_one_loop_ms_h(p->eng);
		pc98_bounded(p->filetype, sizeof p->filetype, "FMP");
		pc98_bounded(p->engine, sizeof p->engine, "WinFMP");
	} else if (k == PC98_KIND_BGMDRV) {
		p->eng = bgmdrv_open_mem(filename, data, len, &p->cfg);
		if (!p->eng) { free(p); return NULL; }
		bgmdrv_analyze_mem(filename, data, len, &p->cfg, &inf);
		fill_from_info(p, &inf);
		if (bgmdrv_title_h(p->eng)[0]) pc98_bounded(p->title, sizeof p->title, bgmdrv_title_h(p->eng));
		p->one_loop_ms[0] = bgmdrv_one_loop_ms_h(p->eng);
		pc98_bounded(p->filetype, sizeof p->filetype, "BGMDRV");
		pc98_bounded(p->engine, sizeof p->engine, "ymfm BGMDRV");
	} else if (k == PC98_KIND_NA) {
		p->eng = na_open_mem(filename, data, len, &p->cfg);
		if (!p->eng) { free(p); return NULL; }
		na_analyze_mem(filename, data, len, &p->cfg, &inf);
		fill_from_info(p, &inf);
		if (na_title_h(p->eng)[0]) pc98_bounded(p->title, sizeof p->title, na_title_h(p->eng));
		p->one_loop_ms[0] = na_one_loop_ms_h(p->eng);
		pc98_bounded(p->filetype, sizeof p->filetype, "NA");
		pc98_bounded(p->engine, sizeof p->engine, "ymfm Packen NA");
	} else if (k == PC98_KIND_MFD) {
		p->eng = mfd_open_mem(filename, data, len, &p->cfg);
		if (!p->eng) { free(p); return NULL; }
		mfd_analyze_mem(filename, data, len, &p->cfg, &inf);
		fill_from_info(p, &inf);
		if (mfd_title_h(p->eng)[0]) pc98_bounded(p->title, sizeof p->title, mfd_title_h(p->eng));
		p->one_loop_ms[0] = mfd_one_loop_ms_h(p->eng);
		pc98_bounded(p->filetype, sizeof p->filetype, "USO");
		if (mfd_engine_name_h(p->eng)[0])
			pc98_bounded(p->engine, sizeof p->engine, mfd_engine_name_h(p->eng));
		else
			pc98_bounded(p->engine, sizeof p->engine, "fmgen USMD");
	} else if (k == PC98_KIND_N3G) {
		p->eng = n3g_open_mem(filename, data, len, &p->cfg);
		if (!p->eng) { free(p); return NULL; }
		n3g_analyze_mem(filename, data, len, &p->cfg, &inf);
		fill_from_info(p, &inf);
		if (n3g_title_h(p->eng)[0]) pc98_bounded(p->title, sizeof p->title, n3g_title_h(p->eng));
		p->one_loop_ms[0] = n3g_one_loop_ms_h(p->eng);
		pc98_bounded(p->filetype, sizeof p->filetype, "MDT");
		pc98_bounded(p->engine, sizeof p->engine, "ymfm N3G");
	} else if (k == PC98_KIND_PAI) {
		p->eng = pai_open_mem(filename, data, len, &p->cfg);
		if (!p->eng) { free(p); return NULL; }
		pai_analyze_mem(filename, data, len, &p->cfg, &inf);
		fill_from_info(p, &inf);
		if (pai_title_h(p->eng)[0]) pc98_bounded(p->title, sizeof p->title, pai_title_h(p->eng));
		if (pai_game_h(p->eng)[0]) pc98_bounded(p->game, sizeof p->game, pai_game_h(p->eng));
		p->one_loop_ms[0] = pai_one_loop_ms_h(p->eng);
		pc98_bounded(p->filetype, sizeof p->filetype, "PAI");
		pc98_bounded(p->engine, sizeof p->engine, "ymfm Synthia");
	} else if (k == PC98_KIND_MSB) {
		p->eng = msb_open_mem(filename, data, len, &p->cfg);
		if (!p->eng) { free(p); return NULL; }
		msb_analyze_mem(filename, data, len, &p->cfg, &inf);
		fill_from_info(p, &inf);
		if (msb_title_h(p->eng)[0]) pc98_bounded(p->title, sizeof p->title, msb_title_h(p->eng));
		if (msb_game_h(p->eng)[0]) pc98_bounded(p->game, sizeof p->game, msb_game_h(p->eng));
		p->one_loop_ms[0] = msb_one_loop_ms_h(p->eng);
		pc98_bounded(p->filetype, sizeof p->filetype, "MSB");
		pc98_bounded(p->engine, sizeof p->engine, "ymfm MBMUS");
	} else if (k == PC98_KIND_OPNMD) {
		p->eng = md_open_mem(filename, data, len, &p->cfg);
		if (!p->eng) { free(p); return NULL; }
		md_analyze_mem(filename, data, len, &p->cfg, &inf);
		fill_from_info(p, &inf);
		if (md_title_h(p->eng)[0]) pc98_bounded(p->title, sizeof p->title, md_title_h(p->eng));
		if (md_game_h(p->eng)[0]) pc98_bounded(p->game, sizeof p->game, md_game_h(p->eng));
		p->one_loop_ms[0] = md_one_loop_ms_h(p->eng);
		pc98_bounded(p->filetype, sizeof p->filetype, "MD");
		pc98_bounded(p->engine, sizeof p->engine, "ymfm OPNA / OPNDRV");
	} else if (k == PC98_KIND_NTL) {
		p->eng = ntl_open_mem(filename, data, len, &p->cfg);
		if (!p->eng) { free(p); return NULL; }
		ntl_analyze_mem(filename, data, len, &p->cfg, &inf);
		fill_from_info(p, &inf);
		if (ntl_title_h(p->eng)[0]) pc98_bounded(p->title, sizeof p->title, ntl_title_h(p->eng));
		if (ntl_game_h(p->eng)[0]) pc98_bounded(p->game, sizeof p->game, ntl_game_h(p->eng));
		p->one_loop_ms[0] = ntl_one_loop_ms_h(p->eng);
		pc98_bounded(p->filetype, sizeof p->filetype, "NTL");
		pc98_bounded(p->engine, sizeof p->engine, "ymfm ArtDink NTL");
	} else if (k == PC98_KIND_GNTL) {
		p->eng = gntl_open_mem(filename, data, len, &p->cfg);
		if (!p->eng) { free(p); return NULL; }
		gntl_analyze_mem(filename, data, len, &p->cfg, &inf);
		fill_from_info(p, &inf);
		if (gntl_title_h(p->eng)[0]) pc98_bounded(p->title, sizeof p->title, gntl_title_h(p->eng));
		if (gntl_game_h(p->eng)[0]) pc98_bounded(p->game, sizeof p->game, gntl_game_h(p->eng));
		p->one_loop_ms[0] = gntl_one_loop_ms_h(p->eng);
		pc98_bounded(p->filetype, sizeof p->filetype, "NTL");
		pc98_bounded(p->engine, sizeof p->engine, gntl_engine_h(p->eng));
	} else if (k == PC98_KIND_FMD) {
		p->eng = fmd_open_mem(filename, data, len, &p->cfg);
		if (!p->eng) { free(p); return NULL; }
		fmd_analyze_mem(filename, data, len, &p->cfg, &inf);
		fill_from_info(p, &inf);
		if (fmd_title_h(p->eng)[0]) pc98_bounded(p->title, sizeof p->title, fmd_title_h(p->eng));
		if (fmd_game_h(p->eng)[0]) pc98_bounded(p->game, sizeof p->game, fmd_game_h(p->eng));
		p->one_loop_ms[0] = fmd_one_loop_ms_h(p->eng);
		pc98_bounded(p->filetype, sizeof p->filetype, fmd_type_h(p->eng));
		pc98_bounded(p->engine, sizeof p->engine, fmd_engine_h(p->eng));
	} else {
		p->eng = hoot_open_mem(filename, data, len, &p->cfg);
		if (!p->eng) { free(p); return NULL; }
		hoot_analyze_mem(filename, data, len, &p->cfg, &inf);
		fill_from_info(p, &inf);
		p->songs = hoot_songs_h(p->eng);
		p->song = hoot_song_h(p->eng);
		if (hoot_title_h(p->eng)[0]) pc98_bounded(p->title, sizeof p->title, hoot_title_h(p->eng));
		if (hoot_game_h(p->eng)[0]) pc98_bounded(p->game, sizeof p->game, hoot_game_h(p->eng));
		pc98_bounded(p->filetype, sizeof p->filetype, "PC98 set");
		pc98_bounded(p->engine, sizeof p->engine, "hootrip / S98 cache");
	}
	return p;
}

void pc98_player_close(pc98_player *p)
{
	if (!p) return;
	if (p->kind == PC98_KIND_S98) s98_close(p->eng);
	else if (p->kind == PC98_KIND_PMD) pmd_close_h(p->eng);
	else if (p->kind == PC98_KIND_FMP) fmp_close_h(p->eng);
	else if (p->kind == PC98_KIND_BGMDRV) bgmdrv_close_h(p->eng);
	else if (p->kind == PC98_KIND_NA) na_close_h(p->eng);
	else if (p->kind == PC98_KIND_MFD) mfd_close_h(p->eng);
	else if (p->kind == PC98_KIND_N3G) n3g_close_h(p->eng);
	else if (p->kind == PC98_KIND_PAI) pai_close_h(p->eng);
	else if (p->kind == PC98_KIND_MSB) msb_close_h(p->eng);
	else if (p->kind == PC98_KIND_OPNMD) md_close_h(p->eng);
	else if (p->kind == PC98_KIND_NTL) ntl_close_h(p->eng);
	else if (p->kind == PC98_KIND_GNTL) gntl_close_h(p->eng);
	else if (p->kind == PC98_KIND_FMD) fmd_close_h(p->eng);
	else if (p->kind == PC98_KIND_SET) hoot_close_h(p->eng);
	free(p);
}

int pc98_player_process(pc98_player *p, float *buf, int count)
{
	if (!p || !p->eng || !buf || count <= 0) return 0;
	if (p->kind == PC98_KIND_S98) return s98_process(p->eng, buf, count);
	if (p->kind == PC98_KIND_PMD) return pmd_process_h(p->eng, buf, count);
	if (p->kind == PC98_KIND_FMP) return fmp_process_h(p->eng, buf, count);
	if (p->kind == PC98_KIND_BGMDRV) return bgmdrv_process_h(p->eng, buf, count);
	if (p->kind == PC98_KIND_NA) return na_process_h(p->eng, buf, count);
	if (p->kind == PC98_KIND_MFD) return mfd_process_h(p->eng, buf, count);
	if (p->kind == PC98_KIND_N3G) return n3g_process_h(p->eng, buf, count);
	if (p->kind == PC98_KIND_PAI) return pai_process_h(p->eng, buf, count);
	if (p->kind == PC98_KIND_MSB) return msb_process_h(p->eng, buf, count);
	if (p->kind == PC98_KIND_OPNMD) return md_process_h(p->eng, buf, count);
	if (p->kind == PC98_KIND_NTL) return ntl_process_h(p->eng, buf, count);
	if (p->kind == PC98_KIND_GNTL) return gntl_process_h(p->eng, buf, count);
	if (p->kind == PC98_KIND_FMD) return fmd_process_h(p->eng, buf, count);
	if (p->kind == PC98_KIND_SET) return hoot_process_h(p->eng, buf, count);
	return 0;
}

int pc98_player_seek_ms(pc98_player *p, int ms)
{
	if (!p || !p->eng) return -1;
	if (p->kind == PC98_KIND_S98) return s98_seek_ms(p->eng, ms);
	if (p->kind == PC98_KIND_PMD) return pmd_seek_ms_h(p->eng, ms);
	if (p->kind == PC98_KIND_FMP) return fmp_seek_ms_h(p->eng, ms);
	if (p->kind == PC98_KIND_BGMDRV) return bgmdrv_seek_ms_h(p->eng, ms);
	if (p->kind == PC98_KIND_NA) return na_seek_ms_h(p->eng, ms);
	if (p->kind == PC98_KIND_MFD) return mfd_seek_ms_h(p->eng, ms);
	if (p->kind == PC98_KIND_N3G) return n3g_seek_ms_h(p->eng, ms);
	if (p->kind == PC98_KIND_PAI) return pai_seek_ms_h(p->eng, ms);
	if (p->kind == PC98_KIND_MSB) return msb_seek_ms_h(p->eng, ms);
	if (p->kind == PC98_KIND_OPNMD) return md_seek_ms_h(p->eng, ms);
	if (p->kind == PC98_KIND_NTL) return ntl_seek_ms_h(p->eng, ms);
	if (p->kind == PC98_KIND_GNTL) return gntl_seek_ms_h(p->eng, ms);
	if (p->kind == PC98_KIND_FMD) return fmd_seek_ms_h(p->eng, ms);
	if (p->kind == PC98_KIND_SET) return hoot_seek_ms_h(p->eng, ms);
	return -1;
}

int pc98_player_set_song(pc98_player *p, int song)
{
	if (!p) return -1;
	if (p->kind != PC98_KIND_SET)
		return (song == 0) ? 0 : -1;
	if (hoot_set_song_h(p->eng, song) != 0)
		return -1;
	p->song = hoot_song_h(p->eng);
	if (hoot_title_h(p->eng)[0])
		pc98_bounded(p->title, sizeof p->title, hoot_title_h(p->eng));
	return 0;
}

int pc98_player_song(const pc98_player *p) { return p ? p->song : 0; }
int pc98_player_songs(const pc98_player *p) { return p ? p->songs : 0; }

int pc98_player_rate(const pc98_player *p)
{
	if (!p || !p->eng) return PC98_DEFAULT_RATE;
	if (p->kind == PC98_KIND_S98) return s98_rate(p->eng);
	if (p->kind == PC98_KIND_PMD) return pmd_rate_h(p->eng);
	if (p->kind == PC98_KIND_FMP) return fmp_rate_h(p->eng);
	if (p->kind == PC98_KIND_BGMDRV) return bgmdrv_rate_h(p->eng);
	if (p->kind == PC98_KIND_NA) return na_rate_h(p->eng);
	if (p->kind == PC98_KIND_MFD) return mfd_rate_h(p->eng);
	if (p->kind == PC98_KIND_N3G) return n3g_rate_h(p->eng);
	if (p->kind == PC98_KIND_PAI) return pai_rate_h(p->eng);
	if (p->kind == PC98_KIND_MSB) return msb_rate_h(p->eng);
	if (p->kind == PC98_KIND_OPNMD) return md_rate_h(p->eng);
	if (p->kind == PC98_KIND_NTL) return ntl_rate_h(p->eng);
	if (p->kind == PC98_KIND_GNTL) return gntl_rate_h(p->eng);
	if (p->kind == PC98_KIND_FMD) return fmd_rate_h(p->eng);
	if (p->kind == PC98_KIND_SET) return hoot_rate_h(p->eng);
	return PC98_DEFAULT_RATE;
}

int pc98_player_one_loop_ms(const pc98_player *p, int song)
{
	if (!p) return 0;
	if (p->kind == PC98_KIND_SET)
		return hoot_one_loop_ms_h(p->eng, song);
	if (song < 0 || song >= p->songs) song = 0;
	return p->one_loop_ms[song];
}

int pc98_player_play_ms(const pc98_player *p, int song)
{
	int one = pc98_player_one_loop_ms(p, song);
	int loops = pc98_player_loop_count(p);
	if (one <= 0) return 0;
	if (loops < 1) loops = 1;
	return one * loops;
}

int pc98_player_total_one_loop_ms(const pc98_player *p)
{
	int i, t = 0;
	if (!p) return 0;
	for (i = 0; i < p->songs; ++i)
		t += pc98_player_one_loop_ms(p, i);
	return t;
}

int pc98_player_loop_count(const pc98_player *p)
{
	return p ? p->cfg.loop_count : 1;
}

void pc98_player_set_loop_count(pc98_player *p, int loops)
{
	if (!p) return;
	if (loops < 1) loops = 1;
	if (loops > 3) loops = 3;
	p->cfg.loop_count = loops;
	if (p->kind == PC98_KIND_S98) s98_set_loops(p->eng, loops);
	else if (p->kind == PC98_KIND_PMD) pmd_set_loops_h(p->eng, loops);
	else if (p->kind == PC98_KIND_FMP) fmp_set_loops_h(p->eng, loops);
	else if (p->kind == PC98_KIND_BGMDRV) bgmdrv_set_loops_h(p->eng, loops);
	else if (p->kind == PC98_KIND_NA) na_set_loops_h(p->eng, loops);
	else if (p->kind == PC98_KIND_MFD) mfd_set_loops_h(p->eng, loops);
	else if (p->kind == PC98_KIND_N3G) n3g_set_loops_h(p->eng, loops);
	else if (p->kind == PC98_KIND_PAI) pai_set_loops_h(p->eng, loops);
	else if (p->kind == PC98_KIND_MSB) msb_set_loops_h(p->eng, loops);
	else if (p->kind == PC98_KIND_OPNMD) md_set_loops_h(p->eng, loops);
	else if (p->kind == PC98_KIND_NTL) ntl_set_loops_h(p->eng, loops);
	else if (p->kind == PC98_KIND_GNTL) gntl_set_loops_h(p->eng, loops);
	else if (p->kind == PC98_KIND_FMD) fmd_set_loops_h(p->eng, loops);
	else if (p->kind == PC98_KIND_SET) hoot_set_loops_h(p->eng, loops);
}

void pc98_player_apply_mute(pc98_player *p, const pc98_cfg *cfg)
{
	if (!p || !cfg) return;
	p->cfg.mute_fm = cfg->mute_fm;
	p->cfg.mute_ssg = cfg->mute_ssg;
	p->cfg.mute_rhythm = cfg->mute_rhythm;
	p->cfg.mute_adpcm = cfg->mute_adpcm;
	if (p->kind == PC98_KIND_S98) s98_apply_mute(p->eng, cfg);
	else if (p->kind == PC98_KIND_PMD) pmd_apply_mute_h(p->eng, cfg);
	else if (p->kind == PC98_KIND_FMP) fmp_apply_mute_h(p->eng, cfg);
	else if (p->kind == PC98_KIND_BGMDRV) bgmdrv_apply_mute_h(p->eng, cfg);
	else if (p->kind == PC98_KIND_NA) na_apply_mute_h(p->eng, cfg);
	else if (p->kind == PC98_KIND_MFD) mfd_apply_mute_h(p->eng, cfg);
	else if (p->kind == PC98_KIND_N3G) n3g_apply_mute_h(p->eng, cfg);
	else if (p->kind == PC98_KIND_PAI) pai_apply_mute_h(p->eng, cfg);
	else if (p->kind == PC98_KIND_MSB) msb_apply_mute_h(p->eng, cfg);
	else if (p->kind == PC98_KIND_OPNMD) md_apply_mute_h(p->eng, cfg);
	else if (p->kind == PC98_KIND_NTL) ntl_apply_mute_h(p->eng, cfg);
	else if (p->kind == PC98_KIND_GNTL) gntl_apply_mute_h(p->eng, cfg);
	else if (p->kind == PC98_KIND_FMD) fmd_apply_mute_h(p->eng, cfg);
	else if (p->kind == PC98_KIND_SET) hoot_apply_mute_h(p->eng, cfg);
}

pc98_kind pc98_player_kind(const pc98_player *p) { return p ? p->kind : PC98_KIND_NONE; }
const char *pc98_player_title(const pc98_player *p) { return p ? p->title : ""; }
const char *pc98_player_artist(const pc98_player *p) { return p ? p->artist : ""; }
const char *pc98_player_game(const pc98_player *p) { return p ? p->game : ""; }
const char *pc98_player_engine(const pc98_player *p) { return p ? p->engine : ""; }
const char *pc98_player_filetype(const pc98_player *p) { return p ? p->filetype : ""; }

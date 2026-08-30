/*
 * PMD via vendored pmdmini PMDWin 0.52 + ymfm (static).
 * Probe is content-only so ZX Spectrum .M is not claimed.
 */
#include "pmd_engine.h"
#include "pc98_util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <vector>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "pmdwincore.h"

struct pmd_state {
	PMDWIN *win;
	int rate;
	int loops_want;
	int one_loop_ms;
	int looping;
	int ended;
	int64_t frames_left;
	char title[256];
	char artist[256];
	std::vector<int16_t> tmp;
};

static int is_pmd_hdr(const uint8_t *h, size_t n)
{
	if (!h || n < 3) return 0;
	if (h[0] > 0x0f) return 0;
	if (h[1] != 0x18 && h[1] != 0x1a) return 0;
	if (h[2] && h[2] != 0xe6) return 0;
	return 1;
}

int pmd_probe_mem(const uint8_t *data, size_t len)
{
	return is_pmd_hdr(data, len);
}

static void apply_paths(PMDWIN *w, const char *filename, const pc98_cfg *cfg)
{
	char dir[PC98_PATH_MAX];
	char *paths[4];
	int n = 0;
	pc98_dir_of(filename, dir, sizeof dir);
	if (dir[0]) paths[n++] = dir;
	if (cfg && cfg->rhythm_path[0]) paths[n++] = (char *)cfg->rhythm_path;
	if (cfg && cfg->dll_dir[0]) paths[n++] = (char *)cfg->dll_dir;
	paths[n] = NULL;
	if (n)
		w->setpcmdir((TCHAR **)paths);
	if (cfg && cfg->rhythm_path[0])
		w->loadrhythmsample((TCHAR *)cfg->rhythm_path);
	else if (cfg && cfg->dll_dir[0])
		w->loadrhythmsample((TCHAR *)cfg->dll_dir);
}

static void apply_mute(PMDWIN *w, const pc98_cfg *cfg)
{
	int i;
	if (!w || !cfg) return;
	for (i = 0; i < NumOfAllPart; ++i)
		w->maskoff(i);
	if (cfg->mute_fm) {
		for (i = 0; i < 6; ++i) w->maskon(i);
		for (i = 11; i < 14; ++i) w->maskon(i);
	}
	if (cfg->mute_ssg) {
		for (i = 6; i < 9; ++i) w->maskon(i);
	}
	if (cfg->mute_adpcm)
		w->maskon(9);
	if (cfg->mute_rhythm)
		w->maskon(10);
}

static int write_temp(const uint8_t *data, size_t len, char *out, size_t cap)
{
#ifdef _WIN32
	char tmpdir[MAX_PATH], name[MAX_PATH];
	DWORD n;
	FILE *fp;
	n = GetTempPathA(MAX_PATH, tmpdir);
	if (!n || n >= MAX_PATH) return -1;
	if (!GetTempFileNameA(tmpdir, "pmd", 0, name)) return -1;
	fp = fopen(name, "wb");
	if (!fp) return -1;
	if (fwrite(data, 1, len, fp) != len) { fclose(fp); DeleteFileA(name); return -1; }
	fclose(fp);
	pc98_bounded(out, cap, name);
	return 0;
#else
	char tmpl[] = "/tmp/xmp-pc98-pmdXXXXXX";
	int fd = mkstemp(tmpl);
	FILE *fp;
	if (fd < 0) return -1;
	fp = fdopen(fd, "wb");
	if (!fp) { close(fd); return -1; }
	if (fwrite(data, 1, len, fp) != len) { fclose(fp); unlink(tmpl); return -1; }
	fclose(fp);
	pc98_bounded(out, cap, tmpl);
	return 0;
#endif
}

static void remove_temp(const char *p)
{
	if (!p || !p[0]) return;
#ifdef _WIN32
	DeleteFileA(p);
#else
	unlink(p);
#endif
}

static void measure(PMDWIN *w, const char *path, const uint8_t *data, size_t len,
		int *one_ms, int *looping, char *title, size_t tcap, char *artist, size_t acap)
{
	int32_t length = 0, loop = 0;
	char tmp[PC98_PATH_MAX];
	const char *use = path;
	int made = 0;
	tmp[0] = '\0';
	if ((!use || !use[0]) && data && len) {
		if (write_temp(data, len, tmp, sizeof tmp) == 0) {
			use = tmp;
			made = 1;
		}
	}
	if (use && use[0] && w->getlength((TCHAR *)use, &length, &loop)) {
		/* PMDWin getlength: length = ms to first loop (one complete play);
		 * loop = loop-section ms only. Playlist one-loop is the first play,
		 * not the tail after L — Night Shifter NS01 is ~106s with a 3.4s L. */
		if (length > 0)
			*one_ms = (int)length;
		*looping = loop > 0;
	}
	if (*one_ms < 100) *one_ms = 1000;
	if (data && len) {
		char dest[1024];
		dest[0] = 0;
		w->getmemo3(dest, (uint8_t *)data, (int32_t)len, 1);
		if (dest[0] && title) pc98_bounded(title, tcap, dest);
		dest[0] = 0;
		w->getmemo3(dest, (uint8_t *)data, (int32_t)len, 2);
		if (dest[0] && artist) pc98_bounded(artist, acap, dest);
	}
	if (made) remove_temp(tmp);
}

static PMDWIN *make_win(const char *filename, const pc98_cfg *cfg)
{
	PMDWIN *w = new PMDWIN();
	char initdir[PC98_PATH_MAX];
	initdir[0] = 0;
	if (cfg && cfg->rhythm_path[0]) pc98_bounded(initdir, sizeof initdir, cfg->rhythm_path);
	else if (cfg && cfg->dll_dir[0]) pc98_bounded(initdir, sizeof initdir, cfg->dll_dir);
	else pc98_dir_of(filename, initdir, sizeof initdir);
	w->init(initdir[0] ? (TCHAR *)initdir : NULL);
	w->setpcmrate(cfg && cfg->rate > 0 ? cfg->rate : SOUND_44K);
	w->setrhythmwithssgeffect(true);
	w->setppsuse(true);
	apply_paths(w, filename, cfg);
	return w;
}

static void fill_known_game(const char *filename, char *artist, size_t acap,
		char *game, size_t gcap)
{
	if (!filename) return;
	/* Hoot id nightsft_98 = Night Shifter (Four-Nine, 1993). .LSP is PMD. */
	if (strstr(filename, "nightsft_98") || strstr(filename, "NIGHTSFT")) {
		if (game && (!game[0] || pc98_ieq(game, "nightsft_98")))
			pc98_bounded(game, gcap, "Night Shifter");
		if (artist && !artist[0])
			pc98_bounded(artist, acap, "Kazumichi Moegi");
	}
}

int pmd_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out)
{
	PMDWIN *w;
	if (!out || !pmd_probe_mem(data, len)) return -1;
	memset(out, 0, sizeof *out);
	out->kind = PC98_KIND_PMD;
	out->songs = 1;
	pc98_bounded(out->filetype, sizeof out->filetype, "PMD");
	pc98_bounded(out->engine, sizeof out->engine, "pmdmini / PMDWin");
	if (filename)
		pc98_basename(filename, out->title, sizeof out->title);
	w = make_win(filename, cfg);
	measure(w, filename, data, len, &out->one_loop_ms[0], &out->looping,
			out->title, sizeof out->title, out->artist, sizeof out->artist);
	fill_known_game(filename, out->artist, sizeof out->artist,
			out->game, sizeof out->game);
	delete w;
	return 0;
}

void *pmd_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg)
{
	pmd_state *s;
	int rc;
	if (!pmd_probe_mem(data, len)) return NULL;
	s = new pmd_state();
	s->rate = cfg && cfg->rate > 0 ? cfg->rate : PC98_DEFAULT_RATE;
	s->loops_want = cfg && cfg->loop_count > 0 ? cfg->loop_count : 1;
	s->win = make_win(filename, cfg);
	if (filename && filename[0])
		rc = s->win->music_load((TCHAR *)filename);
	else
		rc = s->win->music_load2((uint8_t *)data, (int32_t)len);
	if (rc != PMDWIN_OK && rc < WARNING_PPC_ALREADY_LOAD) {
		delete s->win;
		delete s;
		return NULL;
	}
	measure(s->win, filename, data, len, &s->one_loop_ms, &s->looping,
			s->title, sizeof s->title, s->artist, sizeof s->artist);
	fill_known_game(filename, s->artist, sizeof s->artist, NULL, 0);
	apply_mute(s->win, cfg);
	s->win->music_start();
	s->frames_left = ((int64_t)s->one_loop_ms * s->loops_want * s->rate) / 1000;
	if (s->frames_left < s->rate)
		s->frames_left = s->rate;
	return s;
}

void pmd_close_h(void *h)
{
	pmd_state *s = (pmd_state *)h;
	if (!s) return;
	if (s->win) {
		s->win->music_stop();
		delete s->win;
	}
	delete s;
}

int pmd_process_h(void *h, float *buf, int count)
{
	pmd_state *s = (pmd_state *)h;
	int n, i;
	if (!s || !buf || count <= 0 || s->ended) return 0;
	n = count;
	if (s->frames_left > 0 && n > (int)s->frames_left)
		n = (int)s->frames_left;
	s->tmp.resize((size_t)n * 2);
	s->win->getpcmdata(s->tmp.data(), n);
	/* pmdmini ymfm applies FM ×2 at SetVolumeFM(0) (scale /32768 not /65536).
	 * fmgen S98 is unity at 0 dB. Halve so PMD sits with those mixes. */
	for (i = 0; i < n; ++i) {
		float l = (float)s->tmp[i * 2] / 32768.0f * 0.5f;
		float r = (float)s->tmp[i * 2 + 1] / 32768.0f * 0.5f;
		if (l > 1.0f) l = 1.0f;
		if (l < -1.0f) l = -1.0f;
		if (r > 1.0f) r = 1.0f;
		if (r < -1.0f) r = -1.0f;
		buf[i * 2] = l;
		buf[i * 2 + 1] = r;
	}
	if (s->frames_left > 0) {
		s->frames_left -= n;
		if (s->frames_left <= 0) s->ended = 1;
	}
	return n;
}

int pmd_seek_ms_h(void *h, int ms)
{
	pmd_state *s = (pmd_state *)h;
	if (!s || ms < 0) return -1;
	s->win->setpos(ms);
	s->ended = 0;
	s->frames_left = ((int64_t)(s->one_loop_ms * s->loops_want - ms) * s->rate) / 1000;
	if (s->frames_left < 0) s->frames_left = 0;
	return ms;
}

int pmd_one_loop_ms_h(void *h)
{
	pmd_state *s = (pmd_state *)h;
	return s ? s->one_loop_ms : 0;
}

int pmd_rate_h(void *h)
{
	pmd_state *s = (pmd_state *)h;
	return s ? s->rate : PC98_DEFAULT_RATE;
}

void pmd_set_loops_h(void *h, int loops)
{
	pmd_state *s = (pmd_state *)h;
	if (!s) return;
	if (loops < 1) loops = 1;
	if (loops > 3) loops = 3;
	s->loops_want = loops;
	s->frames_left = ((int64_t)s->one_loop_ms * s->loops_want * s->rate) / 1000;
}

void pmd_apply_mute_h(void *h, const pc98_cfg *cfg)
{
	pmd_state *s = (pmd_state *)h;
	if (!s || !s->win) return;
	apply_mute(s->win, cfg);
}

const char *pmd_title_h(void *h)
{
	pmd_state *s = (pmd_state *)h;
	return s ? s->title : "";
}

const char *pmd_artist_h(void *h)
{
	pmd_state *s = (pmd_state *)h;
	return s ? s->artist : "";
}

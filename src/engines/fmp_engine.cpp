/*
 * FMP / MMD via optional WinFMP.dll (C60), loaded at runtime from the
 * plugin folder. Same IFMPMD C exports as PMDWin when present.
 */
#include "fmp_engine.h"
#include "pc98_util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#ifndef WINAPI
#define WINAPI
#endif
#endif

typedef int (WINAPI *fmp_int0)(void);
typedef int (WINAPI *fmp_init_t)(char *path);
typedef int (WINAPI *fmp_load_t)(char *filename);
typedef int (WINAPI *fmp_load2_t)(uint8_t *data, int size);
typedef void (WINAPI *fmp_void0)(void);
typedef void (WINAPI *fmp_rate_t)(int rate);
typedef void (WINAPI *fmp_pcm_t)(int16_t *buf, int nsamples);
typedef int (WINAPI *fmp_len_t)(char *filename, int *length, int *loop);
typedef void (WINAPI *fmp_pos_t)(int pos);
typedef int (WINAPI *fmp_getpos_t)(void);
typedef char *(WINAPI *fmp_memo_t)(char *dest, uint8_t *data, int size, int al);
typedef int (WINAPI *fmp_mask_t)(int ch);

struct fmp_api {
#ifdef _WIN32
	HMODULE mod;
#endif
	int loaded;
	fmp_init_t init;
	fmp_load_t music_load;
	fmp_load2_t music_load2;
	fmp_void0 music_start;
	fmp_void0 music_stop;
	fmp_pcm_t getpcmdata;
	fmp_len_t getlength;
	fmp_rate_t setpcmrate;
	fmp_pos_t setpos;
	fmp_getpos_t getpos;
	fmp_memo_t getmemo3;
	fmp_mask_t maskon;
	fmp_mask_t maskoff;
};

#ifdef _WIN32
static FARPROC fmp_sym(HMODULE m, const char *name)
{
	FARPROC p = GetProcAddress(m, name);
	if (p) return p;
	{
		char decorated[64];
		snprintf(decorated, sizeof decorated, "_%s@4", name);
		p = GetProcAddress(m, decorated);
		if (p) return p;
		snprintf(decorated, sizeof decorated, "_%s@8", name);
		p = GetProcAddress(m, decorated);
		if (p) return p;
		snprintf(decorated, sizeof decorated, "_%s@16", name);
		p = GetProcAddress(m, decorated);
	}
	return p;
}

static int fmp_bind(fmp_api *a, HMODULE m)
{
	a->init = (fmp_init_t)fmp_sym(m, "pmdwininit");
	if (!a->init) a->init = (fmp_init_t)fmp_sym(m, "init");
	if (!a->init) a->init = (fmp_init_t)fmp_sym(m, "fmpinit");
	a->music_load = (fmp_load_t)fmp_sym(m, "music_load");
	a->music_load2 = (fmp_load2_t)fmp_sym(m, "music_load2");
	a->music_start = (fmp_void0)fmp_sym(m, "music_start");
	a->music_stop = (fmp_void0)fmp_sym(m, "music_stop");
	a->getpcmdata = (fmp_pcm_t)fmp_sym(m, "getpcmdata");
	a->getlength = (fmp_len_t)fmp_sym(m, "getlength");
	a->setpcmrate = (fmp_rate_t)fmp_sym(m, "setpcmrate");
	a->setpos = (fmp_pos_t)fmp_sym(m, "setpos");
	a->getpos = (fmp_getpos_t)fmp_sym(m, "getpos");
	a->getmemo3 = (fmp_memo_t)fmp_sym(m, "getmemo3");
	a->maskon = (fmp_mask_t)fmp_sym(m, "maskon");
	a->maskoff = (fmp_mask_t)fmp_sym(m, "maskoff");
	return a->music_load && a->getpcmdata && a->music_start ? 1 : 0;
}

static int fmp_load_dll(fmp_api *a, const pc98_cfg *cfg)
{
	char path[PC98_PATH_MAX];
	HMODULE m;
	memset(a, 0, sizeof *a);
	path[0] = 0;
	if (cfg && cfg->dll_dir[0])
		snprintf(path, sizeof path, "%sWinFMP.dll", cfg->dll_dir);
	m = path[0] ? LoadLibraryA(path) : NULL;
	if (!m) m = LoadLibraryA("WinFMP.dll");
	if (!m) return 0;
	if (!fmp_bind(a, m)) {
		FreeLibrary(m);
		return 0;
	}
	a->mod = m;
	a->loaded = 1;
	if (a->init) {
		char *dir = (cfg && cfg->rhythm_path[0]) ? (char *)cfg->rhythm_path :
			(cfg && cfg->dll_dir[0]) ? (char *)cfg->dll_dir : NULL;
		a->init(dir);
	}
	if (a->setpcmrate)
		a->setpcmrate(cfg && cfg->rate > 0 ? cfg->rate : PC98_DEFAULT_RATE);
	return 1;
}
#else
static int fmp_load_dll(fmp_api *a, const pc98_cfg *cfg)
{
	(void)cfg;
	memset(a, 0, sizeof *a);
	return 0;
}
#endif

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

int fmp_probe_mem(const uint8_t *data, size_t len)
{
	uint16_t w0, w1;
	if (!data || len < 16) return 0;
	if (data[0] <= 0x0f && (data[1] == 0x18 || data[1] == 0x1a) &&
			(data[2] == 0 || data[2] == 0xe6))
		return 0; /* PMD */
	if (len >= 3 && data[0] == 'S' && data[1] == '9' && data[2] == '8')
		return 0;
	if (pc98_looks_bgmdrv(data, len))
		return 0; /* PC-98 BGMDRV .MUS — not FMP */
	if (pc98_looks_msb(data, len))
		return 0; /* MBMUS .MSB */
	if (pc98_looks_opnmd(data, len))
		return 0; /* OPNDRV .MD */
	if (pc98_looks_ntl_any(data, len))
		return 0; /* ArtDink .NTL (FM or MIDI) */
	if (pc98_looks_fmdpack(data, len))
		return 0; /* packed FMD GS/MT-32, not WinFMP */
	if (data[0] == 'M' && data[1] == 'Z') return 0;
	w0 = le16(data);
	w1 = le16(data + 2);
	/* FMD: part-count 1..32, next word is an offset. */
	if (w0 >= 1 && w0 <= 32)
		return 1;
	/* OPI/OVI/OZI: first word is file size (or close). CheckFile often
	 * only has a 64-byte peek, so accept a large size-like word. */
	if (w0 > 256 && w1 < w0)
		return 1;
	/* Full-file confirm: first word near EOF. */
	if (len > 64 && w0 > 64 && w0 <= len && (len - w0) < 2048)
		return 1;
	/* MMD: small first words */
	if (w0 > 32 && w0 < 0x200 && w1 < 0x200)
		return 1;
	return 0;
}

struct fmp_state {
	fmp_api api;
	int rate;
	int loops_want;
	int one_loop_ms;
	int looping;
	int ended;
	int64_t frames_left;
	int owns_dll;
	char title[256];
	std::vector<int16_t> tmp;
};

static void fmp_apply_mute(fmp_api *a, const pc98_cfg *cfg)
{
	int i;
	if (!a || !a->maskon || !cfg) return;
	if (a->maskoff) {
		for (i = 0; i < 16; ++i) a->maskoff(i);
	}
	if (cfg->mute_fm) { for (i = 0; i < 6; ++i) a->maskon(i); }
	if (cfg->mute_ssg) { for (i = 6; i < 9; ++i) a->maskon(i); }
	if (cfg->mute_adpcm && a->maskon) a->maskon(9);
	if (cfg->mute_rhythm && a->maskon) a->maskon(10);
}

static int fmp_measure(fmp_api *a, const char *filename, const uint8_t *data, size_t len,
		int *one_ms, int *looping, char *title, size_t tcap)
{
	int length = 0, loop = 0;
	if (a->getmemo3 && data && title) {
		char dest[1024];
		dest[0] = 0;
		a->getmemo3(dest, (uint8_t *)data, (int)len, 1);
		if (dest[0]) pc98_bounded(title, tcap, dest);
	}
	if (a->getlength && filename && filename[0] &&
			a->getlength((char *)filename, &length, &loop)) {
		/* Same PMDWin-style API as pmd_engine: length = first complete play. */
		if (length > 0)
			*one_ms = length;
		*looping = loop > 0;
	}
	if (*one_ms < 100) *one_ms = 1000;
	return 0;
}

int fmp_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out)
{
	fmp_api api;
	if (!out || !fmp_probe_mem(data, len)) return -1;
	memset(out, 0, sizeof *out);
	out->kind = PC98_KIND_FMP;
	out->songs = 1;
	pc98_bounded(out->filetype, sizeof out->filetype, "FMP");
	pc98_bounded(out->engine, sizeof out->engine, "WinFMP");
	if (filename) pc98_basename(filename, out->title, sizeof out->title);
	out->one_loop_ms[0] = 1000;
	if (fmp_load_dll(&api, cfg)) {
		fmp_measure(&api, filename, data, len, &out->one_loop_ms[0],
				&out->looping, out->title, sizeof out->title);
#ifdef _WIN32
		if (api.mod) FreeLibrary(api.mod);
#endif
	}
	return 0;
}

void *fmp_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg)
{
	fmp_state *s;
	int rc = -1;
	if (!fmp_probe_mem(data, len)) return NULL;
	s = new fmp_state();
	s->rate = cfg && cfg->rate > 0 ? cfg->rate : PC98_DEFAULT_RATE;
	s->loops_want = cfg && cfg->loop_count > 0 ? cfg->loop_count : 1;
	if (!fmp_load_dll(&s->api, cfg)) {
		delete s;
		return NULL;
	}
	s->owns_dll = 1;
	if (s->api.music_load && filename && filename[0])
		rc = s->api.music_load((char *)filename);
	else if (s->api.music_load2 && data)
		rc = s->api.music_load2((uint8_t *)data, (int)len);
	if (rc != 0 && rc < 13) {
#ifdef _WIN32
		if (s->api.mod) FreeLibrary(s->api.mod);
#endif
		delete s;
		return NULL;
	}
	fmp_measure(&s->api, filename, data, len, &s->one_loop_ms, &s->looping,
			s->title, sizeof s->title);
	fmp_apply_mute(&s->api, cfg);
	s->api.music_start();
	s->frames_left = ((int64_t)s->one_loop_ms * s->loops_want * s->rate) / 1000;
	if (s->frames_left < s->rate) s->frames_left = s->rate;
	return s;
}

void fmp_close_h(void *h)
{
	fmp_state *s = (fmp_state *)h;
	if (!s) return;
	if (s->api.music_stop) s->api.music_stop();
#ifdef _WIN32
	if (s->owns_dll && s->api.mod) FreeLibrary(s->api.mod);
#endif
	delete s;
}

int fmp_process_h(void *h, float *buf, int count)
{
	fmp_state *s = (fmp_state *)h;
	int n, i;
	if (!s || !buf || count <= 0 || s->ended) return 0;
	n = count;
	if (s->frames_left > 0 && n > (int)s->frames_left)
		n = (int)s->frames_left;
	s->tmp.resize((size_t)n * 2);
	s->api.getpcmdata(s->tmp.data(), n);
	for (i = 0; i < n; ++i) {
		buf[i * 2] = (float)s->tmp[i * 2] / 32768.0f;
		buf[i * 2 + 1] = (float)s->tmp[i * 2 + 1] / 32768.0f;
	}
	if (s->frames_left > 0) {
		s->frames_left -= n;
		if (s->frames_left <= 0) s->ended = 1;
	}
	return n;
}

int fmp_seek_ms_h(void *h, int ms)
{
	fmp_state *s = (fmp_state *)h;
	if (!s || ms < 0) return -1;
	if (s->api.setpos) s->api.setpos(ms);
	s->ended = 0;
	s->frames_left = ((int64_t)(s->one_loop_ms * s->loops_want - ms) * s->rate) / 1000;
	if (s->frames_left < 0) s->frames_left = 0;
	return ms;
}

int fmp_one_loop_ms_h(void *h)
{
	fmp_state *s = (fmp_state *)h;
	return s ? s->one_loop_ms : 0;
}

int fmp_rate_h(void *h)
{
	fmp_state *s = (fmp_state *)h;
	return s ? s->rate : PC98_DEFAULT_RATE;
}

void fmp_set_loops_h(void *h, int loops)
{
	fmp_state *s = (fmp_state *)h;
	if (!s) return;
	if (loops < 1) loops = 1;
	if (loops > 3) loops = 3;
	s->loops_want = loops;
}

void fmp_apply_mute_h(void *h, const pc98_cfg *cfg)
{
	fmp_state *s = (fmp_state *)h;
	if (!s) return;
	fmp_apply_mute(&s->api, cfg);
}

const char *fmp_title_h(void *h)
{
	fmp_state *s = (fmp_state *)h;
	return s ? s->title : "";
}

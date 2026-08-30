/*
 * FUGA packed FMD (.GMD / .MMD): XOR 0xA5, RCP v2 tracks (ValleyBell fuga2rcp).
 * .GMD is Roland GS (SC-55). .MMD is CM-32L + CM-32P (CM-64).
 * Rendered with TinySoundFont + a user SF2. GS/MT reset, master volume,
 * and reverb come from RCP 0x98 / 0xDD–0xDF (Oerstedia GMD/MMD comments).
 * We never claim .mid.
 */
#include "fmd_engine.h"
#include "pc98_util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <vector>
#include <algorithm>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#define TSF_IMPLEMENTATION
#define _USE_MATH_DEFINES
#include "tsf.h"

#define FMD_TICKRES   48
#define FMD_MAX_EV    300000
#define FMD_MAX_MEAS  4096
#define FMD_MAX_RUN   256
#define FMD_TRK       18
#define FMD_REV_MAX   4096

enum {
	EV_NOTEON  = 0x90,
	EV_NOTEOFF = 0x80,
	EV_CC      = 0xB0,
	EV_PC      = 0xC0,
	EV_AT      = 0xD0,
	EV_PB      = 0xE0,
	EV_TEMPO   = 0xFF,
	EV_MIX     = 0xF1
};

enum {
	MIX_MASTER  = 0,
	MIX_REV     = 1,
	MIX_REVTIME = 2,
	MIX_GSRESET = 3,
	MIX_MTRESET = 4,
	MIX_BEND    = 5
};

struct fmd_ev {
	uint32_t tick;
	uint8_t st, ch, a, b;
};

struct fmd_run {
	uint8_t ch, note;
	uint32_t off;
};

/* MT-32 program → GM bank 0 (MMD through a GS-ordered CM-64/MT-32 SF2). */
static const uint8_t k_mt32_gm[128] = {
	0, 1, 2, 4, 4, 5, 5, 3, 16, 16, 16, 16, 19, 19, 19, 21,
	6, 6, 6, 7, 7, 7, 8, 8, 62, 63, 62, 63, 38, 39, 38, 39,
	88, 89, 52, 98, 96, 97, 86, 39, 14, 96, 76, 80, 48, 49, 45, 48,
	40, 40, 42, 42, 43, 46, 46, 24, 25, 26, 27, 104, 32, 32, 33, 34,
	36, 37, 35, 35, 73, 73, 72, 72, 74, 75, 64, 65, 66, 67, 71, 71,
	68, 69, 70, 22, 56, 56, 57, 57, 60, 60, 58, 61, 61, 11, 11, 99,
	9, 14, 13, 12, 107, 107, 77, 78, 78, 76, 47, 117, 118, 118, 118, 116,
	116, 119, 115, 112, 55, 124, 123, 117, 14, 47, 0, 0, 0, 0, 0, 0
};

static tsf *g_sf;
static char g_sf_path[PC98_PATH_MAX];

struct fmd_state {
	std::vector<uint8_t> file;
	std::vector<fmd_ev> ev;
	char title[256];
	char game[256];
	char engine[64];
	char filetype[16];
	char sf2_path[PC98_PATH_MAX];
	int rate;
	int loops_want;
	int one_loop_ms;
	int is_mt;
	int mute_rhy;
	int mute_mel;
	int bpm;
	int tickres;
	uint32_t loop_tick;
	uint32_t end_tick;
	uint32_t play_limit;
	uint32_t play_samples;
	size_t ev_i;
	uint32_t cur_tick;
	int64_t tick_acc;
	int tempo_scale;
	int ended;
	int tick_left;
	tsf *sf;
	int own_sf;
	float master_gain;
	float rev_wet;
	float rev_fb;
	int rev_len;
	int rev_i;
	float rev_l[FMD_REV_MAX];
	float rev_r[FMD_REV_MAX];
	float cho_wet;
	int cho_len;
	int cho_i;
	float cho_l[FMD_REV_MAX];
	float cho_r[FMD_REV_MAX];
};

static int path_ok(const char *p)
{
	FILE *f;
	if (!p || !p[0]) return 0;
	f = fopen(p, "rb");
	if (!f) return 0;
	fclose(f);
	return 1;
}

static void join2(char *dst, size_t cap, const char *a, const char *b)
{
	size_t n;
	if (!dst || cap < 4) return;
	dst[0] = 0;
	if (!a || !a[0]) {
		pc98_bounded(dst, cap, b ? b : "");
		return;
	}
	n = strlen(a);
	if (n && (a[n - 1] == '\\' || a[n - 1] == '/'))
		snprintf(dst, cap, "%s%s", a, b);
	else
		snprintf(dst, cap, "%s\\%s", a, b);
}

static int score_sf(const char *name, int mt)
{
	char l[256];
	int i, s = 0;
	if (!name) return 0;
	for (i = 0; name[i] && i < 255; i++)
		l[i] = (char)tolower((unsigned char)name[i]);
	l[i] = 0;
	if (!strstr(l, ".sf2")) return 0;
	if (mt) {
		if (strstr(l, "cm-64") || strstr(l, "cm64")) s += 60;
		if (strstr(l, "cm-32") || strstr(l, "cm32")) s += 50;
		if (strstr(l, "mt32") || strstr(l, "mt-32")) s += 40;
		if (strstr(l, "sc-55") || strstr(l, "sc55")) s -= 25;
	} else {
		if (strstr(l, "mkii") || strstr(l, "mk2") || strstr(l, "sc-55-2") ||
				strstr(l, "sc55mk"))
			s += 70;
		if (strstr(l, "sc-55") || strstr(l, "sc55")) s += 45;
		if (strstr(l, "cm-64") || strstr(l, "mt32") || strstr(l, "cm32"))
			s -= 25;
	}
	return s;
}

#ifdef _WIN32
static int best_in_dir(const char *dir, int mt, char *out, size_t cap)
{
	char pat[PC98_PATH_MAX];
	WIN32_FIND_DATAA fd;
	HANDLE h;
	int best = 0;
	if (!dir || !dir[0]) return 0;
	join2(pat, sizeof pat, dir, "*.sf2");
	h = FindFirstFileA(pat, &fd);
	if (h == INVALID_HANDLE_VALUE) return 0;
	do {
		int sc = score_sf(fd.cFileName, mt);
		if (sc > best) {
			join2(out, cap, dir, fd.cFileName);
			best = sc;
		}
	} while (FindNextFileA(h, &fd));
	FindClose(h);
	return best > 0 && path_ok(out);
}
#else
static int best_in_dir(const char *, int, char *, size_t) { return 0; }
#endif

static int try_named(const char *dir, const char *name, char *out, size_t cap)
{
	join2(out, cap, dir, name);
	return path_ok(out);
}

#ifdef __cplusplus
extern "C"
#endif
int fmd_find_sf2(const pc98_cfg *cfg, const char *filename, int mt,
		char *out, size_t cap)
{
	const char *pref = mt ?
		(cfg && cfg->mt_sf2[0] ? cfg->mt_sf2 : NULL) :
		(cfg && cfg->gs_sf2[0] ? cfg->gs_sf2 : NULL);
	char dir[PC98_PATH_MAX];
	const char *gsn[] = {
		"SC-55mkII.sf2", "SC-55mk2.sf2", "SC55mkII.sf2", "SC-55.sf2",
		"sc55.sf2", NULL
	};
	const char *mtn[] = {
		"CM-64.sf2", "CM64.sf2", "CM-32L.sf2", "CM32L.sf2",
		"MT32 GS 2.51.sf2", "MT-32.sf2", "MT32.sf2", NULL
	};
	const char *const *names = mt ? mtn : gsn;
	int i;
	const char *fixed[] = {
		"C:\\Program Files (x86)\\XMPlay\\Plugins\\midi soundfonts",
		"C:\\Program Files\\XMPlay\\Plugins\\midi soundfonts",
		NULL
	};

	if (pref && path_ok(pref)) {
		pc98_bounded(out, cap, pref);
		return 1;
	}
	if (cfg && cfg->dll_dir[0]) {
		for (i = 0; names[i]; i++)
			if (try_named(cfg->dll_dir, names[i], out, cap)) return 1;
		join2(dir, sizeof dir, cfg->dll_dir, "midi soundfonts");
		for (i = 0; names[i]; i++)
			if (try_named(dir, names[i], out, cap)) return 1;
		if (best_in_dir(dir, mt, out, cap)) return 1;
		if (best_in_dir(cfg->dll_dir, mt, out, cap)) return 1;
		pc98_dir_of(cfg->dll_dir, dir, sizeof dir);
		if (dir[0]) {
			join2(dir, sizeof dir, dir, "midi soundfonts");
			if (best_in_dir(dir, mt, out, cap)) return 1;
		}
	}
	if (filename) {
		pc98_dir_of(filename, dir, sizeof dir);
		if (dir[0] && best_in_dir(dir, mt, out, cap)) return 1;
	}
	for (i = 0; fixed[i]; i++) {
		for (int n = 0; names[n]; n++)
			if (try_named(fixed[i], names[n], out, cap)) return 1;
		if (best_in_dir(fixed[i], mt, out, cap)) return 1;
	}
	out[0] = 0;
	return 0;
}

#ifdef __cplusplus
extern "C"
#endif
void *fmd_font_get(const char *path)
{
	if (!path || !path[0]) return NULL;
	if (g_sf && g_sf_path[0] && pc98_ieq(g_sf_path, path))
		return g_sf;
	if (g_sf) {
		tsf_close(g_sf);
		g_sf = NULL;
		g_sf_path[0] = 0;
	}
	g_sf = tsf_load_filename(path);
	if (!g_sf) return NULL;
	pc98_bounded(g_sf_path, sizeof g_sf_path, path);
	tsf_set_max_voices(g_sf, 128);
	return g_sf;
}

static void push_ev(std::vector<fmd_ev> *ev, uint32_t tick, uint8_t st,
		uint8_t ch, uint8_t a, uint8_t b)
{
	fmd_ev e;
	if (ev->size() >= FMD_MAX_EV) return;
	e.tick = tick;
	e.st = st;
	e.ch = (uint8_t)(ch & 15);
	e.a = a;
	e.b = b;
	ev->push_back(e);
}

static void run_off(std::vector<fmd_ev> *ev, fmd_run *run, int nrun, uint32_t now)
{
	int i;
	for (i = 0; i < nrun; i++) {
		if (run[i].off && run[i].off <= now) {
			push_ev(ev, run[i].off, EV_NOTEOFF, run[i].ch, run[i].note, 0);
			run[i].off = 0;
		}
	}
}

static void run_cut(std::vector<fmd_ev> *ev, fmd_run *run, int nrun, uint32_t now)
{
	int i;
	for (i = 0; i < nrun; i++) {
		if (run[i].off) {
			push_ev(ev, now, EV_NOTEOFF, run[i].ch, run[i].note, 0);
			run[i].off = 0;
		}
	}
}

static void run_on(std::vector<fmd_ev> *ev, fmd_run *run, int *nrun,
		uint32_t now, uint8_t ch, uint8_t note, uint8_t vel, uint16_t dur)
{
	int i, slot = -1;
	if (!dur || !vel) return;
	for (i = 0; i < *nrun; i++) {
		if (run[i].off && run[i].ch == ch && run[i].note == note) {
			push_ev(ev, now, EV_NOTEOFF, ch, note, 0);
			run[i].off = now + dur;
			push_ev(ev, now, EV_NOTEON, ch, note, vel);
			return;
		}
		if (!run[i].off && slot < 0)
			slot = i;
	}
	push_ev(ev, now, EV_NOTEON, ch, note, vel);
	if (slot >= 0) {
		run[slot].ch = ch;
		run[slot].note = note;
		run[slot].off = now + dur;
		return;
	}
	if (*nrun < FMD_MAX_RUN) {
		run[*nrun].ch = ch;
		run[*nrun].note = note;
		run[*nrun].off = now + dur;
		(*nrun)++;
	} else {
		/* table full: still emit a matching off so the note cannot stick */
		push_ev(ev, now + dur, EV_NOTEOFF, ch, note, 0);
	}
}

static void run_flush(std::vector<fmd_ev> *ev, fmd_run *run, int nrun)
{
	int i;
	for (i = 0; i < nrun; i++) {
		if (run[i].off)
			push_ev(ev, run[i].off, EV_NOTEOFF, run[i].ch, run[i].note, 0);
	}
}

static unsigned rcp_syx(const uint8_t *in, unsigned nin, uint8_t *out,
		unsigned cap, uint8_t p1, uint8_t p2, uint8_t ch)
{
	unsigned i, o = 0;
	unsigned sum = 0;
	for (i = 0; i < nin && o < cap; i++) {
		uint8_t d = in[i];
		if (d & 0x80) {
			if (d == 0x80) d = p1;
			else if (d == 0x81) d = p2;
			else if (d == 0x82) d = ch;
			else if (d == 0x83) { sum = 0; continue; }
			else if (d == 0x84) d = (uint8_t)((0x100 - sum) & 0x7F);
			else if (d == 0xF7) {
				out[o++] = 0xF7;
				return o;
			} else
				continue;
		}
		out[o++] = d;
		sum += d;
	}
	return o;
}

static int gs_part_ch(unsigned p)
{
	p &= 15;
	if (p == 0) return 9;
	if (p <= 9) return (int)p - 1;
	return (int)p;
}

static void apply_roland(std::vector<fmd_ev> *ev, uint32_t tick,
		const uint8_t gs[6])
{
	unsigned addr = ((unsigned)gs[2] << 16) | ((unsigned)gs[3] << 8) | gs[4];
	uint8_t v = gs[5];
	if (gs[1] == 0x42) {
		if (addr == 0x40007F) {
			push_ev(ev, tick, EV_MIX, 0, MIX_GSRESET, 0);
			return;
		}
		if (addr == 0x400004) {
			push_ev(ev, tick, EV_MIX, 0, MIX_MASTER, v);
			return;
		}
		if (addr == 0x400130)
			return;
		if (addr == 0x400133) {
			push_ev(ev, tick, EV_MIX, 0, MIX_REV, v);
			return;
		}
		if (addr == 0x400134) {
			push_ev(ev, tick, EV_MIX, 0, MIX_REVTIME, v);
			return;
		}
		if ((gs[2] == 0x40) && ((gs[3] & 0xF0) == 0x10 || (gs[3] & 0xF0) == 0x20)) {
			int ch = gs_part_ch(gs[3]);
			if (gs[4] == 0x15 && v)
				push_ev(ev, tick, EV_PC, (uint8_t)ch, 0, 1);
			if (gs[4] == 0x19)
				push_ev(ev, tick, EV_CC, (uint8_t)ch, 91, v);
			if (gs[4] == 0x10) {
				int br = v;
				/* Recomposer writes 0x40+semitones (OERS_002: 0x4C = 12). */
				if (br >= 0x40 && br <= 0x58)
					br -= 0x40;
				if (br > 0 && br <= 24)
					push_ev(ev, tick, EV_MIX, (uint8_t)ch,
							MIX_BEND, (uint8_t)br);
			}
		}
		return;
	}
	if (gs[1] == 0x16) {
		if (gs[2] == 0x7F)
			push_ev(ev, tick, EV_MIX, 0, MIX_MTRESET, 0);
		else if (gs[2] == 0x10 && gs[3] == 0x00 && gs[4] == 0x16)
			push_ev(ev, tick, EV_MIX, 0, MIX_MASTER, v > 100 ? 100 : v);
		else if (gs[2] == 0x10 && gs[3] == 0x00 && gs[4] == 0x03)
			push_ev(ev, tick, EV_MIX, 0, MIX_REV, (uint8_t)(v * 18));
		else if (gs[2] == 0x10 && gs[3] == 0x00 && gs[4] == 0x02)
			push_ev(ev, tick, EV_MIX, 0, MIX_REVTIME, (uint8_t)(v * 18));
		else if (gs[2] == 0x52 && gs[3] == 0x00 && gs[4] == 0x10)
			push_ev(ev, tick, EV_MIX, 0, MIX_MASTER, v > 100 ? 100 : v);
		else if (gs[2] == 0x52 && gs[3] == 0x00 && gs[4] == 0x03)
			push_ev(ev, tick, EV_MIX, 0, MIX_REV, (uint8_t)(v * 18));
	}
}

static void apply_syx_bytes(std::vector<fmd_ev> *ev, uint32_t tick,
		const uint8_t *s, unsigned n)
{
	uint8_t gs[6];
	if (n < 8 || s[0] != 0x41) return;
	if (s[3] != 0x12) return;
	gs[0] = s[1];
	gs[1] = s[2];
	gs[2] = s[4];
	gs[3] = s[5];
	gs[4] = s[6];
	gs[5] = s[7];
	apply_roland(ev, tick, gs);
}

static int expand_trk(const uint8_t *d, size_t n, unsigned base, unsigned tlen,
		int gbl_tr, int bpm, std::vector<fmd_ev> *ev,
		uint32_t *loop_tick, uint32_t *max_tick, int *saw_cm, char *title)
{
	unsigned end, pos, parent = 0;
	unsigned meas[FMD_MAX_MEAS];
	int nmeas = 0;
	uint8_t rhythm, mid, mute, cmd, p1, p2;
	int8_t tr, start;
	uint16_t dly, dur;
	uint32_t tick = 0;
	int midi_dev, ch, dummy;
	int lp_i = 0;
	unsigned lp_pos[8];
	uint32_t lp_tick[8];
	uint16_t lp_cnt[8];
	fmd_run run[FMD_MAX_RUN];
	int nrun = 0;
	int ended = 0;
	int steps = 0;
	uint8_t gs[6];

	memset(gs, 0, sizeof gs);
	gs[0] = 0x10;
	gs[1] = 0x42;

	(void)bpm;
	if (base + 0x2C > n) return -1;
	end = base + tlen;
	if (end > n) end = (unsigned)n;
	pos = base + 2;
	rhythm = d[pos + 1];
	mid = d[pos + 2];
	tr = (int8_t)d[pos + 3];
	start = (int8_t)d[pos + 4];
	mute = d[pos + 5];
	pos += 0x2A;
	if (mute == 1) return 0;
	dummy = (mid & 0x80) != 0;
	if (dummy) return 0;
	ch = mid & 0x0F;
	midi_dev = mid >> 4;
	(void)midi_dev;
	(void)rhythm;
	if (tr & 0x80)
		tr = 0;
	else {
		if (tr & 0x40) tr = (int8_t)(tr | (int8_t)0x80);
		tr = (int8_t)(tr + gbl_tr);
	}
	if (start > 0)
		tick = (uint32_t)start;
	meas[nmeas++] = pos;

	while (pos + 4 <= end && !ended && steps++ < 400000 && tick < 120000) {
		unsigned prev = pos;
		cmd = d[pos];
		dly = d[pos + 1];
		p1 = d[pos + 2];
		dur = p1;
		p2 = d[pos + 3];
		pos += 4;
		run_off(ev, run, nrun, tick);

		if (cmd < 0x80) {
			int note = (cmd + tr) & 0x7F;
			run_on(ev, run, &nrun, tick, (uint8_t)ch, (uint8_t)note, p2, dur);
		} else switch (cmd) {
		case 0x98: {
			uint8_t raw[256], out[256];
			unsigned n = 0, m;
			while (pos + 4 <= end && d[pos] == 0xF7 && n + 2 < sizeof raw) {
				raw[n++] = d[pos + 2];
				raw[n++] = d[pos + 3];
				pos += 4;
			}
			m = rcp_syx(raw, n, out, sizeof out, p1, p2, (uint8_t)ch);
			apply_syx_bytes(ev, tick, out, m);
			break;
		}
		case 0xDD:
			gs[2] = p1;
			gs[3] = p2;
			break;
		case 0xDE:
			gs[4] = p1;
			gs[5] = p2;
			apply_roland(ev, tick, gs);
			break;
		case 0xDF:
			gs[0] = p1;
			gs[1] = p2;
			break;
		case 0xE2:
			push_ev(ev, tick, EV_CC, (uint8_t)ch, 0, p2);
			push_ev(ev, tick, EV_PC, (uint8_t)ch, p1, 0);
			break;
		case 0xE6:
			p1--;
			if (p1 & 0x80)
				ch = 0;
			else
				ch = p1 & 0x0F;
			break;
		case 0xE7:
			push_ev(ev, tick, EV_TEMPO, 0, p1 ? p1 : 64, 0);
			break;
		case 0xEA:
			push_ev(ev, tick, EV_AT, (uint8_t)ch, p1, 0);
			break;
		case 0xEB:
			push_ev(ev, tick, EV_CC, (uint8_t)ch, p1, p2);
			break;
		case 0xEC:
			if (p1 < 0x80)
				push_ev(ev, tick, EV_PC, (uint8_t)ch, p1, 0);
			break;
		case 0xEE:
			push_ev(ev, tick, EV_PB, (uint8_t)ch, p1, p2);
			break;
		case 0xF6: {
			char buf[256];
			int o = 0;
			if (o < 254) { buf[o++] = (char)p1; buf[o++] = (char)p2; }
			while (pos + 4 <= end && d[pos] == 0xF7) {
				if (o < 254) {
					buf[o++] = (char)d[pos + 2];
					buf[o++] = (char)d[pos + 3];
				}
				pos += 4;
			}
			buf[o] = 0;
			while (o > 0 && buf[o - 1] == ' ') buf[--o] = 0;
			if (strstr(buf, "CM-32") || strstr(buf, "CM64") || strstr(buf, "CM-64"))
				*saw_cm = 1;
			if (title && !title[0] && buf[0] && buf[0] != '*' && o >= 4)
				pc98_bounded(title, 256, buf);
			dly = 0;
			break;
		}
		case 0xF7:
			break;
		case 0xF8:
			/* A repeated bar must not push/pop the F9/F8 stack. */
			if (parent) {
				dly = 0;
				break;
			}
			if (lp_i > 0) {
				lp_i--;
				lp_cnt[lp_i]++;
				if (dly == 0 || dly >= 0x7F) {
					if (loop_tick && *loop_tick == 0xFFFFFFFFu)
						*loop_tick = lp_tick[lp_i];
					run_cut(ev, run, nrun, tick);
					ended = 1;
				} else if (lp_cnt[lp_i] < dly) {
					/* Recomposer/rcp2mid: jump back in the stream but
					 * never rewind time — otherwise every repeat stacks
					 * on the same ticks (OERS_002 guitar/drums). */
					pos = lp_pos[lp_i];
					lp_i++;
				}
			}
			dly = 0;
			break;
		case 0xF9:
			if (parent) {
				dly = 0;
				break;
			}
			if (lp_i < 8) {
				lp_pos[lp_i] = pos;
				lp_tick[lp_i] = tick;
				lp_cnt[lp_i] = 0;
				lp_i++;
			}
			dly = 0;
			break;
		case 0xFC:
			/* Repeat the target bar (until FD). Do not skip inside F8 —
			 * OERS_002's guitar track FC-jumps to the intro phrase. */
			if (parent) {
				pos = parent;
				parent = 0;
			} else {
				int hops = 0;
				pos = prev;
				do {
					unsigned rpos;
					if (pos + 4 > end || hops++ > 32) break;
					p1 = d[pos + 2];
					p2 = d[pos + 3];
					rpos = (p1 & ~3u) | ((unsigned)p2 << 8);
					pos += 4;
					if (!parent) parent = pos;
					if (rpos < 2 || base + rpos >= end) break;
					if (base + rpos == prev) break;
					pos = base + rpos;
					prev = pos;
				} while (pos < end && d[pos] == 0xFC);
			}
			dly = 0;
			break;
		case 0xFD:
			if (parent) {
				pos = parent;
				parent = 0;
			}
			if (nmeas < FMD_MAX_MEAS)
				meas[nmeas++] = pos;
			dly = 0;
			break;
		case 0xFE:
			ended = 1;
			dly = 0;
			break;
		default:
			break;
		}
		tick += dly;
	}
	run_flush(ev, run, nrun);
	if (tick > *max_tick) *max_tick = tick;
	return 0;
}

static void extend_loop(std::vector<fmd_ev> *ev, uint32_t loop_tick,
		uint32_t end_tick, uint32_t until)
{
	size_t i, n0;
	uint32_t span, base;
	if (!ev || !loop_tick || end_tick <= loop_tick || until <= end_tick)
		return;
	span = end_tick - loop_tick;
	if (span < 4) return;
	n0 = ev->size();
	base = end_tick;
	while (base < until) {
		for (i = 0; i < n0; i++) {
			fmd_ev e = (*ev)[i];
			if (e.tick < loop_tick || e.tick > end_tick)
				continue;
			e.tick = e.tick - loop_tick + base;
			if (e.tick <= until)
				ev->push_back(e);
		}
		if (base + span <= base) break;
		base += span;
	}
}

static int parse_song(fmd_state *s)
{
	uint8_t *d;
	size_t n;
	unsigned p, i, tlen;
	int gbl, saw_cm = 0;
	uint32_t until = 0;
	struct {
		std::vector<fmd_ev> ev;
		uint32_t loop_tick, end_tick;
	} tr[FMD_TRK];
	int ntr = 0;

	n = s->file.size();
	if (n < 80) return -1;
	d = s->file.data();
	for (i = 0; i < n; i++)
		d[i] ^= 0xA5;
	s->bpm = d[0];
	if (s->bpm < 20 || s->bpm > 250) s->bpm = 120;
	s->tickres = FMD_TICKRES;
	gbl = (int8_t)d[4];
	s->ev.clear();
	p = 6;
	for (i = 0; i < FMD_TRK; i++) {
		uint32_t lp = 0xFFFFFFFFu, en = 0;
		if (p + 4 > n) break;
		tlen = d[p] | ((unsigned)d[p + 1] << 8);
		tlen = (tlen & ~3u) | ((tlen & 3u) << 16);
		if (tlen < 0x2C || p + tlen > n) break;
		expand_trk(d, n, p, tlen, gbl, s->bpm, &tr[ntr].ev, &lp,
				&en, &saw_cm, s->title);
		tr[ntr].loop_tick = (lp == 0xFFFFFFFFu) ? 0 : lp;
		tr[ntr].end_tick = en;
		if (en > until) until = en;
		ntr++;
		p += tlen;
	}
	if (!ntr && until == 0) return -1;
	/* Tracks that hit infinite F8 stop early; keep them looping so they
	 * cover the longest finite expand (OERS_002 guitar F8×3 vs pads). */
	for (i = 0; i < (unsigned)ntr; i++) {
		extend_loop(&tr[i].ev, tr[i].loop_tick, tr[i].end_tick, until);
		s->ev.insert(s->ev.end(), tr[i].ev.begin(), tr[i].ev.end());
	}
	if (s->ev.empty() && until == 0) return -1;
	std::sort(s->ev.begin(), s->ev.end(),
			[](const fmd_ev &a, const fmd_ev &b) {
				if (a.tick != b.tick) return a.tick < b.tick;
				return a.st < b.st; /* note-off before note-on */
			});
	s->end_tick = until;
	s->loop_tick = 0;
	if (s->end_tick < s->tickres)
		s->end_tick = s->tickres;
	s->one_loop_ms = (int)((int64_t)s->end_tick * 60000 / (s->bpm * s->tickres));
	if (s->one_loop_ms < 1000) s->one_loop_ms = 1000;
	if (s->one_loop_ms > 8 * 60 * 1000) s->one_loop_ms = 8 * 60 * 1000;
	if (saw_cm) s->is_mt = 1;
	return 0;
}

static void fill_game(const char *filename, char *game, size_t gcap)
{
	if (filename && (strstr(filename, "oerstd") || strstr(filename, "OERS_") ||
			strstr(filename, "oers")))
		pc98_bounded(game, gcap, "Oerstedia");
}

static void mix_defaults(fmd_state *s, int mt)
{
	int ms;
	s->master_gain = mt ? (100.f / 127.f) : (96.f / 127.f);
	s->rev_wet = mt ? 0.22f : 0.37f;
	s->rev_fb = mt ? 0.42f : 0.55f;
	ms = mt ? 45 : 70;
	s->rev_len = s->rate * ms / 1000;
	if (s->rev_len < 256) s->rev_len = 256;
	if (s->rev_len > FMD_REV_MAX) s->rev_len = FMD_REV_MAX;
	s->rev_i = 0;
	memset(s->rev_l, 0, sizeof s->rev_l);
	memset(s->rev_r, 0, sizeof s->rev_r);
	s->cho_wet = 0.f;
	s->cho_len = s->rate * 18 / 1000;
	if (s->cho_len < 64) s->cho_len = 64;
	if (s->cho_len > FMD_REV_MAX) s->cho_len = FMD_REV_MAX;
	s->cho_i = 0;
	memset(s->cho_l, 0, sizeof s->cho_l);
	memset(s->cho_r, 0, sizeof s->cho_r);
}

static void init_channels(fmd_state *s)
{
	int c;
	if (!s->sf) return;
	tsf_reset(s->sf);
	tsf_set_output(s->sf, TSF_STEREO_INTERLEAVED, s->rate, -8.0f);
	for (c = 0; c < 16; c++) {
		int drum = (c == 9);
		tsf_channel_set_presetnumber(s->sf, c, 0, drum);
		tsf_channel_set_pitchrange(s->sf, c, 2.0f);
		tsf_channel_midi_control(s->sf, c, 7, 100);
		tsf_channel_midi_control(s->sf, c, 11, 127);
		tsf_channel_midi_control(s->sf, c, 10, 64);
		tsf_channel_midi_control(s->sf, c, 64, 0);
		tsf_channel_set_pitchwheel(s->sf, c, 8192);
	}
}

static void apply_ev(fmd_state *s, const fmd_ev *e)
{
	int drum;
	if (!s->sf) return;
	if (s->mute_rhy && e->ch == 9) return;
	if (s->mute_mel && e->ch != 9) return;
	drum = (e->ch == 9);
	switch (e->st) {
	case EV_NOTEON:
		tsf_channel_note_on(s->sf, e->ch, e->a, e->b / 127.f);
		break;
	case EV_NOTEOFF:
		tsf_channel_note_off(s->sf, e->ch, e->a);
		break;
	case EV_CC:
		tsf_channel_midi_control(s->sf, e->ch, e->a, e->b);
		if (e->a == 91)
			s->rev_wet = (e->b / 127.f) * 0.48f;
		if (e->a == 93)
			s->cho_wet = (e->b / 127.f) * 0.32f;
		break;
	case EV_PC: {
		int pc = e->a;
		if (s->is_mt && !drum)
			pc = k_mt32_gm[pc & 127];
		tsf_channel_set_presetnumber(s->sf, e->ch, pc, drum);
		break;
	}
	case EV_AT:
		break;
	case EV_PB:
		tsf_channel_set_pitchwheel(s->sf, e->ch, e->a | (e->b << 7));
		break;
	case EV_TEMPO:
		s->tempo_scale = e->a ? e->a : 64;
		break;
	case EV_MIX:
		switch (e->a) {
		case MIX_MASTER:
			s->master_gain = e->b / 127.f;
			if (s->master_gain < 0.15f) s->master_gain = 0.15f;
			break;
		case MIX_REV:
			s->rev_wet = (e->b / 127.f) * 0.48f;
			break;
		case MIX_REVTIME: {
			int ms = 20 + (e->b * 60) / 127;
			s->rev_len = s->rate * ms / 1000;
			if (s->rev_len < 256) s->rev_len = 256;
			if (s->rev_len > FMD_REV_MAX) s->rev_len = FMD_REV_MAX;
			s->rev_fb = 0.38f + (e->b / 127.f) * 0.28f;
			break;
		}
		case MIX_GSRESET:
			init_channels(s);
			mix_defaults(s, 0);
			break;
		case MIX_MTRESET:
			init_channels(s);
			mix_defaults(s, 1);
			break;
		case MIX_BEND:
			if (s->sf)
				tsf_channel_set_pitchrange(s->sf, e->ch, (float)e->b);
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}
}

static void apply_upto(fmd_state *s, uint32_t tick)
{
	while (s->ev_i < s->ev.size() && s->ev[s->ev_i].tick <= tick) {
		apply_ev(s, &s->ev[s->ev_i]);
		s->ev_i++;
	}
}

static void reset_play(fmd_state *s)
{
	s->ev_i = 0;
	s->cur_tick = 0;
	s->tick_acc = 0;
	s->tick_left = 0;
	s->tempo_scale = 64;
	s->play_samples = 0;
	s->ended = 0;
	mix_defaults(s, s->is_mt);
	init_channels(s);
	apply_upto(s, 0);
}

static int setup(fmd_state *s, const char *filename, const uint8_t *data,
		size_t len, const pc98_cfg *cfg, int want_sf)
{
	char stem[256];
	if (!data || !pc98_looks_fmdpack(data, len)) return -1;
	s->file.assign(data, data + len);
	s->is_mt = filename && (pc98_iends(filename, ".mmd") ||
			pc98_iends(filename, ".MMD"));
	s->title[0] = 0;
	if (parse_song(s) != 0) return -1;
	s->rate = cfg && cfg->rate > 0 ? cfg->rate : PC98_DEFAULT_RATE;
	s->loops_want = cfg && cfg->loop_count > 0 ? cfg->loop_count : 1;
	s->mute_rhy = cfg ? cfg->mute_rhythm : 0;
	s->mute_mel = cfg ? cfg->mute_fm : 0;
	pc98_basename(filename ? filename : "", stem, sizeof stem);
	if (!s->title[0])
		pc98_bounded(s->title, sizeof s->title, stem[0] ? stem : "FMD");
	fill_game(filename, s->game, sizeof s->game);
	pc98_bounded(s->filetype, sizeof s->filetype, s->is_mt ? "MMD" : "GMD");
	s->sf = NULL;
	s->own_sf = 0;
	s->sf2_path[0] = 0;
	if (want_sf) {
		if (fmd_find_sf2(cfg, filename, s->is_mt, s->sf2_path, sizeof s->sf2_path))
			s->sf = (tsf *)fmd_font_get(s->sf2_path);
		if (s->sf) {
			const char *bn = strrchr(s->sf2_path, '\\');
			if (!bn) bn = strrchr(s->sf2_path, '/');
			bn = bn ? bn + 1 : s->sf2_path;
			snprintf(s->engine, sizeof s->engine, "%s (%s)",
					s->is_mt ? "CM-64 / TinySoundFont" : "SC-55 / TinySoundFont",
					bn);
		} else {
			pc98_bounded(s->engine, sizeof s->engine,
					s->is_mt ? "CM-64 (no SF2)" : "SC-55 (no SF2)");
		}
	} else {
		pc98_bounded(s->engine, sizeof s->engine,
				s->is_mt ? "CM-64 / TinySoundFont" : "SC-55 / TinySoundFont");
	}
	s->play_limit = (uint32_t)(((int64_t)s->one_loop_ms * s->loops_want * s->rate) / 1000);
	if (s->play_limit < (uint32_t)s->rate) s->play_limit = (uint32_t)s->rate;
	reset_play(s);
	return 0;
}

int fmd_probe_mem(const uint8_t *data, size_t len)
{
	return pc98_looks_fmdpack(data, len);
}

int fmd_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out)
{
	fmd_state tmp;
	if (!out) return -1;
	memset(out, 0, sizeof *out);
	if (setup(&tmp, filename, data, len, cfg, 0) != 0)
		return -1;
	out->kind = PC98_KIND_FMD;
	out->songs = 1;
	out->looping = tmp.loop_tick ? 1 : 1;
	out->one_loop_ms[0] = tmp.one_loop_ms;
	pc98_bounded(out->title, sizeof out->title, tmp.title);
	pc98_bounded(out->game, sizeof out->game, tmp.game);
	pc98_bounded(out->filetype, sizeof out->filetype, tmp.filetype);
	pc98_bounded(out->engine, sizeof out->engine, tmp.engine);
	return 0;
}

void *fmd_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg)
{
	fmd_state *s = new fmd_state();
	if (setup(s, filename, data, len, cfg, 1) != 0) {
		delete s;
		return NULL;
	}
	return s;
}

void fmd_close_h(void *h)
{
	fmd_state *s = (fmd_state *)h;
	if (!s) return;
	delete s;
}

static int64_t samples_for_ticks(const fmd_state *s, int ticks)
{
	/* rate * 60 * 64 / (bpm * scale * tickres) per tick */
	int64_t num = (int64_t)s->rate * 60 * 64 * ticks;
	int64_t den = (int64_t)s->bpm * s->tempo_scale * s->tickres;
	if (den <= 0) den = 1;
	return num / den;
}

int fmd_process_h(void *h, float *buf, int count)
{
	fmd_state *s = (fmd_state *)h;
	int i;
	if (!s || !buf || count <= 0) return 0;
	memset(buf, 0, (size_t)count * 2 * sizeof(float));
	if (!s->sf) return count;
	for (i = 0; i < count; ) {
		int chunk, left;
		if (s->play_limit && s->play_samples >= s->play_limit) {
			s->ended = 1;
			break;
		}
		if (s->tick_left <= 0) {
			if (s->cur_tick >= s->end_tick) {
				if (s->loops_want > 1 && s->loop_tick < s->end_tick &&
						s->play_samples < s->play_limit) {
					size_t k;
					int c;
					s->cur_tick = s->loop_tick;
					s->ev_i = 0;
					while (s->ev_i < s->ev.size() &&
							s->ev[s->ev_i].tick < s->loop_tick)
						s->ev_i++;
					if (s->sf) {
						for (c = 0; c < 16; c++)
							tsf_channel_note_off_all(s->sf, c);
					}
					for (k = 0; k < s->ev_i; k++) {
						if (s->ev[k].st != EV_NOTEON &&
								s->ev[k].st != EV_NOTEOFF)
							apply_ev(s, &s->ev[k]);
					}
				} else {
					s->ended = 1;
					break;
				}
			}
			apply_upto(s, s->cur_tick);
			s->tick_left = (int)samples_for_ticks(s, 1);
			if (s->tick_left < 1) s->tick_left = 1;
			s->cur_tick++;
		}
		left = count - i;
		chunk = s->tick_left;
		if (chunk > left) chunk = left;
		if (s->play_limit) {
			uint32_t room = s->play_limit - s->play_samples;
			if ((uint32_t)chunk > room) chunk = (int)room;
		}
		if (chunk <= 0) break;
		tsf_render_float(s->sf, buf + i * 2, chunk, 0);
		if (s->cho_len > 0 && s->cho_wet > 0.001f) {
			int n, ci = s->cho_i, cl = s->cho_len;
			float cw = s->cho_wet;
			float *p = buf + i * 2;
			for (n = 0; n < chunk; n++) {
				float in_l = p[n * 2], in_r = p[n * 2 + 1];
				float c_l = s->cho_l[ci], c_r = s->cho_r[ci];
				s->cho_l[ci] = in_l;
				s->cho_r[ci] = in_r;
				p[n * 2] = in_l + c_l * cw;
				p[n * 2 + 1] = in_r + c_r * cw;
				if (++ci >= cl) ci = 0;
			}
			s->cho_i = ci;
		}
		if (s->rev_len > 0 && s->rev_wet > 0.001f) {
			int n, ri = s->rev_i, rl = s->rev_len;
			float wet = s->rev_wet, fb = s->rev_fb, g = s->master_gain;
			float *p = buf + i * 2;
			for (n = 0; n < chunk; n++) {
				float in_l = p[n * 2], in_r = p[n * 2 + 1];
				float w_l = s->rev_l[ri], w_r = s->rev_r[ri];
				s->rev_l[ri] = in_l * 0.55f + w_r * fb;
				s->rev_r[ri] = in_r * 0.55f + w_l * fb;
				p[n * 2] = (in_l + w_l * wet) * g;
				p[n * 2 + 1] = (in_r + w_r * wet) * g;
				if (++ri >= rl) ri = 0;
			}
			s->rev_i = ri;
		} else if (s->master_gain != 1.f) {
			int n;
			float g = s->master_gain;
			float *p = buf + i * 2;
			for (n = 0; n < chunk; n++) {
				p[n * 2] *= g;
				p[n * 2 + 1] *= g;
			}
		}
		s->play_samples += (uint32_t)chunk;
		s->tick_left -= chunk;
		i += chunk;
	}
	return count;
}

int fmd_seek_ms_h(void *h, int ms)
{
	fmd_state *s = (fmd_state *)h;
	uint32_t target, tick;
	if (!s) return -1;
	if (ms < 0) ms = 0;
	reset_play(s);
	target = (uint32_t)(((int64_t)ms * s->rate) / 1000);
	tick = (uint32_t)(((int64_t)ms * s->bpm * s->tickres) / 60000);
	apply_upto(s, tick);
	s->cur_tick = tick;
	s->play_samples = target;
	s->tick_acc = 0;
	if (s->sf) {
		/* kill hanging notes from the fast-forward */
		int c;
		for (c = 0; c < 16; c++)
			tsf_channel_sounds_off_all(s->sf, c);
		/* re-apply program/cc only */
		{
			size_t save = s->ev_i, k;
			s->ev_i = 0;
			init_channels(s);
			for (k = 0; k < save; k++) {
				if (s->ev[k].st != EV_NOTEON && s->ev[k].st != EV_NOTEOFF)
					apply_ev(s, &s->ev[k]);
			}
			s->ev_i = save;
		}
	}
	return 0;
}

int fmd_one_loop_ms_h(void *h)
{
	fmd_state *s = (fmd_state *)h;
	return s ? s->one_loop_ms : 0;
}

int fmd_rate_h(void *h)
{
	fmd_state *s = (fmd_state *)h;
	return s ? s->rate : PC98_DEFAULT_RATE;
}

void fmd_set_loops_h(void *h, int loops)
{
	fmd_state *s = (fmd_state *)h;
	if (!s) return;
	if (loops < 1) loops = 1;
	s->loops_want = loops;
	s->play_limit = (uint32_t)(((int64_t)s->one_loop_ms * s->loops_want * s->rate) / 1000);
}

void fmd_apply_mute_h(void *h, const pc98_cfg *cfg)
{
	fmd_state *s = (fmd_state *)h;
	if (!s || !cfg) return;
	s->mute_rhy = cfg->mute_rhythm;
	s->mute_mel = cfg->mute_fm;
}

const char *fmd_title_h(void *h)
{
	fmd_state *s = (fmd_state *)h;
	return s ? s->title : "";
}

const char *fmd_game_h(void *h)
{
	fmd_state *s = (fmd_state *)h;
	return s ? s->game : "";
}

const char *fmd_engine_h(void *h)
{
	fmd_state *s = (fmd_state *)h;
	return s ? s->engine : "";
}

const char *fmd_type_h(void *h)
{
	fmd_state *s = (fmd_state *)h;
	return s ? s->filetype : "";
}

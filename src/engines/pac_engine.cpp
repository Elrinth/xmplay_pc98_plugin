/*
 * ArtDink MUSIC.PAC — LE32 size table + packed .NTL songs.
 * Eikan wa Kimi ni 3 (Hoot ARTDI_98.COM + HSB3.EXE ArtDink driver).
 * F_* style → ymfm OPNA (ntl_engine); G_* style → SC-55 SF2 (gntl_engine).
 */
#include "pac_engine.h"
#include "ntl_engine.h"
#include "gntl_engine.h"
#include "pc98_util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <vector>

struct pac_song {
	size_t off;
	size_t len;
	int is_midi; /* 1 GNTL, 0 NTL, -1 unknown/bad */
	int one_loop_ms;
};

struct pac_state {
	std::vector<uint8_t> file;
	std::vector<pac_song> songs;
	pc98_cfg cfg;
	int song;
	int loops;
	void *child;
	int child_midi;
	char title[256], game[256], engine[64], chip[64], filetype[32];

	pac_state()
		: song(0), loops(1), child(NULL), child_midi(0)
	{
		memset(&cfg, 0, sizeof cfg);
		title[0] = game[0] = engine[0] = chip[0] = filetype[0] = 0;
	}
};

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int peek_looks_pac(const uint8_t *d, size_t n, const char *filename)
{
	unsigned i, good = 0;
	if (!filename || !pc98_iends(filename, ".pac")) return 0;
	if (!d || n < 32) return 0;
	for (i = 0; i + 4 <= n && i < 64 * 4; i += 4) {
		uint32_t s = rd32(d + i);
		if (s < 16 || s > 0x10000) break;
		good++;
	}
	return good >= 8 ? 1 : 0;
}

static int parse_table(const uint8_t *d, size_t n, std::vector<pac_song> *out)
{
	unsigned count = 0, i, ok;
	uint32_t sum, off;
	if (!d || n < 64 || !out) return -1;
	for (unsigned c = (unsigned)(n / 4); c >= 2; --c) {
		if (c > PC98_MAX_SONGS) continue;
		sum = 0;
		int bad = 0;
		for (i = 0; i < c; ++i) {
			uint32_t s = rd32(d + i * 4);
			if (s < 16 || s > 0x20000) { bad = 1; break; }
			sum += s;
			if ((uint64_t)c * 4u + sum > n) { bad = 1; break; }
		}
		if (bad) continue;
		if ((uint32_t)c * 4u + sum == (uint32_t)n) {
			count = c;
			break;
		}
	}
	if (count < 2) return -1;
	out->clear();
	out->reserve(count);
	off = count * 4u;
	ok = 0;
	for (i = 0; i < count; ++i) {
		pac_song s;
		s.off = off;
		s.len = rd32(d + i * 4);
		s.one_loop_ms = 0;
		s.is_midi = -1;
		if (s.off + s.len > n) return -1;
		if (pc98_looks_ntl(d + s.off, s.len)) {
			s.is_midi = 0;
			ok++;
		} else if (pc98_looks_ntl_midi(d + s.off, s.len)) {
			s.is_midi = 1;
			ok++;
		}
		out->push_back(s);
		off += (uint32_t)s.len;
	}
	if (ok * 2 < count || ok < 2) return -1;
	return (int)count;
}

int pac_probe_mem(const uint8_t *data, size_t len)
{
	std::vector<pac_song> tmp;
	if (len >= 256 && parse_table(data, len, &tmp) > 0)
		return 1;
	return 0;
}

int pac_probe_named(const char *filename, const uint8_t *data, size_t len)
{
	if (pac_probe_mem(data, len))
		return 1;
	return peek_looks_pac(data, len, filename);
}

static void close_child(pac_state *s)
{
	if (!s->child) return;
	if (s->child_midi) gntl_close_h(s->child);
	else ntl_close_h(s->child);
	s->child = NULL;
}

static int open_child(pac_state *s, int idx)
{
	const uint8_t *chunk;
	size_t clen;
	close_child(s);
	if (idx < 0 || idx >= (int)s->songs.size()) return -1;
	chunk = s->file.data() + s->songs[idx].off;
	clen = s->songs[idx].len;
	s->child = NULL;
	s->child_midi = 0;
	if (s->songs[idx].is_midi == 0 ||
			(s->songs[idx].is_midi < 0 && pc98_looks_ntl(chunk, clen))) {
		s->child = ntl_open_mem(NULL, chunk, clen, &s->cfg);
		s->child_midi = 0;
		if (s->child) s->songs[idx].is_midi = 0;
	} else if (s->songs[idx].is_midi == 1 ||
			(s->songs[idx].is_midi < 0 && pc98_looks_ntl_midi(chunk, clen))) {
		s->child = gntl_open_mem(NULL, chunk, clen, &s->cfg);
		s->child_midi = 1;
		if (s->child) s->songs[idx].is_midi = 1;
	}
	if (!s->child) return -1;
	if (s->child_midi) {
		gntl_set_loops_h(s->child, s->loops);
		pc98_bounded(s->engine, sizeof s->engine, gntl_engine_h(s->child));
		pc98_bounded(s->chip, sizeof s->chip, "SC-55 (SF2)");
	} else {
		ntl_set_loops_h(s->child, s->loops);
		pc98_bounded(s->engine, sizeof s->engine, "ymfm ArtDink NTL / PAC");
		pc98_bounded(s->chip, sizeof s->chip, "YM2608 OPNA");
	}
	s->song = idx;
	return 0;
}

static int measure_song(pac_state *s, int idx)
{
	void *h;
	const uint8_t *chunk;
	size_t clen;
	int ms = 0;
	if (idx < 0 || idx >= (int)s->songs.size()) return 0;
	if (s->songs[idx].one_loop_ms > 0)
		return s->songs[idx].one_loop_ms;
	chunk = s->file.data() + s->songs[idx].off;
	clen = s->songs[idx].len;
	if (s->songs[idx].is_midi == 0) {
		h = ntl_open_mem(NULL, chunk, clen, &s->cfg);
		if (h) { ms = ntl_one_loop_ms_h(h); ntl_close_h(h); }
	} else if (s->songs[idx].is_midi == 1) {
		h = gntl_open_mem(NULL, chunk, clen, &s->cfg);
		if (h) { ms = gntl_one_loop_ms_h(h); gntl_close_h(h); }
	}
	if (ms < 100) ms = 1000;
	s->songs[idx].one_loop_ms = ms;
	return ms;
}

int pac_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out)
{
	pac_state tmp;
	int i, n;
	char stem[256];
	if (!out || !data) return -1;
	memset(out, 0, sizeof *out);
	n = parse_table(data, len, &tmp.songs);
	if (n < 2) return -1;
	tmp.file.assign(data, data + len);
	if (cfg) tmp.cfg = *cfg;
	out->kind = PC98_KIND_PAC;
	out->songs = n;
	out->looping = 1;
	for (i = 0; i < n && i < PC98_MAX_SONGS; ++i)
		out->one_loop_ms[i] = measure_song(&tmp, i);
	pc98_basename(filename ? filename : "", stem, sizeof stem);
	pc98_bounded(out->title, sizeof out->title, stem[0] ? stem : "MUSIC.PAC");
	pc98_bounded(out->game, sizeof out->game, "Eikan wa Kimi ni 3");
	pc98_bounded(out->filetype, sizeof out->filetype, "PAC");
	pc98_bounded(out->engine, sizeof out->engine, "ArtDink NTL/GNTL (MUSIC.PAC)");
	return 0;
}

void *pac_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg)
{
	pac_state *s;
	int n, i;
	char stem[256];
	if (!data) return NULL;
	s = new pac_state();
	n = parse_table(data, len, &s->songs);
	if (n < 2) { delete s; return NULL; }
	s->file.assign(data, data + len);
	if (cfg) s->cfg = *cfg;
	s->loops = (cfg && cfg->loop_count > 0) ? cfg->loop_count : 1;
	pc98_basename(filename ? filename : "", stem, sizeof stem);
	pc98_bounded(s->title, sizeof s->title, stem[0] ? stem : "MUSIC.PAC");
	pc98_bounded(s->game, sizeof s->game, "Eikan wa Kimi ni 3");
	pc98_bounded(s->filetype, sizeof s->filetype, "PAC");
	/* Lengths measured lazily in pac_one_loop_ms_h / analyze. */
	if (open_child(s, 0) != 0) {
		int opened = 0;
		for (i = 0; i < n; ++i) {
			if (s->songs[i].is_midi >= 0 && open_child(s, i) == 0) {
				opened = 1;
				break;
			}
		}
		if (!opened) { delete s; return NULL; }
	}
	return s;
}

void pac_close_h(void *h)
{
	pac_state *s = (pac_state *)h;
	if (!s) return;
	close_child(s);
	delete s;
}

int pac_process_h(void *h, float *buf, int count)
{
	pac_state *s = (pac_state *)h;
	if (!s || !s->child || !buf || count <= 0) return 0;
	if (s->child_midi) return gntl_process_h(s->child, buf, count);
	return ntl_process_h(s->child, buf, count);
}

int pac_seek_ms_h(void *h, int ms)
{
	pac_state *s = (pac_state *)h;
	if (!s || !s->child) return -1;
	if (s->child_midi) return gntl_seek_ms_h(s->child, ms);
	return ntl_seek_ms_h(s->child, ms);
}

int pac_set_song_h(void *h, int song)
{
	pac_state *s = (pac_state *)h;
	if (!s) return -1;
	if (song < 0 || song >= (int)s->songs.size()) return -1;
	return open_child(s, song);
}

int pac_song_h(void *h)
{
	pac_state *s = (pac_state *)h;
	return s ? s->song : 0;
}

int pac_songs_h(void *h)
{
	pac_state *s = (pac_state *)h;
	return s ? (int)s->songs.size() : 0;
}

int pac_one_loop_ms_h(void *h, int song)
{
	pac_state *s = (pac_state *)h;
	if (!s || song < 0 || song >= (int)s->songs.size()) return 0;
	if (s->songs[song].one_loop_ms <= 0)
		measure_song(s, song);
	return s->songs[song].one_loop_ms;
}

int pac_rate_h(void *h)
{
	pac_state *s = (pac_state *)h;
	if (!s || !s->child) return PC98_DEFAULT_RATE;
	if (s->child_midi) return gntl_rate_h(s->child);
	return ntl_rate_h(s->child);
}

void pac_set_loops_h(void *h, int loops)
{
	pac_state *s = (pac_state *)h;
	if (!s) return;
	if (loops < 1) loops = 1;
	if (loops > 3) loops = 3;
	s->loops = loops;
	if (!s->child) return;
	if (s->child_midi) gntl_set_loops_h(s->child, loops);
	else ntl_set_loops_h(s->child, loops);
}

void pac_apply_mute_h(void *h, const pc98_cfg *cfg)
{
	pac_state *s = (pac_state *)h;
	if (!s || !cfg) return;
	s->cfg = *cfg;
	if (!s->child) return;
	if (s->child_midi) gntl_apply_mute_h(s->child, cfg);
	else ntl_apply_mute_h(s->child, cfg);
}

const char *pac_title_h(void *h)
{
	pac_state *s = (pac_state *)h;
	return s ? s->title : "";
}
const char *pac_game_h(void *h)
{
	pac_state *s = (pac_state *)h;
	return s ? s->game : "";
}
const char *pac_engine_h(void *h)
{
	pac_state *s = (pac_state *)h;
	return s ? s->engine : "";
}
const char *pac_chip_h(void *h)
{
	pac_state *s = (pac_state *)h;
	return s ? s->chip : "";
}
const char *pac_type_h(void *h)
{
	pac_state *s = (pac_state *)h;
	return s ? s->filetype : "";
}

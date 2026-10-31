/*
 * U.Kakenasi MFD.COM 1.8 "Multi Music Driver" (Dec 1994) MIDI songs (.MFD).
 * Sequencer recovered from the PIYO-packed MFD.COM (Night Slave, Melody 1996).
 *
 * File: "MF" 01 00, le32 size. At +8: le32 size-8, 00, 28h, tempo, tracks.
 * Track table at +16: le16 order offset, MIDI channel (FFh = off), unused.
 * Offsets are relative to +8. An order list is le16 words: >= 10h phrase,
 * 1 = loop start, 2 cnt = loop end (cnt 0 = forever), 0 = end.
 * Phrase: note < 80h + step (+ gate when step is 0), commands 80h–FFh.
 * Each track keeps six note slots with their own gate countdown.
 *
 * Timer: PIT counter 2*768000/tempo at 2.4576 MHz, sequencer on every second
 * IRQ, so one tick is 1.25/tempo s (48 per quarter note).
 * MPU-401 → Roland GS (SC-55). Rendered with TinySoundFont.
 */
#include "mfdmidi_engine.h"
#include "fmd_engine.h"
#include "pc98_util.h"

#include <stdio.h>
#include <string.h>
#include <vector>

#include "tsf.h"

#define MM_BASE     8
#define MM_MAXTRK   32
#define MM_SLOTS    6
#define MM_DEPTH    16
#define MM_FIFO     64
#define MM_CAP_US   (20LL * 60 * 1000000)
#define MM_TAIL_MS  1000

struct MmTrack {
	int ch;            /* 0xFF = off */
	int ended;
	int pc, op;        /* phrase / order file offsets */
	uint8_t step, q, vel;
	uint8_t note[MM_SLOTS], gate[MM_SLOTS];
	int expr, expr_sent;
	int depth;
	uint8_t cnt[MM_DEPTH];
	int ret[MM_DEPTH];
	uint8_t sx_dev, sx_model, sx_a1, sx_a2;
	int64_t loop_us, end_us;
};

struct MmState {
	std::vector<uint8_t> file;
	int ntrk;
	uint8_t tempo0, tempo;
	MmTrack trk[MM_MAXTRK];
	int64_t now_us;
	char title[256], engine[96], sf2_path[PC98_PATH_MAX], sf2_name[128];
	int rate, loops_want, looping, one_loop_ms, ended;
	uint32_t play_limit, play_samples;
	double tick_acc;
	int tick_left;
	tsf *sf;
	int own_sf;
	int sink;          /* 0 none, 1 TSF, 2 seek (no notes) */
	uint32_t mute_midi;
	float master;
	uint8_t prog[16], drum[16];
	uint8_t held[16][128];
	float fifo[MM_FIFO * 2];
	int fifo_pos, fifo_len;
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

int mfdmidi_probe_mem(const uint8_t *d, size_t n)
{
	unsigned ntrk, i, size;
	if (!d || n < 24) return 0;
	if (d[0] != 'M' || d[1] != 'F' || d[2] != 1 || d[3] != 0) return 0;
	size = (unsigned)d[4] | ((unsigned)d[5] << 8) | ((unsigned)d[6] << 16) | ((unsigned)d[7] << 24);
	if (size < 24 || size > PC98_MAX_FILE) return 0;
	/* CheckFile only peeks 256 bytes; a full load must match the size field. */
	if (n > 256 && n != size) return 0;
	if (d[MM_BASE + 5] != 0x28) return 0;
	ntrk = d[MM_BASE + 7];
	if (ntrk < 1 || ntrk > MM_MAXTRK) return 0;
	for (i = 0; i < ntrk; i++) {
		size_t e = MM_BASE + 8 + i * 4;
		unsigned off;
		if (e + 4 > n) break;
		off = rd16(d + e);
		if (MM_BASE + off + 2 > size) return 0;
	}
	return 1;
}

/* ---- MIDI out (what MFD.COM writes to the MPU) ---- */

static void init_chs(MmState *s)
{
	int c;
	s->master = 1.0f;
	for (c = 0; c < 16; c++) {
		s->prog[c] = 0;
		s->drum[c] = (c == 9);
	}
	if (!s->sf) return;
	for (c = 0; c < 16; c++) {
		tsf_channel_sounds_off_all(s->sf, c);
		tsf_channel_midi_control(s->sf, c, 121, 0);
		tsf_channel_set_presetnumber(s->sf, c, 0, c == 9);
		tsf_channel_set_pitchrange(s->sf, c, 2.0f);
		tsf_channel_midi_control(s->sf, c, 7, 100);
		tsf_channel_midi_control(s->sf, c, 11, 127);
		tsf_channel_midi_control(s->sf, c, 10, 64);
		tsf_channel_set_pitchwheel(s->sf, c, 8192);
	}
}

static void midi3(MmState *s, int st, int a, int b)
{
	int ch = st & 15, ty = st & 0xF0;
	if (!s->sink || st < 0x80 || st >= 0xF0) return;
	a &= 0x7F;
	b &= 0x7F;
	if (ty == 0x90 && b == 0) ty = 0x80;
	if (ty == 0x80 || ty == 0x90) {
		if (s->sink == 2) {
			s->held[ch][a] = ty == 0x90 ? (uint8_t)b : 0;
			return;
		}
		if (!s->sf) return;
		if (ty == 0x80) tsf_channel_note_off(s->sf, ch, a);
		else if (!(s->mute_midi & (1u << ch))) tsf_channel_note_on(s->sf, ch, a, b / 127.f);
		return;
	}
	if (ty == 0xC0) s->prog[ch] = (uint8_t)a;
	if (!s->sf) return;
	switch (ty) {
	case 0xB0: tsf_channel_midi_control(s->sf, ch, a, b); break;
	case 0xC0: tsf_channel_set_presetnumber(s->sf, ch, a, s->drum[ch]); break;
	case 0xE0: tsf_channel_set_pitchwheel(s->sf, ch, a | (b << 7)); break;
	default: break; /* poly / channel pressure */
	}
}

/* GS part number → MIDI channel: 10h is part 10, 11h–19h parts 1–9. */
static int gs_part_ch(int p)
{
	p &= 15;
	if (p == 0) return 9;
	return p <= 9 ? p - 1 : p;
}

static void sysex(MmState *s, const uint8_t *m, int n)
{
	if (!s->sink || n < 4) return;
	if (n == 8 && m[1] == 0x7F && m[3] == 0x04 && m[4] == 0x01) {
		s->master = m[6] / 127.f;
		return;
	}
	if (n < 11 || m[1] != 0x41 || m[3] != 0x42 || m[4] != 0x12) return;
	if (m[5] == 0x40 && m[6] == 0x00 && m[7] == 0x7F) {
		init_chs(s);
		return;
	}
	if (m[5] == 0x40 && m[6] == 0x00 && m[7] == 0x04) {
		s->master = m[8] / 127.f;
		return;
	}
	if (m[5] == 0x40 && (m[6] & 0xF0) == 0x10 && m[7] == 0x15) {
		int ch = gs_part_ch(m[6]);
		s->drum[ch] = m[8] ? 1 : 0;
		if (s->sf) tsf_channel_set_presetnumber(s->sf, ch, s->prog[ch], s->drum[ch]);
	}
}

/* ---- sequencer (MFD.COM timer IRQ) ---- */

static uint8_t fetch(MmState *s, MmTrack *t)
{
	if (t->pc < 0 || t->pc >= (int)s->file.size()) {
		t->pc = (int)s->file.size();
		return 0xFF;
	}
	return s->file[t->pc++];
}

static int st_of(const MmTrack *t, int k)
{
	return (((t->ch | 0x80) + 0x10 * k) & 0xFF);
}

/* Order list walk (MFD.COM 1602h). 0 when the list is done. */
static int next_phrase(MmState *s, MmTrack *t)
{
	const uint8_t *d = s->file.data();
	int n = (int)s->file.size();
	int guard = 0;
	while (guard++ < 4096) {
		int v;
		if (t->op < 0 || t->op + 2 > n) return 0;
		v = rd16(d + t->op);
		t->op += 2;
		if (v >= 0x10) {
			t->pc = MM_BASE + v;
			return 1;
		}
		if (v == 1) {
			if (t->depth < MM_DEPTH) {
				t->cnt[t->depth] = 0;
				t->ret[t->depth] = t->op;
				t->depth++;
			}
			continue;
		}
		if (v == 2) {
			int c, k;
			if (t->op + 2 > n) return 0;
			c = d[t->op];
			t->op += 2;
			k = t->depth - 1;
			if (k < 0) {
				t->op -= 4;
				return 0;
			}
			t->cnt[k]++;
			if (c && c == t->cnt[k]) {
				t->depth = k;
				continue;
			}
			if (c == 0 && t->loop_us < 0)
				t->loop_us = s->now_us;
			t->op = t->ret[k];
			continue;
		}
		t->op -= 2;
		return 0;
	}
	return 0;
}

static void note_on(MmState *s, MmTrack *t, int note, uint8_t gate)
{
	int i, freeslot = -1;
	for (i = 0; i < MM_SLOTS; i++) {
		if (t->note[i] == note) {
			t->gate[i] = gate;
			return;
		}
		if (t->note[i] == 0xFF) freeslot = i;
	}
	if (freeslot < 0) return;
	t->note[freeslot] = (uint8_t)note;
	t->gate[freeslot] = gate;
	midi3(s, st_of(t, 1), note, t->vel);
}

static void notes_off(MmState *s, MmTrack *t)
{
	int i;
	for (i = 0; i < MM_SLOTS; i++) {
		if (t->note[i] & 0x80) continue;
		midi3(s, st_of(t, 0), t->note[i], 0);
		t->note[i] = 0xFF;
	}
}

static void track_end(MmState *s, MmTrack *t)
{
	/* MFD.COM drops the slots without a note-off; release them instead. */
	notes_off(s, t);
	t->ended = 1;
	t->end_us = s->now_us;
}

/* Wait part of a grouped opcode: 0 = keep reading, 1..n fixed, last = byte. */
static int grp_wait(MmState *s, MmTrack *t, int k, int last)
{
	if (k == 0) return 0;
	t->step = (uint8_t)(k == last ? fetch(s, t) : k);
	return 1;
}

static void parse(MmState *s, MmTrack *t)
{
	int guard = 0;
	while (guard++ < 65536) {
		int b = fetch(s, t);
		if (b < 0x80) {
			int st = fetch(s, t);
			if (st) {
				t->step = (uint8_t)st;
				note_on(s, t, b, (uint8_t)(st - t->q));
				return;
			}
			if (t->ch & 0x80) {
				note_on(s, t, b, 1);
				continue;
			}
			st = fetch(s, t);
			if (st) note_on(s, t, b, (uint8_t)st);
			continue;
		}
		if (b < 0xA0) {
			static const uint8_t cc[4] = { 1, 7, 10, 11 };
			int w = grp_wait(s, t, b & 7, 7);
			int v = fetch(s, t);
			int c = cc[(b - 0x80) >> 3];
			midi3(s, st_of(t, 3), c, v);
			if (c == 11) t->expr = v;
			if (w) return;
			continue;
		}
		if (b < 0xA8) {
			int w = grp_wait(s, t, b & 7, 7);
			int v = fetch(s, t);
			midi3(s, st_of(t, 4), v, 0);
			if (w) return;
			continue;
		}
		if (b < 0xB0) {
			int w = grp_wait(s, t, b & 7, 7);
			int c = fetch(s, t);
			int v = fetch(s, t);
			midi3(s, st_of(t, 3), c, v);
			if (w) return;
			continue;
		}
		if (b < 0xD0) {
			int w = grp_wait(s, t, b & 15, 15);
			int v;
			if (b < 0xC0) {
				v = (int8_t)fetch(s, t);
			} else {
				v = fetch(s, t);
				v |= fetch(s, t) << 8;
			}
			v = (v + 0x2000) & 0xFFFF;
			midi3(s, st_of(t, 6), v & 0x7F, (v >> 7) & 0x7F);
			if (w) return;
			continue;
		}
		if (b < 0xDF) {
			t->q = (uint8_t)(b - 0xD1);
			continue;
		}
		switch (b) {
		case 0xDF:
			t->q = fetch(s, t);
			continue;
		case 0xE0: case 0xE1: {
			int w = b == 0xE1;
			if (w) t->step = fetch(s, t);
			s->tempo = (uint8_t)(fetch(s, t) - 0x40 + s->tempo0);
			fetch(s, t);
			if (w) return;
			continue;
		}
		case 0xE2: case 0xE3: {
			int w = b == 0xE3;
			if (w) t->step = fetch(s, t);
			t->ch = (t->ch & 0x80) | fetch(s, t);
			if (w) return;
			continue;
		}
		case 0xE4: case 0xE5: {
			int w = b == 0xE5;
			if (w) t->step = fetch(s, t);
			midi3(s, st_of(t, 5), fetch(s, t), 0);
			if (w) return;
			continue;
		}
		case 0xE6: case 0xE7: {
			int w = b == 0xE7, a;
			if (w) t->step = fetch(s, t);
			a = fetch(s, t);
			midi3(s, st_of(t, 2), a, fetch(s, t));
			if (w) return;
			continue;
		}
		case 0xEC: case 0xEF: midi3(s, st_of(t, 3), 10, 0x40); continue;
		case 0xED: midi3(s, st_of(t, 3), 10, 0x00); continue;
		case 0xEE: midi3(s, st_of(t, 3), 10, 0x7F); continue;
		case 0xF0: {
			int r = fetch(s, t);
			if (!r) continue;
			t->step = (uint8_t)r;
			notes_off(s, t);
			return;
		}
		case 0xF1:
			t->vel = fetch(s, t);
			continue;
		case 0xFC: {
			int sub = fetch(s, t);
			int w = fetch(s, t);
			int a = fetch(s, t);
			int c = fetch(s, t);
			if (sub == 0xDF) {
				t->sx_dev = (uint8_t)a;
				t->sx_model = (uint8_t)c;
			} else if (sub == 0xDD) {
				t->sx_a1 = (uint8_t)a;
				t->sx_a2 = (uint8_t)c;
			} else if (sub == 0xDE) {
				uint8_t m[11];
				m[0] = 0xF0; m[1] = 0x41; m[2] = t->sx_dev; m[3] = t->sx_model;
				m[4] = 0x12; m[5] = t->sx_a1; m[6] = t->sx_a2;
				m[7] = (uint8_t)a; m[8] = (uint8_t)c;
				m[9] = (uint8_t)((0 - (m[5] + m[6] + m[7] + m[8])) & 0x7F);
				m[10] = 0xF7;
				sysex(s, m, 11);
			}
			if (w) {
				t->step = (uint8_t)w;
				return;
			}
			continue;
		}
		case 0xFE: case 0xFF:
			if (next_phrase(s, t)) continue;
			track_end(s, t);
			return;
		default: /* E8–EB, F2–FB, FD */
			continue;
		}
	}
	track_end(s, t);
}

static void seq_tick(MmState *s)
{
	int i, k, live = 0;
	for (i = 0; i < s->ntrk; i++) {
		MmTrack *t = &s->trk[i];
		int v;
		if (t->ch == 0xFF || t->ended) continue;
		live = 1;
		for (k = 0; k < MM_SLOTS; k++) {
			if (t->note[k] == 0xFF) continue;
			if (--t->gate[k] == 0) {
				midi3(s, st_of(t, 0), t->note[k], 0);
				t->note[k] = 0xFF;
			}
		}
		v = t->expr < 0 ? 0 : t->expr;
		if (v != t->expr_sent) {
			t->expr_sent = v;
			midi3(s, st_of(t, 3), 11, v);
		}
		if (--t->step == 0)
			parse(s, t);
	}
	if (!live) s->ended = 1;
}

static double tick_us(const MmState *s)
{
	unsigned t = s->tempo < 12 ? 12 : s->tempo;
	unsigned cnt = ((768000u / t) << 1) & 0xFFFF;
	if (!cnt) cnt = 0x10000;
	return 2.0 * cnt * 1000000.0 / 2457600.0;
}

static void reset_seq(MmState *s)
{
	const uint8_t *d = s->file.data();
	size_t n = s->file.size();
	int i;
	static const uint8_t mvol[8] = { 0xF0, 0x7F, 0x7F, 0x04, 0x01, 0x00, 0x7F, 0xF7 };
	s->tempo0 = s->tempo = d[MM_BASE + 6];
	s->ntrk = d[MM_BASE + 7];
	if (s->ntrk > MM_MAXTRK) s->ntrk = MM_MAXTRK;
	s->now_us = 0;
	s->ended = 0;
	for (i = 0; i < s->ntrk; i++) {
		MmTrack *t = &s->trk[i];
		size_t e = MM_BASE + 8 + (size_t)i * 4;
		memset(t, 0, sizeof *t);
		t->ch = 0xFF;
		t->loop_us = t->end_us = -1;
		memset(t->note, 0xFF, sizeof t->note);
		if (e + 4 > n) continue;
		t->op = MM_BASE + rd16(d + e);
		t->step = 2;
		t->vel = 0x7F;
		t->expr = 0x7F;
		t->expr_sent = -1;
		if (d[e + 2] != 0xFF && next_phrase(s, t))
			t->ch = d[e + 2];
	}
	/* MFD.COM play: all notes off / reset controllers, GM master volume. */
	for (i = 0; i < 16; i++)
		midi3(s, 0xB0 | i, 123, 0);
	for (i = 0; i < 16; i++)
		midi3(s, 0xB0 | i, 121, 0);
	sysex(s, mvol, 8);
}

static void measure(MmState *s)
{
	int i, any_loop = 0;
	int64_t end = 0;
	int sink = s->sink;
	s->sink = 0;
	reset_seq(s);
	while (!s->ended && s->now_us < MM_CAP_US) {
		int done = 1;
		seq_tick(s);
		s->now_us += (int64_t)tick_us(s);
		for (i = 0; i < s->ntrk; i++) {
			const MmTrack *t = &s->trk[i];
			if (t->ch == 0xFF || t->ended) continue;
			if (t->loop_us < 0) done = 0;
		}
		if (done) break;
	}
	for (i = 0; i < s->ntrk; i++) {
		const MmTrack *t = &s->trk[i];
		if (t->ch == 0xFF) continue;
		if (t->loop_us >= 0) {
			any_loop = 1;
			if (t->loop_us > end) end = t->loop_us;
		} else if (t->end_us > end) {
			end = t->end_us;
		}
	}
	if (end <= 0) end = s->now_us;
	s->looping = any_loop;
	s->one_loop_ms = (int)(end / 1000) + (any_loop ? 0 : MM_TAIL_MS);
	if (s->one_loop_ms < 500) s->one_loop_ms = 500;
	s->sink = sink;
}

static void set_limit(MmState *s)
{
	int loops = s->looping ? s->loops_want : 1;
	s->play_limit = (uint32_t)(((int64_t)s->one_loop_ms * loops * s->rate) / 1000);
	if (s->play_limit < (uint32_t)s->rate) s->play_limit = (uint32_t)s->rate;
}

static void open_font(MmState *s)
{
	if (s->sf && s->own_sf) fmd_font_release(s->sf);
	s->sf = NULL;
	s->own_sf = 0;
	if (!s->sf2_path[0]) return;
	s->sf = (tsf *)fmd_font_open(s->sf2_path);
	s->own_sf = s->sf ? 1 : 0;
	if (s->sf) tsf_set_output(s->sf, TSF_STEREO_INTERLEAVED, s->rate, -8.0f);
}

static void restart(MmState *s)
{
	open_font(s);
	init_chs(s);
	memset(s->held, 0, sizeof s->held);
	s->sink = 1;
	reset_seq(s);
	s->play_samples = 0;
	s->tick_acc = 0;
	s->tick_left = 0;
	s->fifo_pos = s->fifo_len = 0;
}

static int setup(MmState *s, const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, int need_sf)
{
	char stem[256];
	if (!mfdmidi_probe_mem(data, len) || len < MM_BASE + 8) return -1;
	s->file.assign(data, data + len);
	s->rate = cfg && cfg->rate > 0 ? cfg->rate : PC98_DEFAULT_RATE;
	s->loops_want = cfg && cfg->loop_count > 0 ? cfg->loop_count : 1;
	s->mute_midi = cfg ? cfg->mute_midi_mask : 0;
	s->sf = NULL;
	s->own_sf = 0;
	s->sink = 0;
	s->sf2_path[0] = s->sf2_name[0] = 0;
	pc98_basename(filename ? filename : "", stem, sizeof stem);
	pc98_bounded(s->title, sizeof s->title, stem[0] ? stem : "MFD");
	pc98_bounded(s->engine, sizeof s->engine, "MFD.COM / TinySoundFont GS");
	measure(s);
	set_limit(s);
	if (need_sf) {
		if (fmd_find_sf2(cfg, filename, 0, s->sf2_path, sizeof s->sf2_path)) {
			const char *bn = strrchr(s->sf2_path, '\\');
			if (!bn) bn = strrchr(s->sf2_path, '/');
			bn = bn ? bn + 1 : s->sf2_path;
			pc98_bounded(s->sf2_name, sizeof s->sf2_name, bn);
		}
		restart(s);
		if (s->sf)
			snprintf(s->engine, sizeof s->engine, "MFD.COM / TinySoundFont (%s)", s->sf2_name);
		else
			pc98_bounded(s->engine, sizeof s->engine, "MFD.COM (no SF2)");
	}
	return 0;
}

int mfdmidi_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out)
{
	MmState *s;
	if (!out) return -1;
	memset(out, 0, sizeof *out);
	s = new MmState();
	if (setup(s, filename, data, len, cfg, 0) != 0) {
		delete s;
		return -1;
	}
	out->kind = PC98_KIND_MFDMIDI;
	out->songs = 1;
	out->looping = s->looping;
	out->one_loop_ms[0] = s->one_loop_ms;
	pc98_bounded(out->title, sizeof out->title, s->title);
	pc98_bounded(out->filetype, sizeof out->filetype, "MFD");
	pc98_bounded(out->engine, sizeof out->engine, s->engine);
	delete s;
	return 0;
}

void *mfdmidi_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg)
{
	MmState *s = new MmState();
	if (setup(s, filename, data, len, cfg, 1) != 0) {
		delete s;
		return NULL;
	}
	return s;
}

void mfdmidi_close_h(void *h)
{
	MmState *s = (MmState *)h;
	if (!s) return;
	if (s->sf && s->own_sf) fmd_font_release(s->sf);
	delete s;
}

/* One sequencer tick, then that tick's worth of samples. */
static void step_tick(MmState *s)
{
	double sp;
	seq_tick(s);
	sp = s->rate * tick_us(s) / 1000000.0 + s->tick_acc;
	s->tick_left = (int)sp;
	s->tick_acc = sp - s->tick_left;
	s->now_us += (int64_t)tick_us(s);
}

int mfdmidi_process_h(void *h, float *buf, int count)
{
	MmState *s = (MmState *)h;
	int done = 0;
	if (!s || !buf || count <= 0) return 0;
	memset(buf, 0, (size_t)count * 2 * sizeof(float));
	if (!s->sf) return 0;
	while (done < count) {
		int take, gen, i;
		if (s->play_samples >= s->play_limit) break;
		if (s->fifo_pos >= s->fifo_len) {
			while (s->tick_left <= 0)
				step_tick(s);
			gen = s->tick_left > MM_FIFO ? MM_FIFO : s->tick_left;
			memset(s->fifo, 0, sizeof s->fifo);
			tsf_render_float(s->sf, s->fifo, gen, 0);
			s->fifo_pos = 0;
			s->fifo_len = gen;
			s->tick_left -= gen;
		}
		take = s->fifo_len - s->fifo_pos;
		if (take > count - done) take = count - done;
		if ((uint32_t)take > s->play_limit - s->play_samples)
			take = (int)(s->play_limit - s->play_samples);
		for (i = 0; i < take; i++) {
			buf[(done + i) * 2] = s->fifo[(s->fifo_pos + i) * 2] * s->master;
			buf[(done + i) * 2 + 1] = s->fifo[(s->fifo_pos + i) * 2 + 1] * s->master;
		}
		s->fifo_pos += take;
		s->play_samples += (uint32_t)take;
		done += take;
	}
	return done;
}

int mfdmidi_seek_ms_h(void *h, int ms)
{
	MmState *s = (MmState *)h;
	uint32_t target;
	int ch, note;
	if (!s) return -1;
	if (ms < 0) ms = 0;
	restart(s);
	target = (uint32_t)(((int64_t)ms * s->rate) / 1000);
	if (target > s->play_limit) target = s->play_limit;
	/* Controllers up to the seek point, then the notes still held there. */
	s->sink = 2;
	while (s->play_samples + (uint32_t)s->tick_left < target) {
		s->play_samples += (uint32_t)s->tick_left;
		s->tick_left = 0;
		step_tick(s);
	}
	s->sink = 1;
	for (ch = 0; ch < 16; ch++)
		for (note = 0; note < 128; note++)
			if (s->held[ch][note])
				midi3(s, 0x90 | ch, note, s->held[ch][note]);
	s->tick_left -= (int)(target - s->play_samples);
	s->play_samples = target;
	return (int)(((int64_t)target * 1000) / s->rate);
}

int mfdmidi_one_loop_ms_h(void *h) { MmState *s = (MmState *)h; return s ? s->one_loop_ms : 0; }
int mfdmidi_rate_h(void *h) { MmState *s = (MmState *)h; return s ? s->rate : PC98_DEFAULT_RATE; }

void mfdmidi_set_loops_h(void *h, int loops)
{
	MmState *s = (MmState *)h;
	if (!s) return;
	s->loops_want = loops < 1 ? 1 : loops;
	set_limit(s);
}

void mfdmidi_apply_mute_h(void *h, const pc98_cfg *cfg)
{
	MmState *s = (MmState *)h;
	if (!s || !cfg) return;
	s->mute_midi = cfg->mute_midi_mask;
}

const char *mfdmidi_title_h(void *h) { MmState *s = (MmState *)h; return s ? s->title : ""; }
const char *mfdmidi_engine_h(void *h) { MmState *s = (MmState *)h; return s ? s->engine : ""; }
const char *mfdmidi_sf2_h(void *h) { MmState *s = (MmState *)h; return s ? s->sf2_name : ""; }

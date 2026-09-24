/*
 * MsDRV 4.x light (.MS) — KENJI (KAKERA).
 * Spec: MST4_5A + ValleyBell MsDRV_SeqFormat.
 * Ekudorado: _N OPN, _B2 OPNA, _GS/_88 MIDI GS, _SB OPL3 (silent).
 * Banks: sibling {prefix}.OPN / {prefix}.SSG (48-byte / 16-byte programs).
 */
#include "msdrv_engine.h"
#include "pc98_util.h"
#include "fmd_engine.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>
#include <vector>

#include "ymfm.h"
#include "ymfm_opn.h"
#include "tsf.h"

#define MS_TRK       36
#define MS_HDR       0xA0
#define MS_CLOCK     7987200u
#define MS_MAX_TICKS 800000
#define MS_NEST      8
#define MS_OPN_N     256
#define MS_OPN_SZ    48
#define MS_SSG_SZ    16

enum { CK_NONE=0, CK_FM, CK_SSG, CK_MIDI, CK_OPL };

static const int k_op[4] = { 0, 8, 4, 12 }; /* file OP1,OP3,OP2,OP4 */
static const uint8_t k_car[8] = {
	0x08, 0x08, 0x08, 0x08, 0x0A, 0x0E, 0x0E, 0x0F
};
static const int k_fnum[12] = {
	0x269, 0x28E, 0x2B4, 0x2DE, 0x30A, 0x338,
	0x369, 0x39C, 0x3D3, 0x40E, 0x44B, 0x48D
};

class ms_iface : public ymfm::ymfm_interface {
public:
	uint8_t ymfm_external_read(ymfm::access_class, uint32_t) override { return 0; }
};

struct ms_trk {
	int active, ended;
	int pc, base, wait, gate_left;
	int note3;          /* 1 = 3-byte notes */
	int ch_id, kind, hw;
	int vol, inst, pan, detune;
	int keyed, cur_note;
	int loop_sp, loop_pc[MS_NEST], loop_cnt[MS_NEST];
	int sub_sp, sub_ret[MS_NEST], sub_end[MS_NEST];
	int pb, pb_step, pb_dst;
	int looped_inf;
};

struct ms_state {
	std::vector<uint8_t> file, opn, ssg;
	int trk_off[MS_TRK];
	ms_trk trk[MS_TRK];
	ms_iface iface;
	ymfm::ym2608 *opna;
	ymfm::ym2608::output_data out;
	tsf *sf;
	int rate, loops_want, one_loop_ms, ended, song_ended;
	int tempo, timebase, tempo_mod;
	int mute_fm, mute_ssg, mute_rhythm;
	int64_t chip_pos, chip_step, tick_acc;
	int last_l, last_r;
	uint32_t play_samples, play_limit;
	uint8_t ssg_mix;
	int variant; /* 0 OPN, 1 OPNA, 2 MIDI, 3 OPL */
	int loop_hit;
	int saw_inf_loop;
	char title[256];

	ms_state()
		: opna(NULL), sf(NULL), rate(PC98_DEFAULT_RATE), loops_want(1),
		  one_loop_ms(0), ended(0), song_ended(0), tempo(120), timebase(48),
		  tempo_mod(0x40), mute_fm(0), mute_ssg(0), mute_rhythm(0),
		  chip_pos(0), chip_step(0), tick_acc(0), last_l(0), last_r(0),
		  play_samples(0), play_limit(0), ssg_mix(0x38), variant(0), loop_hit(0), saw_inf_loop(0)
	{
		memset(trk_off, 0, sizeof trk_off);
		memset(trk, 0, sizeof trk);
		title[0] = 0;
	}
};

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int16_t rd16s(const uint8_t *p)
{
	return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

int msdrv_probe_mem(const uint8_t *data, size_t len)
{
	uint32_t off[MS_TRK], eof;
	int i, used = 0;
	if (!data || len < MS_HDR) return 0;
	for (i = 0; i < MS_TRK; i++) {
		off[i] = rd32(data + i * 4);
		if (off[i]) {
			if (off[i] < MS_HDR || off[i] >= len) return 0;
			used++;
		}
	}
	if (used < 1) return 0;
	if (rd32(data + 0x90) || rd32(data + 0x94) || rd32(data + 0x98))
		return 0;
	eof = rd32(data + 0x9C);
	if (eof < MS_HDR || eof > len + 16) return 0;
	for (i = 0; i < MS_TRK; i++) {
		uint8_t c;
		if (!off[i]) continue;
		c = data[off[i]];
		if (c == 0x8B || c == 0x8A || c == 0xE6 || c == 0x82 ||
		    c == 0x9C || c == 0xFE || c == 0x85 || c == 0x80 || c < 0x80)
			return 1;
		return 0;
	}
	return 0;
}

static int kind_of(int id)
{
	if (id >= 0x40 && id <= 0x42) return CK_SSG;
	if (id >= 0x50 && id <= 0x55) return CK_FM;
	if (id >= 0x00 && id <= 0x1F) return CK_MIDI;
	if (id >= 0x70 && id <= 0x81) return CK_OPL;
	if (id >= 0xF0 && id <= 0xF2) return CK_FM;
	return CK_NONE;
}
static int hw_of(int id, int kind)
{
	if (kind == CK_SSG) return id - 0x40;
	if (kind == CK_FM) {
		if (id >= 0x50 && id <= 0x55) return id - 0x50;
		return 2;
	}
	if (kind == CK_MIDI) return id & 15;
	if (kind == CK_OPL) return id - 0x70;
	return 0;
}

static void wr(ms_state *s, uint8_t aa, uint8_t dd)
{
	if (!s->opna) return;
	s->opna->write(0, aa);
	s->opna->write(1, dd);
}
static void wrx(ms_state *s, int ext, uint8_t aa, uint8_t dd)
{
	if (!s->opna) return;
	if (ext) { s->opna->write(2, aa); s->opna->write(3, dd); }
	else { s->opna->write(0, aa); s->opna->write(1, dd); }
}

static void apply_fm(ms_state *s, ms_trk *t)
{
	const uint8_t *pat;
	int c = t->hw, ext, slot, i, alg, mask, atten, tl, pan;
	if (!s->opna || c < 0 || c > 5) return;
	if (s->opn.size() < (size_t)(t->inst + 1) * MS_OPN_SZ) return;
	pat = &s->opn[(size_t)t->inst * MS_OPN_SZ];
	if (pat[0x2F] != 0) return;
	ext = c >= 3;
	slot = ext ? c - 3 : c;
	alg = pat[0] & 7;
	mask = k_car[alg];
	atten = 0x7F - (t->vol & 0x7F);
	if (atten < 0) atten = 0;
	for (i = 0; i < 4; i++) {
		int ro = k_op[i];
		wrx(s, ext, (uint8_t)(0x30 + slot + ro), pat[1 + i]);
		tl = pat[5 + i] & 0x7F;
		if (mask & (1 << i)) { tl += atten; if (tl > 0x7F) tl = 0x7F; }
		wrx(s, ext, (uint8_t)(0x40 + slot + ro), (uint8_t)tl);
		wrx(s, ext, (uint8_t)(0x50 + slot + ro), pat[9 + i]);
		wrx(s, ext, (uint8_t)(0x60 + slot + ro), pat[0xD + i]);
		wrx(s, ext, (uint8_t)(0x70 + slot + ro), pat[0x11 + i]);
		wrx(s, ext, (uint8_t)(0x80 + slot + ro), pat[0x15 + i]);
	}
	wrx(s, ext, (uint8_t)(0xB0 + slot), (uint8_t)(pat[0] & 0x3F));
	pan = 0xC0;
	if (t->pan == 1) pan = 0x40;
	else if (t->pan == 2) pan = 0x80;
	else if ((t->pan & 0x80) && t->pan != 0) pan = 0x80;
	else if (t->pan > 0 && t->pan < 0x80) pan = 0x40;
	pan |= (pat[0x23] & 0x37);
	wrx(s, ext, (uint8_t)(0xB4 + slot), (uint8_t)pan);
}

static int fm_key(int hw)
{
	return hw <= 2 ? hw : (hw - 3 + 4);
}

static void fm_off(ms_state *s, ms_trk *t)
{
	if (t->kind != CK_FM || !t->keyed) return;
	wr(s, 0x28, (uint8_t)fm_key(t->hw));
	t->keyed = 0;
}
static void fm_on(ms_state *s, ms_trk *t, int note)
{
	int n, blk, fn, ext, slot;
	int fnote = note + (t->pb / 256);
	if (t->kind != CK_FM) return;
	if (fnote < 0) fnote = 0;
	if (fnote > 127) fnote = 127;
	n = fnote % 12;
	blk = fnote / 12;
	if (blk < 1) blk = 1;
	if (blk > 7) blk = 7;
	fn = k_fnum[n] + t->detune;
	if (fn < 0) fn = 0;
	if (fn > 0x7FF) fn = 0x7FF;
	ext = t->hw >= 3;
	slot = ext ? t->hw - 3 : t->hw;
	if (t->keyed) fm_off(s, t);
	wrx(s, ext, (uint8_t)(0xA4 + slot), (uint8_t)((blk << 3) | ((fn >> 8) & 7)));
	wrx(s, ext, (uint8_t)(0xA0 + slot), (uint8_t)fn);
	wr(s, 0x28, (uint8_t)(0xF0 | fm_key(t->hw)));
	t->keyed = 1;
	t->cur_note = note;
}

static void ssg_off(ms_state *s, ms_trk *t)
{
	if (t->kind != CK_SSG || !t->keyed) return;
	wr(s, (uint8_t)(0x08 + t->hw), 0);
	t->keyed = 0;
}
static void ssg_on(ms_state *s, ms_trk *t, int note)
{
	double freq, period;
	int per, vol, c = t->hw;
	int fnote = note + (t->pb / 256);
	if (t->kind != CK_SSG || c < 0 || c > 2) return;
	if (fnote < 0) fnote = 0;
	if (fnote > 127) fnote = 127;
	freq = 440.0 * pow(2.0, (fnote - 69) / 12.0);
	period = (MS_CLOCK / 2.0) / (16.0 * freq);
	per = (int)(period + 0.5) + t->detune;
	if (per < 1) per = 1;
	if (per > 0xFFF) per = 0xFFF;
	vol = (t->vol & 0x7F) / 8;
	if (vol > 15) vol = 15;
	wr(s, (uint8_t)(c * 2), (uint8_t)(per & 0xFF));
	wr(s, (uint8_t)(c * 2 + 1), (uint8_t)((per >> 8) & 0x0F));
	wr(s, (uint8_t)(0x08 + c), (uint8_t)vol);
	s->ssg_mix = (uint8_t)((s->ssg_mix & ~(1 << c)) | (8 << c));
	wr(s, 0x07, s->ssg_mix);
	t->keyed = 1;
	t->cur_note = note;
}

static void midi_off(ms_state *s, ms_trk *t)
{
	if (!s->sf || t->kind != CK_MIDI || !t->keyed) return;
	tsf_channel_note_off(s->sf, t->hw, t->cur_note);
	t->keyed = 0;
}
static void midi_on(ms_state *s, ms_trk *t, int note, int vel)
{
	if (!s->sf || t->kind != CK_MIDI) return;
	if (t->keyed) midi_off(s, t);
	if (vel <= 0) vel = t->vol;
	if (vel <= 0) return;
	tsf_channel_note_on(s->sf, t->hw, note & 127, (vel & 0x7F) / 127.f);
	t->keyed = 1;
	t->cur_note = note & 127;
}

static void note_off(ms_state *s, ms_trk *t)
{
	if (t->kind == CK_FM) fm_off(s, t);
	else if (t->kind == CK_SSG) ssg_off(s, t);
	else if (t->kind == CK_MIDI) midi_off(s, t);
	t->gate_left = 0;
}
static void note_on(ms_state *s, ms_trk *t, int note, int gate, int vel)
{
	if (note == 0 || gate == 0) { note_off(s, t); return; }
	if (vel >= 0) t->vol = vel;
	if (t->kind == CK_FM) {
		if (s->mute_fm) { note_off(s, t); return; }
		apply_fm(s, t);
		fm_on(s, t, note);
	} else if (t->kind == CK_SSG) {
		if (s->mute_ssg) { note_off(s, t); return; }
		ssg_on(s, t, note);
	} else if (t->kind == CK_MIDI) {
		midi_on(s, t, note, vel >= 0 ? vel : t->vol);
	}
	t->gate_left = gate;
}

static void trk_step(ms_state *s, int ti, int dry)
{
	ms_trk *t = &s->trk[ti];
	const uint8_t *d = s->file.data();
	size_t n = s->file.size();
	int pc, cmd;
	if (!t->active || t->ended) return;
	if (t->wait > 0) {
		t->wait--;
		if (t->gate_left > 0) {
			t->gate_left--;
			if (t->gate_left == 0 && !dry) note_off(s, t);
		}
		if (!dry && t->pb_step) {
			if (t->pb < t->pb_dst) {
				t->pb += t->pb_step;
				if (t->pb > t->pb_dst) t->pb = t->pb_dst;
			} else if (t->pb > t->pb_dst) {
				t->pb -= t->pb_step;
				if (t->pb < t->pb_dst) t->pb = t->pb_dst;
			}
		}
		return;
	}
	pc = t->pc;
	if (pc < 0 || (size_t)pc >= n) { t->ended = 1; return; }
	if (t->sub_sp > 0 && pc >= t->sub_end[t->sub_sp - 1]) {
		t->pc = t->sub_ret[--t->sub_sp];
		return;
	}
	cmd = d[pc];
	if (cmd < 0x80) {
		int delay, gate, vel = -1;
		if (t->note3) {
			if ((size_t)pc + 3 > n) { t->ended = 1; return; }
			delay = d[pc + 1]; gate = d[pc + 2]; t->pc = pc + 3;
		} else {
			if ((size_t)pc + 4 > n) { t->ended = 1; return; }
			delay = d[pc + 1]; gate = d[pc + 2]; vel = d[pc + 3];
			t->pc = pc + 4;
		}
		if (!dry) note_on(s, t, cmd, gate, vel);
		else t->gate_left = gate;
		t->wait = delay;
		return;
	}
	switch (cmd) {
	case 0x80:
		if ((size_t)pc + 3 > n) { t->ended = 1; return; }
		s->timebase = d[pc + 1] | (d[pc + 2] << 8);
		if (s->timebase <= 0) s->timebase = 48;
		t->pc = pc + 3; break;
	case 0x81: t->pc = pc + 4; break;
	case 0x82:
		if ((size_t)pc + 2 > n) { t->ended = 1; return; }
		t->inst = d[pc + 1];
		if (!dry && t->kind == CK_FM) apply_fm(s, t);
		if (!dry && t->kind == CK_MIDI && s->sf)
			tsf_channel_set_presetnumber(s->sf, t->hw, t->inst & 127, t->hw == 9);
		t->pc = pc + 2; break;
	case 0x83: {
		uint32_t st, en;
		if ((size_t)pc + 9 > n) { t->ended = 1; return; }
		st = rd32(d + pc + 1); en = rd32(d + pc + 5);
		t->pc = pc + 9;
		if (t->sub_sp < MS_NEST) {
			t->sub_ret[t->sub_sp] = t->pc;
			t->sub_end[t->sub_sp] = t->base + (int)en;
			t->sub_sp++;
			t->pc = t->base + (int)st;
		}
		break;
	}
	case 0x84:
		if (t->sub_sp > 0) t->pc = t->sub_ret[--t->sub_sp];
		else if ((size_t)pc + 3 <= n) t->pc = pc + rd16s(d + pc + 1);
		else t->ended = 1;
		break;
	case 0x85:
		if ((size_t)pc + 2 > n) { t->ended = 1; return; }
		t->vol = d[pc + 1] & 0x7F;
		if (!dry && t->kind == CK_MIDI && s->sf)
			tsf_channel_midi_control(s->sf, t->hw, 7, t->vol);
		if (!dry && t->keyed && t->kind == CK_FM) apply_fm(s, t);
		if (!dry && t->keyed && t->kind == CK_SSG) {
			int v = t->vol / 8; if (v > 15) v = 15;
			wr(s, (uint8_t)(0x08 + t->hw), (uint8_t)v);
		}
		t->pc = pc + 2; break;
	case 0x8A:
		if ((size_t)pc + 2 > n) { t->ended = 1; return; }
		s->tempo = d[pc + 1]; if (s->tempo <= 0) s->tempo = 120;
		t->pc = pc + 2; break;
	case 0x8B:
		if ((size_t)pc + 2 > n) { t->ended = 1; return; }
		t->note3 = (d[pc + 1] == 1);
		t->pc = pc + 2; break;
	case 0x8C: case 0x8E: t->pc = pc + 4; break;
	case 0x8D: case 0x8F:
		if ((size_t)pc + 4 > n) { t->ended = 1; return; }
		t->pc = pc + 4 + d[pc + 3]; break;
	case 0x91: case 0x9E: case 0xC2: case 0xC4: t->pc = pc + 1; break;
	case 0x94:
		if ((size_t)pc + 3 > n) { t->ended = 1; return; }
		if (!dry) wr(s, d[pc + 1], d[pc + 2]);
		t->pc = pc + 3; break;
	case 0x96: t->pc = pc + 3; break;
	case 0x9B: {
		int cnt;
		if ((size_t)pc + 2 > n) { t->ended = 1; return; }
		cnt = d[pc + 1];
		if (t->loop_sp <= 0) { t->pc = pc + 2; break; }
		if (cnt == 0) {
			t->pc = t->loop_pc[t->loop_sp - 1];
			s->saw_inf_loop = 1; t->looped_inf = 1;
		} else {
			int idx = t->loop_sp - 1;
			if (t->loop_cnt[idx] == 0) t->loop_cnt[idx] = cnt;
			t->loop_cnt[idx]--;
			if (t->loop_cnt[idx] > 0) t->pc = t->loop_pc[idx];
			else { t->loop_sp--; t->pc = pc + 2; }
		}
		break;
	}
	case 0x9C:
		if (t->loop_sp < MS_NEST) {
			t->loop_pc[t->loop_sp] = pc + 1;
			t->loop_cnt[t->loop_sp] = 0;
			t->loop_sp++;
		}
		t->pc = pc + 1; break;
	case 0x9D:
		if ((size_t)pc + 2 > n) { t->ended = 1; return; }
		t->detune = (int8_t)d[pc + 1];
		t->pc = pc + 2; break;
	case 0x9F:
		if ((size_t)pc + 2 > n) { t->ended = 1; return; }
		t->pan = d[pc + 1];
		if (!dry && t->kind == CK_MIDI && s->sf)
			tsf_channel_midi_control(s->sf, t->hw, 10, ((t->pan + 0x80) & 0xFF) >> 1);
		t->pc = pc + 2; break;
	case 0xA4:
		if ((size_t)pc + 3 > n) { t->ended = 1; return; }
		t->pb = (int16_t)(d[pc + 1] | (d[pc + 2] << 8));
		t->pc = pc + 3; break;
	case 0xA5: case 0xA6: case 0xA8: case 0xA9: case 0xAA:
	case 0xB0: case 0xB1:
	case 0xD1: case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6:
		t->pc = pc + 2; break;
	case 0xA7: case 0xAB: case 0xAC: t->pc = pc + 3; break;
	case 0xAD:
		if ((size_t)pc + 3 > n) { t->ended = 1; return; }
		t->pb_step = (int16_t)(d[pc + 1] | (d[pc + 2] << 8));
		if (t->pb_step < 0) t->pb_step = -t->pb_step;
		t->pc = pc + 3; break;
	case 0xAE:
		if ((size_t)pc + 3 > n) { t->ended = 1; return; }
		t->pb_dst = (int16_t)(d[pc + 1] | (d[pc + 2] << 8));
		t->pc = pc + 3; break;
	case 0xAF:
		if ((size_t)pc + 3 > n) { t->ended = 1; return; }
		t->pb = (int16_t)(d[pc + 1] | (d[pc + 2] << 8));
		t->pc = pc + 3; break;
	case 0xC1: t->pc = pc + 3; break;
	case 0xC3: t->pc = pc + 2; break;
	case 0xC5:
		if ((size_t)pc + 3 > n) { t->ended = 1; return; }
		t->pc = pc + 3 + (d[pc + 1] | (d[pc + 2] << 8)); break;
	case 0xD0:
		if ((size_t)pc + 3 > n) { t->ended = 1; return; }
		if (!dry && !s->mute_rhythm) wr(s, 0x10, d[pc + 2]);
		t->wait = d[pc + 1]; t->pc = pc + 3; break;
	case 0xDD: case 0xDE: case 0xDF:
	case 0xE2: case 0xE7: case 0xEB: case 0xED: case 0xEE:
		t->pc = pc + 4; break;
	case 0xE6: {
		int id;
		if ((size_t)pc + 3 > n) { t->ended = 1; return; }
		t->wait = d[pc + 1];
		id = d[pc + 2];
		t->ch_id = id;
		t->kind = kind_of(id);
		t->hw = hw_of(id, t->kind);
		if (!dry && t->kind == CK_MIDI && s->sf) {
			tsf_channel_set_presetnumber(s->sf, t->hw, t->inst & 127, t->hw == 9);
			tsf_channel_midi_control(s->sf, t->hw, 7, t->vol ? t->vol : 100);
		}
		t->pc = pc + 3; break;
	}
	case 0xEA: t->pc = pc + 3; break;
	case 0xEC:
		if ((size_t)pc + 3 <= n) {
			t->wait = d[pc + 1];
			t->inst = d[pc + 2];
			if (!dry && t->kind == CK_MIDI && s->sf)
				tsf_channel_set_presetnumber(s->sf, t->hw, t->inst & 127, t->hw == 9);
		}
		t->pc = pc + 3; break;
	case 0xFE:
		t->ended = 1; if (!dry) note_off(s, t); break;
	case 0xFF:
		t->ended = 1; s->song_ended = 1; if (!dry) note_off(s, t); break;
	default: t->pc = pc + 1; break;
	}
}

static void tick(ms_state *s, int dry)
{
	int i, alive = 0;
	if (s->song_ended) { s->ended = 1; return; }
	for (i = 0; i < MS_TRK; i++) {
		int guard;
		if (!s->trk[i].active || s->trk[i].ended) continue;
		alive++;
		guard = 64;
		while (guard-- > 0 && s->trk[i].active && !s->trk[i].ended &&
		       s->trk[i].wait == 0)
			trk_step(s, i, dry);
		if (s->trk[i].wait > 0)
			trk_step(s, i, dry);
	}
	if (!alive) s->ended = 1;
}

static int ticks_per_sec(const ms_state *s)
{
	int tb = s->timebase > 0 ? s->timebase : 48;
	int bpm = s->tempo > 0 ? s->tempo : 120;
	int mod = s->tempo_mod > 0 ? s->tempo_mod : 0x40;
	int tps = (int)((double)bpm * mod / 64.0 * tb / 60.0 + 0.5);
	return tps > 0 ? tps : 1;
}

static void reset_trks(ms_state *s)
{
	int i;
	s->ended = 0; s->song_ended = 0; s->saw_inf_loop = 0;
	s->tempo = 120; s->timebase = 48; s->tempo_mod = 0x40;
	s->ssg_mix = 0x38;
	memset(s->trk, 0, sizeof s->trk);
	for (i = 0; i < MS_TRK; i++) {
		if (!s->trk_off[i]) continue;
		s->trk[i].active = 1;
		s->trk[i].base = s->trk_off[i];
		s->trk[i].pc = s->trk_off[i];
		s->trk[i].vol = 0x64;
		s->trk[i].pan = 3;
		s->trk[i].ch_id = 0xFF;
	}
}

static void reset_chip(ms_state *s)
{
	int i;
	if (!s->opna) return;
	s->opna->reset();
	wr(s, 0x29, 0x80);
	wr(s, 0x07, 0x38);
	wr(s, 0x11, 0x3F);
	for (i = 0; i < 6; i++)
		wr(s, 0x28, (uint8_t)(i <= 2 ? i : i - 3 + 4));
}

static void detect_variant(ms_state *s)
{
	int i, fm = 0, fm6 = 0, midi = 0, opl = 0;
	for (i = 0; i < MS_TRK; i++) {
		if (s->trk[i].kind == CK_FM) { fm = 1; if (s->trk[i].hw >= 3) fm6 = 1; }
		else if (s->trk[i].kind == CK_MIDI) midi = 1;
		else if (s->trk[i].kind == CK_OPL) opl = 1;
	}
	if (opl && !fm) s->variant = 3;
	else if (midi && !fm) s->variant = 2;
	else if (fm6) s->variant = 1;
	else s->variant = 0;
}

static int measure_ms(ms_state *s)
{
	int ticks = 0, loop_at = -1, i;
	reset_trks(s);
	s->saw_inf_loop = 0;
	while (!s->ended && ticks < MS_MAX_TICKS) {
		tick(s, 1);
		ticks++;
		/* After each tick, if any track just took an infinite loop, remember max. */
		if (s->saw_inf_loop) {
			if (loop_at < 0 || ticks > loop_at) loop_at = ticks;
			s->saw_inf_loop = 0;
		}
		/* Done when every still-active track has hit an infinite loop once. */
		{
			int active = 0, looped = 0;
			for (i = 0; i < MS_TRK; i++) {
				if (!s->trk[i].active || s->trk[i].ended) continue;
				active++;
				if (s->trk[i].looped_inf) looped++;
			}
			if (active > 0 && looped >= active && loop_at >= 0)
				break;
		}
	}
	if (loop_at < 0) loop_at = ticks;
	{
		int tps, ms;
		reset_trks(s);
		ticks = 0;
		while (ticks < loop_at && !s->ended && ticks < MS_MAX_TICKS) {
			tick(s, 1); ticks++;
		}
		tps = ticks_per_sec(s);
		if (tps < 1) tps = 1;
		ms = (int)((int64_t)loop_at * 1000 / tps);
		if (ms < 1000) ms = 1000;
		if (ms > 10 * 60 * 1000) ms = 10 * 60 * 1000;
		return ms;
	}
}

static int load_bank(const char *dir, const char *prefix, const char *ext,
		std::vector<uint8_t> *out, size_t want)
{
	char path[PC98_PATH_MAX], e2[8];
	FILE *fp;
	long sz;
	size_t i;
	snprintf(path, sizeof path, "%s%s%s", dir, prefix, ext);
	fp = fopen(path, "rb");
	if (!fp) {
		snprintf(e2, sizeof e2, "%s", ext);
		for (i = 0; e2[i]; i++) e2[i] = (char)tolower((unsigned char)e2[i]);
		snprintf(path, sizeof path, "%s%s%s", dir, prefix, e2);
		fp = fopen(path, "rb");
	}
	if (!fp) return 0;
	fseek(fp, 0, SEEK_END); sz = ftell(fp); fseek(fp, 0, SEEK_SET);
	if (sz < (long)want) { fclose(fp); return 0; }
	out->resize(want);
	if (fread(out->data(), 1, want, fp) != want) { out->clear(); fclose(fp); return 0; }
	fclose(fp);
	return 1;
}

static void find_banks(ms_state *s, const char *filename)
{
	char dir[PC98_PATH_MAX], base[PC98_PATH_MAX], prefix[PC98_PATH_MAX];
	char *us;
	pc98_dir_of(filename, dir, sizeof dir);
	pc98_basename(filename, base, sizeof base);
	pc98_bounded(prefix, sizeof prefix, base);
	us = strchr(prefix, '_');
	if (us) *us = 0;
	if (!prefix[0]) return;
	load_bank(dir, prefix, ".OPN", &s->opn, (size_t)MS_OPN_N * MS_OPN_SZ);
	load_bank(dir, prefix, ".SSG", &s->ssg, (size_t)MS_OPN_N * MS_SSG_SZ);
}

static int setup(ms_state *s, const char *filename, const uint8_t *data,
		size_t len, const pc98_cfg *cfg)
{
	int i;
	char base[128];
	if (!msdrv_probe_mem(data, len)) return -1;
	s->file.assign(data, data + len);
	for (i = 0; i < MS_TRK; i++) {
		uint32_t o = rd32(data + i * 4);
		s->trk_off[i] = (o && o < len) ? (int)o : 0;
	}
	s->rate = PC98_DEFAULT_RATE;
	s->loops_want = (cfg && cfg->loop_count > 0) ? cfg->loop_count : 1;
	s->mute_fm = cfg ? cfg->mute_fm : 0;
	s->mute_ssg = cfg ? cfg->mute_ssg : 0;
	s->mute_rhythm = cfg ? cfg->mute_rhythm : 0;
	find_banks(s, filename);

	reset_trks(s);
	for (i = 0; i < 4000 && !s->ended; i++) tick(s, 1);
	detect_variant(s);

	pc98_basename(filename, base, sizeof base);
	pc98_bounded(s->title, sizeof s->title, base);

	s->one_loop_ms = measure_ms(s);

	if (s->variant == 2) {
		char sf2[PC98_PATH_MAX];
		if (fmd_find_sf2(cfg, filename, 0, sf2, sizeof sf2))
			s->sf = (tsf *)fmd_font_get(sf2);
		if (s->sf)
			tsf_set_output(s->sf, TSF_STEREO_INTERLEAVED, s->rate, 0);
	} else if (s->variant != 3) {
		s->opna = new ymfm::ym2608(s->iface);
		s->chip_step = ((int64_t)MS_CLOCK << 16) / s->rate;
		reset_chip(s);
	}

	reset_trks(s);
	if (s->opna) reset_chip(s);
	if (s->sf) {
		int c;
		tsf_reset(s->sf);
		tsf_set_output(s->sf, TSF_STEREO_INTERLEAVED, s->rate, 0);
		for (c = 0; c < 16; c++) {
			tsf_channel_set_presetnumber(s->sf, c, 0, c == 9);
			tsf_channel_midi_control(s->sf, c, 7, 100);
			tsf_channel_midi_control(s->sf, c, 10, 64);
		}
	}
	s->play_limit = (uint32_t)(((int64_t)s->one_loop_ms * s->loops_want * s->rate) / 1000);
	s->play_samples = 0;
	s->tick_acc = 0;
	s->chip_pos = 0;
	return 0;
}

int msdrv_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out)
{
	ms_state tmp;
	const char *ft, *eng;
	if (!out) return -1;
	memset(out, 0, sizeof *out);
	if (setup(&tmp, filename, data, len, cfg) != 0) {
		delete tmp.opna;
		return -1;
	}
	out->kind = PC98_KIND_MSDRV;
	out->songs = 1;
	out->looping = 1;
	out->one_loop_ms[0] = tmp.one_loop_ms;
	pc98_bounded(out->title, sizeof out->title, tmp.title);
	if (tmp.variant == 0) { ft = "MsDRV OPN"; eng = "ymfm MsDRV OPN"; }
	else if (tmp.variant == 1) { ft = "MsDRV OPNA"; eng = "ymfm MsDRV OPNA"; }
	else if (tmp.variant == 2) {
		ft = "MsDRV GS";
		eng = tmp.sf ? "TinySoundFont MsDRV GS" : "MsDRV GS (no SF2)";
	} else { ft = "MsDRV OPL3"; eng = "MsDRV OPL3 (unsupported)"; }
	pc98_bounded(out->filetype, sizeof out->filetype, ft);
	pc98_bounded(out->engine, sizeof out->engine, eng);
	delete tmp.opna;
	return 0;
}

void *msdrv_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg)
{
	ms_state *s = new ms_state();
	if (setup(s, filename, data, len, cfg) != 0) {
		delete s->opna;
		delete s;
		return NULL;
	}
	return s;
}

void msdrv_close_h(void *h)
{
	ms_state *s = (ms_state *)h;
	if (!s) return;
	delete s->opna;
	delete s;
}

int msdrv_process_h(void *h, float *buf, int count)
{
	ms_state *s = (ms_state *)h;
	int i, tps;
	int64_t need;
	if (!s || !buf || count <= 0) return 0;
	tps = ticks_per_sec(s);
	need = ((int64_t)s->rate << 16) / tps;
	for (i = 0; i < count; i++) {
		float L = 0, R = 0;
		if (s->play_limit && s->play_samples >= s->play_limit) {
			buf[i * 2] = buf[i * 2 + 1] = 0;
			continue;
		}
		s->tick_acc += 1 << 16;
		while (s->tick_acc >= need) {
			tick(s, 0);
			s->tick_acc -= need;
		}
		if (s->opna) {
			s->chip_pos += s->chip_step;
			while (s->chip_pos >= 0x10000) {
				int fm_l, fm_r, ssg;
				s->opna->generate(&s->out);
				fm_l = s->out.data[0];
				fm_r = s->out.data[1];
				ssg = s->out.data[2];
				if (s->mute_fm) { fm_l = 0; fm_r = 0; }
				if (s->mute_ssg) ssg = 0;
				s->last_l = fm_l + ssg;
				s->last_r = fm_r + ssg;
				s->chip_pos -= 0x10000;
			}
			L = s->last_l / 32768.f;
			R = s->last_r / 32768.f;
		}
		if (s->sf) {
			float m[2] = {0, 0};
			tsf_render_float(s->sf, m, 1, 0);
			L += m[0]; R += m[1];
		}
		if (L > 1) L = 1;
		if (L < -1) L = -1;
		if (R > 1) R = 1;
		if (R < -1) R = -1;
		buf[i * 2] = L; buf[i * 2 + 1] = R;
		s->play_samples++;
	}
	return count;
}

int msdrv_seek_ms_h(void *h, int ms)
{
	ms_state *s = (ms_state *)h;
	int tps, ticks, i;
	if (!s) return -1;
	if (ms < 0) ms = 0;
	reset_trks(s);
	if (s->opna) reset_chip(s);
	if (s->sf) {
		tsf_note_off_all(s->sf);
		tsf_reset(s->sf);
		tsf_set_output(s->sf, TSF_STEREO_INTERLEAVED, s->rate, 0);
	}
	tps = ticks_per_sec(s);
	ticks = (int)((int64_t)ms * tps / 1000);
	for (i = 0; i < ticks && !s->ended; i++) tick(s, 0);
	s->play_samples = (uint32_t)(((int64_t)ms * s->rate) / 1000);
	s->tick_acc = 0;
	s->chip_pos = 0;
	return 0;
}

int msdrv_one_loop_ms_h(void *h)
{
	ms_state *s = (ms_state *)h;
	return s ? s->one_loop_ms : 0;
}
int msdrv_rate_h(void *h)
{
	ms_state *s = (ms_state *)h;
	return s ? s->rate : PC98_DEFAULT_RATE;
}
void msdrv_set_loops_h(void *h, int loops)
{
	ms_state *s = (ms_state *)h;
	if (!s) return;
	s->loops_want = loops > 0 ? loops : 1;
	s->play_limit = (uint32_t)(((int64_t)s->one_loop_ms * s->loops_want * s->rate) / 1000);
}
void msdrv_apply_mute_h(void *h, const pc98_cfg *cfg)
{
	ms_state *s = (ms_state *)h;
	if (!s || !cfg) return;
	s->mute_fm = cfg->mute_fm;
	s->mute_ssg = cfg->mute_ssg;
	s->mute_rhythm = cfg->mute_rhythm;
}
const char *msdrv_title_h(void *h)
{
	ms_state *s = (ms_state *)h;
	return s ? s->title : "";
}

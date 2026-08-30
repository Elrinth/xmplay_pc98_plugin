/*
 * MBMUSic driver 1.03 — Kirishima (KG##.MSB / KG##N.MSB).
 * Words are obfuscated (xchg/rol/not/ror). 17 parts: FM1–3, SSG, FM4–6.
 */
#include "msb_engine.h"
#include "pc98_util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <vector>

#include "ymfm.h"
#include "ymfm_opn.h"

#define MSB_CH        9
#define MSB_CLOCK     7987200u
#define MSB_MAX_TICKS 400000
#define MSB_NEST      8
#define MSB_F7        16

/* Op order M1,C1,M2,C2 → regs +0,+8,+4,+C. Carrier bits match that order. */
static const uint8_t k_car[8] = {
	0x08, 0x08, 0x08, 0x08, 0x0A, 0x0E, 0x0E, 0x0F
};
static const int k_op[4] = { 0, 8, 4, 12 };

static const uint8_t k_def_pat[25] = {
	0x31, 0x31, 0x31, 0x31, 0x18, 0x18, 0x18, 0x18,
	0x1F, 0x1F, 0x1F, 0x1F, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x05, 0x05, 0x05, 0x05, 0x3C
};

class msb_iface : public ymfm::ymfm_interface {
public:
	uint8_t ymfm_external_read(ymfm::access_class, uint32_t) override { return 0; }
};

struct msb_ch {
	int enabled, ended, ssg, ext;
	int pc, wait, len, gate, keyed, slur, nogate;
	int vol, inst, detune, pan;
	int loop_pc[MSB_NEST];
	int loop_n;
	int call_pc[MSB_NEST];
	int call_n;
	int f7_at[MSB_F7];
	int f7_left[MSB_F7];
	int f7_n;
	int did_loop;
};

struct msb_state {
	std::vector<uint8_t> file;
	int part[MSB_CH];
	uint8_t voice[256][25];
	int has_voice[256];
	msb_ch ch[MSB_CH];
	msb_iface iface;
	ymfm::ym2608 *opna;
	ymfm::ym2608::output_data out;
	int rate, loops_want, one_loop_ms, ended;
	int ta_word, mute_fm, mute_ssg;
	int64_t chip_pos, chip_step, irq_acc;
	int last_l, last_r;
	uint32_t play_samples, play_limit;
	uint8_t ssg_mix;
	char title[256], game[256];

	msb_state()
		: opna(NULL), rate(PC98_DEFAULT_RATE), loops_want(1), one_loop_ms(0),
		  ended(0), ta_word(0x200), mute_fm(0), mute_ssg(0),
		  chip_pos(0), chip_step(0), irq_acc(0), last_l(0), last_r(0),
		  play_samples(0), play_limit(0), ssg_mix(0xB8)
	{
		memset(part, 0, sizeof part);
		memset(voice, 0, sizeof voice);
		memset(has_voice, 0, sizeof has_voice);
		memset(ch, 0, sizeof ch);
		title[0] = 0;
		game[0] = 0;
	}
};

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int32_t fmgen_vol(double db)
{
	if (db <= -192.0) return 0;
	if (db > 20.0) db = 20.0;
	return (int32_t)(65536.0 * pow(10.0, db / 40.0));
}

static void msb_deob(uint8_t *out, const uint8_t *in, size_t n)
{
	size_t i;
	memcpy(out, in, n);
	for (i = 0; i + 1 < n; i += 2) {
		unsigned dx = out[i] | ((unsigned)out[i + 1] << 8);
		unsigned dl, dh;
		dx = ((dx & 0xFF) << 8) | (dx >> 8);
		dx = ((dx << 1) | (dx >> 15)) & 0xFFFF;
		dx = (~dx) & 0xFFFF;
		dl = dx & 0xFF;
		dh = (dx >> 8) & 0xFF;
		dl = ((dl >> 4) | (dl << 4)) & 0xFF;
		dh = ((dh >> 2) | (dh << 6)) & 0xFF;
		out[i] = (uint8_t)dl;
		out[i + 1] = (uint8_t)dh;
	}
}

static void wr(msb_state *s, uint8_t aa, uint8_t dd)
{
	if (!s->opna) return;
	s->opna->write(0, aa);
	s->opna->write(1, dd);
}

static void wrx(msb_state *s, int ext, uint8_t aa, uint8_t dd)
{
	if (!s->opna) return;
	if (ext) {
		s->opna->write(2, aa);
		s->opna->write(3, dd);
	} else {
		s->opna->write(0, aa);
		s->opna->write(1, dd);
	}
}

static int fm_slot(int c)
{
	if (c <= 2) return c;
	return c - 6;
}

static int key_id(int c)
{
	if (c <= 2) return c;
	return c - 6 + 4;
}

static void apply_fm(msb_state *s, int c, const uint8_t *pat, int vol)
{
	int ext = c >= 6;
	int slot = fm_slot(c);
	int i, alg, mask, atten;
	alg = pat[24] & 7;
	mask = k_car[alg];
	atten = 127 - vol;
	if (atten < 0) atten = 0;
	if (atten > 127) atten = 127;
	for (i = 0; i < 24; ++i) {
		int op = i & 3;
		int grp = i / 4;
		int v = pat[i];
		if (grp == 1 && (mask & (1 << op))) {
			v += atten;
			if (v > 127) v = 127;
		}
		wrx(s, ext, (uint8_t)(0x30 + slot + k_op[op] + grp * 16),
				(uint8_t)v);
	}
	wrx(s, ext, (uint8_t)(0xB0 + slot), pat[24]);
	wrx(s, ext, (uint8_t)(0xB4 + slot), (uint8_t)s->ch[c].pan);
}

static void keyoff(msb_state *s, int c)
{
	msb_ch *ch = &s->ch[c];
	if (ch->ssg) {
		wr(s, (uint8_t)(0x08 + (c - 3)), 0);
	} else {
		wr(s, 0x28, (uint8_t)key_id(c));
	}
	ch->keyed = 0;
}

static void play_note(msb_state *s, int c, int a4, int a0, int rest, int dry)
{
	msb_ch *ch = &s->ch[c];
	int slur = ch->slur;
	ch->slur = 0;
	if (dry) return;
	if (rest) {
		keyoff(s, c);
		return;
	}
	if (ch->ssg) {
		int per = ((a4 & 7) << 8) | (a0 & 0xFF);
		int ssgc = c - 3;
		int vol = ch->vol >> 3;
		if (vol > 15) vol = 15;
		wr(s, (uint8_t)(ssgc * 2), (uint8_t)per);
		wr(s, (uint8_t)(ssgc * 2 + 1), (uint8_t)(per >> 8));
		wr(s, (uint8_t)(0x08 + ssgc), (uint8_t)vol);
		s->ssg_mix &= (uint8_t)~(1 << ssgc);
		wr(s, 0x07, s->ssg_mix);
		ch->keyed = 1;
	} else {
		int fn = ((a4 & 7) << 8) | a0;
		int blk = a4 & 0x38;
		int ext = c >= 6;
		int slot = fm_slot(c);
		fn += ch->detune;
		if (fn < 0) fn = 0;
		if (fn > 0x7FF) fn = 0x7FF;
		if (ch->keyed && !slur) keyoff(s, c);
		wrx(s, ext, (uint8_t)(0xA4 + slot), (uint8_t)(blk | ((fn >> 8) & 7)));
		wrx(s, ext, (uint8_t)(0xA0 + slot), (uint8_t)fn);
		if (!slur || !ch->keyed)
			wr(s, 0x28, (uint8_t)(0xF0 | key_id(c)));
		ch->keyed = 1;
	}
}

static int fetchb(msb_state *s, msb_ch *ch, int *out)
{
	if (ch->pc < 0 || ch->pc >= (int)s->file.size()) return 0;
	*out = s->file[ch->pc++];
	return 1;
}

static void sel_inst(msb_state *s, int c, int id, int dry)
{
	const uint8_t *pat;
	s->ch[c].inst = id;
	if (s->has_voice[id & 255])
		pat = s->voice[id & 255];
	else
		pat = k_def_pat;
	if (!dry && !s->ch[c].ssg)
		apply_fm(s, c, pat, s->ch[c].vol);
}

static void do_cmd(msb_state *s, int c, int cmd, int dry, int measure)
{
	msb_ch *ch = &s->ch[c];
	int a = 0, b = 0, n, dest, cmdpos;
	cmdpos = ch->pc - 1;
	switch (cmd) {
	case 0xD1:
	case 0xD2:
		if (ch->loop_n > 0) ch->loop_n--;
		return;
	case 0xD6:
		if (fetchb(s, ch, &a)) {
			ch->vol -= a;
			if (ch->vol < 0) ch->vol = 0;
			if (!dry && !ch->ssg) sel_inst(s, c, ch->inst, dry);
		}
		return;
	case 0xD7:
		if (fetchb(s, ch, &a)) {
			ch->vol += a;
			if (ch->vol > 127) ch->vol = 127;
			if (!dry && !ch->ssg) sel_inst(s, c, ch->inst, dry);
		}
		return;
	case 0xD8:
	case 0xDE:
		if (fetchb(s, ch, &a)) ch->gate = a;
		return;
	case 0xD9:
		ch->pc += 6;
		return;
	case 0xDA:
		ch->pc += 3;
		return;
	case 0xDB:
		ch->pc += 2;
		if (!fetchb(s, ch, &n)) return;
		if (n > 0 && n < 200 && ch->pc + n <= (int)s->file.size()) {
			if (!s->title[0]) {
				char tmp[256];
				int i, o = 0;
				for (i = 0; i < n && o < 250; ++i) {
					unsigned char x = s->file[ch->pc + i];
					if (x >= 0x20 && x != 0x7F) tmp[o++] = (char)x;
					else if (x >= 0x80) tmp[o++] = (char)x;
				}
				tmp[o] = 0;
				pc98_bounded(s->title, sizeof s->title, tmp);
			}
			ch->pc += n;
		}
		return;
	case 0xDC:
	case 0xE7:
		ch->pc += 2;
		return;
	case 0xDF:
		ch->nogate = 1;
		return;
	case 0xE0:
		if (ch->loop_n < MSB_NEST)
			ch->loop_pc[ch->loop_n++] = ch->pc;
		return;
	case 0xE1:
		ch->slur = 1;
		return;
	case 0xE2:
		if (fetchb(s, ch, &a)) {
			ch->vol = a;
			if (!dry && !ch->ssg) sel_inst(s, c, ch->inst, dry);
			if (!dry && ch->ssg) {
				int v = a >> 3;
				if (v > 15) v = 15;
				wr(s, (uint8_t)(0x08 + (c - 3)), (uint8_t)v);
			}
		}
		return;
	case 0xE3:
	case 0xE4:
		ch->pc += 2;
		return;
	case 0xE5:
		fetchb(s, ch, &a);
		ch->loop_n = 0;
		ch->call_n = 0;
		return;
	case 0xE8:
		if (ch->call_n > 0)
			ch->pc = ch->call_pc[--ch->call_n];
		return;
	case 0xE6:
		if (!fetchb(s, ch, &a)) return;
		if (ch->pc + 25 <= (int)s->file.size()) {
			memcpy(s->voice[a & 255], &s->file[ch->pc], 25);
			s->has_voice[a & 255] = 1;
			ch->pc += 25;
		}
		return;
	case 0xE9:
	case 0xEA:
		if (ch->pc + 1 >= (int)s->file.size()) return;
		{
			int16_t rel = (int16_t)rd16(&s->file[ch->pc]);
			dest = cmdpos + rel;
			ch->pc += 2;
			if (cmd == 0xE9) {
				if (ch->call_n < MSB_NEST)
					ch->call_pc[ch->call_n++] = ch->pc;
				if (dest >= 0 && dest < (int)s->file.size())
					ch->pc = dest;
				return;
			}
			if (measure && dest < cmdpos) {
				ch->did_loop = 1;
				ch->ended = 1;
				return;
			}
			if (dest >= 0 && dest < (int)s->file.size())
				ch->pc = dest;
		}
		return;
	case 0xEB:
		if (fetchb(s, ch, &a)) {
			if (ch->ssg) {
				s->ssg_mix = (uint8_t)(s->ssg_mix & a);
				if (!dry) wr(s, 0x07, s->ssg_mix);
			} else {
				sel_inst(s, c, a, dry);
			}
		}
		return;
	case 0xED:
	case 0xEF:
		fetchb(s, ch, &a);
		return;
	case 0xEE:
		ch->pc += 2;
		return;
	case 0xF0:
		if (fetchb(s, ch, &a)) ch->detune = (int8_t)a;
		return;
	case 0xF1:
		ch->pc += 2;
		return;
	case 0xF2:
		if (ch->ssg) ch->pc += 3;
		return;
	case 0xF3:
		ch->pc += 3;
		return;
	case 0xF4:
		if (fetchb(s, ch, &a) && fetchb(s, ch, &b)) {
			ch->len = a > 0 ? a : 1;
			ch->gate = b;
		}
		return;
	case 0xF5:
		if (ch->pc + 1 < (int)s->file.size()) {
			s->ta_word = rd16(&s->file[ch->pc]);
			ch->pc += 2;
		}
		return;
	case 0xF6:
		if (fetchb(s, ch, &a)) {
			ch->pan = a;
			if (!dry && !ch->ssg)
				wrx(s, c >= 6, (uint8_t)(0xB4 + fm_slot(c)), (uint8_t)a);
		}
		return;
	case 0xF7:
		if (ch->pc + 2 < (int)s->file.size()) {
			int16_t rel = (int16_t)rd16(&s->file[ch->pc]);
			int cnt = s->file[ch->pc + 2];
			int slot = -1, k;
			ch->pc += 3;
			dest = cmdpos - rel;
			for (k = 0; k < ch->f7_n; ++k) {
				if (ch->f7_at[k] == cmdpos) { slot = k; break; }
			}
			if (slot < 0 && ch->f7_n < MSB_F7) {
				slot = ch->f7_n++;
				ch->f7_at[slot] = cmdpos;
				ch->f7_left[slot] = 0;
			}
			if (slot >= 0) {
				if (ch->f7_left[slot] <= 0)
					ch->f7_left[slot] = cnt > 0 ? cnt : 1;
				ch->f7_left[slot]--;
				if (ch->f7_left[slot] > 0 && dest >= 0 &&
						dest < (int)s->file.size()) {
					if (measure && dest < cmdpos) {
						ch->did_loop = 1;
						ch->ended = 1;
						return;
					}
					ch->pc = dest;
				}
			}
		}
		return;
	case 0xF8:
		ch->pc += 4;
		return;
	case 0xFA:
		ch->pc += 8;
		return;
	case 0xFC:
	case 0xFD:
	case 0xFE:
		ch->ended = 1;
		if (!dry) keyoff(s, c);
		return;
	case 0xFF:
		ch->wait = ch->len > 0 ? ch->len : 1;
		if (!dry) keyoff(s, c);
		return;
	default:
		return;
	}
}

static void fetch(msb_state *s, int c, int dry, int measure)
{
	msb_ch *ch = &s->ch[c];
	int guard = 0;
	while (guard++ < 256 && !ch->ended) {
		int b, a0;
		if (ch->pc < 0 || ch->pc >= (int)s->file.size()) {
			ch->ended = 1;
			return;
		}
		b = s->file[ch->pc++];
		if (b < 0xD0) {
			if (!fetchb(s, ch, &a0)) { ch->ended = 1; return; }
			ch->wait = ch->len > 0 ? ch->len : 8;
			play_note(s, c, b, a0, 0, dry);
			return;
		}
		if (b == 0xFF) {
			ch->wait = ch->len > 0 ? ch->len : 8;
			if (!dry) keyoff(s, c);
			return;
		}
		do_cmd(s, c, b, dry, measure);
	}
}

static void irq(msb_state *s, int dry, int measure)
{
	int i, live = 0;
	for (i = 0; i < MSB_CH; ++i) {
		msb_ch *ch = &s->ch[i];
		if (!ch->enabled || ch->ended) continue;
		live++;
		if (!ch->nogate && ch->gate > 0 && ch->wait <= ch->gate && ch->keyed && !dry)
			keyoff(s, i);
		if (--ch->wait <= 0)
			fetch(s, i, dry, measure);
	}
	if (!live) s->ended = 1;
}

static int ta_period(int word)
{
	int na = (word >> 2) & 0x3FF;
	int n = 1024 - na;
	if (n < 16) n = 16;
	return n;
}

static void reset_chip(msb_state *s)
{
	int i;
	if (!s->opna) return;
	s->opna->reset();
	s->opna->setfmvolume(32768);
	s->opna->setpsgvolume(fmgen_vol(-18.0));
	wr(s, 0x29, 0x80);
	wr(s, 0x27, 0x30);
	wr(s, 0x07, 0xB8);
	wr(s, 0x22, 0);
	for (i = 0; i < 3; ++i) wr(s, 0x28, (uint8_t)i);
	for (i = 4; i < 7; ++i) wr(s, 0x28, (uint8_t)i);
	s->ssg_mix = 0xB8;
}

static void reset_play(msb_state *s, int dry)
{
	int i;
	memset(s->ch, 0, sizeof s->ch);
	s->ended = 0;
	s->irq_acc = 0;
	s->chip_pos = 0;
	s->play_samples = 0;
	s->last_l = s->last_r = 0;
	s->ta_word = 0x200;
	s->ssg_mix = 0xB8;
	if (!dry) reset_chip(s);
	for (i = 0; i < MSB_CH; ++i) {
		if (s->part[i] <= 0) continue;
		s->ch[i].enabled = 1;
		s->ch[i].ssg = (i >= 3 && i <= 5);
		s->ch[i].ext = (i >= 6);
		s->ch[i].pc = s->part[i];
		s->ch[i].wait = 1;
		s->ch[i].len = 8;
		s->ch[i].vol = 0x60;
		s->ch[i].pan = 0xC0;
	}
}

static int parse_header(msb_state *s)
{
	const uint8_t *d = s->file.data();
	size_t n = s->file.size();
	int i, got = 0, off, nxt;
	if (n < 0x26) return -1;
	for (i = 0; i < MSB_CH; ++i) {
		off = rd16(d + i * 2);
		nxt = (i < 16) ? rd16(d + (i + 1) * 2) : (int)n;
		if (off < 0x22 || off >= (int)n) { s->part[i] = 0; continue; }
		if (nxt <= off + 2) { s->part[i] = 0; continue; }
		if (d[off] == 0xFC && nxt <= off + 2) { s->part[i] = 0; continue; }
		s->part[i] = off;
		got++;
	}
	return got > 0 ? 0 : -1;
}

static int measure_ms(msb_state *s)
{
	int ticks = 0;
	int64_t us = 0;
	reset_play(s, 1);
	while (!s->ended && ticks < MSB_MAX_TICKS) {
		us += (int64_t)72 * ta_period(s->ta_word);
		irq(s, 1, 1);
		ticks++;
	}
	{
		int ms = (int)((us * 1000) / (int64_t)MSB_CLOCK);
		if (ms < 800) ms = 800;
		if (ms > 12 * 60 * 1000) ms = 12 * 60 * 1000;
		return ms;
	}
}

static void fill_game(const char *filename, char *game, size_t gcap)
{
	if (filename && strstr(filename, "kirisima"))
		pc98_bounded(game, gcap, "Kirishima Shinryoushitsu");
}

static int setup(msb_state *s, const char *filename, const uint8_t *data,
		size_t len, const pc98_cfg *cfg)
{
	uint32_t sr;
	if (!data || !pc98_looks_msb(data, len)) return -1;
	s->file.resize(len);
	msb_deob(s->file.data(), data, len);
	if (parse_header(s) != 0) return -1;
	s->rate = cfg && cfg->rate > 0 ? cfg->rate : PC98_DEFAULT_RATE;
	s->loops_want = cfg && cfg->loop_count > 0 ? cfg->loop_count : 1;
	s->mute_fm = cfg ? cfg->mute_fm : 0;
	s->mute_ssg = cfg ? cfg->mute_ssg : 0;
	{
		char stem[256];
		pc98_basename(filename ? filename : "", stem, sizeof stem);
		pc98_bounded(s->title, sizeof s->title, stem[0] ? stem : "MSB");
	}
	fill_game(filename, s->game, sizeof s->game);
	s->one_loop_ms = measure_ms(s);
	s->opna = new ymfm::ym2608(s->iface);
	sr = s->opna->sample_rate(MSB_CLOCK);
	if (sr == 0) sr = 55467;
	s->chip_step = ((int64_t)sr << 16) / s->rate;
	s->play_limit = (uint32_t)(((int64_t)s->one_loop_ms * s->loops_want * s->rate) / 1000);
	if (s->play_limit < (uint32_t)s->rate) s->play_limit = (uint32_t)s->rate;
	reset_play(s, 0);
	return 0;
}

int msb_probe_mem(const uint8_t *data, size_t len)
{
	return pc98_looks_msb(data, len);
}

int msb_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out)
{
	msb_state tmp;
	if (!out) return -1;
	memset(out, 0, sizeof *out);
	if (setup(&tmp, filename, data, len, cfg) != 0) {
		delete tmp.opna;
		return -1;
	}
	out->kind = PC98_KIND_MSB;
	out->songs = 1;
	out->looping = 1;
	out->one_loop_ms[0] = tmp.one_loop_ms;
	pc98_bounded(out->title, sizeof out->title, tmp.title);
	pc98_bounded(out->game, sizeof out->game, tmp.game);
	pc98_bounded(out->filetype, sizeof out->filetype, "MSB");
	pc98_bounded(out->engine, sizeof out->engine, "ymfm MBMUS");
	delete tmp.opna;
	return 0;
}

void *msb_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg)
{
	msb_state *s = new msb_state();
	if (setup(s, filename, data, len, cfg) != 0) {
		delete s->opna;
		delete s;
		return NULL;
	}
	return s;
}

void msb_close_h(void *h)
{
	msb_state *s = (msb_state *)h;
	if (!s) return;
	delete s->opna;
	delete s;
}

int msb_process_h(void *h, float *buf, int count)
{
	msb_state *s = (msb_state *)h;
	int i;
	if (!s || !s->opna || !buf || count <= 0) return 0;
	for (i = 0; i < count; ++i) {
		int64_t need;
		if (s->play_limit && s->play_samples >= s->play_limit) {
			buf[i * 2] = 0;
			buf[i * 2 + 1] = 0;
			continue;
		}
		need = (int64_t)s->rate * 72 * ta_period(s->ta_word);
		s->irq_acc += (int64_t)MSB_CLOCK;
		while (s->irq_acc >= need) {
			irq(s, 0, 0);
			s->irq_acc -= need;
			need = (int64_t)s->rate * 72 * ta_period(s->ta_word);
		}
		s->chip_pos += s->chip_step;
		while (s->chip_pos >= 0x10000) {
			int fm_l = 0, fm_r = 0, ssg = 0;
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
		buf[i * 2] = (float)s->last_l / 32768.f;
		buf[i * 2 + 1] = (float)s->last_r / 32768.f;
		s->play_samples++;
	}
	return count;
}

int msb_seek_ms_h(void *h, int ms)
{
	msb_state *s = (msb_state *)h;
	uint32_t target;
	if (!s) return -1;
	if (ms < 0) ms = 0;
	reset_play(s, 0);
	target = (uint32_t)(((int64_t)ms * s->rate) / 1000);
	while (s->play_samples < target && !s->ended) {
		irq(s, 0, 0);
		s->play_samples += (uint32_t)(((int64_t)s->rate * 72 * ta_period(s->ta_word)
					+ MSB_CLOCK / 2) / MSB_CLOCK);
	}
	s->irq_acc = 0;
	return 0;
}

int msb_one_loop_ms_h(void *h)
{
	msb_state *s = (msb_state *)h;
	return s ? s->one_loop_ms : 0;
}

int msb_rate_h(void *h)
{
	msb_state *s = (msb_state *)h;
	return s ? s->rate : PC98_DEFAULT_RATE;
}

void msb_set_loops_h(void *h, int loops)
{
	msb_state *s = (msb_state *)h;
	if (!s) return;
	if (loops < 1) loops = 1;
	s->loops_want = loops;
	s->play_limit = (uint32_t)(((int64_t)s->one_loop_ms * s->loops_want * s->rate) / 1000);
}

void msb_apply_mute_h(void *h, const pc98_cfg *cfg)
{
	msb_state *s = (msb_state *)h;
	if (!s || !cfg) return;
	s->mute_fm = cfg->mute_fm;
	s->mute_ssg = cfg->mute_ssg;
}

const char *msb_title_h(void *h)
{
	msb_state *s = (msb_state *)h;
	return s ? s->title : "";
}

const char *msb_game_h(void *h)
{
	msb_state *s = (msb_state *)h;
	return s ? s->game : "";
}

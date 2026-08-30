/*
 * ArtDink Sekigahara .NTL (driver inside SEK2.EXE).
 * F_* = FM+SSG. G_* is GM/MIDI (see gntl_engine).
 * Patches live in the EXE, not the .NTL; we use a default YM voice.
 */
#include "ntl_engine.h"
#include "pc98_util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <vector>

#include "ymfm.h"
#include "ymfm_opn.h"

#define NTL_CH        9
#define NTL_CLOCK     7987200u
#define NTL_MAX_TICKS 400000

static const int k_fnum[12] = {
	0x269, 0x28E, 0x2B4, 0x2DE, 0x30A, 0x338,
	0x369, 0x39C, 0x3D3, 0x40E, 0x44B, 0x48D
};
static const uint8_t k_def_pat[25] = {
	0x31, 0x31, 0x31, 0x31, 0x14, 0x18, 0x14, 0x08,
	0x1F, 0x1F, 0x1F, 0x1F, 0x00, 0x05, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x07, 0x07, 0x07, 0x05, 0x3C
};

class ntl_iface : public ymfm::ymfm_interface {
public:
	uint8_t ymfm_external_read(ymfm::access_class, uint32_t) override { return 0; }
};

struct ntl_ch {
	int enabled, ended, ssg, conductor;
	int pc, start, end, wait, keyed, slur, oct, vol, detune, def_len;
	int loop_pc, loop_n, did_loop;
};

struct ntl_state {
	std::vector<uint8_t> file;
	int part[NTL_CH];
	int part_end[NTL_CH];
	int pssg[NTL_CH];
	int pcond[NTL_CH];
	ntl_ch ch[NTL_CH];
	ntl_iface iface;
	ymfm::ym2608 *opna;
	ymfm::ym2608::output_data out;
	int rate, loops_want, one_loop_ms, ended, tb;
	int mute_fm, mute_ssg;
	int64_t chip_pos, chip_step, irq_acc;
	int last_l, last_r;
	uint32_t play_samples, play_limit;
	uint8_t ssg_mix;
	char title[256], game[256];

	ntl_state()
		: opna(NULL), rate(PC98_DEFAULT_RATE), loops_want(1), one_loop_ms(0),
		  ended(0), tb(0xC0), mute_fm(0), mute_ssg(0),
		  chip_pos(0), chip_step(0), irq_acc(0), last_l(0), last_r(0),
		  play_samples(0), play_limit(0), ssg_mix(0xB8)
	{
		memset(part, 0, sizeof part);
		memset(part_end, 0, sizeof part_end);
		memset(pssg, 0, sizeof pssg);
		memset(pcond, 0, sizeof pcond);
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

static void wr(ntl_state *s, uint8_t aa, uint8_t dd)
{
	if (!s->opna) return;
	s->opna->write(0, aa);
	s->opna->write(1, dd);
}

static void wrx(ntl_state *s, int ext, uint8_t aa, uint8_t dd)
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

static void apply_def(ntl_state *s, int c, int vol)
{
	int ext = c >= 3;
	int slot = ext ? c - 3 : c;
	int i, atten;
	atten = (15 - (vol & 15)) * 4;
	for (i = 0; i < 24; ++i) {
		int v = k_def_pat[i];
		if (i >= 4 && i < 8 && (i == 5 || i == 7)) {
			v += atten;
			if (v > 127) v = 127;
		}
		wrx(s, ext, (uint8_t)(0x30 + slot + (i & 3) * 4 + (i / 4) * 16),
				(uint8_t)v);
	}
	wrx(s, ext, (uint8_t)(0xB0 + slot), k_def_pat[24]);
	wrx(s, ext, (uint8_t)(0xB4 + slot), 0xC0);
}

static int key_id(int c)
{
	return c >= 3 ? (c - 3 + 4) : c;
}

static void keyoff(ntl_state *s, int c)
{
	ntl_ch *ch = &s->ch[c];
	if (ch->ssg)
		wr(s, (uint8_t)(0x08 + (c >= 6 ? c - 6 : 0)), 0);
	else
		wr(s, 0x28, (uint8_t)key_id(c));
	ch->keyed = 0;
}

static void play_note(ntl_state *s, int c, int name, int dry)
{
	ntl_ch *ch = &s->ch[c];
	int n, oct, fn, blk;
	if (dry || ch->conductor) return;
	n = name - 0x40;
	if (n < 0) n = 0;
	oct = ch->oct;
	while (n > 11) { n -= 12; oct++; }
	if (oct < 0) oct = 0;
	if (oct > 7) oct = 7;
	if (ch->ssg) {
		int per, ssgc, i;
		per = 0xEE8;
		if (n < 12) per = (k_fnum[n] << 2);
		for (i = 0; i < oct; ++i) per >>= 1;
		per += ch->detune;
		if (per < 1) per = 1;
		if (per > 0xFFF) per = 0xFFF;
		ssgc = c >= 6 ? c - 6 : 0;
		wr(s, (uint8_t)(ssgc * 2), (uint8_t)per);
		wr(s, (uint8_t)(ssgc * 2 + 1), (uint8_t)(per >> 8));
		wr(s, (uint8_t)(0x08 + ssgc), (uint8_t)(ch->vol & 15));
		s->ssg_mix &= (uint8_t)~(1 << ssgc);
		wr(s, 0x07, s->ssg_mix);
		ch->keyed = 1;
		return;
	}
	fn = k_fnum[n < 12 ? n : 0] + ch->detune;
	if (fn < 0) fn = 0;
	if (fn > 0x7FF) fn = 0x7FF;
	blk = oct << 3;
	{
		int ext = c >= 3;
		int slot = ext ? c - 3 : c;
		if (ch->keyed && !ch->slur) keyoff(s, c);
		wrx(s, ext, (uint8_t)(0xA4 + slot), (uint8_t)(blk | ((fn >> 8) & 7)));
		wrx(s, ext, (uint8_t)(0xA0 + slot), (uint8_t)fn);
		if (!ch->slur || !ch->keyed)
			wr(s, 0x28, (uint8_t)(0xF0 | key_id(c)));
	}
	ch->keyed = 1;
	ch->slur = 0;
}

static int fetchb(ntl_state *s, ntl_ch *ch, int *out)
{
	if (ch->pc < 0 || ch->pc >= (int)s->file.size()) return 0;
	*out = s->file[ch->pc++];
	return 1;
}

static void do_cmd(ntl_state *s, int c, int cmd, int dry, int measure)
{
	ntl_ch *ch = &s->ch[c];
	int a = 0, b = 0;
	switch (cmd) {
	case 0x80:
		return;
	case 0x81:
		if (fetchb(s, ch, &a)) {
			ch->def_len = a > 0 ? a : 1;
			ch->wait = ch->def_len;
		}
		return;
	case 0x82:
		ch->slur = 1;
		return;
	case 0x83:
		if (fetchb(s, ch, &a))
			ch->detune = (int)a - 0x20;
		return;
	case 0x84:
		if (fetchb(s, ch, &a)) {
			ch->vol = a & 15;
			if (!dry && !ch->ssg && !ch->conductor)
				apply_def(s, c, ch->vol);
		}
		return;
	case 0x85:
		if (fetchb(s, ch, &a) && a > 0 && ch->pc + a <= (int)s->file.size())
			ch->pc += a;
		return;
	case 0x86:
		if (fetchb(s, ch, &a))
			ch->oct = a & 7;
		return;
	case 0x87:
	case 0x88:
	case 0x8F:
	case 0x91:
	case 0x92:
		fetchb(s, ch, &a);
		return;
	case 0x8A:
		if (!fetchb(s, ch, &a) || !fetchb(s, ch, &b)) return;
		if (a > 1) {
			ch->loop_pc = ch->pc;
			ch->loop_n = a;
		} else if (ch->loop_n > 1) {
			ch->loop_n--;
			if (ch->loop_n > 0) {
				if (measure) {
					ch->did_loop = 1;
					ch->ended = 1;
					return;
				}
				if (ch->loop_pc > 0)
					ch->pc = ch->loop_pc;
			}
		}
		return;
	case 0x96:
		if (fetchb(s, ch, &a))
			s->tb = a & 0xFF;
		return;
	case 0x9C:
	case 0x9D:
		return;
	default:
		return;
	}
}

static void fetch(ntl_state *s, int c, int dry, int measure)
{
	ntl_ch *ch = &s->ch[c];
	int guard = 0;
	while (guard++ < 256 && !ch->ended) {
		int b, ln;
		if (ch->pc < 0 || ch->pc >= (int)s->file.size() ||
				(ch->end > 0 && ch->pc >= ch->end)) {
			if (measure) {
				ch->did_loop = 1;
				ch->ended = 1;
				return;
			}
			if (ch->start > 0) {
				ch->pc = ch->start;
				continue;
			}
			ch->ended = 1;
			return;
		}
		b = s->file[ch->pc++];
		if (b < 0x80) {
			if (!fetchb(s, ch, &ln)) { ch->ended = 1; return; }
			if (ln < 1) ln = 1;
			ch->wait = ln;
			ch->def_len = ln;
			play_note(s, c, b, dry);
			return;
		}
		do_cmd(s, c, b, dry, measure);
		if (ch->wait > 0 && b == 0x81)
			return;
	}
}

static void irq(ntl_state *s, int dry, int measure)
{
	int i, live = 0;
	for (i = 0; i < NTL_CH; ++i) {
		ntl_ch *ch = &s->ch[i];
		if (!ch->enabled || ch->ended) continue;
		live++;
		if (--ch->wait <= 0)
			fetch(s, i, dry, measure);
	}
	if (!live) s->ended = 1;
}

static int tb_clocks(int tb)
{
	int n = 256 - (tb & 0xFF);
	if (n < 1) n = 1;
	return 1152 * n;
}

static void reset_chip(ntl_state *s)
{
	int i;
	if (!s->opna) return;
	s->opna->reset();
	s->opna->setfmvolume(32768);
	s->opna->setpsgvolume(fmgen_vol(-18.0));
	wr(s, 0x29, 0x80);
	wr(s, 0x27, 0x30);
	wr(s, 0x07, 0xB8);
	for (i = 0; i < 3; ++i) wr(s, 0x28, (uint8_t)i);
	for (i = 4; i < 7; ++i) wr(s, 0x28, (uint8_t)i);
	s->ssg_mix = 0xB8;
}

static int parse_header(ntl_state *s)
{
	const uint8_t *d = s->file.data();
	size_t n = s->file.size();
	int cnt, i, j, off, typ, fm = 0, ssg = 0, cond = 0, p, te;
	int offs[64], noff = 0;
	if (n < 16 || !pc98_looks_ntl(d, n)) return -1;
	cnt = d[3];
	memset(s->part, 0, sizeof s->part);
	memset(s->part_end, 0, sizeof s->part_end);
	for (i = 0; i < cnt && 4 + (i + 1) * 3 <= (int)n; ++i) {
		off = (int)rd16(d + 4 + i * 3);
		typ = d[4 + i * 3 + 2];
		if (off > 0 && off < (int)n && noff < 64)
			offs[noff++] = off;
		if (off <= 0 || off >= (int)n) continue;
		if (typ >= 0x10 && typ <= 0x15 && fm < 6) {
			s->part[fm] = off;
			s->pssg[fm] = 0;
			fm++;
		} else if (typ >= 0x20 && typ <= 0x22 && ssg < 3) {
			s->part[6 + ssg] = off;
			s->pssg[6 + ssg] = 1;
			ssg++;
		} else if (typ >= 0x90 && typ <= 0x95 && cond < 1) {
			s->part[5] = off;
			s->pcond[5] = 1;
			cond++;
		}
	}
	for (i = 0; i < NTL_CH; ++i) {
		int nxt = (int)n;
		if (s->part[i] <= 0) continue;
		for (j = 0; j < noff; ++j) {
			if (offs[j] > s->part[i] && offs[j] < nxt)
				nxt = offs[j];
		}
		s->part_end[i] = nxt;
	}
	/* title after table: prefer an ASCII run, else a 0-terminated SJIS string */
	te = 4 + cnt * 3;
	p = te;
	while (p + 4 < (int)n && p < te + 48) {
		if (d[p] >= 'A' && d[p] < 0x7F) {
			char tmp[256];
			int o = 0;
			while (p < (int)n && d[p] >= 0x20 && d[p] < 0x7F && o < 250)
				tmp[o++] = (char)d[p++];
			tmp[o] = 0;
			if (o >= 4)
				pc98_bounded(s->title, sizeof s->title, tmp);
			break;
		}
		p++;
	}
	if (!s->title[0]) {
		p = te;
		while (p < (int)n && p < te + 16 && d[p] < 0x20) p++;
		if (p < (int)n && d[p] >= 0x80) {
			char tmp[256];
			int o = 0;
			while (p < (int)n && d[p] && o < 250)
				tmp[o++] = (char)d[p++];
			tmp[o] = 0;
			if (tmp[0]) pc98_bounded(s->title, sizeof s->title, tmp);
		}
	}
	return (fm + ssg) > 0 ? 0 : -1;
}

static void reset_play(ntl_state *s, int dry)
{
	int i;
	memset(s->ch, 0, sizeof s->ch);
	s->ended = 0;
	s->irq_acc = 0;
	s->chip_pos = 0;
	s->play_samples = 0;
	s->last_l = s->last_r = 0;
	s->tb = 0xC0;
	s->ssg_mix = 0xB8;
	if (!dry) reset_chip(s);
	for (i = 0; i < NTL_CH; ++i) {
		if (s->part[i] <= 0) continue;
		s->ch[i].enabled = 1;
		s->ch[i].ssg = s->pssg[i];
		s->ch[i].conductor = s->pcond[i];
		s->ch[i].pc = s->part[i];
		s->ch[i].start = s->part[i];
		s->ch[i].end = s->part_end[i];
		s->ch[i].wait = 1;
		s->ch[i].oct = 4;
		s->ch[i].vol = 10;
		s->ch[i].def_len = 12;
		if (!s->ch[i].ssg && !s->ch[i].conductor && !dry)
			apply_def(s, i, s->ch[i].vol);
	}
}

static int measure_ms(ntl_state *s)
{
	int ticks = 0;
	int64_t us = 0;
	reset_play(s, 1);
	while (!s->ended && ticks < NTL_MAX_TICKS) {
		us += (int64_t)tb_clocks(s->tb);
		irq(s, 1, 1);
		ticks++;
	}
	{
		int ms = (int)((us * 1000) / (int64_t)NTL_CLOCK);
		if (ms < 800) ms = 800;
		if (ms > 12 * 60 * 1000) ms = 12 * 60 * 1000;
		return ms;
	}
}

static void fill_game(const char *filename, char *game, size_t gcap)
{
	if (filename && (strstr(filename, "sekiga") || strstr(filename, "SEKIGA")))
		pc98_bounded(game, gcap, "Sekigahara");
}

static int setup(ntl_state *s, const char *filename, const uint8_t *data,
		size_t len, const pc98_cfg *cfg)
{
	uint32_t sr;
	if (!data || !pc98_looks_ntl(data, len)) return -1;
	s->file.assign(data, data + len);
	if (parse_header(s) != 0) return -1;
	s->rate = cfg && cfg->rate > 0 ? cfg->rate : PC98_DEFAULT_RATE;
	s->loops_want = cfg && cfg->loop_count > 0 ? cfg->loop_count : 1;
	s->mute_fm = cfg ? cfg->mute_fm : 0;
	s->mute_ssg = cfg ? cfg->mute_ssg : 0;
	if (!s->title[0]) {
		char stem[256];
		pc98_basename(filename ? filename : "", stem, sizeof stem);
		pc98_bounded(s->title, sizeof s->title, stem[0] ? stem : "NTL");
	}
	fill_game(filename, s->game, sizeof s->game);
	s->one_loop_ms = measure_ms(s);
	s->opna = new ymfm::ym2608(s->iface);
	sr = s->opna->sample_rate(NTL_CLOCK);
	if (sr == 0) sr = 55467;
	s->chip_step = ((int64_t)sr << 16) / s->rate;
	s->play_limit = (uint32_t)(((int64_t)s->one_loop_ms * s->loops_want * s->rate) / 1000);
	if (s->play_limit < (uint32_t)s->rate) s->play_limit = (uint32_t)s->rate;
	reset_play(s, 0);
	return 0;
}

int ntl_probe_mem(const uint8_t *data, size_t len)
{
	return pc98_looks_ntl(data, len);
}

int ntl_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out)
{
	ntl_state tmp;
	if (!out) return -1;
	memset(out, 0, sizeof *out);
	if (setup(&tmp, filename, data, len, cfg) != 0) {
		delete tmp.opna;
		return -1;
	}
	out->kind = PC98_KIND_NTL;
	out->songs = 1;
	out->looping = 1;
	out->one_loop_ms[0] = tmp.one_loop_ms;
	pc98_bounded(out->title, sizeof out->title, tmp.title);
	pc98_bounded(out->game, sizeof out->game, tmp.game);
	pc98_bounded(out->filetype, sizeof out->filetype, "NTL");
	pc98_bounded(out->engine, sizeof out->engine, "ymfm ArtDink NTL");
	delete tmp.opna;
	return 0;
}

void *ntl_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg)
{
	ntl_state *s = new ntl_state();
	if (setup(s, filename, data, len, cfg) != 0) {
		delete s->opna;
		delete s;
		return NULL;
	}
	return s;
}

void ntl_close_h(void *h)
{
	ntl_state *s = (ntl_state *)h;
	if (!s) return;
	delete s->opna;
	delete s;
}

int ntl_process_h(void *h, float *buf, int count)
{
	ntl_state *s = (ntl_state *)h;
	int i;
	if (!s || !s->opna || !buf || count <= 0) return 0;
	for (i = 0; i < count; ++i) {
		int64_t need;
		if (s->play_limit && s->play_samples >= s->play_limit) {
			buf[i * 2] = 0;
			buf[i * 2 + 1] = 0;
			continue;
		}
		need = (int64_t)s->rate * tb_clocks(s->tb);
		s->irq_acc += (int64_t)NTL_CLOCK;
		while (s->irq_acc >= need) {
			irq(s, 0, 0);
			s->irq_acc -= need;
			need = (int64_t)s->rate * tb_clocks(s->tb);
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

int ntl_seek_ms_h(void *h, int ms)
{
	ntl_state *s = (ntl_state *)h;
	uint32_t target;
	if (!s) return -1;
	if (ms < 0) ms = 0;
	reset_play(s, 0);
	target = (uint32_t)(((int64_t)ms * s->rate) / 1000);
	while (s->play_samples < target && !s->ended) {
		irq(s, 0, 0);
		s->play_samples += (uint32_t)(((int64_t)s->rate * tb_clocks(s->tb) +
					NTL_CLOCK / 2) / NTL_CLOCK);
	}
	s->irq_acc = 0;
	return 0;
}

int ntl_one_loop_ms_h(void *h)
{
	ntl_state *s = (ntl_state *)h;
	return s ? s->one_loop_ms : 0;
}

int ntl_rate_h(void *h)
{
	ntl_state *s = (ntl_state *)h;
	return s ? s->rate : PC98_DEFAULT_RATE;
}

void ntl_set_loops_h(void *h, int loops)
{
	ntl_state *s = (ntl_state *)h;
	if (!s) return;
	if (loops < 1) loops = 1;
	s->loops_want = loops;
	s->play_limit = (uint32_t)(((int64_t)s->one_loop_ms * s->loops_want * s->rate) / 1000);
}

void ntl_apply_mute_h(void *h, const pc98_cfg *cfg)
{
	ntl_state *s = (ntl_state *)h;
	if (!s || !cfg) return;
	s->mute_fm = cfg->mute_fm;
	s->mute_ssg = cfg->mute_ssg;
}

const char *ntl_title_h(void *h)
{
	ntl_state *s = (ntl_state *)h;
	return s ? s->title : "";
}

const char *ntl_game_h(void *h)
{
	ntl_state *s = (ntl_state *)h;
	return s ? s->game : "";
}

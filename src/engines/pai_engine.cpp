/*
 * Synthia 3.00 (FM3+PSG3) — Music Driver 『しんしあ』 / Y. Yamada.
 * .PAI "PAI 3.00M" songs (k_chojin_98, guernica_98, …).
 *
 * Header: 10-byte magic, word trailer @0x0A, 11 part offsets @0x0C,
 * instrument pointer @0x2A. Driver loads FM1–3 and SSG1–3 (skips FM4–6).
 * Voices are 26-byte records {id, 25 PMD-style ops}, selected by F2.
 * Tempo is Timer B (FF), L is EC, 80 jumps to L. EE is a self-modifying
 * repeat. Sequence is PMD-like (note<0x80 + length) but command sizes
 * are not PMD's — do not feed these files to PMDWin.
 */
#include "pai_engine.h"
#include "pc98_util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <vector>

#include "ymfm.h"
#include "ymfm_opn.h"

#define PAI_CH        6
#define PAI_CLOCK     7987200u
#define PAI_MAX_TICKS 400000
#define PAI_CAP_MS    (12 * 60 * 1000)

static const uint16_t k_fnum[16] = {
	0x026A, 0x028F, 0x02B6, 0x02DF, 0x030B, 0x0339, 0x036A, 0x039E,
	0x03D5, 0x0410, 0x044E, 0x048F, 0x0EE8, 0x0E12, 0x0D48, 0x0C89
};

static const uint16_t k_ssg[16] = {
	0x0EE8, 0x0E12, 0x0D48, 0x0C89, 0x0BD5, 0x0B2B, 0x0A8A, 0x09F3,
	0x0964, 0x08DD, 0x085E, 0x07E6, 0x8080, 0x8080, 0xE0A0, 0xF0E0
};

/* S20S_4.BIN 0x38A — which TLs take volume (bits 7..4 = C2,C1,M2,M1). */
static const uint8_t k_car[8] = {
	0x80, 0x80, 0x80, 0x80, 0xA0, 0xE0, 0xE0, 0xF0
};

/*
 * Extra bytes after a command. Index = cmd - 0x80.
 * From S20S_4.BIN tables 0x1542 (FM) and 0x15B2 (SSG). EE is special.
 */
static const uint8_t k_fm_sz[128] = {
	1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
	1,3,3,1,1,1,1,1, 1,1,2,1,1,1,1,2,
	1,1,1,1,1,1,1,1, 2,5,4,1,1,2,4,1,
	1,1,1,1,0,2,4,2, 0,6,1,2,2,1,1,1,
	0,1,1,0,1,1,1,1, 1,3,3,1,1,1,1,1,
	1,1,2,1,1,1,1,1, 1,2,1,1,2,1,1,1,
	2,5,4,1,1,2,4,1, 1,1,1,1,0,2,4,2,
	0,6,1,2,2,1,1,1, 0,1,1,0,1,1,1,1
};

static const uint8_t k_ssg_sz[128] = {
	1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1, 1,3,3,1,1,1,1,1,
	1,1,2,1,1,1,1,2, 1,1,1,1,1,1,1,1,
	2,5,4,1,1,2,4,1, 1,1,1,1,0,2,4,2,
	0,6,1,2,2,1,1,1, 0,1,1,0,1,1,1,1
};

static const uint8_t k_def_pat[25] = {
	0x31, 0x31, 0x31, 0x31,
	0x14, 0x14, 0x14, 0x14,
	0x1F, 0x1F, 0x1F, 0x1F,
	0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
	0x05, 0x05, 0x05, 0x05,
	0x3C
};

class pai_iface : public ymfm::ymfm_interface {
public:
	uint8_t ymfm_external_read(ymfm::access_class, uint32_t) override { return 0; }
};

struct pai_ch {
	int enabled;
	int ended;
	int ssg;
	int pc;
	int wait;
	int gate;
	int loop_pc;
	int did_loop;
	int vol;
	int q, q2;
	int inst;
	int transpose;
	int detune;
	int tie;
	int keyed;
	int alg;
	uint8_t tl[4];
	uint8_t pan;
};

struct pai_state {
	std::vector<uint8_t> orig;
	std::vector<uint8_t> file;
	int part[PAI_CH];
	int inst_off;
	int inst_end;
	pai_ch ch[PAI_CH];
	pai_iface iface;
	ymfm::ym2608 *opna;
	ymfm::ym2608::output_data out;
	int rate;
	int loops_want;
	int one_loop_ms;
	int ended;
	int tb;
	int mute_fm, mute_ssg;
	int64_t chip_pos, chip_step;
	int64_t irq_acc;
	int last_l, last_r;
	uint32_t play_samples, play_limit;
	uint8_t ssg_mix;
	char title[256];
	char game[256];

	pai_state()
		: inst_off(0), inst_end(0), opna(NULL), rate(PC98_DEFAULT_RATE),
		  loops_want(1), one_loop_ms(0), ended(0), tb(0xC8),
		  mute_fm(0), mute_ssg(0), chip_pos(0), chip_step(0), irq_acc(0),
		  last_l(0), last_r(0), play_samples(0), play_limit(0), ssg_mix(0xB8)
	{
		memset(part, 0, sizeof part);
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

static void title_from_path(char *dst, size_t cap, const char *filename)
{
	char stem[256];
	pc98_basename(filename ? filename : "", stem, sizeof stem);
	pc98_bounded(dst, cap, stem[0] ? stem : "PAI");
}

static void fill_known_game(const char *filename, char *game, size_t gcap)
{
	if (!filename || !game) return;
	if (strstr(filename, "k_chojin_98") || strstr(filename, "K_CHOJIN"))
		pc98_bounded(game, gcap, "Kousoku Choujin");
	else if (strstr(filename, "guernica_98") || strstr(filename, "GUERNICA"))
		pc98_bounded(game, gcap, "Guernica");
}

static void wr(pai_state *s, uint8_t aa, uint8_t dd)
{
	if (!s->opna) return;
	s->opna->write(0, aa);
	s->opna->write(1, dd);
}

static int tb_period(int tb)
{
	int n = 256 - (tb & 0xFF);
	if (n < 1) n = 1;
	return n;
}

static int carriers(int alg)
{
	uint8_t m = k_car[alg & 7];
	int r = 0;
	if (m & 0x80) r |= 8;
	if (m & 0x40) r |= 2;
	if (m & 0x20) r |= 4;
	if (m & 0x10) r |= 1;
	return r;
}

static const uint8_t *find_voice(pai_state *s, int id)
{
	int p, cap;
	if (s->inst_off <= 0) return NULL;
	p = s->inst_off;
	cap = 0;
	while (p + 26 <= (int)s->file.size() && p < s->inst_end && cap++ < 256) {
		if (s->file[p] == (uint8_t)id)
			return &s->file[p + 1];
		p += 26;
	}
	return NULL;
}

static void apply_tl(pai_state *s, int c)
{
	pai_ch *ch = &s->ch[c];
	int atten, mask, i;
	int slot[4] = { 0, 1, 2, 3 };
	if (ch->ssg || !s->opna) return;
	atten = (~ch->vol) & 0x7F;
	mask = carriers(ch->alg);
	for (i = 0; i < 4; ++i) {
		int tl = ch->tl[slot[i]];
		if (mask & (1 << i)) {
			tl += atten;
			if (tl > 127) tl = 127;
		}
		wr(s, (uint8_t)(0x40 + c + i * 4), (uint8_t)tl);
	}
}

static void apply_voice(pai_state *s, int c, int id)
{
	pai_ch *ch = &s->ch[c];
	const uint8_t *pat;
	int i;
	if (ch->ssg) return;
	ch->inst = id;
	pat = (id >= 0) ? find_voice(s, id) : NULL;
	if (!pat) pat = k_def_pat;
	for (i = 0; i < 24; ++i)
		wr(s, (uint8_t)(0x30 + c + (i & 3) * 4 + (i / 4) * 16), pat[i]);
	ch->alg = pat[24] & 7;
	wr(s, (uint8_t)(0xB0 + c), pat[24]);
	wr(s, (uint8_t)(0xB4 + c), ch->pan);
	ch->tl[0] = pat[4];
	ch->tl[1] = pat[5];
	ch->tl[2] = pat[6];
	ch->tl[3] = pat[7];
	apply_tl(s, c);
}

static void keyoff_fm(pai_state *s, int c)
{
	wr(s, 0x28, (uint8_t)c);
	s->ch[c].keyed = 0;
}

static void keyon_fm(pai_state *s, int c)
{
	wr(s, 0x28, (uint8_t)(0xF0 | c));
	s->ch[c].keyed = 1;
}

static void ssg_tone(pai_state *s, int c, int on)
{
	int bit = 1 << (c - 3);
	if (on) s->ssg_mix &= (uint8_t)~bit;
	else s->ssg_mix |= (uint8_t)bit;
	wr(s, 0x07, s->ssg_mix);
}

static void keyoff_ch(pai_state *s, int c)
{
	pai_ch *ch = &s->ch[c];
	if (ch->ssg) {
		wr(s, (uint8_t)(0x08 + (c - 3)), 0);
		ssg_tone(s, c, 0);
	} else {
		keyoff_fm(s, c);
	}
	ch->keyed = 0;
}

static int apply_transpose(int note, int tr)
{
	int oct, n;
	if ((note & 0x0F) == 0x0F) return note;
	oct = (note >> 4) & 0x0F;
	n = note & 0x0F;
	n += tr;
	while (n < 0) { n += 12; oct--; }
	while (n >= 12) { n -= 12; oct++; }
	if (oct < 0) oct = 0;
	if (oct > 7) oct = 7;
	if (n < 0) n = 0;
	if (n > 14) n = 14;
	return (oct << 4) | n;
}

static void play_note(pai_state *s, int c, int note, int dry)
{
	pai_ch *ch = &s->ch[c];
	int nn, rest;
	nn = apply_transpose(note, ch->transpose);
	rest = (nn & 0x0F) == 0x0F;
	if (ch->tie) {
		ch->tie = 0;
		if (!rest) return;
	}
	if (dry) return;
	if (rest) {
		keyoff_ch(s, c);
		return;
	}
	if (ch->ssg) {
		int ssgc = c - 3;
		int oct = (nn >> 4) & 0x0F;
		int per = k_ssg[nn & 0x0F];
		int vol;
		if (oct > 15) oct = 15;
		per >>= oct;
		if (per < 1) per = 1;
		wr(s, (uint8_t)(ssgc * 2), (uint8_t)per);
		wr(s, (uint8_t)(ssgc * 2 + 1), (uint8_t)(per >> 8));
		vol = ch->vol;
		if (vol > 15) vol = 15;
		if (vol < 0) vol = 0;
		wr(s, (uint8_t)(0x08 + ssgc), (uint8_t)vol);
		ssg_tone(s, c, 1);
		ch->keyed = 1;
	} else {
		int oct = (nn >> 4) & 0x0F;
		int fn = k_fnum[nn & 0x0F] + ch->detune;
		int blk;
		if (fn < 0) fn = 0;
		if (fn > 0x7FF) fn = 0x7FF;
		blk = (oct & 7) << 3;
		if (ch->keyed) keyoff_fm(s, c);
		apply_tl(s, c);
		wr(s, (uint8_t)(0xA4 + c), (uint8_t)(blk | ((fn >> 8) & 7)));
		wr(s, (uint8_t)(0xA0 + c), (uint8_t)fn);
		keyon_fm(s, c);
	}
}

static int fetch_byte(pai_state *s, pai_ch *ch, int *out)
{
	if (ch->pc < 0 || ch->pc >= (int)s->file.size()) return 0;
	*out = s->file[ch->pc++];
	return 1;
}

static int cmd_size(const pai_ch *ch, int cmd)
{
	if (cmd < 0x80 || cmd > 0xFF) return 1;
	return ch->ssg ? k_ssg_sz[cmd - 0x80] : k_fm_sz[cmd - 0x80];
}

static void do_ee(pai_state *s, pai_ch *ch)
{
	int cnt, cur, off;
	if (!fetch_byte(s, ch, &cnt)) { ch->ended = 1; return; }
	if (cnt == 0) {
		if (ch->pc < (int)s->file.size()) ch->pc++;
		if (ch->pc + 1 >= (int)s->file.size()) { ch->ended = 1; return; }
		off = rd16(&s->file[ch->pc]);
		ch->pc = off + 2;
		return;
	}
	if (ch->pc >= (int)s->file.size()) { ch->ended = 1; return; }
	s->file[ch->pc] = (uint8_t)((s->file[ch->pc] + 1) & 0xFF);
	cur = s->file[ch->pc++];
	if (ch->pc + 1 >= (int)s->file.size()) { ch->ended = 1; return; }
	off = rd16(&s->file[ch->pc]);
	ch->pc += 2;
	if (cur != cnt)
		ch->pc = off + 2;
}

static void do_cmd(pai_state *s, int c, int cmd, int dry)
{
	pai_ch *ch = &s->ch[c];
	int a = 0, b = 0;
	switch (cmd) {
	case 0xFF:
		if (fetch_byte(s, ch, &a))
			s->tb = a & 0xFF;
		return;
	case 0xFE:
		if (fetch_byte(s, ch, &a)) ch->q = a;
		return;
	case 0xFD:
		if (fetch_byte(s, ch, &a)) ch->q2 = a;
		return;
	case 0xFC:
		if (fetch_byte(s, ch, &a)) {
			ch->vol = a;
			if (!ch->ssg && !dry) apply_tl(s, c);
		}
		return;
	case 0xFB:
		ch->vol += ch->ssg ? 1 : 4;
		if (ch->vol > (ch->ssg ? 15 : 127)) ch->vol = ch->ssg ? 15 : 127;
		if (!ch->ssg && !dry) apply_tl(s, c);
		return;
	case 0xFA:
		if (fetch_byte(s, ch, &a)) {
			ch->vol += a;
			if (ch->vol > (ch->ssg ? 15 : 127)) ch->vol = ch->ssg ? 15 : 127;
			if (!ch->ssg && !dry) apply_tl(s, c);
		}
		return;
	case 0xF8:
		ch->vol -= ch->ssg ? 1 : 4;
		if (ch->vol < 0) ch->vol = 0;
		if (!ch->ssg && !dry) apply_tl(s, c);
		return;
	case 0xF7:
		if (fetch_byte(s, ch, &a)) {
			ch->vol -= a;
			if (ch->vol < 0) ch->vol = 0;
			if (!ch->ssg && !dry) apply_tl(s, c);
		}
		return;
	case 0xF4:
		if (ch->pc + 1 < (int)s->file.size()) {
			ch->detune = (int16_t)rd16(&s->file[ch->pc]);
			ch->pc += 2;
		}
		return;
	case 0xF3:
		if (ch->pc + 1 < (int)s->file.size()) {
			ch->detune += (int16_t)rd16(&s->file[ch->pc]);
			ch->pc += 2;
		}
		return;
	case 0xF2:
		if (fetch_byte(s, ch, &a) && !ch->ssg && !dry)
			apply_voice(s, c, a);
		return;
	case 0xF0:
		ch->tie = 1;
		return;
	case 0xEE:
		do_ee(s, ch);
		return;
	case 0xEC:
		ch->loop_pc = ch->pc;
		return;
	case 0xEB:
		if (fetch_byte(s, ch, &a)) ch->transpose = (int8_t)a;
		return;
	case 0xEA:
		if (fetch_byte(s, ch, &a)) ch->transpose += (int8_t)a;
		return;
	case 0xE0:
		if (ch->pc + 1 < (int)s->file.size()) {
			a = s->file[ch->pc++];
			b = s->file[ch->pc++];
			if (!dry) wr(s, (uint8_t)b, (uint8_t)a);
		}
		return;
	case 0xD8:
		if (fetch_byte(s, ch, &a) && !ch->ssg) {
			ch->pan = (uint8_t)((ch->pan & 0x3F) | ((a << 6) & 0xC0));
			if (!dry) wr(s, (uint8_t)(0xB4 + c), ch->pan);
		}
		return;
	default:
		ch->pc += cmd_size(ch, cmd);
		return;
	}
}

static void fetch(pai_state *s, int c, int dry, int measure)
{
	pai_ch *ch = &s->ch[c];
	int guard = 0;
	while (guard++ < 256 && !ch->ended) {
		int b, ln;
		if (ch->pc < 0 || ch->pc >= (int)s->file.size()) {
			ch->ended = 1;
			return;
		}
		b = s->file[ch->pc++];
		if (b < 0x80) {
			if (!fetch_byte(s, ch, &ln)) { ch->ended = 1; return; }
			if (ln < 1) ln = 1;
			ch->wait = ln;
			if (ch->q2)
				ch->gate = ((ln * ch->q2) >> 8) + ch->q;
			else
				ch->gate = ch->q;
			play_note(s, c, b, dry);
			return;
		}
		if (b == 0x80) {
			ch->pc--;
			if (ch->loop_pc > 0) {
				ch->pc = ch->loop_pc;
				if (measure) {
					ch->did_loop = 1;
					ch->ended = 1;
					return;
				}
				continue;
			}
			if (!dry) keyoff_ch(s, c);
			ch->ended = 1;
			return;
		}
		do_cmd(s, c, b, dry);
	}
}

static void irq(pai_state *s, int dry, int measure)
{
	int i, live = 0;
	for (i = 0; i < PAI_CH; ++i) {
		pai_ch *ch = &s->ch[i];
		if (!ch->enabled || ch->ended) continue;
		live++;
		if (ch->gate > 0 && ch->wait <= ch->gate && ch->keyed && !dry)
			keyoff_ch(s, i);
		if (--ch->wait <= 0)
			fetch(s, i, dry, measure);
	}
	if (!live) s->ended = 1;
}

static void reset_chip(pai_state *s)
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
	s->ssg_mix = 0xB8;
}

static void reset_play(pai_state *s, int dry)
{
	int i;
	s->file = s->orig;
	memset(s->ch, 0, sizeof s->ch);
	s->tb = 0xC8;
	s->ended = 0;
	s->irq_acc = 0;
	s->chip_pos = 0;
	s->play_samples = 0;
	s->last_l = s->last_r = 0;
	s->ssg_mix = 0xB8;
	if (!dry) reset_chip(s);
	for (i = 0; i < PAI_CH; ++i) {
		if (s->part[i] <= 0) continue;
		s->ch[i].enabled = 1;
		s->ch[i].ssg = (i >= 3);
		s->ch[i].pc = s->part[i];
		s->ch[i].wait = 1;
		s->ch[i].vol = s->ch[i].ssg ? 7 : 0x6C;
		s->ch[i].pan = 0xC0;
		s->ch[i].alg = 4;
		if (!s->ch[i].ssg && !dry)
			apply_voice(s, i, -1);
	}
}

static int parse_header(pai_state *s)
{
	const uint8_t *d = s->orig.data();
	size_t n = s->orig.size();
	int i, got = 0, off, trailer;
	int src[6] = { 0, 1, 2, 6, 7, 8 };
	if (n < 0x2C || !pc98_looks_pai(d, n)) return -1;
	trailer = rd16(d + 0x0A);
	s->inst_off = rd16(d + 0x2A);
	if (s->inst_off <= 0 || s->inst_off >= (int)n)
		s->inst_off = rd16(d + 0x28);
	s->inst_end = (trailer > s->inst_off && trailer <= (int)n) ? trailer : (int)n;
	memset(s->part, 0, sizeof s->part);
	for (i = 0; i < 6; ++i) {
		off = rd16(d + 0x0C + src[i] * 2);
		if (off <= 0 || off >= (int)n) continue;
		if (d[off] == 0x80) continue;
		s->part[i] = off;
		got++;
	}
	return got > 0 ? 0 : -1;
}

static int measure_ms(pai_state *s)
{
	int ticks = 0;
	int64_t us = 0;
	reset_play(s, 1);
	while (!s->ended && ticks < PAI_MAX_TICKS) {
		us += (int64_t)1152 * tb_period(s->tb);
		irq(s, 1, 1);
		ticks++;
	}
	{
		int ms = (int)((us * 1000) / (int64_t)PAI_CLOCK);
		if (ms < 500) ms = 500;
		if (ms > PAI_CAP_MS) ms = PAI_CAP_MS;
		return ms;
	}
}

static int setup(pai_state *s, const char *filename, const uint8_t *data,
		size_t len, const pc98_cfg *cfg)
{
	uint32_t sr;
	if (!data || !pc98_looks_pai(data, len) || len < 0x2C)
		return -1;
	s->orig.assign(data, data + len);
	s->file = s->orig;
	if (parse_header(s) != 0) return -1;
	s->rate = cfg && cfg->rate > 0 ? cfg->rate : PC98_DEFAULT_RATE;
	s->loops_want = cfg && cfg->loop_count > 0 ? cfg->loop_count : 1;
	s->mute_fm = cfg ? cfg->mute_fm : 0;
	s->mute_ssg = cfg ? cfg->mute_ssg : 0;
	title_from_path(s->title, sizeof s->title, filename);
	fill_known_game(filename, s->game, sizeof s->game);
	s->one_loop_ms = measure_ms(s);
	s->opna = new ymfm::ym2608(s->iface);
	sr = s->opna->sample_rate(PAI_CLOCK);
	if (sr == 0) sr = 55467;
	s->chip_step = ((int64_t)sr << 16) / s->rate;
	if (s->loops_want < 1) s->loops_want = 1;
	s->play_limit = (uint32_t)(((int64_t)s->one_loop_ms * s->loops_want * s->rate) / 1000);
	if (s->play_limit < (uint32_t)s->rate)
		s->play_limit = (uint32_t)s->rate;
	reset_play(s, 0);
	return 0;
}

int pai_probe_mem(const uint8_t *data, size_t len)
{
	return pc98_looks_pai(data, len);
}

int pai_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out)
{
	pai_state tmp;
	if (!out) return -1;
	memset(out, 0, sizeof *out);
	if (setup(&tmp, filename, data, len, cfg) != 0) {
		delete tmp.opna;
		return -1;
	}
	out->kind = PC98_KIND_PAI;
	out->songs = 1;
	out->looping = 1;
	out->one_loop_ms[0] = tmp.one_loop_ms;
	pc98_bounded(out->title, sizeof out->title, tmp.title);
	pc98_bounded(out->game, sizeof out->game, tmp.game);
	pc98_bounded(out->filetype, sizeof out->filetype, "PAI");
	pc98_bounded(out->engine, sizeof out->engine, "ymfm Synthia");
	delete tmp.opna;
	return 0;
}

void *pai_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg)
{
	pai_state *s = new pai_state();
	if (setup(s, filename, data, len, cfg) != 0) {
		delete s->opna;
		delete s;
		return NULL;
	}
	return s;
}

void pai_close_h(void *h)
{
	pai_state *s = (pai_state *)h;
	if (!s) return;
	delete s->opna;
	delete s;
}

int pai_process_h(void *h, float *buf, int count)
{
	pai_state *s = (pai_state *)h;
	int i;
	if (!s || !s->opna || !buf || count <= 0) return 0;
	for (i = 0; i < count; ++i) {
		int64_t need;
		if (s->play_limit && s->play_samples >= s->play_limit) {
			buf[i * 2] = 0;
			buf[i * 2 + 1] = 0;
			continue;
		}
		need = (int64_t)s->rate * 1152 * tb_period(s->tb);
		s->irq_acc += (int64_t)PAI_CLOCK;
		while (s->irq_acc >= need) {
			irq(s, 0, 0);
			s->irq_acc -= need;
			need = (int64_t)s->rate * 1152 * tb_period(s->tb);
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

int pai_seek_ms_h(void *h, int ms)
{
	pai_state *s = (pai_state *)h;
	uint32_t target;
	if (!s) return -1;
	if (ms < 0) ms = 0;
	reset_play(s, 0);
	target = (uint32_t)(((int64_t)ms * s->rate) / 1000);
	while (s->play_samples < target && !s->ended) {
		irq(s, 0, 0);
		{
			int64_t need = (int64_t)s->rate * 1152 * tb_period(s->tb);
			s->play_samples += (uint32_t)((need + (int64_t)PAI_CLOCK / 2) /
					(int64_t)PAI_CLOCK);
		}
	}
	s->irq_acc = 0;
	return 0;
}

int pai_one_loop_ms_h(void *h)
{
	pai_state *s = (pai_state *)h;
	return s ? s->one_loop_ms : 0;
}

int pai_rate_h(void *h)
{
	pai_state *s = (pai_state *)h;
	return s ? s->rate : PC98_DEFAULT_RATE;
}

void pai_set_loops_h(void *h, int loops)
{
	pai_state *s = (pai_state *)h;
	if (!s) return;
	if (loops < 1) loops = 1;
	s->loops_want = loops;
	s->play_limit = (uint32_t)(((int64_t)s->one_loop_ms * s->loops_want * s->rate) / 1000);
}

void pai_apply_mute_h(void *h, const pc98_cfg *cfg)
{
	pai_state *s = (pai_state *)h;
	if (!s || !cfg) return;
	s->mute_fm = cfg->mute_fm;
	s->mute_ssg = cfg->mute_ssg;
}

const char *pai_title_h(void *h)
{
	pai_state *s = (pai_state *)h;
	return s ? s->title : "";
}

const char *pai_game_h(void *h)
{
	pai_state *s = (pai_state *)h;
	return s ? s->game : "";
}

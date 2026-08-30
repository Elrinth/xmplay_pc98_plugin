/*
 * Kinpukurin / Libido BGMDRV.COM player (Faladia, Freewill, Blue, …).
 * 14-part table, 25-byte TONE.DAT patches, YM2608 SSG+FM1–3.
 * Channels 6–13 are MIDI on the original driver and stay silent here.
 * Timing: PIT 2.4576 MHz / 0x1194 (from BGMDRV.COM INT8 install).
 */
#include "bgmdrv_engine.h"
#include "pc98_util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <vector>

#include "ymfm.h"
#include "ymfm_opn.h"

#define BGM_CH        14
#define BGM_OPN       6
#define BGM_STACK     8
#define BGM_TONES     60
#define BGM_TONE_SZ   25
#define BGM_CLOCK     7987200u
#define BGM_PIT       2457600u
#define BGM_DIV       0x1194u
#define BGM_CAP_MS    (10 * 60 * 1000)

static const uint16_t k_ssg_period[16] = {
	0x0000, 0x1DD6, 0x1C28, 0x1A92, 0x1916, 0x17AE, 0x1658, 0x1516,
	0x13E4, 0x12CA, 0x11BA, 0x10BC, 0x0FCC, 0x0000, 0x04D2, 0x051C
};

static const uint16_t k_fm_fnum2[16] = {
	0x0000, 0x04D2, 0x051C, 0x056A, 0x05BC, 0x0614, 0x0670, 0x06D2,
	0x073C, 0x07A8, 0x081E, 0x089A, 0x091C, 0x7078, 0x6068, 0x5058
};

static const uint8_t k_vol_tbl[16] = {
	0x54, 0x48, 0x3E, 0x35, 0x32, 0x2D, 0x29, 0x26,
	0x21, 0x1D, 0x1A, 0x15, 0x11, 0x0E, 0x09, 0x05
};

class bgm_iface : public ymfm::ymfm_interface {
public:
	uint8_t ymfm_external_read(ymfm::access_class, uint32_t) override { return 0; }
};

struct bgm_ch {
	int enabled;
	int ended;
	int pc;
	int base;
	int wait;
	int gate;
	int tempo;
	int tempo_ct;
	int vol;
	int inst;
	int gate_ratio;
	int detune;
	int slur;
	int temp_vol_ct;
	int saved_vol;
	int loop_n;
	int call_n;
	int loop_count[BGM_STACK];
	int loop_pc[BGM_STACK];
	int call_pc[BGM_STACK];
};

struct bgm_state {
	std::vector<uint8_t> file;
	uint8_t tone[BGM_TONES * BGM_TONE_SZ];
	int has_tone;
	bgm_ch ch[BGM_CH];
	bgm_iface iface;
	ymfm::ym2608 *opna;
	ymfm::ym2608::output_data out;
	int rate;
	int loops_want;
	int loops_done;
	int looping;
	int one_loop_ms;
	int ended;
	int64_t irq_count;
	int64_t f4_loop_tick;
	int mute_fm, mute_ssg;
	int64_t chip_pos, chip_step;
	int64_t irq_acc;
	int last_l, last_r;
	uint32_t play_samples, play_limit;
	char title[256];

	bgm_state()
		: has_tone(0), opna(NULL), rate(PC98_DEFAULT_RATE), loops_want(1),
		  loops_done(0), looping(0), one_loop_ms(0), ended(0),
		  irq_count(0), f4_loop_tick(-1), mute_fm(0), mute_ssg(0),
		  chip_pos(0), chip_step(0), irq_acc(0), last_l(0), last_r(0),
		  play_samples(0), play_limit(0)
	{
		memset(tone, 0, sizeof tone);
		memset(ch, 0, sizeof ch);
		title[0] = 0;
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

static void bounded(char *dst, size_t cap, const char *src)
{
	pc98_bounded(dst, cap, src);
}

static void title_from_path(char *dst, size_t cap, const char *filename)
{
	char stem[256];
	pc98_basename(filename, stem, sizeof stem);
	bounded(dst, cap, stem[0] ? stem : "BGMDRV");
}

static int load_tone_file(const char *filename, uint8_t *tone)
{
	char dir[PC98_PATH_MAX], path[PC98_PATH_MAX];
	const char *names[] = { "TONE.DAT", "tone.dat", "Tone.dat" };
	int i;
	if (!filename) return 0;
	pc98_dir_of(filename, dir, sizeof dir);
	if (!dir[0]) return 0;
	for (i = 0; i < 3; ++i) {
		FILE *fp;
		long sz;
		snprintf(path, sizeof path, "%s%s", dir, names[i]);
		fp = fopen(path, "rb");
		if (!fp) continue;
		fseek(fp, 0, SEEK_END);
		sz = ftell(fp);
		fseek(fp, 0, SEEK_SET);
		if (sz < BGM_TONE_SZ) { fclose(fp); continue; }
		if (sz > (long)(BGM_TONES * BGM_TONE_SZ))
			sz = (long)(BGM_TONES * BGM_TONE_SZ);
		memset(tone, 0, BGM_TONES * BGM_TONE_SZ);
		if (fread(tone, 1, (size_t)sz, fp) == 0) { fclose(fp); continue; }
		fclose(fp);
		return 1;
	}
	return 0;
}

static void wr(bgm_state *s, uint8_t aa, uint8_t dd)
{
	if (!s->opna) return;
	s->opna->write(0, aa);
	s->opna->write(1, dd);
}

static void keyoff_ssg(bgm_state *s, int c)
{
	wr(s, (uint8_t)(0x08 + c), 0);
	wr(s, (uint8_t)(c * 2), 0);
	wr(s, (uint8_t)(c * 2 + 1), 0);
}

static void keyoff_fm(bgm_state *s, int c)
{
	wr(s, 0x28, (uint8_t)(c - 3));
}

static void keyoff_ch(bgm_state *s, int c)
{
	if (c < 3) keyoff_ssg(s, c);
	else if (c < BGM_OPN) keyoff_fm(s, c);
}

static void apply_fm_patch(bgm_state *s, int c, int inst, int vol)
{
	uint8_t pat[25];
	int i, alg, add, tl, fm;
	if (inst < 0) inst = 0;
	if (inst > 59) inst = 59;
	if (vol < 0) vol = 0;
	if (vol > 15) vol = 15;
	memcpy(pat, s->tone + inst * BGM_TONE_SZ, 25);
	alg = pat[24] & 7;
	add = k_vol_tbl[vol];
	for (i = 0; i < 4; ++i) {
		int carrier = 0;
		if (i == 3) carrier = 1;
		else if (i == 2 && alg >= 4) carrier = 1;
		else if (i == 1 && alg >= 5) carrier = 1;
		else if (i == 0 && alg >= 7) carrier = 1;
		if (!carrier) continue;
		tl = (int)pat[4 + i] + add;
		if (tl > 0x7F) tl = 0x7F;
		pat[4 + i] = (uint8_t)tl;
	}
	fm = c - 3;
	for (i = 0; i < 24; ++i)
		wr(s, (uint8_t)(0x30 + fm + (i & 3) * 4 + (i / 4) * 16), pat[i]);
	wr(s, (uint8_t)(0xB0 + fm), pat[24]);
}

static void note_ssg(bgm_state *s, int c, int pitch)
{
	int nib = pitch & 0x0F;
	int oct = (pitch >> 4) & 0x0F;
	int per, i;
	if (nib == 0) {
		keyoff_ssg(s, c);
		return;
	}
	per = k_ssg_period[nib];
	for (i = 0; i < oct; ++i)
		per >>= 1;
	per += (int8_t)s->ch[c].detune;
	if (per < 0) per = 0;
	if (per > 0x0FFF) per = 0x0FFF;
	wr(s, (uint8_t)(c * 2), (uint8_t)per);
	wr(s, (uint8_t)(c * 2 + 1), (uint8_t)(per >> 8));
	if (s->ch[c].inst & 0x80) {
		wr(s, (uint8_t)(0x08 + c), 0x10);
		wr(s, 0x0D, (uint8_t)(s->ch[c].inst & 0x0F));
	} else {
		int v = s->ch[c].inst & 0x0F;
		if (v > 0x0F) v = 0x0F;
		wr(s, (uint8_t)(0x08 + c), (uint8_t)v);
	}
}

static void note_fm(bgm_state *s, int c, int pitch)
{
	int nib = pitch & 0x0F;
	int blk, fnum, fm;
	if (!s->ch[c].slur)
		keyoff_fm(s, c);
	apply_fm_patch(s, c, s->ch[c].inst & 0x7F, s->ch[c].vol);
	if (nib == 0)
		return;
	fnum = k_fm_fnum2[nib] >> 1;
	fnum += (int8_t)s->ch[c].detune;
	if (fnum < 0) fnum = 0;
	if (fnum > 0x07FF) fnum = 0x07FF;
	blk = ((pitch & 0x70) - 0x10) >> 1;
	fm = c - 3;
	wr(s, (uint8_t)(0xA4 + fm), (uint8_t)((blk & 0x38) | ((fnum >> 8) & 7)));
	wr(s, (uint8_t)(0xA0 + fm), (uint8_t)fnum);
	wr(s, 0x28, (uint8_t)(0xF0 | fm));
}

static void set_note_time(bgm_ch *ch, int dur)
{
	int wait, gate;
	if (dur < 1) dur = 1;
	wait = dur - 1;
	if (ch->gate_ratio) {
		uint32_t g = (uint32_t)wait * (uint32_t)ch->gate_ratio;
		gate = (int)(g >> 8) + 1;
	} else {
		gate = dur;
	}
	ch->wait = wait;
	ch->gate = gate;
}

static int peek_slur(bgm_state *s, bgm_ch *ch)
{
	if (ch->pc < (int)s->file.size() && s->file[ch->pc] == 0xFD) {
		ch->pc++;
		ch->slur = 1;
	} else {
		ch->slur = 0;
	}
	return 0xFF;
}

static int do_cmd(bgm_state *s, int c, int op);
static int play_note(bgm_state *s, int c, int pitch, int dur)
{
	bgm_ch *ch = &s->ch[c];
	if (c < 3)
		note_ssg(s, c, pitch);
	else if (c < BGM_OPN)
		note_fm(s, c, pitch);
	set_note_time(ch, dur);
	return peek_slur(s, ch);
}

static int fetch_byte(bgm_state *s, bgm_ch *ch, int *out)
{
	if (ch->pc < 0 || ch->pc >= (int)s->file.size())
		return 0;
	*out = s->file[ch->pc++];
	return 1;
}

static int fetch_word(bgm_state *s, bgm_ch *ch, int *out)
{
	int a, b;
	if (!fetch_byte(s, ch, &a) || !fetch_byte(s, ch, &b))
		return 0;
	*out = a | (b << 8);
	return 1;
}

static int do_cmd(bgm_state *s, int c, int op)
{
	bgm_ch *ch = &s->ch[c];
	int arg = 0, w = 0;

	if (c >= BGM_OPN) {
		/* MIDI parts: consume arguments so F4/F6/F9 still keep time. */
		switch (op) {
		case 0xA0: case 0xA1: case 0xA2:
		case 0xF0: case 0xF1: case 0xF2: case 0xF5:
		case 0xF7: case 0xF8: case 0xF9: case 0xFE: case 0xFF:
			fetch_byte(s, ch, &arg);
			if (op == 0xF0) ch->tempo = arg;
			if (op == 0xF6) { ch->ended = 1; ch->enabled = 0; return 0xFF; }
			if (op == 0xF9 && ch->loop_n < BGM_STACK) {
				ch->loop_count[ch->loop_n] = arg;
				ch->loop_pc[ch->loop_n] = ch->pc;
				ch->loop_n++;
			}
			return 0;
		case 0xA4: case 0xE3: case 0xF3: case 0xF4: case 0xFB:
			if (op == 0xF4 && fetch_word(s, ch, &w)) {
				ch->pc = ch->base + w;
				return 0;
			}
			if (op == 0xFB && fetch_word(s, ch, &w)) {
				if (ch->call_n < BGM_STACK)
					ch->call_pc[ch->call_n++] = ch->pc;
				ch->pc = ch->base + w;
				return 0;
			}
			ch->pc += 2;
			return 0;
		case 0xF6:
			ch->ended = 1;
			ch->enabled = 0;
			return 0xFF;
		case 0xFA:
			if (ch->loop_n > 0) {
				int i = ch->loop_n - 1;
				ch->loop_count[i]--;
				if (ch->loop_count[i] > 0)
					ch->pc = ch->loop_pc[i];
				else
					ch->loop_n--;
			}
			return 0;
		case 0xFC:
			if (ch->call_n > 0)
				ch->pc = ch->call_pc[--ch->call_n];
			return 0;
		default:
			return 0;
		}
	}

	switch (op) {
	case 0xE0:
		if (ch->vol > 0) ch->vol--;
		return 0;
	case 0xE1:
		if (ch->vol < 15) ch->vol++;
		return 0;
	case 0xE2:
		if (!fetch_byte(s, ch, &arg)) return 0xFF;
		ch->saved_vol = ch->vol;
		ch->vol = arg;
		ch->temp_vol_ct = 2;
		return 0;
	case 0xE3:
		if (!fetch_word(s, ch, &w)) return 0xFF;
		/* byteswap: file is period big-endian for SSG env 0B/0C */
		wr(s, 0x0B, (uint8_t)(w >> 8));
		wr(s, 0x0C, (uint8_t)w);
		return 0;
	case 0xF0:
		if (!fetch_byte(s, ch, &arg)) return 0xFF;
		ch->tempo = arg;
		return 0;
	case 0xF1:
		if (!fetch_byte(s, ch, &arg)) return 0xFF;
		ch->vol = arg;
		return 0;
	case 0xF2:
		if (!fetch_byte(s, ch, &arg)) return 0xFF;
		ch->gate_ratio = arg;
		return 0;
	case 0xF3:
		if (!fetch_word(s, ch, &w)) return 0xFF;
		wr(s, (uint8_t)(w >> 8), (uint8_t)w);
		return 0;
	case 0xF4: {
		int old = ch->pc;
		if (!fetch_word(s, ch, &w)) return 0xFF;
		ch->pc = ch->base + w;
		if (c < BGM_OPN && ch->pc + 4 < old && s->f4_loop_tick < 0 &&
				s->irq_count > 200)
			s->f4_loop_tick = s->irq_count;
		return 0;
	}
	case 0xF5:
		if (!fetch_byte(s, ch, &arg)) return 0xFF;
		if (c < 3)
			wr(s, (uint8_t)(0x08 + c), 0x10);
		ch->inst = arg | 0x80;
		return 0;
	case 0xF6:
		keyoff_ch(s, c);
		ch->ended = 1;
		ch->enabled = 0;
		return 0xFF;
	case 0xF7:
		if (!fetch_byte(s, ch, &arg)) return 0xFF;
		ch->inst = arg;
		return 0;
	case 0xF8:
		fetch_byte(s, ch, &arg);
		return 0;
	case 0xF9:
		if (!fetch_byte(s, ch, &arg)) return 0xFF;
		if (ch->loop_n < BGM_STACK) {
			ch->loop_count[ch->loop_n] = arg;
			ch->loop_pc[ch->loop_n] = ch->pc;
			ch->loop_n++;
		}
		return 0;
	case 0xFA:
		if (ch->loop_n > 0) {
			int i = ch->loop_n - 1;
			ch->loop_count[i]--;
			if (ch->loop_count[i] > 0)
				ch->pc = ch->loop_pc[i];
			else
				ch->loop_n--;
		}
		return 0;
	case 0xFB:
		if (!fetch_word(s, ch, &w)) return 0xFF;
		if (ch->call_n < BGM_STACK)
			ch->call_pc[ch->call_n++] = ch->pc;
		ch->pc = ch->base + w;
		return 0;
	case 0xFC:
		if (ch->call_n > 0)
			ch->pc = ch->call_pc[--ch->call_n];
		return 0;
	case 0xFD:
		return peek_slur(s, ch);
	case 0xFE:
		if (!fetch_byte(s, ch, &arg)) return 0xFF;
		ch->detune = arg;
		return 0;
	case 0xFF:
		fetch_byte(s, ch, &arg);
		return 0;
	default:
		return 0;
	}
}

static int fetch_events(bgm_state *s, int c)
{
	bgm_ch *ch = &s->ch[c];
	int guard = 0;
	while (guard++ < 4096) {
		int b, dur;
		if (ch->ended || !ch->enabled)
			return 0xFF;
		if (!fetch_byte(s, ch, &b)) {
			ch->ended = 1;
			ch->enabled = 0;
			return 0xFF;
		}
		if (b < 0x90) {
			if (!fetch_byte(s, ch, &dur)) {
				ch->ended = 1;
				return 0xFF;
			}
			return play_note(s, c, b, dur);
		}
		if (do_cmd(s, c, b) != 0)
			return 0xFF;
	}
	return 0xFF;
}

static void gate_tick(bgm_state *s, int c)
{
	bgm_ch *ch = &s->ch[c];
	if (ch->gate <= 0) return;
	ch->gate--;
	if (ch->gate == 0 && !ch->slur)
		keyoff_ch(s, c);
}

static void advance_row(bgm_state *s, int c)
{
	bgm_ch *ch = &s->ch[c];
	if (ch->temp_vol_ct > 0) {
		ch->temp_vol_ct--;
		if (ch->temp_vol_ct == 0)
			ch->vol = ch->saved_vol;
	}
	if (ch->wait == 0) {
		fetch_events(s, c);
		return;
	}
	ch->wait--;
	if (ch->wait == 0)
		return;
	gate_tick(s, c);
}

static void irq_tick(bgm_state *s)
{
	int c, live = 0;
	for (c = 0; c < BGM_CH; ++c) {
		bgm_ch *ch = &s->ch[c];
		if (!ch->enabled) continue;
		live = 1;
		if (ch->tempo_ct == 0) {
			ch->tempo_ct = ch->tempo;
			advance_row(s, c);
		} else {
			ch->tempo_ct--;
		}
	}
	s->irq_count++;
	if (!live)
		s->ended = 1;
}

static void reset_chip(bgm_state *s)
{
	int i;
	if (!s->opna) return;
	s->opna->reset();
	s->opna->setfmvolume(32768);
	s->opna->setpsgvolume(fmgen_vol(-18.0));
	wr(s, 0x27, 0x30);
	wr(s, 0x0A, 0);
	wr(s, 0x09, 0);
	wr(s, 0x08, 0);
	wr(s, 0x07, 0xB8);
	for (i = 0; i < 3; ++i)
		wr(s, 0x28, (uint8_t)i);
}

static int parse_parts(bgm_state *s)
{
	const uint8_t *d = s->file.data();
	size_t n = s->file.size();
	int c, got = 0;
	memset(s->ch, 0, sizeof s->ch);
	for (c = 0; c < BGM_CH; ++c) {
		size_t i = (size_t)c * 4;
		int flags, off;
		if (i + 4 > n) break;
		flags = d[i + 1];
		off = rd16(d + i + 2);
		if (flags != 0xFF) continue;
		if (off < 0 || off >= (int)n) continue;
		s->ch[c].enabled = 1;
		s->ch[c].pc = off;
		s->ch[c].base = 0;
		s->ch[c].wait = 8;
		got++;
	}
	return got > 0 ? 0 : -1;
}

static void reset_play(bgm_state *s)
{
	parse_parts(s);
	reset_chip(s);
	s->ended = 0;
	s->loops_done = 0;
	s->chip_pos = 0;
	s->irq_acc = 0;
	s->irq_count = 0;
	s->f4_loop_tick = -1;
	s->play_samples = 0;
	s->last_l = s->last_r = 0;
}

static int measure_ms(bgm_state *s)
{
	int64_t ticks = 0;
	int64_t cap = ((int64_t)90 * 1000 * (int64_t)BGM_PIT) / (1000 * (int64_t)BGM_DIV);
	reset_play(s);
	while (s->irq_count < cap && !s->ended) {
		irq_tick(s);
		if (s->f4_loop_tick > 0)
			break;
	}
	ticks = s->irq_count;
	s->looping = 0;
	if (!s->ended && s->f4_loop_tick > 0) {
		s->looping = 1;
		ticks = s->f4_loop_tick;
	}
	{
		int ms = (int)((ticks * 1000 * (int64_t)BGM_DIV) / (int64_t)BGM_PIT);
		if (ms < 100) ms = 1000;
		return ms;
	}
}

static void chip_sample(bgm_state *s)
{
	int fm_l = 0, fm_r = 0, ssg = 0;
	s->chip_pos += s->chip_step;
	while (s->chip_pos >= 0x10000) {
		s->opna->generate(&s->out);
		fm_l = s->out.data[0];
		fm_r = s->out.data[1];
		ssg = s->out.data[2];
		s->chip_pos -= 0x10000;
	}
	if (s->mute_fm) { fm_l = 0; fm_r = 0; }
	if (s->mute_ssg) ssg = 0;
	s->last_l = fm_l + ssg;
	s->last_r = fm_r + ssg;
}

static int setup(bgm_state *s, const char *filename, const uint8_t *data,
		size_t len, const pc98_cfg *cfg)
{
	uint32_t sr;
	if (!data || len < 8 || !pc98_looks_bgmdrv(data, len))
		return -1;
	s->file.assign(data, data + len);
	s->rate = cfg && cfg->rate > 0 ? cfg->rate : PC98_DEFAULT_RATE;
	s->loops_want = cfg && cfg->loop_count > 0 ? cfg->loop_count : 1;
	s->mute_fm = cfg ? cfg->mute_fm : 0;
	s->mute_ssg = cfg ? cfg->mute_ssg : 0;
	s->has_tone = load_tone_file(filename, s->tone);
	title_from_path(s->title, sizeof s->title, filename);
	s->opna = new ymfm::ym2608(s->iface);
	sr = s->opna->sample_rate(BGM_CLOCK);
	if (sr == 0) sr = 55467;
	s->chip_step = ((int64_t)sr << 16) / s->rate;
	s->one_loop_ms = measure_ms(s);
	if (s->loops_want < 1) s->loops_want = 1;
	s->play_limit = (uint32_t)(((int64_t)s->one_loop_ms * s->loops_want * s->rate) / 1000);
	if (s->play_limit < (uint32_t)s->rate)
		s->play_limit = (uint32_t)s->rate;
	reset_play(s);
	return 0;
}

int bgmdrv_probe_mem(const uint8_t *data, size_t len)
{
	return pc98_looks_bgmdrv(data, len);
}

int bgmdrv_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out)
{
	bgm_state tmp;
	if (!out) return -1;
	memset(out, 0, sizeof *out);
	if (!pc98_looks_bgmdrv(data, len)) return -1;
	tmp.rate = cfg && cfg->rate > 0 ? cfg->rate : PC98_DEFAULT_RATE;
	tmp.file.assign(data, data + len);
	tmp.has_tone = load_tone_file(filename, tmp.tone);
	tmp.opna = new ymfm::ym2608(tmp.iface);
	tmp.chip_step = 0x10000;
	out->kind = PC98_KIND_BGMDRV;
	out->songs = 1;
	title_from_path(out->title, sizeof out->title, filename);
	bounded(out->filetype, sizeof out->filetype, "BGMDRV");
	bounded(out->engine, sizeof out->engine, "ymfm BGMDRV");
	out->one_loop_ms[0] = measure_ms(&tmp);
	out->looping = tmp.looping;
	delete tmp.opna;
	return 0;
}

void *bgmdrv_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg)
{
	bgm_state *s = new bgm_state();
	if (setup(s, filename, data, len, cfg) != 0) {
		delete s->opna;
		delete s;
		return NULL;
	}
	return s;
}

void bgmdrv_close_h(void *h)
{
	bgm_state *s = (bgm_state *)h;
	if (!s) return;
	delete s->opna;
	delete s;
}

int bgmdrv_process_h(void *h, float *buf, int count)
{
	bgm_state *s = (bgm_state *)h;
	int n;
	if (!s || !s->opna || !buf || count <= 0) return 0;
	n = 0;
	while (n < count) {
		int64_t need;
		if (s->ended && s->play_samples > (uint32_t)s->rate / 10)
			break;
		if (s->play_limit && s->play_samples >= s->play_limit)
			break;
		need = (int64_t)s->rate * (int64_t)BGM_DIV;
		s->irq_acc += (int64_t)BGM_PIT;
		while (s->irq_acc >= need) {
			irq_tick(s);
			s->irq_acc -= need;
		}
		chip_sample(s);
		{
			float fl = (float)s->last_l / 32768.0f;
			float fr = (float)s->last_r / 32768.0f;
			if (fl > 1.0f) fl = 1.0f;
			if (fl < -1.0f) fl = -1.0f;
			if (fr > 1.0f) fr = 1.0f;
			if (fr < -1.0f) fr = -1.0f;
			buf[n * 2] = fl;
			buf[n * 2 + 1] = fr;
		}
		s->play_samples++;
		n++;
	}
	return n;
}

int bgmdrv_seek_ms_h(void *h, int ms)
{
	bgm_state *s = (bgm_state *)h;
	int64_t target, pos;
	if (!s || ms < 0) return -1;
	reset_play(s);
	target = ((int64_t)ms * s->rate) / 1000;
	pos = 0;
	while (pos < target && !s->ended) {
		int64_t need = (int64_t)s->rate * (int64_t)BGM_DIV;
		s->irq_acc += (int64_t)BGM_PIT;
		while (s->irq_acc >= need) {
			irq_tick(s);
			s->irq_acc -= need;
		}
		pos++;
		s->play_samples++;
	}
	return (int)((pos * 1000) / s->rate);
}

int bgmdrv_one_loop_ms_h(void *h)
{
	bgm_state *s = (bgm_state *)h;
	return s ? s->one_loop_ms : 0;
}

int bgmdrv_rate_h(void *h)
{
	bgm_state *s = (bgm_state *)h;
	return s ? s->rate : PC98_DEFAULT_RATE;
}

void bgmdrv_set_loops_h(void *h, int loops)
{
	bgm_state *s = (bgm_state *)h;
	if (!s) return;
	if (loops < 1) loops = 1;
	if (loops > 3) loops = 3;
	s->loops_want = loops;
	s->play_limit = (uint32_t)(((int64_t)s->one_loop_ms * s->loops_want * s->rate) / 1000);
}

void bgmdrv_apply_mute_h(void *h, const pc98_cfg *cfg)
{
	bgm_state *s = (bgm_state *)h;
	if (!s || !cfg) return;
	s->mute_fm = cfg->mute_fm;
	s->mute_ssg = cfg->mute_ssg;
}

const char *bgmdrv_title_h(void *h)
{
	bgm_state *s = (bgm_state *)h;
	return s ? s->title : "";
}

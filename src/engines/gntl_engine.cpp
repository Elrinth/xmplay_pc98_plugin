/*
 * ArtDink Sekigahara G_*.NTL — GM/MIDI twin of F_* FM (SEK2.EXE).
 * Same byte stream as ntl_engine: 00–3F note + default length, 40–7F
 * note + length, 80+ commands. 0x85 is a raw MIDI dump (GM On, CC).
 * Rendered with TinySoundFont + the user's SC-55 SF2. Never claims .mid.
 */
#include "gntl_engine.h"
#include "fmd_engine.h"
#include "pc98_util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <vector>

#include "tsf.h"

#define GNTL_CH        16
#define GNTL_CLOCK     7987200u
#define GNTL_MAX_TICKS 400000
#define GNTL_REV_MAX   4096

struct gntl_ch {
	int enabled, ended, pc, start, end, wait;
	int keyed, slur, oct, vol, def_len, note, ch;
	int loop_pc, loop_n, did_loop;
};

struct gntl_state {
	std::vector<uint8_t> file;
	gntl_ch ch[GNTL_CH];
	int rate, loops_want, one_loop_ms, ended, tb;
	int mute_mel, mute_rhy;
	int64_t irq_acc;
	uint32_t play_samples, play_limit;
	char title[256], game[256], engine[64];
	char sf2_path[PC98_PATH_MAX];
	tsf *sf;
	float master_gain, rev_wet, rev_fb;
	int rev_len, rev_i;
	float rev_l[GNTL_REV_MAX];
	float rev_r[GNTL_REV_MAX];

	gntl_state()
		: rate(PC98_DEFAULT_RATE), loops_want(1), one_loop_ms(0),
		  ended(0), tb(0xC0), mute_mel(0), mute_rhy(0), irq_acc(0),
		  play_samples(0), play_limit(0), sf(NULL),
		  master_gain(1.f), rev_wet(0), rev_fb(0), rev_len(0), rev_i(0)
	{
		memset(ch, 0, sizeof ch);
		title[0] = 0;
		game[0] = 0;
		engine[0] = 0;
		sf2_path[0] = 0;
		memset(rev_l, 0, sizeof rev_l);
		memset(rev_r, 0, sizeof rev_r);
	}
};

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int tb_clocks(int tb)
{
	int n = 256 - (tb & 0xFF);
	if (n < 1) n = 1;
	return 1152 * n;
}

static int midi_note(int name, int oct)
{
	int n = name & 0x0F;
	int o = oct & 7;
	int m = 12 * (o + 1) + n;
	if (m < 0) m = 0;
	if (m > 127) m = 127;
	return m;
}

static int fetchb(gntl_state *s, gntl_ch *ch, int *out)
{
	if (ch->pc < 0 || ch->pc >= (int)s->file.size()) return 0;
	*out = s->file[ch->pc++];
	return 1;
}

static void note_off(gntl_state *s, gntl_ch *ch)
{
	if (!ch->keyed) return;
	if (s->sf && !ch->ended)
		tsf_channel_note_off(s->sf, ch->ch, ch->note);
	ch->keyed = 0;
}

static void note_on(gntl_state *s, gntl_ch *ch, int name, int dry)
{
	int n = midi_note(name, ch->oct);
	int vel, drum;
	if (dry) {
		ch->note = n;
		ch->keyed = 1;
		return;
	}
	drum = (ch->ch == 9);
	if (s->mute_rhy && drum) return;
	if (s->mute_mel && !drum) return;
	if (ch->keyed && !ch->slur)
		note_off(s, ch);
	else if (ch->keyed && ch->slur && ch->note != n)
		note_off(s, ch);
	ch->note = n;
	vel = 127 - (ch->vol & 127);
	if (vel < 24) vel = 24;
	if (vel > 127) vel = 127;
	if (s->sf)
		tsf_channel_note_on(s->sf, ch->ch, n, vel / 127.f);
	ch->keyed = 1;
	ch->slur = 0;
}

static void apply_cc(gntl_state *s, int ch, int cc, int val)
{
	if (!s->sf) return;
	if (cc < 0 || val < 0) return;
	tsf_channel_midi_control(s->sf, ch & 15, cc & 127, val & 127);
}

static void apply_pc(gntl_state *s, int ch, int pc)
{
	int drum;
	if (!s->sf) return;
	drum = ((ch & 15) == 9);
	tsf_channel_set_presetnumber(s->sf, ch & 15, pc & 127, drum);
}

static void feed_midi(gntl_state *s, gntl_ch *ch, const uint8_t *m, int n, int dry)
{
	int i = 0, st = 0;
	if (dry || !m || n <= 0) return;
	while (i < n) {
		int b = m[i], need, a, c;
		if (b == 0xF0) {
			while (i < n && m[i] != 0xF7) i++;
			if (i < n) i++;
			if (s->sf) {
				int c2;
				tsf_reset(s->sf);
				tsf_set_output(s->sf, TSF_STEREO_INTERLEAVED, s->rate, -8.0f);
				for (c2 = 0; c2 < 16; c2++) {
					tsf_channel_set_presetnumber(s->sf, c2, 0, c2 == 9);
					tsf_channel_set_pitchrange(s->sf, c2, 2.0f);
					tsf_channel_midi_control(s->sf, c2, 7, 100);
					tsf_channel_midi_control(s->sf, c2, 11, 127);
					tsf_channel_midi_control(s->sf, c2, 10, 64);
				}
			}
			st = 0;
			continue;
		}
		if (b >= 0x80) {
			st = b;
			i++;
			/* 85 dumps walk B1..BF then C0 for the 17th part — treat as CC. */
			if ((st & 0xF0) == 0xC0 && i + 1 < n && m[i] == 0x79)
				st = 0xB0 | (st & 0x0F);
		}
		if (st < 0x80) {
			i++;
			continue;
		}
		need = 2;
		if ((st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0)
			need = 1;
		if (st >= 0xF0)
			need = (st == 0xF2) ? 2 : (st == 0xF3) ? 1 : 0;
		if (i + need > n) break;
		a = need > 0 ? m[i] : 0;
		c = need > 1 ? m[i + 1] : 0;
		i += need;
		switch (st & 0xF0) {
		case 0xB0:
			apply_cc(s, st & 15, a, c);
			if ((st & 15) != ch->ch)
				ch->ch = st & 15;
			break;
		case 0xC0:
			apply_pc(s, st & 15, a);
			ch->ch = st & 15;
			break;
		case 0x90:
			if (s->sf && c > 0)
				tsf_channel_note_on(s->sf, st & 15, a, c / 127.f);
			else if (s->sf)
				tsf_channel_note_off(s->sf, st & 15, a);
			ch->ch = st & 15;
			break;
		case 0x80:
			if (s->sf)
				tsf_channel_note_off(s->sf, st & 15, a);
			break;
		case 0xE0:
			if (s->sf)
				tsf_channel_set_pitchwheel(s->sf, st & 15, a | (c << 7));
			break;
		default:
			break;
		}
	}
}

static void do_cmd(gntl_state *s, gntl_ch *ch, int cmd, int dry, int measure)
{
	int a = 0, b = 0;
	switch (cmd) {
	case 0x80:
		return;
	case 0x81:
		if (fetchb(s, ch, &a)) {
			ch->def_len = a > 0 ? a : 1;
			ch->wait = ch->def_len;
			if (!dry) note_off(s, ch);
		}
		return;
	case 0x82:
		ch->slur = 1;
		return;
	case 0x83:
		fetchb(s, ch, &a);
		return;
	case 0x84:
		if (fetchb(s, ch, &a)) {
			ch->vol = a & 127;
			if (!dry && s->sf) {
				int v = 127 - ch->vol;
				if (v < 16) v = 16;
				tsf_channel_midi_control(s->sf, ch->ch, 7, v);
			}
		}
		return;
	case 0x85:
		if (!fetchb(s, ch, &a) || a < 0) return;
		if (ch->pc + a > (int)s->file.size())
			a = (int)s->file.size() - ch->pc;
		if (a > 0) {
			feed_midi(s, ch, s->file.data() + ch->pc, a, dry);
			ch->pc += a;
		}
		return;
	case 0x86:
		if (fetchb(s, ch, &a))
			ch->oct = a & 7;
		return;
	case 0x87:
	case 0x8F:
	case 0x91:
	case 0x92:
		fetchb(s, ch, &a);
		return;
	case 0x88:
		fetchb(s, ch, &a);
		fetchb(s, ch, &b);
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
	case 0x8C:
	case 0x9C:
	case 0x9D:
		return;
	case 0x96:
		if (fetchb(s, ch, &a))
			s->tb = a & 0xFF;
		return;
	case 0x9A:
		if (fetchb(s, ch, &a) && !dry)
			apply_pc(s, ch->ch, a);
		return;
	default:
		return;
	}
}

static void fetch(gntl_state *s, int c, int dry, int measure)
{
	gntl_ch *ch = &s->ch[c];
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
			if (!dry) note_off(s, ch);
			ch->ended = 1;
			return;
		}
		b = s->file[ch->pc++];
		if (b < 0x40) {
			ch->wait = ch->def_len > 0 ? ch->def_len : 1;
			note_on(s, ch, b, dry);
			return;
		}
		if (b < 0x80) {
			if (!fetchb(s, ch, &ln)) { ch->ended = 1; return; }
			if (ln < 1) ln = 1;
			ch->wait = ln;
			ch->def_len = ln;
			note_on(s, ch, b, dry);
			return;
		}
		do_cmd(s, ch, b, dry, measure);
		if (ch->wait > 0 && b == 0x81)
			return;
	}
}

static void irq(gntl_state *s, int dry, int measure)
{
	int i, live = 0;
	for (i = 0; i < GNTL_CH; ++i) {
		gntl_ch *ch = &s->ch[i];
		if (!ch->enabled || ch->ended) continue;
		live++;
		if (--ch->wait <= 0)
			fetch(s, i, dry, measure);
	}
	if (!live) s->ended = 1;
}

static void mix_defaults(gntl_state *s)
{
	int ms = 70;
	s->master_gain = 96.f / 127.f;
	s->rev_wet = 0.37f;
	s->rev_fb = 0.55f;
	s->rev_len = s->rate * ms / 1000;
	if (s->rev_len < 256) s->rev_len = 256;
	if (s->rev_len > GNTL_REV_MAX) s->rev_len = GNTL_REV_MAX;
	s->rev_i = 0;
	memset(s->rev_l, 0, sizeof s->rev_l);
	memset(s->rev_r, 0, sizeof s->rev_r);
}

static void init_sf(gntl_state *s)
{
	int c;
	if (!s->sf) return;
	tsf_reset(s->sf);
	tsf_set_output(s->sf, TSF_STEREO_INTERLEAVED, s->rate, -8.0f);
	for (c = 0; c < 16; c++) {
		tsf_channel_set_presetnumber(s->sf, c, 0, c == 9);
		tsf_channel_set_pitchrange(s->sf, c, 2.0f);
		tsf_channel_midi_control(s->sf, c, 7, 100);
		tsf_channel_midi_control(s->sf, c, 11, 127);
		tsf_channel_midi_control(s->sf, c, 10, 64);
		tsf_channel_midi_control(s->sf, c, 64, 0);
		tsf_channel_set_pitchwheel(s->sf, c, 8192);
	}
}

static int parse_header(gntl_state *s)
{
	const uint8_t *d = s->file.data();
	size_t n = s->file.size();
	int cnt, i, j, off, typ, used = 0, p, te;
	int offs[64], noff = 0;
	if (n < 16 || !pc98_looks_ntl_midi(d, n)) return -1;
	cnt = d[3];
	memset(s->ch, 0, sizeof s->ch);
	for (i = 0; i < cnt && 4 + (i + 1) * 3 <= (int)n; ++i) {
		off = (int)rd16(d + 4 + i * 3);
		typ = d[4 + i * 3 + 2];
		if (off > 0 && off < (int)n && noff < 64)
			offs[noff++] = off;
		if (off <= 0 || off >= (int)n) continue;
		if (!((typ >= 0x30 && typ <= 0x3F) || (typ >= 0xB0 && typ <= 0xBF)))
			continue;
		if (used >= GNTL_CH) break;
		s->ch[used].start = off;
		s->ch[used].pc = off;
		s->ch[used].ch = typ & 0x0F;
		if (s->ch[used].ch > 15) s->ch[used].ch = 15;
		s->ch[used].enabled = 1;
		used++;
	}
	for (i = 0; i < used; ++i) {
		int nxt = (int)n;
		for (j = 0; j < noff; ++j) {
			if (offs[j] > s->ch[i].start && offs[j] < nxt)
				nxt = offs[j];
		}
		s->ch[i].end = nxt;
	}
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
	return used > 0 ? 0 : -1;
}

static void reset_play(gntl_state *s, int dry)
{
	int i;
	s->ended = 0;
	s->irq_acc = 0;
	s->play_samples = 0;
	s->tb = 0xC0;
	if (!dry) {
		mix_defaults(s);
		init_sf(s);
	}
	for (i = 0; i < GNTL_CH; ++i) {
		gntl_ch *ch = &s->ch[i];
		if (!ch->enabled) continue;
		ch->ended = 0;
		ch->pc = ch->start;
		ch->wait = 1;
		ch->keyed = 0;
		ch->slur = 0;
		ch->oct = 4;
		ch->vol = 0x28;
		ch->def_len = 12;
		ch->loop_pc = 0;
		ch->loop_n = 0;
		ch->did_loop = 0;
		ch->note = 0;
		ch->ch = ch->ch; /* kept from parse; 85 may retarget */
	}
}

static int measure_ms(gntl_state *s)
{
	int ticks = 0;
	int64_t us = 0;
	reset_play(s, 1);
	while (!s->ended && ticks < GNTL_MAX_TICKS) {
		us += (int64_t)tb_clocks(s->tb);
		irq(s, 1, 1);
		ticks++;
	}
	{
		int ms = (int)((us * 1000) / (int64_t)GNTL_CLOCK);
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

static int setup(gntl_state *s, const char *filename, const uint8_t *data,
		size_t len, const pc98_cfg *cfg, int want_sf)
{
	if (!data || !pc98_looks_ntl_midi(data, len)) return -1;
	s->file.assign(data, data + len);
	if (parse_header(s) != 0) return -1;
	s->rate = cfg && cfg->rate > 0 ? cfg->rate : PC98_DEFAULT_RATE;
	s->loops_want = cfg && cfg->loop_count > 0 ? cfg->loop_count : 1;
	s->mute_rhy = cfg ? cfg->mute_rhythm : 0;
	s->mute_mel = cfg ? cfg->mute_fm : 0;
	if (!s->title[0]) {
		char stem[256];
		pc98_basename(filename ? filename : "", stem, sizeof stem);
		pc98_bounded(s->title, sizeof s->title, stem[0] ? stem : "NTL");
	}
	fill_game(filename, s->game, sizeof s->game);
	s->sf = NULL;
	s->sf2_path[0] = 0;
	if (want_sf) {
		if (fmd_find_sf2(cfg, filename, 0, s->sf2_path, sizeof s->sf2_path))
			s->sf = (tsf *)fmd_font_get(s->sf2_path);
		if (s->sf) {
			const char *bn = strrchr(s->sf2_path, '\\');
			if (!bn) bn = strrchr(s->sf2_path, '/');
			bn = bn ? bn + 1 : s->sf2_path;
			snprintf(s->engine, sizeof s->engine, "SC-55 / TinySoundFont (%s)", bn);
		} else {
			pc98_bounded(s->engine, sizeof s->engine, "SC-55 (no SF2)");
		}
	} else {
		pc98_bounded(s->engine, sizeof s->engine, "SC-55 / TinySoundFont");
	}
	s->one_loop_ms = measure_ms(s);
	s->play_limit = (uint32_t)(((int64_t)s->one_loop_ms * s->loops_want * s->rate) / 1000);
	if (s->play_limit < (uint32_t)s->rate) s->play_limit = (uint32_t)s->rate;
	reset_play(s, 0);
	return 0;
}

int gntl_probe_mem(const uint8_t *data, size_t len)
{
	return pc98_looks_ntl_midi(data, len);
}

int gntl_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out)
{
	gntl_state tmp;
	if (!out) return -1;
	memset(out, 0, sizeof *out);
	if (setup(&tmp, filename, data, len, cfg, 0) != 0)
		return -1;
	out->kind = PC98_KIND_GNTL;
	out->songs = 1;
	out->looping = 1;
	out->one_loop_ms[0] = tmp.one_loop_ms;
	pc98_bounded(out->title, sizeof out->title, tmp.title);
	pc98_bounded(out->game, sizeof out->game, tmp.game);
	pc98_bounded(out->filetype, sizeof out->filetype, "NTL");
	pc98_bounded(out->engine, sizeof out->engine, tmp.engine);
	return 0;
}

void *gntl_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg)
{
	gntl_state *s = new gntl_state();
	if (setup(s, filename, data, len, cfg, 1) != 0) {
		delete s;
		return NULL;
	}
	return s;
}

void gntl_close_h(void *h)
{
	gntl_state *s = (gntl_state *)h;
	if (!s) return;
	delete s;
}

int gntl_process_h(void *h, float *buf, int count)
{
	gntl_state *s = (gntl_state *)h;
	int i;
	if (!s || !buf || count <= 0) return 0;
	memset(buf, 0, (size_t)count * 2 * sizeof(float));
	if (!s->sf) return count;
	for (i = 0; i < count; ) {
		int chunk, left, n;
		int64_t need = (int64_t)s->rate * tb_clocks(s->tb);
		if (s->play_limit && s->play_samples >= s->play_limit)
			break;
		while (s->irq_acc >= need) {
			irq(s, 0, 0);
			s->irq_acc -= need;
			need = (int64_t)s->rate * tb_clocks(s->tb);
		}
		{
			int64_t remain = need - s->irq_acc;
			chunk = (int)((remain + (int64_t)GNTL_CLOCK - 1) / (int64_t)GNTL_CLOCK);
		}
		left = count - i;
		if (chunk > left) chunk = left;
		if (chunk < 1) chunk = 1;
		if (s->play_limit) {
			uint32_t room = s->play_limit - s->play_samples;
			if ((uint32_t)chunk > room) chunk = (int)room;
		}
		if (chunk <= 0) break;
		tsf_render_float(s->sf, buf + i * 2, chunk, 0);
		if (s->rev_len > 0 && s->rev_wet > 0.001f) {
			int ri = s->rev_i, rl = s->rev_len;
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
			float g = s->master_gain;
			float *p = buf + i * 2;
			for (n = 0; n < chunk; n++) {
				p[n * 2] *= g;
				p[n * 2 + 1] *= g;
			}
		}
		s->irq_acc += (int64_t)GNTL_CLOCK * chunk;
		s->play_samples += (uint32_t)chunk;
		i += chunk;
	}
	return count;
}

int gntl_seek_ms_h(void *h, int ms)
{
	gntl_state *s = (gntl_state *)h;
	uint32_t target;
	if (!s) return -1;
	if (ms < 0) ms = 0;
	reset_play(s, 0);
	target = (uint32_t)(((int64_t)ms * s->rate) / 1000);
	while (s->play_samples < target && !s->ended) {
		irq(s, 0, 0);
		s->play_samples += (uint32_t)(((int64_t)s->rate * tb_clocks(s->tb) +
					GNTL_CLOCK / 2) / GNTL_CLOCK);
	}
	s->irq_acc = 0;
	return 0;
}

int gntl_one_loop_ms_h(void *h)
{
	gntl_state *s = (gntl_state *)h;
	return s ? s->one_loop_ms : 0;
}

int gntl_rate_h(void *h)
{
	gntl_state *s = (gntl_state *)h;
	return s ? s->rate : PC98_DEFAULT_RATE;
}

void gntl_set_loops_h(void *h, int loops)
{
	gntl_state *s = (gntl_state *)h;
	if (!s) return;
	if (loops < 1) loops = 1;
	s->loops_want = loops;
	s->play_limit = (uint32_t)(((int64_t)s->one_loop_ms * s->loops_want * s->rate) / 1000);
}

void gntl_apply_mute_h(void *h, const pc98_cfg *cfg)
{
	gntl_state *s = (gntl_state *)h;
	if (!s || !cfg) return;
	s->mute_mel = cfg->mute_fm;
	s->mute_rhy = cfg->mute_rhythm;
}

const char *gntl_title_h(void *h)
{
	gntl_state *s = (gntl_state *)h;
	return s ? s->title : "";
}

const char *gntl_game_h(void *h)
{
	gntl_state *s = (gntl_state *)h;
	return s ? s->game : "";
}

const char *gntl_engine_h(void *h)
{
	gntl_state *s = (gntl_state *)h;
	return s ? s->engine : "";
}

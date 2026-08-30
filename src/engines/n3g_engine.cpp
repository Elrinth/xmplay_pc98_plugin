#include "n3g_engine.h"

#include "pc98_util.h"
#include "ymfm_opn.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define N3G_CH        5
#define N3G_CLOCK     7987200
#define N3G_PIT_HZ    546
#define N3G_MAX_TICKS 400000

struct n3g_ch {
	int enabled;
	int pc;
	int wait;
	int note;
	int vol;
	int oct;
	int ended;
};

struct n3g_state {
	ymfm::ymfm_interface iface;
	ymfm::ym2608 *opna;
	ymfm::ym2608::output_data out;
	std::vector<uint8_t> file;
	n3g_ch ch[N3G_CH];
	int rate;
	int loops_want;
	int mute_fm;
	int mute_ssg;
	int one_loop_ms;
	uint32_t play_limit;
	uint32_t play_pos;
	int irq_left;
	int samples_per_irq;
	int64_t chip_pos;
	int64_t chip_step;
	int last_l, last_r;
	int ended;
	int track[5];
	char title[256];

	n3g_state()
		: opna(NULL), rate(PC98_DEFAULT_RATE), loops_want(1),
		  mute_fm(0), mute_ssg(0), one_loop_ms(0), play_limit(0),
		  play_pos(0), irq_left(0), samples_per_irq(1), chip_pos(0),
		  chip_step(0), last_l(0), last_r(0), ended(0)
	{
		memset(ch, 0, sizeof ch);
		memset(track, 0, sizeof track);
		title[0] = 0;
	}
};

static void bounded(char *dst, size_t cap, const char *src)
{
	if (!dst || cap == 0) return;
	if (!src) { dst[0] = 0; return; }
	strncpy(dst, src, cap - 1);
	dst[cap - 1] = 0;
}

static void title_from_path(char *dst, size_t cap, const char *path)
{
	char stem[256];
	pc98_basename(path ? path : "", stem, sizeof stem);
	bounded(dst, cap, stem[0] ? stem : "MDT");
}

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static void wr(n3g_state *s, uint8_t aa, uint8_t dd)
{
	if (!s->opna) return;
	s->opna->write(0, aa);
	s->opna->write(1, dd);
}

static void wrx(n3g_state *s, int ext, uint8_t aa, uint8_t dd)
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

static void reset_chip(n3g_state *s)
{
	int i;
	if (!s->opna) return;
	s->opna->reset();
	wr(s, 0x29, 0x80);
	wr(s, 0x27, 0x30);
	wr(s, 0x07, 0xB8);
	for (i = 0; i < 3; ++i) wr(s, 0x28, (uint8_t)i);
	for (i = 4; i < 7; ++i) wr(s, 0x28, (uint8_t)i);
}

static int default_pat(uint8_t *p)
{
	static const uint8_t d[25] = {
		0x31,0x31,0x31,0x31,
		0x14,0x14,0x14,0x14,
		0x1F,0x1F,0x1F,0x1F,
		0x00,0x00,0x00,0x00,
		0x00,0x00,0x00,0x00,
		0x05,0x05,0x05,0x05,
		0x3C
	};
	memcpy(p, d, 25);
	return 0;
}

static void apply_fm(n3g_state *s, int c, const uint8_t *pat)
{
	int fm = c;
	int ext = fm >= 3;
	int slot = ext ? fm - 3 : fm;
	int i;
	for (i = 0; i < 24; ++i)
		wrx(s, ext, (uint8_t)(0x30 + slot + (i & 3) * 4 + (i / 4) * 16), pat[i]);
	wrx(s, ext, (uint8_t)(0xB0 + slot), pat[24]);
	wrx(s, ext, (uint8_t)(0xB4 + slot), 0xC0);
}

static void note_on(n3g_state *s, int c, int pitch, int vol)
{
	static const int fnum[12] = {
		0x269,0x28E,0x2B4,0x2DE,0x30A,0x338,
		0x369,0x39C,0x3D3,0x40E,0x44B,0x48D
	};
	int note = pitch % 12;
	int oct = pitch / 12;
	int fn, blk, fm, ext, slot, id;
	uint8_t pat[25];
	if (oct < 0) oct = 0;
	if (oct > 7) oct = 7;
	fn = fnum[note];
	blk = oct << 3;
	fm = c;
	ext = fm >= 3;
	slot = ext ? fm - 3 : fm;
	id = ext ? (slot + 4) : slot;
	default_pat(pat);
	(void)vol;
	apply_fm(s, c, pat);
	wrx(s, ext, (uint8_t)(0xA4 + slot), (uint8_t)((blk & 0x38) | ((fn >> 8) & 7)));
	wrx(s, ext, (uint8_t)(0xA0 + slot), (uint8_t)fn);
	wr(s, 0x28, (uint8_t)(0xF0 | id));
}

static void note_off(n3g_state *s, int c)
{
	int fm = c;
	int id = fm >= 3 ? (fm - 3 + 4) : fm;
	wr(s, 0x28, (uint8_t)id);
}

static int fetch(n3g_state *s, n3g_ch *ch, int c)
{
	const uint8_t *d = s->file.data();
	size_t n = s->file.size();
	int steps = 0;
	while (ch->enabled && !ch->ended && steps++ < 96) {
		int b, nxt;
		if (ch->pc < 0 || ch->pc >= (int)n) {
			ch->ended = 1;
			return 0;
		}
		b = d[ch->pc++];
		nxt = (ch->pc < (int)n) ? d[ch->pc] : -1;
		if (b == 0x60 || b == 0xFF) {
			ch->ended = 1;
			note_off(s, c);
			return 0;
		}
		if (b == 0xC8) {
			ch->wait = (nxt >= 0) ? d[ch->pc++] : 8;
			if (ch->wait < 1) ch->wait = 1;
			note_off(s, c);
			return 1;
		}
		if (b == 0xC9) {
			int pitch = (ch->pc < (int)n) ? d[ch->pc++] : 0xA0;
			int dur = (ch->pc < (int)n) ? d[ch->pc++] : 8;
			if (dur < 1) dur = 1;
			ch->wait = dur;
			if (pitch >= 0xA0 && pitch <= 0xBF)
				note_on(s, c, (pitch - 0xA0) + ch->oct * 12, ch->vol);
			else
				note_on(s, c, (pitch & 0x7F) % 84, ch->vol);
			return 1;
		}
		/* Bare pitch + duration (OPENING.MDT is mostly this). */
		if (b >= 0xA0 && b <= 0xBF) {
			int dur = 8;
			if (nxt >= 0 && nxt < 0x80) {
				dur = d[ch->pc++];
				if (dur < 1) dur = 1;
			}
			ch->wait = dur;
			note_on(s, c, (b - 0xA0) + ch->oct * 12, ch->vol);
			return 1;
		}
		if (b == 0xF1) {
			/* u16 payload (phrase pointer or long duration). */
			if (ch->pc + 1 < (int)n) ch->pc += 2;
			continue;
		}
		if (b == 0xF2) {
			int8_t rel;
			if (ch->pc >= (int)n) { ch->ended = 1; return 0; }
			rel = (int8_t)d[ch->pc++];
			ch->pc += rel;
			continue;
		}
		if (b == 0xBC || b == 0xE0 || b == 0xCC || b == 0xCF) {
			if (ch->pc < (int)n) ch->pc++;
			continue;
		}
		/* Unknown high opcode: eat a data byte when it looks like an arg. */
		if (b >= 0x80 && nxt >= 0 && nxt < 0x80)
			ch->pc++;
	}
	return 0;
}

static void irq(n3g_state *s)
{
	int i, live = 0;
	for (i = 0; i < N3G_CH; ++i) {
		n3g_ch *ch = &s->ch[i];
		if (!ch->enabled || ch->ended) continue;
		live++;
		if (--ch->wait > 0) continue;
		if (!fetch(s, ch, i)) {
			if (ch->ended) continue;
			ch->wait = 1;
		}
	}
	if (!live) s->ended = 1;
}

static void reset_play(n3g_state *s)
{
	int i;
	memset(s->ch, 0, sizeof s->ch);
	for (i = 0; i < N3G_CH; ++i) {
		if (s->track[i] <= 0) continue;
		s->ch[i].enabled = 1;
		s->ch[i].pc = s->track[i];
		s->ch[i].wait = 1;
		s->ch[i].oct = 4;
		s->ch[i].vol = 0x7F;
	}
	s->ended = 0;
	s->play_pos = 0;
	s->irq_left = 0;
	s->chip_pos = 0;
	s->last_l = s->last_r = 0;
	reset_chip(s);
}

static int parse_header(n3g_state *s)
{
	const uint8_t *d = s->file.data();
	size_t n = s->file.size();
	int i, got = 0;
	if (n < 20 || !pc98_looks_mdt(d, n)) return -1;
	for (i = 0; i < 5; ++i) {
		s->track[i] = (int)rd16(d + 10 + i * 2);
		if (s->track[i] > 0 && s->track[i] < (int)n)
			got++;
		else
			s->track[i] = 0;
	}
	return got > 0 ? 0 : -1;
}

static int measure_ms(n3g_state *s)
{
	int ticks = 0;
	reset_play(s);
	while (!s->ended && ticks < N3G_MAX_TICKS) {
		irq(s);
		ticks++;
	}
	if (ticks < 8) ticks = N3G_PIT_HZ * 8;
	return (int)(((int64_t)ticks * 1000) / N3G_PIT_HZ);
}

static int setup(n3g_state *s, const char *filename, const uint8_t *data,
		size_t len, const pc98_cfg *cfg)
{
	uint32_t sr;
	if (!data || len < 20 || !pc98_looks_mdt(data, len))
		return -1;
	s->file.assign(data, data + len);
	if (parse_header(s) != 0) return -1;
	s->rate = cfg && cfg->rate > 0 ? cfg->rate : PC98_DEFAULT_RATE;
	s->loops_want = cfg && cfg->loop_count > 0 ? cfg->loop_count : 1;
	s->mute_fm = cfg ? cfg->mute_fm : 0;
	s->mute_ssg = cfg ? cfg->mute_ssg : 0;
	title_from_path(s->title, sizeof s->title, filename);
	s->opna = new ymfm::ym2608(s->iface);
	sr = s->opna->sample_rate(N3G_CLOCK);
	if (sr == 0) sr = 55467;
	s->chip_step = ((int64_t)sr << 16) / s->rate;
	s->samples_per_irq = s->rate / N3G_PIT_HZ;
	if (s->samples_per_irq < 1) s->samples_per_irq = 1;
	s->one_loop_ms = measure_ms(s);
	if (s->loops_want < 1) s->loops_want = 1;
	s->play_limit = (uint32_t)(((int64_t)s->one_loop_ms * s->loops_want * s->rate) / 1000);
	if (s->play_limit < (uint32_t)s->rate)
		s->play_limit = (uint32_t)s->rate;
	reset_play(s);
	return 0;
}

int n3g_probe_mem(const uint8_t *data, size_t len)
{
	return pc98_looks_mdt(data, len);
}

int n3g_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out)
{
	n3g_state tmp;
	if (!out) return -1;
	memset(out, 0, sizeof *out);
	if (setup(&tmp, filename, data, len, cfg) != 0) {
		delete tmp.opna;
		return -1;
	}
	out->kind = PC98_KIND_N3G;
	out->songs = 1;
	out->one_loop_ms[0] = tmp.one_loop_ms;
	bounded(out->title, sizeof out->title, tmp.title);
	bounded(out->filetype, sizeof out->filetype, "MDT");
	bounded(out->engine, sizeof out->engine, "ymfm N3G");
	delete tmp.opna;
	return 0;
}

void *n3g_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg)
{
	n3g_state *s = new n3g_state();
	if (setup(s, filename, data, len, cfg) != 0) {
		delete s->opna;
		delete s;
		return NULL;
	}
	return s;
}

void n3g_close_h(void *h)
{
	n3g_state *s = (n3g_state *)h;
	if (!s) return;
	delete s->opna;
	delete s;
}

int n3g_process_h(void *h, float *buf, int count)
{
	n3g_state *s = (n3g_state *)h;
	int i;
	if (!s || !buf || count <= 0) return 0;
	for (i = 0; i < count; ++i) {
		if (s->play_pos >= s->play_limit) {
			buf[i * 2] = 0;
			buf[i * 2 + 1] = 0;
			continue;
		}
		if (--s->irq_left <= 0) {
			irq(s);
			s->irq_left = s->samples_per_irq;
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
		s->play_pos++;
	}
	return count;
}

int n3g_seek_ms_h(void *h, int ms)
{
	n3g_state *s = (n3g_state *)h;
	uint32_t target;
	if (!s) return -1;
	if (ms < 0) ms = 0;
	reset_play(s);
	target = (uint32_t)(((int64_t)ms * s->rate) / 1000);
	while (s->play_pos < target && !s->ended) {
		irq(s);
		s->play_pos += (uint32_t)s->samples_per_irq;
	}
	s->irq_left = s->samples_per_irq;
	return 0;
}

int n3g_one_loop_ms_h(void *h)
{
	n3g_state *s = (n3g_state *)h;
	return s ? s->one_loop_ms : 0;
}

int n3g_rate_h(void *h)
{
	n3g_state *s = (n3g_state *)h;
	return s ? s->rate : PC98_DEFAULT_RATE;
}

void n3g_set_loops_h(void *h, int loops)
{
	n3g_state *s = (n3g_state *)h;
	if (!s) return;
	if (loops < 1) loops = 1;
	s->loops_want = loops;
	s->play_limit = (uint32_t)(((int64_t)s->one_loop_ms * s->loops_want * s->rate) / 1000);
}

void n3g_apply_mute_h(void *h, const pc98_cfg *cfg)
{
	n3g_state *s = (n3g_state *)h;
	if (!s || !cfg) return;
	s->mute_fm = cfg->mute_fm;
	s->mute_ssg = cfg->mute_ssg;
}

const char *n3g_title_h(void *h)
{
	n3g_state *s = (n3g_state *)h;
	return s ? s->title : "";
}

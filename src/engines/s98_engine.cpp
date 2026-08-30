/*
 * S98 v1/v2/v3 player. Timing follows the public S98 spec (Mamiya / Ru^3).
 * Chips: cisc fmgen YM2203 / YM2608 — same core and volume law as in_s98.
 * Mix at 55466 Hz (S98Amp Frequency) then interpolate to the XMPlay rate.
 */
#include "s98_engine.h"
#include "pc98_util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <vector>
#include <string>
#include <utility>

/* pmdmini already exposes FM::OPNA as a ymfm shim. Rename cisc fmgen. */
#define FM CISCFM
#include "fmgen/opna.h"

#define S98_MAX_DEV  8
#define S98_CAP_MS   (10 * 60 * 1000)
#define S98_MIX_RATE 55466

enum {
	S98_NONE = 0,
	S98_PSG = 1,
	S98_OPN = 2,
	S98_OPN2 = 3,
	S98_OPNA = 4,
	S98_OPM = 5,
	S98_OPLL = 6,
	S98_OPL = 7,
	S98_OPL2 = 8,
	S98_OPL3 = 9,
	S98_AY = 15,
	S98_DCSG = 16
};

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int getvv(const uint8_t *p, size_t n, size_t *i)
{
	int s = 0, v = 0;
	if (*i >= n) return 2;
	do {
		uint8_t b = p[*i];
		*i += 1;
		v |= (b & 0x7f) << s;
		s += 7;
		if (!(b & 0x80))
			break;
	} while (*i < n && s < 28);
	return v + 2;
}

struct s98_chip {
	int type;
	uint32_t clock;
	int used;
	FM::OPN *opn;
	FM::OPNA *opna;
	int last_l, last_r;
	int ssg_atten;

	s98_chip() : type(0), clock(0), used(0), opn(NULL), opna(NULL),
		last_l(0), last_r(0), ssg_atten(18) {}
};

struct s98_state {
	std::vector<uint8_t> file;
	int version;
	uint32_t timer_num, timer_den;
	uint32_t dump_ofs, loop_ofs, tag_ofs;
	int ndev;
	s98_chip chip[S98_MAX_DEV];
	int rate;
	int mix_rate;
	double mix_pos;
	int prev_l, prev_r, curr_l, curr_r;
	char rhythm_dir[PC98_PATH_MAX];
	int loops_want;
	int loops_done;
	int looping;
	int one_loop_ms;
	int ended;
	size_t pc;
	int64_t wait_left;
	double samples_per_sync;
	int64_t chip_acc[S98_MAX_DEV];
	int mute_fm, mute_ssg, mute_rhythm, mute_adpcm;
	int ssg_atten_db; /* in_s98: 18 on PC-9801, 8 on PC-8801; -1 = chip default */
	char title[256], artist[256], game[256];
};

static void bounded(char *dst, size_t cap, const char *src)
{
	size_t n;
	if (!dst || cap == 0) return;
	if (!src) { dst[0] = '\0'; return; }
	n = strlen(src);
	if (n >= cap) n = cap - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static void parse_tags(s98_state *s, const uint8_t *p, size_t n)
{
	size_t i = 0;
	if (n >= 5 && memcmp(p, "[S98]", 5) == 0)
		i = 5;
	if (i + 3 <= n && (unsigned char)p[i] == 0xEF &&
			(unsigned char)p[i + 1] == 0xBB && (unsigned char)p[i + 2] == 0xBF)
		i += 3;
	while (i < n) {
		char line[512];
		size_t L = 0;
		while (i < n && p[i] && p[i] != '\n' && L + 1 < sizeof line)
			line[L++] = (char)p[i++];
		while (i < n && (p[i] == '\n' || p[i] == '\r' || p[i] == 0))
			i++;
		line[L] = 0;
		if (L == 0) continue;
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq++ = 0;
		if (pc98_ieq(line, "title")) bounded(s->title, sizeof s->title, eq);
		else if (pc98_ieq(line, "artist")) bounded(s->artist, sizeof s->artist, eq);
		else if (pc98_ieq(line, "game")) bounded(s->game, sizeof s->game, eq);
		else if (pc98_ieq(line, "system")) {
			/* in_s98 plugin.ini: VolumeSSG98=18, VolumeSSG66=8 (dB down). */
			if (strstr(eq, "9801") || strstr(eq, "PC-98") || strstr(eq, "pc-98"))
				s->ssg_atten_db = 18;
			else if (strstr(eq, "8801") || strstr(eq, "PC-88") || strstr(eq, "pc-88"))
				s->ssg_atten_db = 8;
		}
	}
}

static int parse_header(s98_state *s, const uint8_t *d, size_t n)
{
	int i;
	if (n < 0x20 || memcmp(d, "S98", 3) != 0)
		return -1;
	s->title[0] = s->artist[0] = s->game[0] = 0;
	s->ssg_atten_db = -1;
	s->version = d[3];
	if (s->version >= '0' && s->version <= '9')
		s->version -= '0';
	s->timer_num = rd32(d + 4);
	s->timer_den = rd32(d + 8);
	if (s->timer_num == 0) s->timer_num = 10;
	if (s->timer_den == 0) s->timer_den = 1000;
	s->tag_ofs = rd32(d + 0x10);
	s->dump_ofs = rd32(d + 0x14);
	s->loop_ofs = rd32(d + 0x18);
	s->ndev = 1;
	s->chip[0].type = S98_OPNA;
	s->chip[0].clock = 7987200;
	if (s->version >= 2) {
		uint32_t count = (s->version >= 3) ? rd32(d + 0x1C) : 4;
		size_t base = 0x20;
		int got = 0;
		if (count > S98_MAX_DEV) count = S98_MAX_DEV;
		for (i = 0; i < (int)count; ++i) {
			if (base + 16 > n) break;
			uint32_t t = rd32(d + base);
			uint32_t c = rd32(d + base + 4);
			base += 16;
			if (t == 0) break;
			s->chip[got].type = (int)t;
			s->chip[got].clock = c ? c : 7987200;
			got++;
		}
		if (got > 0) s->ndev = got;
	}
	if (s->dump_ofs == 0 || s->dump_ofs >= n)
		return -1;
	if (s->tag_ofs && s->tag_ofs < n)
		parse_tags(s, d + s->tag_ofs, n - s->tag_ofs);
	if (s->mix_rate <= 0) s->mix_rate = S98_MIX_RATE;
	s->samples_per_sync = (double)s->mix_rate * (double)s->timer_num / (double)s->timer_den;
	if (s->samples_per_sync < 0.001)
		s->samples_per_sync = (double)s->mix_rate / 1000.0;
	return 0;
}

static int s98_default_ssg_atten(int type)
{
	if (type == S98_OPN || type == S98_PSG || type == S98_AY)
		return 8; /* in_s98 VolumeSSG66 */
	return 18; /* in_s98 VolumeSSG98 — PC-9801-86 */
}

static void rhythm_dir_from_cfg(char *dst, size_t cap, const pc98_cfg *cfg)
{
	if (!dst || cap == 0) return;
	dst[0] = 0;
	if (cfg && cfg->rhythm_path[0]) {
		size_t L = strlen(cfg->rhythm_path);
		if (L > 0 && (cfg->rhythm_path[L - 1] == '\\' || cfg->rhythm_path[L - 1] == '/'))
			pc98_bounded(dst, cap, cfg->rhythm_path);
		else
			snprintf(dst, cap, "%s\\", cfg->rhythm_path);
		return;
	}
	if (cfg && cfg->dll_dir[0]) {
		size_t L = strlen(cfg->dll_dir);
		if (L > 0 && (cfg->dll_dir[L - 1] == '\\' || cfg->dll_dir[L - 1] == '/'))
			pc98_bounded(dst, cap, cfg->dll_dir);
		else
			snprintf(dst, cap, "%s\\", cfg->dll_dir);
	}
}

static void chip_apply_volumes(s98_chip *c)
{
	/* in_s98 plugin.ini: VolumeFM98=0, VolumeSSG98=18 → SetVolumePSG(-18).
	 * fmgen units are ~0.5 dB (10^(db/40)). */
	if (c->opna) {
		c->opna->SetVolumeFM(0);
		c->opna->SetVolumePSG(-c->ssg_atten);
		c->opna->SetVolumeADPCM(0);
		c->opna->SetVolumeRhythmTotal(0);
	}
	if (c->opn) {
		c->opn->SetVolumeFM(0);
		c->opn->SetVolumePSG(-c->ssg_atten);
	}
}

static void opna_enable_6ch(s98_chip *c)
{
	if (!c->opna) return;
	c->opna->SetReg(0x29, 0x80);
}

static void chip_init(s98_chip *c, int mix_rate, int ssg_atten_db, const char *rhythm_dir)
{
	uint32_t clock = c->clock ? c->clock : 7987200;
	c->used = 0;
	c->opn = NULL;
	c->opna = NULL;
	c->ssg_atten = ssg_atten_db >= 0 ? ssg_atten_db : s98_default_ssg_atten(c->type);
	if (mix_rate <= 0) mix_rate = S98_MIX_RATE;
	if (c->type == S98_OPN || c->type == S98_PSG || c->type == S98_AY) {
		c->opn = new FM::OPN();
		c->opn->Init(clock, (uint)mix_rate, false, 0);
		c->opn->Reset();
		c->used = 1;
	} else if (c->type == S98_OPNA || c->type == S98_OPN2 || c->type == 0) {
		c->opna = new FM::OPNA();
		c->opna->Init(clock, (uint)mix_rate, false, rhythm_dir && rhythm_dir[0] ? rhythm_dir : 0);
		c->opna->Reset();
		c->used = 1;
	}
	chip_apply_volumes(c);
	opna_enable_6ch(c);
}

static void chip_free(s98_chip *c)
{
	delete c->opn;
	delete c->opna;
	c->opn = NULL;
	c->opna = NULL;
}

static void chip_write(s98_chip *c, int ext, uint8_t aa, uint8_t dd)
{
	if (c->opn) {
		c->opn->SetReg(aa, dd);
		(void)ext;
		return;
	}
	if (c->opna)
		c->opna->SetReg(ext ? (0x100u | aa) : aa, dd);
}

static void chip_tick(s98_chip *c, const s98_state *s)
{
	FM::Sample buf[2];
	buf[0] = buf[1] = 0;
	if (c->opn)
		c->opn->Mix(buf, 1);
	else if (c->opna)
		c->opna->Mix(buf, 1);
	if (s && s->mute_fm && s->mute_ssg) {
		c->last_l = c->last_r = 0;
		return;
	}
	c->last_l = (int)buf[0];
	c->last_r = (int)buf[1];
}

static int64_t sync_to_samples(s98_state *s, int nsync)
{
	return (int64_t)(s->samples_per_sync * (double)nsync + 0.5);
}

static int run_until_wait(s98_state *s)
{
	const uint8_t *d = s->file.data();
	size_t n = s->file.size();
	while (s->wait_left <= 0 && !s->ended) {
		if (s->pc >= n) {
			s->ended = 1;
			return -1;
		}
		uint8_t cmd = d[s->pc++];
		if (cmd == 0xFF) {
			s->wait_left += sync_to_samples(s, 1);
		} else if (cmd == 0xFE) {
			int vv = getvv(d, n, &s->pc);
			s->wait_left += sync_to_samples(s, vv);
		} else if (cmd == 0xFD) {
			if (s->loop_ofs && s->loop_ofs < n && s->loops_done + 1 < s->loops_want) {
				s->loops_done++;
				s->pc = s->loop_ofs;
			} else {
				s->ended = 1;
				return -1;
			}
		} else if (cmd < 0x80) {
			int dev = cmd / 2;
			int ext = cmd & 1;
			if (s->pc + 1 >= n) {
				s->ended = 1;
				return -1;
			}
			uint8_t aa = d[s->pc++];
			uint8_t dd = d[s->pc++];
			if (dev >= 0 && dev < s->ndev)
				chip_write(&s->chip[dev], ext, aa, dd);
		}
	}
	return 0;
}

static int measure_ms(const uint8_t *d, size_t n, uint32_t dump, uint32_t loop,
		uint32_t tnum, uint32_t tden, int *looping)
{
	size_t pc = dump;
	double sec = 0;
	double sync = (double)tnum / (double)tden;
	int seen_loop = 0;
	if (looping) *looping = 0;
	if (sync <= 0) sync = 0.001;
	while (pc < n && sec < (S98_CAP_MS / 1000.0)) {
		uint8_t cmd = d[pc++];
		if (cmd == 0xFF) {
			sec += sync;
		} else if (cmd == 0xFE) {
			int vv = getvv(d, n, &pc);
			sec += sync * (double)vv;
		} else if (cmd == 0xFD) {
			if (loop && loop < n && !seen_loop) {
				seen_loop = 1;
				if (looping) *looping = 1;
				return (int)(sec * 1000.0);
			}
			return (int)(sec * 1000.0);
		} else if (cmd < 0x80) {
			if (pc + 1 >= n) break;
			pc += 2;
		}
	}
	return (int)(sec * 1000.0);
}

static void reset_play(s98_state *s)
{
	int i;
	s->pc = s->dump_ofs;
	s->wait_left = 0;
	s->loops_done = 0;
	s->ended = 0;
	for (i = 0; i < s->ndev; ++i) {
		if (s->chip[i].opn) s->chip[i].opn->Reset();
		if (s->chip[i].opna) s->chip[i].opna->Reset();
		chip_apply_volumes(&s->chip[i]);
		opna_enable_6ch(&s->chip[i]);
	}
	s->mix_pos = 0;
	s->prev_l = s->prev_r = s->curr_l = s->curr_r = 0;
}

int s98_probe(const uint8_t *data, size_t len)
{
	if (!data || len < 0x20) return 0;
	if (memcmp(data, "S98", 3) != 0) return 0;
	{
		int v = data[3];
		if (v >= '0' && v <= '3') return 1;
		if (v <= 3) return 1;
	}
	return 0;
}

int s98_analyze(const uint8_t *data, size_t len, const pc98_cfg *cfg, pc98_info *out)
{
	s98_state tmp;
	int looping = 0;
	if (!out) return -1;
	memset(out, 0, sizeof *out);
	if (!s98_probe(data, len)) return -1;
	tmp.rate = cfg && cfg->rate > 0 ? cfg->rate : PC98_DEFAULT_RATE;
	tmp.mix_rate = S98_MIX_RATE;
	tmp.file.assign(data, data + len);
	if (parse_header(&tmp, data, len) != 0) return -1;
	out->kind = PC98_KIND_S98;
	out->songs = 1;
	bounded(out->title, sizeof out->title, tmp.title);
	bounded(out->artist, sizeof out->artist, tmp.artist);
	bounded(out->game, sizeof out->game, tmp.game);
	bounded(out->filetype, sizeof out->filetype, "S98");
	bounded(out->engine, sizeof out->engine, "fmgen S98");
	out->one_loop_ms[0] = measure_ms(data, len, tmp.dump_ofs, tmp.loop_ofs,
			tmp.timer_num, tmp.timer_den, &looping);
	if (out->one_loop_ms[0] < 100)
		out->one_loop_ms[0] = 1000;
	out->looping = looping;
	return 0;
}

void *s98_open(const uint8_t *data, size_t len, const pc98_cfg *cfg)
{
	s98_state *s;
	int i, looping = 0;
	if (!s98_probe(data, len)) return NULL;
	s = new s98_state();
	s->rate = cfg && cfg->rate > 0 ? cfg->rate : PC98_DEFAULT_RATE;
	s->mix_rate = S98_MIX_RATE;
	s->loops_want = cfg && cfg->loop_count > 0 ? cfg->loop_count : 1;
	s->mute_fm = cfg ? cfg->mute_fm : 0;
	s->mute_ssg = cfg ? cfg->mute_ssg : 0;
	s->mute_rhythm = cfg ? cfg->mute_rhythm : 0;
	s->mute_adpcm = cfg ? cfg->mute_adpcm : 0;
	rhythm_dir_from_cfg(s->rhythm_dir, sizeof s->rhythm_dir, cfg);
	s->file.assign(data, data + len);
	if (parse_header(s, data, len) != 0) {
		delete s;
		return NULL;
	}
	for (i = 0; i < s->ndev; ++i)
		chip_init(&s->chip[i], s->mix_rate, s->ssg_atten_db, s->rhythm_dir);
	s->one_loop_ms = measure_ms(data, len, s->dump_ofs, s->loop_ofs,
			s->timer_num, s->timer_den, &looping);
	s->looping = looping;
	if (s->one_loop_ms < 100) s->one_loop_ms = 1000;
	reset_play(s);
	return s;
}

void s98_close(void *h)
{
	s98_state *s = (s98_state *)h;
	int i;
	if (!s) return;
	for (i = 0; i < s->ndev; ++i)
		chip_free(&s->chip[i]);
	delete s;
}

static void mix_one(s98_state *s)
{
	int ch, l = 0, r = 0;
	if (s->ended && s->wait_left <= 0) {
		s->curr_l = s->curr_r = 0;
		return;
	}
	if (run_until_wait(s) < 0 && s->wait_left <= 0) {
		s->curr_l = s->curr_r = 0;
		return;
	}
	for (ch = 0; ch < s->ndev; ++ch) {
		s98_chip *c = &s->chip[ch];
		if (!c->used) continue;
		chip_tick(c, s);
		l += c->last_l;
		r += c->last_r;
	}
	s->curr_l = l;
	s->curr_r = r;
	s->wait_left--;
}

int s98_process(void *h, float *buf, int count)
{
	s98_state *s = (s98_state *)h;
	int n;
	double step;
	if (!s || !buf || count <= 0) return 0;
	if (s->mix_rate <= 0) s->mix_rate = S98_MIX_RATE;
	step = (double)s->mix_rate / (double)(s->rate > 0 ? s->rate : PC98_DEFAULT_RATE);
	n = 0;
	while (n < count) {
		double t;
		int l, r;
		if (s->ended && s->wait_left <= 0 && s->mix_pos < 1.0)
			break;
		s->mix_pos += step;
		while (s->mix_pos >= 1.0) {
			s->prev_l = s->curr_l;
			s->prev_r = s->curr_r;
			mix_one(s);
			s->mix_pos -= 1.0;
		}
		t = s->mix_pos;
		l = (int)((double)s->prev_l + ((double)s->curr_l - (double)s->prev_l) * t);
		r = (int)((double)s->prev_r + ((double)s->curr_r - (double)s->prev_r) * t);
		{
			float fl = (float)l / 32768.0f;
			float fr = (float)r / 32768.0f;
			if (fl > 1.0f) fl = 1.0f;
			if (fl < -1.0f) fl = -1.0f;
			if (fr > 1.0f) fr = 1.0f;
			if (fr < -1.0f) fr = -1.0f;
			buf[n * 2] = fl;
			buf[n * 2 + 1] = fr;
		}
		n++;
	}
	return n;
}

int s98_seek_ms(void *h, int ms)
{
	s98_state *s = (s98_state *)h;
	int64_t target, pos;
	if (!s || ms < 0) return -1;
	reset_play(s);
	target = ((int64_t)ms * s->rate) / 1000;
	pos = 0;
	while (pos < target && !s->ended) {
		if (run_until_wait(s) < 0) break;
		{
			int64_t step = s->wait_left;
			if (step > target - pos) step = target - pos;
			s->wait_left -= step;
			pos += step;
		}
	}
	return (int)((pos * 1000) / s->rate);
}

int s98_one_loop_ms(void *h)
{
	s98_state *s = (s98_state *)h;
	return s ? s->one_loop_ms : 0;
}

int s98_rate(void *h)
{
	s98_state *s = (s98_state *)h;
	return s ? s->rate : PC98_DEFAULT_RATE;
}

void s98_set_loops(void *h, int loops)
{
	s98_state *s = (s98_state *)h;
	if (!s) return;
	if (loops < 1) loops = 1;
	if (loops > 3) loops = 3;
	s->loops_want = loops;
}

void s98_apply_mute(void *h, const pc98_cfg *cfg)
{
	s98_state *s = (s98_state *)h;
	if (!s || !cfg) return;
	s->mute_fm = cfg->mute_fm;
	s->mute_ssg = cfg->mute_ssg;
	s->mute_rhythm = cfg->mute_rhythm;
	s->mute_adpcm = cfg->mute_adpcm;
}

const char *s98_title(void *h)
{
	s98_state *s = (s98_state *)h;
	return s ? s->title : "";
}

const char *s98_artist(void *h)
{
	s98_state *s = (s98_state *)h;
	return s ? s->artist : "";
}

const char *s98_game(void *h)
{
	s98_state *s = (s98_state *)h;
	return s ? s->game : "";
}

/*
 * Melody / K.Kondo Ultra Super Music Driver (.USO).
 * Sequencer recovered from PIYO+LZEXE USMD.EXE (Ver 2.0, May 27 1994).
 *
 * Notes: bytes < 0xB0 (low nibble = pitch, high bits = duration index).
 * 0xB0–0xBF: note + explicit 1/2-byte duration. Commands are 0xE0–0xFF.
 * .MFD sibling is SC-55 MIDI, not the FM bank.
 *
 * Mix: cisc fmgen OPNA @ 55466 Hz, VolumeFM=0, SSG −18 dB (same as S98).
 * Night Slave NSA##.USO uses a matching ##.S98 dump when one is found
 * next to the file or in the local S98 library (identical to in_s98).
 */
#include "mfd_engine.h"
#include "s98_engine.h"
#include "pc98_util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <vector>

#define FM CISCFM
#include "fmgen/opna.h"

#define USMD_CH        16
#define USMD_TONES     64
#define USMD_TONE_SZ   25
#define USMD_CLOCK     7987200u
#define USMD_MIX_RATE  55466
#define USMD_TICK_HZ   105
#define USMD_CAP_TICKS 400000
#define USMD_STACK     8

static const uint16_t k_dur[16] = {
	0x00C0, 0x0090, 0x0060, 0x0048, 0x0030, 0x0024, 0x0018, 0x0012,
	0x000C, 0x0009, 0x0006, 0x0000, 0x0025, 0x004C, 0x0075, 0x00A1
};

static const uint16_t k_fnum[96] = {
	0x0000,0x0025,0x004C,0x0075,0x00A1,0x00CF,0x0100,0x0134,
	0x016B,0x01A6,0x01E4,0x0225,0x026A,0x028F,0x02B6,0x02DF,
	0x030B,0x0339,0x036A,0x039E,0x03D5,0x0410,0x044E,0x048F,
	0x04D4,0x04F9,0x0520,0x0549,0x0575,0x05A3,0x05D4,0x0608,
	0x063F,0x067A,0x06B8,0x06F9,0x073E,0x0763,0x078A,0x07B3,
	0x07DF,0x080D,0x083E,0x0872,0x08A9,0x08E4,0x0922,0x0963,
	0x09A8,0x09CD,0x09F4,0x0A1D,0x0A49,0x0A77,0x0AA8,0x0ADC,
	0x0B13,0x0B4E,0x0B8C,0x0BCD,0x0C12,0x0C37,0x0C5E,0x0C87,
	0x0CB3,0x0CE1,0x0D12,0x0D46,0x0D7D,0x0DB8,0x0DF6,0x0E37,
	0x0E7C,0x0EA1,0x0EC8,0x0EF1,0x0F1D,0x0F4B,0x0F7C,0x0FB0,
	0x0FE7,0x1022,0x1060,0x10A1,0x10E6,0x110B,0x1132,0x115B,
	0x1187,0x11B5,0x11E6,0x121A,0x1251,0x128C,0x12CA,0x130B
};

static const uint8_t k_defpat[25] = {
	0x52,0x31,0x34,0x51, 0x19,0x13,0x7F,0x7F,
	0x1F,0x5F,0x1F,0x5F, 0x0A,0x0F,0x0C,0x0E,
	0x06,0x08,0x04,0x06, 0x2A,0x29,0x19,0x18, 0x3C
};

struct usmd_ch {
	int enabled, ended, is_ssg;
	int fm; /* 0–5 */
	int pc, wait, gate;
	int trans, detune, vol, inst, slur, flags;
	int loop_cnt[32];
};

struct usmd_state {
	std::vector<uint8_t> file;
	uint8_t tone[USMD_TONES * USMD_TONE_SZ];
	int ntone;
	usmd_ch ch[USMD_CH];
	FM::OPNA *opna;
	int rate, mix_rate;
	double mix_pos, samples_per_tick, tick_acc;
	int prev_l, prev_r, curr_l, curr_r;
	int loops_want, loops_done, looping, one_loop_ms, ended;
	int mute_fm, mute_ssg;
	int64_t ticks, loop_tick;
	char title[256];
	char engine[64];
	char rhythm_dir[PC98_PATH_MAX];
	void *s98;
	int use_s98;

	usmd_state()
		: ntone(0), opna(NULL), rate(PC98_DEFAULT_RATE), mix_rate(USMD_MIX_RATE),
		  mix_pos(0), samples_per_tick(USMD_MIX_RATE / (double)USMD_TICK_HZ),
		  tick_acc(0), prev_l(0), prev_r(0), curr_l(0), curr_r(0),
		  loops_want(1), loops_done(0), looping(0), one_loop_ms(0), ended(0),
		  mute_fm(0), mute_ssg(0), ticks(0), loop_tick(-1), s98(NULL), use_s98(0)
	{
		memset(tone, 0, sizeof tone);
		memset(ch, 0, sizeof ch);
		title[0] = engine[0] = rhythm_dir[0] = 0;
	}
};

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static int rd16s(const uint8_t *p)
{
	int v = (int)rd16(p);
	if (v >= 0x8000) v -= 0x10000;
	return v;
}

static void bounded(char *dst, size_t cap, const char *src)
{
	pc98_bounded(dst, cap, src);
}

static void title_from_path(char *dst, size_t cap, const char *filename)
{
	char stem[256];
	pc98_basename(filename, stem, sizeof stem);
	bounded(dst, cap, stem[0] ? stem : "USO");
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

/* NSA02.USO / nsa02.uso → 2. -1 if not an NSA## stem. */
static int uso_nsa_nn(const char *filename)
{
	char stem[256];
	size_t n;
	int nn;
	pc98_basename(filename, stem, sizeof stem);
	n = strlen(stem);
	if (n == 6 && (stem[5] == 'V' || stem[5] == 'v'))
		n = 5;
	if (n != 5) return -1;
	if ((stem[0] != 'N' && stem[0] != 'n') ||
			(stem[1] != 'S' && stem[1] != 's') ||
			(stem[2] != 'A' && stem[2] != 'a'))
		return -1;
	if (stem[3] < '0' || stem[3] > '9' || stem[4] < '0' || stem[4] > '9')
		return -1;
	nn = (stem[3] - '0') * 10 + (stem[4] - '0');
	return nn;
}

static int file_exists(const char *p)
{
	FILE *fp;
	if (!p || !p[0]) return 0;
	fp = fopen(p, "rb");
	if (!fp) return 0;
	fclose(fp);
	return 1;
}

static int find_uso_s98(const char *filename, char *out, size_t cap)
{
	char dir[PC98_PATH_MAX];
	int nn;
	static const char *libs[] = {
		"F:\\YeOldeDisk\\Musik\\S98 (Pc98 AND PC88)\\Night Slave (PC-98)(1996)(Melody)",
		NULL
	};
	int i;
	if (!filename || !out || cap < 8) return 0;
	out[0] = 0;
	nn = uso_nsa_nn(filename);
	if (nn < 0) return 0;
	pc98_dir_of(filename, dir, sizeof dir);
	if (dir[0]) {
		snprintf(out, cap, "%s%02d.S98", dir, nn);
		if (file_exists(out)) return 1;
		snprintf(out, cap, "%s%02d.s98", dir, nn);
		if (file_exists(out)) return 1;
	}
	for (i = 0; libs[i]; ++i) {
		snprintf(out, cap, "%s\\%02d.S98", libs[i], nn);
		if (file_exists(out)) return 1;
		snprintf(out, cap, "%s\\%02d.s98", libs[i], nn);
		if (file_exists(out)) return 1;
	}
	out[0] = 0;
	return 0;
}

static void *open_s98_path(const char *path, const pc98_cfg *cfg)
{
	FILE *fp;
	long sz;
	std::vector<uint8_t> raw;
	void *h;
	fp = fopen(path, "rb");
	if (!fp) return NULL;
	fseek(fp, 0, SEEK_END);
	sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (sz < 32 || sz > (long)PC98_MAX_FILE) {
		fclose(fp);
		return NULL;
	}
	raw.resize((size_t)sz);
	if (fread(raw.data(), 1, (size_t)sz, fp) != (size_t)sz) {
		fclose(fp);
		return NULL;
	}
	fclose(fp);
	h = s98_open(raw.data(), raw.size(), cfg);
	return h;
}

static int looks_pat(const uint8_t *p)
{
	int i, ar = 0;
	if ((p[24] & 7) > 7) return 0;
	if ((p[24] >> 3) > 7) return 0;
	for (i = 0; i < 4; ++i)
		if ((p[i] & 0x70) > 0x70) return 0;
	for (i = 8; i < 12; ++i)
		if (p[i] & 0x1F) ar = 1;
	return ar;
}

static void collect_tones(usmd_state *s)
{
	size_t i, n = s->file.size();
	const uint8_t *d = s->file.data();
	s->ntone = 0;
	memset(s->tone, 0, sizeof s->tone);
	for (i = 0; i + 25 <= n && s->ntone < USMD_TONES; ++i) {
		if (!looks_pat(d + i)) continue;
		if ((d[i + 24] & 0xC0) != 0) continue;
		memcpy(s->tone + s->ntone * USMD_TONE_SZ, d + i, 25);
		s->ntone++;
		i += 24;
	}
	if (s->ntone == 0) {
		memcpy(s->tone, k_defpat, 25);
		s->ntone = 1;
	}
}

static void wr(usmd_state *s, uint8_t aa, uint8_t dd)
{
	if (s->opna)
		s->opna->SetReg(aa, dd);
}

static void wrx(usmd_state *s, int ext, uint8_t aa, uint8_t dd)
{
	if (s->opna)
		s->opna->SetReg(ext ? (0x100u | aa) : aa, dd);
}

static void apply_fm_patch(usmd_state *s, usmd_ch *ch)
{
	uint8_t pat[25];
	int inst, i, ext, slot;
	inst = ch->inst;
	if (inst < 0) inst = 0;
	if (s->ntone > 0)
		inst %= s->ntone;
	else
		inst = 0;
	memcpy(pat, s->tone + inst * USMD_TONE_SZ, 25);
	ext = ch->fm >= 3;
	slot = ext ? ch->fm - 3 : ch->fm;
	for (i = 0; i < 24; ++i)
		wrx(s, ext, (uint8_t)(0x30 + slot + (i & 3) * 4 + (i / 4) * 16), pat[i]);
	wrx(s, ext, (uint8_t)(0xB0 + slot), pat[24]);
	wrx(s, ext, (uint8_t)(0xB4 + slot), 0xC0);
}

static uint16_t pack_fnum(int raw)
{
	int al = 0, cx = raw;
	if (cx > 0x134F) cx = 0x134F;
	if (cx < 0) cx = 0;
	while (cx >= 0x026A) {
		cx -= 0x026A;
		al += 8;
	}
	cx += 0x026A;
	return (uint16_t)((cx & 0xFF) | ((((cx >> 8) & 0xFF) | al) << 8));
}

static void keyoff_fm(usmd_state *s, usmd_ch *ch)
{
	int id = ch->fm >= 3 ? (ch->fm - 3 + 4) : ch->fm;
	wr(s, 0x28, (uint8_t)id);
}

static void keyoff_ssg(usmd_state *s, usmd_ch *ch)
{
	int c = ch->fm;
	if (c < 0 || c > 2) return;
	wr(s, (uint8_t)(0x08 + c), 0);
}

static void keyoff(usmd_state *s, usmd_ch *ch)
{
	if (ch->is_ssg) keyoff_ssg(s, ch);
	else keyoff_fm(s, ch);
}

static void note_fm(usmd_state *s, usmd_ch *ch, int pitch)
{
	int raw, ext, slot, id;
	uint16_t packed;
	if (!ch->slur)
		keyoff_fm(s, ch);
	apply_fm_patch(s, ch);
	if (pitch <= 0) return;
	if (pitch > 95) pitch = 95;
	raw = (int)k_fnum[pitch] + (int8_t)ch->detune;
	packed = pack_fnum(raw);
	ext = ch->fm >= 3;
	slot = ext ? ch->fm - 3 : ch->fm;
	id = ext ? (slot + 4) : slot;
	wrx(s, ext, (uint8_t)(0xA4 + slot), (uint8_t)(packed >> 8));
	wrx(s, ext, (uint8_t)(0xA0 + slot), (uint8_t)packed);
	wr(s, 0x28, (uint8_t)(0xF0 | id));
}

static void note_ssg(usmd_state *s, usmd_ch *ch, int pitch)
{
	int per, c = ch->fm;
	if (c < 0 || c > 2) return;
	if (pitch <= 0) {
		keyoff_ssg(s, ch);
		return;
	}
	if (pitch > 95) pitch = 95;
	per = (int)k_fnum[pitch] + (int8_t)ch->detune;
	if (per < 0) per = 0;
	if (per > 0x0FFF) per = 0x0FFF;
	wr(s, (uint8_t)(c * 2), (uint8_t)per);
	wr(s, (uint8_t)(c * 2 + 1), (uint8_t)(per >> 8));
	wr(s, (uint8_t)(0x08 + c), 0x0C);
}

static int fetch_byte(usmd_state *s, usmd_ch *ch, int *out)
{
	if (ch->pc < 0 || ch->pc >= (int)s->file.size())
		return 0;
	*out = s->file[ch->pc++];
	return 1;
}

static int fetch_word(usmd_state *s, usmd_ch *ch, int *out)
{
	if (ch->pc + 1 >= (int)s->file.size())
		return 0;
	*out = rd16s(s->file.data() + ch->pc);
	ch->pc += 2;
	return 1;
}

static void set_wait(usmd_ch *ch, int dur)
{
	if (dur < 1) dur = 1;
	ch->wait = dur;
	ch->gate = dur;
	if (ch->vol >= 2 && ch->vol <= 14) {
		int g = dur - (dur >> (ch->vol / 2));
		if (g < 1) g = 1;
		ch->gate = g;
	}
}

static void play_note(usmd_state *s, usmd_ch *ch, int nibble, int dur)
{
	int pitch = nibble;
	if (nibble == 0) {
		keyoff(s, ch);
		set_wait(ch, dur);
		ch->slur = 0;
		return;
	}
	pitch = nibble + ch->trans;
	if (ch->is_ssg)
		note_ssg(s, ch, pitch);
	else
		note_fm(s, ch, pitch);
	set_wait(ch, dur);
}

static int do_cmd(usmd_state *s, usmd_ch *ch, int cmd)
{
	int arg = 0, rel = 0, slot, cnt;
	switch (cmd) {
	case 0xFF:
		keyoff(s, ch);
		ch->ended = 1;
		ch->enabled = 0;
		return 1;
	case 0xFE:
		if (!fetch_byte(s, ch, &arg)) return 1;
		ch->trans = (int8_t)arg;
		return 0;
	case 0xFD:
		if (!fetch_byte(s, ch, &arg)) return 1;
		return 0;
	case 0xFC:
		if (!fetch_byte(s, ch, &arg)) return 1;
		ch->detune = (int8_t)arg;
		return 0;
	case 0xFB:
		if (!fetch_word(s, ch, &rel)) return 1;
		return 0;
	case 0xF8:
		if (!fetch_byte(s, ch, &arg)) return 1;
		ch->vol = arg;
		return 0;
	case 0xF7:
		ch->flags |= 0x10;
		return 0;
	case 0xF6:
		if (!fetch_byte(s, ch, &arg)) return 1;
		ch->slur = arg ? 1 : 0;
		if (arg) ch->flags |= 2;
		else ch->flags &= ~2;
		return 0;
	case 0xF5:
		if (!fetch_word(s, ch, &rel)) return 1;
		ch->pc += rel;
		if (rel < -4 && s->ticks > 200 && s->loop_tick < 0)
			s->loop_tick = s->ticks;
		return 0;
	case 0xF4:
		if (!fetch_byte(s, ch, &slot)) return 1;
		slot &= 31;
		if (ch->loop_cnt[slot] == 0) {
			if (!fetch_byte(s, ch, &cnt)) return 1;
			ch->loop_cnt[slot] = cnt;
			ch->pc += 2;
		} else {
			if (!fetch_byte(s, ch, &cnt)) return 1;
			ch->loop_cnt[slot]--;
			if (ch->loop_cnt[slot] == 0) {
				if (!fetch_word(s, ch, &rel)) return 1;
				ch->pc += rel;
				if (rel < -4 && s->ticks > 200 && s->loop_tick < 0)
					s->loop_tick = s->ticks;
			} else {
				ch->pc += 2;
			}
		}
		return 0;
	case 0xF3:
		if (!fetch_byte(s, ch, &slot)) return 1;
		slot &= 31;
		if (ch->loop_cnt[slot] == 0) {
			if (!fetch_byte(s, ch, &cnt)) return 1;
			ch->loop_cnt[slot] = cnt;
			if (!fetch_word(s, ch, &rel)) return 1;
			ch->pc += rel;
		} else {
			if (!fetch_byte(s, ch, &cnt)) return 1;
			ch->loop_cnt[slot]--;
			if (ch->loop_cnt[slot] == 0)
				ch->pc += 2;
			else {
				if (!fetch_word(s, ch, &rel)) return 1;
				ch->pc += rel;
			}
		}
		return 0;
	case 0xE1:
		if (!fetch_byte(s, ch, &arg)) return 1;
		ch->inst = arg;
		if (!ch->is_ssg)
			apply_fm_patch(s, ch);
		return 0;
	case 0xE0:
		if (!fetch_byte(s, ch, &arg)) return 1;
		ch->inst = arg;
		return 0;
	default:
		return 0;
	}
}

static int fetch_events(usmd_state *s, usmd_ch *ch)
{
	int guard = 0;
	while (guard++ < 4096) {
		int b, dur, d;
		if (ch->ended || !ch->enabled)
			return 1;
		if (!fetch_byte(s, ch, &b)) {
			ch->ended = 1;
			ch->enabled = 0;
			return 1;
		}
		if (b < 0xB0) {
			dur = (int)k_dur[((b >> 3) & 0x1E) >> 1];
			play_note(s, ch, b & 0x0F, dur);
			return 0;
		}
		if (b < 0xC0) {
			if (!fetch_byte(s, ch, &d)) return 1;
			if (d & 0x80) {
				int lo;
				if (!fetch_byte(s, ch, &lo)) return 1;
				d = ((d & 0x7F) << 8) | lo;
			}
			play_note(s, ch, b & 0x0F, d);
			return 0;
		}
		if (do_cmd(s, ch, b))
			return 1;
	}
	return 1;
}

static void irq_tick(usmd_state *s)
{
	int c, live = 0;
	for (c = 0; c < USMD_CH; ++c) {
		usmd_ch *ch = &s->ch[c];
		if (!ch->enabled) continue;
		live = 1;
		if (ch->wait > 0) {
			ch->wait--;
			if (ch->gate > 0) {
				ch->gate--;
				if (ch->gate == 0 && !ch->slur)
					keyoff(s, ch);
			}
			if (ch->wait == 0)
				fetch_events(s, ch);
		} else {
			fetch_events(s, ch);
		}
	}
	s->ticks++;
	if (!live)
		s->ended = 1;
}

static int parse_parts(usmd_state *s)
{
	const uint8_t *d = s->file.data();
	size_t n = s->file.size();
	int parts, i, got = 0, ssg_n = 0;
	if (n < 32 || !pc98_looks_uso(d, n)) return -1;
	parts = (int)rd16(d + 2);
	memset(s->ch, 0, sizeof s->ch);
	for (i = 0; i < parts && i < USMD_CH; ++i) {
		int cid = (int)rd16(d + 16 + i * 10);
		int off = (int)rd16(d + 16 + i * 10 + 2);
		usmd_ch *ch;
		if (off <= 0 || off >= (int)n) continue;
		if ((cid & 0xFF00) == 0x0A00) {
			int lane = (cid & 0xFF) - 1;
			if (lane < 0 || lane > 5) continue;
			ch = &s->ch[lane];
			if (ch->enabled) continue;
			ch->enabled = 1;
			ch->is_ssg = 0;
			ch->fm = lane;
		} else if (cid == 0) {
			ch = &s->ch[6 + ssg_n];
			if (ssg_n < 3) {
				ch->enabled = 1;
				ch->is_ssg = 1;
				ch->fm = ssg_n;
				ssg_n++;
			} else
				continue;
		} else if (cid >= 1 && cid <= 6) {
			ch = &s->ch[cid - 1];
			if (ch->enabled) continue;
			ch->enabled = 1;
			ch->is_ssg = 0;
			ch->fm = cid - 1;
			if (ch->fm > 5) ch->fm = 5;
		} else
			continue;
		ch->pc = off;
		ch->wait = 0;
		ch->vol = 0;
		ch->trans = 0;
		got++;
	}
	return got > 0 ? 0 : -1;
}

static void reset_chip(usmd_state *s)
{
	int i;
	if (!s->opna) return;
	s->opna->Reset();
	s->opna->SetVolumeFM(0);
	s->opna->SetVolumePSG(-18);
	s->opna->SetVolumeADPCM(0);
	s->opna->SetVolumeRhythmTotal(0);
	wr(s, 0x29, 0x80);
	wr(s, 0x27, 0x30);
	wr(s, 0x07, 0xB8);
	for (i = 0; i < 3; ++i)
		wr(s, 0x28, (uint8_t)i);
	for (i = 4; i < 7; ++i)
		wr(s, 0x28, (uint8_t)i);
}

static void reset_play(usmd_state *s)
{
	parse_parts(s);
	if (s->opna)
		reset_chip(s);
	s->ended = 0;
	s->loops_done = 0;
	s->ticks = 0;
	s->loop_tick = -1;
	s->tick_acc = 0;
	s->mix_pos = 0;
	s->prev_l = s->prev_r = s->curr_l = s->curr_r = 0;
}

static int measure_ticks(usmd_state *s)
{
	int64_t cap = USMD_CAP_TICKS;
	reset_play(s);
	while (s->ticks < cap && !s->ended) {
		irq_tick(s);
		if (s->loop_tick > 0)
			break;
	}
	s->looping = 0;
	if (!s->ended && s->loop_tick > 0) {
		s->looping = 1;
		return (int)s->loop_tick;
	}
	return (int)s->ticks;
}

static void mix_one(usmd_state *s)
{
	FM::Sample buf[2];
	s->tick_acc += 1.0;
	while (s->tick_acc >= s->samples_per_tick && !s->ended) {
		irq_tick(s);
		s->tick_acc -= s->samples_per_tick;
		if (s->looping && s->loop_tick > 0 && s->ticks >= s->loop_tick) {
			if (s->loops_done + 1 < s->loops_want) {
				s->loops_done++;
				{
					int64_t keep = s->ticks;
					reset_play(s);
					s->ticks = keep;
				}
			} else {
				s->ended = 1;
			}
		}
	}
	buf[0] = buf[1] = 0;
	if (s->opna && !s->ended)
		s->opna->Mix(buf, 1);
	if (s->mute_fm && s->mute_ssg)
		s->curr_l = s->curr_r = 0;
	else {
		s->curr_l = (int)buf[0];
		s->curr_r = (int)buf[1];
	}
}

static int setup_native(usmd_state *s, const char *filename, const uint8_t *data,
		size_t len, const pc98_cfg *cfg)
{
	int ticks, ms;
	if (!data || len < 32 || !pc98_looks_uso(data, len))
		return -1;
	s->file.assign(data, data + len);
	s->rate = cfg && cfg->rate > 0 ? cfg->rate : PC98_DEFAULT_RATE;
	s->mix_rate = USMD_MIX_RATE;
	s->loops_want = cfg && cfg->loop_count > 0 ? cfg->loop_count : 1;
	s->mute_fm = cfg ? cfg->mute_fm : 0;
	s->mute_ssg = cfg ? cfg->mute_ssg : 0;
	s->samples_per_tick = (double)s->mix_rate / (double)USMD_TICK_HZ;
	rhythm_dir_from_cfg(s->rhythm_dir, sizeof s->rhythm_dir, cfg);
	title_from_path(s->title, sizeof s->title, filename);
	bounded(s->engine, sizeof s->engine, "fmgen USMD");
	collect_tones(s);
	s->opna = new FM::OPNA();
	s->opna->Init(USMD_CLOCK, (uint)s->mix_rate, false,
			s->rhythm_dir[0] ? s->rhythm_dir : 0);
	ticks = measure_ticks(s);
	ms = (int)((ticks * 1000) / USMD_TICK_HZ);
	if (ms < 100) ms = 1000;
	s->one_loop_ms = ms;
	reset_play(s);
	return 0;
}

static int setup(usmd_state *s, const char *filename, const uint8_t *data,
		size_t len, const pc98_cfg *cfg)
{
	char s98path[PC98_PATH_MAX];
	if (find_uso_s98(filename, s98path, sizeof s98path)) {
		s->s98 = open_s98_path(s98path, cfg);
		if (s->s98) {
			s->use_s98 = 1;
			s->rate = s98_rate(s->s98);
			s->one_loop_ms = s98_one_loop_ms(s->s98);
			title_from_path(s->title, sizeof s->title, filename);
			if (s98_title(s->s98) && s98_title(s->s98)[0])
				bounded(s->title, sizeof s->title, s98_title(s->s98));
			bounded(s->engine, sizeof s->engine, "fmgen S98");
			return 0;
		}
	}
	return setup_native(s, filename, data, len, cfg);
}

int mfd_probe_mem(const uint8_t *data, size_t len)
{
	return pc98_looks_uso(data, len);
}

int mfd_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out)
{
	char s98path[PC98_PATH_MAX];
	if (!out) return -1;
	memset(out, 0, sizeof *out);
	if (!pc98_looks_uso(data, len)) return -1;
	out->kind = PC98_KIND_MFD;
	out->songs = 1;
	title_from_path(out->title, sizeof out->title, filename);
	bounded(out->filetype, sizeof out->filetype, "USO");
	if (find_uso_s98(filename, s98path, sizeof s98path)) {
		FILE *fp = fopen(s98path, "rb");
		if (fp) {
			long sz;
			fseek(fp, 0, SEEK_END);
			sz = ftell(fp);
			fseek(fp, 0, SEEK_SET);
			if (sz > 32 && sz < (long)PC98_MAX_FILE) {
				std::vector<uint8_t> raw((size_t)sz);
				if (fread(raw.data(), 1, (size_t)sz, fp) == (size_t)sz &&
						s98_analyze(raw.data(), raw.size(), cfg, out) == 0) {
					out->kind = PC98_KIND_MFD;
					if (!out->title[0])
						title_from_path(out->title, sizeof out->title, filename);
					bounded(out->filetype, sizeof out->filetype, "USO");
					bounded(out->engine, sizeof out->engine, "fmgen S98");
					fclose(fp);
					return 0;
				}
			}
			fclose(fp);
		}
	}
	{
		usmd_state tmp;
		if (setup_native(&tmp, filename, data, len, cfg) != 0)
			return -1;
		bounded(out->engine, sizeof out->engine, "fmgen USMD");
		out->one_loop_ms[0] = tmp.one_loop_ms;
		out->looping = tmp.looping;
		delete tmp.opna;
	}
	return 0;
}

void *mfd_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg)
{
	usmd_state *s = new usmd_state();
	if (setup(s, filename, data, len, cfg) != 0) {
		delete s->opna;
		if (s->s98) s98_close(s->s98);
		delete s;
		return NULL;
	}
	return s;
}

void mfd_close_h(void *h)
{
	usmd_state *s = (usmd_state *)h;
	if (!s) return;
	if (s->s98) s98_close(s->s98);
	delete s->opna;
	delete s;
}

int mfd_process_h(void *h, float *buf, int count)
{
	usmd_state *s = (usmd_state *)h;
	int n;
	double step;
	if (!s || !buf || count <= 0) return 0;
	if (s->use_s98 && s->s98)
		return s98_process(s->s98, buf, count);
	if (!s->opna) return 0;
	if (s->mix_rate <= 0) s->mix_rate = USMD_MIX_RATE;
	step = (double)s->mix_rate / (double)(s->rate > 0 ? s->rate : PC98_DEFAULT_RATE);
	n = 0;
	while (n < count) {
		double t;
		int l, r;
		if (s->ended && s->mix_pos < 1.0)
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

int mfd_seek_ms_h(void *h, int ms)
{
	usmd_state *s = (usmd_state *)h;
	int64_t target, pos;
	if (!s || ms < 0) return -1;
	if (s->use_s98 && s->s98)
		return s98_seek_ms(s->s98, ms);
	reset_play(s);
	target = ((int64_t)ms * s->rate) / 1000;
	pos = 0;
	while (pos < target && !s->ended) {
		mix_one(s);
		pos++;
	}
	return (int)((pos * 1000) / (s->rate > 0 ? s->rate : PC98_DEFAULT_RATE));
}

int mfd_one_loop_ms_h(void *h)
{
	usmd_state *s = (usmd_state *)h;
	if (!s) return 0;
	if (s->use_s98 && s->s98)
		return s98_one_loop_ms(s->s98);
	return s->one_loop_ms;
}

int mfd_rate_h(void *h)
{
	usmd_state *s = (usmd_state *)h;
	if (!s) return PC98_DEFAULT_RATE;
	if (s->use_s98 && s->s98)
		return s98_rate(s->s98);
	return s->rate;
}

void mfd_set_loops_h(void *h, int loops)
{
	usmd_state *s = (usmd_state *)h;
	if (!s) return;
	if (loops < 1) loops = 1;
	if (loops > 3) loops = 3;
	s->loops_want = loops;
	if (s->use_s98 && s->s98)
		s98_set_loops(s->s98, loops);
}

void mfd_apply_mute_h(void *h, const pc98_cfg *cfg)
{
	usmd_state *s = (usmd_state *)h;
	if (!s || !cfg) return;
	s->mute_fm = cfg->mute_fm;
	s->mute_ssg = cfg->mute_ssg;
	if (s->use_s98 && s->s98)
		s98_apply_mute(s->s98, cfg);
}

const char *mfd_title_h(void *h)
{
	usmd_state *s = (usmd_state *)h;
	return s ? s->title : "";
}

const char *mfd_engine_name_h(void *h)
{
	usmd_state *s = (usmd_state *)h;
	return s && s->engine[0] ? s->engine : "fmgen USMD";
}

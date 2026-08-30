/*
 * OPNDRV 2.05J — FUGA System uncompressed FM (.MD).
 * Header holds 8 part words: FM1–3, SSG1–3, then OPNA rhythm (86-board).
 * Notes are MIDI numbers. Rhythm notes use the GM drum map (38=SD, 42=HH…).
 * Dummy parts are the header stub F2 00 FC 00 80 C0 FB only.
 * Tempo is the part-table BPM byte at 48 PPQ. F0 sets BPM.
 * Length is one pass to FE / backward F9; FC/FB inner repeats expand.
 * .GMD / .MMD are packed FMD MIDI and are not played here.
 */
#include "md_engine.h"
#include "pc98_util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <vector>

#include "ymfm.h"
#include "ymfm_opn.h"

#define MD_CH        8
#define MD_FM        3
#define MD_SSG0      3
#define MD_RHY0      6
#define MD_CLOCK     7987200u
#define MD_MAX_TICKS 400000
#define MD_NEST      5
#define MD_RHY_N     6

static const int k_op[4] = { 0, 8, 4, 12 };
static const uint8_t k_car[8] = {
	0x08, 0x08, 0x08, 0x08, 0x0A, 0x0E, 0x0E, 0x0F
};
static const int k_fnum[12] = {
	0x269, 0x28E, 0x2B4, 0x2DE, 0x30A, 0x338,
	0x369, 0x39C, 0x3D3, 0x40E, 0x44B, 0x48D
};
static const uint8_t k_def_pat[25] = {
	0x31, 0x31, 0x31, 0x31, 0x18, 0x18, 0x18, 0x18,
	0x1F, 0x1F, 0x1F, 0x1F, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x05, 0x05, 0x05, 0x05, 0x3C
};

class md_iface : public ymfm::ymfm_interface {
public:
	const uint8_t *rom;
	size_t rom_n;
	md_iface() : rom(NULL), rom_n(0) {}
	uint8_t ymfm_external_read(ymfm::access_class type, uint32_t address) override
	{
		if (type == ymfm::ACCESS_ADPCM_A && rom && address < rom_n)
			return rom[address];
		return 0;
	}
};

struct md_ch {
	int enabled, ended, ssg, rhy;
	int pc, start, wait, gate_left, q, keyed, slur;
	int vol, inst, did_loop, rhy_mask;
	int lp_pc[MD_NEST];
	int lp_n[MD_NEST];
	int lp_sp;
};

struct md_rhy_smp {
	std::vector<int16_t> pcm;
	int rate;
};

struct md_rhy_voice {
	int on;
	int pos, step, end;
};

struct md_state {
	std::vector<uint8_t> file;
	int part[MD_CH];
	int nvoice;
	md_ch ch[MD_CH];
	md_iface iface;
	ymfm::ym2608 *opna;
	ymfm::ym2608::output_data out;
	int rate, loops_want, one_loop_ms, ended;
	int bpm, tb;
	int mute_fm, mute_ssg, mute_rhythm;
	int64_t chip_pos, chip_step, irq_acc;
	int last_l, last_r;
	uint32_t play_samples, play_limit;
	uint8_t ssg_mix;
	std::vector<uint8_t> rhy_rom;
	md_rhy_smp rhy_smp[MD_RHY_N];
	md_rhy_voice rhy_v[MD_RHY_N];
	int rhy_have_rom, rhy_have_wav;
	char title[256], game[256];

	md_state()
		: opna(NULL), rate(PC98_DEFAULT_RATE), loops_want(1), one_loop_ms(0),
		  ended(0), bpm(120), tb(0xE9), mute_fm(0), mute_ssg(0), mute_rhythm(0),
		  chip_pos(0), chip_step(0), irq_acc(0), last_l(0), last_r(0),
		  play_samples(0), play_limit(0), ssg_mix(0xB8),
		  rhy_have_rom(0), rhy_have_wav(0)
	{
		nvoice = 0;
		memset(part, 0, sizeof part);
		memset(ch, 0, sizeof ch);
		memset(rhy_v, 0, sizeof rhy_v);
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

static void wr(md_state *s, uint8_t aa, uint8_t dd)
{
	if (!s->opna) return;
	s->opna->write(0, aa);
	s->opna->write(1, dd);
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

static int load_wav16(const char *path, md_rhy_smp *out)
{
	FILE *fp;
	uint8_t hdr[44];
	uint32_t rate, data_bytes;
	size_t ns;
	if (!path || !out) return 0;
	fp = fopen(path, "rb");
	if (!fp) return 0;
	if (fread(hdr, 1, 44, fp) != 44) {
		fclose(fp);
		return 0;
	}
	if (memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
		fclose(fp);
		return 0;
	}
	rate = (uint32_t)hdr[24] | ((uint32_t)hdr[25] << 8) |
		((uint32_t)hdr[26] << 16) | ((uint32_t)hdr[27] << 24);
	/* find data chunk */
	{
		long pos = 12;
		uint8_t id[8];
		fseek(fp, 12, SEEK_SET);
		data_bytes = 0;
		while (fread(id, 1, 8, fp) == 8) {
			uint32_t sz = (uint32_t)id[4] | ((uint32_t)id[5] << 8) |
				((uint32_t)id[6] << 16) | ((uint32_t)id[7] << 24);
			if (memcmp(id, "data", 4) == 0) {
				data_bytes = sz;
				break;
			}
			if (fseek(fp, (long)sz, SEEK_CUR) != 0)
				break;
			(void)pos;
		}
	}
	if (data_bytes < 2 || data_bytes > 2u * 1024u * 1024u) {
		fclose(fp);
		return 0;
	}
	ns = data_bytes / 2;
	out->pcm.resize(ns);
	if (fread(out->pcm.data(), 2, ns, fp) != ns) {
		out->pcm.clear();
		fclose(fp);
		return 0;
	}
	fclose(fp);
	out->rate = rate > 0 ? (int)rate : 8000;
	return 1;
}

static void load_rhy_assets(md_state *s, const char *dir)
{
	static const char *wav[MD_RHY_N] = {
		"2608_BD.WAV", "2608_SD.WAV", "2608_TOP.WAV",
		"2608_HH.WAV", "2608_TOM.WAV", "2608_RIM.WAV"
	};
	char path[PC98_PATH_MAX];
	FILE *fp;
	int i, got = 0;
	if (!dir || !dir[0]) return;
	snprintf(path, sizeof path, "%sym2608_adpcm_rom.bin", dir);
	fp = fopen(path, "rb");
	if (fp) {
		fseek(fp, 0, SEEK_END);
		{
			long sz = ftell(fp);
			fseek(fp, 0, SEEK_SET);
			if (sz >= 0x2000 && sz <= 0x40000) {
				s->rhy_rom.resize((size_t)sz);
				if (fread(s->rhy_rom.data(), 1, (size_t)sz, fp) == (size_t)sz) {
					s->iface.rom = s->rhy_rom.data();
					s->iface.rom_n = s->rhy_rom.size();
					s->rhy_have_rom = 1;
				}
			}
		}
		fclose(fp);
	}
	if (s->rhy_have_rom) return;
	for (i = 0; i < MD_RHY_N; ++i) {
		snprintf(path, sizeof path, "%s%s", dir, wav[i]);
		if (load_wav16(path, &s->rhy_smp[i]))
			got++;
	}
	s->rhy_have_wav = (got == MD_RHY_N);
}

/* GM percussion → YM2608 rhythm bits (BD SD CYM HH TOM RIM). */
static int rhy_mask_for_note(int note)
{
	note &= 0x7F;
	switch (note) {
	case 35: case 36: return 0x01;
	case 37: case 39: return 0x20;
	case 38: case 40: return 0x02;
	case 42: case 44: return 0x08;
	case 46: return 0x08;
	case 49: case 51: case 52: case 55: case 57: case 59: return 0x04;
	case 41: case 43: case 45: case 47: case 48: case 50: return 0x10;
	default:
		if (note < 35) return 0x01;
		if (note <= 40) return 0x02;
		if (note <= 46) return 0x08;
		if (note <= 50) return 0x10;
		return 0x04;
	}
}

static void rhy_set_levels(md_state *s, int vol)
{
	int i, tl, inst;
	tl = (15 - (vol & 15)) * 4;
	if (tl < 0) tl = 0;
	if (tl > 63) tl = 63;
	wr(s, 0x11, (uint8_t)tl);
	inst = 0xC0 | ((15 - (vol & 15)) & 0x1F);
	for (i = 0; i < MD_RHY_N; ++i)
		wr(s, (uint8_t)(0x18 + i), (uint8_t)inst);
}

static void rhy_hit(md_state *s, int mask, int vol)
{
	int i;
	if (s->mute_rhythm || !mask) return;
	wr(s, 0x10, (uint8_t)(mask & 0x3F));
	if (s->rhy_have_rom || !s->rhy_have_wav) return;
	for (i = 0; i < MD_RHY_N; ++i) {
		if (!(mask & (1 << i)) || s->rhy_smp[i].pcm.empty()) continue;
		s->rhy_v[i].on = 1;
		s->rhy_v[i].pos = 0;
		s->rhy_v[i].end = (int)s->rhy_smp[i].pcm.size() << 16;
		{
			int sr = s->rhy_smp[i].rate > 0 ? s->rhy_smp[i].rate : 8000;
			int out = s->rate > 0 ? s->rate : PC98_DEFAULT_RATE;
			s->rhy_v[i].step = (int)(((int64_t)sr << 16) / out);
			if (s->rhy_v[i].step < 1) s->rhy_v[i].step = 1;
		}
		(void)vol;
	}
}

static void rhy_dump(md_state *s, int mask)
{
	int i;
	if (!mask) return;
	wr(s, 0x10, (uint8_t)(0x80 | (mask & 0x3F)));
	for (i = 0; i < MD_RHY_N; ++i) {
		if (mask & (1 << i))
			s->rhy_v[i].on = 0;
	}
}

static void rhy_mix(md_state *s, int *l, int *r)
{
	int i, acc = 0;
	if (s->mute_rhythm || !s->rhy_have_wav || s->rhy_have_rom) return;
	for (i = 0; i < MD_RHY_N; ++i) {
		md_rhy_voice *v = &s->rhy_v[i];
		int idx;
		if (!v->on) continue;
		idx = v->pos >> 16;
		if (idx >= (int)s->rhy_smp[i].pcm.size()) {
			v->on = 0;
			continue;
		}
		acc += s->rhy_smp[i].pcm[idx];
		v->pos += v->step;
		if (v->pos >= v->end)
			v->on = 0;
	}
	*l += acc;
	*r += acc;
}

static void voice_to_pat(const uint8_t *v, uint8_t *pat)
{
	int i;
	/* +0 ALG; +1..4 DT/ML; +5..8 TL; +9..12 KS/AR; … +21..24 SL/RR */
	for (i = 0; i < 24; ++i)
		pat[i] = v[1 + i];
	pat[24] = v[0];
}

static void apply_fm(md_state *s, int c, const uint8_t *pat, int vol)
{
	int i, alg, mask, atten;
	alg = pat[24] & 7;
	mask = k_car[alg];
	atten = (15 - (vol & 15)) * 4;
	if (atten < 0) atten = 0;
	for (i = 0; i < 24; ++i) {
		int op = i & 3;
		int grp = i / 4;
		int v = pat[i];
		if (grp == 1 && (mask & (1 << op))) {
			v += atten;
			if (v > 127) v = 127;
		}
		wr(s, (uint8_t)(0x30 + c + k_op[op] + grp * 16), (uint8_t)v);
	}
	wr(s, (uint8_t)(0xB0 + c), pat[24]);
	wr(s, (uint8_t)(0xB4 + c), 0xC0);
}

static const uint8_t *voice_ptr(md_state *s, int idx)
{
	int off;
	if (idx < 0 || idx >= s->nvoice) return NULL;
	off = 0x0A + idx * 36;
	if (off + 25 > (int)s->file.size()) return NULL;
	return &s->file[off];
}

static void sel_inst(md_state *s, int c, int id, int dry)
{
	uint8_t pat[25];
	const uint8_t *v;
	s->ch[c].inst = id;
	if (s->ch[c].ssg || s->ch[c].rhy || dry) return;
	v = voice_ptr(s, id);
	if (v)
		voice_to_pat(v, pat);
	else
		memcpy(pat, k_def_pat, 25);
	apply_fm(s, c, pat, s->ch[c].vol);
}

static void keyoff(md_state *s, int c)
{
	md_ch *ch = &s->ch[c];
	if (ch->rhy) {
		if (ch->rhy_mask)
			rhy_dump(s, ch->rhy_mask);
		ch->rhy_mask = 0;
	} else if (ch->ssg)
		wr(s, (uint8_t)(0x08 + (c - MD_SSG0)), 0);
	else
		wr(s, 0x28, (uint8_t)c);
	ch->keyed = 0;
}

static void play_note(md_state *s, int c, int al, int rest, int dry)
{
	md_ch *ch = &s->ch[c];
	int midi, oct, n, fn, blk;
	if (dry) return;
	if (rest) {
		keyoff(s, c);
		return;
	}
	/* OPNDRV stores MIDI note numbers (OERS_000.MD melody = GMD − 12). */
	midi = al & 0x7F;
	n = midi % 12;
	oct = midi / 12 - 1;
	if (oct < 0) oct = 0;
	if (oct > 7) oct = 7;
	if (ch->rhy) {
		int mask = rhy_mask_for_note(midi);
		ch->rhy_mask = mask;
		rhy_hit(s, mask, ch->vol);
		ch->keyed = 1;
		return;
	}
	if (ch->ssg) {
		/* YM2203 SSG period @ 7.9872 MHz / 16; C4 (60) ≈ 1911. */
		double hz = 440.0 * pow(2.0, (midi - 69) / 12.0);
		int per = (int)(499200.0 / hz + 0.5);
		int i = c - MD_SSG0;
		if (per < 1) per = 1;
		if (per > 0xFFF) per = 0xFFF;
		wr(s, (uint8_t)(i * 2), (uint8_t)per);
		wr(s, (uint8_t)(i * 2 + 1), (uint8_t)(per >> 8));
		wr(s, (uint8_t)(0x08 + i), (uint8_t)(ch->vol & 15));
		s->ssg_mix &= (uint8_t)~(1 << i);
		wr(s, 0x07, s->ssg_mix);
		ch->keyed = 1;
		return;
	}
	fn = k_fnum[n];
	blk = oct << 3;
	if (ch->keyed && !ch->slur) keyoff(s, c);
	wr(s, (uint8_t)(0xA4 + c), (uint8_t)(blk | ((fn >> 8) & 7)));
	wr(s, (uint8_t)(0xA0 + c), (uint8_t)fn);
	if (!ch->slur || !ch->keyed)
		wr(s, 0x28, (uint8_t)(0xF0 | c));
	ch->keyed = 1;
	ch->slur = 0;
}

static void set_tempo_f0(md_state *s, int ah)
{
	/* OPNDRV F0 AH is the same BPM byte as the MD header / GMD twin. */
	if (ah >= 20 && ah <= 250)
		s->bpm = ah;
}

static int fetch_word(md_state *s, md_ch *ch, int *al, int *ah)
{
	if (ch->pc < 0 || ch->pc + 1 >= (int)s->file.size()) return 0;
	{
		unsigned w = rd16(&s->file[ch->pc]);
		ch->pc += 2;
		*al = (int)(w & 0xFF);
		*ah = (int)(w >> 8);
	}
	return 1;
}

static void do_cmd(md_state *s, int c, int al, int ah, int dry, int measure)
{
	md_ch *ch = &s->ch[c];
	int dest;
	switch (al) {
	case 0xF0:
		set_tempo_f0(s, ah);
		return;
	case 0xF1:
		return;
	case 0xF2:
		ch->vol = ah & 15;
		if (!dry && ch->rhy)
			rhy_set_levels(s, ch->vol);
		if (!dry && !ch->ssg && !ch->rhy) sel_inst(s, c, ch->inst, dry);
		if (!dry && ch->ssg)
			wr(s, (uint8_t)(0x08 + (c - MD_SSG0)), (uint8_t)(ah & 15));
		return;
	case 0xF3:
		/* slur marker; lookahead in fetch() sets ch->slur */
		return;
	case 0xF4:
		ch->q = ah;
		if (ch->q < 1) ch->q = 1;
		if (ch->q > 8) ch->q = 8;
		return;
	case 0xF5:
		sel_inst(s, c, ah, dry);
		return;
	case 0xF9:
		if (ch->pc + 1 >= (int)s->file.size()) return;
		{
			int16_t rel = (int16_t)rd16(&s->file[ch->pc]);
			ch->pc += 2;
			dest = ch->pc + rel;
			if (measure && dest < ch->pc - 2) {
				ch->did_loop = 1;
				ch->ended = 1;
				return;
			}
			if (dest >= 0 && dest < (int)s->file.size())
				ch->pc = dest;
		}
		return;
	case 0xFA:
		/* skip by extra word if current FC remaining count != AH */
		if (ch->lp_sp <= 0)
			return;
		if (ch->pc + 1 >= (int)s->file.size()) return;
		{
			int16_t rel = (int16_t)rd16(&s->file[ch->pc]);
			ch->pc += 2;
			if (ch->lp_n[ch->lp_sp - 1] != ah)
				ch->pc += rel;
		}
		return;
	case 0xFB:
		if (ch->lp_sp <= 0) return;
		{
			int sp = ch->lp_sp - 1;
			if (ch->lp_n[sp] > 0) ch->lp_n[sp]--;
			if (ch->lp_n[sp] > 0) {
				/* Finite FC/FB repeats are not the song loop.
				 * Stopping here cut OERS_002.MD to ~5 s (one riff). */
				ch->pc = ch->lp_pc[sp];
			} else {
				ch->lp_sp--;
			}
		}
		return;
	case 0xFC:
		if (ch->lp_sp < MD_NEST) {
			ch->lp_pc[ch->lp_sp] = ch->pc;
			ch->lp_n[ch->lp_sp] = ah > 0 ? ah : 1;
			ch->lp_sp++;
		}
		return;
	case 0xFD:
		return;
	case 0xFE:
		if (measure) {
			ch->did_loop = 1;
			ch->ended = 1;
			return;
		}
		ch->pc = ch->start;
		return;
	case 0xD0:
		/* FM extra (detune / TL); no extra word */
		return;
	case 0xD1: {
		int v = (ch->vol + ah) & 0xFF;
		if (v <= 15) {
			ch->vol = v;
			if (!dry && ch->rhy)
				rhy_set_levels(s, ch->vol);
			if (!dry && !ch->ssg && !ch->rhy) sel_inst(s, c, ch->inst, dry);
			if (!dry && ch->ssg)
				wr(s, (uint8_t)(0x08 + (c - MD_SSG0)), (uint8_t)v);
		}
		return;
	}
	case 0xE2:
		/* flag + extra detune word (always consumed) */
		if (ch->pc + 1 < (int)s->file.size())
			ch->pc += 2;
		return;
	default:
		return;
	}
}

static void set_wait(md_ch *ch, int len)
{
	if (len < 1) len = 1;
	ch->wait = len;
	ch->gate_left = (len * ch->q) / 8;
	if (ch->gate_left < 1) ch->gate_left = 1;
	if (ch->gate_left > len) ch->gate_left = len;
}

static void fetch(md_state *s, int c, int dry, int measure)
{
	md_ch *ch = &s->ch[c];
	int guard = 0;
	while (guard++ < 256 && !ch->ended) {
		int al, ah;
		if (!fetch_word(s, ch, &al, &ah)) {
			ch->ended = 1;
			return;
		}
		if (al < 0x80) {
			set_wait(ch, ah);
			if (!dry && ch->pc + 1 < (int)s->file.size() &&
					s->file[ch->pc] == 0xF3)
				ch->slur = 1;
			play_note(s, c, al, 0, dry);
			return;
		}
		if (al == 0x80) {
			set_wait(ch, ah);
			if (!dry) keyoff(s, c);
			return;
		}
		do_cmd(s, c, al, ah, dry, measure);
	}
}

static void irq(md_state *s, int dry, int measure)
{
	int i, live = 0;
	for (i = 0; i < MD_CH; ++i) {
		md_ch *ch = &s->ch[i];
		if (!ch->enabled || ch->ended) continue;
		live++;
		if (ch->gate_left > 0) {
			ch->gate_left--;
			if (ch->gate_left <= 0 && ch->keyed && !dry && !ch->rhy)
				keyoff(s, i);
		}
		if (--ch->wait <= 0)
			fetch(s, i, dry, measure);
	}
	if (!live) s->ended = 1;
}

static int tick_clocks(md_state *s)
{
	int bpm = s->bpm;
	int c;
	if (bpm < 20) bpm = 120;
	/* 48 PPQ, matching packed FMD/GMD (OERS_000: 1440 ticks @ 110 = 16 s). */
	c = (int)((int64_t)MD_CLOCK * 60 / ((int64_t)bpm * 48));
	if (c < 256) c = 256;
	return c;
}

static void reset_chip(md_state *s)
{
	int i;
	if (!s->opna) return;
	s->opna->reset();
	s->opna->setfmvolume(32768);
	s->opna->setpsgvolume(fmgen_vol(-18.0));
	wr(s, 0x29, 0x80); /* 86-board: 6-slot FM + rhythm */
	wr(s, 0x27, 0x30);
	wr(s, 0x26, (uint8_t)s->tb);
	wr(s, 0x07, 0xB8);
	wr(s, 0x22, 0);
	for (i = 0; i < 3; ++i) wr(s, 0x28, (uint8_t)i);
	rhy_set_levels(s, 12);
	s->ssg_mix = 0xB8;
}

static int is_dummy_part(md_state *s, int off)
{
	if (off < 0 || off + 8 > (int)s->file.size()) return 1;
	/* Header stub only: F2 00 FC 00 80 C0 FB. Real parts may start F2 00 FC 00. */
	if (s->file[off] == 0xF2 && s->file[off + 1] == 0 &&
			s->file[off + 2] == 0xFC && s->file[off + 3] == 0 &&
			s->file[off + 4] == 0x80 && s->file[off + 6] == 0xFB)
		return 1;
	if (s->file[off] == 0xFD && s->file[off + 2] == 0xFE) return 1;
	return 0;
}

static int parse_header(md_state *s)
{
	const uint8_t *d = s->file.data();
	size_t n = s->file.size();
	int pt, i, got = 0, rel, abs_;
	if (n < 0x40 || !pc98_looks_opnmd(d, n)) return -1;
	s->nvoice = (int)rd16(d + 8);
	pt = (int)rd16(d);
	if (pt + 16 > (int)n) return -1;
	/* 8 part words (+0..+0F): FM×3, SSG×3, rhythm×2. +16h is BPM. */
	s->bpm = (pt + 0x17 <= (int)n) ? d[pt + 0x16] : 120;
	if (s->bpm < 20 || s->bpm > 250) s->bpm = 120;
	memset(s->part, 0, sizeof s->part);
	for (i = 0; i < MD_CH; ++i) {
		rel = (int)rd16(d + pt + i * 2);
		abs_ = pt + rel;
		if (abs_ <= 0 || abs_ >= (int)n) continue;
		if (is_dummy_part(s, abs_)) continue;
		s->part[i] = abs_;
		got++;
	}
	return got > 0 ? 0 : -1;
}

static void reset_play(md_state *s, int dry)
{
	int i;
	memset(s->ch, 0, sizeof s->ch);
	s->ended = 0;
	s->irq_acc = 0;
	s->chip_pos = 0;
	s->play_samples = 0;
	s->last_l = s->last_r = 0;
	s->tb = 0xE9;
	s->ssg_mix = 0xB8;
	memset(s->rhy_v, 0, sizeof s->rhy_v);
	if (!dry) reset_chip(s);
	for (i = 0; i < MD_CH; ++i) {
		if (s->part[i] <= 0) continue;
		s->ch[i].enabled = 1;
		s->ch[i].ssg = (i >= MD_SSG0 && i < MD_RHY0);
		s->ch[i].rhy = (i >= MD_RHY0);
		s->ch[i].pc = s->part[i];
		s->ch[i].start = s->part[i];
		s->ch[i].wait = 1;
		s->ch[i].q = 7;
		s->ch[i].vol = 12;
		if (!s->ch[i].ssg && !s->ch[i].rhy && !dry)
			sel_inst(s, i, 0, 0);
		if (s->ch[i].rhy && !dry)
			rhy_set_levels(s, s->ch[i].vol);
	}
}

static int measure_ms(md_state *s)
{
	int ticks = 0;
	int64_t us = 0;
	reset_play(s, 1);
	while (!s->ended && ticks < MD_MAX_TICKS) {
		us += (int64_t)tick_clocks(s);
		irq(s, 1, 1);
		ticks++;
	}
	{
		int ms = (int)((us * 1000) / (int64_t)MD_CLOCK);
		if (ms < 800) ms = 800;
		if (ms > 12 * 60 * 1000) ms = 12 * 60 * 1000;
		return ms;
	}
}

static void fill_game(const char *filename, char *game, size_t gcap)
{
	if (filename && (strstr(filename, "oerstd") || strstr(filename, "OERS_")))
		pc98_bounded(game, gcap, "Oerstedia");
}

static int setup(md_state *s, const char *filename, const uint8_t *data,
		size_t len, const pc98_cfg *cfg)
{
	uint32_t sr;
	if (!data || !pc98_looks_opnmd(data, len)) return -1;
	s->file.assign(data, data + len);
	if (parse_header(s) != 0) return -1;
	s->rate = cfg && cfg->rate > 0 ? cfg->rate : PC98_DEFAULT_RATE;
	s->loops_want = cfg && cfg->loop_count > 0 ? cfg->loop_count : 1;
	s->mute_fm = cfg ? cfg->mute_fm : 0;
	s->mute_ssg = cfg ? cfg->mute_ssg : 0;
	s->mute_rhythm = cfg ? cfg->mute_rhythm : 0;
	{
		char rdir[PC98_PATH_MAX];
		rhythm_dir_from_cfg(rdir, sizeof rdir, cfg);
		load_rhy_assets(s, rdir);
	}
	{
		char stem[256];
		pc98_basename(filename ? filename : "", stem, sizeof stem);
		pc98_bounded(s->title, sizeof s->title, stem[0] ? stem : "MD");
	}
	fill_game(filename, s->game, sizeof s->game);
	s->one_loop_ms = measure_ms(s);
	s->opna = new ymfm::ym2608(s->iface);
	sr = s->opna->sample_rate(MD_CLOCK);
	if (sr == 0) sr = 55467;
	s->chip_step = ((int64_t)sr << 16) / s->rate;
	s->play_limit = (uint32_t)(((int64_t)s->one_loop_ms * s->loops_want * s->rate) / 1000);
	if (s->play_limit < (uint32_t)s->rate) s->play_limit = (uint32_t)s->rate;
	reset_play(s, 0);
	return 0;
}

int md_probe_mem(const uint8_t *data, size_t len)
{
	return pc98_looks_opnmd(data, len);
}

int md_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out)
{
	md_state tmp;
	if (!out) return -1;
	memset(out, 0, sizeof *out);
	if (setup(&tmp, filename, data, len, cfg) != 0) {
		delete tmp.opna;
		return -1;
	}
	out->kind = PC98_KIND_OPNMD;
	out->songs = 1;
	out->looping = 1;
	out->one_loop_ms[0] = tmp.one_loop_ms;
	pc98_bounded(out->title, sizeof out->title, tmp.title);
	pc98_bounded(out->game, sizeof out->game, tmp.game);
	pc98_bounded(out->filetype, sizeof out->filetype, "MD");
	pc98_bounded(out->engine, sizeof out->engine, "ymfm OPNA / OPNDRV");
	delete tmp.opna;
	return 0;
}

void *md_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg)
{
	md_state *s = new md_state();
	if (setup(s, filename, data, len, cfg) != 0) {
		delete s->opna;
		delete s;
		return NULL;
	}
	return s;
}

void md_close_h(void *h)
{
	md_state *s = (md_state *)h;
	if (!s) return;
	delete s->opna;
	delete s;
}

int md_process_h(void *h, float *buf, int count)
{
	md_state *s = (md_state *)h;
	int i;
	if (!s || !s->opna || !buf || count <= 0) return 0;
	for (i = 0; i < count; ++i) {
		int64_t need;
		if (s->play_limit && s->play_samples >= s->play_limit) {
			buf[i * 2] = 0;
			buf[i * 2 + 1] = 0;
			continue;
		}
		need = (int64_t)s->rate * tick_clocks(s);
		s->irq_acc += (int64_t)MD_CLOCK;
		while (s->irq_acc >= need) {
			irq(s, 0, 0);
			s->irq_acc -= need;
			need = (int64_t)s->rate * tick_clocks(s);
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
		{
			int l = s->last_l, r = s->last_r;
			rhy_mix(s, &l, &r);
			buf[i * 2] = (float)l / 32768.f;
			buf[i * 2 + 1] = (float)r / 32768.f;
		}
		s->play_samples++;
	}
	return count;
}

int md_seek_ms_h(void *h, int ms)
{
	md_state *s = (md_state *)h;
	uint32_t target;
	if (!s) return -1;
	if (ms < 0) ms = 0;
	reset_play(s, 0);
	target = (uint32_t)(((int64_t)ms * s->rate) / 1000);
	while (s->play_samples < target && !s->ended) {
		irq(s, 0, 0);
		s->play_samples += (uint32_t)(((int64_t)s->rate * tick_clocks(s) +
					MD_CLOCK / 2) / MD_CLOCK);
	}
	s->irq_acc = 0;
	return 0;
}

int md_one_loop_ms_h(void *h)
{
	md_state *s = (md_state *)h;
	return s ? s->one_loop_ms : 0;
}

int md_rate_h(void *h)
{
	md_state *s = (md_state *)h;
	return s ? s->rate : PC98_DEFAULT_RATE;
}

void md_set_loops_h(void *h, int loops)
{
	md_state *s = (md_state *)h;
	if (!s) return;
	if (loops < 1) loops = 1;
	s->loops_want = loops;
	s->play_limit = (uint32_t)(((int64_t)s->one_loop_ms * s->loops_want * s->rate) / 1000);
}

void md_apply_mute_h(void *h, const pc98_cfg *cfg)
{
	md_state *s = (md_state *)h;
	if (!s || !cfg) return;
	s->mute_fm = cfg->mute_fm;
	s->mute_ssg = cfg->mute_ssg;
	s->mute_rhythm = cfg->mute_rhythm;
}

const char *md_title_h(void *h)
{
	md_state *s = (md_state *)h;
	return s ? s->title : "";
}

const char *md_game_h(void *h)
{
	md_state *s = (md_state *)h;
	return s ? s->game : "";
}

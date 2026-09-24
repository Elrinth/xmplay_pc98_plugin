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
#define EIKAN_MAX_MEAS 80000

static const int k_fnum[12] = {
	0x26A, 0x28F, 0x2B6, 0x2DF, 0x30B, 0x339,
	0x36A, 0x39E, 0x3D5, 0x410, 0x44E, 0x48F
};
static const int k_ssg[12] = {
	0xEE8, 0xE12, 0xD48, 0xC88, 0xBD4, 0xB2A,
	0xA8A, 0x9F2, 0x964, 0x8DC, 0x85E, 0x7E6
};
#define EIKAN_PIT_CNT 0x2A00
#define EIKAN_PIT_CLK 1996800u
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
	int pc, start, end, wait, keyed, slur, oct, vol, vol_b, detune, def_len;
	int loop_pc, loop_n, did_loop, gate, eff;
	int voice; /* Eikan FM instrument index (opcode 84) */
};

struct ntl_state {
	std::vector<uint8_t> file;
	std::vector<uint8_t> file0; /* pristine copy for Eikan in-place 0x8A counts */
	int eikan; /* ArtDink Eikan dialect (PAC): bit6 note len, off+3, 8E count+off16 loops; 89/8A word->[di+8] */
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
	int eikan_rate, eikan_acc, song_looped;
	int64_t eikan_pit_acc, eikan_pit_step;
	uint8_t voices[32][32]; /* Eikan FM bank from NTL (idx*32) */
	int voice_ok[32];
	uint8_t voices_ssg[16][16]; /* Eikan SSG bank (idx*16): env[5], mix, noise, ... */
	int voice_ssg_ok[16];

	ntl_state()
		: eikan(0), opna(NULL), rate(PC98_DEFAULT_RATE), loops_want(1), one_loop_ms(0),
		  ended(0), tb(0xC0), mute_fm(0), mute_ssg(0),
		  chip_pos(0), chip_step(0), irq_acc(0), last_l(0), last_r(0),
		  play_samples(0), play_limit(0), ssg_mix(0xB8),
		  eikan_rate(0x5000), eikan_acc(0), song_looped(0), eikan_pit_acc(0), eikan_pit_step(0)
	{
		memset(part, 0, sizeof part);
		memset(part_end, 0, sizeof part_end);
		memset(pssg, 0, sizeof pssg);
		memset(pcond, 0, sizeof pcond);
		memset(ch, 0, sizeof ch);
		memset(voices, 0, sizeof voices);
		memset(voice_ok, 0, sizeof voice_ok);
		memset(voices_ssg, 0, sizeof voices_ssg);
		memset(voice_ssg_ok, 0, sizeof voice_ssg_ok);
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

/* Eikan NTL voice: 4 ops × (DT/MUL,TL,KS/AR,DR,SR,SL/RR) + FB/ALG (+7 pad).
 * Voice byte order matches OPN regs 40/44/48/4C = Op1,Op3,Op2,Op4.
 * Carriers (by algorithm) get TL += vol_a + vol_b (opcodes 8F + 83), 1:1. */
static const uint8_t k_eikan_carriers[8] = {
	0x08, 0x08, 0x08, 0x08, 0x0A, 0x0E, 0x0E, 0x0F
};

static int eikan_atten(const ntl_ch *ch)
{
	int a = (ch->vol & 0x7F) + (ch->vol_b & 0x7F);
	if (a > 127) a = 127;
	return a;
}

static void apply_eikan_tl(ntl_state *s, int c)
{
	ntl_ch *ch = &s->ch[c];
	int ext = c >= 3;
	int slot = ext ? c - 3 : c;
	int idx = ch->voice;
	int i, atten, alg, mask;
	const uint8_t *v;
	if (ch->ssg || idx < 0 || idx >= 32 || !s->voice_ok[idx])
		return;
	v = s->voices[idx];
	atten = eikan_atten(ch);
	alg = v[24] & 7;
	mask = k_eikan_carriers[alg];
	for (i = 0; i < 4; ++i) {
		if (!(mask & (1 << i)))
			continue;
		int val = v[4 + i] + atten;
		if (val > 127) val = 127;
		/* i=0..3 → regs 40,44,48,4C (+slot) = Op1,Op3,Op2,Op4 */
		wrx(s, ext, (uint8_t)(0x40 + slot + i * 4), (uint8_t)val);
	}
}

static void apply_voice(ntl_state *s, int c, int idx, int vol)
{
	int ext = c >= 3;
	int slot = ext ? c - 3 : c;
	int i, atten, alg, mask;
	const uint8_t *v;
	if (idx < 0 || idx >= 32 || !s->voice_ok[idx]) {
		apply_def(s, c, vol);
		return;
	}
	v = s->voices[idx];
	/* Eikan: 8F/83 sum added 1:1 to carrier TL (0 = loudest). */
	atten = vol;
	if (atten < 0) atten = 0;
	if (atten > 127) atten = 127;
	alg = v[24] & 7;
	mask = k_eikan_carriers[alg];
	for (i = 0; i < 24; ++i) {
		int val = v[i];
		/* bytes 4..7 = TL for ops 0..3 (Op1,Op3,Op2,Op4) */
		if (i >= 4 && i < 8 && (mask & (1 << (i - 4)))) {
			val += atten;
			if (val > 127) val = 127;
		}
		wrx(s, ext, (uint8_t)(0x30 + slot + ((i >> 2) * 16) + ((i & 3) * 4)),
				(uint8_t)val);
	}
	wrx(s, ext, (uint8_t)(0xB0 + slot), v[24]);
	wrx(s, ext, (uint8_t)(0xB4 + slot), 0xC0);
}

/* Eikan SSG program (opcode 84 → JT type-2 @10CC): mute amp, set mixer
 * tone/noise enable from voice[5] (ROL by ch, AND into shadow), optional
 * noise period voice[6] when voice[5] < 0xFE. Song1 "drums" = noise on SSG C. */
static void apply_ssg_voice(ntl_state *s, int c, int idx)
{
	ntl_ch *ch = &s->ch[c];
	int ssgc = c >= 6 ? c - 6 : 0;
	const uint8_t *v;
	uint8_t mix_byte, al, chmask;
	if (!ch->ssg || idx < 0 || idx >= 16 || !s->voice_ssg_ok[idx])
		return;
	v = s->voices_ssg[idx];
	wr(s, (uint8_t)(0x08 + ssgc), 0);
	mix_byte = v[5];
	al = mix_byte;
	/* rol al, cl  (cl = ssgc) */
	if (ssgc)
		al = (uint8_t)(((al << ssgc) | (al >> (8 - ssgc))) & 0xFF);
	al = (uint8_t)(al & 0x3F);
	chmask = (uint8_t)(9 << ssgc); /* tone+noise disable bits for this ch */
	s->ssg_mix = (uint8_t)(al & (s->ssg_mix | chmask));
	wr(s, 0x07, s->ssg_mix);
	if (mix_byte < 0xFE)
		wr(s, 0x06, v[6]);
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
	if (dry) return; /* conductor (typ&0x80) still sounds; tempo master only */
	n = s->eikan ? (name & 0x0F) : (name - 0x40);
	if (n < 0) n = 0;
	oct = ch->oct;
	while (n > 11) { n -= 12; oct++; }
	if (oct < 0) oct = 0;
	if (oct > 7) oct = 7;
	if (ch->ssg) {
		int per, ssgc, i;
		per = (n < 12) ? k_ssg[n] : 0xEE8;
		for (i = 0; i < oct; ++i) per >>= 1;
		per += ch->detune;
		if (per < 1) per = 1;
		if (per > 0xFFF) per = 0xFFF;
		ssgc = c >= 6 ? c - 6 : 0;
		wr(s, (uint8_t)(ssgc * 2), (uint8_t)per);
		wr(s, (uint8_t)(ssgc * 2 + 1), (uint8_t)(per >> 8));
		{
			int lvl = s->eikan ? (eikan_atten(ch) >> 3) : (ch->vol & 15);
			if (s->eikan) lvl = 15 - lvl;
			wr(s, (uint8_t)(0x08 + ssgc), (uint8_t)(lvl & 15));
		}
		/* Non-Eikan: enable tone. Eikan: opcode 84 owns mixer (tone vs noise). */
		if (!s->eikan) {
			s->ssg_mix &= (uint8_t)~(1 << ssgc);
			wr(s, 0x07, s->ssg_mix);
		}
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

	if (s->eikan) {
		switch (cmd) {
		case 0x80:
			if (!dry) keyoff(s, c);
			ch->ended = 1;
			if (measure) ch->did_loop = 1;
			return;
		case 0x81:
			if (fetchb(s, ch, &a)) {
				ch->wait = a > 0 ? a : 1;
				if (!dry) keyoff(s, c);
			}
			return;
		case 0x82:
			return; /* slur marker; lookahead at irq keyoff */
		case 0x83: /* expression / soft-volume → [di+0xb]; NOT detune */
			if (fetchb(s, ch, &a)) {
				ch->vol_b = a & 0x7F;
				if (!dry && !ch->ssg)
					apply_eikan_tl(s, c);
				else if (!dry && ch->ssg && ch->keyed) {
					int lvl = eikan_atten(ch) >> 3;
					int ssgc = c >= 6 ? c - 6 : 0;
					wr(s, (uint8_t)(0x08 + ssgc), (uint8_t)(15 - lvl));
				}
			}
			return;
		case 0x84: /* instrument program (not volume!) */
			if (fetchb(s, ch, &a)) {
				if (ch->ssg) {
					ch->voice = a & 15;
					if (!dry)
						apply_ssg_voice(s, c, ch->voice);
				} else {
					ch->voice = a & 31;
					if (!dry)
						apply_voice(s, c, ch->voice, eikan_atten(ch));
				}
			}
			return;
		case 0x85:
			if (fetchb(s, ch, &a) && a > 0) {
				if (ch->pc + a > (int)s->file.size())
					a = (int)s->file.size() - ch->pc;
				if (a > 0) ch->pc += a;
			}
			return;
		case 0x86:
			if (fetchb(s, ch, &a))
				ch->oct = a & 7;
			return;
		case 0x87:
			if (fetchb(s, ch, &a))
				ch->def_len = a > 0 ? a : 1;
			return;
		case 0x88:
			if (!fetchb(s, ch, &a) || !fetchb(s, ch, &b)) return;
			s->eikan_rate = (a & 0xFF) | ((b & 0xFF) << 8);
			if (s->eikan_rate < 1) s->eikan_rate = 1;
			return;
		case 0x89:
		case 0x8A:
			if (!fetchb(s, ch, &a) || !fetchb(s, ch, &b)) return;
			ch->eff = (a & 0xFF) | ((b & 0xFF) << 8);
			return;
		case 0x8B: {
			int off, npc;
			if (ch->pc + 1 >= (int)s->file.size()) return;
			off = (int)rd16(&s->file[ch->pc]);
			npc = ch->pc - off;
			if (measure) {
				ch->did_loop = 1;
				s->song_looped = 1;
				ch->ended = 1;
				return;
			}
			if (npc < ch->start) npc = ch->start;
			ch->pc = npc;
			return;
		}
		case 0x8C:
			ch->wait = ch->def_len > 0 ? ch->def_len : 1;
			if (!dry) keyoff(s, c);
			return;
		case 0x8D: {
			int off, addr;
			if (!fetchb(s, ch, &a)) return;
			if (ch->pc + 1 >= (int)s->file.size()) return;
			off = (int)rd16(&s->file[ch->pc]);
			addr = ch->pc + off;
			if (addr >= 0 && addr < (int)s->file.size())
				s->file[addr] = (uint8_t)(a & 0xFF);
			ch->pc += 2;
			return;
		}
		case 0x8E: {
			int cpos = ch->pc;
			int cnt, off;
			if (cpos >= (int)s->file.size()) return;
			/* execute in measure too — counts live in file (restored from file0) */
			cnt = s->file[cpos];
			if (cnt == 0) {
				ch->pc = cpos + 3;
				return;
			}
			s->file[cpos] = (uint8_t)(cnt - 1);
			if (s->file[cpos] == 0)
				ch->pc = cpos + 3;
			else {
				off = (int)rd16(&s->file[cpos + 1]);
				ch->pc = (cpos + 1) - off;
			}
			return;
		}
		case 0x8F: /* main volume → [di+0xa]; 0=loudest, added 1:1 to carrier TL */
			if (fetchb(s, ch, &a)) {
				ch->vol = a & 0x7F;
				if (!dry && !ch->ssg)
					apply_eikan_tl(s, c);
				else if (!dry && ch->ssg) {
					/* SSG: (vol_a+vol_b)>>3 inverted → level 0..15 */
					int lvl = eikan_atten(ch) >> 3;
					int ssgc = c >= 6 ? c - 6 : 0;
					wr(s, (uint8_t)(0x08 + ssgc), (uint8_t)(15 - lvl));
				}
			}
			return;
		case 0x90: {
			int off, addr, val;
			if (ch->pc + 1 >= (int)s->file.size()) return;
			off = (int)rd16(&s->file[ch->pc]);
			addr = ch->pc + off;
			val = (addr >= 0 && addr < (int)s->file.size()) ? s->file[addr] : 0;
			if (val == 1)
				ch->pc = ch->pc + off + 3;
			else
				ch->pc += 2;
			return;
		}
		case 0x91:
			if (fetchb(s, ch, &a))
				ch->detune += (int)(int8_t)(a & 0xFF);
			return;
		case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x9A:
			fetchb(s, ch, &a);
			return;
		case 0x97: case 0x98: case 0x99:
			ch->detune = 0;
			return;
		case 0x9B:
			return;
		case 0x9C:
			ch->oct = (ch->oct + 1) & 7;
			return;
		case 0x9D:
			ch->oct = (ch->oct - 1) & 7;
			return;
		case 0x9E:
			if (ch->pc + 1 < (int)s->file.size())
				ch->pc += 2;
			return;
		case 0xA0: case 0xA1: case 0xA2:
			return;
		default:
			if (cmd >= 0x80 && cmd < 0xC0)
				fetchb(s, ch, &a);
			return;
		}
	}

	switch (cmd) {
	case 0x80:
		return;
	case 0x81:
		if (fetchb(s, ch, &a)) {
			ch->def_len = a > 0 ? a : 1;
			ch->wait = ch->def_len;
			if (!dry) keyoff(s, c);
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
	case 0x99:
		fetchb(s, ch, &a);
		return;
	case 0x8C:
	case 0x8E:
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
		if (cmd >= 0x80 && cmd < 0xC0)
			fetchb(s, ch, &a);
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
		if (s->eikan && b < 0x40) {
			ch->wait = ch->def_len > 0 ? ch->def_len : 1;
			play_note(s, c, b, dry);
			return;
		}
		if (s->eikan && b < 0x80) {
			if (!fetchb(s, ch, &ln)) { ch->ended = 1; return; }
			if (ln < 1) ln = 1;
			ch->wait = ln;
			play_note(s, c, b, dry);
			return;
		}
		if (!s->eikan && b < 0x80) {
			if (!fetchb(s, ch, &ln)) { ch->ended = 1; return; }
			if (ln < 1) ln = 1;
			ch->wait = ln;
			ch->def_len = ln;
			play_note(s, c, b, dry);
			return;
		}
		do_cmd(s, c, b, dry, measure);
		if (ch->wait > 0 && (b == 0x81 || (s->eikan && b == 0x8C)))
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
		if (--ch->wait <= 0) {
			if (s->eikan && !dry && ch->keyed) {
				if (!(ch->pc < (int)s->file.size() && s->file[ch->pc] == 0x82))
					keyoff(s, i);
			}
			fetch(s, i, dry, measure);
		} else if (s->eikan && !dry && ch->gate > 0 && ch->wait == ch->gate && ch->keyed) {
			if (!(ch->pc < (int)s->file.size() && s->file[ch->pc] == 0x82))
				keyoff(s, i);
		}
	}
	if (!live) s->ended = 1;
}

static int64_t eikan_irq_clocks(ntl_state *s)
{
	double pit_hz = (double)EIKAN_PIT_CLK / (double)EIKAN_PIT_CNT;
	double music_hz = pit_hz * (double)s->eikan_rate / 65536.0;
	if (music_hz < 1.0) music_hz = 1.0;
	return (int64_t)((double)NTL_CLOCK / music_hz + 0.5);
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
	/* Eikan (HSB3) stores part offsets relative to byte 3; stream at off+3
	 * starts with 0x8F/0x96. Sekigahara uses absolute off to the stream. */
	{
		/* Eikan part offs point 3 bytes before the stream. Stream usually
		 * starts with 0x8F/0x96. Do NOT require d[o0]<0x80 — title/SJIS
		 * padding there often has high bytes (broke song 001 etc). */
		int o0 = (cnt > 0 && 6 < (int)n) ? (int)rd16(d + 4) : 0;
		int at3 = (o0 > 0 && o0 + 3 < (int)n) ? d[o0 + 3] : 0;
		s->eikan = (at3 == 0x8F || at3 == 0x96) ? 1 : 0;
	}
	memset(s->pssg, 0, sizeof s->pssg);
	memset(s->pcond, 0, sizeof s->pcond);
	for (i = 0; i < cnt && 4 + (i + 1) * 3 <= (int)n; ++i) {
		int cid, hi, lo, slot;
		off = (int)rd16(d + 4 + i * 3);
		typ = d[4 + i * 3 + 2];
		if (s->eikan) off += 3;
		if (off > 0 && off < (int)n && noff < 64)
			offs[noff++] = off;
		if (off <= 0 || off >= (int)n) continue;
		if (s->eikan) {
			/* typ&0x80 = conductor flag; typ&0x7f = ch id (10-15 FM, 20-22 SSG) */
			cid = typ & 0x7F;
			hi = cid & 0xF0;
			lo = cid & 0x0F;
			if (hi == 0x10 && lo < 6)
				slot = lo;
			else if (hi == 0x20 && lo < 3)
				slot = 6 + lo;
			else
				continue;
			s->part[slot] = off;
			s->pssg[slot] = (hi == 0x20) ? 1 : 0;
			s->pcond[slot] = (typ & 0x80) ? 1 : 0;
		} else if (typ >= 0x10 && typ <= 0x15 && fm < 6) {
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
	/* Eikan FM voice bank sits right after the part table:
	 * [n_fm][off16][n_ssg][off16]; off relative to byte 3; each voice is
	 * idx_byte + 32 data bytes. */
	if (s->eikan) {
		int bx = 4 + cnt * 3;
		int n_fm, off_fm, n_ssg, off_ssg, j, idx, src;
		memset(s->voice_ok, 0, sizeof s->voice_ok);
		memset(s->voice_ssg_ok, 0, sizeof s->voice_ssg_ok);
		if (bx + 6 <= (int)n) {
			n_fm = d[bx];
			off_fm = (int)rd16(d + bx + 1);
			src = 3 + off_fm;
			for (j = 0; j < n_fm && src + 33 <= (int)n; ++j) {
				idx = d[src++] & 31;
				memcpy(s->voices[idx], d + src, 32);
				s->voice_ok[idx] = 1;
				src += 32;
			}
			n_ssg = d[bx + 3];
			off_ssg = (int)rd16(d + bx + 4);
			src = 3 + off_ssg;
			for (j = 0; j < n_ssg && src + 17 <= (int)n; ++j) {
				idx = d[src++] & 15;
				memcpy(s->voices_ssg[idx], d + src, 16);
				s->voice_ssg_ok[idx] = 1;
				src += 16;
			}
		}
	}
	{
		int any = 0;
		for (i = 0; i < NTL_CH; ++i)
			if (s->part[i] > 0) any++;
		/* eikan path never bumps fm/ssg; count assigned slots */
		return any > 0 ? 0 : -1;
	}
}

static void reset_play(ntl_state *s, int dry)
{
	int i;
	if (!s->file0.empty())
		s->file = s->file0; /* restore 0x8A counts */
	memset(s->ch, 0, sizeof s->ch);
	s->ended = 0;
	s->irq_acc = 0;
	s->chip_pos = 0;
	s->play_samples = 0;
	s->last_l = s->last_r = 0;
	s->tb = 0xC0;
	s->ssg_mix = s->eikan ? 0x3F : 0xB8;
	s->eikan_rate = 0x5000;
	s->eikan_acc = 0;
	s->song_looped = 0;
	s->eikan_pit_acc = 0;
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
		s->ch[i].vol = s->eikan ? 0 : 10; /* Eikan 8F: 0=loudest */
		s->ch[i].vol_b = 0;
		s->ch[i].def_len = s->eikan ? 0x30 : 12;
		s->ch[i].gate = 0;
		s->ch[i].eff = 0;
		s->ch[i].voice = 0;
		if (!s->ch[i].ssg && !s->ch[i].conductor && !dry) {
			if (s->eikan)
				apply_voice(s, i, s->ch[i].voice, eikan_atten(&s->ch[i]));
			else
				apply_def(s, i, s->ch[i].vol);
		}
	}
}

static int measure_ms(ntl_state *s)
{
	int ticks = 0;
	int64_t us = 0;
	reset_play(s, 1);
	while (!s->ended && ticks < (s->eikan ? EIKAN_MAX_MEAS : NTL_MAX_TICKS)) {
		int i, live = 0, looped = 0, en = 0;
		if (s->eikan)
			us += eikan_irq_clocks(s);
		else
			us += (int64_t)tb_clocks(s->tb);
		irq(s, 1, 1);
		for (i = 0; i < NTL_CH; ++i) {
			if (!s->ch[i].enabled) continue;
			en++;
			if (!s->ch[i].ended) live++;
			if (s->ch[i].did_loop) looped++;
		}
		if (en > 0 && looped >= en) s->ended = 1;
		if (s->eikan && s->song_looped) s->ended = 1;
		if (!live) s->ended = 1;
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
	if (filename && (strstr(filename, "eikan") || strstr(filename, "EIKAN") ||
			strstr(filename, "MUSIC.PAC") || strstr(filename, "music.pac")))
		pc98_bounded(game, gcap, "Eikan wa Kimi ni 3");
}

static int setup(ntl_state *s, const char *filename, const uint8_t *data,
		size_t len, const pc98_cfg *cfg)
{
	uint32_t sr;
	if (!data || !pc98_looks_ntl(data, len)) return -1;
	s->file.assign(data, data + len);
	s->file0 = s->file;
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
	s->eikan_pit_step = ((int64_t)s->rate << 16) * (int64_t)EIKAN_PIT_CNT / (int64_t)EIKAN_PIT_CLK;
	if (s->eikan_pit_step < 1) s->eikan_pit_step = 1;
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
	pc98_bounded(out->engine, sizeof out->engine,
			tmp.eikan ? "ymfm ArtDink NTL (Eikan)" : "ymfm ArtDink NTL");
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
		if (s->eikan) {
			s->eikan_pit_acc += 0x10000;
			while (s->eikan_pit_acc >= s->eikan_pit_step) {
				int sum;
				s->eikan_pit_acc -= s->eikan_pit_step;
				sum = s->eikan_acc + s->eikan_rate;
				if (sum > 0xFFFF)
					irq(s, 0, 0);
				s->eikan_acc = sum & 0xFFFF;
			}
		} else {
			need = (int64_t)s->rate * tb_clocks(s->tb);
			s->irq_acc += (int64_t)NTL_CLOCK;
			while (s->irq_acc >= need) {
				irq(s, 0, 0);
				s->irq_acc -= need;
				need = (int64_t)s->rate * tb_clocks(s->tb);
			}
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

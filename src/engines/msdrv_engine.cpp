/*
 * MsDRV 4.x light (.MS) — KENJI (KAKERA).
 * Spec: MST4_5A + ValleyBell MsDRV_SeqFormat.
 * Ekudorado: _N OPN, _B2 OPNA, _GS/_88 MIDI GS (SC-88), _SB OPL3.
 * Banks: sibling {prefix}.OPN / {prefix}.SSG (48-byte / 16-byte programs).
 */
#include "msdrv_engine.h"
#include "pc98_util.h"
#include "fmd_engine.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>
#include <vector>

#include "ymfm.h"
#include "ymfm_opn.h"
#include "ymfm_opl.h"
#include "tsf.h"

#define MS_TRK       36
#define MS_HDR       0xA0
#define MS_CLOCK_OPNA 7987200u
#define MS_CLOCK_OPN  3993600u
#define MS_CLOCK     MS_CLOCK_OPNA
#define MS_MAX_TICKS 800000
#define MS_NEST      8
#define MS_OPN_N     256
#define MS_OPN_SZ    48
#define MS_SSG_SZ    16
#define MS_OPL_CLOCK 14318181u
static const uint8_t k_opl_slot[9] = { 0,1,2,8,9,10,16,17,18 };

enum { CK_NONE=0, CK_FM, CK_SSG, CK_MIDI, CK_OPL };

static const int k_op[4] = { 0, 4, 8, 12 }; /* MSDRV4L writes file order to HW op1..op4 ( empirically from Unicorn capture) */
static const uint8_t k_car[8] = {
	0x08, 0x08, 0x08, 0x08, 0x0A, 0x0E, 0x0E, 0x0F
};
static const int k_fnum[12] = {
	0x269, 0x28E, 0x2B4, 0x2DE, 0x30A, 0x338,
	0x369, 0x39C, 0x3D3, 0x40E, 0x44B, 0x48D
};

class ms_iface : public ymfm::ymfm_interface {
public:
	const uint8_t *rom;
	size_t rom_n;
	ms_iface() : rom(NULL), rom_n(0) {}
	uint8_t ymfm_external_read(ymfm::access_class type, uint32_t address) override
	{
		if (type == ymfm::ACCESS_ADPCM_A && rom && address < rom_n)
			return rom[address];
		return 0;
	}
};

struct ms_trk {
	int active, ended;
	int pc, base, wait, gate_left;
	int note3;          /* 1 = 3-byte notes */
	int ch_id, kind, hw;
	int vol, inst, pan, detune;
	int keyed, cur_note;
	int loop_sp, loop_pc[MS_NEST], loop_cnt[MS_NEST];
	int sub_sp, sub_ret[MS_NEST], sub_end[MS_NEST];
	int pb, pb_step, pb_dst;
	int looped_inf;
};

struct ms_state {
	std::vector<uint8_t> file, opn, ssg;
	int trk_off[MS_TRK];
	ms_trk trk[MS_TRK];
	ms_iface iface;
	ymfm::ym2608 *opna;
	ymfm::ym2608::output_data out;
	ymfm::ym2203 *opn_chip; /* true YM2203 for _N (mono) */
	ymfm::ym2203::output_data opn_out;
	ymfm::ymf262 *opl;
	ymfm::ymf262::output_data opl_out;
	tsf *sf;
	int sf_owned;
	int rate, loops_want, one_loop_ms, ended, song_ended;
	int tempo, timebase, tempo_mod;
	int mute_fm, mute_ssg, mute_rhythm;
	int64_t chip_pos, chip_step, tick_acc;
	int last_l, last_r;
	int prev_l, prev_r;          /* linear-resample history */
	uint32_t chip_clock;         /* YM2203 3.9936M or YM2608 7.9872M */
	int skip_audio;              /* seek: advance seq+regs, skip generate */
	uint8_t sh_opn[2][256];
	uint8_t sh_opn_set[2][256];
	uint8_t sh_opl[2][256];
	uint8_t sh_opl_set[2][256];
	/* Fixed-quantum TSF FIFO so host chunk size cannot change float order. */
	float tsf_fifo[64 * 2];
	int tsf_fifo_pos, tsf_fifo_len;
	uint32_t play_samples, play_limit;
	uint8_t ssg_mix;
	int variant; /* 0 OPN, 1 OPNA, 2 MIDI, 3 OPL */
	int loop_hit;
	int saw_inf_loop;
	int ch3_special;             /* A9: OPN/OPNA CH3 special/effect mode */
	int mono;                    /* OPN: force L=R */
	/* Software ADPCM-A rhythm (when ROM missing) — 6 voices from 2608_*.WAV */
	struct { std::vector<int16_t> pcm; int rate; int pos, step, end, on, vol; } rhy[6];
	int rhy_have_wav;
	std::vector<uint8_t> rhy_rom;
	/* Optional register log for tools/msdrv_reglog */
	FILE *reg_log;
	uint32_t reg_tick;
	char title[256];

	ms_state()
		: opna(NULL), opn_chip(NULL), opl(NULL), sf(NULL), sf_owned(0), rate(PC98_DEFAULT_RATE), loops_want(1),
		  one_loop_ms(0), ended(0), song_ended(0), tempo(120), timebase(48),
		  tempo_mod(0x40), mute_fm(0), mute_ssg(0), mute_rhythm(0),
		  chip_pos(0), chip_step(0), tick_acc(0), last_l(0), last_r(0),
		  prev_l(0), prev_r(0), chip_clock(MS_CLOCK_OPNA), skip_audio(0),
		  tsf_fifo_pos(0), tsf_fifo_len(0),
		  play_samples(0), play_limit(0), ssg_mix(0x38), variant(0), loop_hit(0), saw_inf_loop(0), ch3_special(0), mono(0), rhy_have_wav(0), reg_log(NULL), reg_tick(0)
	{
		memset(trk_off, 0, sizeof trk_off);
		memset(trk, 0, sizeof trk);
		memset(sh_opn_set, 0, sizeof sh_opn_set);
		memset(sh_opl_set, 0, sizeof sh_opl_set);
		title[0] = 0;
	}
};

static void rhy_hit(ms_state *s, uint8_t mask);
static void rhy_mix(ms_state *s, int *l, int *r);
static void load_rhythm(ms_state *s, const pc98_cfg *cfg);

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int16_t rd16s(const uint8_t *p)
{
	return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

int msdrv_probe_mem(const uint8_t *data, size_t len)
{
	uint32_t off[MS_TRK], eof;
	int i, used = 0;
	if (!data || len < MS_HDR) return 0;
	for (i = 0; i < MS_TRK; i++) {
		off[i] = rd32(data + i * 4);
		if (off[i]) {
			if (off[i] < MS_HDR || off[i] >= len) return 0;
			used++;
		}
	}
	if (used < 1) return 0;
	if (rd32(data + 0x90) || rd32(data + 0x94) || rd32(data + 0x98))
		return 0;
	eof = rd32(data + 0x9C);
	if (eof < MS_HDR || eof > len + 16) return 0;
	for (i = 0; i < MS_TRK; i++) {
		uint8_t c;
		if (!off[i]) continue;
		c = data[off[i]];
		if (c == 0x8B || c == 0x8A || c == 0xE6 || c == 0x82 ||
		    c == 0x9C || c == 0xFE || c == 0x85 || c == 0x80 || c < 0x80)
			return 1;
		return 0;
	}
	return 0;
}

static int kind_of(int id)
{
	if (id >= 0x40 && id <= 0x42) return CK_SSG;
	if (id >= 0x50 && id <= 0x55) return CK_FM;
	if (id >= 0x00 && id <= 0x1F) return CK_MIDI;
	if (id >= 0x70 && id <= 0x81) return CK_OPL;
	if (id >= 0xF0 && id <= 0xF2) return CK_FM;
	return CK_NONE;
}
static int hw_of(int id, int kind)
{
	if (kind == CK_SSG) return id - 0x40;
	if (kind == CK_FM) {
		if (id >= 0x50 && id <= 0x55) return id - 0x50;
		return 2;
	}
	if (kind == CK_MIDI) return id & 15;
	if (kind == CK_OPL) return id - 0x70;
	return 0;
}

static void log_reg(ms_state *s, const char *chip, int port, uint8_t aa, uint8_t dd)
{
	if (!s->reg_log) return;
	fprintf(s->reg_log, "%u %s %d %02X %02X\n", s->reg_tick, chip, port, aa, dd);
}

static void wrx(ms_state *s, int ext, uint8_t aa, uint8_t dd)
{
	int p = ext ? 1 : 0;
	s->sh_opn[p][aa] = dd;
	s->sh_opn_set[p][aa] = 1;
	if (s->skip_audio) {
		log_reg(s, s->opn_chip ? "OPN" : "OPNA", p, aa, dd);
		return;
	}
	if (s->opn_chip) {
		if (ext) return; /* YM2203 has no extended port */
		log_reg(s, "OPN", 0, aa, dd);
		s->opn_chip->write(0, aa);
		s->opn_chip->write(1, dd);
		return;
	}
	if (!s->opna) return;
	log_reg(s, "OPNA", p, aa, dd);
	if (ext) { s->opna->write(2, aa); s->opna->write(3, dd); }
	else { s->opna->write(0, aa); s->opna->write(1, dd); }
}
static void wr(ms_state *s, uint8_t aa, uint8_t dd)
{
	wrx(s, 0, aa, dd);
}

static void apply_fm(ms_state *s, ms_trk *t)
{
	const uint8_t *pat;
	int c = t->hw, ext, slot, i, alg, mask, atten, tl, pan;
	if ((!s->opna && !s->opn_chip) || c < 0 || c > 5) return;
	if (s->opn_chip && c > 2) return; /* YM2203: 3 FM only */
	if (s->opn.size() < (size_t)(t->inst + 1) * MS_OPN_SZ) return;
	pat = &s->opn[(size_t)t->inst * MS_OPN_SZ];
	if (pat[0x2F] != 0) return;
	ext = c >= 3;
	slot = ext ? c - 3 : c;
	alg = pat[0] & 7;
	mask = k_car[alg];
	atten = 0x7F - (t->vol & 0x7F);
	if (atten < 0) atten = 0;
	for (i = 0; i < 4; i++) {
		int ro = k_op[i];
		wrx(s, ext, (uint8_t)(0x30 + slot + ro), pat[1 + i]);
		tl = pat[5 + i] & 0x7F;
		if (mask & (1 << i)) { tl += atten; if (tl > 0x7F) tl = 0x7F; }
		wrx(s, ext, (uint8_t)(0x40 + slot + ro), (uint8_t)tl);
		wrx(s, ext, (uint8_t)(0x50 + slot + ro), pat[9 + i]);
		wrx(s, ext, (uint8_t)(0x60 + slot + ro), pat[0xD + i]);
		wrx(s, ext, (uint8_t)(0x70 + slot + ro), pat[0x11 + i]);
		wrx(s, ext, (uint8_t)(0x80 + slot + ro), pat[0x15 + i]);
	}
	wrx(s, ext, (uint8_t)(0xB0 + slot), (uint8_t)(pat[0] & 0x3F));
	/* 9F raw: 00=centre(C0), 01..7F=right(40), 80..FF=left(80).
	 * OPN (variant 0) is mono — always both LR enables. */
	if (s->variant == 0)
		pan = 0xC0;
	else {
		int pp = t->pan & 0xFF;
		if (pp == 0) pan = 0xC0;
		else if (pp & 0x80) pan = 0x80;
		else pan = 0x40;
	}
	pan |= (pat[0x23] & 0x37);
	wrx(s, ext, (uint8_t)(0xB4 + slot), (uint8_t)pan);
}

static int fm_key(int hw)
{
	return hw <= 2 ? hw : (hw - 3 + 4);
}

static void fm_off(ms_state *s, ms_trk *t)
{
	if (t->kind != CK_FM || !t->keyed) return;
	wr(s, 0x28, (uint8_t)fm_key(t->hw));
	t->keyed = 0;
}
static void fm_on(ms_state *s, ms_trk *t, int note)
{
	int n, blk, fn, ext, slot;
	int fnote = note + (t->pb / 256);
	if (t->kind != CK_FM) return;
	if (fnote < 0) fnote = 0;
	if (fnote > 127) fnote = 127;
	n = fnote % 12;
	/* k_fnum is calibrated for ~4 MHz (26K YM2203). Hardware:
	 * Fout = clock/144 * FNUM * 2^(BLOCK-1) / 2^20.
	 * OPN @ 3.9936 MHz → BLOCK = note/12 - 1; OPNA @ 7.9872 MHz → note/12 - 2. */
	blk = fnote / 12 - (s->variant == 0 ? 1 : 2);
	if (blk < 0) blk = 0;
	if (blk > 7) blk = 7;
	fn = k_fnum[n] + t->detune;
	if (fn < 0) fn = 0;
	if (fn > 0x7FF) fn = 0x7FF;
	/* Special CH3 (A9): F0/F1/F2 write extension fnum; key-on CH3. */
	if (s->ch3_special && t->ch_id >= 0xF0 && t->ch_id <= 0xF2) {
		static const uint8_t k_ext_hi[3] = { 0xAD, 0xAE, 0xAC }; /* op slots */
		static const uint8_t k_ext_lo[3] = { 0xA9, 0xAA, 0xA8 };
		int si = t->ch_id - 0xF0;
		if (t->keyed) fm_off(s, t);
		wr(s, k_ext_hi[si], (uint8_t)((blk << 3) | ((fn >> 8) & 7)));
		wr(s, k_ext_lo[si], (uint8_t)fn);
		wr(s, 0x28, (uint8_t)(0xF0 | 2)); /* key-on FM3 */
		t->keyed = 1;
		t->cur_note = note;
		t->hw = 2;
		return;
	}
	ext = t->hw >= 3;
	slot = ext ? t->hw - 3 : t->hw;
	if (t->keyed) fm_off(s, t);
	wrx(s, ext, (uint8_t)(0xA4 + slot), (uint8_t)((blk << 3) | ((fn >> 8) & 7)));
	wrx(s, ext, (uint8_t)(0xA0 + slot), (uint8_t)fn);
	wr(s, 0x28, (uint8_t)(0xF0 | fm_key(t->hw)));
	t->keyed = 1;
	t->cur_note = note;
}

static void ssg_off(ms_state *s, ms_trk *t)
{
	if (t->kind != CK_SSG || !t->keyed) return;
	wr(s, (uint8_t)(0x08 + t->hw), 0);
	t->keyed = 0;
}
static void ssg_on(ms_state *s, ms_trk *t, int note)
{
	double freq, period;
	int per, vol, c = t->hw;
	int fnote = note + (t->pb / 256);
	if (t->kind != CK_SSG || c < 0 || c > 2) return;
	if (fnote < 0) fnote = 0;
	if (fnote > 127) fnote = 127;
	freq = 440.0 * pow(2.0, (fnote - 69) / 12.0);
	/* YM2608 SSG runs at master/2; YM2203 SSG at master. Both yield ~4 MHz. */
	{
		double ssg_clk = (s->variant == 0) ? (double)s->chip_clock
						   : ((double)s->chip_clock / 2.0);
		period = ssg_clk / (16.0 * freq);
	}
	per = (int)(period + 0.5) + t->detune;
	if (per < 1) per = 1;
	if (per > 0xFFF) per = 0xFFF;
	vol = (t->vol & 0x7F) / 8;
	if (vol > 15) vol = 15;
	wr(s, (uint8_t)(c * 2), (uint8_t)(per & 0xFF));
	wr(s, (uint8_t)(c * 2 + 1), (uint8_t)((per >> 8) & 0x0F));
	wr(s, (uint8_t)(0x08 + c), (uint8_t)vol);
	s->ssg_mix = (uint8_t)((s->ssg_mix & ~(1 << c)) | (8 << c));
	wr(s, 0x07, s->ssg_mix);
	t->keyed = 1;
	t->cur_note = note;
}


static void opl_wr(ms_state *s, int port, uint8_t reg, uint8_t val)
{
	int p = port ? 1 : 0;
	s->sh_opl[p][reg] = val;
	s->sh_opl_set[p][reg] = 1;
	log_reg(s, "OPL3", p, reg, val);
	if (!s->opl || s->skip_audio) return;
	s->opl->write((uint32_t)(port ? 2 : 0), reg);
	s->opl->write((uint32_t)(port ? 3 : 1), val);
}


static void sf_silence(tsf *sf)
{
	int c;
	if (!sf) return;
	for (c = 0; c < 16; c++)
		tsf_channel_sounds_off_all(sf, c);
	tsf_note_off_all(sf);
	tsf_reset(sf);
}

static void reset_opl(ms_state *s)
{
	int i;
	if (!s->opl) return;
	s->opl->reset();
	/* Enable OPL3 mode + wave select */
	opl_wr(s, 1, 0x05, 0x01); /* OPL3 NEW */
	opl_wr(s, 0, 0x01, 0x20); /* waveform select enable */
	opl_wr(s, 0, 0x08, 0x00);
	opl_wr(s, 0, 0xBD, 0x00); /* no OPL rhythm / percussion mode */
	for (i = 0; i < 9; i++) {
		opl_wr(s, 0, (uint8_t)(0xB0 + i), 0);
		opl_wr(s, 1, (uint8_t)(0xB0 + i), 0);
		opl_wr(s, 0, (uint8_t)(0xA0 + i), 0);
		opl_wr(s, 1, (uint8_t)(0xA0 + i), 0);
		opl_wr(s, 0, (uint8_t)(0xC0 + i), 0x30);
		opl_wr(s, 1, (uint8_t)(0xC0 + i), 0x30);
	}
}

static void apply_opl(ms_state *s, ms_trk *t)
{
	const uint8_t *v;
	int port, ch, sm, sc, vol_tl, i;
	uint8_t fb_cnt, pan;
	if (!s->opl || t->kind != CK_OPL || t->hw < 0 || t->hw > 17) return;
	if (s->opn.size() < (size_t)(t->inst + 1) * MS_OPN_SZ) return;
	v = s->opn.data() + t->inst * MS_OPN_SZ;
	/* Type byte at +0x2F: 0=OPN, 1=OPL. Refuse OPN patches on OPL (garbage/hiss). */
	if (v[0x2F] != 1) return;
	port = t->hw >= 9 ? 1 : 0;
	ch = t->hw % 9;
	sm = k_opl_slot[ch];
	sc = sm + 3;
	/* MST: fb in D5..D3, cnt in D0. OPL 0xC0 wants FB in D3..D1, CNT in D0,
	 * plus stereo enables in D5..D4 for OPL3. */
	fb_cnt = (uint8_t)((((v[0] & 0x38) >> 2) & 0x0E) | (v[0] & 0x01));
	pan = 0x30;
	opl_wr(s, port, (uint8_t)(0xC0 + ch), (uint8_t)(pan | fb_cnt));
	for (i = 0; i < 2; i++) {
		int slot = i ? sc : sm;
		int o = i; /* op1 / op2 in file */
		uint8_t mult = v[1 + o] & 0x0F;
		/* MST packs TL in D6..D1 (D0 unused) — shift into OPL's 6-bit TL. */
		uint8_t tl = (uint8_t)((v[5 + o] >> 1) & 0x3F);
		uint8_t ks_ar = v[9 + o];
		uint8_t am_dr = v[0x0D + o];
		uint8_t vib_ws = v[0x11 + o];
		uint8_t sl_rr = v[0x15 + o];
		/* File: vib@D5 egt@D4 ksr@D3 ws@D2..0 → OPL 0x20: AM VIB EGT KSR MULT */
		uint8_t r20 = (uint8_t)((am_dr & 0x80) |
			((vib_ws & 0x20) ? 0x40 : 0) |
			((vib_ws & 0x10) ? 0x20 : 0) |
			((vib_ws & 0x08) ? 0x10 : 0) | mult);
		/* Volume: attenuate carrier (op2) only; keep some headroom. */
		vol_tl = tl;
		if (i == 1) {
			int add = (127 - (t->vol & 127)) * 48 / 127;
			vol_tl = tl + add;
			if (vol_tl > 0x3F) vol_tl = 0x3F;
		}
		uint8_t r40 = (uint8_t)((ks_ar & 0xC0) | (vol_tl & 0x3F));
		/* AR in D4..D1 of ks_ar; DR in D4..D1 of am_dr (D0 unused each). */
		uint8_t r60 = (uint8_t)((((ks_ar >> 1) & 0x0F) << 4) | ((am_dr >> 1) & 0x0F));
		uint8_t r80 = sl_rr;
		uint8_t rE0 = (uint8_t)(vib_ws & 0x07);
		opl_wr(s, port, (uint8_t)(0x20 + slot), r20);
		opl_wr(s, port, (uint8_t)(0x40 + slot), r40);
		opl_wr(s, port, (uint8_t)(0x60 + slot), r60);
		opl_wr(s, port, (uint8_t)(0x80 + slot), r80);
		opl_wr(s, port, (uint8_t)(0xE0 + slot), rE0);
	}
}

static void opl_off(ms_state *s, ms_trk *t)
{
	int port, ch;
	if (!s->opl || t->kind != CK_OPL || !t->keyed) return;
	port = t->hw >= 9 ? 1 : 0;
	ch = t->hw % 9;
	/* Drop KEYON; zeroing A0/B0 is fine for our purposes. */
	opl_wr(s, port, (uint8_t)(0xB0 + ch), 0);
	t->keyed = 0;
}

static void opl_on(ms_state *s, ms_trk *t, int note)
{
	int port, ch, fnote, block, fnum;
	double freq;
	if (!s->opl || t->kind != CK_OPL || t->hw < 0 || t->hw > 17) return;
	apply_opl(s, t);
	port = t->hw >= 9 ? 1 : 0;
	ch = t->hw % 9;
	fnote = note + (t->pb / 256);
	if (fnote < 0) fnote = 0;
	if (fnote > 127) fnote = 127;
	/* OPL3: Fnum = freq * 2^(20-block) / (clock/288). block ≈ note/12 - 1. */
	block = fnote / 12 - 1;
	if (block < 0) block = 0;
	if (block > 7) block = 7;
	freq = 440.0 * pow(2.0, (fnote - 69) / 12.0);
	fnum = (int)(freq * (double)(1 << (20 - block)) / (MS_OPL_CLOCK / 288.0) + 0.5);
	if (fnum < 0) fnum = 0;
	if (fnum > 0x3FF) fnum = 0x3FF;
	opl_wr(s, port, (uint8_t)(0xA0 + ch), (uint8_t)(fnum & 0xFF));
	opl_wr(s, port, (uint8_t)(0xB0 + ch),
		(uint8_t)(0x20 | ((block & 7) << 2) | ((fnum >> 8) & 3)));
	t->keyed = 1;
	t->cur_note = note;
}

static void midi_off(ms_state *s, ms_trk *t)
{
	if (!s->sf || t->kind != CK_MIDI || !t->keyed) return;
	if (!s->skip_audio)
		tsf_channel_note_off(s->sf, t->hw, t->cur_note);
	t->keyed = 0;
}
static void midi_on(ms_state *s, ms_trk *t, int note, int vel)
{
	if (!s->sf || t->kind != CK_MIDI) return;
	if (t->keyed) midi_off(s, t);
	if (vel <= 0) vel = t->vol;
	if (vel <= 0) return;
	if (!s->skip_audio)
		tsf_channel_note_on(s->sf, t->hw, note & 127, (vel & 0x7F) / 127.f);
	t->keyed = 1;
	t->cur_note = note & 127;
}

static void note_off(ms_state *s, ms_trk *t)
{
	if (t->kind == CK_FM) fm_off(s, t);
	else if (t->kind == CK_SSG) ssg_off(s, t);
	else if (t->kind == CK_MIDI) midi_off(s, t);
	else if (t->kind == CK_OPL) opl_off(s, t);
	t->gate_left = 0;
}
static void note_on(ms_state *s, ms_trk *t, int note, int gate, int vel)
{
	if (note == 0 || gate == 0) { note_off(s, t); return; }
	if (vel >= 0) t->vol = vel;
	if (t->kind == CK_FM) {
		if (s->mute_fm) { note_off(s, t); return; }
		apply_fm(s, t);
		fm_on(s, t, note);
	} else if (t->kind == CK_SSG) {
		if (s->mute_ssg) { note_off(s, t); return; }
		ssg_on(s, t, note);
	} else if (t->kind == CK_MIDI) {
		midi_on(s, t, note, vel >= 0 ? vel : t->vol);
	} else if (t->kind == CK_OPL) {
		opl_on(s, t, note);
	}
	t->gate_left = gate;
}

static void trk_step(ms_state *s, int ti, int dry)
{
	ms_trk *t = &s->trk[ti];
	const uint8_t *d = s->file.data();
	size_t n = s->file.size();
	int pc, cmd;
	if (!t->active || t->ended) return;
	if (t->wait > 0) {
		t->wait--;
		if (t->gate_left > 0) {
			t->gate_left--;
			if (t->gate_left == 0 && !dry) note_off(s, t);
		}
		if (!dry && t->pb_step) {
			if (t->pb < t->pb_dst) {
				t->pb += t->pb_step;
				if (t->pb > t->pb_dst) t->pb = t->pb_dst;
			} else if (t->pb > t->pb_dst) {
				t->pb -= t->pb_step;
				if (t->pb < t->pb_dst) t->pb = t->pb_dst;
			}
		}
		return;
	}
	pc = t->pc;
	if (pc < 0 || (size_t)pc >= n) { t->ended = 1; return; }
	if (t->sub_sp > 0 && pc >= t->sub_end[t->sub_sp - 1]) {
		t->pc = t->sub_ret[--t->sub_sp];
		return;
	}
	cmd = d[pc];
	if (cmd < 0x80) {
		int delay, gate, vel = -1;
		if (t->note3) {
			if ((size_t)pc + 3 > n) { t->ended = 1; return; }
			delay = d[pc + 1]; gate = d[pc + 2]; t->pc = pc + 3;
		} else {
			if ((size_t)pc + 4 > n) { t->ended = 1; return; }
			delay = d[pc + 1]; gate = d[pc + 2]; vel = d[pc + 3];
			t->pc = pc + 4;
		}
		if (!dry) note_on(s, t, cmd, gate, vel);
		else t->gate_left = gate;
		t->wait = delay;
		return;
	}
	switch (cmd) {
	case 0x80:
		if ((size_t)pc + 3 > n) { t->ended = 1; return; }
		s->timebase = d[pc + 1] | (d[pc + 2] << 8);
		if (s->timebase <= 0) s->timebase = 48;
		t->pc = pc + 3; break;
	case 0x81: {
		uint8_t mode, reg, val;
		if ((size_t)pc + 4 > n) { t->ended = 1; return; }
		mode = d[pc + 1]; reg = d[pc + 2]; val = d[pc + 3];
		if (!dry && s->opl) {
			/* Songs use mode 00 (seen as enable/test) and 80/81 for ports. */
			if (mode == 0x00 || mode == 0x80) opl_wr(s, 0, reg, val);
			else if (mode == 0x81) opl_wr(s, 1, reg, val);
		}
		t->pc = pc + 4; break;
	}
	case 0x82:
		if ((size_t)pc + 2 > n) { t->ended = 1; return; }
		t->inst = d[pc + 1];
		if (!dry && t->kind == CK_FM) apply_fm(s, t);
		if (!dry && t->kind == CK_OPL) apply_opl(s, t);
		if (!dry && !s->skip_audio && t->kind == CK_MIDI && s->sf)
			tsf_channel_set_presetnumber(s->sf, t->hw, t->inst & 127, t->hw == 9);
		t->pc = pc + 2; break;
	case 0x83: {
		uint32_t st, en;
		if ((size_t)pc + 9 > n) { t->ended = 1; return; }
		st = rd32(d + pc + 1); en = rd32(d + pc + 5);
		t->pc = pc + 9;
		if (t->sub_sp < MS_NEST) {
			t->sub_ret[t->sub_sp] = t->pc;
			t->sub_end[t->sub_sp] = t->base + (int)en;
			t->sub_sp++;
			t->pc = t->base + (int)st;
		}
		break;
	}
	case 0x84:
		if (t->sub_sp > 0) t->pc = t->sub_ret[--t->sub_sp];
		else if ((size_t)pc + 3 <= n) t->pc = pc + rd16s(d + pc + 1);
		else t->ended = 1;
		break;
	case 0x85:
		if ((size_t)pc + 2 > n) { t->ended = 1; return; }
		t->vol = d[pc + 1] & 0x7F;
		if (!dry && !s->skip_audio && t->kind == CK_MIDI && s->sf)
			tsf_channel_midi_control(s->sf, t->hw, 7, t->vol);
		if (!dry && t->keyed && t->kind == CK_FM) apply_fm(s, t);
		if (!dry && t->keyed && t->kind == CK_SSG) {
			int v = t->vol / 8; if (v > 15) v = 15;
			wr(s, (uint8_t)(0x08 + t->hw), (uint8_t)v);
		}
		t->pc = pc + 2; break;
	case 0x8A:
		if ((size_t)pc + 2 > n) { t->ended = 1; return; }
		s->tempo = d[pc + 1]; if (s->tempo <= 0) s->tempo = 120;
		t->pc = pc + 2; break;
	case 0x8B:
		if ((size_t)pc + 2 > n) { t->ended = 1; return; }
		t->note3 = (d[pc + 1] == 1);
		t->pc = pc + 2; break;
	case 0x8C: case 0x8E: t->pc = pc + 4; break;
	case 0x8D: case 0x8F:
		if ((size_t)pc + 4 > n) { t->ended = 1; return; }
		t->pc = pc + 4 + d[pc + 3]; break;
	case 0x91: case 0x9E: case 0xC2: case 0xC4: t->pc = pc + 1; break;
	case 0x94:
		if ((size_t)pc + 3 > n) { t->ended = 1; return; }
		if (!dry) wr(s, d[pc + 1], d[pc + 2]);
		t->pc = pc + 3; break;
	case 0x96: t->pc = pc + 3; break;
	case 0x9B: {
		int cnt;
		if ((size_t)pc + 2 > n) { t->ended = 1; return; }
		cnt = d[pc + 1];
		if (t->loop_sp <= 0) { t->pc = pc + 2; break; }
		if (cnt == 0) {
			t->pc = t->loop_pc[t->loop_sp - 1];
			s->saw_inf_loop = 1; t->looped_inf = 1;
		} else {
			int idx = t->loop_sp - 1;
			if (t->loop_cnt[idx] == 0) t->loop_cnt[idx] = cnt;
			t->loop_cnt[idx]--;
			if (t->loop_cnt[idx] > 0) t->pc = t->loop_pc[idx];
			else { t->loop_sp--; t->pc = pc + 2; }
		}
		break;
	}
	case 0x9C:
		if (t->loop_sp < MS_NEST) {
			t->loop_pc[t->loop_sp] = pc + 1;
			t->loop_cnt[t->loop_sp] = 0;
			t->loop_sp++;
		}
		t->pc = pc + 1; break;
	case 0x9D:
		if ((size_t)pc + 2 > n) { t->ended = 1; return; }
		t->detune = (int8_t)d[pc + 1];
		t->pc = pc + 2; break;
	case 0x9F:
		if ((size_t)pc + 2 > n) { t->ended = 1; return; }
		t->pan = d[pc + 1];
		if (!dry && !s->skip_audio && t->kind == CK_MIDI && s->sf)
			tsf_channel_midi_control(s->sf, t->hw, 10, ((t->pan + 0x80) & 0xFF) >> 1);
		t->pc = pc + 2; break;
	case 0xA4:
		if ((size_t)pc + 3 > n) { t->ended = 1; return; }
		t->pb = (int16_t)(d[pc + 1] | (d[pc + 2] << 8));
		t->pc = pc + 3; break;
	case 0xA5: case 0xA6: case 0xA8: case 0xAA:
	case 0xB0: case 0xB1:
		t->pc = pc + 2; break;
	case 0xA9:
		if ((size_t)pc + 2 > n) { t->ended = 1; return; }
		s->ch3_special = d[pc + 1] ? 1 : 0;
		if (!dry && (s->opna || s->opn_chip)) {
			/* bit6 of 0x27 enables CH3 special / effect mode */
			wr(s, 0x27, (uint8_t)(s->ch3_special ? 0x40 : 0x00));
		}
		t->pc = pc + 2; break;
	case 0xD1: case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: {
		int idx = cmd - 0xD1;
		if ((size_t)pc + 2 > n) { t->ended = 1; return; }
		if (!dry && !s->mute_rhythm) {
			/* ADPCM-A pan/level regs 0x18..0x1D */
			wr(s, (uint8_t)(0x18 + idx), d[pc + 1]);
			if (s->rhy_have_wav)
				s->rhy[idx].vol = d[pc + 1] & 0x1F;
		}
		t->pc = pc + 2; break;
	}
	case 0xA7: case 0xAB: case 0xAC: t->pc = pc + 3; break;
	case 0xAD:
		if ((size_t)pc + 3 > n) { t->ended = 1; return; }
		t->pb_step = (int16_t)(d[pc + 1] | (d[pc + 2] << 8));
		if (t->pb_step < 0) t->pb_step = -t->pb_step;
		t->pc = pc + 3; break;
	case 0xAE:
		if ((size_t)pc + 3 > n) { t->ended = 1; return; }
		t->pb_dst = (int16_t)(d[pc + 1] | (d[pc + 2] << 8));
		t->pc = pc + 3; break;
	case 0xAF:
		if ((size_t)pc + 3 > n) { t->ended = 1; return; }
		t->pb = (int16_t)(d[pc + 1] | (d[pc + 2] << 8));
		t->pc = pc + 3; break;
	case 0xC1: t->pc = pc + 3; break;
	case 0xC3: t->pc = pc + 2; break;
	case 0xC5:
		if ((size_t)pc + 3 > n) { t->ended = 1; return; }
		t->pc = pc + 3 + (d[pc + 1] | (d[pc + 2] << 8)); break;
	case 0xD0:
		if ((size_t)pc + 3 > n) { t->ended = 1; return; }
		if (!dry && !s->mute_rhythm) {
			uint8_t mask = d[pc + 2] & 0x3F;
			if (mask) {
				/* bit7=1 → key-on selected ADPCM-A channels */
				wr(s, 0x10, (uint8_t)(0x80 | mask));
				rhy_hit(s, mask);
			} else {
				wr(s, 0x10, 0x00);
			}
		}
		t->wait = d[pc + 1]; t->pc = pc + 3; break;
	case 0xDD: case 0xDE: case 0xDF:
	case 0xE2: case 0xE7: case 0xEB: case 0xED: case 0xEE:
		t->pc = pc + 4; break;
	case 0xE6: {
		int id;
		if ((size_t)pc + 3 > n) { t->ended = 1; return; }
		t->wait = d[pc + 1];
		id = d[pc + 2];
		t->ch_id = id;
		t->kind = kind_of(id);
		t->hw = hw_of(id, t->kind);
		if (!dry && !s->skip_audio && t->kind == CK_MIDI && s->sf) {
			tsf_channel_set_presetnumber(s->sf, t->hw, t->inst & 127, t->hw == 9);
			tsf_channel_midi_control(s->sf, t->hw, 7, t->vol ? t->vol : 100);
		}
		t->pc = pc + 3; break;
	}
	case 0xEA: t->pc = pc + 3; break;
	case 0xEC:
		if ((size_t)pc + 3 <= n) {
			t->wait = d[pc + 1];
			t->inst = d[pc + 2];
			if (!dry && !s->skip_audio && t->kind == CK_MIDI && s->sf)
				tsf_channel_set_presetnumber(s->sf, t->hw, t->inst & 127, t->hw == 9);
		}
		t->pc = pc + 3; break;
	case 0xFE:
		t->ended = 1; if (!dry) note_off(s, t); break;
	case 0xFF:
		t->ended = 1; s->song_ended = 1; if (!dry) note_off(s, t); break;
	default: t->pc = pc + 1; break;
	}
}

static void tick(ms_state *s, int dry)
{
	int i, alive = 0;
	s->reg_tick++;
	if (s->song_ended) { s->ended = 1; return; }
	for (i = 0; i < MS_TRK; i++) {
		int guard;
		if (!s->trk[i].active || s->trk[i].ended) continue;
		alive++;
		guard = 64;
		while (guard-- > 0 && s->trk[i].active && !s->trk[i].ended &&
		       s->trk[i].wait == 0)
			trk_step(s, i, dry);
		if (s->trk[i].wait > 0)
			trk_step(s, i, dry);
	}
	if (!alive) s->ended = 1;
}

static int ticks_per_sec(const ms_state *s)
{
	int tb = s->timebase > 0 ? s->timebase : 48;
	int bpm = s->tempo > 0 ? s->tempo : 120;
	int mod = s->tempo_mod > 0 ? s->tempo_mod : 0x40;
	int tps = (int)((double)bpm * mod / 64.0 * tb / 60.0 + 0.5);
	return tps > 0 ? tps : 1;
}

static void reset_trks(ms_state *s)
{
	int i;
	s->ended = 0; s->song_ended = 0; s->saw_inf_loop = 0;
	s->tempo = 120; s->timebase = 48; s->tempo_mod = 0x40;
	s->ssg_mix = 0x38;
	memset(s->trk, 0, sizeof s->trk);
	for (i = 0; i < MS_TRK; i++) {
		if (!s->trk_off[i]) continue;
		s->trk[i].active = 1;
		s->trk[i].base = s->trk_off[i];
		s->trk[i].pc = s->trk_off[i];
		s->trk[i].vol = 0x64;
		s->trk[i].pan = 0;
		s->trk[i].ch_id = 0xFF;
	}
}

static void clear_shadow(ms_state *s)
{
	memset(s->sh_opn_set, 0, sizeof s->sh_opn_set);
	memset(s->sh_opl_set, 0, sizeof s->sh_opl_set);
}

/* After a register-only seek: reset chip, replay shadowed regs (skip key strobes),
 * then re-key channels that the sequencer still has held. */
static void commit_shadow(ms_state *s)
{
	int p, a, i;
	int was_skip = s->skip_audio;
	s->skip_audio = 0;
	if (s->opna) {
		s->opna->reset();
		/* Base init (also lands in shadow). */
		wr(s, 0x29, 0x80);
		wr(s, 0x07, 0x38);
		wr(s, 0x11, 0x3F);
		for (p = 0; p < 2; p++) {
			for (a = 0; a < 256; a++) {
				if (!s->sh_opn_set[p][a]) continue;
				if (p == 0 && a == 0x28) continue; /* key-on strobe */
				if (p == 0) {
					s->opna->write(0, (uint8_t)a);
					s->opna->write(1, s->sh_opn[p][a]);
				} else {
					s->opna->write(2, (uint8_t)a);
					s->opna->write(3, s->sh_opn[p][a]);
				}
			}
		}
		for (i = 0; i < MS_TRK; i++) {
			ms_trk *tr = &s->trk[i];
			if (!tr->keyed) continue;
			if (tr->kind == CK_FM) {
				tr->keyed = 0;
				fm_on(s, tr, tr->cur_note);
			} else if (tr->kind == CK_SSG) {
				tr->keyed = 0;
				ssg_on(s, tr, tr->cur_note);
			}
		}
	}
	if (s->opl) {
		s->opl->reset();
		opl_wr(s, 1, 0x05, 0x01);
		opl_wr(s, 0, 0x01, 0x20);
		opl_wr(s, 0, 0x08, 0x00);
		for (p = 0; p < 2; p++) {
			for (a = 0; a < 256; a++) {
				if (!s->sh_opl_set[p][a]) continue;
				if ((a & 0xF0) == 0xB0) continue; /* key bit in B0 */
				s->opl->write((uint32_t)(p ? 2 : 0), (uint8_t)a);
				s->opl->write((uint32_t)(p ? 3 : 1), s->sh_opl[p][a]);
			}
		}
		for (i = 0; i < MS_TRK; i++) {
			ms_trk *tr = &s->trk[i];
			if (tr->kind == CK_OPL && tr->keyed) {
				tr->keyed = 0;
				opl_on(s, tr, tr->cur_note);
			}
		}
	}
	s->skip_audio = was_skip;
	s->prev_l = s->prev_r = s->last_l = s->last_r = 0;
	s->chip_pos = 0;
}


static int load_wav16(const char *path, std::vector<int16_t> *pcm, int *rate_out)
{
	FILE *fp;
	uint8_t hdr[44];
	uint32_t rate, data_bytes = 0, pos;
	uint16_t ch, bps;
	size_t ns;
	if (!(fp = fopen(path, "rb"))) return 0;
	if (fread(hdr, 1, 44, fp) < 44 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
		fclose(fp); return 0;
	}
	rate = (uint32_t)hdr[24] | ((uint32_t)hdr[25] << 8) | ((uint32_t)hdr[26] << 16) | ((uint32_t)hdr[27] << 24);
	ch = (uint16_t)(hdr[22] | (hdr[23] << 8));
	bps = (uint16_t)(hdr[34] | (hdr[35] << 8));
	if (ch != 1 || bps != 16) { fclose(fp); return 0; }
	/* Find data chunk (may not be at 36) */
	fseek(fp, 12, SEEK_SET);
	for (;;) {
		uint8_t id[8];
		if (fread(id, 1, 8, fp) != 8) { fclose(fp); return 0; }
		data_bytes = (uint32_t)id[4] | ((uint32_t)id[5] << 8) | ((uint32_t)id[6] << 16) | ((uint32_t)id[7] << 24);
		if (!memcmp(id, "data", 4)) break;
		if (fseek(fp, (long)data_bytes, SEEK_CUR) != 0) { fclose(fp); return 0; }
		(void)pos;
	}
	if (data_bytes < 2 || data_bytes > 2u * 1024u * 1024u) { fclose(fp); return 0; }
	ns = data_bytes / 2;
	pcm->resize(ns);
	if (fread(pcm->data(), 2, ns, fp) != ns) { pcm->clear(); fclose(fp); return 0; }
	fclose(fp);
	if (rate_out) *rate_out = rate > 0 ? (int)rate : 8000;
	return 1;
}

static void load_rhythm(ms_state *s, const pc98_cfg *cfg)
{
	static const char *names[6] = {
		"2608_BD.WAV", "2608_SD.WAV", "2608_TOP.WAV",
		"2608_HH.WAV", "2608_TOM.WAV", "2608_RIM.WAV"
	};
	static const char *names_lo[6] = {
		"2608_bd.wav", "2608_sd.wav", "2608_top.wav",
		"2608_hh.wav", "2608_tom.wav", "2608_rim.wav"
	};
	char dir[PC98_PATH_MAX], path[PC98_PATH_MAX];
	FILE *fp;
	int i, got = 0, rate;
	dir[0] = 0;
	if (cfg && cfg->rhythm_path[0])
		pc98_bounded(dir, sizeof dir, cfg->rhythm_path);
	else if (cfg && cfg->dll_dir[0])
		pc98_bounded(dir, sizeof dir, cfg->dll_dir);
	if (!dir[0]) return;
	/* Prefer a raw ADPCM-A ROM if present (same as PMD/fmgen). */
	snprintf(path, sizeof path, "%s%cym2608_adpcm_rom.bin", dir,
		(dir[strlen(dir)-1] == '/' || dir[strlen(dir)-1] == '\\') ? 0 : '/');
	if (path[strlen(path)-1] == 0) /* path had trailing slash already handled poorly */
		snprintf(path, sizeof path, "%sym2608_adpcm_rom.bin", dir);
	fp = fopen(path, "rb");
	if (!fp) {
		snprintf(path, sizeof path, "%s/ym2608_adpcm_rom.bin", dir);
		fp = fopen(path, "rb");
	}
	if (fp) {
		fseek(fp, 0, SEEK_END);
		long sz = ftell(fp);
		fseek(fp, 0, SEEK_SET);
		if (sz >= 0x2000 && sz <= 0x40000) {
			s->rhy_rom.resize((size_t)sz);
			if (fread(s->rhy_rom.data(), 1, (size_t)sz, fp) == (size_t)sz) {
				s->iface.rom = s->rhy_rom.data();
				s->iface.rom_n = s->rhy_rom.size();
			}
		}
		fclose(fp);
		if (s->iface.rom_n) return;
	}
	for (i = 0; i < 6; i++) {
		snprintf(path, sizeof path, "%s/%s", dir, names[i]);
		if (!load_wav16(path, &s->rhy[i].pcm, &rate)) {
			snprintf(path, sizeof path, "%s/%s", dir, names_lo[i]);
			if (!load_wav16(path, &s->rhy[i].pcm, &rate))
				continue;
		}
		s->rhy[i].rate = rate;
		s->rhy[i].vol = 0x1F;
		s->rhy[i].on = 0;
		got++;
	}
	/* Also try risingdaw-refs bundled path */
	if (got < 6) {
		const char *alt = "/workspace/risingdaw-refs/x68-drums/ym2608";
		got = 0;
		for (i = 0; i < 6; i++) {
			snprintf(path, sizeof path, "%s/%s", alt, names_lo[i]);
			if (load_wav16(path, &s->rhy[i].pcm, &rate)) {
				s->rhy[i].rate = rate;
				s->rhy[i].vol = 0x1F;
				s->rhy[i].on = 0;
				got++;
			}
		}
	}
	s->rhy_have_wav = (got == 6);
}

static void rhy_hit(ms_state *s, uint8_t mask)
{
	int i;
	if (!s->rhy_have_wav || s->iface.rom_n) return;
	for (i = 0; i < 6; i++) {
		if (!(mask & (1 << i))) continue;
		if (s->rhy[i].pcm.empty()) continue;
		s->rhy[i].on = 1;
		s->rhy[i].pos = 0;
		s->rhy[i].step = (s->rhy[i].rate << 16) / (s->rate > 0 ? s->rate : 44100);
		s->rhy[i].end = (int)s->rhy[i].pcm.size() << 16;
	}
}

static void rhy_mix(ms_state *s, int *l, int *r)
{
	int i;
	if (!s->rhy_have_wav || s->iface.rom_n) return;
	for (i = 0; i < 6; i++) {
		int sample, vol;
		if (!s->rhy[i].on) continue;
		if (s->rhy[i].pos >= s->rhy[i].end) { s->rhy[i].on = 0; continue; }
		sample = s->rhy[i].pcm[(size_t)(s->rhy[i].pos >> 16)];
		vol = s->rhy[i].vol & 0x1F;
		sample = (sample * (vol + 1)) / 32;
		*l += sample;
		*r += sample;
		s->rhy[i].pos += s->rhy[i].step;
	}
}

static void reset_chip(ms_state *s)
{
	int i;
	if (s->opn_chip) {
		s->opn_chip->reset();
		wr(s, 0x07, 0x38);
		for (i = 0; i < 3; i++)
			wr(s, 0x28, (uint8_t)i);
		return;
	}
	if (!s->opna) return;
	s->opna->reset();
	wr(s, 0x29, 0x80);
	wr(s, 0x07, 0x38);
	wr(s, 0x11, 0x3F); /* ADPCM-A total level */
	for (i = 0; i < 6; i++) {
		wr(s, 0x28, (uint8_t)(i <= 2 ? i : i - 3 + 4));
		wr(s, (uint8_t)(0x18 + i), 0xDF); /* ADPCM-A pan L+R + level */
	}
}

static void detect_variant(ms_state *s)
{
	int i, fm = 0, fm6 = 0, midi = 0, opl = 0;
	for (i = 0; i < MS_TRK; i++) {
		if (s->trk[i].kind == CK_FM) { fm = 1; if (s->trk[i].hw >= 3) fm6 = 1; }
		else if (s->trk[i].kind == CK_MIDI) midi = 1;
		else if (s->trk[i].kind == CK_OPL) opl = 1;
	}
	if (opl && !fm) s->variant = 3;
	else if (midi && !fm) s->variant = 2;
	else if (fm6) s->variant = 1;
	else s->variant = 0;
}

static int measure_ms(ms_state *s)
{
	int ticks = 0, loop_at = -1, i;
	reset_trks(s);
	s->saw_inf_loop = 0;
	while (!s->ended && ticks < MS_MAX_TICKS) {
		tick(s, 1);
		ticks++;
		/* After each tick, if any track just took an infinite loop, remember max. */
		if (s->saw_inf_loop) {
			if (loop_at < 0 || ticks > loop_at) loop_at = ticks;
			s->saw_inf_loop = 0;
		}
		/* Done when every still-active track has hit an infinite loop once. */
		{
			int active = 0, looped = 0;
			for (i = 0; i < MS_TRK; i++) {
				if (!s->trk[i].active || s->trk[i].ended) continue;
				active++;
				if (s->trk[i].looped_inf) looped++;
			}
			if (active > 0 && looped >= active && loop_at >= 0)
				break;
		}
	}
	if (loop_at < 0) loop_at = ticks;
	{
		int tps, ms;
		reset_trks(s);
		ticks = 0;
		while (ticks < loop_at && !s->ended && ticks < MS_MAX_TICKS) {
			tick(s, 1); ticks++;
		}
		tps = ticks_per_sec(s);
		if (tps < 1) tps = 1;
		ms = (int)((int64_t)loop_at * 1000 / tps);
		if (ms < 1000) ms = 1000;
		if (ms > 10 * 60 * 1000) ms = 10 * 60 * 1000;
		return ms;
	}
}

static int load_bank(const char *dir, const char *prefix, const char *ext,
		std::vector<uint8_t> *out, size_t want)
{
	char path[PC98_PATH_MAX], e2[8];
	FILE *fp;
	long sz;
	size_t i;
	snprintf(path, sizeof path, "%s%s%s", dir, prefix, ext);
	fp = fopen(path, "rb");
	if (!fp) {
		snprintf(e2, sizeof e2, "%s", ext);
		for (i = 0; e2[i]; i++) e2[i] = (char)tolower((unsigned char)e2[i]);
		snprintf(path, sizeof path, "%s%s%s", dir, prefix, e2);
		fp = fopen(path, "rb");
	}
	if (!fp) return 0;
	fseek(fp, 0, SEEK_END); sz = ftell(fp); fseek(fp, 0, SEEK_SET);
	if (sz < (long)want) { fclose(fp); return 0; }
	out->resize(want);
	if (fread(out->data(), 1, want, fp) != want) { out->clear(); fclose(fp); return 0; }
	fclose(fp);
	return 1;
}

static void find_banks(ms_state *s, const char *filename)
{
	char dir[PC98_PATH_MAX], base[PC98_PATH_MAX], prefix[PC98_PATH_MAX];
	char *us;
	pc98_dir_of(filename, dir, sizeof dir);
	pc98_basename(filename, base, sizeof base);
	pc98_bounded(prefix, sizeof prefix, base);
	us = strchr(prefix, '_');
	if (us) *us = 0;
	if (!prefix[0]) return;
	load_bank(dir, prefix, ".OPN", &s->opn, (size_t)MS_OPN_N * MS_OPN_SZ);
	load_bank(dir, prefix, ".SSG", &s->ssg, (size_t)MS_OPN_N * MS_SSG_SZ);
}

static int setup(ms_state *s, const char *filename, const uint8_t *data,
		size_t len, const pc98_cfg *cfg)
{
	int i;
	char base[128];
	if (!msdrv_probe_mem(data, len)) return -1;
	s->file.assign(data, data + len);
	for (i = 0; i < MS_TRK; i++) {
		uint32_t o = rd32(data + i * 4);
		s->trk_off[i] = (o && o < len) ? (int)o : 0;
	}
	s->rate = cfg && cfg->rate > 0 ? cfg->rate : PC98_DEFAULT_RATE;
	s->loops_want = (cfg && cfg->loop_count > 0) ? cfg->loop_count : 1;
	s->mute_fm = cfg ? cfg->mute_fm : 0;
	s->mute_ssg = cfg ? cfg->mute_ssg : 0;
	s->mute_rhythm = cfg ? cfg->mute_rhythm : 0;
	find_banks(s, filename);

	reset_trks(s);
	for (i = 0; i < 4000 && !s->ended; i++) tick(s, 1);
	detect_variant(s);

	pc98_basename(filename, base, sizeof base);
	pc98_bounded(s->title, sizeof s->title, base);

	s->one_loop_ms = measure_ms(s);

	if (s->variant == 2) {
		char sf2[PC98_PATH_MAX];
		if (fmd_find_sf2(cfg, filename, 0, sf2, sizeof sf2)) {
			/* Own a dedicated instance — shared FMD cache retains filter/voice
			 * state across close/open and breaks chunk-identity tests. */
			s->sf = tsf_load_filename(sf2);
			s->sf_owned = s->sf ? 1 : 0;
			if (!s->sf)
				s->sf = (tsf *)fmd_font_get(sf2);
		}
		/* Match FMD: modest headroom so dense GS scores don't clip/lag the mixer. */
		if (s->sf)
			tsf_set_output(s->sf, TSF_STEREO_INTERLEAVED, s->rate, -8.0f);
	} else if (s->variant == 3) {
		uint32_t sr;
		s->opl = new ymfm::ymf262(s->iface);
		sr = s->opl->sample_rate(MS_OPL_CLOCK);
		if (sr == 0) sr = 49716;
		s->chip_clock = MS_OPL_CLOCK;
		s->chip_step = ((int64_t)sr << 16) / s->rate;
		reset_opl(s);
	} else if (s->variant == 0) {
		uint32_t sr;
		/* True YM2203 — mono FM+SSG. Avoids OPNA stereo pan regs on a mono chip. */
		s->chip_clock = MS_CLOCK_OPN;
		s->mono = 1;
		s->opn_chip = new ymfm::ym2203(s->iface);
		sr = s->opn_chip->sample_rate(s->chip_clock);
		if (sr == 0) sr = 27733;
		s->chip_step = ((int64_t)sr << 16) / s->rate;
		reset_chip(s);
	} else {
		uint32_t sr;
		s->chip_clock = MS_CLOCK_OPNA;
		s->mono = 0;
		s->opna = new ymfm::ym2608(s->iface);
		sr = s->opna->sample_rate(s->chip_clock);
		if (sr == 0) sr = 55467;
		s->chip_step = ((int64_t)sr << 16) / s->rate;
		reset_chip(s);
		load_rhythm(s, cfg);
	}

	reset_trks(s);
	if (s->opna || s->opn_chip) reset_chip(s);
	if (s->opl) reset_opl(s);
	if (s->sf) {
		int c;
		sf_silence(s->sf);
		tsf_set_output(s->sf, TSF_STEREO_INTERLEAVED, s->rate, -8.0f);
		for (c = 0; c < 16; c++) {
			tsf_channel_set_presetnumber(s->sf, c, 0, c == 9);
			tsf_channel_midi_control(s->sf, c, 7, 100);
			tsf_channel_midi_control(s->sf, c, 11, 127);
			tsf_channel_midi_control(s->sf, c, 10, 64);
		}
	}
	s->play_limit = (uint32_t)(((int64_t)s->one_loop_ms * s->loops_want * s->rate) / 1000);

	{
		const char *rl = getenv("MSDRV_REGLOG");
		if (rl && rl[0]) {
			s->reg_log = fopen(rl, "w");
			if (s->reg_log)
				fprintf(s->reg_log, "# tick chip port reg val\n");
		}
	}
	s->play_samples = 0;
	s->tick_acc = 0;
	s->chip_pos = 0;
	s->tsf_fifo_pos = 0;
	s->tsf_fifo_len = 0;
	return 0;
}

int msdrv_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out)
{
	ms_state tmp;
	const char *ft, *eng;
	if (!out) return -1;
	memset(out, 0, sizeof *out);
	if (setup(&tmp, filename, data, len, cfg) != 0) {
		if (tmp.sf) { sf_silence(tmp.sf); if (tmp.sf_owned) tsf_close(tmp.sf); }
		delete tmp.opna; delete tmp.opl;
		return -1;
	}
	out->kind = PC98_KIND_MSDRV;
	out->songs = 1;
	out->looping = 1;
	out->one_loop_ms[0] = tmp.one_loop_ms;
	pc98_bounded(out->title, sizeof out->title, tmp.title);
	if (tmp.variant == 0) { ft = "MsDRV OPN"; eng = "ymfm MsDRV OPN"; }
	else if (tmp.variant == 1) { ft = "MsDRV OPNA"; eng = "ymfm MsDRV OPNA"; }
	else if (tmp.variant == 2) {
		ft = "MsDRV GS";
		eng = tmp.sf ? "TinySoundFont MsDRV GS" : "MsDRV GS (no SF2)";
	} else { ft = "MsDRV OPL3"; eng = "ymfm MsDRV OPL3"; }
	pc98_bounded(out->filetype, sizeof out->filetype, ft);
	pc98_bounded(out->engine, sizeof out->engine, eng);
	if (tmp.sf) { sf_silence(tmp.sf); if (tmp.sf_owned) tsf_close(tmp.sf); }
	delete tmp.opna; delete tmp.opl;
	return 0;
}

void *msdrv_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg)
{
	ms_state *s = new ms_state();
	if (setup(s, filename, data, len, cfg) != 0) {
		delete s->opna;
		delete s->opn_chip;
		delete s->opl;
		delete s;
		return NULL;
	}
	return s;
}

void msdrv_close_h(void *h)
{
	ms_state *s = (ms_state *)h;
	if (!s) return;
	if (s->reg_log) { fclose(s->reg_log); s->reg_log = NULL; }
	/* Shared TSF cache — must silence voices so the next open starts clean. */
	if (s->sf) {
		sf_silence(s->sf);
		if (s->sf_owned) tsf_close(s->sf);
		s->sf = NULL; s->sf_owned = 0;
	}
	delete s->opna;
	delete s->opn_chip;
	delete s->opl;
	delete s;
}

int msdrv_process_h(void *h, float *buf, int count)
{
	ms_state *s = (ms_state *)h;
	int done = 0;
	if (!s || count <= 0) return 0;
	/* Seek fast-forward may pass a small discard buffer; skip_audio never writes buf. */
	if (!s->skip_audio) {
		if (!buf) return 0;
		memset(buf, 0, (size_t)count * 2 * sizeof(float));
	}
	while (done < count) {
		int tps, i, room;
		if (s->play_limit && s->play_samples >= s->play_limit)
			break;
		tps = ticks_per_sec(s);
		if (tps < 1) tps = 1;

		/* Due ticks — flush any TSF lookahead first (should already be empty). */
		while (s->tick_acc >= s->rate) {
			s->tsf_fifo_pos = 0;
			s->tsf_fifo_len = 0;
			tick(s, 0);
			s->tick_acc -= s->rate;
			tps = ticks_per_sec(s);
			if (tps < 1) tps = 1;
		}

		room = count - done;
		if (s->play_limit) {
			uint32_t left = s->play_limit - s->play_samples;
			if ((uint32_t)room > left) room = (int)left;
		}
		if (room < 1) break;

		/* Emit one host sample (or a short burst for chip paths). For TSF we
		 * refill a fixed 64-sample FIFO that never extends past the next tick,
		 * so any host buffer size yields the same float stream. */
		if (s->skip_audio) {
			/* Seek fast-forward: advance sequencer time without chip/TSF audio. */
			int chunk = (int)(((int64_t)s->rate - s->tick_acc + tps - 1) / tps);
			if (chunk < 1) chunk = 1;
			if (chunk > room) chunk = room;
			/* Cap jumps so we still service ticks often enough. */
			if (chunk > 4096) chunk = 4096;
			s->tick_acc += (int64_t)chunk * tps;
			s->play_samples += (uint32_t)chunk;
			done += chunk;
			continue;
		}

		if (s->sf) {
			int emitted = 0;
			while (emitted < room) {
				int until_tick, avail, take, gen;
				if (s->tick_acc >= s->rate)
					break; /* let outer loop fire the tick */
				until_tick = (int)(((int64_t)s->rate - s->tick_acc + tps - 1) / tps);
				if (until_tick < 1) until_tick = 1;
				avail = s->tsf_fifo_len - s->tsf_fifo_pos;
				if (avail <= 0) {
					gen = 64;
					if (gen > until_tick) gen = until_tick;
					memset(s->tsf_fifo, 0, sizeof s->tsf_fifo);
					tsf_render_float(s->sf, s->tsf_fifo, gen, 0);
					s->tsf_fifo_pos = 0;
					s->tsf_fifo_len = gen;
					avail = gen;
				}
				take = avail;
				if (take > room - emitted) take = room - emitted;
				if (take > until_tick) take = until_tick;
				for (i = 0; i < take; i++) {
					buf[(done + emitted + i) * 2] += s->tsf_fifo[(s->tsf_fifo_pos + i) * 2];
					buf[(done + emitted + i) * 2 + 1] += s->tsf_fifo[(s->tsf_fifo_pos + i) * 2 + 1];
				}
				s->tsf_fifo_pos += take;
				for (i = 0; i < take; i++) {
					s->tick_acc += tps;
					s->play_samples++;
				}
				emitted += take;
			}
			for (i = 0; i < emitted; i++) {
				float L = buf[(done + i) * 2], R = buf[(done + i) * 2 + 1];
				if (L > 1) L = 1; if (L < -1) L = -1;
				if (R > 1) R = 1; if (R < -1) R = -1;
				buf[(done + i) * 2] = L; buf[(done + i) * 2 + 1] = R;
			}
			done += emitted;
			continue;
		}

		/* FM / OPL: linear resample from chip rate; SSG -18 dB; overall ×0.5. */
		{
			int chunk = (int)(((int64_t)s->rate - s->tick_acc + tps - 1) / tps);
			if (chunk < 1) chunk = 1;
			if (chunk > room) chunk = room;
			if (s->opna || s->opn_chip || s->opl) {
				for (i = 0; i < chunk; i++) {
					float L, R, frac, mix_l, mix_r;
					s->chip_pos += s->chip_step;
					while (s->chip_pos >= 0x10000) {
						s->prev_l = s->last_l;
						s->prev_r = s->last_r;
						if (s->opn_chip) {
							int fm, ssg, m;
							s->opn_chip->generate(&s->opn_out);
							fm = s->opn_out.data[0];
							ssg = s->opn_out.data[1];
							if (s->mute_fm) fm = 0;
							if (s->mute_ssg) ssg = 0;
							ssg = ssg / 8;
							m = fm + ssg;
							s->last_l = s->last_r = m; /* YM2203 mono → both speakers */
						} else if (s->opna) {
							int fm_l, fm_r, ssg;
							s->opna->generate(&s->out);
							fm_l = s->out.data[0];
							fm_r = s->out.data[1];
							ssg = s->out.data[2];
							if (s->mute_fm) { fm_l = 0; fm_r = 0; }
							if (s->mute_ssg) ssg = 0;
							ssg = ssg / 8;
							s->last_l = fm_l + ssg;
							s->last_r = fm_r + ssg;
							rhy_mix(s, &s->last_l, &s->last_r);
						} else {
							s->opl->generate(&s->opl_out);
							s->last_l = s->opl_out.data[0] + s->opl_out.data[2];
							s->last_r = s->opl_out.data[1] + s->opl_out.data[3];
						}
						s->chip_pos -= 0x10000;
					}
					frac = (float)s->chip_pos / 65536.f;
					mix_l = s->prev_l + (s->last_l - s->prev_l) * frac;
					mix_r = s->prev_r + (s->last_r - s->prev_r) * frac;
					L = mix_l * (0.5f / 32768.f);
					R = mix_r * (0.5f / 32768.f);
					/* Soft clip — hard clip was a crackle source when peaking ~1.0. */
					if (L > 1.f) L = 1.f + (L - 1.f) * 0.1f;
					if (L < -1.f) L = -1.f + (L + 1.f) * 0.1f;
					if (R > 1.f) R = 1.f + (R - 1.f) * 0.1f;
					if (R < -1.f) R = -1.f + (R + 1.f) * 0.1f;
					if (L > 1.f) L = 1.f; if (L < -1.f) L = -1.f;
					if (R > 1.f) R = 1.f; if (R < -1.f) R = -1.f;
					buf[(done + i) * 2] = L;
					buf[(done + i) * 2 + 1] = R;
					s->tick_acc += tps;
					s->play_samples++;
				}
			} else {
				for (i = 0; i < chunk; i++) {
					s->tick_acc += tps;
					s->play_samples++;
				}
			}
			done += chunk;
		}
	}
	return count;
}

int msdrv_seek_ms_h(void *h, int ms)
{
	ms_state *s = (ms_state *)h;
	uint32_t target, settle, pre;
	float discard[512 * 2];
	int i;
	if (!s) return -1;
	if (ms < 0) ms = 0;
	reset_trks(s);
	clear_shadow(s);
	if (s->opna) reset_chip(s);
	if (s->opl) reset_opl(s);
	if (s->sf) {
		int c;
		tsf_note_off_all(s->sf);
		tsf_reset(s->sf);
		tsf_set_output(s->sf, TSF_STEREO_INTERLEAVED, s->rate, -8.0f);
		for (c = 0; c < 16; c++) {
			tsf_channel_set_presetnumber(s->sf, c, 0, c == 9);
			tsf_channel_midi_control(s->sf, c, 7, 100);
			tsf_channel_midi_control(s->sf, c, 11, 127);
			tsf_channel_midi_control(s->sf, c, 10, 64);
		}
	}
	s->play_samples = 0;
	s->tick_acc = 0;
	s->chip_pos = 0;
	s->prev_l = s->prev_r = s->last_l = s->last_r = 0;
	s->tsf_fifo_pos = 0;
	s->tsf_fifo_len = 0;
	target = (uint32_t)(((int64_t)ms * s->rate) / 1000);
	/* ~50 ms settle after register-only fast-forward. */
	settle = (uint32_t)(s->rate / 20);
	if (settle > target) settle = target;
	pre = target - settle;

	s->skip_audio = 1;
	while (s->play_samples < pre && !s->ended) {
		int n = (int)(pre - s->play_samples);
		if (n > 8192) n = 8192;
		msdrv_process_h(s, NULL, n);
	}
	s->skip_audio = 0;

	if (s->opna || s->opl)
		commit_shadow(s);

	if (s->sf) {
		/* Controllers/programs already applied during skip; re-trigger held notes. */
		tsf_note_off_all(s->sf);
		for (i = 0; i < MS_TRK; i++) {
			ms_trk *tr = &s->trk[i];
			if (tr->kind == CK_MIDI && tr->keyed && s->sf) {
				tsf_channel_set_presetnumber(s->sf, tr->hw, tr->inst & 127, tr->hw == 9);
				tsf_channel_midi_control(s->sf, tr->hw, 7, tr->vol ? tr->vol : 100);
				tsf_channel_note_on(s->sf, tr->hw, tr->cur_note & 127,
					(tr->vol & 0x7F) / 127.f);
			}
		}
	}

	while (s->play_samples < target && !s->ended) {
		int n = (int)(target - s->play_samples);
		if (n > 512) n = 512;
		msdrv_process_h(s, discard, n);
	}
	return (int)(((int64_t)s->play_samples * 1000) / s->rate);
}

int msdrv_one_loop_ms_h(void *h)
{
	ms_state *s = (ms_state *)h;
	return s ? s->one_loop_ms : 0;
}
int msdrv_rate_h(void *h)
{
	ms_state *s = (ms_state *)h;
	return s ? s->rate : PC98_DEFAULT_RATE;
}
void msdrv_set_loops_h(void *h, int loops)
{
	ms_state *s = (ms_state *)h;
	if (!s) return;
	s->loops_want = loops > 0 ? loops : 1;
	s->play_limit = (uint32_t)(((int64_t)s->one_loop_ms * s->loops_want * s->rate) / 1000);
}
void msdrv_apply_mute_h(void *h, const pc98_cfg *cfg)
{
	ms_state *s = (ms_state *)h;
	if (!s || !cfg) return;
	s->mute_fm = cfg->mute_fm;
	s->mute_ssg = cfg->mute_ssg;
	s->mute_rhythm = cfg->mute_rhythm;
}
const char *msdrv_title_h(void *h)
{
	ms_state *s = (ms_state *)h;
	return s ? s->title : "";
}

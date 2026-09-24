/*
 * Kajihara MMD.COM (.MMD) — PMD MIDI mode songs from MC.EXE.
 * Format: Valley Bell MMD_Format / mmd2mid (BPM + 18 track headers).
 * EMI songs embed Roland GS SysEx → play with SC-55 / GS SF2.
 * Not FUGA XOR-A5 RCP, not WinFMP. TSF render in 64-sample blocks.
 */
#include "mmd_engine.h"
#include "fmd_engine.h"
#include "pc98_util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <vector>
#include <algorithm>

#include "tsf.h"

#define MMD_NTRK 18
#define MMD_TPQ  48
#define MMD_FIFO 64
#define MMD_NLP  16

enum { EV_NOTEON = 1, EV_NOTEOFF, EV_CC, EV_PC, EV_PB, EV_AT, EV_TEMPO };

struct MmdEv {
	uint32_t tick;
	uint8_t type, ch, a, b;
};
struct MmdHdr { uint16_t ptr; uint8_t transp, ch; };
struct RunNote { uint8_t ch, note; uint32_t off; };

struct MmdState {
	std::vector<uint8_t> file;
	std::vector<MmdEv> ev;
	char title[256], game[256], engine[96], filetype[16], chip[64];
	char sf2_path[PC98_PATH_MAX], sf2_name[128];
	int rate, loops_want, one_loop_ms, bpm0, tempo_us;
	uint32_t loop_tick, end_tick, play_limit, play_samples;
	size_t ev_i;
	uint32_t cur_tick;
	int ended, tick_left;
	tsf *sf;
	uint32_t mute_midi;
	float fifo[MMD_FIFO * 2];
	int fifo_pos, fifo_len;
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

int mmd_probe_mem(const uint8_t *data, size_t len)
{
	int i, en = 0;
	uint16_t usy;
	if (!data || len < 0x52) return 0;
	if (data[0] < 20 || data[0] > 250) return 0;
	if (data[0] <= 0x0f && (data[1] == 0x18 || data[1] == 0x1a)) return 0;
	if (fmd_probe_mem(data, len)) return 0;
	for (i = 0; i < MMD_NTRK; i++) {
		uint16_t ptr = rd16(data + 2 + i * 4);
		uint8_t ch = data[2 + i * 4 + 3];
		if (ch == 0xFF) continue;
		if (ch > 0x1F) return 0;
		if (ptr >= len || ptr < 2) return 0;
		en++;
	}
	if (en < 1) return 0;
	usy = rd16(data + 0x4A);
	if (usy && usy >= len) return 0;
	return 1;
}

static int8_t track_transp(uint8_t tr, int8_t gbl)
{
	int8_t t;
	if (tr & 0x80) return 0;
	t = (int8_t)((tr & 0x40) ? (int)tr - 0x80 : (int)tr);
	return (int8_t)(t + gbl);
}

static uint32_t tempo_us_from(uint16_t bpm, uint8_t scale)
{
	double bpm_f = (double)bpm * (double)(scale ? scale : 0x40) / 64.0;
	if (bpm_f < 1.0) bpm_f = 1.0;
	return (uint32_t)(60000000.0 / bpm_f + 0.5);
}

static void flush_due(std::vector<MmdEv> *out, uint32_t tick, std::vector<RunNote> *runs, int all)
{
	size_t i = 0;
	while (i < runs->size()) {
		if (all || (*runs)[i].off <= tick) {
			MmdEv e;
			e.tick = all ? tick : (*runs)[i].off;
			e.type = EV_NOTEOFF; e.ch = (*runs)[i].ch;
			e.a = (*runs)[i].note; e.b = 0;
			out->push_back(e);
			runs->erase(runs->begin() + (std::ptrdiff_t)i);
		} else i++;
	}
}

static void expand_song(const uint8_t *d, size_t n, int inf_loops,
		std::vector<MmdEv> *out, uint32_t *loop_tick, uint32_t *end_tick,
		int *bpm0, char *title, size_t tcap)
{
	MmdHdr th[MMD_NTRK];
	int8_t gbl;
	int i, t;
	uint32_t song_loop = 0, song_end = 0;
	int found_loop = 0;
	uint16_t usy;

	out->clear();
	*bpm0 = d[0];
	gbl = (int8_t)d[1];
	for (i = 0; i < MMD_NTRK; i++) {
		th[i].ptr = rd16(d + 2 + i * 4);
		th[i].transp = d[2 + i * 4 + 2];
		th[i].ch = d[2 + i * 4 + 3];
	}
	usy = rd16(d + 0x4A);
	(void)usy;
	if (title && tcap) {
		title[0] = 0;
		if (th[0].ptr > 0x50) {
			size_t k = 0;
			while (0x50 + k < th[0].ptr && 0x50 + k < n && d[0x50 + k] && k + 1 < tcap) {
				title[k] = (char)d[0x50 + k];
				k++;
			}
			title[k] = 0;
		}
	}

	for (t = 0; t < MMD_NTRK; t++) {
		uint32_t inPos, tick = 0;
		uint8_t cmdMem[4] = {0,0,0,0};
		uint8_t loopIdx = 0;
		uint8_t loopCMem[MMD_NLP][4];
		uint32_t loopPos[MMD_NLP], loopTickA[MMD_NLP];
		uint16_t loopCnt[MMD_NLP];
		uint8_t trkEnd = 0, midChn, midiOk;
		int8_t transp;
		std::vector<RunNote> runs;
		uint32_t trk_loop_tick = 0;
		int trk_has_inf = 0;
		int pass = inf_loops > 0 ? inf_loops : 1;

		if (th[t].ch == 0xFF || th[t].ptr >= n) continue;
		midChn = (uint8_t)(th[t].ch & 0x0F);
		midiOk = 1;
		transp = track_transp(th[t].transp, gbl);
		inPos = th[t].ptr;
		memset(loopCnt, 0, sizeof loopCnt);

		while (inPos < n && !trkEnd) {
			uint8_t cmdType = d[inPos];
			uint8_t cmdDelay;
			int b;

			flush_due(out, tick, &runs, 0);

			if (cmdType >= 0x80 && cmdType <= 0x8F) {
				uint8_t mask = (uint8_t)(cmdType & 0x0F);
				inPos++;
				for (b = 0; b < 4; b++, mask = (uint8_t)(mask << 1)) {
					if (mask & 0x08) {
						if (inPos >= n) { trkEnd = 1; break; }
						cmdMem[b] = d[inPos++];
					}
				}
			} else {
				if (inPos + 4 > n) { trkEnd = 1; break; }
				memcpy(cmdMem, d + inPos, 4);
				inPos += 4;
			}
			if (trkEnd) break;

			cmdType = cmdMem[0];
			cmdDelay = (cmdType >= 0xF0) ? 0 : cmdMem[1];

			if (cmdType < 0x80) {
				uint8_t dur = cmdMem[2], vel = cmdMem[3];
				if (dur && vel && midiOk) {
					uint8_t note = (uint8_t)((cmdType + transp) & 0x7F);
					size_t ri; int extend = 0;
					for (ri = 0; ri < runs.size(); ri++) {
						if (runs[ri].ch == midChn && runs[ri].note == note) {
							runs[ri].off = tick + dur; extend = 1; break;
						}
					}
					if (!extend) {
						MmdEv e;
						e.tick = tick; e.type = EV_NOTEON; e.ch = midChn;
						e.a = note; e.b = vel;
						out->push_back(e);
						RunNote r; r.ch = midChn; r.note = note; r.off = tick + dur;
						runs.push_back(r);
					}
				}
			} else switch (cmdType) {
			case 0x90: case 0x91: case 0x92: case 0x93:
			case 0x94: case 0x95: case 0x96: case 0x97:
				break;
			case 0x98:
				while (inPos < n && d[inPos] != 0xF7) inPos++;
				if (inPos < n) inPos++;
				break;
			case 0xDD: case 0xDE: case 0xDF:
				break;
			case 0xE2:
				if (midiOk) {
					MmdEv e;
					e.tick = tick; e.type = EV_CC; e.ch = midChn; e.a = 0; e.b = cmdMem[3];
					out->push_back(e);
					e.a = 32; e.b = 0; out->push_back(e);
					e.type = EV_PC; e.a = cmdMem[2]; e.b = 0; out->push_back(e);
				}
				break;
			case 0xE6: {
				uint8_t nb = (uint8_t)(cmdMem[2] - 1);
				if (nb == 0xFF) midiOk = 0;
				else { midiOk = 1; midChn = (uint8_t)(nb & 0x0F); }
				break;
			}
			case 0xE7: {
				MmdEv e;
				e.tick = tick; e.type = EV_TEMPO; e.ch = 0; e.a = cmdMem[2]; e.b = 0;
				out->push_back(e);
				break;
			}
			case 0xEA:
				if (midiOk) {
					MmdEv e; e.tick = tick; e.type = EV_AT; e.ch = midChn;
					e.a = cmdMem[2]; e.b = 0; out->push_back(e);
				}
				break;
			case 0xEB:
				if (midiOk) {
					MmdEv e; e.tick = tick; e.type = EV_CC; e.ch = midChn;
					e.a = cmdMem[2]; e.b = cmdMem[3]; out->push_back(e);
				}
				break;
			case 0xEC:
				if (midiOk) {
					MmdEv e; e.tick = tick; e.type = EV_PC; e.ch = midChn;
					e.a = cmdMem[2]; e.b = 0; out->push_back(e);
				}
				break;
			case 0xEE:
				if (midiOk) {
					MmdEv e; e.tick = tick; e.type = EV_PB; e.ch = midChn;
					e.a = cmdMem[2]; e.b = cmdMem[3]; out->push_back(e);
				}
				break;
			case 0xF8:
				if (loopIdx == 0) break;
				loopIdx--;
				loopCnt[loopIdx]++;
				if (cmdMem[1] == 0 || cmdMem[1] >= 0x7F) {
					trk_loop_tick = loopTickA[loopIdx];
					trk_has_inf = 1;
					if (loopCnt[loopIdx] < (uint16_t)pass) {
						memcpy(cmdMem, loopCMem[loopIdx], 4);
						inPos = loopPos[loopIdx];
						loopIdx++;
					} else trkEnd = 1;
				} else if (loopCnt[loopIdx] < cmdMem[1]) {
					memcpy(cmdMem, loopCMem[loopIdx], 4);
					inPos = loopPos[loopIdx];
					loopIdx++;
				}
				break;
			case 0xF9:
				if (loopIdx < MMD_NLP) {
					memcpy(loopCMem[loopIdx], cmdMem, 4);
					loopPos[loopIdx] = inPos;
					loopTickA[loopIdx] = tick;
					loopCnt[loopIdx] = 0;
					if (loopIdx > 0 && loopPos[loopIdx] == loopPos[loopIdx - 1])
						loopIdx--;
					loopIdx++;
				}
				break;
			case 0xFE:
			case 0xFF:
				trkEnd = 1;
				break;
			default:
				break;
			}
			tick += cmdDelay;
		}
		flush_due(out, tick, &runs, 1);
		if (tick > song_end) song_end = tick;
		if (trk_has_inf) {
			if (!found_loop || trk_loop_tick < song_loop)
				song_loop = trk_loop_tick;
			found_loop = 1;
		}
	}

	std::sort(out->begin(), out->end(),
		[](const MmdEv &a, const MmdEv &b) {
			if (a.tick != b.tick) return a.tick < b.tick;
			return a.type < b.type;
		});
	*loop_tick = found_loop ? song_loop : 0;
	*end_tick = song_end;
}

static void init_chs(tsf *sf)
{
	int c;
	if (!sf) return;
	for (c = 0; c < 16; c++) {
		tsf_channel_set_presetnumber(sf, c, 0, c == 9);
		tsf_channel_set_pitchrange(sf, c, 2.0f);
		tsf_channel_midi_control(sf, c, 7, 100);
		tsf_channel_midi_control(sf, c, 11, 127);
		tsf_channel_midi_control(sf, c, 10, 64);
		tsf_channel_set_pitchwheel(sf, c, 8192);
	}
}

static void apply_ev(MmdState *s, const MmdEv *e)
{
	if (!s->sf) return;
	if ((e->type == EV_NOTEON || e->type == EV_NOTEOFF || e->type == EV_CC ||
			e->type == EV_PC || e->type == EV_PB || e->type == EV_AT) &&
			(s->mute_midi & (1u << (e->ch & 15))) && e->type == EV_NOTEON)
		return;
	switch (e->type) {
	case EV_NOTEON: tsf_channel_note_on(s->sf, e->ch, e->a, e->b / 127.f); break;
	case EV_NOTEOFF: tsf_channel_note_off(s->sf, e->ch, e->a); break;
	case EV_CC: tsf_channel_midi_control(s->sf, e->ch, e->a, e->b); break;
	case EV_PC: tsf_channel_set_presetnumber(s->sf, e->ch, e->a, e->ch == 9); break;
	case EV_PB: tsf_channel_set_pitchwheel(s->sf, e->ch, e->a | (e->b << 7)); break;
	case EV_TEMPO:
		s->tempo_us = (int)tempo_us_from((uint16_t)s->bpm0, e->a ? e->a : 0x40);
		if (s->tempo_us < 1) s->tempo_us = 500000;
		break;
	default: break;
	}
}

static void apply_upto(MmdState *s, uint32_t tick)
{
	while (s->ev_i < s->ev.size() && s->ev[s->ev_i].tick <= tick) {
		apply_ev(s, &s->ev[s->ev_i]);
		s->ev_i++;
	}
}

static int samples_per_tick(const MmdState *s)
{
	double sec = (double)s->tempo_us / 1000000.0 / (double)MMD_TPQ;
	int sp = (int)(sec * s->rate + 0.5);
	return sp > 0 ? sp : 1;
}

static int setup(MmdState *s, const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, int need_sf)
{
	uint32_t lt = 0, et = 0;
	int bpm = 120;
	std::vector<MmdEv> once;

	if (!mmd_probe_mem(data, len)) return -1;
	s->file.assign(data, data + len);
	s->rate = cfg && cfg->rate > 0 ? cfg->rate : PC98_DEFAULT_RATE;
	s->loops_want = cfg && cfg->loop_count > 0 ? cfg->loop_count : 2;
	s->mute_midi = cfg ? cfg->mute_midi_mask : 0;
	s->title[0] = s->game[0] = 0;

	expand_song(data, len, 1, &once, &lt, &et, &bpm, s->title, sizeof s->title);
	s->bpm0 = bpm;
	s->loop_tick = lt;
	s->tempo_us = (int)tempo_us_from((uint16_t)bpm, 0x40);
	s->one_loop_ms = (int)((int64_t)et * s->tempo_us / ((int64_t)MMD_TPQ * 1000));
	if (s->one_loop_ms < 100) s->one_loop_ms = 1000;

	expand_song(data, len, s->loops_want, &s->ev, &lt, &et, &bpm, NULL, 0);
	s->end_tick = et;

	pc98_bounded(s->filetype, sizeof s->filetype, "MMD");
	pc98_bounded(s->chip, sizeof s->chip, "Roland GS (SC-55)");
	pc98_bounded(s->engine, sizeof s->engine, "MMD.COM / TinySoundFont GS");
	s->sf2_path[0] = s->sf2_name[0] = 0;
	s->sf = NULL;
	if (need_sf) {
		if (fmd_find_sf2(cfg, filename, 0, s->sf2_path, sizeof s->sf2_path))
			s->sf = (tsf *)fmd_font_get(s->sf2_path);
		if (!s->sf) {
			static const char *fb[] = {
				"/workspace/sf2/FluidR3_GM.sf2",
				"/usr/share/sounds/sf2/FluidR3_GM.sf2",
				NULL
			};
			int fi;
			for (fi = 0; fb[fi]; fi++) {
				FILE *f = fopen(fb[fi], "rb");
				if (!f) continue;
				fclose(f);
				pc98_bounded(s->sf2_path, sizeof s->sf2_path, fb[fi]);
				s->sf = (tsf *)fmd_font_get(s->sf2_path);
				if (s->sf) break;
			}
		}
		if (s->sf) {
			const char *bn = strrchr(s->sf2_path, '\\');
			if (!bn) bn = strrchr(s->sf2_path, '/');
			bn = bn ? bn + 1 : s->sf2_path;
			pc98_bounded(s->sf2_name, sizeof s->sf2_name, bn);
			tsf_set_output(s->sf, TSF_STEREO_INTERLEAVED, s->rate, -8.0f);
			init_chs(s->sf);
			snprintf(s->engine, sizeof s->engine, "MMD.COM / TinySoundFont (%s)", bn);
		} else {
			pc98_bounded(s->engine, sizeof s->engine, "MMD.COM (no SF2)");
		}
	}
	s->play_limit = (uint32_t)(((int64_t)s->one_loop_ms * s->loops_want * s->rate) / 1000);
	if (s->play_limit < (uint32_t)s->rate) s->play_limit = (uint32_t)s->rate;
	s->play_samples = 0;
	s->ev_i = 0;
	s->cur_tick = 0;
	s->ended = 0;
	s->fifo_pos = s->fifo_len = 0;
	s->tick_left = 0;
	return 0;
}

int mmd_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out)
{
	MmdState tmp;
	if (!out) return -1;
	memset(out, 0, sizeof *out);
	if (setup(&tmp, filename, data, len, cfg, 0) != 0) return -1;
	out->kind = PC98_KIND_MMD;
	out->songs = 1;
	out->looping = 1;
	out->one_loop_ms[0] = tmp.one_loop_ms;
	pc98_bounded(out->title, sizeof out->title, tmp.title);
	pc98_bounded(out->filetype, sizeof out->filetype, "MMD");
	pc98_bounded(out->engine, sizeof out->engine, "MMD.COM / TinySoundFont GS");
	return 0;
}

void *mmd_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg)
{
	MmdState *s = new MmdState();
	if (setup(s, filename, data, len, cfg, 1) != 0) { delete s; return NULL; }
	return s;
}

void mmd_close_h(void *h)
{
	MmdState *s = (MmdState *)h;
	if (s) delete s;
}

int mmd_process_h(void *h, float *buf, int count)
{
	MmdState *s = (MmdState *)h;
	int done = 0;
	if (!s || !buf || count <= 0) return 0;
	memset(buf, 0, (size_t)count * 2 * sizeof(float));
	if (s->ended || !s->sf) return count;

	while (done < count) {
		int take, gen;
		if (s->play_limit && s->play_samples >= s->play_limit) { s->ended = 1; break; }

		if (s->fifo_pos >= s->fifo_len) {
			if (s->tick_left <= 0) {
				if (s->ev_i >= s->ev.size() && s->cur_tick > s->end_tick + MMD_TPQ) {
					s->ended = 1; break;
				}
				apply_upto(s, s->cur_tick);
				s->tick_left = samples_per_tick(s);
				s->cur_tick++;
			}
			gen = s->tick_left;
			if (gen > MMD_FIFO) gen = MMD_FIFO;
			memset(s->fifo, 0, sizeof s->fifo);
			tsf_render_float(s->sf, s->fifo, gen, 0);
			s->fifo_pos = 0;
			s->fifo_len = gen;
			s->tick_left -= gen;
		}
		take = s->fifo_len - s->fifo_pos;
		if (take > count - done) take = count - done;
		if (s->play_limit) {
			uint32_t room = s->play_limit - s->play_samples;
			if ((uint32_t)take > room) take = (int)room;
		}
		if (take <= 0) break;
		{
			int i;
			for (i = 0; i < take; i++) {
				buf[(done + i) * 2] += s->fifo[(s->fifo_pos + i) * 2];
				buf[(done + i) * 2 + 1] += s->fifo[(s->fifo_pos + i) * 2 + 1];
			}
		}
		s->fifo_pos += take;
		s->play_samples += (uint32_t)take;
		done += take;
	}
	return count;
}

int mmd_seek_ms_h(void *h, int ms)
{
	MmdState *s = (MmdState *)h;
	uint32_t tick;
	size_t k;
	if (!s) return -1;
	if (ms < 0) ms = 0;
	if (s->sf) {
		tsf_note_off_all(s->sf);
		tsf_reset(s->sf);
		tsf_set_output(s->sf, TSF_STEREO_INTERLEAVED, s->rate, -8.0f);
		init_chs(s->sf);
	}
	s->tempo_us = (int)tempo_us_from((uint16_t)s->bpm0, 0x40);
	s->ev_i = 0;
	s->cur_tick = 0;
	s->play_samples = (uint32_t)(((int64_t)ms * s->rate) / 1000);
	s->ended = 0;
	s->fifo_pos = s->fifo_len = 0;
	s->tick_left = 0;
	tick = (uint32_t)(((int64_t)ms * 1000) * (int64_t)MMD_TPQ / s->tempo_us);
	for (k = 0; k < s->ev.size() && s->ev[k].tick <= tick; k++) {
		if (s->ev[k].type != EV_NOTEON && s->ev[k].type != EV_NOTEOFF)
			apply_ev(s, &s->ev[k]);
		s->ev_i = k + 1;
	}
	s->cur_tick = tick;
	return 0;
}

int mmd_one_loop_ms_h(void *h) { MmdState *s = (MmdState *)h; return s ? s->one_loop_ms : 0; }
int mmd_rate_h(void *h) { MmdState *s = (MmdState *)h; return s ? s->rate : PC98_DEFAULT_RATE; }
void mmd_set_loops_h(void *h, int loops)
{
	MmdState *s = (MmdState *)h;
	if (!s) return;
	if (loops < 1) loops = 1;
	s->loops_want = loops;
	s->play_limit = (uint32_t)(((int64_t)s->one_loop_ms * s->loops_want * s->rate) / 1000);
}
void mmd_apply_mute_h(void *h, const pc98_cfg *cfg)
{
	MmdState *s = (MmdState *)h;
	if (!s || !cfg) return;
	s->mute_midi = cfg->mute_midi_mask;
}
const char *mmd_title_h(void *h) { MmdState *s = (MmdState *)h; return s ? s->title : ""; }
const char *mmd_game_h(void *h) { MmdState *s = (MmdState *)h; return s ? s->game : ""; }
const char *mmd_engine_h(void *h) { MmdState *s = (MmdState *)h; return s ? s->engine : ""; }
const char *mmd_type_h(void *h) { MmdState *s = (MmdState *)h; return s ? s->filetype : ""; }
const char *mmd_chip_h(void *h) { MmdState *s = (MmdState *)h; return s ? s->chip : ""; }
const char *mmd_sf2_h(void *h) { MmdState *s = (MmdState *)h; return s ? s->sf2_name : ""; }

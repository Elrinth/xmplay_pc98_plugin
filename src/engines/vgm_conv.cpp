/*
 * VGM / VGZ for PC-98 / PC-88 rips (YM2608 on the -86 board, YM2203 on -26K
 * and PC-88). The stream is rewritten as an S98 v3 image with a 1/44100 s
 * sync, so it plays through the S98 engine (cisc fmgen, loop, seek, mute).
 * Chips other than YM2203/YM2608 are left to xmp-gamemusic.
 */
#include "vgm_conv.h"
#include "pc98.h"

#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>

#include "../../third_party/miniz/miniz_tinfl.c"

#define VGM_S98_OPN  2
#define VGM_S98_OPNA 4
#define VGM_RATE     44100

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(std::vector<uint8_t> &v, size_t at, uint32_t x)
{
	v[at] = (uint8_t)x;
	v[at + 1] = (uint8_t)(x >> 8);
	v[at + 2] = (uint8_t)(x >> 16);
	v[at + 3] = (uint8_t)(x >> 24);
}

/* gzip member -> raw bytes. max_out caps the output; whole = d is the full
 * file, so the ISIZE trailer is real (a CheckFile peek has no trailer). */
static int gunzip(const uint8_t *d, size_t n, size_t max_out, int whole,
		std::vector<uint8_t> &out)
{
	size_t i = 10, in_sz, out_sz;
	uint8_t flg;
	tinfl_decompressor inf;
	tinfl_status st;
	int flags;
	if (n < 18 || d[0] != 0x1F || d[1] != 0x8B || d[2] != 8)
		return -1;
	flg = d[3];
	if (flg & 4) {
		if (i + 2 > n) return -1;
		i += 2 + (size_t)(d[i] | (d[i + 1] << 8));
	}
	if (flg & 8) { while (i < n && d[i]) i++; i++; }
	if (flg & 16) { while (i < n && d[i]) i++; i++; }
	if (flg & 2) i += 2;
	if (i >= n) return -1;
	out_sz = max_out;
	if (whole && n - i >= 8) {
		uint32_t isz = rd32(d + n - 4);
		if (isz && isz < out_sz) out_sz = isz;
	}
	out.resize(out_sz);
	tinfl_init(&inf);
	in_sz = n - i;
	flags = TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF | TINFL_FLAG_HAS_MORE_INPUT;
	st = tinfl_decompress(&inf, d + i, &in_sz, out.data(), out.data(), &out_sz, flags);
	if (st < TINFL_STATUS_DONE && out_sz == 0)
		return -1;
	out.resize(out_sz);
	return 0;
}

struct vgm_hdr {
	uint32_t ver, data_ofs, loop_pos, gd3_pos;
	uint32_t opn, opna; /* clocks, bit 30 = dual chip */
};

/* Every chip clock field up to VGM 1.71 except YM2203 (0x44), YM2608 (0x48)
 * and NES APU (0x84, checked in parse_hdr). */
static const uint16_t g_other_clocks[] = {
	0x0C, 0x10, 0x2C, 0x30, 0x38, 0x40, 0x4C, 0x50, 0x54, 0x58, 0x5C,
	0x60, 0x64, 0x68, 0x6C, 0x70, 0x74, 0x80, 0x88, 0x8C, 0x90,
	0x98, 0x9C, 0xA0, 0xA4, 0xA8, 0xAC, 0xB0, 0xB4, 0xB8, 0xC0, 0xC4,
	0xC8, 0xCC, 0xD0, 0xD8, 0xDC, 0xE0
};

static int parse_hdr(const uint8_t *d, size_t n, vgm_hdr *h)
{
	size_t end, k;
	if (n < 0x40 || memcmp(d, "Vgm ", 4) != 0)
		return -1;
	h->ver = rd32(d + 8);
	h->data_ofs = 0x40;
	if (h->ver >= 0x150 && rd32(d + 0x34))
		h->data_ofs = 0x34 + rd32(d + 0x34);
	/* Header fields past the data start are command bytes, not clocks. */
	end = h->data_ofs;
	if (end > n) end = n;
	/* 1.70 extra header (0xBC) may sit inside the clock area too. */
	if (h->ver >= 0x170 && end >= 0xC0 && rd32(d + 0xBC) &&
			0xBC + (size_t)rd32(d + 0xBC) < end)
		end = 0xBC + (size_t)rd32(d + 0xBC);
	h->opn = 0x48 <= end ? rd32(d + 0x44) : 0;
	h->opna = 0x4C <= end ? rd32(d + 0x48) : 0;
	if (h->ver < 0x151)
		h->opn = h->opna = 0;
	if (!(h->opn & 0x3FFFFFFF) && !(h->opna & 0x3FFFFFFF))
		return -1;
	for (k = 0; k < sizeof g_other_clocks / sizeof g_other_clocks[0]; ++k) {
		size_t o = g_other_clocks[k];
		if (o + 4 <= end && (rd32(d + o) & 0x3FFFFFFF))
			return -1;
	}
	/* NES APU (0x84) next to a YM2608 is a Famicom EPSM export (FamiStudio).
	 * The APU is not emulated here; its 0xB4 writes are dropped. */
	if (0x88 <= end && (rd32(d + 0x84) & 0x3FFFFFFF) && !(h->opna & 0x3FFFFFFF))
		return -1;
	h->loop_pos = rd32(d + 0x1C) ? 0x1C + rd32(d + 0x1C) : 0;
	h->gd3_pos = rd32(d + 0x14) ? 0x14 + rd32(d + 0x14) : 0;
	return 0;
}

int vgm_probe_mem(const uint8_t *data, size_t len)
{
	vgm_hdr h;
	if (!data || len < 4)
		return 0;
	if (data[0] == 0x1F && data[1] == 0x8B) {
		std::vector<uint8_t> raw;
		if (gunzip(data, len, 0x100, 0, raw) != 0)
			return 0;
		return parse_hdr(raw.data(), raw.size(), &h) == 0;
	}
	return parse_hdr(data, len, &h) == 0;
}

static void put_utf8(std::string &s, uint32_t c)
{
	if (c < 0x80) {
		s += (char)c;
	} else if (c < 0x800) {
		s += (char)(0xC0 | (c >> 6));
		s += (char)(0x80 | (c & 0x3F));
	} else if (c < 0x10000) {
		s += (char)(0xE0 | (c >> 12));
		s += (char)(0x80 | ((c >> 6) & 0x3F));
		s += (char)(0x80 | (c & 0x3F));
	} else {
		s += (char)(0xF0 | (c >> 18));
		s += (char)(0x80 | ((c >> 12) & 0x3F));
		s += (char)(0x80 | ((c >> 6) & 0x3F));
		s += (char)(0x80 | (c & 0x3F));
	}
}

/* GD3: track, track(jp), game, game(jp), system, system(jp), author,
 * author(jp), date, ripper, notes - UTF-16LE, NUL terminated. */
static void parse_gd3(const uint8_t *d, size_t n, uint32_t pos, std::string f[11])
{
	size_t i, end;
	int k = 0;
	if (!pos || pos + 12 > n || memcmp(d + pos, "Gd3 ", 4) != 0)
		return;
	i = pos + 12;
	end = i + rd32(d + pos + 8);
	if (end > n) end = n;
	while (i + 1 < end && k < 11) {
		uint32_t c = d[i] | (d[i + 1] << 8);
		i += 2;
		if (c == 0) { k++; continue; }
		if (c >= 0xD800 && c < 0xDC00 && i + 1 < end) {
			uint32_t lo = d[i] | (d[i + 1] << 8);
			if (lo >= 0xDC00 && lo < 0xE000) {
				i += 2;
				c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
			}
		}
		if (c == '\n' || c == '\r') c = ' ';
		put_utf8(f[k], c);
	}
}

static void add_tag(std::vector<uint8_t> &o, const char *key, const std::string &en,
		const std::string &jp)
{
	const std::string &v = en.empty() ? jp : en;
	if (v.empty()) return;
	o.insert(o.end(), key, key + strlen(key));
	o.push_back('=');
	o.insert(o.end(), v.begin(), v.end());
	o.push_back('\n');
}

struct conv {
	std::vector<uint8_t> o;
	uint32_t wait;
};

static void flush_wait(conv &c)
{
	uint32_t n = c.wait;
	c.wait = 0;
	if (n == 0) return;
	if (n == 1) {
		c.o.push_back(0xFF);
		return;
	}
	n -= 2;
	c.o.push_back(0xFE);
	do {
		uint8_t b = n & 0x7F;
		n >>= 7;
		c.o.push_back(n ? (uint8_t)(b | 0x80) : b);
	} while (n);
}

static void put_write(conv &c, int dev, int port, uint8_t aa, uint8_t dd)
{
	if (dev < 0) return;
	flush_wait(c);
	c.o.push_back((uint8_t)(dev * 2 + port));
	c.o.push_back(aa);
	c.o.push_back(dd);
}

/* Bytes taken by commands we skip (VGM 1.71 table). 0 = unknown. */
static size_t skip_len(const uint8_t *d, size_t i, size_t n)
{
	uint8_t b = d[i];
	if (b == 0x67) {
		if (i + 7 > n) return 0;
		return 7 + (rd32(d + i + 3) & 0x7FFFFFFF);
	}
	if (b == 0x68) return 12;
	if (b >= 0x30 && b <= 0x3F) return 2;
	if (b == 0x4F || b == 0x50) return 2;
	if (b >= 0x40 && b <= 0x5F) return 3;
	if (b == 0x90 || b == 0x91 || b == 0x95) return 5;
	if (b == 0x92) return 6;
	if (b == 0x93) return 11;
	if (b == 0x94) return 2;
	if (b >= 0xA0 && b <= 0xBF) return 3;
	if (b >= 0xC0 && b <= 0xDF) return 4;
	if (b >= 0xE0) return 5;
	return 0;
}

static uint8_t *convert(const uint8_t *d, size_t n, size_t *out_len)
{
	vgm_hdr h;
	conv c;
	int ndev = 0, opna0 = -1, opna1 = -1, opn0 = -1, opn1 = -1;
	uint32_t loop_out = 0;
	size_t i, devbase = 0x20, dump;
	std::string gd3[11];
	uint8_t *res;

	if (parse_hdr(d, n, &h) != 0 || h.data_ofs >= n)
		return NULL;
	c.wait = 0;
	c.o.assign(0x20, 0);
	memcpy(c.o.data(), "S983", 4);
	wr32(c.o, 4, 1);
	wr32(c.o, 8, VGM_RATE);
	if (h.opna & 0x3FFFFFFF) {
		opna0 = ndev++;
		if (h.opna & 0x40000000) opna1 = ndev++;
	}
	if (h.opn & 0x3FFFFFFF) {
		opn0 = ndev++;
		if (h.opn & 0x40000000) opn1 = ndev++;
	}
	wr32(c.o, 0x1C, (uint32_t)ndev);
	c.o.resize(devbase + 16 * (size_t)ndev, 0);
	for (int k = 0; k < ndev; ++k) {
		int opna = (k == opna0 || k == opna1);
		wr32(c.o, devbase + 16 * k, opna ? VGM_S98_OPNA : VGM_S98_OPN);
		wr32(c.o, devbase + 16 * k + 4, (opna ? h.opna : h.opn) & 0x3FFFFFFF);
	}
	dump = c.o.size();
	wr32(c.o, 0x14, (uint32_t)dump);

	i = h.data_ofs;
	while (i < n) {
		uint8_t b;
		if (h.loop_pos && i == h.loop_pos) {
			flush_wait(c);
			loop_out = (uint32_t)c.o.size();
		}
		b = d[i];
		if (b == 0x66)
			break;
		if (b >= 0x70 && b <= 0x7F) {
			c.wait += (b & 15) + 1u;
			i++;
		} else if (b >= 0x80 && b <= 0x8F) {
			c.wait += b & 15u; /* YM2612 DAC byte + wait: no chip here */
			i++;
		} else if (b == 0x62) {
			c.wait += 735;
			i++;
		} else if (b == 0x63) {
			c.wait += 882;
			i++;
		} else if (b == 0x61) {
			if (i + 3 > n) break;
			c.wait += d[i + 1] | (d[i + 2] << 8);
			i += 3;
		} else if (b == 0x55 || b == 0x56 || b == 0x57 ||
				b == 0xA5 || b == 0xA6 || b == 0xA7) {
			int dev;
			if (i + 3 > n) break;
			if (b == 0x55) dev = opn0;
			else if (b == 0xA5) dev = opn1;
			else if (b < 0xA0) dev = opna0;
			else dev = opna1;
			put_write(c, dev, (b & 0x0F) == 7, d[i + 1], d[i + 2]);
			i += 3;
		} else {
			size_t L = skip_len(d, i, n);
			if (L == 0) break;
			i += L;
		}
	}
	flush_wait(c);
	c.o.push_back(0xFD);
	wr32(c.o, 0x18, loop_out);

	parse_gd3(d, n, h.gd3_pos, gd3);
	wr32(c.o, 0x10, (uint32_t)c.o.size());
	c.o.insert(c.o.end(), (const uint8_t *)"[S98]", (const uint8_t *)"[S98]" + 5);
	c.o.push_back(0xEF); c.o.push_back(0xBB); c.o.push_back(0xBF);
	add_tag(c.o, "title", gd3[0], gd3[1]);
	add_tag(c.o, "game", gd3[2], gd3[3]);
	add_tag(c.o, "system", gd3[4], gd3[5]);
	add_tag(c.o, "artist", gd3[6], gd3[7]);
	add_tag(c.o, "year", gd3[8], std::string());
	add_tag(c.o, "s98by", gd3[9], std::string());
	c.o.push_back(0);

	res = (uint8_t *)malloc(c.o.size());
	if (!res) return NULL;
	memcpy(res, c.o.data(), c.o.size());
	*out_len = c.o.size();
	return res;
}

uint8_t *vgm_to_s98(const uint8_t *data, size_t len, size_t *out_len)
{
	if (!data || !out_len || len < 4)
		return NULL;
	if (data[0] == 0x1F && data[1] == 0x8B) {
		std::vector<uint8_t> raw;
		if (gunzip(data, len, PC98_MAX_FILE, 1, raw) != 0)
			return NULL;
		return convert(raw.data(), raw.size(), out_len);
	}
	return convert(data, len, out_len);
}

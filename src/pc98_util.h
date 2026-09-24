#ifndef XMP_PC98_UTIL_H
#define XMP_PC98_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline void pc98_bounded(char *dst, size_t cap, const char *src)
{
	size_t n;
	if (!dst || cap == 0) return;
	if (!src) { dst[0] = '\0'; return; }
	n = strlen(src);
	if (n >= cap) n = cap - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static inline int pc98_ieq(const char *a, const char *b)
{
	if (!a || !b) return 0;
	for (;;) {
		unsigned char ca = (unsigned char)*a++;
		unsigned char cb = (unsigned char)*b++;
		if (tolower(ca) != tolower(cb)) return 0;
		if (!ca) return 1;
	}
}

static inline int pc98_iends(const char *s, const char *suf)
{
	size_t n, m;
	if (!s || !suf) return 0;
	n = strlen(s);
	m = strlen(suf);
	if (m > n) return 0;
	return pc98_ieq(s + n - m, suf);
}

static inline void pc98_dir_of(const char *path, char *dir, size_t cap)
{
	const char *slash;
	size_t n;
	if (!dir || cap == 0) return;
	dir[0] = '\0';
	if (!path) return;
	slash = strrchr(path, '\\');
	if (!slash) slash = strrchr(path, '/');
	if (!slash) return;
	n = (size_t)(slash - path + 1);
	if (n >= cap) n = cap - 1;
	memcpy(dir, path, n);
	dir[n] = '\0';
}

static inline void pc98_basename(const char *path, char *out, size_t cap)
{
	const char *slash, *dot;
	size_t n;
	if (!out || cap == 0) return;
	out[0] = '\0';
	if (!path) return;
	slash = strrchr(path, '\\');
	if (!slash) slash = strrchr(path, '/');
	slash = slash ? slash + 1 : path;
	dot = strrchr(slash, '.');
	n = dot && dot > slash ? (size_t)(dot - slash) : strlen(slash);
	if (n >= cap) n = cap - 1;
	memcpy(out, slash, n);
	out[n] = '\0';
}

static inline int pc98_is_stop_title(const char *s)
{
	if (!s || !s[0]) return 0;
	if (strstr(s, "\xe6\xbc\x94\xe5\xa5\x8f\xe5\x81\x9c\xe6\xad\xa2")) /* 演奏停止 */
		return 1;
	if (strstr(s, "[STOP]") || strstr(s, "[stop]")) return 1;
	if (pc98_ieq(s, "STOP") || pc98_ieq(s, "[STOP]")) return 1;
	return 0;
}

/* Hoot sidecar (not C64 Compute! MUS, not PMD .M). */
static inline int pc98_is_sidecar_name(const char *name)
{
	if (!name) return 0;
	return pc98_iends(name, ".mus") || pc98_iends(name, ".uso") ||
		pc98_iends(name, ".bgm") || pc98_iends(name, ".amd") ||
		pc98_iends(name, ".mdt") || pc98_iends(name, ".mlo") ||
		pc98_iends(name, ".o") || pc98_iends(name, ".mfd") ||
		pc98_iends(name, ".sdt") || pc98_iends(name, ".msb") ||
		pc98_iends(name, ".ntl");
}

/* PC-98 BGMDRV part table: (part, flags, le16 offset). flags 0xFF=on, 0x00=off. */
static inline int pc98_looks_bgmdrv(const uint8_t *d, size_t n)
{
	int enabled = 0, entries = 0;
	size_t i = 0;
	if (!d || n < 8) return 0;
	while (i + 4 <= n && entries < 24) {
		unsigned part = d[i];
		unsigned flags = d[i + 1];
		if (part == 0 || part > 32) break;
		if (flags != 0xFF && flags != 0x00) break;
		if (flags == 0xFF) enabled++;
		entries++;
		i += 4;
	}
	return enabled >= 2;
}

/* Packen NA / MUAP98 .O : word0=0x26, 17 little-endian part offsets. */
static inline int pc98_looks_na(const uint8_t *d, size_t n)
{
	unsigned hdr, i, good = 0;
	if (!d || n < 0x28) return 0;
	hdr = (unsigned)d[0] | ((unsigned)d[1] << 8);
	if (hdr != 0x0026) return 0;
	for (i = 0; i < 17; ++i) {
		unsigned off = (unsigned)d[2 + i * 2] | ((unsigned)d[3 + i * 2] << 8);
		if (off == 0 || off >= n) continue;
		if (d[off] == 0xFC) continue;
		if (off < 0x26) return 0;
		good++;
	}
	return good >= 2;
}

/* Melody MFD .USO : ver=1, fmt=2, magic 0x3E0E (Night Slave) or 0x436B (Idol). */
static inline int pc98_looks_uso(const uint8_t *d, size_t n)
{
	unsigned ver, parts, fmt, magic, i;
	if (!d || n < 32) return 0;
	ver = (unsigned)d[0] | ((unsigned)d[1] << 8);
	parts = (unsigned)d[2] | ((unsigned)d[3] << 8);
	fmt = (unsigned)d[4] | ((unsigned)d[5] << 8);
	if (ver != 1 || fmt != 2 || parts < 2 || parts > 16) return 0;
	magic = (unsigned)d[14] | ((unsigned)d[15] << 8);
	if (magic != 0x3E0E && magic != 0x436B) return 0;
	/* Full file: verify part offsets. CheckFile may only peek a header. */
	if (16 + parts * 10 + 2 > n)
		return n >= 16;
	for (i = 0; i < parts; ++i) {
		unsigned off = (unsigned)d[16 + i * 10 + 2] | ((unsigned)d[16 + i * 10 + 3] << 8);
		if (off < 16 || off >= n) return 0;
	}
	return 1;
}

/* N3G .MDT : 10×u16 header, five increasing track offsets. */
static inline int pc98_looks_mdt(const uint8_t *d, size_t n)
{
	unsigned t0, t1, t2, t3, t4, p0;
	if (!d || n < 64) return 0;
	p0 = (unsigned)d[0] | ((unsigned)d[1] << 8);
	t0 = (unsigned)d[10] | ((unsigned)d[11] << 8);
	t1 = (unsigned)d[12] | ((unsigned)d[13] << 8);
	t2 = (unsigned)d[14] | ((unsigned)d[15] << 8);
	t3 = (unsigned)d[16] | ((unsigned)d[17] << 8);
	t4 = (unsigned)d[18] | ((unsigned)d[19] << 8);
	if (t0 < 0x14) return 0;
	if (t1 <= t0 || t2 <= t1 || t3 <= t2 || t4 <= t3) return 0;
	if (t4 > 0x200000u) return 0;
	/* Full file: tracks and instrument bank must sit inside the blob.
	 * CheckFile may only peek 64–256 bytes (tracks often live past that). */
	if (n > t4) {
		if (p0 < t4 || p0 >= n) return 0;
	}
	return 1;
}

/* Synthia 3.00 .PAI (Kousoku Choujin, Guernica, …). Magic only — not .pai. */
static inline int pc98_looks_pai(const uint8_t *d, size_t n)
{
	if (!d || n < 10) return 0;
	return memcmp(d, "PAI 3.00M", 9) == 0 && d[9] == 0;
}

/* MBMUS .MSB: word-obfuscated; first part offset decrypts to 0x0026. */
static inline int pc98_looks_msb(const uint8_t *d, size_t n)
{
	unsigned dx, i, good = 0;
	uint8_t buf[36];
	if (!d || n < 36) return 0;
	if (d[0] != 0xFF || d[1] != 0xCE) return 0;
	memcpy(buf, d, 36);
	for (i = 0; i < 36; i += 2) {
		dx = buf[i] | ((unsigned)buf[i + 1] << 8);
		dx = ((dx & 0xFF) << 8) | (dx >> 8);
		dx = ((dx << 1) | (dx >> 15)) & 0xFFFF;
		dx = (~dx) & 0xFFFF;
		buf[i] = (uint8_t)(((dx & 0xFF) >> 4) | ((dx & 0xFF) << 4));
		buf[i + 1] = (uint8_t)((((dx >> 8) & 0xFF) >> 2) |
				(((dx >> 8) & 0xFF) << 6));
	}
	if (buf[0] != 0x26 || buf[1] != 0x00) return 0;
	for (i = 0; i < 17; ++i) {
		unsigned off = buf[i * 2] | ((unsigned)buf[i * 2 + 1] << 8);
		if (off < 0x22) continue;
		/* CheckFile only peeks 256 bytes; part offsets live later. */
		if (n >= 512 && off >= n) continue;
		good++;
	}
	return good >= 6;
}

/* OPNDRV 2.05 .MD (FUGA / Oerstedia). Four identical part-table pointers. */
static inline int pc98_looks_opnmd(const uint8_t *d, size_t n)
{
	unsigned w, nv, expect;
	if (!d || n < 0x40) return 0;
	w = (unsigned)d[0] | ((unsigned)d[1] << 8);
	if (w < 0x20 || w > 0x200) return 0;
	if (((unsigned)d[2] | ((unsigned)d[3] << 8)) != w) return 0;
	if (((unsigned)d[4] | ((unsigned)d[5] << 8)) != w) return 0;
	if (((unsigned)d[6] | ((unsigned)d[7] << 8)) != w) return 0;
	nv = (unsigned)d[8] | ((unsigned)d[9] << 8);
	if (nv < 1 || nv > 32) return 0;
	expect = 0x0A + nv * 36 + 2;
	return w == expect;
}

/* ArtDink .NTL container (F_* FM or G_* MIDI). */
static inline int pc98_looks_ntl_any(const uint8_t *d, size_t n)
{
	unsigned sz, cnt, off;
	if (!d || n < 16 || d[0] != 0) return 0;
	sz = (unsigned)d[1] | ((unsigned)d[2] << 8);
	cnt = d[3];
	if (cnt < 3 || cnt > 64) return 0;
	if (sz < 16 + cnt * 3) return 0;
	/* Full file: declared size must match. CheckFile peeks 256 bytes. */
	if (n >= sz && n != sz) return 0;
	off = (unsigned)d[4] | ((unsigned)d[5] << 8);
	if (off < 4 + cnt * 3u) return 0;
	if (n >= sz && off >= n) return 0;
	return 1;
}

/* FM/SSG .NTL only. Peek shorter than the table still claims (Open re-probes). */
static inline int pc98_looks_ntl(const uint8_t *d, size_t n)
{
	unsigned cnt, i, fm = 0, table;
	if (!pc98_looks_ntl_any(d, n)) return 0;
	cnt = d[3];
	table = 4 + cnt * 3;
	if (n < table) return 1;
	for (i = 0; i < cnt; ++i) {
		unsigned typ = d[4 + i * 3 + 2];
		if (typ >= 0x10 && typ <= 0x22)
			fm++;
	}
	return fm > 0;
}

/* G_* .NTL: same container, MIDI part types 0x31–0x3F / 0xB0–0xBF, no FM. */
static inline int pc98_looks_ntl_midi(const uint8_t *d, size_t n)
{
	unsigned cnt, i, midi = 0, table;
	if (!pc98_looks_ntl_any(d, n)) return 0;
	if (pc98_looks_ntl(d, n)) return 0;
	cnt = d[3];
	table = 4 + cnt * 3;
	if (n < table) return 1;
	for (i = 0; i < cnt; ++i) {
		unsigned typ = d[4 + i * 3 + 2];
		if ((typ >= 0x30 && typ <= 0x3F) || (typ >= 0xB0 && typ <= 0xBF))
			midi++;
	}
	return midi > 0;
}

/* FUGA packed FMD (Oerstedia .GMD/.MMD). XOR 0xA5 RCP, not WinFMP MMD. */
static inline int pc98_looks_fmdpack(const uint8_t *d, size_t n)
{
	unsigned tempo, beatn, beatd, tlen, id;
	if (!d || n < 16) return 0;
	tempo = d[0] ^ 0xA5;
	beatn = d[1] ^ 0xA5;
	beatd = d[2] ^ 0xA5;
	if (tempo < 20 || tempo > 250) return 0;
	if (beatn < 1 || beatn > 16 || beatd < 1 || beatd > 16) return 0;
	tlen = (d[6] ^ 0xA5) | ((unsigned)(d[7] ^ 0xA5) << 8);
	tlen = (tlen & ~3u) | ((tlen & 3u) << 16);
	if (tlen < 0x2C || tlen > 256u * 1024u) return 0;
	id = d[8] ^ 0xA5;
	if (id == 0 || id > 32) return 0;
	return 1;
}

#ifdef __cplusplus
}
#endif

#endif

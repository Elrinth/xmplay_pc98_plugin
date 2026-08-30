/*
 * xmp-pc98 — native XMPlay input plugin for PC-98 / S98.
 * Not a wrap of in_s98.dll, in_fmpmd.dll, or ZXTune.
 * Classic XMPlay is 32-bit only. DllMain only DisableThreadLibraryCalls.
 */
#if defined(__GNUC__)
#define XMPIN_GetInterface XMPIN_GetInterface_Declared
#endif
#include "xmpin.h"
#if defined(__GNUC__)
#undef XMPIN_GetInterface
#endif

#include "pc98.h"
#include "config.h"
#include "pc98_util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define PLUGIN_NAME    PC98_PLUGIN_NAME
#define PLUGIN_VERSION PC98_PLUGIN_VERSION
#define PLUGIN_XMPVER  PC98_PLUGIN_XMPVER
#define INFO_WRITE_MAX 32766

static XMPFUNC_IN *xmpfin;
static XMPFUNC_MISC *xmpfmisc;
static XMPFUNC_FILE *xmpffile;

static pc98_player *g_play;
static char g_name_hint[512];
static pc98_cfg g_cfg;
#ifdef _WIN32
static HINSTANCE g_hinst;
#endif
static char g_dll_dir[PC98_PATH_MAX];

static void sanitize_line(char *s)
{
	if (!s) return;
	for (; *s; ++s)
		if (*s == '\t' || *s == '\r' || *s == '\n')
			*s = ' ';
}

static void write_kv(char **cursor, char *end, const char *name, const char *value)
{
	size_t nl, vl, need;
	if (!cursor || !*cursor || !end || !name || !value || !value[0])
		return;
	nl = strlen(name);
	vl = strlen(value);
	need = nl + 1 + vl + 1;
	if (*cursor + need >= end)
		return;
	memcpy(*cursor, name, nl); *cursor += nl;
	**cursor = '\t'; *cursor += 1;
	memcpy(*cursor, value, vl); *cursor += vl;
	**cursor = '\r'; *cursor += 1;
	**cursor = '\0';
}

static void *xmp_alloc(DWORD n)
{
	if (!xmpfmisc || !xmpfmisc->Alloc || n == 0)
		return NULL;
	return xmpfmisc->Alloc(n);
}

static void remember_hint(const char *filename)
{
	g_name_hint[0] = '\0';
	if (filename)
		pc98_bounded(g_name_hint, sizeof g_name_hint, filename);
}

static void find_dll_dir(void)
{
#ifdef _WIN32
	if (g_hinst && !g_dll_dir[0]) {
		char dir[MAX_PATH];
		DWORD n = GetModuleFileNameA(g_hinst, dir, MAX_PATH);
		if (n > 0 && n < MAX_PATH) {
			char *slash = strrchr(dir, '\\');
			if (!slash) slash = strrchr(dir, '/');
			if (slash) {
				slash[1] = '\0';
				pc98_bounded(g_dll_dir, sizeof g_dll_dir, dir);
			}
		}
	}
#endif
	pc98_bounded(g_cfg.dll_dir, sizeof g_cfg.dll_dir, g_dll_dir);
}

static int slurp_xmpfile(XMPFILE file, unsigned char **out, size_t *out_len)
{
	DWORD type, sz, got, pos;
	unsigned char *buf;
	if (out) *out = NULL;
	if (out_len) *out_len = 0;
	if (!file || !out || !out_len || !xmpffile || !xmpffile->Read)
		return 0;
	type = xmpffile->GetType(file);
	if (type == XMPFILE_TYPE_MEMORY) {
		const void *mem;
		if (!xmpffile->GetMemory || !xmpffile->GetSize)
			return 0;
		mem = xmpffile->GetMemory(file);
		sz = xmpffile->GetSize(file);
		if (!mem || sz < 4 || (size_t)sz > PC98_MAX_FILE)
			return 0;
		buf = (unsigned char *)malloc(sz);
		if (!buf) return 0;
		memcpy(buf, mem, sz);
		*out = buf;
		*out_len = sz;
		return 1;
	}
	sz = xmpffile->GetSize ? xmpffile->GetSize(file) : 0;
	pos = xmpffile->Tell ? xmpffile->Tell(file) : 0;
	if (xmpffile->Seek)
		xmpffile->Seek(file, 0);
	if (sz > 0) {
		if (sz < 4 || (size_t)sz > PC98_MAX_FILE) {
			if (xmpffile->Seek) xmpffile->Seek(file, pos);
			return 0;
		}
		buf = (unsigned char *)malloc(sz);
		if (!buf) {
			if (xmpffile->Seek) xmpffile->Seek(file, pos);
			return 0;
		}
		got = xmpffile->Read(file, buf, sz);
		if (xmpffile->Seek) xmpffile->Seek(file, pos);
		if (got < 4) { free(buf); return 0; }
		*out = buf;
		*out_len = got;
		return 1;
	}
	{
		size_t cap = 4096, total = 0;
		buf = (unsigned char *)malloc(cap);
		if (!buf) return 0;
		for (;;) {
			DWORD chunk;
			if (total == cap) {
				size_t ncap = cap * 2;
				unsigned char *nb;
				if (ncap > PC98_MAX_FILE) ncap = PC98_MAX_FILE;
				if (ncap <= cap) { free(buf); return 0; }
				nb = (unsigned char *)realloc(buf, ncap);
				if (!nb) { free(buf); return 0; }
				buf = nb;
				cap = ncap;
			}
			chunk = xmpffile->Read(file, buf + total, (DWORD)(cap - total));
			if (chunk == 0) break;
			total += chunk;
			if (total >= PC98_MAX_FILE) break;
		}
		if (xmpffile->Seek) xmpffile->Seek(file, pos);
		if (total < 4) { free(buf); return 0; }
		*out = buf;
		*out_len = total;
		return 1;
	}
}

static XMPFILE open_if_needed(const char *filename, XMPFILE file, int *opened)
{
	*opened = 0;
	if (file) return file;
	if (!filename || !xmpffile || !xmpffile->Open)
		return NULL;
	file = xmpffile->Open(filename);
	if (file) *opened = 1;
	return file;
}

static void close_if_opened(XMPFILE file, int opened)
{
	if (opened && file && xmpffile && xmpffile->Close)
		xmpffile->Close(file);
}

static void set_length_now(int play_ms);

static void apply_cfg(void)
{
	if (!g_play)
		return;
	pc98_player_set_loop_count(g_play, g_cfg.loop_count);
	pc98_player_apply_mute(g_play, &g_cfg);
	set_length_now(pc98_player_play_ms(g_play, pc98_player_song(g_play)));
	if (xmpfin && xmpfin->UpdateTitle)
		xmpfin->UpdateTitle(NULL);
	if (xmpfmisc && xmpfmisc->RefreshInfo)
		xmpfmisc->RefreshInfo(XMPINFO_REFRESH_MAIN | XMPINFO_REFRESH_GENERAL);
}

static void unload_playback(void)
{
	if (g_play) {
		pc98_player_close(g_play);
		g_play = NULL;
	}
	g_name_hint[0] = '\0';
}

static void append_tag(char **p, char *end, const char *key, const char *val)
{
	size_t kl, vl;
	if (!p || !*p || !end || !key || !val || !val[0])
		return;
	kl = strlen(key);
	vl = strlen(val);
	if (*p + kl + 1 + vl + 1 + 1 >= end)
		return;
	memcpy(*p, key, kl); *p += kl;
	**p = '\0'; *p += 1;
	memcpy(*p, val, vl); *p += vl;
	**p = '\0'; *p += 1;
}

static char *finish_tags(char *stack, char *p, size_t stack_sz)
{
	char *end = stack + stack_sz;
	char *out;
	size_t n;
	if (p + 1 < end)
		*p++ = '\0';
	n = (size_t)(p - stack);
	out = (char *)xmp_alloc((DWORD)n);
	if (!out) return NULL;
	memcpy(out, stack, n);
	return out;
}

static char *build_tags_info(const pc98_info *inf)
{
	char stack[8192];
	char *p = stack;
	char *end = stack + sizeof stack;
	if (!xmpfmisc || !inf) return NULL;
	append_tag(&p, end, "filetype", inf->filetype);
	append_tag(&p, end, "title", inf->title);
	append_tag(&p, end, "artist", inf->artist);
	append_tag(&p, end, "album", inf->game);
	append_tag(&p, end, "comment", inf->engine);
	return finish_tags(stack, p, sizeof stack);
}

static char *build_tags_play(pc98_player *pl)
{
	char stack[8192];
	char *p = stack;
	char *end = stack + sizeof stack;
	if (!pl || !xmpfmisc) return NULL;
	append_tag(&p, end, "filetype", pc98_player_filetype(pl));
	append_tag(&p, end, "title", pc98_player_title(pl));
	append_tag(&p, end, "artist", pc98_player_artist(pl));
	append_tag(&p, end, "album", pc98_player_game(pl));
	append_tag(&p, end, "comment", pc98_player_engine(pl));
	return finish_tags(stack, p, sizeof stack);
}

static void set_length_now(int play_ms)
{
	float sec;
	if (!xmpfin || !xmpfin->SetLength || play_ms <= 0)
		return;
	sec = (float)play_ms / 1000.0f;
	if (sec > 0.0f && sec < 86400.0f)
		xmpfin->SetLength(sec, TRUE);
}

static float info_sec(int one_ms)
{
	if (one_ms > 0)
		return (float)one_ms / 1000.0f;
	/* Cache miss / unmeasured: 1s, never dummy 3:00. */
	return 1.0f;
}

static void WINAPI pc_About(HWND win)
{
	char buf[2800];
	snprintf(buf, sizeof buf,
		PLUGIN_NAME " " PLUGIN_VERSION "\r\n"
		"Native XMPlay input plugin for NEC PC-98 music.\r\n"
		"S98 (cisc fmgen, same core as in_s98), PMD (pmdmini / PMDWin),\r\n"
		"FMP (optional WinFMP.dll), BGMDRV .MUS, Packen NA .O, Melody\r\n"
		"MFD/USO, N3G .MDT, Synthia .PAI, MBMUS .MSB, OPNDRV .MD,\r\n"
		"ArtDink .NTL (F_* FM, G_* GM/SF2), FUGA packed .GMD/.MMD (SC-55 / CM-64 SF2),\r\n"
		"and Hoot sets via hootrip.\r\n\r\n"
		"This is NOT a wrap of S98Amp / in_s98.dll, in_fmpmd.dll, or ZXTune.\r\n"
		"Delete or disable S98Amp, in_fmpmd, and ZXTune's S98/.M claims\r\n"
		"so they do not fight this plugin. ZXTune .M is ZX Spectrum, not PMD.\r\n"
		"We do not claim .vgm/.vgz (xmp-gamemusic) or .sap (xmp-pokey).\r\n"
		"We do not claim .mid (XMPlay MIDI), .mml source, or .com/.exe.\r\n\r\n"
		"CheckFile probes a 256-byte peek (S98, PMD/FMP/BGMDRV/NA/USO/MDT/PAI,\r\n"
		"MSB/MD/NTL/GMD, set.pc98). G_*.NTL is GM (SF2), not OPNA. .O is claimed only if it looks like Packen NA, not\r\n"
		"as a generic object file. Playlist length is measured one loop.\r\n"
		"S98 mixes at 55466 Hz through cisc fmgen (VolumeFM=0, SSG -18\r\n"
		"on PC-9801 / -8 on PC-8801), then interpolates to XMPlay.\r\n\r\n"
		"Hoot: run pc98-scan to write set.pc98; first play may spawn\r\n"
		"hootrip.exe and cache .s98 for leftover custom drivers.\r\n"
		"Put 2608_*.WAV next to the DLL or set the rhythm path.\r\n"
		"WinFMP.dll is optional for .OPI/.FMD/.MMD.\r\n"
		"Packed Oerstedia .GMD/.MMD use SC-55.sf2 and an MT-32/CM-64 SF2\r\n"
		"from XMPlay's midi soundfonts folder (or gs_sf2 / mt_sf2 in the ini).\r\n"
		"32-bit XMPlay only (PE32 i386). License: GPLv2+.");
#ifdef _WIN32
	MessageBoxA(win, buf, PLUGIN_NAME, MB_OK | MB_ICONINFORMATION);
#else
	(void)win;
	(void)buf;
#endif
}

#ifdef _WIN32
#define IDC_LOOP1   1001
#define IDC_LOOP2   1002
#define IDC_LOOP3   1003
#define IDC_MUTEFM  1010
#define IDC_MUTESSG 1011
#define IDC_MUTERHY 1012
#define IDC_MUTEPCM 1013
#define IDC_RATE0   1020
#define IDC_RATE1   1021
#define IDC_RHYTHM  1030
#define IDC_XML     1031
#define IDC_ROOT    1032
#define IDC_CACHE   1033
#define IDC_HOOTRIP 1034
#define IDC_GSSF2   1035
#define IDC_MTSF2   1036

static pc98_cfg g_cfg_dlg_backup;

static void cfg_dlg_read_all(HWND hwnd)
{
	if (IsDlgButtonChecked(hwnd, IDC_LOOP1) == BST_CHECKED)
		g_cfg.loop_count = 1;
	else if (IsDlgButtonChecked(hwnd, IDC_LOOP3) == BST_CHECKED)
		g_cfg.loop_count = 3;
	else
		g_cfg.loop_count = 2;
	g_cfg.mute_fm = IsDlgButtonChecked(hwnd, IDC_MUTEFM) == BST_CHECKED;
	g_cfg.mute_ssg = IsDlgButtonChecked(hwnd, IDC_MUTESSG) == BST_CHECKED;
	g_cfg.mute_rhythm = IsDlgButtonChecked(hwnd, IDC_MUTERHY) == BST_CHECKED;
	g_cfg.mute_adpcm = IsDlgButtonChecked(hwnd, IDC_MUTEPCM) == BST_CHECKED;
	g_cfg.rate = (IsDlgButtonChecked(hwnd, IDC_RATE1) == BST_CHECKED) ? 48000 : 44100;
	GetDlgItemTextA(hwnd, IDC_RHYTHM, g_cfg.rhythm_path, PC98_PATH_MAX);
	GetDlgItemTextA(hwnd, IDC_XML, g_cfg.hoot_xml, PC98_PATH_MAX);
	GetDlgItemTextA(hwnd, IDC_ROOT, g_cfg.music_root, PC98_PATH_MAX);
	GetDlgItemTextA(hwnd, IDC_CACHE, g_cfg.cache_dir, PC98_PATH_MAX);
	GetDlgItemTextA(hwnd, IDC_HOOTRIP, g_cfg.hootrip_path, PC98_PATH_MAX);
	GetDlgItemTextA(hwnd, IDC_GSSF2, g_cfg.gs_sf2, PC98_PATH_MAX);
	GetDlgItemTextA(hwnd, IDC_MTSF2, g_cfg.mt_sf2, PC98_PATH_MAX);
	pc98_cfg_clamp(&g_cfg);
}

static INT_PTR CALLBACK cfg_dlg(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	(void)lp;
	switch (msg) {
	case WM_INITDIALOG:
		g_cfg_dlg_backup = g_cfg;
		CheckRadioButton(hwnd, IDC_LOOP1, IDC_LOOP3,
			IDC_LOOP1 + (g_cfg.loop_count - 1));
		CheckDlgButton(hwnd, IDC_MUTEFM, g_cfg.mute_fm ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(hwnd, IDC_MUTESSG, g_cfg.mute_ssg ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(hwnd, IDC_MUTERHY, g_cfg.mute_rhythm ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(hwnd, IDC_MUTEPCM, g_cfg.mute_adpcm ? BST_CHECKED : BST_UNCHECKED);
		CheckRadioButton(hwnd, IDC_RATE0, IDC_RATE1,
			g_cfg.rate == 48000 ? IDC_RATE1 : IDC_RATE0);
		SetDlgItemTextA(hwnd, IDC_RHYTHM, g_cfg.rhythm_path);
		SetDlgItemTextA(hwnd, IDC_XML, g_cfg.hoot_xml);
		SetDlgItemTextA(hwnd, IDC_ROOT, g_cfg.music_root);
		SetDlgItemTextA(hwnd, IDC_CACHE, g_cfg.cache_dir);
		SetDlgItemTextA(hwnd, IDC_HOOTRIP, g_cfg.hootrip_path);
		SetDlgItemTextA(hwnd, IDC_GSSF2, g_cfg.gs_sf2);
		SetDlgItemTextA(hwnd, IDC_MTSF2, g_cfg.mt_sf2);
		return TRUE;
	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case IDOK:
			cfg_dlg_read_all(hwnd);
			pc98_cfg_save_ini(&g_cfg, g_dll_dir);
			apply_cfg();
			EndDialog(hwnd, IDOK);
			return TRUE;
		case IDCANCEL:
			g_cfg = g_cfg_dlg_backup;
			pc98_cfg_clamp(&g_cfg);
			EndDialog(hwnd, IDCANCEL);
			return TRUE;
		}
		break;
	}
	return FALSE;
}

static void WINAPI pc_Config(HWND win)
{
	WORD *p;
	DLGTEMPLATE *dlg;
	unsigned char raw[4096];
	memset(raw, 0, sizeof raw);
	dlg = (DLGTEMPLATE *)raw;
	dlg->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT;
	dlg->cdit = 28;
	dlg->x = 10; dlg->y = 10; dlg->cx = 260; dlg->cy = 228;
	p = (WORD *)(dlg + 1);
	*p++ = 0;
	*p++ = 0;
	{
		const wchar_t *cap = L"PC-98 / S98";
		size_t i;
		for (i = 0; cap[i]; ++i) *p++ = (WORD)cap[i];
		*p++ = 0;
	}
	*p++ = 9;
	{
		const wchar_t *fnt = L"MS Shell Dlg";
		size_t i;
		for (i = 0; fnt[i]; ++i) *p++ = (WORD)fnt[i];
		*p++ = 0;
	}
#define ADDCTL(_id, _x, _y, _w, _h, _style, _clsid, _title) do { \
	DLGITEMTEMPLATE *item; \
	if (((uintptr_t)p) & 3) p = (WORD *)((((uintptr_t)p) + 3) & ~(uintptr_t)3); \
	item = (DLGITEMTEMPLATE *)p; \
	item->style = WS_CHILD | WS_VISIBLE | (_style); \
	item->x = (short)(_x); item->y = (short)(_y); \
	item->cx = (short)(_w); item->cy = (short)(_h); \
	item->id = (WORD)(_id); \
	p = (WORD *)(item + 1); \
	*p++ = 0xFFFF; *p++ = (WORD)(_clsid); \
	{ const wchar_t *_t = (_title); size_t _i; \
	for (_i = 0; _t[_i]; ++_i) *p++ = (WORD)_t[_i]; *p++ = 0; } \
	*p++ = 0; \
} while (0)
	ADDCTL(-1, 8, 8, 40, 10, 0, 0x0082, L"Loops");
	ADDCTL(IDC_LOOP1, 50, 6, 28, 12, WS_TABSTOP | BS_AUTORADIOBUTTON | WS_GROUP, 0x0080, L"1");
	ADDCTL(IDC_LOOP2, 82, 6, 28, 12, WS_TABSTOP | BS_AUTORADIOBUTTON, 0x0080, L"2");
	ADDCTL(IDC_LOOP3, 114, 6, 28, 12, WS_TABSTOP | BS_AUTORADIOBUTTON, 0x0080, L"3");
	ADDCTL(-1, 8, 22, 40, 10, 0, 0x0082, L"Mute");
	ADDCTL(IDC_MUTEFM, 50, 20, 36, 12, WS_TABSTOP | BS_AUTOCHECKBOX, 0x0080, L"FM");
	ADDCTL(IDC_MUTESSG, 90, 20, 36, 12, WS_TABSTOP | BS_AUTOCHECKBOX, 0x0080, L"SSG");
	ADDCTL(IDC_MUTERHY, 130, 20, 44, 12, WS_TABSTOP | BS_AUTOCHECKBOX, 0x0080, L"Rhythm");
	ADDCTL(IDC_MUTEPCM, 178, 20, 50, 12, WS_TABSTOP | BS_AUTOCHECKBOX, 0x0080, L"ADPCM");
	ADDCTL(-1, 8, 38, 40, 10, 0, 0x0082, L"Rate");
	ADDCTL(IDC_RATE0, 50, 36, 56, 12, WS_TABSTOP | BS_AUTORADIOBUTTON | WS_GROUP, 0x0080, L"44100");
	ADDCTL(IDC_RATE1, 110, 36, 56, 12, WS_TABSTOP | BS_AUTORADIOBUTTON, 0x0080, L"48000");
	ADDCTL(-1, 8, 54, 48, 10, 0, 0x0082, L"Rhythm");
	ADDCTL(IDC_RHYTHM, 58, 52, 192, 12, WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER, 0x0081, L"");
	ADDCTL(-1, 8, 70, 48, 10, 0, 0x0082, L"Hoot XML");
	ADDCTL(IDC_XML, 58, 68, 192, 12, WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER, 0x0081, L"");
	ADDCTL(-1, 8, 86, 48, 10, 0, 0x0082, L"Music");
	ADDCTL(IDC_ROOT, 58, 84, 192, 12, WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER, 0x0081, L"");
	ADDCTL(-1, 8, 102, 48, 10, 0, 0x0082, L"Cache");
	ADDCTL(IDC_CACHE, 58, 100, 192, 12, WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER, 0x0081, L"");
	ADDCTL(-1, 8, 118, 48, 10, 0, 0x0082, L"hootrip");
	ADDCTL(IDC_HOOTRIP, 58, 116, 192, 12, WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER, 0x0081, L"");
	ADDCTL(-1, 8, 134, 48, 10, 0, 0x0082, L"SC-55");
	ADDCTL(IDC_GSSF2, 58, 132, 192, 12, WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER, 0x0081, L"");
	ADDCTL(-1, 8, 150, 48, 10, 0, 0x0082, L"CM-64");
	ADDCTL(IDC_MTSF2, 58, 148, 192, 12, WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER, 0x0081, L"");
	ADDCTL(IDOK, 148, 206, 46, 14, WS_TABSTOP | BS_DEFPUSHBUTTON, 0x0080, L"OK");
	ADDCTL(IDCANCEL, 200, 206, 46, 14, WS_TABSTOP | BS_PUSHBUTTON, 0x0080, L"Cancel");
#undef ADDCTL
	DialogBoxIndirectParamA(g_hinst, dlg, win, cfg_dlg, 0);
}
#else
static void WINAPI pc_Config(HWND win) { (void)win; }
#endif

static BOOL WINAPI pc_CheckFile(const char *filename, XMPFILE file)
{
	unsigned char hdr[256];
	size_t got = 0;
	int opened = 0;
	DWORD type;

	file = open_if_needed(filename, file, &opened);
	if (!file)
		return FALSE;
	type = xmpffile->GetType ? xmpffile->GetType(file) : (DWORD)-1;
	if (type == XMPFILE_TYPE_MEMORY) {
		const void *mem;
		DWORD sz;
		if (!xmpffile->GetMemory || !xmpffile->GetSize) {
			close_if_opened(file, opened);
			return FALSE;
		}
		mem = xmpffile->GetMemory(file);
		sz = xmpffile->GetSize(file);
		if (!mem || sz < 4) {
			close_if_opened(file, opened);
			return FALSE;
		}
		got = sz < sizeof hdr ? (size_t)sz : sizeof hdr;
		memcpy(hdr, mem, got);
	} else {
		DWORD pos = xmpffile->Tell ? xmpffile->Tell(file) : 0;
		if (xmpffile->Seek)
			xmpffile->Seek(file, 0);
		got = xmpffile->Read ? xmpffile->Read(file, hdr, (DWORD)sizeof hdr) : 0;
		if (xmpffile->Seek)
			xmpffile->Seek(file, pos);
		if (got < 4) {
			close_if_opened(file, opened);
			return FALSE;
		}
	}
	close_if_opened(file, opened);
	return pc98_probe(filename, hdr, got) != PC98_KIND_NONE ? TRUE : FALSE;
}

static DWORD WINAPI pc_GetFileInfo(const char *filename, XMPFILE file,
		float **length, char **tags)
{
	unsigned char *data = NULL;
	size_t len = 0;
	int opened = 0;
	pc98_info inf;
	int n, i;

	if (length) *length = NULL;
	if (tags) *tags = NULL;

	file = open_if_needed(filename, file, &opened);
	if (!file)
		return 0;
	if (!slurp_xmpfile(file, &data, &len)) {
		close_if_opened(file, opened);
		return 0;
	}
	close_if_opened(file, opened);

	if (pc98_analyze(filename, data, len, &g_cfg, &inf) != 0) {
		free(data);
		return 0;
	}
	free(data);

	n = inf.songs > 0 ? inf.songs : 1;
	if (length) {
		float *lens = (float *)xmp_alloc((DWORD)(sizeof(float) * (unsigned)n));
		if (lens) {
			for (i = 0; i < n; ++i)
				lens[i] = info_sec(inf.one_loop_ms[i]);
		}
		*length = lens;
	}
	if (tags)
		*tags = build_tags_info(&inf);
	return (DWORD)n | XMPIN_INFO_NOSUBTAGS;
}

static DWORD WINAPI pc_Open(const char *filename, XMPFILE file)
{
	unsigned char *data = NULL;
	size_t len = 0;
	int opened = 0;

	unload_playback();
	remember_hint(filename);
	find_dll_dir();

	file = open_if_needed(filename, file, &opened);
	if (!file)
		return 0;
	if (!slurp_xmpfile(file, &data, &len)) {
		close_if_opened(file, opened);
		return 0;
	}
	close_if_opened(file, opened);

	g_play = pc98_player_open(filename, data, len, &g_cfg);
	free(data);
	if (!g_play)
		return 0;
	set_length_now(pc98_player_play_ms(g_play, pc98_player_song(g_play)));
	return 2;
}

static void WINAPI pc_Close(void)
{
	unload_playback();
}

static void WINAPI pc_SetFormat(XMPFORMAT *form)
{
	if (!form)
		return;
	if (!g_play) {
		form->rate = 0;
		form->chan = 0;
		form->res = 0;
		form->chanmask = 0;
		return;
	}
	form->rate = (DWORD)pc98_player_rate(g_play);
	form->chan = 2;
	form->res = 4;
	form->chanmask = 0;
}

static char *WINAPI pc_GetTags(void)
{
	if (!g_play)
		return NULL;
	return build_tags_play(g_play);
}

static void WINAPI pc_GetInfoText(char *format, char *length)
{
	char tmp[256];
	int m, s, play;
	if (format) format[0] = '\0';
	if (length) length[0] = '\0';
	if (!g_play)
		return;
	if (format) {
		snprintf(tmp, sizeof tmp, "%s · %s",
			pc98_player_filetype(g_play), pc98_player_engine(g_play));
		sanitize_line(tmp);
		pc98_bounded(format, 256, tmp);
	}
	if (length) {
		play = pc98_player_one_loop_ms(g_play, pc98_player_song(g_play));
		m = play / 60000;
		s = (play / 1000) % 60;
		if (pc98_player_songs(g_play) > 1)
			snprintf(tmp, sizeof tmp, "%d:%02d track %d/%d",
				m, s, pc98_player_song(g_play) + 1, pc98_player_songs(g_play));
		else
			snprintf(tmp, sizeof tmp, "%d:%02d", m, s);
		sanitize_line(tmp);
		pc98_bounded(length, 256, tmp);
	}
}

static void WINAPI pc_GetGeneralInfo(char *buf)
{
	char local[4096];
	char *p, *end;
	char num[32];
	if (!buf) return;
	buf[0] = '\0';
	if (!g_play) return;
	p = local;
	end = local + sizeof local - 2;
	local[0] = '\0';
	write_kv(&p, end, "Title", pc98_player_title(g_play));
	write_kv(&p, end, "Artist", pc98_player_artist(g_play));
	write_kv(&p, end, "Game", pc98_player_game(g_play));
	write_kv(&p, end, "Format", pc98_player_filetype(g_play));
	write_kv(&p, end, "Engine", pc98_player_engine(g_play));
	if (pc98_player_songs(g_play) > 1) {
		snprintf(num, sizeof num, "%d", pc98_player_songs(g_play));
		write_kv(&p, end, "Tracks", num);
		snprintf(num, sizeof num, "%d", pc98_player_song(g_play) + 1);
		write_kv(&p, end, "Current track", num);
	}
	snprintf(num, sizeof num, "%d", pc98_player_loop_count(g_play));
	write_kv(&p, end, "Loop count", num);
	write_kv(&p, end, "Player", PLUGIN_NAME " " PLUGIN_VERSION);
	write_kv(&p, end, "Note", "Not S98Amp / in_fmpmd / ZXTune");
	pc98_bounded(buf, INFO_WRITE_MAX, local);
}

static void WINAPI pc_GetMessage(char *buf)
{
	if (!buf) return;
	buf[0] = '\0';
}

static double WINAPI pc_GetGranularity(void)
{
	return 0.05;
}

static double WINAPI pc_SetPosition(DWORD pos)
{
	int sub;
	int ms;

	if (!g_play)
		return -1.0;

	if (pos == (DWORD)XMPIN_POS_LOOP || pos == (DWORD)XMPIN_POS_AUTOLOOP)
		return -2.0;

	if (pos & XMPIN_POS_SUBSONG) {
		sub = (int)(pos & 0xFFFFu);
		if (pc98_player_set_song(g_play, sub) != 0)
			return -1.0;
		set_length_now(pc98_player_play_ms(g_play, sub));
		if (xmpfin && xmpfin->UpdateTitle)
			xmpfin->UpdateTitle(NULL);
		return 0.0;
	}

	ms = (int)((double)pos * 50.0 + 0.5);
	ms = pc98_player_seek_ms(g_play, ms);
	if (ms < 0)
		return -1.0;
	return (double)ms / 1000.0;
}

static DWORD WINAPI pc_Process(float *buf, DWORD count)
{
	int frames, got;
	if (!buf || !g_play)
		return 0;
	frames = (int)(count / 2u);
	if (frames <= 0)
		return 0;
	got = pc98_player_process(g_play, buf, frames);
	if (got <= 0)
		return 0;
	return (DWORD)got * 2u;
}

static DWORD WINAPI pc_GetSubSongs(float *length)
{
	if (!g_play)
		return 0;
	if (length)
		*length = (float)pc98_player_total_one_loop_ms(g_play) / 1000.0f;
	return (DWORD)pc98_player_songs(g_play);
}

static DWORD WINAPI pc_GetConfig(void *config)
{
	if (config)
		memcpy(config, &g_cfg, sizeof g_cfg);
	return (DWORD)sizeof g_cfg;
}

static void WINAPI pc_SetConfig(void *config, DWORD size)
{
	if (!config || size == 0)
		return;
	pc98_cfg_defaults(&g_cfg);
	if (size > sizeof g_cfg)
		size = (DWORD)sizeof g_cfg;
	memcpy(&g_cfg, config, size);
	find_dll_dir();
	pc98_bounded(g_cfg.dll_dir, sizeof g_cfg.dll_dir, g_dll_dir);
	pc98_cfg_clamp(&g_cfg);
	apply_cfg();
}

static const char g_exts[] =
	"PC-98 / S98\0s98/m/m2/ms/mp/mz/opi/ovi/ozi/fmd/mmd/gmd/mus/uso/o/mdt/msb/ntl/md/pc98";

static XMPIN g_xmpin = {
	XMPIN_FLAG_CONFIG,
	PLUGIN_NAME " " PLUGIN_VERSION,
	g_exts,
	pc_About,
	pc_Config,
	pc_CheckFile,
	pc_GetFileInfo,
	pc_Open,
	pc_Close,
	NULL,
	pc_SetFormat,
	pc_GetTags,
	pc_GetInfoText,
	pc_GetGeneralInfo,
	pc_GetMessage,
	pc_SetPosition,
	pc_GetGranularity,
	NULL,
	pc_Process,
	NULL,
	NULL,
	pc_GetSubSongs,
	NULL,
	NULL,
	NULL,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	pc_GetConfig,
	pc_SetConfig,
	NULL
};

static XMPIN *WINAPI xmpin_get_interface_impl(DWORD face, InterfaceProc faceproc)
{
	if (face != XMPIN_FACE)
		return NULL;
	if (!faceproc)
		return NULL;
	xmpfin = (XMPFUNC_IN *)faceproc(XMPFUNC_IN_FACE);
	xmpfmisc = (XMPFUNC_MISC *)faceproc(XMPFUNC_MISC_FACE);
	xmpffile = (XMPFUNC_FILE *)faceproc(XMPFUNC_FILE_FACE);
	if (!xmpfin || !xmpfmisc || !xmpffile)
		return NULL;
	if (!xmpfmisc->Alloc || !xmpffile->Read)
		return NULL;
	find_dll_dir();
	if (g_cfg.loop_count < 1) {
		pc98_cfg_load_ini(&g_cfg, g_dll_dir);
		if (g_cfg.loop_count < 1)
			pc98_cfg_defaults(&g_cfg);
	}
	pc98_bounded(g_cfg.dll_dir, sizeof g_cfg.dll_dir, g_dll_dir);
	if (!g_cfg.music_root[0])
		pc98_bounded(g_cfg.music_root, sizeof g_cfg.music_root,
			"D:\\spel\\pc98\\pc98 music");
	pc98_cfg_clamp(&g_cfg);
	(void)PLUGIN_XMPVER;
	return &g_xmpin;
}

extern "C" {

BOOL WINAPI DllMain(HINSTANCE hDLL, DWORD reason, LPVOID reserved)
{
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
#ifdef _WIN32
		g_hinst = hDLL;
		DisableThreadLibraryCalls(hDLL);
#endif
	}
	return TRUE;
}

#if defined(__GNUC__) && defined(_WIN32) && !defined(_WIN64)
XMPIN *WINAPI XMPIN_GetInterface_(DWORD face, InterfaceProc faceproc)
{
	return xmpin_get_interface_impl(face, faceproc);
}
#if __GNUC__ >= 8
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattribute-alias"
#endif
__attribute__((dllexport)) void XMPIN_GetInterface(void)
	__attribute__((alias("XMPIN_GetInterface_@8")));
#if __GNUC__ >= 8
#pragma GCC diagnostic pop
#endif
#else
__declspec(dllexport) XMPIN *WINAPI XMPIN_GetInterface(DWORD face, InterfaceProc faceproc)
{
	return xmpin_get_interface_impl(face, faceproc);
}
#endif

} /* extern "C" */

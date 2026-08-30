#include "hoot_engine.h"
#include "s98_engine.h"
#include "bgmdrv_engine.h"
#include "pc98_util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <vector>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

struct hoot_song {
	int code;
	char name[256];
	char cache[PC98_PATH_MAX];
	int one_loop_ms;
};

struct hoot_set {
	char id[64];
	char title[256];
	char folder[PC98_PATH_MAX];
	int nsong;
	hoot_song songs[PC98_MAX_SONGS];
};

static int file_exists(const char *p);
static int load_file(const char *path, std::vector<uint8_t> *out);

static int is_pc98set(const uint8_t *data, size_t len)
{
	if (!data || len < 8) return 0;
	return memcmp(data, "PC98SET", 7) == 0;
}

static const uint8_t *find_bytes(const uint8_t *h, size_t n, const char *n1)
{
	size_t m = strlen(n1);
	size_t i;
	if (m == 0 || m > n) return NULL;
	for (i = 0; i + m <= n; ++i) {
		if (memcmp(h + i, n1, m) == 0)
			return h + i;
	}
	return NULL;
}

int hoot_probe_mem(const uint8_t *data, size_t len)
{
	if (is_pc98set(data, len)) return 1;
	if (len >= 5 && memcmp(data, "<?xml", 5) == 0) {
		if (find_bytes(data, len, "song") &&
				(find_bytes(data, len, "soft") || find_bytes(data, len, "game")))
			return 1;
	}
	return 0;
}

static int sibling_set_path(const char *filename, char *out, size_t cap)
{
	char dir[PC98_PATH_MAX];
	if (!filename || !out || cap < 12) return 0;
	pc98_dir_of(filename, dir, sizeof dir);
	if (!dir[0]) return 0;
	snprintf(out, cap, "%sset.pc98", dir);
	return 1;
}

int hoot_probe_file(const char *filename, const uint8_t *data, size_t len)
{
	char setpath[PC98_PATH_MAX];
	if (hoot_probe_mem(data, len)) return 1;
	if (pc98_looks_bgmdrv(data, len)) return 1;
	if (filename && pc98_iends(filename, ".uso")) return 1;
	if (filename && pc98_is_sidecar_name(filename) &&
			sibling_set_path(filename, setpath, sizeof setpath) &&
			file_exists(setpath))
		return 1;
	return 0;
}

int pc98_set_write(const char *path, const char *set_id, const char *title,
		int nsong, const char *const *names, const int *codes)
{
	FILE *fp;
	int i, n = 0;
	if (!path || !set_id) return -1;
	fp = fopen(path, "wb");
	if (!fp) return -1;
	fprintf(fp, "PC98SET 1\n");
	fprintf(fp, "id=%s\n", set_id);
	if (title && title[0]) fprintf(fp, "title=%s\n", title);
	for (i = 0; i < nsong; ++i) {
		const char *nm = names && names[i] ? names[i] : "";
		int code = codes ? codes[i] : i + 1;
		if (pc98_is_stop_title(nm))
			continue;
		fprintf(fp, "%d\t%d\t%s\n", n + 1, code, nm);
		n++;
		if (n >= PC98_MAX_SONGS) break;
	}
	if (n == 0)
		fprintf(fp, "1\t1\t%s\n", title && title[0] ? title : set_id);
	fclose(fp);
	return 0;
}

static void add_song(hoot_set *st, int code, const char *name)
{
	hoot_song *s;
	if (!st || st->nsong >= PC98_MAX_SONGS) return;
	if (pc98_is_stop_title(name)) return;
	s = &st->songs[st->nsong++];
	memset(s, 0, sizeof *s);
	s->code = code > 0 ? code : st->nsong;
	pc98_bounded(s->name, sizeof s->name, name ? name : "");
}

int pc98_set_parse(const uint8_t *data, size_t len, pc98_info *out,
		char paths[][PC98_PATH_MAX], int *codes, int max_songs)
{
	hoot_set st;
	char *buf, *line, *next;
	int i;
	if (!data || !is_pc98set(data, len)) return -1;
	memset(&st, 0, sizeof st);
	buf = (char *)malloc(len + 1);
	if (!buf) return -1;
	memcpy(buf, data, len);
	buf[len] = 0;
	for (line = buf; line && *line; line = next) {
		char *tab1, *tab2;
		next = strchr(line, '\n');
		if (next) *next++ = 0;
		if (line[0] && line[strlen(line) - 1] == '\r')
			line[strlen(line) - 1] = 0;
		if (!line[0] || line[0] == '#') continue;
		if (strncmp(line, "PC98SET", 7) == 0) continue;
		if (strncmp(line, "id=", 3) == 0) {
			pc98_bounded(st.id, sizeof st.id, line + 3);
			continue;
		}
		if (strncmp(line, "title=", 6) == 0) {
			pc98_bounded(st.title, sizeof st.title, line + 6);
			continue;
		}
		tab1 = strchr(line, '\t');
		if (!tab1) continue;
		*tab1++ = 0;
		tab2 = strchr(tab1, '\t');
		if (tab2) *tab2++ = 0;
		add_song(&st, atoi(tab1), tab2 ? tab2 : "");
	}
	free(buf);
	if (out) {
		memset(out, 0, sizeof *out);
		out->kind = PC98_KIND_SET;
		out->songs = st.nsong > 0 ? st.nsong : 1;
		pc98_bounded(out->title, sizeof out->title, st.title[0] ? st.title : st.id);
		pc98_bounded(out->game, sizeof out->game, st.title);
		pc98_bounded(out->set_id, sizeof out->set_id, st.id);
		pc98_bounded(out->filetype, sizeof out->filetype, "PC98 set");
		pc98_bounded(out->engine, sizeof out->engine, "hootrip / S98 cache");
		for (i = 0; i < out->songs && i < PC98_MAX_SONGS; ++i)
			out->one_loop_ms[i] = 0;
	}
	if (codes) {
		for (i = 0; i < st.nsong && i < max_songs; ++i)
			codes[i] = st.songs[i].code;
	}
	if (paths) {
		for (i = 0; i < st.nsong && i < max_songs; ++i)
			pc98_bounded(paths[i], PC98_PATH_MAX, st.songs[i].name);
	}
	return st.nsong;
}

static int parse_set(const uint8_t *data, size_t len, hoot_set *st)
{
	char *buf, *line, *next;
	if (!data || !st || !is_pc98set(data, len)) return -1;
	memset(st, 0, sizeof *st);
	buf = (char *)malloc(len + 1);
	if (!buf) return -1;
	memcpy(buf, data, len);
	buf[len] = 0;
	for (line = buf; line && *line; line = next) {
		char *tab1, *tab2;
		next = strchr(line, '\n');
		if (next) *next++ = 0;
		if (line[0] && line[strlen(line) - 1] == '\r')
			line[strlen(line) - 1] = 0;
		if (!line[0] || line[0] == '#') continue;
		if (strncmp(line, "PC98SET", 7) == 0) continue;
		if (strncmp(line, "id=", 3) == 0) {
			pc98_bounded(st->id, sizeof st->id, line + 3);
			continue;
		}
		if (strncmp(line, "title=", 6) == 0) {
			pc98_bounded(st->title, sizeof st->title, line + 6);
			continue;
		}
		tab1 = strchr(line, '\t');
		if (!tab1) continue;
		*tab1++ = 0;
		tab2 = strchr(tab1, '\t');
		if (tab2) *tab2++ = 0;
		add_song(st, atoi(tab1), tab2 ? tab2 : "");
	}
	free(buf);
	if (st->nsong == 0)
		add_song(st, 1, st->title[0] ? st->title : st->id);
	return 0;
}

static void cache_path_for(const pc98_cfg *cfg, const char *set_id, int idx, int code,
		char *out, size_t cap)
{
	const char *root;
	char fallback[PC98_PATH_MAX];
	root = (cfg && cfg->cache_dir[0]) ? cfg->cache_dir : NULL;
	if (!root && cfg && cfg->dll_dir[0]) {
		snprintf(fallback, sizeof fallback, "%scache\\", cfg->dll_dir);
		root = fallback;
	}
	if (!root) root = "cache";
	snprintf(out, cap, "%s%s\\%02d.s98", root, set_id, idx);
	(void)code;
}

static int file_exists(const char *p)
{
#ifdef _WIN32
	DWORD a = GetFileAttributesA(p);
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
	struct stat st;
	return p && stat(p, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

static int load_file(const char *path, std::vector<uint8_t> *out)
{
	FILE *fp;
	long sz;
	if (!path || !out) return -1;
	fp = fopen(path, "rb");
	if (!fp) return -1;
	fseek(fp, 0, SEEK_END);
	sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (sz <= 0 || sz > (long)PC98_MAX_FILE) { fclose(fp); return -1; }
	out->resize((size_t)sz);
	if (fread(out->data(), 1, (size_t)sz, fp) != (size_t)sz) { fclose(fp); return -1; }
	fclose(fp);
	return 0;
}

static int cmp_name256(const void *a, const void *b)
{
	const char *sa = (const char *)a;
	const char *sb = (const char *)b;
	while (*sa && *sb) {
		int d = tolower((unsigned char)*sa) - tolower((unsigned char)*sb);
		if (d) return d;
		sa++;
		sb++;
	}
	return (int)(unsigned char)*sa - (int)(unsigned char)*sb;
}

static int is_sidecar_filename(const char *n)
{
	if (!n || n[0] == '.') return 0;
	if (pc98_ieq(n, "set.pc98")) return 0;
	return pc98_is_sidecar_name(n);
}

static void folder_star(const char *folder, char *spec, size_t cap)
{
	size_t n = folder ? strlen(folder) : 0;
	if (n && (folder[n - 1] == '\\' || folder[n - 1] == '/'))
		snprintf(spec, cap, "%s*", folder);
	else
#ifdef _WIN32
		snprintf(spec, cap, "%s\\*", folder ? folder : "");
#else
		snprintf(spec, cap, "%s/*", folder ? folder : "");
#endif
}

static int collect_folder_sidecars(const char *folder, char names[][256],
		int *codes, int maxn)
{
	int n = 0;
	if (!folder || !folder[0] || !names || maxn <= 0) return 0;
#ifdef _WIN32
	{
		char spec[PC98_PATH_MAX];
		WIN32_FIND_DATAA fd;
		HANDLE h;
		folder_star(folder, spec, sizeof spec);
		h = FindFirstFileA(spec, &fd);
		if (h == INVALID_HANDLE_VALUE) return 0;
		do {
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
			if (!is_sidecar_filename(fd.cFileName)) continue;
			if (n >= maxn) break;
			pc98_bounded(names[n], 256, fd.cFileName);
			n++;
		} while (FindNextFileA(h, &fd));
		FindClose(h);
	}
#else
	{
		DIR *d = opendir(folder);
		struct dirent *e;
		if (!d) return 0;
		while ((e = readdir(d)) != NULL) {
			if (e->d_name[0] == '.') continue;
			if (!is_sidecar_filename(e->d_name)) continue;
			if (n >= maxn) break;
			pc98_bounded(names[n], 256, e->d_name);
			n++;
		}
		closedir(d);
	}
#endif
	if (n > 1)
		qsort(names, (size_t)n, 256, cmp_name256);
	if (codes) {
		int i;
		for (i = 0; i < n; ++i)
			codes[i] = i + 1;
	}
	return n;
}

static void folder_id_from_path(const char *filename, char *out, size_t cap)
{
	char dir[PC98_PATH_MAX];
	char *p;
	if (!out || cap == 0) return;
	out[0] = 0;
	pc98_dir_of(filename, dir, sizeof dir);
	if (!dir[0]) {
		pc98_basename(filename, out, cap);
		return;
	}
	p = dir + strlen(dir);
	while (p > dir && (p[-1] == '\\' || p[-1] == '/'))
		*--p = 0;
	pc98_basename(dir, out, cap);
}

static int set_is_dummy(const hoot_set *st)
{
	if (!st || st->nsong != 1) return 0;
	if (pc98_ieq(st->songs[0].name, st->id)) return 1;
	if (st->title[0] && pc98_ieq(st->songs[0].name, st->title)) return 1;
	return 0;
}

static void expand_if_dummy(hoot_set *st, const char *filename)
{
	char dir[PC98_PATH_MAX];
	char collected[PC98_MAX_SONGS][256];
	int codes[PC98_MAX_SONGS];
	int ns, i;
	if (!st || !filename || !set_is_dummy(st)) return;
	pc98_dir_of(filename, dir, sizeof dir);
	if (!dir[0]) return;
	ns = collect_folder_sidecars(dir, collected, codes, PC98_MAX_SONGS);
	if (ns <= 0) return;
	st->nsong = 0;
	for (i = 0; i < ns; ++i)
		add_song(st, codes[i], collected[i]);
}

static int match_song(const hoot_set *st, const char *filename)
{
	char stem[256], base[256];
	int i;
	if (!st || !filename) return -1;
	pc98_basename(filename, stem, sizeof stem);
	{
		const char *slash = strrchr(filename, '\\');
		if (!slash) slash = strrchr(filename, '/');
		pc98_bounded(base, sizeof base, slash ? slash + 1 : filename);
	}
	for (i = 0; i < st->nsong; ++i) {
		if (pc98_ieq(st->songs[i].name, stem) || pc98_ieq(st->songs[i].name, base))
			return i;
	}
	return -1;
}

static int load_set_for_file(const char *filename, const uint8_t *data, size_t len,
		hoot_set *st)
{
	char setpath[PC98_PATH_MAX];
	char collected[PC98_MAX_SONGS][256];
	int codes[PC98_MAX_SONGS];
	int ns, i;
	if (!st) return -1;
	if (data && is_pc98set(data, len)) {
		if (parse_set(data, len, st) != 0) return -1;
		if (filename)
			pc98_dir_of(filename, st->folder, sizeof st->folder);
		expand_if_dummy(st, filename);
		return 0;
	}
	if (filename && sibling_set_path(filename, setpath, sizeof setpath) &&
			file_exists(setpath)) {
		std::vector<uint8_t> raw;
		if (load_file(setpath, &raw) == 0 && parse_set(raw.data(), raw.size(), st) == 0) {
			pc98_dir_of(filename, st->folder, sizeof st->folder);
			expand_if_dummy(st, filename);
			return 0;
		}
	}
	if (!filename) return -1;
	memset(st, 0, sizeof *st);
	pc98_dir_of(filename, st->folder, sizeof st->folder);
	folder_id_from_path(filename, st->id, sizeof st->id);
	pc98_bounded(st->title, sizeof st->title, st->id);
	{
		char dir[PC98_PATH_MAX];
		pc98_dir_of(filename, dir, sizeof dir);
		ns = collect_folder_sidecars(dir, collected, codes, PC98_MAX_SONGS);
	}
	if (ns > 0) {
		for (i = 0; i < ns; ++i)
			add_song(st, codes[i], collected[i]);
		return 0;
	}
	{
		char stem[256];
		pc98_basename(filename, stem, sizeof stem);
		add_song(st, 1, stem[0] ? stem : st->id);
	}
	return 0;
}

static void resolve_cache(hoot_set *st, const pc98_cfg *cfg)
{
	int i;
	for (i = 0; i < st->nsong; ++i) {
		char p[PC98_PATH_MAX];
		cache_path_for(cfg, st->id, i + 1, st->songs[i].code, p, sizeof p);
		pc98_bounded(st->songs[i].cache, sizeof st->songs[i].cache, p);
		if (file_exists(p)) {
			std::vector<uint8_t> raw;
			pc98_info inf;
			if (load_file(p, &raw) == 0 && s98_analyze(raw.data(), raw.size(), cfg, &inf) == 0)
				st->songs[i].one_loop_ms = inf.one_loop_ms[0];
		}
	}
}

static int spawn_hootrip(const pc98_cfg *cfg, const char *set_id)
{
	char cmd[2048];
	const char *exe, *xml, *out;
	if (!cfg || !set_id || !set_id[0]) return -1;
	exe = cfg->hootrip_path[0] ? cfg->hootrip_path : "hootrip.exe";
	xml = cfg->hoot_xml[0] ? cfg->hoot_xml : NULL;
	out = cfg->cache_dir[0] ? cfg->cache_dir : NULL;
	if (!xml || !out) return -1;
#ifdef _WIN32
	snprintf(cmd, sizeof cmd,
		"\"%s\" --archive \"%s\" pc98-rip \"%s\" --out \"%s\"",
		exe, xml, set_id, out);
	{
		STARTUPINFOA si;
		PROCESS_INFORMATION pi;
		DWORD code = 1;
		memset(&si, 0, sizeof si);
		si.cb = sizeof si;
		si.dwFlags = STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_HIDE;
		if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
				NULL, NULL, &si, &pi))
			return -1;
		WaitForSingleObject(pi.hProcess, 10 * 60 * 1000);
		GetExitCodeProcess(pi.hProcess, &code);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		return code == 0 ? 0 : -1;
	}
#else
	snprintf(cmd, sizeof cmd,
		"\"%s\" --archive \"%s\" pc98-rip \"%s\" --out \"%s\"",
		exe, xml, set_id, out);
	if (system(cmd) == 0) return 0;
	return -1;
#endif
}

struct hoot_state {
	hoot_set set;
	pc98_cfg cfg;
	int song;
	void *s98;
	void *bgm;
	int loops;
};

static int sidecar_path(const hoot_set *st, int song, char *out, size_t cap)
{
	if (!st || !out || cap < 8) return 0;
	if (song < 0 || song >= st->nsong) return 0;
	if (!st->folder[0] || !st->songs[song].name[0]) return 0;
	snprintf(out, cap, "%s%s", st->folder, st->songs[song].name);
	return 1;
}

static int open_track(hoot_state *h, int song, int allow_rip)
{
	std::vector<uint8_t> raw;
	char side[PC98_PATH_MAX];
	if (song < 0 || song >= h->set.nsong) return -1;
	if (h->s98) { s98_close(h->s98); h->s98 = NULL; }
	if (h->bgm) { bgmdrv_close_h(h->bgm); h->bgm = NULL; }
	if (sidecar_path(&h->set, song, side, sizeof side) && file_exists(side) &&
			load_file(side, &raw) == 0 && bgmdrv_probe_mem(raw.data(), raw.size())) {
		h->bgm = bgmdrv_open_mem(side, raw.data(), raw.size(), &h->cfg);
		if (h->bgm) {
			bgmdrv_set_loops_h(h->bgm, h->loops);
			h->set.songs[song].one_loop_ms = bgmdrv_one_loop_ms_h(h->bgm);
			h->song = song;
			return 0;
		}
	}
	if (!file_exists(h->set.songs[song].cache) && allow_rip)
		spawn_hootrip(&h->cfg, h->set.id);
	resolve_cache(&h->set, &h->cfg);
	if (!file_exists(h->set.songs[song].cache))
		return -1;
	if (load_file(h->set.songs[song].cache, &raw) != 0)
		return -1;
	h->s98 = s98_open(raw.data(), raw.size(), &h->cfg);
	if (!h->s98) return -1;
	s98_set_loops(h->s98, h->loops);
	h->song = song;
	return 0;
}

int hoot_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out)
{
	hoot_set st;
	int i;
	if (!out) return -1;
	if (load_set_for_file(filename, data, len, &st) != 0) return -1;
	resolve_cache(&st, cfg);
	memset(out, 0, sizeof *out);
	out->kind = PC98_KIND_SET;
	out->songs = st.nsong;
	pc98_bounded(out->title, sizeof out->title, st.title[0] ? st.title : st.id);
	pc98_bounded(out->game, sizeof out->game, st.title);
	pc98_bounded(out->set_id, sizeof out->set_id, st.id);
	pc98_bounded(out->filetype, sizeof out->filetype, "PC98 set");
	pc98_bounded(out->engine, sizeof out->engine, "hootrip / S98 cache");
	for (i = 0; i < st.nsong; ++i) {
		out->one_loop_ms[i] = st.songs[i].one_loop_ms;
		/* Cache miss: do not invent 3:00. Leave 0; GetFileInfo substitutes 1s. */
	}
	return 0;
}

void *hoot_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg)
{
	hoot_state *h;
	int i, start;
	if (!hoot_probe_file(filename, data, len) && !(data && is_pc98set(data, len)))
		return NULL;
	h = new hoot_state();
	if (cfg) h->cfg = *cfg;
	else pc98_cfg_defaults(&h->cfg);
	h->loops = h->cfg.loop_count > 0 ? h->cfg.loop_count : 1;
	if (load_set_for_file(filename, data, len, &h->set) != 0) {
		delete h;
		return NULL;
	}
	resolve_cache(&h->set, &h->cfg);
	start = match_song(&h->set, filename);
	if (start < 0) start = 0;
	if (open_track(h, start, 1) == 0)
		return h;
	for (i = 0; i < h->set.nsong; ++i) {
		if (i == start) continue;
		if (open_track(h, i, 1) == 0)
			return h;
	}
	delete h;
	return NULL;
}

void hoot_close_h(void *hh)
{
	hoot_state *h = (hoot_state *)hh;
	if (!h) return;
	if (h->s98) s98_close(h->s98);
	if (h->bgm) bgmdrv_close_h(h->bgm);
	delete h;
}

int hoot_process_h(void *hh, float *buf, int count)
{
	hoot_state *h = (hoot_state *)hh;
	if (!h) return 0;
	if (h->bgm) return bgmdrv_process_h(h->bgm, buf, count);
	if (!h->s98) return 0;
	return s98_process(h->s98, buf, count);
}

int hoot_seek_ms_h(void *hh, int ms)
{
	hoot_state *h = (hoot_state *)hh;
	if (!h) return -1;
	if (h->bgm) return bgmdrv_seek_ms_h(h->bgm, ms);
	if (!h->s98) return -1;
	return s98_seek_ms(h->s98, ms);
}

int hoot_set_song_h(void *hh, int song)
{
	hoot_state *h = (hoot_state *)hh;
	if (!h) return -1;
	return open_track(h, song, 1);
}

int hoot_song_h(void *hh)
{
	hoot_state *h = (hoot_state *)hh;
	return h ? h->song : 0;
}

int hoot_songs_h(void *hh)
{
	hoot_state *h = (hoot_state *)hh;
	return h ? h->set.nsong : 0;
}

int hoot_one_loop_ms_h(void *hh, int song)
{
	hoot_state *h = (hoot_state *)hh;
	if (!h || song < 0 || song >= h->set.nsong) return 0;
	if (h->bgm && song == h->song)
		return bgmdrv_one_loop_ms_h(h->bgm);
	if (h->s98 && song == h->song)
		return s98_one_loop_ms(h->s98);
	return h->set.songs[song].one_loop_ms;
}

int hoot_rate_h(void *hh)
{
	hoot_state *h = (hoot_state *)hh;
	if (h && h->bgm) return bgmdrv_rate_h(h->bgm);
	if (h && h->s98) return s98_rate(h->s98);
	return PC98_DEFAULT_RATE;
}

void hoot_set_loops_h(void *hh, int loops)
{
	hoot_state *h = (hoot_state *)hh;
	if (!h) return;
	if (loops < 1) loops = 1;
	if (loops > 3) loops = 3;
	h->loops = loops;
	if (h->bgm) bgmdrv_set_loops_h(h->bgm, loops);
	if (h->s98) s98_set_loops(h->s98, loops);
}

void hoot_apply_mute_h(void *hh, const pc98_cfg *cfg)
{
	hoot_state *h = (hoot_state *)hh;
	if (!h) return;
	if (h->bgm) bgmdrv_apply_mute_h(h->bgm, cfg);
	if (h->s98) s98_apply_mute(h->s98, cfg);
}

const char *hoot_title_h(void *hh)
{
	hoot_state *h = (hoot_state *)hh;
	if (!h) return "";
	if (h->song >= 0 && h->song < h->set.nsong && h->set.songs[h->song].name[0])
		return h->set.songs[h->song].name;
	return h->set.title;
}

const char *hoot_game_h(void *hh)
{
	hoot_state *h = (hoot_state *)hh;
	return h ? h->set.title : "";
}

/* ---- scan / XML ------------------------------------------------------- */

struct xml_song { int code; char name[256]; };
struct xml_game {
	char folder[64];
	char title[256];
	int nsong;
	xml_song songs[PC98_MAX_SONGS];
};

static void xml_add_game(std::vector<xml_game> *games, const char *folder,
		const char *title)
{
	xml_game g;
	size_t i;
	if (!folder || !folder[0]) return;
	for (i = 0; i < games->size(); ++i) {
		if (pc98_ieq((*games)[i].folder, folder)) {
			if (title && title[0] && !(*games)[i].title[0])
				pc98_bounded((*games)[i].title, sizeof (*games)[i].title, title);
			return;
		}
	}
	memset(&g, 0, sizeof g);
	pc98_bounded(g.folder, sizeof g.folder, folder);
	if (title) pc98_bounded(g.title, sizeof g.title, title);
	games->push_back(g);
}

static xml_game *xml_find(std::vector<xml_game> *games, const char *folder)
{
	size_t i;
	for (i = 0; i < games->size(); ++i)
		if (pc98_ieq((*games)[i].folder, folder))
			return &(*games)[i];
	return NULL;
}

static void take_tag(const char *s, const char *tag, char *out, size_t cap)
{
	char open[64], close[64];
	const char *a, *b;
	snprintf(open, sizeof open, "<%s>", tag);
	snprintf(close, sizeof close, "</%s>", tag);
	a = strstr(s, open);
	if (!a) return;
	a += strlen(open);
	b = strstr(a, close);
	if (!b || b <= a) return;
	{
		size_t n = (size_t)(b - a);
		if (n >= cap) n = cap - 1;
		memcpy(out, a, n);
		out[n] = 0;
	}
}

static void ingest_xml_text(const char *text, std::vector<xml_game> *games)
{
	const char *p = text;
	while (p && *p) {
		const char *soft = strstr(p, "<soft");
		const char *game = strstr(p, "<game");
		const char *block = NULL;
		const char *end = NULL;
		char folder[64], title[256];
		if (!soft && !game) break;
		if (!soft) block = game;
		else if (!game) block = soft;
		else block = (soft < game) ? soft : game;
		end = strstr(block + 1, (block == soft) ? "</soft>" : "</game>");
		if (!end) end = block + strlen(block);
		{
			size_t n = (size_t)(end - block) + 8;
			std::string chunk(block, n < 8000 ? n : 8000);
			folder[0] = title[0] = 0;
			take_tag(chunk.c_str(), "folder", folder, sizeof folder);
			if (!folder[0]) take_tag(chunk.c_str(), "code", folder, sizeof folder);
			if (!folder[0]) {
				const char *nm = strstr(chunk.c_str(), "name=\"");
				if (nm) {
					nm += 6;
					pc98_bounded(folder, sizeof folder, nm);
					{
						char *q = strchr(folder, '"');
						if (q) *q = 0;
					}
				}
			}
			take_tag(chunk.c_str(), "title", title, sizeof title);
			if (folder[0]) {
				xml_game *g;
				xml_add_game(games, folder, title);
				g = xml_find(games, folder);
				if (g) {
					const char *sp = chunk.c_str();
					while (g->nsong < PC98_MAX_SONGS) {
						const char *song = strstr(sp, "<song");
						const char *se;
						char sname[256], scode[32];
						if (!song) break;
						se = strstr(song, "</song>");
						if (!se) se = song + 32;
						{
							std::string sc(song, (size_t)(se - song) + 8);
							sname[0] = scode[0] = 0;
							take_tag(sc.c_str(), "title", sname, sizeof sname);
							if (!sname[0]) take_tag(sc.c_str(), "name", sname, sizeof sname);
							take_tag(sc.c_str(), "code", scode, sizeof scode);
							if (sname[0] && !pc98_is_stop_title(sname)) {
								xml_song *xs = &g->songs[g->nsong++];
								xs->code = scode[0] ? atoi(scode) : g->nsong;
								pc98_bounded(xs->name, sizeof xs->name, sname);
							}
						}
						sp = se + 1;
					}
				}
			}
		}
		p = end + 1;
	}
}

static void load_xml_dir(const char *xml_dir, std::vector<xml_game> *games)
{
#ifdef _WIN32
	char spec[PC98_PATH_MAX];
	WIN32_FIND_DATAA fd;
	HANDLE h;
	if (!xml_dir || !xml_dir[0]) return;
	snprintf(spec, sizeof spec, "%s\\*.xml", xml_dir);
	h = FindFirstFileA(spec, &fd);
	if (h == INVALID_HANDLE_VALUE) {
		snprintf(spec, sizeof spec, "%s*.xml", xml_dir);
		h = FindFirstFileA(spec, &fd);
	}
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		char path[PC98_PATH_MAX];
		std::vector<uint8_t> raw;
		snprintf(path, sizeof path, "%s\\%s", xml_dir, fd.cFileName);
		if (load_file(path, &raw) == 0) {
			raw.push_back(0);
			ingest_xml_text((const char *)raw.data(), games);
		}
	} while (FindNextFileA(h, &fd));
	FindClose(h);
#else
	DIR *d;
	struct dirent *e;
	if (!xml_dir || !xml_dir[0]) return;
	d = opendir(xml_dir);
	if (!d) return;
	while ((e = readdir(d)) != NULL) {
		char path[PC98_PATH_MAX];
		std::vector<uint8_t> raw;
		if (!strstr(e->d_name, ".xml") && !strstr(e->d_name, ".XML"))
			continue;
		snprintf(path, sizeof path, "%s/%s", xml_dir, e->d_name);
		if (load_file(path, &raw) == 0) {
			raw.push_back(0);
			ingest_xml_text((const char *)raw.data(), games);
		}
	}
	closedir(d);
#endif
}

#ifdef _WIN32
static int walk_letter(const char *letter_path, const std::vector<xml_game> *games, int write_sets)
{
	char spec[PC98_PATH_MAX];
	WIN32_FIND_DATAA fd;
	HANDLE h;
	int n = 0;
	snprintf(spec, sizeof spec, "%s\\*", letter_path);
	h = FindFirstFileA(spec, &fd);
	if (h == INVALID_HANDLE_VALUE) return 0;
	do {
		char folder[PC98_PATH_MAX], setpath[PC98_PATH_MAX];
		const xml_game *g = NULL;
		size_t i;
		const char *names[PC98_MAX_SONGS];
		int codes[PC98_MAX_SONGS];
		int ns = 0;
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
		if (fd.cFileName[0] == '.') continue;
		snprintf(folder, sizeof folder, "%s\\%s", letter_path, fd.cFileName);
		for (i = 0; i < games->size(); ++i) {
			if (pc98_ieq((*games)[i].folder, fd.cFileName)) {
				g = &(*games)[i];
				break;
			}
		}
		char collected[PC98_MAX_SONGS][256];
		int cc[PC98_MAX_SONGS];
		int cn = 0;
		if (g && g->nsong > 0) {
			for (ns = 0; ns < g->nsong; ++ns) {
				names[ns] = g->songs[ns].name;
				codes[ns] = g->songs[ns].code;
			}
		} else {
			cn = collect_folder_sidecars(folder, collected, cc, PC98_MAX_SONGS);
			if (cn > 0) {
				for (ns = 0; ns < cn; ++ns) {
					names[ns] = collected[ns];
					codes[ns] = cc[ns];
				}
			} else {
				names[0] = fd.cFileName;
				codes[0] = 1;
				ns = 1;
			}
		}
		snprintf(setpath, sizeof setpath, "%s\\set.pc98", folder);
		if (write_sets)
			pc98_set_write(setpath, fd.cFileName, g ? g->title : fd.cFileName,
					ns, names, codes);
		n++;
	} while (FindNextFileA(h, &fd));
	FindClose(h);
	return n;
}
#else
static int walk_letter(const char *letter_path, const std::vector<xml_game> *games, int write_sets)
{
	DIR *d = opendir(letter_path);
	struct dirent *e;
	int n = 0;
	if (!d) return 0;
	while ((e = readdir(d)) != NULL) {
		char folder[PC98_PATH_MAX], setpath[PC98_PATH_MAX];
		struct stat st;
		const xml_game *g = NULL;
		size_t i;
		const char *names[PC98_MAX_SONGS];
		int codes[PC98_MAX_SONGS];
		int ns = 0;
		if (e->d_name[0] == '.') continue;
		snprintf(folder, sizeof folder, "%s/%s", letter_path, e->d_name);
		if (stat(folder, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
		for (i = 0; i < games->size(); ++i) {
			if (pc98_ieq((*games)[i].folder, e->d_name)) {
				g = &(*games)[i];
				break;
			}
		}
		char collected[PC98_MAX_SONGS][256];
		int cc[PC98_MAX_SONGS];
		int cn = 0;
		if (g && g->nsong > 0) {
			for (ns = 0; ns < g->nsong; ++ns) {
				names[ns] = g->songs[ns].name;
				codes[ns] = g->songs[ns].code;
			}
		} else {
			cn = collect_folder_sidecars(folder, collected, cc, PC98_MAX_SONGS);
			if (cn > 0) {
				for (ns = 0; ns < cn; ++ns) {
					names[ns] = collected[ns];
					codes[ns] = cc[ns];
				}
			} else {
				names[0] = e->d_name;
				codes[0] = 1;
				ns = 1;
			}
		}
		snprintf(setpath, sizeof setpath, "%s/set.pc98", folder);
		if (write_sets)
			pc98_set_write(setpath, e->d_name, g ? g->title : e->d_name,
					ns, names, codes);
		n++;
	}
	closedir(d);
	return n;
}
#endif

int hoot_scan_tree(const char *music_root, const char *xml_dir, int write_sets)
{
	std::vector<xml_game> games;
	int total = 0;
#ifdef _WIN32
	char spec[PC98_PATH_MAX];
	WIN32_FIND_DATAA fd;
	HANDLE h;
	if (!music_root || !music_root[0]) return -1;
	load_xml_dir(xml_dir, &games);
	snprintf(spec, sizeof spec, "%s\\*", music_root);
	h = FindFirstFileA(spec, &fd);
	if (h == INVALID_HANDLE_VALUE) return 0;
	do {
		char letter[PC98_PATH_MAX];
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
		if (fd.cFileName[0] == '.') continue;
		if (strlen(fd.cFileName) > 2) continue;
		snprintf(letter, sizeof letter, "%s\\%s", music_root, fd.cFileName);
		total += walk_letter(letter, &games, write_sets);
	} while (FindNextFileA(h, &fd));
	FindClose(h);
#else
	DIR *d;
	struct dirent *e;
	if (!music_root || !music_root[0]) return -1;
	load_xml_dir(xml_dir, &games);
	d = opendir(music_root);
	if (!d) return 0;
	while ((e = readdir(d)) != NULL) {
		char letter[PC98_PATH_MAX];
		struct stat st;
		if (e->d_name[0] == '.') continue;
		if (strlen(e->d_name) > 2) continue;
		snprintf(letter, sizeof letter, "%s/%s", music_root, e->d_name);
		if (stat(letter, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
		total += walk_letter(letter, &games, write_sets);
	}
	closedir(d);
#endif
	return total;
}

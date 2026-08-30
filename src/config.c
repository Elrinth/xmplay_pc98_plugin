#include "config.h"
#include "pc98_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

void pc98_cfg_defaults(pc98_cfg *c)
{
	if (!c) return;
	memset(c, 0, sizeof *c);
	c->loop_count = PC98_DEFAULT_LOOPS;
	c->rate = PC98_DEFAULT_RATE;
	c->fade_ms = 0;
}

void pc98_cfg_clamp(pc98_cfg *c)
{
	if (!c) return;
	if (c->loop_count < 1) c->loop_count = 1;
	if (c->loop_count > 3) c->loop_count = 3;
	if (c->rate != 48000) c->rate = 44100;
	if (c->fade_ms < 0) c->fade_ms = 0;
	if (c->fade_ms > 20000) c->fade_ms = 20000;
	c->mute_fm = c->mute_fm ? 1 : 0;
	c->mute_ssg = c->mute_ssg ? 1 : 0;
	c->mute_rhythm = c->mute_rhythm ? 1 : 0;
	c->mute_adpcm = c->mute_adpcm ? 1 : 0;
}

static void trim(char *s)
{
	char *e;
	while (*s == ' ' || *s == '\t') memmove(s, s + 1, strlen(s));
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
		*--e = '\0';
}

void pc98_cfg_load_ini(pc98_cfg *c, const char *dll_dir)
{
	char path[PC98_PATH_MAX];
	FILE *fp;
	char line[1024];
	if (!c) return;
	pc98_cfg_defaults(c);
	if (dll_dir)
		pc98_bounded(c->dll_dir, sizeof c->dll_dir, dll_dir);
	if (!dll_dir || !dll_dir[0]) return;
	snprintf(path, sizeof path, "%sxmp-pc98.ini", dll_dir);
	fp = fopen(path, "r");
	if (!fp) return;
	while (fgets(line, sizeof line, fp)) {
		char *eq;
		trim(line);
		if (line[0] == '#' || line[0] == ';' || line[0] == '[' || !line[0])
			continue;
		eq = strchr(line, '=');
		if (!eq) continue;
		*eq++ = '\0';
		trim(line);
		trim(eq);
		if (pc98_ieq(line, "loop_count")) c->loop_count = atoi(eq);
		else if (pc98_ieq(line, "mute_fm")) c->mute_fm = atoi(eq);
		else if (pc98_ieq(line, "mute_ssg")) c->mute_ssg = atoi(eq);
		else if (pc98_ieq(line, "mute_rhythm")) c->mute_rhythm = atoi(eq);
		else if (pc98_ieq(line, "mute_adpcm")) c->mute_adpcm = atoi(eq);
		else if (pc98_ieq(line, "fade_ms")) c->fade_ms = atoi(eq);
		else if (pc98_ieq(line, "rate")) c->rate = atoi(eq);
		else if (pc98_ieq(line, "rhythm_path")) pc98_bounded(c->rhythm_path, sizeof c->rhythm_path, eq);
		else if (pc98_ieq(line, "hoot_xml")) pc98_bounded(c->hoot_xml, sizeof c->hoot_xml, eq);
		else if (pc98_ieq(line, "music_root")) pc98_bounded(c->music_root, sizeof c->music_root, eq);
		else if (pc98_ieq(line, "cache_dir")) pc98_bounded(c->cache_dir, sizeof c->cache_dir, eq);
		else if (pc98_ieq(line, "hootrip_path")) pc98_bounded(c->hootrip_path, sizeof c->hootrip_path, eq);
		else if (pc98_ieq(line, "gs_sf2")) pc98_bounded(c->gs_sf2, sizeof c->gs_sf2, eq);
		else if (pc98_ieq(line, "mt_sf2")) pc98_bounded(c->mt_sf2, sizeof c->mt_sf2, eq);
	}
	fclose(fp);
	pc98_cfg_clamp(c);
}

void pc98_cfg_save_ini(const pc98_cfg *c, const char *dll_dir)
{
	char path[PC98_PATH_MAX];
	FILE *fp;
	if (!c || !dll_dir || !dll_dir[0]) return;
	snprintf(path, sizeof path, "%sxmp-pc98.ini", dll_dir);
	fp = fopen(path, "w");
	if (!fp) return;
	fprintf(fp, "[pc98]\n");
	fprintf(fp, "loop_count=%d\n", c->loop_count);
	fprintf(fp, "mute_fm=%d\n", c->mute_fm);
	fprintf(fp, "mute_ssg=%d\n", c->mute_ssg);
	fprintf(fp, "mute_rhythm=%d\n", c->mute_rhythm);
	fprintf(fp, "mute_adpcm=%d\n", c->mute_adpcm);
	fprintf(fp, "fade_ms=%d\n", c->fade_ms);
	fprintf(fp, "rate=%d\n", c->rate);
	fprintf(fp, "rhythm_path=%s\n", c->rhythm_path);
	fprintf(fp, "hoot_xml=%s\n", c->hoot_xml);
	fprintf(fp, "music_root=%s\n", c->music_root);
	fprintf(fp, "cache_dir=%s\n", c->cache_dir);
	fprintf(fp, "hootrip_path=%s\n", c->hootrip_path);
	fprintf(fp, "gs_sf2=%s\n", c->gs_sf2);
	fprintf(fp, "mt_sf2=%s\n", c->mt_sf2);
	fclose(fp);
}

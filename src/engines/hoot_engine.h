#ifndef XMP_HOOT_ENGINE_H
#define XMP_HOOT_ENGINE_H

#include "pc98.h"

#ifdef __cplusplus
extern "C" {
#endif

int hoot_probe_mem(const uint8_t *data, size_t len);
int hoot_probe_file(const char *filename, const uint8_t *data, size_t len);
int hoot_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out);
void *hoot_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg);
void hoot_close_h(void *h);
int hoot_process_h(void *h, float *buf, int count);
int hoot_seek_ms_h(void *h, int ms);
int hoot_set_song_h(void *h, int song);
int hoot_song_h(void *h);
int hoot_songs_h(void *h);
int hoot_one_loop_ms_h(void *h, int song);
int hoot_rate_h(void *h);
void hoot_set_loops_h(void *h, int loops);
void hoot_apply_mute_h(void *h, const pc98_cfg *cfg);
const char *hoot_title_h(void *h);
const char *hoot_game_h(void *h);

int hoot_scan_tree(const char *music_root, const char *xml_dir, int write_sets);

#ifdef __cplusplus
}
#endif

#endif

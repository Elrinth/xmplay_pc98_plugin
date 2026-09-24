#ifndef XMP_PAC_ENGINE_H
#define XMP_PAC_ENGINE_H

#include "pc98.h"

#ifdef __cplusplus
extern "C" {
#endif

int pac_probe_mem(const uint8_t *data, size_t len);
int pac_probe_named(const char *filename, const uint8_t *data, size_t len);
int pac_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out);
void *pac_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg);
void pac_close_h(void *h);
int pac_process_h(void *h, float *buf, int count);
int pac_seek_ms_h(void *h, int ms);
int pac_set_song_h(void *h, int song);
int pac_song_h(void *h);
int pac_songs_h(void *h);
int pac_one_loop_ms_h(void *h, int song);
int pac_rate_h(void *h);
void pac_set_loops_h(void *h, int loops);
void pac_apply_mute_h(void *h, const pc98_cfg *cfg);
const char *pac_title_h(void *h);
const char *pac_game_h(void *h);
const char *pac_engine_h(void *h);
const char *pac_chip_h(void *h);
const char *pac_type_h(void *h);

#ifdef __cplusplus
}
#endif

#endif

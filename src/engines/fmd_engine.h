#ifndef XMP_FMD_ENGINE_H
#define XMP_FMD_ENGINE_H

#include "pc98.h"

#ifdef __cplusplus
extern "C" {
#endif

int fmd_probe_mem(const uint8_t *data, size_t len);
int fmd_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out);
void *fmd_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg);
void fmd_close_h(void *h);
int fmd_process_h(void *h, float *buf, int count);
int fmd_seek_ms_h(void *h, int ms);
int fmd_one_loop_ms_h(void *h);
int fmd_rate_h(void *h);
void fmd_set_loops_h(void *h, int loops);
void fmd_apply_mute_h(void *h, const pc98_cfg *cfg);
const char *fmd_title_h(void *h);
const char *fmd_game_h(void *h);
const char *fmd_engine_h(void *h);
const char *fmd_type_h(void *h);
int fmd_find_sf2(const pc98_cfg *cfg, const char *filename, int mt,
		char *out, size_t cap);
void *fmd_font_get(const char *path);

#ifdef __cplusplus
}
#endif

#endif

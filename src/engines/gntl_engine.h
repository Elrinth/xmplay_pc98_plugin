#ifndef XMP_GNTL_ENGINE_H
#define XMP_GNTL_ENGINE_H

#include "pc98.h"

#ifdef __cplusplus
extern "C" {
#endif

int gntl_probe_mem(const uint8_t *data, size_t len);
int gntl_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out);
void *gntl_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg);
void gntl_close_h(void *h);
int gntl_process_h(void *h, float *buf, int count);
int gntl_seek_ms_h(void *h, int ms);
int gntl_one_loop_ms_h(void *h);
int gntl_rate_h(void *h);
void gntl_set_loops_h(void *h, int loops);
void gntl_apply_mute_h(void *h, const pc98_cfg *cfg);
const char *gntl_title_h(void *h);
const char *gntl_game_h(void *h);
const char *gntl_engine_h(void *h);

#ifdef __cplusplus
}
#endif

#endif

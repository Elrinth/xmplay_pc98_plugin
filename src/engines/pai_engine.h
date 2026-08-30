#ifndef XMP_PAI_ENGINE_H
#define XMP_PAI_ENGINE_H

#include "pc98.h"

#ifdef __cplusplus
extern "C" {
#endif

int pai_probe_mem(const uint8_t *data, size_t len);
int pai_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out);
void *pai_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg);
void pai_close_h(void *h);
int pai_process_h(void *h, float *buf, int count);
int pai_seek_ms_h(void *h, int ms);
int pai_one_loop_ms_h(void *h);
int pai_rate_h(void *h);
void pai_set_loops_h(void *h, int loops);
void pai_apply_mute_h(void *h, const pc98_cfg *cfg);
const char *pai_title_h(void *h);
const char *pai_game_h(void *h);

#ifdef __cplusplus
}
#endif

#endif

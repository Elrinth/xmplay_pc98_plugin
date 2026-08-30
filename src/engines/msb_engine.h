#ifndef XMP_MSB_ENGINE_H
#define XMP_MSB_ENGINE_H

#include "pc98.h"

#ifdef __cplusplus
extern "C" {
#endif

int msb_probe_mem(const uint8_t *data, size_t len);
int msb_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out);
void *msb_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg);
void msb_close_h(void *h);
int msb_process_h(void *h, float *buf, int count);
int msb_seek_ms_h(void *h, int ms);
int msb_one_loop_ms_h(void *h);
int msb_rate_h(void *h);
void msb_set_loops_h(void *h, int loops);
void msb_apply_mute_h(void *h, const pc98_cfg *cfg);
const char *msb_title_h(void *h);
const char *msb_game_h(void *h);

#ifdef __cplusplus
}
#endif

#endif

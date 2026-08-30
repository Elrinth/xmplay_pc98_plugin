#ifndef XMP_BGMDRV_ENGINE_H
#define XMP_BGMDRV_ENGINE_H

#include "pc98.h"

#ifdef __cplusplus
extern "C" {
#endif

int bgmdrv_probe_mem(const uint8_t *data, size_t len);
int bgmdrv_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out);
void *bgmdrv_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg);
void bgmdrv_close_h(void *h);
int bgmdrv_process_h(void *h, float *buf, int count);
int bgmdrv_seek_ms_h(void *h, int ms);
int bgmdrv_one_loop_ms_h(void *h);
int bgmdrv_rate_h(void *h);
void bgmdrv_set_loops_h(void *h, int loops);
void bgmdrv_apply_mute_h(void *h, const pc98_cfg *cfg);
const char *bgmdrv_title_h(void *h);

#ifdef __cplusplus
}
#endif

#endif

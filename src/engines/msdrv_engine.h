#ifndef XMP_MSDRV_ENGINE_H
#define XMP_MSDRV_ENGINE_H

#include "pc98.h"

#ifdef __cplusplus
extern "C" {
#endif

int msdrv_probe_mem(const uint8_t *data, size_t len);
int msdrv_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out);
void *msdrv_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg);
void msdrv_close_h(void *h);
int msdrv_process_h(void *h, float *buf, int count);
int msdrv_seek_ms_h(void *h, int ms);
int msdrv_one_loop_ms_h(void *h);
int msdrv_rate_h(void *h);
void msdrv_set_loops_h(void *h, int loops);
void msdrv_apply_mute_h(void *h, const pc98_cfg *cfg);
const char *msdrv_title_h(void *h);
const char *msdrv_chip_name_h(void *h);
const char *msdrv_rhythm_source_h(void *h);
int msdrv_variant_h(void *h);
const char *msdrv_sf2_name_h(void *h);

#ifdef __cplusplus
}
#endif

#endif

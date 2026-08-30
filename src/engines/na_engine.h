#ifndef XMP_NA_ENGINE_H
#define XMP_NA_ENGINE_H

#include "pc98.h"

#ifdef __cplusplus
extern "C" {
#endif

int na_probe_mem(const uint8_t *data, size_t len);
int na_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out);
void *na_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg);
void na_close_h(void *h);
int na_process_h(void *h, float *buf, int count);
int na_seek_ms_h(void *h, int ms);
int na_one_loop_ms_h(void *h);
int na_rate_h(void *h);
void na_set_loops_h(void *h, int loops);
void na_apply_mute_h(void *h, const pc98_cfg *cfg);
const char *na_title_h(void *h);

#ifdef __cplusplus
}
#endif

#endif

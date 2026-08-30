#ifndef XMP_FMP_ENGINE_H
#define XMP_FMP_ENGINE_H

#include "pc98.h"

#ifdef __cplusplus
extern "C" {
#endif

int fmp_probe_mem(const uint8_t *data, size_t len);
int fmp_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out);
void *fmp_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg);
void fmp_close_h(void *h);
int fmp_process_h(void *h, float *buf, int count);
int fmp_seek_ms_h(void *h, int ms);
int fmp_one_loop_ms_h(void *h);
int fmp_rate_h(void *h);
void fmp_set_loops_h(void *h, int loops);
void fmp_apply_mute_h(void *h, const pc98_cfg *cfg);
const char *fmp_title_h(void *h);

#ifdef __cplusplus
}
#endif

#endif

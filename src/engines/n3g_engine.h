#ifndef XMP_N3G_ENGINE_H
#define XMP_N3G_ENGINE_H

#include "pc98.h"

#ifdef __cplusplus
extern "C" {
#endif

int n3g_probe_mem(const uint8_t *data, size_t len);
int n3g_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out);
void *n3g_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg);
void n3g_close_h(void *h);
int n3g_process_h(void *h, float *buf, int count);
int n3g_seek_ms_h(void *h, int ms);
int n3g_one_loop_ms_h(void *h);
int n3g_rate_h(void *h);
void n3g_set_loops_h(void *h, int loops);
void n3g_apply_mute_h(void *h, const pc98_cfg *cfg);
const char *n3g_title_h(void *h);

#ifdef __cplusplus
}
#endif

#endif

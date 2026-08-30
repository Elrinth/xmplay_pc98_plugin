#ifndef XMP_NTL_ENGINE_H
#define XMP_NTL_ENGINE_H

#include "pc98.h"

#ifdef __cplusplus
extern "C" {
#endif

int ntl_probe_mem(const uint8_t *data, size_t len);
int ntl_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out);
void *ntl_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg);
void ntl_close_h(void *h);
int ntl_process_h(void *h, float *buf, int count);
int ntl_seek_ms_h(void *h, int ms);
int ntl_one_loop_ms_h(void *h);
int ntl_rate_h(void *h);
void ntl_set_loops_h(void *h, int loops);
void ntl_apply_mute_h(void *h, const pc98_cfg *cfg);
const char *ntl_title_h(void *h);
const char *ntl_game_h(void *h);

#ifdef __cplusplus
}
#endif

#endif

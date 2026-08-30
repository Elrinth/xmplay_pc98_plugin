#ifndef XMP_MD_ENGINE_H
#define XMP_MD_ENGINE_H

#include "pc98.h"

#ifdef __cplusplus
extern "C" {
#endif

int md_probe_mem(const uint8_t *data, size_t len);
int md_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out);
void *md_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg);
void md_close_h(void *h);
int md_process_h(void *h, float *buf, int count);
int md_seek_ms_h(void *h, int ms);
int md_one_loop_ms_h(void *h);
int md_rate_h(void *h);
void md_set_loops_h(void *h, int loops);
void md_apply_mute_h(void *h, const pc98_cfg *cfg);
const char *md_title_h(void *h);
const char *md_game_h(void *h);

#ifdef __cplusplus
}
#endif

#endif

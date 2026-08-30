#ifndef XMP_PMD_ENGINE_H
#define XMP_PMD_ENGINE_H

#include "pc98.h"

#ifdef __cplusplus
extern "C" {
#endif

int pmd_probe_mem(const uint8_t *data, size_t len);
int pmd_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out);
void *pmd_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg);
void pmd_close_h(void *h);
int pmd_process_h(void *h, float *buf, int count);
int pmd_seek_ms_h(void *h, int ms);
int pmd_one_loop_ms_h(void *h);
int pmd_rate_h(void *h);
void pmd_set_loops_h(void *h, int loops);
void pmd_apply_mute_h(void *h, const pc98_cfg *cfg);
const char *pmd_title_h(void *h);
const char *pmd_artist_h(void *h);

#ifdef __cplusplus
}
#endif

#endif

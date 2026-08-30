#ifndef XMP_MFD_ENGINE_H
#define XMP_MFD_ENGINE_H

#include "pc98.h"

#ifdef __cplusplus
extern "C" {
#endif

int mfd_probe_mem(const uint8_t *data, size_t len);
int mfd_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out);
void *mfd_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg);
void mfd_close_h(void *h);
int mfd_process_h(void *h, float *buf, int count);
int mfd_seek_ms_h(void *h, int ms);
int mfd_one_loop_ms_h(void *h);
int mfd_rate_h(void *h);
void mfd_set_loops_h(void *h, int loops);
void mfd_apply_mute_h(void *h, const pc98_cfg *cfg);
const char *mfd_title_h(void *h);
const char *mfd_engine_name_h(void *h);

#ifdef __cplusplus
}
#endif

#endif

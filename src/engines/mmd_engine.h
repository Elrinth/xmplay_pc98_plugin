#ifndef XMP_MMD_ENGINE_H
#define XMP_MMD_ENGINE_H

#include "pc98.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Kajihara MMD.COM / MC.EXE MIDI songs (.MMD) — NOT FUGA XOR-A5 packed RCP,
 * and NOT WinFMP. Header: BPM, transp, 18×(ptr,transp,ch). Targets GS (SC-55). */

int mmd_probe_mem(const uint8_t *data, size_t len);
int mmd_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out);
void *mmd_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg);
void mmd_close_h(void *h);
int mmd_process_h(void *h, float *buf, int count);
int mmd_seek_ms_h(void *h, int ms);
int mmd_one_loop_ms_h(void *h);
int mmd_rate_h(void *h);
void mmd_set_loops_h(void *h, int loops);
void mmd_apply_mute_h(void *h, const pc98_cfg *cfg);
const char *mmd_title_h(void *h);
const char *mmd_game_h(void *h);
const char *mmd_engine_h(void *h);
const char *mmd_type_h(void *h);
const char *mmd_chip_h(void *h);
const char *mmd_sf2_h(void *h);

#ifdef __cplusplus
}
#endif

#endif

#ifndef XMP_MFDMIDI_ENGINE_H
#define XMP_MFDMIDI_ENGINE_H

#include "pc98.h"

#ifdef __cplusplus
extern "C" {
#endif

/* U.Kakenasi MFD.COM 1.8 MIDI songs (.MFD, "MF" 01 00). Not the .USO FM files. */

int mfdmidi_probe_mem(const uint8_t *data, size_t len);
int mfdmidi_analyze_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out);
void *mfdmidi_open_mem(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg);
void mfdmidi_close_h(void *h);
int mfdmidi_process_h(void *h, float *buf, int count);
int mfdmidi_seek_ms_h(void *h, int ms);
int mfdmidi_one_loop_ms_h(void *h);
int mfdmidi_rate_h(void *h);
void mfdmidi_set_loops_h(void *h, int loops);
void mfdmidi_apply_mute_h(void *h, const pc98_cfg *cfg);
const char *mfdmidi_title_h(void *h);
const char *mfdmidi_engine_h(void *h);
const char *mfdmidi_sf2_h(void *h);

#ifdef __cplusplus
}
#endif

#endif

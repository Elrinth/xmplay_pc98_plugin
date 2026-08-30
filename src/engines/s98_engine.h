#ifndef XMP_S98_ENGINE_H
#define XMP_S98_ENGINE_H

#include "pc98.h"

#ifdef __cplusplus
extern "C" {
#endif

int s98_probe(const uint8_t *data, size_t len);
int s98_analyze(const uint8_t *data, size_t len, const pc98_cfg *cfg, pc98_info *out);
void *s98_open(const uint8_t *data, size_t len, const pc98_cfg *cfg);
void s98_close(void *h);
int s98_process(void *h, float *buf, int count);
int s98_seek_ms(void *h, int ms);
int s98_one_loop_ms(void *h);
int s98_rate(void *h);
void s98_set_loops(void *h, int loops);
void s98_apply_mute(void *h, const pc98_cfg *cfg);
const char *s98_title(void *h);
const char *s98_artist(void *h);
const char *s98_game(void *h);

#ifdef __cplusplus
}
#endif

#endif

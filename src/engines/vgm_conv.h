#ifndef XMP_VGM_CONV_H
#define XMP_VGM_CONV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 if data (raw "Vgm " or a gzip peek of one) uses only YM2203/YM2608.
 * Works on a 256-byte CheckFile peek. */
int vgm_probe_mem(const uint8_t *data, size_t len);

/* Whole VGM/VGZ -> in-memory S98 v3 (malloc'd, caller frees). NULL on error. */
uint8_t *vgm_to_s98(const uint8_t *data, size_t len, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif

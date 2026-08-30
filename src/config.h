#ifndef XMP_PC98_CONFIG_H
#define XMP_PC98_CONFIG_H

#include "pc98.h"

#ifdef __cplusplus
extern "C" {
#endif

void pc98_cfg_load_ini(pc98_cfg *c, const char *dll_dir);
void pc98_cfg_save_ini(const pc98_cfg *c, const char *dll_dir);

#ifdef __cplusplus
}
#endif

#endif

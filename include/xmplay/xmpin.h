// XMPlay input plugin header
// new plugins can be submitted to plugins@xmplay.com

#pragma once

#include "xmpfunc.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef XMPIN_FACE
#define XMPIN_FACE		4
#endif

#define XMPIN_FLAG_CANSTREAM	1
#define XMPIN_FLAG_OWNFILE		2
#define XMPIN_FLAG_NOXMPFILE	4
#define XMPIN_FLAG_LOOP			8
#define XMPIN_FLAG_TAIL			16
#define XMPIN_FLAG_CONFIG		64
#define XMPIN_FLAG_LOOPSOUND	128
#define XMPIN_FLAG_NETSEEK		256
#define XMPIN_FLAG_NOCHECK		512
#define XMPIN_FLAG_MULTIEXT		1024
#define XMPIN_FLAG_OPTIONS		2048

#define XMPIN_POS_LOOP		-1
#define XMPIN_POS_AUTOLOOP	-2
#define XMPIN_POS_TAIL		-3
#define XMPIN_POS_SUBSONG	0x80000000
#define XMPIN_POS_SUBSONG1	0x40000000

#define XMPIN_VIS_INIT		1
#define XMPIN_VIS_FULL		2
#define XMPIN_VIS_MAIN		4

#define XMPIN_INFO_NOSUBTAGS	0x10000

typedef struct {
	DWORD flags;
	const char *name;
	const char *exts;

	void (WINAPI *About)(HWND win);
	void (WINAPI *Config)(HWND win);
	BOOL (WINAPI *CheckFile)(const char *filename, XMPFILE file);
#if XMPIN_FACE==4
	DWORD (WINAPI *GetFileInfo)(const char *filename, XMPFILE file, float **length, char **tags);
#else
	BOOL (WINAPI *GetFileInfo)(const char *filename, XMPFILE file, float *length, char *tags[8]);
#endif

	DWORD (WINAPI *Open)(const char *filename, XMPFILE file);
	void (WINAPI *Close)();
	void *reserved1;
	void (WINAPI *SetFormat)(XMPFORMAT *form);

#if XMPIN_FACE==4
	char *(WINAPI *GetTags)();
#else
	BOOL (WINAPI *GetTags)(char *tags[8]);
#endif
	void (WINAPI *GetInfoText)(char *format, char *length);
	void (WINAPI *GetGeneralInfo)(char *buf);
	void (WINAPI *GetMessage)(char *buf);
	double (WINAPI *SetPosition)(DWORD pos);
	double (WINAPI *GetGranularity)();
	DWORD (WINAPI *GetBuffering)();
	DWORD (WINAPI *Process)(float *buf, DWORD count);
	BOOL (WINAPI *WriteFile)(const char *filename);

	void (WINAPI *GetSamples)(char *buf);
	DWORD (WINAPI *GetSubSongs)(float *length);
#if XMPIN_FACE==4
	void *reserved3;
#else
	char *(WINAPI *GetCues)();
#endif

	float (WINAPI *GetDownloaded)();

	const char *visname;
	BOOL (WINAPI *VisOpen)(DWORD colors[3]);
	void (WINAPI *VisClose)();
	void (WINAPI *VisSize)(HDC dc, SIZE *size);
	BOOL (WINAPI *VisRender)(DWORD *buf, SIZE size, DWORD flags);
	BOOL (WINAPI *VisRenderDC)(HDC dc, SIZE size, DWORD flags);
	void (WINAPI *VisButton)(DWORD x, DWORD y);
	void (WINAPI *VisConfig)(HWND win);

	DWORD (WINAPI *GetConfig)(void *config);
	void (WINAPI *SetConfig)(void *config, DWORD size);
	DLGPROC Options;
} XMPIN;

XMPIN *WINAPI XMPIN_GetInterface(DWORD face, InterfaceProc faceproc);

#define XMPFUNC_IN_FACE		11

typedef struct {
	void (WINAPI *SetLength)(float length, BOOL seekable);
	void (WINAPI *SetGain)(DWORD mode, float gain);
	BOOL (WINAPI *UpdateTitle)(const char *track);
	BOOL (WINAPI *GetLooping)();
} XMPFUNC_IN;

#ifdef __cplusplus
}
#endif

// XMPlay plugin functions header
// new plugins can be submitted to plugins@xmplay.com

#pragma once

#include <wtypes.h>

typedef unsigned __int64 QWORD;

#ifdef __cplusplus
extern "C" {
#endif

#define Utf2Uni(src,slen,dst,dlen) MultiByteToWideChar(CP_UTF8,0,src,slen,dst,dlen)

typedef void *(WINAPI *InterfaceProc)(DWORD face);

#define XMPFUNC_MISC_FACE		0
#define XMPFUNC_REGISTRY_FACE	1
#define XMPFUNC_FILE_FACE		2
#define XMPFUNC_TEXT_FACE		3
#define XMPFUNC_STATUS_FACE		4

#define XMPCONFIG_NET_BUFFER	0
#define XMPCONFIG_NET_RESTRICT	1
#define XMPCONFIG_NET_RECONNECT	2
#define XMPCONFIG_NET_PROXY		3
#define XMPCONFIG_NET_PROXYCONF	4
#define XMPCONFIG_NET_TIMEOUT	5
#define XMPCONFIG_NET_PREBUF	6
#define XMPCONFIG_OUTPUT		7

#define XMPINFO_TEXT_GENERAL	0
#define XMPINFO_TEXT_MESSAGE	1
#define XMPINFO_TEXT_SAMPLES	2

#define XMPINFO_REFRESH_MAIN		1
#define XMPINFO_REFRESH_GENERAL		2
#define XMPINFO_REFRESH_MESSAGE		4
#define XMPINFO_REFRESH_SAMPLES		8

typedef void *XMPFILE;

#define XMPFILE_TYPE_MEMORY		0
#define XMPFILE_TYPE_FILE		1
#define XMPFILE_TYPE_NETFILE	2
#define XMPFILE_TYPE_NETSTREAM	3

typedef void (WINAPI *XMPSHORTCUTPROC)();
typedef void (WINAPI *XMPSHORTCUTPROCEX)(DWORD id);

typedef struct {
	DWORD id;
	const char *text;
	union {
		XMPSHORTCUTPROC proc;
		XMPSHORTCUTPROCEX procex;
	};
} XMPSHORTCUT;

typedef struct {
	DWORD rate;
	DWORD chan;
	WORD res;
	WORD chanmask;
} XMPFORMAT;

typedef struct {
	float time;
	const char *title;
	const char *performer;
} XMPCUE;

#define TAG_FORMATTED_TITLE		(char*)-1
#define TAG_FILENAME			(char*)-2
#define TAG_TRACK_TITLE			(char*)-3
#define TAG_LENGTH				(char*)-4
#define TAG_SUBSONGS			(char*)-5
#define TAG_SUBSONG				(char*)-6
#define TAG_RATING				(char*)-7
#define TAG_TRACK_URL			(char*)-8
#define TAG_TITLE				(char*)0
#define TAG_ARTIST				(char*)1
#define TAG_ALBUM				(char*)2
#define TAG_DATE				(char*)3
#define TAG_TRACK				(char*)4
#define TAG_GENRE				(char*)5
#define TAG_COMMENT				(char*)6
#define TAG_FILETYPE			(char*)7

typedef struct {
	DWORD (WINAPI *GetVersion)();
	HWND (WINAPI *GetWindow)();
	void *(WINAPI *Alloc)(DWORD len);
	void *(WINAPI *ReAlloc)(void *mem, DWORD len);
	void (WINAPI *Free)(void *mem);
	BOOL (WINAPI *CheckCancel)();
	DWORD (WINAPI *GetConfig)(DWORD option);
	const char *(WINAPI *GetSkinConfig)(const char *name);
	void (WINAPI *ShowBubble)(const char *text, DWORD time);
	void (WINAPI *RefreshInfo)(DWORD mode);
	char *(WINAPI *GetInfoText)(DWORD mode);
	char *(WINAPI *FormatInfoText)(char *buf, const char *name, const char *value);
	char *(WINAPI *GetTag)(const char *tag);
	BOOL (WINAPI *RegisterShortcut)(const XMPSHORTCUT *cut);
	BOOL (WINAPI *PerformShortcut)(DWORD id);
	const XMPCUE *(WINAPI *GetCue)(DWORD cue);
	BOOL (WINAPI *DDE)(const char *command);
	char *(WINAPI *ProcessID3v2)(const BYTE *id3v2, DWORD size);
	void (WINAPI *ErrorMessage)(const char *text, const char *caption);
	HWND (WINAPI *GetPopupParent)();
} XMPFUNC_MISC;

typedef struct {
	DWORD (WINAPI *Get)(const char *section, const char *key, void *data, DWORD size);
	DWORD (WINAPI *GetString)(const char *section, const char *key, char *data, DWORD size);
	BOOL (WINAPI *GetInt)(const char *section, const char *key, int *data);
	BOOL (WINAPI *Set)(const char *section, const char *key, const void *data, DWORD size);
	BOOL (WINAPI *SetString)(const char *section, const char *key, const char *data);
	BOOL (WINAPI *SetInt)(const char *section, const char *key, const int *data);
} XMPFUNC_REGISTRY;

typedef struct {
	XMPFILE (WINAPI *Open)(const char *filename);
	XMPFILE (WINAPI *OpenMemory)(const void *buf, DWORD len);
	void (WINAPI *Close)(XMPFILE file);
	DWORD (WINAPI *GetType)(XMPFILE file);
	union {
		DWORD (WINAPI *GetSize)(XMPFILE file);
		QWORD (WINAPI *GetSize64)(XMPFILE file);
	};
	const char *(WINAPI *GetFilename)(XMPFILE file);
	const void *(WINAPI *GetMemory)(XMPFILE file);
	DWORD (WINAPI *Read)(XMPFILE file, void *buf, DWORD len);
	BOOL (WINAPI *Seek)(XMPFILE file, DWORD pos);
	union {
		DWORD (WINAPI *Tell)(XMPFILE file);
		QWORD (WINAPI *Tell64)(XMPFILE file);
	};
	void (WINAPI *NetSetRate)(XMPFILE file, DWORD rate);
	int (WINAPI *NetIsActive)(XMPFILE file);
	BOOL (WINAPI *NetPreBuf)(XMPFILE file);
	DWORD (WINAPI *NetAvailable)(XMPFILE file);
	char *(WINAPI *ArchiveList)(XMPFILE file);
	XMPFILE (WINAPI *ArchiveExtract)(XMPFILE file, const char *entry, DWORD len);
	XMPFILE (WINAPI *OpenRange)(const char *filename, QWORD offset, QWORD length);
	BOOL (WINAPI *Seek64)(XMPFILE file, QWORD pos);
	QWORD (WINAPI *GetOffset)(XMPFILE file);
} XMPFUNC_FILE;

typedef struct {
	char *(WINAPI *Ansi)(const char *text, int len);
	char *(WINAPI *Unicode)(const WCHAR *text, int len);
	char *(WINAPI *Utf8)(const char *text, int len);
} XMPFUNC_TEXT;

typedef struct {
	BOOL (WINAPI *IsPlaying)();
	double (WINAPI *GetTime)();
	QWORD (WINAPI *GetWritten)();
	DWORD (WINAPI *GetLatency)();
	const XMPFORMAT *(WINAPI *GetFormat)(BOOL in);
} XMPFUNC_STATUS;

#ifdef __cplusplus
}
#endif

#ifndef XMP_PC98_H
#define XMP_PC98_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PC98_PLUGIN_NAME    "PC-98 / S98"
#define PC98_PLUGIN_VERSION "1.0.20"
#define PC98_PLUGIN_XMPVER  1002000
#define PC98_MAX_FILE       (16u * 1024u * 1024u)
#define PC98_MAX_SONGS      256
#define PC98_DEFAULT_RATE   44100
#define PC98_DEFAULT_LOOPS  1
#define PC98_PATH_MAX       512
#define PC98_DUMMY_MS       180000

typedef enum {
	PC98_KIND_NONE = 0,
	PC98_KIND_S98,
	PC98_KIND_PMD,
	PC98_KIND_FMP,
	PC98_KIND_SET,
	PC98_KIND_BGMDRV,
	PC98_KIND_NA,
	PC98_KIND_MFD,
	PC98_KIND_N3G,
	PC98_KIND_PAI,
	PC98_KIND_MSB,
	PC98_KIND_OPNMD,
	PC98_KIND_NTL,
	PC98_KIND_GNTL,
	PC98_KIND_FMD,
	PC98_KIND_MSDRV
} pc98_kind;

typedef struct {
	int loop_count;
	int mute_fm;
	int mute_ssg;
	int mute_rhythm;
	int mute_adpcm;
	int fade_ms;
	int rate;
	char rhythm_path[PC98_PATH_MAX];
	char hoot_xml[PC98_PATH_MAX];
	char music_root[PC98_PATH_MAX];
	char cache_dir[PC98_PATH_MAX];
	char hootrip_path[PC98_PATH_MAX];
	char dll_dir[PC98_PATH_MAX];
	char gs_sf2[PC98_PATH_MAX];
	char mt_sf2[PC98_PATH_MAX];
} pc98_cfg;

typedef struct {
	pc98_kind kind;
	int songs;
	int looping;
	int one_loop_ms[PC98_MAX_SONGS];
	char title[256];
	char artist[256];
	char game[256];
	char comment[256];
	char filetype[32];
	char engine[64];
	char set_id[64];
} pc98_info;

typedef struct pc98_player pc98_player;

void pc98_cfg_defaults(pc98_cfg *c);
void pc98_cfg_clamp(pc98_cfg *c);

pc98_kind pc98_probe(const char *filename, const uint8_t *data, size_t len);
int pc98_analyze(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg, pc98_info *out);

pc98_player *pc98_player_open(const char *filename, const uint8_t *data, size_t len,
		const pc98_cfg *cfg);
void pc98_player_close(pc98_player *p);
int pc98_player_process(pc98_player *p, float *buf, int count);
int pc98_player_seek_ms(pc98_player *p, int ms);
int pc98_player_set_song(pc98_player *p, int song);
int pc98_player_song(const pc98_player *p);
int pc98_player_songs(const pc98_player *p);
int pc98_player_rate(const pc98_player *p);
int pc98_player_one_loop_ms(const pc98_player *p, int song);
int pc98_player_play_ms(const pc98_player *p, int song);
int pc98_player_total_one_loop_ms(const pc98_player *p);
int pc98_player_loop_count(const pc98_player *p);
void pc98_player_set_loop_count(pc98_player *p, int loops);
void pc98_player_apply_mute(pc98_player *p, const pc98_cfg *cfg);
pc98_kind pc98_player_kind(const pc98_player *p);
const char *pc98_player_title(const pc98_player *p);
const char *pc98_player_artist(const pc98_player *p);
const char *pc98_player_game(const pc98_player *p);
const char *pc98_player_engine(const pc98_player *p);
const char *pc98_player_filetype(const pc98_player *p);

int pc98_set_write(const char *path, const char *set_id, const char *title,
		int nsong, const char *const *names, const int *codes);
int pc98_set_parse(const uint8_t *data, size_t len, pc98_info *out,
		char paths[][PC98_PATH_MAX], int *codes, int max_songs);

#ifdef __cplusplus
}
#endif

#endif

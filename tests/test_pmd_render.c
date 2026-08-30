#include "pc98.h"
#include "engines/pmd_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define ACCESS _access
#else
#include <unistd.h>
#define ACCESS access
#endif

static const char *k_paths[] = {
	"D:\\spel\\pc98\\pc98 music\\#\\5venus_98\\5_01.M",
	"D:\\spel\\pc98\\pc98 music\\S\\sailor_98\\PASO1_N.M",
	"D:\\spel\\pc98\\pc98 music\\M\\mime_98\\b01_b.M",
	"D:\\spel\\pc98\\pc98 music\\N\\nightsft_98\\NS01.LSP",
	NULL
};

static int load(const char *path, uint8_t **out, size_t *len)
{
	FILE *fp;
	long sz;
	fp = fopen(path, "rb");
	if (!fp) return -1;
	fseek(fp, 0, SEEK_END);
	sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (sz < 16) { fclose(fp); return -1; }
	*out = (uint8_t *)malloc((size_t)sz);
	if (!*out) { fclose(fp); return -1; }
	*len = fread(*out, 1, (size_t)sz, fp);
	fclose(fp);
	return 0;
}

int main(void)
{
	pc98_cfg cfg;
	int tested = 0, fail = 0, i;

	pc98_cfg_defaults(&cfg);
	for (i = 0; k_paths[i]; ++i) {
		uint8_t *data = NULL;
		size_t len = 0;
		pc98_info inf;
		void *h;
		float buf[1024];
		int got;
		if (ACCESS(k_paths[i], 0) != 0)
			continue;
		if (load(k_paths[i], &data, &len) != 0)
			continue;
		tested++;
		if (!pmd_probe_mem(data, len)) {
			fprintf(stderr, "pmd probe fail: %s\n", k_paths[i]);
			fail++;
			free(data);
			continue;
		}
		if (pc98_probe(k_paths[i], data, 16) != PC98_KIND_PMD) {
			fprintf(stderr, "pc98_probe PMD fail: %s\n", k_paths[i]);
			fail++;
		}
		if (pmd_analyze_mem(k_paths[i], data, len, &cfg, &inf) != 0) {
			fprintf(stderr, "pmd analyze fail: %s\n", k_paths[i]);
			fail++;
			free(data);
			continue;
		}
		if (inf.one_loop_ms[0] < 200) {
			fprintf(stderr, "pmd short length %d: %s\n", inf.one_loop_ms[0], k_paths[i]);
			fail++;
		}
		if (strstr(k_paths[i], "NS01.LSP") && inf.one_loop_ms[0] < 60000) {
			fprintf(stderr, "NS01.LSP one-loop %d (want first play ~106s, not L-tail)\n",
				inf.one_loop_ms[0]);
			fail++;
		}
		h = pmd_open_mem(k_paths[i], data, len, &cfg);
		if (!h) {
			fprintf(stderr, "pmd open fail: %s\n", k_paths[i]);
			fail++;
			free(data);
			continue;
		}
		got = pmd_process_h(h, buf, 512);
		if (got <= 0) {
			fprintf(stderr, "pmd process fail: %s\n", k_paths[i]);
			fail++;
		}
		pmd_close_h(h);
		printf("  PMD %s length=%dms frames=%d title=%s\n",
			k_paths[i], inf.one_loop_ms[0], got, inf.title);
		free(data);
	}
	if (tested == 0) {
		/* Still verify ZX-like junk is rejected. */
		uint8_t zx[16] = { 0x4D, 0x00, 0x00, 0x00 };
		if (pmd_probe_mem(zx, sizeof zx)) {
			fprintf(stderr, "pmd probe accepted ZX-like junk\n");
			return 1;
		}
		printf("test_pmd_render: no local PMD files; probe-reject ok (skipped live)\n");
		return 0;
	}
	printf("test_pmd_render: %d files, %d failed\n", tested, fail);
	return fail ? 1 : 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "engines/mmd_engine.h"
#include "pc98.h"
#include "pc98_util.h"

static uint8_t *load(const char *path, size_t *n) {
	FILE *f = fopen(path, "rb"); long sz; uint8_t *b;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
	b = (uint8_t *)malloc((size_t)sz); fread(b, 1, (size_t)sz, f); fclose(f);
	*n = (size_t)sz; return b;
}
static double peak(const float *a, int n) {
	double p = 0; int i; for (i = 0; i < n * 2; i++) { double v = fabs(a[i]); if (v > p) p = v; }
	return p;
}
static int render(void *h, float *buf, int frames) {
	int i = 0;
	while (i < frames) {
		int c = frames - i; if (c > 4096) c = 4096;
		int r = mmd_process_h(h, buf + (size_t)i * 2, c);
		if (r <= 0) break;
		i += r;
	}
	return i;
}
int main(void) {
	pc98_cfg cfg;
	const char *A = "/workspace/eime_98/EMI_01.MMD";
	const char *B = "/workspace/eime_98/EMI_25.MMD";
	uint8_t *da, *db; size_t na, nb;
	void *ha, *hb, *hb2;
	float *bufA, *bufB, *bufF;
	int rate, n2s, n500, i;
	double maxdiff;

	memset(&cfg, 0, sizeof cfg);
	cfg.rate = 44100; cfg.loop_count = 1;
	pc98_bounded(cfg.gs_sf2, sizeof cfg.gs_sf2, "/workspace/sf2/FluidR3_GM.sf2");

	da = load(A, &na); db = load(B, &nb);
	if (!da || !db) { perror("load"); return 1; }

	ha = mmd_open_mem(A, da, na, &cfg);
	if (!ha) { fprintf(stderr, "open A\n"); return 1; }
	rate = mmd_rate_h(ha);
	n2s = rate * 2; n500 = rate / 2;
	bufA = (float *)calloc((size_t)n2s * 2, sizeof(float));
	render(ha, bufA, n2s);
	printf("A 2s peak=%.4f\n", peak(bufA, n2s));
	mmd_close_h(ha);

	hb = mmd_open_mem(B, db, nb, &cfg);
	if (!hb) { fprintf(stderr, "open B\n"); return 1; }
	bufB = (float *)calloc((size_t)n500 * 2, sizeof(float));
	render(hb, bufB, n500);

	hb2 = mmd_open_mem(B, db, nb, &cfg);
	if (!hb2) { fprintf(stderr, "open B2\n"); return 1; }
	bufF = (float *)calloc((size_t)n500 * 2, sizeof(float));
	render(hb2, bufF, n500);

	maxdiff = 0;
	for (i = 0; i < n500 * 2; i++) {
		double d = fabs(bufB[i] - bufF[i]);
		if (d > maxdiff) maxdiff = d;
	}
	printf("B@500ms after A peak=%.4f fresh=%.4f maxdiff=%.8f\n",
		peak(bufB, n500), peak(bufF, n500), maxdiff);

	/* seek silence: after playing, seek 0 and check first 50ms matches fresh reopen from 0 */
	mmd_seek_ms_h(hb, 0);
	{
		float *s1 = (float *)calloc((size_t)(rate/20) * 2, sizeof(float));
		float *s2 = (float *)calloc((size_t)(rate/20) * 2, sizeof(float));
		double md = 0;
		render(hb, s1, rate / 20);
		mmd_close_h(hb2);
		hb2 = mmd_open_mem(B, db, nb, &cfg);
		render(hb2, s2, rate / 20);
		for (i = 0; i < (rate/20)*2; i++) {
			double d = fabs(s1[i] - s2[i]);
			if (d > md) md = d;
		}
		printf("seek0 vs fresh open maxdiff=%.8f\n", md);
		free(s1); free(s2);
		if (md > 1e-5) { fprintf(stderr, "FAIL seek\n"); return 3; }
	}

	mmd_close_h(hb); mmd_close_h(hb2);
	free(da); free(db); free(bufA); free(bufB); free(bufF);
	if (maxdiff > 1e-6) { fprintf(stderr, "FAIL carry\n"); return 2; }
	printf("PASS\n");
	return 0;
}

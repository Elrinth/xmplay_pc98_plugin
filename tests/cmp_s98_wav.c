#include "pc98.h"
#include "engines/s98_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

int main(int argc, char **argv)
{
	const char *path;
	FILE *fp;
	uint8_t *raw;
	long sz;
	pc98_cfg cfg;
	void *h;
	float *buf;
	int want, got, i;
	double acc = 0, peak = 0;

	path = argc > 1 ? argv[1] :
		"F:\\YeOldeDisk\\Musik\\S98 (Pc98 AND PC88)\\Night Slave (PC-98)(1996)(Melody)\\02.S98";
	fp = fopen(path, "rb");
	if (!fp) {
		fprintf(stderr, "cannot open %s\n", path);
		return 1;
	}
	fseek(fp, 0, SEEK_END);
	sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	raw = (uint8_t *)malloc((size_t)sz);
	if (!raw || fread(raw, 1, (size_t)sz, fp) != (size_t)sz) {
		fclose(fp);
		return 1;
	}
	fclose(fp);
	pc98_cfg_defaults(&cfg);
	h = s98_open(raw, (size_t)sz, &cfg);
	free(raw);
	if (!h) {
		fprintf(stderr, "s98_open failed\n");
		return 1;
	}
	want = cfg.rate * 10;
	buf = (float *)calloc((size_t)want * 2, sizeof(float));
	got = s98_process(h, buf, want);
	for (i = 0; i < got * 2; ++i) {
		double a = fabs((double)buf[i]);
		acc += (double)buf[i] * (double)buf[i];
		if (a > peak) peak = a;
	}
	printf("cmp_s98_wav: frames=%d rms=%.5f peak=%.5f (16-bit peak~%.0f)\n",
		got, got ? sqrt(acc / (got * 2)) : 0.0, peak, peak * 32768.0);
	printf("  ref WAV first10s rms~0.032 peak~0.212 (55466 Hz, L=R)\n");
	s98_close(h);
	free(buf);
	return 0;
}

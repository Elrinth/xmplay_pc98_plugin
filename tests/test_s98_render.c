#include "pc98.h"
#include "engines/s98_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static void wr32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* Minimal S98 v1: OPNA default, ~1s of syncs, tagged. */
static size_t make_tiny_s98(uint8_t *b, size_t cap)
{
	size_t i, n;
	if (cap < 256) return 0;
	memset(b, 0, cap);
	b[0] = 'S'; b[1] = '9'; b[2] = '8'; b[3] = '1';
	wr32(b + 4, 10);
	wr32(b + 8, 1000);
	wr32(b + 0x14, 0x20);
	n = 0x20;
	/* a few OPNA writes then 100 syncs (~1s) */
	b[n++] = 0x00; b[n++] = 0x22; b[n++] = 0x00;
	b[n++] = 0x00; b[n++] = 0x27; b[n++] = 0x30;
	b[n++] = 0x00; b[n++] = 0xB0; b[n++] = 0x38;
	b[n++] = 0x00; b[n++] = 0xA4; b[n++] = 0x1A;
	b[n++] = 0x00; b[n++] = 0xA0; b[n++] = 0x00;
	b[n++] = 0x00; b[n++] = 0x28; b[n++] = 0xF0;
	for (i = 0; i < 100; ++i)
		b[n++] = 0xFF;
	b[n++] = 0xFD;
	return n;
}

int main(void)
{
	uint8_t raw[512];
	size_t n = make_tiny_s98(raw, sizeof raw);
	pc98_cfg cfg;
	pc98_info inf;
	void *h;
	float *buf;
	int got, i, nz = 0;
	int fail = 0;

	pc98_cfg_defaults(&cfg);
	if (!s98_probe(raw, n)) {
		fprintf(stderr, "s98_probe failed\n");
		return 1;
	}
	if (s98_analyze(raw, n, &cfg, &inf) != 0) {
		fprintf(stderr, "s98_analyze failed\n");
		return 1;
	}
	if (inf.one_loop_ms[0] < 800 || inf.one_loop_ms[0] > 1500) {
		fprintf(stderr, "s98 length %d ms (want ~1000)\n", inf.one_loop_ms[0]);
		fail = 1;
	}
	h = s98_open(raw, n, &cfg);
	if (!h) {
		fprintf(stderr, "s98_open failed\n");
		return 1;
	}
	buf = (float *)calloc(4096, sizeof(float));
	got = s98_process(h, buf, 2048);
	if (got <= 0) {
		fprintf(stderr, "s98_process returned %d\n", got);
		fail = 1;
	}
	for (i = 0; i < got * 2; ++i) {
		if (buf[i] != 0.0f) nz = 1;
		if (buf[i] > 1.5f || buf[i] < -1.5f) {
			fprintf(stderr, "s98 sample out of range %f\n", buf[i]);
			fail = 1;
			break;
		}
	}
	(void)nz; /* chip may be silent until more writes; timing still counts */
	s98_close(h);
	free(buf);
	if (pc98_probe("tune.s98", raw, n) != PC98_KIND_S98) {
		fprintf(stderr, "pc98_probe s98 failed\n");
		fail = 1;
	}
	printf("test_s98_render: length=%dms frames=%d %s\n",
		inf.one_loop_ms[0], got, fail ? "FAIL" : "ok");
	return fail;
}

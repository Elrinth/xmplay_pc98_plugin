#include "pc98.h"
#include "engines/ntl_engine.h"
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <vector>

static void write_wav(const char *path, const float *x, int frames, int rate) {
	FILE *f = fopen(path, "wb"); if (!f) return;
	std::vector<int16_t> pcm(frames * 2);
	float peak = 0.f;
	for (int i = 0; i < frames * 2; i++) {
		float a = fabsf(x[i]); if (a > peak) peak = a;
	}
	float g = peak > 1e-6f ? (0.85f / peak) : 1.f;
	for (int i = 0; i < frames * 2; i++) {
		float v = x[i] * g; if (v > 1) v = 1; if (v < -1) v = -1;
		pcm[i] = (int16_t)lrintf(v * 32767.f);
	}
	int dsz = frames * 4, rsz = 36 + dsz;
	fwrite("RIFF",1,4,f); fwrite(&rsz,4,1,f); fwrite("WAVEfmt ",1,8,f);
	int32_t c16=16; int16_t fmt=1,ch=2,bps=16,ba=4; int32_t br=rate*4;
	fwrite(&c16,4,1,f); fwrite(&fmt,2,1,f); fwrite(&ch,2,1,f);
	fwrite(&rate,4,1,f); fwrite(&br,4,1,f); fwrite(&ba,2,1,f); fwrite(&bps,2,1,f);
	fwrite("data",1,4,f); fwrite(&dsz,4,1,f);
	fwrite(pcm.data(),2,frames*2,f); fclose(f);
	printf("peak_raw=%.5f gain=%.2f -> %s\n", peak, g, path);
}

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s in.NTL out.wav [seconds]\n", argv[0]);
		return 2;
	}
	pc98_cfg cfg; memset(&cfg, 0, sizeof cfg); pc98_cfg_defaults(&cfg);
	cfg.rate = 44100; cfg.loop_count = 1;
	FILE *fp = fopen(argv[1], "rb");
	if (!fp) { perror(argv[1]); return 1; }
	fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
	std::vector<uint8_t> data(sz); fread(data.data(), 1, sz, fp); fclose(fp);

	void *h = ntl_open_mem(argv[1], data.data(), data.size(), &cfg);
	if (!h) { fprintf(stderr, "ntl open fail\n"); return 1; }
	int rate = ntl_rate_h(h);
	int loop_ms = ntl_one_loop_ms_h(h);
	int ms = (argc >= 4) ? (int)(atof(argv[3]) * 1000) : (loop_ms > 0 ? loop_ms : 60000);
	if (ms < 2000) ms = 2000;
	if (ms > 120000) ms = 120000;
	int frames = (int)((int64_t)ms * rate / 1000);
	std::vector<float> buf(frames * 2, 0.f);
	int got = 0;
	while (got < frames) {
		int n = frames - got; if (n > 2048) n = 2048;
		int r = ntl_process_h(h, buf.data() + got * 2, n);
		if (r <= 0) break;
		got += r;
	}
	write_wav(argv[2], buf.data(), got, rate);
	printf("[ntl] loop=%dms render=%dms frames=%d\n", loop_ms, ms, got);
	ntl_close_h(h);
	return 0;
}

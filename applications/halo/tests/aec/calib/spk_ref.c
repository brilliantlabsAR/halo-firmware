/* Host model of the Halo speaker chain as the AEC tap sees it:
 * (LC3-decoded) PCM -> volume (100 = passthrough) -> speaker_protect
 * with the 0.8.17 Kconfig + speaker.start{volume, gain, budget}.
 * The production drivers/audio/max98357a/speaker_protect.c is compiled
 * unchanged (calib_build.py).
 *
 *   spk_ref in.wav out.wav [volume=100] [gain_db=6] [budget=100] [display_drop=0]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "speaker_protect.h"

static int16_t *rd(const char *p, size_t *n)
{
	FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(1); }
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t *b = malloc(sz);
	if (fread(b, 1, sz, f) != (size_t)sz) { perror(p); exit(1); }
	fclose(f);
	long i = 12;
	while (i + 8 <= sz) {
		uint32_t c = b[i+4] | (b[i+5]<<8) | (b[i+6]<<16) | ((uint32_t)b[i+7]<<24);
		if (!memcmp(b + i, "data", 4)) {
			*n = c / 2; int16_t *s = malloc(c); memcpy(s, b + i + 8, c); free(b); return s;
		}
		i += 8 + c + (c & 1);
	}
	exit(1);
}

static void wr(const char *p, const int16_t *s, size_t n)
{
	FILE *f = fopen(p, "wb");
	uint32_t data = n * 2, riff = 36 + data, sr = 16000, br = 32000;
	uint8_t h[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',
		16,0,0,0,1,0,1,0,0,0,0,0,0,0,0,0,2,0,16,0,'d','a','t','a',0,0,0,0};
	memcpy(h+4,&riff,4); memcpy(h+24,&sr,4); memcpy(h+28,&br,4); memcpy(h+40,&data,4);
	fwrite(h,1,44,f); fwrite(s,2,n,f); fclose(f);
}

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage\n"); return 2; }
	int vol = argc > 3 ? atoi(argv[3]) : 100;
	int gain = argc > 4 ? atoi(argv[4]) : 6;
	int budget = argc > 5 ? atoi(argv[5]) : 100;
	int drop = argc > 6 ? atoi(argv[6]) : 0;
	size_t n; int16_t *x = rd(argv[1], &n);
	struct spk_protect_params p = {
		.sample_rate = 16000, .channels = 1, .hpf_cutoff_hz = 200,
		.budget_percent = 80, .env_ms = 3, .hold_ms = 20,
		.release_ms = 150, .ramp_ms = 15, .bass_drive_percent = 0,
		.peak_cap_percent = 300,
	};
	if (budget > 0) { p.budget_percent = budget; p.env_ms = 1; } /* STREAM_BUDGET_ENV_MS */
	static struct spk_protect sp;
	if (spk_protect_init(&sp, &p)) { fprintf(stderr, "init failed\n"); return 1; }
	spk_protect_set_budget_drop(&sp, drop);
	if (gain > 0) spk_protect_set_pregain(&sp, spk_protect_db10_to_q15(gain * 10));
	spk_protect_reset(&sp);
	/* volume then protect, in the driver's 320-sample (640 B) blocks */
	uint32_t scale = ((uint32_t)vol << 16) / 100U;
	for (size_t o = 0; o < n; o += 320) {
		size_t m = (n - o < 320) ? n - o : 320;
		if (vol != 100) for (size_t i = 0; i < m; i++)
			x[o+i] = (int16_t)(((int32_t)x[o+i] * (int32_t)scale) >> 16);
		spk_protect_process(&sp, x + o, m);
	}
	struct spk_protect_stats st; spk_protect_stats_read(&sp, &st, false);
	fprintf(stderr, "spk_ref: min_gain %.3f limited %u/%u peak_out %d peak_capped %u\n",
		st.min_gain_q15 / 32768.0, st.limited_frames, st.frames, st.peak_out, st.peak_capped_frames);
	wr(argv[2], x, n);
	return 0;
}

/*
 * Standalone host simulator for the AEC margin-servo (AEC_MARGIN_SERVO).
 * The host test_aec suite feeds mic and reference in perfect lockstep, so it
 * cannot reproduce the emit-vs-mic CLOCK SKEW that walks `margin` negative on
 * the device. This drives the real audio_aec.c with a controllable skew: the
 * reference (I2S/emit) clock runs `ref_ppm` slower than the mic/wall clock, so
 * fewer reference samples are written per unit wall time and the read window
 * (mic-anchored) drifts past the write head - exactly the worn failure.
 *
 * Build servo-off vs servo-on and compare margin/ref_underruns over time.
 *   gcc -O2 -I. -I <inc> -DCONFIG_HALO_AUDIO_AEC_TAPS=1024 -DCONFIG_HALO_LOG_LEVEL=3 \
 *       [-DCONFIG_HALO_AUDIO_AEC_FDAF=1 -DCONFIG_HALO_AUDIO_AEC_FDAF_PARTS=3] \
 *       [-DAEC_MARGIN_SERVO] -o skew_sim <aec.c> skew_sim.c -lm
 *   ./skew_sim <ref_ppm> <seconds>
 */
#include <math.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <halo/audio_aec.h>
#include <max98357a_audio.h>

void max98357a_audio_set_tx_tap(max98357a_audio_tx_tap_t tap) { (void)tap; }
uint32_t host_uptime_ms; /* virtual clock for k_uptime_get() */

#define SR  16000
#define BLK 320 /* 20ms */

static uint32_t rng = 0x2468ace0;
static float frand(void)
{
	rng = rng * 1664525u + 1013904223u;
	return ((int32_t)rng >> 8) / 8388608.0f;
}

int main(int argc, char **argv)
{
	double ref_ppm = (argc > 1) ? atof(argv[1]) : 250.0; /* ref clock slow-by */
	int secs = (argc > 2) ? atoi(argv[2]) : 240;

	audio_aec_enable(true);

	double ref_debt = 0.0;
	int nblk = secs * 1000 / 20;

	printf("# ref clock %.0f ppm slow, %ds\n", ref_ppm, secs);
	printf("# t(s)  margin  ref_underruns  ref_skew_adj  resyncs  ref_pads\n");

	for (int blk = 0; blk < nblk; blk++) {
		host_uptime_ms = (uint32_t)(blk * 20); /* wall = mic clock */

		/* reference: slow clock delivers fewer samples per 20ms wall */
		double want = SR * 0.02 * (1.0 - ref_ppm / 1e6);

		ref_debt += want;
		int rn = (int)(ref_debt + 0.5);

		ref_debt -= rn;
		if (rn > BLK) {
			rn = BLK;
		}

		int16_t refblk[BLK * 2];

		for (int i = 0; i < rn; i++) {
			double ph = 2 * M_PI * 300.0 * (blk * (double)BLK + i) / SR;
			float s = 0.30f * (float)sin(ph) + 0.05f * frand();
			int16_t v = (int16_t)(s * 8000);

			refblk[2 * i] = v;
			refblk[2 * i + 1] = v;
		}
		audio_aec_feed_reference(refblk, (size_t)rn * 2, SR, 2);

		/* mic: exactly one block per 20ms (mic == wall clock); a delayed,
		 * scaled echo of the same tone so p_ref>0 keeps the path engaged */
		int16_t mic[BLK];

		for (int i = 0; i < BLK; i++) {
			double ph = 2 * M_PI * 300.0 * (blk * (double)BLK + i - 30) / SR;
			float e = 0.12f * (float)sin(ph);

			mic[i] = (int16_t)(e * 8000 + 40 * frand());
		}
		audio_aec_process(mic, BLK, SR, 1);

		if (blk % 250 == 249) { /* ~ every 5s */
			struct audio_aec_stats st;
			size_t taps;

			audio_aec_snapshot(&st, &taps);
			printf("%5d  %6d  %12u  %11d  %7u  %8llu\n",
			       (blk + 1) * 20 / 1000, st.margin_last,
			       st.ref_underruns, st.ref_skew_adj, st.resyncs,
			       (unsigned long long)st.ref_pads);
		}
	}
	return 0;
}

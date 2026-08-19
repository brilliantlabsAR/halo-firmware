/* Offline driver: run a real (mic, reference) WAV pair through the actual
 * firmware audio_aec.c (FDAF), so suppressor changes can be A/B'd against
 * real worn echo + Silero, no device. Mirrors test_aec.c's feed/process/clock
 * ordering. mic.wav = raw echo (an AEC-off duplex capture), ref.wav = the
 * duplex_ref stamped on the mic clock.
 *
 *   gcc -O2 -I. -I../../../../../modules/halo/include \
 *       -DCONFIG_HALO_AUDIO_AEC_TAPS=1024 -DCONFIG_HALO_LOG_LEVEL=3 \
 *       -DCONFIG_HALO_AUDIO_AEC_FDAF=1 -DCONFIG_HALO_AUDIO_AEC_FDAF_PARTS=3 \
 *       -o aec_wav ../../../../../modules/halo/src/audio_aec.c aec_wav.c -lm
 *   ./aec_wav mic.wav ref.wav out.wav
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <halo/audio_aec.h>
#include <max98357a_audio.h>

#define SR  16000
#define BLK 320   /* 20 ms */

uint32_t host_uptime_ms; /* backs the host k_uptime_get_32() stub */
/* audio_aec's sys-init hook registers a speaker tap; unused offline */
void max98357a_audio_set_tx_tap(max98357a_audio_tx_tap_t tap) { (void)tap; }

/* Minimal WAV reader: scan for the "data" chunk, return int16 samples. */
static int16_t *read_wav(const char *path, size_t *n_out)
{
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); exit(1); }
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t *buf = malloc(sz); fread(buf, 1, sz, f); fclose(f);
	long i = 12; /* skip RIFF/WAVE */
	while (i + 8 <= sz) {
		uint32_t csz = buf[i+4] | (buf[i+5]<<8) | (buf[i+6]<<16) | ((uint32_t)buf[i+7]<<24);
		if (memcmp(buf + i, "data", 4) == 0) {
			size_t n = csz / 2;
			int16_t *s = malloc(n * sizeof(int16_t));
			memcpy(s, buf + i + 8, n * sizeof(int16_t));
			free(buf); *n_out = n; return s;
		}
		i += 8 + csz + (csz & 1);
	}
	fprintf(stderr, "no data chunk in %s\n", path); exit(1);
}

static void write_wav(const char *path, const int16_t *s, size_t n)
{
	FILE *f = fopen(path, "wb");
	uint32_t data = n * 2, riff = 36 + data;
	uint8_t h[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',
		16,0,0,0, 1,0, 1,0, 0,0,0,0, 0,0,0,0, 2,0, 16,0, 'd','a','t','a',0,0,0,0};
	uint32_t br = SR * 2;
	memcpy(h+4,&riff,4); memcpy(h+24,(uint32_t[]){SR},4); memcpy(h+28,&br,4);
	memcpy(h+40,&data,4);
	fwrite(h,1,44,f); fwrite(s,2,n,f); fclose(f);
}

int main(int argc, char **argv)
{
	if (argc < 4) { fprintf(stderr, "usage: %s mic.wav ref.wav out.wav\n", argv[0]); return 2; }
	size_t nm, nr;
	int16_t *mic = read_wav(argv[1], &nm);
	int16_t *ref = read_wav(argv[2], &nr);
	size_t n = nm < nr ? nm : nr;

	audio_aec_enable(true);
	int16_t *out = calloc(n, sizeof(int16_t));

	for (size_t off = 0; off + BLK <= n; off += BLK) {
		host_uptime_ms += 20;
		/* The real speaker taps the reference only while emitting; the
		 * harness pads inter-reply gaps with exact-zero LC3 silence. Feed
		 * only non-silent blocks so the AEC's speaker-idle gate works
		 * (feeding the zero padding would keep it "active" forever). */
		int64_t e = 0;
		for (int i = 0; i < BLK; i++) e += (int64_t)ref[off+i] * ref[off+i];
		if (e / BLK > 1) {
			audio_aec_feed_reference(ref + off, BLK, SR, 1);
		}
		/* mic captured ~same instant (real emission-capture delay is in
		 * the data); process in place -> cancelled output */
		int16_t blk[BLK];
		memcpy(blk, mic + off, BLK * sizeof(int16_t));
		audio_aec_process(blk, BLK, SR, 1);
		memcpy(out + off, blk, BLK * sizeof(int16_t));
#ifdef AEC_WAV_DUMP
		/* per-block gate-tuning dump: sample_index sy se perr pref gmean
		 * gate_fast gate_mid gate_floor gate_rel. sy/se = the suppressor's
		 * smoothed predicted-echo / residual power; the gate_* columns are
		 * the envelope-release gate's internals + decision (0 unless built
		 * with -DAEC_SUP_GCAP_ENV_GATE=1). Feed to scratchpad gate tuning on
		 * real-firmware-aligned data. Enable with -DAEC_WAV_DUMP; pipe
		 * stdout to a .csv. */
		{
			struct audio_aec_stats st; size_t tp;
			audio_aec_snapshot(&st, &tp);
			printf("%zu %.6e %.6e %.6e %.6e %.4f %.6e %.6e %.6e %u\n", off,
			       (double)st.sup_sy, (double)st.sup_se,
			       (double)st.p_err, (double)st.p_ref,
			       (double)st.sup_gmean,
			       (double)st.sup_gate_fast, (double)st.sup_gate_mid,
			       (double)st.sup_gate_floor, st.sup_gate_rel);
		}
#endif
	}
	write_wav(argv[3], out, (n / BLK) * BLK);
	return 0;
}

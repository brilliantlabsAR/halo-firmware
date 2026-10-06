/* Replay driver for the worn-calibration harness: runs a recorded (mic,
 * reference) WAV pair through the real firmware audio_aec.c (FDAF build),
 * like ../host/aec_wav.c, plus what the offline scoring needs:
 *  - a runtime tune set (-t key=value,...) applied through the same C API
 *    frame.microphone.aec_tune calls (audio_aec_tune_set), so any set the
 *    device accepts can be replayed without a rebuild
 *  - an explicit speaker-session feed window (-f start,end in samples): the
 *    reference is fed for EVERY block inside it (zeros included), like the
 *    device tap during an open speaker session, and not at all outside it.
 *    Without -f: aec_wav's policy (feed only non-silent blocks)
 *  - a shadow signal (-s near.wav -o near_out.wav) pushed through the same
 *    per-block suppressor kernels: the exact gain the AEC applied to a
 *    near-end-only component (the linear canceller's prediction depends on
 *    the reference alone)
 *  - a per-block dump (-d file.tsv): path, gate release, onset duck, ...
 *  - the effective mic gain relative to gain 1 (-g scale), through
 *    audio_aec_set_mic_gain_scale as the mic stream sets it on the device,
 *    so a capture recorded at another gain replays with the gate keys
 *    scaled as they were on the device (the tune set stays gain-1 values)
 *
 * The shadow and path hooks are not in audio_aec.c: calib_build.py inserts
 * them into a copy of the source (hook_source()) before compiling, so this
 * file only links against a hooked copy. Built with -DREPLAY_NO_TUNE for
 * sources older than frame.microphone.aec_tune (the 0.8.17 reference).
 *
 *   aec_replay [-t k=v,...] [-g scale] [-f s,e] [-s sh.wav -o sh_out.wav] [-d d.tsv] mic.wav ref.wav out.wav
 *   aec_replay -k      list the aec_tune keys: name default min max type
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <halo/audio_aec.h>
#include <max98357a_audio.h>

#define SR  16000
#define BLK 320

/* hooks (calib_build.py hook_source) */
extern const int16_t *host_shadow_in;
extern float host_shadow_out[BLK];
extern int host_shadow_done;
extern int host_last_path; /* 0 passthrough/bypass, 1 first-engage zero, 2 processed */

uint32_t host_uptime_ms; /* backs the host k_uptime_get_32() stub */
void max98357a_audio_set_tx_tap(max98357a_audio_tx_tap_t tap) { (void)tap; }

#ifndef REPLAY_NO_TUNE
static void apply_tune(const char *spec_in)
{
	struct audio_aec_tune t;
	size_t nk;
	const struct audio_aec_tune_key *keys = audio_aec_tune_keys(&nk);
	char *spec = strdup(spec_in);

	audio_aec_tune_get(&t);
	for (char *kv = strtok(spec, ","); kv; kv = strtok(NULL, ",")) {
		char *eq = strchr(kv, '=');
		const struct audio_aec_tune_key *k = NULL;

		if (eq == NULL) {
			fprintf(stderr, "tune: bad item '%s'\n", kv);
			exit(2);
		}
		*eq = '\0';
		for (size_t i = 0; i < nk; i++) {
			if (strcmp(keys[i].name, kv) == 0) {
				k = &keys[i];
			}
		}
		if (k == NULL) {
			fprintf(stderr, "tune: unknown key '%s'\n", kv);
			exit(2);
		}
		uint8_t *f = (uint8_t *)&t + k->offset;

		if (k->type == AUDIO_AEC_TUNE_U32) {
			uint32_t u = (uint32_t)strtod(eq + 1, NULL);

			memcpy(f, &u, sizeof(u));
		} else {
			float v = strtof(eq + 1, NULL);

			memcpy(f, &v, sizeof(v));
		}
	}
	if (audio_aec_tune_set(&t) != 0) {
		fprintf(stderr, "tune: rejected (%s)\n", audio_aec_tune_check(&t));
		exit(2);
	}
	free(spec);
}
#endif

static int16_t *read_wav(const char *path, size_t *n_out)
{
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); exit(1); }
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t *buf = malloc(sz);
	if (fread(buf, 1, sz, f) != (size_t)sz) { perror(path); exit(1); }
	fclose(f);
	long i = 12;
	while (i + 8 <= sz) {
		uint32_t csz = buf[i+4] | (buf[i+5]<<8) | (buf[i+6]<<16) | ((uint32_t)buf[i+7]<<24);
		if (memcmp(buf + i, "data", 4) == 0) {
			size_t n = csz / 2;
			int16_t *s = malloc(n * sizeof(int16_t) + 1);
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
	if (!f) { perror(path); exit(1); }
	uint32_t data = n * 2, riff = 36 + data, sr = SR, br = SR * 2;
	uint8_t h[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',
		16,0,0,0, 1,0, 1,0, 0,0,0,0, 0,0,0,0, 2,0, 16,0, 'd','a','t','a',0,0,0,0};
	memcpy(h+4,&riff,4); memcpy(h+24,&sr,4); memcpy(h+28,&br,4); memcpy(h+40,&data,4);
	fwrite(h,1,44,f); fwrite(s,2,n,f); fclose(f);
}

static int16_t clip16(float v)
{
	return (int16_t)(v > 32767.f ? 32767.f : v < -32768.f ? -32768.f : v);
}

int main(int argc, char **argv)
{
	const char *shp = NULL, *sho = NULL, *dmp = NULL, *tune = NULL, *gscale = NULL;
	long fs = -1, fe = -1;
	int c;

	while ((c = getopt(argc, argv, "kt:g:f:s:o:d:")) != -1) {
		switch (c) {
		case 'k':
#ifndef REPLAY_NO_TUNE
		{
			struct audio_aec_tune t;
			size_t nk;
			const struct audio_aec_tune_key *keys = audio_aec_tune_keys(&nk);

			audio_aec_tune_get(&t);
			for (size_t i = 0; i < nk; i++) {
				const uint8_t *f = (const uint8_t *)&t + keys[i].offset;
				uint32_t u;
				float v;

				if (keys[i].type == AUDIO_AEC_TUNE_U32) {
					memcpy(&u, f, sizeof(u));
					printf("%s %u %g %g u32\n", keys[i].name, (unsigned)u,
					       (double)keys[i].min, (double)keys[i].max);
				} else {
					memcpy(&v, f, sizeof(v));
					printf("%s %.9g %g %g float\n", keys[i].name, (double)v,
					       (double)keys[i].min, (double)keys[i].max);
				}
			}
			return 0;
		}
#else
			return 2;
#endif
		case 't': tune = optarg; break;
		case 'g': gscale = optarg; break;
		case 'f': if (sscanf(optarg, "%ld,%ld", &fs, &fe) != 2) return 2; break;
		case 's': shp = optarg; break;
		case 'o': sho = optarg; break;
		case 'd': dmp = optarg; break;
		default: return 2;
		}
	}
	if (argc - optind < 3) {
		fprintf(stderr, "usage: aec_replay [-t k=v,...] [-g scale] [-f s,e] [-s sh -o sh_out] "
			"[-d dump] mic ref out\n");
		return 2;
	}
	size_t nm, nr, ns = 0;
	int16_t *mic = read_wav(argv[optind], &nm);
	int16_t *ref = read_wav(argv[optind + 1], &nr);
	int16_t *sh = shp ? read_wav(shp, &ns) : NULL;
	size_t n = nm < nr ? nm : nr;

	if (sh && ns < n) {
		n = ns;
	}
	n = (n / BLK) * BLK;

	FILE *df = dmp ? fopen(dmp, "w") : NULL;

	if (df) {
		fprintf(df, "off\tpath\tgate_rel\tonset\tgmean\tp_ref\tp_err\n");
	}
	audio_aec_enable(true);
	if (tune && *tune) {
#ifndef REPLAY_NO_TUNE
		apply_tune(tune);
#else
		fprintf(stderr, "tune: this build has no aec_tune\n");
		return 2;
#endif
	}
	if (gscale) {
#ifndef REPLAY_NO_TUNE
		if (audio_aec_set_mic_gain_scale(strtof(gscale, NULL)) != 0) {
			fprintf(stderr, "gain scale: rejected (%s)\n", gscale);
			return 2;
		}
#else
		fprintf(stderr, "gain scale: this build has no mic gain scale\n");
		return 2;
#endif
	}
	int16_t *out = calloc(n + 1, sizeof(int16_t));
	int16_t *sout = sh ? calloc(n + 1, sizeof(int16_t)) : NULL;
	static const int16_t zero[BLK];

	for (size_t off = 0; off + BLK <= n; off += BLK) {
		host_uptime_ms += 20;
		int feed;

		if (fs >= 0) {
			feed = (long)off >= fs && (long)off < fe;
		} else {
			int64_t e = 0;

			for (int i = 0; i < BLK; i++) {
				e += (int64_t)ref[off + i] * ref[off + i];
			}
			feed = e / BLK > 1;
		}
		if (feed) {
			audio_aec_feed_reference(ref + off, BLK, SR, 1);
		}

		int16_t blk[BLK];

		memcpy(blk, mic + off, sizeof(blk));
		/* the suppressor runs one block behind (hold-back): feed the
		 * shadow's previous block so it lines up with the held content */
		host_shadow_in = sh ? (off >= BLK ? sh + off - BLK : zero) : NULL;
		host_shadow_done = 0;
		audio_aec_process(blk, BLK, SR, 1);
		memcpy(out + off, blk, sizeof(blk));
		if (sh) {
			for (int i = 0; i < BLK; i++) {
				if (host_last_path == 0) {
					sout[off + i] = sh[off + i];
				} else if (host_last_path == 1) {
					sout[off + i] = 0;
				} else if (host_shadow_done) {
					sout[off + i] = clip16(host_shadow_out[i]);
				} else {
					sout[off + i] = host_shadow_in[i];
				}
			}
		}
		if (df) {
			struct audio_aec_stats st;
			size_t tp;

			audio_aec_snapshot(&st, &tp);
			fprintf(df, "%zu\t%d\t%u\t%u\t%.4f\t%.4e\t%.4e\n", off, host_last_path,
				(unsigned)st.sup_gate_rel, (unsigned)st.sup_onset,
				(double)st.sup_gmean, (double)st.p_ref, (double)st.p_err);
		}
	}
	write_wav(argv[optind + 2], out, n);
	if (sh && sho) {
		write_wav(sho, sout, n);
	}
	if (df) {
		fclose(df);
	}
	return 0;
}

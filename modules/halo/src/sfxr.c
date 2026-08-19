/*
 * Copyright (c) 2026 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <halo/mem_manager.h>
#include <halo/pm_manager.h>
#include <halo/sfxr.h>
#if defined(CONFIG_HALO_BATTERY_MANAGER)
#include <halo/battery_manager.h>
#endif
#if defined(CONFIG_HALO_CUE_GATE_DIRTY_BOOT)
#include <halo/file_manager.h>
#endif
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(halo_sfxr, CONFIG_HALO_LOG_LEVEL);

#define SFXR_DEFAULT_SAMPLE_RATE 16000
#define SFXR_DEFAULT_DURATION_MS 1000
#define SFXR_DEFAULT_VOLUME      20
#define SFXR_PCM_CHUNK_BYTES     640
#define SFXR_TAU                 6.28318530717958647692f

/* Shutdown quiesce: while set, non-system playback is rejected and any
 * in-flight non-system playback is cancelled at the next PCM chunk. System
 * sounds (startup/shutdown) are exempt so the shutdown sound can still play. */
static atomic_t sfxr_quiesce_flag;
/* Number of threads currently inside halo_sfxr_play_internal(). */
static atomic_t sfxr_playing_count;

struct sfxr_rng {
	uint32_t state;
};

struct sfxr_state {
	struct halo_sfxr_params params;
	struct sfxr_rng rng;

	float ratio;
	float fperiod;
	float maxperiod;
	float slide;
	float dslide;
	float square_duty;
	float square_slide;
	float chg_mod;
	float fphase;
	float dphase;
	float fltp;
	float fltdp;
	float fltw;
	float fltw_d;
	float fltdmp;
	float fltphp;
	float flthp;
	float flthp_d;
	float vib_phase;
	float vib_speed;
	float vib_amp;
	float phaserbuffer[1024];
	float noisebuffer[32];

	int period;
	int chg_time;
	int chg_limit;
	int phase;
	int env_stage;
	int env_time;
	float env_vol;
	float env_length[3];
	int iphase;
	int ipp;
	int rep_time;
	int rep_limit;
};

static float squaref(float value)
{
	return value * value;
}

static float cubef(float value)
{
	return value * value * value;
}

static int trunc_to_int(float value)
{
	return (int)value;
}

static float clampf_local(float value, float min, float max)
{
	if (value < min) {
		return min;
	}

	if (value > max) {
		return max;
	}

	return value;
}

static int clampi_local(int value, int min, int max)
{
	if (value < min) {
		return min;
	}

	if (value > max) {
		return max;
	}

	return value;
}

static void rng_seed(struct sfxr_rng *rng, uint32_t seed)
{
	rng->state = seed ? seed : 0x6d2b79f5u;

	for (int i = 0; i < 6; i++) {
		uint32_t x = rng->state;

		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		rng->state = x ? x : 0x6d2b79f5u;
	}
}

static uint32_t rng_next(struct sfxr_rng *rng)
{
	uint32_t x = rng->state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	rng->state = x ? x : 0x6d2b79f5u;

	return rng->state;
}

static float rng_unit(struct sfxr_rng *rng)
{
	return (float)(rng_next(rng) >> 8) * (1.0f / 16777216.0f);
}

static float rng_range(struct sfxr_rng *rng, float low, float high)
{
	return low + rng_unit(rng) * (high - low);
}

static bool rng_maybe(struct sfxr_rng *rng, float weight)
{
	if (weight <= 0.0f) {
		return false;
	}

	return trunc_to_int(rng_range(rng, 0.0f, weight)) == 0;
}

void halo_sfxr_reset(struct halo_sfxr_params *params)
{
	if (!params) {
		return;
	}

	memset(params, 0, sizeof(*params));

	params->seed = 1;
	params->supersampling = 8;
	params->waveform = HALO_SFXR_WAVEFORM_SQUARE;
	params->volume.master = 0.5f;
	params->volume.sound = 0.5f;
	params->envelope.sustain = 0.3f;
	params->envelope.decay = 0.4f;
	params->frequency.start = 0.3f;
	params->lowpass.cutoff = 1.0f;
}

static void halo_sfxr_sanitize(struct halo_sfxr_params *params)
{
	params->repeatspeed = clampf_local(params->repeatspeed, 0.0f, 1.0f);
	params->waveform = clampi_local(params->waveform, HALO_SFXR_WAVEFORM_SQUARE,
					HALO_SFXR_WAVEFORM_NOISE);
	params->supersampling = clampi_local(params->supersampling, 1, 16);

	params->envelope.attack = clampf_local(params->envelope.attack, 0.0f, 1.0f);
	params->envelope.sustain = clampf_local(params->envelope.sustain, 0.0f, 1.0f);
	params->envelope.punch = clampf_local(params->envelope.punch, 0.0f, 1.0f);
	params->envelope.decay = clampf_local(params->envelope.decay, 0.0f, 1.0f);

	params->frequency.start = clampf_local(params->frequency.start, 0.0f, 1.0f);
	params->frequency.min = clampf_local(params->frequency.min, 0.0f, 1.0f);
	params->frequency.slide = clampf_local(params->frequency.slide, -1.0f, 1.0f);
	params->frequency.dslide = clampf_local(params->frequency.dslide, -1.0f, 1.0f);

	params->vibrato.depth = clampf_local(params->vibrato.depth, 0.0f, 1.0f);
	params->vibrato.speed = clampf_local(params->vibrato.speed, 0.0f, 1.0f);
	params->vibrato.delay = clampf_local(params->vibrato.delay, 0.0f, 1.0f);

	params->change.amount = clampf_local(params->change.amount, -1.0f, 1.0f);
	params->change.speed = clampf_local(params->change.speed, 0.0f, 1.0f);

	params->duty.ratio = clampf_local(params->duty.ratio, 0.0f, 1.0f);
	params->duty.sweep = clampf_local(params->duty.sweep, -1.0f, 1.0f);

	params->phaser.offset = clampf_local(params->phaser.offset, -1.0f, 1.0f);
	params->phaser.sweep = clampf_local(params->phaser.sweep, -1.0f, 1.0f);

	params->lowpass.cutoff = clampf_local(params->lowpass.cutoff, 0.0f, 1.0f);
	params->lowpass.sweep = clampf_local(params->lowpass.sweep, -1.0f, 1.0f);
	params->lowpass.resonance = clampf_local(params->lowpass.resonance, 0.0f, 1.0f);
	params->highpass.cutoff = clampf_local(params->highpass.cutoff, 0.0f, 1.0f);
	params->highpass.sweep = clampf_local(params->highpass.sweep, -1.0f, 1.0f);
}

static void make_random_pickup(struct halo_sfxr_params *params, struct sfxr_rng *rng)
{
	halo_sfxr_reset(params);
	params->frequency.start = rng_range(rng, 0.4f, 0.9f);
	params->envelope.attack = 0.0f;
	params->envelope.sustain = rng_range(rng, 0.0f, 0.1f);
	params->envelope.punch = rng_range(rng, 0.3f, 0.6f);
	params->envelope.decay = rng_range(rng, 0.1f, 0.5f);

	if (rng_maybe(rng, 1.0f)) {
		params->change.speed = rng_range(rng, 0.5f, 0.7f);
		params->change.amount = rng_range(rng, 0.2f, 0.6f);
	}
}

static void make_random_laser(struct halo_sfxr_params *params, struct sfxr_rng *rng)
{
	halo_sfxr_reset(params);
	params->waveform = trunc_to_int(rng_range(rng, 0.0f, 3.0f));
	if (params->waveform == HALO_SFXR_WAVEFORM_SINE && rng_maybe(rng, 1.0f)) {
		params->waveform = trunc_to_int(rng_range(rng, 0.0f, 1.0f));
	}

	if (rng_maybe(rng, 2.0f)) {
		params->frequency.start = rng_range(rng, 0.3f, 0.9f);
		params->frequency.min = rng_range(rng, 0.0f, 0.1f);
		params->frequency.slide = rng_range(rng, -0.65f, -0.35f);
	} else {
		params->frequency.start = rng_range(rng, 0.5f, 1.0f);
		params->frequency.min =
			MAX(params->frequency.start - rng_range(rng, 0.2f, 0.4f), 0.2f);
		params->frequency.slide = rng_range(rng, -0.35f, -0.15f);
	}

	if (rng_maybe(rng, 1.0f)) {
		params->duty.ratio = rng_range(rng, 0.0f, 0.5f);
		params->duty.sweep = rng_range(rng, 0.0f, 0.2f);
	} else {
		params->duty.ratio = rng_range(rng, 0.4f, 0.9f);
		params->duty.sweep = rng_range(rng, -0.7f, 0.0f);
	}

	params->envelope.attack = 0.0f;
	params->envelope.sustain = rng_range(rng, 0.1f, 0.3f);
	params->envelope.decay = rng_range(rng, 0.0f, 0.4f);

	if (rng_maybe(rng, 1.0f)) {
		params->envelope.punch = rng_range(rng, 0.0f, 0.3f);
	}

	if (rng_maybe(rng, 2.0f)) {
		params->phaser.offset = rng_range(rng, 0.0f, 0.2f);
		params->phaser.sweep = rng_range(rng, -0.2f, 0.0f);
	}

	if (rng_maybe(rng, 1.0f)) {
		params->highpass.cutoff = rng_range(rng, 0.0f, 0.3f);
	}
}

static void make_random_explosion(struct halo_sfxr_params *params, struct sfxr_rng *rng)
{
	halo_sfxr_reset(params);
	params->waveform = HALO_SFXR_WAVEFORM_NOISE;

	if (rng_maybe(rng, 1.0f)) {
		params->frequency.start = rng_range(rng, 0.1f, 0.5f);
		params->frequency.slide = rng_range(rng, -0.1f, 0.3f);
	} else {
		params->frequency.start = rng_range(rng, 0.2f, 0.9f);
		params->frequency.slide = rng_range(rng, -0.2f, -0.4f);
	}
	params->frequency.start = squaref(params->frequency.start);

	if (rng_maybe(rng, 4.0f)) {
		params->frequency.slide = 0.0f;
	}
	if (rng_maybe(rng, 2.0f)) {
		params->repeatspeed = rng_range(rng, 0.3f, 0.8f);
	}

	params->envelope.attack = 0.0f;
	params->envelope.sustain = rng_range(rng, 0.1f, 0.4f);
	params->envelope.punch = rng_range(rng, 0.2f, 0.8f);
	params->envelope.decay = rng_range(rng, 0.0f, 0.5f);

	if (rng_maybe(rng, 1.0f)) {
		params->phaser.offset = rng_range(rng, -0.3f, 0.6f);
		params->phaser.sweep = rng_range(rng, -0.3f, 0.0f);
	}
	if (rng_maybe(rng, 1.0f)) {
		params->vibrato.depth = rng_range(rng, 0.0f, 0.7f);
		params->vibrato.speed = rng_range(rng, 0.0f, 0.6f);
	}
	if (rng_maybe(rng, 2.0f)) {
		params->change.speed = rng_range(rng, 0.6f, 0.9f);
		params->change.amount = rng_range(rng, -0.8f, 0.8f);
	}
}

static void make_random_powerup(struct halo_sfxr_params *params, struct sfxr_rng *rng)
{
	halo_sfxr_reset(params);

	if (rng_maybe(rng, 1.0f)) {
		params->waveform = HALO_SFXR_WAVEFORM_SAWTOOTH;
	} else {
		params->duty.ratio = rng_range(rng, 0.0f, 0.6f);
	}

	if (rng_maybe(rng, 1.0f)) {
		params->frequency.start = rng_range(rng, 0.2f, 0.5f);
		params->frequency.slide = rng_range(rng, 0.1f, 0.5f);
		params->repeatspeed = rng_range(rng, 0.4f, 0.8f);
	} else {
		params->frequency.start = rng_range(rng, 0.2f, 0.5f);
		params->frequency.slide = rng_range(rng, 0.05f, 0.25f);
		if (rng_maybe(rng, 1.0f)) {
			params->vibrato.depth = rng_range(rng, 0.0f, 0.7f);
			params->vibrato.speed = rng_range(rng, 0.0f, 0.6f);
		}
	}

	params->envelope.attack = 0.0f;
	params->envelope.sustain = rng_range(rng, 0.0f, 0.4f);
	params->envelope.decay = rng_range(rng, 0.1f, 0.5f);
}

static void make_random_hit(struct halo_sfxr_params *params, struct sfxr_rng *rng)
{
	halo_sfxr_reset(params);
	params->waveform = trunc_to_int(rng_range(rng, 0.0f, 3.0f));

	if (params->waveform == HALO_SFXR_WAVEFORM_SINE) {
		params->waveform = HALO_SFXR_WAVEFORM_NOISE;
	} else if (params->waveform == HALO_SFXR_WAVEFORM_SQUARE) {
		params->duty.ratio = rng_range(rng, 0.0f, 0.6f);
	}

	params->frequency.start = rng_range(rng, 0.2f, 0.8f);
	params->frequency.slide = rng_range(rng, -0.7f, -0.3f);
	params->envelope.attack = 0.0f;
	params->envelope.sustain = rng_range(rng, 0.0f, 0.1f);
	params->envelope.decay = rng_range(rng, 0.1f, 0.3f);

	if (rng_maybe(rng, 1.0f)) {
		params->highpass.cutoff = rng_range(rng, 0.0f, 0.3f);
	}
}

static void make_random_jump(struct halo_sfxr_params *params, struct sfxr_rng *rng)
{
	halo_sfxr_reset(params);
	params->waveform = HALO_SFXR_WAVEFORM_SQUARE;
	params->duty.ratio = rng_range(rng, 0.0f, 0.6f);
	params->frequency.start = rng_range(rng, 0.3f, 0.6f);
	params->frequency.slide = rng_range(rng, 0.1f, 0.3f);
	params->envelope.attack = 0.0f;
	params->envelope.sustain = rng_range(rng, 0.1f, 0.4f);
	params->envelope.decay = rng_range(rng, 0.1f, 0.3f);

	if (rng_maybe(rng, 1.0f)) {
		params->highpass.cutoff = rng_range(rng, 0.0f, 0.3f);
	}
	if (rng_maybe(rng, 1.0f)) {
		params->lowpass.cutoff = rng_range(rng, 0.4f, 1.0f);
	}
}

static void make_random_blip(struct halo_sfxr_params *params, struct sfxr_rng *rng)
{
	halo_sfxr_reset(params);
	params->waveform = trunc_to_int(rng_range(rng, 0.0f, 2.0f));

	if (params->waveform == HALO_SFXR_WAVEFORM_SQUARE) {
		params->duty.ratio = rng_range(rng, 0.0f, 0.6f);
	}

	params->frequency.start = rng_range(rng, 0.2f, 0.6f);
	params->envelope.attack = 0.0f;
	params->envelope.sustain = rng_range(rng, 0.1f, 0.2f);
	params->envelope.decay = rng_range(rng, 0.0f, 0.2f);
	params->highpass.cutoff = 0.1f;
}

struct preset_name {
	const char *name;
	enum halo_sfxr_preset preset;
};

static const struct preset_name preset_names[] = {
	{"pickup", HALO_SFXR_PRESET_RANDOM_PICKUP},
	{"laser", HALO_SFXR_PRESET_RANDOM_LASER},
	{"explosion", HALO_SFXR_PRESET_RANDOM_EXPLOSION},
	{"powerup", HALO_SFXR_PRESET_RANDOM_POWERUP},
	{"hit", HALO_SFXR_PRESET_RANDOM_HIT},
	{"jump", HALO_SFXR_PRESET_RANDOM_JUMP},
	{"blip", HALO_SFXR_PRESET_RANDOM_BLIP},
};

const char *halo_sfxr_preset_name(enum halo_sfxr_preset preset)
{
	for (size_t i = 0; i < ARRAY_SIZE(preset_names); i++) {
		if (preset_names[i].preset == preset) {
			return preset_names[i].name;
		}
	}

	return NULL;
}

int halo_sfxr_preset_from_name(const char *name, enum halo_sfxr_preset *preset)
{
	if (!name || !preset) {
		return -EINVAL;
	}

	for (size_t i = 0; i < ARRAY_SIZE(preset_names); i++) {
		if (strcmp(name, preset_names[i].name) == 0) {
			*preset = preset_names[i].preset;
			return 0;
		}
	}

	return -ENOENT;
}

int halo_sfxr_make_preset(enum halo_sfxr_preset preset, uint32_t seed,
			  struct halo_sfxr_params *out)
{
	struct sfxr_rng rng;

	if (!out) {
		return -EINVAL;
	}

	rng_seed(&rng, seed);

	switch (preset) {
	case HALO_SFXR_PRESET_RANDOM_PICKUP:
		make_random_pickup(out, &rng);
		break;
	case HALO_SFXR_PRESET_RANDOM_LASER:
		make_random_laser(out, &rng);
		break;
	case HALO_SFXR_PRESET_RANDOM_EXPLOSION:
		make_random_explosion(out, &rng);
		break;
	case HALO_SFXR_PRESET_RANDOM_POWERUP:
		make_random_powerup(out, &rng);
		break;
	case HALO_SFXR_PRESET_RANDOM_HIT:
		make_random_hit(out, &rng);
		break;
	case HALO_SFXR_PRESET_RANDOM_JUMP:
		make_random_jump(out, &rng);
		break;
	case HALO_SFXR_PRESET_RANDOM_BLIP:
		make_random_blip(out, &rng);
		break;
	default:
		return -EINVAL;
	}

	out->seed = seed ? seed : 1;
	halo_sfxr_sanitize(out);

	return 0;
}

static void sfxr_reset_sound(struct sfxr_state *state)
{
	const struct halo_sfxr_params *params = &state->params;

	state->fperiod = 100.0f / (squaref(params->frequency.start) + 0.001f) * state->ratio;
	state->maxperiod = 100.0f / (squaref(params->frequency.min) + 0.001f) * state->ratio;
	state->period = trunc_to_int(state->fperiod);

	state->slide = 1.0f - cubef(params->frequency.slide) * 0.01f;
	state->dslide = -cubef(params->frequency.dslide) * 0.000001f;

	state->square_duty = 0.5f - params->duty.ratio * 0.5f;
	state->square_slide = -params->duty.sweep * 0.00005f;

	if (params->change.amount >= 0.0f) {
		state->chg_mod = 1.0f - squaref(params->change.amount) * 0.9f;
	} else {
		state->chg_mod = 1.0f + squaref(params->change.amount) * 10.0f;
	}

	state->chg_time = 0;
	if (params->change.speed == 1.0f) {
		state->chg_limit = 0;
	} else {
		state->chg_limit =
			trunc_to_int((squaref(1.0f - params->change.speed) * 20000.0f +
				      32.0f) *
				     state->ratio);
	}
}

static void sfxr_state_init(struct sfxr_state *state, const struct halo_sfxr_params *params,
			    int sample_rate)
{
	memset(state, 0, sizeof(*state));
	state->params = *params;
	halo_sfxr_sanitize(&state->params);
	state->ratio = (float)sample_rate / 44100.0f;

	rng_seed(&state->rng, state->params.seed ^ 0xa5a55a5au);

	for (int i = 0; i < ARRAY_SIZE(state->noisebuffer); i++) {
		state->noisebuffer[i] = rng_range(&state->rng, -1.0f, 1.0f);
	}

	sfxr_reset_sound(state);

	state->env_stage = 0;
	state->env_length[0] = squaref(state->params.envelope.attack) * 100000.0f;
	state->env_length[1] = squaref(state->params.envelope.sustain) * 100000.0f;
	state->env_length[2] = squaref(state->params.envelope.decay) * 100000.0f;

	state->fphase = squaref(state->params.phaser.offset) * 1020.0f;
	if (state->params.phaser.offset < 0.0f) {
		state->fphase = -state->fphase;
	}
	state->dphase = squaref(state->params.phaser.sweep);
	if (state->params.phaser.sweep < 0.0f) {
		state->dphase = -state->dphase;
	}
	state->iphase = MIN(abs(trunc_to_int(state->fphase)), 1023);

	state->fltw = cubef(state->params.lowpass.cutoff) * 0.1f;
	state->fltw_d = 1.0f + state->params.lowpass.sweep * 0.0001f;
	state->fltdmp = 5.0f / (1.0f + squaref(state->params.lowpass.resonance) * 20.0f) *
			(0.01f + state->fltw);
	state->fltdmp = MIN(state->fltdmp, 0.8f);
	state->flthp = squaref(state->params.highpass.cutoff) * 0.1f;
	state->flthp_d = 1.0f + state->params.highpass.sweep * 0.0003f;

	state->vib_speed = squaref(state->params.vibrato.speed) * 0.01f;
	state->vib_amp = state->params.vibrato.depth * 0.5f;

	state->rep_limit =
		trunc_to_int((squaref(1.0f - state->params.repeatspeed) * 20000.0f + 32.0f) *
			     state->ratio);
	if (state->params.repeatspeed == 0.0f) {
		state->rep_limit = 0;
	}
}

static bool sfxr_next_sample(struct sfxr_state *state, int16_t *out)
{
	const struct halo_sfxr_params *params = &state->params;
	float rfperiod;
	float ssample = 0.0f;

	state->rep_time++;
	if (state->rep_limit != 0 && state->rep_time >= state->rep_limit) {
		state->rep_time = 0;
		sfxr_reset_sound(state);
	}

	state->chg_time++;
	if (state->chg_limit != 0 && state->chg_time >= state->chg_limit) {
		state->chg_limit = 0;
		state->fperiod *= state->chg_mod;
	}

	state->slide += state->dslide;
	state->fperiod *= state->slide;

	if (state->fperiod > state->maxperiod) {
		state->fperiod = state->maxperiod;
		if (params->frequency.min > 0.0f) {
			return false;
		}
	}

	rfperiod = state->fperiod;
	if (state->vib_amp > 0.0f) {
		state->vib_phase += state->vib_speed;
		rfperiod = state->fperiod * (1.0f + sinf(state->vib_phase) * state->vib_amp);
	}

	state->period = trunc_to_int(rfperiod);
	if (state->period < 8) {
		state->period = 8;
	}

	state->square_duty = clampf_local(state->square_duty + state->square_slide, 0.0f,
					  0.5f);

	state->env_time++;
	if (state->env_time > state->env_length[state->env_stage]) {
		state->env_time = 0;
		state->env_stage++;
		if (state->env_stage >= 3) {
			return false;
		}
	}

	if (state->env_stage == 0) {
		state->env_vol = (state->env_length[0] > 0.0f) ?
					 ((float)state->env_time / state->env_length[0]) :
					 1.0f;
	} else if (state->env_stage == 1) {
		state->env_vol =
			1.0f +
			(1.0f - (float)state->env_time / state->env_length[1]) * 2.0f *
				params->envelope.punch;
	} else {
		state->env_vol =
			1.0f - (float)state->env_time / MAX(state->env_length[2], 1.0f);
	}

	state->fphase += state->dphase;
	state->iphase = MIN(abs(trunc_to_int(state->fphase)), 1023);

	if (state->flthp_d != 0.0f) {
		state->flthp = clampf_local(state->flthp * state->flthp_d, 0.00001f, 0.1f);
	}

	for (int si = 0; si < params->supersampling; si++) {
		float sample = 0.0f;
		float fp;
		float previous_lowpass;

		state->phase++;
		if (state->phase >= state->period) {
			state->phase %= state->period;
			if (params->waveform == HALO_SFXR_WAVEFORM_NOISE) {
				for (int i = 0; i < ARRAY_SIZE(state->noisebuffer); i++) {
					state->noisebuffer[i] =
						rng_range(&state->rng, -1.0f, 1.0f);
				}
			}
		}

		fp = (float)state->phase / (float)state->period;

		switch (params->waveform) {
		case HALO_SFXR_WAVEFORM_SQUARE:
			sample = (fp < state->square_duty) ? 0.5f : -0.5f;
			break;
		case HALO_SFXR_WAVEFORM_SAWTOOTH:
			sample = 1.0f - fp * 2.0f;
			break;
		case HALO_SFXR_WAVEFORM_SINE:
			sample = sinf(fp * SFXR_TAU);
			break;
		case HALO_SFXR_WAVEFORM_NOISE:
			sample = state->noisebuffer[((state->phase * 32) / state->period) & 31];
			break;
		default:
			break;
		}

		previous_lowpass = state->fltp;
		state->fltw = clampf_local(state->fltw * state->fltw_d, 0.0f, 0.1f);
		if (params->lowpass.cutoff != 1.0f) {
			state->fltdp += (sample - state->fltp) * state->fltw;
			state->fltdp -= state->fltdp * state->fltdmp;
		} else {
			state->fltp = sample;
			state->fltdp = 0.0f;
		}
		state->fltp += state->fltdp;

		state->fltphp += state->fltp - previous_lowpass;
		state->fltphp -= state->fltphp * state->flthp;
		sample = state->fltphp;

		state->phaserbuffer[state->ipp & 1023] = sample;
		sample += state->phaserbuffer[(state->ipp - state->iphase + 1024) & 1023];
		state->ipp = (state->ipp + 1) & 1023;

		ssample += sample * state->env_vol;
	}

	ssample = (ssample / (float)params->supersampling) * params->volume.master;
	ssample *= 2.0f * params->volume.sound;
	ssample = clampf_local(ssample, -1.0f, 1.0f);

	*out = (int16_t)trunc_to_int(ssample * 32000.0f);
	return true;
}

static int sfxr_stream_to_speaker(audio_speaker_t *speaker, struct sfxr_state *state,
				  uint8_t *pcm, int sample_rate, int duration_ms,
				  atomic_t *cancel, bool quiesce_cancellable)
{
	const int total_samples = (int)(((int64_t)sample_rate * duration_ms) / 1000);
	const int fade_in_samples = MAX(sample_rate / 200, 1);
	const int fade_out_samples = MAX(sample_rate / 50, 1);
	const size_t samples_per_chunk = SFXR_PCM_CHUNK_BYTES / sizeof(int16_t);
	int sample_index = 0;
	bool finished = false;

	while (sample_index < total_samples) {
		size_t chunk_samples = MIN(samples_per_chunk, (size_t)(total_samples - sample_index));
		size_t generated_samples = 0;

		if (cancel && atomic_get(cancel)) {
			return -ECANCELED;
		}
		if (quiesce_cancellable && atomic_get(&sfxr_quiesce_flag)) {
			return -ECANCELED;
		}

		for (size_t i = 0; i < chunk_samples; i++) {
			int16_t sample = 0;
			float gain = 1.0f;
			int remaining = total_samples - sample_index;

			if (!finished && !sfxr_next_sample(state, &sample)) {
				finished = true;
				break;
			}

			if (sample_index < fade_in_samples) {
				gain *= (float)sample_index / (float)fade_in_samples;
			}
			if (remaining < fade_out_samples) {
				gain *= (float)remaining / (float)fade_out_samples;
			}

			sample = (int16_t)((float)sample * gain);
			sys_put_le16((uint16_t)sample, &pcm[i * sizeof(int16_t)]);
			sample_index++;
			generated_samples++;
		}

		if (generated_samples == 0) {
			break;
		}

		int ret = audio_speaker_write(speaker, pcm, generated_samples * sizeof(int16_t));
		if (ret < 0) {
			return ret;
		}
		if (ret != (int)(generated_samples * sizeof(int16_t))) {
			return -EIO;
		}

		if (finished) {
			break;
		}
	}

	return 0;
}

static int halo_sfxr_play_internal(const struct halo_sfxr_params *params,
				   const struct halo_sfxr_play_options *options,
				   atomic_t *cancel)
{
	struct halo_sfxr_play_options opts = {
		.sample_rate = SFXR_DEFAULT_SAMPLE_RATE,
		.duration_ms = SFXR_DEFAULT_DURATION_MS,
		.volume = SFXR_DEFAULT_VOLUME,
		.owner = AUDIO_OWNER_SYSTEM,
	};
	struct sfxr_state *state = NULL;
	audio_speaker_t *speaker = NULL;
	uint8_t *pcm = NULL;
	int saved_volume = -1;
	bool pm_locked = false;
	int ret = 0;

	if (!params) {
		return -EINVAL;
	}

	if (options) {
		if (options->sample_rate > 0) {
			opts.sample_rate = options->sample_rate;
		}
		if (options->duration_ms > 0) {
			opts.duration_ms = options->duration_ms;
		}
		if (options->volume >= 0) {
			opts.volume = options->volume;
		}
		if (options->owner != AUDIO_OWNER_NONE) {
			opts.owner = options->owner;
		}
	}

	if (opts.sample_rate != 8000 && opts.sample_rate != 16000) {
		return -EINVAL;
	}
	if (opts.duration_ms <= 0 || opts.duration_ms > 10000) {
		return -EINVAL;
	}
	if (opts.volume < 0 || opts.volume > 100) {
		return -EINVAL;
	}

	bool quiesce_cancellable = (opts.owner != AUDIO_OWNER_SYSTEM);

	if (quiesce_cancellable && atomic_get(&sfxr_quiesce_flag)) {
		return -ECANCELED;
	}

	atomic_inc(&sfxr_playing_count);

	state = halo_calloc(1, sizeof(*state), HALO_MEM_REGION_AUTO);
	pcm = halo_malloc(SFXR_PCM_CHUNK_BYTES, HALO_MEM_REGION_AUTO);
	if (!state || !pcm) {
		ret = -ENOMEM;
		goto out;
	}

	sfxr_state_init(state, params, opts.sample_rate);

#if defined(CONFIG_HALO_PM_MANAGER)
	pm_locked = (halo_pm_prevent_sleep() == 0);
#endif

	speaker = audio_speaker_init(opts.sample_rate, 16, 1, opts.owner);
	if (!speaker) {
		ret = -EBUSY;
		goto out;
	}

	saved_volume = audio_speaker_get_volume(speaker);
	ret = audio_speaker_set_volume_transient(speaker, opts.volume);
	if (ret != 0) {
		goto out;
	}

	ret = audio_speaker_start(speaker);
	if (ret != 0) {
		goto out;
	}

	ret = sfxr_stream_to_speaker(speaker, state, pcm, opts.sample_rate, opts.duration_ms,
				     cancel, quiesce_cancellable);

out:
	if (speaker) {
		audio_speaker_stop(speaker);
		if (saved_volume >= 0) {
			(void)audio_speaker_set_volume_transient(speaker, saved_volume);
		}
		audio_speaker_destroy(speaker);
	}

#if defined(CONFIG_HALO_PM_MANAGER)
	if (pm_locked) {
		(void)halo_pm_allow_sleep();
	}
#endif

	halo_free(pcm);
	halo_free(state);

	atomic_dec(&sfxr_playing_count);

	return ret;
}

int halo_sfxr_quiesce(int timeout_ms)
{
	int waited = 0;

	atomic_set(&sfxr_quiesce_flag, 1);
	halo_sfxr_stop_async();

	while (atomic_get(&sfxr_playing_count) > 0 || halo_sfxr_is_async_playing()) {
		if (waited >= timeout_ms) {
			LOG_WRN("SFXR quiesce timed out after %d ms (%ld active)", waited,
				atomic_get(&sfxr_playing_count));
			return -ETIMEDOUT;
		}
		k_sleep(K_MSEC(10));
		waited += 10;
	}

	return 0;
}

void halo_sfxr_quiesce_cancel(void)
{
	atomic_clear(&sfxr_quiesce_flag);
}

int halo_sfxr_play(const struct halo_sfxr_params *params,
		   const struct halo_sfxr_play_options *options)
{
	return halo_sfxr_play_internal(params, options, NULL);
}

int halo_sfxr_play_named(const char *name, uint32_t seed,
			 const struct halo_sfxr_play_options *options)
{
	struct halo_sfxr_params params;
	enum halo_sfxr_preset preset;
	int ret;

	ret = halo_sfxr_preset_from_name(name, &preset);
	if (ret != 0) {
		return ret;
	}

	ret = halo_sfxr_make_preset(preset, seed, &params);
	if (ret != 0) {
		return ret;
	}

	return halo_sfxr_play(&params, options);
}

#if defined(CONFIG_HALO_LUA_SOUND)

struct sfxr_async_request {
	struct halo_sfxr_params params;
	struct halo_sfxr_play_options opts;
};

K_THREAD_STACK_DEFINE(sfxr_async_stack, CONFIG_HALO_SFXR_ASYNC_THREAD_STACK_SIZE);
K_SEM_DEFINE(sfxr_async_sem, 0, 1);
K_MUTEX_DEFINE(sfxr_async_lock);

static struct k_thread sfxr_async_thread;
static struct sfxr_async_request sfxr_async_req;
static atomic_t sfxr_async_cancel;
static atomic_t sfxr_async_active;
static bool sfxr_async_pending;
static bool sfxr_async_started;
static bool sfxr_async_req_system; /* pending/active request is AUDIO_OWNER_SYSTEM */

static void sfxr_async_thread_fn(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (1) {
		struct sfxr_async_request req;
		int ret;

		k_sem_take(&sfxr_async_sem, K_FOREVER);

		k_mutex_lock(&sfxr_async_lock, K_FOREVER);
		if (!sfxr_async_pending) {
			k_mutex_unlock(&sfxr_async_lock);
			continue;
		}
		req = sfxr_async_req;
		sfxr_async_pending = false;
		atomic_clear(&sfxr_async_cancel);
		atomic_set(&sfxr_async_active, 1);
		k_mutex_unlock(&sfxr_async_lock);

		ret = halo_sfxr_play_internal(&req.params, &req.opts, &sfxr_async_cancel);
		if (ret != 0 && ret != -ECANCELED) {
			LOG_WRN("Async SFXR playback failed: %d", ret);
		}

		atomic_set(&sfxr_async_active, 0);
		atomic_clear(&sfxr_async_cancel);
	}
}

static void sfxr_async_start_worker(void)
{
	if (sfxr_async_started) {
		return;
	}

	sfxr_async_started = true;
	k_thread_create(&sfxr_async_thread, sfxr_async_stack,
			K_THREAD_STACK_SIZEOF(sfxr_async_stack), sfxr_async_thread_fn, NULL,
			NULL, NULL, CONFIG_HALO_SFXR_ASYNC_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&sfxr_async_thread, "sfxr_async");
}

int halo_sfxr_play_named_async(const char *name, uint32_t seed,
			       const struct halo_sfxr_play_options *options)
{
	struct halo_sfxr_play_options opts = {
		.sample_rate = SFXR_DEFAULT_SAMPLE_RATE,
		.duration_ms = SFXR_DEFAULT_DURATION_MS,
		.volume = SFXR_DEFAULT_VOLUME,
		.owner = AUDIO_OWNER_LUA,
	};
	struct halo_sfxr_params params;
	enum halo_sfxr_preset preset;
	int ret;

	if (options) {
		opts = *options;
		if (opts.sample_rate <= 0) {
			opts.sample_rate = SFXR_DEFAULT_SAMPLE_RATE;
		}
		if (opts.duration_ms <= 0) {
			opts.duration_ms = SFXR_DEFAULT_DURATION_MS;
		}
		if (opts.volume < 0) {
			opts.volume = SFXR_DEFAULT_VOLUME;
		}
		if (opts.owner == AUDIO_OWNER_NONE) {
			opts.owner = AUDIO_OWNER_LUA;
		}
	}

	if (opts.sample_rate != 8000 && opts.sample_rate != 16000) {
		return -EINVAL;
	}
	if (opts.duration_ms <= 0 || opts.duration_ms > 10000) {
		return -EINVAL;
	}
	if (opts.volume < 0 || opts.volume > 100) {
		return -EINVAL;
	}

	ret = halo_sfxr_preset_from_name(name, &preset);
	if (ret != 0) {
		return ret;
	}

	ret = halo_sfxr_make_preset(preset, seed, &params);
	if (ret != 0) {
		return ret;
	}

	k_mutex_lock(&sfxr_async_lock, K_FOREVER);
	if (sfxr_async_pending || atomic_get(&sfxr_async_active)) {
		k_mutex_unlock(&sfxr_async_lock);
		return -EBUSY;
	}

	sfxr_async_req.params = params;
	sfxr_async_req.opts = opts;
	sfxr_async_req_system = (opts.owner == AUDIO_OWNER_SYSTEM);
	sfxr_async_pending = true;
	sfxr_async_start_worker();
	k_sem_give(&sfxr_async_sem);
	k_mutex_unlock(&sfxr_async_lock);

	return 0;
}

void halo_sfxr_stop_async(void)
{
	k_mutex_lock(&sfxr_async_lock, K_FOREVER);
	sfxr_async_pending = false;
	/* System-owned cues (e.g. the power-off crossing blip) are left to
	 * finish: they are tens of milliseconds long and callers that need
	 * the speaker (deep-sleep quiesce) wait for drain anyway. Cancelling
	 * them here made the cue inaudible whenever the user released the
	 * button promptly at the beep. */
	if (!(atomic_get(&sfxr_async_active) && sfxr_async_req_system)) {
		atomic_set(&sfxr_async_cancel, 1);
	}
	k_mutex_unlock(&sfxr_async_lock);
}

bool halo_sfxr_is_async_playing(void)
{
	return sfxr_async_pending || atomic_get(&sfxr_async_active);
}

#else

int halo_sfxr_play_named_async(const char *name, uint32_t seed,
			       const struct halo_sfxr_play_options *options)
{
	ARG_UNUSED(name);
	ARG_UNUSED(seed);
	ARG_UNUSED(options);

	return -ENOTSUP;
}

void halo_sfxr_stop_async(void)
{
}

bool halo_sfxr_is_async_playing(void)
{
	return false;
}

#endif /* CONFIG_HALO_LUA_SOUND */

#if defined(CONFIG_HALO_STARTUP_SOUND) || defined(CONFIG_HALO_SHUTDOWN_SOUND)

/* Boot/shutdown cue gate: two independent checks decide whether the cues
 * play. Both skip the cue outright — a skipped cue performs no amplifier
 * power-cycle at all, which a lowered volume would not achieve. */

#if defined(CONFIG_HALO_CUE_GATE_DIRTY_BOOT)

#define CUE_GATE_DIRTY_KEY "system/cue_dirty"

#if defined(CONFIG_HALO_STARTUP_SOUND)
/* True when the previous boot did not settle after starting a cue. */
static bool cue_gate_boot_was_dirty(void)
{
	uint8_t dirty = 0;

	(void)halo_settings_get(CUE_GATE_DIRTY_KEY, &dirty, sizeof(dirty));
	return dirty != 0;
}
#endif

static void cue_gate_set_dirty(bool dirty)
{
	uint8_t val = dirty ? 1 : 0;
	int ret = halo_settings_set(CUE_GATE_DIRTY_KEY, &val, sizeof(val));

	if (ret != 0) {
		LOG_WRN("Cue dirty flag write failed: %d", ret);
	}
}

#endif /* CONFIG_HALO_CUE_GATE_DIRTY_BOOT */

/* Fresh under-load battery reading, logged at cue time. Returns false when
 * the cue must be skipped. */
static bool cue_gate_battery_ok(const char *cue)
{
#if defined(CONFIG_HALO_BATTERY_MANAGER)
	(void)halo_battery_update_now();

	int mv = halo_battery_get_voltage();
	int level = halo_battery_get_level();
	bool charging = halo_battery_is_charging();

	LOG_INF("%s cue battery: %d mV, %d%%, %scharging", cue, mv, level,
		charging ? "" : "not ");

#if defined(CONFIG_HALO_CUE_GATE_MIN_VOLTAGE_MV)
	if (CONFIG_HALO_CUE_GATE_MIN_VOLTAGE_MV > 0 && mv >= 0 &&
	    mv < CONFIG_HALO_CUE_GATE_MIN_VOLTAGE_MV) {
		LOG_WRN("%s cue skipped: %d mV under the %d mV floor", cue, mv,
			CONFIG_HALO_CUE_GATE_MIN_VOLTAGE_MV);
		return false;
	}
#endif
#else
	ARG_UNUSED(cue);
#endif
	return true;
}

void halo_sound_cue_boot_settled(void)
{
#if defined(CONFIG_HALO_CUE_GATE_DIRTY_BOOT)
	cue_gate_set_dirty(false);
#endif
}

#else

void halo_sound_cue_boot_settled(void)
{
}

#endif /* CONFIG_HALO_STARTUP_SOUND || CONFIG_HALO_SHUTDOWN_SOUND */

#if defined(CONFIG_HALO_STARTUP_SOUND)

K_THREAD_STACK_DEFINE(startup_sound_stack, CONFIG_HALO_STARTUP_SOUND_THREAD_STACK_SIZE);

static struct k_thread startup_sound_thread;
static bool startup_sound_started;

/* Given once the startup sound has played, failed, or timed out waiting for the
 * speaker, so the boot splash can hold the logo for exactly that long. */
static K_SEM_DEFINE(startup_sound_done, 0, 1);

static void startup_sound_thread_fn(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	const struct halo_sfxr_play_options opts = {
		.sample_rate = SFXR_DEFAULT_SAMPLE_RATE,
		.duration_ms = CONFIG_HALO_STARTUP_SOUND_DURATION_MS,
		.volume = CONFIG_HALO_STARTUP_SOUND_VOLUME,
		.owner = AUDIO_OWNER_SYSTEM,
	};
	int waited = 0;

#if defined(CONFIG_HALO_CUE_GATE_DIRTY_BOOT)
	if (cue_gate_boot_was_dirty()) {
		LOG_WRN("Boot cue skipped: previous boot did not settle");
		k_sem_give(&startup_sound_done);
		return;
	}
#endif
	if (!cue_gate_battery_ok("Boot")) {
		k_sem_give(&startup_sound_done);
		return;
	}
#if defined(CONFIG_HALO_CUE_GATE_DIRTY_BOOT)
	/* Cleared by halo_sound_cue_boot_settled() once boot settles; a boot
	 * that does not get there skips the cues on its next attempt. */
	cue_gate_set_dirty(true);
#endif

	/* No fixed pre-delay: retry until the speaker is actually ready, then
	 * play. The elapsed wait is logged so the real readiness latency is
	 * visible instead of hidden behind a guessed delay. */
	while (true) {
		int ret = halo_sfxr_play_named(CONFIG_HALO_STARTUP_SOUND_PRESET,
					       CONFIG_HALO_STARTUP_SOUND_SEED, &opts);
		if (ret == 0) {
			LOG_INF("Startup sound played (speaker ready after %d ms)", waited);
			break;
		}
		if (ret != -EBUSY && ret != -ENODEV && ret != -EAGAIN) {
			LOG_WRN("Startup sound failed: %d", ret);
			break;
		}
		if (waited >= CONFIG_HALO_STARTUP_SOUND_TIMEOUT_MS) {
			LOG_WRN("Startup sound: speaker not ready after %d ms (last err %d)",
				waited, ret);
			break;
		}
		LOG_DBG("Startup sound: speaker not ready (%d), retrying", ret);
		k_sleep(K_MSEC(CONFIG_HALO_STARTUP_SOUND_RETRY_MS));
		waited += CONFIG_HALO_STARTUP_SOUND_RETRY_MS;
	}

	k_sem_give(&startup_sound_done);
}

void halo_startup_sound_schedule(void)
{
	if (startup_sound_started) {
		return;
	}

	startup_sound_started = true;
	k_thread_create(&startup_sound_thread, startup_sound_stack,
			K_THREAD_STACK_SIZEOF(startup_sound_stack), startup_sound_thread_fn, NULL,
			NULL, NULL, CONFIG_HALO_STARTUP_SOUND_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&startup_sound_thread, "startup_sound");
}

int halo_startup_sound_wait(int timeout_ms)
{
	k_timeout_t t = (timeout_ms < 0) ? K_FOREVER : K_MSEC(timeout_ms);

	return k_sem_take(&startup_sound_done, t) == 0 ? 0 : -EAGAIN;
}

#else

void halo_startup_sound_schedule(void)
{
}

int halo_startup_sound_wait(int timeout_ms)
{
	ARG_UNUSED(timeout_ms);
	return 0;
}

#endif

#if defined(CONFIG_HALO_SHUTDOWN_SOUND)

static int shutdown_sound_try_play(const struct halo_sfxr_play_options *opts)
{
	int waited = 0;

	/* The speaker may still be tearing down from playback that was just
	 * quiesced (or held by LE audio); retry briefly rather than skipping
	 * the sound on the first -EBUSY. */
	while (true) {
		int ret = halo_sfxr_play_named(CONFIG_HALO_SHUTDOWN_SOUND_PRESET,
					       CONFIG_HALO_SHUTDOWN_SOUND_SEED, opts);
		if (ret == 0) {
			return 0;
		}
		if (ret != -EBUSY && ret != -ENODEV && ret != -EAGAIN) {
			LOG_WRN("Shutdown sound failed: %d", ret);
			return ret;
		}
		if (waited >= CONFIG_HALO_SHUTDOWN_SOUND_TIMEOUT_MS) {
			LOG_WRN("Shutdown sound: speaker not free after %d ms (last err %d)",
				waited, ret);
			return ret;
		}
		k_sleep(K_MSEC(50));
		waited += 50;
	}
}

int halo_shutdown_sound_play(void)
{
	const struct halo_sfxr_play_options opts = {
		.sample_rate = SFXR_DEFAULT_SAMPLE_RATE,
		.duration_ms = CONFIG_HALO_SHUTDOWN_SOUND_DURATION_MS,
		.volume = CONFIG_HALO_SHUTDOWN_SOUND_VOLUME,
		.owner = AUDIO_OWNER_SYSTEM,
	};

	if (!cue_gate_battery_ok("Shutdown")) {
		return 0;
	}

#if defined(CONFIG_HALO_CUE_GATE_DIRTY_BOOT)
	/* A power-off that interrupts the shutdown cue must suppress the next
	 * boot's cue too: dirty across the playback, clean once drained. */
	cue_gate_set_dirty(true);
	int ret = shutdown_sound_try_play(&opts);

	cue_gate_set_dirty(false);
	return ret;
#else
	return shutdown_sound_try_play(&opts);
#endif
}

#else

int halo_shutdown_sound_play(void)
{
	return 0;
}

#endif /* CONFIG_HALO_SHUTDOWN_SOUND */

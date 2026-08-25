// Copyright (c) 2025 Brilliant Labs
// SPDX-License-Identifier: Apache-2.0
#define DT_DRV_COMPAT maxim_max98357a

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2s.h>
#include <drivers/i2s_sync.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/audio.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdint.h>
#include "max98357a_audio.h"

#if IS_ENABLED(CONFIG_MAX98357A_AUDIO_PROTECTION_EQ)
#include "speaker_protect.h"
#endif

LOG_MODULE_REGISTER(max98357a_audio, CONFIG_LOG_DEFAULT_LEVEL);

/* New DMA block parameters configurable via Kconfig */
#define MAX98357A_AUDIO_TX_BLOCK_SIZE   CONFIG_MAX98357A_AUDIO_SYNC_BLOCK_SIZE
#define MAX98357A_AUDIO_TX_BLOCK_COUNT  CONFIG_MAX98357A_AUDIO_SYNC_BLOCK_COUNT

/* Memory slab pool used by i2s_sync for DMA-able buffers */
static uint8_t __aligned(4) max98357a_tx_pool[MAX98357A_AUDIO_TX_BLOCK_SIZE * MAX98357A_AUDIO_TX_BLOCK_COUNT];
static struct k_mem_slab max98357a_tx_slab; /* initialized at init */

struct max98357a_audio_data {
	const struct device *i2s_dev;              /* underlying i2s_sync capable device */
	struct k_sem free_block_sem;               /* counts free blocks for producer */
	struct k_sem drain_sem;                    /* signalled when playback drained */
	struct max98357a_audio_stream_cfg config;  /* cached user config */
	enum max98357a_audio_trigger_cmd state;    /* current stream state */
	bool configured;                           /* configuration done flag */
	uint8_t volume;                            /* logical volume (0-100) */
	atomic_t bytes_queued;                     /* queued (pending + inflight) data bytes */
	atomic_t in_flight_blocks;                 /* blocks currently owned by HW */
#if IS_ENABLED(CONFIG_MAX98357A_AUDIO_PROTECTION_EQ)
	struct spk_protect prot;                   /* current-protection DSP chain */
	bool prot_ready;                           /* init succeeded for current config */
#if IS_ENABLED(CONFIG_MAX98357A_AUDIO_PROTECT_TUNING)
	struct max98357a_protect_tuning tune;      /* runtime overrides */
	bool tune_dirty;                           /* apply at next stream start */
#endif
#endif
};

struct max98357a_audio_config {
	const struct device *i2s_dev; /* i2s_sync controller */
	struct gpio_dt_spec sd_mode_gpio;
};

/* Block context stored in pending SW queue */
struct max98357a_block_ctx {
	void *mem;
	size_t len;
};

/* Simple circular queue for pending blocks awaiting submission to hardware.
 * NOTE: current implementation is single-instance (one amplifier) global queue to keep callback
 * signature simple (i2s_sync callback lacks user data). Can be extended with a registry if needed.
 */
#define MAX98357A_QUEUE_LEN MAX98357A_AUDIO_TX_BLOCK_COUNT
static struct max98357a_block_ctx block_queue[MAX98357A_QUEUE_LEN];
static atomic_t queue_head; /* next write */
static atomic_t queue_tail; /* next read */
static struct max98357a_audio_data *g_amp_data; /* single instance pointer */

/* Single-slot TX tap (AEC reference feed); see max98357a_audio.h */
static max98357a_audio_tx_tap_t g_tx_tap;

#if IS_ENABLED(CONFIG_MAX98357A_AUDIO_PROTECTION_EQ)
/* Per-stream digital pre-gain (dB*10, 0 = unity). Set by the stream layer
 * for one playback session; the halo audio_stream layer clears it on every
 * speaker acquisition so it cannot leak into the next owner's stream.
 */
static int g_stream_pregain_db10;

/* Display power state (display-aware budget ducking). Tracked here so the
 * chain can be rebuilt at any time (stream reconfiguration) without asking
 * the display; the display side pushes changes via
 * max98357a_audio_set_display_active().
 */
static bool g_display_active;

static inline int display_budget_drop(void)
{
	return g_display_active ?
	       CONFIG_MAX98357A_AUDIO_PROTECT_DISPLAY_BUDGET_DROP : 0;
}
#endif

/* TX-path diagnostic counters (see max98357a_audio.h): every way the tap
 * feed could diverge from real emission is counted, so a reference-
 * timeline slip can be attributed from session data.
 */
static struct max98357a_audio_tx_diag g_tx_diag;

void max98357a_audio_tx_diag_get(struct max98357a_audio_tx_diag *out)
{
	*out = g_tx_diag;
}

void max98357a_audio_tx_diag_zero(void)
{
	memset(&g_tx_diag, 0, sizeof(g_tx_diag));
}

void max98357a_audio_set_tx_tap(max98357a_audio_tx_tap_t tap)
{
	g_tx_tap = tap;
}

/* Emission-time TX tap: a block is fed to the tap when its DMA COMPLETES
 * (it has just finished emitting), not when it is handed to the DMA. A
 * handoff-time tap leads emission by the DMA queue occupancy - up to the
 * full queue (~160ms) when a self-paced feed keeps it saturated - which
 * wrecks any consumer that models the feed as emission-clocked (the AEC
 * learned an emission epoch ~140ms early and its reference went acausal).
 * Completions arrive in send order, so a small (mem, len) FIFO carries
 * each in-flight block's length to its completion callback.
 */
struct tap_inflight {
	const void *mem;
	size_t len;
};
#define TAP_FIFO_LEN 8
static struct tap_inflight tap_fifo[TAP_FIFO_LEN];
static uint8_t tap_fifo_head, tap_fifo_tail;

static inline void tx_tap_sent(const void *mem, size_t len)
{
	if (g_tx_tap == NULL) {
		return;
	}

	unsigned int key = irq_lock();

	tap_fifo[tap_fifo_head % TAP_FIFO_LEN] =
		(struct tap_inflight){mem, len};
	tap_fifo_head++;
	irq_unlock(key);
}

static inline void tx_tap_completed(struct max98357a_audio_data *data,
				    const void *mem)
{
	max98357a_audio_tx_tap_t tap = g_tx_tap;

	if (tap == NULL || tap_fifo_head == tap_fifo_tail) {
		return;
	}

	struct tap_inflight in = tap_fifo[tap_fifo_tail % TAP_FIFO_LEN];

	tap_fifo_tail++;
	if (in.mem != mem) {
		/* order broke (send failure raced a completion): drop and
		 * let the AEC's gap/resync handling recover
		 */
		g_tx_diag.tap_drops++;
		return;
	}
	tap((const int16_t *)in.mem, in.len / sizeof(int16_t),
	    data->config.sample_rate,
	    data->config.mode == MAX98357A_AUDIO_STEREO ? 2 : 1);
}

static inline bool queue_is_empty(void) {
	return atomic_get(&queue_head) == atomic_get(&queue_tail);
}
static inline bool queue_is_full(void) {
	return ((atomic_get(&queue_head) + 1) % MAX98357A_QUEUE_LEN) == atomic_get(&queue_tail);
}
static void queue_push(struct max98357a_block_ctx ctx) {
	int h = atomic_get(&queue_head);
	block_queue[h] = ctx;
	atomic_set(&queue_head, (h + 1) % MAX98357A_QUEUE_LEN);
}
static struct max98357a_block_ctx queue_pop(void) {
	int t = atomic_get(&queue_tail);
	struct max98357a_block_ctx ctx = block_queue[t];
	atomic_set(&queue_tail, (t + 1) % MAX98357A_QUEUE_LEN);
	return ctx;
}

#if IS_ENABLED(CONFIG_MAX98357A_AUDIO_PROTECT_TUNING)
/* Scalar override helper: -1 = keep the Kconfig default. */
static inline int tune_or(int override, int fallback)
{
	return (override >= 0) ? override : fallback;
}
#endif

#if IS_ENABLED(CONFIG_MAX98357A_AUDIO_PROTECTION_EQ)
static void max98357a_protect_init(struct max98357a_audio_data *data)
{
	struct spk_protect_params params = {
		.sample_rate = (int)data->config.sample_rate,
		.channels = (data->config.mode == MAX98357A_AUDIO_STEREO) ? 2 : 1,
		.hpf_cutoff_hz = CONFIG_MAX98357A_AUDIO_PROTECT_HPF_HZ,
		.budget_percent = CONFIG_MAX98357A_AUDIO_PROTECT_BUDGET_PERCENT,
		.env_ms = CONFIG_MAX98357A_AUDIO_PROTECT_ENV_MS,
		.hold_ms = CONFIG_MAX98357A_AUDIO_PROTECT_HOLD_MS,
		.release_ms = CONFIG_MAX98357A_AUDIO_PROTECT_RELEASE_MS,
		.ramp_ms = CONFIG_MAX98357A_AUDIO_PROTECT_RAMP_MS,
		.bass_drive_percent = CONFIG_MAX98357A_AUDIO_BASS_ENHANCE_PERCENT,
		.peak_cap_percent = CONFIG_MAX98357A_AUDIO_PROTECT_PEAK_CAP_PERCENT,
	};

#if IS_ENABLED(CONFIG_MAX98357A_AUDIO_PROTECT_TUNING)
	params.hpf_cutoff_hz = tune_or(data->tune.hpf_hz, params.hpf_cutoff_hz);
	params.budget_percent = tune_or(data->tune.budget_percent, params.budget_percent);
	params.env_ms = tune_or(data->tune.env_ms, params.env_ms);
	params.hold_ms = tune_or(data->tune.hold_ms, params.hold_ms);
	params.release_ms = tune_or(data->tune.release_ms, params.release_ms);
	params.ramp_ms = tune_or(data->tune.ramp_ms, params.ramp_ms);
	params.bass_drive_percent =
		tune_or(data->tune.bass_percent, params.bass_drive_percent);
	params.peak_cap_percent =
		tune_or(data->tune.peak_percent, params.peak_cap_percent);
#endif

	data->prot_ready = (spk_protect_init(&data->prot, &params) == 0);
	if (!data->prot_ready) {
		LOG_ERR("speaker protection init failed (rate %u ch %d) - "
			"audio will play unprotected",
			data->config.sample_rate, params.channels);
		return;
	}

#if IS_ENABLED(CONFIG_MAX98357A_AUDIO_PROTECT_TUNING)
	if (data->tune.voicing_set) {
		int32_t gains[SPK_PROTECT_BANDS];

		for (int i = 0; i < SPK_PROTECT_BANDS; i++) {
			gains[i] = spk_protect_db10_to_q15(data->tune.voicing_db10[i]);
		}
		spk_protect_set_voicing(&data->prot, gains);
	}
	if (data->tune.weights_set) {
		int32_t weights[SPK_PROTECT_BANDS];

		for (int i = 0; i < SPK_PROTECT_BANDS; i++) {
			weights[i] = (int32_t)(((int64_t)data->tune.weights_x100[i] * 4096) / 100);
		}
		spk_protect_set_weights(&data->prot, weights);
	}
	data->tune_dirty = false;
#endif

	spk_protect_set_budget_drop(&data->prot, display_budget_drop());

	/* Total pre-gain = per-stream gain + bench-tuning override (if any) */
	{
		int pregain_db10 = g_stream_pregain_db10;

#if IS_ENABLED(CONFIG_MAX98357A_AUDIO_PROTECT_TUNING)
		pregain_db10 += data->tune.pregain_db10;
#endif
		if (pregain_db10 != 0) {
			spk_protect_set_pregain(&data->prot,
						spk_protect_db10_to_q15(pregain_db10));
		}
	}
}
#endif

void max98357a_audio_set_display_active(bool active)
{
#if IS_ENABLED(CONFIG_MAX98357A_AUDIO_PROTECTION_EQ)
	g_display_active = active;

	struct max98357a_audio_data *data = g_amp_data;

	if (data && data->prot_ready) {
		spk_protect_set_budget_drop(&data->prot, display_budget_drop());
	}
#else
	ARG_UNUSED(active);
#endif
}

void max98357a_audio_set_stream_pregain(int gain_db10)
{
#if IS_ENABLED(CONFIG_MAX98357A_AUDIO_PROTECTION_EQ)
	if (gain_db10 < 0) {
		gain_db10 = 0;
	}
	if (gain_db10 > 120) {
		gain_db10 = 120;
	}
	g_stream_pregain_db10 = gain_db10;

	/* Apply to the live chain too: the stream layer sets the gain after
	 * the stream has been configured (which rebuilt the chain at unity).
	 */
	struct max98357a_audio_data *data = g_amp_data;

	if (data && data->prot_ready) {
		int total = gain_db10;

#if IS_ENABLED(CONFIG_MAX98357A_AUDIO_PROTECT_TUNING)
		total += data->tune.pregain_db10;
#endif
		spk_protect_set_pregain(&data->prot,
					spk_protect_db10_to_q15(total));
	}
#else
	ARG_UNUSED(gain_db10);
#endif
}

#if IS_ENABLED(CONFIG_MAX98357A_AUDIO_PROTECT_TUNING)
int max98357a_audio_protect_tune(const struct device *dev,
				 const struct max98357a_protect_tuning *tuning)
{
	struct max98357a_audio_data *data = dev->data;

	if (!tuning) {
		return -EINVAL;
	}
	if (tuning->pregain_db10 < -600 || tuning->pregain_db10 > 120) {
		return -EINVAL;
	}
	if (tuning->budget_percent > 100 ||
	    (tuning->budget_percent == 0)) {
		return -EINVAL;
	}

	data->tune = *tuning;

	/* Streams re-init the chain at START; when idle (or never
	 * configured) apply immediately so the next play uses it even if
	 * the stream config is unchanged.
	 */
	if (data->configured && data->state != MAX98357A_AUDIO_TRIGGER_START) {
		max98357a_protect_init(data);
	} else {
		data->tune_dirty = true;
	}

	return 0;
}

int max98357a_audio_protect_get(const struct device *dev,
				struct max98357a_protect_tuning *out)
{
	struct max98357a_audio_data *data = dev->data;

	if (!out) {
		return -EINVAL;
	}
	*out = data->tune;
	out->hpf_hz = tune_or(out->hpf_hz, CONFIG_MAX98357A_AUDIO_PROTECT_HPF_HZ);
	out->budget_percent =
		tune_or(out->budget_percent, CONFIG_MAX98357A_AUDIO_PROTECT_BUDGET_PERCENT);
	out->env_ms = tune_or(out->env_ms, CONFIG_MAX98357A_AUDIO_PROTECT_ENV_MS);
	out->hold_ms = tune_or(out->hold_ms, CONFIG_MAX98357A_AUDIO_PROTECT_HOLD_MS);
	out->release_ms =
		tune_or(out->release_ms, CONFIG_MAX98357A_AUDIO_PROTECT_RELEASE_MS);
	out->ramp_ms = tune_or(out->ramp_ms, CONFIG_MAX98357A_AUDIO_PROTECT_RAMP_MS);
	out->bass_percent =
		tune_or(out->bass_percent, CONFIG_MAX98357A_AUDIO_BASS_ENHANCE_PERCENT);
	out->peak_percent =
		tune_or(out->peak_percent,
			CONFIG_MAX98357A_AUDIO_PROTECT_PEAK_CAP_PERCENT);
	return 0;
}

int max98357a_audio_protect_stats(const struct device *dev,
				  int32_t *min_gain_q15, uint32_t *frames,
				  uint32_t *limited_frames, int32_t *peak_out,
				  uint32_t *peak_capped_frames, bool reset)
{
	struct max98357a_audio_data *data = dev->data;
	struct spk_protect_stats stats;

	if (!data->prot_ready) {
		return -ENODEV;
	}
	spk_protect_stats_read(&data->prot, &stats, reset);
	if (peak_capped_frames) {
		*peak_capped_frames = stats.peak_capped_frames;
	}
	if (min_gain_q15) {
		*min_gain_q15 = stats.min_gain_q15;
	}
	if (frames) {
		*frames = stats.frames;
	}
	if (limited_frames) {
		*limited_frames = stats.limited_frames;
	}
	if (peak_out) {
		*peak_out = stats.peak_out;
	}
	return 0;
}
#endif /* CONFIG_MAX98357A_AUDIO_PROTECT_TUNING */

/* Silence-feed block: while a session is open (START) and an AEC tap is
 * registered, a drained queue is fed this zero block instead of letting
 * emission stop. Emission holes were the AEC's remaining alignment
 * hazard: every hole forces the reference timeline's epoch to be
 * re-learned from ms-quantized callback timestamps, and a per-reply
 * epoch shift of even 1ms (16 samples) rotates the per-bin phase enough
 * to zero the cancellation for that whole reply (host scenario 14's
 * SC14_TRUE_GAPS control reproduces this). With the silence-feed the
 * tap runs continuously from the first real block to session stop, the
 * epoch is established exactly once, and mid-reply BLE starvation
 * becomes literally inaudible instead of a resync.
 */
static int32_t silence_block[MAX98357A_AUDIO_TX_BLOCK_SIZE / sizeof(int32_t)];

static void max98357a_i2s_tx_cb(const struct device *i2s_dev,
								enum i2s_sync_status status,
								void *buffer)
{
	struct max98357a_audio_data *data = g_amp_data; /* single instance */
	if (!data) {
		return;
	}

	if (buffer) {
		/* this block just finished emitting - feed the tap now */
		tx_tap_completed(data, buffer);
		if (buffer != (void *)silence_block) {
			k_mem_slab_free(&max98357a_tx_slab, buffer);
			k_sem_give(&data->free_block_sem);
		}
		atomic_dec(&data->in_flight_blocks);
	}
	if (status != I2S_SYNC_STATUS_OK) {
		g_tx_diag.err_completions++;
		LOG_WRN("i2s_sync tx status %d", status);
	}

	/* Feed next pending block if running or stopping (to drain queue) */
	if ((data->state == MAX98357A_AUDIO_TRIGGER_START ||
	     data->state == MAX98357A_AUDIO_TRIGGER_STOP) && !queue_is_empty()) {
		struct max98357a_block_ctx ctx = queue_pop();
		if (i2s_sync_send(i2s_dev, ctx.mem, ctx.len)) {
			LOG_ERR("i2s_sync_send failed in cb");
			g_tx_diag.cb_send_fails++;
			k_mem_slab_free(&max98357a_tx_slab, ctx.mem);
			k_sem_give(&data->free_block_sem);
		} else {
			tx_tap_sent(ctx.mem, ctx.len);
			g_tx_diag.real_sends++;
			atomic_inc(&data->in_flight_blocks);
		}
	} else if (data->state == MAX98357A_AUDIO_TRIGGER_START &&
		   g_tx_tap != NULL) {
		/* queue drained mid-session: keep emitting (silence) so the
		 * reference timeline never breaks; the loop sustains itself
		 * from this completion until a real block is queued or the
		 * session stops
		 */
		if (i2s_sync_send(i2s_dev, silence_block,
				  MAX98357A_AUDIO_TX_BLOCK_SIZE) == 0) {
			tx_tap_sent(silence_block,
				    MAX98357A_AUDIO_TX_BLOCK_SIZE);
			g_tx_diag.silence_sends++;
			atomic_inc(&data->in_flight_blocks);
		} else {
			g_tx_diag.cb_send_fails++;
		}
	}

	/* Drain detection */
	if (queue_is_empty() && atomic_get(&data->in_flight_blocks) == 0) {
		k_sem_give(&data->drain_sem);
	}
}

/* Work queue & ring buffer removed: i2s_sync callback handles flow control */

static int max98357a_audio_configure_impl(const struct device *dev,
                                          const struct max98357a_audio_stream_cfg *config)
{
	struct max98357a_audio_data *data = dev->data;
	struct i2s_sync_config current;

	if (!config) {
		return -EINVAL;
	}

	/* Validate underlying controller parameters (controller config is static) */
	if (i2s_sync_get_config(data->i2s_dev, &current) == 0) {
		if (current.sample_rate != config->sample_rate ||
		    current.bit_depth != config->bits || current.channel_count !=
		    (config->mode == MAX98357A_AUDIO_STEREO ? 2 : 1)) {
			current.sample_rate = config->sample_rate;
			current.bit_depth = config->bits;
			current.channel_count = config->mode == MAX98357A_AUDIO_STEREO ? 2 : 1;
			i2s_sync_set_config(data->i2s_dev, &current);
		}
	}

	/* Store configuration */
	data->config = *config;
	data->configured = true;
#if IS_ENABLED(CONFIG_MAX98357A_AUDIO_PROTECTION_EQ)
	max98357a_protect_init(data);
#endif

	LOG_DBG("MAX98357A sync configured: %d Hz, %d bits, %s, block %d x %d",
		config->sample_rate, config->bits,
		config->mode == MAX98357A_AUDIO_STEREO ? "stereo" : "mono",
		MAX98357A_AUDIO_TX_BLOCK_SIZE, MAX98357A_AUDIO_TX_BLOCK_COUNT);

	return 0;
}

static int max98357a_audio_set_volume_impl(const struct device *dev, uint8_t volume)
{
	struct max98357a_audio_data *data = dev->data;

	if (volume > 100) {
		return -EINVAL;
	}

	data->volume = volume;

	return 0;
}

static int max98357a_audio_trigger_impl(const struct device *dev,
										enum max98357a_audio_trigger_cmd cmd)
{
	const struct max98357a_audio_config *cfg = dev->config;
	struct max98357a_audio_data *data = dev->data;
	int ret = 0;

	if (!data->configured) {
		return -ENODEV;
	}

	switch (cmd) {
	case MAX98357A_AUDIO_TRIGGER_START:
		if (data->state == MAX98357A_AUDIO_TRIGGER_START) {
			break;
		}

		/* Enable SD_MODE pin */
		if (cfg->sd_mode_gpio.port) {
			gpio_pin_set_dt(&cfg->sd_mode_gpio, 1);
		}
#if IS_ENABLED(CONFIG_MAX98357A_AUDIO_PROTECT_TUNING)
		/* Overrides changed while a stream was active: rebuild the
		 * chain now, before playback begins.
		 */
		if (data->tune_dirty) {
			max98357a_protect_init(data);
		}
#endif
#if IS_ENABLED(CONFIG_MAX98357A_AUDIO_PROTECTION_EQ)
		/* Clear filter state (no stale transient) and restart the
		 * onset ramp to spread turn-on inrush current.
		 */
		if (data->prot_ready) {
			spk_protect_reset(&data->prot);
		}
#endif
		data->state = MAX98357A_AUDIO_TRIGGER_START; /* First write will arm HW */
		break;

	case MAX98357A_AUDIO_TRIGGER_STOP:
		if (data->state == MAX98357A_AUDIO_TRIGGER_STOP) {
			break; /* Already stopped */
		}
		
		/* First, mark as stopping to prevent new data acceptance */
		data->state = MAX98357A_AUDIO_TRIGGER_STOP;
		
		/* Wait for queued blocks to be sent and in-flight blocks to complete */
		int wait_ms = 100; /* Increased timeout for graceful drain */
		while ((!queue_is_empty() || atomic_get(&data->in_flight_blocks) > 0) && wait_ms-- > 0) {
			k_sleep(K_MSEC(1));
		}
		
		if (wait_ms <= 0) {
			LOG_WRN("Timeout waiting for audio drain, forcing stop");
		}
		
		/* Now disable hardware */
		i2s_sync_disable(data->i2s_dev, I2S_DIR_TX);
		
		/* Disable amplifier */
		if (cfg->sd_mode_gpio.port) {
			gpio_pin_set_dt(&cfg->sd_mode_gpio, 0);
		}
		
		/* Clean up any remaining blocks (should be none if drain succeeded) */
		while (!queue_is_empty()) {
			struct max98357a_block_ctx ctx = queue_pop();
			k_mem_slab_free(&max98357a_tx_slab, ctx.mem);
			k_sem_give(&data->free_block_sem);
		}
		
		/* Reset counters */
		atomic_set(&queue_head, 0);
		atomic_set(&queue_tail, 0);
		atomic_set(&data->bytes_queued, 0);
		atomic_set(&data->in_flight_blocks, 0);
		/* drop tap entries whose completions will never arrive */
		tap_fifo_tail = tap_fifo_head;
		break;

	case MAX98357A_AUDIO_TRIGGER_PAUSE:
		data->state = MAX98357A_AUDIO_TRIGGER_PAUSE;
		i2s_sync_disable(data->i2s_dev, I2S_DIR_TX);
		break;

	case MAX98357A_AUDIO_TRIGGER_RESUME:
		if (data->state == MAX98357A_AUDIO_TRIGGER_PAUSE) {
			data->state = MAX98357A_AUDIO_TRIGGER_START;
			if (!queue_is_empty()) {
				struct max98357a_block_ctx ctx = queue_pop();
				if (i2s_sync_send(data->i2s_dev, ctx.mem, ctx.len)) {
					LOG_ERR("Resume send failed");
					k_mem_slab_free(&max98357a_tx_slab, ctx.mem);
					k_sem_give(&data->free_block_sem);
				} else {
					atomic_inc(&data->in_flight_blocks);
				}
			}
		}
		break;

	case MAX98357A_AUDIO_TRIGGER_DRAIN:
		if (queue_is_empty() && atomic_get(&data->in_flight_blocks) == 0) {
			k_sem_give(&data->drain_sem); /* already drained */
		}
		ret = k_sem_take(&data->drain_sem, K_MSEC(5000));
		if (ret != 0) {
			LOG_WRN("Drain timeout");
		}
		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int max98357a_audio_write_impl(const struct device *dev, const void *data,
									  size_t size, size_t *bytes_written,
									  k_timeout_t timeout)
{
	struct max98357a_audio_data *dev_data = dev->data;
	int ret = 0;

	if (!dev_data->configured) {
		return -ENODEV;
	}

	if (!data || size == 0) {
		return -EINVAL;
	}

	/* Reject writes if not in START state */
	if (dev_data->state != MAX98357A_AUDIO_TRIGGER_START) {
		if (bytes_written) {
			*bytes_written = 0;
		}
		return -EIO; /* Device not in active state */
	}

	size_t remaining = size;
	const uint8_t *src = data;
	size_t total_written = 0;
	while (remaining > 0) {
		/* Check state again in loop - might change during write */
		if (dev_data->state != MAX98357A_AUDIO_TRIGGER_START) {
			/* State changed during write, stop accepting more data */
			break;
		}
		
		size_t chunk = MIN(remaining, MAX98357A_AUDIO_TX_BLOCK_SIZE);
		/* wait for a free block token */
		ret = k_sem_take(&dev_data->free_block_sem, timeout);
		if (ret) {
			return -ETIMEDOUT;
		}
		void *mem_block;
		ret = k_mem_slab_alloc(&max98357a_tx_slab, &mem_block, K_NO_WAIT);
		if (ret) {
			k_sem_give(&dev_data->free_block_sem);
			return -ENOMEM;
		}

		/* Volume scaling, then speaker current protection.
		 *
		 * Volume is applied FIRST so the protection chain sees the
		 * absolute output level: its energy budget corresponds to
		 * real transducer drive, engaging only when a battery trip
		 * is physically possible instead of colouring quiet audio.
		 */
		uint8_t vol = dev_data->volume; /* 0..100 */
		if (vol == 0) {
			memset(mem_block, 0, chunk);
		} else {
			int16_t *dst_s = (int16_t *)mem_block;
			const int16_t *src_s = (const int16_t *)src;
			size_t samples = chunk / sizeof(int16_t);

			if (vol == 100) {
				memcpy(mem_block, src, chunk);
			} else {
				uint32_t scale = ((uint32_t)vol << 16) / 100U;

				for (size_t i = 0; i < samples; ++i) {
					dst_s[i] = (int16_t)(((int32_t)src_s[i] *
							      (int32_t)scale) >> 16);
				}
				if ((chunk & 1U) != 0U) {
					((uint8_t *)mem_block)[chunk - 1] =
						src[chunk - 1];
				}
			}

#if IS_ENABLED(CONFIG_MAX98357A_AUDIO_PROTECTION_EQ)
			if (dev_data->prot_ready) {
				spk_protect_process(&dev_data->prot, dst_s, samples);
			}
#endif
		}

		struct max98357a_block_ctx ctx = { .mem = mem_block, .len = chunk };
		bool send_now = false;
		if (dev_data->state == MAX98357A_AUDIO_TRIGGER_START && queue_is_empty() &&
		    atomic_get(&dev_data->in_flight_blocks) == 0) {
			send_now = true; /* first active block */
		}
		if (send_now) {
			if (i2s_sync_send(dev_data->i2s_dev, mem_block, chunk)) {
				LOG_ERR("i2s_sync_send failed");
				k_mem_slab_free(&max98357a_tx_slab, mem_block);
				k_sem_give(&dev_data->free_block_sem);
				return -EIO;
			}
			tx_tap_sent(mem_block, chunk);
			g_tx_diag.real_sends++;
			atomic_inc(&dev_data->in_flight_blocks);
		} else {
			if (queue_is_full()) {
				k_mem_slab_free(&max98357a_tx_slab, mem_block);
				k_sem_give(&dev_data->free_block_sem);
				return -ENOSPC;
			}
			queue_push(ctx);
		}
		src += chunk;
		remaining -= chunk;
		total_written += chunk;
		atomic_add(&dev_data->bytes_queued, chunk);
	}
	if (bytes_written) {
		*bytes_written = total_written;
	}
	return 0;
}

static const struct max98357a_audio_driver_api max98357a_audio_api __maybe_unused = {
	.configure = max98357a_audio_configure_impl,
	.set_volume = max98357a_audio_set_volume_impl,
	.trigger = max98357a_audio_trigger_impl,
	.write = max98357a_audio_write_impl,
};

/* Generic audio interface adaptation functions */
static int max98357a_generic_configure(const struct device *dev,
				       const struct audio_config *config)
{
	struct max98357a_audio_stream_cfg max98357a_config = {
		.sample_rate = config->sample_rate,
		.bits = config->bits_per_sample,
		.mode = (config->channels == AUDIO_STEREO) ? MAX98357A_AUDIO_STEREO : MAX98357A_AUDIO_MONO,
		.is_signed = config->is_signed,
	};

	return max98357a_audio_configure_impl(dev, &max98357a_config);
}

static int max98357a_generic_set_volume(const struct device *dev, uint8_t volume)
{
	return max98357a_audio_set_volume_impl(dev, volume);
}

static int max98357a_generic_trigger(const struct device *dev, enum audio_trigger cmd)
{
	enum max98357a_audio_trigger_cmd max98357a_cmd;

	switch (cmd) {
	case AUDIO_TRIGGER_START:
		max98357a_cmd = MAX98357A_AUDIO_TRIGGER_START;
		break;
	case AUDIO_TRIGGER_STOP:
		max98357a_cmd = MAX98357A_AUDIO_TRIGGER_STOP;
		break;
	case AUDIO_TRIGGER_PAUSE:
		max98357a_cmd = MAX98357A_AUDIO_TRIGGER_PAUSE;
		break;
	case AUDIO_TRIGGER_RESUME:
		max98357a_cmd = MAX98357A_AUDIO_TRIGGER_RESUME;
		break;
	case AUDIO_TRIGGER_DRAIN:
		max98357a_cmd = MAX98357A_AUDIO_TRIGGER_DRAIN;
		break;
	default:
		return -EINVAL;
	}

	return max98357a_audio_trigger_impl(dev, max98357a_cmd);
}

static int max98357a_generic_write(const struct device *dev, const void *data,
				   size_t size, size_t *bytes_written,
				   k_timeout_t timeout)
{
	return max98357a_audio_write_impl(dev, data, size, bytes_written, timeout);
}

static int max98357a_generic_read(const struct device *dev, void *data, size_t size,
				  size_t *bytes_read, k_timeout_t timeout)
{
	/* MAX98357A is output-only */
	return -ENOSYS;
}

static const struct audio_driver_api max98357a_generic_api = {
	.configure = max98357a_generic_configure,
	.set_volume = max98357a_generic_set_volume,
	.trigger = max98357a_generic_trigger,
	.write = max98357a_generic_write,
	.read = max98357a_generic_read,
};

static int max98357a_audio_init(const struct device *dev) 
{
	const struct max98357a_audio_config *cfg = dev->config;
	struct max98357a_audio_data *data = dev->data;
	int ret;

	/* Check I2S device readiness */
	if (!device_is_ready(cfg->i2s_dev)) {
		LOG_ERR("I2S device not ready");
		return -ENODEV;
	}
	data->i2s_dev = cfg->i2s_dev;

	/* Initialize SD_MODE GPIO */
	if (cfg->sd_mode_gpio.port) {
		if (!device_is_ready(cfg->sd_mode_gpio.port)) {
			LOG_ERR("SD_MODE GPIO device not ready");
			return -ENODEV;
		}

		ret = gpio_pin_configure_dt(&cfg->sd_mode_gpio, GPIO_OUTPUT_INACTIVE);
		if (ret != 0) {
			LOG_ERR("Failed to configure SD_MODE GPIO: %d", ret);
			return ret;
		}
	}

	/* Initialize mem slab & flow control */
	k_mem_slab_init(&max98357a_tx_slab, max98357a_tx_pool,
			MAX98357A_AUDIO_TX_BLOCK_SIZE,
			MAX98357A_AUDIO_TX_BLOCK_COUNT);
	k_sem_init(&data->free_block_sem, MAX98357A_AUDIO_TX_BLOCK_COUNT,
		MAX98357A_AUDIO_TX_BLOCK_COUNT);
	k_sem_init(&data->drain_sem, 0, 1);
	atomic_set(&queue_head, 0);
	atomic_set(&queue_tail, 0);
	atomic_set(&data->in_flight_blocks, 0);
	i2s_sync_register_cb(data->i2s_dev, I2S_DIR_TX, max98357a_i2s_tx_cb);
	g_amp_data = data; /* store global */

	data->state = MAX98357A_AUDIO_TRIGGER_STOP;
	data->configured = false;
	data->volume = 50; /* Default volume */
	atomic_set(&data->bytes_queued, 0);
#if IS_ENABLED(CONFIG_MAX98357A_AUDIO_PROTECTION_EQ)
	data->prot_ready = false; /* initialised on first configure */
#if IS_ENABLED(CONFIG_MAX98357A_AUDIO_PROTECT_TUNING)
	/* No overrides at boot: scalars at -1 mean "Kconfig default" (a
	 * zeroed struct would read as hpf_hz=0 = HPF disabled).
	 */
	data->tune.hpf_hz = -1;
	data->tune.budget_percent = -1;
	data->tune.env_ms = -1;
	data->tune.hold_ms = -1;
	data->tune.release_ms = -1;
	data->tune.ramp_ms = -1;
	data->tune.bass_percent = -1;
	data->tune.peak_percent = -1;
#endif
#endif

	LOG_INF("MAX98357A audio driver (sync) initialized; block %d x %d", MAX98357A_AUDIO_TX_BLOCK_SIZE, MAX98357A_AUDIO_TX_BLOCK_COUNT);

	return 0;
}

#define MAX98357A_AUDIO_DEFINE(n)						\
	static struct max98357a_audio_data max98357a_audio_data_##n;		\
										\
	static const struct max98357a_audio_config max98357a_audio_config_##n = { \
		.i2s_dev = DEVICE_DT_GET(DT_INST_PHANDLE(n, i2s)),		\
		.sd_mode_gpio = GPIO_DT_SPEC_INST_GET_OR(n, sd_mode_gpios, {}), \
	};									\
										\
	DEVICE_DT_INST_DEFINE(n, max98357a_audio_init, NULL,\
		      &max98357a_audio_data_##n,\
		      &max98357a_audio_config_##n,\
		      POST_KERNEL, CONFIG_MAX98357A_AUDIO_INIT_PRIORITY, \
		      COND_CODE_1(CONFIG_MAX98357A_AUDIO_GENERIC_API,   \
			   (&max98357a_generic_api), (&max98357a_audio_api)));

DT_INST_FOREACH_STATUS_OKAY(MAX98357A_AUDIO_DEFINE)

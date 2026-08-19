/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ble_audio.c
 * @brief BLE LE Audio Unicast Sink implementation for Halo
 *
 * This file implements LE Audio functionality based on Alif's unicast_sink sample.
 * Key components:
 * - BAP (Basic Audio Profile) Unicast Server (sink + source)
 * - PACS (Published Audio Capabilities Server)
 * - TMAP (Telephony and Media Audio Profile)
 * - CAP (Common Audio Profile) Acceptor / CAS
 * - VCS (Volume Control Service)
 * - MICS (Microphone Control Service) with an AICS gain instance
 *
 * Note: GAF Advertising is NOT used here. Use halo_ble_conn_adv_start() for advertising.
 *
 * Audio Data Flow:
 * 1. Client connects and discovers capabilities (via normal BLE advertising)
 * 2. Client configures codec via ASE (Audio Stream Endpoint)
 * 3. Client establishes CIS (Connected Isochronous Stream)
 * 4. Audio data flows through ISO datapath to I2S output
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/__assert.h>
#include <app_version.h>
#include "alif_ble.h"
#include "gapm.h"
#include "gapm_le_init.h"
#include "gap_le.h"
#include "gapc.h"
#include "gapc_le.h"
#include "gapm_le_adv.h"
#include "gapc_sec.h"
#include "gatt_db.h"
#include "gatt_srv.h"
#include "cap_cas.h"
#include "hap_has.h"
#include "bap_capa_srv.h"
#include "bap_uc_srv.h"
#include "tmap_tmas.h"
#include "hap.h"
#include "arc_vcs.h"
#include "arc_mics.h"
#include "arc_aics.h"
#include "ke_mem.h"

#include "halo/ble_manager.h"
#include "halo/file_manager.h"
#include "halo/ble_connection.h"
#include "halo/ble_service.h"
#include "halo/ble_audio.h"
#include "halo/mem_manager.h"
#include "halo/audio_stream.h"

/* Alif LE Audio datapath components (local copies) */
#include "iso_datapath_ctoh.h"
#include "iso_datapath_htoc.h"
#include "sdu_queue.h"
#include "gapi_isooshm.h"

#include <zephyr/audio/codec.h>

LOG_MODULE_REGISTER(halo_ble_audio, CONFIG_HALO_LOG_LEVEL);

/* ======================================================================================== */
/* Configuration Macros                                                                      */
/* ======================================================================================== */

#define BLE_AUDIO_INIT_MAGIC 0x41454441 /* 'AEDA' */

/**
 * Audio output (Sink/Speaker) configuration based on Kconfig:
 * - CONFIG_HALO_BLE_AUDIO_SINK_ENABLE: Enable/disable sink functionality
 * - CONFIG_HALO_BLE_AUDIO_OUTPUT_STEREO: Stereo (2 channels) or Mono (1 channel)
 */
#ifdef CONFIG_HALO_BLE_AUDIO_SINK_ENABLE
#ifdef CONFIG_HALO_BLE_AUDIO_OUTPUT_STEREO
#define AUDIO_OUTPUT_CHANNELS 2
#define LOCATION_SINK         (GAF_LOC_FRONT_LEFT_BIT | GAF_LOC_FRONT_RIGHT_BIT)
#else
#define AUDIO_OUTPUT_CHANNELS 1
#define LOCATION_SINK         GAF_LOC_FRONT_LEFT_BIT
#endif
#else
#define AUDIO_OUTPUT_CHANNELS 0
#define LOCATION_SINK         0
#endif

/**
 * Audio input (Source/Microphone) configuration based on Kconfig:
 * - CONFIG_HALO_BLE_AUDIO_SOURCE_ENABLE: Enable/disable source functionality
 * - CONFIG_HALO_BLE_AUDIO_INPUT_STEREO: Stereo (2 channels) or Mono (1 channel)
 */
#ifdef CONFIG_HALO_BLE_AUDIO_SOURCE_ENABLE
#ifdef CONFIG_HALO_BLE_AUDIO_INPUT_STEREO
#define AUDIO_INPUT_CHANNELS 2
#define LOCATION_SOURCE      (GAF_LOC_FRONT_LEFT_BIT | GAF_LOC_FRONT_RIGHT_BIT)
#else
#define AUDIO_INPUT_CHANNELS 1
#define LOCATION_SOURCE      GAF_LOC_FRONT_LEFT_BIT
#endif
#else
#define AUDIO_INPUT_CHANNELS 0
#define LOCATION_SOURCE      0
#endif

/**
 * Maximum number of ASE (Audio Stream Endpoints)
 * = Sink ASEs (output channels) + Source ASEs (input channels)
 */
#define MAX_NUMBER_OF_ASE (AUDIO_OUTPUT_CHANNELS + AUDIO_INPUT_CHANNELS)

/* Compile-time check: at least one of sink or source must be enabled */
#if !defined(CONFIG_HALO_BLE_AUDIO_SINK_ENABLE) && !defined(CONFIG_HALO_BLE_AUDIO_SOURCE_ENABLE)
#error "At least one of CONFIG_HALO_BLE_AUDIO_SINK_ENABLE or CONFIG_HALO_BLE_AUDIO_SOURCE_ENABLE must be enabled"
#endif

/** ISO over shared memory data path */
#define DATA_PATH_CONFIG          DATA_PATH_ISOOSHM
/** Number of simultaneous connections */
#define MAX_NUMBER_OF_CONNECTIONS 1
/** Control delay in microseconds */
#define CONTROL_DELAY_US          100

/** Presentation delay configuration (microseconds) */
/* TODO: Adjust based on your audio pipeline latency requirements */
#define MINIMUM_PRESENTATION_DELAY_US 7500
#define MAXIMUM_PRESENTATION_DELAY_US 40000

/** Volume control step size */
#define VOLUME_CTRL_STEP_SIZE 3

/* ======================================================================================== */
/* ASE (Audio Stream Endpoint) State Machine                                                 */
/* ======================================================================================== */
/*
 * ASE State Transitions (BAP Spec):
 *
 *  IDLE --> CODEC_CONFIGURED --> QOS_CONFIGURED --> ENABLING --> STREAMING
 *    ^                                                              |
 *    |                                                              v
 *    +<---------- RELEASING <---------- DISABLING <-----------------+
 *
 * Each state transition is triggered by ASCP (Audio Stream Control Point) operations
 */

/** ASE direction enumeration */
enum ase_direction {
	ASE_DIR_UNKNOWN = 0,
	ASE_DIR_SOURCE, /**< Audio from device to client (microphone) */
	ASE_DIR_SINK,   /**< Audio from client to device (speaker) */
};

/** ASE instance structure - tracks state of each audio stream endpoint */
struct ase_instance {
	struct k_work work;      /**< Work context for async operations */
	uint8_t ase_lid;         /**< ASE local index */
	uint8_t stream_lid;      /**< Stream local index */
	uint8_t dir;             /**< Stream direction: Source or Sink */
	void *p_codec_cfg;       /**< Allocated codec configuration */
	void *p_enable_metadata; /**< Allocated metadata from enable request */
	void *p_update_metadata; /**< Allocated metadata from update_metadata request */
};

/** Audio datapath configuration */
struct audio_datapath_config {
	uint32_t pres_delay_us;      /**< Presentation delay in microseconds */
	uint32_t sampling_rate_hz;   /**< Sampling rate in Hz (16000, 24000, 32000, 48000) */
	bool frame_duration_is_10ms; /**< true = 10ms frame, false = 7.5ms frame */
};

/** ASE configuration structure */
struct ase_config {
	uint8_t nb_ases;                              /**< Number of ASEs */
	uint8_t frame_octet;                          /**< SDU size (octets per codec frame) */
	struct audio_datapath_config datapath_config; /**< Data path configuration */
};

/** PACS record tracking for memory management */
struct pacs_record_info {
	bap_capa_t *p_capa; /**< Pointer to allocated capability structure */
	uint8_t record_id;  /**< Record ID for cleanup */
};

/** Maximum PACS records (sink + source) */
#define MAX_PACS_RECORDS (64)

/* ======================================================================================== */
/* BLE Audio Context                                                                         */
/* ======================================================================================== */

/**
 * @brief BLE Audio context structure
 *
 * This structure maintains the complete state of the LE Audio subsystem.
 * Using __attribute__((noinit)) to preserve state across soft resets.
 */
struct ble_audio_ctx {
	uint32_t initialized; /**< Magic number to detect initialization */
	uint8_t volume;       /**< Current volume level (0-255) */
	bool mute;            /**< Mute state */

	/* Microphone Control (MICS/AICS) state */
	uint8_t mic_mute;       /**< MICS Mute value (see arc_mic_mute) */
	uint8_t mic_input_lid;  /**< AICS input local index for the microphone */
	uint8_t mic_aics_mute;  /**< AICS per-input mute value */

	/* ASE management */
	struct ase_instance ase[MAX_NUMBER_OF_ASE * MAX_NUMBER_OF_CONNECTIONS];
	struct ase_config ase_config_sink; /**< Sink ASE configuration */
	struct ase_config ase_config_src;  /**< Source ASE configuration */
	uint8_t total_ases;                /**< Total number of configured ASEs */

	/* PACS record tracking for memory cleanup */
	struct pacs_record_info pacs_records[MAX_PACS_RECORDS];
	uint8_t pacs_record_count; /**< Number of registered PACS records */
} audio_ctx __attribute__((noinit));

/* ======================================================================================== */
/* Utility Functions                                                                         */
/* ======================================================================================== */

/**
 * @brief Convert BAP sampling frequency enum to Hz
 * @param freq BAP_SAMPLING_FREQ_xxx enum value
 * @return Sampling rate in Hz, or 0 if unknown
 */
static uint32_t audio_bap_sampling_freq_to_hz(uint8_t freq)
{
	switch (freq) {
	case BAP_SAMPLING_FREQ_8000HZ:
		return 8000;
	case BAP_SAMPLING_FREQ_11025HZ:
		return 11025;
	case BAP_SAMPLING_FREQ_16000HZ:
		return 16000;
	case BAP_SAMPLING_FREQ_22050HZ:
		return 22050;
	case BAP_SAMPLING_FREQ_24000HZ:
		return 24000;
	case BAP_SAMPLING_FREQ_32000HZ:
		return 32000;
	case BAP_SAMPLING_FREQ_44100HZ:
		return 44100;
	case BAP_SAMPLING_FREQ_48000HZ:
		return 48000;
	case BAP_SAMPLING_FREQ_88200HZ:
		return 88200;
	case BAP_SAMPLING_FREQ_96000HZ:
		return 96000;
	case BAP_SAMPLING_FREQ_176400HZ:
		return 176400;
	case BAP_SAMPLING_FREQ_192000HZ:
		return 192000;
	case BAP_SAMPLING_FREQ_384000HZ:
		return 384000;
	default:
		return 0;
	}
}

/* ======================================================================================== */
/* ASE Instance Management Functions                                                         */
/* ======================================================================================== */

/**
 * @brief Get array index by ASE local ID
 * @param ase_lid ASE local ID
 * @return Array index or -1 if not found
 */
static int get_ase_index_by_lid(size_t const ase_lid)
{
	if (ase_lid == GAF_INVALID_LID) {
		return -1;
	}

	size_t iter = ARRAY_SIZE(audio_ctx.ase);
	while (iter--) {
		if (audio_ctx.ase[iter].ase_lid == ase_lid) {
			return iter;
		}
	}
	return -1;
}

/**
 * @brief Get ASE context by array index
 * @param ase_index Index in the ASE array
 * @return Pointer to ASE instance or NULL if invalid
 */
static struct ase_instance *get_ase_context_by_idx(size_t const ase_index)
{
	if (ase_index >= ARRAY_SIZE(audio_ctx.ase)) {
		return NULL;
	}
	return &audio_ctx.ase[ase_index];
}

/**
 * @brief Get ASE context by ASE local ID
 * @param ase_lid ASE local ID
 * @return Pointer to ASE instance or NULL if not found
 */
static struct ase_instance *get_ase_context_by_lid(size_t const ase_lid)
{
	int const index = get_ase_index_by_lid(ase_lid);
	if (index < 0) {
		return NULL;
	}
	return &audio_ctx.ase[index];
}

/* ======================================================================================== */
/* Audio Datapath Implementation                                                             */
/* Uses Alif iso_datapath_ctoh for ISO data reception + audio_stream.c for decoding/playback */
/* ======================================================================================== */

/** Maximum number of sink channels (stereo support) */
#define MAX_SINK_CHANNELS 2

/** SDU queue depth (buffers per channel) for the ISO datapath.
 * This is jitter/starvation headroom, not steady-state depth: with the decoder
 * woken per-SDU the queue normally holds 1-2 entries. 128 was ~1.3 s of
 * buffering (and heap) for no benefit; 16 covers the decoder's 100 ms sem
 * fallback plus scheduling jitter (~16 frames at 10 ms) with margin to spare. */
#define SDU_QUEUE_LENGTH 16

/* Forward declarations for cleanup functions */
static int audio_datapath_cleanup_sink(void);
static int audio_datapath_cleanup_source(void);

/**
 * @brief Audio sink channel context
 *
 * Per-channel state for ISO reception and LC3 decoding
 */
struct sink_channel {
	struct iso_datapath_ctoh *iso_dp; /**< ISO datapath from controller */
	struct sdu_queue *sdu_queue;      /**< SDU buffer queue */
	audio_codec_ctx_t *lc3_decoder;   /**< LC3 decoder instance */
	uint8_t stream_lid;               /**< Stream local ID */
	bool enabled;                     /**< Channel is active */
};

/**
 * @brief Audio sink datapath state
 *
 * Manages ISO reception, LC3 decoding, and speaker output
 */
static struct {
	/* Speaker hardware */
	audio_speaker_t *speaker;

	/* Per-channel data */
	struct sink_channel channel[MAX_SINK_CHANNELS];
	uint8_t channel_count;

	/* Configuration */
	uint32_t sampling_rate_hz;
	uint32_t pres_delay_us;
	bool frame_duration_is_10ms;
	size_t octets_per_frame;

	/* Buffers for decoding */
	int16_t *pcm_buffer;      /**< Interleaved output buffer */
	int16_t *channel_buffer;  /**< Single channel temp buffer */
	size_t pcm_frame_samples; /**< Samples per frame per channel */

	/* Decoder thread */
	struct k_thread decoder_thread;
	k_tid_t decoder_tid;
	K_KERNEL_STACK_MEMBER(decoder_stack, 4096);
	volatile bool thread_abort;
	/* Given by the ISO transfer-complete ISR when an SDU is enqueued; the
	 * decoder thread blocks on this instead of polling. */
	struct k_sem data_ready;

	/* State */
	bool initialized;
	bool streaming;
	uint8_t volume;
	bool mute;
} sink_datapath = {
	.speaker = NULL,
	.channel_count = 0,
	.initialized = false,
	.streaming = false,
	.thread_abort = false,
	.volume = 64,
	.mute = false,
};

/* ======================================================================================== */
/* Audio Source Datapath (Microphone -> BLE)                                                */
/* ======================================================================================== */

/** Maximum number of source channels (stereo support) */
#define MAX_SOURCE_CHANNELS 2

/**
 * @brief Audio source channel context
 *
 * Per-channel state for microphone capture and LC3 encoding
 */
struct source_channel {
	struct iso_datapath_htoc *iso_dp; /**< ISO datapath to controller */
	struct sdu_queue *sdu_queue;      /**< SDU buffer queue */
	audio_codec_ctx_t *lc3_encoder;   /**< LC3 encoder instance */
	uint8_t stream_lid;               /**< Stream local ID */
	bool enabled;                     /**< Channel is active */
};

/**
 * @brief Audio source datapath state
 *
 * Manages microphone capture, LC3 encoding, and ISO transmission
 */
static struct {
	/* Microphone hardware */
	audio_microphone_t *microphone;

	/* Per-channel data */
	struct source_channel channel[MAX_SOURCE_CHANNELS];
	uint8_t channel_count;

	/* Configuration */
	uint32_t sampling_rate_hz;
	uint32_t pres_delay_us;
	bool frame_duration_is_10ms;
	size_t octets_per_frame;

	/* Buffers for encoding */
	int16_t *pcm_buffer;      /**< Interleaved input buffer */
	int16_t *channel_buffer;  /**< Single channel temp buffer for de-interleaving */
	size_t pcm_frame_samples; /**< Samples per frame per channel */

	/* Encoder thread */
	struct k_thread encoder_thread;
	k_tid_t encoder_tid;
	K_KERNEL_STACK_MEMBER(encoder_stack, 8192);
	volatile bool thread_abort;

	/* Sequence number for SDUs */
	uint16_t sdu_seq_num;

	/* State */
	bool initialized;
	bool streaming;
} source_datapath = {
	.microphone = NULL,
	.channel_count = 0,
	.initialized = false,
	.streaming = false,
	.thread_abort = false,
	.sdu_seq_num = 0,
};

/**
 * @brief Decoder thread function
 *
 * Continuously reads SDUs from ISO datapath, decodes LC3, and writes to speaker
 */
static void sink_decoder_thread_func(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	gapi_isooshm_sdu_buf_t *p_sdu;
	int ret;

	while (!sink_datapath.thread_abort) {
		/* Block until the ISO transfer-complete ISR signals a new SDU, rather
		 * than spinning at ~2 kHz. The bounded fallback keeps thread_abort
		 * observed promptly at teardown and drains any SDU that raced the
		 * signal; the K_NO_WAIT msgq reads below remain the authoritative
		 * drain, so the exact semaphore count never gates correctness. */
		k_sem_take(&sink_datapath.data_ready, K_MSEC(100));

		bool got_data = false;

		/* Try to get SDU from each enabled channel */
		for (int ch = 0; ch < sink_datapath.channel_count; ch++) {
			struct sink_channel *channel = &sink_datapath.channel[ch];
			if (!channel->enabled || !channel->sdu_queue || !channel->lc3_decoder) {
				continue;
			}

			/* Get SDU from queue (non-blocking) */
			p_sdu = NULL;
			ret = k_msgq_get(&channel->sdu_queue->msgq, &p_sdu, K_NO_WAIT);

			if (ret || !p_sdu) {
				continue;
			}

			if (ret || !p_sdu) {
				continue;
			}
			if (!p_sdu->sdu_len) {
				/* Empty SDU, skip. Capture metadata before the free —
				 * reading p_sdu after k_mem_slab_free is a use-after-free. */
				uint32_t const timestamp = p_sdu->timestamp;
				uint16_t const seq_num = p_sdu->seq_num;
				k_mem_slab_free(&channel->sdu_queue->slab, p_sdu);
				iso_datapath_ctoh_notify_sdu_done(channel->iso_dp, timestamp,
								  seq_num);
				continue;
			}

			got_data = true;

			/* Clamp the controller-reported sdu_len to the buffer we
			 * actually allocated (payload_size). A rogue/oversized length
			 * would make the LC3 decoder read past the end of p_sdu->data. */
			if (p_sdu->sdu_len > channel->sdu_queue->payload_size) {
				LOG_WRN("ch%d sdu_len %u > payload %zu, clamping", ch,
					p_sdu->sdu_len, channel->sdu_queue->payload_size);
				p_sdu->sdu_len = channel->sdu_queue->payload_size;
			}

			/* Decode LC3 frame */
			bool bad_frame = (p_sdu->status != GAPI_ISOOSHM_SDU_STATUS_VALID);

			if (!bad_frame) {
				ret = audio_lc3_decode_frame(
					channel->lc3_decoder, p_sdu->data, p_sdu->sdu_len,
					sink_datapath.channel_buffer,
					sink_datapath.pcm_frame_samples * sizeof(int16_t));

				if (ret != 0) {
					LOG_WRN("LC3 decode failed for ch%d: %d", ch, ret);
					bad_frame = true;
				}
			}

			if (bad_frame) {
				/* Fill with silence on decode error */
				memset(sink_datapath.channel_buffer, 0,
				       sink_datapath.pcm_frame_samples * sizeof(int16_t));
			}

#if AUDIO_OUTPUT_CHANNELS == 2
			/* Stereo output: interleave mono to both L/R channels */
			for (size_t i = 0; i < sink_datapath.pcm_frame_samples; i++) {
				sink_datapath.pcm_buffer[i * 2] = sink_datapath.channel_buffer[i];
				sink_datapath.pcm_buffer[i * 2 + 1] =
					sink_datapath.channel_buffer[i];
			}
#else
			/* Mono output: direct copy */
			memcpy(sink_datapath.pcm_buffer, sink_datapath.channel_buffer,
			       sink_datapath.pcm_frame_samples * sizeof(int16_t));
#endif

			/* Capture SDU metadata before the free — reading p_sdu after
			 * k_mem_slab_free is a use-after-free. */
			uint32_t const timestamp = p_sdu->timestamp;
			uint16_t const seq_num = p_sdu->seq_num;

			/* Free SDU buffer */
			k_mem_slab_free(&channel->sdu_queue->slab, p_sdu);

			/* Notify ISO datapath that we processed an SDU */
			iso_datapath_ctoh_notify_sdu_done(channel->iso_dp, timestamp, seq_num);
		}

		/* Write to speaker if we got data */
		if (got_data && sink_datapath.speaker && sink_datapath.streaming) {
			size_t pcm_bytes = sink_datapath.pcm_frame_samples * AUDIO_OUTPUT_CHANNELS *
					   sizeof(int16_t);
			audio_speaker_write(sink_datapath.speaker,
					    (uint8_t *)sink_datapath.pcm_buffer, pcm_bytes);
		}
	}
}

/**
 * @brief Encoder thread function
 *
 * Continuously reads PCM from microphone, encodes LC3, and transmits via ISO datapath
 */
static void source_encoder_thread_func(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int ret;
	void *mic_buffer = NULL;
	size_t mic_buffer_len = 0;
	uint32_t timestamp = 0;

	while (!source_datapath.thread_abort) {
		if (!source_datapath.streaming || !source_datapath.microphone) {
			k_sleep(K_MSEC(10));
			continue;
		}

		/* Safety check: ensure channel_count is valid before processing */
		if (source_datapath.channel_count == 0) {
			LOG_ERR("Invalid channel count (0), aborting encoder thread");
			break;
		}

		/* Read PCM data from microphone */
		ret = audio_microphone_read(source_datapath.microphone, &mic_buffer,
					    &mic_buffer_len);
		if (ret != 0 || !mic_buffer) {
			/* -EAGAIN is a normal read timeout while the stream is
			 * starting up or being torn down - not worth a warning */
			if (ret != -EAGAIN) {
				LOG_WRN("Microphone read failed: %d", ret);
			}
			k_sleep(K_MSEC(1));
			continue;
		}

		/* MICS/AICS mute: keep the ISO stream cadence but transmit silence,
		 * as required by the Microphone Control Profile */
		if (audio_ctx.mic_mute == ARC_MIC_MUTE_MUTED ||
		    audio_ctx.mic_aics_mute != ARC_AIC_MUTE_NOT_MUTED) {
			memset(mic_buffer, 0, mic_buffer_len);
		}

		int16_t *pcm = (int16_t *)mic_buffer;
		size_t samples_total = mic_buffer_len / sizeof(int16_t);
		size_t samples_per_frame = source_datapath.pcm_frame_samples;
		size_t frame_samples_total = samples_per_frame * source_datapath.channel_count;

		/* Validate octets_per_frame before encoding */
		if (source_datapath.octets_per_frame == 0) {
			/* Not yet configured, skip this frame */
			audio_microphone_free_buffer(source_datapath.microphone, mic_buffer);
			continue;
		}

		/* Process all frames in the buffer (microphone may return multiple frames) */
		for (size_t frame_offset = 0; frame_offset < samples_total;
		     frame_offset += frame_samples_total) {
			/* Check if we have a complete frame */
			if (frame_offset + frame_samples_total > samples_total) {
				break; /* Incomplete frame, skip */
			}

			/* Process each enabled channel */
			for (int ch = 0; ch < source_datapath.channel_count; ch++) {
				struct source_channel *channel = &source_datapath.channel[ch];
				if (!channel->enabled || !channel->sdu_queue ||
				    !channel->lc3_encoder) {
					continue;
				}

				/* De-interleave channel from PCM buffer */
				for (size_t i = 0; i < samples_per_frame; i++) {
					source_datapath.channel_buffer[i] =
						pcm[frame_offset + ch +
						    i * source_datapath.channel_count];
				}

				/* Allocate SDU buffer */
				gapi_isooshm_sdu_buf_t *p_sdu = NULL;
				ret = k_mem_slab_alloc(&channel->sdu_queue->slab, (void **)&p_sdu,
						       K_NO_WAIT);
				if (ret != 0 || !p_sdu) {
					LOG_WRN("SDU alloc failed for ch%d: %d", ch, ret);
					continue;
				}

				/* Encode LC3 frame */
				ret = audio_lc3_encode_frame(
					channel->lc3_encoder, source_datapath.channel_buffer,
					samples_per_frame * sizeof(int16_t), p_sdu->data,
					source_datapath.octets_per_frame);

				if (ret != 0) {
					LOG_WRN("LC3 encode failed for ch%d: %d", ch, ret);
					k_mem_slab_free(&channel->sdu_queue->slab, p_sdu);
					continue;
				}

				/* Set SDU metadata */
				p_sdu->sdu_len = source_datapath.octets_per_frame;
				p_sdu->timestamp = timestamp;
				p_sdu->seq_num = source_datapath.sdu_seq_num;
				p_sdu->status = GAPI_ISOOSHM_SDU_STATUS_VALID;

				/* Put SDU to queue */
				ret = k_msgq_put(&channel->sdu_queue->msgq, &p_sdu, K_NO_WAIT);
				if (ret != 0) {
					LOG_WRN("SDU queue full for ch%d", ch);
					k_mem_slab_free(&channel->sdu_queue->slab, p_sdu);
					continue;
				}

				/* Notify ISO datapath that SDU is ready */
				iso_datapath_htoc_notify_sdu_available(channel->iso_dp, timestamp,
								       source_datapath.sdu_seq_num);
			}

			/* Increment sequence number and timestamp for next frame */
			source_datapath.sdu_seq_num++;
			/* Timestamp increments by frame duration in microseconds */
			timestamp += source_datapath.frame_duration_is_10ms ? 10000 : 7500;
		}

		/* Free microphone buffer */
		audio_microphone_free_buffer(source_datapath.microphone, mic_buffer);
	}
}

/**
 * @brief Create audio sink datapath (BLE -> Speaker)
 *
 * Initialize ISO datapath and speaker hardware for audio playback.
 *
 * @param cfg Datapath configuration (sample rate, frame duration, presentation delay)
 * @return 0 on success, negative error code on failure
 */
static int audio_datapath_create_sink(struct audio_datapath_config const *const cfg)
{
	if (!cfg) {
		return -EINVAL;
	}

	/* Clean up if already initialized */
	if (sink_datapath.initialized) {
		audio_datapath_cleanup_sink();
	}

	/* Store configuration */
	sink_datapath.sampling_rate_hz = cfg->sampling_rate_hz;
	sink_datapath.pres_delay_us = cfg->pres_delay_us;
	sink_datapath.frame_duration_is_10ms = cfg->frame_duration_is_10ms;

	/* Determine channel count from LOCATION_SINK */
	sink_datapath.channel_count = __builtin_popcount(LOCATION_SINK);
	if (sink_datapath.channel_count > MAX_SINK_CHANNELS) {
		sink_datapath.channel_count = MAX_SINK_CHANNELS;
	}

	/* Calculate samples per frame */
	uint32_t frame_duration_us = cfg->frame_duration_is_10ms ? 10000 : 7500;
	sink_datapath.pcm_frame_samples = (cfg->sampling_rate_hz * frame_duration_us) / 1000000;

	/* Allocate PCM buffers */
	size_t pcm_buffer_size =
		sink_datapath.pcm_frame_samples * AUDIO_OUTPUT_CHANNELS * sizeof(int16_t);
	sink_datapath.pcm_buffer = halo_malloc(pcm_buffer_size, HALO_MEM_REGION_AUTO);
	if (!sink_datapath.pcm_buffer) {
		LOG_ERR("Failed to allocate PCM buffer");
		return -ENOMEM;
	}

	sink_datapath.channel_buffer = halo_malloc(
		sink_datapath.pcm_frame_samples * sizeof(int16_t), HALO_MEM_REGION_AUTO);
	if (!sink_datapath.channel_buffer) {
		LOG_ERR("Failed to allocate channel buffer");
		halo_free(sink_datapath.pcm_buffer);
		sink_datapath.pcm_buffer = NULL;
		return -ENOMEM;
	}

	/* Initialize channel state */
	for (int ch = 0; ch < MAX_SINK_CHANNELS; ch++) {
		sink_datapath.channel[ch].iso_dp = NULL;
		sink_datapath.channel[ch].sdu_queue = NULL;
		sink_datapath.channel[ch].lc3_decoder = NULL;
		sink_datapath.channel[ch].stream_lid = 0xFF;
		sink_datapath.channel[ch].enabled = false;
	}

	/* Initialize speaker with configured output channels */
	sink_datapath.speaker = audio_speaker_init(cfg->sampling_rate_hz, 16, AUDIO_OUTPUT_CHANNELS,
						   AUDIO_OWNER_LE_AUDIO);
	if (!sink_datapath.speaker) {
		LOG_ERR("Failed to initialize speaker");
		halo_free(sink_datapath.pcm_buffer);
		halo_free(sink_datapath.channel_buffer);
		sink_datapath.pcm_buffer = NULL;
		sink_datapath.channel_buffer = NULL;
		return -EIO;
	}

	/* Apply initial volume */
	int volume_percent = sink_datapath.mute ? 0 : (sink_datapath.volume * 100 / 127);

	audio_speaker_set_volume(sink_datapath.speaker, volume_percent);

	/* Create decoder thread. Init the wake semaphore first (counting, so a
	 * burst of enqueues is not collapsed) and reset any stale count from a
	 * prior stream session. */
	k_sem_init(&sink_datapath.data_ready, 0, K_SEM_MAX_LIMIT);
	sink_datapath.thread_abort = false;
	sink_datapath.decoder_tid = k_thread_create(
		&sink_datapath.decoder_thread, sink_datapath.decoder_stack,
		K_KERNEL_STACK_SIZEOF(sink_datapath.decoder_stack), sink_decoder_thread_func, NULL,
		NULL, NULL, K_PRIO_PREEMPT(5), 0, K_NO_WAIT);

	if (!sink_datapath.decoder_tid) {
		LOG_ERR("Failed to create decoder thread");
		audio_speaker_destroy(sink_datapath.speaker);
		sink_datapath.speaker = NULL;
		halo_free(sink_datapath.pcm_buffer);
		halo_free(sink_datapath.channel_buffer);
		sink_datapath.pcm_buffer = NULL;
		sink_datapath.channel_buffer = NULL;
		return -ENOMEM;
	}

	k_thread_name_set(sink_datapath.decoder_tid, "ble_audio_dec");

	sink_datapath.initialized = true;

	return 0;
}

/**
 * @brief Create audio sink channel for a specific stream
 *
 * Create SDU queue, ISO datapath, and LC3 decoder for the channel.
 *
 * @param octets_per_frame LC3 frame size in octets
 * @param stream_lid Stream local ID
 * @return 0 on success, negative error code on failure
 */
static int audio_datapath_channel_create_sink(size_t const octets_per_frame,
					      uint8_t const stream_lid)
{

	if (!sink_datapath.initialized) {
		LOG_ERR("Sink datapath not initialized");
		return -EINVAL;
	}

	/* Find free channel slot */
	int ch_idx = -1;
	for (int i = 0; i < sink_datapath.channel_count; i++) {
		if (sink_datapath.channel[i].stream_lid == 0xFF) {
			ch_idx = i;
			break;
		}
	}

	if (ch_idx < 0) {
		LOG_ERR("No free channel slot");
		return -ENOMEM;
	}

	struct sink_channel *channel = &sink_datapath.channel[ch_idx];

	/* Store frame size */
	sink_datapath.octets_per_frame = octets_per_frame;
	channel->stream_lid = stream_lid;

	/* Create SDU queue */
	channel->sdu_queue = sdu_queue_create(SDU_QUEUE_LENGTH, octets_per_frame);
	if (!channel->sdu_queue) {
		LOG_ERR("Failed to create SDU queue");
		return -ENOMEM;
	}

	/* Let the ISO ISR wake the decoder thread when this queue gets an SDU. */
	channel->sdu_queue->data_ready = &sink_datapath.data_ready;

	/* Create ISO datapath (but don't bind/start yet) */
	channel->iso_dp = iso_datapath_ctoh_init(stream_lid, channel->sdu_queue);
	if (!channel->iso_dp) {
		LOG_ERR("Failed to create ISO datapath");
		sdu_queue_delete(channel->sdu_queue);
		channel->sdu_queue = NULL;
		return -ENOMEM;
	}

	/* Create LC3 decoder */
	int lc3_duration = sink_datapath.frame_duration_is_10ms ? 1000 : 750;
	int lc3_bitrate;
	if (sink_datapath.frame_duration_is_10ms) {
		lc3_bitrate = octets_per_frame * 800;
	} else {
		lc3_bitrate = (octets_per_frame * 8 * 1000000) / 7500;
	}

	channel->lc3_decoder =
		audio_lc3_decoder_create(sink_datapath.sampling_rate_hz, lc3_duration, lc3_bitrate);

	if (!channel->lc3_decoder) {
		LOG_ERR("Failed to create LC3 decoder");
		iso_datapath_ctoh_delete(channel->iso_dp);
		sdu_queue_delete(channel->sdu_queue);
		channel->iso_dp = NULL;
		channel->sdu_queue = NULL;
		return -ENOMEM;
	}

	return 0;
}

/**
 * @brief Start audio sink channel streaming
 *
 * Start ISO datapath and speaker for the channel.
 *
 * @param stream_lid Stream local ID
 * @return 0 on success, negative error code on failure
 */
static int audio_datapath_channel_start_sink(uint8_t const stream_lid)
{
	if (!sink_datapath.initialized) {
		LOG_ERR("Sink not init");
		return -EINVAL;
	}

	/* Find channel by stream_lid */
	struct sink_channel *channel = NULL;
	for (int i = 0; i < sink_datapath.channel_count; i++) {
		if (sink_datapath.channel[i].stream_lid == stream_lid) {
			channel = &sink_datapath.channel[i];
			break;
		}
	}

	if (!channel || !channel->iso_dp) {
		LOG_ERR("Channel not found for stream_lid %u", stream_lid);
		return -EINVAL;
	}

	/* Start ISO datapath */
	int ret = iso_datapath_ctoh_start(channel->iso_dp);
	if (ret) {
		LOG_ERR("Failed to start ISO datapath: %d", ret);
		return ret;
	}

	channel->enabled = true;

	/* Start speaker if not already started */
	if (!sink_datapath.streaming && sink_datapath.speaker) {
		ret = audio_speaker_start(sink_datapath.speaker);
		if (ret) {
			LOG_ERR("Failed to start speaker: %d", ret);
			return ret;
		}

		sink_datapath.streaming = true;
	}

	return 0;
}

/**
 * @brief Stop audio sink channel streaming
 *
 * Stop ISO datapath for the channel.
 *
 * @param stream_lid Stream local ID
 * @return 0 on success, negative error code on failure
 */
static int audio_datapath_channel_stop_sink(uint8_t const stream_lid)
{
	/* Find channel by stream_lid */
	struct sink_channel *channel = NULL;
	for (int i = 0; i < sink_datapath.channel_count; i++) {
		if (sink_datapath.channel[i].stream_lid == stream_lid) {
			channel = &sink_datapath.channel[i];
			break;
		}
	}

	if (!channel) {
		return 0;
	}

	channel->enabled = false;

	/* Stop ISO datapath */
	if (channel->iso_dp) {
		iso_datapath_ctoh_stop(channel->iso_dp);
	}

	/* Check if any channel is still active */
	bool any_active = false;
	for (int i = 0; i < sink_datapath.channel_count; i++) {
		if (sink_datapath.channel[i].enabled) {
			any_active = true;
			break;
		}
	}

	/* Stop speaker if no active channels */
	if (!any_active && sink_datapath.streaming) {
		if (sink_datapath.speaker) {
			audio_speaker_stop(sink_datapath.speaker);
		}

		sink_datapath.streaming = false;
	}

	return 0;
}

/**
 * @brief Clean up audio sink datapath
 *
 * Release all sink datapath resources.
 */
static int audio_datapath_cleanup_sink(void)
{
	if (!sink_datapath.initialized) {
		return 0;
	}

	if (sink_datapath.decoder_tid) {
		sink_datapath.thread_abort = true;
		/* Wake the decoder immediately so the join doesn't block this GAF
		 * host callback for up to the sem fallback (~100 ms); it's otherwise
		 * parked in k_sem_take. The K_MSEC(500) below is just a safety cap. */
		k_sem_give(&sink_datapath.data_ready);
		k_thread_join(sink_datapath.decoder_tid, K_MSEC(500));
		sink_datapath.decoder_tid = NULL;
	}

	/* Stop and cleanup all channels */
	for (int i = 0; i < MAX_SINK_CHANNELS; i++) {
		struct sink_channel *channel = &sink_datapath.channel[i];

		if (channel->iso_dp) {
			iso_datapath_ctoh_delete(channel->iso_dp);
			channel->iso_dp = NULL;
		}

		if (channel->sdu_queue) {
			sdu_queue_delete(channel->sdu_queue);
			channel->sdu_queue = NULL;
		}

		if (channel->lc3_decoder) {
			audio_lc3_decoder_destroy(channel->lc3_decoder);
			channel->lc3_decoder = NULL;
		}

		channel->stream_lid = 0xFF;
		channel->enabled = false;
	}

	/* Stop speaker */
	if (sink_datapath.speaker) {
		audio_speaker_destroy(sink_datapath.speaker);
		sink_datapath.speaker = NULL;
	}

	/* Free buffers */
	if (sink_datapath.pcm_buffer) {
		halo_free(sink_datapath.pcm_buffer);
		sink_datapath.pcm_buffer = NULL;
	}

	if (sink_datapath.channel_buffer) {
		halo_free(sink_datapath.channel_buffer);
		sink_datapath.channel_buffer = NULL;
	}

	/* Reset state */
	sink_datapath.initialized = false;
	sink_datapath.streaming = false;
	sink_datapath.thread_abort = false;

	return 0;
}

/**
 * @brief Create audio source datapath (Microphone -> BLE)
 *
 * Initialize microphone capture, LC3 encoding, and ISO transmison.
 *
 * @param cfg Datapath configuration
 * @return 0 on success, negative error code on failure
 */
static int audio_datapath_create_source(struct audio_datapath_config const *const cfg)
{
	if (!cfg) {
		return -EINVAL;
	}

	/* Clean up if already initialized */
	if (source_datapath.initialized) {
		audio_datapath_cleanup_source();
	}

	/* Store configuration */
	source_datapath.sampling_rate_hz = cfg->sampling_rate_hz;
	source_datapath.pres_delay_us = cfg->pres_delay_us;
	source_datapath.frame_duration_is_10ms = cfg->frame_duration_is_10ms;

	/* Determine channel count from LOCATION_SOURCE */
	source_datapath.channel_count = __builtin_popcount(LOCATION_SOURCE);
	if (source_datapath.channel_count > MAX_SOURCE_CHANNELS) {
		source_datapath.channel_count = MAX_SOURCE_CHANNELS;
	}

	/* Validate channel count */
	if (source_datapath.channel_count == 0) {
		LOG_ERR("No source channels configured (LOCATION_SOURCE=0)");
		return -EINVAL;
	}

	/* Calculate samples per frame */
	uint32_t frame_duration_us = cfg->frame_duration_is_10ms ? 10000 : 7500;
	source_datapath.pcm_frame_samples = (cfg->sampling_rate_hz * frame_duration_us) / 1000000;

	/* Allocate PCM buffers */
	size_t pcm_buffer_size =
		source_datapath.pcm_frame_samples * AUDIO_INPUT_CHANNELS * sizeof(int16_t);
	source_datapath.pcm_buffer = halo_malloc(pcm_buffer_size, HALO_MEM_REGION_AUTO);
	if (!source_datapath.pcm_buffer) {
		LOG_ERR("Failed to allocate PCM buffer");
		return -ENOMEM;
	}

	source_datapath.channel_buffer = halo_malloc(
		source_datapath.pcm_frame_samples * sizeof(int16_t), HALO_MEM_REGION_AUTO);
	if (!source_datapath.channel_buffer) {
		LOG_ERR("Failed to allocate channel buffer");
		halo_free(source_datapath.pcm_buffer);
		source_datapath.pcm_buffer = NULL;
		return -ENOMEM;
	}

	/* Initialize channel state */
	for (int ch = 0; ch < MAX_SOURCE_CHANNELS; ch++) {
		source_datapath.channel[ch].iso_dp = NULL;
		source_datapath.channel[ch].sdu_queue = NULL;
		source_datapath.channel[ch].lc3_encoder = NULL;
		source_datapath.channel[ch].stream_lid = 0xFF;
		source_datapath.channel[ch].enabled = false;
	}

	/* Initialize microphone with configured input channels */
	source_datapath.microphone = audio_microphone_init(
		cfg->sampling_rate_hz, 16, AUDIO_INPUT_CHANNELS, 0, AUDIO_OWNER_LE_AUDIO);
	if (!source_datapath.microphone) {
		LOG_ERR("Failed to initialize microphone");
		halo_free(source_datapath.pcm_buffer);
		halo_free(source_datapath.channel_buffer);
		source_datapath.pcm_buffer = NULL;
		source_datapath.channel_buffer = NULL;
		return -EIO;
	}

	/* Create encoder thread */
	source_datapath.thread_abort = false;
	source_datapath.sdu_seq_num = 0;
	source_datapath.encoder_tid = k_thread_create(
		&source_datapath.encoder_thread, source_datapath.encoder_stack,
		K_KERNEL_STACK_SIZEOF(source_datapath.encoder_stack), source_encoder_thread_func,
		NULL, NULL, NULL, K_PRIO_PREEMPT(5), 0, K_NO_WAIT);

	if (!source_datapath.encoder_tid) {
		LOG_ERR("Failed to create encoder thread");
		audio_microphone_destroy(source_datapath.microphone);
		source_datapath.microphone = NULL;
		halo_free(source_datapath.pcm_buffer);
		halo_free(source_datapath.channel_buffer);
		source_datapath.pcm_buffer = NULL;
		source_datapath.channel_buffer = NULL;
		return -ENOMEM;
	}

	k_thread_name_set(source_datapath.encoder_tid, "ble_audio_enc");

	source_datapath.initialized = true;

	return 0;
}

/**
 * @brief Create audio source channel for a specific stream
 *
 * Create SDU queue, ISO datapath, and LC3 encoder for the channel.
 *
 * @param octets_per_frame LC3 frame size in bytes
 * @param stream_lid Stream local ID
 * @return 0 on success, negative error code on failure
 */
static int audio_datapath_channel_create_source(size_t const octets_per_frame,
						uint8_t const stream_lid)
{
	if (!source_datapath.initialized) {
		LOG_ERR("Source not initialized");
		return -EINVAL;
	}

	/* Find free channel slot */
	struct source_channel *channel = NULL;
	for (int i = 0; i < source_datapath.channel_count; i++) {
		if (!source_datapath.channel[i].lc3_encoder) {
			channel = &source_datapath.channel[i];
			break;
		}
	}

	if (!channel) {
		LOG_ERR("No free channel slots");
		return -ENOMEM;
	}

	/* Store octets_per_frame for encoder thread */
	source_datapath.octets_per_frame = octets_per_frame;

	/* Create SDU queue for outgoing LC3 frames */
	channel->sdu_queue = sdu_queue_create(SDU_QUEUE_LENGTH, octets_per_frame);
	if (!channel->sdu_queue) {
		LOG_ERR("Failed to create SDU queue");
		return -ENOMEM;
	}

	/* Create ISO datapath (host -> controller) */
	/* First channel is timing master */
	bool is_timing_master = (channel == &source_datapath.channel[0]);
	channel->iso_dp = iso_datapath_htoc_init(stream_lid, channel->sdu_queue, is_timing_master);
	if (!channel->iso_dp) {
		LOG_ERR("Failed to create ISO datapath htoc");
		sdu_queue_delete(channel->sdu_queue);
		channel->sdu_queue = NULL;
		return -EIO;
	}

	/* Calculate LC3 bitrate from frame size */
	int bitrate;
	if (source_datapath.frame_duration_is_10ms) {
		bitrate = octets_per_frame * 800; /* 10ms: bytes * 8 * 100 */
	} else {
		bitrate = (octets_per_frame * 8 * 1000000) / 7500; /* 7.5ms */
	}

	/* Create LC3 encoder */
	int duration = source_datapath.frame_duration_is_10ms ? 1000 : 750;
	channel->lc3_encoder =
		audio_lc3_encoder_create(source_datapath.sampling_rate_hz, duration, bitrate);
	if (!channel->lc3_encoder) {
		LOG_ERR("Failed to create LC3 encoder");
		iso_datapath_htoc_delete(channel->iso_dp);
		sdu_queue_delete(channel->sdu_queue);
		channel->iso_dp = NULL;
		channel->sdu_queue = NULL;
		return -ENOMEM;
	}

	channel->stream_lid = stream_lid;
	channel->enabled = false;

	return 0;
}

/**
 * @brief Start audio source channel streaming
 *
 * Bind ISO datapath and start microphone capture.
 *
 * @param stream_lid Stream local ID
 * @return 0 on success, negative error code on failure
 */
static int audio_datapath_channel_start_source(uint8_t const stream_lid)
{
	/* Find channel by stream_lid */
	struct source_channel *channel = NULL;
	for (int i = 0; i < source_datapath.channel_count; i++) {
		if (source_datapath.channel[i].stream_lid == stream_lid) {
			channel = &source_datapath.channel[i];
			break;
		}
	}

	if (!channel || !channel->iso_dp) {
		LOG_ERR("Channel not found for stream_lid=%u", stream_lid);
		return -EINVAL;
	}

	/* Bind ISO datapath to controller */
	int ret = iso_datapath_htoc_bind(channel->iso_dp);
	if (ret != 0) {
		LOG_ERR("Failed to bind ISO datapath: %d", ret);
		return ret;
	}

	/* Enable channel */
	channel->enabled = true;

	/* Start microphone if not already streaming */
	if (!source_datapath.streaming && source_datapath.microphone) {
		ret = audio_microphone_start(source_datapath.microphone);
		if (ret != 0) {
			LOG_ERR("Failed to start microphone: %d", ret);
			channel->enabled = false;
			iso_datapath_htoc_unbind(channel->iso_dp);
			return ret;
		}
		source_datapath.streaming = true;
	}

	return 0;
}

/**
 * @brief Stop audio source channel streaming
 *
 * Unbind ISO datapath and stop microphone if no channels are active.
 *
 * @param stream_lid Stream local ID
 * @return 0 on success, negative error code on failure
 */
static int audio_datapath_channel_stop_source(uint8_t const stream_lid)
{
	/* Find channel by stream_lid */
	struct source_channel *channel = NULL;
	for (int i = 0; i < source_datapath.channel_count; i++) {
		if (source_datapath.channel[i].stream_lid == stream_lid) {
			channel = &source_datapath.channel[i];
			break;
		}
	}

	if (!channel) {
		return 0;
	}

	/* Disable channel */
	channel->enabled = false;

	/* Unbind ISO datapath */
	if (channel->iso_dp) {
		iso_datapath_htoc_unbind(channel->iso_dp);
	}

	/* Check if any channels are still enabled */
	bool any_enabled = false;
	for (int i = 0; i < source_datapath.channel_count; i++) {
		if (source_datapath.channel[i].enabled) {
			any_enabled = true;
			break;
		}
	}

	/* Stop microphone if no channels are active */
	if (!any_enabled && source_datapath.streaming && source_datapath.microphone) {
		audio_microphone_stop(source_datapath.microphone);
		source_datapath.streaming = false;
	}

	return 0;
}

/**
 * @brief Clean up audio source datapath
 *
 * Stop encoder thread, destroy all channels, and release microphone.
 *
 * @return 0 on success
 */
static int audio_datapath_cleanup_source(void)
{
	if (!source_datapath.initialized) {
		return 0;
	}

	/* Stop microphone */
	if (source_datapath.streaming && source_datapath.microphone) {
		audio_microphone_stop(source_datapath.microphone);
		source_datapath.streaming = false;
	}

	/* Stop encoder thread */
	if (source_datapath.encoder_tid) {
		source_datapath.thread_abort = true;
		k_thread_join(source_datapath.encoder_tid, K_MSEC(1000));
		source_datapath.encoder_tid = NULL;
	}

	/* Clean up all channels */
	for (int ch = 0; ch < MAX_SOURCE_CHANNELS; ch++) {
		struct source_channel *channel = &source_datapath.channel[ch];

		if (channel->iso_dp) {
			iso_datapath_htoc_unbind(channel->iso_dp);
			iso_datapath_htoc_delete(channel->iso_dp);
			channel->iso_dp = NULL;
		}

		if (channel->sdu_queue) {
			sdu_queue_delete(channel->sdu_queue);
			channel->sdu_queue = NULL;
		}

		if (channel->lc3_encoder) {
			audio_lc3_encoder_destroy(channel->lc3_encoder);
			channel->lc3_encoder = NULL;
		}

		channel->stream_lid = 0xFF;
		channel->enabled = false;
	}

	/* Destroy microphone (only if owned by LE_AUDIO) */
	if (source_datapath.microphone) {
		if (audio_microphone_check_owner(source_datapath.microphone,
						 AUDIO_OWNER_LE_AUDIO)) {
			audio_microphone_destroy(source_datapath.microphone);
		}
		source_datapath.microphone = NULL;
	}

	/* Free buffers */
	if (source_datapath.pcm_buffer) {
		halo_free(source_datapath.pcm_buffer);
		source_datapath.pcm_buffer = NULL;
	}

	if (source_datapath.channel_buffer) {
		halo_free(source_datapath.channel_buffer);
		source_datapath.channel_buffer = NULL;
	}

	/* Reset state */
	source_datapath.initialized = false;
	source_datapath.streaming = false;
	source_datapath.thread_abort = false;
	source_datapath.sdu_seq_num = 0;

	return 0;
}

/**
 * @brief Set volume for sink channel
 *
 * Apply volume to speaker hardware.
 *
 * @param volume Volume level (0-127 after >> 1 from VCS 0-255)
 * @param mute Mute state
 * @return 0 on success
 */
static int audio_datapath_channel_volume_sink(uint8_t const volume, bool const mute)
{
	/* Store volume state */
	sink_datapath.volume = volume;
	sink_datapath.mute = mute;

	if (!sink_datapath.speaker) {
		return 0;
	}

	/* Convert 0-127 to 0-100 for speaker API */
	int volume_percent = mute ? 0 : (volume * 100 / 127);
	int ret = audio_speaker_set_volume(sink_datapath.speaker, volume_percent);
	if (ret != 0) {
		LOG_ERR("Failed to set volume: %d", ret);
		return ret;
	}

	return 0;
}

/* ======================================================================================== */
/* Worker Queue for Async Audio Operations                                                   */
/* ======================================================================================== */

#define WORKER_PRIORITY   2
#define WORKER_STACK_SIZE 2048

K_KERNEL_STACK_DEFINE(worker_task_stack, WORKER_STACK_SIZE);
static struct k_work_q worker_queue;

/**
 * @brief Work handler to enable streaming after CIS establishment
 *
 * This is called via work queue to add a small delay after CIS setup
 * before starting actual audio streaming.
 */
static void enable_streaming(struct k_work *const p_job)
{
	/* Small delay needed due to lower layer latency */
	k_sleep(K_MSEC(2));

	struct ase_instance const *const p_ase = CONTAINER_OF(p_job, struct ase_instance, work);

	if (p_ase->dir == ASE_DIR_SINK) {
		audio_datapath_channel_start_sink(p_ase->stream_lid);
	} else if (p_ase->dir == ASE_DIR_SOURCE) {
		audio_datapath_channel_start_source(p_ase->stream_lid);
	}
}

/* ======================================================================================== */
/* Volume Control Server (VCS) Callbacks                                                     */
/* ======================================================================================== */

/**
 * @brief VCS bond data callback
 *
 * Called when VCS client configuration is updated.
 * Can be used to save bond data to persistent storage.
 */
static void volume_control_bond_data_callback(uint8_t const conidx, uint8_t const cli_cfg_bf)
{
	/* TODO: Save bond data to persistent storage if needed */
}

/**
 * @brief VCS volume change callback
 *
 * Called when volume or mute state changes (from remote or local).
 * Apply the volume change to your audio output.
 */
static void volume_control_volume_callback(uint8_t const volume, uint8_t mute, bool const local)
{

	/* Apply volume to audio datapath
	 * VCS uses 0-255 range, codec typically uses smaller range
	 * >> 1 converts to 0-127 range
	 */
	audio_datapath_channel_volume_sink((volume >> 1), mute);

	/* Update context */
	audio_ctx.volume = volume;
	audio_ctx.mute = mute;
}

/**
 * @brief VCS flags change callback
 */
static void volume_control_flags_callback(uint8_t const flags)
{
}

/**
 * @brief Callbacks for Volume Control Server.
 */
static const arc_vcs_cb_t volume_control_callbacks = {
	.cb_bond_data = volume_control_bond_data_callback,
	.cb_volume = volume_control_volume_callback,
	.cb_flags = volume_control_flags_callback,
};

#ifdef CONFIG_HALO_BLE_AUDIO_MICS
/* ======================================================================================== */
/* Microphone Control Server (MICS) + Audio Input Control (AICS) Callbacks                   */
/* ======================================================================================== */

#ifdef CONFIG_HALO_BLE_AUDIO_AICS
/**
 * @brief Apply a gain requested via AICS to the microphone hardware
 *
 * When the LE Audio source stream owns the microphone the gain is applied (and
 * persisted) immediately; otherwise only the persisted setting is updated so it
 * takes effect the next time the microphone is initialized.
 */
static void mic_control_apply_gain(int gain)
{
	if (source_datapath.microphone &&
	    audio_microphone_check_owner(source_datapath.microphone, AUDIO_OWNER_LE_AUDIO)) {
		audio_microphone_set_gain(source_datapath.microphone, gain);
	} else {
		halo_settings_set("audio/gain", &gain, sizeof(gain));
	}
}
#endif /* CONFIG_HALO_BLE_AUDIO_AICS */

/**
 * @brief MICS bond data callback (client subscribed to Mute notifications)
 */
static void mic_control_bond_data_callback(uint8_t const con_lid, uint8_t const cli_cfg_bf)
{
}

/**
 * @brief MICS mute change callback
 *
 * The source encoder thread picks this up and transmits silence while muted,
 * keeping the ISO stream cadence as the Microphone Control Profile requires.
 */
static void mic_control_mute_callback(uint8_t const mute)
{
	audio_ctx.mic_mute = mute;
	LOG_INF("MICS mute changed: %u", mute);
}

static const arc_mics_cb_t mic_control_callbacks = {
	.cb_bond_data = mic_control_bond_data_callback,
	.cb_mute = mic_control_mute_callback,
};

#ifdef CONFIG_HALO_BLE_AUDIO_AICS
/**
 * @brief AICS bond data callback
 */
static void mic_input_bond_data_callback(uint8_t const input_lid, uint8_t const con_lid,
					 uint8_t const cli_cfg_bf)
{
}

/**
 * @brief AICS input state callback (gain / per-input mute / gain mode)
 */
static void mic_input_state_callback(uint8_t const input_lid, arc_aic_state_t *const p_state)
{
	if (!p_state) {
		return;
	}

	audio_ctx.mic_aics_mute = p_state->mute;
	mic_control_apply_gain(p_state->gain);
}

/**
 * @brief AICS description write callback
 *
 * The description is not writable (ARC_AICS_CFG_DESC_WR is not set), so reject
 * any write that somehow reaches us.
 */
static void mic_input_description_req_callback(uint8_t const input_lid, uint8_t const con_lid,
					       uint8_t const desc_len, const char *const p_desc)
{
	arc_aics_set_description_cfm(false, input_lid, 0, NULL);
}

static const arc_aics_cb_t mic_input_callbacks = {
	.cb_bond_data = mic_input_bond_data_callback,
	.cb_state = mic_input_state_callback,
	.cb_description_req = mic_input_description_req_callback,
};
#endif /* CONFIG_HALO_BLE_AUDIO_AICS */
#endif /* CONFIG_HALO_BLE_AUDIO_MICS */

/* ======================================================================================== */
/* BAP Unicast Server Callbacks                                                              */
/* ======================================================================================== */

/**
 * @brief Unicast server command completion callback
 *
 * Called when async BAP Unicast Server commands complete.
 */
static void on_unicast_server_cb_cmp_evt(uint8_t const cmd_type, uint16_t const status,
					 uint8_t const ase_lid)
{
	if (status != 0) {
		LOG_WRN("UC cmd %u failed: status=%u, ase=%u", cmd_type, status, ase_lid);
	}
}

/**
 * @brief ISO link quality statistics callback
 *
 * Useful for debugging audio quality issues.
 */
static void on_unicast_server_cb_quality_cmp_evt(
	uint16_t const status, uint8_t const ase_lid, uint32_t const tx_unacked_packets,
	uint32_t const tx_flushed_packets, uint32_t const tx_last_subevent_packets,
	uint32_t const retransmitted_packets, uint32_t const crc_error_packets,
	uint32_t const rx_unreceived_packets, uint32_t const duplicate_packets)
{
	ARG_UNUSED(tx_unacked_packets);
	ARG_UNUSED(tx_flushed_packets);
	ARG_UNUSED(tx_last_subevent_packets);
	ARG_UNUSED(retransmitted_packets);
	ARG_UNUSED(duplicate_packets);
}

/**
 * @brief ASCS bond data callback
 *
 * Called when ASCS client configuration is updated.
 */
static void on_unicast_server_cb_bond_data(uint8_t const conidx, uint8_t const cli_cfg_bf,
					   uint16_t const ase_cli_cfg_bf)
{
}

/**
 * @brief ASE state change callback
 *
 * This is the main state machine callback. Handle audio datapath
 * creation/destruction based on ASE state transitions.
 *
 * State flow: IDLE -> CODEC_CONFIGURED -> QOS_CONFIGURED -> ENABLING -> STREAMING -> RELEASING ->
 * IDLE
 */
static void on_unicast_server_cb_ase_state(uint8_t const ase_lid, uint8_t const conidx,
					   uint8_t const state, bap_qos_cfg_t *const p_qos_cfg)
{

	switch (state) {
	case BAP_UC_ASE_STATE_IDLE: {
		/* ASE released - clean up the datapath for this ASE's direction only,
		 * so releasing a microphone stream doesn't tear down an active
		 * speaker stream (and vice versa) during bidirectional use */
		struct ase_instance *const p_ase = get_ase_context_by_lid(ase_lid);

		if (!p_ase || p_ase->dir == ASE_DIR_SINK) {
			audio_datapath_cleanup_sink();
		}
		if (!p_ase || p_ase->dir == ASE_DIR_SOURCE) {
			audio_datapath_cleanup_source();
		}
		break;
	}

	case BAP_UC_ASE_STATE_QOS_CONFIGURED: {
		/* QoS configured - create audio datapath */
		struct ase_instance *const p_ase = get_ase_context_by_lid(ase_lid);
		if (!p_ase) {
			break;
		}

		if (p_ase->dir == ASE_DIR_SINK) {
			audio_datapath_create_sink(&audio_ctx.ase_config_sink.datapath_config);
		} else if (p_ase->dir == ASE_DIR_SOURCE) {
			audio_datapath_create_source(&audio_ctx.ase_config_src.datapath_config);
		}
		break;
	}

	case BAP_UC_ASE_STATE_ENABLING:
		/* Enabling - waiting for client to establish CIS */
		break;

	case BAP_UC_ASE_STATE_RELEASING:
		/* Confirm release */
		bap_uc_srv_release_cfm(ase_lid, BAP_UC_CP_RSP_CODE_SUCCESS, 0, true);
		break;

	case BAP_UC_ASE_STATE_STREAMING: {
		/* Streaming started - start audio channel via work queue */
		struct ase_instance *const p_ase = get_ase_context_by_lid(ase_lid);
		if (!p_ase) {
			LOG_ERR("No ASE context for lid %u", ase_lid);
			break;
		}

		k_work_init(&p_ase->work, enable_streaming);
		k_work_submit_to_queue(&worker_queue, &p_ase->work);
		break;
	}

	default:
		break;
	}
}

/**
 * @brief CIS (Connected Isochronous Stream) state callback
 *
 * Called when CIS is established or released. Updates stream_lid mapping.
 * The stream_lid is needed to identify which ISO channel to use for audio.
 */
static void on_unicast_server_cb_cis_state(uint8_t const stream_lid, uint8_t const conidx,
					   uint8_t const ase_lid_sink, uint8_t const ase_lid_src,
					   uint8_t const cig_id, uint8_t const cis_id,
					   uint16_t const conhdl, gapi_ug_config_t *const p_cig_cfg,
					   gapi_us_config_t *const p_cis_cfg)
{
	struct ase_instance *p_ase;

	/* Update stream_lid for sink ASE */
	p_ase = get_ase_context_by_lid(ase_lid_sink);
	if (p_ase) {
		p_ase->stream_lid = stream_lid;
	}

	/* Update stream_lid for source ASE */
	p_ase = get_ase_context_by_lid(ase_lid_src);
	if (p_ase) {
		p_ase->stream_lid = stream_lid;
	}

	/* If conhdl is invalid, CIS is released */
	if (conhdl == GAP_INVALID_CONHDL) {
		return;
	}
}

/* Defined below, after the PAC capability tables. */
static bool frame_octet_within_capa(uint8_t dir, uint8_t sampling_freq, uint8_t frame_dur,
				    uint16_t frame_octet);

/**
 * @brief Codec configuration request callback
 *
 * Client requests to configure the codec for an ASE.
 * Validate the codec configuration and respond with preferred QoS settings.
 *
 * This is a critical callback - proper implementation required for audio to work!
 */
static void
on_unicast_server_cb_configure_codec_req(uint8_t const conidx, uint8_t const ase_instance_idx,
					 uint8_t const ase_lid, uint8_t const tgt_latency,
					 uint8_t const tgt_phy, gaf_codec_id_t *const p_codec_id,
					 const bap_cfg_ptr_t *const p_cfg)
{
	size_t const config_len = p_cfg->p_add_cfg ? p_cfg->p_add_cfg->len : 0;
	size_t const size = sizeof(bap_cfg_t) + config_len;
	uint_fast8_t const ase_lid_cfm = ase_lid == GAF_INVALID_LID ? ase_instance_idx : ase_lid;
	bap_cfg_t *p_ase_codec_cfg = NULL;

	/* Preferred QoS settings to report to client */
	bap_qos_req_t qos_req = {
		.pres_delay_min_us = MINIMUM_PRESENTATION_DELAY_US,
		.pres_delay_max_us = MAXIMUM_PRESENTATION_DELAY_US,
		.pref_pres_delay_min_us = MINIMUM_PRESENTATION_DELAY_US,
		.pref_pres_delay_max_us = MAXIMUM_PRESENTATION_DELAY_US,
		/* Transport latency based on target latency */
		.trans_latency_max_ms = (tgt_latency == BAP_UC_TGT_LATENCY_LOWER) ? 20 : 100,
		.framing = ISO_UNFRAMED_MODE,
		.phy_bf = (GAP_PHY_LE_1MBPS | GAP_PHY_LE_2MBPS),
		/* Retransmission based on target latency */
		.retx_nb = (tgt_latency == BAP_UC_TGT_LATENCY_LOWER) ? 5 : 13,
	};

	struct ase_instance *p_ase = get_ase_context_by_lid(ase_lid);
	if (!p_ase) {
		p_ase = get_ase_context_by_idx(ase_instance_idx);
	}
	if (!p_ase) {
		LOG_ERR("Invalid ASE instance! ase_instance_idx:%u, ase_lid:%u", ase_instance_idx,
			ase_lid);
		goto bap_codec_config_cfm;
	}

	/* Validate PHY */
	if (tgt_phy < BAP_UC_TGT_PHY_1M || tgt_phy > BAP_UC_TGT_PHY_2M) {
		LOG_ERR("Invalid PHY %u", tgt_phy);
		goto bap_codec_config_cfm;
	}

	/* Only LC3 codec is supported */
	if (p_codec_id->codec_id[0] != GAPI_CODEC_FORMAT_LC3) {
		LOG_ERR("Invalid codec %u (only LC3 supported)", p_codec_id->codec_id[0]);
		goto bap_codec_config_cfm;
	}

	/* Reject an out-of-range frame_octet before it sizes the SDU slab. This is
	 * an attacker-controlled uint16 from the air; honour it only if it falls
	 * within a capability record we actually advertised for this direction +
	 * sampling frequency + frame duration. Also stops the silent uint8_t
	 * truncation on store (ase_config.frame_octet) mis-sizing the decoder. */
	if (!frame_octet_within_capa(p_ase->dir, p_cfg->param.sampling_freq,
				     p_cfg->param.frame_dur, p_cfg->param.frame_octet)) {
		LOG_ERR("Rejecting codec config: frame_octet %u out of range (freq %u, dur %u)",
			p_cfg->param.frame_octet, p_cfg->param.sampling_freq,
			p_cfg->param.frame_dur);
		goto bap_codec_config_cfm;
	}

	/* Allocate codec configuration buffer */
	p_ase_codec_cfg = halo_malloc(size, HALO_MEM_REGION_AUTO);
	if (!p_ase_codec_cfg) {
		LOG_ERR("Failed to allocate codec configuration buffer");
		goto bap_codec_config_cfm;
	}

	/* Update ASE context */
	p_ase->ase_lid = ase_lid_cfm;
	p_ase->stream_lid = ase_instance_idx;

	/* Store configuration based on direction */
	if (p_ase->dir == ASE_DIR_SINK) {
		audio_ctx.ase_config_sink.datapath_config.pres_delay_us = qos_req.pres_delay_max_us;
		audio_ctx.ase_config_sink.datapath_config.sampling_rate_hz =
			audio_bap_sampling_freq_to_hz(p_cfg->param.sampling_freq);
		audio_ctx.ase_config_sink.datapath_config.frame_duration_is_10ms =
			p_cfg->param.frame_dur;
		audio_ctx.ase_config_sink.frame_octet = p_cfg->param.frame_octet;
	} else if (p_ase->dir == ASE_DIR_SOURCE) {
		audio_ctx.ase_config_src.datapath_config.pres_delay_us = qos_req.pres_delay_max_us;
		audio_ctx.ase_config_src.datapath_config.sampling_rate_hz =
			audio_bap_sampling_freq_to_hz(p_cfg->param.sampling_freq);
		audio_ctx.ase_config_src.datapath_config.frame_duration_is_10ms =
			p_cfg->param.frame_dur;
		audio_ctx.ase_config_src.frame_octet = p_cfg->param.frame_octet;
	}

	/* Copy codec configuration. Copy the whole LTV (len byte + data) starting
	 * at add_cfg, matching the allocation (sizeof(bap_cfg_t) reserves the len
	 * byte, +config_len reserves data[]) and the correct metadata path in
	 * on_unicast_server_cb_enable_req(). Writing 1 + config_len into
	 * add_cfg.data instead overran the allocation by one byte and over-read the
	 * air-supplied source LTV by one, for an attacker-controlled config_len. */
	memcpy(&p_ase_codec_cfg->param, &p_cfg->param, sizeof(bap_cfg_param_t));
	p_ase_codec_cfg->add_cfg.len = config_len;
	if (config_len) {
		memcpy(&p_ase_codec_cfg->add_cfg, p_cfg->p_add_cfg,
		       sizeof(p_ase_codec_cfg->add_cfg.len) + config_len);
	}

bap_codec_config_cfm:
	/* Store codec configuration in ASE context for later release */
	if (p_ase && p_ase_codec_cfg) {
		p_ase->p_codec_cfg = p_ase_codec_cfg;
	}

	bap_uc_srv_configure_codec_cfm(conidx,
				       p_ase_codec_cfg ? BAP_UC_CP_RSP_CODE_SUCCESS
						       : BAP_UC_CP_RSP_CODE_INSUFFICIENT_RESOURCES,
				       0, ase_lid_cfm, &qos_req, p_ase_codec_cfg, CONTROL_DELAY_US,
				       DATA_PATH_CONFIG);
}

/**
 * @brief QoS configuration request callback (ROM version > 1.0)
 *
 * Validates and confirms QoS parameters.
 */
#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 /* ROM version > 1.0 */
static void on_unicast_server_cb_configure_qos_req(uint8_t const ase_lid, uint8_t const stream_lid,
						   const bap_qos_cfg_t *const p_qos_cfg)
{
	uint_fast8_t rsp_code = BAP_UC_CP_RSP_CODE_SUCCESS;
	uint8_t reason = 0;

	/* Validate presentation delay */
	if (p_qos_cfg->pres_delay_us < MINIMUM_PRESENTATION_DELAY_US ||
	    p_qos_cfg->pres_delay_us > MAXIMUM_PRESENTATION_DELAY_US) {
		rsp_code = BAP_UC_CP_RSP_CODE_UNSUPPORTED_CFG_PARAM;
		reason = BAP_UC_CP_REASON_PRES_DELAY;
	} else {
		struct ase_instance *const p_ase = get_ase_context_by_lid(ase_lid);

		if (p_ase) {
			p_ase->stream_lid = stream_lid;

			/* Update presentation delay with actual value from client */
			if (p_ase->dir == ASE_DIR_SINK) {
				audio_ctx.ase_config_sink.datapath_config.pres_delay_us =
					p_qos_cfg->pres_delay_us;
			} else if (p_ase->dir == ASE_DIR_SOURCE) {
				audio_ctx.ase_config_src.datapath_config.pres_delay_us =
					p_qos_cfg->pres_delay_us;
			}
		} else {
			rsp_code = BAP_UC_CP_RSP_CODE_INVALID_ASE_ID;
			reason = BAP_UC_CP_REASON_INVALID_ASE_CIS_MAPPING;
		}
	}

	bap_uc_srv_configure_qos_cfm(ase_lid, rsp_code, reason);
}
#endif

/**
 * @brief Enable request callback
 *
 * Client requests to enable streaming. Create the audio channel.
 */
static void on_unicast_server_cb_enable_req(uint8_t const ase_lid,
					    bap_cfg_metadata_ptr_t *const p_metadata)
{
	uint8_t rsp_code = BAP_UC_CP_RSP_CODE_SUCCESS;
	size_t const metadata_len =
		p_metadata->p_add_metadata ? p_metadata->p_add_metadata->len : 0;
	size_t const size = sizeof(bap_cfg_metadata_t) + metadata_len;
	struct ase_instance *p_ase = get_ase_context_by_lid(ase_lid);
	bap_cfg_metadata_t *p_ase_metadata = NULL;

	if (!p_ase) {
		LOG_ERR("Invalid ASE ID %u", ase_lid);
		rsp_code = BAP_UC_CP_RSP_CODE_INVALID_ASE_ID;
		goto bap_cfg_metadata_cfm;
	}

	/* Allocate metadata buffer */
	p_ase_metadata = halo_malloc(size, HALO_MEM_REGION_AUTO);
	if (!p_ase_metadata) {
		LOG_ERR("Failed to allocate metadata buffer");
		rsp_code = BAP_UC_CP_RSP_CODE_INSUFFICIENT_RESOURCES;
		goto bap_cfg_metadata_cfm;
	}

	p_ase_metadata->param.context_bf = p_metadata->param.context_bf;
	p_ase_metadata->add_metadata.len = metadata_len;
	if (metadata_len) {
		memcpy(&p_ase_metadata->add_metadata, p_metadata->p_add_metadata,
		       sizeof(p_ase_metadata->add_metadata.len) + metadata_len);
	}

	/* Create audio channel based on direction */
	if (p_ase->dir == ASE_DIR_SINK) {
		if (audio_datapath_channel_create_sink(audio_ctx.ase_config_sink.frame_octet,
						       p_ase->stream_lid)) {
			LOG_ERR("Failed to create sink channel");
			rsp_code = BAP_UC_CP_RSP_CODE_UNSPECIFIED_ERROR;
		}
	} else if (p_ase->dir == ASE_DIR_SOURCE) {
		if (audio_datapath_channel_create_source(audio_ctx.ase_config_src.frame_octet,
							 p_ase->stream_lid)) {
			LOG_ERR("Failed to create source channel");
			rsp_code = BAP_UC_CP_RSP_CODE_UNSPECIFIED_ERROR;
		}
	}

bap_cfg_metadata_cfm:
	/* Store enable metadata in ASE context for later release */
	if (p_ase && p_ase_metadata) {
		p_ase->p_enable_metadata = p_ase_metadata;
	}

	bap_uc_srv_enable_cfm(ase_lid, rsp_code, 0, p_ase_metadata);
}

/**
 * @brief Update metadata request callback
 */
static void on_unicast_server_cb_update_metadata_req(uint8_t const ase_lid,
						     bap_cfg_metadata_ptr_t *const p_metadata)
{
	size_t const metadata_len =
		p_metadata->p_add_metadata ? p_metadata->p_add_metadata->len : 0;
	size_t const size = sizeof(bap_cfg_metadata_t) + metadata_len;

	struct ase_instance *p_ase = get_ase_context_by_lid(ase_lid);
	bap_cfg_metadata_t *p_ase_metadata = halo_malloc(size, HALO_MEM_REGION_AUTO);

	if (p_ase_metadata) {
		p_ase_metadata->param.context_bf = p_metadata->param.context_bf;
		p_ase_metadata->add_metadata.len = metadata_len;
		if (metadata_len) {
			memcpy(&p_ase_metadata->add_metadata, p_metadata->p_add_metadata,
			       sizeof(p_ase_metadata->add_metadata.len) + metadata_len);
		}
	}

	/* Store update metadata in ASE context for later release */
	if (p_ase && p_ase_metadata) {
		p_ase->p_update_metadata = p_ase_metadata;
	}

	bap_uc_srv_update_metadata_cfm(ase_lid,
				       p_ase_metadata ? BAP_UC_CP_RSP_CODE_SUCCESS
						      : BAP_UC_CP_RSP_CODE_INSUFFICIENT_RESOURCES,
				       0, p_ase_metadata);
}

/**
 * @brief Release request callback
 */
static void on_unicast_server_cb_release_req(uint8_t const ase_lid)
{
	struct ase_instance *p_ase = get_ase_context_by_lid(ase_lid);

	bap_uc_srv_release_cfm(ase_lid, BAP_UC_CP_RSP_CODE_SUCCESS, 0, true);

	if (p_ase) {
		if (p_ase->p_codec_cfg) {
			halo_free(p_ase->p_codec_cfg);
			p_ase->p_codec_cfg = NULL;
		}
		if (p_ase->p_enable_metadata) {
			halo_free(p_ase->p_enable_metadata);
			p_ase->p_enable_metadata = NULL;
		}
		if (p_ase->p_update_metadata) {
			halo_free(p_ase->p_update_metadata);
			p_ase->p_update_metadata = NULL;
		}
	}
}

/**
 * @brief Data path update request callback
 *
 * Called when data path should be started or stopped.
 */
static void on_unicast_server_cb_dp_update_req(uint8_t const ase_lid, bool const start)
{
	struct ase_instance *p_ase = get_ase_context_by_lid(ase_lid);
	bool const valid_config = p_ase && p_ase->stream_lid != GAF_INVALID_LID;

	bap_uc_srv_dp_update_cfm(ase_lid, valid_config);

	if (!valid_config) {
		LOG_ERR("Invalid data path configuration for ASE %d", ase_lid);
		return;
	}

	if (!start) {
		if (p_ase->dir == ASE_DIR_SINK) {
			audio_datapath_channel_stop_sink(p_ase->stream_lid);
			p_ase->stream_lid = GAF_INVALID_LID;
		} else if (p_ase->dir == ASE_DIR_SOURCE) {
			audio_datapath_channel_stop_source(p_ase->stream_lid);
			p_ase->stream_lid = GAF_INVALID_LID;
		}
	}
}

static const struct bap_uc_srv_cb bap_uc_srv_cb = {
	.cb_cmp_evt = on_unicast_server_cb_cmp_evt,
	.cb_quality_cmp_evt = on_unicast_server_cb_quality_cmp_evt,
	.cb_bond_data = on_unicast_server_cb_bond_data,
	.cb_ase_state = on_unicast_server_cb_ase_state,
	.cb_cis_state = on_unicast_server_cb_cis_state,
	.cb_configure_codec_req = on_unicast_server_cb_configure_codec_req,
#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 /* ROM version > 1.0 */
	.cb_configure_qos_req = on_unicast_server_cb_configure_qos_req,
#endif
	.cb_enable_req = on_unicast_server_cb_enable_req,
	.cb_update_metadata_req = on_unicast_server_cb_update_metadata_req,
	.cb_release_req = on_unicast_server_cb_release_req,
	.cb_dp_update_req = on_unicast_server_cb_dp_update_req,
};

/* ---------------------------------------------------------------------------------------- */
/* PAC Capability server */

static void on_capabilities_server_cb_bond_data(uint8_t const conidx, uint8_t const cli_cfg_bf,
						uint16_t const pac_cli_cfg_bf)
{
}

static void on_capabilities_server_cb_location_req(uint8_t const conidx,
#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 /* ROM version > 1.0 */
						   uint16_t const token,
#endif
						   uint8_t const direction,
						   uint32_t const location_bf)
{
}

static struct bap_capa_srv_cb capa_srv_cbs = {
	.cb_bond_data = on_capabilities_server_cb_bond_data,
#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 /* ROM version > 1.0 */
	.cb_location_req = on_capabilities_server_cb_location_req,
#else /* ROM version 1.0 */
	.cb_location = on_capabilities_server_cb_location_req,
#endif
};

/** Maximum number of records per PAC characteristic - Shall be higher than 0 */
#define NB_RECORDS_PER_PAC_MAX (1U)
/** Compute number of PAC characteristics based on number of records */
#define NB_PAC(nb_records)     ROUND_UP((nb_records), NB_RECORDS_PER_PAC_MAX)

/* Capability record structure */
struct capa_record {
	uint16_t sampling_freq_bf;
	uint8_t frame_dur_bf;
	uint16_t frame_octet_min;
	uint16_t frame_octet_max;
};

#ifdef CONFIG_HALO_BLE_AUDIO_SINK_ENABLE
/* Sink capabilities: 8k, 16k @ 10ms/7.5ms */
static const struct capa_record sink_capas[] = {
	{BAP_SAMPLING_FREQ_8000HZ_BIT, BAP_FRAME_DUR_10MS_BIT, 26, 26},
	{BAP_SAMPLING_FREQ_8000HZ_BIT, BAP_FRAME_DUR_7_5MS_BIT, 26, 26},
	{BAP_SAMPLING_FREQ_16000HZ_BIT, BAP_FRAME_DUR_10MS_BIT, 40, 40},
	{BAP_SAMPLING_FREQ_16000HZ_BIT, BAP_FRAME_DUR_7_5MS_BIT, 30, 30},
	// {BAP_SAMPLING_FREQ_24000HZ_BIT, BAP_FRAME_DUR_10MS_BIT, 60, 60},
	// {BAP_SAMPLING_FREQ_32000HZ_BIT, BAP_FRAME_DUR_10MS_BIT, 60, 80},
	// {BAP_SAMPLING_FREQ_48000HZ_BIT, BAP_FRAME_DUR_10MS_BIT, 75, 155},
};
#endif

#ifdef CONFIG_HALO_BLE_AUDIO_SOURCE_ENABLE
/* Source capabilities: 8k, 16k @ 10ms/7.5ms (BAP 8_1/8_2/16_1/16_2), plus
 * 32k/48k when high-quality capture is enabled (BAP 32_1/32_2/48_1..48_6).
 * The PDM front-end supports 8/16/32/48 kHz only — 24/44.1 kHz would require
 * resampling, so those configurations are deliberately not advertised. */
static const struct capa_record source_capas[] = {
	{BAP_SAMPLING_FREQ_8000HZ_BIT, BAP_FRAME_DUR_10MS_BIT, 26, 26},
	{BAP_SAMPLING_FREQ_8000HZ_BIT, BAP_FRAME_DUR_7_5MS_BIT, 26, 26},
	{BAP_SAMPLING_FREQ_16000HZ_BIT, BAP_FRAME_DUR_10MS_BIT, 40, 40},
	{BAP_SAMPLING_FREQ_16000HZ_BIT, BAP_FRAME_DUR_7_5MS_BIT, 30, 30},
#ifdef CONFIG_HALO_BLE_AUDIO_SOURCE_HQ
	{BAP_SAMPLING_FREQ_32000HZ_BIT, BAP_FRAME_DUR_10MS_BIT, 60, 80},
	{BAP_SAMPLING_FREQ_32000HZ_BIT, BAP_FRAME_DUR_7_5MS_BIT, 60, 60},
	{BAP_SAMPLING_FREQ_48000HZ_BIT, BAP_FRAME_DUR_10MS_BIT, 75, 155},
	{BAP_SAMPLING_FREQ_48000HZ_BIT, BAP_FRAME_DUR_7_5MS_BIT, 75, 117},
#endif
};
#endif

/**
 * @brief Validate an air-supplied LC3 frame_octet against advertised PAC capabilities.
 *
 * The codec-configure control-point write carries an attacker-controlled
 * frame_octet that ultimately sizes the per-channel SDU slab (SDU_QUEUE_LENGTH
 * buffers of frame_octet bytes each). Accept it only when it lies within the
 * octet range of a capability record we actually advertised for this direction,
 * sampling frequency and frame duration. The capa tables encode freq/dur as
 * bitfield positions: sampling_freq bit = 1 << (freq_enum - 1) and frame_dur
 * bit = 1 << dur_enum (see enum bap_sampling_freq / bap_frame_dur vs their _BF
 * counterparts in bap.h).
 *
 * @return true iff (dir, sampling_freq, frame_dur) matches an advertised record
 *         and frame_octet is within that record's [min, max].
 */
static bool frame_octet_within_capa(uint8_t const dir, uint8_t const sampling_freq,
				    uint8_t const frame_dur, uint16_t const frame_octet)
{
	if (sampling_freq < BAP_SAMPLING_FREQ_MIN || frame_dur > BAP_FRAME_DUR_MAX) {
		return false;
	}
	uint16_t const freq_bit = (uint16_t)(1U << (sampling_freq - 1));
	uint8_t const dur_bit = (uint8_t)(1U << frame_dur);

	const struct capa_record *capas = NULL;
	size_t n = 0;

	if (dir == ASE_DIR_SINK) {
#ifdef CONFIG_HALO_BLE_AUDIO_SINK_ENABLE
		capas = sink_capas;
		n = ARRAY_SIZE(sink_capas);
#endif
	} else if (dir == ASE_DIR_SOURCE) {
#ifdef CONFIG_HALO_BLE_AUDIO_SOURCE_ENABLE
		capas = source_capas;
		n = ARRAY_SIZE(source_capas);
#endif
	}

	for (size_t i = 0; i < n; i++) {
		if ((capas[i].sampling_freq_bf & freq_bit) &&
		    (capas[i].frame_dur_bf & dur_bit) &&
		    frame_octet >= capas[i].frame_octet_min &&
		    frame_octet <= capas[i].frame_octet_max) {
			return true;
		}
	}
	return false;
}

/* ======================================================================================== */
/* Public API Implementation                                                                 */
/* ======================================================================================== */

/**
 * @brief Initialize the LE Audio service
 *
 * This function initializes all LE Audio components:
 * 1. Worker queue for async audio operations
 * 2. GAF Advertiser for LE Audio announcements
 * 3. BAP Unicast Server for audio streaming
 * 4. TMAP (Telephony and Media Audio Profile)
 * 5. PACS (Published Audio Capabilities Server)
 * 6. VCS (Volume Control Service)
 *
 * @param reset If true, reset the service state even if already initialized
 * @return 0 on success, negative error code on failure
 */
int halo_ble_audio_init(bool reset)
{

	if (audio_ctx.initialized == BLE_AUDIO_INIT_MAGIC) {
		LOG_WRN("BLE Audio Service already initialized");
		return 0;
	}

	if (reset) {
		memset(&audio_ctx, 0, sizeof(audio_ctx));
	}

	k_work_queue_start(&worker_queue, worker_task_stack,
			   K_KERNEL_STACK_SIZEOF(worker_task_stack), WORKER_PRIORITY, NULL);
	k_thread_name_set(&worker_queue.thread, "ble_audio_workq");

	int err;
	size_t iter;

	/* ------------------------------------------------------------------------------------ */
	/* Step 2: Calculate number of ASEs based on location configuration                     */
	/* ------------------------------------------------------------------------------------ */
	audio_ctx.ase_config_sink.nb_ases = __builtin_popcount(LOCATION_SINK);
	audio_ctx.ase_config_src.nb_ases = __builtin_popcount(LOCATION_SOURCE);
	audio_ctx.total_ases = audio_ctx.ase_config_sink.nb_ases + audio_ctx.ase_config_src.nb_ases;

	LOG_INF("ASE: sink=%u, src=%u", audio_ctx.ase_config_sink.nb_ases,
		audio_ctx.ase_config_src.nb_ases);

	if (audio_ctx.total_ases > MAX_NUMBER_OF_ASE) {
		LOG_ERR("Number of ASEs exceeds maximum allowed (%u)", MAX_NUMBER_OF_ASE);
		return -1;
	}

	/* Skip BAP/TMAP/PACS/VCS configuration if not reset (already configured in ROM) */
	if (!reset) {
		audio_ctx.pacs_record_count = 0;
		audio_ctx.initialized = BLE_AUDIO_INIT_MAGIC;
		return 0;
	}

	/* ------------------------------------------------------------------------------------ */
	/* Step 3: Configure BAP Unicast Server                                                 */
	/* ------------------------------------------------------------------------------------ */
	{
		bap_uc_srv_cfg_t uc_srv_cfg = {
			.nb_ase_chars_sink = audio_ctx.ase_config_sink.nb_ases,
			.nb_ase_chars_src = audio_ctx.ase_config_src.nb_ases,
			.nb_ases_cfg = audio_ctx.total_ases * MAX_NUMBER_OF_CONNECTIONS,
			.cfg_bf = 0,
			.pref_mtu = GAP_LE_MAX_OCTETS,
			.shdl = GATT_INVALID_HDL,
		};

		err = bap_uc_srv_configure(&bap_uc_srv_cb, &uc_srv_cfg);
		if (err != GAF_ERR_NO_ERROR) {
			LOG_ERR("Unable to configure BAP unicast server! Error %u (0x%02X)", err,
				err);
			return -1;
		}

		if (!bap_uc_srv_is_configured()) {
			LOG_ERR("BAP unicast server is not ready!");
			return -1;
		}

		/* Initialize ASE instance array */
		iter = ARRAY_SIZE(audio_ctx.ase);
		while (iter--) {
			audio_ctx.ase[iter].ase_lid = GAF_INVALID_LID;
			audio_ctx.ase[iter].stream_lid = GAF_INVALID_LID;
			audio_ctx.ase[iter].dir = (iter < uc_srv_cfg.nb_ase_chars_sink)
							  ? ASE_DIR_SINK
							  : ASE_DIR_SOURCE;
			audio_ctx.ase[iter].p_codec_cfg = NULL;
			audio_ctx.ase[iter].p_enable_metadata = NULL;
			audio_ctx.ase[iter].p_update_metadata = NULL;
		}
	}

	/* ------------------------------------------------------------------------------------ */
	/* Step 4: Configure TMAP (Telephony and Media Audio Profile)                           */
	/* ------------------------------------------------------------------------------------ */
	{
		tmap_tmas_cfg_param_t cfg_tmas = {
			/* Support multiple roles: Call Terminal, Media Sender/Receiver, Broadcast
			 */
			.role_bf = TMAP_ROLE_CT_BIT | TMAP_ROLE_BMR_BIT,
			.shdl = GATT_INVALID_HDL,
		};
// SINK
#if CONFIG_HALO_BLE_AUDIO_SINK_ENABLE
		cfg_tmas.role_bf |= TMAP_ROLE_UMR_BIT;
#endif
// SOURCE
#if CONFIG_HALO_BLE_AUDIO_SOURCE_ENABLE
		cfg_tmas.role_bf |= TMAP_ROLE_UMS_BIT;
#endif

		err = tmap_tmas_configure(&cfg_tmas);
		if (err != GAF_ERR_NO_ERROR) {
			LOG_ERR("Unable to configure TMAP! Error %u (0x%02X)", err, err);
			return -1;
		}
	}

	/* ------------------------------------------------------------------------------------ */
	/* Step 5: Configure PACS (Published Audio Capabilities Server)                         */
	/*
	 * Define what codec configurations this device supports.
	 * Client will read these to determine compatible configurations.
	 *
	 * Mandatory configurations per BAP spec:
	 * - Sink: 16_2 (16kHz/10ms/40B), 24_2 (24kHz/10ms/60B)
	 * - Source: 16_2 (16kHz/10ms/40B)
	 * - For media: 48_x variants at 48kHz
	 * ------------------------------------------------------------------------------------ */
	{
		bap_capa_srv_cfg_t capa_srv_cfg = {
		/* PAC characteristics based on enabled features */
#ifdef CONFIG_HALO_BLE_AUDIO_SINK_ENABLE
			.nb_pacs_sink = NB_PAC(ARRAY_SIZE(sink_capas)),
#else
			.nb_pacs_sink = 0,
#endif
#ifdef CONFIG_HALO_BLE_AUDIO_SOURCE_ENABLE
			.nb_pacs_src = NB_PAC(ARRAY_SIZE(source_capas)),
#else
			.nb_pacs_src = 0,
#endif
			.cfg_bf = (BAP_CAPA_SRV_CFG_PAC_NTF_BIT | BAP_CAPA_SRV_CFG_LOC_NTF_BIT |
				   BAP_CAPA_SRV_CFG_SUPP_CONTEXT_NTF_BIT |
				   BAP_CAPA_SRV_CFG_LOC_WR_BIT |
#if CONFIG_ALIF_BLE_ROM_IMAGE_V1_0
				   BAP_CAPA_SRV_CFG_LOC_SUPP_BIT |
#endif
				   BAP_CAPA_SRV_CFG_CHECK_LOCK_BIT),
			.pref_mtu = 0,
			.shdl = GATT_INVALID_HDL,
			.location_bf_sink = LOCATION_SINK,
			.location_bf_src = LOCATION_SOURCE,
		/* Support audio contexts based on enabled features */
#ifdef CONFIG_HALO_BLE_AUDIO_SINK_ENABLE
			.supp_context_bf_sink = BAP_CONTEXT_TYPE_ALL,
#else
			.supp_context_bf_sink = 0,
#endif
#ifdef CONFIG_HALO_BLE_AUDIO_SOURCE_ENABLE
			.supp_context_bf_src = BAP_CONTEXT_TYPE_ALL,
#else
			.supp_context_bf_src = 0,
#endif
		};

		err = bap_capa_srv_configure(&capa_srv_cbs, &capa_srv_cfg);
		if (err != GAF_ERR_NO_ERROR) {
			LOG_ERR("Unable to configure PACS! Error %u (0x%02X)", err, err);
			return -1;
		}

		gaf_codec_id_t codec_id = GAF_CODEC_ID_LC3;
		uint32_t record_id = 1;
		uint32_t pac_lid = 0;
		size_t nb_records = 0;

#ifdef CONFIG_HALO_BLE_AUDIO_SINK_ENABLE
		/* Add sink capability records */
		for (size_t i = 0; i < ARRAY_SIZE(sink_capas); i++) {
			if (nb_records >= NB_RECORDS_PER_PAC_MAX) {
				pac_lid++;
				nb_records = 0;
			}

			bap_capa_t *p_capa = halo_malloc(sizeof(bap_capa_t), HALO_MEM_REGION_AUTO);
			if (!p_capa) {
				LOG_ERR("Failed to allocate sink capa %zu", i);
				return -1;
			}
			p_capa->param.sampling_freq_bf = sink_capas[i].sampling_freq_bf;
			p_capa->param.frame_dur_bf = sink_capas[i].frame_dur_bf;
			p_capa->param.chan_cnt_bf = 1;
			p_capa->param.frame_octet_min = sink_capas[i].frame_octet_min;
			p_capa->param.frame_octet_max = sink_capas[i].frame_octet_max;
			p_capa->param.max_frames_sdu = 1;
			p_capa->add_capa.len = 0;

			err = bap_capa_srv_set_record(pac_lid, record_id, &codec_id, p_capa, NULL);
			if (err != GAF_ERR_NO_ERROR) {
				LOG_ERR("Failed to add sink record %zu: %u", i, err);
				halo_free(p_capa);
				return -1;
			}
			nb_records++;
			record_id++;

			/* Track for cleanup */
			if (audio_ctx.pacs_record_count < MAX_PACS_RECORDS) {
				audio_ctx.pacs_records[audio_ctx.pacs_record_count].p_capa = p_capa;
				audio_ctx.pacs_records[audio_ctx.pacs_record_count].record_id =
					record_id - 1;
				audio_ctx.pacs_record_count++;
			} else {
				LOG_ERR("Too many PACS records (max %d), cannot track capability",
					MAX_PACS_RECORDS);
				halo_free(p_capa);
				return -ENOMEM;
			}
		}
		pac_lid++;
		nb_records = 0;
#endif /* CONFIG_HALO_BLE_AUDIO_SINK_ENABLE */

#ifdef CONFIG_HALO_BLE_AUDIO_SOURCE_ENABLE
		/* Add source capability records */
		for (size_t i = 0; i < ARRAY_SIZE(source_capas); i++) {
			if (nb_records >= NB_RECORDS_PER_PAC_MAX) {
				pac_lid++;
				nb_records = 0;
			}

			bap_capa_t *p_capa = halo_malloc(sizeof(bap_capa_t), HALO_MEM_REGION_AUTO);
			if (!p_capa) {
				LOG_ERR("Failed to allocate source capa %zu", i);
				return -1;
			}
			p_capa->param.sampling_freq_bf = source_capas[i].sampling_freq_bf;
			p_capa->param.frame_dur_bf = source_capas[i].frame_dur_bf;
			p_capa->param.chan_cnt_bf = 1;
			p_capa->param.frame_octet_min = source_capas[i].frame_octet_min;
			p_capa->param.frame_octet_max = source_capas[i].frame_octet_max;
			p_capa->param.max_frames_sdu = 1;
			p_capa->add_capa.len = 0;

			err = bap_capa_srv_set_record(pac_lid, record_id, &codec_id, p_capa, NULL);
			if (err != GAF_ERR_NO_ERROR) {
				LOG_ERR("Failed to add source record %zu: %u", i, err);
				halo_free(p_capa);
				return -1;
			}
			nb_records++;
			record_id++;

			/* Track for cleanup */
			if (audio_ctx.pacs_record_count < MAX_PACS_RECORDS) {
				audio_ctx.pacs_records[audio_ctx.pacs_record_count].p_capa = p_capa;
				audio_ctx.pacs_records[audio_ctx.pacs_record_count].record_id =
					record_id - 1;
				audio_ctx.pacs_record_count++;
			} else {
				LOG_ERR("Too many PACS records (max %d), cannot track capability",
					MAX_PACS_RECORDS);
				halo_free(p_capa);
				return -ENOMEM;
			}
		}
#endif /* CONFIG_HALO_BLE_AUDIO_SOURCE_ENABLE */

		if (!bap_capa_srv_is_configured()) {
			LOG_ERR("PACS is not ready!");
			return -1;
		}
	}

	/* ------------------------------------------------------------------------------------ */
	/* Step 6: Configure VCS (Volume Control Service)                                       */
	/* ------------------------------------------------------------------------------------ */
	{
		/* Load saved volume from persistent storage if available */
		/* Use speaker volume scaled to VCS range (0-255) */
		int speaker_vol = audio_speaker_get_volume(NULL);
		audio_ctx.volume = (speaker_vol * 255) / 100;
		audio_ctx.mute = false; /* Unmuted by default */

		err = arc_vcs_configure(&volume_control_callbacks, VOLUME_CTRL_STEP_SIZE, 0,
					audio_ctx.volume, audio_ctx.mute, GATT_INVALID_HDL,
					ARC_VCS_CFG_FLAGS_NTF_BIT, 0, NULL);
		if (err != GAF_ERR_NO_ERROR) {
			LOG_ERR("Unable to configure VCS! Error %u (0x%02X)", err, err);
			return -1;
		}
		LOG_DBG("VCS (Volume Control Service) is configured");
	}

	/* ------------------------------------------------------------------------------------ */
	/* Step 7: Configure CAP (Common Audio Profile) Acceptor / CAS                          */
	/*
	 * TMAP formally requires the CAP Acceptor role; exposing CAS also lets
	 * CAP-centric hosts (Android LE Audio stack) classify the device correctly.
	 * Not part of a coordinated set, so no CSIS instance is included.
	 *
	 * Failure here is logged but non-fatal: BAP streaming works without CAS and
	 * the previously shipped sink behavior must not regress.
	 * ------------------------------------------------------------------------------------ */
	{
		cap_cas_cfg_param_t cas_cfg = {
			.set_lid = GAF_INVALID_LID,
			.shdl = GATT_INVALID_HDL,
		};

		err = cap_configure(CAP_CFG_CAS_SUPP_BIT, &cas_cfg, NULL);
		if (err != GAF_ERR_NO_ERROR) {
			LOG_ERR("Unable to configure CAP/CAS! Error %u (0x%02X)", err, err);
		} else {
			LOG_DBG("CAP (Common Audio Service) is configured");
		}
	}

#ifdef CONFIG_HALO_BLE_AUDIO_MICS
	/* ------------------------------------------------------------------------------------ */
	/* Step 8: Configure MICS (Microphone Control Service) with an AICS instance            */
	/*
	 * MICS exposes the standard microphone Mute control. An AICS instance for
	 * microphone gain (-10..+10 dB, 1 dB steps, matching the PDM hardware
	 * range shared with frame.microphone.gain()) can be included, but costs a
	 * GATT user slot from the ROM's fixed pool (BLE_GATT_USER_NB = 12), which
	 * is fully subscribed once the ANCS client is enabled - so AICS is opt-in
	 * (CONFIG_HALO_BLE_AUDIO_AICS) and MICS degrades to mute-only without it.
	 *
	 * Like CAS, failures are logged but non-fatal.
	 * ------------------------------------------------------------------------------------ */
	{
		uint8_t input_lid = GAF_INVALID_LID;
		uint8_t nb_inputs = 0;

#ifdef CONFIG_HALO_BLE_AUDIO_AICS
		err = arc_aics_configure(&mic_input_callbacks, 1, 0);
		if (err != GAF_ERR_NO_ERROR) {
			LOG_ERR("Unable to configure AICS! Error %u (0x%02X)", err, err);
			goto aics_done;
		}

		static const char mic_input_desc[] = "Microphones";
		arc_aic_gain_prop_t gain_prop = {
			.gain_units = 10, /* units of 0.1 dB -> 1 dB per gain step */
			.gain_min = -10,
			.gain_max = 10,
		};

		err = arc_aics_add(&gain_prop, ARC_AIC_INPUT_TYPE_MICROPHONE,
				   sizeof(mic_input_desc) - 1, ARC_AICS_CFG_DESC_NTF_BIT,
				   GATT_INVALID_HDL, &input_lid);
		if (err != GAF_ERR_NO_ERROR) {
			LOG_ERR("Unable to add AICS input! Error %u (0x%02X)", err, err);
			input_lid = GAF_INVALID_LID;
			goto aics_done;
		}

		arc_aics_set_description(input_lid, sizeof(mic_input_desc) - 1,
					 (const uint8_t *)mic_input_desc);
		arc_aics_set_gain(input_lid, (uint8_t)(int8_t)audio_microphone_get_gain(NULL));
		arc_aics_set_gain_mode(input_lid, ARC_AIC_GAIN_MODE_MANUAL);
		arc_aics_set_status(input_lid, ARC_AIC_INPUT_STATUS_ACTIVE);

		nb_inputs = 1;
aics_done:
#endif /* CONFIG_HALO_BLE_AUDIO_AICS */

		audio_ctx.mic_input_lid = input_lid;
		audio_ctx.mic_mute = ARC_MIC_MUTE_NOT_MUTED;
		audio_ctx.mic_aics_mute = ARC_AIC_MUTE_NOT_MUTED;

		err = arc_mics_configure(&mic_control_callbacks, GATT_INVALID_HDL,
					 audio_ctx.mic_mute, 0, nb_inputs,
					 nb_inputs ? &input_lid : NULL);
		if (err != GAF_ERR_NO_ERROR) {
			LOG_ERR("Unable to configure MICS! Error %u (0x%02X)", err, err);
		} else {
			LOG_DBG("MICS (Microphone Control Service) is configured%s",
				nb_inputs ? " with AICS gain input" : " (mute only)");
		}
	}
#endif /* CONFIG_HALO_BLE_AUDIO_MICS */

	audio_ctx.initialized = BLE_AUDIO_INIT_MAGIC;

	return 0;
}

uint16_t halo_ble_audio_get_tmap_roles(void)
{
	uint16_t role_bf = TMAP_ROLE_CT_BIT | TMAP_ROLE_BMR_BIT;

#if CONFIG_HALO_BLE_AUDIO_SINK_ENABLE
	role_bf |= TMAP_ROLE_UMR_BIT;
#endif
#if CONFIG_HALO_BLE_AUDIO_SOURCE_ENABLE
	role_bf |= TMAP_ROLE_UMS_BIT;
#endif

	return role_bf;
}

void halo_ble_audio_get_context_types(uint16_t *sink_context_bf, uint16_t *src_context_bf)
{
	if (sink_context_bf) {
#ifdef CONFIG_HALO_BLE_AUDIO_SINK_ENABLE
		*sink_context_bf = BAP_CONTEXT_TYPE_ALL;
#else
		*sink_context_bf = 0;
#endif
	}

	if (src_context_bf) {
#ifdef CONFIG_HALO_BLE_AUDIO_SOURCE_ENABLE
		*src_context_bf = BAP_CONTEXT_TYPE_ALL;
#else
		*src_context_bf = 0;
#endif
	}
}

/**
 * @brief Deinitialize the LE Audio service
 *
 * Cleans up all audio resources.
 *
 * @return 0 on success
 */
int halo_ble_audio_deinit(void)
{
	if (audio_ctx.initialized != BLE_AUDIO_INIT_MAGIC) {
		LOG_WRN("BLE Audio Service not initialized");
		return 0;
	}

	/* Clean up audio datapaths */
	audio_datapath_cleanup_sink();
	audio_datapath_cleanup_source();

	/* Clean up PACS records */
	for (uint8_t i = 0; i < audio_ctx.pacs_record_count; i++) {
		if (audio_ctx.pacs_records[i].p_capa) {
			bap_capa_srv_remove_record(audio_ctx.pacs_records[i].record_id);
			halo_free(audio_ctx.pacs_records[i].p_capa);
			audio_ctx.pacs_records[i].p_capa = NULL;
		}
	}
	audio_ctx.pacs_record_count = 0;

	audio_ctx.initialized = 0;

	LOG_DBG("BLE Audio Service deinitialized");

	return 0;
}

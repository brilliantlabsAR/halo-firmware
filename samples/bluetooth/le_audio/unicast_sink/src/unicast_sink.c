/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "gaf_adv.h"
#include "bap_capa_srv.h"
#include "bap_uc_srv.h"
#include "tmap_tmas.h"
#include "ke_mem.h"
#include "cap_cas.h"
#include "hap_has.h"
#include "hap.h"
#include "arc_vcs.h"

#include "bluetooth/le_audio/audio_utils.h"

#include "unicast_sink.h"
#include "audio_datapath.h"
#include "storage.h"

LOG_MODULE_REGISTER(unicast_sink, CONFIG_UNICAST_SINK_LOG_LEVEL);

/** Default location for sink. */
#if CONFIG_UNICAST_LOCATION_BOTH
#define LOCATION_SINK (GAF_LOC_FRONT_LEFT_BIT | GAF_LOC_FRONT_RIGHT_BIT)
#elif CONFIG_UNICAST_LOCATION_LEFT
#define LOCATION_SINK (GAF_LOC_FRONT_LEFT_BIT)
#elif CONFIG_UNICAST_LOCATION_RIGHT
#define LOCATION_SINK (GAF_LOC_FRONT_RIGHT_BIT)
#else
#define LOCATION_SINK 0
#endif

/** Default location for source */
#if CONFIG_UNICAST_BIDIR
#if CONFIG_UNICAST_LOCATION_RIGHT
#define LOCATION_SOURCE (GAF_LOC_FRONT_RIGHT_BIT)
#else /* CONFIG_UNICAST_LOCATION_LEFT || CONFIG_UNICAST_LOCATION_BOTH */
#define LOCATION_SOURCE (GAF_LOC_FRONT_LEFT_BIT)
#endif
#else
#define LOCATION_SOURCE 0
#endif

#define MAX_NUMBER_OF_ASE         3
/** Only ISOOSHM data path supported */
#define DATA_PATH_CONFIG          DATA_PATH_ISOOSHM
/** Number of simultaneous connections supported */
#define MAX_NUMBER_OF_CONNECTIONS 1
#define CONTROL_DELAY_US          100

/* Smallest retx number which can be used is 8 for now */
/* #define RETX_NUMBER              8 */
/* Smallest tmax which can be used is 40ms for now */
/* #define MAX_TRANSPORT_LATENCY_MS 30 */

#define ADV_SET_LOCAL_IDX     0
#define ADV_TIMEOUT           0 /* Infinite (until explicitly stopped) */
#define ADV_TIMEOUT_DIRECT    5
#define ADV_SID               0
#define ADV_INTERVAL_QUICK_MS 45
#define ADV_INTERVAL_MS       150
#define ADV_PHY               GAP_PHY_1MBPS
#define ADV_PHY_2nd           GAP_PHY_2MBPS
#define ADV_MAX_TX_PWR        -20
#define ADV_MAX_SKIP          1

#define I2S_SINK_SAMPLE_RATE DT_PROP(CODEC_I2S_NODE, sample_rate)

#if I2S_SINK_SAMPLE_RATE != 16000 && I2S_SINK_SAMPLE_RATE != 24000 &&                              \
	I2S_SINK_SAMPLE_RATE != 32000 && I2S_SINK_SAMPLE_RATE != 48000
#error "Invalid sample rate"
#endif

#if DT_NODE_EXISTS(I2S_MIC_NODE)
#define I2S_SOURCE_SAMPLE_RATE DT_PROP(I2S_MIC_NODE, sample_rate)

#if I2S_SOURCE_SAMPLE_RATE != 16000 && I2S_SOURCE_SAMPLE_RATE != 24000 &&                          \
	I2S_SOURCE_SAMPLE_RATE != 32000 && I2S_SOURCE_SAMPLE_RATE != 48000
#error "Invalid microphone sample rate"
#endif

#else
/* Use sink sample rate as default if microphone node is not defined */
#define I2S_SOURCE_SAMPLE_RATE I2S_SINK_SAMPLE_RATE
#endif

enum {
	ASE_DIR_UNKNOWN = 0,
	ASE_DIR_SOURCE,
	ASE_DIR_SINK,
};

struct ase_instance {
	/** Work context */
	struct k_work work;
	/** ASE local index */
	uint8_t ase_lid;
	/** Stream local index */
	uint8_t stream_lid;
	/** Stream direction: Source or Sink */
	uint8_t dir;
};

struct ase_config {
	/** Number of ASEs */
	uint8_t nb_ases;
	/** SDU size */
	uint8_t frame_octet;
	/** Data path configuration */
	struct audio_datapath_config datapath_config;
};

struct unicast_env {
	/** Advertising is ongoing */
	bool advertising_ongoing;
	/** ASE instances */
	struct ase_instance ase[MAX_NUMBER_OF_ASE * MAX_NUMBER_OF_CONNECTIONS];
	/** ASE configuration (Sink direction) */
	struct ase_config ase_config_sink;
	/** ASE configuration (Source direction) */
	struct ase_config ase_config_src;
	/** Total number of ASEs */
	uint8_t total_ases;
};

/** Unicast environment */
static struct unicast_env unicast_env = {
	.advertising_ongoing = false,
};

/** Array providing string description of each ASE state */
static const char *ase_state_name[BAP_UC_ASE_STATE_MAX] = {
	[BAP_UC_ASE_STATE_IDLE] = "Idle",
	[BAP_UC_ASE_STATE_CODEC_CONFIGURED] = "Codec Configured",
	[BAP_UC_ASE_STATE_QOS_CONFIGURED] = "QoS Configured",
	[BAP_UC_ASE_STATE_ENABLING] = "Enabling",
	[BAP_UC_ASE_STATE_STREAMING] = "Streaming",
	[BAP_UC_ASE_STATE_DISABLING] = "Disabling",
	[BAP_UC_ASE_STATE_RELEASING] = "Releasing",
};
/** Array providing string description of each Sampling Frequency */
static const char *sampling_freq_name[BAP_SAMPLING_FREQ_MAX + 1] = {
	[BAP_SAMPLING_FREQ_8000HZ] = "8kHz",       [BAP_SAMPLING_FREQ_11025HZ] = "11.025kHz",
	[BAP_SAMPLING_FREQ_16000HZ] = "16kHz",     [BAP_SAMPLING_FREQ_22050HZ] = "22.050kHz",
	[BAP_SAMPLING_FREQ_24000HZ] = "24kHz",     [BAP_SAMPLING_FREQ_32000HZ] = "32kHz",
	[BAP_SAMPLING_FREQ_44100HZ] = "44.1kHz",   [BAP_SAMPLING_FREQ_48000HZ] = "48kHz",
	[BAP_SAMPLING_FREQ_88200HZ] = "88.2kHz",   [BAP_SAMPLING_FREQ_96000HZ] = "96kHz",
	[BAP_SAMPLING_FREQ_176400HZ] = "176.4kHz", [BAP_SAMPLING_FREQ_192000HZ] = "192kHz",
	[BAP_SAMPLING_FREQ_384000HZ] = "384kHz",
};
/** Array providing string description of each Frame Duration */
static const char *frame_dur_name[BAP_FRAME_DUR_MAX + 1] = {
	[BAP_FRAME_DUR_7_5MS] = "7.5ms",
	[BAP_FRAME_DUR_10MS] = "10ms",
};
/** Array providing string description of each location */
static const char *location_name[GAF_LOC_RIGHT_SURROUND_POS + 1] = {
	[GAF_LOC_FRONT_LEFT_POS] = "FRONT LEFT",
	[GAF_LOC_FRONT_RIGHT_POS] = "FRONT RIGHT",
	[GAF_LOC_FRONT_CENTER_POS] = "CENTER",
	[GAF_LOC_LFE1_POS] = "LFE1",
	[GAF_LOC_BACK_LEFT_POS] = "BACK LEFT",
	[GAF_LOC_BACK_RIGHT_POS] = "BACK RIGHT",
	[GAF_LOC_FRONT_LEFT_CENTER_POS] = "FRONT LEFT CENTER",
	[GAF_LOC_FRONT_RIGHT_CENTER_POS] = "FRONT RIGHT CENTER",
	[GAF_LOC_BACK_CENTER_POS] = "BACK CENTER",
	[GAF_LOC_LFE2_POS] = "LFE2",
	[GAF_LOC_SIDE_LEFT_POS] = "SIDE LEFT",
	[GAF_LOC_SIDE_RIGHT_POS] = "SIDE RIGHT",
	[GAF_LOC_TOP_FRONT_LEFT_POS] = "TOP FRONT LEFT",
	[GAF_LOC_TOP_FRONT_RIGHT_POS] = "TOP FRONT RIGHT",
	[GAF_LOC_TOP_FRONT_CENTER_POS] = "TOP FRONT CENTER",
	[GAF_LOC_TOP_CENTER_POS] = "TOP CENTER",
	[GAF_LOC_TOP_BACK_LEFT_POS] = "TOP BACK LEFT",
	[GAF_LOC_TOP_BACK_RIGHT_POS] = "TOP BACK RIGHT",
	[GAF_LOC_TOP_SIDE_LEFT_POS] = "TOP SIDE LEFT",
	[GAF_LOC_TOP_SIDE_RIGHT_POS] = "TOP SIDE RIGHT",
	[GAF_LOC_TOP_BACK_CENTER_POS] = "TOP BACK CENTER",
	[GAF_LOC_BOTTOM_FRONT_CENTER_POS] = "BOTTOM FRONT CENTER",
	[GAF_LOC_BOTTOM_FRONT_LEFT_POS] = " BOTTOM FRONT LEFT",
	[GAF_LOC_BOTTOM_FRONT_RIGHT_POS] = "BOTTOM FRONT RIGHT",
	[GAF_LOC_FRONT_LEFT_WIDE_POS] = "FRONT LEFT WIDE",
	[GAF_LOC_FRONT_RIGHT_WIDE_POS] = "FRONT RIGHT WIDE",
	[GAF_LOC_LEFT_SURROUND_POS] = "LEFT SURROUND",
	[GAF_LOC_RIGHT_SURROUND_POS] = "RIGHT SURROUND",
};

/* ---------------------------------------------------------------------------------------- */

#define WORKER_PRIORITY   2
#define WORKER_STACK_SIZE 2048

K_KERNEL_STACK_DEFINE(worker_task_stack, WORKER_STACK_SIZE);
static struct k_work_q worker_queue;

static void enable_streaming(struct k_work *const p_job)
{
	/* Small delay is needed due to lower layer latency */
	k_sleep(K_MSEC(2));

	struct ase_instance const *const p_ase = CONTAINER_OF(p_job, struct ase_instance, work);

	if (p_ase->dir == ASE_DIR_SINK) {
		audio_datapath_channel_start_sink(p_ase->stream_lid);
	} else if (p_ase->dir == ASE_DIR_SOURCE) {
		audio_datapath_channel_start_source(p_ase->stream_lid);
	}
}

/* ---------------------------------------------------------------------------------------- */
int get_ase_lid_by_index(size_t const ase_index)
{
	if (ase_index >= ARRAY_SIZE(unicast_env.ase)) {
		return -1;
	}
	return unicast_env.ase[ase_index].ase_lid;
}

int get_ase_index_by_lid(size_t const ase_lid)
{
	if (ase_lid == GAF_INVALID_LID) {
		return -1;
	}

	size_t iter = ARRAY_SIZE(unicast_env.ase);

	while (iter--) {
		if (unicast_env.ase[iter].ase_lid == ase_lid) {
			return iter;
		}
	}
	return -1;
}

void *get_ase_context_by_idx(size_t const ase_index)
{
	if (ase_index >= ARRAY_SIZE(unicast_env.ase)) {
		return NULL;
	}
	return &unicast_env.ase[ase_index];
}

void *get_ase_context_by_lid(size_t const ase_lid)
{
	int const index = get_ase_index_by_lid(ase_lid);

	if (index < 0) {
		return NULL;
	}
	return &unicast_env.ase[index];
}

/* ---------------------------------------------------------------------------------------- */
/* GAF advertising */

static void on_gaf_advertising_cmp_evt(uint8_t const cmd_type, uint16_t const status,
				       uint8_t const set_lid)
{
	(void)set_lid;

	__ASSERT(status == GAF_ERR_NO_ERROR, "GAF advertising error:%u, cmd:%u", status, cmd_type);

	switch (cmd_type) {
	case GAF_ADV_CMD_TYPE_START: {
		LOG_DBG("GAF advertising started");
		unicast_env.advertising_ongoing = true;
		break;
	}
	case GAF_ADV_CMD_TYPE_STOP: {
		LOG_INF("GAF advertising stopped");
		break;
	}
	case GAF_ADV_CMD_TYPE_START_DIRECTED: {
		LOG_DBG("GAF directed advertising started");
		unicast_env.advertising_ongoing = true;
		break;
	}
	case GAF_ADV_CMD_TYPE_START_DIRECTED_FAST: {
		LOG_DBG("GAF high-duty cycle directed advertising started");
		unicast_env.advertising_ongoing = true;
		break;
	}
	default:
		break;
	}
}

static void on_gaf_advertising_stopped(uint8_t const set_lid, uint8_t const reason)
{
	(void)set_lid;

	static const char *const reason_str[] = {"Requested by Upper Layer", "Internal error",
						 "Timeout", "Connection established"};

	LOG_DBG("GAF advertising stopped. Reason: %s",
		reason < ARRAY_SIZE(reason_str) ? reason_str[reason] : "Unknown");

	unicast_env.advertising_ongoing = false;

	if (reason != GAF_ADV_STOP_REASON_CON_ESTABLISHED) {
		/* Restart normal advertising */
		unicast_sink_adv_start(NULL);
	}
}

static const struct gaf_adv_cb gaf_adv_cbs = {
	.cb_cmp_evt = on_gaf_advertising_cmp_evt,
	.cb_stopped = on_gaf_advertising_stopped,
};

/* ---------------------------------------------------------------------------------------- */
/* BAP Unicast Server */

static void on_unicast_server_cb_cmp_evt(uint8_t const cmd_type, uint16_t const status,
					 uint8_t const ase_lid)
{
	switch (cmd_type) {
	case BAP_UC_SRV_CMD_TYPE_DISABLE:
		LOG_INF("Unicast [ASE %u] BAP_UC_SRV_CMD_TYPE_DISABLE (status: %u)", ase_lid,
			status);
		break;
	case BAP_UC_SRV_CMD_TYPE_RELEASE:
		LOG_INF("Unicast [ASE %u] BAP_UC_SRV_CMD_TYPE_RELEASE (status: %u)", ase_lid,
			status);
		break;
	case BAP_UC_SRV_CMD_TYPE_GET_QUALITY:
		LOG_INF("Unicast [ASE %u] BAP_UC_SRV_CMD_TYPE_GET_QUALITY (status: %u)", ase_lid,
			status);
		break;
	default:
		LOG_ERR("Unicast [ASE %u] unknown cmd %u server (error: %u)", ase_lid, cmd_type,
			status);
		break;
	}
}

static void on_unicast_server_cb_quality_cmp_evt(
	uint16_t const status, uint8_t const ase_lid, uint32_t const tx_unacked_packets,
	uint32_t const tx_flushed_packets, uint32_t const tx_last_subevent_packets,
	uint32_t const retransmitted_packets, uint32_t const crc_error_packets,
	uint32_t const rx_unreceived_packets, uint32_t const duplicate_packets)
{
	LOG_DBG("Unicast [ASE %u] quality_cmp_evt (error: %u). TX unack:%u,flush:%u,num:%u"
		" RX retx:%u,crc:%u,unrx:%u,dup:%u",
		ase_lid, status, tx_unacked_packets, tx_flushed_packets, tx_last_subevent_packets,
		retransmitted_packets, crc_error_packets, rx_unreceived_packets, duplicate_packets);
}

static void on_unicast_server_cb_bond_data(uint8_t const conidx, uint8_t const cli_cfg_bf,
					   uint16_t const ase_cli_cfg_bf)
{
	LOG_DBG("ASCS Bond Data updated (conidx: %d, cli_cfg_bf: 0x%02X, ase_cli_cfg_bf: 0x%02X)",
		conidx, cli_cfg_bf, ase_cli_cfg_bf);
}

static void on_unicast_server_cb_ase_state(uint8_t const ase_lid, uint8_t const conidx,
					   uint8_t const state, bap_qos_cfg_t *const p_qos_cfg)
{
	LOG_DBG("ASE %d - %s", ase_lid, ase_state_name[state]);

	switch (state) {
	case BAP_UC_ASE_STATE_IDLE: {
		audio_datapath_cleanup_sink();
		audio_datapath_cleanup_source();
		break;
	}
	case BAP_UC_ASE_STATE_QOS_CONFIGURED: {
		struct ase_instance *const p_ase = get_ase_context_by_lid(ase_lid);

		if (!p_ase) {
			break;
		}

		if (p_ase->dir == ASE_DIR_SINK) {
			audio_datapath_create_sink(&unicast_env.ase_config_sink.datapath_config);
		} else if (p_ase->dir == ASE_DIR_SOURCE) {
			audio_datapath_create_source(&unicast_env.ase_config_src.datapath_config);
		}
		break;
	}
	case BAP_UC_ASE_STATE_ENABLING: {
		break;
	}
	case BAP_UC_ASE_STATE_RELEASING: {
		bap_uc_srv_release_cfm(ase_lid, BAP_UC_CP_RSP_CODE_SUCCESS, 0, true);
		break;
	}
	case BAP_UC_ASE_STATE_STREAMING: {
		struct ase_instance *const p_ase = get_ase_context_by_lid(ase_lid);

		if (!p_ase) {
			break;
		}

		p_ase->work.handler = enable_streaming;
		k_work_submit_to_queue(&worker_queue, &p_ase->work);
		break;
	}
	default: {
		break;
	}
	}
}

static void on_unicast_server_cb_cis_state(uint8_t const stream_lid, uint8_t const conidx,
					   uint8_t const ase_lid_sink, uint8_t const ase_lid_src,
					   uint8_t const cig_id, uint8_t const cis_id,
					   uint16_t const conhdl, gapi_ug_config_t *const p_cig_cfg,
					   gapi_us_config_t *const p_cis_cfg)
{
	LOG_DBG("CIS %d state - conhdl:0x%04X, cig_id:%d, stream_lid:%d, ASE lid sink:%u,source:%u",
		cis_id, conhdl, cig_id, stream_lid, ase_lid_sink, ase_lid_src);

	struct ase_instance *p_ase;

	p_ase = get_ase_context_by_lid(ase_lid_sink);
	if (p_ase) {
		p_ase->stream_lid = stream_lid;
	}

	p_ase = get_ase_context_by_lid(ase_lid_src);
	if (p_ase) {
		p_ase->stream_lid = stream_lid;
	}

	/* Ignore other prints if the handle is undefined */
	if (conhdl == GAP_INVALID_CONHDL) {
		/* state is released, otherwise established */
		return;
	}

	LOG_DBG("  GROUP: sync_delay_us:%u, tlatency_m2s_us:%u, tlatency_s2m_us:%u, "
		"iso_intv_frames:%u",
		p_cig_cfg->sync_delay_us, p_cig_cfg->tlatency_m2s_us, p_cig_cfg->tlatency_s2m_us,
		p_cig_cfg->iso_intv_frames);
	LOG_DBG("  STREAM: sync_delay_us:%u, Max PDU m2s:%u/s2m:%u, PHY m2s:%u/s2m:%u, "
		"flush to m2s:%u/s2m:%u, burst nbr m2s:%u/s2m:%u, nse:%u",
		p_cis_cfg->sync_delay_us, p_cis_cfg->max_pdu_m2s, p_cis_cfg->max_pdu_s2m,
		p_cis_cfg->phy_m2s, p_cis_cfg->phy_s2m, p_cis_cfg->ft_m2s, p_cis_cfg->ft_s2m,
		p_cis_cfg->bn_m2s, p_cis_cfg->bn_s2m, p_cis_cfg->nse);
}

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
	/* Preferred QoS settings reported to Initiator (client) */
	bap_qos_req_t qos_req = {
		/* Presentation delay max must be at least 40ms.
		 * Minimum depends on the sink capabilities to process the audio
		 */
		.pres_delay_min_us = CONFIG_MINIMUM_PRESENTATION_DELAY_US,
		.pres_delay_max_us = CONFIG_MAXIMUM_PRESENTATION_DELAY_US,
		.pref_pres_delay_min_us = CONFIG_MINIMUM_PRESENTATION_DELAY_US,
		.pref_pres_delay_max_us = CONFIG_MAXIMUM_PRESENTATION_DELAY_US,
#if MAX_TRANSPORT_LATENCY_MS
		.trans_latency_max_ms = MAX_TRANSPORT_LATENCY_MS,
#else
		/* Transport latency maximum according to LE audio specification */
		.trans_latency_max_ms = (tgt_latency == BAP_UC_TGT_LATENCY_LOWER) ? 20 : 100,
#endif
		.framing = ISO_UNFRAMED_MODE,                    /** @ref enum iso_frame */
		.phy_bf = (GAP_PHY_LE_1MBPS | GAP_PHY_LE_2MBPS), /** @ref enum gap_phy */
#if RETX_NUMBER
		.retx_nb = RETX_NUMBER,
#else
		/* Retransmission number according to LE audio specification */
		.retx_nb = (tgt_latency == BAP_UC_TGT_LATENCY_LOWER) ? 5 : 13,
#endif
	};
	struct ase_instance *p_ase = get_ase_context_by_lid(ase_lid);

	if (!p_ase) {
		p_ase = get_ase_context_by_idx(ase_instance_idx);
	}
	if (!p_ase) {
		LOG_ERR("  Invalid ASE instance! ase_instance_idx:%u, ase_lid:%u", ase_instance_idx,
			ase_lid);
		goto bap_codec_config_cfm;
	}

	LOG_DBG("ASE %u Configure Codec requested (ASE inst_idx %u, lid %u, conidx %u). "
		"tgt_latency: %u, tgt_phy: %u",
		ase_lid_cfm, ase_instance_idx, ase_lid, conidx, tgt_latency, tgt_phy);

	if (tgt_phy < BAP_UC_TGT_PHY_1M || tgt_phy > BAP_UC_TGT_PHY_2M) {
		LOG_ERR("  Invalid PHY %u", tgt_phy);
		goto bap_codec_config_cfm;
	}

	if (p_codec_id->codec_id[0] != GAPI_CODEC_FORMAT_LC3) {
		LOG_ERR("  Invalid codec %u", p_codec_id->codec_id[0]);
		goto bap_codec_config_cfm;
	}

	/* Allocate codec configuration buffer
	 * NOTE: ke_malloc_user must be used to reserve buffer from correct heap!
	 */
	p_ase_codec_cfg = ke_malloc_user(size, KE_MEM_PROFILE);
	if (!p_ase_codec_cfg) {
		LOG_ERR("Failed to allocate codec configuration buffer");
		goto bap_codec_config_cfm;
	}

	char const *const p_name = location_name[__builtin_ctz(p_cfg->param.location_bf)];

	LOG_DBG("    Codec LC3, Freq: %s, Duration: %s, Length: %dB, Location: %s",
		sampling_freq_name[p_cfg->param.sampling_freq],
		frame_dur_name[p_cfg->param.frame_dur], p_cfg->param.frame_octet, p_name);

	p_ase->ase_lid = ase_lid_cfm;
	p_ase->stream_lid = ase_instance_idx;

	if (p_ase->dir == ASE_DIR_SINK) {
		unicast_env.ase_config_sink.datapath_config.pres_delay_us =
			qos_req.pres_delay_max_us;
		unicast_env.ase_config_sink.datapath_config.sampling_rate_hz =
			audio_bap_sampling_freq_to_hz(p_cfg->param.sampling_freq);
		unicast_env.ase_config_sink.datapath_config.frame_duration_is_10ms =
			p_cfg->param.frame_dur;
		unicast_env.ase_config_sink.frame_octet = p_cfg->param.frame_octet;
	} else if (p_ase->dir == ASE_DIR_SOURCE) {
		unicast_env.ase_config_src.datapath_config.pres_delay_us =
			qos_req.pres_delay_max_us;
		unicast_env.ase_config_src.datapath_config.sampling_rate_hz =
			audio_bap_sampling_freq_to_hz(p_cfg->param.sampling_freq);
		unicast_env.ase_config_src.datapath_config.frame_duration_is_10ms =
			p_cfg->param.frame_dur;
		unicast_env.ase_config_src.frame_octet = p_cfg->param.frame_octet;
	}

	memcpy(&p_ase_codec_cfg->param, &p_cfg->param, sizeof(bap_cfg_param_t));
	p_ase_codec_cfg->add_cfg.len = config_len;
	if (config_len) {
		memcpy(p_ase_codec_cfg->add_cfg.data, p_cfg->p_add_cfg->data,
		       sizeof(p_ase_codec_cfg->add_cfg.len) + config_len);
	}

bap_codec_config_cfm:

	bap_uc_srv_configure_codec_cfm(
		conidx,
		!!p_ase_codec_cfg ? BAP_UC_CP_RSP_CODE_SUCCESS
				  : BAP_UC_CP_RSP_CODE_INSUFFICIENT_RESOURCES,
		0, ase_lid_cfm, &qos_req, p_ase_codec_cfg, CONTROL_DELAY_US, DATA_PATH_CONFIG);
}

#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 /* ROM version > 1.0 */
static void on_unicast_server_cb_configure_qos_req(uint8_t const ase_lid, uint8_t const stream_lid,
						   const bap_qos_cfg_t *const p_qos_cfg)
{
	uint_fast8_t rsp_code = BAP_UC_CP_RSP_CODE_SUCCESS;
	uint8_t reason = 0;

	if (p_qos_cfg->pres_delay_us < CONFIG_MINIMUM_PRESENTATION_DELAY_US ||
	    p_qos_cfg->pres_delay_us > CONFIG_MAXIMUM_PRESENTATION_DELAY_US) {
		rsp_code = BAP_UC_CP_RSP_CODE_UNSUPPORTED_CFG_PARAM;
		reason = BAP_UC_CP_REASON_PRES_DELAY;
	} else {

		struct ase_instance *const p_ase = get_ase_context_by_lid(ase_lid);

		if (p_ase) {
			p_ase->stream_lid = stream_lid;

			if (p_ase->dir == ASE_DIR_SINK) {
				unicast_env.ase_config_sink.datapath_config.pres_delay_us =
					p_qos_cfg->pres_delay_us;
			} else if (p_ase->dir == ASE_DIR_SOURCE) {
				unicast_env.ase_config_src.datapath_config.pres_delay_us =
					p_qos_cfg->pres_delay_us;
			}
		} else {
			rsp_code = BAP_UC_CP_RSP_CODE_INVALID_ASE_ID;
			reason = BAP_UC_CP_REASON_INVALID_ASE_CIS_MAPPING;
		}
	}

	bap_uc_srv_configure_qos_cfm(ase_lid, rsp_code, reason);
	LOG_DBG("Configure QoS requested (ASE %d)", ase_lid);
}
#endif

static void on_unicast_server_cb_enable_req(uint8_t const ase_lid,
					    bap_cfg_metadata_ptr_t *const p_metadata)
{
	uint8_t rsp_code = BAP_UC_CP_RSP_CODE_SUCCESS;
	size_t const metadata_len =
		p_metadata->p_add_metadata ? p_metadata->p_add_metadata->len : 0;
	size_t const size = sizeof(bap_cfg_metadata_t) + metadata_len;
	struct ase_instance const *const p_ase = get_ase_context_by_lid(ase_lid);
	bap_cfg_metadata_t *p_ase_metadata = NULL;

	if (!p_ase) {
		LOG_ERR("Invalid ASE ID %u", ase_lid);
		rsp_code = BAP_UC_CP_RSP_CODE_INVALID_ASE_ID;
		goto bap_cfg_metadata_cfm;
	}

	/* Allocate metadata buffer
	 * NOTE: ke_malloc_user must be used to reserve buffer from correct heap!
	 */
	p_ase_metadata = ke_malloc_user(size, KE_MEM_PROFILE);
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

	if (p_ase->dir == ASE_DIR_SINK) {
		if (audio_datapath_channel_create_sink(unicast_env.ase_config_sink.frame_octet,
						       p_ase->stream_lid)) {
			LOG_ERR("Failed to create sink channel");
			rsp_code = BAP_UC_CP_RSP_CODE_UNSPECIFIED_ERROR;
		}
	} else if (p_ase->dir == ASE_DIR_SOURCE) {
		if (audio_datapath_channel_create_source(unicast_env.ase_config_src.frame_octet,
							 p_ase->stream_lid)) {
			LOG_ERR("Failed to create source channel");
			rsp_code = BAP_UC_CP_RSP_CODE_UNSPECIFIED_ERROR;
		}
	}

bap_cfg_metadata_cfm:
	bap_uc_srv_enable_cfm(ase_lid, rsp_code, 0, p_ase_metadata);

	LOG_DBG("ASE %u enable %s context_bf:%u", ase_lid, rsp_code ? "FAIL" : "SUCCESS",
		p_metadata->param.context_bf);
}

static void on_unicast_server_cb_update_metadata_req(uint8_t const ase_lid,
						     bap_cfg_metadata_ptr_t *const p_metadata)
{
	size_t const metadata_len =
		p_metadata->p_add_metadata ? p_metadata->p_add_metadata->len : 0;
	size_t const size = sizeof(bap_cfg_metadata_t) + metadata_len;
	/* Allocate metadata buffer
	 * NOTE: ke_malloc_user must be used to reserve buffer from correct heap!
	 */
	bap_cfg_metadata_t *p_ase_metadata = ke_malloc_user(size, KE_MEM_PROFILE);

	if (p_ase_metadata) {
		p_ase_metadata->param.context_bf = p_metadata->param.context_bf;
		p_ase_metadata->add_metadata.len = metadata_len;
		if (metadata_len) {
			memcpy(&p_ase_metadata->add_metadata, p_metadata->p_add_metadata,
			       sizeof(p_ase_metadata->add_metadata.len) + metadata_len);
		}
	}

	bap_uc_srv_update_metadata_cfm(ase_lid,
				       p_ase_metadata ? BAP_UC_CP_RSP_CODE_SUCCESS
						      : BAP_UC_CP_RSP_CODE_INSUFFICIENT_RESOURCES,
				       0, p_ase_metadata);

	LOG_DBG("Update metadata requested (ASE %d)", ase_lid);
}

static void on_unicast_server_cb_release_req(uint8_t const ase_lid)
{
	bap_uc_srv_release_cfm(ase_lid, BAP_UC_CP_RSP_CODE_SUCCESS, 0, true);
	LOG_DBG("Release requested (ASE %d)", ase_lid);
}

static void on_unicast_server_cb_dp_update_req(uint8_t const ase_lid, bool const start)
{
	struct ase_instance const *const p_ase = get_ase_context_by_lid(ase_lid);
	bool const valid_config = !!p_ase && p_ase->stream_lid != GAF_INVALID_LID;

	bap_uc_srv_dp_update_cfm(ase_lid, valid_config);

	if (!valid_config) {
		LOG_ERR("Invalid data path configuration for ASE %d", ase_lid);
		return;
	}

	if (!start) {
		if (p_ase->dir == ASE_DIR_SINK) {
			audio_datapath_channel_stop_sink(p_ase->stream_lid);
		} else if (p_ase->dir == ASE_DIR_SOURCE) {
			audio_datapath_channel_stop_source(p_ase->stream_lid);
		}
		return;
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
/* PAC Capability records and server */

static void on_capabilities_server_cb_bond_data(uint8_t const conidx, uint8_t const cli_cfg_bf,
						uint16_t const pac_cli_cfg_bf)
{
	LOG_DBG("PACS Bond Data (conidx:%d, cli_cfg_bf:0x%02X, pac_cli_cfg_bf:0x%02X)", conidx,
		cli_cfg_bf, pac_cli_cfg_bf);
}

static void on_capabilities_server_cb_location_req(uint8_t const conidx,
#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 /* ROM version > 1.0 */
						   uint16_t const token,
#endif
						   uint8_t const direction,
						   uint32_t const location_bf)
{
	LOG_DBG("BAP CAP server_cb_location_req. conidx:%u, dir:%u, location:%u", conidx, direction,
		location_bf);
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
/** bap_capa_t size must be correctly aligned! */
#define BAP_CAPA_SIZE          CO_ALIGN4_HI(sizeof(bap_capa_t))

#if (NB_RECORDS_PER_PAC_MAX == 0)
#error "Invalid value for NB_RECORDS_PER_PAC_MAX"
#endif

enum capa_type {
	CAPA_TYPE_16_7_5MS,
	CAPA_TYPE_16_10MS,
	CAPA_TYPE_24_7_5MS,
	CAPA_TYPE_24_10MS,
	CAPA_TYPE_32_7_5MS,
	CAPA_TYPE_32_10MS,
	CAPA_TYPE_48_7_5MS,
	CAPA_TYPE_48_10MS_1,
	CAPA_TYPE_48_10MS_2,
	CAPA_TYPE_MAX,

	CAPA_TYPE_16_7_5MS_BIT = BIT(CAPA_TYPE_16_7_5MS),
	CAPA_TYPE_16_10MS_BIT = BIT(CAPA_TYPE_16_10MS),
	CAPA_TYPE_24_7_5MS_BIT = BIT(CAPA_TYPE_24_7_5MS),
	CAPA_TYPE_24_10MS_BIT = BIT(CAPA_TYPE_24_10MS),
	CAPA_TYPE_32_7_5MS_BIT = BIT(CAPA_TYPE_32_7_5MS),
	CAPA_TYPE_32_10MS_BIT = BIT(CAPA_TYPE_32_10MS),
	CAPA_TYPE_48_7_5MS_BIT = BIT(CAPA_TYPE_48_7_5MS),
	CAPA_TYPE_48_10MS_1_BIT = BIT(CAPA_TYPE_48_10MS_1),
	CAPA_TYPE_48_10MS_2_BIT = BIT(CAPA_TYPE_48_10MS_2),
};

/** Alloc and initialize the capability record */
static inline void *bap_capa_record_alloc_and_init(enum capa_type const type_bit)
{
	/* Allocate capa record buffer
	 * NOTE: ke_malloc_user must be used to reserve buffer from correct heap!
	 */
	bap_capa_t *p_capa = ke_malloc_user(BAP_CAPA_SIZE, KE_MEM_PROFILE);

	if (!p_capa) {
		__ASSERT(0, "Failed to allocate memory for capability record");
		LOG_ERR("Failed to allocate memory for capability record");
		return NULL;
	}

	switch (type_bit) {
	case CAPA_TYPE_16_7_5MS_BIT:
	case CAPA_TYPE_16_10MS_BIT: {
		p_capa->param.sampling_freq_bf = BAP_SAMPLING_FREQ_16000HZ_BIT;
		p_capa->param.frame_dur_bf = type_bit == CAPA_TYPE_16_10MS_BIT
						     ? BAP_FRAME_DUR_10MS_BIT
						     : BAP_FRAME_DUR_7_5MS_BIT;
		p_capa->param.chan_cnt_bf = 1;
		p_capa->param.frame_octet_min = 40;
		p_capa->param.frame_octet_max = 40;
		p_capa->param.max_frames_sdu = 1;
		p_capa->add_capa.len = 0;
		break;
	}
	case CAPA_TYPE_24_7_5MS_BIT:
	case CAPA_TYPE_24_10MS_BIT: {
		p_capa->param.sampling_freq_bf = BAP_SAMPLING_FREQ_24000HZ_BIT;
		p_capa->param.frame_dur_bf = type_bit == CAPA_TYPE_24_10MS_BIT
						     ? BAP_FRAME_DUR_10MS_BIT
						     : BAP_FRAME_DUR_7_5MS_BIT;
		p_capa->param.chan_cnt_bf = 1;
		p_capa->param.frame_octet_min = 60;
		p_capa->param.frame_octet_max = 60;
		p_capa->param.max_frames_sdu = 1;
		p_capa->add_capa.len = 0;
		break;
	}
	case CAPA_TYPE_32_7_5MS_BIT:
	case CAPA_TYPE_32_10MS_BIT: {
		p_capa->param.sampling_freq_bf = BAP_SAMPLING_FREQ_32000HZ_BIT;
		p_capa->param.frame_dur_bf = type_bit == CAPA_TYPE_32_10MS_BIT
						     ? BAP_FRAME_DUR_10MS_BIT
						     : BAP_FRAME_DUR_7_5MS_BIT;
		p_capa->param.chan_cnt_bf = 1;
		p_capa->param.frame_octet_min = 60;
		p_capa->param.frame_octet_max = 80;
		p_capa->param.max_frames_sdu = 1;
		p_capa->add_capa.len = 0;
		break;
	}
	case CAPA_TYPE_48_7_5MS_BIT:
	case CAPA_TYPE_48_10MS_1_BIT: {
		p_capa->param.sampling_freq_bf = BAP_SAMPLING_FREQ_48000HZ_BIT;
		p_capa->param.frame_dur_bf = type_bit == CAPA_TYPE_48_10MS_1_BIT
						     ? BAP_FRAME_DUR_10MS_BIT
						     : BAP_FRAME_DUR_7_5MS_BIT;
		p_capa->param.chan_cnt_bf = 1;
		p_capa->param.frame_octet_min = 75;
		p_capa->param.frame_octet_max = 155;
		p_capa->param.max_frames_sdu = 1;
		p_capa->add_capa.len = 0;
		break;
	}
	case CAPA_TYPE_48_10MS_2_BIT: {
		p_capa->param.sampling_freq_bf = BAP_SAMPLING_FREQ_48000HZ_BIT;
		p_capa->param.frame_dur_bf = BAP_FRAME_DUR_10MS_BIT;
		p_capa->param.chan_cnt_bf = 1;
		p_capa->param.frame_octet_min = 120;
		p_capa->param.frame_octet_max = 155;
		p_capa->param.max_frames_sdu = 1;
		p_capa->add_capa.len = 0;
		break;
	}
	default: {
		ke_free(p_capa);
		__ASSERT(0, "Invalid capability type");
		LOG_ERR("Invalid capability type");
		p_capa = NULL;
	}
	}

	return p_capa;
}

static uint32_t get_records_sink(void)
{
	/** Following codec capabilities settings are mandatory for Sink direction
	 *     -> 16_2 : LC3 / 16kHz / 10ms / 40 bytes
	 *     -> 24_2 : LC3 / 24kHz / 10ms / 60 bytes
	 * Following codec capabilities are mandatory when Unicast Media Receiver role is
	 * supported
	 *     -> 48_1 : LC3 / 48kHz / 7.5ms / 75 bytes
	 *     -> 48_2 : LC3 / 48kHz / 10ms / 100 bytes
	 *     -> 48_3 : LC3 / 48kHz / 7.5ms / 90 bytes
	 *     -> 48_4 : LC3 / 48kHz / 10ms / 120 bytes
	 *     -> 48_5 : LC3 / 48kHz / 7.5ms / 117 bytes
	 *     -> 48_6 : LC3 / 48kHz / 10ms / 155 bytes
	 * Following codec capabilities are mandatory when Call Terminal role is supported
	 *     -> 32_1 : LC3 / 24kHz / 7.5ms / 60 bytes
	 *     -> 32_2 : LC3 / 24kHz / 10ms / 80 bytes
	 * Following codec capability is mandatory when Call Gateway role is supported
	 *     -> 32_2 : LC3 / 24kHz / 10ms / 80 bytes
	 */

	/* Codec configuration is fixed so use static values until available */
#if DYNAMIC_CODEC_CONFIGURATION_EXISTS

	size_t const role_bitmap = tmap_tmas_get_roles();

	/**
	 * See table 'Unicast Server audio capability support requirements' in BAP 1.0.3
	 * specification
	 * At least 2 record are used, one for 16kHz, one for 24kHz
	 */
	uint32_t nb_record_bits = CAPA_TYPE_16_10MS_BIT + CAPA_TYPE_24_10MS_BIT;

	if (role_bitmap & TMAP_ROLE_UMR_BIT) {
		nb_record_bits += CAPA_TYPE_48_10MS_1_BIT;
	}

	if (role_bitmap & (TMAP_ROLE_CT_BIT | TMAP_ROLE_CG_BIT)) {
		nb_record_bits += CAPA_TYPE_32_10MS_BIT;
	}

	return nb_record_bits;
#else

	uint32_t nb_record_bits = CAPA_TYPE_16_10MS_BIT + CAPA_TYPE_16_7_5MS_BIT;

#if 24000 <= I2S_SINK_SAMPLE_RATE
	nb_record_bits += CAPA_TYPE_24_10MS_BIT + CAPA_TYPE_24_7_5MS_BIT;
#endif
#if 32000 <= I2S_SINK_SAMPLE_RATE
	nb_record_bits += CAPA_TYPE_32_10MS_BIT + CAPA_TYPE_32_7_5MS_BIT;
#endif
#if 48000 <= I2S_SINK_SAMPLE_RATE
	nb_record_bits += CAPA_TYPE_48_10MS_1_BIT + CAPA_TYPE_48_7_5MS_BIT;
#endif
	return nb_record_bits;
#endif
}

static uint32_t get_records_source(void)
{
	/**
	 * Following codec capabilities settings are mandatory for Source direction
	 *     -> 16_2 : LC3 / 16kHz / 10ms / 40 bytes
	 * Following codec capabilities are mandatory when Unicast Media Sender role is
	 * supported
	 *     -> 48_2 : LC3 / 48kHz / 10ms / 100 bytes
	 *     At least one of:
	 *         -> 48_4 : LC3 / 48kHz / 10ms / 120 bytes
	 *         -> 48_6 : LC3 / 48kHz / 10ms / 155 bytes
	 * Following codec capabilities are mandatory when Call Terminal role is supported
	 *     -> 32_2 : LC3 / 32kHz / 10ms / 80 bytes
	 * Following codec capability is mandatory when Call Gateway role is supported
	 *     -> 32_2 : LC3 / 32kHz / 10ms / 80 bytes
	 */

	/* Codec configuration is fixed so use static values until available */
#if DYNAMIC_CODEC_CONFIGURATION_EXISTS

	size_t const role_bitmap = tmap_tmas_get_roles();

	/* At least 1 record is used, for 16kHz */
	uint32_t nb_record_bits = CAPA_TYPE_16_10MS_BIT;

	if (role_bitmap & (TMAP_ROLE_CT_BIT | TMAP_ROLE_CG_BIT)) {
		nb_record_bits += CAPA_TYPE_32_10MS_BIT;
	}

	if (role_bitmap & TMAP_ROLE_UMS_BIT) {
		nb_record_bits += CAPA_TYPE_48_10MS_2_BIT;
	}

	return nb_record_bits;
#else

	uint32_t nb_record_bits = CAPA_TYPE_16_10MS_BIT + CAPA_TYPE_16_7_5MS_BIT;

#if 24000 <= I2S_SOURCE_SAMPLE_RATE
	nb_record_bits += CAPA_TYPE_24_10MS_BIT + CAPA_TYPE_24_7_5MS_BIT;
#endif
#if 32000 <= I2S_SOURCE_SAMPLE_RATE
	nb_record_bits += CAPA_TYPE_32_10MS_BIT + CAPA_TYPE_32_7_5MS_BIT;
#endif
#if 48000 <= I2S_SOURCE_SAMPLE_RATE
	nb_record_bits += CAPA_TYPE_48_10MS_2_BIT;
#endif
	return nb_record_bits;

#endif
}

static void add_capa_reconds(uint32_t codec_bitmap, uint32_t *const p_pac_lid,
			     size_t *const p_record_id)
{
	if (!codec_bitmap) {
		return;
	}

	bap_capa_t *p_capa;
	gaf_codec_id_t const codec_id = GAF_CODEC_ID_LC3;
	size_t nb_records = 0;
	size_t record_id = *p_record_id;
	uint32_t pac_lid = *p_pac_lid;
	uint16_t err;

	while (codec_bitmap) {
		uint32_t const bit = 0x1 << __builtin_ctz(codec_bitmap);

		if (nb_records >= NB_RECORDS_PER_PAC_MAX) {
			pac_lid++;
			nb_records = 0;
		}
		p_capa = bap_capa_record_alloc_and_init(bit);
		if (!p_capa) {
			return;
		}
		err = bap_capa_srv_set_record(pac_lid, record_id, &codec_id, p_capa, NULL);
		if (err != GAF_ERR_NO_ERROR) {
			__ASSERT(0, "BAP capability record %u add failed! error:%u", record_id,
				 err);
			LOG_ERR("BAP capability record %u add failed! error:%u", record_id, err);
			return;
		}
		codec_bitmap &= ~bit;
		nb_records++;
		record_id++;
	}
	*p_pac_lid = pac_lid + 1;
	*p_record_id = record_id;
}

static int configure_bap_capabilities(uint32_t const loc_bf_sink, uint32_t const loc_bf_src)
{
	uint32_t capas_bitmap_sink = get_records_sink();
	uint32_t capas_bitmap_src = get_records_source();
	size_t const nb_records_sink = (loc_bf_sink) ? __builtin_popcount(capas_bitmap_sink) : 0;
	size_t const nb_records_src = (loc_bf_src) ? __builtin_popcount(capas_bitmap_src) : 0;

	/** Number of Sink PAC characteristics */
	size_t const nb_pacs_sink = NB_PAC(nb_records_sink);
	/** Number of Source PAC characteristics */
	size_t const nb_pacs_src = NB_PAC(nb_records_src);

	size_t pac_lid = 0;
	size_t record_id = 1; /* Records start from ID 1 */
	uint16_t err;

	bap_capa_srv_cfg_t capa_srv_cfg = {
		.nb_pacs_sink = nb_pacs_sink,
		.nb_pacs_src = nb_pacs_src,
		.cfg_bf = (BAP_CAPA_SRV_CFG_PAC_NTF_BIT | BAP_CAPA_SRV_CFG_LOC_NTF_BIT |
			   BAP_CAPA_SRV_CFG_SUPP_CONTEXT_NTF_BIT | BAP_CAPA_SRV_CFG_LOC_WR_BIT |
#if CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 /* ROM version == 1.0 */
			   BAP_CAPA_SRV_CFG_LOC_SUPP_BIT |
#endif
			   BAP_CAPA_SRV_CFG_CHECK_LOCK_BIT),
		.pref_mtu = 0,
		.shdl = GATT_INVALID_HDL,
		.location_bf_sink = loc_bf_sink,
		.location_bf_src = loc_bf_src,
		.supp_context_bf_sink = (nb_pacs_sink) ? BAP_CONTEXT_TYPE_ALL : 0,
		.supp_context_bf_src = (nb_pacs_src) ? BAP_CONTEXT_TYPE_ALL : 0,
	};

	err = bap_capa_srv_configure(&capa_srv_cbs, &capa_srv_cfg);
	if (err != GAF_ERR_NO_ERROR) {
		LOG_ERR("BAP capability server configuration failed! error:%u", err);
		return -1;
	}

	if (nb_records_sink) {
		add_capa_reconds(capas_bitmap_sink, &pac_lid, &record_id);
	}

	if (nb_records_src) {
		add_capa_reconds(capas_bitmap_src, &pac_lid, &record_id);
	}

	/* Sanity check that BAP capability server is ready */
	if (!bap_capa_srv_is_configured()) {
		LOG_ERR("BAP capability server is not ready!");
		return -1;
	}

	LOG_DBG("BAP capability server is configured");

	return 0;
}

/* ---------------------------------------------------------------------------------------- */

/* Telephony and Media Audio Service */
static int init_tmap(void)
{
	uint32_t err;
	tmap_tmas_cfg_param_t cfg_tmas = {
		.role_bf = TMAP_ROLE_CT_BIT | TMAP_ROLE_UMS_BIT | TMAP_ROLE_UMR_BIT |
			   TMAP_ROLE_BMR_BIT,
		.shdl = GATT_INVALID_HDL,
	};

	err = tmap_tmas_configure(&cfg_tmas);
	if (err != GAF_ERR_NO_ERROR) {
		LOG_ERR("Unable to configure Telephony and Media Audio Service! Error %u (0x%02X)",
			err, err);
		return -1;
	}

	LOG_DBG("Telephony and Media Audio Service (TMAP) is configured");

	return 0;
}

/* ---------------------------------------------------------------------------------------- */
/* Volume Control Service */

struct volume {
	uint8_t volume;
	bool mute;
};

static struct volume env_volume = {
	.volume = 60,
	.mute = false,
};

static void volume_renderer_cb_bond_data(uint8_t const conidx, uint8_t const cli_cfg_bf)
{
	LOG_DBG("Volume Control Server Bond Data updated (conidx = %d, cli_cfg_bf = 0x%02X)",
		conidx, cli_cfg_bf);
}

static void volume_renderer_cb_volume(uint8_t const volume, uint8_t mute, bool const local)
{
	LOG_DBG("Volume updated (volume = %d, mute = %d, local = %d)", volume, mute, local);

	/* Codec API uses 0.5dB steps so divide by 2 to fit into 0...127.
	 *   WM8904 codec uses 1dB steps so total volume will be divided by 4 to match
	 *   codec's 0..63 range.
	 */
	audio_datapath_channel_volume_sink((volume >> 1), mute);

	env_volume.volume = volume;
	env_volume.mute = mute;
	storage_save(SETTINGS_NAME_VOLUME, &env_volume, sizeof(env_volume));
}

static void volume_renderer_cb_flags(uint8_t const flags)
{
	LOG_DBG("Volume Control Server Flags updated (flags = 0x%02X)", flags);
}

int init_volume_control_service(void)
{
	uint32_t err;

	storage_load(SETTINGS_NAME_VOLUME, &env_volume, sizeof(env_volume));

	audio_datapath_channel_volume_sink((env_volume.volume >> 1), env_volume.mute);

	static const arc_vcs_cb_t cbs_arc_vcs = {
		.cb_bond_data = volume_renderer_cb_bond_data,
		.cb_volume = volume_renderer_cb_volume,
		.cb_flags = volume_renderer_cb_flags,
	};

	err = arc_vcs_configure(&cbs_arc_vcs, CONFIG_VOLUME_CTRL_STEP_SIZE, 0, env_volume.volume,
				env_volume.mute, GATT_INVALID_HDL, ARC_VCS_CFG_FLAGS_NTF_BIT, 0,
				NULL);
	if (err != GAF_ERR_NO_ERROR) {
		LOG_ERR("Unable to configure Volume Control Server! Error %u (0x%02X)", err, err);
		return -1;
	}

	LOG_DBG("Volume Control Server is configured");

	return 0;
}

/* ---------------------------------------------------------------------------------------- */

int unicast_sink_init(void)
{
	k_work_queue_start(&worker_queue, worker_task_stack,
			   K_KERNEL_STACK_SIZEOF(worker_task_stack), WORKER_PRIORITY, NULL);
	k_thread_name_set(&worker_queue.thread, "unicast_srv_workq");

	size_t iter;
	uint16_t err;
	struct gaf_adv_cfg config = {
		.nb_sets = 1,
	};

	err = gaf_adv_configure(&config, &gaf_adv_cbs);
	if (err != GAF_ERR_NO_ERROR) {
		LOG_ERR("Unable to configure GAF advertiser! Error %u (0x%02X)", err, err);
		return -1;
	}
	LOG_DBG("GAF advertiser is configured");

	unicast_env.ase_config_sink.nb_ases = __builtin_popcount(LOCATION_SINK);
	unicast_env.ase_config_src.nb_ases = __builtin_popcount(LOCATION_SOURCE);
	LOG_INF("Number of sink:%u, source:%u", unicast_env.ase_config_sink.nb_ases,
		unicast_env.ase_config_src.nb_ases);

	unicast_env.total_ases =
		unicast_env.ase_config_sink.nb_ases + unicast_env.ase_config_src.nb_ases;

	if (unicast_env.total_ases > MAX_NUMBER_OF_ASE) {
		LOG_ERR("Number of ASEs exceeds maximum allowed (%u)", MAX_NUMBER_OF_ASE);
		return -1;
	}

	bap_uc_srv_cfg_t uc_srv_cfg = {
		.nb_ase_chars_sink = unicast_env.ase_config_sink.nb_ases,
		.nb_ase_chars_src = unicast_env.ase_config_src.nb_ases,
		.nb_ases_cfg = unicast_env.total_ases * MAX_NUMBER_OF_CONNECTIONS,
		.cfg_bf = 0,
		.pref_mtu = GAP_LE_MAX_OCTETS,
		.shdl = GATT_INVALID_HDL,
	};

	err = bap_uc_srv_configure(&bap_uc_srv_cb, &uc_srv_cfg);
	if (err != GAF_ERR_NO_ERROR) {
		LOG_ERR("Unable to configure BAP unicast server! Error %u (0x%02X)", err, err);
		return -1;
	}

	/* Sanity check that BAP capability server is ready */
	if (!bap_uc_srv_is_configured()) {
		LOG_ERR("BAP unicast server is not ready!");
		return -1;
	}

	LOG_DBG("BAP unicast server is configured");

	if (init_tmap()) {
		return -1;
	}

	iter = ARRAY_SIZE(unicast_env.ase);
	while (iter--) {
		unicast_env.ase[iter].ase_lid = GAF_INVALID_LID;
		unicast_env.ase[iter].stream_lid = GAF_INVALID_LID;
		unicast_env.ase[iter].dir =
			(iter < uc_srv_cfg.nb_ase_chars_sink) ? ASE_DIR_SINK : ASE_DIR_SOURCE;
	}

	err = configure_bap_capabilities(LOCATION_SINK, LOCATION_SOURCE);
	if (err != GAF_ERR_NO_ERROR) {
		LOG_ERR("Unable to configure BAP capabilities! Error %u (0x%02X)", err, err);
		return -1;
	}

	LOG_DBG("BAP capabilities are configured");

	if (init_volume_control_service()) {
		return -1;
	}

	return 0;
}

int unicast_sink_adv_start(void const *const p_address)
{
	if (unicast_env.advertising_ongoing) {
		LOG_DBG("...advertising already ongoing...");
		return 0;
	}

	extern const char device_name[];

	size_t const name_len = strlen(device_name);
	uint16_t err;
	char adv_data[32];

	adv_data[0] = name_len + 1;
	adv_data[1] = GAP_AD_TYPE_COMPLETE_NAME;
	strncpy(&adv_data[2], device_name, sizeof(adv_data) - 2);

	err = gaf_adv_set_params(ADV_SET_LOCAL_IDX, ADV_INTERVAL_QUICK_MS, ADV_INTERVAL_MS, ADV_PHY,
				 ADV_PHY_2nd, ADV_ALL_CHNLS_EN, ADV_MAX_TX_PWR, ADV_MAX_SKIP);
	if (err != GAF_ERR_NO_ERROR) {
		LOG_ERR("Failed to set advertising params, err %u (0x%02X)", err, err);
		return -1;
	}

	gap_bdaddr_t const *const p_client_addr = p_address;

	uint32_t adv_config = GAPM_ADV_MODE_GEN_DISC;
#if CONFIG_PRIVACY_ENABLED
	adv_config |= GAF_ADV_CFG_PRIVACY_BIT;
#endif

	if (p_client_addr && p_client_addr->addr_type != 0xff) {
		LOG_INF("Starting directed advertising with address %02X:%02X:%02X:%02X:%02X:%02X",
			p_client_addr->addr[5], p_client_addr->addr[4], p_client_addr->addr[3],
			p_client_addr->addr[2], p_client_addr->addr[1], p_client_addr->addr[0]);
#if CONFIG_USE_DIRECTED_FAST_ADVERTISING
		err = gaf_adv_start_directed_fast(ADV_SET_LOCAL_IDX, adv_config, p_client_addr);
#else
		err = gaf_adv_start_directed(ADV_SET_LOCAL_IDX, adv_config, ADV_TIMEOUT_DIRECT,
					     ADV_SID, (name_len + 2), (uint8_t *)adv_data, NULL,
					     p_client_addr);
#endif
	} else {
		LOG_INF("Starting general advertising");
		err = gaf_adv_start(
			ADV_SET_LOCAL_IDX, (adv_config | GAF_ADV_CFG_GENERAL_ANNOUNCEMENT_BIT),
			ADV_TIMEOUT, ADV_SID, (name_len + 2), (uint8_t *)adv_data, NULL);
	}
	if (err != GAF_ERR_NO_ERROR) {
		LOG_ERR("Failed to start advertising, err %u (0x%02X)", err, err);
		return -1;
	}

	return 0;
}

int unicast_sink_adv_stop(void)
{
	if (!unicast_env.advertising_ongoing) {
		LOG_DBG("...advertising not ongoing...");
		return 0;
	}

	uint16_t const err = gaf_adv_stop(ADV_SET_LOCAL_IDX);

	if (err != GAF_ERR_NO_ERROR) {
		LOG_ERR("Failed to stop advertising, err %u (0x%02X)", err, err);
		return -1;
	}

	return 0;
}

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

#include "bap_capa_cli.h"
#include "bap_uc_cli.h"
#include "ke_mem.h"

#include "bluetooth/le_audio/audio_utils.h"

#include "main.h"
#include "unicast_source.h"
#include "storage.h"
#include "audio_datapath.h"

LOG_MODULE_REGISTER(unicast_source, CONFIG_UNICAST_SOURCE_LOG_LEVEL);

#ifndef CONFIG_NUMBER_OF_CLIENTS
#define CONFIG_NUMBER_OF_CLIENTS 2
#endif

#define STREAM_SINK_ASE_MAX_CNT 2
#if CONFIG_LE_AUDIO_BIDIRECTIONAL
#define STREAM_SOURCE_ASE_MAX_CNT 1
#else
#define STREAM_SOURCE_ASE_MAX_CNT 0
#endif
#define STREAM_ASE_MAX_CNT (STREAM_SINK_ASE_MAX_CNT + STREAM_SOURCE_ASE_MAX_CNT)

#define SINK_PAC_MAX_CNT   10
#define SOURCE_PAC_MAX_CNT 10

#define DISCOVER_PACS_BEFORE_ASCS 1

#define CONFIGURE_ALL_FROM_CMP_EVT 1

#define PRESENTATION_DELAY_US (CONFIG_LE_AUDIO_PRESENTATION_DELAY_MS * 1000)
#define CONTROL_DELAY_US      100
#define DATA_PATH_CONFIG      DATA_PATH_ISOOSHM
#define STREAM_PHY_TYPE       BAP_UC_TGT_PHY_2M

#define DEFAULT_FRAME_OCTETS 120

#define LATENCY_TARGET_LOWER    1
#define LATENCY_TARGET_BALANCED 2
#define LATENCY_TARGET_RELIABLE 3

#if CONFIG_LE_AUDIO_TARGET_LATENCY == LATENCY_TARGET_RELIABLE
#define BAP_UC_LINK_TYPE         BAP_UC_TGT_LATENCY_RELIABLE
#define MAX_TRANSPORT_LATENCY_MS 40 /* 40 = ok, 100 = nok */
#define RETX_NUMBER              0  /* 13 */

#elif CONFIG_LE_AUDIO_TARGET_LATENCY == LATENCY_TARGET_LOWER
#define BAP_UC_LINK_TYPE         BAP_UC_TGT_LATENCY_LOWER
#define MAX_TRANSPORT_LATENCY_MS 20
#define RETX_NUMBER              5

#elif CONFIG_LE_AUDIO_TARGET_LATENCY == LATENCY_TARGET_BALANCED
#error "Not supported by the standard... todo check"
#define BAP_UC_LINK_TYPE         BAP_UC_TGT_LATENCY_BALENCED
#define MAX_TRANSPORT_LATENCY_MS 30
#define RETX_NUMBER              8

#else
#error "Invalid latency target"
#endif

/* FIXME: A single group cannot be used for bidirection and multiple acceptors */
#define UC_GROUP_PER_PEER (CONFIG_LE_AUDIO_BIDIRECTIONAL && (CONFIG_NUMBER_OF_CLIENTS > 1))

/** Group type values */
enum unicast_client_group_type {
	/** Media */
	UNICAST_CLIENT_GROUP_TYPE_MEDIA = 0U,
	/** Call */
	UNICAST_CLIENT_GROUP_TYPE_CALL,
	/** Ringtone */
	UNICAST_CLIENT_GROUP_TYPE_RINGTONE,
	/** Keep this last */
	UNICAST_CLIENT_GROUP_TYPE_MAX,
};

enum ase_state_bits {
	ASE_STATE_ZERO = 0,
	ASE_STATE_INITIALIZED_POS = 0,
	ASE_STATE_INITIALIZED = BIT(ASE_STATE_INITIALIZED_POS),
	ASE_STATE_CODEC_CONFIGURED_POS = 1,
	ASE_STATE_CODEC_CONFIGURED = BIT(ASE_STATE_CODEC_CONFIGURED_POS),
	ASE_STATE_QOS_CONFIGURED_POS = 2,
	ASE_STATE_QOS_CONFIGURED = BIT(ASE_STATE_QOS_CONFIGURED_POS),
	ASE_STATE_ENABLED_POS = 4,
	ASE_STATE_ENABLED = BIT(ASE_STATE_ENABLED_POS),
	ASE_STATE_STREAMING_POS = 3,
	ASE_STATE_STREAMING = BIT(ASE_STATE_STREAMING_POS),
};

enum {
	ASE_DIR_UNKNOWN = 0,
	ASE_DIR_SOURCE,
	ASE_DIR_SINK,
};

/** ASE structure */
struct unicast_client_ase {
	/** Work context */
	struct k_work work;
	/** ASE instance media type \ref unicast_client_group_type */
	uint8_t type;
	/** Stream direction: Source or Sink */
	uint8_t dir;
	/** ASE local index */
	uint8_t ase_lid;
	/** ASE instance index */
	uint8_t ase_instance_idx;
	/** CIS ID */
	uint8_t cis_id;
	/** Number of octets */
	uint8_t number_of_octets;
	/** Maximum packet size */
	uint8_t max_sdu_size;
	/** Stream index */
	uint8_t stream_lid;
	/** \ref enum ase_state_bits for state bitmask info*/
	uint8_t state_bitmask;
	/** Retransmission number */
	uint8_t retx_number;
	/** Presentation delay in microseconds */
	uint32_t presentation_delay;
	/** Audio datapath channel create function */
	audio_datapath_channel_create_t channel_create;
	/** Audio datapath channel start function */
	audio_datapath_start_stop_channel_t channel_start;
	/** Audio datapath channel stop function */
	audio_datapath_start_stop_channel_t channel_stop;
};

enum pac_type {
	PAC_UNKNOWN = 0,
	PAC_SOURCE,
	PAC_SINK,
};

struct pac_capa {
	enum pac_type type;
	uint32_t sampling_freq_hz;
	uint8_t frame_duration_bf;
	uint8_t frame_octet_min;
	uint8_t frame_octet_max;
	uint8_t max_frames_sdu;
};

struct unicast_peer {
	/** Connection index */
	uint8_t conidx;

	/** ASE information for Sink and Source direction */
	struct unicast_client_ase ase[STREAM_ASE_MAX_CNT];

	/** Status bit field */
	uint8_t status_bf;

	/** PACS data from server */
	struct pac_capa pacs_sink[SINK_PAC_MAX_CNT];
	struct pac_capa pacs_source[SOURCE_PAC_MAX_CNT];
	uint8_t nb_pacs_sink;
	uint8_t nb_pacs_source;
#if UC_GROUP_PER_PEER
	uint8_t group_lid;
#endif
	/** Bonding data */
	struct bap_uc_cli_ascs ascs_bond_data;
};

struct unicast_source_env {
	struct audio_datapath_config datapath_config;
	struct unicast_peer peers[CONFIG_NUMBER_OF_CLIENTS];
#if !UC_GROUP_PER_PEER
	/** Group local index */
	uint8_t group_lid[UNICAST_CLIENT_GROUP_TYPE_MAX];
#endif
};

static struct unicast_source_env unicast_env;

#define WORKER_PRIORITY   6
#define WORKER_STACK_SIZE 2048

K_KERNEL_STACK_DEFINE(worker_task_stack, WORKER_STACK_SIZE);
static struct k_work_q worker_queue;

K_SEM_DEFINE(bap_complete_sem, 0, 1);

/* ---------------------------------------------------------------------------------------- */

int wait_bap_complete(void)
{
	if (k_sem_take(&bap_complete_sem, K_MSEC(2000))) {
		LOG_ERR("Failed to get BAP complete semaphore!");
		return -1;
	}
	return 0;
}
#define bap_ready_sem_give() k_sem_give(&bap_complete_sem)

int alloc_unicast_env_for_conidx(uint8_t const conidx)
{
	/* Find first free context and mark it to used */
	for (size_t index = 0; index < ARRAY_SIZE(unicast_env.peers); index++) {
		if (unicast_env.peers[index].conidx == GAP_INVALID_CONIDX) {
			unicast_env.peers[index].conidx = conidx;
			return 0;
		}
	}
	return -ENOMEM;
}

void *get_unicast_env_by_index(size_t const index)
{
	if (index < ARRAY_SIZE(unicast_env.peers)) {
		return &unicast_env.peers[index];
	}

	LOG_ERR("Invalid peer index %u", index);
	return NULL;
}

void *get_unicast_env_by_connection_index(uint8_t const conidx)
{
	for (size_t index = 0; index < ARRAY_SIZE(unicast_env.peers); index++) {
		if (unicast_env.peers[index].conidx == conidx) {
			return &unicast_env.peers[index];
		}
	}

	LOG_ERR("Connection index %u not found", conidx);
	return NULL;
}

/* ---------------------------------------------------------------------------------------- */

static uint32_t bap_sampling_freq_bitmask_to_hz(uint32_t const bitmask)
{
	return audio_bap_sampling_freq_to_hz((!bitmask) ? 0 : 32 - __builtin_clz(bitmask));
}

static void *get_ase_config_by_index(size_t const con_lid, size_t const ase_index)
{
	struct unicast_peer *p_unicast_env = get_unicast_env_by_connection_index(con_lid);

	if (!p_unicast_env) {
		return NULL;
	}

	if (ase_index < ARRAY_SIZE(p_unicast_env->ase)) {
		return &p_unicast_env->ase[ase_index];
	}
	return NULL;
}

static void *get_ase_config_by_id(size_t const ase_lid)
{
	size_t iter, index;
	struct unicast_client_ase *p_ase;

	if (ase_lid == GAF_INVALID_LID) {
		return NULL;
	}

	index = ARRAY_SIZE(unicast_env.peers);
	while (index--) {
		if (unicast_env.peers[index].conidx == GAP_INVALID_CONIDX) {
			continue;
		}
		iter = ARRAY_SIZE(unicast_env.peers[index].ase);
		while (iter--) {
			p_ase = &unicast_env.peers[index].ase[iter];
			if (p_ase->ase_lid == ase_lid) {
				return p_ase;
			}
		}
	}
	return NULL;
}

/* ---------------------------------------------------------------------------------------- */

static void datapath_start(struct k_work *p_job)
{
	k_sleep(K_MSEC(20));

	struct unicast_client_ase *const p_ase =
		CONTAINER_OF(p_job, struct unicast_client_ase, work);
	uint_fast8_t const mask = p_ase->state_bitmask;

	if (GAF_INVALID_LID == p_ase->ase_lid || GAF_INVALID_LID == p_ase->cis_id ||
	    !(mask & ASE_STATE_STREAMING)) {
		LOG_ERR("Invalid ASE %u or CIS %u or state %u", p_ase->ase_lid, p_ase->cis_id,
			mask);
		return;
	}

	p_ase->channel_start(p_ase->stream_lid);

	LOG_INF("Channel %u, CIS %u: active", p_ase->ase_lid, p_ase->cis_id);
}

static void enable_streaming(struct k_work *const p_job)
{
	struct unicast_client_ase *const p_ase =
		CONTAINER_OF(p_job, struct unicast_client_ase, work);

	bap_cfg_metadata_t const cfg_metadata = {
		.param.context_bf =
#if CONFIG_LE_AUDIO_BIDIRECTIONAL
			BAP_CONTEXT_TYPE_CONVERSATIONAL_BIT,
#else
			BAP_CONTEXT_TYPE_MEDIA_BIT,
#endif
		.add_metadata.len = 0,
	};
	uint16_t err;
	uint_fast8_t const mask = p_ase->state_bitmask;

	if (GAF_INVALID_LID == p_ase->ase_lid || GAF_INVALID_LID == p_ase->cis_id ||
	    !(mask & ASE_STATE_QOS_CONFIGURED) || (mask & ASE_STATE_ENABLED)) {
		return;
	}

	p_ase->work.handler = datapath_start;

	LOG_INF("ASE %u enabling...", p_ase->ase_lid);

	size_t retry_count = 5;

	while (retry_count--) {
		err = bap_uc_cli_enable(p_ase->ase_lid, &cfg_metadata);
		if (err == GAF_ERR_COMMAND_DISALLOWED) {
			/* Enable came a bit too near the other command(s). Delay a bit and retry */
			k_sleep(K_MSEC(1));
			continue;
		} else if (err != GAF_ERR_NO_ERROR) {
			LOG_ERR("Failed to enable ASE %u! error %u", p_ase->ase_lid, err);
			__ASSERT(0, "Failed to enable ASE");
			return;
		}
		break;
	}
}

static void enable_streaming_all_ase(struct unicast_peer *p_unicast_env)
{
	k_sem_reset(&bap_complete_sem);

	for (size_t iter = 0; iter < ARRAY_SIZE(p_unicast_env->ase); iter++) {
		struct unicast_client_ase *const p_ase = &p_unicast_env->ase[iter];

		if (!p_ase->work.handler) {
			continue;
		}
		k_work_submit_to_queue(&worker_queue, &p_ase->work);
	}
}

/* TODO: Handle graceful shutdown of the streaming */
#if STREAM_DISABLE_IMPL
static void disable_streaming(struct k_work *const p_job)
{
	uint16_t err;

	for (size_t iter = 0; iter < ARRAY_SIZE(p_unicast_env->ase); iter++) {
		struct unicast_client_ase *const p_ase = &p_unicast_env->ase[iter];
		uint_fast8_t const mask = p_ase->state_bitmask;

		if (GAF_INVALID_LID == p_ase->ase_lid || GAF_INVALID_LID == p_ase->cis_id ||
		    !(mask & ASE_STATE_ENABLED)) {
			continue;
		}

		LOG_INF("ASE %u disabling...", p_ase->ase_lid);
		err = bap_uc_cli_disable(p_ase->ase_lid);
		if (err != GAF_ERR_NO_ERROR) {
			LOG_ERR("Failed to disable ASE %u! error %u", p_ase->ase_lid, err);
			continue;
		}
		wait_bap_complete();
		p_ase->state_bitmask = mask & ~(ASE_STATE_STREAMING | ASE_STATE_ENABLED);

		/* TODO: just stop...??? */
		/* audio_datapath_cleanup_source(); */
	}
}

static K_WORK_DEFINE(disable_job, disable_streaming);
#endif

static void configure_qos(struct unicast_peer *p_unicast_env)
{
	LOG_INF("QoS configuration...");

	k_sem_reset(&bap_complete_sem);

	uint_fast16_t err;

	for (size_t iter = 0; iter < ARRAY_SIZE(p_unicast_env->ase); iter++) {
		struct unicast_client_ase *p_ase = &p_unicast_env->ase[iter];

		if (GAF_INVALID_LID == p_ase->ase_lid || GAF_INVALID_LID == p_ase->cis_id ||
		    !(p_ase->state_bitmask & ASE_STATE_CODEC_CONFIGURED)) {
			LOG_INF("Skip QoS config: %u", iter);
			continue;
		}

		p_ase->work.handler = enable_streaming;
		p_ase->max_sdu_size = p_ase->number_of_octets;

		struct bap_uc_cli_qos_cfg qos_cfg = {
			.phy = STREAM_PHY_TYPE,
#if RETX_NUMBER
			.retx_nb = RETX_NUMBER,
#else
			.retx_nb = p_ase->retx_number,
#endif
			.max_sdu_size = p_ase->max_sdu_size,
			.pres_delay_us = p_ase->presentation_delay,
		};

		LOG_DBG("ASE %u, CIS %u QoS configing: phy %u, retx:%u, max_sdu:%uB, "
			"pres_delay:%uus",
			p_ase->ase_lid, p_ase->cis_id, qos_cfg.phy, qos_cfg.retx_nb,
			qos_cfg.max_sdu_size, qos_cfg.pres_delay_us);

		if (p_ase->dir == ASE_DIR_SOURCE) {
			audio_datapath_create_sink(&unicast_env.datapath_config);
		} else if (p_ase->dir == ASE_DIR_SINK) {
			audio_datapath_create_source(&unicast_env.datapath_config);
		} else {
			LOG_ERR("Invalid ASE direction %u", p_ase->dir);
			__ASSERT(0, "Invalid ASE direction");
			continue;
		}

		err = bap_uc_cli_configure_qos(p_ase->ase_lid,
#if UC_GROUP_PER_PEER
					       p_unicast_env->group_lid,
#else
					       unicast_env.group_lid[p_ase->type],
#endif
					       p_ase->cis_id, &qos_cfg);
		if (err != GAF_ERR_NO_ERROR) {
			LOG_ERR("ASE %u, CIS %u Failed to configure qos! error %u", p_ase->ase_lid,
				p_ase->cis_id, err);
			continue;
		}

		wait_bap_complete();
	}

	LOG_INF("QoS configuration ready");
}

static void *get_best_stream(struct pac_capa *p_pac_base, size_t count)
{
	struct pac_capa *p_pac = NULL;
	uint32_t sampling_freq_hz = 0;
	while (count--) {
		/* Use 32kHz 7.5ms frame duration if available */
		if ((32000 == p_pac_base[count].sampling_freq_hz) &&
		    p_pac_base[count].frame_duration_bf & BAP_FRAME_DUR_7_5MS_BIT) {
			p_pac = &p_pac_base[count];
			break;
		}
		/* Otherwise use the best 10ms sampling freq available */
		if ((p_pac_base[count].frame_duration_bf & BAP_FRAME_DUR_10MS_BIT) &&
		    sampling_freq_hz < p_pac_base[count].sampling_freq_hz) {
			sampling_freq_hz = p_pac_base[count].sampling_freq_hz;
			p_pac = &p_pac_base[count];
		}
	}
	return p_pac;
}

static void *bap_config_alloc_and_init(enum bap_frame_dur const frame_dur,
				       struct pac_capa const *const p_pac,
				       uint32_t const location_bf)
{
	/* Allocate config buffer
	 * NOTE: ke_malloc_user must be used to reserve buffer from correct heap!
	 */
	struct bap_cfg *p_config = ke_malloc_user((sizeof(*p_config) + 8), KE_MEM_PROFILE);

	if (!p_config) {
		__ASSERT(0, "Failed to allocate memory for codec capability");
		LOG_ERR("Failed to allocate memory for codec capability");
		return NULL;
	}

	/* TODO: SDU size bigger than 84B cause issues with encoding */
	p_config->param.location_bf = location_bf;
	p_config->param.frame_octet = ROUND_UP(DEFAULT_FRAME_OCTETS, sizeof(uint32_t));
	p_config->param.sampling_freq = audio_hz_to_bap_sampling_freq(p_pac->sampling_freq_hz);
	p_config->param.frame_dur = frame_dur;
	p_config->param.frames_sdu = p_pac->max_frames_sdu;
	p_config->add_cfg.len = 0;

	if (p_pac->frame_octet_min == p_pac->frame_octet_max) {
		p_config->param.frame_octet = p_pac->frame_octet_max;
	} else if (p_pac->frame_octet_max < p_config->param.frame_octet) {
		p_config->param.frame_octet = p_pac->frame_octet_max;
	} else if (p_config->param.frame_octet < p_pac->frame_octet_min) {
		p_config->param.frame_octet = p_pac->frame_octet_min;
	}

	return p_config;
}

static int configure_codec(struct unicast_peer *const p_unicast_env)
{
	k_sem_reset(&bap_complete_sem);

	uint_fast8_t const conidx = p_unicast_env->conidx;

	static const char *const ase_type[] = {
		[ASE_DIR_UNKNOWN] = "INVALID",
		[ASE_DIR_SOURCE] = "SOURCE",
		[ASE_DIR_SINK] = "SINK",
	};
	uint_fast8_t const type = UNICAST_CLIENT_GROUP_TYPE_MEDIA;

	struct pac_capa *p_pac =
		get_best_stream(p_unicast_env->pacs_sink, p_unicast_env->nb_pacs_sink);

	if (!p_pac) {
		LOG_WRN("Unable to find a valid PAC");
		return -1;
	}

	uint32_t sdu_intv_us = 0;
	enum bap_frame_dur frame_dur = BAP_FRAME_DUR_10MS;

	if ((p_pac->frame_duration_bf & BAP_FRAME_DUR_7_5MS_BIT) &&
	    !(p_pac->frame_duration_bf & BAP_FRAME_DUR_10MS_PREF_BIT)) {
		sdu_intv_us = 7500;
		frame_dur = BAP_FRAME_DUR_7_5MS;
	} else if (p_pac->frame_duration_bf & BAP_FRAME_DUR_10MS_BIT) {
		sdu_intv_us = 10000;
	}
	if (!sdu_intv_us) {
		__ASSERT(0, "Invalid frame duration");
		LOG_ERR("Invalid frame duration");
		return -1;
	}

	LOG_INF("Starting streaming with sampling frequency: %uHz, SDU interval: %uus",
		p_pac->sampling_freq_hz, sdu_intv_us);

	bap_uc_cli_grp_param_t grp_param = {
		.sdu_intv_m2s_us = sdu_intv_us,
		.sdu_intv_s2m_us = sdu_intv_us,
		.packing = ISO_PACKING_SEQUENTIAL, /** @ref enum iso_packing */
		.framing = ISO_UNFRAMED_MODE,      /** @ref enum iso_frame */
		.sca = 0,
		.tlatency_m2s_ms = MAX_TRANSPORT_LATENCY_MS,
		.tlatency_s2m_ms = MAX_TRANSPORT_LATENCY_MS,
	};
	uint16_t err;

#if UC_GROUP_PER_PEER
	if (p_unicast_env->group_lid == GAF_INVALID_LID) {
		err = bap_uc_cli_create_group(1 + type + conidx, &grp_param,
					      &p_unicast_env->group_lid);
		if (err != GAF_ERR_NO_ERROR) {
			LOG_ERR("Failed to create group! error %u", err);
			return -1;
		}

		LOG_INF("ASE group %u created", p_unicast_env->group_lid);
	}
#else
	if (unicast_env.group_lid[type] == GAF_INVALID_LID) {
		err = bap_uc_cli_create_group(1 + type, &grp_param, &unicast_env.group_lid[type]);
		if (err != GAF_ERR_NO_ERROR) {
			LOG_ERR("Failed to create group! error %u", err);
			return -1;
		}

		LOG_INF("ASE group %u created", unicast_env.group_lid[type]);
	}
#endif

	gaf_codec_id_t codec_id = GAF_CODEC_ID_LC3;

	uint32_t const ase_lid_base = ARRAY_SIZE(p_unicast_env->ase) * p_unicast_env->conidx;

	for (size_t iter = 0; iter < ARRAY_SIZE(p_unicast_env->ase); iter++) {
		struct unicast_client_ase *const p_ase = &p_unicast_env->ase[iter];

		if (p_ase->ase_lid != GAF_INVALID_LID || p_ase->dir == ASE_DIR_UNKNOWN) {
			LOG_DBG("ASE %u is invalid - skip", iter);
			continue;
		}

		/* TODO read positions from PAC */
		struct bap_cfg *p_config = bap_config_alloc_and_init(
			frame_dur, p_pac, CO_BIT(GAF_LOC_FRONT_LEFT_POS + iter));

		if (!p_config) {
			LOG_ERR("Failed to allocate memory for codec config");
			return -1;
		}

		p_ase->work.handler = NULL;
		p_ase->number_of_octets = p_config->param.frame_octet;
		p_ase->ase_lid = ase_lid_base + iter;
		p_ase->cis_id = (type << 2) + p_ase->ase_lid;
		p_ase->type = type;

		if (p_ase->dir == ASE_DIR_SINK) {
			/* Server is working as a sink so create a source channels */
			p_ase->channel_create = audio_datapath_channel_create_source;
			p_ase->channel_start = audio_datapath_channel_start_source;
			p_ase->channel_stop = audio_datapath_channel_stop_source;
		} else if (p_ase->dir == ASE_DIR_SOURCE) {
			/* Server is working as a source so create a sink channels */
			p_ase->channel_create = audio_datapath_channel_create_sink;
			p_ase->channel_start = audio_datapath_channel_start_sink;
			p_ase->channel_stop = audio_datapath_channel_stop_sink;
		} else {
			LOG_ERR("Invalid ASE direction %u", p_ase->dir);
			__ASSERT(0, "Invalid ASE direction");
			continue;
		}

		LOG_DBG("Codec %s setup: conidx %u, ase_instance_idx %u, ase_lid %u, "
			"octets %uB",
			ase_type[p_ase->dir], conidx, p_ase->ase_instance_idx, p_ase->ase_lid,
			p_config->param.frame_octet);

		err = bap_uc_cli_configure_codec(conidx, p_ase->ase_instance_idx, p_ase->ase_lid,
						 DATA_PATH_CONFIG, &codec_id, BAP_UC_LINK_TYPE,
						 STREAM_PHY_TYPE, CONTROL_DELAY_US, p_config);
		if (err != GAF_ERR_NO_ERROR) {
			LOG_ERR("Failed to configure codec! error %u", err);
			return -1;
		}

		wait_bap_complete();
	}

	LOG_INF("Codec setup ready");

	return 0;
}

/* ---------------------------------------------------------------------------------------- */
/** BAP Unicast Client callbacks */

static void on_bap_uc_cli_cmp_evt(uint8_t const cmd_type, uint16_t const status,
				  uint8_t const con_lid, uint8_t const ase_lid,
				  uint8_t const char_type)
{
	static const char *const cmd_type_str[] = {"DISCOVER",  "CONFIGURE_CODEC", "CONFIGURE_QOS",
						   "ENABLE",    "UPDATE META",     "DISABLE",
						   "RELEASE",   "GET_QUALITY",     "SET_CFG",
						   "GET_STATE", "GROUP_REMOVE",    "CIS_CONTROL"};

	if (status != GAF_ERR_NO_ERROR) {
		LOG_ERR("BAP command '%s' failed with status %u",
			cmd_type < ARRAY_SIZE(cmd_type_str) ? cmd_type_str[cmd_type] : "??",
			status);
		return;
	}

	LOG_DBG("BAP command '%s' completed status:%u, con_lid:%u, ase_lid:%u, char_type:%u",
		cmd_type < ARRAY_SIZE(cmd_type_str) ? cmd_type_str[cmd_type] : "??", status,
		con_lid, ase_lid, char_type);

	struct unicast_client_ase *p_ase = get_ase_config_by_id(ase_lid);

	switch (cmd_type) {
	case BAP_UC_CLI_CMD_TYPE_DISCOVER: {
#if DISCOVER_PACS_BEFORE_ASCS
		peer_ready(con_lid);
#else
		/* Discover PACS */
		uint16_t const err =
			bap_capa_cli_discover(con_lid, GATT_INVALID_HDL, GATT_INVALID_HDL);

		if (err != GAF_ERR_NO_ERROR) {
			LOG_ERR("PACS discovery start failed. Error:%u", err);
		}
#endif
		break;
	}
	case BAP_UC_CLI_CMD_TYPE_CONFIGURE_CODEC: {
		if (p_ase) {
			p_ase->state_bitmask |= ASE_STATE_CODEC_CONFIGURED;
		}
		LOG_DBG("Codec configured %p", p_ase);
		break;
	}
	case BAP_UC_CLI_CMD_TYPE_CONFIGURE_QOS: {
		if (p_ase) {
			p_ase->state_bitmask |= ASE_STATE_QOS_CONFIGURED;

			/*
			if (p_ase->work.handler) {
				k_work_submit_to_queue(&worker_queue, &p_ase->work);
			}
			*/
		}
		break;
	}
	case BAP_UC_CLI_CMD_TYPE_ENABLE: {
		if (p_ase) {
			p_ase->state_bitmask |= ASE_STATE_ENABLED;
		}
		break;
	}
	case BAP_UC_CLI_CMD_TYPE_UPDATE_METADATA: {
		break;
	}
	case BAP_UC_CLI_CMD_TYPE_DISABLE: {
		break;
	}
	case BAP_UC_CLI_CMD_TYPE_RELEASE: {
		break;
	}
	case BAP_UC_CLI_CMD_TYPE_GET_QUALITY: {
		break;
	}
	case BAP_UC_CLI_CMD_TYPE_SET_CFG: {
		break;
	}
	case BAP_UC_CLI_CMD_TYPE_GET_STATE: {
		break;
	}
	case BAP_UC_CLI_CMD_TYPE_REMOVE_GROUP: {
		break;
	}
	case BAP_UC_CLI_CMD_TYPE_CIS_CONTROL: {
		break;
	}
	default: {
		break;
	}
	}
}

static void on_bap_uc_cli_quality_cmp_evt(
	uint16_t const status, uint8_t const ase_lid, uint32_t const tx_unacked_packets,
	uint32_t const tx_flushed_packets, uint32_t const tx_last_subevent_packets,
	uint32_t const retransmitted_packets, uint32_t const crc_error_packets,
	uint32_t const rx_unreceived_packets, uint32_t const duplicate_packets)
{
	LOG_DBG("BAP UC CLI quality event: status %u, ase_lid %u", status, ase_lid);
}

static void on_bap_uc_cli_bond_data(uint8_t const con_lid,
				    struct bap_uc_cli_ascs *const p_ascs_info)
{
	struct unicast_peer *p_unicast_env = get_unicast_env_by_connection_index(con_lid);

	if (!p_unicast_env) {
		return;
	}

	uint8_t const nb_chars =
		p_ascs_info->nb_ases_sink + p_ascs_info->nb_ases_src + BAP_UC_CHAR_TYPE_ASE;
	uint16_t const size =
		sizeof(bap_uc_cli_ascs_t) + (nb_chars * sizeof(bap_uc_cli_ascs_char_t));

	LOG_DBG("PEER %u Bond data: ASCS - char cnt:%u, size:%uB, nb of sink:%u/source:%u", con_lid,
		nb_chars, size, p_ascs_info->nb_ases_sink, p_ascs_info->nb_ases_src);

	for (uint8_t iter = 0; iter < nb_chars; iter++) {
		bap_uc_cli_ascs_char_t const *const p_char = &p_ascs_info->char_info[iter];

		LOG_DBG("ASE %u, val_hdl:%u, desc_hdl:%u", p_char->ase_id, p_char->val_hdl,
			p_char->desc_hdl);
	}

	size_t const nb_ases_sink = MIN(p_ascs_info->nb_ases_sink, STREAM_SINK_ASE_MAX_CNT);
	size_t const nb_ases_source = MIN(p_ascs_info->nb_ases_src, STREAM_SOURCE_ASE_MAX_CNT);

	for (size_t iter = 0; iter < nb_ases_sink; iter++) {
		p_unicast_env->ase[iter].dir = ASE_DIR_SINK;
	}

	for (size_t iter = nb_ases_sink; iter < (nb_ases_sink + nb_ases_source); iter++) {
		p_unicast_env->ase[iter].dir = ASE_DIR_SOURCE;
	}
}

static void on_bap_uc_cli_error(uint8_t const ase_lid, uint8_t const opcode, uint8_t const rsp_code,
				uint8_t const reason)
{
	LOG_ERR("ASE %u opcode: %u, rsp_code: %u, reason: %u", ase_lid, opcode, rsp_code, reason);
}

static void handle_cis_state(struct unicast_client_ase *const p_ase, uint8_t const stream_lid,
			     uint8_t const event)
{
	if (!p_ase) {
		return;
	}

	switch (event) {
	case BAP_UC_CLI_CIS_EVENT_ASE_BOUND: {
		/* An ASE has been bound with the Stream */
		p_ase->stream_lid = stream_lid;

		__ASSERT(p_ase->channel_create != NULL, "Channel create callback is not set");

		int const retval = p_ase->channel_create(p_ase->number_of_octets, stream_lid);

		if (retval) {
			__ASSERT(0, "Failed to create datapath channel. error %d", retval);
			LOG_ERR("Failed to create datapath channel. error %d", retval);
		}
		break;
	}
	case BAP_UC_CLI_CIS_EVENT_ASE_UNBOUND: {
		/* TODO: is the Codec and/or QoS remove needed? */

		/* An ASE has been unbound from the Stream */
		p_ase->state_bitmask = ASE_STATE_ZERO;
		/* TODO convert datapath to reference based */
		audio_datapath_cleanup_source();
		audio_datapath_cleanup_sink();
		break;
	}
	case BAP_UC_CLI_CIS_EVENT_ESTABLISHED: {
		/* CIS has been successfully established */
		p_ase->state_bitmask |= ASE_STATE_STREAMING;
		break;
	}
	case BAP_UC_CLI_CIS_EVENT_FAILED: {
		/* CIS has failed to be established */
		break;
	}
	case BAP_UC_CLI_CIS_EVENT_DISCONNECTED: {
		/* CIS has been disconnected or connection has been lost */
		p_ase->state_bitmask = ~(ASE_STATE_STREAMING | ASE_STATE_ENABLED);
		break;
	}
	};
}

static void on_bap_uc_cli_cis_state(uint8_t const stream_lid, uint8_t const event,
				    uint8_t const con_lid, uint8_t const ase_lid_sink,
				    uint8_t const ase_lid_src, uint8_t const grp_lid,
				    uint8_t const cis_id, uint16_t const conhdl,
				    gapi_ug_config_t *const p_cig_cfg,
				    gapi_us_config_t *const p_cis_cfg)
{
	static const char *const cis_state_str[] = {
		[BAP_UC_CLI_CIS_EVENT_ASE_BOUND] = "ASE_BOUND",
		[BAP_UC_CLI_CIS_EVENT_ASE_UNBOUND] = "ASE_UNBOUND",
		[BAP_UC_CLI_CIS_EVENT_ESTABLISHED] = "ESTABLISHED",
		[BAP_UC_CLI_CIS_EVENT_FAILED] = "FAILED",
		[BAP_UC_CLI_CIS_EVENT_DISCONNECTED] = "DISCONNECTED",
	};

	LOG_DBG("CIS %u state %s - stream %u (conhdl:%u), ASE sink:%u,source:%u", cis_id,
		event < ARRAY_SIZE(cis_state_str) ? cis_state_str[event] : "??", stream_lid, conhdl,
		ase_lid_sink, ase_lid_src);

	if (event == BAP_UC_CLI_CIS_EVENT_FAILED) {
		__ASSERT(false, "CIS failed to be established");
		LOG_ERR("CIS failed to be established");
		/* TODO: implement disconnect handling if connection is lost... if the GAF does not
		 * handle it automatically
		 */
		/* k_work_submit_to_queue(&worker_queue, &disable_job); */
		return;
	}

	if (conhdl != GAP_INVALID_CONHDL) {
		/* state is established */
		LOG_DBG("  GROUP: sync_delay_us:%u, tlatency_m2s_us:%u, tlatency_s2m_us:%u, "
			"iso_intv_frames:%u",
			p_cig_cfg->sync_delay_us, p_cig_cfg->tlatency_m2s_us,
			p_cig_cfg->tlatency_s2m_us, p_cig_cfg->iso_intv_frames);
		LOG_DBG("  STREAM: sync_delay_us:%u, Max PDU m2s:%u/s2m:%u, PHY m2s:%u/s2m:%u, "
			"flush to m2s:%u/s2m:%u, burst nbr m2s:%u/s2m:%u, nse:%u",
			p_cis_cfg->sync_delay_us, p_cis_cfg->max_pdu_m2s, p_cis_cfg->max_pdu_s2m,
			p_cis_cfg->phy_m2s, p_cis_cfg->phy_s2m, p_cis_cfg->ft_m2s,
			p_cis_cfg->ft_s2m, p_cis_cfg->bn_m2s, p_cis_cfg->bn_s2m, p_cis_cfg->nse);
	}

	struct unicast_client_ase *p_ase;

	p_ase = get_ase_config_by_id(ase_lid_sink);
	if (p_ase) {
		handle_cis_state(p_ase, stream_lid, event);
	}

	p_ase = get_ase_config_by_id(ase_lid_src);
	if (p_ase) {
		handle_cis_state(p_ase, stream_lid, event);
	}
}

static void on_bap_uc_cli_state_empty(uint8_t const con_lid, uint8_t const ase_instance_idx,
				      uint8_t const ase_lid, uint8_t const state)
{
	static const char *const states_str[] = {"IDLE",     "CODEC_CONFIGURED", "QOS_CONFIGURED",
						 "ENABLING", "STREAMING",        "DISABLING",
						 "RELEASING"};
	struct unicast_client_ase *p_ase;

	LOG_DBG("ASE state '%s': con_lid:%u, ase_inst_idx:%u, ase_lid:%u",
		state < ARRAY_SIZE(states_str) ? states_str[state] : "??", con_lid,
		ase_instance_idx, ase_lid);

	p_ase = get_ase_config_by_id(ase_lid);
	if (!p_ase) {
		p_ase = get_ase_config_by_index(con_lid, ase_instance_idx);
	}
	if (!p_ase) {
		return;
	}

	switch (state) {
	case BAP_UC_ASE_STATE_IDLE: {
		/* Initialize ASE state */
		p_ase->ase_instance_idx = ase_instance_idx;
		p_ase->ase_lid =
			(con_lid == GAP_INVALID_CONIDX && ase_instance_idx == GAF_INVALID_LID)
				? GAF_INVALID_LID
				: ase_lid;
		p_ase->cis_id = GAF_INVALID_LID;
		p_ase->stream_lid = GAF_INVALID_LID;
		p_ase->state_bitmask = ASE_STATE_ZERO;

		LOG_DBG("ASE IDLE: %u", p_ase->ase_lid);
		break;
	}
	case BAP_UC_ASE_STATE_CODEC_CONFIGURED: {
		break;
	}
	case BAP_UC_ASE_STATE_QOS_CONFIGURED: {
		break;
	}
	case BAP_UC_ASE_STATE_ENABLING: {
		break;
	}
	case BAP_UC_ASE_STATE_STREAMING: {
		break;
	}
	case BAP_UC_ASE_STATE_DISABLING: {
		break;
	}
	case BAP_UC_ASE_STATE_RELEASING: {
		break;
	}
	default: {
		break;
	}
	}
}

static void on_bap_uc_cli_state_codec(uint8_t const con_lid, uint8_t const ase_instance_idx,
				      uint8_t const ase_lid, gaf_codec_id_t *const p_codec_id,
				      bap_qos_req_t *const p_qos_req,
				      const bap_cfg_ptr_t *const p_cfg)
{
	static const char *sampling_freq_name[BAP_SAMPLING_FREQ_MAX + 1] = {
		[BAP_SAMPLING_FREQ_8000HZ] = "8kHz",
		[BAP_SAMPLING_FREQ_11025HZ] = "11.025kHz",
		[BAP_SAMPLING_FREQ_16000HZ] = "16kHz",
		[BAP_SAMPLING_FREQ_22050HZ] = "22.050kHz",
		[BAP_SAMPLING_FREQ_24000HZ] = "24kHz",
		[BAP_SAMPLING_FREQ_32000HZ] = "32kHz",
		[BAP_SAMPLING_FREQ_44100HZ] = "44.1kHz",
		[BAP_SAMPLING_FREQ_48000HZ] = "48kHz",
		[BAP_SAMPLING_FREQ_88200HZ] = "88.2kHz",
		[BAP_SAMPLING_FREQ_96000HZ] = "96kHz",
		[BAP_SAMPLING_FREQ_176400HZ] = "176.4kHz",
		[BAP_SAMPLING_FREQ_192000HZ] = "192kHz",
		[BAP_SAMPLING_FREQ_384000HZ] = "384kHz",
	};
	static const char *frame_dur_name[BAP_FRAME_DUR_MAX + 1] = {
		[BAP_FRAME_DUR_7_5MS] = "7.5ms",
		[BAP_FRAME_DUR_10MS] = "10ms",
	};

	if (p_codec_id->codec_id[GAF_CODEC_ID_FORMAT_POS] != GAPI_CODEC_FORMAT_LC3) {
		LOG_ERR("ASE codec configured: Not LC3 coded! ignored...");
		return;
	}

	LOG_DBG("ASE codec configured: con_lid %u, ase_instance_idx %u, ase_lid %u", con_lid,
		ase_instance_idx, ase_lid);
	LOG_DBG("    Freq: %s, Duration: %s, Length: %dB, Location: %u",
		sampling_freq_name[p_cfg->param.sampling_freq],
		frame_dur_name[p_cfg->param.frame_dur], p_cfg->param.frame_octet,
		p_cfg->param.location_bf);
	LOG_DBG("    QoS: pres_delay_us %u..%u, pref_pres_delay_us %u..%u, trans_latency_max_ms "
		"%u, retx_nb %u, framing %u, phy_bf %u",
		p_qos_req->pres_delay_min_us, p_qos_req->pres_delay_max_us,
		p_qos_req->pref_pres_delay_min_us, p_qos_req->pref_pres_delay_max_us,
		p_qos_req->trans_latency_max_ms, p_qos_req->retx_nb, p_qos_req->framing,
		p_qos_req->phy_bf);

	unicast_env.datapath_config.sampling_rate_hz =
		audio_bap_sampling_freq_to_hz(p_cfg->param.sampling_freq);
	unicast_env.datapath_config.frame_duration_is_10ms = p_cfg->param.frame_dur;

	struct unicast_client_ase *const p_ase = get_ase_config_by_id(ase_lid);

	if (!p_ase) {
		LOG_ERR("Invalid ASE");
		return;
	}

	if (p_ase->ase_lid != ase_lid) {
		LOG_ERR("ASE LID mismatch");
		return;
	}

	/* p_ase->state_bitmask |= ASE_STATE_CODEC_CONFIGURED; */
	p_ase->retx_number = p_qos_req->retx_nb;
	p_ase->presentation_delay = MIN(MAX(p_qos_req->pres_delay_min_us, PRESENTATION_DELAY_US),
					p_qos_req->pres_delay_max_us);

	bap_ready_sem_give();
}

static void on_bap_uc_cli_state_qos(uint8_t const ase_lid, const bap_qos_cfg_t *const p_qos_cfg)
{
	LOG_DBG("ASE %u QoS configured: pres_delay:%uus, trans_latency_max:%ums, retx_nb %u, "
		"framing:%u, phy:%u, max_sdu_size:%u, sdu_intv_us:%u",
		ase_lid, p_qos_cfg->pres_delay_us, p_qos_cfg->trans_latency_max_ms,
		p_qos_cfg->retx_nb, p_qos_cfg->framing, p_qos_cfg->phy, p_qos_cfg->max_sdu_size,
		p_qos_cfg->sdu_intv_us);

	bap_ready_sem_give();

	/* TODO: move datapath configuration params here */
}

static void on_bap_uc_cli_state_metadata(uint8_t const ase_lid, uint8_t const state,
					 const bap_cfg_metadata_ptr_t *const p_metadata)
{
	static const char *const states_str[] = {
		"IDLE",      "CODEC_CONFIGURED", "QOS_CONFIGURED", "ENABLING",
		"STREAMING", "DISABLING",        "RELEASING",
	};

	LOG_DBG("ASE %u metadata state %s", ase_lid,
		state < ARRAY_SIZE(states_str) ? states_str[state] : "??");

	switch (state) {
	case BAP_UC_ASE_STATE_IDLE: {
		break;
	}
	case BAP_UC_ASE_STATE_CODEC_CONFIGURED: {
		break;
	}
	case BAP_UC_ASE_STATE_QOS_CONFIGURED: {
		break;
	}
	case BAP_UC_ASE_STATE_ENABLING: {
		break;
	}
	case BAP_UC_ASE_STATE_STREAMING: {
		break;
	}
	case BAP_UC_ASE_STATE_DISABLING: {
		break;
	}
	case BAP_UC_ASE_STATE_RELEASING: {
		break;
	}
	default: {
		__ASSERT(false, "Invalid metadata state");
	}
	}
}

static void on_bap_uc_cli_svc_changed(uint8_t const con_lid)
{
	LOG_DBG("ASE service changed event: con_lid %u", con_lid);
}

static void on_bap_uc_cli_dp_update_req(uint8_t const ase_lid, bool const start)
{
	LOG_DBG("ASE %u data path %s request", ase_lid, start ? "START" : "STOP");

	bap_uc_cli_dp_update_cfm(ase_lid, true);

	struct unicast_client_ase *const p_ase = get_ase_config_by_id(ase_lid);

	if (!p_ase || !p_ase->work.handler) {
		return;
	}

	if (start) {
		k_work_submit_to_queue(&worker_queue, &p_ase->work);
		return;
	}

	audio_datapath_channel_stop_source(ase_lid);
}

const struct bap_uc_cli_cb bap_cli_cbs = {
	.cb_cmp_evt = on_bap_uc_cli_cmp_evt,
	.cb_quality_cmp_evt = on_bap_uc_cli_quality_cmp_evt,
	.cb_bond_data = on_bap_uc_cli_bond_data,
	.cb_error = on_bap_uc_cli_error,
	.cb_cis_state = on_bap_uc_cli_cis_state,
	.cb_state_empty = on_bap_uc_cli_state_empty,
	.cb_state_codec = on_bap_uc_cli_state_codec,
	.cb_state_qos = on_bap_uc_cli_state_qos,
	.cb_state_metadata = on_bap_uc_cli_state_metadata,
	.cb_svc_changed = on_bap_uc_cli_svc_changed,
	.cb_dp_update_req = on_bap_uc_cli_dp_update_req,
};

/* ---------------------------------------------------------------------------------------- */

static void on_bap_capa_client_cb_cmp_evt(uint8_t const cmd_type, uint16_t status,
					  uint8_t const con_lid, uint8_t const param_1,
					  uint8_t const pac_lid)
{
	/* cmd_type: enum bap_capa_cli_cmd_type */
	static const char *const cmd_str[] = {"DISCOVER", "GET", "SET_CFG", "SET_LOCATION"};

	LOG_DBG("BAP Capabilities completed: cmd:%s, status:%u, conlid:%u, pac_lid:%u",
		cmd_type < ARRAY_SIZE(cmd_str) ? cmd_str[cmd_type] : "??", status, con_lid,
		pac_lid);

	__ASSERT(status == GAF_ERR_NO_ERROR, "BAP Capa Client cmd failed!");

	if (BAP_CAPA_CLI_CMD_TYPE_DISCOVER == cmd_type) {
#if DISCOVER_PACS_BEFORE_ASCS
		status = bap_uc_cli_discover(con_lid, GATT_MIN_HDL, GATT_MAX_HDL);
		if (status != GAF_ERR_NO_ERROR) {
			LOG_ERR("ACSC discovery start failed. Error:%u", status);
		}
#else
		peer_ready(con_lid);
#endif
	}
}

static void on_bap_capa_client_cb_bond_data(uint8_t const con_lid,
					    bap_capa_cli_pacs_t *const p_pacs_info)
{
	struct unicast_peer *p_unicast_env = get_unicast_env_by_connection_index(con_lid);

	if (!p_unicast_env) {
		return;
	}

	uint8_t const nb_chars =
		p_pacs_info->nb_pacs_sink + p_pacs_info->nb_pacs_src + BAP_UC_CHAR_TYPE_ASE;
	uint16_t const size =
		sizeof(bap_capa_cli_pacs_char_t) + (nb_chars * sizeof(bap_capa_cli_pacs_char_t));

	LOG_DBG("Bond data: PACS - char cnt:%u, size:%uB, nb of sink:%u/source:%u, opt_feat_bf:%u",
		nb_chars, size, p_pacs_info->nb_pacs_sink, p_pacs_info->nb_pacs_src,
		p_pacs_info->opt_feat_bf);

	p_unicast_env->nb_pacs_sink = p_pacs_info->nb_pacs_sink;
	p_unicast_env->nb_pacs_source = p_pacs_info->nb_pacs_src;
}

static void on_bap_capa_client_cb_record(uint8_t const con_lid, uint8_t pac_lid,
					 uint8_t const record_lid, uint8_t const nb_records,
					 const gaf_codec_id_t *const p_codec_id,
					 const bap_capa_ptr_t *const p_capa,
					 const bap_capa_metadata_ptr_t *const p_metadata)
{
	if (!nb_records) {
		LOG_WRN("BAP Capabilities record: No records! ignored...");
		return;
	}
	if (p_codec_id->codec_id[GAF_CODEC_ID_FORMAT_POS] != GAPI_CODEC_FORMAT_LC3) {
		LOG_ERR("BAP Capabilities record: Not LC3 coded! ignored...");
		return;
	}

	uint32_t const sampling_freq_hz =
		bap_sampling_freq_bitmask_to_hz(p_capa->param.sampling_freq_bf);

	LOG_DBG("BAP Capa: PAC id:%u, record id:%u, nb_records:%u, sampling_freq:%uHz, "
		"frame_dur_bf:%u, chan_cnt_bf:%u, frame_octet_min:%u, "
		"frame_octet_max:%u, max_frames_sdu:%u, meta.context_bf:%u",
		pac_lid, record_lid, nb_records, sampling_freq_hz, p_capa->param.frame_dur_bf,
		p_capa->param.chan_cnt_bf, p_capa->param.frame_octet_min,
		p_capa->param.frame_octet_max, p_capa->param.max_frames_sdu,
		p_metadata->param.context_bf);

	struct pac_capa *p_pac;
	struct unicast_peer *p_unicast_env = get_unicast_env_by_connection_index(con_lid);

	if (!p_unicast_env) {
		return;
	}

	if (p_unicast_env->nb_pacs_sink <= pac_lid) {
		pac_lid -= p_unicast_env->nb_pacs_sink;
		if (pac_lid >= ARRAY_SIZE(p_unicast_env->pacs_source)) {
			LOG_ERR("Too many source PACs");
			return;
		}
		p_pac = &p_unicast_env->pacs_source[pac_lid];
	} else {
		if (pac_lid >= ARRAY_SIZE(p_unicast_env->pacs_sink)) {
			LOG_ERR("Too many sink PACs");
			return;
		}
		p_pac = &p_unicast_env->pacs_sink[pac_lid];
	}

	p_pac->sampling_freq_hz = sampling_freq_hz;
	p_pac->frame_duration_bf = p_capa->param.frame_dur_bf;
	p_pac->frame_octet_min = p_capa->param.frame_octet_min;
	p_pac->frame_octet_max = p_capa->param.frame_octet_max;
	p_pac->max_frames_sdu = p_capa->param.max_frames_sdu;
}

static void on_bap_capa_client_cb_location(uint8_t const con_lid, uint8_t const direction,
					   uint32_t const location_bf)
{
	LOG_DBG("BAP Capabilities location: conidx %u, dir:%s, bf:%u", con_lid,
		(direction == GAF_DIRECTION_SINK) ? "Sink" : "Source", location_bf);
	if (location_bf & GAF_LOC_FRONT_LEFT_BIT) {
		LOG_DBG("  Front left");
	}
	if (location_bf & GAF_LOC_FRONT_CENTER_BIT) {
		LOG_DBG("  Front center");
	}
	if (location_bf & GAF_LOC_FRONT_RIGHT_BIT) {
		LOG_DBG("  Front right");
	}
}

static void on_bap_capa_client_cb_context(uint8_t const con_lid, uint8_t const context_type,
					  uint16_t const context_bf_sink,
					  uint16_t const context_bf_src)
{
	struct unicast_peer *p_unicast_env = get_unicast_env_by_connection_index(con_lid);

	if (!p_unicast_env) {
		return;
	}

	/* context bitfields see \ref enum bap_context_type_bf */
	if (p_unicast_env->nb_pacs_sink) {
		LOG_DBG("BAP context(SINK): type:%s, bf:%u",
			(context_type == BAP_CAPA_CONTEXT_TYPE_SUPP) ? "SUPP" : "AVAIL",
			context_bf_sink);
	}
	if (p_unicast_env->nb_pacs_source) {
		LOG_DBG("BAP context(SOURCE): type:%s, bf:%u",
			(context_type == BAP_CAPA_CONTEXT_TYPE_SUPP) ? "SUPP" : "AVAIL",
			context_bf_src);
	}
}

static void on_bap_capa_client_cb_svc_changed(uint8_t const con_lid)
{
	LOG_DBG("BAP Capabilities service changed event: conidx %u", con_lid);
}

static const struct bap_capa_cli_cb bap_capa_cli_callbacks = {
	.cb_cmp_evt = on_bap_capa_client_cb_cmp_evt,
	.cb_bond_data = on_bap_capa_client_cb_bond_data,
	.cb_record = on_bap_capa_client_cb_record,
	.cb_location = on_bap_capa_client_cb_location,
	.cb_context = on_bap_capa_client_cb_context,
	.cb_svc_changed = on_bap_capa_client_cb_svc_changed,
};

/* ---------------------------------------------------------------------------------------- */

int unicast_source_configure(void)
{
#if !UC_GROUP_PER_PEER
	for (size_t iter = 0; iter < ARRAY_SIZE(unicast_env.group_lid); iter++) {
		unicast_env.group_lid[iter] = GAF_INVALID_LID;
	}
#endif
	for (size_t index = 0; index < ARRAY_SIZE(unicast_env.peers); index++) {

		struct unicast_peer *const p_unicast_env = &unicast_env.peers[index];

		p_unicast_env->conidx = GAP_INVALID_CONIDX;
#if UC_GROUP_PER_PEER
		p_unicast_env->group_lid = GAF_INVALID_LID;
#endif
		for (size_t iter = 0; iter < ARRAY_SIZE(p_unicast_env->ase); iter++) {
			p_unicast_env->ase[iter].ase_instance_idx = GAF_INVALID_LID;
			p_unicast_env->ase[iter].ase_lid = GAF_INVALID_LID;
		}
	}

	k_work_queue_start(&worker_queue, worker_task_stack,
			   K_KERNEL_STACK_SIZEOF(worker_task_stack), WORKER_PRIORITY, NULL);
	k_thread_name_set(&worker_queue.thread, "unicast_cli_workq");

	struct bap_uc_cli_cfg bap_cli_cfg = {
		/* Configuration bit field. @ref enum bap_uc_cli_cfg_bf */
		.cfg_bf = BAP_UC_CLI_CFG_RELIABLE_WR_BIT,
		/* Number of ASE configurations that can be maintained
		 * Shall be at larger than 0
		 */
		.nb_ases_cfg = ARRAY_SIZE(unicast_env.peers) * ARRAY_SIZE(unicast_env.peers[0].ase),
		/* Preferred MTU
		 * Values from 0 to 63 are equivalent to 64
		 */
		.pref_mtu = GAP_LE_MAX_OCTETS,
		/* Timeout duration in seconds for reception of notification for ASE Control Point
		 * characteristic and for
		 * some notifications of ASE characteristic
		 * From 1s to 5s, 0 means 1s
		 */
		.timeout_s = 3,
	};
	uint16_t err;

	err = bap_uc_cli_configure(&bap_cli_cbs, &bap_cli_cfg);
	if (err != GAF_ERR_NO_ERROR) {
		LOG_ERR("Error %u configuring BAP client", err);
		return -1;
	}
	LOG_DBG("BAP client configured");

	struct bap_capa_cli_cfg capa_cli_cfg = {
		.pref_mtu = 0,
	};

	err = bap_capa_cli_configure(&bap_capa_cli_callbacks, &capa_cli_cfg);
	if (err != GAF_ERR_NO_ERROR) {
		LOG_ERR("Error %u configuring BAP capa client", err);
		return -1;
	}
	LOG_DBG("BAP capa client configured");

	/* Create a datapath configuration before QoS configuration to be able to
	 * setup streaming channels during the setup phase.
	 * Presentation delay is set to 10ms (internal buffering, effects to queue size).
	 */
	unicast_env.datapath_config.pres_delay_us = 2 * MAX_TRANSPORT_LATENCY_MS;

	return 0;
}

int unicast_source_discover(uint8_t const con_lid)
{
	uint16_t err;

	LOG_DBG("Discovering PACS and ACSC for connection:%u", con_lid);

	if (alloc_unicast_env_for_conidx(con_lid)) {
		LOG_ERR("Failed to allocate unicast environment for connection:%u", con_lid);
		return -ENOMEM;
	}

#if DISCOVER_PACS_BEFORE_ASCS
	err = bap_capa_cli_discover(con_lid, GATT_INVALID_HDL, GATT_INVALID_HDL);
#else
	err = bap_uc_cli_discover(con_lid, GATT_MIN_HDL, GATT_MAX_HDL);
#endif

	if (err != GAF_ERR_NO_ERROR) {
		LOG_ERR("Unicast client discovery start failed. Error:%u", err);
		return -ENOEXEC;
	}

	return 0;
}

int unicast_setup_streams(uint8_t const con_lid)
{
	LOG_DBG("Configuring streams for connection:%u", con_lid);

	struct unicast_peer *p_unicast_env = get_unicast_env_by_connection_index(con_lid);

	if (!p_unicast_env) {
		return -EINVAL;
	}

	if (configure_codec(p_unicast_env)) {
		return -1;
	}

	configure_qos(p_unicast_env);

	return 0;
}

int unicast_enable_streams(uint8_t const con_lid)
{
	LOG_DBG("Enabling streams for connection:%u", con_lid);

	struct unicast_peer *p_unicast_env = get_unicast_env_by_connection_index(con_lid);

	if (!p_unicast_env) {
		return -EINVAL;
	}

	enable_streaming_all_ase(p_unicast_env);

	return 0;
}

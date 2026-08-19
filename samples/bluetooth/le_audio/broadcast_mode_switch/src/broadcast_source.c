/* Copyright Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include "bluetooth/le_audio/audio_utils.h"

#include "ke_mem.h"
#include "bap.h"
#include "bap_bc.h"
#include "bap_bc_src.h"
#include "broadcast_source.h"
#include "audio_datapath.h"

LOG_MODULE_REGISTER(broadcast_source, CONFIG_BROADCAST_SOURCE_LOG_LEVEL);

#define PRESENTATION_DELAY_US (CONFIG_LE_AUDIO_PRESENTATION_DELAY_MS * 1000)
#define SUBGROUP_ID           0
#define BIS_ID_BASE           0
#define INVALID_LID           0xFF

#if CONFIG_ALIF_BLE_AUDIO_NMB_CHANNELS >= 2
#define STREAM_LOCATIONS (GAF_LOC_FRONT_LEFT_BIT | GAF_LOC_FRONT_RIGHT_BIT)
#else
#define STREAM_LOCATIONS (GAF_LOC_FRONT_LEFT_BIT)
#endif

/* ---------------- FSM (non-blocking) ---------------- */
typedef enum {
	SRC_STATE_IDLE = 0,
	SRC_STATE_CONFIGURED,
	SRC_STATE_PA_ENABLING,
	SRC_STATE_PA_ENABLED,
	SRC_STATE_GROUP_ENABLED,
	SRC_STATE_STREAMING,
	SRC_STATE_STOPPING_STREAMING,
	SRC_STATE_DISABLING_GROUP,
	SRC_STATE_DISABLING_PA,
} src_state_t;


/* Broadcast source environment structure */
struct broadcast_source_env {
	/* State management */
	src_state_t state;

	/* Stream information */
	uint32_t stream_ids[CONFIG_ALIF_BLE_AUDIO_NMB_CHANNELS];
	uint8_t bcast_grp_lid;
	bool bap_source_modules_configured;

	/* Audio datapath configuration */
	struct audio_datapath_config datapath_cfg;
	bool datapath_cfg_valid;

	/* Advertising configuration */
	uint8_t adv_sid;
};

static struct broadcast_source_env source_env = {
	.state = SRC_STATE_IDLE,
	.bcast_grp_lid = INVALID_LID,
	.bap_source_modules_configured = false,
	.datapath_cfg = {
		.i2s_dev = DEVICE_DT_GET(DT_ALIAS(i2s_bus)),
		.pres_delay_us = PRESENTATION_DELAY_US,
		.sampling_rate_hz = CONFIG_ALIF_BLE_AUDIO_FS_HZ,
		.octets_per_frame = 0, /* Will be set when configuring */
		.frame_duration_is_10ms = IS_ENABLED(CONFIG_ALIF_BLE_AUDIO_FRAME_DURATION_10MS),
	},
	.datapath_cfg_valid = true,
	.adv_sid = 0xFF
};

static K_SEM_DEFINE(src_stop_sem,     0, 8);
static K_SEM_DEFINE(src_disable_sem,  0, 1);
static K_SEM_DEFINE(src_dis_pa_sem,   0, 1);

static void *alloc_stream_config(uint32_t const location_bf)
{
	bap_cfg_t *p_stream_cfg = ke_malloc_user(sizeof(*p_stream_cfg), KE_MEM_PROFILE);

	if (!p_stream_cfg) {
		LOG_ERR("Failed to allocate memory for stream configuration");
		return NULL;
	}
	p_stream_cfg->param = (bap_cfg_param_t){
		.sampling_freq = BAP_SAMPLING_FREQ_UNKNOWN,
		.frame_dur     = BAP_FRAME_DUR_UNKNOWN,
		.frames_sdu    = 0,
		.frame_octet   = 0,
		.location_bf   = location_bf,
	};
	p_stream_cfg->add_cfg.len = 0;
	return p_stream_cfg;
}

static void broadcast_source_disable_pa(void)
{
	source_env.state = SRC_STATE_DISABLING_PA;
	uint16_t err = bap_bc_src_disable_pa(source_env.bcast_grp_lid);

	if (err) {
		if (source_env.bcast_grp_lid != INVALID_LID) {
			err = bap_bc_src_remove_group(source_env.bcast_grp_lid);
			if (err) {
				LOG_WRN("remove_group(%u) -> %u", source_env.bcast_grp_lid, err);
			} else {
				LOG_INF("Removed broadcast group %u", source_env.bcast_grp_lid);
			}
		}

		/* Reset local state */
		memset(source_env.stream_ids, 0, sizeof(source_env.stream_ids));
		source_env.bcast_grp_lid = INVALID_LID;
		source_env.state = SRC_STATE_IDLE;
	}
}

static void on_bap_bc_src_cmp_evt(uint8_t cmd_type, uint16_t status,
					uint8_t grp_lid, uint8_t sgrp_lid)
{
	ARG_UNUSED(sgrp_lid);

	switch (cmd_type) {
	case BAP_BC_SRC_CMD_TYPE_ENABLE_PA:
		LOG_INF("Periodic advertising %s (status %u)",
				status ? "enable done (err)" : "enabled", status);
		if (status == 0) {
			bap_bc_src_enable(source_env.bcast_grp_lid);
			source_env.state = SRC_STATE_PA_ENABLED;
		}
		break;

	case BAP_BC_SRC_CMD_TYPE_ENABLE:
		LOG_INF("Broadcast group %u %s", source_env.bcast_grp_lid,
				status ? "enable done (err)" : "enabled");
		if (status == 0) {
			source_env.state = SRC_STATE_GROUP_ENABLED;
			uint16_t err = bap_bc_src_start_streaming(source_env.bcast_grp_lid,
								0xFFFFFFFF);

			if (err) {
				LOG_ERR("Failed to start streaming, err %u", err);
			} else {
				LOG_INF("Start streaming command sent");
			}
		}
		break;

	case BAP_BC_SRC_CMD_TYPE_START_STREAMING:
		LOG_INF("Started streaming (status %u)", status);
		if (status == 0) {
			source_env.state = SRC_STATE_STREAMING;
			LOG_INF("Number of streams to start: %zu",
					ARRAY_SIZE(source_env.stream_ids));
			for (size_t i = 0; i < ARRAY_SIZE(source_env.stream_ids); i++) {
				LOG_INF("Starting stream %zu with ID %u", i,
						source_env.stream_ids[i]);
				audio_datapath_channel_start_source(source_env.stream_ids[i]);
			}
		}
		break;

	case BAP_BC_SRC_CMD_TYPE_STOP_STREAMING:
		LOG_INF("Stopped streaming (status %u)", status);
		k_sem_give(&src_stop_sem);
		if (source_env.state == SRC_STATE_STOPPING_STREAMING) {
			source_env.state = SRC_STATE_DISABLING_GROUP;
			uint16_t err = bap_bc_src_disable(source_env.bcast_grp_lid, true);

			if (err) {
				LOG_WRN("bap_bc_src_disable -> %u; will try PA disable path", err);
				broadcast_source_disable_pa();
			}
		}
		break;

	case BAP_BC_SRC_CMD_TYPE_DISABLE:
		LOG_INF("Broadcast group %u disabled (status %u)", grp_lid, status);
		k_sem_give(&src_disable_sem);
		if (source_env.state == SRC_STATE_DISABLING_GROUP) {
			broadcast_source_disable_pa();
		}
		break;

	case BAP_BC_SRC_CMD_TYPE_REMOVE_GROUP:
		LOG_INF("Broadcast group %u removed (status %u)", grp_lid, status);
		break;

	default:
		LOG_WRN("Unexpected bap_bc_src cmd complete: %u (status %u)", cmd_type, status);
		break;
	}
}

static void on_bap_bc_src_info(uint8_t grp_lid, const gapi_bg_config_t *p_bg_cfg,
				uint8_t nb_bis, const uint16_t *p_conhdl)
{
	ARG_UNUSED(grp_lid); ARG_UNUSED(p_bg_cfg); ARG_UNUSED(nb_bis); ARG_UNUSED(p_conhdl);
	LOG_DBG("BAP BC SRC info");
}

static const bap_bc_src_cb_t bap_bc_src_cbs = {
	.cb_cmp_evt = on_bap_bc_src_cmp_evt,
	.cb_info    = on_bap_bc_src_info,
};

static int get_adv_param(bap_bc_adv_param_t *p_adv_param)
{
	if (source_env.adv_sid == 0xFF) {
		source_env.adv_sid = (uint8_t)(sys_rand32_get() & 0x0F);
	} else {
		source_env.adv_sid = (uint8_t)((source_env.adv_sid + 1) & 0x0F);
	}
	if (source_env.adv_sid > (0x0F - 0x01)) {
		source_env.adv_sid = 1;
	}

	p_adv_param->adv_intv_min_slot = 160;
	p_adv_param->adv_intv_max_slot = 160;
	p_adv_param->ch_map = ADV_ALL_CHNLS_EN;
	p_adv_param->phy_prim = GAPM_PHY_TYPE_LE_1M;
	p_adv_param->phy_second = GAPM_PHY_TYPE_LE_2M;
	p_adv_param->adv_sid = source_env.adv_sid;
#if CONFIG_ALIF_BLE_ROM_IMAGE_V1_0
	p_adv_param->max_tx_pwr = -2;
#else
	p_adv_param->tx_pwr = -2;
	p_adv_param->own_addr_type = GAPM_STATIC_ADDR;
	p_adv_param->max_skip = 0;
	p_adv_param->send_tx_pwr = false;
#endif
	return 0;
}

static int broadcast_source_configure_group(void)
{
	LOG_INF("Starting broadcast source group configuration");

	const bap_bc_grp_param_t grp_param = {
		.sdu_intv_us = IS_ENABLED(CONFIG_ALIF_BLE_AUDIO_FRAME_DURATION_10MS) ?
						10000 : 7500,
		.max_sdu         = CONFIG_ALIF_BLE_AUDIO_OCTETS_PER_CODEC_FRAME,
		.max_tlatency_ms = CONFIG_ALIF_BLE_AUDIO_MAX_TLATENCY,
		.packing         = 0,
		.framing         = ISO_UNFRAMED_MODE,
		.phy_bf          = GAPM_PHY_TYPE_LE_2M,
		.rtn             = CONFIG_ALIF_BLE_AUDIO_RTN
	};

	const gaf_codec_id_t codec_id = GAF_CODEC_ID_LC3;

	bap_bc_adv_param_t adv_param;
	int ret = get_adv_param(&adv_param);

	if (ret) {
		LOG_ERR("Failed to get advertising parameters, err %d", ret);
		return -1;
	}

	const bap_bc_per_adv_param_t per_adv_param = {
		.adv_intv_min_frame = 160,
		.adv_intv_max_frame = 160,
	};

	bap_bcast_id_t bcast_id;

	sys_rand_get(bcast_id.id, sizeof(bcast_id.id));

	uint16_t err = bap_bc_src_add_group(&bcast_id, NULL,
						CONFIG_ALIF_BLE_AUDIO_NMB_CHANNELS, 1,
						&grp_param, &adv_param, &per_adv_param,
						PRESENTATION_DELAY_US, &source_env.bcast_grp_lid);

	if (err) {
		LOG_ERR("Failed to add broadcast group, err %u", err);
		return -1;
	}

	LOG_INF("Broadcast group added, got local ID %u (SID=%u)",
			source_env.bcast_grp_lid, adv_param.adv_sid);

	/* Subgroup config */
	bap_cfg_t *sgrp_cfg = ke_malloc_user(sizeof(*sgrp_cfg), KE_MEM_PROFILE);

	if (!sgrp_cfg) {
		LOG_ERR("Failed to allocate memory for subgroup configuration");
		return -ENOMEM;
	}

	sgrp_cfg->param = (bap_cfg_param_t){
		.location_bf  = 0,
		.frame_octet  = CONFIG_ALIF_BLE_AUDIO_OCTETS_PER_CODEC_FRAME,
		.frame_dur    = IS_ENABLED(CONFIG_ALIF_BLE_AUDIO_FRAME_DURATION_10MS) ?
						BAP_FRAME_DUR_10MS : BAP_FRAME_DUR_7_5MS,
		.frames_sdu   = 0,
		.sampling_freq = audio_hz_to_bap_sampling_freq(CONFIG_ALIF_BLE_AUDIO_FS_HZ),
	};
	sgrp_cfg->add_cfg.len = 0;

	if (sgrp_cfg->param.sampling_freq == BAP_SAMPLING_FREQ_UNKNOWN) {
		LOG_ERR("Unsupported sampling frequency: %u Hz", CONFIG_ALIF_BLE_AUDIO_FS_HZ);
		return -1;
	}

	bap_cfg_metadata_t *sgrp_meta = ke_malloc_user(sizeof(*sgrp_meta), KE_MEM_PROFILE);

	if (!sgrp_meta) {
		ke_free(sgrp_cfg);
		LOG_ERR("Failed to allocate memory for subgroup metadata");
		return -ENOMEM;
	}
	sgrp_meta->param.context_bf = BAP_CONTEXT_TYPE_UNSPECIFIED_BIT | BAP_CONTEXT_TYPE_MEDIA_BIT;
	sgrp_meta->add_metadata.len = 0;

	err = bap_bc_src_set_subgroup(source_env.bcast_grp_lid, SUBGROUP_ID,
					&codec_id, sgrp_cfg, sgrp_meta);

	if (err) {
		LOG_ERR("Failed to set subgroup, err %u", err);
		return -1;
	}

	const uint16_t dp_id = GAPI_DP_ISOOSHM;
	size_t stream_bits = STREAM_LOCATIONS;

	LOG_INF("STREAM_LOCATIONS: 0x%zx, CONFIG_ALIF_BLE_AUDIO_NMB_CHANNELS: %d",
			stream_bits, CONFIG_ALIF_BLE_AUDIO_NMB_CHANNELS);

	for (size_t iter = 0; stream_bits != 0; stream_bits >>= 1, iter++) {
		if (!(stream_bits & 1)) {
			continue;
		}

		const uint32_t stream_id = iter + BIS_ID_BASE;

		LOG_INF("Creating stream %zu with ID %u", iter, stream_id);
		bap_cfg_t *stream_cfg = alloc_stream_config(0x1 << iter);

		if (!stream_cfg) {
			LOG_ERR("Failed to allocate memory for stream configuration");
			return -ENOMEM;
		}

		err = bap_bc_src_set_stream(source_env.bcast_grp_lid, stream_id,
						SUBGROUP_ID, dp_id, 0, stream_cfg);

		if (err) {
			ke_free(stream_cfg);
			LOG_ERR("Failed to set stream %u, err %u", stream_id, err);
			return -1;
		}

		err = audio_datapath_channel_create_source(sgrp_cfg->param.frame_octet, stream_id);

		if (err) {
			LOG_ERR("Failed to create source channel for stream %u, err %d",
					stream_id, err);
			return -1;
		}

		source_env.stream_ids[iter] = stream_id;
	}

	source_env.state = SRC_STATE_CONFIGURED;
	LOG_INF("Broadcast stream added successfully");
	return 0;
}

static int broadcast_source_enable(void)
{
	LOG_INF("Enabling broadcast source PA and streaming");

	while (k_sem_take(&src_disable_sem, K_NO_WAIT) == 0) {
	}
	while (k_sem_take(&src_dis_pa_sem, K_NO_WAIT) == 0) {
	}
	while (k_sem_take(&src_stop_sem, K_NO_WAIT) == 0) {
	}

	uint8_t ad_data[1 + sizeof(CONFIG_BROADCAST_NAME)];

	ad_data[0] = sizeof(ad_data) - 1;
	ad_data[1] = GAP_AD_TYPE_COMPLETE_NAME;
	memcpy(&ad_data[2], CONFIG_BROADCAST_NAME, sizeof(ad_data) - 2);

	LOG_INF("Enabling PA for broadcast group %u", source_env.bcast_grp_lid);
	uint16_t err = bap_bc_src_enable_pa(source_env.bcast_grp_lid,
						sizeof(ad_data), 0,
						ad_data, NULL,
						sizeof(CONFIG_BROADCAST_NAME) - 1,
						CONFIG_BROADCAST_NAME,
						0, NULL);

	if (err) {
		LOG_ERR("Failed to enable PA, err %u", err);
		return -ENODEV;
	}

	source_env.state = SRC_STATE_PA_ENABLING;
	LOG_INF("PA enable issued; ENABLE will follow via callback");
	return 0;
}

int broadcast_source_configure(void)
{
	if (source_env.bap_source_modules_configured) {
		LOG_INF("BAP BC source modules already configured");
		return 0;
	}
	uint16_t err = bap_bc_src_configure(&bap_bc_src_cbs);

	if (err) {
		LOG_ERR("Failed to configure bap_bc_src, err %u", err);
		return -ENODEV;
	}
	source_env.bap_source_modules_configured = true;
	LOG_INF("BAP BC source modules configured successfully");
	return 0;
}

int broadcast_source_start_broadcasting(void)
{
	LOG_INF("Starting broadcast source broadcasting");

	if (!source_env.bap_source_modules_configured) {
		LOG_ERR("BAP BC source modules not configured");
		return -ENODEV;
	}

	if (source_env.state == SRC_STATE_STREAMING ||
		source_env.state == SRC_STATE_GROUP_ENABLED ||
		source_env.state == SRC_STATE_PA_ENABLING ||
		source_env.state == SRC_STATE_PA_ENABLED) {
		LOG_INF("Broadcast source already active/in progress (state=%d)", source_env.state);
		return 0;
	}

	LOG_INF("Creating audio datapath for source");
	int ret = audio_datapath_create_source(&source_env.datapath_cfg);

	if (ret) {
		LOG_ERR("Failed to create audio datapath, err %d", ret);
		return ret;
	}

	LOG_INF("Configuring broadcast source group");
	ret = broadcast_source_configure_group();

	if (ret) {
		LOG_ERR("Failed to configure broadcast source group, err %d", ret);
		return ret;
	}

	LOG_INF("Broadcast group configured, enabling PA and streaming");
	return broadcast_source_enable();
}

int broadcast_source_start(void)
{
	int ret = broadcast_source_configure();

	if (ret != 0) {
		return ret;
	}
	return broadcast_source_start_broadcasting();
}

int broadcast_source_stop(void)
{
	LOG_INF("Cleaning up audio datapath for source");
	audio_datapath_cleanup_source();

	if (source_env.state == SRC_STATE_STREAMING ||
			source_env.state == SRC_STATE_GROUP_ENABLED ||
			source_env.state == SRC_STATE_PA_ENABLING ||
			source_env.state == SRC_STATE_PA_ENABLED) {
		source_env.state = SRC_STATE_STOPPING_STREAMING;
		uint16_t err = bap_bc_src_stop_streaming(source_env.bcast_grp_lid,
							0xFFFFFFFF);

		if (err) {
			LOG_WRN("Failed to stop streaming immediately, err %u.", err);
			source_env.state = SRC_STATE_DISABLING_GROUP;
			err = bap_bc_src_disable(source_env.bcast_grp_lid, true);
			if (err) {
				LOG_WRN("Disable returned %u; falling back to PA disable", err);
				broadcast_source_disable_pa();
			}
		}
		return 0;
	}

	return 0;
}

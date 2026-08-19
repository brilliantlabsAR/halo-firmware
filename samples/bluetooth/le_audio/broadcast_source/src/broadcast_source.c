/* Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
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

#if CONFIG_ALIF_BLE_AUDIO_NMB_CHANNELS >= 2
#define STREAM_LOCATIONS (GAF_LOC_FRONT_LEFT_BIT | GAF_LOC_FRONT_RIGHT_BIT)
#else
#define STREAM_LOCATIONS (GAF_LOC_FRONT_LEFT_BIT)
#endif

static const char bc_password[] = CONFIG_BROADCAST_PASSWORD;

static uint32_t stream_ids[CONFIG_ALIF_BLE_AUDIO_NMB_CHANNELS];
/* Local ID of the broadcast group */
static uint8_t bcast_grp_lid;

static void *alloc_stream_config(uint32_t const location_bf)
{
	/* NOTE: ke_malloc_user must be used to reserve buffer from correct heap! */
	bap_cfg_t *p_stream_cfg = ke_malloc_user(sizeof(*p_stream_cfg), KE_MEM_PROFILE);

	if (!p_stream_cfg) {
		LOG_ERR("Failed to allocate memory for stream configuration");
		return NULL;
	}

	p_stream_cfg->param = (bap_cfg_param_t){
		/* Just set specific location. Other parameters are inherited from subgroup */
		.sampling_freq = BAP_SAMPLING_FREQ_UNKNOWN,
		.frame_dur = BAP_FRAME_DUR_UNKNOWN,
		.frames_sdu = 0,
		.frame_octet = 0,
		.location_bf = location_bf,
	};
	p_stream_cfg->add_cfg.len = 0;

	return p_stream_cfg;
}

static void on_bap_bc_src_cmp_evt(const uint8_t cmd_type, const uint16_t status,
				  const uint8_t grp_lid, const uint8_t sgrp_lid)
{
	LOG_DBG("BAP BC SRC event complete, type %u status %u grp_lid %u sgrp_lid %u", cmd_type,
		status, grp_lid, sgrp_lid);

	switch (cmd_type) {
	case BAP_BC_SRC_CMD_TYPE_ENABLE_PA: {
		LOG_INF("Periodic advertising enabled");

		uint16_t err = bap_bc_src_enable(bcast_grp_lid);

		if (err) {
			LOG_ERR("Failed to enable broadcast source, err %u", err);
		}
		break;
	}

	case BAP_BC_SRC_CMD_TYPE_ENABLE: {
		LOG_INF("Broadcast group %u enabled", bcast_grp_lid);

		uint16_t err = bap_bc_src_start_streaming(bcast_grp_lid, 0xFFFFFFFF);

		if (err) {
			LOG_ERR("Failed to start streaming, err %u", err);
		}
		break;
	}

	case BAP_BC_SRC_CMD_TYPE_START_STREAMING: {
		LOG_INF("Started streaming");
		for (size_t iter = 0; iter < ARRAY_SIZE(stream_ids); iter++) {
			audio_datapath_start(stream_ids[iter]);
		}
		break;
	}

	default: {
		LOG_WRN("Unexpected bap_bc_src command complete event: %u", cmd_type);
		break;
	}
	}
}

static void on_bap_bc_src_info(const uint8_t grp_lid, const gapi_bg_config_t *p_bg_cfg,
			       const uint8_t nb_bis, const uint16_t *p_conhdl)
{
	LOG_DBG("BAP BC SRC info, grp %u, cfg %p, nb_bis %u, p_conhdl %p", grp_lid, p_bg_cfg,
		nb_bis, p_conhdl);
}

static const bap_bc_src_cb_t bap_bc_src_cbs = {.cb_cmp_evt = on_bap_bc_src_cmp_evt,
					       .cb_info = on_bap_bc_src_info};

static int get_adv_param(bap_bc_adv_param_t *p_adv_param)
{
	p_adv_param->adv_intv_min_slot = 160;
	p_adv_param->adv_intv_max_slot = 160;
	p_adv_param->ch_map = ADV_ALL_CHNLS_EN;
	p_adv_param->phy_prim = GAPM_PHY_TYPE_LE_1M;
	p_adv_param->phy_second = GAPM_PHY_TYPE_LE_2M;
	p_adv_param->adv_sid = 1;
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
	const bap_bc_grp_param_t grp_param = {
		.sdu_intv_us = IS_ENABLED(CONFIG_ALIF_BLE_AUDIO_FRAME_DURATION_10MS) ? 10000 : 7500,
		.max_sdu = CONFIG_ALIF_BLE_AUDIO_OCTETS_PER_CODEC_FRAME,
		.max_tlatency_ms = CONFIG_ALIF_BLE_AUDIO_MAX_TLATENCY,
		.packing = 0,
		.framing = ISO_UNFRAMED_MODE,
		.phy_bf = GAPM_PHY_TYPE_LE_2M,
		.rtn = CONFIG_ALIF_BLE_AUDIO_RTN};

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

	const size_t bc_password_len = sizeof(bc_password) - 1;

	if (0 != bc_password_len && (bc_password_len < 4 ||  bc_password_len > GAP_KEY_LEN)) {
		LOG_ERR("Broadcast password is invalid, len must be either 0 or %u, actual %u",
			GAP_KEY_LEN, bc_password_len);
		return -1;
	}

	gaf_bcast_code_t code;
	const gaf_bcast_code_t *ptr = NULL;

	if (bc_password_len) {
		memset(code.bcast_code, 0, GAP_KEY_LEN);
		memcpy(code.bcast_code, bc_password, bc_password_len);
		ptr = &code;
	}

	uint16_t err = bap_bc_src_add_group(&bcast_id, ptr, CONFIG_ALIF_BLE_AUDIO_NMB_CHANNELS, 1,
					    &grp_param, &adv_param, &per_adv_param,
					    PRESENTATION_DELAY_US, &bcast_grp_lid);

	if (err) {
		LOG_ERR("Failed to add broadcast group, err %u", err);
		return -1;
	}

	LOG_DBG("Broadcast group added, got local ID %u", bcast_grp_lid);

	/* This struct must be accessible to the BLE stack for the lifetime of the BIG, so is
	 * statically allocated
	 * NOTE: ke_malloc_user must be used to reserve buffer from correct heap!
	 */
	bap_cfg_t *sgrp_cfg = ke_malloc_user(sizeof(*sgrp_cfg), KE_MEM_PROFILE);

	if (!sgrp_cfg) {
		LOG_ERR("Failed to allocate memory for subgroup configuration");
		return -ENOMEM;
	}
	sgrp_cfg->param = (bap_cfg_param_t){
		/* Location is unspecified at subgroup level */
		.location_bf = 0,
		.frame_octet = CONFIG_ALIF_BLE_AUDIO_OCTETS_PER_CODEC_FRAME,
		.frame_dur = IS_ENABLED(CONFIG_ALIF_BLE_AUDIO_FRAME_DURATION_10MS)
				     ? BAP_FRAME_DUR_10MS
				     : BAP_FRAME_DUR_7_5MS,
		/* 0 is unspecified, data will not be placed in BASE */
		.frames_sdu = 0,
		/* Convert to sampling frequency */
		.sampling_freq = audio_hz_to_bap_sampling_freq(CONFIG_ALIF_BLE_AUDIO_FS_HZ),
	};
	sgrp_cfg->add_cfg.len = 0;

	/* Validate sampling frequency conversion */
	if (sgrp_cfg->param.sampling_freq == BAP_SAMPLING_FREQ_UNKNOWN) {
		LOG_ERR("Unsupported sampling frequency: %u Hz", CONFIG_ALIF_BLE_AUDIO_FS_HZ);
		return -1;
	}

	/* This struct must be accessible to the BLE stack for the lifetime of the BIG, so is
	 * statically allocated
	 * NOTE: ke_malloc_user must be used to reserve buffer from correct heap!
	 */
	bap_cfg_metadata_t *sgrp_meta = ke_malloc_user(sizeof(*sgrp_meta), KE_MEM_PROFILE);

	if (!sgrp_meta) {
		ke_free(sgrp_cfg);
		LOG_ERR("Failed to allocate memory for subgroup metadata");
		return -ENOMEM;
	}
	sgrp_meta->param.context_bf = BAP_CONTEXT_TYPE_UNSPECIFIED_BIT | BAP_CONTEXT_TYPE_MEDIA_BIT;
	sgrp_meta->add_metadata.len = 0;

	err = bap_bc_src_set_subgroup(bcast_grp_lid, SUBGROUP_ID, &codec_id, sgrp_cfg, sgrp_meta);

	if (err) {
		LOG_ERR("Failed to set subgroup, err %u", err);
		return -1;
	}
	LOG_DBG("Broadcast subgroup added");

	const uint16_t dp_id = GAPI_DP_ISOOSHM;

	size_t stream_bits = STREAM_LOCATIONS;

	for (size_t iter = 0; stream_bits != 0; stream_bits >>= 1, iter++) {
		if (!(stream_bits & 1)) {
			continue;
		}

		const uint32_t stream_id = iter + BIS_ID_BASE;
		bap_cfg_t *stream_cfg = alloc_stream_config(0x1 << iter);

		if (!stream_cfg) {
			LOG_ERR("Failed to allocate memory for stream configuration");
			return -ENOMEM;
		}

		err = bap_bc_src_set_stream(bcast_grp_lid, stream_id, SUBGROUP_ID, dp_id, 0,
					    stream_cfg);
		if (err) {
			ke_free(stream_cfg);
			LOG_ERR("Failed to set stream %u, err %u", stream_id, err);
			return -1;
		}

		err = audio_datapath_channel_create(sgrp_cfg->param.frame_octet, stream_id);
		if (err) {
			return -1;
		}

		stream_ids[iter] = stream_id;
	}

	LOG_DBG("Broadcast stream added");

	return 0;
}

static int broadcast_source_enable(void)
{
	uint8_t ad_data[1 + sizeof(CONFIG_BLE_DEVICE_NAME)];

	ad_data[0] = sizeof(ad_data) - 1; /* Size of data following the size byte */
	ad_data[1] = GAP_AD_TYPE_COMPLETE_NAME;

	memcpy(&ad_data[2], CONFIG_BLE_DEVICE_NAME, sizeof(ad_data) - 2);

	uint16_t err = bap_bc_src_enable_pa(bcast_grp_lid, sizeof(ad_data), 0, ad_data, NULL,
					    sizeof(CONFIG_BROADCAST_NAME) - 1,
					    CONFIG_BROADCAST_NAME, 0, NULL);

	if (err) {
		LOG_ERR("Failed to enable PA with error %u", err);
		return -1;
	}

	return 0;
}

int broadcast_source_start(void)
{
	int ret;
	uint16_t err = bap_bc_src_configure(&bap_bc_src_cbs);

	if (err) {
		LOG_ERR("Error %u configuring bap_bc_src", err);
		return -1;
	}

	LOG_DBG("bap_bc_src configured");

	ret = audio_datapath_init();
	if (ret) {
		LOG_ERR("Failed to initialise audio datapath, err %d", ret);
		return ret;
	}

	ret = broadcast_source_configure_group();
	if (ret) {
		LOG_ERR("Failed to configure broadcast source group, err %d", ret);
		return ret;
	}

	LOG_DBG("Broadcast group configured");

	return broadcast_source_enable();
}

/* Copyright Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/types.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/init.h>
#include <zephyr/sys/__assert.h>
#include <string.h>

#include "bluetooth/le_audio/audio_utils.h"

#include "bap_bc_sink.h"
#include "bap_bc_scan.h"
#include "audio_datapath.h"
#include "broadcast_sink.h"

LOG_MODULE_REGISTER(broadcast_sink, CONFIG_BROADCAST_SINK_LOG_LEVEL);

static K_SEM_DEFINE(disable_done_sem, 0, 1);
static K_SEM_DEFINE(stop_streams_sem, 0, 32);

#define SYNCHRONISATION_TIMEOUT_MS 2000
#define SYNCHRONISATION_TIMEOUT    (SYNCHRONISATION_TIMEOUT_MS / 10)
#define SCAN_TIMEOUT_MS            1000
#define SCAN_TIMEOUT               (SCAN_TIMEOUT_MS / 10)
#define SINK_TIMEOUT_MS            1000
#define SINK_TIMEOUT               (SINK_TIMEOUT_MS / 10)
#define INVALID_CHANNEL_INDEX      0xFF
#define INVALID_LID			0xFF

#define GAF_LOC_LEFT_OR_CENTRE_MASK                                                                \
	(GAF_LOC_FRONT_LEFT_BIT | GAF_LOC_FRONT_LEFT_BIT | GAF_LOC_BACK_LEFT_BIT |                 \
	 GAF_LOC_FRONT_LEFT_CENTER_BIT | GAF_LOC_BACK_CENTER_BIT | GAF_LOC_SIDE_LEFT_BIT |         \
	 GAF_LOC_TOP_FRONT_LEFT_BIT | GAF_LOC_TOP_FRONT_CENTER_BIT | GAF_LOC_TOP_CENTER_BIT |      \
	 GAF_LOC_TOP_BACK_LEFT_BIT | GAF_LOC_TOP_SIDE_LEFT_BIT | GAF_LOC_TOP_BACK_CENTER_BIT |     \
	 GAF_LOC_BOTTOM_FRONT_CENTER_BIT | GAF_LOC_BOTTOM_FRONT_LEFT_BIT |                         \
	 GAF_LOC_FRONT_LEFT_WIDE_BIT | GAF_LOC_LEFT_SURROUND_BIT)
#define GAF_LOC_RIGHT_MASK                                                                         \
	(GAF_LOC_FRONT_RIGHT_BIT | GAF_LOC_BACK_RIGHT_BIT | GAF_LOC_FRONT_RIGHT_CENTER_BIT |       \
	 GAF_LOC_SIDE_RIGHT_BIT | GAF_LOC_TOP_FRONT_RIGHT_BIT | GAF_LOC_TOP_BACK_RIGHT_BIT |       \
	 GAF_LOC_TOP_SIDE_RIGHT_BIT | GAF_LOC_BOTTOM_FRONT_RIGHT_BIT |                             \
	 GAF_LOC_FRONT_RIGHT_WIDE_BIT)

/* Configuration obtained from advertising reports */
struct broadcast_sink_env {
	/* Details of PA, group etc. to enable */
	bap_bcast_id_t bcast_id;
	uint32_t chosen_streams_bf;
	uint32_t started_streams_bf;
	uint8_t left_channel_pos;
	uint8_t right_channel_pos;
	uint8_t pa_lid;
	uint8_t grp_lid;

	/* Audio datapath configuration */
	struct audio_datapath_config datapath_cfg;
	bool datapath_cfg_valid;
};

#define I2S_NODE   DT_ALIAS(i2s_bus)
#define CODEC_NODE DT_ALIAS(audio_codec)
/* #define MCLK_GEN_NODE DT_ALIAS(mclk_gen) */

BUILD_ASSERT(!DT_PROP(I2S_NODE, mono_mode), "I2S must be configured in stereo mode");

const struct device *i2s_dev = DEVICE_DT_GET(I2S_NODE);
const struct device *codec_dev = DEVICE_DT_GET(CODEC_NODE);
/* const struct device *mclk_gen_dev = DEVICE_DT_GET(MCLK_GEN_NODE); */

static uint8_t expected_streams;
static bool public_broadcast_found;
static struct broadcast_sink_env sink_env;


static void reset_sink_config(void)
{
	memset(&sink_env, 0, sizeof(sink_env));
	sink_env.datapath_cfg_valid = true;
	sink_env.datapath_cfg.i2s_dev = i2s_dev;
	sink_env.right_channel_pos = INVALID_CHANNEL_INDEX;
	sink_env.left_channel_pos  = INVALID_CHANNEL_INDEX;

	/* NEW: these were implicitly 0; make them explicitly invalid */
	sink_env.pa_lid  = INVALID_LID;
	sink_env.grp_lid = INVALID_LID;
}
static int start_scanning(void)
{
	LOG_INF("Start scanning for broadcast sources");

	/* Zero timeout value causes it to scan until explicitly stopped */
	uint16_t err = bap_bc_scan_start(0);

	if (err != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to start bap_bc_scan, err %u", err);
		return -ENODEV;
	}

	reset_sink_config();
	public_broadcast_found = false;

	return 0;
}

static int sink_enable(void)
{
	if (!sink_env.datapath_cfg_valid) {
		LOG_ERR("Cannot enable sink for invalid config");
		return -1;
	}

	if (sink_env.left_channel_pos != INVALID_CHANNEL_INDEX) {
		sink_env.chosen_streams_bf |= (1U << (sink_env.left_channel_pos - 1));
	}

	if (sink_env.right_channel_pos != INVALID_CHANNEL_INDEX) {
		sink_env.chosen_streams_bf |= (1U << (sink_env.right_channel_pos - 1));
	}

	LOG_INF("Chosen streams bitfield: %x", sink_env.chosen_streams_bf);

	uint16_t err =
		bap_bc_sink_enable(sink_env.pa_lid, &sink_env.bcast_id, sink_env.chosen_streams_bf,
					NULL, 0, SINK_TIMEOUT, &sink_env.grp_lid);

	if (err) {
		LOG_ERR("Failed to enable bap_bc_sink, err %u", err);
		return -1;
	}

	return 0;
}

static void terminate_pa_sync(void)
{
	uint16_t err = bap_bc_scan_pa_terminate(sink_env.pa_lid);

	if (err) {
		LOG_ERR("Failed to terminate sync with PA, err %u", err);
	}
}

static int start_streaming(void)
{
	gaf_codec_id_t codec_id = GAF_CODEC_ID_LC3;

	uint16_t err = bap_bc_sink_start_streaming(sink_env.grp_lid, sink_env.left_channel_pos,
							&codec_id, GAPI_DP_ISOOSHM, 0, NULL);

	if (err) {
		LOG_ERR("Failed to start streaming with err %u", err);
		start_scanning();
		return -1;
	}

	if (sink_env.right_channel_pos != INVALID_CHANNEL_INDEX) {
		uint16_t err =
			bap_bc_sink_start_streaming(sink_env.grp_lid, sink_env.right_channel_pos,
							&codec_id, GAPI_DP_ISOOSHM, 0, NULL);

		if (err) {
			LOG_ERR("Failed to start streaming with err %u", err);
			start_scanning();
			return -1;
		}
	}

	return 0;
}

static void on_bap_bc_scan_cmp_evt(uint8_t cmd_type, uint16_t status, uint8_t pa_lid)
{
	switch (cmd_type) {
	case BAP_BC_SCAN_CMD_TYPE_START:
		LOG_INF("Scan start cmd complete, status %u", status);
		break;
	case BAP_BC_SCAN_CMD_TYPE_STOP:
		LOG_INF("Scan stop cmd complete, status %u", status);
		break;
	case BAP_BC_SCAN_CMD_TYPE_PA_SYNCHRONIZE:
		LOG_INF("PA synchronise cmd complete, status %u", status);
		break;
	case BAP_BC_SCAN_CMD_TYPE_PA_TERMINATE:
		LOG_INF("PA terminate cmd complete, status %u", status);
		break;
	default:
		LOG_INF("Unexpected cmd_type %u", cmd_type);
		break;
	}
}

static void on_bap_bc_scan_timeout(void)
{
	LOG_INF("Scan timeout");
}

static void on_bap_bc_scan_report(const bap_adv_id_t *p_adv_id,
			const bap_bcast_id_t *p_bcast_id,
			uint8_t info_bf, const gaf_adv_report_air_info_t *p_air_info,
			uint16_t length, const uint8_t *p_data)
{
	LOG_INF("Got scan report");
	LOG_INF("Public Broadcast Profile %s supported",
		(info_bf & BAP_BC_SCAN_PUBLIC_BROADCAST_SUPPORT_BIT) ? "is" : "is not");
	LOG_INF("Broadcast ID: %02x %02x %02x", p_bcast_id->id[0], p_bcast_id->id[1],
		p_bcast_id->id[2]);
	LOG_INF("Air info: tx_pwr %u, rssi %u", p_air_info->tx_pwr, p_air_info->rssi);
	LOG_HEXDUMP_DBG(p_data, length, "adv data: ");

	/* Store broadcast ID for later */
	memcpy(&sink_env.bcast_id, p_bcast_id, sizeof(bap_bcast_id_t));
}

static void on_bap_bc_scan_public_bcast(const bap_adv_id_t *p_adv_id,
					const bap_bcast_id_t *p_bcast_id, uint8_t pbp_features_bf,
					uint8_t broadcast_name_len, const uint8_t *p_broadcast_name,
					uint8_t metadata_len, const uint8_t *p_metadata)
{
	bool correct_stream = false;

	LOG_INF("Got public broadcast report");
	LOG_INF("PBP features: encrypted: %s, standard quality: %s, high quality: %s",
		(pbp_features_bf & BAP_BC_PBP_FEATURES_ENCRYPTED_BIT) ? "yes" : "no",
		(pbp_features_bf & BAP_BC_PBP_FEATURES_STANDARD_QUALITY_PRESENT_BIT) ? "yes" : "no",
		(pbp_features_bf & BAP_BC_PBP_FEATURES_HIGH_QUALITY_PRESENT_BIT) ? "yes" : "no");
	if (broadcast_name_len < 20) {
		char broadcast_name[20];

		memcpy(broadcast_name, p_broadcast_name, broadcast_name_len);
		broadcast_name[broadcast_name_len] = '\0';
		LOG_INF("Broadcast name %s", broadcast_name);

		correct_stream =
			(0 == memcmp(CONFIG_BROADCAST_NAME, broadcast_name, broadcast_name_len));
	}
	LOG_HEXDUMP_DBG(p_metadata, metadata_len, "metadata: ");

	/* If we found a non-encrypted public broadcast, synchronise to this */
	if (correct_stream && !(pbp_features_bf & BAP_BC_PBP_FEATURES_ENCRYPTED_BIT) &&
			!public_broadcast_found) {
		LOG_INF("Synchronising to public broadcast");
		public_broadcast_found = true;

		uint16_t err = bap_bc_scan_pa_synchronize(p_adv_id, 0,
							BAP_BC_SCAN_REPORT_MASK,
							SYNCHRONISATION_TIMEOUT, SCAN_TIMEOUT,
							&sink_env.pa_lid);

		if (err != GAP_ERR_NO_ERROR) {
			LOG_ERR("Failed to start PA synchronise procedure, err %u", err);
		}
	}
}

static void on_bap_bc_scan_pa_established(uint8_t pa_lid, const bap_adv_id_t *p_adv_id, uint8_t phy,
					  uint16_t interval_frames)
{
	LOG_INF("PA synchronised, pa_lid %u interval %u ms", pa_lid, (interval_frames * 5) / 4);
	sink_env.pa_lid = pa_lid;

	uint16_t err = bap_bc_scan_stop();

	if (err) {
		LOG_INF("Failed to stop scanning, err %u", err);
	}
}

static void on_bap_bc_scan_pa_terminated(uint8_t pa_lid, uint8_t reason)
{
	LOG_INF("PA desynchronised, reason %u", reason);
	sink_env.pa_lid = INVALID_LID;
}

static void on_bap_bc_scan_pa_report(uint8_t pa_lid, const gaf_adv_report_air_info_t *p_air_info,
				     uint16_t length, const uint8_t *p_data)
{
	LOG_INF("PA report");
	LOG_INF("Air info: tx_pwr %u rssi %u", p_air_info->tx_pwr, p_air_info->rssi);
	LOG_HEXDUMP_DBG(p_data, length, "periodic adv data: ");
}

static void on_bap_bc_scan_big_info_report(uint8_t pa_lid, const gapm_le_big_info_t *p_report)
{
	LOG_INF("BIGinfo report");
	LOG_INF("SDU interval %u us, ISO interval %u ms, max_pdu %u max_sdu %u",
		p_report->sdu_interval, p_report->iso_interval, p_report->max_pdu,
		p_report->max_sdu);
	LOG_INF("num_bis %u, NSE %u, BN %u, PTO %u, IRC %u, PHY %u, framing %u, encrypted %u",
		p_report->num_bis, p_report->nse, p_report->bn, p_report->pto, p_report->irc,
		p_report->phy, p_report->framing, p_report->encrypted);
}

static void on_bap_bc_scan_group_report(uint8_t pa_lid, uint8_t nb_subgroups, uint8_t nb_streams,
					uint32_t pres_delay_us)
{
	LOG_INF("Group report: %u subgroups, %u streams, presentation delay %u us", nb_subgroups,
		nb_streams, pres_delay_us);
	expected_streams = nb_streams;

	/* Store presentation delay for later use */
	sink_env.datapath_cfg.pres_delay_us = pres_delay_us;
}

static void on_bap_bc_scan_subgroup_report(uint8_t pa_lid, uint8_t sgrp_id, uint32_t stream_pos_bf,
					   const gaf_codec_id_t *p_codec_id,
					   const bap_cfg_ptr_t *p_cfg,
					   const bap_cfg_metadata_ptr_t *p_metadata)
{
	LOG_INF("Subgroup report");
	LOG_INF("sgrp_id %u, stream_bf %x, codec_id %02x %02x %02x %02x %02x", sgrp_id,
		stream_pos_bf, p_codec_id->codec_id[0], p_codec_id->codec_id[1],
		p_codec_id->codec_id[2], p_codec_id->codec_id[3], p_codec_id->codec_id[4]);
	LOG_INF("BAP cfg: loc_bf %x frame_octet %u sampling_freq %u frame_dur %u "
		"frames_sdu %u",
		p_cfg->param.location_bf, p_cfg->param.frame_octet, p_cfg->param.sampling_freq,
		p_cfg->param.frame_dur, p_cfg->param.frames_sdu);

	/* Validate config is OK and store relevant info for later use */
	if (p_cfg->param.sampling_freq < BAP_SAMPLING_FREQ_MIN ||
			p_cfg->param.sampling_freq > BAP_SAMPLING_FREQ_MAX) {
		LOG_WRN("Invalid sampling frequency %d(bap_sampling_freq)",
			p_cfg->param.sampling_freq);

		sink_env.datapath_cfg_valid = false;
	}

	if (p_cfg->param.frame_dur != BAP_FRAME_DUR_10MS) {
		LOG_WRN("Frame duration is not compatible, need 10 ms");
		sink_env.datapath_cfg_valid = false;
	}

	sink_env.datapath_cfg.octets_per_frame = p_cfg->param.frame_octet;
	sink_env.datapath_cfg.frame_duration_is_10ms = p_cfg->param.frame_dur == BAP_FRAME_DUR_10MS;
	sink_env.datapath_cfg.sampling_rate_hz =
		audio_bap_sampling_freq_to_hz(p_cfg->param.sampling_freq);
}

static void assign_audio_channel(uint8_t stream_count, uint8_t stream_pos, uint16_t loc_bf)
{
#ifdef CONFIG_AUDIO_LOCATION_USE_GAF
	if ((loc_bf & GAF_LOC_LEFT_OR_CENTRE_MASK) &&
	    (sink_env.left_channel_pos == INVALID_CHANNEL_INDEX)) {
#else /* CONFIG_AUDIO_LOCATION_IMPLICIT */
	if (stream_count == 0) {
#endif
		LOG_INF("Stream index %u is left or centre channel", stream_pos);
		sink_env.left_channel_pos = stream_pos;
	}

#ifdef CONFIG_AUDIO_LOCATION_USE_GAF
	if ((loc_bf & GAF_LOC_RIGHT_MASK) &&
	    (sink_env.right_channel_pos == INVALID_CHANNEL_INDEX)) {
#else /* CONFIG_AUDIO_LOCATION_IMPLICIT */
	if (stream_count == 1) {
#endif
		LOG_INF("Stream index %u is right channel", stream_pos);
		sink_env.right_channel_pos = stream_pos;
	}
}

static void on_bap_bc_scan_stream_report(uint8_t pa_lid, uint8_t sgrp_id, uint8_t stream_pos,
					 const gaf_codec_id_t *p_codec_id,
					 const bap_cfg_ptr_t *p_cfg)
{
	static uint8_t stream_report_count;

	LOG_INF("Stream report %u", stream_pos);
	LOG_INF("BAP cfg: loc_bf %x frame_octet %u sampling_freq %u frame_dur %u frames_sdu %u",
		p_cfg->param.location_bf, p_cfg->param.frame_octet, p_cfg->param.sampling_freq,
		p_cfg->param.frame_dur, p_cfg->param.frames_sdu);

	assign_audio_channel(stream_report_count, stream_pos, p_cfg->param.location_bf);

	if (++stream_report_count >= expected_streams) {
		expected_streams = 0;
		stream_report_count = 0;

		LOG_INF("Disabling PA reports");
		uint16_t err = bap_bc_scan_pa_report_ctrl(sink_env.pa_lid, 0);

		if (err) {
			LOG_ERR("Failed to disable PA reports");
		}

		if (sink_env.left_channel_pos == INVALID_CHANNEL_INDEX) {
			LOG_INF("A left or centre channel must be present");
			sink_env.datapath_cfg_valid = false;
		}

		if (sink_env.datapath_cfg_valid) {
			LOG_INF("Compatible audio source found");

			/* Enable BC sink for the compatible source */
			sink_enable();
		} else {
			LOG_INF("Audio source is not compatible");

			/* Restart scanning for another source */
			start_scanning();
		}
	}
}

static void on_bap_bc_sink_cmp_evt(uint8_t cmd_type, uint16_t status, uint8_t grp_lid,
					uint8_t stream_pos)
{
	switch (cmd_type) {
	case BAP_BC_SINK_CMD_TYPE_ENABLE:
		LOG_INF("enable cmd complete, status %u, grp %u, stream %u",
				status, grp_lid, stream_pos);
		break;

	case BAP_BC_SINK_CMD_TYPE_START_STREAMING:
		LOG_INF("start streaming cmd complete, status %u, grp %u, stream %u",
				status, grp_lid, stream_pos);
		sink_env.started_streams_bf |= (1U << (stream_pos - 1));
		if (sink_env.started_streams_bf == sink_env.chosen_streams_bf) {
			int ret = audio_datapath_create_sink(&sink_env.datapath_cfg);

			if (ret) {
				audio_datapath_cleanup_sink();
				LOG_ERR("Failed to create audio datapath");
				start_scanning();
			}
			audio_datapath_start_sink();
		}
		break;

	case BAP_BC_SINK_CMD_TYPE_STOP_STREAMING:
		LOG_INF("stop streaming cmd complete, status %u, grp %u, stream %u",
				status, grp_lid, stream_pos);
		if (stream_pos >= 1 && stream_pos <= 32) {
			sink_env.started_streams_bf &= ~(1U << (stream_pos - 1));
		}
		k_sem_give(&stop_streams_sem);   /* <-- NEW: counts a finished BIS stop */
		break;

	case BAP_BC_SINK_CMD_TYPE_DISABLE:
		LOG_INF("disable cmd complete, status %u, grp %u", status, grp_lid);
		k_sem_give(&disable_done_sem);
		break;

	default:
		LOG_ERR("Unexpected cmd type %u", cmd_type);
		break;
	}
}


static void on_bap_bc_sink_quality_cmp_evt(uint16_t status, uint8_t grp_lid,
				uint8_t stream_pos,
				uint32_t crc_error_packets, uint32_t rx_unrx_packets,
				uint32_t duplicate_packets)
{
	LOG_INF("cb_sink_quality, status %u group %u stream %u crc_err %u missing %u duplicate %u",
		status, grp_lid, stream_pos, crc_error_packets, rx_unrx_packets, duplicate_packets);
}

static void on_bap_bc_sink_status(uint8_t grp_lid, uint8_t state, uint32_t stream_pos_bf,
					const gapi_bg_sync_config_t *p_bg_cfg, uint8_t nb_bis,
					const uint16_t *p_conhdl)
{
	switch (state) {
	case BAP_BC_SINK_ESTABLISHED:
		LOG_INF("sync established with group %u", grp_lid);
		terminate_pa_sync();
		start_streaming();
		break;
	case BAP_BC_SINK_FAILED:
	case BAP_BC_SINK_CANCELLED:
	case BAP_BC_SINK_LOST:
	case BAP_BC_SINK_PEER_TERMINATE:
	case BAP_BC_SINK_MIC_FAILURE:
		LOG_INF("no sync with group %u, state %u", grp_lid, state);
		audio_datapath_cleanup_sink();
		start_scanning();
		break;
	case BAP_BC_SINK_UPPER_TERMINATE:
		LOG_INF("no sync with group %u, state %u", grp_lid, state);
		audio_datapath_cleanup_sink();
		break;
	default:
		LOG_ERR("Unexpected bc_sink state %u", state);
		break;
	}
}

static const bap_bc_scan_cb_t scan_cbs = {
	.cb_cmp_evt = on_bap_bc_scan_cmp_evt,
	.cb_timeout = on_bap_bc_scan_timeout,
	.cb_report = on_bap_bc_scan_report,
	.cb_public_bcast_source = on_bap_bc_scan_public_bcast,
	.cb_pa_established = on_bap_bc_scan_pa_established,
	.cb_pa_terminated = on_bap_bc_scan_pa_terminated,
	.cb_pa_report = on_bap_bc_scan_pa_report,
	.cb_big_info_report = on_bap_bc_scan_big_info_report,
	.cb_group_report = on_bap_bc_scan_group_report,
	.cb_subgroup_report = on_bap_bc_scan_subgroup_report,
	.cb_stream_report = on_bap_bc_scan_stream_report,
};

static const bap_bc_sink_cb_t sink_cbs = {
	.cb_cmp_evt = on_bap_bc_sink_cmp_evt,
	.cb_quality_cmp_evt = on_bap_bc_sink_quality_cmp_evt,
	.cb_status = on_bap_bc_sink_status,
};

/* Track if BAP BC modules are already configured */
static bool bap_modules_configured;

int broadcast_sink_configure(void)
{
	uint16_t err;

	/* Only configure once */
	if (bap_modules_configured) {
		LOG_INF("BAP BC modules already configured");
		return 0;
	}

	err = bap_bc_scan_configure(BAP_ROLE_SUPP_BC_SINK_BIT | BAP_ROLE_SUPP_BC_SCAN_BIT,
					&scan_cbs);
	if (err != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to configure bap_bc_scan, err %u", err);
		return -ENODEV;
	}

	err = bap_bc_sink_configure(BAP_ROLE_SUPP_BC_SINK_BIT | BAP_ROLE_SUPP_BC_SCAN_BIT,
					&sink_cbs);
	if (err != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to configure bap_bc_sink, err %u", err);
		return -ENODEV;
	}

	bap_modules_configured = true;
	LOG_INF("BAP BC modules configured successfully");
	return 0;
}

int broadcast_sink_start(void)
{
	/* Configure modules if not already done */
	int ret = broadcast_sink_configure();

	if (ret != 0) {
		return ret;
	}

	return start_scanning();
}

int broadcast_sink_start_scanning(void)
{
	/* Only start scanning if modules are configured */
	if (!bap_modules_configured) {
		LOG_ERR("BAP BC modules not configured, call broadcast_sink_configure() first");
		return -ENODEV;
	}

	return start_scanning();
}

int broadcast_sink_stop(void)
{
	LOG_INF("Stopping broadcast sink");

	if (sink_env.grp_lid != INVALID_LID) {
		uint16_t err;

		/* Disable the broadcast sink group */
		err = bap_bc_sink_disable(sink_env.grp_lid);

		if (err != 0) {
			LOG_WRN("Failed to disable broadcast sink group after retries, err %u",
					err);
		} else {
			LOG_INF("Group disabled");
		}

		/* Only now mark grp_lid invalid */
		sink_env.grp_lid = INVALID_LID;
	}

	/* 3) Clean up audio datapath */
	audio_datapath_cleanup_sink();

	/* 4) Terminate PA if still synced */
	if (sink_env.pa_lid != INVALID_LID) {
		uint16_t err = bap_bc_scan_pa_terminate(sink_env.pa_lid);

		if (err) {
			LOG_DBG("PA terminate returned err %u (may already be terminated)", err);
		}
		sink_env.pa_lid = INVALID_LID;
	}

	uint16_t err = bap_bc_scan_stop();

	if (err) {
		LOG_INF("Scan stop returned err %u (expected if not scanning)", err);
	} else {
		LOG_INF("Scanning stopped");
	}

	/* 6) Reset local state */
	reset_sink_config();
	public_broadcast_found = false;

	LOG_INF("Broadcast sink stopped and reset");
	return 0;
}



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
#include <zephyr/sys/util.h>
#include <string.h>

#include "bluetooth/le_audio/audio_utils.h"

#include "bap_bc_sink.h"
#include "bap_bc_scan.h"
#include "bap_bc_deleg.h"
#include "audio_datapath.h"
#include "broadcast_sink.h"
#include "gaf_adv.h"
#include "gapm_le.h"

LOG_MODULE_REGISTER(broadcast_sink, CONFIG_BROADCAST_SINK_LOG_LEVEL);

/* ---------- Device ---------- */
#define MAX_DEVICE_NAME_LEN 29

/* ---------- Timing ---------- */
#define SYNCHRONISATION_TIMEOUT_MS 2000
#define SYNCHRONISATION_TIMEOUT    (SYNCHRONISATION_TIMEOUT_MS / 10)
#define SCAN_TIMEOUT_MS            1000
#define SCAN_TIMEOUT               (SCAN_TIMEOUT_MS / 10)
#define SINK_TIMEOUT_MS            1000
#define SINK_TIMEOUT               (SINK_TIMEOUT_MS / 10)

/* ---------- Misc ---------- */
#define INVALID_CHANNEL_INDEX      0xFF
#define SD_MSGQ_LEN                8
#define SD_MAX_SUBGROUPS           8
#define SD_WAIT                    K_SECONDS(5)   /* be generous to avoid races */

/* ---------- Channels from GAF Location ---------- */
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

/* ---------- Devicetree ---------- */
#define I2S_NODE   DT_ALIAS(i2s_bus)
#define CODEC_NODE DT_ALIAS(audio_codec)

BUILD_ASSERT(!DT_PROP(I2S_NODE, mono_mode), "I2S must be configured in stereo mode");

static const struct device *const i2s_dev   = DEVICE_DT_GET(I2S_NODE);
static const struct device *const codec_dev = DEVICE_DT_GET(CODEC_NODE);

/* ================================================================
 *                   RUNTIME ENVIRONMENT
 * ================================================================
 */

struct broadcast_sink_env {
	/* IDs / LIDs */
	bap_bcast_id_t bcast_id;
	bap_adv_id_t   adv_id;        /* stored from Add Source to reuse for PA sync */
	uint8_t        pa_lid;
	uint8_t        grp_lid;
	uint8_t        src_lid;
	uint8_t        con_lid;

	/* Stream selection */
	uint32_t chosen_streams_bf;
	uint32_t started_streams_bf;
	uint8_t  left_channel_pos;
	uint8_t  right_channel_pos;

	/* Scan/session state */
	bool     scanning_active;
	uint8_t  expected_streams;
	uint8_t  stream_report_count;

	/* Audio datapath */
	struct audio_datapath_config datapath_cfg;
	bool     datapath_cfg_valid;
};

static struct broadcast_sink_env sink_env;

/* ================================================================
 *               WORKER QUEUE
 * ================================================================
 */

enum sd_evt_type {
	SD_EVT_REMOTE_SCAN,
	SD_EVT_ADD,
	SD_EVT_MODIFY,
	SD_EVT_REMOVE,
	SD_EVT_ESTABLISHED
};

struct sd_evt {
	enum sd_evt_type type;

	/* BA context */
	uint8_t  src_lid;
	uint8_t  con_lid;

	/* Remote scan */
	uint8_t  remote_scan_state; /* 0 stop, 1 start */

	/* Add/Modify context */
	uint8_t  nb_subgroups;
	uint8_t  pa_sync_req;       /* 0x00 off, 0x01 PAST avail, 0x02 PAST not avail */
	uint16_t pa_intv_frames;

	bap_adv_id_t   adv_id;
	bap_bcast_id_t bcast_id;
};

K_MSGQ_DEFINE(sd_msgq, sizeof(struct sd_evt), SD_MSGQ_LEN, 4);
static struct k_work sd_work;

/* Async completion semaphores */
static K_SEM_DEFINE(sem_sink_disabled, 0, 1);
static K_SEM_DEFINE(sem_pa_synced,     0, 1);
static K_SEM_DEFINE(sem_pa_terminated, 0, 1);
static K_SEM_DEFINE(sem_scan_started,  0, 1);
static K_SEM_DEFINE(sem_scan_stopped,  0, 1);


/* ================================================================
 *                   HELPERS / INTERNAL API
 * ================================================================
 */

static void reset_sink_config(void)
{
	memset(&sink_env, 0, sizeof(sink_env));
	sink_env.datapath_cfg_valid = true;
	sink_env.datapath_cfg.i2s_dev = i2s_dev;
	sink_env.right_channel_pos = INVALID_CHANNEL_INDEX;
	sink_env.left_channel_pos  = INVALID_CHANNEL_INDEX;
	sink_env.src_lid = GAF_INVALID_LID;
	sink_env.con_lid = GAF_INVALID_LID;
	sink_env.pa_lid  = GAF_INVALID_LID;
	sink_env.grp_lid = GAF_INVALID_LID;
	sink_env.chosen_streams_bf  = 0;
	sink_env.started_streams_bf = 0;
	sink_env.scanning_active = false;
	sink_env.expected_streams = 0;
	sink_env.stream_report_count = 0;

	LOG_DBG("Reset sink config");
}

static int start_scanning(void)
{
	uint16_t err = bap_bc_scan_start(0);

	if (err != GAP_ERR_NO_ERROR) {
		LOG_ERR("Scanning failed: err=%u", err);
		return -ENODEV;
	}

	reset_sink_config();
	LOG_INF("Scanning started");
	return 0;
}

static int sink_enable(void)
{
	sink_env.chosen_streams_bf  = 0;
	sink_env.started_streams_bf = 0;

	if (!sink_env.datapath_cfg_valid) {
		LOG_ERR("Sink enable failed: invalid configuration");
		return -EINVAL;
	}

	if (sink_env.left_channel_pos != INVALID_CHANNEL_INDEX) {
		sink_env.chosen_streams_bf |= (1U << (sink_env.left_channel_pos - 1));
	}
	if (sink_env.right_channel_pos != INVALID_CHANNEL_INDEX) {
		sink_env.chosen_streams_bf |= (1U << (sink_env.right_channel_pos - 1));
	}

	LOG_INF("Sink enabled: pa_lid=%u bcast_id=%02x:%02x:%02x L=%u R=%u chosen_bf=0x%08x",
		sink_env.pa_lid,
		sink_env.bcast_id.id[0], sink_env.bcast_id.id[1], sink_env.bcast_id.id[2],
		sink_env.left_channel_pos, sink_env.right_channel_pos, sink_env.chosen_streams_bf);

	uint16_t err = bap_bc_sink_enable(sink_env.pa_lid, &sink_env.bcast_id,
					sink_env.chosen_streams_bf,
					NULL,
					0,
					SINK_TIMEOUT,
					&sink_env.grp_lid);
	if (err != GAF_ERR_NO_ERROR) {
		LOG_ERR("Sink enable failed: err=%u", err);
		return -EIO;
	}

	LOG_INF("Sink enabled: grp_lid=%u (pending ESTABLISHED)", sink_env.grp_lid);
	return 0;
}

static int start_streaming(void)
{
	gaf_codec_id_t codec_id = GAF_CODEC_ID_LC3;

	LOG_INF("Start streaming: grp=%u L=%u R=%u",
		sink_env.grp_lid, sink_env.left_channel_pos, sink_env.right_channel_pos);

	uint16_t err = 0;

	if (sink_env.left_channel_pos != INVALID_CHANNEL_INDEX) {
		err = bap_bc_sink_start_streaming(sink_env.grp_lid, sink_env.left_channel_pos,
						&codec_id, GAPI_DP_ISOOSHM, 0, NULL);
		LOG_INF("Start streaming: L pos=%u rc=%u", sink_env.left_channel_pos, err);
		if (err) {
			return -EIO;
		}
	}

	if (sink_env.right_channel_pos != INVALID_CHANNEL_INDEX) {
		err = bap_bc_sink_start_streaming(sink_env.grp_lid, sink_env.right_channel_pos,
						&codec_id, GAPI_DP_ISOOSHM, 0, NULL);
		LOG_INF("Start streaming: R pos=%u rc=%u", sink_env.right_channel_pos, err);
		if (err) {
			return -EIO;
		}
	}

	return 0;
}

static void sd_teardown_sink_and_wait(void)
{
	if (sink_env.grp_lid != GAF_INVALID_LID) {
		while (k_sem_count_get(&sem_sink_disabled) > 0) {
			k_sem_take(&sem_sink_disabled, K_NO_WAIT);
		}

		LOG_INF("Sink disable: grp=%u", sink_env.grp_lid);
		uint16_t rc = bap_bc_sink_disable(sink_env.grp_lid);

		if (rc != GAF_ERR_NO_ERROR) {
			LOG_WRN("Sink disable: rc=%u", rc);
		}

		bool ok = (k_sem_take(&sem_sink_disabled, SD_WAIT) == 0);

		if (!ok) {
			LOG_ERR("Sink disable: timeout waiting for status");
		}

		LOG_INF("Sink disable: done");
		sink_env.grp_lid = GAF_INVALID_LID;
	}

	audio_datapath_cleanup_sink();
}

/* ================================================================
 *                   BASS / DELEGATOR CALLBACKS
 * ================================================================
 */

static void on_bass_cmp_evt(uint8_t cmd_type, uint16_t status, uint8_t src_lid)
{
	LOG_DBG("BASS cmp event: cmd=%u status=%u src=%u", cmd_type, status, src_lid);
}

static void on_bass_solicite_stopped(uint8_t reason)
{
	LOG_DBG("BASS solicitation stopped: reason=%u", reason);
}

static void on_bass_bond_data(uint8_t con_lid, uint16_t cli_cfg_bf)
{
	LOG_DBG("BASS bond data: con=%u cli_cfg=0x%04x", con_lid, cli_cfg_bf);
}

static void on_bass_remote_scan(uint8_t con_lid, uint8_t state)
{
	LOG_INF("BASS remote scan: con=%u state=%u", con_lid, state);

	struct sd_evt evt = {0};

	evt.type = SD_EVT_REMOTE_SCAN;
	evt.remote_scan_state = state ? 1 : 0;

	int rc = k_msgq_put(&sd_msgq, &evt, K_NO_WAIT);

	if (rc != 0) {
		LOG_ERR("BASS remote scan: msgq_put rc=%d", rc);
		return;
	}

	(void)k_work_submit(&sd_work);
}

static void on_bass_bcast_code(uint8_t src_lid, uint8_t con_lid,
	const gaf_bcast_code_t *p_bcast_code)
{
	ARG_UNUSED(p_bcast_code);
	LOG_INF("BASS broadcast code: src=%u con=%u", src_lid, con_lid);
}

/* Add Source request from BA */
static void on_bass_add_source_req(uint8_t src_lid, uint8_t con_lid, const bap_adv_id_t *p_adv_id,
	const bap_bcast_id_t *p_bcast_id, uint8_t pa_sync_req,
	uint16_t pa_intv_frames, uint8_t nb_subgroups, uint16_t metadata_len)
{
	ARG_UNUSED(pa_intv_frames);
	ARG_UNUSED(metadata_len);

	LOG_INF("BASS add source request: src=%u con=%u pa_sync=%u nb_sgrp=%u meta_len=%u",
		src_lid, con_lid, pa_sync_req, nb_subgroups, metadata_len);

	for (uint8_t i = 0; i < nb_subgroups; i++) {
		uint32_t bis_mask = 0;
		bap_cfg_metadata_ptr_t meta_ptr = (bap_cfg_metadata_ptr_t){0};
		uint16_t err = bap_bc_deleg_get_sgrp_info(src_lid, &bis_mask, &meta_ptr);

		LOG_DBG("BASS add: get_sgrp[%u] rc=%u bis=0x%08x ctx=0x%04x",
			i, err, bis_mask, meta_ptr.param.context_bf);
		if (err != GAF_ERR_NO_ERROR) {
			break;
		}
	}

	struct sd_evt evt = {0};

	evt.type = SD_EVT_ADD;
	evt.src_lid = src_lid;
	evt.con_lid = con_lid;
	evt.pa_sync_req = pa_sync_req;
	evt.nb_subgroups = MIN(nb_subgroups, SD_MAX_SUBGROUPS);
	memcpy(&evt.adv_id,   p_adv_id,   sizeof(evt.adv_id));
	memcpy(&evt.bcast_id, p_bcast_id, sizeof(evt.bcast_id));

	int rc = k_msgq_put(&sd_msgq, &evt, K_NO_WAIT);

	if (rc != 0) {
		LOG_ERR("BASS add source request: msgq_put rc=%d", rc);
		return;
	}

	(void)k_work_submit(&sd_work);
}

/* Modify Source request from BA */
static void on_bass_modify_source_req(uint8_t src_lid, uint8_t con_lid, uint8_t pa_sync_req,
	uint16_t pa_intv_frames, uint8_t nb_subgroups, uint16_t metadata_len)
{
	ARG_UNUSED(pa_intv_frames);
	ARG_UNUSED(metadata_len);

	LOG_INF("BASS modify source request: src=%u con=%u pa_sync=%u nb_sgrp=%u meta_len=%u",
		src_lid, con_lid, pa_sync_req, nb_subgroups, metadata_len);

	for (uint8_t i = 0; i < nb_subgroups; i++) {
		uint32_t bis_mask = 0;
		bap_cfg_metadata_ptr_t meta_ptr = (bap_cfg_metadata_ptr_t){0};
		uint16_t err = bap_bc_deleg_get_sgrp_info(src_lid, &bis_mask, &meta_ptr);

		LOG_DBG("BASS modify source request: get_sgrp[%u] rc=%u bis=0x%08x ctx=0x%04x",
			i, err, bis_mask, meta_ptr.param.context_bf);
		if (err != GAF_ERR_NO_ERROR) {
			break;
		}
	}

	struct sd_evt evt = {0};

	evt.type = SD_EVT_MODIFY;
	evt.src_lid = src_lid;
	evt.con_lid = con_lid;
	evt.pa_sync_req = pa_sync_req;
	evt.nb_subgroups = MIN(nb_subgroups, SD_MAX_SUBGROUPS);

	int rc = k_msgq_put(&sd_msgq, &evt, K_NO_WAIT);

	if (rc != 0) {
		LOG_ERR("BASS modify source request: msgq_put rc=%d", rc);
		return;
	}

	(void)k_work_submit(&sd_work);
}

/* Remove Source request from BA */
static void on_bass_remove_source_req(uint8_t src_lid, uint8_t con_lid)
{
	LOG_INF("BASS remove_source_req: src=%u con=%u", src_lid, con_lid);

	struct sd_evt evt = {0};

	evt.type = SD_EVT_REMOVE;
	evt.src_lid = src_lid;
	evt.con_lid = con_lid;

	int rc = k_msgq_put(&sd_msgq, &evt, K_NO_WAIT);

	if (rc != 0) {
		LOG_ERR("BASS remove source request: msgq_put rc=%d", rc);
		return;
	}

	(void)k_work_submit(&sd_work);
}

static const struct bap_bc_deleg_cb bass_cbs = {
	.cb_cmp_evt           = on_bass_cmp_evt,
	.cb_solicite_stopped  = on_bass_solicite_stopped,
	.cb_bond_data         = on_bass_bond_data,
	.cb_remote_scan       = on_bass_remote_scan,
	.cb_bcast_code        = on_bass_bcast_code,
	.cb_add_source_req    = on_bass_add_source_req,
	.cb_modify_source_req = on_bass_modify_source_req,
	.cb_remove_source_req = on_bass_remove_source_req,
};

/* ================================================================
 *                   SCAN CALLBACKS
 * ================================================================
 */

static void on_bap_bc_scan_cmp_evt(uint8_t cmd_type, uint16_t status, uint8_t pa_lid)
{
	LOG_DBG("Scan cmp event: cmd=%u status=%u pa_lid=%u", cmd_type, status, pa_lid);

	switch (cmd_type) {
	case BAP_BC_SCAN_CMD_TYPE_START:
		k_sem_give(&sem_scan_started);
		break;
	case BAP_BC_SCAN_CMD_TYPE_STOP:
		k_sem_give(&sem_scan_stopped);
		break;
	case BAP_BC_SCAN_CMD_TYPE_PA_SYNCHRONIZE:
		/* wait in pa_established */
		break;
	case BAP_BC_SCAN_CMD_TYPE_PA_TERMINATE:
		k_sem_give(&sem_pa_terminated);
		break;
	default:
		break;
	}
}

static void on_bap_bc_scan_timeout(void)
{
	LOG_WRN("Scan timeout");
}

static void on_bap_bc_scan_report(const bap_adv_id_t *p_adv_id, const bap_bcast_id_t *p_bcast_id,
				uint8_t info_bf, const gaf_adv_report_air_info_t *p_air_info,
				uint16_t length, const uint8_t *p_data)
{
	ARG_UNUSED(info_bf);
	ARG_UNUSED(length);
	ARG_UNUSED(p_data);

	LOG_DBG("Scan adv report rssi=%d", p_air_info ? p_air_info->rssi : 0);
	memcpy(&sink_env.bcast_id, p_bcast_id, sizeof(sink_env.bcast_id));
	memcpy(&sink_env.adv_id,   p_adv_id,   sizeof(sink_env.adv_id));
}

static void on_bap_bc_scan_public_bcast(const bap_adv_id_t *p_adv_id,
					const bap_bcast_id_t *p_bcast_id, uint8_t pbp_features_bf,
					uint8_t broadcast_name_len, const uint8_t *p_broadcast_name,
					uint8_t metadata_len, const uint8_t *p_metadata)
{
	ARG_UNUSED(p_adv_id);
	ARG_UNUSED(p_bcast_id);
	ARG_UNUSED(pbp_features_bf);
	ARG_UNUSED(broadcast_name_len);
	ARG_UNUSED(p_broadcast_name);
	ARG_UNUSED(metadata_len);
	ARG_UNUSED(p_metadata);
}

static void on_bap_bc_scan_pa_established(uint8_t pa_lid, const bap_adv_id_t *p_adv_id,
					uint8_t phy, uint16_t interval_frames)
{
	ARG_UNUSED(p_adv_id);
	LOG_INF("PA established: pa_lid=%u phy=%u intv=%u", pa_lid, phy, interval_frames);
	sink_env.pa_lid = pa_lid;
	k_sem_give(&sem_pa_synced);
}

static void on_bap_bc_scan_pa_terminated(uint8_t pa_lid, uint8_t reason)
{
	LOG_INF("PA terminated: pa_lid=%u reason=%u", pa_lid, reason);
}

static void on_bap_bc_scan_pa_report(uint8_t pa_lid, const gaf_adv_report_air_info_t *p_air_info,
					uint16_t length, const uint8_t *p_data)
{
	ARG_UNUSED(pa_lid);
	ARG_UNUSED(length);
	ARG_UNUSED(p_data);
	LOG_DBG("PA report: rssi=%d", p_air_info ? p_air_info->rssi : 0);
}

static void on_bap_bc_scan_big_info_report(uint8_t pa_lid, const gapm_le_big_info_t *p_report)
{
	LOG_INF("BIGinfo: pa=%u sdu_int=%u iso_int=%ums max_pdu=%u max_sdu=%u num_bis=%u enc=%u",
		pa_lid, p_report->sdu_interval, p_report->iso_interval, p_report->max_pdu,
		p_report->max_sdu, p_report->num_bis, p_report->encrypted);
}

static void on_bap_bc_scan_group_report(uint8_t pa_lid, uint8_t nb_subgroups, uint8_t nb_streams,
					uint32_t pres_delay_us)
{
	LOG_INF("Group report: pa=%u subgrp=%u streams=%u pres_delay=%uus",
		pa_lid, nb_subgroups, nb_streams, pres_delay_us);

	sink_env.expected_streams = nb_streams;
	sink_env.stream_report_count = 0;
	sink_env.datapath_cfg.pres_delay_us = pres_delay_us;
}

static void on_bap_bc_scan_subgroup_report(uint8_t pa_lid, uint8_t sgrp_id, uint32_t stream_pos_bf,
					const gaf_codec_id_t *p_codec_id,
					const bap_cfg_ptr_t *p_cfg,
					const bap_cfg_metadata_ptr_t *p_metadata)
{
	ARG_UNUSED(pa_lid);
	ARG_UNUSED(p_codec_id);
	ARG_UNUSED(p_metadata);

	LOG_INF("Subgroup: id=%u stream_bf=0x%08x loc_bf=0x%04x frame_oct=%u samp=%u "
		"frame_dur=%u frames_sdu=%u",
		sgrp_id, stream_pos_bf, p_cfg->param.location_bf,
		p_cfg->param.frame_octet, p_cfg->param.sampling_freq,
		p_cfg->param.frame_dur, p_cfg->param.frames_sdu);

	/* Validate and stash config */
	if (p_cfg->param.sampling_freq < BAP_SAMPLING_FREQ_MIN ||
		p_cfg->param.sampling_freq > BAP_SAMPLING_FREQ_MAX) {
		LOG_WRN("Invalid sampling_freq=%u", p_cfg->param.sampling_freq);
		sink_env.datapath_cfg_valid = false;
	}
	if (p_cfg->param.frame_dur != BAP_FRAME_DUR_10MS) {
		LOG_WRN("Invalid frame_dur=%u need 10ms", p_cfg->param.frame_dur);
		sink_env.datapath_cfg_valid = false;
	}

	sink_env.datapath_cfg.octets_per_frame       = p_cfg->param.frame_octet;
	sink_env.datapath_cfg.frame_duration_is_10ms =
		(p_cfg->param.frame_dur == BAP_FRAME_DUR_10MS);
	sink_env.datapath_cfg.sampling_rate_hz       =
		audio_bap_sampling_freq_to_hz(p_cfg->param.sampling_freq);

	LOG_INF("Datapath cfg: sr=%uHz octets=%u 10ms=%u",
		sink_env.datapath_cfg.sampling_rate_hz,
		sink_env.datapath_cfg.octets_per_frame,
		sink_env.datapath_cfg.frame_duration_is_10ms);
}

static void assign_audio_channel(uint8_t stream_count, uint8_t stream_pos, uint16_t loc_bf)
{
#ifdef CONFIG_AUDIO_LOCATION_USE_GAF
	if ((loc_bf & GAF_LOC_LEFT_OR_CENTRE_MASK) &&
		(sink_env.left_channel_pos == INVALID_CHANNEL_INDEX))
#else
	if (stream_count == 0)
#endif
	{
		sink_env.left_channel_pos = stream_pos;
		LOG_INF("Select LEFT/CENTER stream_pos=%u (count=%u)", stream_pos, stream_count);
	}

#ifdef CONFIG_AUDIO_LOCATION_USE_GAF
	if ((loc_bf & GAF_LOC_RIGHT_MASK) &&
		(sink_env.right_channel_pos == INVALID_CHANNEL_INDEX))
#else
	if (stream_count == 1)
#endif
	{
		sink_env.right_channel_pos = stream_pos;
		LOG_INF("Select RIGHT stream_pos=%u (count=%u)", stream_pos, stream_count);
	}
}

static void on_bap_bc_scan_stream_report(uint8_t pa_lid, uint8_t sgrp_id, uint8_t stream_pos,
					const gaf_codec_id_t *p_codec_id,
					const bap_cfg_ptr_t *p_cfg)
{
	ARG_UNUSED(pa_lid);
	ARG_UNUSED(sgrp_id);
	ARG_UNUSED(p_codec_id);

	LOG_INF("Stream report: pos=%u loc_bf=0x%04x (count=%u/%u)",
		stream_pos, p_cfg->param.location_bf,
		sink_env.stream_report_count + 1, sink_env.expected_streams);

	assign_audio_channel(sink_env.stream_report_count, stream_pos, p_cfg->param.location_bf);

	if (++sink_env.stream_report_count >= sink_env.expected_streams) {
		sink_env.expected_streams = 0;
		sink_env.stream_report_count = 0;

		uint16_t rc = bap_bc_scan_pa_report_ctrl(sink_env.pa_lid, 0);

		LOG_DBG("PA report ctrl: disable rc=%u", rc);

		if (sink_env.left_channel_pos == INVALID_CHANNEL_INDEX) {
			LOG_ERR("No LEFT/CENTER stream present – aborting");
			sink_env.datapath_cfg_valid = false;
		}

		if (sink_env.datapath_cfg_valid) {
			int er = sink_enable();

			if (er != 0) {
				LOG_ERR("Sink enable failed: rc=%d", er);
			}
		} else {
			int er = start_scanning();

			if (er != 0) {
				LOG_ERR("Restart scanning failed: rc=%d", er);
			}
		}
	}
}

static void on_bap_bc_scan_pa_sync_req(uint8_t pa_lid, uint8_t src_lid, uint8_t con_lid)
{
	ARG_UNUSED(src_lid);
	ARG_UNUSED(con_lid);
	LOG_INF("PA sync req: pa=%u", pa_lid);
	bap_bc_scan_pa_synchronize_cfm(pa_lid, true, 0, 0, 0, 0);
}

static void on_bap_bc_scan_pa_terminate_req(uint8_t pa_lid, uint8_t con_lid)
{
	ARG_UNUSED(con_lid);
	LOG_INF("PA terminate req: pa=%u", pa_lid);
	bap_bc_scan_pa_terminate_cfm(pa_lid, true);
}

static const bap_bc_scan_cb_t scan_cbs = {
	.cb_cmp_evt             = on_bap_bc_scan_cmp_evt,
	.cb_timeout             = on_bap_bc_scan_timeout,
	.cb_report              = on_bap_bc_scan_report,
	.cb_public_bcast_source = on_bap_bc_scan_public_bcast,
	.cb_pa_established      = on_bap_bc_scan_pa_established,
	.cb_pa_terminated       = on_bap_bc_scan_pa_terminated,
	.cb_pa_report           = on_bap_bc_scan_pa_report,
	.cb_big_info_report     = on_bap_bc_scan_big_info_report,
	.cb_group_report        = on_bap_bc_scan_group_report,
	.cb_subgroup_report     = on_bap_bc_scan_subgroup_report,
	.cb_stream_report       = on_bap_bc_scan_stream_report,
	.cb_pa_sync_req         = on_bap_bc_scan_pa_sync_req,
	.cb_pa_terminate_req    = on_bap_bc_scan_pa_terminate_req,
};

/* ================================================================
 *                     SINK CALLBACKS
 * ================================================================
 */

static void on_bap_bc_sink_cmp_evt(uint8_t cmd_type, uint16_t status, uint8_t grp_lid,
				    uint8_t stream_pos)
{
	LOG_INF("Sink cmp event: cmd=%u status=%u grp=%u stream=%u", cmd_type, status, grp_lid,
		stream_pos);

	if (cmd_type == BAP_BC_SINK_CMD_TYPE_START_STREAMING && status == GAF_ERR_NO_ERROR) {
		sink_env.started_streams_bf |= (1U << (stream_pos - 1));
		LOG_INF("SINK started_bf=0x%08x chosen_bf=0x%08x",
			sink_env.started_streams_bf, sink_env.chosen_streams_bf);

		if (sink_env.started_streams_bf == sink_env.chosen_streams_bf) {
			LOG_INF("Datapath: create start (sr=%uHz oct=%u 10ms=%u pres_delay=%uus)",
				sink_env.datapath_cfg.sampling_rate_hz,
				sink_env.datapath_cfg.octets_per_frame,
				sink_env.datapath_cfg.frame_duration_is_10ms,
				sink_env.datapath_cfg.pres_delay_us);

			int ret = audio_datapath_create_sink(&sink_env.datapath_cfg);

			if (ret) {
				LOG_ERR("Datapath create failed rc=%d", ret);
				audio_datapath_cleanup_sink();
				return;
			}

			ret = audio_datapath_start();
			LOG_INF("Datapath start rc=%d", ret);
			if (ret) {
				audio_datapath_cleanup_sink();
			}
		}
	}
}

static void on_bap_bc_sink_quality_cmp_evt(uint16_t status, uint8_t grp_lid, uint8_t stream_pos,
					uint32_t crc_error_packets, uint32_t rx_unrx_packets,
					uint32_t duplicate_packets)
{
	ARG_UNUSED(status);
	LOG_DBG("SINK quality: grp=%u stream=%u crc=%u miss=%u dup=%u",
		grp_lid, stream_pos, crc_error_packets, rx_unrx_packets, duplicate_packets);
}

static void on_bap_bc_sink_status(uint8_t grp_lid, uint8_t state, uint32_t stream_pos_bf,
				const gapi_bg_sync_config_t *p_bg_cfg, uint8_t nb_bis,
				const uint16_t *p_conhdl)
{
	ARG_UNUSED(p_bg_cfg);
	ARG_UNUSED(p_conhdl);

	LOG_INF("SINK status: grp=%u state=%u stream_bf=0x%08x nb_bis=%u",
		grp_lid, state, stream_pos_bf, nb_bis);

	switch (state) {
	case BAP_BC_SINK_ESTABLISHED: {
		LOG_INF("SINK established (streams=0x%08x, nb_bis=%u) -> "
			"deferring PA terminate & start_streaming",
			stream_pos_bf, nb_bis);

		struct sd_evt evt = { .type = SD_EVT_ESTABLISHED };
		int rc = k_msgq_put(&sd_msgq, &evt, K_NO_WAIT);

		if (rc != 0) {
			LOG_ERR("status established: msgq_put rc=%d", rc);
			break;
		}
		(void)k_work_submit(&sd_work);
		break;
	}

	case BAP_BC_SINK_FAILED:
	case BAP_BC_SINK_CANCELLED:
	case BAP_BC_SINK_LOST:
	case BAP_BC_SINK_PEER_TERMINATE:
	case BAP_BC_SINK_MIC_FAILURE:
	case BAP_BC_SINK_UPPER_TERMINATE:
		k_sem_give(&sem_sink_disabled);
		break;

	default:
		break;
	}
}

static void on_bap_bc_sink_enable_req(uint8_t grp_lid, uint8_t src_lid, uint8_t con_lid,
				       uint32_t stream_pos_bf
#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 /* ROM version > 1.0 */
				       , uint32_t stream_pos_bf_opt
#endif /* !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 */
						       )
{
	LOG_INF("SINK enable_req: grp=%u src=%u con=%u stream_bf=0x%08x",
		grp_lid, src_lid, con_lid, stream_pos_bf);
	bap_bc_sink_enable_cfm(grp_lid, true, stream_pos_bf, 1000, 1);
}

static void on_bap_bc_sink_disable_req(uint8_t grp_lid, uint8_t con_lid)
{
	LOG_INF("SINK disable_req: grp=%u con=%u", grp_lid, con_lid);
	bap_bc_sink_disable_cfm(grp_lid, true);
}

static const bap_bc_sink_cb_t sink_cbs = {
	.cb_cmp_evt         = on_bap_bc_sink_cmp_evt,
	.cb_quality_cmp_evt = on_bap_bc_sink_quality_cmp_evt,
	.cb_status          = on_bap_bc_sink_status,
	.cb_enable_req      = on_bap_bc_sink_enable_req,
	.cb_disable_req     = on_bap_bc_sink_disable_req,
};

/* ================================================================
 *                         WORKER (SM)
 * ================================================================
 */

static void sd_handle_remote_scan(const struct sd_evt *e)
{
	if (e->remote_scan_state) {
		int rc = start_scanning();

		if (rc != 0) {
			LOG_ERR("Remote scan start: rc=%d", rc);
			return;
		}

		bool ok = (k_sem_take(&sem_scan_started, SD_WAIT) == 0);

		if (!ok) {
			LOG_ERR("Remote scan start: timeout waiting for START");
			return;
		}

		sink_env.scanning_active = true;
	} else {
		uint16_t err = bap_bc_scan_stop();

		if (err != GAF_ERR_NO_ERROR && err != GAF_ERR_COMMAND_DISALLOWED) {
			LOG_WRN("Remote scan stop: err=%u", err);
		}

		bool ok = (k_sem_take(&sem_scan_stopped, SD_WAIT) == 0);

		if (!ok) {
			LOG_ERR("Remote scan stop: timeout waiting for STOP");
			return;
		}

		sink_env.scanning_active = false;
	}
}

static void sd_handle_add(const struct sd_evt *e)
{
	/* Persist IDs for later Modify */
	sink_env.src_lid = e->src_lid;
	sink_env.con_lid = e->con_lid;
	memcpy(&sink_env.bcast_id, &e->bcast_id, sizeof(sink_env.bcast_id));
	memcpy(&sink_env.adv_id,   &e->adv_id,   sizeof(sink_env.adv_id));

	LOG_INF("Add source request: src=%u con=%u bcast_id=%02x:%02x:%02x",
		e->src_lid, e->con_lid,
		sink_env.bcast_id.id[0], sink_env.bcast_id.id[1], sink_env.bcast_id.id[2]);

	/* Confirm ADD from worker context */
	bap_bc_deleg_add_source_cfm(e->src_lid, true);
	LOG_INF("Add source request: cfm sent");

	if (e->pa_sync_req && sink_env.pa_lid == GAF_INVALID_LID) {
		uint16_t rc = bap_bc_scan_pa_synchronize(&sink_env.adv_id, 0,
							BAP_BC_SCAN_REPORT_MASK,
							SYNCHRONISATION_TIMEOUT, SCAN_TIMEOUT,
							&sink_env.pa_lid);
		LOG_INF("PA sync start: rc=%u -> pa_lid=%u", rc, sink_env.pa_lid);
		bool ok = (k_sem_take(&sem_pa_synced, SD_WAIT) == 0);

		if (!ok) {
			LOG_ERR("Add source request: timeout waiting for PA establish");
		}
	}

	if (sink_env.scanning_active) {
		uint16_t rc = bap_bc_scan_stop();

		if (rc != GAF_ERR_NO_ERROR && rc != GAF_ERR_COMMAND_DISALLOWED) {
			LOG_WRN("Scan stop: err=%u", rc);
		}
		bool ok = (k_sem_take(&sem_scan_stopped, SD_WAIT) == 0);

		if (!ok) {
			LOG_ERR("Add source request: timeout waiting for scan stop");
		} else {
			sink_env.scanning_active = false;
		}
	}
}

static void sd_handle_modify(const struct sd_evt *e)
{
	LOG_INF("Modify source request: src=%u con=%u pa_sync=%u", e->src_lid, e->con_lid,
		e->pa_sync_req);

	/* 1) Bring BIG down and wait */
	sd_teardown_sink_and_wait();

	/* 2) Confirm MODIFY from worker context (let PA be driven by stack PA_* reqs) */
	bap_bc_deleg_modify_source_cfm(e->src_lid, true);
	LOG_INF("Modify source request: cfm sent");
}

static void sd_handle_remove(const struct sd_evt *e)
{
	LOG_INF("Remove source request: src=%u con=%u", e->src_lid, e->con_lid);

	/* 1) Bring BIG down and wait */
	sd_teardown_sink_and_wait();

	/* 2) Confirm REMOVE (stack will drive PA terminate if needed) */
	bap_bc_deleg_remove_source_cfm(sink_env.src_lid, true);
	LOG_INF("Remove source request: cfm sent");

	reset_sink_config();
}

static void sd_handle_established(void)
{
	/* Drop PA, but don't block the worker waiting for it */
	if (sink_env.pa_lid != GAF_INVALID_LID) {
		uint16_t rc = bap_bc_scan_pa_terminate(sink_env.pa_lid);

		LOG_INF("Established: request PA terminate rc=%u (pa_lid=%u)", rc, sink_env.pa_lid);
		sink_env.pa_lid = GAF_INVALID_LID;
	}

	/* Start streaming */
	int rc = start_streaming();

	if (rc) {
		LOG_ERR("Established: start_streaming rc=%d", rc);
	}
}

static void sd_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	struct sd_evt e;

	while (k_msgq_get(&sd_msgq, &e, K_NO_WAIT) == 0) {
		switch (e.type) {
		case SD_EVT_REMOTE_SCAN:
			sd_handle_remote_scan(&e);
			break;

		case SD_EVT_ADD:
			sd_handle_add(&e);
			break;

		case SD_EVT_MODIFY:
			sd_handle_modify(&e);
			break;

		case SD_EVT_REMOVE:
			sd_handle_remove(&e);
			break;

		case SD_EVT_ESTABLISHED:
			sd_handle_established();
			break;

		default:
			break;
		}
	}
}

/* ================================================================
 *                     SYSTEM / MODULE INIT
 * ================================================================
 */

int broadcast_sink_start_solicitation(const char *device_name, uint16_t appearance)
{
	/* Prepare extended advertising data for BASS solicitation */
	uint8_t adv_data_buffer[64];
	uint8_t adv_data_len = 0;

	if (device_name != NULL) {
		size_t name_len = strlen(device_name);

		if (name_len > 0) {
			if (name_len > MAX_DEVICE_NAME_LEN) {
				name_len = MAX_DEVICE_NAME_LEN;
			}
			if ((adv_data_len + 2 + name_len) <= sizeof(adv_data_buffer)) {
				adv_data_buffer[adv_data_len++] = (uint8_t)(name_len + 1);
				adv_data_buffer[adv_data_len++] = GAP_AD_TYPE_COMPLETE_NAME;
				memcpy(&adv_data_buffer[adv_data_len], device_name, name_len);
				adv_data_len += (uint8_t)name_len;
			}
		}
	}

	/* Add appearance*/
	if ((adv_data_len + 4) <= sizeof(adv_data_buffer)) {
		adv_data_buffer[adv_data_len++] = 3;
		adv_data_buffer[adv_data_len++] = GAP_AD_TYPE_APPEARANCE;
		adv_data_buffer[adv_data_len++] = (uint8_t)(appearance & 0xFF);
		adv_data_buffer[adv_data_len++] = (uint8_t)((appearance >> 8) & 0xFF);
	}

	/* Create LTV structure for the EA data */
	uint8_t ltv_buffer[sizeof(gaf_ltv_t) + 31];
	gaf_ltv_t *adv_data_ltv = (gaf_ltv_t *)ltv_buffer;

	adv_data_ltv->len = adv_data_len;
	if (adv_data_len > 0) {
		memcpy(adv_data_ltv->data, adv_data_buffer, adv_data_len);
		LOG_INF("Solicitation EA payload: %u bytes", adv_data_len);
	}

	/* Start BASS solicitation advertising */
	bap_bc_adv_param_t adv_param = {
		.adv_intv_min_slot = 160,
		.adv_intv_max_slot = 160,
		.ch_map            = ADV_ALL_CHNLS_EN,
		.phy_prim          = GAPM_PHY_TYPE_LE_1M,
		.phy_second        = GAPM_PHY_TYPE_LE_2M,
		.adv_sid           = 0x01,
#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 /* ROM version > 1.0 */
		.tx_pwr = -2,
#else
		.max_tx_pwr = -2,
#endif /* !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 */
	};

	uint16_t err = bap_bc_deleg_start_solicite(0, &adv_param,
					(adv_data_len > 0) ? adv_data_ltv : NULL);

	if (err != GAF_ERR_NO_ERROR) {
		LOG_ERR("BASS solicitation failed, err=%u (0x%02X)", err, err);
		return -EIO;
	}

	LOG_INF("BASS solicitation started");
	return 0;
}

static int _broadcast_sink_init(void)
{
	if (!device_is_ready(i2s_dev)) {
		LOG_ERR("I2S not ready");
		return -ENODEV;
	}

	if (!device_is_ready(codec_dev)) {
		LOG_ERR("Codec not ready");
		return -ENODEV;
	}

	LOG_INF("Audio HW ready");
	return 0;
}
SYS_INIT(_broadcast_sink_init, APPLICATION, 0);

int broadcast_sink_init(void)
{
	reset_sink_config();

	/* Worker */
	k_work_init(&sd_work, sd_work_fn);

	/* Configure Delegator (BASS) */
	bap_bc_deleg_cfg_t bass_cfg = {
		.nb_srcs  = 1,
		.cfg_bf   = 0,
		.shdl     = GATT_INVALID_HDL,
		.pref_mtu = GAP_LE_MAX_OCTETS,
	};
	uint16_t err = bap_bc_deleg_configure(&bass_cbs, &bass_cfg);

	if (err != GAF_ERR_NO_ERROR) {
		LOG_ERR("deleg_configure err=%u", err);
		return -ENODEV;
	}

	/* Configure Scan + Deleg + Sink roles */
	err = bap_bc_scan_configure(BAP_ROLE_SUPP_BC_SINK_BIT | BAP_ROLE_SUPP_BC_DELEG_BIT |
					BAP_ROLE_SUPP_BC_SCAN_BIT,
					&scan_cbs);
	if (err != GAP_ERR_NO_ERROR) {
		LOG_ERR("scan_configure err=%u", err);
		return -ENODEV;
	}

	err = bap_bc_sink_configure(BAP_ROLE_SUPP_BC_SINK_BIT | BAP_ROLE_SUPP_BC_DELEG_BIT |
					BAP_ROLE_SUPP_BC_SCAN_BIT,
					&sink_cbs);
	if (err != GAP_ERR_NO_ERROR) {
		LOG_ERR("sink_configure err=%u", err);
		return -ENODEV;
	}

	if (!bap_bc_deleg_is_configured()) {
		LOG_ERR("BASS not configured");
		return -ENODEV;
	}

	LOG_INF("Broadcast sink BLE initialized");
	return 0;
}

/* Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include "alif_ble.h"
#include "gapm.h"
#include "gap_le.h"
#include "broadcast_source.h"
#include "audio_datapath.h"

LOG_MODULE_REGISTER(main, CONFIG_MAIN_LOG_LEVEL);

#define MIC_ENABLED (DT_NODE_EXISTS(DT_ALIAS(i2s_mic)) && DT_NODE_EXISTS(DT_ALIAS(sw0)))

#if MIC_ENABLED
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET_OR(DT_ALIAS(sw0), gpios, {0});
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {0});

#if CONFIG_I2S_SYNC_BUFFER_FORMAT_SEQUENTIAL
#error "Sequential buffer format is not supported"
#endif
#endif

/**
 * Bluetooth stack configuration
 */
static const gapm_config_t gapm_cfg = {
	.role = GAP_ROLE_LE_BROADCASTER,
	.pairing_mode = GAPM_PAIRING_DISABLE,
	.privacy_cfg = 0,
	.renew_dur = 1500,
	.private_identity.addr = {0, 0, 0, 0, 0, 0},
	.irk.key = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
	.gap_start_hdl = 0,
	.gatt_start_hdl = 0,
	.att_cfg = 0,
	.sugg_max_tx_octets = GAP_LE_MAX_OCTETS,
	/* Use the minimum transmission time to minimize latency */
	.sugg_max_tx_time = GAP_LE_MIN_TIME,
	.tx_pref_phy = GAP_PHY_ANY,
	.rx_pref_phy = GAP_PHY_ANY,
	.tx_path_comp = 0,
	.rx_path_comp = 0,
	.class_of_device = 0,  /* BT Classic only */
	.dflt_link_policy = 0, /* BT Classic only */
};

#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 /* ROM version > 1.0 */
static void on_gapm_err(uint32_t metainfo, uint8_t code)
{
	LOG_ERR("gapm error %d", code);
}

static const gapm_cb_t gapm_err_cbs = {
	.cb_hw_error = on_gapm_err,
};

/* For the broadcaster role, callbacks are not mandatory */
static const gapm_callbacks_t gapm_cbs = {
	.p_con_req_cbs = NULL,
	.p_sec_cbs = NULL,
	.p_info_cbs = NULL,
	.p_le_config_cbs = NULL,
	.p_bt_config_cbs = NULL,
	.p_gapm_cbs = &gapm_err_cbs,
};
#else /* ROM version 1.0 */
static void on_gapm_err(enum co_error err)
{
	LOG_ERR("gapm error %d", err);
}

static const gapm_err_info_config_cb_t gapm_err_cbs = {
	.ctrl_hw_error = on_gapm_err,
};

/* For the broadcaster role, callbacks are not mandatory */
static const gapm_callbacks_t gapm_cbs = {
	.p_con_req_cbs = NULL,
	.p_sec_cbs = NULL,
	.p_info_cbs = NULL,
	.p_le_config_cbs = NULL,
	.p_bt_config_cbs = NULL,
	.p_err_info_config_cbs = &gapm_err_cbs,
};
#endif

static void on_gapm_process_complete(uint32_t metainfo, uint16_t status)
{
	if (status) {
		LOG_ERR("gapm process completed with error %u", status);
		return;
	}

	LOG_DBG("gapm process completed successfully");

	int ret = broadcast_source_start();

	if (ret != 0) {
		LOG_ERR("Failed to start broadcast source with error %d", ret);
	}
}

#if MIC_ENABLED

#define BUTTON_DEBOUNCE_MS  5
#define PTT_MINIMUM_TIME_MS 200
#define BUTTON_INVERTED     true

static void set_led(bool const state)
{
	if (!led.port) {
		return;
	}
	gpio_pin_set_dt(&led, state);
}

static bool get_button_state(void)
{
	if (!button.port) {
		return false;
	}
	return !!gpio_pin_get_dt(&button) ^ BUTTON_INVERTED;
}

static void stop_push_to_talk(struct k_work *work)
{
	ARG_UNUSED(work);
	/* Stop push to talk.... */
	audio_datapath_mic_control(false);
	set_led(false);
}

static K_WORK_DELAYABLE_DEFINE(stop_ptt_work, stop_push_to_talk);

static bool last_button_state = true;

static void debounce_expired(struct k_work *work)
{
	ARG_UNUSED(work);

	bool const button_state = get_button_state();

	if (button_state == last_button_state) {
		/* Ignore if state is not changed */
		return;
	}
	last_button_state = button_state;

	if (button_state) {
		/* pressed... */
		(void)k_work_cancel_delayable(&stop_ptt_work);
		audio_datapath_mic_control(true);
		set_led(true);
		return;
	}

	/* Delay shutdown a bit to avoid weird behaviors */
	k_work_reschedule(&stop_ptt_work, K_MSEC(PTT_MINIMUM_TIME_MS));
}

static K_WORK_DELAYABLE_DEFINE(debounce_work, debounce_expired);

static void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	k_work_reschedule(&debounce_work, K_MSEC(BUTTON_DEBOUNCE_MS));
}

static int configure_button(void)
{
	static struct gpio_callback button_cb_data;

	/* Configure button */
	if (!gpio_is_ready_dt(&button)) {
		LOG_WRN("Button is not ready or not configured... ignore");
		return 0;
	}

	if (gpio_pin_configure_dt(&button, GPIO_INPUT)) {
		LOG_ERR("Button configure failed");
		return -1;
	}

	if (gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_BOTH) != 0) {
		LOG_ERR("button int conf failed");
		return -1;
	}

	gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
	if (gpio_add_callback(button.port, &button_cb_data) != 0) {
		LOG_ERR("cb add failed");
		return -1;
	}

	last_button_state = get_button_state();

	LOG_INF("push-to-talk functionality enabled");

	return 0;
}

static int configure_led(void)
{
	if (!gpio_is_ready_dt(&led)) {
		return 0;
	}

	if (gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE)) {
		LOG_ERR("led configure failed");
		return -1;
	}

	set_led(false);
	return 0;
}
#else
#define configure_button() 0
#define configure_led()    0
#endif

int main(void)
{
	if (configure_button()) {
		return -1;
	}

	if (configure_led()) {
		return -1;
	}

	int ret = alif_ble_enable(NULL);

	if (ret) {
		LOG_ERR("Failed to enable bluetooth, err %d", ret);
		return ret;
	}

	LOG_DBG("BLE enabled");

	uint16_t err = gapm_configure(0, &gapm_cfg, &gapm_cbs, on_gapm_process_complete);

	if (err != 0) {
		LOG_ERR("gapm_configure error %u", err);
		return -1;
	}

	while (1) {
		k_sleep(K_SECONDS(5));
	}

	return 0;
}

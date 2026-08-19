/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/printk.h>

#include <zephyr/drivers/can.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ALIF_CAN, LOG_LEVEL_INF);

/* Bitrate macros */
#define CAN_NOMINAL_BITRATE      500000U  /* 500kbps       */
#define CAN_NOMINAL_SAMPLE_POINT 800U     /* In permille   */
#define CAN_FD_BITRATE           2000000U /* 2Mbps         */
#define CAN_FD_SAMPLE_POINT      800U     /* In permille   */

#define CAN_RX_FILTER_CODE_1	 0x5A5U
#define CAN_RX_FILTER_CODE_2	 0x01FF5A5AU
#define CAN_RX_FILTER_MASK		 0U

/* Application Message Frame types */
enum can_frame_type {
	CAN_FRAME_TYPE_STD_ID_CLASSIC_DATA,
	CAN_FRAME_TYPE_STD_ID_RTR,
	CAN_FRAME_TYPE_STD_ID_FD_DATA,
	CAN_FRAME_TYPE_EXT_ID_RTR,
	CAN_FRAME_TYPE_EXT_ID_CLASSIC_DATA,
	CAN_FRAME_TYPE_EXT_ID_FD_DATA,
	CAN_FRAME_TYPE_OVER
};

static struct can_frame rx_frame = {0};
static struct can_frame tx_frame = {
						.data = "!!!!!!***** CANFD"
								" TESTAPP Message"
								" Communication Test"
								" *****!!!!!!",
};
/* Define for Zephyr notification objects */
#define CAN_RX_SUCCESS        0x01U
#define CAN_ERROR             0x02U
#define CAN_ALL_NOTIFICATIONS (CAN_RX_SUCCESS | CAN_ERROR)

/* Thread information */
#define STACK_SIZE			  1024U
#define LB_THREAD_PRIORITY	  5U

K_THREAD_STACK_DEFINE(lb_thread_stack, STACK_SIZE);
static struct k_thread lb_thread_data;

/* Define an event object */
struct k_event can_event;

/**
 * @fn          static void rx_event(const struct device *dev,
 *                                   struct can_frame *frame,
 *                                   void *user_data
 * @brief       Handles Rx event
 * @param[in]   dev        : CAN struct device
 * @param[in]   frame      : CAN Message frame
 * @param[in]   user_data  : User data
 * @return      none
 */
static void rx_event(const struct device *dev, struct can_frame *frame, void *user_data)
{
	uint8_t iter;

	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	rx_frame.id = frame->id;
	rx_frame.dlc = frame->dlc;
	rx_frame.flags = frame->flags;
	rx_frame.timestamp = frame->timestamp;

	/* Copy the data*/
	for (iter = 0U; iter < can_dlc_to_bytes(frame->dlc); iter++) {
		rx_frame.data[iter] = frame->data[iter];
	}

	/* Notify Rx Success */
	k_event_set(&can_event, CAN_RX_SUCCESS);
}

/**
 * @fn          static void state_change_event(const struct device *dev,
 *							enum can_state state,
 *							struct can_bus_err_cnt err_cnt,
 *							void *user_data)
 * @brief       Handles State change event
 * @param[in]   dev        : CAN struct device
 * @param[in]   state      : Error state
 * @param[in]   err_cnt    : Error counts
 * @param[in]   user_data  : User data
 * @return      none
 */
static void state_change_event(const struct device *dev, enum can_state state,
			struct can_bus_err_cnt err_cnt, void *user_data)
{
	if ((state != CAN_STATE_ERROR_ACTIVE) || (state != CAN_STATE_STOPPED)) {
		/* Notify CAN Error */
		k_event_set(&can_event, CAN_ERROR);
	}
	LOG_INF("New state:%d, tx_err:%d, rx_err:%d", state, err_cnt.tx_err_cnt,
		err_cnt.rx_err_cnt);
}

/**
 * @fn          static bool rx_event(const struct device *dev,
 *                                   struct can_frame *frame,
 *                                   void *user_data
 * @brief       Handles Rx event
 * @param[in]   dev        : CAN struct device
 * @param[in]   frame      : CAN Message frame
 * @param[in]   user_data  : User data
 * @return      Send status
 */

static bool can_transmit_msg(const struct device *dev, const enum can_frame_type msg_type)
{
	int32_t status = 0;
	bool tx_status = true;

	switch (msg_type) {
	case CAN_FRAME_TYPE_STD_ID_CLASSIC_DATA:
		/* Sending Classic CAN DATA message of
		 * length 5 bytes with Message Id 0x5A5
		 */
		tx_frame.id = 0x5A5U;
		tx_frame.dlc = 0x5U;
		tx_frame.flags = 0;
		break;
	case CAN_FRAME_TYPE_STD_ID_RTR:
		/* Sending Classic CAN Remote request message
		 * with Message Id 0x5A5
		 */
		tx_frame.id = 0x5A5U;
		tx_frame.dlc = 0x0U;
		tx_frame.flags = CAN_FRAME_RTR;
		break;
	case CAN_FRAME_TYPE_STD_ID_FD_DATA:
		/* Sending FD CAN DATA message of
		 * length 64 bytes with Message Id 0x5A5
		 */
		tx_frame.id = 0x5A5U;
		tx_frame.dlc = 0xFU;
		tx_frame.flags = (CAN_FRAME_FDF | CAN_FRAME_BRS);
		break;
	case CAN_FRAME_TYPE_EXT_ID_RTR:
		/* Sending Classic CAN Remote request message
		 * with Extended Message Id 0x1FF5A5AU
		 */
		tx_frame.id = 0x01FF5A5AU;
		tx_frame.dlc = 0x0U;
		tx_frame.flags = (CAN_FRAME_IDE | CAN_FRAME_RTR);
		break;
	case CAN_FRAME_TYPE_EXT_ID_CLASSIC_DATA:
		/* Sending Classic CAN data message of
		 * length 8 bytes with Extended Message Id 0x1FF5A5AU
		 */
		tx_frame.id = 0x01FF5A5AU;
		tx_frame.dlc = 0x8U;
		tx_frame.flags = CAN_FRAME_IDE;

		break;
	case CAN_FRAME_TYPE_EXT_ID_FD_DATA:
		/* Sending FD CAN message of length 16 bytes
		 * with Extended Message Id 0x1FF5A5AU
		 */
		tx_frame.id = 0x01FF5A5AU;
		tx_frame.dlc = 0xAU;
		tx_frame.flags = (CAN_FRAME_IDE | CAN_FRAME_FDF | CAN_FRAME_BRS);

		break;

	case CAN_FRAME_TYPE_OVER:
	default:
		return tx_status;
	}

	LOG_INF("Sending MsgType: %d", msg_type);

	/* Sends the message to CAN HAL Driver */
	status = can_send(dev, &tx_frame, K_FOREVER, NULL, NULL);
	if (status != 0) {
		LOG_INF("Error: Failed to send message");
		tx_status = false;
	}

	return tx_status;
}

/**
 * @fn      static bool can_process_rx_msg(void)
 * @brief   Processes the received messages
 * @note    none
 * @param   none
 * @retval  Msg processed status
 */
static bool can_process_rx_msg(void)
{
	bool rx_status = true;

	if ((rx_frame.id != tx_frame.id) || (rx_frame.dlc != tx_frame.dlc) ||
	    (rx_frame.flags != tx_frame.flags)) {
		LOG_INF("Error : Tx and Rx msg Header mismatch");
		rx_status = false;

	} else if (memcmp(rx_frame.data, tx_frame.data, can_dlc_to_bytes(rx_frame.dlc)) != 0) {
		LOG_INF("Error : Tx and Rx msg Payload mismatch");
		rx_status = false;
	}

	LOG_INF("Msg Rx Success");

	return rx_status;
}

/**
 * @fn          static void loopback_thread_entry(void *arg1, void *arg2, void *arg3)
 * @brief       Performs CAN Loopback test
 * @param[in]   arg1  : Argument 1
 * @param[in]   arg2  : Argument 2
 * @param[in]   arg3  : Argument 3
 * @return      none
 */
static void loopback_thread_entry(void *arg1, void *arg2, void *arg3)
{
	const struct device *can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
	enum can_frame_type msg_type = CAN_FRAME_TYPE_STD_ID_CLASSIC_DATA;
	struct can_timing timing	 = {0};
	struct can_filter rx_filter;
	uint32_t event;
	can_mode_t can_cap;
	int ret;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	LOG_INF("*** CAN Loopback Demo Started ***");

	if (!device_is_ready(can_dev)) {
		LOG_INF("Device is not ready");
		return;
	}

	ret = can_get_capabilities(can_dev, &can_cap);

	LOG_INF("Device capabilities: %d", can_cap);

	if (can_calc_timing(can_dev, &timing, CAN_NOMINAL_BITRATE, CAN_NOMINAL_SAMPLE_POINT) != 0) {
		LOG_INF("Error in bit time calculation");
	}

	if (can_set_timing(can_dev, &timing) != 0) {
		LOG_INF("Error in setting Nominal Bitrate");
		return;
	}

	LOG_INF("Set Nominal Bitrate: %d, sample point: %d", CAN_NOMINAL_BITRATE,
		CAN_NOMINAL_SAMPLE_POINT);

	if (can_calc_timing_data(can_dev, &timing, CAN_FD_BITRATE, CAN_FD_SAMPLE_POINT) != 0) {
		LOG_INF("Error in data bit time calculation");
	}

	if (can_set_timing_data(can_dev, &timing) != 0) {
		LOG_INF("Error in setting Data Bitrate");
	}

	LOG_INF("Set FD Bitrate: %d, sample point: %d", CAN_FD_BITRATE, CAN_FD_SAMPLE_POINT);

	rx_filter.id = CAN_RX_FILTER_CODE_1;
	rx_filter.mask = CAN_RX_FILTER_MASK;
	rx_filter.flags = 0U;

	ret = can_add_rx_filter(can_dev, rx_event, NULL, &rx_filter);
	if (ret < 0) {
		LOG_INF("Error in Nominal Rx filter config");
		return;
	}

	LOG_INF("Normal Filter ID: %d", rx_filter.id);

	rx_filter.id = CAN_RX_FILTER_CODE_2;
	rx_filter.mask = CAN_RX_FILTER_MASK;
	rx_filter.flags = CAN_FILTER_IDE;

	ret = can_add_rx_filter(can_dev, rx_event, NULL, &rx_filter);

	if (ret < 0) {
		LOG_INF("Error in Extended Rx filter config");
	}

	LOG_INF("Extended Filter ID: %d", rx_filter.id);

	can_set_state_change_callback(can_dev, state_change_event, 0);

	if (can_set_mode(can_dev, (CAN_MODE_LOOPBACK | CAN_MODE_FD)) != 0) {
		LOG_INF("Error in setting Loopback with FD mode");
	}

	if (can_start(can_dev) != 0) {
		LOG_INF("Error in starting CAN");
	}

	/* Performs Loopback over all message types */
	while (msg_type != CAN_FRAME_TYPE_OVER) {
		if (can_transmit_msg(can_dev, msg_type)) {

			event = k_event_wait(&can_event, CAN_ALL_NOTIFICATIONS, false, K_FOREVER);

			if (event & CAN_ERROR) {
				LOG_INF("Error in Loopback communication");
				break;
			} else if (event & CAN_RX_SUCCESS) {
				if (!can_process_rx_msg()) {
					break;
				}
			}
			msg_type++;
		} else {
			break;
		}

		/* Sleep */
		k_msleep(10);
	}

	if (can_stop(can_dev) != 0) {
		LOG_INF("Error in stopping device");
	}

	LOG_INF("*** CAN Loopback Demo is over ***");
}

/**
 * @fn          int main(void)
 * @brief       Entry point of app
 * @param[in]   none
 * @return      none
 */
int main(void)
{

	/* Create an event */
	k_event_init(&can_event);

	/* Create the thread */
	k_tid_t lb_tid = k_thread_create(&lb_thread_data, lb_thread_stack, STACK_SIZE,
					 &loopback_thread_entry, NULL, NULL, NULL,
					 LB_THREAD_PRIORITY, 0, K_NO_WAIT);
	if (!lb_tid) {
		LOG_INF("Unable to Create Loopback thread");
	}

	return 0;
}

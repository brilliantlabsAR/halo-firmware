/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_BLE_MANAGER_H
#define HALO_BLE_MANAGER_H

#include <zephyr/kernel.h>
#include <zephyr/sys/dlist.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief BLE event types
 */
typedef enum {
	HALO_BLE_EVENT_CONNECTED,      /**< Device connected */
	HALO_BLE_EVENT_DISCONNECTED,   /**< Device disconnected */
	HALO_BLE_EVENT_PAIRED,         /**< Pairing succeeded (may precede encryption on reconnect) */
	HALO_BLE_EVENT_UNPAIRED,       /**< Device unpaired */
	HALO_BLE_EVENT_MTU_CHANGED,    /**< MTU size changed */
	HALO_BLE_EVENT_PHY_UPDATED,    /**< PHY mode updated */
	HALO_BLE_EVENT_ADV_STARTED,    /**< Advertising started */
	HALO_BLE_EVENT_ADV_STOPPED,    /**< Advertising stopped */
	HALO_BLE_EVENT_ENCRYPTED,      /**< Link encryption established */
} halo_ble_event_t;

/**
 * @brief BLE event data structure
 */
struct halo_ble_event_data {
	halo_ble_event_t event;
	uint8_t conidx;
	union {
		struct {
			uint16_t reason;
		} disconnected;
		struct {
			uint16_t mtu;
		} mtu_changed;
		struct {
			uint8_t tx_phy;
			uint8_t rx_phy;
		} phy_updated;
		struct {
			uint8_t reason;
		} adv_stopped;
	};
};

/**
 * @brief BLE event callback function
 * 
 * @param event Event data
 * @param user_data User data pointer
 * @return 0 on success, negative error code on failure
 */
typedef int (*halo_ble_event_cb_t)(const struct halo_ble_event_data *event, void *user_data);

/**
 * @brief BLE event callback node
 */
struct halo_ble_callback {
	sys_dnode_t node;
	halo_ble_event_cb_t callback;
	void *user_data;
	const char *name;
	int priority;              /**< Higher priority callbacks are called first */
	uint32_t event_mask;       /**< Bit mask of events to receive */
};

/**
 * @brief BLE connection parameters
 */
struct halo_ble_conn_params {
	uint16_t interval_min;     /**< Connection interval min (units of 1.25ms) */
	uint16_t interval_max;     /**< Connection interval max (units of 1.25ms) */
	uint16_t latency;          /**< Slave latency */
	uint16_t timeout;          /**< Supervision timeout (units of 10ms) */
};

/**
 * @brief BLE advertising parameters
 */
struct halo_ble_adv_params {
	uint16_t interval_min;     /**< Advertising interval min (units of 0.625ms) */
	uint16_t interval_max;     /**< Advertising interval max (units of 0.625ms) */
	uint16_t duration;         /**< Advertising duration (0 = infinite) */
	uint8_t channel_map;       /**< Channel map (0x07 = all channels) */
};

/**
 * @brief Initialize the BLE manager
 * 
 * @param device_name Device name (NULL to use default)
 * @return 0 on success, negative error code on failure
 */
int halo_ble_init(const char *device_name);


/**
 * @brief Deinitialize the BLE manager
 * 
 * @param disable If true, disable BLE hardware
 * @return 0 on success, negative error code on failure
 */
int halo_ble_conn_deinit(bool disable);

/**
 * @brief Start advertising
 * 
 * @param params Advertising parameters (NULL for defaults)
 * @return 0 on success, negative error code on failure
 */
int halo_ble_adv_start(const struct halo_ble_adv_params *params);

/**
 * @brief Stop advertising
 * 
 * @return 0 on success, negative error code on failure
 */
int halo_ble_adv_stop(void);

/**
 * @brief Disconnect current connection
 * 
 * @return 0 on success, negative error code on failure
 */
int halo_ble_disconnect(void);

/**
 * @brief Register a BLE event callback
 * 
 * @param cb Callback structure (must remain valid)
 * @param callback Callback function
 * @param event_mask Bit mask of events (1 << HALO_BLE_EVENT_xxx)
 * @param user_data User data pointer
 * @param name Callback name for debugging
 * @param priority Priority (0-100, higher = called first)
 * @return 0 on success, negative error code on failure
 */
int halo_ble_register_callback(struct halo_ble_callback *cb,
                                halo_ble_event_cb_t callback,
                                uint32_t event_mask,
                                void *user_data,
                                const char *name,
                                int priority);

/**
 * @brief Unregister a BLE event callback
 * 
 * @param cb Callback structure
 * @return 0 on success, negative error code on failure
 */
int halo_ble_unregister_callback(struct halo_ble_callback *cb);

/**
 * @brief Check if device is connected
 * 
 * @return true if connected, false otherwise
 */
bool halo_ble_is_connected(void);

/**
 * @brief Check if device is paired
 *
 * @return true if paired, false otherwise
 */
bool halo_ble_is_paired(void);

/**
 * @brief Check if the current link is encrypted
 *
 * Unlike halo_ble_is_paired(), this only returns true once the controller
 * reports encryption established (auth_info). Use this to gate access to
 * sensitive plaintext data on a link that may still be unencrypted (e.g. a
 * bonded peer reconnecting, where "paired" is set before encryption completes,
 * or a GATT client talking to an untrusted server that does not enforce
 * encryption itself).
 *
 * @return true if the active link is encrypted, false otherwise
 */
bool halo_ble_is_encrypted(void);

/**
 * @brief Check if advertising is active
 * 
 * @return true if advertising, false otherwise
 */
bool halo_ble_is_advertising(void);

/**
 * @brief Get connection index
 * 
 * @return Connection index (GAP_INVALID_CONIDX if not connected)
 */
uint8_t halo_ble_get_conidx(void);

/**
 * @brief Get current MTU size
 * 
 * @return MTU size in bytes (excluding ATT header)
 */
uint16_t halo_ble_get_mtu(void);

/**
 * @brief Get device MAC address
 * 
 * @param addr Buffer to store address (6 bytes)
 * @return 0 on success, negative error code on failure
 */
int halo_ble_get_address(uint8_t addr[6]);

/**
 * @brief Update connection parameters
 * 
 * @param params Connection parameters
 * @return 0 on success, negative error code on failure
 */
int halo_ble_update_conn_params(const struct halo_ble_conn_params *params);

/**
 * @brief Helper macro to create event mask
 * 
 * @param event Event type
 */
#define HALO_BLE_EVENT_MASK(event) (1U << (event))

/**
 * @brief Helper macro to define multiple events
 */
#define HALO_BLE_EVENT_MASK_ALL 0xFFFFFFFF

/**
 * @brief Helper macro to define BLE callback statically
 * 
 * @param _name Variable name
 * @param _cb_func Callback function
 * @param _event_mask Event mask
 * @param _user_data User data pointer
 * @param _cb_name Callback name string
 * @param _prio Priority (0-100)
 */
#define HALO_BLE_CALLBACK_DEFINE(_name, _cb_func, _event_mask, _user_data, _cb_name, _prio) \
	static struct halo_ble_callback _name = { \
		.callback = _cb_func, \
		.event_mask = _event_mask, \
		.user_data = _user_data, \
		.name = _cb_name, \
		.priority = _prio, \
	}

#ifdef __cplusplus
}
#endif

#endif /* HALO_BLE_MANAGER_H */

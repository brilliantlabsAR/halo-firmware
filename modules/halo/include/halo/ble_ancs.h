/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_BLE_ANCS_H
#define HALO_BLE_ANCS_H

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file ble_ancs.h
 * @brief Apple Notification Center Service (ANCS) client
 *
 * GATT client for the ANCS exposed by a connected iOS device:
 *   Service        7905F431-B5CE-4E99-A40F-4B1E122D00D0
 *   Notif. Source  9FBF120D-6301-42D9-8C58-25E699A21DBD (notify)
 *   Control Point  69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9 (write)
 *   Data Source    22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB (notify)
 *
 * The service is discovered on connection and (re)discovered on GATT
 * Service Changed. Subscription requires an encrypted (bonded) link;
 * CCCD writes are retried until encryption completes. All callbacks
 * are invoked from the BLE host thread and must not block.
 */

/* ANCS EventID (Notification Source) */
enum halo_ancs_event_id {
	HALO_ANCS_EVENT_ADDED = 0,
	HALO_ANCS_EVENT_MODIFIED = 1,
	HALO_ANCS_EVENT_REMOVED = 2,
};

/* ANCS EventFlags (Notification Source) */
#define HALO_ANCS_FLAG_SILENT          (1U << 0)
#define HALO_ANCS_FLAG_IMPORTANT       (1U << 1)
#define HALO_ANCS_FLAG_PRE_EXISTING    (1U << 2)
#define HALO_ANCS_FLAG_POSITIVE_ACTION (1U << 3)
#define HALO_ANCS_FLAG_NEGATIVE_ACTION (1U << 4)

/* ANCS CategoryID */
enum halo_ancs_category_id {
	HALO_ANCS_CATEGORY_OTHER = 0,
	HALO_ANCS_CATEGORY_INCOMING_CALL = 1,
	HALO_ANCS_CATEGORY_MISSED_CALL = 2,
	HALO_ANCS_CATEGORY_VOICEMAIL = 3,
	HALO_ANCS_CATEGORY_SOCIAL = 4,
	HALO_ANCS_CATEGORY_SCHEDULE = 5,
	HALO_ANCS_CATEGORY_EMAIL = 6,
	HALO_ANCS_CATEGORY_NEWS = 7,
	HALO_ANCS_CATEGORY_HEALTH_AND_FITNESS = 8,
	HALO_ANCS_CATEGORY_BUSINESS_AND_FINANCE = 9,
	HALO_ANCS_CATEGORY_LOCATION = 10,
	HALO_ANCS_CATEGORY_ENTERTAINMENT = 11,
};

/* ANCS notification attribute IDs (Get Notification Attributes) */
enum halo_ancs_attr_id {
	HALO_ANCS_ATTR_APP_IDENTIFIER = 0,
	HALO_ANCS_ATTR_TITLE = 1,                 /* needs max length */
	HALO_ANCS_ATTR_SUBTITLE = 2,              /* needs max length */
	HALO_ANCS_ATTR_MESSAGE = 3,               /* needs max length */
	HALO_ANCS_ATTR_MESSAGE_SIZE = 4,
	HALO_ANCS_ATTR_DATE = 5,                  /* "yyyyMMdd'T'HHmmSS" */
	HALO_ANCS_ATTR_POSITIVE_ACTION_LABEL = 6,
	HALO_ANCS_ATTR_NEGATIVE_ACTION_LABEL = 7,
	HALO_ANCS_ATTR_COUNT = 8,
};

/* ANCS app attribute IDs (Get App Attributes) */
enum halo_ancs_app_attr_id {
	HALO_ANCS_APP_ATTR_DISPLAY_NAME = 0,
	HALO_ANCS_APP_ATTR_COUNT = 1,
};

/* ANCS notification actions (Perform Notification Action) */
enum halo_ancs_action_id {
	HALO_ANCS_ACTION_POSITIVE = 0,
	HALO_ANCS_ACTION_NEGATIVE = 1,
};

/* ANCS Control Point ATT application error codes */
#define HALO_ANCS_ERR_UNKNOWN_COMMAND   0xA0
#define HALO_ANCS_ERR_INVALID_COMMAND   0xA1
#define HALO_ANCS_ERR_INVALID_PARAMETER 0xA2
#define HALO_ANCS_ERR_ACTION_FAILED     0xA3

/**
 * @brief A Notification Source event (one iOS notification add/change/remove)
 */
struct halo_ancs_notification {
	uint8_t event_id;       /**< see halo_ancs_event_id */
	uint8_t event_flags;    /**< HALO_ANCS_FLAG_* bitmask */
	uint8_t category_id;    /**< see halo_ancs_category_id */
	uint8_t category_count; /**< active notifications in this category */
	uint32_t uid;           /**< notification UID (key for attribute requests) */
};

/**
 * @brief Request descriptor for Get Notification Attributes
 *
 * attr_mask is a bitmask of (1 << HALO_ANCS_ATTR_*). Title, subtitle and
 * message require a maximum length; a zero max length disables the
 * attribute even if set in the mask.
 */
struct halo_ancs_attr_request {
	uint8_t attr_mask;
	uint16_t title_max;
	uint16_t subtitle_max;
	uint16_t message_max;
};

/** One attribute value in a response (data points into an internal buffer,
 *  valid only for the duration of the callback) */
struct halo_ancs_attr {
	uint8_t id;
	uint16_t length;
	const uint8_t *data;
};

/**
 * @brief Result of a Get Notification Attributes request
 *
 * status: 0 on success, HALO_ANCS_ERR_* (positive) on an ANCS control
 * point error, or a negative errno (-ETIMEDOUT, -ENOTCONN, -EMSGSIZE).
 * Attributes the phone reports as unavailable have length 0.
 */
struct halo_ancs_attr_result {
	uint32_t uid;
	int status;
	uint8_t attr_count;
	struct halo_ancs_attr attrs[HALO_ANCS_ATTR_COUNT];
};

/**
 * @brief Result of a Get App Attributes request
 *
 * app_id is NUL-terminated; pointers are valid only during the callback.
 */
struct halo_ancs_app_attr_result {
	const char *app_id;
	int status;
	uint8_t attr_count;
	struct halo_ancs_attr attrs[HALO_ANCS_APP_ATTR_COUNT];
};

/**
 * @brief Result of a Perform Notification Action request
 *
 * status: 0 on success, HALO_ANCS_ERR_* or negative errno on failure.
 */
struct halo_ancs_action_result {
	uint32_t uid;
	uint8_t action_id;
	int status;
};

/** Callbacks - all invoked from the BLE host thread; must not block. */
struct halo_ancs_callbacks {
	/** ANCS became available (discovered) / unavailable on the peer */
	void (*availability)(bool available, void *user_data);
	/** A Notification Source event arrived */
	void (*notification)(const struct halo_ancs_notification *notif, void *user_data);
	/** Get Notification Attributes completed (or failed) */
	void (*attributes)(const struct halo_ancs_attr_result *result, void *user_data);
	/** Get App Attributes completed (or failed) */
	void (*app_attributes)(const struct halo_ancs_app_attr_result *result, void *user_data);
	/** Perform Notification Action completed (or failed) */
	void (*action)(const struct halo_ancs_action_result *result, void *user_data);
	void *user_data;
};

/**
 * @brief Initialize the ANCS client
 *
 * Call after the BLE manager core is initialized (halo_ble_init()).
 *
 * @param reset true on first initialization after BLE stack bring-up
 *              (registers the GATT client user), false on re-init
 * @return 0 on success, negative error code on failure
 */
int halo_ble_ancs_init(bool reset);

/**
 * @brief Deinitialize the ANCS client
 *
 * @return 0 on success, negative error code on failure
 */
int halo_ble_ancs_deinit(void);

/**
 * @brief Register (or clear, with NULL) the callback set
 *
 * @param cbs Callback set (copied), or NULL to clear
 * @return 0 on success, negative error code on failure
 */
int halo_ble_ancs_set_callbacks(const struct halo_ancs_callbacks *cbs);

/**
 * @brief Enable or disable ANCS notification delivery
 *
 * When enabled, the client subscribes to the Notification Source and Data
 * Source characteristics as soon as the service is discovered and the link
 * is encrypted (retrying until encryption completes). iOS then replays all
 * existing notifications with the PreExisting flag set. When disabled the
 * subscriptions are removed.
 *
 * @param enable true to subscribe, false to unsubscribe
 * @return 0 on success, negative error code on failure
 */
int halo_ble_ancs_set_enabled(bool enable);

/**
 * @brief Check whether ANCS was discovered on the connected peer
 */
bool halo_ble_ancs_is_available(void);

/**
 * @brief Check whether Notification Source events are currently subscribed
 */
bool halo_ble_ancs_is_subscribed(void);

/**
 * @brief Request attributes of a notification (async)
 *
 * The result is delivered through the attributes callback. Only one
 * control point command may be in flight at a time.
 *
 * @param uid Notification UID from a Notification Source event
 * @param req Attributes to request
 * @return 0 on success, -EBUSY if a command is in flight, -ENOTCONN if
 *         ANCS is not available/subscribable, other negative errno on error
 */
int halo_ble_ancs_get_notification_attrs(uint32_t uid,
					 const struct halo_ancs_attr_request *req);

/**
 * @brief Request attributes of an app (async)
 *
 * The result is delivered through the app_attributes callback.
 *
 * @param app_id App bundle identifier (NUL-terminated,
 *               e.g. "com.apple.MobileSMS")
 * @return 0 on success, -EBUSY/-ENOTCONN/-EINVAL on failure
 */
int halo_ble_ancs_get_app_attrs(const char *app_id);

/**
 * @brief Perform a positive/negative action on a notification (async)
 *
 * The result is delivered through the action callback.
 *
 * @param uid Notification UID
 * @param action_id HALO_ANCS_ACTION_POSITIVE or HALO_ANCS_ACTION_NEGATIVE
 * @return 0 on success, -EBUSY/-ENOTCONN/-EINVAL on failure
 */
int halo_ble_ancs_perform_action(uint32_t uid, uint8_t action_id);

#ifdef __cplusplus
}
#endif

#endif /* HALO_BLE_ANCS_H */

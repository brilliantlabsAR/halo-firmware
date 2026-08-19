/**
 * @brief Bluetooth Hello service
 *
 * Copyright Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef BT_SRV_HELLO_H_
#define BT_SRV_HELLO_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hello service UUIDs */
#define HELLO_UUID_128_SVC                                                                         \
	{                                                                                          \
		0x12, 0x34, 0x56, 0x78, 0x90, 0x12, 0x23, 0x34, 0x45, 0x56, 0x67, 0x78, 0x89,      \
			0x90, 0x00, 0x00                                                           \
	}

#define HELLO_UUID_128_CHAR0                                                                       \
	{                                                                                          \
		0x12, 0x34, 0x56, 0x78, 0x90, 0x12, 0x23, 0x34, 0x45, 0x56, 0x67, 0x78, 0x89,      \
			0x15, 0x00, 0x00                                                           \
	}

#define HELLO_UUID_128_CHAR1                                                                       \
	{                                                                                          \
		0x12, 0x34, 0x56, 0x78, 0x90, 0x12, 0x23, 0x34, 0x45, 0x56, 0x67, 0x78, 0x89,      \
			0x16, 0x00, 0x00                                                           \
	}

/* Hello service attribute indexes */
enum hello_attr_idx {
	HELLO_IDX_SERVICE,
	HELLO_IDX_CHAR0_CHAR,
	HELLO_IDX_CHAR0_VAL,
	HELLO_IDX_CHAR0_NTF_CFG,
	HELLO_IDX_CHAR1_CHAR,
	HELLO_IDX_CHAR1_VAL,
	HELLO_IDX_NB,
};

/* Hello service meta information */
#define HELLO_METAINFO_CHAR0_NTF_SEND 0x4321

/**
 * @brief Initialize the hello service
 *
 * @return 0 on success, negative error code otherwise
 */
int bt_srv_hello_init(void);

/**
 * @brief Send notification for characteristic 0
 *
 * @param conn_idx Connection index
 * @param data Data to send
 * @param len Length of data
 * @return 0 on success, negative error code otherwise
 */
int bt_srv_hello_notify(uint8_t conn_idx, const void *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* BT_SRV_HELLO_H_ */

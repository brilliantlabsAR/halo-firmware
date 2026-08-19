/* Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

/**
 *    @file
 *          Example project configuration file for CHIP.
 *
 *          This is a place to put application or project-specific overrides
 *          to the default configuration values for general CHIP features.
 *
 */

#pragma once

//  Switching from Thread child to router may cause a few second packet stall.
//  Until this is improved in OpenThread we need to increase the retransmission
//  interval to survive the stall.

#define CHIP_CONFIG_MRP_LOCAL_ACTIVE_RETRY_INTERVAL (15000_ms32)

#define CHIP_CONFIG_MRP_LOCAL_IDLE_RETRY_INTERVAL (15000_ms32)

#define CHIP_CONFIG_RMP_DEFAULT_ACK_TIMEOUT (20000_ms32)


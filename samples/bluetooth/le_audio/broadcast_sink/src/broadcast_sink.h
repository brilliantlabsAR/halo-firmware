/**
 * Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef _BROADCAST_SINK_H
#define _BROADCAST_SINK_H

/**
 * @brief Start the LE audio broadcast sink
 *
 * @retval 0 on success
 * @retval Negative error code on failure
 */
int broadcast_sink_start(void);

#endif /* _BROADCAST_SINK_H */

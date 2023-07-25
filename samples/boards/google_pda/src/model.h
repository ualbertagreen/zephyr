/*
 * Copyright (c) 2022 The Chromium OS Authors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _MODEL_H_
#define _MODEL_H_

#include <zephyr/kernel.h>
#include "mask.h"

/**
 * @brief Initializes the snooper model
 *
 * @param dev pointer to the Twinkie device
 *
 * @return 0 on success
 */
int model_init(const struct device *dev);

/**
 * @brief Resets the snooper model
 */
void reset_snooper();

/**
 * @brief Sets the role as source or sink, and set the pull up resistor and active cc line if source
 *
 * @param role_mask a bit mask of the role to be set
 */
void set_role(snooper_mask_t role_mask);

/**
 * @brief Starts or stops the snooper by setting the snoop status
 *
 * @param s true for start, false for stop
 */
void start_snooper(bool s);

/**
 * @brief Sets whether the Twinkie will continuously output data when no pd messages are received
 *
 * @param e true for continuous output, false for output on pd messages only
 */
void set_empty_print(bool e);

/**
 * @brief Sets how fast the twinkie will output data when no pd messages are received
 *
 * @param s true for slow output, false for fast output
 */
void set_sleep_time(uint32_t st);

/**
 * @brief Sets whether twinkie automatically turns off when no receiver is connected.
 *
 * @param s true for auto stop, false for continuous output
 */
void set_auto_stop(bool s);

/**
 * @brief Used only for sharing isr
 */
void ucpd_isr(void);

#endif

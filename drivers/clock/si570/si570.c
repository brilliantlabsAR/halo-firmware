/* Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/clock_control.h>
#include "si570.h"

LOG_MODULE_REGISTER(si570, CONFIG_SI570_LOG_LEVEL);

#define DT_DRV_COMPAT skyworks_si570

struct si570_data {
	uint32_t hs_div;
	uint32_t n1;
	uint64_t reference_freq;
	uint64_t fxtal;
	uint64_t current_freq;
};

struct si570_config {
	const struct i2c_dt_spec i2c;
	const uint64_t factory_fout;
	const uint64_t initial_frequency;
};

static int si570_on(const struct device *dev, clock_control_subsys_t subsys)
{
	(void)subsys;
	(void)dev;

	/* SI570 device is always on, no action required */
	return 0;
}

static int si570_off(const struct device *dev, clock_control_subsys_t subsys)
{
	(void)subsys;
	(void)dev;

	/* SI570 device cannot be turned off via the I2C interface */
	return -ENOTSUP;
}

static int si570_get_rate(const struct device *dev, clock_control_subsys_t subsys, uint32_t *rate)
{
	(void)subsys;

	if ((dev == NULL) || (rate == NULL)) {
		return -EINVAL;
	}

	struct si570_data *dev_data = dev->data;

	/* Frequency greater than UINT32_MAX can be set, but API does not support returning a value
	 * this large.
	 */
	if (dev_data->current_freq > UINT32_MAX) {
		LOG_WRN("SI570 frequency cannot be returned correctly, greater than UINT32_MAX");
	}

	*rate = (uint32_t)dev_data->current_freq;

	return 0;
}

static bool can_update_without_interruption(struct si570_data *dev_data, uint64_t new_freq)
{
	/* The frequency can be updated without interruption of the clock if:
	 * - The frequency change is less than 3500 ppm, AND
	 * - Only RFREQ needs to be updated, HS_DIV and N1 remain the same.
	 */

	uint64_t fdiff = (new_freq > dev_data->current_freq) ? new_freq - dev_data->current_freq
							     : dev_data->current_freq - new_freq;

	if (((fdiff * 1000000ULL) / dev_data->current_freq) >=
	    SI570_MAX_CHANGE_WITHOUT_INTERRUPTION_PPM) {
		LOG_DBG("Frequency cannot be changed without interruption because ppm change is "
			"too high");
		return false;
	}

	uint64_t fdco = new_freq * dev_data->hs_div * dev_data->n1;

	if ((fdco >= SI570_FDCO_MIN) && (fdco <= SI570_FDCO_MAX)) {
		LOG_DBG("Frequency can be changed without interruption");
		return true;
	}

	LOG_DBG("Frequency cannot be changed without interruption because FDCO would be out of "
		"range");
	return false;
}

static int write_rfreq_to_device(const struct device *dev)
{
	struct si570_data *dev_data = dev->data;
	const struct si570_config *dev_cfg = dev->config;

	/* Freeze M so that RFREQ can be updated atomically */
	int ret = i2c_reg_write_byte_dt(&dev_cfg->i2c, SI570_REG_RESET_FREEZE_CONTROL,
					SI570_BIT_FREEZE_M);

	if (ret) {
		return ret;
	}

	k_busy_wait(SI570_TRANSACTION_GAP_US);

	/* Write new RFREQ value (part of N1 is included in same register writes, but this value
	 * does not change).
	 */
	uint8_t n1_reg_val = (dev_data->n1 - SI570_OFFSET_N1) & 0xFF;
	uint8_t buf[6];

	buf[0] = SI570_REG_N1_REFERENCE_FREQUENCY_0;
	buf[1] = ((n1_reg_val << SI570_RSHIFT_N1_1_0) & SI570_MASK_N1_1_0) |
		 ((dev_data->reference_freq >> SI570_LSHIFT_RFREQ_37_32) & SI570_MASK_RFREQ_37_32);
	buf[2] = (dev_data->reference_freq >> SI570_LSHIFT_RFREQ_31_24) & 0xFF;
	buf[3] = (dev_data->reference_freq >> SI570_LSHIFT_RFREQ_23_16) & 0xFF;
	buf[4] = (dev_data->reference_freq >> SI570_LSHIFT_RFREQ_15_8) & 0xFF;
	buf[5] = dev_data->reference_freq & 0xFF;

	ret = i2c_write_dt(&dev_cfg->i2c, buf, ARRAY_SIZE(buf));
	if (ret) {
		return ret;
	}

	k_busy_wait(SI570_TRANSACTION_GAP_US);

	/* Unfreeze M to apply the new RFREQ value */
	return i2c_reg_write_byte_dt(&dev_cfg->i2c, SI570_REG_RESET_FREEZE_CONTROL, 0);
}

static uint64_t calculate_reference_frequency(uint64_t new_freq, uint64_t hs_div, uint64_t n1,
					      uint64_t fxtal)
{
	uint64_t fdco = new_freq * hs_div * n1;

	return (fdco << 28) / fxtal;
}

static int si570_small_frequency_change(const struct device *dev, uint64_t new_freq)
{
	struct si570_data *dev_data = dev->data;

	dev_data->reference_freq = calculate_reference_frequency(new_freq, dev_data->hs_div,
								 dev_data->n1, dev_data->fxtal);

	int ret = write_rfreq_to_device(dev);

	if (ret) {
		LOG_ERR("Failed to update SI570 registers to frequency %llu", new_freq);
		return ret;
	}

	/* Wait for "Settling Time for Small Frequency Change" specified in datasheet */
	k_sleep(K_USEC(SI570_SMALL_FREQUENCY_SETTLING_TIME_USEC));

	dev_data->current_freq = new_freq;
	return 0;
}

static int optimise_hs_div_n1(struct si570_data *dev_data, uint64_t new_freq)
{
	const uint8_t allowed_hs_div_vals[] = {4, 5, 6, 7, 9, 11};

	/* Optimise for the values of HS_DIV and N1 which get FDCO closest to the midpoint.
	 * Optimising instead for the lowest FDCO would result in lower power consumption, but a
	 * frequency closer to the midpoint has the advantage that subsequent frequency updates are
	 * less likely to require changes to HS_DIV and N1, which would interrupt the output clock
	 * signal.
	 */
	uint8_t best_hs_div = UINT8_MAX;
	uint8_t best_n1 = UINT8_MAX;
	uint64_t min_fdiff_from_centre = UINT64_MAX;

	for (uint32_t i = 0; i < ARRAY_SIZE(allowed_hs_div_vals); i++) {
		uint8_t hs_div = allowed_hs_div_vals[i];

		for (uint8_t n1 = 1; n1 <= 128; (n1 == 1) ? (n1 = 2) : (n1 += 2)) {
			uint64_t fdco = new_freq * (uint64_t)hs_div * (uint64_t)n1;

			if (fdco > SI570_FDCO_MAX) {
				break;
			}

			if (fdco < SI570_FDCO_MIN) {
				continue;
			}

			uint64_t fdiff = (fdco > SI570_FDCO_MID) ? (fdco - SI570_FDCO_MID)
								 : (SI570_FDCO_MID - fdco);

			if (fdiff < min_fdiff_from_centre) {
				min_fdiff_from_centre = fdiff;
				best_hs_div = hs_div;
				best_n1 = n1;
			}
		}
	}

	if (best_hs_div == UINT8_MAX || best_n1 == UINT8_MAX) {
		return -ENOTSUP;
	}

	dev_data->hs_div = best_hs_div;
	dev_data->n1 = best_n1;
	return 0;
}

static int write_all_dividers_to_device(const struct device *dev)
{
	int ret;
	struct si570_data *dev_data = dev->data;
	const struct si570_config *dev_cfg = dev->config;

	/* Freeze the DCO so that update of all values is atomic */
	ret = i2c_reg_write_byte_dt(&dev_cfg->i2c, SI570_REG_FREEZE_DCO, SI570_BIT_FREEZE_DCO);
	if (ret) {
		return ret;
	}

	k_busy_wait(SI570_TRANSACTION_GAP_US);

	uint16_t hs_div_reg_val = (dev_data->hs_div - SI570_OFFSET_HS_DIV) & 0xFF;
	uint16_t n1_reg_val = (dev_data->n1 - SI570_OFFSET_N1) & 0xFF;
	uint8_t buf[7];

	buf[0] = SI570_REG_HIGH_SPEED_N1_DIVS;
	buf[1] = ((hs_div_reg_val << SI570_RSHIFT_HS_DIV) & SI570_MASK_HS_DIV) |
		 ((n1_reg_val >> SI570_LSHIFT_N1_6_2) & SI570_MASK_N1_6_2);
	buf[2] = ((n1_reg_val << SI570_RSHIFT_N1_1_0) & SI570_MASK_N1_1_0) |
		 ((dev_data->reference_freq >> SI570_LSHIFT_RFREQ_37_32) & SI570_MASK_RFREQ_37_32);
	buf[3] = (dev_data->reference_freq >> SI570_LSHIFT_RFREQ_31_24) & 0xFF;
	buf[4] = (dev_data->reference_freq >> SI570_LSHIFT_RFREQ_23_16) & 0xFF;
	buf[5] = (dev_data->reference_freq >> SI570_LSHIFT_RFREQ_15_8) & 0xFF;
	buf[6] = dev_data->reference_freq & 0xFF;

	ret = i2c_write_dt(&dev_cfg->i2c, buf, ARRAY_SIZE(buf));
	if (ret) {
		return ret;
	}

	k_busy_wait(SI570_TRANSACTION_GAP_US);

	/* Unfreeze the DCO */
	ret = i2c_reg_write_byte_dt(&dev_cfg->i2c, SI570_REG_FREEZE_DCO, 0);
	if (ret) {
		return ret;
	}

	k_busy_wait(SI570_TRANSACTION_GAP_US);

	/* Apply the new frequency */
	return i2c_reg_write_byte_dt(&dev_cfg->i2c, SI570_REG_RESET_FREEZE_CONTROL,
				     SI570_BIT_NEWFREQ);
}

static int si570_large_frequency_change(const struct device *dev, uint64_t new_freq)
{
	struct si570_data *dev_data = dev->data;

	int ret = optimise_hs_div_n1(dev_data, new_freq);

	if (ret) {
		LOG_ERR("Cannot find HS_DIV and N1 to achieve requested frequency");
		return ret;
	}

	dev_data->reference_freq = calculate_reference_frequency(new_freq, dev_data->hs_div,
								 dev_data->n1, dev_data->fxtal);

	/* Update the register values */
	ret = write_all_dividers_to_device(dev);

	if (ret) {
		LOG_ERR("Failed to update SI570 registers to frequency %llu", new_freq);
		return ret;
	}

	/* Wait for "Settling Time for Large Frequency Change" value specified in datasheet */
	k_sleep(K_USEC(SI570_LARGE_FREQUENCY_SETTLING_TIME_USEC));

	dev_data->current_freq = new_freq;

	return 0;
}

static int si570_set_rate(const struct device *dev, clock_control_subsys_t subsys,
			  clock_control_subsys_rate_t rate)
{
	(void)subsys;

	if (dev == NULL) {
		return -EINVAL;
	}

	struct si570_data *dev_data = dev->data;
	uint64_t desired_freq = *(uint64_t *)rate;

	if (desired_freq < SI570_FREQUENCY_MIN || desired_freq > SI570_FREQUENCY_MAX) {
		return -ENOTSUP;
	}

	if (can_update_without_interruption(dev_data, desired_freq)) {
		return si570_small_frequency_change(dev, desired_freq);
	}

	return si570_large_frequency_change(dev, desired_freq);
}

static int si570_read_data(const struct device *dev)
{
	const struct si570_config *dev_cfg = dev->config;
	struct si570_data *dev_data = dev->data;

	uint8_t regs[6];

	int ret =
		i2c_burst_read_dt(&dev_cfg->i2c, SI570_REG_HIGH_SPEED_N1_DIVS, regs, sizeof(regs));

	if (ret) {
		LOG_DBG("Failed to burst read from SI570 reg %u", SI570_REG_HIGH_SPEED_N1_DIVS);
		return ret;
	}

	dev_data->hs_div =
		((regs[0] & SI570_MASK_HS_DIV) >> SI570_RSHIFT_HS_DIV) + SI570_OFFSET_HS_DIV;
	dev_data->n1 = (((regs[0] & SI570_MASK_N1_6_2) << SI570_LSHIFT_N1_6_2) |
			((regs[1] & SI570_MASK_N1_1_0) >> SI570_RSHIFT_N1_1_0)) +
		       SI570_OFFSET_N1;
	dev_data->reference_freq =
		((uint64_t)(regs[1] & SI570_MASK_RFREQ_37_32) << SI570_LSHIFT_RFREQ_37_32) |
		((uint64_t)regs[2] << SI570_LSHIFT_RFREQ_31_24) |
		((uint64_t)regs[3] << SI570_LSHIFT_RFREQ_23_16) |
		((uint64_t)regs[4] << SI570_LSHIFT_RFREQ_15_8) | (uint64_t)regs[5];

	LOG_DBG("HS_DIV: %u", dev_data->hs_div);
	LOG_DBG("N1: %u", dev_data->n1);
	LOG_DBG("REF FREQ: %llu", dev_data->reference_freq);

	return 0;
}

static int si570_init(const struct device *dev)
{
	if (dev == NULL) {
		return -EINVAL;
	}

	const struct si570_config *dev_cfg = dev->config;
	struct si570_data *dev_data = dev->data;

	/* Write to the RECALL bit, which loads NVM contents into RAM, and is the recommended
	 * approach in the datasheet to start from initial conditions.
	 */
	uint8_t reg_val = SI570_BIT_RECALL;

	int ret = i2c_reg_write_byte_dt(&dev_cfg->i2c, SI570_REG_RESET_FREEZE_CONTROL, reg_val);

	if (ret) {
		LOG_DBG("Failed to write to SI570 reg %u", SI570_REG_RESET_FREEZE_CONTROL);
		return ret;
	}

	/* The datasheet doesn't explicitly define any delay required after a recall operation, but
	 * it has been observed that incorrect values can be read back if this delay is not
	 * inserted. It's possibly related to the "Power up time (tOSC)" specified in the datasheet,
	 * which is 10 ms.
	 */
	k_sleep(K_USEC(SI570_POWER_UP_TIME_USEC));

	/* After RECALL operation, the current frequency of the device is equal to the factory fout
	 */
	dev_data->current_freq = dev_cfg->factory_fout;

	/* Read the register values back from the device */
	ret = si570_read_data(dev);
	if (ret) {
		return ret;
	}

	/* Calculate the FXTAL of the device. This is unique to each device, the values of N1,
	 * HS_DIV and RFREQ are factory tuned to give the desired factory_fout.
	 */
	uint64_t fdco = dev_cfg->factory_fout * dev_data->hs_div * dev_data->n1;

	/* RFREQ is a 38 bit fractional value, where the 10 MSBs represent the integer portion and
	 * the 28 LSBs represent the fractional portion. To avoid floating point calculations, we
	 * can left shift FDCO by 28 bits before performing the division. The maximum valid value of
	 * FDCO is 5.67 GHz, which left-shifted by 28 bits will still fit in uint64_t.
	 */
	dev_data->fxtal = (fdco << 28) / dev_data->reference_freq;

	LOG_DBG("FXTAL: %llu", dev_data->fxtal);

	if (dev_cfg->initial_frequency == 0) {
		/* No initial frequency to set, init is complete */
		return 0;
	}

	uint64_t initial_rate = dev_cfg->initial_frequency;

	return si570_set_rate(dev, NULL, &initial_rate);
}

static const struct clock_control_driver_api si570_driver_api = {
	.on = si570_on, .off = si570_off, .set_rate = si570_set_rate, .get_rate = si570_get_rate};

#define SI570_DEFINE(inst)                                                                         \
	static struct si570_data si570_data_##inst;                                                \
	static const struct si570_config si570_config_##inst = {                                   \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.factory_fout = DT_INST_PROP(inst, factory_fout),                                  \
		.initial_frequency = DT_INST_PROP_OR(inst, initial_freq, 0)};                      \
	DEVICE_DT_INST_DEFINE(inst, si570_init, NULL, &si570_data_##inst, &si570_config_##inst,    \
			      POST_KERNEL, CONFIG_SI570_INIT_PRIORITY, &si570_driver_api);

DT_INST_FOREACH_STATUS_OKAY(SI570_DEFINE)

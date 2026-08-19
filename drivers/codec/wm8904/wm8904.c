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
#include <zephyr/drivers/i2s.h>
#include "wm8904.h"

LOG_MODULE_REGISTER(wm8904, CONFIG_WM8904_LOG_LEVEL);

#define DT_DRV_COMPAT cirrus_wm8904

struct wm8904_data {
	/* Cached configuration settings */
	uint16_t dac_digital_settings;
	uint16_t aif_format; /* Audio interface format */
	uint16_t clock_rate; /* Clock rate configuration */
	bool is_mono;
	uint32_t sample_rate;
	bool config_cached;
	/* FLL settings */
	uint8_t fll_fratio;
	uint8_t fll_outdiv;
	uint16_t fll_k;
	uint8_t fll_n;
	/* Volume settings */
	uint8_t hp_volume_left;
	uint8_t hp_volume_right;
};

/* Error handling is done directly in the functions */

/**
 * @brief Helper function to write a 16-bit register in an I2C device.
 */
static int cwm_i2c_wr(const struct i2c_dt_spec *spec, uint8_t reg_addr, uint16_t data)
{
	uint8_t buf[3] = {reg_addr, data >> 8, data & 0xFF};

	return i2c_write_dt(spec, buf, sizeof(buf));
}

/**
 * @brief Helper function to read a 16-bit register in an I2C device.
 */
static int cwm_i2c_rd(const struct i2c_dt_spec *spec, uint8_t reg_addr, uint16_t *data)
{
	uint8_t buf[2];

	int ret = i2c_write_read_dt(spec, &reg_addr, sizeof(reg_addr), buf, sizeof(buf));

	if (ret) {
		return ret;
	}

	*data = ((uint16_t)buf[0] << 8) | buf[1];

	return 0;
}



/* Configure audio interface format */
static int cwm_configure_audio_interface(const struct device *dev, struct audio_codec_cfg *cfg)
{
	struct wm8904_data *data = dev->data;

	/* Configure I2S format */
	uint16_t aif_format = AUD_INT1_AIF_FMT_I2S;

	/* Set word length based on I2S config */
	switch (cfg->dai_cfg.i2s.word_size) {
	case AUDIO_PCM_WIDTH_16_BITS:
		aif_format |= AUD_INT1_AIF_WL_16BIT;
		break;
	case AUDIO_PCM_WIDTH_20_BITS:
		aif_format |= AUD_INT1_AIF_WL_20BIT;
		break;
	case AUDIO_PCM_WIDTH_24_BITS:
		aif_format |= AUD_INT1_AIF_WL_24BIT;
		break;
	case AUDIO_PCM_WIDTH_32_BITS:
		aif_format |= AUD_INT1_AIF_WL_32BIT;
		break;
	default:
		return -EINVAL;
	}

	/* Store configuration in driver data */
	data->aif_format = aif_format;

	return 0;
}

/* Configure clock rate */
static int cwm_configure_clock_rate(const struct device *dev, uint32_t rate)
{
	struct wm8904_data *data = dev->data;
	uint16_t clock_rate = 0;

	/* Set clock_rate for sample rates (16k, 24k, 32k, 44.1k, 48k).
	 * WM8904: SYSCLK/fs ratio = 256, set divider accordingly.
	 */
	switch (rate) {
	case 16000:
	case 24000:
		clock_rate = CLK_RTE0_SYSCLK_SRC_MCLK | CLK_RTE0_MCLK_DIV_6;
		break;
	case 32000:
	case 48000:
		clock_rate = CLK_RTE0_SYSCLK_SRC_MCLK | CLK_RTE0_MCLK_DIV_3;
		break;
	case 44100:
		clock_rate = CLK_RTE0_SYSCLK_SRC_MCLK | CLK_RTE0_MCLK_DIV_3;
		break;
	case 88200:
	case 96000:
		clock_rate = CLK_RTE0_SYSCLK_SRC_MCLK | CLK_RTE0_MCLK_DIV_1_5;
		break;
	default:
		LOG_ERR("Unsupported sample rate for: %u", rate);
		return -EINVAL;
	}


	data->clock_rate = clock_rate;

	return 0;
}

/* Configure the codec according to the provided configuration */
static int cwm_configure(const struct device *dev, struct audio_codec_cfg *cfg)
{
	struct wm8904_data *data = dev->data;
	int ret;
	uint32_t sample_rate;
	uint16_t dac_digital_settings;

	/* Default DAC settings, including de-emphasis level */
	dac_digital_settings = DAC_DG1_OSR128 | DAC_DG1_UNMUTE_RAMP | DAC_DG1_MUTERATE;

	/* Configure audio interface format */
	ret = cwm_configure_audio_interface(dev, cfg);
	if (ret) {
		LOG_ERR("Failed to configure audio interface format: %d", ret);
		return ret;
	}

	/* Get the sample rate from the config */
	sample_rate = cfg->dai_cfg.i2s.frame_clk_freq;

	/* Configure sample rate */
	ret = cwm_configure_clock_rate(dev, sample_rate);
	if (ret) {
		LOG_ERR("Failed to configure sample rate %u: %d", sample_rate, ret);
		return ret;
	}

	/* Set de-emphasis based on sample rate using switch-case for clarity. */
	switch (sample_rate) {
	case 16000:
	case 24000:
		dac_digital_settings |= DAC_DG1_DEEMPH(0); /* b00 = no de-emphasis, 16- or 24kHz */
		break;
	case 32000:
		dac_digital_settings |= DAC_DG1_DEEMPH(1); /* b01 = 32kHz */
		break;
	case 44100:
		dac_digital_settings |= DAC_DG1_DEEMPH(2); /* b10 = 44.1kHz */
		break;
	case 48000:
		dac_digital_settings |= DAC_DG1_DEEMPH(3); /* b11 = 48kHz */
		break;
	default:
		LOG_ERR("Unsupported sample rate: %u", sample_rate);
		return -EINVAL;
	}

	/* Store configuration in driver data */
	data->dac_digital_settings = dac_digital_settings;
	data->is_mono = (cfg->dai_cfg.i2s.channels == 1);
	data->sample_rate = sample_rate;

	/* Compute FLL and divider settings dynamically. */
	/* WM8904 FLL: Fout = (Fref * (N + K/65536) * FRATIO) / (OUTDIV * 2)
	 * For simplicity, assume MCLK (Fref) = 12.288 MHz (standard audio clock),
	 * OUTDIV=8, FRATIO=8, K=0 for integer N.
	 * N = (Fout * OUTDIV * 2) / (Fref * FRATIO)
	 * For 16k, 24k, 32k, 44.1k, 48k sample rates, Fout = 256 * fs (SYSCLK = 256*fs).
	 */
	uint32_t sysclk = 256 * sample_rate;
	uint32_t fref = 12288000; /* MCLK (Hz) */
	uint8_t outdiv = 8, fratio = 8;
	uint32_t n;
	uint16_t k = 0;

	n = (sysclk * outdiv * 2) / (fref * fratio);
	/* K is zero for integer N; can extend for non-integer N if needed */

	data->fll_fratio = fratio;
	data->fll_outdiv = outdiv;
	data->fll_k = k;
	data->fll_n = n;

	data->config_cached = true;

	LOG_DBG("Configuration stored, will be applied on next start");

	return 0;
}

/* Start the codec output */
static void cwm_start_output(const struct device *dev)
{
	const struct wm8904_driver_config *dev_cfg = dev->config;
	const struct i2c_dt_spec *i2c = &dev_cfg->i2c;
	struct wm8904_data *data = dev->data;
	int ret;

	/* Program sample rate register(s) based on configuration */
	ret = cwm_i2c_wr(i2c, WM8904_CLOCK_RATES_1, data->clock_rate);
	if (ret) {
		LOG_ERR("Failed to set sample rate (clock rates 1): %d", ret);
		return;
	}

	/* Set high performance bias and disable bias current generator */
	ret = cwm_i2c_wr(i2c, WM8904_BIAS_CONTROL_0, BIAS_CNTL_ISEL_HP_BIAS);
	if (ret) {
		LOG_ERR("Failed to set bias control: %d", ret);
		return;
	}

	/* Enable VMID buffer to unused outputs, vmid reference voltage with fast startup */
	ret = cwm_i2c_wr(i2c, WM8904_VMID_CONTROL_0,
		VMID_CNTL0_VMID_BUF_ENA | VMID_CNTL0_VMID_RES_FAST | VMID_CNTL0_VMID_ENA);
	if (ret) {
		LOG_ERR("Failed to enable VMID buffer: %d", ret);
		return;
	}
	k_msleep(100); /* Delay for VMID startup */

	/* VMID reference voltage setup with normal operation */
	ret = cwm_i2c_wr(i2c, WM8904_VMID_CONTROL_0,
		VMID_CNTL0_VMID_BUF_ENA | VMID_CNTL0_VMID_RES_NORMAL | VMID_CNTL0_VMID_ENA);
	if (ret) {
		LOG_ERR("Failed to set VMID reference voltage: %d", ret);
		return;
	}

	/* Enable bias current generator */
	ret = cwm_i2c_wr(i2c, WM8904_BIAS_CONTROL_0,
		BIAS_CNTL_ISEL_HP_BIAS | BIAS_CNTL_BIAS_ENA);
	if (ret) {
		LOG_ERR("Failed to enable bias current generator: %d", ret);
		return;
	}

	/* Enable ADC left and right input programmable gain amplifiers */
	ret = cwm_i2c_wr(i2c, WM8904_POWER_MANAGEMENT_0,
		PWR_MGMT0_INL_ENA | PWR_MGMT0_INR_ENA);
	if (ret) {
		LOG_ERR("Failed to enable ADC input gain amplifiers: %d", ret);
		return;
	}

	/* Enable left and right headphone output */
	ret = cwm_i2c_wr(i2c, WM8904_POWER_MANAGEMENT_2,
		PWR_MGMT2_HPL_PGA_ENA | PWR_MGMT2_HPR_PGA_ENA);
	if (ret) {
		LOG_ERR("Failed to enable headphone output: %d", ret);
		return;
	}

	/* Configure DAC digital settings based on cached parameters */
	ret = cwm_i2c_wr(i2c, WM8904_DAC_DIGITAL_1, data->dac_digital_settings);
	if (ret) {
		LOG_ERR("Failed to configure DAC digital settings: %d", ret);
		return;
	}

	/* Configure output routing. Input select for left/right headphone and left/right line
	 * output mux. No bypass used.
	 */
	ret = cwm_i2c_wr(i2c, WM8904_ANALOGUE_OUT12_ZC, 0x0000);
	if (ret) {
		LOG_ERR("Failed to configure output routing: %d", ret);
		return;
	}

	/* Enable charge pump digits. Adjusts output voltage to optimize power consumption */
	ret = cwm_i2c_wr(i2c, WM8904_CHARGE_PUMP_0, CHRG_PMP_CP_ENA);
	if (ret) {
		LOG_ERR("Failed to enable charge pump: %d", ret);
		return;
	}

	/* Enable dynamic charge pump power based on real time audio level */
	ret = cwm_i2c_wr(i2c, WM8904_CHARGE_PUMP_0, CLS_W0_CP_DYN_PWR);
	if (ret) {
		LOG_ERR("Failed to enable dynamic charge pump power: %d", ret);
		return;
	}

	/******************************************************************************************/
	/* Configure FLL to provide system clock.
	 * System clock requirements, assuming stereo DAC/ADC operation:
	 * - Must be >= 3 MHz
	 * - Must be >=256 * Fs
	 *
	 * For a frequency of 48 kHz, the system clock must be >= 12.288 MHz.
	 *
	 * The system clock must also be an integer multiple of the sample rate.
	 *
	 * In this case we can choose exactly 12.288 MHz
	 *
	 * With 2-channel 16-bit audio at 48 kHz, MCLK is set to 1.536 MHz which is too slow to be
	 * used directly, so the FLL (frequency locked loop) must be used as the system clock
	 * source.
	 *
	 * To set up the FLL, we have the following equations:
	 *
	 * (1): Fout = Fvco / FLL_OUTDIV
	 * (2): Fvco = Fref * N.K * FLL_FRATIO
	 *
	 * Fvco must be in the range 90 - 100 MHz
	 * N.K must be a fractional (not integer) value for the best performance
	 *
	 * Since we have Fref = 1.536 MHz and Fout = 12.288 MHz = 8 * Fref, we can re-arrange the
	 * equations to find N.K in terms of the other settable parameters:
	 *
	 *          Fout = (Fref * N.K * FLL_FRATIO) / FLL_OUTDIV
	 *      8 * Fref = (Fref * N.K * FLL_FRATIO) / FLL_OUTDIV
	 * (3):      N.K = (8 * FLL_OUTDIV) / FLL_FRATIO
	 *
	 * This means that for N.K to be a fractional value, (8 * FLL_OUTDIV) must *not* be
	 * divisible by FLL_FRATIO, and therefore we cannot set FLL_FRATIO to 1, 2, or 4. If we set
	 * FLL_FRATIO=8, we must choose an odd value of FLL_OUTDIV.
	 *
	 * Then we can choose a value of FLL_OUTDIV using equation (1) and the constraint that Fvco
	 * must be within 90 - 100 MHz:
	 * FLL_OUTDIV = 8  --> Fvco = 12.288 * 8 = 98.304 MHz
	 *
	 * Choosing FLL_FRATIO = 8, we can then calculate the value of N.K from equation (3):
	 * N.K = (4 * 8) / 8 = 3.0
	 *
	 * So for the register values:
	 * FLL_FRATIO = 8
	 * FLL_OUTDIV = 8
	 * N = 4
	 * K = 0.0 --> register value is 0.0 * 65536 = 0
	 */
	/* Configure FLL for system clock */
	ret = cwm_i2c_wr(i2c, WM8904_FLL_CONTROL_1, 0x0000);
	if (ret) {
		LOG_ERR("Failed to configure FLL control 1: %d", ret);
		return;
	}

	/* Configure FLL parameters using values from cwm_configure */
	ret = cwm_i2c_wr(i2c, WM8904_FLL_CONTROL_2,
		FLL_C2_OUTDIV(data->fll_outdiv) |
		(data->fll_fratio == 8 ? FLL_C2_FRATIO_DIV8 : 0));
	if (ret) {
		LOG_ERR("Failed to configure FLL control 2: %d", ret);
		return;
	}

	ret = cwm_i2c_wr(i2c, WM8904_FLL_CONTROL_3, FLL_C3_K(data->fll_k));
	if (ret) {
		LOG_ERR("Failed to configure FLL control 3: %d", ret);
		return;
	}

	ret = cwm_i2c_wr(i2c, WM8904_FLL_CONTROL_4, FLL_C4_N(data->fll_n));
	if (ret) {
		LOG_ERR("Failed to configure FLL control 4: %d", ret);
		return;
	}

	ret = cwm_i2c_wr(i2c, WM8904_FLL_CONTROL_5, FLL_C5_CLK_REF_SRC_BCLK);
	if (ret) {
		LOG_ERR("Failed to configure FLL control 5: %d", ret);
		return;
	}

	ret = cwm_i2c_wr(i2c, WM8904_FLL_CONTROL_1, FLL_C1_FRACN_ENA | FLL_C1_FLL_ENA);
	if (ret) {
		LOG_ERR("Failed to enable FLL: %d", ret);
		return;
	}
	k_msleep(5); /* Delay for FLL startup */

	/* Apply sample rate configuration */
	ret = cwm_i2c_wr(i2c, WM8904_CLOCK_RATES_0, data->clock_rate);
	if (ret) {
		LOG_ERR("Failed to configure sample rate: %d", ret);
		return;
	}

	/* Set SYSCLK source to FLL output, Enable system clock, DSP clock enable */
	ret = cwm_i2c_wr(i2c, WM8904_CLOCK_RATES_2,
		CLK_RTE2_SYSCLK_SRC | CLK_RTE2_CLK_SYS_ENA | CLK_RTE2_CLK_DSP_ENA);
	if (ret) {
		LOG_ERR("Failed to configure system clock source: %d", ret);
		return;
	}

	/* Apply audio interface format configuration */
	ret = cwm_i2c_wr(i2c, WM8904_AUDIO_INTERFACE_1, data->aif_format);
	if (ret) {
		LOG_ERR("Failed to configure audio interface format: %d", ret);
		return;
	}

	/* Set up IN2L and IN2R as the ADC inputs, Single ended mode(default) */
	ret = cwm_i2c_wr(i2c, WM8904_ANALOGUE_LEFT_INPUT_1, ANLG_LIN1_IP_SEL_N_IN2L);
	if (ret) {
		LOG_ERR("Failed to configure left input: %d", ret);
		return;
	}
	ret = cwm_i2c_wr(i2c, WM8904_ANALOGUE_RIGHT_INPUT_1, ANLG_RIN1_IP_SEL_N_IN2R);
	if (ret) {
		LOG_ERR("Failed to configure right input: %d", ret);
		return;
	}

	/* Configure mono/stereo mode */
	if (data->is_mono) {
		/* Send left input to both DACs */
		ret = cwm_i2c_wr(i2c, WM8904_AUDIO_INTERFACE_0, 0);
		if (ret) {
			LOG_ERR("Failed to configure mono mode: %d", ret);
			return;
		}
	} else {
		ret = cwm_i2c_wr(i2c, WM8904_AUDIO_INTERFACE_0,
			AUD_INT0_AIFADCR_SRC | AUD_INT0_AIFDACR_SRC);
		if (ret) {
			LOG_ERR("Failed to configure stereo mode: %d", ret);
			return;
		}
	}

	/* Enable DAC and ADC */
	ret = cwm_i2c_wr(i2c, WM8904_POWER_MANAGEMENT_6,
		PWR_MGMT6_DACL_ENA | PWR_MGMT6_DACR_ENA | PWR_MGMT6_ADCL_ENA | PWR_MGMT6_ADCR_ENA);
	if (ret) {
		LOG_ERR("Failed to enable DAC and ADC: %d", ret);
		return;
	}
	k_msleep(5); /* Delay for DAC/ADC startup */

	/* Unmute analog input PGA and use 0dB default volume */
	ret = cwm_i2c_wr(i2c, WM8904_ANALOGUE_LEFT_INPUT_0, ANLG_LIN0_VOL(0x05));
	if (ret) {
		LOG_ERR("Failed to set left input volume: %d", ret);
		return;
	}

	ret = cwm_i2c_wr(i2c, WM8904_ANALOGUE_RIGHT_INPUT_0, ANLG_RIN0_VOL(0x05));
	if (ret) {
		LOG_ERR("Failed to set right input volume: %d", ret);
		return;
	}

	/* Enable headphone output stages in sequence */
	/* Enable input stage of headphones */
	ret = cwm_i2c_wr(i2c, WM8904_ANALOGUE_HP_0, ANLG_HP0_HPL_ENA | ANLG_HP0_HPR_ENA);
	if (ret) {
		LOG_ERR("Failed to enable headphone input stage: %d", ret);
		return;
	}

	/* Enable intermediate stage of headphones */
	ret = cwm_i2c_wr(i2c, WM8904_ANALOGUE_HP_0,
		ANLG_HP0_HPL_ENA |
		ANLG_HP0_HPR_ENA |
		ANLG_HP0_HPL_ENA_DLY |
		ANLG_HP0_HPR_ENA_DLY);
	if (ret) {
		LOG_ERR("Failed to enable headphone intermediate stage: %d", ret);
		return;
	}

	/* Enable DC servo channels */
	ret = cwm_i2c_wr(i2c, WM8904_DC_SERVO_0,
		DC_SRV0_DCS_ENA_CHAN_0 | DC_SRV0_DCS_ENA_CHAN_1 |
		DC_SRV0_DCS_ENA_CHAN_2 | DC_SRV0_DCS_ENA_CHAN_3);
	if (ret) {
		LOG_ERR("Failed to enable DC servo channels: %d", ret);
		return;
	}

	/* Enable DC servo startup mode */
	ret = cwm_i2c_wr(i2c, WM8904_DC_SERVO_1,
		DC_SRV1_DCS_TRIG_STARTUP_0 | DC_SRV1_DCS_TRIG_STARTUP_1 |
		DC_SRV1_DCS_TRIG_STARTUP_2 | DC_SRV1_DCS_TRIG_STARTUP_3);
	if (ret) {
		LOG_ERR("Failed to enable DC servo startup mode: %d", ret);
		return;
	}
	k_msleep(100); /* Delay for DC servo startup */

	/* Enable output stage of headphones */
	ret = cwm_i2c_wr(i2c, WM8904_ANALOGUE_HP_0,
		ANLG_HP0_HPL_ENA_OUTP | ANLG_HP0_HPR_ENA_OUTP |
		ANLG_HP0_HPL_ENA_DLY | ANLG_HP0_HPR_ENA_DLY |
		ANLG_HP0_HPL_ENA | ANLG_HP0_HPR_ENA);
	if (ret) {
		LOG_ERR("Failed to enable headphone output stage: %d", ret);
		return;
	}

	/* Remove shorts from headphone outputs */
	ret = cwm_i2c_wr(i2c, WM8904_ANALOGUE_HP_0,
		ANLG_HP0_HPL_ENA_OUTP | ANLG_HP0_HPR_ENA_OUTP |
		ANLG_HP0_HPL_ENA_DLY | ANLG_HP0_HPR_ENA_DLY |
		ANLG_HP0_HPL_ENA | ANLG_HP0_HPR_ENA |
		ANLG_HP0_HPL_RMV_SHORT | ANLG_HP0_HPR_RMV_SHORT);
	if (ret) {
		LOG_ERR("Failed to remove shorts from headphone outputs: %d", ret);
		return;
	}

	/* Set headphone volume (both channels) */
	ret = cwm_i2c_wr(i2c, WM8904_ANALOGUE_OUT1_LEFT,
		ANLG_OUT1_HPOUTL_VU | data->hp_volume_left);
	if (ret) {
		LOG_ERR("Failed to set left headphone volume: %d", ret);
		return;
	}

	ret = cwm_i2c_wr(i2c, WM8904_ANALOGUE_OUT1_RIGHT,
		ANLG_OUT1_HPOUTR_VU | data->hp_volume_right);
	if (ret) {
		LOG_ERR("Failed to set right headphone volume: %d", ret);
		return;
	}

	k_msleep(100); /* Delay for volume setting to take effect */

	/* Unmute DAC digital path */
	uint16_t current_dac_settings;

	ret = cwm_i2c_rd(i2c, WM8904_DAC_DIGITAL_1, &current_dac_settings);
	if (ret) {
		LOG_ERR("Failed to read current DAC settings: %d", ret);
		return;
	}
	current_dac_settings &= ~DAC_DG1_MUTE;
	ret = cwm_i2c_wr(i2c, WM8904_DAC_DIGITAL_1, current_dac_settings);
	if (ret) {
		LOG_ERR("Failed to unmute DAC: %d", ret);
		return;
	}

	/* Unmute headphone left output */
	uint16_t current_hpoutl_settings;

	ret = cwm_i2c_rd(i2c, WM8904_ANALOGUE_OUT1_LEFT, &current_hpoutl_settings);
	if (ret) {
		LOG_ERR("Failed to read current headphone left settings: %d", ret);
		return;
	}
	current_hpoutl_settings &= ~ANLG_OUT1_HPOUTL_MUTE;
	ret = cwm_i2c_wr(i2c, WM8904_ANALOGUE_OUT1_LEFT, current_hpoutl_settings);
	if (ret) {
		LOG_ERR("Failed to unmute headphone left output: %d", ret);
		return;
	}

	/* Unmute headphone right output */
	uint16_t current_hpoutr_settings;

	ret = cwm_i2c_rd(i2c, WM8904_ANALOGUE_OUT1_RIGHT, &current_hpoutr_settings);
	if (ret) {
		LOG_ERR("Failed to read current headphone right settings: %d", ret);
		return;
	}
	current_hpoutr_settings &= ~ANLG_OUT1_HPOUTR_MUTE;
	ret = cwm_i2c_wr(i2c, WM8904_ANALOGUE_OUT1_RIGHT, current_hpoutr_settings);
	if (ret) {
		LOG_ERR("Failed to unmute headphone right output: %d", ret);
		return;
	}

	/* Add small delay to allow outputs to settle */
	k_msleep(5);

	LOG_DBG("Started");
}

/* Stop the codec output for power saving, allowing later reconfiguration and restart */
static void cwm_stop_output(const struct device *dev)
{
	const struct wm8904_driver_config *dev_cfg = dev->config;
	const struct i2c_dt_spec *i2c = &dev_cfg->i2c;
	int ret;

	/* 1. Mute DAC outputs first to prevent pops while preserving other settings */
	/* Mute DAC digital path */
	uint16_t current_dac_settings;

	ret = cwm_i2c_rd(i2c, WM8904_DAC_DIGITAL_1, &current_dac_settings);
	if (ret) {
		LOG_ERR("Failed to read current DAC settings: %d", ret);
		return;
	}

	current_dac_settings |= DAC_DG1_MUTE;

	ret = cwm_i2c_wr(i2c, WM8904_DAC_DIGITAL_1, current_dac_settings);
	if (ret) {
		LOG_ERR("Failed to mute DAC: %d", ret);
		return;
	}

	/* Mute headphone left output */
	uint16_t current_hpoutl_settings;

	ret = cwm_i2c_rd(i2c, WM8904_ANALOGUE_OUT1_LEFT, &current_hpoutl_settings);
	if (ret) {
		LOG_ERR("Failed to read current headphone left settings: %d", ret);
		return;
	}

	current_hpoutl_settings |= ANLG_OUT1_HPOUTL_MUTE;

	ret = cwm_i2c_wr(i2c, WM8904_ANALOGUE_OUT1_LEFT, current_hpoutl_settings);
	if (ret) {
		LOG_ERR("Failed to mute headphone left output: %d", ret);
		return;
	}

	/* Mute headphone right output */
	uint16_t current_hpoutr_settings;

	ret = cwm_i2c_rd(i2c, WM8904_ANALOGUE_OUT1_RIGHT, &current_hpoutr_settings);
	if (ret) {
		LOG_ERR("Failed to read current headphone right settings: %d", ret);
		return;
	}

	current_hpoutr_settings |= ANLG_OUT1_HPOUTR_MUTE;

	ret = cwm_i2c_wr(i2c, WM8904_ANALOGUE_OUT1_RIGHT, current_hpoutr_settings);
	if (ret) {
		LOG_ERR("Failed to mute headphone right output: %d", ret);
		return;
	}

	/* Allow mute to take effect */
	k_msleep(2);

	/* Safely power down headphone outputs according to spec v4.1 */
	/* Re-apply shorts to headphone outputs (removing RMV_SHORT flags) */
	ret = cwm_i2c_wr(i2c, WM8904_ANALOGUE_HP_0,
		ANLG_HP0_HPL_ENA_OUTP | ANLG_HP0_HPR_ENA_OUTP |
		ANLG_HP0_HPL_ENA_DLY | ANLG_HP0_HPR_ENA_DLY |
		ANLG_HP0_HPL_ENA | ANLG_HP0_HPR_ENA);
	if (ret) {
		LOG_ERR("Failed to re-apply shorts to headphone outputs: %d", ret);
		return;
	}

	/* Disable output stage of headphones */
	ret = cwm_i2c_wr(i2c, WM8904_ANALOGUE_HP_0,
		ANLG_HP0_HPL_ENA_DLY | ANLG_HP0_HPR_ENA_DLY |
		ANLG_HP0_HPL_ENA | ANLG_HP0_HPR_ENA);
	if (ret) {
		LOG_ERR("Failed to disable headphone output stage: %d", ret);
		return;
	}

	/* Disable intermediate stage of headphones */
	ret = cwm_i2c_wr(i2c, WM8904_ANALOGUE_HP_0,
		ANLG_HP0_HPL_ENA | ANLG_HP0_HPR_ENA);
	if (ret) {
		LOG_ERR("Failed to disable headphone intermediate stage: %d", ret);
		return;
	}

	/* Disable input stage of headphones */
	ret = cwm_i2c_wr(i2c, WM8904_ANALOGUE_HP_0, 0);
	if (ret) {
		LOG_ERR("Failed to disable headphone input stage: %d", ret);
		return;
	}

	/* Disable DAC and ADC to save power */
	ret = cwm_i2c_wr(i2c, WM8904_POWER_MANAGEMENT_6, 0);
	if (ret) {
		LOG_ERR("Failed to disable DAC and ADC: %d", ret);
		return;
	}

	/* Disable clocks to save power */
	ret = cwm_i2c_wr(i2c, WM8904_CLOCK_RATES_2, 0);
	if (ret) {
		LOG_ERR("Failed to disable clocks: %d", ret);
		return;
	}

	/* Disable FLL if it was enabled */
	ret = cwm_i2c_wr(i2c, WM8904_FLL_CONTROL_1, 0);
	if (ret) {
		LOG_ERR("Failed to disable FLL: %d", ret);
		return;
	}

	/* Disable charge pump */
	ret = cwm_i2c_wr(i2c, WM8904_CHARGE_PUMP_0, 0);
	if (ret) {
		LOG_ERR("Failed to disable charge pump: %d", ret);
		return;
	}

	/* Disable VMID */
	ret = cwm_i2c_wr(i2c, WM8904_VMID_CONTROL_0, 0);
	if (ret) {
		LOG_ERR("Failed to disable VMID: %d", ret);
		return;
	}

	/* Disable bias generator */
	ret = cwm_i2c_wr(i2c, WM8904_BIAS_CONTROL_0, 0);
	if (ret) {
		LOG_ERR("Failed to disable bias generator: %d", ret);
		return;
	}

	/* Add small delay to allow outputs to settle */
	k_msleep(5);

	LOG_DBG("Stopped");
}

/* Set a codec property */
static int cwm_set_property(const struct device *dev, audio_property_t property,
			       audio_channel_t channel, audio_property_value_t val)
{
	const struct wm8904_driver_config *dev_cfg = dev->config;
	const struct i2c_dt_spec *i2c = &dev_cfg->i2c;
	struct wm8904_data *data = dev->data;
	int ret;

	switch (property) {
	case AUDIO_PROPERTY_OUTPUT_VOLUME: {
		if (val.vol < 0 || val.vol > 255) {
			LOG_ERR("Invalid volume: %d", val.vol);
			return -EINVAL;
		}

		/* WM8904 volume is in 1dB steps so scale the API value */
		uint16_t const volume = ANLG_OUT1_HPOUTL_VOL(val.vol >> 1);
		bool const both = channel == AUDIO_CHANNEL_ALL;

		if (both || channel == AUDIO_CHANNEL_FRONT_LEFT) {
			ret = cwm_i2c_wr(i2c, WM8904_ANALOGUE_OUT1_LEFT,
				ANLG_OUT1_HPOUTL_VU | volume);
			if (ret) {
				LOG_ERR("Failed to set left headphone volume: %d", ret);
				return ret;
			}
			/* Store for configure to avoid unwanted volume adjustment */
			data->hp_volume_left = volume;
		}
		if (both || channel == AUDIO_CHANNEL_FRONT_RIGHT) {
			ret = cwm_i2c_wr(i2c, WM8904_ANALOGUE_OUT1_RIGHT,
				ANLG_OUT1_HPOUTR_VU | volume);
			if (ret) {
				LOG_ERR("Failed to set right headphone volume: %d", ret);
				return ret;
			}
			/* Store for configure to avoid unwanted volume adjustment */
			data->hp_volume_right = volume;
		}

		break;
	}

	case AUDIO_PROPERTY_OUTPUT_MUTE:
		ret = cwm_i2c_wr(i2c, WM8904_DAC_DIGITAL_1,
				 data->dac_digital_settings | (val.mute ? DAC_DG1_MUTE : 0));
		if (ret) {
			LOG_ERR("Failed to set mute state: %d", ret);
			return ret;
		}
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

/* Apply any cached properties */
static int cwm_apply_properties(const struct device *dev)
{
	/* Property caching not implemented */
	return -ENOSYS;
}

static int cwm_clear_errors(const struct device *dev)
{
	/* This codec doesn't have a specific error register to clear */
	return 0;
}

static int cwm_register_error_callback(const struct device *dev, audio_codec_error_callback_t cb)
{
	/* This codec doesn't support error callbacks */
	return -ENOSYS;
}

/* WM8904 driver API structure */
static const struct audio_codec_api wm8904_driver_api = {
	.configure = cwm_configure,
	.start_output = cwm_start_output,
	.stop_output = cwm_stop_output,
	.set_property = cwm_set_property,
	.apply_properties = cwm_apply_properties,
	.clear_errors = cwm_clear_errors,
	.register_error_callback = cwm_register_error_callback};

int cwm_init(const struct device *dev)
{
	const struct wm8904_driver_config *dev_cfg = dev->config;
	const struct i2c_dt_spec *i2c = &dev_cfg->i2c;
	struct wm8904_data *data = dev->data;
	uint16_t dev_id;
	int ret;

	LOG_DBG("Initializing");

	/* Reset device and then read ID */
	ret = cwm_i2c_wr(i2c, WM8904_SW_RESET_AND_ID, 0xFFFF);
	if (ret) {
		LOG_ERR("Failed to reset WM8904 device: %d", ret);
		return ret;
	}

	/* Verify I2C device is responsive */
	ret = cwm_i2c_rd(i2c, WM8904_SW_RESET_AND_ID, &dev_id);
	if (ret) {
		LOG_ERR("Failed to read WM8904 device ID: %d", ret);
		return ret;
	}

	/* Check if device ID matches expected value */
	if (dev_id != WM8904_DEV_ID) {
		LOG_ERR("Got incorrect dev ID from WM8904 device: 0x%04x (expected 0x%04x)",
			dev_id, WM8904_DEV_ID);
		return -ENODEV;
	}

	/* Setup default audio configuration */
	struct audio_codec_cfg audio_cfg = {
		.mclk_freq = 12288000,
		.dai_type = AUDIO_DAI_TYPE_I2S,
		.dai_cfg.i2s = {
			.word_size = 16,
			.channels = 2,
			.format = I2S_FMT_DATA_FORMAT_I2S,
			.options = 0,
			.frame_clk_freq = 48000,
			.mem_slab = NULL,
			.block_size = 0,
			.timeout = 0,
		},
	};

	/* Configure the codec with default settings */
	ret = cwm_configure(dev, &audio_cfg);
	if (ret) {
		LOG_ERR("Failed to configure WM8904: %d", ret);
		return ret;
	}

	/* Set the default volume level to 0dB (0x39) */
	data->hp_volume_right = data->hp_volume_left = ANLG_OUT1_HPOUTL_VOL(0x39);

	LOG_DBG("Initialisation completed");

	return 0;
}

#define WM8904_HAS_MCLK(inst) DT_INST_NODE_HAS_PROP(inst, clocks)

#define WM8904_DEFINE(inst)                                                                        \
	static struct wm8904_data wm8904_data_##inst;                                              \
	static const struct wm8904_driver_config wm8904_config_##inst = {                          \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.clock_source = DT_INST_PROP_OR(                                                   \
			inst, clk_source,                                                          \
			0), /* Initialize clock-related fields only if clocks property exists */   \
		.mclk_dev = COND_CODE_1(WM8904_HAS_MCLK(inst),                                     \
					(DEVICE_DT_GET(DT_INST_CLOCKS_CTLR_BY_NAME(inst, mclk))),  \
					(NULL)),                                                   \
		.mclk_name = COND_CODE_1(                                                          \
			WM8904_HAS_MCLK(inst),                                                     \
			((clock_control_subsys_t)DT_INST_CLOCKS_CELL_BY_NAME(inst, mclk, name)),   \
			(NULL))};                                                                  \
	DEVICE_DT_INST_DEFINE(inst, cwm_init, NULL, &wm8904_data_##inst, &wm8904_config_##inst, \
			      POST_KERNEL, CONFIG_WM8904_INIT_PRIORITY, &wm8904_driver_api);

DT_INST_FOREACH_STATUS_OKAY(WM8904_DEFINE)

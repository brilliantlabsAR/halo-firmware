/** @file t5838.h
 *
 * @brief Header for added AAD functionality for T5838 implemented in alif PDM driver.
 *
 * @par
 * COPYRIGHT NOTICE: (c) 2023 Irnas.  All rights reserved.
 * Modifications Copyright (c) 2025  Brilliant Labs Ltd
 */

#ifndef T5838_H
#define T5838_H

#ifdef __cplusplus
extern "C" {
#endif

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#include <stdint.h>

#define T5838_REG_AAD_MODE                       0x29
#define T5838_REG_AAD_D_FLOOR_HI                 0x2A
#define T5838_REG_AAD_D_FLOOR_LO                 0x2B
#define T5838_REG_AAD_D_REL_PULSE_MIN_LO         0x2E
#define T5838_REG_AAD_D_ABS_REL_PULSE_MIN_SHARED 0x2F
#define T5838_REG_AAD_D_ABS_PULSE_MIN_LO         0x30
#define T5838_REG_AAD_D_ABS_THR_LO               0x31
#define T5838_REG_AAD_D_ABS_THR_HI               0x32
#define T5838_REG_AAD_D_REL_THR                  0x33
#define T5838_REG_AAD_A_LPF                      0x35
#define T5838_REG_AAD_A_THR                      0x36

/**
 * @brief AAD modes
 *
 * This setting selects desired AAD mode.
 */
enum t5838_aad_select {
	T5838_AAD_SELECT_NONE = 0x00,
	T5838_AAD_SELECT_D1 = 0x01,
	T5838_AAD_SELECT_D2 = 0x02, /* We received info that this is supposed to be 0x0F? */
	T5838_AAD_SELECT_A = 0x08
};

/**
 * @brief AAD A mode low pass filter
 *
 * This setting selects low pass filter for AAD A mode.
 */
enum t5838_aad_a_lpf {
	T5838_AAD_A_LPF_4_4kHz = 0x01,
	T5838_AAD_A_LPF_2_0kHz = 0x02,
	T5838_AAD_A_LPF_1_9kHz = 0x03,
	T5838_AAD_A_LPF_1_8kHz = 0x04,
	T5838_AAD_A_LPF_1_6kHz = 0x05,
	T5838_AAD_A_LPF_1_3kHz = 0x06,
	T5838_AAD_A_LPF_1_1kHz = 0x07
};

/**
 * @brief AAD A mode threshold
 *
 * This setting selects threshold for AAD A mode.
 * @note According to datasheet there are 8 possible values, but table in datasheet describes 9
 * values. it seems to be 2,5dB step for LSB but only every other value exist in table.
 */
enum t5838_aad_a_thr {
	T5838_AAD_A_THR_60dB = 0x00,
	T5838_AAD_A_THR_65dB = 0x02,
	T5838_AAD_A_THR_70dB = 0x04,
	T5838_AAD_A_THR_75dB = 0x06,
	T5838_AAD_A_THR_80dB = 0x08,
	T5838_AAD_A_THR_85dB = 0x0A,
	T5838_AAD_A_THR_90dB = 0x0C,
	T5838_AAD_A_THR_95dB = 0x0E,
	T5838_AAD_A_THR_97_5dB = 0x0F
};

/**
 * @brief AAD D mode absolute threshold
 *
 * This setting selects absolute threshold for AAD D mode.
 * @note Datasheet claim values between 0x00F and 0x7BC are allowed, but only provides us with table
 * of discrete values defined in this enumerator.
 */
enum t5838_aad_d_abs_thr {
	T5838_AAD_D_ABS_THR_40dB = 0x000F,
	T5838_AAD_D_ABS_THR_45dB = 0x0016,
	T5838_AAD_D_ABS_THR_50dB = 0x0032,
	T5838_AAD_D_ABS_THR_55dB = 0x0037,
	T5838_AAD_D_ABS_THR_60dB = 0x005F,
	T5838_AAD_D_ABS_THR_65dB = 0x00A0,
	T5838_AAD_D_ABS_THR_70dB = 0x0113,
	T5838_AAD_D_ABS_THR_75dB = 0x01E0,
	T5838_AAD_D_ABS_THR_80dB = 0x0370,
	T5838_AAD_D_ABS_THR_85dB = 0x062C,
	T5838_AAD_D_ABS_THR_87dB = 0x07BC
};

/**
 * @brief AAD D mode relative threshold
 *
 * This setting selects relative threshold for AAD D mode.
 * @note Datasheet claim values between 0x24 and 0xFF are allowed, but only provides us with table
 * of discrete values defined in this enumerator.
 */
enum t5838_aad_d_rel_thr {
	T5838_AAD_D_REL_THR_3dB = 0x24,
	T5838_AAD_D_REL_THR_6dB = 0x36,
	T5838_AAD_D_REL_THR_9dB = 0x48,
	T5838_AAD_D_REL_THR_12dB = 0x64,
	T5838_AAD_D_REL_THR_15dB = 0x8F,
	T5838_AAD_D_REL_THR_18dB = 0xCA,
	T5838_AAD_D_REL_THR_20dB = 0xFF
};

/**
 * @brief AAD D mode floor
 *
 * This setting selects relative threshold floor for AAD D mode.
 * @note Datasheet claim values between 0x00F and 0x7BC are allowed, but only provides us with table
 * of discrete values defined in this enumerator.
 */
enum t5838_aad_d_floor {
	T5838_AAD_D_FLOOR_40dB = 0x000F,
	T5838_AAD_D_FLOOR_45dB = 0x0016,
	T5838_AAD_D_FLOOR_50dB = 0x0032,
	T5838_AAD_D_FLOOR_55dB = 0x0037,
	T5838_AAD_D_FLOOR_60dB = 0x005F,
	T5838_AAD_D_FLOOR_65dB = 0x00A0,
	T5838_AAD_D_FLOOR_70dB = 0x0113,
	T5838_AAD_D_FLOOR_75dB = 0x01E0,
	T5838_AAD_D_FLOOR_80dB = 0x0370,
	T5838_AAD_D_FLOOR_85dB = 0x062C,
	T5838_AAD_D_FLOOR_87dB = 0x07BC
};

/**
 * @brief AAD D mode relative pulse minimum
 *
 * This setting selects pulse minimum for AAD D mode relative threshold detection.
 * @note Datasheet claim values between 0x000 and 0x12C are allowed, but only provides us with table
 * of discrete values defined in this enumerator.
 */
enum t5838_aad_d_rel_pulse_min {
	T5838_AAD_D_REL_PULSE_MIN_0_7ms = 0x0000,
	T5838_AAD_D_REL_PULSE_MIN_10ms = 0x0064,
	T5838_AAD_D_REL_PULSE_MIN_19ms = 0x00C8,
	T5838_AAD_D_REL_PULSE_MIN_29ms = 0x012C
};

/**
 * @brief AAD D mode absolute pulse minimum
 *
 * This setting selects pulse minimum for AAD D mode absolute threshold detection.
 * @note Datasheet claim values between 0x000 and 0x0DAC are allowed, but only provides us with
 * table of discrete values defined in this enumerator.
 */
enum t5838_aad_d_abs_pulse_min {
	T5838_AAD_D_ABS_PULSE_MIN_1_1ms = 0x0000,
	T5838_AAD_D_ABS_PULSE_MIN_10ms = 0x0064,
	T5838_AAD_D_ABS_PULSE_MIN_19ms = 0x00C8,
	T5838_AAD_D_ABS_PULSE_MIN_29ms = 0x012C,
	T5838_AAD_D_ABS_PULSE_MIN_48ms = 0x01F4,
	T5838_AAD_D_ABS_PULSE_MIN_95ms = 0x03E8,
	T5838_AAD_D_ABS_PULSE_MIN_188ms = 0x07D0,
	T5838_AAD_D_ABS_PULSE_MIN_282ms = 0x0BB8,
	T5838_AAD_D_ABS_PULSE_MIN_328ms = 0x0DAC
};

typedef void (*t5838_wake_handler_t)(const struct device *dev);

/**
 * @brief AAD A configuration structure
 *
 * This structure holds full configuration of AAD A mode to be written to device.
 */
struct t5838_aad_a_conf {
	enum t5838_aad_a_lpf aad_a_lpf;
	enum t5838_aad_a_thr aad_a_thr;
	uint32_t silent_period; // in ms
};

/**
 * @brief AAD D configuration structure
 *
 * This structure holds full configuration of AAD D1 and D2 modes to be written to device.
 */
struct t5838_aad_d_conf {
	enum t5838_aad_d_floor aad_d_floor;
	enum t5838_aad_d_rel_pulse_min aad_d_rel_pulse_min;
	enum t5838_aad_d_abs_pulse_min aad_d_abs_pulse_min;
	enum t5838_aad_d_abs_thr aad_d_abs_thr;
	enum t5838_aad_d_rel_thr aad_d_rel_thr;
};

struct t5838_drv_data {
	struct k_mem_slab *mem_slab;
	uint32_t block_size;
	uint8_t channel_map;
	uint8_t mode;
	uint8_t pcm_width;
	struct k_msgq rx_queue;
	bool configured;
	volatile bool active;
	volatile bool stopping;
	uint16_t *rx_data;
	uint8_t *block;
	uint32_t block_len;
	void *rx_queue_buf;
	uint16_t gain[8];      // gain for each channel
	uint32_t threshold[8]; // threshold selection for each channel
	uint8_t num_channels;
	volatile uint32_t discard_samples;

#ifdef CONFIG_T5838_AAD_TRIGGER
	/* Pointer to child device for putting device back into low power after sampling */
	const struct device *aad_child_dev;
#endif /* CONFIG_T5838_AAD_TRIGGER */
};

struct t5838_drv_cfg {
	uint32_t regs;
	uint8_t fifo_watermark;
	uint8_t queue_depth;
	uint16_t gain;
	bool iir_bypass;
	bool fir_bypass;
	uint32_t discard_duration_ms;
	void (*irq_config)(const struct device *dev);
	const struct pinctrl_dev_config *pcfg;
	/* LPPDM RX DMA drain (present only when the node declares a "rxdma"
	 * dmas entry; otherwise the driver uses the almost-full ISR). dma_dev
	 * NULL => ISR path.
	 */
	const struct device *dma_dev;
	uint8_t dma_ch;
	uint8_t dma_req;
};

#ifdef CONFIG_T5838_AAD_TRIGGER
struct t5838_aad_drv_cfg {
	const struct device *pdm_dev;

	const struct gpio_dt_spec micen;
	bool micen_available;

	const struct gpio_dt_spec wake;
	const struct gpio_dt_spec thsel;
	const struct gpio_dt_spec pdmclk;
};

struct t5838_aad_drv_data {

	bool aad_unlocked;
	enum t5838_aad_select aad_enabled_mode;
	uint32_t silent_period; // in ms
	struct gpio_callback wake_cb;
	bool cb_configured;
	bool int_handled;
	t5838_wake_handler_t wake_handler;

	const struct t5838_aad_drv_cfg *aad_cfg;
    const struct device *dev;
	struct k_spinlock lock;
#ifdef CONFIG_T5838_AAD_TRIGGER_OWN_THREAD
    K_KERNEL_STACK_MEMBER(thread_stack, CONFIG_T5838_AAD_TRIGGER_STACK_SIZE);
    struct k_thread thread;
    struct k_sem wake_sem;
#elif defined(CONFIG_T5838_AAD_TRIGGER_GLOBAL_THREAD)
    struct k_work work;
#endif
};
#endif /* CONFIG_T5838_AAD_TRIGGER */

#ifdef CONFIG_T5838_AAD_TRIGGER
/**
 * @brief Set AAD wake pin interrupt handler function
 *
 * Function will set handler function to be called when interrupt is triggered. We can set handler
 * at any time, but t5838_configure_AAD() must be called for interrupt triggering to be enabled.
 *
 * @note when interrupt gets triggered it will disable further interrupts until
 * t5838_wake_clear() is called.
 *
 * @param[in] dev Pointer to the device structure for the driver instance.
 * @param[in] handler Pointer to the handler function to be called when interrupt is triggered.
 */
void t5838_aad_wake_handler_set(const struct device *dev, t5838_wake_handler_t handler);

/**
 * @brief Clear AAD wake pin interrupt and re-enable AAD interrupts.
 *
 * @param[in] dev Pointer to the device structure for the driver instance.
 *
 * @retval 0 if successful.
 * @retval negative errno code if othewise.
 */
int t5838_aad_wake_clear(const struct device *dev);

/**
 * @brief Configure T5838 device into AAD A mode
 *
 * Function will configure and run T5838 AAD and configure interrupts on wake pin.
 *
 * @param[in] dev Pointer to the device structure for the driver instance.
 * @param[in] aadconf Pointer to the structure containing AAD A configuration
 *
 * @retval 0 if successful.
 * @retval negative errno code if othewise.
 */
int t5838_aad_a_mode_set(const struct device *dev, struct t5838_aad_a_conf *aadconf);

/**
 * @brief Configure T5838 device into AAD D1 mode
 *
 * Function will configure and run T5838 AAD and configure interrupts on wake pin.
 *
 * @param[in] dev Pointer to the device structure for the driver instance.
 * @param[in] aadconf Pointer to the structure containing AAD D configuration
 *
 * @retval 0 if successful.
 * @retval negative errno code if othewise.
 */
int t5838_aad_d1_mode_set(const struct device *dev, struct t5838_aad_d_conf *aadconf);

/**
 * @brief Configure T5838 device into AAD D2 mode
 *
 * Function will configure and run T5838 AAD and configure interrupts on wake pin.
 *
 * @param[in] dev Pointer to the device structure for the driver instance.
 * @param[in] aadconf Pointer to the structure containing AAD D configuration
 *
 * @retval 0 if successful.
 * @retval negative errno code if othewise.
 */
int t5838_aad_d2_mode_set(const struct device *dev, struct t5838_aad_d_conf *aadconf);

/**
 * @brief Disable AAD functionality
 *
 * Function will disable AAD functionality and disable interrupts on wake pin.
 *
 * @param[in] dev Pointer to the device structure for the driver instance.
 *
 * @retval 0 if successful.
 * @retval negative errno code if othewise.
 */
int t5838_aad_mode_disable(const struct device *dev);

/**
 * @brief Reset T5838 device using mic enable pin GPIO
 *
 * @param[in] dev Pointer to the device structure for the driver instance.
 *
 * @retval 0 if successful.
 * @retval negative errno code if othewise.
 */
int t5838_reset(const struct device *dev);

/**
 * @brief Function for putting T5838 into sleep mode with AAD is enabled. Must be called after
 * writing to AAD registers.
 *
 * function clocks device for value set in T5838_ENTER_SLEEP_MODE_CLOCKING_TIME_US to enable AAD.
 *
 * @param[in] dev Pointer to the device structure for the driver instance.
 */
void t5838_aad_sleep(const struct device *dev);

#endif /* CONFIG_T5838_AAD_TRIGGER */

/**
 * @brief Set gain for DMIC
 *
 * @param[in] dev Pointer to the device structure for the driver instance.
 * @param[in] gain Gain value to set
 *
 * @retval 0 if successful.
 * @retval negative errno code if othewise.
 */
int dmic_set_gain(const struct device *dev, int8_t gain);

#ifdef __cplusplus
}
#endif

#endif /* T5838_H */

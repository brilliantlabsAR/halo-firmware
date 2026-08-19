
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/input/button.h>
//#include <t5838.h>

void button_event_cb(const struct device *dev, enum button_action action)
{
	switch (action) {
	case BUTTON_SINGLE_CLICK:
		printf("\nSingle Click: %d\n", k_uptime_get_32());
		break;
	case BUTTON_DOUBLE_CLICK:
		printf("\nDouble Click: %d\n", k_uptime_get_32());
		break;
	case BUTTON_LONG_PRESS:
		printf("\nLong Press: %d\n", k_uptime_get_32());
		break;
	case BUTTON_LONG_PRESS_LEVEL_1:
		printf("\nLong Press Level 1: %d\n", k_uptime_get_32());
		break;
	case BUTTON_LONG_PRESS_LEVEL_2:
		printf("\nLong Press Level 2: %d\n", k_uptime_get_32());
		break;
	case BUTTON_LONG_PRESS_LEVEL_3:
		printf("\nLong Press Level 3: %d\n", k_uptime_get_32());
		break;
	default:
		break;
	}
}

void t5838_cb(const struct device *dev)
{
	printf("\nT5838 interrupt triggered\n");
}

int main(void)
{
	const struct device *button = DEVICE_DT_GET(DT_ALIAS(sw0));

	button_callback_register(button, button_event_cb);

	// const struct device *t5838 = DEVICE_DT_GET(DT_NODELABEL(t5838));

	// /*AAD A CONFIGURATION */
	// struct t5838_aad_a_conf aadcfg = {
	// 	.aad_a_lpf = T5838_AAD_A_LPF_2_0kHz,
	// 	.aad_a_thr = T5838_AAD_A_THR_90dB,
	// 	.silent_period = 1000,
	// };
	// t5838_aad_a_mode_set(t5838, &aadcfg);
	// t5838_aad_wake_handler_set(t5838, t5838_cb);

	while (1) {
		k_sleep(K_SECONDS(1));
	}
	return 0;
}

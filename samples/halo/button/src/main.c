#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/input/button.h>

LOG_MODULE_REGISTER(button_sample, LOG_LEVEL_INF);

/* Event counter for statistics */
static uint32_t event_count[BUTTON_EVENT_NUM] = {0};

/* Get timestamp in milliseconds */
static inline int64_t get_timestamp_ms(void)
{
	return k_uptime_get();
}

/* Unified button event callback - handles all events */
static void button_event_cb(const struct device *dev, enum button_action action)
{
	int64_t timestamp = get_timestamp_ms();
	event_count[action]++;

	switch (action) {
	case BUTTON_LONG_PRESS:
		printk("[%lld ms] Event #%d: Long Press (1 second)\n", timestamp,
		       event_count[action]);
		break;
	case BUTTON_LONG_PRESS_LEVEL_1:
		printk("[%lld ms] Event #%d: Long Press Level 1 (3 seconds)\n", timestamp,
		       event_count[action]);
		break;
	case BUTTON_LONG_PRESS_LEVEL_2:
		printk("[%lld ms] Event #%d: Long Press Level 2 (8 seconds)\n", timestamp,
		       event_count[action]);
		break;
	case BUTTON_LONG_PRESS_LEVEL_3:
		printk("[%lld ms] Event #%d: Long Press Level 3 (10 seconds)\n", timestamp,
		       event_count[action]);
		break;
	case BUTTON_SINGLE_CLICK:
		printk("[%lld ms] Event #%d: Single Click\n", timestamp, event_count[action]);
		break;
	case BUTTON_DOUBLE_CLICK:
		printk("[%lld ms] Event #%d: Double Click\n", timestamp, event_count[action]);
		break;
	default:
		printk("[%lld ms] Unknown button event: %d\n", timestamp, action);
		break;
	}
}

/* Print event statistics */
static void print_statistics(void)
{
	printk("\n========== Button Event Statistics ==========\n");
	printk("Single Click:           %d times\n", event_count[BUTTON_SINGLE_CLICK]);
	printk("Double Click:           %d times\n", event_count[BUTTON_DOUBLE_CLICK]);
	printk("Long Press (1s):        %d times\n", event_count[BUTTON_LONG_PRESS]);
	printk("Long Press Level 1 (3s): %d times\n", event_count[BUTTON_LONG_PRESS_LEVEL_1]);
	printk("Long Press Level 2 (8s): %d times\n", event_count[BUTTON_LONG_PRESS_LEVEL_2]);
	printk("Long Press Level 3 (10s): %d times\n", event_count[BUTTON_LONG_PRESS_LEVEL_3]);
	printk("=============================================\n\n");
}

int main(void)
{
	const struct device *button = DEVICE_DT_GET(DT_ALIAS(sw0));
	int ret;
	uint32_t loop_count = 0;

	printk("\n");
	printk("=============================================\n");
	printk("  Button Event Test Application\n");
	printk("=============================================\n");
	printk("Button device: %s\n", button->name);

	if (!device_is_ready(button)) {
		printk("ERROR: Button device not ready!\n");
		return -1;
	}

	printk("\n");
	printk("Button Events Configuration:\n");
	printk("  - Single Click:     Quick press and release\n");
	printk("  - Double Click:     Two quick clicks within 400ms\n");
	printk("  - Long Press:       Hold for 1 second\n");
	printk("  - Long Press Lv1:   Hold for 3 seconds\n");
	printk("  - Long Press Lv2:   Hold for 8 seconds\n");
	printk("  - Long Press Lv3:   Hold for 10 seconds\n");
	printk("\n");

	/* Select callback registration mode */
#if 1 /* Use unified callback for all events */
	printk("Mode: Unified callback for all events\n");
	ret = button_callback_register(button, button_event_cb);
	if (ret < 0) {
		printk("ERROR: Failed to register button callback\n");
		return ret;
	}
#else /* Use individual callbacks for each event */
	printk("Mode: Individual callbacks for each event\n");
	button_event_callback_register(button, button_event_cb_long_press, BUTTON_LONG_PRESS);
	button_event_callback_register(button, button_event_cb_long_press_level_1,
				       BUTTON_LONG_PRESS_LEVEL_1);
	button_event_callback_register(button, button_event_cb_long_press_level_2,
				       BUTTON_LONG_PRESS_LEVEL_2);
	button_event_callback_register(button, button_event_cb_long_press_level_3,
				       BUTTON_LONG_PRESS_LEVEL_3);
	button_event_callback_register(button, button_event_cb_single_click, BUTTON_SINGLE_CLICK);
	button_event_callback_register(button, button_event_cb_double_click, BUTTON_DOUBLE_CLICK);
#endif

	printk("\n>>> Button test started. Press the button to test events...\n\n");

	/* Main loop - print statistics periodically */
	while (1) {
		k_sleep(K_SECONDS(30));
		loop_count++;

		/* Print statistics every 30 seconds */
		if (loop_count % 1 == 0) {
			print_statistics();
		}
	}

	return 0;
}

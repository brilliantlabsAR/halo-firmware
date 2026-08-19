#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(vbat, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/pm/device.h>

const struct device *vbat = DEVICE_DT_GET(DT_NODELABEL(vbat));

void read_vbat_status(void)
{
	struct sensor_value voltage, soc, charge_status;

	if (!device_is_ready(vbat)) {
		LOG_ERR("vbat sensor not ready");
		return;
	}

	if (sensor_sample_fetch(vbat) != 0) {
		LOG_ERR("Sample fetch failed");
		return;
	}

	sensor_channel_get(vbat, SENSOR_CHAN_GAUGE_VOLTAGE, &voltage);
	sensor_channel_get(vbat, SENSOR_CHAN_GAUGE_STATE_OF_CHARGE, &soc);
	sensor_channel_get(vbat, SENSOR_CHAN_GAUGE_STDBY_CURRENT, &charge_status);

	printf("Voltage: %dmV, SoC: %d%%, Charge Status: %d\n", voltage.val1, soc.val1, charge_status.val1);
}

int main(void)
{
	while (1) {
		read_vbat_status();
		k_sleep(K_SECONDS(1));
	}
	return 0;
}

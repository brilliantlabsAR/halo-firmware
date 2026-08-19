#include <zephyr/kernel.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/sys/printk.h>

int main(void)
{
	printk("Regulator Control Demo\n");

	const struct device *vpos = DEVICE_DT_GET(DT_NODELABEL(vpos));
	const struct device *veng = DEVICE_DT_GET(DT_NODELABEL(veng));

	if (!device_is_ready(vpos) || !device_is_ready(veng)) {
		printk("Regulators not ready!\n");
		return 0;
	}

	while (1) {
		if (regulator_enable(vpos) != 0) {
			printk("Failed to enable VPOS!\n");
		} else {
			printk("VPOS enabled\n");
		}

		if (regulator_enable(veng) != 0) {
			printk("Failed to enable VENG!\n");
		} else {
			printk("VENG enabled\n");
		}

		k_sleep(K_SECONDS(10));
		regulator_disable(vpos);
		printk("VPOS disabled\n");
		regulator_disable(veng);
		printk("VENG disabled\n");

		k_sleep(K_SECONDS(10));
	}
	return 0;
}
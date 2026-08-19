#include <string.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#if CONFIG_DISPLAY
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/display/cdc200.h>
#include <zephyr/drivers/mipi_dsi/dsi_dw.h>
#include <zephyr/pm/device.h>
#include <canvas.h>

static const struct device *dsi = NULL;
static const struct device *panel = NULL;
static const struct device *display = NULL;

static Canvas canvas;

static int color_index = 0;
/* Color list */
static const Color color_list[] = {
	COLOR_HEX(0xFFFFFF), // White
	COLOR_HEX(0xFF0000), // Red
	COLOR_HEX(0x00FF00), // Green
	COLOR_HEX(0x0000FF), // Blue
	COLOR_HEX(0x000000), // Black
	COLOR_HEX(0x646464), // Gray
};
#define COLOR_COUNT (sizeof(color_list) / sizeof(color_list[0]))
#define MODE_COUNT  (COLOR_COUNT + 1)

static void update_display_color(void)
{
	canvas_clear(&canvas, color_list[color_index]);
}

void draw_grayscale_pyramid(const struct shell *sh)
{
	const int cx = LOG_WIDTH / 2;  // Center X coordinate (160)
	const int cy = LOG_HEIGHT / 2; // Center Y coordinate (120)

	const int W = LOG_WIDTH;  // Logical display width (320)
	const int H = LOG_HEIGHT; // Logical display height (240)

	// Iterate through every pixel on the screen
	for (int y = 0; y < LOG_HEIGHT; y++) {
		for (int x = 0; x < LOG_WIDTH; x++) {
			int dx = abs(x - cx); // Horizontal distance to center
			int dy = abs(y - cy); // Vertical distance to center

			// Simulate pyramid height: scale and take the max
			int dist = MAX(dx * H, dy * W);

			// Normalize distance to a grayscale range [0, 255]
			int max_dist = (W / 2) * H; // Maximum possible distance (at corners)
			uint8_t gray = (uint8_t)((dist * 255) / max_dist);
			if (gray > 255) {
				gray = 255;
			}

			// Set the pixel to the computed grayscale value
			Color color = COLOR_RGB(gray, gray, gray);
			canvas_set_pixel(&canvas, x, y, color);
		}
	}

	shell_print(sh, "Pyramid-style grayscale gradient drawn (center black, corners white)");
}

static int cmd_display_draw(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!panel) {
		shell_error(sh, "No display device found");
		return -ENODEV;
	}

	color_index = (color_index - 1 + MODE_COUNT) % MODE_COUNT;
	if (color_index < COLOR_COUNT) {
		update_display_color();
		shell_print(sh, "Color changed to index %d", color_index);
	} else {
		draw_grayscale_pyramid(sh);
	}

	return err;
}

static int cmd_get_device(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!panel) {
		shell_error(sh, "No display device found");
		return -ENODEV;
	}

	shell_print(sh, "Display Device: %s", panel->name);

	return err;
}

static int cmd_display_fill(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;
	uint32_t color = 0;
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (!panel) {
		shell_error(sh, "No display device found");
		return -ENODEV;
	}

	color = strtol(argv[1], NULL, 16);

	shell_print(sh, "Fill Color %06X", color);
	canvas_clear(&canvas, COLOR_HEX(color));

	return err;
}

static int cmd_display_brightness(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;
	uint8_t brightness = 0;
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (!panel) {
		shell_error(sh, "No display device found");
		return -ENODEV;
	}
	brightness = strtol(argv[1], NULL, 10);
	// shell_print(sh, "Brightness: %d", brightness);
	display_set_brightness(panel, brightness);
	return err;
}

static int cmd_display_init(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	panel = DEVICE_DT_GET(DT_CHOSEN(zephyr_panel));

	if (!device_is_ready(panel)) {
		shell_error(sh, "Panel device %s not ready", panel->name);
		return -1;
	}

	dsi = DEVICE_DT_GET(DT_ALIAS(mipi_dsi));
	if (!device_is_ready(dsi)) {
		shell_error(sh, "DSI device %s not ready", dsi->name);
		return -1;
	}

	display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(display)) {
		shell_error(sh, "Display device %s not ready", display->name);
		return -1;
	}

#ifdef CONFIG_PM_DEVICE
	pm_device_action_run(panel, PM_DEVICE_ACTION_RESUME);
#endif

	if (dsi_dw_set_mode(dsi, DSI_DW_VIDEO_MODE)) {
		shell_error(sh, "DSI Host controller set to video mode.");
		return -1;
	}

	cdc200_set_enable(display, true);
	struct cdc200_fb_desc layer;
	cdc200_get_framebuffer(display, 0, &layer);
	canvas_init(&canvas, (uint8_t (*)[240][3])layer.fb_addr);
	canvas_set_font(&canvas, &Dogica8px, 1);

	err = display_blanking_off(panel);

	return err;
}

static int cmd_display_on(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (!panel) {
		shell_error(sh, "No display device found");
		return -ENODEV;
	}

	err = display_blanking_off(panel);

	return err;
}

static int cmd_display_off(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (!panel) {
		shell_error(sh, "No display device found");
		return -ENODEV;
	}

	err = display_blanking_on(panel);

	return err;
}

static int cmd_display_deinit(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (!panel) {
		shell_error(sh, "No display device found");
		return -ENODEV;
	}

	err = display_blanking_on(panel);

	cdc200_set_enable(display, false);

#ifdef CONFIG_PM_DEVICE
	pm_device_action_run(panel, PM_DEVICE_ACTION_SUSPEND);
#endif

	return err;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_display,
	SHELL_CMD_ARG(get_device, NULL, "get the display device", cmd_get_device, 1, 0),
	SHELL_CMD_ARG(init, NULL, "initialize the display device", cmd_display_init, 1, 0),
	SHELL_CMD_ARG(deinit, NULL, "deinitialize the display device", cmd_display_deinit, 1, 0),
	SHELL_CMD_ARG(fill, NULL, "fill the display with a color", cmd_display_fill, 2, 0),
	SHELL_CMD_ARG(brightness, NULL, "set the brightness of the display device",
		      cmd_display_brightness, 2, 0),
	SHELL_CMD_ARG(draw, NULL, "draw and switch color", cmd_display_draw, 1, 0),
	SHELL_CMD_ARG(on, NULL, "turn on the display", cmd_display_on, 1, 0),
	SHELL_CMD_ARG(off, NULL, "turn off the display", cmd_display_off, 1, 0),

	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(display, &sub_display, "Display device commands", NULL);

#endif
/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <math.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/display.h>
#include <zephyr/pm/device.h>

#include "lua.h"
#include "lauxlib.h"

#include <halo/lua_display.h>
#include <halo/lua_service.h>
#include <halo/lua_runtime.h>
#include <halo/pm_manager.h>
#include <halo/file_manager.h>
#if defined(CONFIG_HALO_AUDIO_STREAM)
#include <halo/audio_stream.h>
#endif

LOG_MODULE_REGISTER(lua_display, CONFIG_HALO_LOG_LEVEL);

#ifdef CONFIG_MIPI_DSI
#include <zephyr/drivers/mipi_dsi/dsi_dw.h>
#endif

#include <canvas.h>

#ifdef CONFIG_HALO_BOOT_LOGO
#include <lz4.h>
#include <halo/boot_logo.h>
#endif

#ifdef CONFIG_CDC200
#include <zephyr/drivers/display/cdc200.h>
#include <drivers/display_vga020.h>

/**
 * @brief Font entry structure for font management
 *
 * Each face is stored only at its 8px native size; requested sizes that are
 * multiples of 8 are produced by the canvas integer-scale path, which is
 * lossless for a pixel font.
 */
typedef struct {
	const char *name;
	const GFXfont *font; /* 8px native rasterization */
} FontEntry;

/**
 * @brief Available fonts list
 */
static const FontEntry font_list[] = {
	{"Dogica", &Dogica8px},
	{"DogicaBold", &DogicaBold8px},
};
#endif /* CONFIG_CDC200 */

/**
 * @brief RGB color structure
 */
typedef struct {
	uint8_t r;
	uint8_t g;
	uint8_t b;
} rgb_color_t;

/**
 * @brief YCbCr color structure (compressed 4:3:3 format)
 */
typedef struct {
	uint8_t y;
	uint8_t cb;
	uint8_t cr;
} ycbcr_color_t;

/* Forward declarations */
static int display_service_event_handler(halo_lua_event_t event, void *user_data);

/* Register service with lifecycle management (power-managed service) */
HALO_LUA_SERVICE_DEFINE(display_service, display_service_event_handler, NULL, false);

/**
 * @brief Display state management
 *
 * Centralizes all display-related state for lifecycle management
 */
static struct {
	const struct device *dsi;
	const struct device *panel;
	const struct device *display;
#ifdef CONFIG_CDC200
	Canvas canvas;
#endif
	bool show;
	bool initialized;
	/* True while the panel is actually scanning/emitting. Unlike
	 * display_service.is_active this survives Lua VM restarts (break/
	 * reset signals from a connected host, light-sleep wake), so the
	 * INIT handler can re-sync service state instead of assuming the
	 * display is off. */
	bool hw_active;
	/* Stored as RGB888: the canvas framebuffer is full RGB, so keeping
	 * the palette in compressed YCbCr 4:3:3 only forced a lossy
	 * round-trip on every entry (3-bit chroma cannot even represent
	 * neutral, which tinted GREY/WHITE pink and GREEN olive). The
	 * YCbCr Lua API converts once at assignment instead. */
	rgb_color_t global_palette[16];
} display_state = {.dsi = NULL,
		   .panel = NULL,
		   .display = NULL,
		   .show = false,
		   .initialized = false,
		   .hw_active = false,
		   .global_palette = {
			   {0, 0, 0},       // VOID
			   {255, 255, 255}, // WHITE
			   {128, 128, 128}, // GREY
			   {255, 0, 0},     // RED
			   {255, 192, 203}, // PINK
			   {101, 67, 33},   // DARKBROWN
			   {150, 75, 0},    // BROWN
			   {255, 165, 0},   // ORANGE
			   {255, 255, 0},   // YELLOW
			   {0, 100, 0},     // DARKGREEN
			   {0, 255, 0},     // GREEN
			   {144, 238, 144}, // LIGHTGREEN
			   {25, 25, 112},   // NIGHTBLUE
			   {0, 0, 205},     // SEABLUE
			   {135, 206, 235}, // SKYBLUE
			   {240, 248, 255}  // CLOUDBLUE
		   }};

/* ============================================================================
 * Color Conversion Helper Functions (Optimized)
 * ============================================================================ */

/**
 * @brief Clamp value to 0-255 range
 */
static inline uint8_t clamp_u8(int value)
{
	if (value < 0) {
		return 0;
	}
	if (value > 255) {
		return 255;
	}
	return (uint8_t)value;
}

/**
 * @brief Convert YCbCr (compressed 4:3:3 format) to RGB888
 *
 * Optimized version using integer arithmetic
 */
static rgb_color_t ycbcr_to_rgb888_fast(uint8_t y, uint8_t cb, uint8_t cr)
{
	/* Decompress from 4:3:3 to 8-bit */
	int y_scaled = (y * 219 / 15) + 16;
	int cb_scaled = (cb * 224 / 7) + 16;
	int cr_scaled = (cr * 224 / 7) + 16;

	int cr_offset = cr_scaled - 128;
	int cb_offset = cb_scaled - 128;

	/* ITU-R BT.601 inverse conversion with fixed-point arithmetic */
	int r_tmp = y_scaled + ((91881 * cr_offset) >> 16);
	int g_tmp = y_scaled - ((22554 * cb_offset + 46788 * cr_offset) >> 16);
	int b_tmp = y_scaled + ((116130 * cb_offset) >> 16);

	rgb_color_t rgb;
	rgb.r = clamp_u8(r_tmp);
	rgb.g = clamp_u8(g_tmp);
	rgb.b = clamp_u8(b_tmp);

	return rgb;
}

/* ============================================================================
 * Hardware Initialization and Power Management
 * ============================================================================ */

/**
 * @brief Suspend display (power save mode)
 */
static int display_suspend_handler(void)
{
	int ret;

	/* Check if display hardware is initialized */
	if (!display_state.initialized) {
		return 1; /* Return 1 to indicate skip (no resume needed) */
	}

	/* Note: Framework only calls this if is_active=true, no need to check again */

	ret = display_blanking_on(display_state.panel);
	if (ret != 0) {
		LOG_ERR("Failed to turn on panel blanking: %d", ret);
		return -1;
	}

#ifdef CONFIG_CDC200
	cdc200_set_enable(display_state.display, false);
#endif

#ifdef CONFIG_PM_DEVICE
	if (display_state.panel) {
		enum pm_device_state pm_state;
		ret = pm_device_state_get(display_state.panel, &pm_state);
		if (ret == 0 && pm_state == PM_DEVICE_STATE_ACTIVE) {
			ret = pm_device_action_run(display_state.panel, PM_DEVICE_ACTION_SUSPEND);
			if (ret != 0) {
				LOG_ERR("Failed to suspend panel: %d", ret);
				return -1;
			}
		}
	}
#endif /* CONFIG_PM_DEVICE */

	/* Note: is_active state managed by service framework */
	display_state.hw_active = false;

#if defined(CONFIG_HALO_AUDIO_STREAM)
	/* Display load off the battery IC: restore the audio budget */
	audio_speaker_notify_display_active(false);
#endif

	return 0; /* Return 0 to indicate successful suspend (needs resume) */
}

/**
 * @brief Resume display (exit power save mode)
 */
static int display_resume_handler(void)
{
	int ret;

	/* Note: Framework only calls this if needs_resume=true */

#if defined(CONFIG_HALO_AUDIO_STREAM)
	/* Duck the audio budget BEFORE the panel starts drawing: the resume
	 * sequence pulls its load immediately, and full-budget audio plus
	 * display inrush stacked on the battery-protection IC is the exact
	 * trip scenario the budget drop exists to prevent (bench: the
	 * post-resume ordering spiked ~+24 mA for the whole resume). The
	 * ducking ramp (~15 ms) runs while the resume proceeds. Suspend
	 * keeps the reverse order - restore only after the load is gone.
	 */
	audio_speaker_notify_display_active(true);
#endif

#ifdef CONFIG_PM_DEVICE
	if (display_state.panel) {
		enum pm_device_state pm_state;
		ret = pm_device_state_get(display_state.panel, &pm_state);
		if (ret == 0 && pm_state != PM_DEVICE_STATE_ACTIVE) {
			ret = pm_device_action_run(display_state.panel, PM_DEVICE_ACTION_RESUME);
			if (ret != 0) {
				LOG_ERR("Failed to resume panel: %d", ret);
#if defined(CONFIG_HALO_AUDIO_STREAM)
				/* display did not come up: restore the budget */
				audio_speaker_notify_display_active(false);
#endif
				return ret;
			}
		}
	}
#endif /* CONFIG_PM_DEVICE */

#ifdef CONFIG_MIPI_DSI
	if (display_state.dsi) {
		ret = dsi_dw_set_mode(display_state.dsi, DSI_DW_VIDEO_MODE);
		if (ret != 0) {
			LOG_ERR("Failed to set DSI to video mode: %d", ret);
#if defined(CONFIG_HALO_AUDIO_STREAM)
			audio_speaker_notify_display_active(false);
#endif
			return ret;
		}
	}
#endif

#ifdef CONFIG_CDC200
	cdc200_set_enable(display_state.display, true);
#endif

	/* Restore brightness setting after resume */
	int saved_brightness = 50; /* default */
	if (halo_settings_get("display/brightness", &saved_brightness, sizeof(saved_brightness)) ==
	    0) {
		display_set_brightness(display_state.panel, saved_brightness);
	} else {
		/* Set default brightness if not saved */
		display_set_brightness(display_state.panel, saved_brightness);
	}

	/* Restore pan settings after resume */
	int8_t pan_x = 0; /* default: centered */
	int8_t pan_y = 0;

	halo_settings_get("display/pan_x", &pan_x, sizeof(pan_x));
	halo_settings_get("display/pan_y", &pan_y, sizeof(pan_y));

	if (pan_x != 0 || pan_y != 0) {
		vga020_set_pan(display_state.panel, pan_x, pan_y);
		LOG_DBG("Restored pan settings: x=%d, y=%d", pan_x, pan_y);
	}

	ret = display_blanking_off(display_state.panel);
	if (ret != 0) {
		LOG_ERR("Failed to turn off panel blanking: %d", ret);
		return ret;
	}

	/* Note: is_active state managed by service framework */
	display_state.hw_active = true;

	return 0; /* Always return success - errors are logged */
}

/**
 * @brief Check display hardware availability
 *
 * This only verifies that all required devices are present and ready.
 * It does NOT initialize hardware - that happens on first power_save(false) call.
 */
static int display_hardware_init(void)
{
	if (display_state.initialized) {
		return 0;
	}

	/* Check panel device */
	display_state.panel = DEVICE_DT_GET(DT_CHOSEN(zephyr_panel));
	if (!device_is_ready(display_state.panel)) {
		LOG_ERR("Panel device not ready");
		return -ENODEV;
	}

#ifdef CONFIG_MIPI_DSI
	/* Check DSI device */
	display_state.dsi = DEVICE_DT_GET(DT_ALIAS(mipi_dsi));
	if (!device_is_ready(display_state.dsi)) {
		LOG_ERR("DSI device not ready");
		return -ENODEV;
	}
#endif

#ifdef CONFIG_CDC200
	/* Check CDC200 device */
	display_state.display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(display_state.display)) {
		LOG_ERR("Display device not ready");
		return -ENODEV;
	}

	struct cdc200_fb_desc layer;
	cdc200_get_framebuffer(display_state.display, 0, &layer);
	canvas_init(&display_state.canvas, (uint8_t (*)[PHYS_WIDTH][3])layer.fb_addr);
	canvas_set_font(&display_state.canvas, &Dogica8px, 1);
	canvas_clear(&display_state.canvas, 0x0);
#endif
	display_state.initialized = true;

	return 0;
}

#if defined(CONFIG_HALO_BOOT_LOGO) && defined(CONFIG_CDC200)
/* ============================================================================
 * Boot Logo Splash
 *
 * Drawn during early boot, before the Lua runtime starts, so main.lua cannot
 * clobber it. The display boots in power-save: show() resumes the panel and
 * hide() returns it to power-save afterwards.
 * ============================================================================ */

/* Scratch buffer for the decompressed 4-bit packed indices (2 px/byte). */
static uint8_t boot_logo_pixels[CONFIG_HALO_BOOT_LOGO_MAX_RAW_BYTES];

/* Sum of glyph advances, for centring text without drawing it. */
static int boot_logo_text_width(const GFXfont *font, int scale, const char *text)
{
	int width = 0;

	for (const char *p = text; *p; p++) {
		uint8_t c = (uint8_t)*p;

		if (c < font->first || c > font->last) {
			continue;
		}
		width += font->glyph[c - font->first].xAdvance * scale;
	}

	return width;
}

static void boot_logo_draw(const char *label)
{
	const int width = halo_boot_logo_width;
	const int height = halo_boot_logo_height;
	const uint32_t raw_size = halo_boot_logo_raw_size;

	if (raw_size > sizeof(boot_logo_pixels)) {
		LOG_ERR("Boot logo too large: %u > %zu B (raise CONFIG_HALO_BOOT_LOGO_MAX_RAW_BYTES)",
			raw_size, sizeof(boot_logo_pixels));
		return;
	}

	int decoded = LZ4_decompress_safe((const char *)halo_boot_logo_lz4,
					  (char *)boot_logo_pixels,
					  (int)halo_boot_logo_lz4_size,
					  (int)sizeof(boot_logo_pixels));
	if (decoded != (int)raw_size) {
		LOG_ERR("Boot logo LZ4 decompress failed: %d (expected %u)", decoded, raw_size);
		return;
	}

	/* Pre-convert palette. Mirror the custom-palette path in
	 * lua_display_bitmap so colours match the Lua bitmap API. */
	Color palette_rgb[16];
	for (int i = 0; i < 16; i++) {
		uint8_t r = halo_boot_logo_palette[i][0];
		uint8_t g = halo_boot_logo_palette[i][1];
		uint8_t b = halo_boot_logo_palette[i][2];
		palette_rgb[i] = COLOR_RGB(r, g, b);
	}

	/* Centre on the logical display. */
	int x0 = (LOG_WIDTH - width) / 2;
	int y0 = (LOG_HEIGHT - height) / 2;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;

	canvas_clear(&display_state.canvas, COLOR_RGB(0, 0, 0));

	/* 4-bit indices, 2 px/byte, high nibble first; index 0 is transparent. */
	for (int row = 0; row < height; row++) {
		for (int col = 0; col < width; col++) {
			size_t pixel_index = (size_t)row * width + col;
			uint8_t byte_val = boot_logo_pixels[pixel_index / 2];
			uint8_t color_index =
				(pixel_index & 1) ? (byte_val & 0x0F) : (byte_val >> 4);
			if (color_index == 0) {
				continue;
			}
			canvas_set_pixel(&display_state.canvas, x0 + col, y0 + row,
					 palette_rgb[color_index]);
		}
	}

	if (label && label[0] != '\0') {
		/* Restore the canvas font afterwards so the app's default is
		 * untouched. The label is drawn at 16px (scale 2) to keep it
		 * prominent on the splash screen. */
		const GFXfont *saved_font = display_state.canvas.font;
		uint8_t saved_size = display_state.canvas.font_size;
		const GFXfont *label_font = &Dogica8px;
		const int label_scale = 2;

		canvas_set_font(&display_state.canvas, label_font, label_scale);

		int text_w = boot_logo_text_width(label_font, label_scale, label);
		int text_x = (LOG_WIDTH - text_w) / 2;
		int text_y = y0 + height + 12;

		if (text_x < 0) text_x = 0;
		canvas_draw_string(&display_state.canvas, label, text_x, text_y,
				   COLOR_RGB(255, 255, 255));

		canvas_set_font(&display_state.canvas, saved_font, saved_size);
	}
}

int halo_display_boot_logo_show(const char *label)
{
	int ret = display_hardware_init();
	if (ret < 0) {
		LOG_ERR("Boot logo: display init failed: %d", ret);
		return ret;
	}

	ret = display_resume_handler();
	if (ret < 0) {
		LOG_ERR("Boot logo: display resume failed: %d", ret);
		return ret;
	}

	boot_logo_draw(label);
	return 0;
}

int halo_display_boot_logo_hide(void)
{
	int ret = display_suspend_handler();
	/* suspend_handler returns 1 when the display was never initialised. */
	return (ret < 0) ? ret : 0;
}
#endif /* CONFIG_HALO_BOOT_LOGO && CONFIG_CDC200 */

/* ============================================================================
 * Lua API Functions - Color Palette Management
 * ============================================================================ */

/**
 * @brief Lua: frame.display.assign_color_ycbcr(index, y, cb, cr)
 *
 * Assign a color to global palette using YCbCr values directly
 */
static int lua_display_assign_color_ycbcr(lua_State *L)
{
	static const char *color_names[] = {"VOID",      "WHITE",     "GREY",    "RED",
					    "PINK",      "DARKBROWN", "BROWN",   "ORANGE",
					    "YELLOW",    "DARKGREEN", "GREEN",   "LIGHTGREEN",
					    "NIGHTBLUE", "SEABLUE",   "SKYBLUE", "CLOUDBLUE"};

	lua_Integer color_index;

	if (lua_isinteger(L, 1)) {
		color_index = lua_tointeger(L, 1);
	} else if (lua_isstring(L, 1)) {
		const char *name = lua_tostring(L, 1);
		color_index = -1;
		for (int i = 0; i < ARRAY_SIZE(color_names); i++) {
			if (strcasecmp(name, color_names[i]) == 0) {
				color_index = i; // 0-based index
				break;
			}
		}
		if (color_index == -1) {
			return luaL_error(L, "invalid color name: %s", name);
		}
	} else {
		return luaL_error(L, "color_index must be integer (0-15) or string color name");
	}

	lua_Integer y = luaL_checkinteger(L, 2);
	lua_Integer cb = luaL_checkinteger(L, 3);
	lua_Integer cr = luaL_checkinteger(L, 4);

	if (color_index < 0 || color_index > 15) {
		return luaL_error(L, "color_index must be between 0 and 15");
	}

	if (y < 0 || y > 15) {
		return luaL_error(L, "Y value must be between 0 and 15 (4-bit)");
	}

	if (cb < 0 || cb > 7) {
		return luaL_error(L, "Cb value must be between 0 and 7 (3-bit)");
	}

	if (cr < 0 || cr > 7) {
		return luaL_error(L, "Cr value must be between 0 and 7 (3-bit)");
	}

	rgb_color_t rgb = ycbcr_to_rgb888_fast(y, cb, cr);

	display_state.global_palette[color_index].r = rgb.r;
	display_state.global_palette[color_index].g = rgb.g;
	display_state.global_palette[color_index].b = rgb.b;

	return 0;
}

/**
 * @brief Lua: frame.display.assign_color(index, r, g, b)
 *
 * Assign a color to global palette using RGB888 values
 */
static int lua_display_assign_color(lua_State *L)
{
	static const char *color_names[] = {"VOID",      "WHITE",     "GREY",    "RED",
					    "PINK",      "DARKBROWN", "BROWN",   "ORANGE",
					    "YELLOW",    "DARKGREEN", "GREEN",   "LIGHTGREEN",
					    "NIGHTBLUE", "SEABLUE",   "SKYBLUE", "CLOUDBLUE"};

	lua_Integer color_index;

	if (lua_isinteger(L, 1)) {
		color_index = lua_tointeger(L, 1);
	} else if (lua_isstring(L, 1)) {
		const char *name = lua_tostring(L, 1);
		color_index = -1;
		for (int i = 0; i < ARRAY_SIZE(color_names); i++) {
			if (strcasecmp(name, color_names[i]) == 0) {
				color_index = i; // 0-based index
				break;
			}
		}
		if (color_index == -1) {
			return luaL_error(L, "invalid color name: %s", name);
		}
	} else {
		return luaL_error(L, "color_index must be integer (0-15) or string color name");
	}

	lua_Integer r = luaL_checkinteger(L, 2);
	lua_Integer g = luaL_checkinteger(L, 3);
	lua_Integer b = luaL_checkinteger(L, 4);

	if (color_index < 0 || color_index > 15) {
		return luaL_error(L, "color_index must be between 0 and 15");
	}

	if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
		return luaL_error(L, "RGB values must be between 0 and 255");
	}

	display_state.global_palette[color_index].r = r;
	display_state.global_palette[color_index].g = g;
	display_state.global_palette[color_index].b = b;

	return 0;
}

/* ============================================================================
 * Lua API Functions - Bitmap Rendering (Optimized)
 * ============================================================================ */

/**
 * @brief Display RGB888 bitmap (direct rendering)
 */
static int rgb_display_bitmap(lua_Integer x, lua_Integer y, lua_Integer width, lua_Integer height,
			      const char *pixel_data, size_t data_len)
{
#ifdef CONFIG_CDC200
	if (!pixel_data) {
		return -1;
	}

	canvas_draw_bitmap(&display_state.canvas, x, y, width, height, (const uint8_t *)pixel_data);
#endif
	return 0;
}

/**
 * @brief Lua: frame.display.bitmap(x, y, width, color_format, palette_offset, data, options)
 *
 * Display indexed-color bitmap with optional scaling and custom palette
 *
 * Optimized for performance with pre-computed palette conversion
 */
static int lua_display_bitmap(lua_State *L)
{
#ifdef CONFIG_CDC200
	lua_Integer x = luaL_checkinteger(L, 1);
	lua_Integer y = luaL_checkinteger(L, 2);
	lua_Integer width = luaL_checkinteger(L, 3);

	if (x < 1) x = 1;
	if (y < 1) y = 1;
	x -= 1;
	y -= 1;
	lua_Integer color_format = luaL_checkinteger(L, 4);
	lua_Integer palette_offset = luaL_optinteger(L, 5, 0);

	size_t data_len;
	const char *pixel_data = luaL_checklstring(L, 6, &data_len);

	/* Parse options */
	lua_Integer x_scale = 1;
	lua_Integer y_scale = 1;
	const char *palette_data_str = NULL;
	size_t custom_palette_len = 0;

	if (lua_gettop(L) >= 7 && lua_istable(L, 7)) {
		lua_getfield(L, 7, "x_scale");
		if (lua_isnumber(L, -1)) {
			x_scale = lua_tointeger(L, -1);
		}
		lua_pop(L, 1);

		lua_getfield(L, 7, "y_scale");
		if (lua_isnumber(L, -1)) {
			y_scale = lua_tointeger(L, -1);
		}
		lua_pop(L, 1);

		lua_getfield(L, 7, "palette_data");
		if (lua_isstring(L, -1)) {
			palette_data_str = lua_tolstring(L, -1, &custom_palette_len);
		}
		lua_pop(L, 1);
	}

	if (x_scale < 1 || y_scale < 1) {
		return luaL_error(L, "scale factors must be positive integers");
	}

	if (!pixel_data) {
		return luaL_error(L, "pixel data pointer is null");
	}

	if (color_format != 0 && color_format != 2 && color_format != 4 && color_format != 16) {
		return luaL_error(L, "unsupported color format: %I. Must be 0, 2, 4, or 16",
				  color_format);
	}

	if (palette_offset < 0 || palette_offset > 15) {
		return luaL_error(L, "palette_offset must be between 0 and 15");
	}

	/* Handle RGB888 direct rendering */
	if (color_format == 0) {
		return rgb_display_bitmap(x, y, width, data_len / (width * 3), pixel_data,
					  data_len);
	}

	/* Prepare palette (OPTIMIZATION: pre-convert all colors) */
	Color palette_rgb[16];
	bool use_custom_palette = false;

	if (palette_data_str != NULL && custom_palette_len >= 3 && custom_palette_len % 3 == 0) {
		/* Use custom RGB palette */
		int num_colors = custom_palette_len / 3;
		num_colors = (num_colors > 16) ? 16 : num_colors;

		for (int i = 0; i < num_colors; i++) {
			uint8_t r = palette_data_str[i * 3];
			uint8_t g = palette_data_str[i * 3 + 1];
			uint8_t b = palette_data_str[i * 3 + 2];
			palette_rgb[i] = COLOR_RGB(r, g, b);
		}
		if (num_colors < 16) {
			memset(&palette_rgb[num_colors], 0, (16 - num_colors) * sizeof(Color));
		}
		use_custom_palette = true;
	} else {
		/* Global palette is stored as RGB: pack it directly. */
		for (int i = 0; i < 16; i++) {
			palette_rgb[i] = COLOR_RGB(display_state.global_palette[i].r,
						   display_state.global_palette[i].g,
						   display_state.global_palette[i].b);
		}
		use_custom_palette = false;
	}

	/* Calculate bits per pixel and pixels per byte */
	int bits_per_pixel;
	int pixels_per_byte;

	switch (color_format) {
	case 2: // 1-bit
		bits_per_pixel = 1;
		pixels_per_byte = 8;
		break;
	case 4: // 2-bit
		bits_per_pixel = 2;
		pixels_per_byte = 4;
		break;
	case 16: // 4-bit
		bits_per_pixel = 4;
		pixels_per_byte = 2;
		break;
	default:
		return luaL_error(L, "unsupported color_format: %I", color_format);
	}

	lua_Integer height = (data_len * pixels_per_byte) / width;
	if (height <= 0) {
		return luaL_error(L, "invalid data length for given width and color format");
	}

	/* Render bitmap (OPTIMIZED: single lookup per pixel) */
	for (lua_Integer i = 0; i < height; i++) {
		for (lua_Integer j = 0; j < width; j++) {
			size_t pixel_index = i * width + j;
			size_t byte_index = pixel_index / pixels_per_byte;
			size_t bit_offset;

			switch (bits_per_pixel) {
			case 1: // 1-bit
				bit_offset = 7 - (pixel_index % pixels_per_byte);
				break;
			case 2: // 2-bit
				bit_offset = 6 - ((pixel_index % pixels_per_byte) * 2);
				break;
			case 4: // 4-bit
				bit_offset = (pixel_index % pixels_per_byte) ? 0 : 4;
				break;
			default:
				bit_offset = 0;
			}

			uint8_t color_index;
			if (byte_index >= data_len) {
				color_index = 0;
			} else {
				uint8_t byte_val = pixel_data[byte_index];
				color_index =
					(byte_val >> bit_offset) & ((1 << bits_per_pixel) - 1);

				/* Skip transparent pixels (index 0) */
				if (color_index == 0) {
					continue;
				}

				/* palette_offset shifts linearly and never wraps:
				 * an index pushed past the palette is skipped, so
				 * an offset can neither reach the VOID entry nor
				 * alias back to a low colour. */
				color_index += palette_offset;
				if (color_index > 15) {
					continue;
				}
			}

			/* Get color from pre-converted palette */
			Color pixel_color = palette_rgb[color_index];

			/* Apply scaling */
			for (int xs = 0; xs < x_scale; xs++) {
				for (int ys = 0; ys < y_scale; ys++) {
					lua_Integer dest_x = x + j * x_scale + xs;
					lua_Integer dest_y = y + i * y_scale + ys;
					canvas_set_pixel(&display_state.canvas, dest_x, dest_y,
							 pixel_color);
				}
			}
		}
	}

#endif

	return 0;
}

/* ============================================================================
 * Lua API Functions - Text and Basic Drawing
 * ============================================================================ */

/**
 * @brief Lua: frame.display.text(text, x, y, color)
 *
 * Display text at specified position with color
 */
static int lua_display_text(lua_State *L)
{
#ifdef CONFIG_CDC200
	const char *text = luaL_checkstring(L, 1);
	lua_Integer x = luaL_checkinteger(L, 2);
	lua_Integer y = luaL_checkinteger(L, 3);
	lua_Integer color = luaL_optinteger(L, 4, 0xFFFFFF);

	if (x < 1) x = 1;
	if (y < 1) y = 1;
	x -= 1;
	y -= 1;

	uint8_t r = (color >> 16) & 0xFF;
	uint8_t g = (color >> 8) & 0xFF;
	uint8_t b = color & 0xFF;

	Color c = COLOR_RGB(r, g, b);

	canvas_draw_string(&display_state.canvas, text, x, y, c);
#endif
	return 0;
}

/**
 * @brief Lua: frame.display.clear(color)
 *
 * Clear entire display with specified color
 */
static int lua_display_clear(lua_State *L)
{
#ifdef CONFIG_CDC200
	lua_Integer color = luaL_optinteger(L, 1, 0x000000);

	uint8_t r = (color >> 16) & 0xFF;
	uint8_t g = (color >> 8) & 0xFF;
	uint8_t b = color & 0xFF;

	Color c = COLOR_RGB(r, g, b);

	LOG_DBG("Clearing display with color 0x%06x", (unsigned int)color);
	canvas_clear(&display_state.canvas, c);
#endif
	return 0;
}

/**
 * @brief Lua: frame.display.show(enable)
 *
 * Enable/disable display output using blanking control
 */
static int lua_display_show(lua_State *L)
{
	// Deliberately a no-op (decision predates the public repo); registered
	// so existing apps that call it keep working.
	return 0;
}

/**
 * @brief Lua: frame.display.set_brightness(level)
 *
 * Set display brightness level
 * @param level Brightness level (-2 to 2)
 */
static int lua_display_set_brightness(lua_State *L)
{
	if (!display_state.initialized) {
		return luaL_error(L, "Display not initialized");
	}

	lua_Integer level = luaL_checkinteger(L, 1);

	if (level < -2 || level > 2) {
		return luaL_error(L, "brightness level must be between -2 and 2");
	}

	/* Map level (-2 to 2) to brightness (0-100) */
	int brightness;
	switch (level) {
	case -2:
		brightness = 10;
		break;
	case -1:
		brightness = 25;
		break;
	case 0:
		brightness = 50;
		break;
	case 1:
		brightness = 75;
		break;
	case 2:
		brightness = 100;
		break;
	default:
		brightness = 50; /* fallback */
		break;
	}

	/* Save brightness setting persistently */
	halo_settings_set("display/brightness", &brightness, sizeof(brightness));

	return 0;
}

/**
 * @brief Lua: frame.display.get_brightness()
 *
 * Get current display brightness level (-2 to 2)
 * @return Current brightness level
 */
static int lua_display_get_brightness(lua_State *L)
{
	int brightness = 50; /* default */

	/* Try to load from settings */
	if (halo_settings_get("display/brightness", &brightness, sizeof(brightness)) != 0) {
		/* If not found, use default */
		brightness = 50;
	}

	/* Map brightness (0-100) back to level (-2 to 2) */
	int level;
	switch (brightness) {
	case 10:
		level = -2;
		break;
	case 25:
		level = -1;
		break;
	case 50:
		level = 0;
		break;
	case 75:
		level = 1;
		break;
	case 100:
		level = 2;
		break;
	default:
		level = 0; /* fallback */
		break;
	}

	lua_pushinteger(L, level);
	return 1;
}

/**
 * @brief Lua: frame.display.brightness([level])
 *
 * Get or set display brightness level (0-100)
 * @param level Optional brightness level 0-100. If not provided, returns current brightness.
 * @return If getting: current brightness level. If setting: nothing.
 */
static int lua_display_brightness(lua_State *L)
{
	/* Check if argument provided (setter) or not (getter) */
	if (lua_gettop(L) == 0) {
		/* Getter: return current brightness */
		int brightness = 50; /* default */

		/* Try to load from settings */
		if (halo_settings_get("display/brightness", &brightness, sizeof(brightness)) != 0) {
			/* If not found, use default */
			brightness = 50;
		}

		lua_pushinteger(L, brightness);
		return 1;
	} else {
		/* Setter: set brightness */
		if (!display_state.initialized) {
			return luaL_error(L, "Display not initialized");
		}

		int brightness = luaL_checkinteger(L, 1);

		if (brightness < 0 || brightness > 100) {
			return luaL_error(L, "Brightness must be 0-100");
		}

		display_set_brightness(display_state.panel, brightness);

		/* Save brightness setting persistently */
		halo_settings_set("display/brightness", &brightness, sizeof(brightness));

		return 0;
	}
}

/**
 * @brief Lua: frame.display.power_save([enable])
 *
 * Enable/disable power save mode. With no argument, returns the current
 * power save state (true = display suspended).
 */
static int lua_display_power_save(lua_State *L)
{
	if (!display_state.initialized) {
		return luaL_error(L, "Display not initialized");
	}

	if (lua_gettop(L) == 0) {
		lua_pushboolean(L, !display_service.is_active);
		return 1;
	}

	if (!lua_isboolean(L, 1)) {
		return luaL_error(L, "argument must be boolean");
	}

	bool enable = lua_toboolean(L, 1);

	/* Check if already in desired state */
	bool is_sleeping = !display_service.is_active;
	if (is_sleeping == enable) {
		return 0; /* Already in desired state */
	}

	/* Use unified service power save API */
	int ret = halo_lua_service_power_save(&display_service, enable);

	if (ret < 0) {
		return luaL_error(L, "Failed to %s power save mode: %d",
				  enable ? "enter" : "exit", ret);
	}

	return 0;
}

/* ============================================================================
 * Lua API Functions - Geometric Shapes
 * ============================================================================ */

/**
 * @brief Lua: frame.display.set_pixel(x, y, color)
 *
 * Set single pixel color
 */
static int lua_display_set_pixel(lua_State *L)
{
#ifdef CONFIG_CDC200
	int x = luaL_checkinteger(L, 1);
	int y = luaL_checkinteger(L, 2);
	int color = luaL_checkinteger(L, 3);

	if (x < 1) x = 1;
	if (y < 1) y = 1;
	x -= 1;
	y -= 1;

	canvas_set_pixel(&display_state.canvas, x, y,
			 COLOR_RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF));
#endif
	return 0;
}

/**
 * @brief Lua: frame.display.line(x0, y0, x1, y1, color)
 *
 * Draw line between two points
 */
static int lua_display_draw_line(lua_State *L)
{
#ifdef CONFIG_CDC200
	int x0 = luaL_checkinteger(L, 1);
	int y0 = luaL_checkinteger(L, 2);
	int x1 = luaL_checkinteger(L, 3);
	int y1 = luaL_checkinteger(L, 4);
	int color = luaL_checkinteger(L, 5);

	if (x0 < 1) x0 = 1;
	if (y0 < 1) y0 = 1;
	if (x1 < 1) x1 = 1;
	if (y1 < 1) y1 = 1;
	x0 -= 1; y0 -= 1; x1 -= 1; y1 -= 1;

	canvas_draw_line(&display_state.canvas, x0, y0, x1, y1,
			 COLOR_RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF));
#endif
	return 0;
}

/**
 * @brief Lua: frame.display.rect(x, y, w, h, color, filled)
 *
 * Draw rectangle
 */
static int lua_display_draw_rect(lua_State *L)
{
#ifdef CONFIG_CDC200
	int x = luaL_checkinteger(L, 1);
	int y = luaL_checkinteger(L, 2);
	int w = luaL_checkinteger(L, 3);
	int h = luaL_checkinteger(L, 4);

	if (x < 1) x = 1;
	if (y < 1) y = 1;
	x -= 1;
	y -= 1;
	int color = luaL_checkinteger(L, 5);
	bool filled = lua_toboolean(L, 6);

	canvas_draw_rect(&display_state.canvas, x, y, w, h,
			 COLOR_RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF),
			 filled);
#endif
	return 0;
}

/**
 * @brief Lua: frame.display.circle(cx, cy, r, color, filled)
 *
 * Draw circle
 */
static int lua_display_draw_circle(lua_State *L)
{
#ifdef CONFIG_CDC200
	int cx = luaL_checkinteger(L, 1);
	int cy = luaL_checkinteger(L, 2);
	int r = luaL_checkinteger(L, 3);

	if (cx < 1) cx = 1;
	if (cy < 1) cy = 1;
	cx -= 1;
	cy -= 1;
	int color = luaL_checkinteger(L, 4);
	bool filled = lua_toboolean(L, 5);

	canvas_draw_circle(&display_state.canvas, cx, cy, r,
			   COLOR_RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF),
			   filled);
#endif
	return 0;
}

/**
 * @brief Lua: frame.display.polygon(points, color)
 *
 * Draw polygon from array of points
 */
static int lua_display_draw_polygon(lua_State *L)
{
#ifdef CONFIG_CDC200
	luaL_checktype(L, 1, LUA_TTABLE);
	int color = luaL_checkinteger(L, 2);
	int count = lua_rawlen(L, 1);

	if (count > 64) {
		return luaL_error(L, "too many polygon points (max 64)");
	}

	int points[64];
	for (int i = 0; i < count; ++i) {
		lua_rawgeti(L, 1, i + 1);
		int coord = luaL_checkinteger(L, -1);
		lua_pop(L, 1);
		if (coord < 1) coord = 1;
		points[i] = coord - 1;
	}

	canvas_draw_polygon(&display_state.canvas, points, count / 2,
			    COLOR_RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF));
#endif
	return 0;
}

/**
 * @brief Lua: frame.display.char(codepoint, x, y, color)
 *
 * Draw single character
 */
static int lua_display_draw_char(lua_State *L)
{
#ifdef CONFIG_CDC200
	int c = luaL_checkinteger(L, 1);
	int x = luaL_checkinteger(L, 2);
	int y = luaL_checkinteger(L, 3);
	int color = luaL_checkinteger(L, 4);

	if (x < 1) x = 1;
	if (y < 1) y = 1;
	x -= 1;
	y -= 1;

	canvas_draw_char(&display_state.canvas, c, x, y,
			 COLOR_RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF));
#endif
	return 0;
}

/* ============================================================================
 * Lua API Functions - Font Management
 * ============================================================================ */

/**
 * @brief Lua: frame.display.set_font(font_id, size, scale)
 *
 * Set current font for text rendering
 */
static int lua_display_set_font(lua_State *L)
{
#ifdef CONFIG_CDC200
	int font_id = luaL_checkinteger(L, 1);
	int size = luaL_optinteger(L, 2, 8);
	int scale = luaL_optinteger(L, 3, 1);

	if (font_id < 0 || font_id >= ARRAY_SIZE(font_list)) {
		return luaL_error(L, "invalid font id (must be 0-%d)", ARRAY_SIZE(font_list) - 1);
	}

	/* Fonts are stored at 8px; other sizes are integer multiples rendered
	 * via the canvas scale path, compounded with the caller's scale.
	 */
	if (size < 8 || size % 8 != 0) {
		return luaL_error(L, "invalid font size (must be a multiple of 8)");
	}
	if (scale < 1 || (size / 8) * scale > UINT8_MAX) {
		return luaL_error(L, "invalid scale");
	}

	canvas_set_font(&display_state.canvas, font_list[font_id].font,
			(size / 8) * scale);

#endif
	return 0;
}

/**
 * @brief Lua: frame.display.get_font_list()
 *
 * Get table of available fonts
 */
static int lua_display_get_font_list(lua_State *L)
{
#ifdef CONFIG_CDC200
	lua_newtable(L);
	for (int i = 0; i < ARRAY_SIZE(font_list); ++i) {
		lua_pushinteger(L, i);
		lua_pushstring(L, font_list[i].name);
		lua_settable(L, -3);
	}
#else
	lua_newtable(L); /* Return empty table if CDC200 not available */
#endif
	return 1;
}

/**
 * @brief Lua: frame.display.width()
 *
 * Get the logical display width (after rotation)
 * @return width in pixels
 */
static int lua_display_width(lua_State *L)
{
	lua_pushinteger(L, LOG_WIDTH);
	return 1;
}

/**
 * @brief Lua: frame.display.height()
 *
 * Get the logical display height (after rotation)
 * @return height in pixels
 */
static int lua_display_height(lua_State *L)
{
	lua_pushinteger(L, LOG_HEIGHT);
	return 1;
}

/**
 * @brief Lua: frame.display.set_pan(x, y)
 *
 * Set display pan/shift offset to move content within [-50, 50] pixel range
 * - x=0, y=0: centered (default)
 * - x=-50, y=-50: shifted to top-left
 * - x=50, y=50: shifted to bottom-right
 *
 * @param x Horizontal pan offset (-50 to +50)
 * @param y Vertical pan offset (-50 to +50)
 */
static int lua_display_set_pan(lua_State *L)
{
	int8_t x_offset = luaL_checkinteger(L, 1);
	int8_t y_offset = luaL_checkinteger(L, 2);

	if (!display_state.initialized) {
		return luaL_error(L, "Display not initialized");
	}

	if (!display_state.panel) {
		return luaL_error(L, "Panel device not available");
	}

#if CONFIG_CANVAS_ROTATE_90 || CONFIG_CANVAS_ROTATE_270
	// Swap x and y for 90/270 degree rotation
	int8_t temp = x_offset;
	x_offset = y_offset;
	y_offset = temp;
#endif

	/* Save pan settings to NVS first. */
	halo_settings_set("display/pan_x", &x_offset, sizeof(x_offset));
	halo_settings_set("display/pan_y", &y_offset, sizeof(y_offset));

	/* If display is in power save, apply settings on resume. */
	if (!display_service.is_active) {
		return 0;
	}

	int ret = vga020_set_pan(display_state.panel, x_offset, y_offset);
	if (ret != 0) {
		return luaL_error(L, "Failed to set pan: %d", ret);
	}

	return 0;
}

/**
 * @brief Lua: frame.display.get_pan()
 *
 * Get current display pan/shift offset
 *
 * @return x, y Current pan offsets (-50 to +50)
 */
static int lua_display_get_pan(lua_State *L)
{
	int8_t pan_x = 0; /* default: centered */
	int8_t pan_y = 0;

	/* Read from NVS */
	halo_settings_get("display/pan_x", &pan_x, sizeof(pan_x));
	halo_settings_get("display/pan_y", &pan_y, sizeof(pan_y));

#if CONFIG_CANVAS_ROTATE_90 || CONFIG_CANVAS_ROTATE_270
	// Swap x and y for 90/270 degree rotation
	int8_t temp = pan_x;
	pan_x = pan_y;
	pan_y = temp;
#endif
	lua_pushinteger(L, pan_x);
	lua_pushinteger(L, pan_y);
	return 2;
}

/* ============================================================================
 * Power Management Integration
 * ============================================================================ */

/**
 * @brief PM callback handler for display power management
 *
 * Handles suspend/resume events from pm_manager
 */

/**
 * @brief Service lifecycle event handler
 *
 * Handles lifecycle events like INIT, DEINIT, INTERRUPT, RESTART
 */
static int display_service_event_handler(halo_lua_event_t event, void *user_data)
{
	ARG_UNUSED(user_data);

	int ret = 0;

	switch (event) {
	case HALO_LUA_EVENT_INIT:
		/* Sync service state with the actual hardware. The VM restarts
		 * whenever a host sends break/reset (every SDK connect does);
		 * if the previous session left the panel on, assuming "off"
		 * here makes power_save(true) a silent no-op and makes standby
		 * skip the display entirely. */
		display_service.is_active = display_state.hw_active;
		break;

	case HALO_LUA_EVENT_DEINIT:
		break;

	case HALO_LUA_EVENT_INTERRUPT:
		break;

	case HALO_LUA_EVENT_SUSPEND:
		ret = display_suspend_handler();
		break;

	case HALO_LUA_EVENT_RESUME:
		ret = display_resume_handler();
		break;

	default:
		break;
	}

	return ret;
}

/* ============================================================================
 * Library Registration
 * ============================================================================ */

/**
 * @brief Open and register display library with Lua VM
 *
 * Registers all display functions under frame.display table and
 * initializes the display hardware
 */
int lua_open_display_library(lua_State *L)
{
	/* Register service for lifecycle management */
	int ret = halo_lua_service_register(&display_service);
	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("Failed to register display service: %d", ret);
		return ret;
	}

	/* Initialize display hardware */
	ret = display_hardware_init();
	if (ret < 0) {
		LOG_ERR("Failed to initialize display hardware: %d", ret);
		return ret;
	}

	/* Get or create the global 'frame' table */
	lua_getglobal(L, "frame");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_setglobal(L, "frame");
	}

	/* Create 'display' subtable */
	lua_newtable(L);

	/* Register color management functions */
	lua_pushcfunction(L, lua_display_assign_color_ycbcr);
	lua_setfield(L, -2, "assign_color_ycbcr");

	lua_pushcfunction(L, lua_display_assign_color);
	lua_setfield(L, -2, "assign_color");

	/* Register bitmap and text functions */
	lua_pushcfunction(L, lua_display_bitmap);
	lua_setfield(L, -2, "bitmap");

	lua_pushcfunction(L, lua_display_text);
	lua_setfield(L, -2, "text");

	lua_pushcfunction(L, lua_display_clear);
	lua_setfield(L, -2, "clear");

	/* Register display control functions */
	lua_pushcfunction(L, lua_display_show);
	lua_setfield(L, -2, "show");

	lua_pushcfunction(L, lua_display_power_save);
	lua_setfield(L, -2, "power_save");

	lua_pushcfunction(L, lua_display_set_brightness);
	lua_setfield(L, -2, "set_brightness");

	lua_pushcfunction(L, lua_display_get_brightness);
	lua_setfield(L, -2, "get_brightness");

	lua_pushcfunction(L, lua_display_brightness);
	lua_setfield(L, -2, "brightness");

	/* Register geometric shape functions */
	lua_pushcfunction(L, lua_display_set_pixel);
	lua_setfield(L, -2, "set_pixel");

	lua_pushcfunction(L, lua_display_draw_line);
	lua_setfield(L, -2, "line");

	lua_pushcfunction(L, lua_display_draw_rect);
	lua_setfield(L, -2, "rect");

	lua_pushcfunction(L, lua_display_draw_circle);
	lua_setfield(L, -2, "circle");

	lua_pushcfunction(L, lua_display_draw_polygon);
	lua_setfield(L, -2, "polygon");

	lua_pushcfunction(L, lua_display_draw_char);
	lua_setfield(L, -2, "char");

	/* Register font management functions */
	lua_pushcfunction(L, lua_display_set_font);
	lua_setfield(L, -2, "set_font");

	lua_pushcfunction(L, lua_display_get_font_list);
	lua_setfield(L, -2, "get_font_list");

	/* Register display dimension functions */
	lua_pushcfunction(L, lua_display_width);
	lua_setfield(L, -2, "width");

	lua_pushcfunction(L, lua_display_height);
	lua_setfield(L, -2, "height");

	/* Register display pan control */
	lua_pushcfunction(L, lua_display_set_pan);
	lua_setfield(L, -2, "set_pan");

	lua_pushcfunction(L, lua_display_get_pan);
	lua_setfield(L, -2, "get_pan");

	/* Set 'display' table in 'frame' */
	lua_setfield(L, -2, "display");

	/* Pop frame table */
	lua_pop(L, 1);

	LOG_DBG("Display library registered successfully with %d APIs", 21);

	return 0;
}

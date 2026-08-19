#ifndef CANVAS_H
#define CANVAS_H

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/devicetree.h>
#include <Fonts/GFXFF/gfxfont.h>

#ifdef __cplusplus
extern "C" {
#endif


// Get physical dimensions from device tree (zephyr,display)
#define DISPLAY_NODE DT_CHOSEN(zephyr_display)
#define PHYS_WIDTH  DT_PROP(DISPLAY_NODE, width)   // Physical buffer width from DT
#define PHYS_HEIGHT DT_PROP(DISPLAY_NODE, height)  // Physical buffer height from DT

// Logical display dimensions - conditional rotation based on CONFIG_CANVAS_ROTATE_90
#ifdef CONFIG_CANVAS_ROTATE_90
// After 90° rotation: swap width and height
#define LOG_WIDTH  PHYS_HEIGHT  // Logical width = physical height (after 90° rotation)
#define LOG_HEIGHT PHYS_WIDTH   // Logical height = physical width (after 90° rotation)
#else
// No rotation: logical dimensions same as physical
#define LOG_WIDTH  PHYS_WIDTH
#define LOG_HEIGHT PHYS_HEIGHT
#endif

// Color type - stored as 0x00RRGGBB
typedef uint32_t Color;

// Color macros for easy color creation
#define COLOR_RGB(r, g, b) ((Color)(((r) << 16) | ((g) << 8) | (b)))
#define COLOR_HEX(hex)     ((Color)(hex))

// Canvas structure
typedef struct {
	const GFXfont *font;
	uint8_t font_size;
	uint8_t (*buffer)[PHYS_WIDTH][3];
} Canvas;

/**
 * Initialize canvas (doesn't clear memory)
 *
 * @param canvas Pointer to canvas object
 * @param buffer Frame buffer pointer
 */
void canvas_init(Canvas *canvas, uint8_t (*buffer)[PHYS_WIDTH][3]);

/**
 * Clear canvas with specified color
 *
 * @param canvas Canvas object
 * @param color Fill color in 0x00RRGGBB format
 */
void canvas_clear(Canvas *canvas, Color color);

/**
 * Set pixel with built-in 90° rotation (optimized)
 *
 * @param canvas Canvas object
 * @param x X coordinate in logical space
 * @param y Y coordinate in logical space
 * @param color Pixel color in 0x00RRGGBB format
 */
static inline void canvas_set_pixel(Canvas *canvas, int x, int y, Color color)
{
	if (x < 0 || x >= LOG_WIDTH || y < 0 || y >= LOG_HEIGHT) {
		return;
	}

	int phys_x = x;
	int phys_y = LOG_HEIGHT - 1 - y;
	uint8_t *pixel = canvas->buffer[phys_x][phys_y];
	pixel[2] = (color >> 16) & 0xFF; // R
	pixel[1] = (color >> 8) & 0xFF;  // G
	pixel[0] = (color >> 0) & 0xFF;  // B
}

/**
 * Draw optimized line (Bresenham's algorithm with memcpy for horizontal lines)
 *
 * @param canvas Canvas object
 * @param x0 Start X in logical space
 * @param y0 Start Y in logical space
 * @param x1 End X in logical space
 * @param y1 End Y in logical space
 * @param color Line color in 0x00RRGGBB format
 */
void canvas_draw_line(Canvas *canvas, int x0, int y0, int x1, int y1, Color color);

/**
 * Draw rectangle with optimized fill
 *
 * @param canvas Canvas object
 * @param x Top-left X in logical space
 * @param y Top-left Y in logical space
 * @param w Width of rectangle
 * @param h Height of rectangle
 * @param color Fill or outline color in 0x00RRGGBB format
 * @param filled True for filled, false for outline
 */
void canvas_draw_rect(Canvas *canvas, int x, int y, int w, int h, Color color, bool filled);

/**
 * Draw circle (midpoint algorithm with optimizations)
 *
 * @param canvas Canvas object
 * @param cx Center X in logical space
 * @param cy Center Y in logical space
 * @param radius Circle radius
 * @param color Circle color in 0x00RRGGBB format
 * @param filled True for filled, false for outline
 */
void canvas_draw_circle(Canvas *canvas, int cx, int cy, int radius, Color color, bool filled);

/**
 * Draw polygon (optimized for triangles)
 *
 * @param canvas Canvas object
 * @param points Array of points [x0,y0, x1,y1,...]
 * @param count Number of points
 * @param color Polygon color in 0x00RRGGBB format
 */
void canvas_draw_polygon(Canvas *canvas, const int *points, int count, Color color);

/**
 * Draw bitmap (with rotation)
 *
 * @param canvas Canvas object
 * @param x Top-left X in logical space
 * @param y Top-left Y in logical space
 * @param width Bitmap width
 * @param height Bitmap height
 * @param bitmap Pixel array (row-major order)
 */
void canvas_draw_bitmap(Canvas *canvas, int x, int y, int width, int height, const uint8_t *bitmap);

/**
 * Set font for text rendering
 *
 * @param canvas Canvas object
 * @param font Pointer to GFXfont structure
 * @param size Font size
 */
void canvas_set_font(Canvas *canvas, const GFXfont *font, uint8_t size);

/**
 * Draw character (with rotation)
 *
 * @param canvas Canvas object
 * @param c Unicode character code
 * @param x Top-left X in logical space
 * @param y Top-left Y in logical space
 * @param color Character color in 0x00RRGGBB format
 * @return Width of character in pixels
 */
int canvas_draw_char(Canvas *canvas, uint16_t c, int x, int y, Color color);

/**
 * Draw string (with rotation)
 *
 * @param canvas Canvas object
 * @param str Null-terminated string
 * @param x Top-left X in logical space
 * @param y Top-left Y in logical space
 * @param color Character color in 0x00RRGGBB format
 * @return Total width of string in pixels
 */
int canvas_draw_string(Canvas *canvas, const char *str, int x, int y, Color color);

#ifdef __cplusplus
}
#endif

#endif // CANVAS_H
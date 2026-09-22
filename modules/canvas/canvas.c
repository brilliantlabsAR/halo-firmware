#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <zephyr/kernel.h>

#include "canvas.h"

// Dogica pixel font (8px native size; larger sizes via the integer
// scale parameter of canvas_set_font, which pixel-doubles losslessly)
#include <Fonts/dogica/Dogica8px.h>
#include <Fonts/dogica/DogicaBold8px.h>

// Initialize canvas
void canvas_init(Canvas *canvas, uint8_t (*buffer)[PHYS_WIDTH][3])
{
	canvas->font = NULL;
	canvas->font_size = 0;
	canvas->buffer = buffer;
}

// Clear canvas with color (optimized)
void canvas_clear(Canvas *canvas, Color color)
{
	// Extract color components
	uint8_t r = (color >> 16) & 0xFF;
	uint8_t g = (color >> 8) & 0xFF;
	uint8_t b = (color >> 0) & 0xFF;

	// Fill using optimized memory pattern
	for (int y = 0; y < PHYS_HEIGHT; y++) {
		for (int x = 0; x < PHYS_WIDTH; x++) {
			uint8_t *pixel = canvas->buffer[y][x];
			pixel[2] = r; // Red
			pixel[1] = g; // Green
			pixel[0] = b; // Blue
		}
	}
}

// Optimized line drawing with memcpy for horizontal lines
/* Liang-Barsky clip of a segment to the logical screen. Returns false when
 * none of it is visible. Coordinates arrive from Lua as raw integers, so
 * without this a far off-screen endpoint makes Bresenham step through
 * billions of pixels that canvas_set_pixel then rejects one by one, with the
 * Lua thread stuck in C where a break signal cannot reach it. */
static bool clip_line_to_screen(int *x0, int *y0, int *x1, int *y1)
{
	const double sx = (double)*x0;
	const double sy = (double)*y0;
	const double dx = (double)*x1 - sx;
	const double dy = (double)*y1 - sy;
	const double p[4] = { -dx, dx, -dy, dy };
	const double q[4] = { sx, (double)(LOG_WIDTH - 1) - sx, sy, (double)(LOG_HEIGHT - 1) - sy };
	double t0 = 0.0;
	double t1 = 1.0;

	for (int i = 0; i < 4; i++) {
		if (p[i] == 0.0) {
			if (q[i] < 0.0) {
				return false; /* parallel to this edge and outside it */
			}
			continue;
		}

		const double t = q[i] / p[i];

		if (p[i] < 0.0) {
			if (t > t1) {
				return false;
			}
			if (t > t0) {
				t0 = t;
			}
		} else {
			if (t < t0) {
				return false;
			}
			if (t < t1) {
				t1 = t;
			}
		}
	}

	/* The clipped endpoints lie inside [0, LOG_WIDTH-1] x [0, LOG_HEIGHT-1],
	 * so they are non-negative and +0.5 truncation rounds them. A segment
	 * that was already on screen keeps t0 = 0, t1 = 1 and is unchanged. */
	*x0 = (int)(sx + t0 * dx + 0.5);
	*y0 = (int)(sy + t0 * dy + 0.5);
	*x1 = (int)(sx + t1 * dx + 0.5);
	*y1 = (int)(sy + t1 * dy + 0.5);
	return true;
}

/* canvas_set_pixel takes int; guard the cast for 64-bit intermediates. */
static inline void plot_if_visible(Canvas *canvas, int64_t x, int64_t y, Color color)
{
	if (x >= 0 && x < LOG_WIDTH && y >= 0 && y < LOG_HEIGHT) {
		canvas_set_pixel(canvas, (int)x, (int)y, color);
	}
}

void canvas_draw_line(Canvas *canvas, int x0, int y0, int x1, int y1, Color color)
{
	// Extract color components once
	uint8_t r = (color >> 16) & 0xFF;
	uint8_t g = (color >> 8) & 0xFF;
	uint8_t b = (color >> 0) & 0xFF;

	// Handle horizontal line (optimized with contiguous fill)
	if (y0 == y1) {

		// Validate and clamp coordinates
		if (x0 > x1) {
			int tmp = x0;
			x0 = x1;
			x1 = tmp;
		}

		if (x0 < 0) {
			x0 = 0;
		}
		if (x1 >= LOG_WIDTH) {
			x1 = LOG_WIDTH - 1;
		}
		if (y0 < 0 || y0 >= LOG_HEIGHT || x0 > x1) {
			return;
		}

		// Fill vertical line in physical memory
		const int col = PHYS_WIDTH - y0 - 1;

		for (int x = x0; x <= x1; x++) {
			uint8_t *pixel = canvas->buffer[x][col];
			pixel[2] = r;
			pixel[1] = g;
			pixel[0] = b;
		}
		return;
	}

	// Handle vertical line (optimized without function calls)
	if (x0 == x1) {

		// Validate and clamp coordinates
		if (y0 > y1) {
			int tmp = y0;
			y0 = y1;
			y1 = tmp;
		}

		if (y0 < 0) {
			y0 = 0;
		}
		if (y1 >= LOG_HEIGHT) {
			y1 = LOG_HEIGHT - 1;
		}
		if (x0 < 0 || x0 >= LOG_WIDTH || y0 > y1) {
			return;
		}

		// Calculate physical column positions
		const int start_col = PHYS_WIDTH - y1 - 1;
		const int end_col = PHYS_WIDTH - y0 - 1;
		// Fill contiguous memory block
		for (int col = start_col; col <= end_col; col++) {
			uint8_t *pixel = canvas->buffer[x0][col];
			pixel[2] = r;
			pixel[1] = g;
			pixel[0] = b;
		}

		return;
	}

	// Handle diagonal lines with Bresenham's algorithm, on the visible part only
	if (!clip_line_to_screen(&x0, &y0, &x1, &y1)) {
		return;
	}

	int dx = abs(x1 - x0);
	int dy = abs(y1 - y0);
	int sx = (x0 < x1) ? 1 : -1;
	int sy = (y0 < y1) ? 1 : -1;
	int err = dx - dy;

	// Draw line segments with continuous color
	while (1) {
		canvas_set_pixel(canvas, x0, y0, color);
		if (x0 == x1 && y0 == y1) {
			break;
		}

		int e2 = 2 * err;
		if (e2 > -dy) {
			err -= dy;
			x0 += sx;
		}
		if (e2 < dx) {
			err += dx;
			y0 += sy;
		}
	}
}

// Optimized rectangle drawing
void canvas_draw_rect(Canvas *canvas, int x, int y, int w, int h, Color color, bool filled)
{
	// Validate parameters
	if (w <= 0 || h <= 0) {
		return;
	}

	// Calculate boundaries
	int x_end = x + w - 1;
	int y_end = y + h - 1;

	// Clamp coordinates to canvas boundaries
	if (x < 0) {
		x = 0;
	}
	if (x_end >= LOG_WIDTH) {
		x_end = LOG_WIDTH - 1;
	}
	if (y < 0) {
		y = 0;
	}
	if (y_end >= LOG_HEIGHT) {
		y_end = LOG_HEIGHT - 1;
	}

	// Calculate actual size after clamping
	w = x_end - x + 1;
	h = y_end - y + 1;

	if (w <= 0 || h <= 0) {
		return;
	}

	if (filled) {
		// Extract color components once
		uint8_t r = (color >> 16) & 0xFF;
		uint8_t g = (color >> 8) & 0xFF;
		uint8_t b = (color >> 0) & 0xFF;

		// Direct pixel fill - avoid function call overhead
		for (int row = x; row <= x_end; row++) {
			for (int col = y; col <= y_end; col++) {
				const int phys_col = PHYS_WIDTH - col - 1;
				uint8_t *pixel = canvas->buffer[row][phys_col];
				pixel[2] = r;
				pixel[1] = g;
				pixel[0] = b;
			}
		}
	} else {
		// Draw four borders using optimized line functions
		canvas_draw_line(canvas, x, y, x_end, y, color);         // Top
		canvas_draw_line(canvas, x, y_end, x_end, y_end, color); // Bottom
		canvas_draw_line(canvas, x, y, x, y_end, color);         // Left
		canvas_draw_line(canvas, x_end, y, x_end, y_end, color); // Right
	}
}

// Optimized circle drawing
void canvas_draw_circle(Canvas *canvas, int cx, int cy, int radius, Color color, bool filled)
{
	// Handle degenerate circles
	if (radius <= 0) {
		canvas_set_pixel(canvas, cx, cy, color);
		return;
	}

	// Centre and radius arrive from Lua as raw integers: cx + radius and
	// radius * radius both overflow int for large inputs, so work in 64 bits.
	const int64_t r = radius;
	const int64_t cx64 = cx;
	const int64_t cy64 = cy;

	// Clip circle to visible area
	if (cx64 + r < 0 || cx64 - r >= LOG_WIDTH || cy64 + r < 0 || cy64 - r >= LOG_HEIGHT) {
		return;
	}

	// Only the rows on screen are worth visiting: a huge radius must not turn
	// into a loop over billions of rows that are skipped one by one.
	int64_t ty_start = cy64 - r;
	int64_t ty_end = cy64 + r;

	if (ty_start < 0) {
		ty_start = 0;
	}
	if (ty_end > LOG_HEIGHT - 1) {
		ty_end = LOG_HEIGHT - 1;
	}

	if (filled) {
		// Fill circle using horizontal spans for better performance
		for (int64_t ty = ty_start; ty <= ty_end; ty++) {
			// Calculate horizontal span for this row
			const int64_t y_dist = ty - cy64;
			const int64_t chord_length = (int64_t)sqrt((double)(r * r - y_dist * y_dist));
			int64_t start_x = cx64 - chord_length;
			int64_t end_x = cx64 + chord_length;

			// Clip horizontal span to visible area
			if (start_x < 0) {
				start_x = 0;
			}
			if (end_x >= LOG_WIDTH) {
				end_x = LOG_WIDTH - 1;
			}

			// Draw horizontal span with optimized function
			if (start_x <= end_x) {
				canvas_draw_line(canvas, (int)start_x, (int)ty, (int)end_x, (int)ty,
						 color);
			}
		}
		return;
	}

	if (r <= 4 * (LOG_WIDTH + LOG_HEIGHT)) {
		// Outline circle using midpoint algorithm: unchanged for every radius
		// that can plausibly be meant for this screen
		int x = 0;
		int y = radius;
		int d = 3 - 2 * radius;

		// Draw eight symmetric points
		while (y >= x) {
			// Draw eight octants
			canvas_set_pixel(canvas, cx + x, cy + y, color);
			canvas_set_pixel(canvas, cx - x, cy + y, color);
			canvas_set_pixel(canvas, cx + x, cy - y, color);
			canvas_set_pixel(canvas, cx - x, cy - y, color);
			canvas_set_pixel(canvas, cx + y, cy + x, color);
			canvas_set_pixel(canvas, cx - y, cy + x, color);
			canvas_set_pixel(canvas, cx + y, cy - x, color);
			canvas_set_pixel(canvas, cx - y, cy - x, color);

			// Update midpoint decision parameter
			if (d < 0) {
				d = d + 4 * x + 6;
			} else {
				d = d + 4 * (x - y) + 10;
				y--;
			}
			x++;
		}
		return;
	}

	// Very large outline: the midpoint walk costs about 0.7 * radius steps,
	// nearly all of them off screen. Scan only the visible rows and columns
	// and plot where the circle crosses each; the two passes together leave
	// no gaps.
	for (int64_t ty = ty_start; ty <= ty_end; ty++) {
		const int64_t y_dist = ty - cy64;
		const int64_t half = (int64_t)(sqrt((double)(r * r - y_dist * y_dist)) + 0.5);

		plot_if_visible(canvas, cx64 - half, ty, color);
		plot_if_visible(canvas, cx64 + half, ty, color);
	}

	int64_t tx_start = cx64 - r;
	int64_t tx_end = cx64 + r;

	if (tx_start < 0) {
		tx_start = 0;
	}
	if (tx_end > LOG_WIDTH - 1) {
		tx_end = LOG_WIDTH - 1;
	}

	for (int64_t tx = tx_start; tx <= tx_end; tx++) {
		const int64_t x_dist = tx - cx64;
		const int64_t half = (int64_t)(sqrt((double)(r * r - x_dist * x_dist)) + 0.5);

		plot_if_visible(canvas, tx, cy64 - half, color);
		plot_if_visible(canvas, tx, cy64 + half, color);
	}
}

// Polygon drawing optimized for triangles
void canvas_draw_polygon(Canvas *canvas, const int *points, int count, Color color)
{
	// Degenerate cases
	if (count < 2) {
		return;
	}

	// Two points: just the segment. The triangle path below would read a
	// third vertex the caller never supplied.
	if (count == 2) {
		canvas_draw_line(canvas, points[0], points[1], points[2], points[3], color);
		return;
	}

	// Simple line rendering for convex/concave polygons
	if (count > 3) {
		// Draw outline with optimized lines
		for (int i = 0; i < count; i++) {
			int j = (i + 1) % count;
			canvas_draw_line(canvas, points[i * 2], points[i * 2 + 1], points[j * 2],
					 points[j * 2 + 1], color);
		}
		return;
	}

	// Optimized triangle filling
	const int *p0 = &points[0];
	const int *p1 = &points[2];
	const int *p2 = &points[4];

	// Sort vertices by y-coordinate
	const int *top = p0, *mid = p1, *bot = p2;
	if (top[1] > mid[1]) {
		const int *t = top;
		top = mid;
		mid = t;
	}
	if (top[1] > bot[1]) {
		const int *t = top;
		top = bot;
		bot = t;
	}
	if (mid[1] > bot[1]) {
		const int *t = mid;
		mid = bot;
		bot = t;
	}

	// Calculate bounding box
	int min_y = top[1];
	int max_y = bot[1];

	// Clamp to visible area
	if (min_y >= LOG_HEIGHT || max_y < 0) {
		return;
	}
	if (min_y < 0) {
		min_y = 0;
	}
	if (max_y >= LOG_HEIGHT) {
		max_y = LOG_HEIGHT - 1;
	}

	// Precompute slopes (constant for each half)
	float slope_left_top = 0, slope_left_bottom = 0;
	if (mid[1] != top[1]) {
		slope_left_top = (mid[0] - top[0]) / (float)(mid[1] - top[1]);
	}
	if (bot[1] != mid[1]) {
		slope_left_bottom = (bot[0] - mid[0]) / (float)(bot[1] - mid[1]);
	}
	float slope_right = (bot[0] - top[0]) / (float)(bot[1] - top[1]);

	float x_left = top[0], x_right = top[0];

	// Fill the triangle using scanlines
	for (int y = min_y; y <= max_y; y++) {
		// Check if we passed the first vertex
		if (y > mid[1] && y <= bot[1]) {
			// Bottom half: use precomputed slopes
			x_left = mid[0] + slope_left_bottom * (y - mid[1]);
			x_right = top[0] + slope_right * (y - top[1]);
		} else {
			// Top half: use precomputed slopes
			x_left = top[0] + slope_left_top * (y - top[1]);
			x_right = top[0] + slope_right * (y - top[1]);
		}

		// Ensure left and right are in order
		if (x_left > x_right) {
			float tmp = x_left;
			x_left = x_right;
			x_right = tmp;
		}

		// Draw the scanline
		int start_x = (int)x_left;
		int end_x = (int)x_right;

		// Clamp to canvas boundaries
		if (start_x < 0) {
			start_x = 0;
		}
		if (end_x >= LOG_WIDTH) {
			end_x = LOG_WIDTH - 1;
		}

		// Draw line between left and right edges
		if (start_x <= end_x) {
			canvas_draw_line(canvas, start_x, y, end_x, y, color);
		}
	}
}

// Optimized bitmap drawing
void canvas_draw_bitmap(Canvas *canvas, int x, int y, int width, int height, const uint8_t *bitmap)
{
	if (width <= 0 || height <= 0 || bitmap == NULL) {
		return;
	}

	int dst_x_end = x + width - 1;
	int dst_y_end = y + height - 1;

	if (dst_x_end < 0 || dst_y_end < 0 || x >= LOG_WIDTH || y >= LOG_HEIGHT) {
		return;
	}

	int src_x_start = 0, src_y_start = 0;
	int src_x_end = width - 1, src_y_end = height - 1;
	int dst_x_start = x, dst_y_start = y;

	if (x < 0) {
		src_x_start = -x;
		dst_x_start = 0;
	}
	if (y < 0) {
		src_y_start = -y;
		dst_y_start = 0;
	}
	if (dst_x_end >= LOG_WIDTH) {
		dst_x_end = LOG_WIDTH - 1;
		src_x_end = LOG_WIDTH - 1 - x;
	}
	if (dst_y_end >= LOG_HEIGHT) {
		dst_y_end = LOG_HEIGHT - 1;
		src_y_end = LOG_HEIGHT - 1 - y;
	}

	if (src_x_start > src_x_end || src_y_start > src_y_end) {
		return;
	}

	const int src_row_stride = width * 3;

	for (int src_y = src_y_start, dst_y = dst_y_start; src_y <= src_y_end; src_y++, dst_y++) {
		const int phys_col_base = PHYS_WIDTH - dst_y - 1;
		const uint8_t *row_ptr = bitmap + src_y * src_row_stride;

		for (int src_x = src_x_start, dst_x = dst_x_start; src_x <= src_x_end; src_x++, dst_x++) {
			const int pixel_offset = src_x * 3;
			uint8_t *pixel = canvas->buffer[dst_x][phys_col_base];
			pixel[2] = row_ptr[pixel_offset + 0];
			pixel[1] = row_ptr[pixel_offset + 1];
			pixel[0] = row_ptr[pixel_offset + 2];
		}
	}
}

void canvas_set_font(Canvas *canvas, const GFXfont *font, uint8_t size)
{
	canvas->font = font;
	canvas->font_size = size;
}

int canvas_draw_char(Canvas *canvas, uint16_t c, int x, int y, Color color)
{
	const GFXfont *gfxFont = canvas->font;
	uint8_t size = canvas->font_size;
	if (!gfxFont || size == 0) {
		return 0;
	}

	uint8_t w, h;
	int8_t xo, yo;
	uint8_t advance = 0;

	if ((c >= pgm_read_word(&gfxFont->first)) && (c <= pgm_read_word(&gfxFont->last))) {
		c -= pgm_read_word(&gfxFont->first);
		GFXglyph *glyph = &(((GFXglyph *)pgm_read_dword(&gfxFont->glyph))[c]);
		w = pgm_read_byte(&glyph->width);
		h = pgm_read_byte(&glyph->height);
		xo = pgm_read_byte(&glyph->xOffset);
		yo = pgm_read_byte(&glyph->yOffset);
		advance = pgm_read_byte(&glyph->xAdvance) * size;

		/* Glyph arrays are indexed from gfxFont->first, so 'H' needs the
		 * same offset the drawn glyph got above. Fonts whose range does
		 * not cover 'H' fall back to the drawn glyph's own ascent.
		 */
		uint16_t first = pgm_read_word(&gfxFont->first);
		int8_t ascent = yo;

		if ('H' >= first && 'H' <= pgm_read_word(&gfxFont->last)) {
			GFXglyph *glyph_ascent =
				&(((GFXglyph *)pgm_read_dword(&gfxFont->glyph))['H' - first]);
			ascent = pgm_read_byte(&glyph_ascent->yOffset);
		}
		y -= ascent * size;

		uint8_t *bitmap = (uint8_t *)pgm_read_dword(&gfxFont->bitmap);
		uint32_t bo = pgm_read_word(&glyph->bitmapOffset);

		uint8_t xx, yy, bits = 0, bit = 0;
		// uint8_t  xa = pgm_read_byte(&glyph->xAdvance);
		int16_t xo16 = 0, yo16 = 0;

		if (size > 1) {
			xo16 = xo;
			yo16 = yo;
		}

		uint16_t hpc = 0; // Horizontal foreground pixel count
		for (yy = 0; yy < h; yy++) {
			for (xx = 0; xx < w; xx++) {
				if (bit == 0) {
					bits = pgm_read_byte(&bitmap[bo++]);
					bit = 0x80;
				}
				if (bits & bit) {
					hpc++;
				} else {
					if (hpc) {
						if (size == 1) {
							canvas_draw_line(canvas, x + xo + xx - hpc,
									 y + yo + yy, x + xo + xx,
									 y + yo + yy, color);
						} else {
							canvas_draw_rect(
								canvas,
								x + (xo16 + xx - hpc) * size,
								y + (yo16 + yy) * size, size * hpc,
								size, color, true);
						}
						hpc = 0;
					}
				}
				bit >>= 1;
			}
			// Draw pixels for this line as we are about to increment yy
			if (hpc) {
				if (size == 1) {
					canvas_draw_line(canvas, x + xo + xx - hpc, y + yo + yy,
							 x + xo + xx, y + yo + yy, color);
				} else {
					canvas_draw_rect(canvas, x + (xo16 + xx - hpc) * size,
							 y + (yo16 + yy) * size, size * hpc, size,
							 color, true);
				}
				hpc = 0;
			}
		}
	}
	return advance;
}

int canvas_draw_string(Canvas *canvas, const char *str, int x, int y, Color color)
{
	int total_w = 0;
	int len = strlen(str);
	for (int i = 0; i < len; i++) {
		int w = canvas_draw_char(canvas, str[i], x, y, color);
		x += w;
		total_w += w;
	}
	return total_w;
}

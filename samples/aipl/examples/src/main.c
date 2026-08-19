/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 *
 */

#include <dave_d0lib.h>
#include <aipl_dave2d.h>
#include <aipl_color_conversion.h>
#include <aipl_color_correction.h>
#include <aipl_white_balance.h>
#include <aipl_lut_transform.h>
#include <aipl_crop.h>
#include <aipl_resize.h>
#include <aipl_rotate.h>
#include <aipl_flip.h>
#include <aipl_utils.h>

#include <math.h>

#include "image.h"
#include "img_assets/assets.h"
#include "dbuf_display/display.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(app, CONFIG_LOG_DEFAULT_LEVEL);

#define D1_HEAP_SIZE 0x180000

#ifdef CONFIG_D0_HEAP_SECTION
#define D0_HEAP_ATTRS __attribute__((section(CONFIG_D0_HEAP_SECTION)))
#else
#define D0_HEAP_ATTRS
#endif

static uint8_t D0_HEAP_ATTRS d0_heap[D1_HEAP_SIZE];

static void crop_scale_example(void)
{
	/* Prepare the source image */
	const image_t *test_img = &SAMPLE_PHOTO_ARGB8888;
	const aipl_image_t src_image = {test_img->data, test_img->pitch, test_img->width,
					test_img->height, test_img->format};

	/* Prepare the destination image for cropping */
	uint32_t p = src_image.pitch / 4;
	uint32_t w = src_image.width / 4;
	uint32_t h = src_image.height / 4;
	aipl_image_t cropped_image;

	aipl_image_create(&cropped_image, p, w, h, src_image.format);

	/* Crop the source image using the specified rectangular area */
	uint32_t x = 200;
	uint32_t y = 100;

	aipl_crop_img(&src_image, &cropped_image, x, y, x + w, y + h);

	/* Prepare the destination image for scaling */
	aipl_image_t scaled_image;

	aipl_image_create(&scaled_image, p * 3, w * 3, h * 3, cropped_image.format);

	/* Scale up the cropped image appliying linear interpolation */
	aipl_resize_img(&cropped_image, &scaled_image, true);

	/* Prepare DAVE2D for image rendering */
	aipl_dave2d_prepare();

	/* Draw source and destination images */
	aipl_image_draw(0, 20, &src_image);
	aipl_image_draw(0, 400, &cropped_image);
	aipl_image_draw(0, 500, &scaled_image);

	/* Execute HW rendering */
	aipl_dave2d_render();

	/* Release the memory occupied by the destination images */
	aipl_image_destroy(&cropped_image);
	aipl_image_destroy(&scaled_image);
}

static void crop_flip_example(void)
{
	/* Prepare the source image */
	const image_t *test_img = &SAMPLE_PHOTO_ARGB8888;
	const aipl_image_t src_image = {test_img->data, test_img->pitch, test_img->width,
					test_img->height, test_img->format};

	/* Prepare the destination image for cropping */
	uint32_t p = src_image.pitch / 2;
	uint32_t w = src_image.width / 2;
	uint32_t h = src_image.height / 2;
	aipl_image_t cropped_image;

	aipl_image_create(&cropped_image, p, w, h, src_image.format);

	/* Crop the source image using the specified rectangular area */
	uint32_t x = 200;
	uint32_t y = 100;

	aipl_crop_img(&src_image, &cropped_image, x, y, x + w, y + h);

	/* Prepare the destination image for flipping */
	aipl_image_t flipped_image;

	aipl_image_create(&flipped_image, p, w, h, cropped_image.format);

	/* Flip the image horizontally and vertically */
	aipl_flip_img(&cropped_image, &flipped_image, true, true);

	/* Prepare DAVE2D for image rendering */
	aipl_dave2d_prepare();

	/* Draw source and destination images */
	aipl_image_draw(0, 20, &src_image);
	aipl_image_draw(0, 400, &cropped_image);
	aipl_image_draw(0, 600, &flipped_image);

	/* Execute HW rendering */
	aipl_dave2d_render();

	/* Release the memory occupied by the destination images */
	aipl_image_destroy(&cropped_image);
	aipl_image_destroy(&flipped_image);
}

static void scale_rotate_example(void)
{
	/* Prepare the source image */
	const image_t *test_img = &SAMPLE_PHOTO_ARGB8888;
	const aipl_image_t src_image = {test_img->data, test_img->pitch, test_img->width,
					test_img->height, test_img->format};

	/* Prepare the destination image for scaling */
	uint32_t p = src_image.pitch / 2;
	uint32_t w = src_image.width / 2;
	uint32_t h = src_image.height / 2;
	aipl_image_t scaled_image;

	aipl_image_create(&scaled_image, p, w, h, src_image.format);

	/* Scale down the source image using nearest-neighbor interpolation */
	aipl_resize_img(&src_image, &scaled_image, false);

	/* Prepare the destination image for rotation */
	aipl_image_t rotated90_image;

	aipl_image_create(&rotated90_image, h, h, w, src_image.format);
	aipl_image_t rotated180_image;

	aipl_image_create(&rotated180_image, p, w, h, src_image.format);

	/* Scale down the image */
	aipl_rotate_img(&scaled_image, &rotated90_image, AIPL_ROTATE_90);
	aipl_rotate_img(&scaled_image, &rotated180_image, AIPL_ROTATE_180);

	/* Prepare DAVE2D for image rendering */
	aipl_dave2d_prepare();

	/* Draw source and destination images */
	aipl_image_draw(0, 20, &src_image);
	aipl_image_draw(0, 400, &scaled_image);
	aipl_image_draw(300, 400, &rotated90_image);
	aipl_image_draw(0, 600, &rotated180_image);

	/* Execute HW rendering */
	aipl_dave2d_render();

	/* Release the memory occupied by the destination images */
	aipl_image_destroy(&scaled_image);
	aipl_image_destroy(&rotated90_image);
	aipl_image_destroy(&rotated180_image);
}

/* Example of conversion from ARGB8888 to ALPHA8 */
static void color_conversion_example(void)
{
	/* Prepare the source image */
	const image_t *test_img = &SAMPLE_PHOTO_ARGB8888;
	const aipl_image_t src_image = {test_img->data, test_img->pitch, test_img->width,
					test_img->height, test_img->format};

	/* Prepare the destination buffer for converted image */
	aipl_image_t dst_image;

	aipl_image_create(&dst_image, src_image.pitch, src_image.width, src_image.height,
			  AIPL_COLOR_ALPHA8);

	/* Peform the conversion */
	aipl_color_convert_img(&src_image, &dst_image);

	/* Prepare DAVE2D for image rendering */
	aipl_dave2d_prepare();

	/* Draw source and destination images */
	aipl_image_draw(0, 20, &src_image);
	aipl_image_draw(0, 400, &dst_image);

	/* Execute HW rendering */
	aipl_dave2d_render();

	/* Release the memory occupied by the destination images */
	aipl_image_destroy(&dst_image);
}

/* Example of color correction to increase color saturation */
static void color_correction_example(void)
{
	/* Prepare the source image */
	const image_t *test_img = &SAMPLE_PHOTO_ARGB8888;
	const aipl_image_t src_image = {test_img->data, test_img->pitch, test_img->width,
					test_img->height, test_img->format};

	/* Prepare the destination color corrected image */
	aipl_image_t dst_image;

	aipl_image_create(&dst_image, src_image.pitch, src_image.width, src_image.height,
			  src_image.format);

	/* Prepare color correction matrix */
	static float ccm[] = {1.3f, -0.3f, -0.3f, -0.3f, 1.3f, -0.3f, -0.3f, -0.3f, 1.3f};

	/* Peform the correction */
	aipl_color_correction_rgb_img(&src_image, &dst_image, ccm);

	/* Prepare DAVE2D for image rendering */
	aipl_dave2d_prepare();

	/* Draw source and destination images */
	aipl_image_draw(0, 20, &src_image);
	aipl_image_draw(0, 400, &dst_image);

	/* Execute HW rendering */
	aipl_dave2d_render();

	/* Release the memory occupied by the destination images */
	aipl_image_destroy(&dst_image);
}

/* Example of using white balance to add a blueish tint */
static void white_balance_example(void)
{
	/* Prepare the source image */
	const image_t *test_img = &SAMPLE_PHOTO_ARGB8888;
	const aipl_image_t src_image = {test_img->data, test_img->pitch, test_img->width,
					test_img->height, test_img->format};

	/* Prepare the destination white balanced image */
	aipl_image_t dst_image;

	aipl_image_create(&dst_image, src_image.pitch, src_image.width, src_image.height,
			  src_image.format);

	/* Peform the correction */
	aipl_white_balance_rgb_img(&src_image, &dst_image, 1.0f, 0.8f, 1.2f);

	/* Prepare DAVE2D for image rendering */
	aipl_dave2d_prepare();

	/* Draw source and destination images */
	aipl_image_draw(0, 20, &src_image);
	aipl_image_draw(0, 400, &dst_image);

	/* Execute HW rendering */
	aipl_dave2d_render();

	/* Release the memory occupied by the destination images */
	aipl_image_destroy(&dst_image);
}

/* Example of gamma correction from sRGB to linear */
static void gamma_correction_example(void)
{
	/* Prepare the source image */
	const image_t *test_img = &SAMPLE_PHOTO_ARGB8888;
	const aipl_image_t src_image = {test_img->data, test_img->pitch, test_img->width,
					test_img->height, test_img->format};

	/* Prepare the destination color corrected image */
	aipl_image_t dst_image;

	aipl_image_create(&dst_image, src_image.pitch, src_image.width, src_image.height,
			  src_image.format);

	/* Prepare gamma correction LUT */
	static uint8_t lut[256];

	for (uint32_t i = 0; i < 256; ++i) {
		float srgb_num = i * (1.0f / 255.0f);

		float lin_num;

		if (srgb_num <= 0.04045f) {
			lin_num = srgb_num / 12.92f;
		} else {
			lin_num = powf((double)(srgb_num + 0.055f) / 1.055, 2.4f);
		}

		lut[i] = lin_num * 255.0f + 0.5f;
	}

	/* Peform the gamma LUT transform */
	aipl_lut_transform_rgb_img(&src_image, &dst_image, lut);

	/* Prepare DAVE2D for image rendering */
	aipl_dave2d_prepare();

	/* Draw source and destination images */
	aipl_image_draw(0, 20, &src_image);
	aipl_image_draw(0, 400, &dst_image);

	/* Execute HW rendering */
	aipl_dave2d_render();

	/* Release the memory occupied by the destination images */
	aipl_image_destroy(&dst_image);
}

/* Example of exposure adjustment */
static void exposure_adjustment_example(void)
{
	/* Prepare the source image */
	const image_t *test_img = &SAMPLE_PHOTO_ARGB8888;
	const aipl_image_t src_image = {test_img->data, test_img->pitch, test_img->width,
					test_img->height, test_img->format};

	/* Prepare the destination color corrected image */
	aipl_image_t dst_image;

	aipl_image_create(&dst_image, src_image.pitch, src_image.width, src_image.height,
			  src_image.format);

	/* Prepare exposure adjustment LUT */
	float exp_adj = 0.5f;
	static uint8_t lut[256];

	for (uint32_t i = 0; i < 256; ++i) {
		lut[i] = aipl_channel_cap((int)(i * powf(2, exp_adj)));
	}

	/* Peform the exposure LUT transform */
	aipl_lut_transform_rgb_img(&src_image, &dst_image, lut);

	/* Prepare DAVE2D for image rendering */
	aipl_dave2d_prepare();

	/* Draw source and destination images */
	aipl_image_draw(0, 20, &src_image);
	aipl_image_draw(0, 400, &dst_image);

	/* Execute HW rendering */
	aipl_dave2d_render();

	/* Release the memory occupied by the destination images */
	aipl_image_destroy(&dst_image);
}

int main(void)
{
	/* Initialize display */
	if (display_init()) {
		LOG_ERR("Display initializing error");
		return -1;
	}

#ifdef CONFIG_D1_MALLOC_D0LIB
	/* Initialize D/AVE D0 heap */
	if (!d0_initheapmanager(d0_heap, sizeof(d0_heap), d0_mm_fixed_range, NULL, 0, 0, 0,
				d0_ma_unified)) {
		LOG_ERR("Heap manager initialization failed");
		return -1;
	}
#endif

	/* Initialize D/AVE2D */
	if (aipl_dave2d_init() != D2_OK) {
		LOG_ERR("D/AVE2D initialization failed");
		return -1;
	}

	/* Run AIPL examples */
	while (true) {
		LOG_INF("Example 1: Crop the image and scale it up");
		crop_scale_example();
		k_msleep(3000);
		LOG_INF("Example 2: Scale down the image and rotate 90/180 degrees");
		scale_rotate_example();
		k_msleep(3000);
		LOG_INF("Example 3: Crop the image and flip it both vertically and horizontally");
		crop_flip_example();
		k_msleep(3000);
		LOG_INF("Example 4: Convert ARGB8888 to ALPHA8");
		color_conversion_example();
		k_msleep(3000);
		LOG_INF("Example 5: Increase color saturation (color correction)");
		color_correction_example();
		k_msleep(3000);
		LOG_INF("Example 6: Use white balance to add a blueish tint");
		white_balance_example();
		k_msleep(3000);
		LOG_INF("Example 7: Gamma correction from sRGB to linear");
		gamma_correction_example();
		k_msleep(3000);
		LOG_INF("Example 8: Exposure adjustment");
		exposure_adjustment_example();
		k_msleep(5000);
	}

	return 0;
}

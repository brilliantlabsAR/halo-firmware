/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 *
 */

#include "dbuf_display/display.h"
#include "dave_d0lib.h"
#include "aipl_dave2d.h"

#include "perf_tests.h"
#include "objects.h"
#include "img_assets/assets.h"

#include "utmr.h"
#include "fps_counter.h"
#include "cpu_usage.h"

#include "aipl_color_conversion.h"
#include "aipl_color_correction.h"
#include "aipl_white_balance.h"
#include "aipl_lut_transform.h"

#include <math.h>

#include "aipl_utils.h"

#define D1_HEAP_SIZE 0x180000

#define FRAME_TIME_MS    20
#define NUM_MEASUREMENTS 100
#define FPS_CNT_INT_MS   100
#define COLOR_FORMATS    (AIPL_COLOR_UYVY + 1)
#define NUM_OPERATIONS   (COLOR_FORMATS + 7)
#define TABLE_COL_WIDTH  20

#ifdef CONFIG_D0_HEAP_SECTION
#define D0_HEAP_ATTRS __attribute__((section(CONFIG_D0_HEAP_SECTION)))
#else
#define D0_HEAP_ATTRS
#endif

static uint8_t D0_HEAP_ATTRS d0_heap[D1_HEAP_SIZE];

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(app, CONFIG_LOG_DEFAULT_LEVEL);

static char *bench_names[NUM_OPERATIONS] = {
	"to ALPHA8",     "to ARGB8888",      "to ARGB4444", "to ARGB1555", "to RGBA8888",
	"to RGBA4444",   "to RGBA5551",      "to BGR888",   "to RGB888",   "to RGB565",
	"to YV12",       "to I420",          "to I422",     "to I444",     "to I400",
	"to NV21",       "to NV12",          "to YUY2",     "to UYVY",     "Color correction",
	"White balance", "Gamma correction", "Flipping",    "Cropping",    "Resize",
	"Rotation",
};
static benchmark_result_t bench_results[COLOR_FORMATS][NUM_OPERATIONS];

static void prepare_color_conversion(aipl_image_t *src, aipl_image_t *dst, op_arg_t *args)
{
	args->src = src;
	args->dst = dst;
}

static void prepare_color_correction(aipl_image_t *src, aipl_image_t *dst, cc_op_arg_t *args)
{
	static const float ccm[] = {2.2583f, -0.1606f, -0.6317f, -0.5501f, 1.4318f,
				    -0.0653f, -0.1248f, -0.5268f, 2.3735f};

	args->src = src;
	args->dst = dst;
	args->ccm = (float *)ccm;
}

static void prepare_white_balance(aipl_image_t *src, aipl_image_t *dst, wb_op_arg_t *args)
{
	const float ar = 1.2f;
	const float ag = 0.8f;
	const float ab = 0.6f;

	args->src = src;
	args->dst = dst;
	args->ar = ar;
	args->ag = ag;
	args->ab = ab;
}

static void prepare_gamma_correction(aipl_image_t *src, aipl_image_t *dst, gc_op_arg_t *args)
{
	static uint8_t srgb_lut[256];

	for (int i = 0; i < 256; ++i) {
		float lin_lum = i * (1.0f / 255.0f);

		float srgb_lum;

		if (lin_lum <= 0.0031308f) {
			srgb_lum = 12.92f * lin_lum;
		} else {
			srgb_lum = 1.055f * powf(lin_lum, 1.0f / 2.4f) - 0.055f;
		}

		srgb_lut[i] = srgb_lum * 255.0f + 0.5f;
	}

	args->src = src;
	args->dst = dst;
	args->gamma_lut = (uint8_t *)srgb_lut;
}

static void prepare_cropping(aipl_image_t *src, aipl_image_t *dst, crop_op_arg_t *args)
{
	args->left = 200;
	args->top = 100;

	args->src = src;
	args->dst = dst;
	args->right = args->left + dst->width;
	args->bottom = args->top + dst->height;
}

static void prepare_flipping(aipl_image_t *src, aipl_image_t *dst, flip_op_arg_t *args)
{
	args->src = src;
	args->dst = dst;

	args->horizontal = true;
	args->vertical = true;
}

static void prepare_resize(aipl_image_t *src, aipl_image_t *dst, op_arg_t *args)
{
	args->src = src;
	args->dst = dst;
}

static void prepare_rotation(aipl_image_t *src, aipl_image_t *dst, rot_op_arg_t *args)
{
	args->src = src;
	args->dst = dst;
	args->rotation = AIPL_ROTATE_90;
}

static benchmark_result_t perform_benchmark(const aipl_image_t *input, aipl_image_t *output,
					    benchmark_t *bench, void *args, const char *name)
{
	utimer_start();

	fps_counter_t fps_counter = fps_counter_create(FPS_CNT_INT_MS * 1000);

	for (uint32_t i = 0; i < NUM_MEASUREMENTS; ++i) {
		d2_device *handle = aipl_dave2d_handle();

		d2_framebuffer(handle, display_inactive_buffer(), display_width(), display_width(),
			       display_height(), d2_mode_rgb565);

		graph_clear_screen();

		if (benchmark_run_once(bench, args) != AIPL_ERR_OK) {
			benchmark_result_t res = {0, 0.0f};
			return res;
		}

		graph_object_t img_obj1 =
			graph_create_image(0, 40, input->data, input->pitch, input->width,
					   input->height, input->format);
		graph_object_t img_obj2 =
			graph_create_image(0, 400, output->data, output->pitch, output->width,
					   output->height, output->format);

		graph_draw_object(&img_obj1);
		graph_draw_object(&img_obj2);

		d2_endframe(handle);
		display_next_frame();
		fps_counter_add_frame(&fps_counter);
		display_set_next_frame_duration(FRAME_TIME_MS);

		graph_destroy_object(&img_obj1);
		graph_destroy_object(&img_obj2);
	}

	benchmark_result_t res = benchmark_summarize(bench);

	bench->execuion_time = 0;
	bench->idle_time = 0;
	bench->num_runs = 0;

	/* Color format, Test name, CPU load[%%], Avg. time[us], Avg. FPS, DAVE2D */
	printk("%s,%s,%u,%.1f,%.1f\r\n", aipl_color_format_str(input->format), name, res.avg_time,
	       (double)res.cpu_load * 100, (double)fps_counter_get_average(&fps_counter));

	utimer_stop();

	return res;
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
		LOG_ERR("Heap manager initialization failed\n");
		return -1;
	}
#endif

	/* Initialize D/AVE2D */
	if (aipl_dave2d_init() != D2_OK) {
		LOG_ERR("D/AVE2D initialization failed\n");
		return -1;
	}

	/* Initialize utimer */
	utimer_init();

	const image_t *test_img = &SAMPLE_PHOTO_ARGB8888;

	aipl_image_t image = {test_img->data, test_img->pitch, test_img->width, test_img->height,
			      test_img->format};

	utimer_start();

	cpu_usage_enable();

	LOG_INF("AIPL BENCHMARK");

#ifdef CONFIG_AIPL_DAVE2D_ACCELERATION
	LOG_INF("AIPL DAVE2D ACCELERATION ENABLED");
#else
	LOG_INF("AIPL DAVE2D ACCELERATION DISABLED");
#endif
#ifdef CONFIG_AIPL_HELIUM_ACCELERATION
	LOG_INF("AIPL HELIUM ACCELERATION ENABLED");
#else
	LOG_INF("AIPL HELIUM ACCELERATION DISABLED");
#endif

	printk("Color format,Test name,Avg. time[us],CPU load[%%],Avg. FPS\n");

	for (int i = AIPL_COLOR_ALPHA8; i < COLOR_FORMATS; ++i) {
		aipl_image_t src;

		if (image.format == i) {
			src = image;
		} else {
			if (aipl_image_create(&src, image.width, image.width, image.height, i) !=
			    AIPL_ERR_OK) {
				LOG_ERR("Not enough memory for source image");
				continue;
			}

			if (aipl_color_convert_img(&image, &src) != AIPL_ERR_OK) {
				LOG_ERR("Failed to convert source image");
				aipl_image_destroy(&src);
				continue;
			}
		}

		/* Color conversions  */
		benchmark_t cnv_bench = create_color_conversion_benchmark();

		int j = AIPL_COLOR_ALPHA8;

		for (; j < COLOR_FORMATS; ++j) {
			if (i == j) {
				continue;
			}

			aipl_image_t dst;

			if (aipl_image_create(&dst, src.width, src.width, src.height, j) !=
			    AIPL_ERR_OK) {
				LOG_ERR("Not enough memory for color conversion destination image");
				continue;
			}

			op_arg_t cnv_args;

			prepare_color_conversion(&src, &dst, &cnv_args);
			bench_results[i][j] = perform_benchmark(&src, &dst, &cnv_bench, &cnv_args,
								bench_names[j]);

			aipl_image_destroy(&dst);
		}

		/* Destination of equal size and format */
		{
			aipl_image_t dst;

			if (aipl_image_create(&dst, src.width, src.width, src.height, src.format) !=
			    AIPL_ERR_OK) {
				LOG_ERR("Not enough memory for destination image with the same "
					"size as source");
				continue;
			}
			benchmark_t cc_bench = create_color_correction_benchmark();
			cc_op_arg_t cc_args;

			prepare_color_correction(&src, &dst, &cc_args);
			bench_results[i][j] =
				perform_benchmark(&src, &dst, &cc_bench, &cc_args, bench_names[j]);
			++j;

			benchmark_t wb_bench = create_white_balance_benchmark();
			wb_op_arg_t wb_args;

			prepare_white_balance(&src, &dst, &wb_args);
			bench_results[i][j] =
				perform_benchmark(&src, &dst, &wb_bench, &wb_args, bench_names[j]);
			++j;

			benchmark_t gc_bench = create_lut_transform_benchmark();
			gc_op_arg_t gc_args;

			prepare_gamma_correction(&src, &dst, &gc_args);
			bench_results[i][j] =
				perform_benchmark(&src, &dst, &gc_bench, &gc_args, bench_names[j]);
			++j;

			benchmark_t flip_bench = create_flipping_benchmark();
			flip_op_arg_t flip_args;

			prepare_flipping(&src, &dst, &flip_args);
			bench_results[i][j] = perform_benchmark(&src, &dst, &flip_bench, &flip_args,
								bench_names[j]);
			++j;

			aipl_image_destroy(&dst);
		}

		/* Destination is half width and height */
		{
			aipl_image_t dst;

			if (aipl_image_create(&dst, src.width / 2, src.width / 2, src.height / 2,
					      src.format) != AIPL_ERR_OK) {
				LOG_ERR("Not enough memory for destination image with the half "
					"source size");
				continue;
			}

			benchmark_t crop_bench = create_cropping_benchmark();
			crop_op_arg_t crop_args;

			prepare_cropping(&src, &dst, &crop_args);
			bench_results[i][j] = perform_benchmark(&src, &dst, &crop_bench, &crop_args,
								bench_names[j]);
			++j;

			benchmark_t resize_bench = create_resize_benchmark();
			op_arg_t resize_args;

			prepare_resize(&src, &dst, &resize_args);
			bench_results[i][j] = perform_benchmark(&src, &dst, &resize_bench,
								&resize_args, bench_names[j]);
			++j;

			aipl_image_destroy(&dst);
		}

		/* Rotation */
		{
			aipl_image_t dst;

			if (aipl_image_create(&dst, src.height, src.height, src.width,
					      src.format) != AIPL_ERR_OK) {
				LOG_ERR("Not enough memory for rotation destination image");
				continue;
			}

			benchmark_t rot_bench = create_rotation_benchmark();
			rot_op_arg_t rot_args;

			prepare_rotation(&src, &dst, &rot_args);
			bench_results[i][j] = perform_benchmark(&src, &dst, &rot_bench, &rot_args,
								bench_names[j]);
			++j;

			aipl_image_destroy(&dst);
		}

		if (image.format != i) {
			aipl_image_destroy(&src);
		}
	}

	LOG_INF("Benchmark complete");

	return 0;
}

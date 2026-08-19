

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if CONFIG_VIDEO

#include <zephyr/drivers/gpio.h>
#include <zephyr/shell/shell.h>
#include <zephyr/drivers/video.h>
#include <zephyr/pm/device.h>

#include <mpix/image.h>
#include <mpix/stats.h>
#include <mpix/auto.h>
#include <mpix/print.h>

#include "base64.h"

#define VIDEO_NODE DT_CHOSEN(zephyr_video)

#if !DT_NODE_HAS_STATUS(VIDEO_NODE, okay)
#error "Unsupported board: video devicetree node is not defined"
#endif

static const struct device *dev = DEVICE_DT_GET(VIDEO_NODE);
static int max_frames = 0;
static int frame = 0;
static bool is_initialized = false;
static bool convert = false;
static struct video_caps caps;

#define VIDEO_THREAD_STACK_SIZE 4096
static k_tid_t tid;
static struct k_thread video_thread;
K_THREAD_STACK_DEFINE(video_thread_stack, VIDEO_THREAD_STACK_SIZE);

struct video_buffer *buffers[1], *vbuf;
struct video_format fmt = {0};
uint8_t preview_buffer[100 * 1024];

void putc_f(char c)
{
	printf("%c", c);
}

static int libmpix_process(const struct shell *sh, struct video_buffer *vbuf_in,
			   struct video_format *fmt_in)
{
	struct mpix_image img = {};
	struct mpix_stats stats = {};
	struct mpix_auto_ctrls ctrls = {};
	struct mpix_format fmt = {};

	fmt.width = fmt_in->width;
	fmt.height = fmt_in->height;
	fmt.fourcc = MPIX_FMT_SBGGR8;

	/* Open the video buffer as an mpix image */
	mpix_image_from_buf(&img, vbuf_in->buffer, vbuf_in->bytesused, &fmt);

	/* Auto Operations */
	mpix_image_stats(&img, &stats);
	mpix_auto_black_level(&ctrls, &stats);
	mpix_auto_white_balance(&ctrls, &stats);

	/* Add the operations to the pipeline */
	mpix_image_debayer(&img, 3);
	mpix_image_correct_black_level(&img);
	mpix_image_correct_white_balance(&img);
	mpix_image_jpeg_encode(&img);

	mpix_image_ctrl_value(&img, MPIX_CID_BLACK_LEVEL, ctrls.black_level);
	mpix_image_ctrl_value(&img, MPIX_CID_RED_BALANCE, ctrls.red_balance_q10);
	mpix_image_ctrl_value(&img, MPIX_CID_BLUE_BALANCE, ctrls.blue_balance_q10);
	mpix_image_ctrl_value(&img, MPIX_CID_JPEG_QUALITY, MPIX_JPEG_QUALITY_DEFAULT);

	/* Convert the image and store it into the output buffer */
	uint32_t start = k_uptime_get_32();
	if (mpix_image_to_buf(&img, preview_buffer, sizeof(preview_buffer)) != 0) {
		shell_error(sh, "Failed to convert the image to JPEG");
		return -1;
	}
	uint32_t end = k_uptime_get_32();
	shell_print(sh, "Image converted to JPEG in %u ms", end - start);
	int data_size = mpix_ring_total_used(&img.last_op->ring);

	printf("\nJPEG FILE:\n");
	base64_encode((const char *)preview_buffer, data_size, putc_f);
	printf("\nJPEG END\n");
	printf("use libmpix_process, size: %d\n", data_size);

	mpix_image_free(&img);

	return 0;
}

static void video_cb(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);
	int ret = 0;
	const struct shell *sh = (const struct shell *)arg1;

	while (1) {
		ret = video_dequeue(dev, VIDEO_EP_OUT, &vbuf, K_FOREVER);
		if (ret != 0) {
			shell_error(sh, "Failed to dequeue the output buffer into %s", dev->name);
			break;
		}

		frame++;

		if (convert) {
			libmpix_process(sh, vbuf, &fmt);
		} else {
			shell_print(sh, "Got frame %u! size: %u; timestamp %u ms", frame,
				    vbuf->bytesused, vbuf->timestamp);
		}

		if (frame < max_frames || max_frames == -1) {
			ret = video_enqueue(dev, VIDEO_EP_OUT, vbuf);
			if (ret != 0) {
				shell_error(sh, "Failed to enqueue the input buffer into %s",
					    dev->name);
				break;
			}
			ret = video_stream_start(dev);
			if (ret != 0) {
				shell_error(sh, "Failed to start the video stream");
				break;
			}

		} else {
			ret = video_stream_stop(dev);
			shell_print(sh, "Capturing stopped");
		}
	}
}

static int cmd_get_device(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!dev) {
		shell_error(sh, "No video device found");
		return -ENODEV;
	}

	shell_print(sh, "Video Device: %s", dev->name);

	return err;
}

static int cmd_video_init(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;
	int i = 0;
	size_t bsize = 0;
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (is_initialized) {
		shell_error(sh, "Video device is already initialized");
		return -ENODEV;
	}

	if (!dev) {
		shell_error(sh, "No video device found");
		return -ENODEV;
	}

#ifdef CONFIG_PM_DEVICE
	pm_device_action_run(dev, PM_DEVICE_ACTION_RESUME);
#endif

	if (video_get_caps(dev, VIDEO_EP_OUT, &caps)) {
		shell_error(sh, "Unable to retrieve video capabilities");
		return -1;
	}

	while (caps.format_caps[i].pixelformat) {
		const struct video_format_cap *fcap = &caps.format_caps[i];
		/* fourcc to string */
		shell_print(sh,
			    "  %c%c%c%c width (min, max, step)[%u; %u; %u] "
			    "height (min, max, step)[%u; %u; %u]",
			    (char)fcap->pixelformat, (char)(fcap->pixelformat >> 8),
			    (char)(fcap->pixelformat >> 16), (char)(fcap->pixelformat >> 24),
			    fcap->width_min, fcap->width_max, fcap->width_step, fcap->height_min,
			    fcap->height_max, fcap->height_step);

		if (fcap->pixelformat == VIDEO_PIX_FMT_BGGR8) {
			fmt.pixelformat = VIDEO_PIX_FMT_BGGR8;
			fmt.width = fcap->width_min;
			fmt.height = fcap->height_min;
			fmt.pitch = fcap->width_min;
		}
		i++;
	}

	if (fmt.pixelformat != VIDEO_PIX_FMT_BGGR8) {
		shell_error(sh, "Desired Pixel format is not supported.");
		return -1;
	}

	err = video_set_format(dev, VIDEO_EP_OUT, &fmt);
	if (err) {
		shell_error(sh, "Failed to set video format. ret - %d", err);
		return err;
	}

	bsize = fmt.pitch * fmt.height;

	shell_print(sh, "- format: %c%c%c%c %ux%u", (char)fmt.pixelformat,
		    (char)(fmt.pixelformat >> 8), (char)(fmt.pixelformat >> 16),
		    (char)(fmt.pixelformat >> 24), fmt.width, fmt.height);

	for (i = 0; i < ARRAY_SIZE(buffers); i++) {
		buffers[i] = video_buffer_alloc(bsize);
		if (buffers[i] == NULL) {
			shell_error(sh, "Unable to alloc video buffer");
			return -1;
		}

		/* Allocated Buffer Information */
		shell_print(sh, "- addr - 0x%x, size - %d, bytesused - %d",
			    (uint32_t)buffers[i]->buffer, bsize, buffers[i]->bytesused);
	}

	tid = k_thread_create(&video_thread, video_thread_stack,
			      K_THREAD_STACK_SIZEOF(video_thread_stack), video_cb, (void *)sh, NULL,
			      NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);

	is_initialized = true;

	return err;
}

static int cmd_video_deinit(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (!dev) {
		shell_error(sh, "No video device found");
		return -ENODEV;
	}

	if (!is_initialized) {
		shell_error(sh, "Video device is not initialized");
		return -ENODEV;
	}

	video_flush(dev, VIDEO_EP_OUT, false);

	err = video_stream_stop(dev);
	if (err) {
		shell_error(sh, "Unable to stop capture (interface). ret - %d", err);
		return err;
	}

	for (int i = 0; i < ARRAY_SIZE(buffers); i++) {
		video_buffer_release(buffers[i]);
	}

#ifdef CONFIG_PM_DEVICE
	pm_device_action_run(dev, PM_DEVICE_ACTION_SUSPEND);
#endif

	is_initialized = false;
	return err;
}

static int cmd_video_take(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;
	int num = strtol(argv[1], NULL, 10);
	int show = strtol(argv[2], NULL, 10);
	if (!dev) {
		shell_error(sh, "No video device found");
		return -ENODEV;
	}
	if (!is_initialized) {
		shell_error(sh, "Video device is not initialized");
		return -ENODEV;
	}

	frame = 0;
	max_frames = num;

	for (int i = 0; i < ARRAY_SIZE(buffers); i++) {
		err = video_enqueue(dev, VIDEO_EP_OUT, buffers[i]);
		if (err) {
			shell_error(sh, "Unable to enqueue video buf");
			return -1;
		}
	}

	convert = show == 1;

	err = video_stream_start(dev);
	if (err) {
		shell_error(sh, "Unable to start capture (interface). ret - %d", err);
		return -1;
	}

	shell_print(sh, "Taking %d pictures", max_frames);

	return err;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_video, SHELL_CMD_ARG(get_device, NULL, "get the video device", cmd_get_device, 1, 0),
	SHELL_CMD_ARG(init, NULL, "initialize the video device", cmd_video_init, 1, 0),
	SHELL_CMD_ARG(deinit, NULL, "deinitialize the video device", cmd_video_deinit, 1, 0),
	SHELL_CMD_ARG(take, NULL, "take n pictures (-1: infinite 0: stop) show: take 10 1",
		      cmd_video_take, 3, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(video, &sub_video, "video device commands", NULL);

#endif
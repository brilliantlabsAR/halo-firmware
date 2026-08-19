/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/drivers/video.h>
#include <zephyr/drivers/video-controls.h>
#include <zephyr/linker/section_tags.h>

#include "lua.h"
#include "lauxlib.h"

#include <halo/lua_camera.h>
#include <halo/lua_service.h>
#include <halo/lua_runtime.h>

#include <mpix/image.h>
#include <mpix/print.h>
#include <mpix/zephyr.h>
#include <mpix/auto.h>
#include <mpix/lua.h>

LOG_MODULE_REGISTER(lua_camera, CONFIG_HALO_LOG_LEVEL);

#define LUA_CAMERA_SKIP_FRAMES 3

enum camera_thread_stage {
	CAM_STAGE_IDLE = 0,
	CAM_STAGE_WAIT_TRIGGER,
	CAM_STAGE_WARMUP,
	CAM_STAGE_DEQUEUE,
	CAM_STAGE_PROCESS,
	CAM_STAGE_STOPPING,
};

/*
 * Guard bytes appended to every DMA pixel buffer allocation.
 *
 * The Alif CPI DMA can flush its internal write-buffer up to one
 * 64-byte aligned burst past the last configured pixel when force-stopped
 * mid-frame (CONFIG_VIDEO_BUFFER_POOL_ALIGN = 64).  Without a guard zone the
 * overflow hits the sys_heap right-chunk metadata at:
 *   0x200e0068 (pool start + 104 B z_heap header) + 307200 (pitch×height)
 * corrupting the free-list → `left_chunk(h, right_chunk(h, c)) == c` assertion
 * fires in k_heap_free during camera_cleanup_resources() DEINIT.
 *
 * 128 bytes covers 2 × 64-byte aligned burst so any stop-timing race is safe.
 * vbuf->bytesused is set by cam_enqueue() to exactly pitch×height, so both the
 * driver and libmpix only see the real frame data.
 */
#define LUA_CAMERA_DMA_GUARD_BYTES 128

/* Forward declarations */
static int camera_service_event_handler(halo_lua_event_t event, void *user_data);

/* Register service with lifecycle management (power-managed service) */
HALO_LUA_SERVICE_DEFINE(camera_service, camera_service_event_handler, NULL, false);

/* Static JPEG buffer (115KB default, configurable) */
static uint8_t camera_jpeg_buffer[CONFIG_HALO_LUA_CAMERA_JPEG_BUFFER_SIZE];

/* ============================================================================
 * Camera State Management
 * ============================================================================ */

/**
 * @brief Camera state with integrated thread and resource management
 *
 * Centralizes all camera-related state for lifecycle management
 */
static struct {
	/* Video device and configuration */
	const struct device *video;
	struct video_caps caps;
	struct video_buffer *vbuf_raw;
	struct video_format fmt_raw;
	struct video_format fmt_jpeg;

	/* Thread management */
	k_tid_t capture_thread_tid;
	struct k_thread capture_thread;
	K_THREAD_STACK_MEMBER(capture_stack, CONFIG_HALO_LUA_VIDEO_TASK_STACK_SIZE);
	struct k_sem capture_sem;
	volatile bool thread_should_exit;

	/* JPEG buffer - points to static buffer */
	uint8_t *jpeg_buffer;
	size_t jpeg_data_size;
	size_t raw_pos;
	size_t jpeg_pos;

	/* State for libmpix */
	struct mpix_palette palette;

	/* State flags */
	int quality;
	bool ready;
	bool initialized;
	volatile bool capture_error;
	volatile enum camera_thread_stage thread_stage;
} camera_state = {
	.video = NULL,
	.vbuf_raw = NULL,
	.capture_thread_tid = NULL,
	.thread_should_exit = false,
	.jpeg_buffer = camera_jpeg_buffer,
	.jpeg_data_size = 0,
	.raw_pos = 0,
	.jpeg_pos = 0,
	.quality = 0,
	.ready = false,
	.initialized = false,
	.capture_error = false,
	.thread_stage = CAM_STAGE_IDLE,
};

/* ============================================================================
 * Image Processing (libmpix pipeline)
 * ============================================================================ */

/**
 * @brief Initialize the image processing library
 */
static int libmpix_init(void)
{
	int32_t *pipeline = lua_mpix_get_pipeline();
	int32_t *ctrls = lua_mpix_get_ctrls();
	size_t i = 0;

	/* Set the default pipeline */
	pipeline[++i] = MPIX_OP_DEBAYER_2X2;
	pipeline[++i] = MPIX_OP_CORRECT_BLACK_LEVEL;
	pipeline[++i] = MPIX_OP_CORRECT_WHITE_BALANCE;
	pipeline[++i] = MPIX_OP_JPEG_ENCODE;
	pipeline[0] = i;

	/* Set the default controls */
	ctrls[MPIX_CID_BLACK_LEVEL] = 10;
	ctrls[MPIX_CID_RED_BALANCE] = 1.2 * (1 << 10);
	ctrls[MPIX_CID_BLUE_BALANCE] = 1.8 * (1 << 10);

	return 0;
}

/**
 * @brief Process raw image buffer through libmpix pipeline
 *
 * Applies: debayer, black level correction, white balance correction, JPEG encode
 * Lua is expected to provide the auto-tuning and can override the operations performed.
 */
static int libmpix_process(struct video_buffer *vbuf_in, struct video_format *fmt_in)
{
	struct mpix_image img = {};
	struct mpix_stats stats = {};
	struct mpix_auto_ctrls auto_ctrls = {};
	struct mpix_format fmt = {};
	int ret;

	if (!camera_state.jpeg_buffer) {
		LOG_ERR("JPEG buffer not allocated");
		return -ENOMEM;
	}

	/* Setup format for libmpix */
	fmt.width = fmt_in->width;
	fmt.height = fmt_in->height;
	fmt.fourcc = MPIX_FMT_SBGGR8;

	/* Open the video buffer as an mpix image */
	mpix_image_from_buf(&img, vbuf_in->buffer, vbuf_in->bytesused, &fmt);

	/* Auto Operations */
	mpix_image_stats(&img, &stats);
	mpix_auto_black_level(&auto_ctrls, &stats);
	mpix_auto_white_balance(&auto_ctrls, &stats);

	int32_t *ctrls = lua_mpix_get_ctrls();
	ctrls[MPIX_CID_BLACK_LEVEL] = auto_ctrls.black_level;
	ctrls[MPIX_CID_RED_BALANCE] = auto_ctrls.red_balance_q10;
	ctrls[MPIX_CID_BLUE_BALANCE] = auto_ctrls.blue_balance_q10;

	lua_mpix_set_stats(&stats);

	/* Add the operations to the pipeline */
	int32_t *pipeline = lua_mpix_get_pipeline();
	ret = mpix_pipeline_add_array(&img, &pipeline[1], pipeline[0]);
	if (ret) {
		MPIX_ERR("Failed to apply the pipeline");
		return EXIT_FAILURE;
	}

	/* Apply control values */
	ctrls[MPIX_CID_BLACK_LEVEL] = auto_ctrls.black_level;
	ctrls[MPIX_CID_RED_BALANCE] = auto_ctrls.red_balance_q10;
	ctrls[MPIX_CID_BLUE_BALANCE] = auto_ctrls.blue_balance_q10;

	for (int i = 0; i < MPIX_NB_CID; i++) {
		mpix_image_ctrl_value(&img, i, ctrls[i]);
	}

	mpix_image_ctrl_value(&img, MPIX_CID_JPEG_QUALITY, camera_state.quality);

	/* Run hooks with extra configuration */
	lua_mpix_hooks(&img, &camera_state.palette);

	/* Convert the image and store it into the output buffer */
	ret = mpix_image_to_buf(&img, camera_state.jpeg_buffer,
				CONFIG_HALO_LUA_CAMERA_JPEG_BUFFER_SIZE);
	if (ret) {
		LOG_ERR("Failed to encode JPEG: %d", ret);
		mpix_image_free(&img);
		return ret;
	}

	camera_state.jpeg_data_size = mpix_ring_total_used(&img.last_op->ring);

	if (CONFIG_HALO_LOG_LEVEL >= LOG_LEVEL_INF) {
		mpix_print_pipeline(img.first_op);
	}

	mpix_image_free(&img);

	return 0;
}

/* ============================================================================
 * Camera Capture Thread
 * ============================================================================ */

static int camera_dequeue_cancelable(struct video_buffer **vbuf)
{
	int waited_ms = 0;

	while (true) {
		/* Compiler barrier: force re-read of thread_should_exit from memory */
		__asm__ __volatile__("" ::: "memory");
		if (camera_state.thread_should_exit) {
			return -ECANCELED;
		}

		/*
		 * Use non-blocking dequeue and short sleep instead of a timed blocking
		 * dequeue. This keeps the thread responsive to thread_should_exit even if
		 * the driver blocks internally on timed dequeue during stream stop/flush.
		 */
		int ret = video_dequeue(camera_state.video, VIDEO_EP_OUT, vbuf, K_NO_WAIT);
		if (ret == 0) {
			return 0;
		}

		if (ret == -EAGAIN) {
			k_sleep(K_MSEC(10));
			waited_ms += 10;
			if (waited_ms >= 1000) {
				return ret;
			}
			continue;
		}

		return ret;
	}

	return -ECANCELED;
}

/**
 * @brief Camera capture thread function
 *
 * Waits for capture triggers, acquires frames, and processes with libmpix
 */
static void camera_capture_thread(void *p0, void *p1, void *p2)
{
	ARG_UNUSED(p0);
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);

	struct video_buffer *vbuf;
	int ret;

	while (true) {
		camera_state.thread_stage = CAM_STAGE_WAIT_TRIGGER;

		/* Exit promptly even if no trigger arrives */
		if (camera_state.thread_should_exit) {
			LOG_DBG("Capture thread exit while waiting trigger");
			break;
		}
		/* Wait for capture trigger */
		int wait_ret = k_sem_take(&camera_state.capture_sem, K_MSEC(10));
		if (wait_ret != 0) {
			continue;
		}
		LOG_DBG("Capture thread woke by trigger/stop signal");

		/* Check exit flag */
		if (camera_state.thread_should_exit) {
			LOG_DBG("Capture thread exit after wake signal");
			break;
		}

		/* Check if video buffer is allocated */
		if (camera_state.vbuf_raw == NULL) {
			LOG_ERR("Video buffer not allocated - capture() must be called first");
			camera_state.ready = false;
			continue;
		}

		/* Clear ready flag */
		camera_state.ready = false;

		/* Skip frames for warmup */
		camera_state.thread_stage = CAM_STAGE_WARMUP;
		for (int i = 0; i < LUA_CAMERA_SKIP_FRAMES; i++) {
			if (camera_state.thread_should_exit) {
				goto capture_cancel;
			}

			/* Enqueue raw buffer */
			ret = video_enqueue(camera_state.video, VIDEO_EP_OUT,
					    camera_state.vbuf_raw);
			if (ret) {
				LOG_ERR("Failed to enqueue video buffer: %d", ret);
				goto capture_error;
			}

			/* Start stream */
			ret = video_stream_start(camera_state.video);
			if (ret) {
				LOG_ERR("Failed to start video stream: %d", ret);
				goto capture_error;
			}

			/* Dequeue frame */
			camera_state.thread_stage = CAM_STAGE_DEQUEUE;
			ret = camera_dequeue_cancelable(&vbuf);
			if (ret == -ECANCELED) {
				goto capture_cancel;
			}
			if (ret) {
				LOG_ERR("Failed to dequeue video buffer: %d", ret);
				goto capture_error;
			}
		}

		if (camera_state.thread_should_exit) {
			goto capture_cancel;
		}

		/* Stop stream */
		video_stream_stop(camera_state.video);

		/* Process image through libmpix pipeline */
		camera_state.thread_stage = CAM_STAGE_PROCESS;

		if (camera_state.thread_should_exit) {
			LOG_DBG("Capture thread exit before PROCESS");
			goto capture_cancel;
		}

		ret = libmpix_process(vbuf, &camera_state.fmt_raw);
		if (ret == 0) {
			if (camera_state.thread_should_exit) {
				LOG_DBG("Capture thread exit after PROCESS");
				camera_state.ready = false;
				continue;
			}
			camera_state.ready = true;
			camera_state.raw_pos = 0;
			camera_state.jpeg_pos = 0;
		} else {
			LOG_ERR("Image processing failed: %d", ret);
		}

		continue;

capture_cancel:
		camera_state.thread_stage = CAM_STAGE_STOPPING;
		video_stream_stop(camera_state.video);
		camera_state.ready = false;
		continue;

capture_error:
		camera_state.thread_stage = CAM_STAGE_STOPPING;
		/* Flush and drain queues so the next capture starts cleanly. */
		video_flush(camera_state.video, VIDEO_EP_OUT, true);

		/* Drain output FIFO after flush. */
		struct video_buffer *stale;

		while (video_dequeue(camera_state.video, VIDEO_EP_OUT,
				     &stale, K_NO_WAIT) == 0) {
		}

		camera_state.ready = false;
		camera_state.capture_error = true;
		LOG_ERR("Capture error - flag set for Lua thread");
	}

	camera_state.thread_stage = CAM_STAGE_IDLE;
}

/* ============================================================================
 * Thread Management
 * ============================================================================ */

/**
 * @brief Start camera capture thread
 */
static int camera_thread_start(void)
{
	if (camera_state.capture_thread_tid != NULL) {
		LOG_WRN("Capture thread already running");
		return -EALREADY;
	}

	camera_state.thread_should_exit = false;

	camera_state.capture_thread_tid = k_thread_create(
		&camera_state.capture_thread, camera_state.capture_stack,
		K_THREAD_STACK_SIZEOF(camera_state.capture_stack), camera_capture_thread, NULL,
		NULL, NULL, CONFIG_HALO_LUA_VIDEO_TASK_PRIORITY, 0, K_NO_WAIT);

	if (camera_state.capture_thread_tid == NULL) {
		LOG_ERR("Failed to create capture thread");
		return -ENOMEM;
	}

	k_thread_name_set(&camera_state.capture_thread, "camera_capture");
	LOG_DBG("Capture thread started tid=%p", camera_state.capture_thread_tid);

	return 0;
}

/**
 * @brief Stop camera capture thread
 */
static int camera_thread_stop(void)
{
	if (camera_state.capture_thread_tid == NULL) {
		LOG_DBG("Capture thread stop: already stopped");
		return 0; /* Already stopped */
	}

	LOG_DBG("Capture thread stop begin tid=%p stage=%d should_exit=%d",
		camera_state.capture_thread_tid, camera_state.thread_stage,
		camera_state.thread_should_exit);

	/* Reset semaphore first to ensure k_sem_give() triggers 0→1 transition */
	k_sem_reset(&camera_state.capture_sem);

	/* Set exit flag and wake thread */
	camera_state.thread_should_exit = true;
	k_sem_give(&camera_state.capture_sem);
	k_wakeup(camera_state.capture_thread_tid);
	LOG_DBG("Capture thread stop signal sent");

	for (int i = 0; i < 3; i++) { 
		int ret = k_thread_join(&camera_state.capture_thread, K_MSEC(100));
		if (ret == 0) {
			LOG_DBG("Capture thread stopped naturally at iteration %d stage=%d",
				i, camera_state.thread_stage);
			camera_state.capture_thread_tid = NULL;
			return 0;
		}

		LOG_DBG("Waiting for thread to finish stage=%d iteration=%d",
			camera_state.thread_stage, i);
	}

	if (camera_state.thread_stage == CAM_STAGE_WAIT_TRIGGER) {
		LOG_WRN("Capture thread stuck in WAIT_TRIGGER, aborting safely");
		k_thread_abort(camera_state.capture_thread_tid);
		k_thread_join(&camera_state.capture_thread, K_FOREVER);
		camera_state.capture_thread_tid = NULL;
		return 0;
	}

	for (int i = 0; i < 7; i++) { 
		int ret = k_thread_join(&camera_state.capture_thread, K_MSEC(100));
		if (ret == 0) {
			LOG_DBG("Capture thread stopped during extended wait at iteration %d stage=%d",
				i, camera_state.thread_stage);
			camera_state.capture_thread_tid = NULL;
			return 0;
		}

		LOG_DBG("Extended wait for thread to finish stage=%d iteration=%d",
			camera_state.thread_stage, i);
	}

	LOG_ERR("Capture thread timeout after 1s, stage=%d, forcing abort",
		camera_state.thread_stage);
	k_thread_abort(camera_state.capture_thread_tid);
	k_thread_join(&camera_state.capture_thread, K_FOREVER);
	camera_state.capture_thread_tid = NULL;
	return -ETIMEDOUT;
}

/* ============================================================================
 * Resource Cleanup
 * ============================================================================ */

/**
 * @brief Clean up camera runtime resources.
 */
static void camera_cleanup_resources(void)
{
	int ret;
	bool thread_stopped_cleanly = true;

	/* Stop capture thread */
	ret = camera_thread_stop();
	if (ret < 0) {
		LOG_WRN("Capture thread stop during cleanup failed: %d", ret);
		thread_stopped_cleanly = false;
	}

	if (!thread_stopped_cleanly) {
		/* Keep buffers if the worker did not stop cleanly. */
		camera_state.ready = false;
		camera_state.capture_error = true;
		return;
	}

	/* Drain driver FIFOs before releasing buffers. */
	if (camera_state.video != NULL) {
		struct video_buffer *stale;

		while (video_dequeue(camera_state.video, VIDEO_EP_OUT,
				     &stale, K_NO_WAIT) == 0) {
		}
	}

	/* Free video buffer */
	if (camera_state.vbuf_raw) {
		video_buffer_release(camera_state.vbuf_raw);
		camera_state.vbuf_raw = NULL;
	}

	/* Reset video format to force reallocation on next capture */
	memset(&camera_state.fmt_raw, 0, sizeof(camera_state.fmt_raw));
	memset(&camera_state.fmt_jpeg, 0, sizeof(camera_state.fmt_jpeg));

	/* Reset state flags */
	camera_state.ready = false;
	camera_state.raw_pos = 0;
	camera_state.jpeg_pos = 0;

	/* JPEG buffer is static, no need to free */
}

/* ============================================================================
 * Hardware Initialization and Power Management
 * ============================================================================ */

/**
 * @brief Initialize camera hardware
 */
static int camera_hardware_init(void)
{
	if (camera_state.initialized) {
		return 0;
	}

	/* Get video device */
	camera_state.video = DEVICE_DT_GET(DT_CHOSEN(zephyr_video));
	if (!device_is_ready(camera_state.video)) {
		LOG_ERR("Video device not ready");
		return -ENODEV;
	}

	/* Suspend video device initially */
#ifdef CONFIG_PM_DEVICE
	pm_device_action_run(camera_state.video, PM_DEVICE_ACTION_SUSPEND);
#endif
	/* Note: is_active state managed by service framework */

	camera_state.initialized = true;

	return 0;
}

/* ============================================================================
 * Lua API Functions
 * ============================================================================ */

/**
 * @brief Lua: frame.camera.capture(options)
 *
 * Trigger camera capture with optional resolution and quality settings
 */
static int lua_camera_capture(lua_State *L)
{
	int ret = 0;
	int i = 0;

	LOG_DBG("Camera capture started");

	if (!camera_state.initialized) {
		return luaL_error(L, "Camera not initialized");
	}

	if (!camera_service.is_active) {
		return luaL_error(L, "camera is asleep");
	}

	if (camera_state.jpeg_buffer == NULL) {
		return luaL_error(L, "JPEG buffer not allocated");
	}

	/* Ensure capture thread is running */
	if (camera_state.capture_thread_tid == NULL) {
		ret = camera_thread_start();
		if (ret < 0) {
			return luaL_error(L, "Failed to start capture thread: %d", ret);
		}
	}

	uint16_t resolution = 640;

	/* Parse resolution option */
	if (lua_getfield(L, 1, "resolution") != LUA_TNIL) {
		resolution = luaL_checkinteger(L, -1);
		lua_pop(L, 1);

		if (resolution != 640) {
			return luaL_error(L, "camera resolution not supported");
		}
	}

	/* Parse quality option */
	if (lua_getfield(L, 1, "quality") != LUA_TNIL) {
		const char *string = luaL_checkstring(L, -1);
		lua_pop(L, 1);

		/* Maps to the JPEG encoder's quantization scaling
		 * (JPEGE_Q_BEST=0 .. JPEGE_Q_LOW=3). Only four hardware
		 * levels exist, so LOW and VERY_LOW are the same level.
		 * Verified on the dev kit 2026-08-14: 640px capture encoded to
		 * ~80/47/25/16/16 KB across the five settings. */
		if (strcmp(string, "VERY_HIGH") == 0) {
			camera_state.quality = 0;
		} else if (strcmp(string, "HIGH") == 0) {
			camera_state.quality = 1;
		} else if (strcmp(string, "MEDIUM") == 0) {
			camera_state.quality = 2;
		} else if (strcmp(string, "LOW") == 0) {
			camera_state.quality = 3;
		} else if (strcmp(string, "VERY_LOW") == 0) {
			camera_state.quality = 3;
		} else {
			return luaL_error(
				L,
				"quality must be either VERY_HIGH, HIGH, MEDIUM, LOW or VERY_LOW");
		}
	}

	/* Set video format if resolution changed */
	if (camera_state.fmt_raw.width != resolution) {

		/* Get capabilities */
		ret = video_get_caps(camera_state.video, VIDEO_EP_OUT, &camera_state.caps);
		if (ret) {
			LOG_ERR("Unable to retrieve video capabilities: %d", ret);
			return luaL_error(L, "Failed to get video capabilities: %d", ret);
		}

		/* Find matching format */
		while (camera_state.caps.format_caps[i].pixelformat) {
			const struct video_format_cap *fcap = &camera_state.caps.format_caps[i];
			if (fcap->width_max == resolution) {

				camera_state.fmt_raw.pixelformat = fcap->pixelformat;
				camera_state.fmt_raw.width = fcap->width_max;
				camera_state.fmt_raw.height = fcap->height_max;
				camera_state.fmt_raw.pitch = fcap->width_max;

				camera_state.fmt_jpeg.pixelformat = VIDEO_PIX_FMT_JPEG;
				camera_state.fmt_jpeg.width = fcap->width_max;
				camera_state.fmt_jpeg.height = fcap->height_max;
				camera_state.fmt_jpeg.pitch =
					fcap->width_max * fcap->height_max / 2;
				break;
			}
			i++;
		}

		ret = video_set_format(camera_state.video, VIDEO_EP_OUT, &camera_state.fmt_raw);
		if (ret) {
			return luaL_error(L, "Failed to set video format: %d", ret);
		}

		/* Allocate video buffer with guard bytes at the end.
		 * LUA_CAMERA_DMA_GUARD_BYTES of padding prevents the CPI DMA's
		 * in-flight write-buffer flush from corrupting the sys_heap
		 * right-chunk header that sits immediately after pitch*height bytes.
		 * See LUA_CAMERA_DMA_GUARD_BYTES definition for details.
		 */
		if (camera_state.vbuf_raw) {
			video_buffer_release(camera_state.vbuf_raw);
		}

		camera_state.vbuf_raw = video_buffer_alloc(
			camera_state.fmt_raw.pitch * camera_state.fmt_raw.height +
			LUA_CAMERA_DMA_GUARD_BYTES);
		if (!camera_state.vbuf_raw) {
			return luaL_error(L, "Failed to allocate video buffer");
		}
	}

	/* Clear error flag before triggering */
	camera_state.capture_error = false;

	/* Trigger capture */
	k_sem_give(&camera_state.capture_sem);

	return 0;
}

/**
 * @brief Lua: frame.camera.image_ready()
 *
 * Check if captured image is ready for reading
 */
static int lua_camera_image_ready(lua_State *L)
{
	if (!camera_service.is_active) {
		return luaL_error(L, "camera is asleep");
	}

	k_yield();

	/* Check if capture thread reported an error */
	if (camera_state.capture_error) {
		camera_state.capture_error = false;
		return luaL_error(L, "Capture error occurred");
	}

	lua_pushboolean(L, camera_state.ready);
	return 1;
}

/**
 * @brief Lua: frame.camera.read(bytes)
 *
 * Read JPEG encoded image data
 */
static int lua_camera_read(lua_State *L)
{
	lua_Integer bytes_requested = luaL_checkinteger(L, 1);
	if (bytes_requested <= 0) {
		return luaL_error(L, "bytes must be greater than 0");
	}

	if (camera_state.jpeg_buffer == NULL) {
		return luaL_error(L, "JPEG buffer not allocated");
	}

	size_t remaining =
		MIN(bytes_requested, camera_state.jpeg_data_size - camera_state.jpeg_pos);
	if (remaining > 0) {
		lua_pushlstring(L, (char *)camera_state.jpeg_buffer + camera_state.jpeg_pos,
				remaining);
		camera_state.jpeg_pos += remaining;
	} else {
		lua_pushnil(L);
	}

	return 1;
}

/**
 * @brief Lua: frame.camera.read_raw(bytes)
 *
 * Read raw sensor data
 */
static int lua_camera_read_raw(lua_State *L)
{
	lua_Integer bytes_requested = luaL_checkinteger(L, 1);
	if (bytes_requested <= 0) {
		return luaL_error(L, "bytes must be greater than 0");
	}

	if (camera_state.vbuf_raw == NULL) {
		return luaL_error(L, "raw buffer not set");
	}

	size_t remaining =
		MIN(bytes_requested, camera_state.vbuf_raw->bytesused - camera_state.raw_pos);
	if (remaining > 0) {
		lua_pushlstring(L, (char *)camera_state.vbuf_raw->buffer + camera_state.raw_pos,
				remaining);
		camera_state.raw_pos += remaining;
	} else {
		lua_pushnil(L);
	}

	return 1;
}

/**
 * @brief Lua: frame.camera.power_save(enable)
 *
 * Enable/disable camera power save mode
 */
static int lua_camera_power_save(lua_State *L)
{
	if (!lua_isboolean(L, 1)) {
		return luaL_error(L, "value must be true or false");
	}

	bool enable = lua_toboolean(L, 1);

	/* Check if already in desired state */
	bool is_sleeping = !camera_service.is_active;
	if (is_sleeping == enable) {
		return 0; /* Already in desired state */
	}

	LOG_DBG("Camera power save: %s", enable ? "enabled" : "disabled");

	/* Use unified service power save API */
	int ret = halo_lua_service_power_save(&camera_service, enable);

	if (ret < 0) {
		return luaL_error(L, "Failed to %s power save mode: %d",
				  enable ? "enter" : "exit", ret);
	}

	return 0;
}

/* ============================================================================
 * Service Lifecycle Management
 * ============================================================================ */

/**
 * @brief Service lifecycle event handler
 *
 * Handles lifecycle events like INIT, DEINIT, INTERRUPT, RESTART
 */
static int camera_service_event_handler(halo_lua_event_t event, void *user_data)
{
	ARG_UNUSED(user_data);

	int ret = 0;

	switch (event) {
	case HALO_LUA_EVENT_INIT:

		/* Initialize semaphore */
		k_sem_init(&camera_state.capture_sem, 0, 1);

		/* JPEG buffer is statically allocated */

		/* Initialize hardware */
		ret = camera_hardware_init();
		if (ret < 0) {
			return ret;
		}

		/* Configure the libmpix initial parameters */
		ret = libmpix_init();
		if (ret != 0) {
			LOG_ERR("Failed to initialize the libmpix library");
		}

		/* Start in power save mode until Lua enables camera. */
		camera_service.is_active = false;
		break;

	case HALO_LUA_EVENT_DEINIT:

		/* Cleanup all resources */
		camera_cleanup_resources();

		/* Mark as uninitialized */
		camera_state.initialized = false;
		break;

	case HALO_LUA_EVENT_INTERRUPT:

		/* Cleanup runtime resources only */
		camera_cleanup_resources();

		/* Keep init state for restart path. */
		break;

	case HALO_LUA_EVENT_SUSPEND:
		/* Check if camera is currently active (not in power save) */
		bool was_active = camera_service.is_active;

		if (was_active) {
			/* Block new capture calls before stopping the worker thread. */
			camera_service.is_active = false;

			/* Stop capture thread before suspend. */
			ret = camera_thread_stop();
			if (ret < 0) {
				LOG_ERR("Failed to stop capture thread before suspend: %d", ret);
				return ret;
			}

#ifdef CONFIG_PM_DEVICE
			if (camera_state.video != NULL) {
				ret = pm_device_action_run(camera_state.video, PM_DEVICE_ACTION_SUSPEND);
				if (ret != 0 && ret != -EALREADY && ret != -ENOSYS) {
					LOG_ERR("Failed to suspend video device: %d", ret);
					return ret;
				}
			}
#endif
		}
		/* Return 0 if active before suspend, otherwise 1. */
		ret = was_active ? 0 : 1;
		break;

	case HALO_LUA_EVENT_RESUME:
		/* Note: Only called if SUSPEND returned 0 (i.e. was_active was true) */
#ifdef CONFIG_PM_DEVICE
		if (camera_state.video != NULL) {
			ret = pm_device_action_run(camera_state.video, PM_DEVICE_ACTION_RESUME);
			if (ret != 0 && ret != -EALREADY && ret != -ENOSYS) {
				LOG_ERR("Failed to resume video device: %d", ret);
			}
		}
#endif
		/* Restore active state so Lua can use the camera after resuming */
		camera_service.is_active = true;
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
 * @brief Open and register camera library with Lua VM
 *
 * Registers all camera functions under frame.camera table
 * API compatibility: frame.camera.*
 */
int lua_open_camera_library(lua_State *L)
{

	/* Register service for lifecycle management */
	int ret = halo_lua_service_register(&camera_service);
	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("Failed to register camera service: %d", ret);
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

	/* Create 'camera' subtable */
	lua_newtable(L);

	/* Register camera functions */
	lua_pushcfunction(L, lua_camera_capture);
	lua_setfield(L, -2, "capture");

	lua_pushcfunction(L, lua_camera_image_ready);
	lua_setfield(L, -2, "image_ready");

	lua_pushcfunction(L, lua_camera_read);
	lua_setfield(L, -2, "read");

	lua_pushcfunction(L, lua_camera_read_raw);
	lua_setfield(L, -2, "read_raw");

	lua_pushcfunction(L, lua_camera_power_save);
	lua_setfield(L, -2, "power_save");

	luaopen_mpix(L);
	lua_setfield(L, -2, "mpix");

	/* Set 'camera' table in 'frame' */
	lua_setfield(L, -2, "camera");

	/* Pop frame table */
	lua_pop(L, 1);

	return 0;
}

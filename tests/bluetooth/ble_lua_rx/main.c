/* Copyright (c) 2026 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * Host-side reproduction and regression test for the Lua RX GATT write
 * handler (modules/halo/src/ble_lua.c, LUA_IDX_RX_VAL / LUA_IDX_AUDIO_RX_VAL).
 *
 * The handler body is transcribed here and driven with the
 * (offset, data, len) triples an ATT write hands the callback. Each payload
 * is placed in a heap buffer of *exactly* len bytes, so a copy that reads
 * past it is caught by AddressSanitizer instead of silently tolerated. A
 * faithful stand-in for Zephyr's ring_buf_put reproduces its defining
 * property: it copies MIN(size, free) bytes and reads them from the source
 * pointer.
 *
 * Two builds, selected by -DVULNERABLE:
 *   make run    - production logic; every check must PASS (exit 0).
 *   make repro  - pre-fix logic; AddressSanitizer aborts in the zero-length
 *                 case, demonstrating the length underflow the fix removes.
 *
 * This is a fast, hardware-free guard on the length arithmetic and routing.
 * End-to-end behaviour against real firmware (whether the ROM forwards a
 * zero-length write or an offset write to the callback at all) is covered by
 * applications/halo/tests/test_ble_rx_write.py on a dev kit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* --- ring_buf stand-in --------------------------------------------------- *
 * The bug depends on exactly one ring_buf_put property: it copies
 * MIN(size, free) bytes and READS them from `src`. With size == 0xFFFFFFFF
 * (from a uint16 `len - 1` at len == 0) that is `free` bytes out of a
 * zero-length source - the out-of-bounds read this test exists to catch. */
struct ring {
	uint8_t *buf;
	uint32_t cap, len;
};
static void ring_init(struct ring *r, uint32_t cap)
{
	r->buf = malloc(cap);
	r->cap = cap;
	r->len = 0;
}
static uint32_t ring_buf_space_get(const struct ring *r) { return r->cap - r->len; }
static void ring_buf_reset(struct ring *r) { r->len = 0; }
static uint32_t ring_buf_put(struct ring *r, const uint8_t *src, uint32_t size)
{
	uint32_t space = ring_buf_space_get(r);
	uint32_t n = size < space ? size : space; /* MIN(size, free) */
	memcpy(r->buf + r->len, src, n);          /* reads n bytes from src   */
	r->len += n;
	return n;
}

/* markers, mirrored from modules/halo/include/halo/ble_lua.h */
#define HALO_LUA_CTRL_DATA_MARKER 0x01
#define HALO_LUA_CTRL_REBOOT      0x04
#define REPL_CAP 1024
#define DATA_CAP 4096

static struct ring g_repl, g_data;
#ifdef VULNERABLE
static struct ring *g_current; /* the pre-fix continuation state machine */
#endif
static bool g_paired = true;

/* Transcription of the LUA_IDX_RX_VAL case body. Returns an ATT status; the
 * newline is folded into the REPL store exactly as the handler does. */
static int rx_write(uint16_t offset, const uint8_t *data, uint16_t len)
{
	if (!g_paired) {
		return 0x05; /* ATT_ERR_INSUFF_AUTHEN */
	}

#ifndef VULNERABLE
	/* --- production logic ------------------------------------------- */
	if (offset != 0) {
		return 0x07; /* ATT_ERR_INVALID_OFFSET */
	}
	if (len == 0) {
		return 0x00;
	}
	if (data[0] == HALO_LUA_CTRL_REBOOT) {
		ring_buf_reset(&g_repl);
		ring_buf_reset(&g_data);
		return 0x00;
	}
	if (data[0] == HALO_LUA_CTRL_DATA_MARKER && len >= 2) {
		uint16_t payload_len = len - 1;
		if (ring_buf_space_get(&g_data) < payload_len) {
			return 0x11; /* ATT_ERR_INSUFF_RESOURCE */
		}
		ring_buf_put(&g_data, data + 1, payload_len);
	} else {
		uint32_t needed = (uint32_t)len + 1;
		if (ring_buf_space_get(&g_repl) < needed) {
			return 0x11;
		}
		ring_buf_put(&g_repl, data, len);
		uint8_t nl = '\n';
		ring_buf_put(&g_repl, &nl, 1);
	}
	return 0x00;
#else
	/* --- pre-fix logic (main @ b4f0dcd) ----------------------------- */
	struct ring *target_ring;
	if (offset == 0 && len > 0) {
		if (data[0] == HALO_LUA_CTRL_REBOOT) {
			ring_buf_reset(&g_repl);
			ring_buf_reset(&g_data);
			return 0x00;
		}
		if (data[0] == HALO_LUA_CTRL_DATA_MARKER && len >= 2) {
			target_ring = &g_data;
		} else {
			target_ring = &g_repl;
			len++; /* reserve for '\n' */
		}
	} else {
		target_ring = (g_current == &g_data) ? &g_data : &g_repl;
	}
	g_current = target_ring;
	if (ring_buf_space_get(target_ring) < len) {
		return 0x11;
	}
	if (target_ring == &g_repl) {
		ring_buf_put(target_ring, data, len - 1); /* underflow at len==0 */
		uint8_t nl = '\n';
		ring_buf_put(target_ring, &nl, 1);
	} else {
		if (offset == 0 && data[0] == HALO_LUA_CTRL_DATA_MARKER) {
			ring_buf_put(target_ring, data + 1, len - 1);
		} else {
			ring_buf_put(target_ring, data, len);
		}
	}
	return 0x00;
#endif
}

/* Hand rx_write a buffer of EXACTLY len bytes so any over-read is real. */
static int att_write(uint16_t offset, const void *bytes, uint16_t len)
{
	uint8_t *p = malloc(len ? len : 1);
	if (len) memcpy(p, bytes, len);
	int rc = rx_write(offset, p, len);
	free(p);
	return rc;
}

static int g_fail;
#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		printf((cond) ? "  PASS: " : "  FAIL: ");                       \
		printf(__VA_ARGS__);                                            \
		printf("\n");                                                   \
		if (!(cond)) g_fail++;                                          \
	} while (0)

static void reset_all(void)
{
	ring_buf_reset(&g_repl);
	ring_buf_reset(&g_data);
#ifdef VULNERABLE
	g_current = NULL;
#endif
}

static const char *repl_str(char *out, size_t cap)
{
	uint32_t n = g_repl.len < cap ? g_repl.len : (uint32_t)cap - 1;
	memcpy(out, g_repl.buf, n);
	out[n] = 0;
	return out;
}

int main(void)
{
	ring_init(&g_repl, REPL_CAP);
	ring_init(&g_data, DATA_CAP);
	char got[128];

#ifdef VULNERABLE
	printf("[pre-fix build - expect an AddressSanitizer abort]\n");
#else
	printf("[production build]\n");
#endif

	/* 1. Zero-length write, REPL current. Pre-fix: len-1 == 0xFFFFFFFF
	 *    drains REPL_CAP bytes out of a zero-length source (ASan aborts). */
	reset_all();
	att_write(0, "print()", 7);
	uint32_t repl_before = g_repl.len;
	att_write(1, "", 0);
	CHECK(g_repl.len == repl_before, "zero-length write stores nothing");

	/* 2. Zero-length write, DATA current. */
	reset_all();
	att_write(0, "\x01Q", 2);
	uint32_t data_before = g_data.len;
	att_write(1, "", 0);
	CHECK(g_data.len == data_before, "zero-length data write stores nothing");

	/* 3. A single REPL statement arrives as exactly one '\n'-terminated
	 *    line - no lost byte, no spurious interior newline. */
	reset_all();
	att_write(0, "print('hello world')", 20);
	CHECK(strcmp(repl_str(got, sizeof got), "print('hello world')\n") == 0,
	      "single statement is one clean line -> \"%s\"", got);

	/* 4. An offset (long-write) fragment is refused, not appended. Under
	 *    NO_OFFSET the controller never delivers this, but the handler
	 *    rejects it defensively rather than mis-framing the stream. */
	reset_all();
	att_write(0, "print('hel", 10);
	int rc = att_write(10, "lo world')", 10);
	CHECK(rc == 0x07, "offset write rejected with ATT_ERR_INVALID_OFFSET (rc=0x%02x)", rc);
	CHECK(strcmp(repl_str(got, sizeof got), "print('hel\n") == 0,
	      "rejected fragment left no partial state -> \"%s\"", got);

	/* 5. Framed data write stores the payload with the marker stripped. */
	reset_all();
	att_write(0, "\x01" "ABCD", 5);
	CHECK(g_data.len == 4 && memcmp(g_data.buf, "ABCD", 4) == 0,
	      "data marker stripped, 4 payload bytes stored (got %u)", g_data.len);

	/* 6. Unpaired write is refused. */
	reset_all();
	g_paired = false;
	rc = att_write(0, "print(1)", 8);
	g_paired = true;
	CHECK(rc == 0x05 && g_repl.len == 0, "unpaired write rejected (rc=0x%02x)", rc);

	printf(g_fail ? "\nFAILED (%d)\n" : "\nOK\n", g_fail);
	return g_fail ? 1 : 0;
}

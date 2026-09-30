/*
 * Copyright (c) 2026 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 *
 * Halo main-board factory test.
 *
 * Runs every test once at boot and reports on the console (115200 8N1):
 *
 *   FT BEGIN halo-mainboard-test <version> <commit>
 *   FT <test> PASS|FAIL|SKIP <detail>
 *   ...
 *   FT DONE <passed>/<total> skip=<skipped>
 *
 * <total> counts PASS and FAIL; SKIP means "not fitted / not applicable".
 * The shell stays up afterwards: `factory run` repeats the whole sequence and
 * `factory <test>` repeats one test. The BLE advertiser keeps running after
 * FT DONE so the station can scan for it (see README.md).
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/audio/dmic.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/pm/device.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>

#include <se_service.h>
#include <t5838.h>

#include "ble_adv.h"

enum ft_result {
	FT_PASS,
	FT_FAIL,
	FT_SKIP,
};

struct ft_ctx {
	const struct shell *sh;
	int pass;
	int fail;
	int skip;
};

static void ft_report(struct ft_ctx *c, const char *test, enum ft_result r, const char *fmt, ...)
{
	static const char *const words[] = {"PASS", "FAIL", "SKIP"};
	char detail[320];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(detail, sizeof(detail), fmt, ap);
	va_end(ap);

	shell_print(c->sh, "FT %s %s %s", test, words[r], detail);

	if (r == FT_PASS) {
		c->pass++;
	} else if (r == FT_FAIL) {
		c->fail++;
	} else {
		c->skip++;
	}
}

static uint32_t isqrt64(uint64_t v)
{
	uint64_t r = 0;
	uint64_t bit = 1ULL << 62;

	while (bit > v) {
		bit >>= 2;
	}
	while (bit) {
		if (v >= r + bit) {
			v -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return (uint32_t)r;
}

static int32_t sv_milli(const struct sensor_value *v)
{
	return v->val1 * 1000 + v->val2 / 1000;
}

/* ---- Secure Enclave ------------------------------------------------------- */

static void t_se(struct ft_ctx *c)
{
	uint32_t part = 0;
	uint8_t rev[128] = {0};
	int err = se_service_get_device_part_number(&part);

	if (err == 0) {
		err = se_service_get_se_revision(rev);
	}
	if (err) {
		ft_report(c, "se", FT_FAIL, "err=%d", err);
		return;
	}
	/* The revision is a free-form string; keep the FT line one token-safe */
	for (char *p = (char *)rev; *p; p++) {
		if (*p == ' ' || *p == '\n' || *p == '\r') {
			*p = '_';
		}
	}
	ft_report(c, "se", FT_PASS, "part=0x%08x rev=%s", part, rev);
}

/* ---- Identity and BLE ------------------------------------------------------ */

/* Static-address prefix the app uses (modules/halo/src/ble_connection.c) */
#define HALO_STATIC_ADDR_PREFIX 0xFE5994

static uint8_t eui_ext[3];
static bool eui_read;

static void t_eui(struct ft_ctx *c)
{
	int err = se_system_get_eui_extension(true, eui_ext);

	if (err) {
		ft_report(c, "eui", FT_FAIL, "err=%d", err);
		return;
	}
	eui_read = true;

	/* An unprogrammed extension makes the app fall back to 11:22:33, which
	 * every such unit would share. */
	if (!eui_ext[0] && !eui_ext[1] && !eui_ext[2]) {
		ft_report(c, "eui", FT_FAIL, "ext=000000 (not programmed)");
		return;
	}
	ft_report(c, "eui", FT_PASS, "ext=%02X%02X%02X addr=FE:59:94:%02X:%02X:%02X", eui_ext[0],
		  eui_ext[1], eui_ext[2], eui_ext[0], eui_ext[1], eui_ext[2]);
}

/*
 * While advertising, print a heartbeat every 5 s so a station or phone scan
 * can be matched against a stack that is demonstrably still running: each
 * beat round-trips to the controller. Not an FT line, so parsers skip it.
 */
#define BLE_HEARTBEAT_S 5

static void ble_heartbeat(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(ble_heartbeat_work, ble_heartbeat);

static void ble_heartbeat(struct k_work *work)
{
	uint8_t hci_ver = 0;
	uint16_t hci_subver = 0;
	int err = mbt_ble_ping(K_SECONDS(1), &hci_ver, &hci_subver);

	shell_print(shell_backend_uart_get_ptr(),
		    "BLE adv alive uptime=%us adv=%s ctrl=%s hci=%u.%u", (uint32_t)(k_uptime_get() / 1000),
		    mbt_ble_adv_running() ? "on" : "off", err ? "no-reply" : "ok", hci_ver,
		    hci_subver);
	k_work_reschedule(&ble_heartbeat_work, K_SECONDS(BLE_HEARTBEAT_S));
}

static char ble_name[16];
static uint8_t ble_addr[6];
static int ble_status = -EAGAIN;

static void t_ble(struct ft_ctx *c)
{
	if (!eui_read) {
		ft_report(c, "ble_adv", FT_FAIL, "eui not read");
		return;
	}

	/* The stack is configured once per boot; a rerun reports the state */
	if (ble_status == -EAGAIN) {
		snprintf(ble_name, sizeof(ble_name), "Halo %02X%02X%02X", eui_ext[0], eui_ext[1],
			 eui_ext[2]);
		ble_addr[0] = (HALO_STATIC_ADDR_PREFIX >> 16) & 0xFF;
		ble_addr[1] = (HALO_STATIC_ADDR_PREFIX >> 8) & 0xFF;
		ble_addr[2] = HALO_STATIC_ADDR_PREFIX & 0xFF;
		memcpy(&ble_addr[3], eui_ext, 3);
		ble_status = mbt_ble_adv_start(ble_name, ble_addr, K_SECONDS(3));
		if (ble_status == 0) {
			k_work_reschedule(&ble_heartbeat_work, K_SECONDS(BLE_HEARTBEAT_S));
		}
	}

	if (ble_status) {
		ft_report(c, "ble_adv", FT_FAIL, "err=%d", ble_status);
		return;
	}
	ft_report(c, "ble_adv", mbt_ble_adv_running() ? FT_PASS : FT_FAIL,
		  "name=\"%s\" addr=%02X:%02X:%02X:%02X:%02X:%02X adv=%s", ble_name, ble_addr[0],
		  ble_addr[1], ble_addr[2], ble_addr[3], ble_addr[4], ble_addr[5],
		  mbt_ble_adv_running() ? "on" : "off");
}

/* ---- Memories -------------------------------------------------------------- */

#define RAM_TEST_WORDS (64 * 1024) /* 256 KB */
static uint32_t ram_buf[RAM_TEST_WORDS] __noinit;

static bool ram_pass(uint32_t (*pattern)(uint32_t i), size_t *bad)
{
	for (size_t i = 0; i < RAM_TEST_WORDS; i++) {
		ram_buf[i] = pattern(i);
	}
	for (size_t i = 0; i < RAM_TEST_WORDS; i++) {
		if (ram_buf[i] != pattern(i)) {
			*bad = i;
			return false;
		}
	}
	return true;
}

static uint32_t pat_addr(uint32_t i)
{
	return (uint32_t)(uintptr_t)&ram_buf[i];
}

static uint32_t pat_naddr(uint32_t i)
{
	return ~(uint32_t)(uintptr_t)&ram_buf[i];
}

static uint32_t pat_55(uint32_t i)
{
	return 0x55555555;
}

static uint32_t pat_aa(uint32_t i)
{
	return 0xAAAAAAAA;
}

static void t_ram(struct ft_ctx *c)
{
	uint32_t (*const patterns[])(uint32_t) = {pat_addr, pat_naddr, pat_55, pat_aa};
	int64_t t0 = k_uptime_get();
	size_t bad;

	for (size_t p = 0; p < ARRAY_SIZE(patterns); p++) {
		if (!ram_pass(patterns[p], &bad)) {
			ft_report(c, "ram", FT_FAIL, "pattern=%u addr=0x%08x read=0x%08x", p,
				  (uint32_t)(uintptr_t)&ram_buf[bad], ram_buf[bad]);
			return;
		}
	}
	ft_report(c, "ram", FT_PASS, "base=0x%08x bytes=%u ms=%u", (uint32_t)(uintptr_t)ram_buf,
		  sizeof(ram_buf), (uint32_t)(k_uptime_get() - t0));
}

/* Linker symbols: image start in MRAM, and its length (== the .bin size) */
extern char __rom_region_start[];
extern char _flash_used[];

static void t_mram_image(struct ft_ctx *c)
{
	const size_t len = (size_t)(uintptr_t)_flash_used;
	uint32_t crc = crc32_ieee((const uint8_t *)__rom_region_start, len);

	/* The station compares this with the CRC-32 of the .bin it wrote */
	ft_report(c, "mram_image", FT_PASS, "crc32=%08x len=%u", crc, len);
}

#define MRAM_TEST_LEN 1024

static void t_mram_write(struct ft_ctx *c)
{
	/* slot1 is free at this stage: MCUboot and the app go in afterwards,
	 * and MCUboot ignores slot1 without a valid image header. */
	static uint8_t wbuf[MRAM_TEST_LEN] __aligned(16);
	static uint8_t rbuf[MRAM_TEST_LEN] __aligned(16);
	const struct flash_area *fa;
	const uint32_t off = FIXED_PARTITION_OFFSET(slot1_partition);
	int err = flash_area_open(FIXED_PARTITION_ID(slot1_partition), &fa);

	if (err) {
		ft_report(c, "mram_write", FT_FAIL, "open err=%d", err);
		return;
	}

	for (size_t i = 0; i < sizeof(wbuf); i++) {
		wbuf[i] = (uint8_t)(i * 7 + 0x5A);
	}

	err = flash_area_write(fa, 0, wbuf, sizeof(wbuf));
	if (!err) {
		err = flash_area_read(fa, 0, rbuf, sizeof(rbuf));
	}
	if (err) {
		ft_report(c, "mram_write", FT_FAIL, "io err=%d", err);
		flash_area_close(fa);
		return;
	}

	bool match = memcmp(wbuf, rbuf, sizeof(wbuf)) == 0;

	/* Leave the area erased either way */
	if (flash_area_erase(fa, 0, sizeof(wbuf)) != 0) {
		memset(wbuf, flash_area_erased_val(fa), sizeof(wbuf));
		flash_area_write(fa, 0, wbuf, sizeof(wbuf));
	}
	flash_area_close(fa);

	if (!match) {
		ft_report(c, "mram_write", FT_FAIL, "read-back mismatch at offset 0x%x", off);
		return;
	}
	ft_report(c, "mram_write", FT_PASS, "offset=0x%x bytes=%u", off, sizeof(wbuf));
}

/* ---- Clocks ---------------------------------------------------------------- */

/*
 * The low-power RTC runs from LFXO (32.768 kHz) and the CPU cycle counter from
 * the HFXO-derived system clock. Counting CPU cycles across 0.5 s of RTC ticks
 * gives their ratio: a missing or badly trimmed crystal, or a fallback to an
 * RC oscillator, shows up as a large ppm error. The limit is provisional until
 * it has been measured on good boards.
 */
#define CLOCK_PPM_LIMIT 500

static void t_clocks(struct ft_ctx *c)
{
	const struct device *rtc = DEVICE_DT_GET(DT_NODELABEL(rtc0));
	const uint32_t cpu_hz = sys_clock_hw_cycles_per_sec();
	uint32_t f, t0, t, c0, c1, r0;

	if (!device_is_ready(rtc)) {
		ft_report(c, "clocks", FT_FAIL, "rtc not ready");
		return;
	}
	counter_start(rtc);
	f = counter_get_frequency(rtc);

	/* Align to an RTC tick edge; give up if the LF clock is not running */
	counter_get_value(rtc, &t0);
	c0 = k_cycle_get_32();
	do {
		counter_get_value(rtc, &t);
	} while (t == t0 && (k_cycle_get_32() - c0) < cpu_hz / 20);
	if (t == t0) {
		ft_report(c, "clocks", FT_FAIL, "lf clock not running");
		return;
	}

	/* Interrupts stay enabled: locking them for 0.5 s would stall the BLE
	 * host and lose SysTick wraps behind k_cycle_get_32(). An ISR landing
	 * between the two reads costs a few us, i.e. tens of ppm at worst. */
	r0 = t;
	c0 = k_cycle_get_32();
	do {
		counter_get_value(rtc, &t);
	} while ((t - r0) < f / 2 && (k_cycle_get_32() - c0) < cpu_hz);
	c1 = k_cycle_get_32();

	const uint32_t ticks = t - r0;
	const int64_t cycles = (uint32_t)(c1 - c0);
	const int64_t expected = (int64_t)ticks * cpu_hz / f;
	const int32_t ppm = (int32_t)((cycles - expected) * 1000000 / expected);

	ft_report(c, "clocks", (ppm < CLOCK_PPM_LIMIT && ppm > -CLOCK_PPM_LIMIT) ? FT_PASS : FT_FAIL,
		  "ppm=%d lf_hz=%u cpu_hz=%u ticks=%u cycles=%u", ppm, f, cpu_hz, ticks,
		  (uint32_t)cycles);
}

/* ---- Power ------------------------------------------------------------------- */

#define VBAT_MIN_MV 3000
#define VBAT_MAX_MV 4500

static void t_vbat(struct ft_ctx *c)
{
	const struct device *vbat = DEVICE_DT_GET(DT_CHOSEN(zephyr_vbat));
	struct sensor_value mv, soc, chg;
	int err;

	if (!device_is_ready(vbat)) {
		ft_report(c, "vbat", FT_FAIL, "not ready");
		return;
	}
	err = sensor_sample_fetch(vbat);
	if (!err) {
		err = sensor_channel_get(vbat, SENSOR_CHAN_GAUGE_VOLTAGE, &mv);
	}
	if (!err) {
		err = sensor_channel_get(vbat, SENSOR_CHAN_GAUGE_STATE_OF_CHARGE, &soc);
	}
	if (!err) {
		/* The driver reports the charger's state pin on this channel */
		err = sensor_channel_get(vbat, SENSOR_CHAN_GAUGE_STDBY_CURRENT, &chg);
	}
	if (err) {
		ft_report(c, "vbat", FT_FAIL, "err=%d", err);
		return;
	}
	ft_report(c, "vbat", (mv.val1 >= VBAT_MIN_MV && mv.val1 <= VBAT_MAX_MV) ? FT_PASS : FT_FAIL,
		  "mv=%d soc=%d charging=%d", mv.val1, soc.val1, chg.val1);
}

/* ---- Sensors on I2C0 (powered from sen_1v8) ---------------------------------- */

struct i2c_sensor {
	const char *test;
	const struct device *dev;
	uint16_t addr;
	uint8_t id_reg;
	uint8_t id;
	enum sensor_channel chan;
	/* Accepted range of the vector magnitude, in milli-units (0 = no check) */
	int32_t min_milli;
	int32_t max_milli;
	/* Each axis must stay below this, in milli-units (0 = no check) */
	int32_t axis_max_milli;
	/* Milli-unit value every axis reads before the first conversion (the
	 * chip's 0x8000 "no data" code), or 0 if the part has none */
	int32_t no_data_milli;
	const char *unit;
};

/* After resume the first conversion takes up to one ODR period plus the
 * mode switch; poll for it rather than read the reset value */
#define SENSOR_SAMPLE_TIMEOUT_MS 100

/* Time allowed from sen_1v8 on to the first ACK. The BMA580 needs more than
 * the 5 ms the first cut allowed (it NACKed on a fitted board); the driver's
 * own power-up wait is 10 ms. */
#define SENSOR_READY_TIMEOUT_MS 50

static void t_i2c_sensor(struct ft_ctx *c, const struct i2c_sensor *s)
{
	const struct device *bus = DEVICE_DT_GET(DT_NODELABEL(i2c0));
	const struct device *rail = DEVICE_DT_GET(DT_NODELABEL(sen_1v8));
	struct sensor_value v[3];
	bool resumed = false;
	uint8_t id = 0;
	int err;

	err = regulator_enable(rail);
	if (err) {
		ft_report(c, s->test, FT_FAIL, "sen_1v8 enable err=%d", err);
		return;
	}

	const int64_t t0 = k_uptime_get();
	int64_t ready_ms;

	do {
		k_msleep(2);
		err = i2c_reg_read_byte(bus, s->addr, s->id_reg, &id);
		ready_ms = k_uptime_get() - t0;
	} while (err && ready_ms < SENSOR_READY_TIMEOUT_MS);
	if (err) {
		ft_report(c, s->test, FT_FAIL, "addr=0x%02x no ack within %d ms", s->addr,
			  SENSOR_READY_TIMEOUT_MS);
		goto out;
	}
	if (id != s->id) {
		ft_report(c, s->test, FT_FAIL, "addr=0x%02x id=0x%02x want=0x%02x", s->addr, id,
			  s->id);
		goto out;
	}

	/* Drivers boot their device suspended; resume runs the hardware init */
	err = pm_device_action_run(s->dev, PM_DEVICE_ACTION_RESUME);
	if (err && err != -EALREADY) {
		ft_report(c, s->test, FT_FAIL, "id=0x%02x resume err=%d", id, err);
		goto out;
	}
	/* Stays resumed through the stuck check; suspended once, at out: */
	resumed = true;
	const int64_t s0 = k_uptime_get();

	do {
		err = sensor_sample_fetch(s->dev);
		if (!err) {
			err = sensor_channel_get(s->dev, s->chan, v);
		}
		if (err || !s->no_data_milli || sv_milli(&v[0]) != s->no_data_milli ||
		    sv_milli(&v[1]) != s->no_data_milli || sv_milli(&v[2]) != s->no_data_milli) {
			break;
		}
		k_msleep(2);
	} while (k_uptime_get() - s0 < SENSOR_SAMPLE_TIMEOUT_MS);
	if (err) {
		ft_report(c, s->test, FT_FAIL, "id=0x%02x sample err=%d", id, err);
		goto out;
	}

	const int32_t x = sv_milli(&v[0]), y = sv_milli(&v[1]), z = sv_milli(&v[2]);

	if (s->no_data_milli && x == s->no_data_milli && y == s->no_data_milli &&
	    z == s->no_data_milli) {
		ft_report(c, s->test, FT_FAIL, "id=0x%02x no data within %d ms ready_ms=%d", id,
			  SENSOR_SAMPLE_TIMEOUT_MS, (int)ready_ms);
		goto out;
	}

	/* A live sensor jitters by a few counts between conversions; three
	 * bit-identical samples mean stuck output. Both parts run at 200 Hz. */
	bool stuck = true;

	for (int i = 0; i < 2 && stuck && !err; i++) {
		struct sensor_value w[3];

		k_msleep(6);
		err = sensor_sample_fetch(s->dev);
		if (!err) {
			err = sensor_channel_get(s->dev, s->chan, w);
		}
		stuck = !err && memcmp(v, w, sizeof(w)) == 0;
	}
	if (err) {
		ft_report(c, s->test, FT_FAIL, "id=0x%02x repeat sample err=%d", id, err);
		goto out;
	}
	if (stuck) {
		ft_report(c, s->test, FT_FAIL, "id=0x%02x stuck x=%d y=%d z=%d (3 identical samples)",
			  id, x, y, z);
		goto out;
	}

	const uint32_t mag = isqrt64((int64_t)x * x + (int64_t)y * y + (int64_t)z * z);
	bool ok = true;

	if (s->max_milli) {
		ok = mag >= s->min_milli && mag <= s->max_milli;
	}
	if (s->axis_max_milli) {
		ok = ok && mag > 0 && abs(x) < s->axis_max_milli && abs(y) < s->axis_max_milli &&
		     abs(z) < s->axis_max_milli;
	}
	ft_report(c, s->test, ok ? FT_PASS : FT_FAIL, "id=0x%02x x=%d y=%d z=%d mag=%u %s ready_ms=%d",
		  id, x, y, z, mag, s->unit, (int)ready_ms);
out:
	if (resumed) {
		pm_device_action_run(s->dev, PM_DEVICE_ACTION_SUSPEND);
	}
	regulator_disable(rail);
}

static void t_imu(struct ft_ctx *c)
{
	/* At rest the accelerometer sees 1 g in whatever orientation. This
	 * driver reports g (not m/s^2), at the +/-2 g range from the DTS. */
	static const struct i2c_sensor bma580 = {
		.test = "imu",
		.dev = DEVICE_DT_GET(DT_NODELABEL(bma580)),
		.addr = 0x18,
		.id_reg = 0x00,
		.id = 0xC4,
		.chan = SENSOR_CHAN_ACCEL_XYZ,
		.min_milli = 700,
		.max_milli = 1300,
		.no_data_milli = -2000,
		.unit = "mg",
	};

	t_i2c_sensor(c, &bma580);
}

static void t_mag(struct ft_ctx *c)
{
	/* No magnitude window: on a bare board the hard-iron offset from
	 * nearby magnetised parts can be many gauss (12.4 G on the first board),
	 * and finished units calibrate it out. Fail only an all-zero reading or
	 * an axis at the edge of the +/-30 G range. */
	static const struct i2c_sensor qmc6308 = {
		.test = "mag",
		.dev = DEVICE_DT_GET(DT_NODELABEL(qmc6308)),
		.addr = 0x2c,
		.id_reg = 0x00,
		.id = 0x80,
		.chan = SENSOR_CHAN_MAGN_XYZ,
		.axis_max_milli = 29000,
		.unit = "mG",
	};

	t_i2c_sensor(c, &qmc6308);
}

/* ---- Microphone ------------------------------------------------------------- */

/*
 * Two mics share the PDM bus, one per clock edge: channel 2 (the app's mono
 * channel) and channel 3. Only one of them is on the main board; the other is
 * on the secondary board, not fitted at this stage. A PDM mic drives DATA on
 * its own edge and floats it on the other, so with the second mic absent the
 * line holds the last bit and the other edge reads the same stream: on board
 * #5 ch2 and ch3 matched sample for sample (`pair=identical`). Either channel
 * therefore carries the main-board mic here, and PASS is gated on ch2, the
 * one the app records. On an assembled unit `pair` should read `distinct`.
 *
 * Per channel:
 *   rms   AC rms of the raw samples
 *   lp    AC rms after a 4-tap moving average (~1.8 kHz corner)
 *   tone  share of the AC power, per mille, in 900-1150 Hz (Goertzel over
 *         every bin in the band). White noise gives ~30; a loud 1 kHz tone
 *         at the mic gives most of it. The band tolerates the PDM sample
 *         rate differing from nominal by a few percent.
 *   min, max
 *   zero  count of exact-zero samples; a real mic stream has few
 *
 * rate_hz is the delivered sample rate per channel, measured over the
 * captured blocks; it should read ~16000.
 *
 * Blocks are 20 ms, as the app uses. The driver's DMA fills at most
 * PDM_DMA_MAX_SETS (512) frames per block but still delivers the configured
 * block size, so a larger block comes back mostly stale: cuts 4-6 used 100 ms
 * (1600 frames) and read ~68% zeros at an apparent 50 kHz.
 */
#define MIC_RATE       16000
#define MIC_CHANNELS   2
#define MIC_BLOCK_MS   20
#define MIC_BLOCK_FRAMES (MIC_RATE * MIC_BLOCK_MS / 1000)
#define MIC_BLOCK      (MIC_BLOCK_FRAMES * 2 * MIC_CHANNELS) /* 16-bit samples */
#define MIC_BLOCKS     8
#define MIC_SETTLE     10 /* blocks (200 ms) discarded while the PDM settles */
#define MIC_MEASURE    15 /* blocks (300 ms) measured */
#define MIC_FRAMES     (MIC_BLOCK_FRAMES * MIC_MEASURE) /* samples per channel */

/* t5838_alif_pdm.c: PDM_DMA_MAX_SETS */
BUILD_ASSERT(MIC_BLOCK_FRAMES <= 512, "PDM DMA fills at most 512 frames per block");
#define MIC_BAND_LO_HZ 900
#define MIC_BAND_HI_HZ 1150
#define MIC_BIN_LO     (MIC_BAND_LO_HZ * MIC_FRAMES / MIC_RATE)
#define MIC_BIN_HI     (MIC_BAND_HI_HZ * MIC_FRAMES / MIC_RATE)
#define MIC_BINS       (MIC_BIN_HI - MIC_BIN_LO + 1)
#define MIC_LP_TAPS    4
/* Limits for when the main-board channel is known: from board 10-58-AB at
 * gain 0, quiet rms ~100-290 */
#define MIC_RMS_MIN    20
#define MIC_RMS_MAX    4000

static struct k_mem_slab mic_slab;
static uint8_t mic_slab_buf[MIC_BLOCK * MIC_BLOCKS] __aligned(4);

struct mic_stats {
	int64_t sum;
	uint64_t sumsq;
	uint64_t lp_sumsq;
	uint32_t lp_n;
	int32_t box;
	int16_t hist[MIC_LP_TAPS];
	int16_t lo;
	int16_t hi;
	uint32_t zeros;
	float s1[MIC_BINS];
	float s2[MIC_BINS];
};

static struct mic_stats mic_st[MIC_CHANNELS];
static float mic_coeff[MIC_BINS];

static void mic_accumulate(struct mic_stats *m, int16_t x, uint32_t i)
{
	m->sum += x;
	m->sumsq += (int32_t)x * x;
	m->lo = MIN(m->lo, x);
	m->hi = MAX(m->hi, x);
	m->zeros += (x == 0);

	m->box += x - m->hist[i % MIC_LP_TAPS];
	m->hist[i % MIC_LP_TAPS] = x;
	if (i + 1 >= MIC_LP_TAPS) {
		const int32_t lp = m->box / MIC_LP_TAPS;

		m->lp_sumsq += (int64_t)lp * lp;
		m->lp_n++;
	}

	for (int k = 0; k < MIC_BINS; k++) {
		const float s0 = (float)x + mic_coeff[k] * m->s1[k] - m->s2[k];

		m->s2[k] = m->s1[k];
		m->s1[k] = s0;
	}
}

static void mic_summary(const struct mic_stats *m, uint32_t n, uint32_t *rms, uint32_t *lp,
			uint32_t *tone)
{
	const int64_t dc = m->sum / n;
	const uint64_t dc2 = (uint64_t)(dc * dc);
	const uint64_t ms = m->sumsq / n;
	const uint64_t lp_ms = m->lp_n ? m->lp_sumsq / m->lp_n : 0;
	const float var = (float)(ms > dc2 ? ms - dc2 : 0);
	float p_band = 0.0f;

	/* Parseval: a component in bin k contributes 2*|X(k)|^2 / n^2 of power */
	for (int k = 0; k < MIC_BINS; k++) {
		const float x2 = m->s1[k] * m->s1[k] + m->s2[k] * m->s2[k] -
				 mic_coeff[k] * m->s1[k] * m->s2[k];

		p_band += 2.0f * x2 / ((float)n * (float)n);
	}

	*rms = isqrt64((uint64_t)var);
	*lp = isqrt64(lp_ms > dc2 ? lp_ms - dc2 : 0);
	*tone = var > 0.0f ? (uint32_t)MIN(1000.0f, 1000.0f * p_band / var) : 0;
}

static void t_mic(struct ft_ctx *c)
{
	const struct device *mic = DEVICE_DT_GET(DT_CHOSEN(zephyr_micphone));
	struct pcm_stream_cfg stream = {
		.pcm_width = 16,
		.pcm_rate = MIC_RATE,
		.mem_slab = &mic_slab,
		.block_size = MIC_BLOCK,
	};
	struct dmic_cfg cfg = {
		.channel = {
			.req_num_streams = 1,
			.req_num_chan = MIC_CHANNELS,
			/* Samples arrive as [ch2, ch3] pairs (t5838_alif_pdm.c) */
			.req_chan_map_lo = BIT(2) | BIT(3),
		},
		.streams = &stream,
	};
	uint32_t n = 0; /* samples per channel */
	uint32_t n_timed = 0;
	uint32_t pair_diff = 0; /* frames where ch2 != ch3 */
	int64_t t_first = 0, t_last = 0;
	int err;

	memset(mic_st, 0, sizeof(mic_st));
	for (int ch = 0; ch < MIC_CHANNELS; ch++) {
		mic_st[ch].lo = INT16_MAX;
		mic_st[ch].hi = INT16_MIN;
	}
	for (int k = 0; k < MIC_BINS; k++) {
		mic_coeff[k] = 2.0f * cosf(2.0f * 3.14159265f * (float)(MIC_BIN_LO + k) /
					   (float)MIC_FRAMES);
	}

	if (!device_is_ready(mic)) {
		ft_report(c, "mic", FT_FAIL, "not ready");
		return;
	}
	/* Fresh slab per run: STOP returns queued blocks to it, and a rerun
	 * must not inherit anything from the last one */
	k_mem_slab_init(&mic_slab, mic_slab_buf, MIC_BLOCK, MIC_BLOCKS);
	err = dmic_configure(mic, &cfg);
	if (!err) {
		/* configure() zeroes the channel gain registers; the app always
		 * sets gain afterwards (default 0) */
		err = dmic_set_gain(mic, 0);
	}
	if (!err) {
		err = dmic_trigger(mic, DMIC_TRIGGER_START);
	}
	if (err) {
		ft_report(c, "mic", FT_FAIL, "start err=%d", err);
		return;
	}

	for (int b = 0; b < MIC_SETTLE + MIC_MEASURE && !err; b++) {
		void *buf;
		uint32_t size;

		err = dmic_read(mic, 0, &buf, &size, 100);
		if (err) {
			break;
		}
		const uint32_t frames = size / (2 * MIC_CHANNELS);

		/* Rate: frames delivered between the first and last block's
		 * arrival, so it excludes the first block's own fill time */
		if (b == MIC_SETTLE - 1) {
			t_first = k_uptime_get();
		} else if (b >= MIC_SETTLE) {
			t_last = k_uptime_get();
			n_timed += frames;
		}
		if (b >= MIC_SETTLE) {
			const int16_t *smp = buf;

			for (uint32_t i = 0; i < frames && n + i < MIC_FRAMES; i++) {
				for (int ch = 0; ch < MIC_CHANNELS; ch++) {
					mic_accumulate(&mic_st[ch], smp[i * MIC_CHANNELS + ch],
						       n + i);
				}
				pair_diff += smp[i * MIC_CHANNELS] != smp[i * MIC_CHANNELS + 1];
			}
			n = MIN(n + frames, MIC_FRAMES);
		}
		k_mem_slab_free(&mic_slab, buf);
	}

	/* STOP also returns any queued blocks to the slab */
	dmic_trigger(mic, DMIC_TRIGGER_STOP);

	if (err || n == 0) {
		ft_report(c, "mic", FT_FAIL, "read err=%d samples=%u", err, n);
		return;
	}

	uint32_t rms[MIC_CHANNELS], lp[MIC_CHANNELS], tone[MIC_CHANNELS];
	bool alive[MIC_CHANNELS];

	for (int ch = 0; ch < MIC_CHANNELS; ch++) {
		mic_summary(&mic_st[ch], n, &rms[ch], &lp[ch], &tone[ch]);
		alive[ch] = rms[ch] >= MIC_RMS_MIN && rms[ch] <= MIC_RMS_MAX &&
			    mic_st[ch].lo != mic_st[ch].hi;
	}

	const int64_t dt = t_last - t_first;
	const uint32_t rate = dt > 0 ? (uint32_t)(n_timed * 1000 / dt) : 0;

	ft_report(c, "mic", alive[0] ? FT_PASS : FT_FAIL,
		  "ch2_rms=%u ch2_lp=%u ch2_tone=%u ch2_min=%d ch2_max=%d ch2_zero=%u ch2_alive=%d "
		  "ch3_rms=%u ch3_lp=%u ch3_tone=%u ch3_min=%d ch3_max=%d ch3_zero=%u ch3_alive=%d "
		  "pair=%s main=ch2 rate_hz=%u samples=%u",
		  rms[0], lp[0], tone[0], mic_st[0].lo, mic_st[0].hi, mic_st[0].zeros, alive[0],
		  rms[1], lp[1], tone[1], mic_st[1].lo, mic_st[1].hi, mic_st[1].zeros, alive[1],
		  pair_diff ? "distinct" : "identical", rate, n);
}

/* ---- Display/camera flex parts on I2C1 ---------------------------------------- */

static void t_i2c1(struct ft_ctx *c)
{
	/* TPS65132 bias PMIC, PAG7982 camera, VGA020 display. None is expected
	 * on a bare main board, so no ACK is a SKIP, not a FAIL. */
	static const uint16_t addrs[] = {0x3e, 0x40, 0x54};
	const struct device *bus = DEVICE_DT_GET(DT_NODELABEL(i2c1));
	char detail[64];
	size_t len = 0;
	int acks = 0;

	for (size_t i = 0; i < ARRAY_SIZE(addrs); i++) {
		uint8_t b;
		bool ack = i2c_read(bus, &b, 1, addrs[i]) == 0;

		acks += ack;
		len += snprintf(&detail[len], sizeof(detail) - len, "%s0x%02x=%s", i ? " " : "",
				addrs[i], ack ? "ack" : "nak");
	}
	ft_report(c, "i2c1", acks ? FT_PASS : FT_SKIP, "%s", detail);
}

/* ---- Sequencing -------------------------------------------------------------- */

struct ft_test {
	const char *name;
	void (*fn)(struct ft_ctx *c);
};

/* eui must precede ble_adv, which advertises the name derived from it.
 * clocks runs before the BLE stack starts adding interrupt load. */
static const struct ft_test tests[] = {
	{"se", t_se},
	{"eui", t_eui},
	{"clocks", t_clocks},
	{"ble_adv", t_ble},
	{"ram", t_ram},
	{"mram_image", t_mram_image},
	{"mram_write", t_mram_write},
	{"vbat", t_vbat},
	{"imu", t_imu},
	{"mag", t_mag},
	{"mic", t_mic},
	{"i2c1", t_i2c1},
};

static K_MUTEX_DEFINE(run_lock);

static void factory_run(const struct shell *sh, const struct ft_test *only)
{
	struct ft_ctx c = {.sh = sh};

	k_mutex_lock(&run_lock, K_FOREVER);
	shell_print(sh, "FT BEGIN halo-mainboard-test %s %s", MBT_VERSION, MBT_GIT);
	for (size_t i = 0; i < ARRAY_SIZE(tests); i++) {
		if (!only || only == &tests[i]) {
			tests[i].fn(&c);
		}
	}
	shell_print(sh, "FT DONE %d/%d skip=%d", c.pass, c.pass + c.fail, c.skip);
	k_mutex_unlock(&run_lock);
}

static int cmd_factory(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2 || strcmp(argv[1], "run") == 0) {
		factory_run(sh, NULL);
		return 0;
	}
	for (size_t i = 0; i < ARRAY_SIZE(tests); i++) {
		if (strcmp(argv[1], tests[i].name) == 0) {
			factory_run(sh, &tests[i]);
			return 0;
		}
	}
	shell_error(sh, "unknown test '%s'; tests:", argv[1]);
	for (size_t i = 0; i < ARRAY_SIZE(tests); i++) {
		shell_print(sh, "  %s", tests[i].name);
	}
	return -EINVAL;
}

SHELL_CMD_ARG_REGISTER(factory, NULL, "factory run | factory <test>", cmd_factory, 1, 1);

int main(void)
{
	/* Let the shell finish its banner so FT BEGIN starts on a clean line */
	k_msleep(100);
	factory_run(shell_backend_uart_get_ptr(), NULL);
	return 0;
}

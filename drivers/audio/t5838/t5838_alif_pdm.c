/*
 * Copyright (c) 2025  Brilliant Labs Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/audio/dmic.h>
#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/cache.h>
#include <soc.h>
#include "t5838.h"
#include "dma_event_router.h"

#define DT_DRV_COMPAT alif_t5838_alif_pdm

LOG_MODULE_REGISTER(t5838, CONFIG_T5838_LOG_LEVEL);

#define PDM_CONFIG_REGISTER      (0x0)  /* PDM Audio Control Register 0  */
#define PDM_CTL_REGISTER         (0x4)  /* PDM Audio Control Register 1  */
#define PDM_THRESHOLD_REGISTER   (0x8)  /* FIFO Watermark Register    */
#define PDM_FIFO_STATUS_REGISTER (0xC)  /* FIFO Status Register      */
#define PDM_ERROR_IRQ            (0x10) /* FIFO Error Interrupt Status Register  */
#define PDM_WARN_IRQ             (0x14) /* FIFO Warning Interrupt Status Register*/
#define PDM_AUDIO_DETECT_IRQ     (0x18) /* Audio Detection Interrupt Status Register */
#define PDM_INTERRUPT_REGISTER   (0x1C) /* Interrupt Enable Register  */
#define PDM_CH0_CH1_AUDIO_OUT    (0x20) /* Channels 0 and 1 Audio Output Register  */
#define PDM_CH2_CH3_AUDIO_OUT    (0x24) /* Channels 2 and 3 Audio Output Register  */
#define PDM_CH4_CH5_AUDIO_OUT    (0x28) /* Channels 4 and 5 Audio Output Register  */
#define PDM_CH6_CH7_AUDIO_OUT    (0x2C) /* Channels 6 and 7 Audio Output Register  */
#define PDM_CH_FIR_COEF          (0x40) /* Channel (n) FIR Filter Coefficient  */
#define PDM_CH_IIR_COEF_SEL      (0xC0) /* Channel (n) IIR Filter Coefficient  */
#define PDM_CH_PHASE             (0xC4) /* Channel (n) Phase Control Register  */
#define PDM_CH_GAIN              (0xC8) /* Channel (n) Gain Control Register  */
#define PDM_CH_PKDET_TH          (0xCC) /* Channel (n) Peak Detector Threshold Register  */
#define PDM_CH_PKDET_ITV         (0xD0) /* Channel (n) Peak Detector Interval Register  */

#define PDM_FIFO_CLEAR            (1U << 31U)   /* To clear FIFO clear bit  */
#define PDM_AUDIO_DETECT_IRQ_STAT (0xFFU << 8U) /* Audio detect interrupt  */
#define PDM_FIFO_ALMOST_FULL_IRQ  (0x1U << 0U)  /* FIFO almost full Interrupt*/
#define PDM_FIFO_OVERFLOW_IRQ     (0x1U << 1U)  /* FIFO overflow interrupt ENABLE
						 * (PDM_IRQ_ENABLE bit 1) */
/* B1 HWRM 14.6.5.3.5: overflow STATUS is bit 0 of PDM_ERROR_IRQ (0x10),
 * edge-triggered STICKY, cleared only by reading that register. It is
 * NOT a bit in PDM_WARN_IRQ - the old code tested (WARN & bit1), which
 * can never be true, so its whole overflow branch was dead code, and an
 * enabled-but-never-read ERROR status holds the OR'ed LPPDM_IRQ line
 * asserted (ISR storm; wedged the device the moment the overflow
 * interrupt was first enabled, 2026-07-13 on the dev kit).
 */
#define PDM_FIFO_OVERFLOW_STATUS  (0x1U << 0U)  /* PDM_ERROR_IRQ bit 0 */
/* PDM_CTL1 (0x4) bit 24: route the FIFO watermark to the DMA handshake
 * instead of (only) the almost-full ISR - "Use DMA handshaking signals for
 * flow control" (B1 HWRM 14.6.5.3.2). Set when the node has a rxdma DMA.
 */
#define PDM_CTL1_USE_DMA          (0x1U << 24U)
/* LPPDM's DMA request line 30 is Group-2 mapped in the local event router
 * (B1 HWRM Table 14-95 + the DMA request MUX table); the request NUMBER is
 * the DT dmas cell, the GROUP selects LPPDM on that line.
 */
#define LPPDM_DMA_GROUP           2U
#define PDM_BYPASS_IIR            (2U)          /* Bypass DC blocking IIR filter*/
#define PDM_BYPASS_FIR            (3U)          /* Bypass DC blocking FIR filter*/
#define PDM_CHANNEL_ENABLE        (0xFFU)       /* To check the which channel is enabled*/

#define PDM_CLK_MODE            16U   /* PDM clock frequency mode  */
#define PDM_MAX_FIR_COEFFICIENT 18    /* PDM channel FIR length  */
#define MAX_DATA_ITEMS          (8)   /* Max data items      */
#define MAX_NUM_CHANNELS        (8)   /* Max number of channel   */
#define MAX_QUEUE_LEN           (100) /* Max Queue length      */
#define PDM_CH_OFFSET           (0x100)

#define PDM_MODE_MICROPHONE_SLEEP                  0x00UL
#define PDM_MODE_STANDARD_VOICE_512_CLK_FRQ        0x01UL
#define PDM_MODE_HIGH_QUALITY_512_CLK_FRQ          0x02UL
#define PDM_MODE_HIGH_QUALITY_768_CLK_FRQ          0x03UL
#define PDM_MODE_HIGH_QUALITY_1024_CLK_FRQ         0x04UL
#define PDM_MODE_WIDE_BANDWIDTH_AUDIO_1536_CLK_FRQ 0x05UL
#define PDM_MODE_FULL_BANDWIDTH_AUDIO_2400_CLK_FRQ 0x06UL
#define PDM_MODE_FULL_BANDWIDTH_AUDIO_3071_CLK_FRQ 0x07UL
#define PDM_MODE_ULTRASOUND_4800_CLOCK_FRQ         0x08UL
#define PDM_MODE_ULTRASOUND_96_SAMPLING_RATE       0x09UL

#define PDM_CHANNEL_0 (1U << 0U)
#define PDM_CHANNEL_1 (1U << 1U)
#define PDM_CHANNEL_2 (1U << 2U)
#define PDM_CHANNEL_3 (1U << 3U)
#define PDM_CHANNEL_4 (1U << 4U)
#define PDM_CHANNEL_5 (1U << 5U)
#define PDM_CHANNEL_6 (1U << 6U)
#define PDM_CHANNEL_7 (1U << 7U)

/* Hardware FIFO depth in sets/frames (the "8-set FIFO"). Doubles as the
 * minimal loss estimate reported on the DMA drain path (see the ISR).
 */
#define PDM_FIFO_DEPTH_FRAMES 8U

static uint8_t alif_pdm_find_mode(struct dmic_cfg *config)
{
	uint8_t mode = 0;

	switch (config->streams[0].pcm_rate) {
	case 8000:
		mode = PDM_MODE_STANDARD_VOICE_512_CLK_FRQ;
		break;
	case 16000:
		mode = PDM_MODE_HIGH_QUALITY_1024_CLK_FRQ;
		break;
	case 32000:
		mode = PDM_MODE_WIDE_BANDWIDTH_AUDIO_1536_CLK_FRQ;
		break;
	case 48000:
		mode = PDM_MODE_FULL_BANDWIDTH_AUDIO_2400_CLK_FRQ;
		break;
	case 96000:
		mode = PDM_MODE_ULTRASOUND_96_SAMPLING_RATE;
		break;
	default:
		mode = PDM_MODE_STANDARD_VOICE_512_CLK_FRQ;
		break;
	}

	return mode;
}

static uint32_t alif_pdm_get_sample_rate(uint8_t mode)
{
	switch (mode) {
	case PDM_MODE_STANDARD_VOICE_512_CLK_FRQ:
		return 8000;
	case PDM_MODE_HIGH_QUALITY_1024_CLK_FRQ:
		return 16000;
	case PDM_MODE_WIDE_BANDWIDTH_AUDIO_1536_CLK_FRQ:
		return 32000;
	case PDM_MODE_FULL_BANDWIDTH_AUDIO_2400_CLK_FRQ:
		return 48000;
	case PDM_MODE_ULTRASOUND_96_SAMPLING_RATE:
		return 96000;
	default:
		return 8000;
	}
}

/* capture-loss ledgers (defined below, ahead of the ISR) */
extern uint64_t t5838_diag_popped;
extern uint64_t t5838_diag_dropped;
extern uint64_t t5838_diag_discarded;

/* ---- LPPDM RX DMA drain (prototype) --------------------------------------
 * The 8-set FIFO overflows on any >~250us CPU blackout when drained by the
 * almost-full ISR. Route the watermark to the DMA handshake (PDM_CTL1
 * USE_DMA + event router group 2 / request 30) and let dma2 (PL330) copy the
 * CH2_CH3 FIFO register, hardware-timed and immune to the blackouts.
 *
 * Read the FIFO at its NATIVE 32-bit width (one [ch3:16|ch2:16] word per set)
 * into a scratch buffer, then extract the enabled channel(s) into a mem_slab
 * block in the callback. A 32-bit read is what actually pops a set (matching
 * the ISR's register reads); a 16-bit sub-word read of the 32-bit-only APB
 * delivered misaligned garbage (audible static/clipping) even though the
 * transfer COUNT looked correct. Single-buffer re-arm: the callback extracts
 * and queues the finished block, then restarts (the FIFO's 8 sets of slack
 * cover the ~us extract+restart gap - a future double-buffer removes even
 * that). The start-of-stream discard (PDM decimator settling) is honoured by
 * dropping whole settling transfers so delivered blocks stay full-size and
 * the AEC capture timeline starts where it expects.
 */
#define PDM_DMA_MAX_SETS 512
static uint32_t pdm_dma_scratch[PDM_DMA_MAX_SETS];

/* Route the LPPDM watermark to a dma2 channel via the local event router.
 * Mirrors the i2s_sync driver (the proven FIFO->DMA peer on this controller):
 * enable the request channel for the group, then SET the ACK-type bit for a
 * level handshake. NOTE we deliberately do NOT use the HAL's
 * dma_event_router_configure(): it has a dead line that unconditionally
 * clears the ACK-type bit (leaving an edge/level mismatch), which lets the
 * DMA over-read the FIFO and return "invalid data" (full-scale) sets - heard
 * as static/clipping. Level handshake gates each read to real FIFO data.
 */
static void pdm_configure_dma_event_router(uint32_t dma_group,
					   uint32_t dma_request)
{
	uint32_t regdata;

	regdata = EVTRTR2_DMA_CTRL_ENA + EVTRTR2_DMA_CTRL_ACK_PERIPH + dma_group;
	sys_write32(regdata, EVTRTRLOCAL_DMA_CTRL0 + (dma_request * 0x4));

	regdata = sys_read32(EVTRTRLOCAL_DMA_ACK_TYPE0 + (dma_group * 0x4));
	regdata |= (0x1u << dma_request);
	sys_write32(regdata, EVTRTRLOCAL_DMA_ACK_TYPE0 + (dma_group * 0x4));
}

static void pdm_dma_callback(const struct device *dma_dev, void *user_data,
			     uint32_t channel, int status);

static uint32_t pdm_dma_sets(const struct t5838_drv_data *drv_data)
{
	uint32_t sets = drv_data->block_size / (2u * drv_data->num_channels);

	return (sets > PDM_DMA_MAX_SETS) ? PDM_DMA_MAX_SETS : sets;
}

static int pdm_dma_start_block(const struct device *dev)
{
	const struct t5838_drv_cfg *drv_cfg = dev->config;
	struct t5838_drv_data *drv_data = dev->data;
	uint32_t bytes = pdm_dma_sets(drv_data) * sizeof(uint32_t);

#if defined(CONFIG_DCACHE)
	sys_cache_data_invd_range(pdm_dma_scratch, bytes);
#endif

	struct dma_block_config blk = {
		.source_address = drv_cfg->regs + PDM_CH2_CH3_AUDIO_OUT,
		.dest_address = POINTER_TO_UINT(pdm_dma_scratch),
		.block_size = bytes,
		.source_addr_adj = DMA_ADDR_ADJ_NO_CHANGE,
		.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT,
	};
	struct dma_config cfg = {
		.dma_slot = drv_cfg->dma_req,
		.channel_direction = PERIPHERAL_TO_MEMORY,
		/* native 32-bit reads (data_size 1 => 4-byte): one FIFO set */
		.source_data_size = 1,
		.dest_data_size = 1,
		/* burst length: PERIPHERAL_TO_MEMORY forces the PL330 into
		 * burst-request mode (breq_only), moving source_burst_length+1
		 * sets per request. The LPPDM handshake signals "at least one
		 * sample available" (single-request semantics), so a LARGE burst
		 * over-reads (each request pulls burst_len+1 sets when only ~1 is
		 * guaranteed) -> the mic delivers ~2x too fast and the AEC
		 * capture timeline races off its ring (margin -1.5M, worn
		 * 2026-07-13). Keep the burst minimal so the over-read per
		 * request is at most one set; the rate is validated on the dev kit's
		 * clock_rate_test. TODO: the clean fix is a SINGLE-request DMA
		 * (needs the PL330 driver to not force breq_only for this slot).
		 */
		.source_burst_length = 1,
		.dest_burst_length = 1,
		.head_block = &blk,
		.user_data = (void *)dev,
		.dma_callback = pdm_dma_callback,
	};

	int ret = dma_config(drv_cfg->dma_dev, drv_cfg->dma_ch, &cfg);

	if (ret < 0) {
		return ret;
	}
	return dma_start(drv_cfg->dma_dev, drv_cfg->dma_ch);
}

static void pdm_dma_callback(const struct device *dma_dev, void *user_data,
			     uint32_t channel, int status)
{
	const struct device *dev = user_data;
	struct t5838_drv_data *drv_data = dev->data;
	uint32_t sets = pdm_dma_sets(drv_data);
	void *out;

	ARG_UNUSED(dma_dev);
	ARG_UNUSED(channel);

	if (status < 0 || !drv_data->active) {
		if (drv_data->active) {
			(void)pdm_dma_start_block(dev);
		}
		return;
	}

#if defined(CONFIG_DCACHE)
	sys_cache_data_invd_range(pdm_dma_scratch, sets * sizeof(uint32_t));
#endif

	/* start-of-stream discard: drop whole settling transfers (counted, not
	 * delivered) until the settling window is covered, so delivered blocks
	 * stay full-size and the capture timeline starts where the AEC expects
	 */
	if (drv_data->discard_samples > 0) {
		/* discard_samples is in all-channel samples (init'd
		 * rate*ms/1000*num_channels, matching the ISR path's `k`).
		 * A DMA transfer carries `sets` sample-TIMES = sets*num_channels
		 * all-channel samples; decrement in that unit or stereo
		 * over-discards ~2x (a whole extra 20ms block). Whole transfers
		 * are dropped regardless; d only advances the counter.
		 */
		uint32_t d = MIN(drv_data->discard_samples,
				 sets * drv_data->num_channels);

		drv_data->discard_samples -= d;
		t5838_diag_discarded += d;
		if (drv_data->active) {
			(void)pdm_dma_start_block(dev);
		}
		return;
	}

	if (k_mem_slab_alloc(drv_data->mem_slab, &out, K_NO_WAIT) == 0) {
		if (drv_data->num_channels > 1) {
			/* stereo: the 32-bit words ARE [ch2,ch3] LRLR pairs */
			memcpy(out, pdm_dma_scratch, sets * sizeof(uint32_t));
		} else {
			/* mono: keep the low half (ch2) of each 32-bit word */
			int16_t *o = out;

			for (uint32_t i = 0; i < sets; i++) {
				o[i] = (int16_t)(pdm_dma_scratch[i] & 0xFFFFu);
			}
		}
		t5838_diag_popped += sets * drv_data->num_channels;
		if (k_msgq_put(&drv_data->rx_queue, &out, K_NO_WAIT) != 0) {
			t5838_diag_dropped += sets * drv_data->num_channels;
			k_mem_slab_free(drv_data->mem_slab, out);
		}
	} else {
		t5838_diag_dropped += sets * drv_data->num_channels;
	}

	if (drv_data->active) {
		(void)pdm_dma_start_block(dev);
	}
}

static int alif_pdm_trigger_start(const struct device *dev)
{
	const struct t5838_drv_cfg *drv_cfg = dev->config;
	struct t5838_drv_data *drv_data = dev->data;

	// apply pinctrl state again
	if (drv_cfg->pcfg != NULL) {
		pinctrl_apply_state(drv_cfg->pcfg, PINCTRL_STATE_DEFAULT);
	}

	drv_data->block = NULL;
	drv_data->block_len = 0;

	// Purge any stale data in the message queue
	k_msgq_purge(&drv_data->rx_queue);

	drv_data->discard_samples = (uint32_t)(alif_pdm_get_sample_rate(drv_data->mode) * drv_cfg->discard_duration_ms / 1000.0 * drv_data->num_channels);

	// Clear FIFO to remove stale data before starting
	sys_write32(PDM_FIFO_CLEAR, drv_cfg->regs + PDM_CONFIG_REGISTER);

	// Clear any pending interrupts
	sys_read32(drv_cfg->regs + PDM_AUDIO_DETECT_IRQ);
	sys_read32(drv_cfg->regs + PDM_WARN_IRQ);

	sys_write32(drv_data->mode << PDM_CLK_MODE | drv_data->channel_map,
		    (drv_cfg->regs + PDM_CONFIG_REGISTER));

	if (drv_cfg->dma_dev != NULL) {
		/* DMA drain: the watermark feeds the dma2 handshake. Enable
		 * ONLY the overflow ERROR IRQ for loss visibility (it should
		 * now read ~0); the ISR early-returns in DMA mode without
		 * touching the FIFO data registers, so it can't race the DMA.
		 */
		int ret;

		sys_write32(PDM_FIFO_OVERFLOW_IRQ,
			    drv_cfg->regs + PDM_INTERRUPT_REGISTER);
		pdm_configure_dma_event_router(LPPDM_DMA_GROUP, drv_cfg->dma_req);
		drv_data->active = true;
		ret = pdm_dma_start_block(dev);
		if (ret < 0) {
			LOG_ERR("PDM DMA start failed: %d", ret);
			drv_data->active = false;
		}
		return ret;
	}

	/* enable interrupts: almost-full drives the drain; overflow makes
	 * FIFO-full sample loss VISIBLE (status + counter). Without it the
	 * hardware drops silently - measured 2026-07-13 on the dev kit: 0.2%
	 * of capture vanishing under duplex load with stat_max pinned at 8
	 * and zero counted events (the upstream alif_pdm driver enables
	 * both).
	 */
	sys_write32(PDM_FIFO_ALMOST_FULL_IRQ | PDM_FIFO_OVERFLOW_IRQ,
		    drv_cfg->regs + PDM_INTERRUPT_REGISTER);

	drv_data->active = true;

	return 0;
}

static int alif_pdm_trigger_stop(const struct device *dev)
{
	const struct t5838_drv_cfg *drv_cfg = dev->config;
	struct t5838_drv_data *drv_data = dev->data;

	if (drv_cfg->dma_dev != NULL) {
		/* clear active first so the completion callback won't re-arm,
		 * then stop the channel
		 */
		drv_data->active = false;
		dma_stop(drv_cfg->dma_dev, drv_cfg->dma_ch);
	}

	// disable channel
	sys_write32(0, drv_cfg->regs + PDM_CONFIG_REGISTER);

	// disable interrupt
	sys_write32(0, drv_cfg->regs + PDM_INTERRUPT_REGISTER);

	drv_data->active = false;

	// Free the in-progress block while the slab is still valid
	if (drv_data->block != NULL && drv_data->mem_slab != NULL) {
		k_mem_slab_free(drv_data->mem_slab, drv_data->block);
		drv_data->block = NULL;
		drv_data->block_len = 0;
	}

	// Drain all queued blocks back to the slab before it gets destroyed
	if (drv_data->mem_slab != NULL) {
		void *queued_block;
		while (k_msgq_get(&drv_data->rx_queue, &queued_block, K_NO_WAIT) == 0) {
			k_mem_slab_free(drv_data->mem_slab, queued_block);
		}
	}

	return 0;
}

/* Diagnostic ledger for the duplex mic-rate anomaly (read via
 * frame.microphone.aec('stats')): samples popped from the FIFO, samples
 * dropped before reaching the rx queue, overflow-clear events, ISR count.
 */
uint64_t t5838_diag_popped;
uint64_t t5838_diag_dropped;
uint64_t t5838_diag_discarded; /* intentional start-of-stream discards */
uint32_t t5838_diag_overflows;
/* ISR-entry gap tracker: how long interrupts were effectively held off.
 * With the FIFO filling at 16kHz, entry gaps bound the deadline misses -
 * gap_max is the worst blackout seen, gap_over counts gaps that ate the
 * whole watermark-to-full slack. Cycle-counter based: undercounts across
 * idle sleep, but blackouts under load (the case that overflows) are
 * measured true.
 */
uint32_t t5838_diag_gap_max_us;
uint32_t t5838_diag_gap_over;
/* estimated frames lost to FIFO-full (per overflow event: the entry gap
 * minus the 8-frame FIFO capacity, at 16 frames/ms; floor one frame).
 * Consumed by the AEC's exact capture-timeline loss accounting - a
 * measured estimate through the exact path beats an inferred lateness
 * floor (the floor is noisy under load and over-slides).
 */
uint64_t t5838_diag_lost_est;
uint32_t t5838_diag_isrs;
uint32_t t5838_diag_stat_max;  /* highest FIFO_STAT count observed */
uint32_t t5838_diag_stat_gt8;  /* FIFO_STAT reads above the 8-deep FIFO */

static void alif_pdm_isr(const struct device *dev)
{
	const struct t5838_drv_cfg *drv_cfg = dev->config;
	struct t5838_drv_data *drv_data = dev->data;
	int ret;
	uint16_t i, k = 0;
	uint32_t fifo_items;
	uint32_t init_status;
	uint16_t available = 0;
	uint16_t remaining = 0;
	uint32_t audio_ch_0_1;
	uint32_t audio_ch_2_3;
	uint32_t audio_ch_4_5;
	uint32_t audio_ch_6_7;
	uint16_t *data = &drv_data->rx_data[0];
	uint32_t to_discard = 0;

	uint8_t audio_ch = drv_data->channel_map & PDM_CHANNEL_ENABLE;

	t5838_diag_isrs++;

	if (drv_cfg->dma_dev != NULL) {
		/* DMA owns the FIFO data registers. Only service the overflow
		 * ERROR status here (read to clear the sticky bit + count) -
		 * with the DMA draining it should essentially never fire, which
		 * is exactly the signal this prototype is measuring. NEVER read
		 * the AUDIO_OUT registers: that would steal sets from the DMA.
		 */
		uint32_t err = sys_read32(drv_cfg->regs + PDM_ERROR_IRQ);

		if (err & PDM_FIFO_OVERFLOW_STATUS) {
			t5838_diag_overflows++;
			/* Unlike the non-DMA path below, the DMA drains the FIFO
			 * continuously, so there is no ISR entry-gap to size the
			 * hole from. Report a minimal fixed FIFO-depth estimate so
			 * the loss still routes through the AEC's exact loss path
			 * (audio_aec_note_mic_loss) and it re-anchors cleanly;
			 * any residual under-estimate is caught by the late-floor
			 * backstop rather than left entirely uncounted.
			 */
			t5838_diag_lost_est += PDM_FIFO_DEPTH_FRAMES;
		}
		return;
	}

	static uint32_t last_isr_cyc;
	uint32_t now_cyc = k_cycle_get_32();
	uint32_t gap_us = 0;

	if (drv_data->active && last_isr_cyc != 0) {
		gap_us = k_cyc_to_us_floor32(now_cyc - last_isr_cyc);

		if (gap_us > t5838_diag_gap_max_us) {
			t5838_diag_gap_max_us = gap_us;
		}
		/* a drain empties the FIFO, so overflow needs an entry gap
		 * longer than the full FIFO refill (8 samples @16kHz):
		 * count those - they should track pdm_overflows 1:1
		 */
		if (gap_us > 8 * 63) {
			t5838_diag_gap_over++;
		}
	}
	last_isr_cyc = now_cyc;

	/* identify (and by reading, ACK) both IRQ sources up front: WARN
	 * (almost-full, level) and ERROR (overflow, edge-STICKY - unread it
	 * holds the OR'ed LPPDM_IRQ line asserted forever)
	 */
	uint32_t err_status = sys_read32(drv_cfg->regs + PDM_ERROR_IRQ);

	init_status = sys_read32(drv_cfg->regs + PDM_WARN_IRQ);
	fifo_items = sys_read32(drv_cfg->regs + PDM_FIFO_STATUS_REGISTER);
	if (fifo_items > t5838_diag_stat_max) {
		t5838_diag_stat_max = fifo_items;
	}
	if (fifo_items > 8) {
		t5838_diag_stat_gt8++;
	}

	/* FIFO overflow: DRAIN, do not clear-and-reconfigure. The overflow
	 * itself lost only the sample(s) the decimator could not push while
	 * the FIFO was full; the FIFO still holds 8 good samples, and a
	 * FIFO clear + decimator restart would throw them (plus the restart
	 * time) away as an UNCOUNTED hole in the capture timeline - enough
	 * of those and the AEC's mic-anchored reference window walks off
	 * its ring and cancellation dies (the 2026-07-13 live-duplex
	 * failure; the upstream alif_pdm driver never clears on overflow
	 * either). The event is counted and the loss SIZED from the entry
	 * gap: everything past the FIFO's 8-frame capacity is frames the
	 * decimator produced with nowhere to put them. The AEC consumes
	 * the estimate through its exact loss-accounting path; only the
	 * estimate's residual error is left to the late-floor backstop.
	 */
	if (err_status & PDM_FIFO_OVERFLOW_STATUS) {
		uint32_t lost = (gap_us > 8 * 63)
			? (gap_us - 8 * 63) * 16u / 1000u : 0u;

		t5838_diag_overflows++;
		t5838_diag_lost_est += (lost > 0u) ? lost : 1u;
	} else if (!(init_status & PDM_FIFO_ALMOST_FULL_IRQ)) {
		return;
	}

	for (i = 0; i < fifo_items; i++) {
		audio_ch_0_1 = sys_read32(drv_cfg->regs + PDM_CH0_CH1_AUDIO_OUT);
		audio_ch_2_3 = sys_read32(drv_cfg->regs + PDM_CH2_CH3_AUDIO_OUT);
		audio_ch_4_5 = sys_read32(drv_cfg->regs + PDM_CH4_CH5_AUDIO_OUT);
		audio_ch_6_7 = sys_read32(drv_cfg->regs + PDM_CH6_CH7_AUDIO_OUT);

		if ((audio_ch & PDM_CHANNEL_0) == PDM_CHANNEL_0) {
			data[k++] = (uint16_t)(audio_ch_0_1);
		}
		if ((audio_ch & PDM_CHANNEL_1) == PDM_CHANNEL_1) {
			data[k++] = (uint16_t)(audio_ch_0_1 >> 16);
		}
		/* FIXME: Swap channel 2/3 order for correct L/R stereo mapping (ch3=left, ch2=right)
		 * This is a temporary workaround until proper channel mapping is implemented
		 */
		if ((audio_ch & PDM_CHANNEL_3) == PDM_CHANNEL_3) {
			data[k++] = (uint16_t)(audio_ch_2_3 >> 16);
		}
		if ((audio_ch & PDM_CHANNEL_2) == PDM_CHANNEL_2) {
			data[k++] = (uint16_t)(audio_ch_2_3);
		}
		if ((audio_ch & PDM_CHANNEL_4) == PDM_CHANNEL_4) {
			data[k++] = (uint16_t)(audio_ch_4_5);
		}
		if ((audio_ch & PDM_CHANNEL_5) == PDM_CHANNEL_5) {
			data[k++] = (uint16_t)(audio_ch_4_5 >> 16);
		}
		if ((audio_ch & PDM_CHANNEL_6) == PDM_CHANNEL_6) {
			data[k++] = (uint16_t)(audio_ch_6_7);
		}
		if ((audio_ch & PDM_CHANNEL_7) == PDM_CHANNEL_7) {
			data[k++] = (uint16_t)(audio_ch_6_7 >> 16);
		}
	}

	t5838_diag_popped += k;

	/* Intentional start-of-stream discards are counted separately from
	 * losses: t5838_diag_dropped is consumed by the AEC's capture-
	 * timeline loss accounting, and the capture timeline starts AFTER
	 * the discard window - counting discards there would inject a huge
	 * phantom loss at every session start.
	 */
	if (drv_data->discard_samples > 0) {
		if (drv_data->discard_samples >= k) {
			drv_data->discard_samples -= k;
			t5838_diag_discarded += k;
			// clear interrupt and discard all data
			sys_read32(drv_cfg->regs + PDM_AUDIO_DETECT_IRQ);
			return;
		} else {
			to_discard = drv_data->discard_samples;
			drv_data->discard_samples = 0;
			t5838_diag_discarded += to_discard;
		}
	}

	if (to_discard > 0) {
		k -= to_discard;
		data += to_discard;
	}

	// push to queue
	if (drv_data->block == NULL) {
		ret = k_mem_slab_alloc(drv_data->mem_slab, (void **)&drv_data->block, K_NO_WAIT);
		if (ret != 0) {
			// Drop this batch but keep interrupt enabled to retry next time
			t5838_diag_dropped += k;
			sys_read32(drv_cfg->regs + PDM_AUDIO_DETECT_IRQ);
			return;
		}
	}

	available = drv_data->block_size - drv_data->block_len;

	if (drv_data->pcm_width == 16) {

		// fit data to block
		if (k * sizeof(int16_t) < available) {
			memcpy(drv_data->block + drv_data->block_len, data,
			       k * sizeof(int16_t));
			drv_data->block_len += k * sizeof(int16_t);
		} else {
			// copy available data
			memcpy(drv_data->block + drv_data->block_len, data,
			       drv_data->block_size - drv_data->block_len);
			if (k_msgq_put(&drv_data->rx_queue, &drv_data->block, K_NO_WAIT) != 0) {
				/* queue full: block lost (and leaked - pre-existing) */
				t5838_diag_dropped += drv_data->block_size / sizeof(int16_t);
			}

			remaining =
				k * sizeof(int16_t) - (drv_data->block_size - drv_data->block_len);

			// reallocate new block
			ret = k_mem_slab_alloc(drv_data->mem_slab, (void **)&drv_data->block,
					       K_NO_WAIT);
			if (ret != 0) {
				drv_data->block = NULL;
				drv_data->block_len = 0;
				// Drop remaining data but keep interrupt enabled
				t5838_diag_dropped += remaining / sizeof(int16_t);
				sys_read32(drv_cfg->regs + PDM_AUDIO_DETECT_IRQ);
				return;
			}
			// copy remaining data
			memcpy(drv_data->block, data + (available / 2), remaining);
			drv_data->block_len = remaining;
		}
	} else {
		// pcm width 8,  only take high byte
		if (k < available) {
			for (int i = 0; i < k; i++) {
				drv_data->block[drv_data->block_len] =
					(int8_t)(data[i] >> 8);
				drv_data->block_len++;
			}
		} else {
			// copy available data
			for (int i = 0; i < available; i++) {
				drv_data->block[drv_data->block_len++] =
					(int8_t)(data[i] >> 8);
			}
			k_msgq_put(&drv_data->rx_queue, &drv_data->block, K_NO_WAIT);
			remaining = k - available;
			// reallocate new block
			ret = k_mem_slab_alloc(drv_data->mem_slab, (void **)&drv_data->block,
					       K_NO_WAIT);
			if (ret != 0) {
				drv_data->block = NULL;
				drv_data->block_len = 0;
				// Drop remaining data but keep interrupt enabled
				sys_read32(drv_cfg->regs + PDM_AUDIO_DETECT_IRQ);
				return;
			}
			// copy remaining data
			drv_data->block_len = 0;
			for (int i = 0; i < remaining; i++) {
				drv_data->block[drv_data->block_len++] =
					(int8_t)(data[i] >> 8);
			}
		}
	}

	// clear interrupt
	sys_read32(drv_cfg->regs + PDM_AUDIO_DETECT_IRQ);

	return;
}

static int alif_pdm_init(const struct device *dev)
{

	const struct t5838_drv_cfg *drv_cfg = dev->config;
	struct t5838_drv_data *drv_data = dev->data;

	// if (drv_cfg->pcfg != NULL) {
	// 	pinctrl_apply_state(drv_cfg->pcfg, PINCTRL_STATE_DEFAULT);
	// }

	// enable clock
	sys_set_bits(HE_PER_CLK_EN, BIT(8));

	drv_data->active = false;
	drv_data->configured = false;
	drv_data->mem_slab = NULL;
	drv_data->block_size = 0;
	drv_data->channel_map = 0;
	drv_data->mode = 0;
	drv_data->block = NULL;
	drv_data->block_len = 0;

	k_msgq_init(&drv_data->rx_queue, drv_data->rx_queue_buf, sizeof(void *),
		    drv_cfg->queue_depth);

	// irq config
	if (drv_cfg->irq_config != NULL) {
		drv_cfg->irq_config(dev);
	}

	// bypass (+ DMA handshake flow control when a rxdma is wired)
	sys_write32((drv_cfg->fir_bypass << PDM_BYPASS_FIR) |
			    (drv_cfg->iir_bypass << PDM_BYPASS_IIR) |
			    (drv_cfg->dma_dev ? PDM_CTL1_USE_DMA : 0U),
		    (drv_cfg->regs + PDM_CTL_REGISTER));

	// fifo watermark
	sys_write32(drv_cfg->fifo_watermark, (drv_cfg->regs + PDM_THRESHOLD_REGISTER));

	LOG_INF("ALIF PDM driver init done");

	return 0;
}
static int alif_pdm_configure(const struct device *dev, struct dmic_cfg *config)
{
	const struct t5838_drv_cfg *drv_cfg = dev->config;
	struct t5838_drv_data *drv_data = dev->data;

	if (drv_data->active) {
		LOG_ERR("Cannot configure device while it is active");
		return -EBUSY;
	}

	if (config->channel.req_num_streams != 1 ||
	    config->channel.req_num_chan > MAX_NUM_CHANNELS ||
	    config->channel.req_chan_map_hi != 0) {
		LOG_ERR("Invalid channel configuration, only 1 stream and 8 channels are "
			"supported");
		return -EINVAL;
	}

	if (config->streams[0].pcm_width != 8 && config->streams[0].pcm_width != 16) {
		LOG_ERR("Invalid pcm width configuration, only 8bit or 16bit are supported");
		return -EINVAL;
	}

	config->channel.act_num_chan = config->channel.req_num_chan;
	config->channel.act_num_streams = config->channel.req_num_streams;
	config->channel.act_chan_map_hi = config->channel.req_chan_map_hi;
	config->channel.act_chan_map_lo = config->channel.req_chan_map_lo;

	drv_data->mem_slab = config->streams[0].mem_slab;
	drv_data->block_size = config->streams[0].block_size;
	drv_data->channel_map = config->channel.act_chan_map_lo;
	drv_data->pcm_width = config->streams[0].pcm_width;
	drv_data->mode = alif_pdm_find_mode(config);

	// TODO: Dynamic gain control
	for (int i = 0; i < MAX_NUM_CHANNELS; i++) {
		if (drv_data->channel_map & (1 << i)) {
			sys_write32(0x000000000, (drv_cfg->regs + PDM_CH_GAIN + i * PDM_CH_OFFSET));
		}
	}

	/* Per-channel PDM phase. Two mics on a shared PDM bus are edge-multiplexed
	 * (Right on one clock edge, Left on the other), so the two channels of a
	 * stereo pair must sample at DIFFERENT phases to separate them. Alif's
	 * stereo DMIC sample uses 0x1F on the even channel and 0x03 on the odd
	 * channel of a pair. Writing the same phase to both (the previous
	 * behaviour) left the second mic mistimed -> inverted/comb -> buzz +
	 * negative L/R correlation. The even channel keeps 0x1F, matching the
	 * known-clean mono path (ch2). */
	for (int i = 0; i < MAX_NUM_CHANNELS; i++) {
		if (drv_data->channel_map & (1 << i)) {
			uint32_t phase = (i & 1) ? 0x00000003 : 0x0000001F;

			sys_write32(phase, (drv_cfg->regs + PDM_CH_PHASE + i * PDM_CH_OFFSET));
		}
	}

	// TODO: Dynamic Threshold control
	for (int i = 0; i < MAX_NUM_CHANNELS; i++) {
		if (drv_data->channel_map & (1 << i)) {
			sys_write32(0x00050000,
				    (drv_cfg->regs + PDM_CH_PKDET_TH + i * PDM_CH_OFFSET));
		}
	}

	// TODO: Dynamic Interval control
	for (int i = 0; i < MAX_NUM_CHANNELS; i++) {
		if (drv_data->channel_map & (1 << i)) {
			/* peak-detect interval: historically watermark - 1
			 * with watermark 6; pinned at 5 so lowering the
			 * FIFO watermark (overflow headroom) cannot change
			 * audio-detect behaviour as a side effect
			 */
			sys_write32(5,
				    (drv_cfg->regs + PDM_CH_PKDET_ITV + i * PDM_CH_OFFSET));
		}
	}

	uint8_t num_channels = 0;
	for (int i = 0; i < 8; i++) {
		if (drv_data->channel_map & (1 << i)) num_channels++;
	}
	drv_data->num_channels = num_channels;
	uint32_t sample_rate = alif_pdm_get_sample_rate(drv_data->mode);
	drv_data->discard_samples = (uint32_t)(sample_rate * drv_cfg->discard_duration_ms / 1000.0 * num_channels);

	drv_data->configured = true;

	LOG_INF("ALIF PDM driver configured");

	return 0;
}

static int alif_pdm_trigger(const struct device *dev, enum dmic_trigger trigger)
{

	struct t5838_drv_data *drv_data = dev->data;

	switch (trigger) {
	case DMIC_TRIGGER_START:
		if (drv_data->configured == false) {
			LOG_ERR("Device not configured");
			return -EIO;
		}
		if (!drv_data->active) {
			alif_pdm_trigger_start(dev);
		}
		break;
	case DMIC_TRIGGER_STOP:
		if (drv_data->active) {
			alif_pdm_trigger_stop(dev);
		}
		break;
	default:
		LOG_ERR("Invalid trigger");
		return -EINVAL;
	}

	return 0;
}
static int alif_pdm_read(const struct device *dev, uint8_t stream, void **buffer, size_t *size,
			 int32_t timeout)
{

	struct t5838_drv_data *drv_data = dev->data;
	int ret;
	ARG_UNUSED(stream);

	if (!drv_data->configured || !drv_data->active) {
		LOG_ERR("Device not configured or not active");
		return -EIO;
	}

	ret = k_msgq_get(&drv_data->rx_queue, buffer, SYS_TIMEOUT_MS(timeout));

	if (ret == 0) {
		*size = drv_data->block_size;
	}

	return ret;
}

int dmic_set_gain(const struct device *dev, int8_t gain)
{
	struct t5838_drv_data *drv_data = dev->data;
	const struct t5838_drv_cfg *drv_cfg = dev->config;
	uint16_t raw_gain = 0;

	if (gain < -10) {
		gain = -10;
	} else if (gain > 10) {
		gain = 10;
	}

	if (gain < 0) {
		raw_gain = -gain;
	} else {
		raw_gain = ((gain + 1) * 22) << 4;
	}

	for (int i = 0; i < MAX_NUM_CHANNELS; i++) {
		if (drv_data->channel_map & (1 << i)) {
			sys_write32(raw_gain, (drv_cfg->regs + PDM_CH_GAIN + i * PDM_CH_OFFSET));
		}
	}

	return 0;
}

static const struct _dmic_ops alif_pdm_api = {
	.configure = alif_pdm_configure,
	.trigger = alif_pdm_trigger,
	.read = alif_pdm_read,
};

#define T5838_ALIF_PDM_INIT(inst)                                                                  \
	IF_ENABLED(CONFIG_PINCTRL, (PINCTRL_DT_INST_DEFINE(inst);))                                \
	static void *rx_quene_buf##inst[DT_INST_PROP(inst, queue_depth)];                          \
	static uint16_t rx_data##inst[MAX_NUM_CHANNELS * MAX_DATA_ITEMS];                          \
	static void alif_pdm_irq_config_func_##inst(const struct device *dev)                      \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(inst), DT_INST_IRQ(inst, priority), alif_pdm_isr,         \
			    DEVICE_DT_INST_GET(inst), 0);                                          \
		irq_enable(DT_INST_IRQN(inst));                                                    \
	};                                                                                         \
	static struct t5838_drv_cfg alif_pdm_config_##inst = {                                     \
		.regs = (uint32_t)DT_INST_REG_ADDR(inst),                                          \
		.fifo_watermark = DT_INST_PROP(inst, fifo_watermark),                              \
		.queue_depth = DT_INST_PROP(inst, queue_depth),                                    \
		.gain = DT_INST_PROP(inst, gain),                                                  \
		.iir_bypass = DT_INST_PROP(inst, iir_bypass),                                      \
		.fir_bypass = DT_INST_PROP(inst, fir_bypass),                                      \
		.discard_duration_ms = DT_INST_PROP(inst, discard_duration_ms),                    \
		.irq_config = alif_pdm_irq_config_func_##inst,                                     \
		IF_ENABLED(CONFIG_PINCTRL, (.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(inst), ))       \
		COND_CODE_1(DT_INST_DMAS_HAS_NAME(inst, rxdma),                                    \
			    (.dma_dev = DEVICE_DT_GET(DT_INST_DMAS_CTLR_BY_NAME(inst, rxdma)),     \
			     .dma_ch = DT_INST_DMAS_CELL_BY_NAME(inst, rxdma, channel),            \
			     .dma_req = DT_INST_DMAS_CELL_BY_NAME(inst, rxdma, periph), ),         \
			    (.dma_dev = NULL, ))};                                                 \
	static struct t5838_drv_data t5838_drv_data##inst = {                                      \
		.rx_data = rx_data##inst,                                                          \
		.rx_queue_buf = rx_quene_buf##inst,                                                \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, &alif_pdm_init, NULL, &t5838_drv_data##inst,                   \
			      &alif_pdm_config_##inst, POST_KERNEL, CONFIG_T5838_INIT_PRIORITY,    \
			      &alif_pdm_api);

DT_INST_FOREACH_STATUS_OKAY(T5838_ALIF_PDM_INIT);
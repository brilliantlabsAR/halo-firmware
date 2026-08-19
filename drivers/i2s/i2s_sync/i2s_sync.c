/* Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/cache.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/dma.h>
#include <drivers/i2s_sync.h>
#include <soc.h>

#include "i2s_sync_int.h"

LOG_MODULE_REGISTER(i2s_sync, CONFIG_I2S_SYNC_LOG_LEVEL);

#define DT_DRV_COMPAT alif_i2s_sync

#define I2S_CLK_SRC_HZ 76800000

#define I2S_CLK_DIVISOR_MAX 0x3FF
#define I2S_CLK_DIVISOR_MIN 2

#define DMA_I2S0_RX_GROUP 0x1
#define DMA_I2S0_TX_GROUP 0x1

#define EVTRTR2_DMA_CTRL_ENA        (1U << 4)
#define EVTRTR2_DMA_CTRL_ACK_PERIPH (0x0 << 16)
#define EVTRTR2_DMA_CTRL_ACK_ROUTER (0x1 << 16)

struct i2s_sync_channel {
	i2s_sync_cb_t cb;
	void *buf;
	size_t block_bytes;
	size_t samples;
	size_t count;
	size_t idx;
	bool overrun;
	bool running;
};

struct i2s_sync_data {
	struct i2s_sync_channel tx;
	struct i2s_sync_channel rx;
	/* Runtime mutable configuration (sample rate, bit depth, channel count) */
	struct {
		uint32_t sample_rate;
		uint32_t bit_depth;
		uint8_t channel_count;
	} runtime_cfg;
};

struct i2s_sync_dma_ch {
	bool enabled;
	uint32_t ch;
	uint32_t request;
};

struct i2s_sync_config_priv {
	struct i2s_t *paddr;
	void (*irq_config)(const struct device *dev);
#ifdef CONFIG_PINCTRL
	const struct pinctrl_dev_config *pincfg;
#endif
	uint32_t sample_rate;
	uint32_t bit_depth;
	uint8_t channel_count;

	const struct device *dma_dev;
	const struct i2s_sync_dma_ch dma_tx;
	const struct i2s_sync_dma_ch dma_rx;
};

static int i2s_register_cb(const struct device *dev, enum i2s_dir dir, i2s_sync_cb_t cb)
{
	struct i2s_sync_data *const dev_data = dev->data;

	if (dir == I2S_DIR_TX) {
		dev_data->tx.cb = cb;
	} else if (dir == I2S_DIR_RX) {
		dev_data->rx.cb = cb;
	} else {
		/* Not possible to register the same callback for both directions, as it would be
		 * impossible to determine within the callback which direction is triggered
		 */
		return -EINVAL;
	}

	LOG_DBG("Registered I2S callback %p for direction %d", cb, dir);

	return 0;
}

static int configure_dma_event_router(const uint32_t dma_group, const uint32_t dma_request)
{
	uint32_t regdata;

	if (dma_group > 3) {
		LOG_ERR("Invalid DMA group %d", dma_group);
		return -EINVAL;
	}

	if (dma_request > 31) {
		LOG_ERR("Invalid DMA peripheral %d", dma_request);
		return -EINVAL;
	}

	/* Enable event router channel */
	regdata = EVTRTR2_DMA_CTRL_ENA + EVTRTR2_DMA_CTRL_ACK_PERIPH + dma_group;
	sys_write32(regdata, EVTRTRLOCAL_DMA_CTRL0 + (dma_request * 0x4));

	/* DMA Handshake enable */
	regdata = sys_read32(EVTRTRLOCAL_DMA_ACK_TYPE0 + (dma_group * 0x4));
	regdata |= (0x1 << dma_request);
	sys_write32(regdata, EVTRTRLOCAL_DMA_ACK_TYPE0 + (dma_group * 0x4));

	return 0;
}

__ramfunc static void dma_tx_callback(const struct device *dma_dev, void *p_user_data,
				      uint32_t const channel, int const status)
{
	const struct device *const dev = p_user_data;
	struct i2s_sync_data *const dev_data = dev->data;
	void *tx_buf = dev_data->tx.buf;

	dev_data->tx.buf = NULL;

	if (dev_data->tx.cb) {
		enum i2s_sync_status cb_status =
			status ? I2S_SYNC_STATUS_TX_ERROR : I2S_SYNC_STATUS_OK;
		dev_data->tx.cb(dev, cb_status, tx_buf);
	}

	if (status) {
		LOG_ERR("I2S:%s tx dma callback ch:%d error: %d", dev->name, channel, status);
		return;
	}
	LOG_DBG("I2S:%s tx dma callback ch:%d completed", dev->name, channel);
}

__ramfunc static int i2s_transmitter_start_dma(const struct device *const dev)
{
	const struct i2s_sync_config_priv *dev_cfg = dev->config;
	struct i2s_t *i2s = dev_cfg->paddr;
	struct i2s_sync_data *dev_data = dev->data;
	/* DMA Burst size is shifter so 1 means 2 bytes, 2 means 4 bytes. */
	/* HWRM states that stereo should be possible by one write to TXDMA register.
	 * This does not seem to work and size must be fixed to 16bits (config value 1)
	 */
	const size_t data_size = 1; /* dev_cfg->channel_count; */
	int ret = 0;

#if CONFIG_DCACHE
	sys_cache_data_flush_and_invd_range(dev_data->tx.buf, dev_data->tx.block_bytes);
#endif

	struct dma_block_config dma_block_cfg = {
		.source_address = POINTER_TO_UINT(dev_data->tx.buf),
		.dest_address = POINTER_TO_UINT(&i2s->TXDMA),
		.block_size = dev_data->tx.block_bytes,
		.source_addr_adj = DMA_ADDR_ADJ_INCREMENT,
		.dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE,
	};

	struct dma_config dma_cfg = {
		.dma_slot = dev_cfg->dma_tx.request,
		.channel_direction = MEMORY_TO_PERIPHERAL,
		.source_data_size = data_size,
		.dest_data_size = data_size,
		.source_burst_length = I2S_FIFO_TRG_LEVEL - 1,
		.dest_burst_length = I2S_FIFO_TRG_LEVEL - 1,
		.head_block = &dma_block_cfg,
		.user_data = (void *)dev,
		.dma_callback = dma_tx_callback,
	};

	ret = dma_config(dev_cfg->dma_dev, dev_cfg->dma_tx.ch, &dma_cfg);
	if (ret < 0) {
		LOG_ERR("I2S:%s tx dma_config failed %d\n", dev->name, ret);
		return ret;
	}

	ret = dma_start(dev_cfg->dma_dev, dev_cfg->dma_tx.ch);
	if (ret < 0) {
		LOG_ERR("I2S:%s tx dma_start failed %d\n", dev->name, ret);
		return ret;
	}

	if (dev_data->tx.running) {
		return 0;
	}

	dev_data->tx.running = true;

	i2s_tx_fifo_clear(i2s);
	i2s_interrupt_clear_tx_overrun(i2s);
	i2s_tx_overrun_interrupt_enable(i2s);
	i2s_tx_channel_enable(i2s);
	i2s_tx_block_enable(i2s);

	LOG_DBG("I2S:%s tx dma started. Bytes %u", dev->name, dev_data->tx.block_bytes);

	return ret;
}

__ramfunc static void i2s_transmitter_start(struct i2s_t *const i2s)
{
	i2s_tx_channel_enable(i2s);
	i2s_tx_interrupt_enable(i2s);
	i2s_tx_block_enable(i2s);
	/* Should immediately get interrupt during which FIFO is filled */
}

__ramfunc static int i2s_send(const struct device *dev, void *buf, size_t len)
{
	if ((buf == NULL) || (len == 0)) {
		return -EINVAL;
	}

	const struct i2s_sync_config_priv *dev_cfg = dev->config;
	struct i2s_sync_data *dev_data = dev->data;

	if (dev_data->tx.buf) {
		return -EINPROGRESS;
	}

	size_t const bytes_per_sample = dev_cfg->bit_depth / 8U;

	if ((len % (dev_cfg->channel_count * bytes_per_sample)) != 0) {
		LOG_ERR("Invalid buffer size");
		return -EINVAL;
	}

	dev_data->tx.buf = buf;
	dev_data->tx.block_bytes = len;

	if (dev_cfg->dma_tx.enabled) {
		/* Configure and start DMA */
		return i2s_transmitter_start_dma(dev);
	}

	dev_data->tx.samples = len / bytes_per_sample;
	dev_data->tx.count = 0;
	dev_data->tx.idx = 0;

	if (!dev_data->tx.running) {
		i2s_transmitter_start(dev_cfg->paddr);
		dev_data->tx.running = true;
	} else {
		i2s_tx_interrupt_enable(dev_cfg->paddr);
	}

	return 0;
}

__ramfunc static void dma_rx_callback(const struct device *dma_dev, void *p_user_data,
				      uint32_t const channel, int const status)
{
	const struct device *const dev = p_user_data;
	struct i2s_sync_data *const dev_data = dev->data;
	void *rx_buf = dev_data->rx.buf;

	dev_data->rx.buf = NULL;

	if (dev_data->rx.cb) {
		enum i2s_sync_status cb_status =
			status ? I2S_SYNC_STATUS_RX_ERROR : I2S_SYNC_STATUS_OK;
		dev_data->rx.cb(dev, cb_status, rx_buf);
	}

	if (status < 0) {
		LOG_ERR("I2S:%s rx dma callback ch:%d error: %d", dev->name, channel, status);
		return;
	}
	LOG_DBG("I2S:%s rx dma callback ch:%d completed", dev->name, channel);
}

__ramfunc static int i2s_receiver_start_dma(const struct device *const dev)
{
	const struct i2s_sync_config_priv *dev_cfg = dev->config;
	struct i2s_t *i2s = dev_cfg->paddr;
	struct i2s_sync_data *dev_data = dev->data;
	const size_t data_size = dev_cfg->channel_count - 1;
	int ret = 0;

	struct dma_block_config dma_block_cfg = {
		.source_address = POINTER_TO_UINT(&i2s->RXDMA),
		.dest_address = POINTER_TO_UINT(dev_data->rx.buf),
		.block_size = dev_data->rx.block_bytes,
		.source_addr_adj = DMA_ADDR_ADJ_NO_CHANGE,
		.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT,
	};

	struct dma_config dma_cfg = {
		.dma_slot = dev_cfg->dma_rx.request,
		.channel_direction = PERIPHERAL_TO_MEMORY,
		/* DMA Burst size is shifter.
		 *   if data_size is 0, then burst size will be 2 bytes.
		 *   if data_size is 1, then burst size will be 4 bytes.
		 */
		.source_data_size = data_size,
		.dest_data_size = data_size,
		.source_burst_length = I2S_FIFO_TRG_LEVEL - 1,
		.dest_burst_length = I2S_FIFO_TRG_LEVEL - 1,
		.head_block = &dma_block_cfg,
		.user_data = (void *)dev,
		.dma_callback = dma_rx_callback,
	};

	ret = dma_config(dev_cfg->dma_dev, dev_cfg->dma_rx.ch, &dma_cfg);
	if (ret < 0) {
		LOG_ERR("I2S:%s rx dma_config failed %d\n", dev->name, ret);
		return ret;
	}

	ret = dma_start(dev_cfg->dma_dev, dev_cfg->dma_rx.ch);
	if (ret < 0) {
		LOG_ERR("I2S:%s rx dma_start failed %d\n", dev->name, ret);
		return ret;
	}

	if (dev_data->rx.running) {
		return 0;
	}

	dev_data->rx.running = true;

	i2s_rx_fifo_clear(i2s);
	i2s_interrupt_clear_rx_overrun(i2s);
	i2s_rx_channel_enable(i2s);
	i2s_rx_block_enable(i2s);

	LOG_DBG("I2S:%s rx dma started. Bytes %u", dev->name, dev_data->rx.block_bytes);

	return ret;
}

__ramfunc static void i2s_receiver_start(struct i2s_t *const i2s)
{
	i2s_rx_channel_enable(i2s);
	i2s_rx_interrupt_enable(i2s);
	i2s_rx_block_enable(i2s);
}

__ramfunc static int i2s_recv(const struct device *dev, void *buf, size_t len)
{
	if ((buf == NULL) || (len == 0)) {
		return -EINVAL;
	}

	const struct i2s_sync_config_priv *dev_cfg = dev->config;
	struct i2s_sync_data *dev_data = dev->data;

	if (dev_data->rx.buf) {
		return -EINPROGRESS;
	}

	size_t const bytes_per_sample = dev_cfg->bit_depth / 8U;

	if ((len % (dev_cfg->channel_count * bytes_per_sample)) != 0) {
		LOG_ERR("Invalid buffer size");
		return -EINVAL;
	}

	dev_data->rx.buf = buf;
	dev_data->rx.block_bytes = len;

	if (dev_cfg->dma_rx.enabled) {
		/* Configure DMA RX */
		return i2s_receiver_start_dma(dev);
	}

	dev_data->rx.samples = len / bytes_per_sample;
	dev_data->rx.count = 0;
	dev_data->rx.idx = 0;

	if (!dev_data->rx.running) {
		i2s_receiver_start(dev_cfg->paddr);
		dev_data->rx.running = true;
	} else {
		i2s_rx_interrupt_enable(dev_cfg->paddr);
	}

	return 0;
}

static void channel_reset(struct i2s_sync_channel *chn)
{
	chn->buf = NULL;
	chn->samples = 0;
	chn->count = 0;
	chn->idx = 0;
}

static void channel_disable(struct i2s_sync_channel *chn)
{
	chn->running = false;
	chn->overrun = false;
	channel_reset(chn);
}

static void i2s_disable_tx(const struct device *dev)
{
	const struct i2s_sync_config_priv *dev_cfg = dev->config;
	struct i2s_sync_data *dev_data = dev->data;
	struct i2s_t *i2s = dev_cfg->paddr;

	if (dev_cfg->dma_tx.enabled) {
		i2s_tx_dma_disable(i2s);
		int ret = dma_stop(dev_cfg->dma_dev, dev_cfg->dma_tx.ch);

		if (ret < 0) {
			LOG_ERR("I2S:%s tx dma_stop failed %d\n", dev->name, ret);
			return;
		}
	}
	i2s_tx_channel_disable(i2s);
	i2s_tx_block_disable(i2s);
	i2s_tx_fifo_interrupt_disable(i2s);
	i2s_tx_overrun_interrupt_disable(i2s);
	i2s_tx_fifo_clear(i2s);
	channel_disable(&dev_data->tx);

	if (dev_cfg->dma_rx.enabled) {
		i2s_rx_dma_disable(i2s);
		int ret = dma_stop(dev_cfg->dma_dev, dev_cfg->dma_rx.ch);

		if (ret < 0) {
			LOG_ERR("I2S:%s rx dma_stop failed %d\n", dev->name, ret);
			return;
		}
	}
}

static void i2s_disable_rx(const struct device *dev)
{
	const struct i2s_sync_config_priv *dev_cfg = dev->config;
	struct i2s_sync_data *dev_data = dev->data;
	struct i2s_t *i2s = dev_cfg->paddr;

	i2s_rx_channel_disable(i2s);
	i2s_rx_block_disable(i2s);
	i2s_rx_fifo_interrupt_disable(i2s);
	i2s_rx_overrun_interrupt_disable(i2s);

	if (dev_cfg->dma_rx.enabled) {
		int ret = dma_stop(dev_cfg->dma_dev, dev_cfg->dma_rx.ch);

		if (ret < 0) {
			LOG_ERR("I2S:%s rx dma_stop failed %d\n", dev->name, ret);
		}
	}

	i2s_rx_fifo_clear(i2s);
	channel_disable(&dev_data->rx);
}

static int i2s_disable(const struct device *dev, enum i2s_dir dir)
{
	switch (dir) {
	case I2S_DIR_TX:
		i2s_disable_tx(dev);
		break;
	case I2S_DIR_RX:
		i2s_disable_rx(dev);
		break;
	case I2S_DIR_BOTH:
		i2s_disable_rx(dev);
		i2s_disable_tx(dev);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int i2s_get_config(const struct device *dev, struct i2s_sync_config *cfg)
{
	if (!dev || !cfg) {
		return -EINVAL;
	}
	struct i2s_sync_data *const dev_data = dev->data;
	cfg->sample_rate = dev_data->runtime_cfg.sample_rate;
	cfg->bit_depth = dev_data->runtime_cfg.bit_depth;
	cfg->channel_count = dev_data->runtime_cfg.channel_count;
	return 0;
}

/* Forward declaration for dynamic reconfiguration routine */
static int configure_clock_source(const struct device *dev);

static int i2s_set_config(const struct device *dev, const struct i2s_sync_config *cfg)
{
	if (!dev || !cfg) {
		return -EINVAL;
	}
	struct i2s_sync_data *dev_data = dev->data;
	const struct i2s_sync_config_priv *dev_cfg = dev->config;
	/* Must be idle */
	if (dev_data->tx.running || dev_data->rx.running || dev_data->tx.buf || dev_data->rx.buf) {
		return -EBUSY;
	}
	if (cfg->bit_depth != 16) {
		return -EINVAL; /* only 16 supported */
	}
	if (cfg->channel_count == 0 || cfg->channel_count > 2) {
		return -EINVAL;
	}
	dev_data->runtime_cfg.sample_rate = cfg->sample_rate;
	dev_data->runtime_cfg.bit_depth = cfg->bit_depth;
	dev_data->runtime_cfg.channel_count = cfg->channel_count;
	/* Reconfigure clock & word length */
	int ret = configure_clock_source(dev);
	if (ret) {
		return ret;
	}
	/* Update word length registers (still fixed to 16 today) */
	i2s_set_rx_wlen(dev_cfg->paddr, dev_data->runtime_cfg.bit_depth);
	i2s_set_tx_wlen(dev_cfg->paddr, dev_data->runtime_cfg.bit_depth);
	LOG_DBG("I2S:%s dynamic reconfig sr=%u depth=%u ch=%u", dev->name,
		dev_data->runtime_cfg.sample_rate, dev_data->runtime_cfg.bit_depth,
		dev_data->runtime_cfg.channel_count);
	return 0;
}

static int enable_clock(struct i2s_t *i2s)
{
	i2s_select_clock_source(i2s);
	i2s_enable_sclk_aon(i2s);
	i2s_enable_module_clk(i2s);
	i2s_global_enable(i2s);
	i2s_configure_clk(i2s);
	i2s_clken(i2s);

	return 0;
}

static int configure_clock_source(const struct device *dev)
{
	const struct i2s_sync_config_priv *dev_cfg = dev->config;
	struct i2s_sync_data *dev_data = dev->data;
	/* Bit clock should be equal to channel_count * bit_depth * sample_rate */
	uint32_t bclk = 2U * dev_data->runtime_cfg.channel_count * dev_data->runtime_cfg.bit_depth *
			dev_data->runtime_cfg.sample_rate;
	uint32_t div = I2S_CLK_SRC_HZ / bclk;
	if ((div > I2S_CLK_DIVISOR_MAX) || (div < I2S_CLK_DIVISOR_MIN)) {
		LOG_ERR("Selected I2S sample rate cannot be acheieved, divisor out of range");
		return -EINVAL;
	}
	i2s_set_clock_divisor(dev_cfg->paddr, div);
	uint32_t bclk_real = I2S_CLK_SRC_HZ / div;
	if (bclk_real != bclk) {
		LOG_WRN("Selected I2S sample rate cannot be achieved, actual BCLK %u, selected %u",
			bclk_real, bclk);
	}
	return 0;
}

static int i2s_sync_init(const struct device *dev)
{
	int ret;
	const struct i2s_sync_config_priv *dev_cfg = dev->config;
	struct i2s_t *i2s = dev_cfg->paddr;

	if (dev_cfg->bit_depth != 16) {
		__ASSERT(0, "Bit depth other than 16 is not yet supported");
		LOG_ERR("Bit depth other than 16 is not yet supported");
		return -EINVAL;
	}

	ret = enable_clock(i2s);
	if (ret) {
		LOG_ERR("Failed to enable clock, err %d", ret);
		return ret;
	}

#ifdef CONFIG_PINCTRL
	/* Set up pincfg if present */
	if (dev_cfg->pincfg != NULL) {
		ret = pinctrl_apply_state(dev_cfg->pincfg, PINCTRL_STATE_DEFAULT);
		if (ret) {
			LOG_ERR("I2S pinctrl failed, err %d", ret);
			return ret;
		}
	}
#endif

	/* Initialise IRQ for this instance */
	dev_cfg->irq_config(dev);

	/* Seed runtime config from static DT config */
	struct i2s_sync_data *dev_data = dev->data;
	dev_data->runtime_cfg.sample_rate = dev_cfg->sample_rate;
	dev_data->runtime_cfg.bit_depth = dev_cfg->bit_depth;
	dev_data->runtime_cfg.channel_count = dev_cfg->channel_count;
	/* Configure I2S peripheral clock */
	ret = configure_clock_source(dev);
	if (ret) {
		return ret;
	}

	LOG_INF("I2S:%s (%p) configured. Clock %u, bits %u", dev->name, i2s,
		dev_data->runtime_cfg.sample_rate, dev_data->runtime_cfg.bit_depth);

	if (dev_cfg->dma_rx.enabled || dev_cfg->dma_tx.enabled) {
		if (!device_is_ready(dev_cfg->dma_dev)) {
			LOG_ERR("I2S:%s DMA %s not ready", dev->name, dev_cfg->dma_dev->name);
			return -ENODEV;
		}

		if (IS_ENABLED(CONFIG_I2S_SYNC_BUFFER_FORMAT_SEQUENTIAL)) {
			LOG_ERR("I2S:%s sequential buffer format not supported", dev->name);
			return -EINVAL;
		}

		/* Enable DMA handshake logic */
		if (dev_cfg->dma_tx.enabled) {
			ret = configure_dma_event_router(DMA_I2S0_TX_GROUP,
							 dev_cfg->dma_tx.request);
			if (ret) {
				return ret;
			}
			i2s_tx_dma_enable(i2s);
			LOG_DBG("I2S:%s TX DMA enabled", dev->name);
		}
		if (dev_cfg->dma_rx.enabled) {
			ret = configure_dma_event_router(DMA_I2S0_RX_GROUP,
							 dev_cfg->dma_rx.request);
			if (ret) {
				return ret;
			}
			i2s_rx_dma_enable(i2s);
			LOG_DBG("I2S:%s RX DMA enabled", dev->name);
		}
	}

	/* Disable RX and TX channels (enabled by default) */
	i2s_rx_channel_disable(i2s);
	i2s_tx_channel_disable(i2s);

	/* Clear both FIFOs */
	i2s_tx_fifo_clear(i2s);
	i2s_rx_fifo_clear(i2s);

	/* Set FIFO trigger level for TX and RX */
	i2s_set_tx_trigger_level(i2s);
	i2s_set_rx_trigger_level(i2s);

	/* Set word length */
	i2s_set_rx_wlen(i2s, dev_data->runtime_cfg.bit_depth);
	i2s_set_tx_wlen(i2s, dev_data->runtime_cfg.bit_depth);

	/* Mask (disable) all interrupts */
	i2s_interrupt_disable_all(i2s);

	return 0;
}

__ramfunc static void i2s_sync_tx_isr_handler(const struct device *dev)
{
	const struct i2s_sync_config_priv *dev_cfg = dev->config;
	struct i2s_sync_data *dev_data = dev->data;
	struct i2s_t *i2s = dev_cfg->paddr;
	int16_t *buf = (int16_t *)dev_data->tx.buf;
	uint32_t tx_free = I2S_FIFO_TRG_LEVEL;

	while (buf && tx_free && (dev_data->tx.count < dev_data->tx.samples)) {
		/* Left channel is always output from first buffer position */
		i2s_write_left_tx(i2s, (uint32_t)buf[dev_data->tx.idx]);

		/* TODO Use correct config variable here! */
		if (dev_cfg->channel_count == 1U) {
			/* In mono mode, right channel is duplicated left channel data */
			i2s_write_right_tx(i2s, (uint32_t)buf[dev_data->tx.idx]);
		} else {
#ifdef CONFIG_I2S_SYNC_BUFFER_FORMAT_SEQUENTIAL
			/* For sequential buffer format, right channel comes from the second half of
			 * buffer
			 */
			i2s_write_right_tx(
				i2s, (uint32_t)buf[dev_data->tx.idx + (dev_data->tx.samples / 2)]);
#else
			/* For interleaved buffer format, right channel comes from next sample of
			 * buffer. Buffer index must be incremented.
			 */
			i2s_write_right_tx(i2s, (uint32_t)buf[++dev_data->tx.idx]);
#endif
		}

		dev_data->tx.idx++;
		dev_data->tx.count += dev_cfg->channel_count;
		tx_free--;
	}

	if (i2s_interrupt_status_tx_overrun(i2s)) {
		/* Clear the interrupt and disable it to avoid triggering again for the same error
		 * condition. Interrupt will be re-enabled on the next call to i2s_sync_send
		 */
		i2s_tx_overrun_interrupt_disable(i2s);
		i2s_interrupt_clear_tx_overrun(i2s);
		dev_data->tx.overrun = true;
	}

	if (dev_data->tx.count == dev_data->tx.samples) {
		i2s_tx_interrupt_disable(i2s);

		dev_data->tx.buf = NULL;
		dev_data->tx.samples = 0;
		dev_data->tx.idx = 0;

		if (dev_data->tx.cb) {
			enum i2s_sync_status status =
				dev_data->tx.overrun ? I2S_SYNC_STATUS_OVERRUN : I2S_SYNC_STATUS_OK;

			dev_data->tx.cb(dev, status, buf);
		}

		dev_data->tx.overrun = false;
	}
}

__ramfunc static void i2s_sync_rx_isr_handler(const struct device *dev)
{
	const struct i2s_sync_config_priv *dev_cfg = (struct i2s_sync_config_priv *)dev->config;
	struct i2s_sync_data *dev_data = dev->data;
	struct i2s_t *i2s = dev_cfg->paddr;
	int16_t *buf = (int16_t *)dev_data->rx.buf;
	uint32_t rx_avail = I2S_FIFO_TRG_LEVEL;

	while (buf && rx_avail && (dev_data->rx.count < dev_data->rx.samples)) {
		/* Left channel is always placed in first buffer position */
		buf[dev_data->rx.idx] = (int16_t)i2s_read_left_rx(i2s);

		if (dev_cfg->channel_count == 1U) {
			/* In mono mode, right channel should be read and then discarded */
			(void)i2s_read_right_rx(i2s);
		} else {
#ifdef CONFIG_I2S_SYNC_BUFFER_FORMAT_SEQUENTIAL
			/* For sequential buffer format, right channel is placed in second half of
			 * buffer
			 */
			buf[dev_data->rx.idx + (dev_data->rx.samples / 2)] =
				(int16_t)i2s_read_right_rx(i2s);
#else
			/* For interleaved buffer format, right channel is placed in the next sample
			 * of buffer. Buffer index must be incremented.
			 */
			buf[++dev_data->rx.idx] = (int16_t)i2s_read_right_rx(i2s);
#endif
		}

		dev_data->rx.idx++;
		dev_data->rx.count += dev_cfg->channel_count;
		rx_avail--;
	}

	if (i2s_interrupt_status_rx_overrun(i2s)) {
		/* Clear the interrupt and disable it to avoid triggering again for the same error
		 * condition. Interrupt will be re-enabled on the next call to i2s_sync_recv
		 */
		i2s_rx_overrun_interrupt_disable(i2s);
		i2s_interrupt_clear_rx_overrun(i2s);
		dev_data->rx.overrun = true;
	}

	if (dev_data->rx.count == dev_data->rx.samples) {
		i2s_rx_interrupt_disable(i2s);

		dev_data->rx.buf = NULL;
		dev_data->rx.samples = 0;
		dev_data->rx.idx = 0;

		if (dev_data->rx.cb) {
			enum i2s_sync_status status =
				dev_data->rx.overrun ? I2S_SYNC_STATUS_OVERRUN : I2S_SYNC_STATUS_OK;

			dev_data->rx.cb(dev, status, buf);
		}

		dev_data->rx.overrun = false;
	}
}

__ramfunc static void i2s_sync_isr(const struct device *dev)
{
	const struct i2s_sync_config_priv *dev_cfg = dev->config;
	struct i2s_sync_data *dev_data = dev->data;
	struct i2s_t *i2s = dev_cfg->paddr;
	const bool tx_overrun = i2s_interrupt_status_tx_overrun(i2s);
	const bool rx_overrun = i2s_interrupt_status_rx_overrun(i2s);

	if ((i2s_interrupt_status_tx_fifo(i2s) || tx_overrun) && dev_data->tx.running) {
		if (!dev_cfg->dma_tx.enabled) {
			i2s_sync_tx_isr_handler(dev);
		} else {
			i2s_interrupt_clear_tx_overrun(i2s);
			LOG_ERR("I2S:%s TX overrun!", dev->name);
		}
	}

	if ((i2s_interrupt_status_rx_fifo(i2s) || rx_overrun) && dev_data->rx.running) {
		if (!dev_cfg->dma_rx.enabled) {
			i2s_sync_rx_isr_handler(dev);
		} else {
			i2s_interrupt_clear_rx_overrun(i2s);
			LOG_ERR("I2S:%s RX overrun!", dev->name);
		}
	}
}

static const struct i2s_sync_driver_api i2s_sync_api = {.register_cb = i2s_register_cb,
							.send = i2s_send,
							.recv = i2s_recv,
							.disable = i2s_disable,
							.get_config = i2s_get_config,
							.set_config = i2s_set_config};

#define I2S_SYNC_INST_DMA_IS_ENABLED(inst)                                                         \
	UTIL_OR(DT_INST_DMAS_HAS_NAME(inst, txdma), DT_INST_DMAS_HAS_NAME(inst, rxdma))

#define I2S_SYNC_DMA_INIT(inst)                                                                    \
	IF_ENABLED(DT_INST_DMAS_HAS_NAME(inst, txdma),                                             \
		   (.dma_tx.enabled = 1,                                                           \
		    .dma_tx.ch = DT_INST_DMAS_CELL_BY_NAME(inst, txdma, channel),                  \
		    .dma_tx.request = DT_INST_DMAS_CELL_BY_NAME(inst, txdma, periph), ))           \
	IF_ENABLED(DT_INST_DMAS_HAS_NAME(inst, rxdma),                                             \
		   (.dma_rx.enabled = 1,                                                           \
		    .dma_rx.ch = DT_INST_DMAS_CELL_BY_NAME(inst, rxdma, channel),                  \
		    .dma_rx.request = DT_INST_DMAS_CELL_BY_NAME(inst, rxdma, periph), ))           \
	COND_CODE_1(DT_INST_DMAS_HAS_NAME(inst, txdma),                                            \
		    (.dma_dev = DEVICE_DT_GET(DT_INST_DMAS_CTLR_BY_NAME(inst, txdma)), ),          \
		    (.dma_dev = DEVICE_DT_GET(DT_INST_DMAS_CTLR_BY_NAME(inst, rxdma)), ))

#define I2S_SYNC_DEFINE(inst)                                                                      \
	static void i2s_sync_irq_config_func_##inst(const struct device *dev)                      \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(inst), DT_INST_IRQ(inst, priority), i2s_sync_isr,         \
			    DEVICE_DT_INST_GET(inst), 0);                                          \
		irq_enable(DT_INST_IRQN(inst));                                                    \
	}                                                                                          \
	IF_ENABLED(DT_INST_NODE_HAS_PROP(inst, pinctrl_0), (PINCTRL_DT_INST_DEFINE(inst)));        \
	static struct i2s_sync_data i2s_sync_data_##inst;                                          \
	static const struct i2s_sync_config_priv i2s_sync_config_##inst = {                        \
		.paddr = (struct i2s_t *)DT_INST_REG_ADDR(inst),                                   \
		.irq_config = i2s_sync_irq_config_func_##inst,                                     \
		IF_ENABLED(DT_INST_NODE_HAS_PROP(inst, pinctrl_0),                                 \
			   (.pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(inst), ))                     \
			.sample_rate = DT_INST_PROP(inst, sample_rate),                            \
		.bit_depth = DT_INST_PROP(inst, bit_depth),                                        \
		.channel_count = DT_INST_PROP(inst, mono_mode) ? 1 : 2,                            \
		COND_CODE_1(I2S_SYNC_INST_DMA_IS_ENABLED(inst), (I2S_SYNC_DMA_INIT(inst)), ())};   \
	DEVICE_DT_INST_DEFINE(inst, i2s_sync_init, NULL, &i2s_sync_data_##inst,                    \
			      &i2s_sync_config_##inst, POST_KERNEL, CONFIG_I2S_INIT_PRIORITY,      \
			      &i2s_sync_api);

DT_INST_FOREACH_STATUS_OKAY(I2S_SYNC_DEFINE)
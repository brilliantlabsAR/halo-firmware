/* Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef _DRIVER_I2S_SYNC_INT_H
#define _DRIVER_I2S_SYNC_INT_H

/**
 * @file
 * @brief Register definitions and register access helper functions for I2S
 */

#include <stdbool.h>
#include <zephyr/types.h>
#include <zephyr/sys/__assert.h>
#include <core_cm55.h>

#define __IOM volatile
#define __OM  volatile
#define __IM  volatile const

struct i2s_t {
	__IOM uint32_t IER;  /*!< Offset:0x0 I2S Global Enable */
	__IOM uint32_t IRER; /*!< Offset:0x4 I2S Rx Block Enable */
	__IOM uint32_t ITER; /*!< Offset:0x8 I2S Tx Block Enable */
	__IOM uint32_t CER;  /*!< Offset:0xC I2S Clock Enable */
	__IOM uint32_t CCR;  /*!< Offset:0x10 I2S Clock Configuration */

	__OM uint32_t RXFFR; /*!< Offset:0x14 I2S  Rx Block FIFO Reset */
	__OM uint32_t TXFFR; /*!< Offset:0x18 I2S  Tx Block FIFO Reset */
	uint32_t RESERVED0;
	union {
		__IM uint32_t LRBR; /*!< Offset:0x20, I2S Left Rx Buffer */
		__OM uint32_t LTHR; /*!< Offset:0x20, I2S Left Tx Holding */
	};
	union {
		__IM uint32_t RRBR; /*!< Offset:0x24, I2S Right Rx Buffer */
		__OM uint32_t RTHR; /*!< Offset:0x24, I2S Right Tx Holding */
	};
	__IOM uint32_t RER;  /*!< Offset:0x28, I2S Rx Channel Enable */
	__IOM uint32_t TER;  /*!< Offset:0x2C, I2S Tx Channel Enable */
	__IOM uint32_t RCR;  /*!< Offset:0x30, I2S Rx Configuration */
	__IOM uint32_t TCR;  /*!< Offset:0x34, I2S Tx Configuration */
	__IM uint32_t ISR;   /*!< Offset:0x38, I2S Interrupt Status */
	__IOM uint32_t IMR;  /*!< Offset:0x3C, I2S Interrupt Mask */
	__IM uint32_t ROR;   /*!< Offset:0x40, I2S Rx Overrun */
	__IM uint32_t TOR;   /*!< Offset:0x44, I2S Tx Overrun */
	__IOM uint32_t RFCR; /*!< Offset:0x48, I2S Rx FIFO Configuration */
	__IOM uint32_t TFCR; /*!< Offset:0x4C, I2S Tx FIFO Configuration */
	__OM uint32_t RFF;   /*!< Offset:0x50, I2S Rx Channel FIFO Reset */
	__OM uint32_t TFF;   /*!< Offset:0x54, I2S Tx Channel FIFO Reset */
	uint32_t RESERVED1[90];
	__IM uint32_t RXDMA; /*!< Offset:0x1C0, I2S Rx Block DMA */
	uint32_t RESERVED2;
	__OM uint32_t TXDMA; /*!< Offset:0x1C8, I2S Tx Block DMA */
	uint32_t RESERVED4[13];
	__IOM uint32_t DMACR; /*!< Offset:0x200, I2S DMA Control */
};

struct clkctl_per_slv_t { /*!< (@ 0x4902F000) CLKCTL_PER_SLV Structure        */
	__IOM uint32_t EXPMST0_CTRL; /*!< (@ 0x00000000) Clock Control Register */
	__IM uint32_t RESERVED;
	__IOM uint32_t UART_CTRL;  /*!< (@ 0x00000008) UART Control Register  */
	__IOM uint32_t CANFD_CTRL; /*!< (@ 0x0000000C) CANFD Control Register */
	__IOM uint32_t I2S0_CTRL;  /*!< (@ 0x00000010) I2Sn Control Register  */
	__IOM uint32_t I2S1_CTRL;  /*!< (@ 0x00000014) I2Sn Control Register  */
	__IOM uint32_t I2S2_CTRL;  /*!< (@ 0x00000018) I2Sn Control Register  */
	__IOM uint32_t I2S3_CTRL;  /*!< (@ 0x0000001C) I2Sn Control Register  */
	__IM uint32_t RESERVED1;
	__IOM uint32_t I3C_CTRL; /*!< (@ 0x00000024) I3C Control Register */
	__IOM uint32_t SSI_CTRL; /*!< (@ 0x00000028) SPI Control Register */
	__IM uint32_t RESERVED2;
	__IOM uint32_t ADC_CTRL; /*!< (@ 0x00000030) ADC Control Register */
	__IOM uint32_t DAC_CTRL; /*!< (@ 0x00000034) DAC Control Register */
	__IOM uint32_t CMP_CTRL; /*!< (@ 0x00000038) CMP Control Register */
	__IM uint32_t RESERVED3;
	__IOM uint32_t FREQ_MON_CTRL0; /*!< (@ 0x00000040) Frequency Monitor 0 Control Register */
	__IM uint32_t FREQ_MON_STAT0;  /*!< (@ 0x00000044) Frequency Monitor 0 Status Register  */
	__IOM uint32_t FREQ_MON_CTRL1; /*!< (@ 0x00000048) Frequency Monitor 1 Control Register */
	__IM uint32_t FREQ_MON_STAT1;  /*!< (@ 0x0000004C) Frequency Monitor 1 Status Register  */
	__IM uint32_t RESERVED4[12];
	__IOM uint32_t GPIO_CTRL0;  /*!< (@ 0x00000080) GPIOn Control Register  */
	__IOM uint32_t GPIO_CTRL1;  /*!< (@ 0x00000084) GPIOn Control Register  */
	__IOM uint32_t GPIO_CTRL2;  /*!< (@ 0x00000088) GPIOn Control Register  */
	__IOM uint32_t GPIO_CTRL3;  /*!< (@ 0x0000008C) GPIOn Control Register  */
	__IOM uint32_t GPIO_CTRL4;  /*!< (@ 0x00000090) GPIOn Control Register  */
	__IOM uint32_t GPIO_CTRL5;  /*!< (@ 0x00000094) GPIOn Control Register  */
	__IOM uint32_t GPIO_CTRL6;  /*!< (@ 0x00000098) GPIOn Control Register  */
	__IOM uint32_t GPIO_CTRL7;  /*!< (@ 0x0000009C) GPIOn Control Register  */
	__IOM uint32_t GPIO_CTRL8;  /*!< (@ 0x000000A0) GPIOn Control Register  */
	__IOM uint32_t GPIO_CTRL9;  /*!< (@ 0x000000A4) GPIOn Control Register  */
	__IOM uint32_t GPIO_CTRL10; /*!< (@ 0x000000A8) GPIOn Control Register */
	__IOM uint32_t GPIO_CTRL11; /*!< (@ 0x000000AC) GPIOn Control Register */
	__IOM uint32_t GPIO_CTRL12; /*!< (@ 0x000000B0) GPIOn Control Register */
	__IOM uint32_t GPIO_CTRL13; /*!< (@ 0x000000B4) GPIOn Control Register */
	__IOM uint32_t GPIO_CTRL14; /*!< (@ 0x000000B8) GPIOn Control Register */
};                                  /*!< Size = 188 (0xbc)             */

struct m55he_cfg_t { /*!< (@ 0x43007000) M55HE_CFG Structure                         */
	__IOM uint32_t HE_DMA_CTRL; /*!< (@ 0x00000000) DMA2 Boot Control Register   */
	__IOM uint32_t HE_DMA_IRQ;  /*!< (@ 0x00000004) DMA2 Boot IRQ Non-Secure Register    */

	__IOM uint32_t
		HE_DMA_PERIPH;      /*!< (@ 0x00000008) DMA2 Boot Peripheral Non-Secure Register */
	__IOM uint32_t HE_DMA_SEL;  /*!< (@ 0x0000000C) DMA2 Select Register    */
	__IOM uint32_t HE_CLK_ENA;  /*!< (@ 0x00000010) Peripheral Clock Enable Register    */
	__IOM uint32_t HE_I2S_CTRL; /*!< (@ 0x00000014) LPI2S Control Register   */
	__IM uint32_t RESERVED[2];
	__IOM uint32_t HE_CAMERA_PIXCLK_R; /*!< (@ 0x00000020) LPCPI Pixel Clock Control Register */
};

#define CLKCTL_PER_SLV_BASE 0x4902F000UL
#define CLKCTL_PER_SLV      ((struct clkctl_per_slv_t *)CLKCTL_PER_SLV_BASE)

#define M55HE_CFG_BASE 0x43007000UL
#define M55HE_CFG      ((struct m55he_cfg_t *)M55HE_CFG_BASE)

/* CLKCTL_PER_SLV I2Sn_CTRL I2S Control field definitions */
#define I2S_CTRL_SCLK_AON         (1U << 20) /* SCLK Always On                          */
#define I2S_CTRL_DIV_BYPASS       (1U << 17) /* Bypass Clock Divider                    */
#define I2S_CTRL_CLK_SEL_Msk      (1U << 16) /* Clock Source Selection Mask             */
#define I2S_CTRL_CLK_SEL_76M8_CLK (0U << 16) /* Enable 76.8MHz Crystal Oscillator Clock */
#define I2S_CTRL_CLK_SEL_EXT_CLK  (1U << 16) /* Enable External Audio clock input       */
#define I2S_CTRL_CKEN             (1U << 12) /* Enable I2S controller clock             */
#define I2S_CTRL_CKDIV_Pos        (0)        /* Clock divider start position            */
#define I2S_CTRL_CKDIV_Msk                                                                         \
	(0x3FF << I2S_CTRL_CKDIV_Pos) /* Clock divider start mask                */

/*!< Number of bytes for 16/32bit resolution*/
#define I2S_16BIT_BUF_TYPE 2
#define I2S_32BIT_BUF_TYPE 4

/*!< Max Audio Resolution for Tx/Rx Channel */
#define I2S_RX_WORDSIZE 32
#define I2S_TX_WORDSIZE 32

/*!< FIFO Depth for Tx & Rx  */
#define I2S_FIFO_DEPTH     16
#define I2S_FIFO_TRG_LEVEL (I2S_FIFO_DEPTH / 2)

/* Register fields and masks */

/* I2S IER.IEN: Global Enable */
#define I2S_IER_IEN_Pos 0U
#define I2S_IER_IEN_Msk (0x1UL << I2S_IER_IEN_Pos)

/* I2S IRER.RXEN: RX block Enable */
#define I2S_IRER_RXEN_Pos 0U
#define I2S_IRER_RXEN_Msk (0x1UL << I2S_IRER_RXEN_Pos)

/* I2S ITER.TXEN: TX block Enable */
#define I2S_ITER_TXEN_Pos 0U
#define I2S_ITER_TXEN_Msk (0x1UL << I2S_ITER_TXEN_Pos)

/* I2S CER.CLKEN: Clock Enable */
#define I2S_CER_CLKEN_Pos 0U
#define I2S_CER_CLKEN_Msk (0x1UL << I2S_CER_CLKEN_Pos)

/* I2S CCR.SCLKG: Clock Gating */
#define I2S_CCR_SCLKG_Pos 0U
#define I2S_CCR_SCLKG_Msk (0x7UL << I2S_CCR_SCLKG_Pos)

/* I2S CCR.WSS: Word Select length */
#define I2S_CCR_WSS_Pos 3U
#define I2S_CCR_WSS_Msk (0x3UL << I2S_CCR_WSS_Pos)

/* I2S RXFFR.RXFFR: Rx Block FIFO Reset */
#define I2S_RXFFR_RXFFR_Pos 0U
#define I2S_RXFFR_RXFFR_Msk (0x1UL << I2S_RXFFR_RXFFR_Pos)

/* I2S TXFFR.TXFFR: Tx Block FIFO Reset */
#define I2S_TXFFR_TXFFR_Pos 0U
#define I2S_TXFFR_TXFFR_Msk (0x1UL << I2S_TXFFR_TXFFR_Pos)

/* I2S RER.RXCHEN: Rx Channel Enable */
#define I2S_RER_RXCHEN_Pos 0U
#define I2S_RER_RXCHEN_Msk (0x1UL << I2S_RER_RXCHEN_Pos)

/* I2S TER.TXCHEN: Tx Channel Enable */
#define I2S_TER_TXCHEN_Pos 0U
#define I2S_TER_TXCHEN_Msk (0x1UL << I2S_TER_TXCHEN_Pos)

/* I2S RCR.WLEN: Data Resolution of Rx */
#define I2S_RCR_WLEN_Pos 0U
#define I2S_RCR_WLEN_Msk (0x7UL << I2S_RCR_WLEN_Pos)

/* I2S TCR.WLEN: Data Resolution of Tx */
#define I2S_TCR_WLEN_Pos 0U
#define I2S_TCR_WLEN_Msk (0x7UL << I2S_TCR_WLEN_Pos)

/* I2S ISR.RXDA: Status of Rx Data Available interrupt */
#define I2S_ISR_RXDA_Pos 0U
#define I2S_ISR_RXDA_Msk (0x1UL << I2S_ISR_RXDA_Pos)

/* I2S ISR.RXFO: Status of Data Overrun interrupt of Rx */
#define I2S_ISR_RXFO_Pos 1U
#define I2S_ISR_RXFO_Msk (0x1UL << I2S_ISR_RXFO_Pos)

/* I2S ISR.TXFE: Status of Tx Empty Trigger Interrupt */
#define I2S_ISR_TXFE_Pos 4U
#define I2S_ISR_TXFE_Msk (0x1UL << I2S_ISR_TXFE_Pos)

/* I2S ISR.TXFO: Status of Data Overrun Interrupt for Tx */
#define I2S_ISR_TXFO_Pos 5U
#define I2S_ISR_TXFO_Msk (0x1UL << I2S_ISR_TXFO_Pos)

/* I2S IMR.RXDAM: Mask Rx Data Available interrupt */
#define I2S_IMR_RXDAM_Pos 0U
#define I2S_IMR_RXDAM_Msk (0x1UL << I2S_IMR_RXDAM_Pos)

/* I2S IMR.RXFOM: Mask Data Overrun interrupt of Rx */
#define I2S_IMR_RXFOM_Pos 1U
#define I2S_IMR_RXFOM_Msk (0x1UL << I2S_IMR_RXFOM_Pos)

/* I2S IMR.TXFEM: Mask Tx Empty Interrupt */
#define I2S_IMR_TXFEM_Pos 4U
#define I2S_IMR_TXFEM_Msk (0x1UL << I2S_IMR_TXFEM_Pos)

/* I2S IMR.TXFOM: Mask Data Overrun Interrupt for Tx */
#define I2S_IMR_TXFOM_Pos 5U
#define I2S_IMR_TXFOM_Msk (0x1UL << I2S_IMR_TXFOM_Pos)

/* I2S ROR.RXCHO: Clear Rx Data Overrun interrupt */
#define I2S_ROR_RXCHO_Pos 0U
#define I2S_ROR_RXCHO_Msk (0x1UL << I2S_ROR_RXCHO_Pos)

/* I2S TOR.TXCHO: Clear Tx Data Overrun interrupt */
#define I2S_TOR_TXCHO_Pos 0U
#define I2S_TOR_TXCHO_Msk (0x1UL << I2S_TOR_TXCHO_Pos)

/* I2S RFCR.RXCHDT: Rx FIFO Trigger Level */
#define I2S_RFCR_RXCHDT_Pos 0U
#define I2S_RFCR_RXCHDT_Msk (0xFUL << I2S_RFCR_RXCHDT_Pos)

/* I2S TFCR.TXCHET: Tx FIFO Trigger Level */
#define I2S_TFCR_TXCHET_Pos 0U
#define I2S_TFCR_TXCHET_Msk (0xFUL << I2S_TFCR_TXCHET_Pos)

/* I2S RFF.RXCHFR: Rx Channel FIFO Reset */
#define I2S_RFF_RXCHFR_Pos 0U
#define I2S_RFF_RXCHFR_Msk (0x1UL << I2S_RFF_RXCHFR_Pos)

/* I2S TFF.TXCHFR: Tx Channel FIFO Reset */
#define I2S_TFF_TXCHFR_Pos 0U
#define I2S_TFF_TXCHFR_Msk (0x1UL << I2S_TFF_TXCHFR_Pos)

/* I2S DMACR.DMAEN_RXBLOCK: DMA Enable for Rx Block */
#define I2S_DMACR_DMAEN_RXBLOCK_Pos 16U
#define I2S_DMACR_DMAEN_RXBLOCK_Msk (0x1UL << I2S_DMACR_DMAEN_RXBLOCK_Pos)

/* I2S DMACR.DMAEN_TXBLOCK: DMA Enable for Tx Block */
#define I2S_DMACR_DMAEN_TXBLOCK_Pos 17U
#define I2S_DMACR_DMAEN_TXBLOCK_Msk (0x1UL << I2S_DMACR_DMAEN_TXBLOCK_Pos)

/* Enums */
enum i2s_wss_t {
	WSS_CLOCK_CYCLES_16 = 0, /*!< 16 sclk cycles */
	WSS_CLOCK_CYCLES_24,     /*!< 24 sclk cycles */
	WSS_CLOCK_CYCLES_32,     /*!< 32 sclk cycles */
	WSS_CLOCK_CYCLES_MAX
};

enum i2s_wlen_t {
	IGNORE_WLEN = 0, /*!< Word length ignored */
	RES_12_BIT,      /*!< 12 bit Data Resolution */
	RES_16_BIT,      /*!< 16 bit Data Resolution */
	RES_20_BIT,      /*!< 20 bit Data Resolution */
	RES_24_BIT,      /*!< 24 bit Data Resolution */
	RES_32_BIT,      /*!< 32 bit Data Resolution */
};

/* I2S Clock register access functions */

static inline __IOM uint32_t *clkctl_from_i2s_base(struct i2s_t *i2s)
{
	uint32_t i2s_base = (uint32_t)i2s;

	switch (i2s_base) {
	case 0x49014000UL:
		return &CLKCTL_PER_SLV->I2S0_CTRL;
	case 0x49015000UL:
		return &CLKCTL_PER_SLV->I2S1_CTRL;
	case 0x43001000UL:
		return &M55HE_CFG->HE_I2S_CTRL;
	case 0x49017000UL:
		return &CLKCTL_PER_SLV->I2S3_CTRL;
	default:
		__ASSERT(0, "I2S instance does not exist");
		return NULL;
	}
}

static inline void i2s_select_clock_source(struct i2s_t *i2s)
{
	__IOM uint32_t *i2s_clkctl = clkctl_from_i2s_base(i2s);

	*i2s_clkctl &= ~I2S_CTRL_CLK_SEL_Msk;
	*i2s_clkctl |= I2S_CTRL_CLK_SEL_76M8_CLK;
}

static inline void i2s_enable_sclk_aon(struct i2s_t *i2s)
{
	__IOM uint32_t *i2s_clkctl = clkctl_from_i2s_base(i2s);

	*i2s_clkctl |= I2S_CTRL_SCLK_AON;
}

static inline void i2s_enable_module_clk(struct i2s_t *i2s)
{
	__IOM uint32_t *i2s_clkctl = clkctl_from_i2s_base(i2s);

	*i2s_clkctl |= I2S_CTRL_CKEN;
}

static inline void i2s_set_clock_divisor(struct i2s_t *i2s, uint32_t div)
{
	__IOM uint32_t *i2s_clkctl = clkctl_from_i2s_base(i2s);

	*i2s_clkctl &= ~I2S_CTRL_CKDIV_Msk;
	*i2s_clkctl |= (div & I2S_CTRL_CKDIV_Msk);
}

/* I2S register access functions */

static inline void i2s_global_enable(struct i2s_t *i2s)
{
	i2s->IER = _VAL2FLD(I2S_IER_IEN, 1U);
}

static inline void i2s_configure_clk(struct i2s_t *i2s)
{
	i2s->CCR = _VAL2FLD(I2S_CCR_SCLKG, 0U) | _VAL2FLD(I2S_CCR_WSS, WSS_CLOCK_CYCLES_16);
}

static inline void i2s_clken(struct i2s_t *i2s)
{
	i2s->CER = _VAL2FLD(I2S_CER_CLKEN, 1U);
}

static inline void i2s_tx_fifo_clear(struct i2s_t *i2s)
{
	i2s->TFF = _VAL2FLD(I2S_TFF_TXCHFR, 1U);
}

static inline void i2s_rx_fifo_clear(struct i2s_t *i2s)
{
	i2s->RFF = _VAL2FLD(I2S_RFF_RXCHFR, 1U);
}

static inline void i2s_set_tx_trigger_level_value(struct i2s_t *i2s, uint32_t const level)
{
	i2s->TFCR = _VAL2FLD(I2S_TFCR_TXCHET, level);
}

static inline void i2s_set_tx_trigger_level(struct i2s_t *i2s)
{
	i2s_set_tx_trigger_level_value(i2s, I2S_FIFO_TRG_LEVEL);
}

static inline void i2s_set_rx_trigger_level_value(struct i2s_t *i2s, uint32_t const level)
{
	i2s->RFCR = _VAL2FLD(I2S_RFCR_RXCHDT, level);
}

static inline void i2s_set_rx_trigger_level(struct i2s_t *i2s)
{
	i2s_set_rx_trigger_level_value(i2s, I2S_FIFO_TRG_LEVEL);
}

static inline enum i2s_wlen_t wlen_to_reg_val(uint32_t wlen)
{
	switch (wlen) {
	case 12:
		return RES_12_BIT;
	case 16:
		return RES_16_BIT;
	case 20:
		return RES_20_BIT;
	case 24:
		return RES_24_BIT;
	case 32:
		return RES_32_BIT;
	default:
		__ASSERT(0, "Selected I2S WLEN is not valid");
		return 0;
	}
}

static inline void i2s_set_rx_wlen(struct i2s_t *i2s, uint32_t wlen)
{
	i2s->RCR = _VAL2FLD(I2S_RCR_WLEN, wlen_to_reg_val(wlen));
}

static inline void i2s_set_tx_wlen(struct i2s_t *i2s, uint32_t wlen)
{
	i2s->TCR = _VAL2FLD(I2S_TCR_WLEN, wlen_to_reg_val(wlen));
}

static inline void i2s_rx_channel_enable(struct i2s_t *i2s)
{
	i2s->RER = _VAL2FLD(I2S_RER_RXCHEN, 1U);
}

static inline void i2s_tx_channel_enable(struct i2s_t *i2s)
{
	i2s->TER = _VAL2FLD(I2S_TER_TXCHEN, 1U);
}

static inline void i2s_rx_channel_disable(struct i2s_t *i2s)
{
	i2s->RER &= ~I2S_RER_RXCHEN_Msk;
}

static inline void i2s_tx_channel_disable(struct i2s_t *i2s)
{
	i2s->TER &= ~I2S_TER_TXCHEN_Msk;
}

static inline void i2s_rx_interrupt_enable(struct i2s_t *i2s)
{
	/* Un-mask the relevant interrupt */
	i2s->IMR &= ~(I2S_IMR_RXDAM_Msk | I2S_IMR_RXFOM_Msk);
}

static inline void i2s_tx_interrupt_enable(struct i2s_t *i2s)
{
	/* Un-mask the relevant interrupt */
	i2s->IMR &= ~(I2S_IMR_TXFEM_Msk | I2S_IMR_TXFOM_Msk);
}

static inline void i2s_tx_overrun_interrupt_enable(struct i2s_t *i2s)
{
	i2s->IMR &= ~I2S_IMR_TXFOM_Msk;
}

static inline void i2s_tx_overrun_interrupt_disable(struct i2s_t *i2s)
{
	i2s->IMR |= I2S_IMR_TXFOM_Msk;
}

static inline void i2s_rx_overrun_interrupt_disable(struct i2s_t *i2s)
{
	i2s->IMR |= I2S_IMR_RXFOM_Msk;
}

static inline void i2s_tx_fifo_interrupt_disable(struct i2s_t *i2s)
{
	i2s->IMR |= I2S_IMR_TXFEM_Msk;
}

static inline void i2s_rx_fifo_interrupt_disable(struct i2s_t *i2s)
{
	i2s->IMR |= I2S_IMR_RXDAM_Msk;
}

static inline void i2s_tx_interrupt_disable(struct i2s_t *i2s)
{
	i2s_tx_overrun_interrupt_disable(i2s);
	i2s_tx_fifo_interrupt_disable(i2s);
}

static inline void i2s_rx_interrupt_disable(struct i2s_t *i2s)
{
	i2s_rx_overrun_interrupt_disable(i2s);
	i2s_rx_fifo_interrupt_disable(i2s);
}

static inline void i2s_interrupt_disable_all(struct  i2s_t *i2s)
{
	i2s->IMR = UINT32_MAX;
}

static inline void i2s_rx_block_enable(struct i2s_t *i2s)
{
	i2s->IRER = _VAL2FLD(I2S_IRER_RXEN, 1U);
}

static inline void i2s_rx_block_disable(struct i2s_t *i2s)
{
	i2s->IRER &= ~I2S_IRER_RXEN_Msk;
}

static inline void i2s_tx_block_enable(struct i2s_t *i2s)
{
	i2s->ITER = _VAL2FLD(I2S_ITER_TXEN, 1U);
}

static inline void i2s_tx_block_disable(struct i2s_t *i2s)
{
	i2s->ITER &= ~I2S_ITER_TXEN_Msk;
}

static inline bool i2s_interrupt_status_tx_overrun(struct i2s_t *i2s)
{
	return (0 != (i2s->ISR & I2S_ISR_TXFO_Msk));
}

static inline bool i2s_interrupt_status_tx_fifo(struct i2s_t *i2s)
{
	return (0 != (i2s->ISR & I2S_ISR_TXFE_Msk));
}

static inline bool i2s_interrupt_status_rx_overrun(struct i2s_t *i2s)
{
	return (0 != (i2s->ISR & I2S_ISR_RXFO_Msk));
}

static inline bool i2s_interrupt_status_rx_fifo(struct i2s_t *i2s)
{
	return (0 != (i2s->ISR & I2S_ISR_RXDA_Msk));
}

static inline void i2s_interrupt_clear_tx_overrun(struct i2s_t *i2s)
{
	uint32_t tmp = i2s->TOR;

	(void)tmp;
}

static inline void i2s_interrupt_clear_rx_overrun(struct i2s_t *i2s)
{
	uint32_t tmp = i2s->ROR;

	(void)tmp;
}

static inline uint32_t i2s_read_left_rx(struct i2s_t *i2s)
{
	return i2s->LRBR;
}

static inline uint32_t i2s_read_right_rx(struct i2s_t *i2s)
{
	return i2s->RRBR;
}

static inline void i2s_write_left_tx(struct i2s_t *i2s, uint32_t data)
{
	i2s->LTHR = data;
}

static inline void i2s_write_right_tx(struct i2s_t *i2s, uint32_t data)
{
	i2s->RTHR = data;
}


static inline void i2s_tx_dma_enable(struct i2s_t *i2s)
{
	i2s->DMACR |= I2S_DMACR_DMAEN_TXBLOCK_Msk;
}

static inline void i2s_tx_dma_disable(struct i2s_t *i2s)
{
	i2s->DMACR &= ~I2S_DMACR_DMAEN_TXBLOCK_Msk;
}

static inline void i2s_rx_dma_enable(struct i2s_t *i2s)
{
	i2s->DMACR |= I2S_DMACR_DMAEN_RXBLOCK_Msk;
}

static inline void i2s_rx_dma_disable(struct i2s_t *i2s)
{
	i2s->DMACR &= ~I2S_DMACR_DMAEN_RXBLOCK_Msk;
}

#endif /* _DRIVER_I2S_SYNC_INT_H */

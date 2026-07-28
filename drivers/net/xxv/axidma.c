/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2026 AMD-Xilinx.
 */

#include <errno.h>
#include <rte_cycles.h>
#include <rte_log.h>

#include "axidma.h"
#include "xxv_ethdev.h"

/* AXI DMA reset with timeout */
int
axidma_reset(struct xxv_eth_dev *priv, uint32_t offset)
{
	uint32_t timeout = DELAY_OF_ONE_MILLISEC;

	/* Reset AXI DMA */
	axidma_write_reg(priv, offset, AXIDMA_CR_RESET);

	/* Wait for reset to complete */
	while (axidma_read_reg(priv, offset) & AXIDMA_CR_RESET) {
		rte_delay_us(1);
		if (--timeout == 0) {
			XXV_LOG(ERR, "AXI DMA reset timeout!");
			return -ETIMEDOUT;
		}
	}

	XXV_LOG(NOTICE, "AXI DMA reset completed");
	return 0;
}

/* AXI DMA register access helpers */
inline uint32_t
axidma_read_reg(struct xxv_eth_dev *dev, uint32_t offset)
{
	return rte_read32((uint8_t *)dev->axidma_base + offset);
}

inline void
axidma_write_reg(struct xxv_eth_dev *dev, uint32_t offset, uint32_t value)
{
	rte_write32(value, (uint8_t *)dev->axidma_base + offset);
}
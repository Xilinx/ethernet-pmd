/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2024 AMD-Xilinx.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <rte_log.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_io.h>
#include <rte_malloc.h>
#include <rte_cycles.h>
#include <rte_memzone.h>
#include <ethdev_driver.h>
#include <bus_platform_driver.h>

#include "xxv_ethdev.h"
#include "axidma.h"

/* Logging registration */
RTE_LOG_REGISTER_DEFAULT(xxv_logtype, NOTICE);

/* Helper functions for register access */
static inline uint32_t
xxv_read_reg(struct xxv_eth_dev *dev, uint32_t offset)
{
	return rte_read32((uint8_t *)dev->reg_base + offset);
}

static inline void
xxv_write_reg(struct xxv_eth_dev *dev, uint32_t offset, uint32_t value)
{
	rte_write32(value, (uint8_t *)dev->reg_base + offset);
}

/* Get private device from eth_dev */
static inline struct xxv_eth_dev *
xxv_eth_dev_private(struct rte_eth_dev *eth_dev)
{
	return eth_dev->data->dev_private;
}

/* Set XXV options */
static void
xxv_setoptions(struct xxv_eth_dev *priv, uint32_t options)
{
	uint32_t reg;

	/* FCS stripping on RX */
	if (options & XAE_OPTION_FCS_STRIP) {
		reg = xxv_read_reg(priv, XXV_RCW1_OFFSET);
		reg |= XXV_RCW1_FCS_MASK;
		xxv_write_reg(priv, XXV_RCW1_OFFSET, reg);
	}

	/* FCS insertion on TX */
	if (options & XAE_OPTION_FCS_INSERT) {
		reg = xxv_read_reg(priv, XXV_TC_OFFSET);
		reg |= XXV_TC_FCS_MASK;
		xxv_write_reg(priv, XXV_TC_OFFSET, reg);
	}

	/* Enable transmitter */
	if (options & XAE_OPTION_TXEN) {
		reg = xxv_read_reg(priv, XXV_TC_OFFSET);
		reg |= XXV_TC_TX_MASK;
		xxv_write_reg(priv, XXV_TC_OFFSET, reg);
	}

	/* Enable receiver */
	if (options & XAE_OPTION_RXEN) {
		reg = xxv_read_reg(priv, XXV_RCW1_OFFSET);
		reg |= XXV_RCW1_RX_MASK;
		xxv_write_reg(priv, XXV_RCW1_OFFSET, reg);
	}

	priv->options |= options;
}

/* Check block lock status */
static int
xxv_check_block_lock(struct xxv_eth_dev *priv)
{
	uint32_t val;
	int timeout = 1000;

	/* Dummy read - this is a sticky bit */
	val = xxv_read_reg(priv, XXV_STATRX_BLKLCK_OFFSET);

	/* Wait for block lock */
	do {
		rte_delay_ms(1);
		timeout--;
		if (timeout <= 0) {
			XXV_LOG(ERR, "Block lock is not set");
			return -ETIMEDOUT;
		}
		val = xxv_read_reg(priv, XXV_STATRX_BLKLCK_OFFSET);
	} while (!(val & XXV_RX_BLKLCK_MASK));

	XXV_LOG(INFO, "Receiver enabled - block lock acquired");
	return 0;
}

/* RX burst function */
static uint16_t
xxv_eth_rx_burst(void *queue, struct rte_mbuf **rx_pkts, uint16_t nb_pkts)
{
	struct axidma_rx_queue *rxq = queue;
	struct axidma_bd *bd;
	struct rte_mbuf *mbuf, *new_mbuf;
	uint16_t nb_rx = 0;
	uint16_t rx_tail;
	uint32_t status;
	rte_iova_t new_iova;
	uint16_t rx_drop = 0;

	rx_tail = rxq->rx_tail;

	for (nb_rx = 0; nb_rx < nb_pkts; nb_rx++) {
		bd = &rxq->bd_ring[rx_tail];

		/* Prefetch next descriptor */
		if (likely(nb_rx + 1 < nb_pkts))
			rte_prefetch0(&rxq->bd_ring[(rx_tail + 1) % rxq->nb_desc]);

		/* Check if descriptor is completed */
		status = bd->status;

		if (!(status & AXIDMA_BD_STS_CMPLT))
			break;

		/* Get mbuf from software ring */
		mbuf = rxq->sw_ring[rx_tail];
		if (unlikely(mbuf == NULL)) {
			/* Memory corruption - descriptor points to invalid mbuf */
			rx_drop++;
			break;
		}

		/* Allocate new mbuf for this descriptor */
		new_mbuf = rte_pktmbuf_alloc(rxq->mb_pool);
		if (unlikely(new_mbuf == NULL)) {
			/* No free mbufs - memory pressure, count as dropped */
			rx_drop++;
			break;
		}

		/* Prefetch mbuf data for next iteration */
		if (likely(nb_rx + 1 < nb_pkts))
			rte_prefetch0(rxq->sw_ring[(rx_tail + 1) % rxq->nb_desc]);

		/* Get packet length from status field (lower 23 bits) 
		 * AXI DMA updates status[22:0] with transferred byte count on RX completion
		 */
		mbuf->data_len = bd->status & 0x007FFFFF;
		mbuf->pkt_len = mbuf->data_len;
		mbuf->port = rxq->queue_id;

		/* Update byte statistics */
		rxq->rx_bytes += mbuf->pkt_len;

		/* Add to output array */
		rx_pkts[nb_rx] = mbuf;

		/* Refill descriptor with new mbuf - cache IOVA calculation */
		rxq->sw_ring[rx_tail] = new_mbuf;
		new_iova = rte_mbuf_data_iova_default(new_mbuf);
		bd->buf_addr = (uint32_t)new_iova;
		bd->buf_addr_msb = (uint32_t)(new_iova >> 32);
		bd->control = rte_pktmbuf_tailroom(new_mbuf);
		bd->status = 0;

		/* Move to next descriptor */
		rx_tail = (rx_tail + 1) % rxq->nb_desc;
	}

	/* Update tail pointer in queue structure */
	if (likely(nb_rx > 0)) {
		/* Ensure all BD writes complete before updating hardware */
		rte_wmb();
		rte_io_wmb();

		/* Update statistics */
		rxq->rx_pkts += nb_rx;

		rxq->rx_tail = rx_tail;

		/* Update hardware tail descriptor pointer to allow DMA to use freed descriptors */
		/* Point to the last refilled descriptor */
		uint16_t tail_index = (rx_tail == 0) ? (rxq->nb_desc - 1) : (rx_tail - 1);
		rte_iova_t tail_bd = rxq->bd_phys_addr + sizeof(struct axidma_bd) * tail_index;

		axidma_write_reg(rxq->priv, AXIDMA_RX_TAILDESC_OFFSET, (uint32_t)tail_bd);
		axidma_write_reg(rxq->priv, AXIDMA_RX_TAILDESC_MSB_OFFSET, (uint32_t)(tail_bd >> 32));
	}

	/* Update error statistics only for actual errors (memory issues) */
	if (unlikely(rx_drop > 0))
		rxq->rx_errors += rx_drop;

	return nb_rx;
}

/* TX burst function */
static uint16_t
xxv_eth_tx_burst(void *queue, struct rte_mbuf **tx_pkts, uint16_t nb_pkts)
{
	struct axidma_tx_queue *txq = queue;
	struct axidma_bd *bd;
	struct rte_mbuf *mbuf;
	uint16_t nb_tx = 0;
	uint16_t tx_tail;
	uint16_t tx_head;
	uint16_t slots_available;
	rte_iova_t mbuf_iova;

	tx_tail = txq->tx_tail;
	tx_head = txq->tx_head;

	/* Calculate available slots in circular queue (reserve 1 slot to distinguish full from empty) */
	slots_available = (txq->nb_desc + tx_tail - tx_head - 1) % txq->nb_desc;

	/* Lazy cleanup: only clean if we don't have enough space */
	if (unlikely(nb_pkts > slots_available)) {
		uint16_t cleaned = 0;

		/* Try to free completed descriptors */
		while (tx_tail != tx_head && cleaned < nb_pkts) {
			bd = &txq->bd_ring[tx_tail];
			if (!(bd->status & AXIDMA_BD_STS_CMPLT))
				break;

			/* Free the transmitted mbuf */
			if (likely(txq->sw_ring[tx_tail] != NULL)) {
				rte_pktmbuf_free(txq->sw_ring[tx_tail]);
				txq->sw_ring[tx_tail] = NULL;
				cleaned++;
			}

			tx_tail = (tx_tail + 1) % txq->nb_desc;
		}

		if (cleaned > 0) {
			txq->tx_tail = tx_tail;
			/* Recalculate available slots after cleanup */
			slots_available = (txq->nb_desc + tx_tail - tx_head - 1) % txq->nb_desc;
		}
	}

	if (nb_pkts > slots_available)
		nb_pkts = slots_available;

	for (nb_tx = 0; nb_tx < nb_pkts; nb_tx++) {
		mbuf = tx_pkts[nb_tx];
		bd = &txq->bd_ring[tx_head];

		/* Prefetch next descriptor */
		if (likely(nb_tx + 1 < nb_pkts))
			rte_prefetch0(&txq->bd_ring[(tx_head + 1) % txq->nb_desc]);

		/* Prefetch next mbuf */
		if (likely(nb_tx + 1 < nb_pkts))
			rte_prefetch0(tx_pkts[nb_tx + 1]);

		/* Setup buffer descriptor - cache IOVA calculation */
		mbuf_iova = rte_mbuf_data_iova(mbuf);
		bd->buf_addr = (uint32_t)mbuf_iova;
		bd->buf_addr_msb = (uint32_t)(mbuf_iova >> 32);
		bd->control = mbuf->pkt_len | AXIDMA_BD_CTRL_TXSOF | AXIDMA_BD_CTRL_TXEOF;
		bd->status = 0;

		/* Store mbuf in software ring */
		txq->sw_ring[tx_head] = mbuf;

		/* Update byte statistics */
		txq->tx_bytes += mbuf->pkt_len;

		/* Move to next descriptor */
		tx_head = (tx_head + 1) % txq->nb_desc;
	}

	/* Update tail pointer to trigger DMA */
	if (likely(nb_tx > 0)) {
		/* Ensure all BD writes complete before updating hardware */
		rte_wmb();
		rte_io_wmb();

		/* Update statistics */
		txq->tx_pkts += nb_tx;

		txq->tx_head = tx_head;

		/* Update hardware tail descriptor to start transmission */
		uint16_t tail_index = (tx_head == 0) ? (txq->nb_desc - 1) : (tx_head - 1);
		rte_iova_t tail_bd = txq->bd_phys_addr + sizeof(struct axidma_bd) * tail_index;

		/* Write tail descriptor - LSB first, then MSB */
		axidma_write_reg(txq->priv, AXIDMA_TX_TAILDESC_OFFSET, (uint32_t)tail_bd);
		axidma_write_reg(txq->priv, AXIDMA_TX_TAILDESC_MSB_OFFSET, (uint32_t)(tail_bd >> 32));
	}

	/* Track missed packets if burst was limited by queue space */
	if (unlikely(nb_tx < nb_pkts))
		txq->tx_errors += (nb_pkts - nb_tx);

	return nb_tx;
}

/* Link update function */
static int
xxv_eth_link_update(struct rte_eth_dev *dev, int wait_to_complete)
{
	struct xxv_eth_dev *priv = xxv_eth_dev_private(dev);
	struct rte_eth_link link;
	uint32_t status_reg;
	uint32_t speed_reg;

	RTE_SET_USED(wait_to_complete);

	/* Read link status from STAT_RX_STATUS_REG1 (0x404) bit 0 */
	status_reg = xxv_read_reg(priv, XXV_STATRX_STATUS_REG1);

	/* Read speed from STAT_CORE_SPEED_REG (0x498) */
	speed_reg = xxv_read_reg(priv, XXV_STAT_CORE_SPEED_REG);

	memset(&link, 0, sizeof(link));

	/* Determine speed based on register value:
	 * 00 - Standalone 25G
	 * 01 - Standalone 10G
	 * 10 - Runtime Switchable 25G
	 * 11 - Runtime Switchable 10G
	 */
	switch (speed_reg & 0x03) {
	case XXV_CORE_SPEED_25G_STANDALONE:
	case XXV_CORE_SPEED_25G_SWITCHABLE:
		link.link_speed = RTE_ETH_SPEED_NUM_25G;
		break;
	case XXV_CORE_SPEED_10G_STANDALONE:
	case XXV_CORE_SPEED_10G_SWITCHABLE:
		link.link_speed = RTE_ETH_SPEED_NUM_10G;
		break;
	default:
		link.link_speed = RTE_ETH_SPEED_NUM_25G; /* Default to 25G */
		break;
	}

	link.link_duplex = RTE_ETH_LINK_FULL_DUPLEX;
	link.link_autoneg = RTE_ETH_LINK_FIXED;

	/* Check bit 0 for link status */
	if (status_reg & XXV_RX_STATUS_MASK)
		link.link_status = RTE_ETH_LINK_UP;
	else
		link.link_status = RTE_ETH_LINK_DOWN;

	return rte_eth_linkstatus_set(dev, &link);
}

/* Promiscuous mode enable */
static int
xxv_eth_promiscuous_enable(struct rte_eth_dev *dev)
{
	RTE_SET_USED(dev);
	return 0;
}

/* Promiscuous mode disable */
static int
xxv_eth_promiscuous_disable(struct rte_eth_dev *dev)
{
	RTE_SET_USED(dev);
	return 0;
}

/* Get device statistics */
static int
xxv_eth_stats_get(struct rte_eth_dev *dev, struct rte_eth_stats *stats,
                   struct eth_queue_stats *q_stats)
{
	unsigned int i;
	struct axidma_rx_queue *rxq;
	struct axidma_tx_queue *txq;

	RTE_SET_USED(q_stats);

	/* Clear stats structure */
	memset(stats, 0, sizeof(*stats));

	/* Aggregate RX queue statistics */
	for (i = 0; i < dev->data->nb_rx_queues; i++) {
		rxq = dev->data->rx_queues[i];
		if (rxq == NULL)
			continue;

		stats->ipackets += rxq->rx_pkts;
		stats->ibytes += rxq->rx_bytes;
		stats->ierrors += rxq->rx_errors;
	}

	/* Aggregate TX queue statistics */
	for (i = 0; i < dev->data->nb_tx_queues; i++) {
		txq = dev->data->tx_queues[i];
		if (txq == NULL)
			continue;

		stats->opackets += txq->tx_pkts;
		stats->obytes += txq->tx_bytes;
		stats->oerrors += txq->tx_errors;
	}

	return 0;
}

/* Reset device statistics */
static int
xxv_eth_stats_reset(struct rte_eth_dev *dev)
{
	unsigned int i;
	struct axidma_rx_queue *rxq;
	struct axidma_tx_queue *txq;

	/* Reset RX queue statistics */
	for (i = 0; i < dev->data->nb_rx_queues; i++) {
		rxq = dev->data->rx_queues[i];
		if (rxq == NULL)
			continue;

		rxq->rx_pkts = 0;
		rxq->rx_bytes = 0;
		rxq->rx_errors = 0;
	}

	/* Reset TX queue statistics */
	for (i = 0; i < dev->data->nb_tx_queues; i++) {
		txq = dev->data->tx_queues[i];
		if (txq == NULL)
			continue;

		txq->tx_pkts = 0;
		txq->tx_bytes = 0;
		txq->tx_errors = 0;
	}

	return 0;
}

/* Forward declarations */
static int xxv_eth_dev_configure(struct rte_eth_dev *dev);
static int xxv_eth_dev_start(struct rte_eth_dev *dev);
static int xxv_eth_dev_stop(struct rte_eth_dev *dev);
static int xxv_eth_dev_close(struct rte_eth_dev *dev);
static int xxv_eth_dev_infos_get(struct rte_eth_dev *dev,
                                 struct rte_eth_dev_info *dev_info);
static int xxv_eth_rx_queue_setup(struct rte_eth_dev *dev, uint16_t queue_idx,
                                   uint16_t nb_desc, unsigned int socket_id,
                                   const struct rte_eth_rxconf *rx_conf,
                                   struct rte_mempool *mb_pool);
static int xxv_eth_tx_queue_setup(struct rte_eth_dev *dev, uint16_t queue_idx,
                                   uint16_t nb_desc, unsigned int socket_id,
                                   const struct rte_eth_txconf *tx_conf);
static void xxv_eth_rx_queue_release(struct rte_eth_dev *dev, uint16_t queue_id);
static void xxv_eth_tx_queue_release(struct rte_eth_dev *dev, uint16_t queue_id);

/* Device operations */
static const struct eth_dev_ops xxv_eth_ops = {
	.dev_configure        = xxv_eth_dev_configure,
	.dev_start            = xxv_eth_dev_start,
	.dev_stop             = xxv_eth_dev_stop,
	.dev_close            = xxv_eth_dev_close,
	.dev_infos_get        = xxv_eth_dev_infos_get,
	.rx_queue_setup       = xxv_eth_rx_queue_setup,
	.tx_queue_setup       = xxv_eth_tx_queue_setup,
	.rx_queue_release     = xxv_eth_rx_queue_release,
	.tx_queue_release     = xxv_eth_tx_queue_release,
	.link_update          = xxv_eth_link_update,
	.promiscuous_enable   = xxv_eth_promiscuous_enable,
	.promiscuous_disable  = xxv_eth_promiscuous_disable,
	.stats_get            = xxv_eth_stats_get,
	.stats_reset          = xxv_eth_stats_reset,
};

/* Configure device */
static int
xxv_eth_dev_configure(struct rte_eth_dev *dev)
{
	struct xxv_eth_dev *priv = xxv_eth_dev_private(dev);
	uint32_t val;
	int ret;

	/* Step 1: Reset the XXV MAC GT */
	val = xxv_read_reg(priv, XXV_REG_GT_RESET);
	val |= XXV_GT_RESET_MASK;
	xxv_write_reg(priv, XXV_REG_GT_RESET, val);

	/* Wait for 1 second for GT reset to complete as per spec */
	rte_delay_ms(1000);

	val = xxv_read_reg(priv, XXV_REG_GT_RESET);
	val &= ~XXV_GT_RESET_MASK;
	xxv_write_reg(priv, XXV_REG_GT_RESET, val);

	/* Step 2: AXI DMA reset (TX channel only) */
	ret = axidma_reset(priv, AXIDMA_TX_CR_OFFSET);
	if (ret) {
		XXV_LOG(ERR, "AXI DMA TX reset failed");
		return ret;
	}

	/* Step 3: Enable TX/RX and FCS */
	priv->options = 0;
	priv->options |= XAE_OPTION_FCS_STRIP;
	priv->options |= XAE_OPTION_FCS_INSERT;
	priv->options |= XAE_OPTION_TXEN;
	priv->options |= XAE_OPTION_RXEN;

	xxv_setoptions(priv, priv->options);

	/* Step 4: Check for block lock */
	ret = xxv_check_block_lock(priv);
	if (ret) {
		XXV_LOG(ERR, "Failed to acquire block lock");
		return ret;
	}

	return 0;
}

/* Start device */
static int
xxv_eth_dev_start(struct rte_eth_dev *dev)
{
	struct xxv_eth_dev *priv = xxv_eth_dev_private(dev);
	struct axidma_rx_queue *rxq;
	struct axidma_tx_queue *txq;
	uint32_t reg_val;

	/* Initialize TX queue - AXI DMA has single TX channel */
	txq = dev->data->tx_queues[0];
	if (txq != NULL) {
		/* Write current descriptor pointer - LSB first, then MSB */
		axidma_write_reg(priv, AXIDMA_TX_CURDESC_OFFSET,
		                     (uint32_t)txq->bd_phys_addr);
		axidma_write_reg(priv, AXIDMA_TX_CURDESC_MSB_OFFSET,
		                     (uint32_t)(txq->bd_phys_addr >> 32));

		/* Start TX DMA channel */
		reg_val = axidma_read_reg(priv, AXIDMA_TX_CR_OFFSET);
		reg_val |= AXIDMA_CR_RS | AXIDMA_CR_IOC_IRQEN | AXIDMA_CR_ERR_IRQEN;
		axidma_write_reg(priv, AXIDMA_TX_CR_OFFSET, reg_val);
	}

	/* Initialize RX queue - AXI DMA has single RX channel */
	rxq = dev->data->rx_queues[0];
	if (rxq != NULL) {
		/* Verify BD ring alignment before writing to hardware */
		if ((rxq->bd_phys_addr & 0x3F) != 0) {
			XXV_LOG(ERR, "RX BD ring not 64-byte aligned");
			return -EINVAL;
		}

		/* Write current descriptor pointer - LSB first, then MSB */
		axidma_write_reg(priv, AXIDMA_RX_CURDESC_OFFSET,
		                     (uint32_t)rxq->bd_phys_addr);
		axidma_write_reg(priv, AXIDMA_RX_CURDESC_MSB_OFFSET,
		                     (uint32_t)(rxq->bd_phys_addr >> 32));

		/* Start RX DMA channel */
		reg_val = axidma_read_reg(priv, AXIDMA_RX_CR_OFFSET);
		reg_val |= AXIDMA_CR_RS | AXIDMA_CR_IOC_IRQEN | AXIDMA_CR_ERR_IRQEN;
		axidma_write_reg(priv, AXIDMA_RX_CR_OFFSET, reg_val);

		/* Write tail descriptor to start RX - point to last BD */
		rte_iova_t tail_bd = rxq->bd_phys_addr +
		                     sizeof(struct axidma_bd) * (rxq->nb_desc - 1);


		/* Write tail descriptor - LSB first, then MSB */
		axidma_write_reg(priv, AXIDMA_RX_TAILDESC_OFFSET,
		                     (uint32_t)tail_bd);
		axidma_write_reg(priv, AXIDMA_RX_TAILDESC_MSB_OFFSET,
		                     (uint32_t)(tail_bd >> 32));
	}

	/* Set RX and TX burst functions */
	dev->rx_pkt_burst = xxv_eth_rx_burst;
	dev->tx_pkt_burst = xxv_eth_tx_burst;

	return 0;
}

/* Stop device */
static int
xxv_eth_dev_stop(struct rte_eth_dev *dev)
{
	struct xxv_eth_dev *priv = xxv_eth_dev_private(dev);

	XXV_LOG(NOTICE, "Stopping device %s", dev->data->name);

	/* Stop AXI DMA channels */
	XXV_LOG(NOTICE, "Stopping AXI DMA channels...");
	axidma_write_reg(priv, AXIDMA_TX_CR_OFFSET, 0);
	axidma_write_reg(priv, AXIDMA_RX_CR_OFFSET, 0);

	XXV_LOG(NOTICE, "Device stopped successfully");
	return 0;
}

/* Close device */
static int
xxv_eth_dev_close(struct rte_eth_dev *dev)
{
	uint16_t i;

	XXV_LOG(NOTICE, "Closing device %s", dev->data->name);

	if (rte_eal_process_type() != RTE_PROC_PRIMARY)
		return 0;

	/* Stop device first */
	xxv_eth_dev_stop(dev);

	/* Release all RX queues */
	for (i = 0; i < dev->data->nb_rx_queues; i++) {
		xxv_eth_rx_queue_release(dev, i);
	}

	/* Release all TX queues */
	for (i = 0; i < dev->data->nb_tx_queues; i++) {
		xxv_eth_tx_queue_release(dev, i);
	}

	XXV_LOG(NOTICE, "Device closed successfully");

	return 0;
}

/* Get device information */
static int
xxv_eth_dev_infos_get(struct rte_eth_dev *dev,
                     struct rte_eth_dev_info *dev_info)
{
	struct axidma_rx_queue *rxq;
	struct axidma_tx_queue *txq;

	dev_info->driver_name = "xxv_eth";
	dev_info->max_rx_queues = 1;
	dev_info->max_tx_queues = 1;
	dev_info->min_rx_bufsize = RTE_ETHER_MIN_LEN;
	dev_info->max_rx_pktlen = RTE_ETHER_MAX_LEN;
	dev_info->max_mac_addrs = 1;

	/* Set descriptor limits */
	dev_info->rx_desc_lim.nb_max = AXIDMA_MAX_BD_COUNT;
	dev_info->rx_desc_lim.nb_min = 32;
	dev_info->rx_desc_lim.nb_align = 1;
	dev_info->tx_desc_lim.nb_max = AXIDMA_MAX_BD_COUNT;
	dev_info->tx_desc_lim.nb_min = 32;
	dev_info->tx_desc_lim.nb_align = 1;

	/* Report actual queue descriptor counts if queues are setup */
	if (dev->data->rx_queues != NULL && dev->data->rx_queues[0] != NULL) {
		rxq = dev->data->rx_queues[0];
		dev_info->nb_rx_queues = 1;
		dev_info->default_rxportconf.ring_size = rxq->nb_desc;
	}

	if (dev->data->tx_queues != NULL && dev->data->tx_queues[0] != NULL) {
		txq = dev->data->tx_queues[0];
		dev_info->nb_tx_queues = 1;
		dev_info->default_txportconf.ring_size = txq->nb_desc;
	}

	XXV_LOG(DEBUG, "Device info retrieved for %s", dev->data->name);
	return 0;
}

/* Setup RX queue */
static int
xxv_eth_rx_queue_setup(struct rte_eth_dev *dev, uint16_t queue_idx,
                       uint16_t nb_desc, unsigned int socket_id,
                       const struct rte_eth_rxconf *rx_conf,
                       struct rte_mempool *mb_pool)
{
	struct axidma_rx_queue *rxq;
	const struct rte_memzone *mz;
	unsigned int i;

	RTE_SET_USED(rx_conf);

	if (queue_idx >= AXIDMA_MAX_RX_QUEUES) {
		XXV_LOG(ERR, "RX queue index %u exceeds maximum %u",
		        queue_idx, AXIDMA_MAX_RX_QUEUES);
		return -EINVAL;
	}

	/* Validate descriptor count is power of 2 */
	if (!rte_is_power_of_2(nb_desc)) {
		XXV_LOG(ERR, "RX descriptor count %u must be power of 2", nb_desc);
		return -EINVAL;
	}

	/* Allocate queue structure */
	rxq = rte_zmalloc_socket("axidma_rxq", sizeof(*rxq),
	                         RTE_CACHE_LINE_SIZE, socket_id);
	if (rxq == NULL) {
		XXV_LOG(ERR, "Failed to allocate RX queue");
		return -ENOMEM;
	}

	/* Allocate software ring for mbufs */
	rxq->sw_ring = rte_zmalloc_socket("axidma_rxq_sw_ring",
	                                  sizeof(struct rte_mbuf *) * nb_desc,
	                                  RTE_CACHE_LINE_SIZE, socket_id);
	if (rxq->sw_ring == NULL) {
		XXV_LOG(ERR, "Failed to allocate RX software ring");
		goto err_free_rxq;
	}

	/* Allocate buffer descriptor ring */
	mz = rte_eth_dma_zone_reserve(dev, "rx_bd_ring", queue_idx,
	                              sizeof(struct axidma_bd) * nb_desc,
	                              AXIDMA_BD_MINIMUM_ALIGNMENT,
	                              socket_id);
	if (mz == NULL) {
		XXV_LOG(ERR, "Failed to allocate RX BD ring");
		goto err_free_sw_ring;
	}

	rxq->bd_ring = (struct axidma_bd *)mz->addr;
	rxq->bd_phys_addr = mz->iova;

	/* Validate BD ring alignment */
	if (rxq->bd_phys_addr & (AXIDMA_BD_MINIMUM_ALIGNMENT - 1)) {
		XXV_LOG(ERR, "RX BD ring IOVA 0x%lx not aligned to 0x%x",
		        rxq->bd_phys_addr, AXIDMA_BD_MINIMUM_ALIGNMENT);
		goto err_free_bd_ring;
	}

	/* Initialize BD ring - link descriptors in circular fashion */
	memset(rxq->bd_ring, 0, sizeof(struct axidma_bd) * nb_desc);

	/* Pre-allocate mbufs and program BDs */
	for (i = 0; i < nb_desc; i++) {
		struct rte_mbuf *mbuf;
		rte_iova_t buf_iova;
		rte_iova_t next_bd_phys;

		mbuf = rte_pktmbuf_alloc(mb_pool);
		if (mbuf == NULL) {
			XXV_LOG(ERR, "Failed to allocate mbuf %u/%u", i, nb_desc);
			/* Free previously allocated mbufs */
			while (i-- > 0)
				rte_pktmbuf_free(rxq->sw_ring[i]);
			goto err_free_bd_ring;
		}
		rxq->sw_ring[i] = mbuf;

		/* Get mbuf physical address */
		buf_iova = rte_mbuf_data_iova_default(mbuf);

		/* Setup next descriptor pointer */
		next_bd_phys = rxq->bd_phys_addr +
		               sizeof(struct axidma_bd) * ((i + 1) % nb_desc);
		rxq->bd_ring[i].next_desc = (uint32_t)next_bd_phys;
		rxq->bd_ring[i].next_desc_msb = (uint32_t)(next_bd_phys >> 32);

		/* Setup buffer address */
		rxq->bd_ring[i].buf_addr = (uint32_t)buf_iova;
		rxq->bd_ring[i].buf_addr_msb = (uint32_t)(buf_iova >> 32);

		/* Setup control with max buffer length */
		rxq->bd_ring[i].control = rte_pktmbuf_tailroom(mbuf);
	}

	/* Flush BD ring to physical memory for DMA access */
	axidma_flush_bd_cache(rxq->bd_ring, sizeof(struct axidma_bd) * nb_desc);

	rxq->priv = xxv_eth_dev_private(dev);
	rxq->mb_pool = mb_pool;
	rxq->queue_id = queue_idx;
	rxq->nb_desc = nb_desc;
	rxq->rx_tail = 0;
	rxq->rx_pkts = 0;
	rxq->rx_bytes = 0;
	rxq->rx_errors = 0;

	dev->data->rx_queues[queue_idx] = rxq;

	return 0;

err_free_bd_ring:
	rte_eth_dma_zone_free(dev, "rx_bd_ring", queue_idx);
err_free_sw_ring:
	rte_free(rxq->sw_ring);
err_free_rxq:
	rte_free(rxq);
	return -ENOMEM;
}

/* Setup TX queue */
static int
xxv_eth_tx_queue_setup(struct rte_eth_dev *dev, uint16_t queue_idx,
                       uint16_t nb_desc, unsigned int socket_id,
                       const struct rte_eth_txconf *tx_conf)
{
	struct axidma_tx_queue *txq;
	const struct rte_memzone *mz;
	unsigned int i;

	RTE_SET_USED(tx_conf);

	if (queue_idx >= AXIDMA_MAX_TX_QUEUES) {
		XXV_LOG(ERR, "TX queue index %u exceeds maximum %u",
		        queue_idx, AXIDMA_MAX_TX_QUEUES);
		return -EINVAL;
	}

	/* Validate descriptor count is power of 2 */
	if (!rte_is_power_of_2(nb_desc)) {
		XXV_LOG(ERR, "TX descriptor count %u must be power of 2", nb_desc);
		return -EINVAL;
	}

	/* Allocate queue structure */
	txq = rte_zmalloc_socket("axidma_txq", sizeof(*txq),
	                         RTE_CACHE_LINE_SIZE, socket_id);
	if (txq == NULL) {
		XXV_LOG(ERR, "Failed to allocate TX queue");
		return -ENOMEM;
	}

	/* Allocate software ring for mbufs */
	txq->sw_ring = rte_zmalloc_socket("axidma_txq_sw_ring",
	                                  sizeof(struct rte_mbuf *) * nb_desc,
	                                  RTE_CACHE_LINE_SIZE, socket_id);
	if (txq->sw_ring == NULL) {
		XXV_LOG(ERR, "Failed to allocate TX software ring");
		goto err_free_txq;
	}

	/* Allocate buffer descriptor ring from hugepage memory */
	mz = rte_eth_dma_zone_reserve(dev, "tx_bd_ring", queue_idx,
	                              sizeof(struct axidma_bd) * nb_desc,
	                              AXIDMA_BD_MINIMUM_ALIGNMENT,
	                              socket_id);
	if (mz == NULL) {
		XXV_LOG(ERR, "Failed to allocate TX BD ring");
		goto err_free_sw_ring;
	}

	txq->bd_ring = (struct axidma_bd *)mz->addr;
	txq->bd_phys_addr = mz->iova;

	/* Validate BD ring alignment */
	if (txq->bd_phys_addr & (AXIDMA_BD_MINIMUM_ALIGNMENT - 1)) {
		XXV_LOG(ERR, "TX BD ring IOVA 0x%lx not aligned to 0x%x",
		        txq->bd_phys_addr, AXIDMA_BD_MINIMUM_ALIGNMENT);
		goto err_free_bd_ring;
	}

	/* Initialize BD ring - link descriptors in circular fashion */
	memset(txq->bd_ring, 0, sizeof(struct axidma_bd) * nb_desc);
	for (i = 0; i < nb_desc; i++) {
		rte_iova_t next_bd_phys = txq->bd_phys_addr +
		                          sizeof(struct axidma_bd) * ((i + 1) % nb_desc);
		txq->bd_ring[i].next_desc = (uint32_t)next_bd_phys;
		txq->bd_ring[i].next_desc_msb = (uint32_t)(next_bd_phys >> 32);
	}

	/* Flush BD ring to physical memory for DMA access */
	axidma_flush_bd_cache(txq->bd_ring, sizeof(struct axidma_bd) * nb_desc);

	txq->priv = xxv_eth_dev_private(dev);
	txq->queue_id = queue_idx;
	txq->nb_desc = nb_desc;
	txq->tx_tail = 0;
	txq->tx_head = 0;
	txq->tx_pkts = 0;
	txq->tx_bytes = 0;
	txq->tx_errors = 0;

	dev->data->tx_queues[queue_idx] = txq;

	XXV_LOG(NOTICE, "TX queue %u setup complete, BD ring at PA: 0x%lx",
	        queue_idx, txq->bd_phys_addr);
	return 0;

err_free_bd_ring:
	rte_eth_dma_zone_free(dev, "tx_bd_ring", queue_idx);
err_free_sw_ring:
	rte_free(txq->sw_ring);
err_free_txq:
	rte_free(txq);
	return -ENOMEM;
}

/* Release RX queue */
static void
xxv_eth_rx_queue_release(struct rte_eth_dev *dev, uint16_t queue_id)
{
	struct axidma_rx_queue *rxq = dev->data->rx_queues[queue_id];
	unsigned int i;

	if (rxq != NULL) {
		XXV_LOG(NOTICE, "Releasing RX queue %u", queue_id);

		/* Free mbufs in software ring */
		if (rxq->sw_ring != NULL) {
			for (i = 0; i < rxq->nb_desc; i++) {
				if (rxq->sw_ring[i] != NULL)
					rte_pktmbuf_free(rxq->sw_ring[i]);
			}
			rte_free(rxq->sw_ring);
		}

		/* Free BD ring memzone using rte_eth_dma_zone_free */
		rte_eth_dma_zone_free(dev, "rx_bd_ring", queue_id);

		rte_free(rxq);
		dev->data->rx_queues[queue_id] = NULL;
	}
}

/* Release TX queue */
static void
xxv_eth_tx_queue_release(struct rte_eth_dev *dev, uint16_t queue_id)
{
	struct axidma_tx_queue *txq = dev->data->tx_queues[queue_id];
	unsigned int i;

	if (txq != NULL) {
		XXV_LOG(NOTICE, "Releasing TX queue %u", queue_id);

		/* Free mbufs in software ring */
		if (txq->sw_ring != NULL) {
			for (i = 0; i < txq->nb_desc; i++) {
				if (txq->sw_ring[i] != NULL)
					rte_pktmbuf_free(txq->sw_ring[i]);
			}
			rte_free(txq->sw_ring);
		}

		/* Free BD ring memzone using rte_eth_dma_zone_free */
		rte_eth_dma_zone_free(dev, "tx_bd_ring", queue_id);

		rte_free(txq);
		dev->data->tx_queues[queue_id] = NULL;
	}
}


/* Probe function - called when device is found */
static int
xxv_eth_probe(struct rte_platform_device *pdev)
{
	struct rte_eth_dev *eth_dev;
	struct xxv_eth_dev *priv;
	char name[RTE_DEV_NAME_MAX_LEN];
	int ret;

	/* Check if we have required resources (XXV + DMA) */
	if (pdev->num_resource < 2) {
		XXV_LOG(ERR, "Expected 2 resources (XXV + DMA), found %u", pdev->num_resource);
		return -ENODEV;
	}

	/* Create unique eth device name */
	snprintf(name, sizeof(name), "net_%s", pdev->name);

	/* Allocate eth_dev */
	eth_dev = rte_eth_dev_allocate(name);
	if (eth_dev == NULL) {
		XXV_LOG(ERR, "Failed to allocate eth_dev");
		return -ENOMEM;
	}

	/* Allocate private data */
	priv = rte_zmalloc(name, sizeof(*priv), RTE_CACHE_LINE_SIZE);
	if (priv == NULL) {
		XXV_LOG(ERR, "Failed to allocate private data");
		ret = -ENOMEM;
		goto err_release_port;
	}

	/* Initialize private data with both resource addresses */
	priv->eth_dev = eth_dev;
	priv->pdev = pdev;
	priv->reg_base = pdev->resource[0].mem.addr;     /* XXV registers */
	priv->axidma_base = pdev->resource[1].mem.addr;  /* AXI DMA registers */
	priv->port_id = eth_dev->data->port_id;

	/* Set eth_dev data */
	eth_dev->data->dev_private = priv;
	eth_dev->data->numa_node = pdev->device.numa_node;
	eth_dev->device = &pdev->device;
	eth_dev->dev_ops = &xxv_eth_ops;

	/* Allocate MAC address array (1 address) */
	eth_dev->data->mac_addrs = rte_zmalloc(name,
	                                       sizeof(struct rte_ether_addr) * 1,
	                                       RTE_CACHE_LINE_SIZE);
	if (eth_dev->data->mac_addrs == NULL) {
		XXV_LOG(ERR, "Failed to allocate MAC address");
		ret = -ENOMEM;
		goto err_free_priv;
	}

	/* Generate random MAC address */
	rte_eth_random_addr(eth_dev->data->mac_addrs[0].addr_bytes);
	/* Set locally administered bit and clear multicast bit */
	eth_dev->data->mac_addrs[0].addr_bytes[0] &= 0xFE;
	eth_dev->data->mac_addrs[0].addr_bytes[0] |= 0x02;

	rte_ether_addr_copy(&eth_dev->data->mac_addrs[0],
	                   (struct rte_ether_addr *)priv->mac_addr);

	/* Mark device as successfully probed */
	rte_eth_dev_probing_finish(eth_dev);

	return 0;

err_free_priv:
	rte_free(priv);
err_release_port:
	rte_eth_dev_release_port(eth_dev);
	return ret;
}

/* Remove function - called when device is removed */
static int
xxv_eth_remove(struct rte_platform_device *pdev)
{
	struct rte_eth_dev *eth_dev;
	struct xxv_eth_dev *priv;
	char name[RTE_DEV_NAME_MAX_LEN];

	/* Find the eth_dev */
	snprintf(name, sizeof(name), "net_%s", pdev->name);
	eth_dev = rte_eth_dev_allocated(name);
	if (eth_dev == NULL)
		return 0;

	priv = xxv_eth_dev_private(eth_dev);

	/* Close device */
	if (eth_dev->data->dev_started)
		xxv_eth_dev_close(eth_dev);

	/* Free private data before releasing port */
	if (priv != NULL) {
		eth_dev->data->dev_private = NULL;
		rte_free(priv);
	}

	/* Release eth_dev - this also frees mac_addrs internally */
	rte_eth_dev_release_port(eth_dev);

	return 0;
}

/* Platform driver structure */
static struct rte_platform_driver xxv_eth_pmd = {
	.probe = xxv_eth_probe,
	.remove = xxv_eth_remove,
    .compatible = "xlnx,xxv-ethernet-1.0",
};

/* Register the driver with DPDK platform bus */
RTE_PMD_REGISTER_PLATFORM(xxv_eth, xxv_eth_pmd);

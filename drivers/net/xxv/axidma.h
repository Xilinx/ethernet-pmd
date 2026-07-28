/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2026 AMD-Xilinx.
 */

#ifndef _AXIDMA_H_
#define _AXIDMA_H_

#include <stdint.h>
#include <rte_io.h>
#include <rte_mbuf.h>

/* AXI DMA register offsets */
#define AXIDMA_TX_CR_OFFSET       0x00000000  /* TX Control */
#define AXIDMA_TX_SR_OFFSET       0x00000004  /* TX Status */
#define AXIDMA_TX_CURDESC_OFFSET  0x00000008  /* TX Current Descriptor (SG mode) */
#define AXIDMA_TX_CURDESC_MSB_OFFSET 0x0000000C /* TX Current Desc MSB (SG mode) */
#define AXIDMA_TX_TAILDESC_OFFSET 0x00000010  /* TX Tail Descriptor (SG mode) */
#define AXIDMA_TX_TAILDESC_MSB_OFFSET 0x00000014 /* TX Tail Desc MSB (SG mode) */

#define AXIDMA_RX_CR_OFFSET       0x00000030  /* RX Control */
#define AXIDMA_RX_SR_OFFSET       0x00000034  /* RX Status */
#define AXIDMA_RX_CURDESC_OFFSET  0x00000038  /* RX Current Descriptor (SG mode) */
#define AXIDMA_RX_CURDESC_MSB_OFFSET 0x0000003C /* RX Current Desc MSB (SG mode) */
#define AXIDMA_RX_TAILDESC_OFFSET 0x00000040  /* RX Tail Descriptor (SG mode) */
#define AXIDMA_RX_TAILDESC_MSB_OFFSET 0x00000044 /* RX Tail Desc MSB (SG mode) */

/* AXI DMA Control register bits */
#define AXIDMA_CR_RS           (1 << 0)    /* Run/Stop */
#define AXIDMA_CR_RESET        (1 << 2)    /* Reset */
#define AXIDMA_CR_IOC_IRQEN    (1 << 12)   /* Interrupt on complete */
#define AXIDMA_CR_DLY_IRQEN    (1 << 13)   /* Delay interrupt enable */
#define AXIDMA_CR_ERR_IRQEN    (1 << 14)   /* Error interrupt enable */

/* AXI DMA Status register bits */
#define AXIDMA_SR_HALTED       (1 << 0)    /* DMA halted */
#define AXIDMA_SR_IDLE         (1 << 1)    /* DMA idle */
#define AXIDMA_SR_SG_INCLD     (1 << 3)    /* Scatter Gather included */
#define AXIDMA_SR_DMA_INT_ERR  (1 << 4)    /* DMA internal error */
#define AXIDMA_SR_DMA_SLV_ERR  (1 << 5)    /* DMA slave error */
#define AXIDMA_SR_DMA_DEC_ERR  (1 << 6)    /* DMA decode error */
#define AXIDMA_SR_SG_INT_ERR   (1 << 8)    /* Scatter Gather internal error */
#define AXIDMA_SR_SG_SLV_ERR   (1 << 9)    /* Scatter Gather slave error */
#define AXIDMA_SR_SG_DEC_ERR   (1 << 10)   /* Scatter Gather decode error */
#define AXIDMA_SR_IOC_IRQ      (1 << 12)   /* Interrupt on complete */
#define AXIDMA_SR_DLY_IRQ      (1 << 13)   /* Delay interrupt */
#define AXIDMA_SR_ERR_IRQ      (1 << 14)   /* Error interrupt */

/* Error mask for status register */
#define AXIDMA_SR_ERR_ALL      (AXIDMA_SR_DMA_INT_ERR | AXIDMA_SR_DMA_SLV_ERR | \
                                AXIDMA_SR_DMA_DEC_ERR | AXIDMA_SR_SG_INT_ERR | \
                                AXIDMA_SR_SG_SLV_ERR | AXIDMA_SR_SG_DEC_ERR)

/* Buffer Descriptor (BD) offsets and bits */
#define AXIDMA_BD_NDESC_OFFSET    0x00  /* Next descriptor pointer */
#define AXIDMA_BD_NDESC_MSB_OFFSET 0x04 /* Next descriptor pointer MSB */
#define AXIDMA_BD_BUFA_OFFSET     0x08  /* Buffer address */
#define AXIDMA_BD_BUFA_MSB_OFFSET 0x0C  /* Buffer address MSB */
#define AXIDMA_BD_CTRL_LEN_OFFSET 0x18  /* Control/Length */
#define AXIDMA_BD_STS_OFFSET      0x1C  /* Status */

/* BD Control bits */
#define AXIDMA_BD_CTRL_TXSOF      (1 << 27) /* Start of frame */
#define AXIDMA_BD_CTRL_TXEOF      (1 << 26) /* End of frame */

/* BD Status bits */
#define AXIDMA_BD_STS_CMPLT       (1 << 31) /* Completed */
#define AXIDMA_BD_STS_DEC_ERR     (1 << 30) /* Decode error */
#define AXIDMA_BD_STS_SLV_ERR     (1 << 29) /* Slave error */
#define AXIDMA_BD_STS_INT_ERR     (1 << 28) /* Internal error */

#define AXIDMA_BD_MINIMUM_ALIGNMENT 0x40  /* 64 byte alignment */
#define AXIDMA_BD_SIZE            0x20    /* 32 bytes per BD */

/* Timeout constants */
#define DELAY_OF_ONE_MILLISEC  1000

/* Driver constants */
#define AXIDMA_MAX_RX_QUEUES   1
#define AXIDMA_MAX_TX_QUEUES   1
#define AXIDMA_MAX_BD_COUNT    4096  /* Maximum number of buffer descriptors */

/* Buffer Descriptor structure for Scatter-Gather mode */
struct axidma_bd {
	uint32_t next_desc;           /* Next descriptor pointer */
	uint32_t next_desc_msb;       /* Next descriptor MSB (for 64-bit) */
	uint32_t buf_addr;            /* Buffer address */
	uint32_t buf_addr_msb;        /* Buffer address MSB (for 64-bit) */
	uint32_t reserved1;
	uint32_t reserved2;
	uint32_t control;             /* Control/Length */
	uint32_t status;              /* Status */
} __rte_aligned(AXIDMA_BD_MINIMUM_ALIGNMENT);

/* Forward declaration */
struct xxv_eth_dev;

/* Queue structures */
struct axidma_rx_queue {
	struct xxv_eth_dev *priv;              /* Pointer to device private data */
	struct rte_mempool *mb_pool;           /* Mempool for RX buffers */
	struct rte_mbuf **sw_ring;             /* Software ring of mbufs */
	struct axidma_bd *bd_ring;             /* Hardware BD ring */
	rte_iova_t bd_phys_addr;               /* Physical address of BD ring */
	uint16_t queue_id;                     /* Queue ID */
	uint16_t nb_desc;                      /* Number of descriptors */
	uint16_t rx_tail;                      /* RX descriptor tail */
	
	/* Statistics */
	uint64_t rx_pkts;                      /* Total packets received */
	uint64_t rx_bytes;                     /* Total bytes received */
	uint64_t rx_errors;                    /* Total RX errors */
};

struct axidma_tx_queue {
	struct xxv_eth_dev *priv;              /* Pointer to device private data */
	struct rte_mbuf **sw_ring;             /* Software ring of mbufs */
	struct axidma_bd *bd_ring;             /* Hardware BD ring */
	rte_iova_t bd_phys_addr;               /* Physical address of BD ring */
	uint16_t queue_id;                     /* Queue ID */
	uint16_t nb_desc;                      /* Number of descriptors */
	uint16_t tx_tail;                      /* TX descriptor tail */
	uint16_t tx_head;                      /* TX descriptor head */
	
	/* Statistics */
	uint64_t tx_pkts;                      /* Total packets transmitted */
	uint64_t tx_bytes;                     /* Total bytes transmitted */
	uint64_t tx_errors;                    /* Total TX errors */
};

/* Helper functions */

/* AXI DMA register access helpers */
uint32_t axidma_read_reg(struct xxv_eth_dev *dev, uint32_t offset);
void axidma_write_reg(struct xxv_eth_dev *dev, uint32_t offset, uint32_t value);

/* Flush BD ring to ensure DMA coherency */
static inline void
axidma_flush_bd_cache(void *addr, size_t size)
{
	/* Write memory barrier to ensure all stores complete */
	rte_wmb();
	
	/* I/O write barrier for device access */
	rte_io_wmb();
	
	/* For ARM platforms with cache, this ensures visibility to DMA.
	 * The rte_wmb() and rte_io_wmb() should be sufficient, but if cache
	 * coherency issues persist, explicit cache flush may be needed via
	 * system-specific calls or marking memory as uncached.
	 */
	RTE_SET_USED(addr);
	RTE_SET_USED(size);
}

/* Function declarations */
int axidma_reset(struct xxv_eth_dev *priv, uint32_t offset);

#endif /* _AXIDMA_H_ */

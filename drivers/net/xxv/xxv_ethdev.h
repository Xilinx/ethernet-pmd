/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2026 AMD-Xilinx.
 */

#ifndef _XXV_ETHDEV_H_
#define _XXV_ETHDEV_H_

#include <stdint.h>
#include <rte_log.h>

/* Logging */
extern int xxv_logtype;

#define XXV_LOG(level, fmt, args...) \
	rte_log(RTE_LOG_ ## level, xxv_logtype, \
		"XXV_PMD: %s(): " fmt "\n", __func__, ##args)

/* XXV Ethernet register offsets */
#define XXV_REG_GT_RESET       0x00000000
#define XXV_REG_RESET          0x00000004
#define XXV_REG_MODE           0x00000008
#define XXV_REG_CONFIG         0x0000000C
#define XXV_TC_OFFSET          0x0000000C  /* Transmit Configuration */
#define XXV_REG_STATUS         0x00000014
#define XXV_RCW1_OFFSET        0x00000014  /* Receive Configuration Word 1 */
#define XXV_REG_TICK           0x00000018
#define XXV_STATRX_BLKLCK_OFFSET 0x0000040C  /* RX Block Lock Status */
#define XXV_STATRX_STATUS_REG1   0x00000404  /* RX Status Register 1 */
#define XXV_STAT_CORE_SPEED_REG  0x00000498  /* Core Speed Register */

/* GT Reset register bits */
#define XXV_GT_RESET_MASK      0x00000001

/* Reset register bits */
#define XXV_RESET_TX           0x00000001
#define XXV_RESET_RX           0x00000002

/* Transmit Configuration bits */
#define XXV_TC_FCS_MASK        0x00000001  /* FCS insertion enable */
#define XXV_TC_TX_MASK         0x00000002  /* Transmitter enable */

/* Receive Configuration Word 1 bits */
#define XXV_RCW1_FCS_MASK      0x00000001  /* FCS stripping enable */
#define XXV_RCW1_RX_MASK       0x00000002  /* Receiver enable */

/* RX Block Lock Status bits */
#define XXV_RX_BLKLCK_MASK     0x00000001  /* Block lock status */

/* RX Status Register 1 bits */
#define XXV_RX_STATUS_MASK     0x00000001  /* RX link status */

/* Core Speed Register values */
#define XXV_CORE_SPEED_25G_STANDALONE   0x00000000  /* 25G standalone mode */
#define XXV_CORE_SPEED_10G_STANDALONE   0x00000001  /* 10G standalone mode */
#define XXV_CORE_SPEED_25G_SWITCHABLE   0x00000002  /* Runtime switchable 25G */
#define XXV_CORE_SPEED_10G_SWITCHABLE   0x00000003  /* Runtime switchable 10G */

/* Configuration register bits */
#define XXV_CONFIG_TX_EN       0x00000001
#define XXV_CONFIG_RX_EN       0x00000002
#define XXV_CONFIG_RX_ENABLE   (1 << 0)
#define XXV_CONFIG_TX_ENABLE   (1 << 1)

/* Device options */
#define XAE_OPTION_FCS_STRIP   0x00000001
#define XAE_OPTION_FCS_INSERT  0x00000002
#define XAE_OPTION_TXEN        0x00000004
#define XAE_OPTION_RXEN        0x00000008

/* Status register bits */
#define XXV_STATUS_RX_ALIGN    0x00000001
#define XXV_STATUS_LINK_UP     0x00000002

/* Driver constants */
#define XXV_MAX_PKT_LEN        9600
#define XXV_MIN_PKT_LEN        64
#define XXV_DEFAULT_MTU        1500

/* pdev->name can itself be up to RTE_DEV_NAME_MAX_LEN bytes; add room
 * for the "net_" prefix so building the eth device name never truncates it.
 */
#define XXV_ETH_NAME_MAX_LEN   (RTE_DEV_NAME_MAX_LEN + 4)

/* Forward declarations */
struct rte_eth_dev;
struct rte_platform_device;
struct axidma_rx_queue;
struct axidma_tx_queue;

/* Private device structure */
struct xxv_eth_dev {
	struct rte_eth_dev *eth_dev;           /* Ethernet device */
	struct rte_platform_device *pdev;      /* Platform device */
	void *reg_base;                        /* XXV register base address */
	void *axidma_base;                     /* AXI DMA register base address */
	uint16_t port_id;                      /* DPDK port ID */
	uint32_t options;                      /* Device options */
	uint8_t mac_addr[6];                   /* MAC address */
};

#endif /* _XXV_ETHDEV_H_ */

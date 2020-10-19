/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2015-2020
 */

#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <rte_common.h>
#include <rte_ethdev_pci.h>

#include <rte_interrupts.h>
#include <rte_log.h>
#include <rte_debug.h>
#include <rte_pci.h>
#include <rte_memory.h>
#include <rte_eal.h>
#include <rte_alarm.h>

#include "txgbe_logs.h"
#include "base/txgbe.h"
#include "txgbe_ethdev.h"
#include "txgbe_rxtx.h"

static int  txgbe_dev_set_link_up(struct rte_eth_dev *dev);
static int  txgbe_dev_set_link_down(struct rte_eth_dev *dev);
static int txgbe_dev_close(struct rte_eth_dev *dev);
static int txgbe_dev_link_update(struct rte_eth_dev *dev,
				int wait_to_complete);
static int txgbe_dev_stats_reset(struct rte_eth_dev *dev);
static void txgbe_vlan_hw_strip_enable(struct rte_eth_dev *dev, uint16_t queue);
static void txgbe_vlan_hw_strip_disable(struct rte_eth_dev *dev,
					uint16_t queue);

static void txgbe_dev_link_status_print(struct rte_eth_dev *dev);
static int txgbe_dev_lsc_interrupt_setup(struct rte_eth_dev *dev, uint8_t on);
static int txgbe_dev_macsec_interrupt_setup(struct rte_eth_dev *dev);
static int txgbe_dev_rxq_interrupt_setup(struct rte_eth_dev *dev);
static int txgbe_dev_interrupt_get_status(struct rte_eth_dev *dev);
static int txgbe_dev_interrupt_action(struct rte_eth_dev *dev,
				      struct rte_intr_handle *handle);
static void txgbe_dev_interrupt_handler(void *param);
static void txgbe_dev_interrupt_delayed_handler(void *param);
static void txgbe_configure_msix(struct rte_eth_dev *dev);

#define TXGBE_SET_HWSTRIP(h, q) do {\
		uint32_t idx = (q) / (sizeof((h)->bitmap[0]) * NBBY); \
		uint32_t bit = (q) % (sizeof((h)->bitmap[0]) * NBBY); \
		(h)->bitmap[idx] |= 1 << bit;\
	} while (0)

#define TXGBE_CLEAR_HWSTRIP(h, q) do {\
		uint32_t idx = (q) / (sizeof((h)->bitmap[0]) * NBBY); \
		uint32_t bit = (q) % (sizeof((h)->bitmap[0]) * NBBY); \
		(h)->bitmap[idx] &= ~(1 << bit);\
	} while (0)

#define TXGBE_GET_HWSTRIP(h, q, r) do {\
		uint32_t idx = (q) / (sizeof((h)->bitmap[0]) * NBBY); \
		uint32_t bit = (q) % (sizeof((h)->bitmap[0]) * NBBY); \
		(r) = (h)->bitmap[idx] >> bit & 1;\
	} while (0)

/*
 * The set of PCI devices this driver supports
 */
static const struct rte_pci_id pci_id_txgbe_map[] = {
	{ RTE_PCI_DEVICE(PCI_VENDOR_ID_WANGXUN, TXGBE_DEV_ID_RAPTOR_SFP) },
	{ RTE_PCI_DEVICE(PCI_VENDOR_ID_WANGXUN, TXGBE_DEV_ID_WX1820_SFP) },
	{ .vendor_id = 0, /* sentinel */ },
};

static const struct rte_eth_desc_lim rx_desc_lim = {
	.nb_max = TXGBE_RING_DESC_MAX,
	.nb_min = TXGBE_RING_DESC_MIN,
	.nb_align = TXGBE_RXD_ALIGN,
};

static const struct rte_eth_desc_lim tx_desc_lim = {
	.nb_max = TXGBE_RING_DESC_MAX,
	.nb_min = TXGBE_RING_DESC_MIN,
	.nb_align = TXGBE_TXD_ALIGN,
	.nb_seg_max = TXGBE_TX_MAX_SEG,
	.nb_mtu_seg_max = TXGBE_TX_MAX_SEG,
};

static const struct eth_dev_ops txgbe_eth_dev_ops;

#define HW_XSTAT(m) {#m, offsetof(struct txgbe_hw_stats, m)}
#define HW_XSTAT_NAME(m, n) {n, offsetof(struct txgbe_hw_stats, m)}
static const struct rte_txgbe_xstats_name_off rte_txgbe_stats_strings[] = {
	/* MNG RxTx */
	HW_XSTAT(mng_bmc2host_packets),
	HW_XSTAT(mng_host2bmc_packets),
	/* Basic RxTx */
	HW_XSTAT(rx_packets),
	HW_XSTAT(tx_packets),
	HW_XSTAT(rx_bytes),
	HW_XSTAT(tx_bytes),
	HW_XSTAT(rx_total_bytes),
	HW_XSTAT(rx_total_packets),
	HW_XSTAT(tx_total_packets),
	HW_XSTAT(rx_total_missed_packets),
	HW_XSTAT(rx_broadcast_packets),
	HW_XSTAT(rx_multicast_packets),
	HW_XSTAT(rx_management_packets),
	HW_XSTAT(tx_management_packets),
	HW_XSTAT(rx_management_dropped),

	/* Basic Error */
	HW_XSTAT(rx_crc_errors),
	HW_XSTAT(rx_illegal_byte_errors),
	HW_XSTAT(rx_error_bytes),
	HW_XSTAT(rx_mac_short_packet_dropped),
	HW_XSTAT(rx_length_errors),
	HW_XSTAT(rx_undersize_errors),
	HW_XSTAT(rx_fragment_errors),
	HW_XSTAT(rx_oversize_errors),
	HW_XSTAT(rx_jabber_errors),
	HW_XSTAT(rx_l3_l4_xsum_error),
	HW_XSTAT(mac_local_errors),
	HW_XSTAT(mac_remote_errors),

	/* Flow Director */
	HW_XSTAT(flow_director_added_filters),
	HW_XSTAT(flow_director_removed_filters),
	HW_XSTAT(flow_director_filter_add_errors),
	HW_XSTAT(flow_director_filter_remove_errors),
	HW_XSTAT(flow_director_matched_filters),
	HW_XSTAT(flow_director_missed_filters),

	/* FCoE */
	HW_XSTAT(rx_fcoe_crc_errors),
	HW_XSTAT(rx_fcoe_mbuf_allocation_errors),
	HW_XSTAT(rx_fcoe_dropped),
	HW_XSTAT(rx_fcoe_packets),
	HW_XSTAT(tx_fcoe_packets),
	HW_XSTAT(rx_fcoe_bytes),
	HW_XSTAT(tx_fcoe_bytes),
	HW_XSTAT(rx_fcoe_no_ddp),
	HW_XSTAT(rx_fcoe_no_ddp_ext_buff),

	/* MACSEC */
	HW_XSTAT(tx_macsec_pkts_untagged),
	HW_XSTAT(tx_macsec_pkts_encrypted),
	HW_XSTAT(tx_macsec_pkts_protected),
	HW_XSTAT(tx_macsec_octets_encrypted),
	HW_XSTAT(tx_macsec_octets_protected),
	HW_XSTAT(rx_macsec_pkts_untagged),
	HW_XSTAT(rx_macsec_pkts_badtag),
	HW_XSTAT(rx_macsec_pkts_nosci),
	HW_XSTAT(rx_macsec_pkts_unknownsci),
	HW_XSTAT(rx_macsec_octets_decrypted),
	HW_XSTAT(rx_macsec_octets_validated),
	HW_XSTAT(rx_macsec_sc_pkts_unchecked),
	HW_XSTAT(rx_macsec_sc_pkts_delayed),
	HW_XSTAT(rx_macsec_sc_pkts_late),
	HW_XSTAT(rx_macsec_sa_pkts_ok),
	HW_XSTAT(rx_macsec_sa_pkts_invalid),
	HW_XSTAT(rx_macsec_sa_pkts_notvalid),
	HW_XSTAT(rx_macsec_sa_pkts_unusedsa),
	HW_XSTAT(rx_macsec_sa_pkts_notusingsa),

	/* MAC RxTx */
	HW_XSTAT(rx_size_64_packets),
	HW_XSTAT(rx_size_65_to_127_packets),
	HW_XSTAT(rx_size_128_to_255_packets),
	HW_XSTAT(rx_size_256_to_511_packets),
	HW_XSTAT(rx_size_512_to_1023_packets),
	HW_XSTAT(rx_size_1024_to_max_packets),
	HW_XSTAT(tx_size_64_packets),
	HW_XSTAT(tx_size_65_to_127_packets),
	HW_XSTAT(tx_size_128_to_255_packets),
	HW_XSTAT(tx_size_256_to_511_packets),
	HW_XSTAT(tx_size_512_to_1023_packets),
	HW_XSTAT(tx_size_1024_to_max_packets),

	/* Flow Control */
	HW_XSTAT(tx_xon_packets),
	HW_XSTAT(rx_xon_packets),
	HW_XSTAT(tx_xoff_packets),
	HW_XSTAT(rx_xoff_packets),

	HW_XSTAT_NAME(tx_xon_packets, "tx_flow_control_xon_packets"),
	HW_XSTAT_NAME(rx_xon_packets, "rx_flow_control_xon_packets"),
	HW_XSTAT_NAME(tx_xoff_packets, "tx_flow_control_xoff_packets"),
	HW_XSTAT_NAME(rx_xoff_packets, "rx_flow_control_xoff_packets"),
};

#define TXGBE_NB_HW_STATS (sizeof(rte_txgbe_stats_strings) / \
			   sizeof(rte_txgbe_stats_strings[0]))

/* Per-priority statistics */
#define UP_XSTAT(m) {#m, offsetof(struct txgbe_hw_stats, up[0].m)}
static const struct rte_txgbe_xstats_name_off rte_txgbe_up_strings[] = {
	UP_XSTAT(rx_up_packets),
	UP_XSTAT(tx_up_packets),
	UP_XSTAT(rx_up_bytes),
	UP_XSTAT(tx_up_bytes),
	UP_XSTAT(rx_up_drop_packets),

	UP_XSTAT(tx_up_xon_packets),
	UP_XSTAT(rx_up_xon_packets),
	UP_XSTAT(tx_up_xoff_packets),
	UP_XSTAT(rx_up_xoff_packets),
	UP_XSTAT(rx_up_dropped),
	UP_XSTAT(rx_up_mbuf_alloc_errors),
	UP_XSTAT(tx_up_xon2off_packets),
};

#define TXGBE_NB_UP_STATS (sizeof(rte_txgbe_up_strings) / \
			   sizeof(rte_txgbe_up_strings[0]))

/* Per-queue statistics */
#define QP_XSTAT(m) {#m, offsetof(struct txgbe_hw_stats, qp[0].m)}
static const struct rte_txgbe_xstats_name_off rte_txgbe_qp_strings[] = {
	QP_XSTAT(rx_qp_packets),
	QP_XSTAT(tx_qp_packets),
	QP_XSTAT(rx_qp_bytes),
	QP_XSTAT(tx_qp_bytes),
	QP_XSTAT(rx_qp_mc_packets),
};

#define TXGBE_NB_QP_STATS (sizeof(rte_txgbe_qp_strings) / \
			   sizeof(rte_txgbe_qp_strings[0]))

static inline int
txgbe_is_sfp(struct txgbe_hw *hw)
{
	switch (hw->phy.type) {
	case txgbe_phy_sfp_avago:
	case txgbe_phy_sfp_ftl:
	case txgbe_phy_sfp_intel:
	case txgbe_phy_sfp_unknown:
	case txgbe_phy_sfp_tyco_passive:
	case txgbe_phy_sfp_unknown_passive:
		return 1;
	default:
		return 0;
	}
}

static inline int32_t
txgbe_pf_reset_hw(struct txgbe_hw *hw)
{
	uint32_t ctrl_ext;
	int32_t status;

	status = hw->mac.reset_hw(hw);

	ctrl_ext = rd32(hw, TXGBE_PORTCTL);
	/* Set PF Reset Done bit so PF/VF Mail Ops can work */
	ctrl_ext |= TXGBE_PORTCTL_RSTDONE;
	wr32(hw, TXGBE_PORTCTL, ctrl_ext);
	txgbe_flush(hw);

	if (status == TXGBE_ERR_SFP_NOT_PRESENT)
		status = 0;
	return status;
}

static inline void
txgbe_enable_intr(struct rte_eth_dev *dev)
{
	struct txgbe_interrupt *intr = TXGBE_DEV_INTR(dev);
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);

	wr32(hw, TXGBE_IENMISC, intr->mask_misc);
	wr32(hw, TXGBE_IMC(0), TXGBE_IMC_MASK);
	wr32(hw, TXGBE_IMC(1), TXGBE_IMC_MASK);
	txgbe_flush(hw);
}

static void
txgbe_disable_intr(struct txgbe_hw *hw)
{
	PMD_INIT_FUNC_TRACE();

	wr32(hw, TXGBE_IENMISC, ~BIT_MASK32);
	wr32(hw, TXGBE_IMS(0), TXGBE_IMC_MASK);
	wr32(hw, TXGBE_IMS(1), TXGBE_IMC_MASK);
	txgbe_flush(hw);
}

static int
txgbe_dev_queue_stats_mapping_set(struct rte_eth_dev *eth_dev,
				  uint16_t queue_id,
				  uint8_t stat_idx,
				  uint8_t is_rx)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(eth_dev);
	struct txgbe_stat_mappings *stat_mappings =
		TXGBE_DEV_STAT_MAPPINGS(eth_dev);
	uint32_t qsmr_mask = 0;
	uint32_t clearing_mask = QMAP_FIELD_RESERVED_BITS_MASK;
	uint32_t q_map;
	uint8_t n, offset;

	if (hw->mac.type != txgbe_mac_raptor)
		return -ENOSYS;

	if (stat_idx & !QMAP_FIELD_RESERVED_BITS_MASK)
		return -EIO;

	PMD_INIT_LOG(DEBUG, "Setting port %d, %s queue_id %d to stat index %d",
		     (int)(eth_dev->data->port_id), is_rx ? "RX" : "TX",
		     queue_id, stat_idx);

	n = (uint8_t)(queue_id / NB_QMAP_FIELDS_PER_QSM_REG);
	if (n >= TXGBE_NB_STAT_MAPPING) {
		PMD_INIT_LOG(ERR, "Nb of stat mapping registers exceeded");
		return -EIO;
	}
	offset = (uint8_t)(queue_id % NB_QMAP_FIELDS_PER_QSM_REG);

	/* Now clear any previous stat_idx set */
	clearing_mask <<= (QSM_REG_NB_BITS_PER_QMAP_FIELD * offset);
	if (!is_rx)
		stat_mappings->tqsm[n] &= ~clearing_mask;
	else
		stat_mappings->rqsm[n] &= ~clearing_mask;

	q_map = (uint32_t)stat_idx;
	q_map &= QMAP_FIELD_RESERVED_BITS_MASK;
	qsmr_mask = q_map << (QSM_REG_NB_BITS_PER_QMAP_FIELD * offset);
	if (!is_rx)
		stat_mappings->tqsm[n] |= qsmr_mask;
	else
		stat_mappings->rqsm[n] |= qsmr_mask;

	PMD_INIT_LOG(DEBUG, "Set port %d, %s queue_id %d to stat index %d",
		     (int)(eth_dev->data->port_id), is_rx ? "RX" : "TX",
		     queue_id, stat_idx);
	PMD_INIT_LOG(DEBUG, "%s[%d] = 0x%08x", is_rx ? "RQSMR" : "TQSM", n,
		     is_rx ? stat_mappings->rqsm[n] : stat_mappings->tqsm[n]);
	return 0;
}

static void
txgbe_dcb_init(struct txgbe_hw *hw, struct txgbe_dcb_config *dcb_config)
{
	int i;
	u8 bwgp;
	struct txgbe_dcb_tc_config *tc;

	UNREFERENCED_PARAMETER(hw);

	dcb_config->num_tcs.pg_tcs = TXGBE_DCB_TC_MAX;
	dcb_config->num_tcs.pfc_tcs = TXGBE_DCB_TC_MAX;
	bwgp = (u8)(100 / TXGBE_DCB_TC_MAX);
	for (i = 0; i < TXGBE_DCB_TC_MAX; i++) {
		tc = &dcb_config->tc_config[i];
		tc->path[TXGBE_DCB_TX_CONFIG].bwg_id = i;
		tc->path[TXGBE_DCB_TX_CONFIG].bwg_percent = bwgp + (i & 1);
		tc->path[TXGBE_DCB_RX_CONFIG].bwg_id = i;
		tc->path[TXGBE_DCB_RX_CONFIG].bwg_percent = bwgp + (i & 1);
		tc->pfc = txgbe_dcb_pfc_disabled;
	}

	/* Initialize default user to priority mapping, UPx->TC0 */
	tc = &dcb_config->tc_config[0];
	tc->path[TXGBE_DCB_TX_CONFIG].up_to_tc_bitmap = 0xFF;
	tc->path[TXGBE_DCB_RX_CONFIG].up_to_tc_bitmap = 0xFF;
	for (i = 0; i < TXGBE_DCB_BWG_MAX; i++) {
		dcb_config->bw_percentage[i][TXGBE_DCB_TX_CONFIG] = 100;
		dcb_config->bw_percentage[i][TXGBE_DCB_RX_CONFIG] = 100;
	}
	dcb_config->rx_pba_cfg = txgbe_dcb_pba_equal;
	dcb_config->pfc_mode_enable = false;
	dcb_config->vt_mode = true;
	dcb_config->round_robin_enable = false;
	/* support all DCB capabilities */
	dcb_config->support.capabilities = 0xFF;
}

/*
 * Ensure that all locks are released before first NVM or PHY access
 */
static void
txgbe_swfw_lock_reset(struct txgbe_hw *hw)
{
	uint16_t mask;

	/*
	 * These ones are more tricky since they are common to all ports; but
	 * swfw_sync retries last long enough (1s) to be almost sure that if
	 * lock can not be taken it is due to an improper lock of the
	 * semaphore.
	 */
	mask = TXGBE_MNGSEM_SWPHY |
	       TXGBE_MNGSEM_SWMBX |
	       TXGBE_MNGSEM_SWFLASH;
	if (hw->mac.acquire_swfw_sync(hw, mask) < 0)
		PMD_DRV_LOG(DEBUG, "SWFW common locks released");

	hw->mac.release_swfw_sync(hw, mask);
}

static int
eth_txgbe_dev_init(struct rte_eth_dev *eth_dev, void *init_params __rte_unused)
{
	struct rte_pci_device *pci_dev = RTE_ETH_DEV_TO_PCI(eth_dev);
	struct txgbe_hw *hw = TXGBE_DEV_HW(eth_dev);
	struct txgbe_vfta *shadow_vfta = TXGBE_DEV_VFTA(eth_dev);
	struct txgbe_hwstrip *hwstrip = TXGBE_DEV_HWSTRIP(eth_dev);
	struct txgbe_dcb_config *dcb_config = TXGBE_DEV_DCB_CONFIG(eth_dev);
	struct txgbe_bw_conf *bw_conf = TXGBE_DEV_BW_CONF(eth_dev);
	struct rte_intr_handle *intr_handle = &pci_dev->intr_handle;
	const struct rte_memzone *mz;
	uint32_t ctrl_ext;
	uint16_t csum;
	int err, i;

	PMD_INIT_FUNC_TRACE();

	eth_dev->dev_ops = &txgbe_eth_dev_ops;
	eth_dev->rx_pkt_burst = &txgbe_recv_pkts;
	eth_dev->tx_pkt_burst = &txgbe_xmit_pkts;
	eth_dev->tx_pkt_prepare = &txgbe_prep_pkts;

	/*
	 * For secondary processes, we don't initialise any further as primary
	 * has already done this work. Only check we don't need a different
	 * RX and TX function.
	 */
	if (rte_eal_process_type() != RTE_PROC_PRIMARY) {
		struct txgbe_tx_queue *txq;
		/* TX queue function in primary, set by last queue initialized
		 * Tx queue may not initialized by primary process
		 */
		if (eth_dev->data->tx_queues) {
			uint16_t nb_tx_queues = eth_dev->data->nb_tx_queues;
			txq = eth_dev->data->tx_queues[nb_tx_queues - 1];
			txgbe_set_tx_function(eth_dev, txq);
		} else {
			/* Use default TX function if we get here */
			PMD_INIT_LOG(NOTICE, "No TX queues configured yet. "
				     "Using default TX function.");
		}

		txgbe_set_rx_function(eth_dev);

		return 0;
	}

	rte_eth_copy_pci_info(eth_dev, pci_dev);

	/* Vendor and Device ID need to be set before init of shared code */
	hw->device_id = pci_dev->id.device_id;
	hw->vendor_id = pci_dev->id.vendor_id;
	hw->hw_addr = (void *)pci_dev->mem_resource[0].addr;
	hw->allow_unsupported_sfp = 1;

	/* Reserve memory for interrupt status block */
	mz = rte_eth_dma_zone_reserve(eth_dev, "txgbe_driver", -1,
		16, TXGBE_ALIGN, SOCKET_ID_ANY);
	if (mz == NULL)
		return -ENOMEM;

	hw->isb_dma = TMZ_PADDR(mz);
	hw->isb_mem = TMZ_VADDR(mz);

	/* Initialize the shared code (base driver) */
	err = txgbe_init_shared_code(hw);
	if (err != 0) {
		PMD_INIT_LOG(ERR, "Shared code init failed: %d", err);
		return -EIO;
	}

	/* Unlock any pending hardware semaphore */
	txgbe_swfw_lock_reset(hw);

	/* Initialize DCB configuration*/
	memset(dcb_config, 0, sizeof(struct txgbe_dcb_config));
	txgbe_dcb_init(hw, dcb_config);

	/* Get Hardware Flow Control setting */
	hw->fc.requested_mode = txgbe_fc_full;
	hw->fc.current_mode = txgbe_fc_full;
	hw->fc.pause_time = TXGBE_FC_PAUSE_TIME;
	for (i = 0; i < TXGBE_DCB_TC_MAX; i++) {
		hw->fc.low_water[i] = TXGBE_FC_XON_LOTH;
		hw->fc.high_water[i] = TXGBE_FC_XOFF_HITH;
	}
	hw->fc.send_xon = 1;

	err = hw->rom.init_params(hw);
	if (err != 0) {
		PMD_INIT_LOG(ERR, "The EEPROM init failed: %d", err);
		return -EIO;
	}

	/* Make sure we have a good EEPROM before we read from it */
	err = hw->rom.validate_checksum(hw, &csum);
	if (err != 0) {
		PMD_INIT_LOG(ERR, "The EEPROM checksum is not valid: %d", err);
		return -EIO;
	}

	err = hw->mac.init_hw(hw);

	/*
	 * Devices with copper phys will fail to initialise if txgbe_init_hw()
	 * is called too soon after the kernel driver unbinding/binding occurs.
	 * The failure occurs in txgbe_identify_phy() for all devices,
	 * but for non-copper devies, txgbe_identify_sfp_module() is
	 * also called. See txgbe_identify_phy(). The reason for the
	 * failure is not known, and only occuts when virtualisation features
	 * are disabled in the bios. A delay of 200ms  was found to be enough by
	 * trial-and-error, and is doubled to be safe.
	 */
	if (err && hw->phy.media_type == txgbe_media_type_copper) {
		rte_delay_ms(200);
		err = hw->mac.init_hw(hw);
	}

	if (err == TXGBE_ERR_SFP_NOT_PRESENT)
		err = 0;

	if (err == TXGBE_ERR_EEPROM_VERSION) {
		PMD_INIT_LOG(ERR, "This device is a pre-production adapter/"
			     "LOM.  Please be aware there may be issues associated "
			     "with your hardware.");
		PMD_INIT_LOG(ERR, "If you are experiencing problems "
			     "please contact your hardware representative "
			     "who provided you with this hardware.");
	} else if (err == TXGBE_ERR_SFP_NOT_SUPPORTED) {
		PMD_INIT_LOG(ERR, "Unsupported SFP+ Module");
	}
	if (err) {
		PMD_INIT_LOG(ERR, "Hardware Initialization Failure: %d", err);
		return -EIO;
	}

	/* Reset the hw statistics */
	txgbe_dev_stats_reset(eth_dev);

	/* disable interrupt */
	txgbe_disable_intr(hw);

	/* Allocate memory for storing MAC addresses */
	eth_dev->data->mac_addrs = rte_zmalloc("txgbe", RTE_ETHER_ADDR_LEN *
					       hw->mac.num_rar_entries, 0);
	if (eth_dev->data->mac_addrs == NULL) {
		PMD_INIT_LOG(ERR,
			     "Failed to allocate %u bytes needed to store "
			     "MAC addresses",
			     RTE_ETHER_ADDR_LEN * hw->mac.num_rar_entries);
		return -ENOMEM;
	}

	/* Copy the permanent MAC address */
	rte_ether_addr_copy((struct rte_ether_addr *)hw->mac.perm_addr,
			&eth_dev->data->mac_addrs[0]);

	/* Allocate memory for storing hash filter MAC addresses */
	eth_dev->data->hash_mac_addrs = rte_zmalloc("txgbe",
			RTE_ETHER_ADDR_LEN * TXGBE_VMDQ_NUM_UC_MAC, 0);
	if (eth_dev->data->hash_mac_addrs == NULL) {
		PMD_INIT_LOG(ERR,
			     "Failed to allocate %d bytes needed to store MAC addresses",
			     RTE_ETHER_ADDR_LEN * TXGBE_VMDQ_NUM_UC_MAC);
		return -ENOMEM;
	}

	/* initialize the vfta */
	memset(shadow_vfta, 0, sizeof(*shadow_vfta));

	/* initialize the hw strip bitmap*/
	memset(hwstrip, 0, sizeof(*hwstrip));

	/* initialize PF if max_vfs not zero */
	txgbe_pf_host_init(eth_dev);

	ctrl_ext = rd32(hw, TXGBE_PORTCTL);
	/* let hardware know driver is loaded */
	ctrl_ext |= TXGBE_PORTCTL_DRVLOAD;
	/* Set PF Reset Done bit so PF/VF Mail Ops can work */
	ctrl_ext |= TXGBE_PORTCTL_RSTDONE;
	wr32(hw, TXGBE_PORTCTL, ctrl_ext);
	txgbe_flush(hw);

	if (txgbe_is_sfp(hw) && hw->phy.sfp_type != txgbe_sfp_type_not_present)
		PMD_INIT_LOG(DEBUG, "MAC: %d, PHY: %d, SFP+: %d",
			     (int)hw->mac.type, (int)hw->phy.type,
			     (int)hw->phy.sfp_type);
	else
		PMD_INIT_LOG(DEBUG, "MAC: %d, PHY: %d",
			     (int)hw->mac.type, (int)hw->phy.type);

	PMD_INIT_LOG(DEBUG, "port %d vendorID=0x%x deviceID=0x%x",
		     eth_dev->data->port_id, pci_dev->id.vendor_id,
		     pci_dev->id.device_id);

	rte_intr_callback_register(intr_handle,
				   txgbe_dev_interrupt_handler, eth_dev);

	/* enable uio/vfio intr/eventfd mapping */
	rte_intr_enable(intr_handle);

	/* enable support intr */
	txgbe_enable_intr(eth_dev);

	/* initialize bandwidth configuration info */
	memset(bw_conf, 0, sizeof(struct txgbe_bw_conf));

	return 0;
}

static int
eth_txgbe_dev_uninit(struct rte_eth_dev *eth_dev)
{
	PMD_INIT_FUNC_TRACE();

	if (rte_eal_process_type() != RTE_PROC_PRIMARY)
		return 0;

	txgbe_dev_close(eth_dev);

	return 0;
}

static int
eth_txgbe_pci_probe(struct rte_pci_driver *pci_drv __rte_unused,
		struct rte_pci_device *pci_dev)
{
	struct rte_eth_dev *pf_ethdev;
	struct rte_eth_devargs eth_da;
	int retval;

	if (pci_dev->device.devargs) {
		retval = rte_eth_devargs_parse(pci_dev->device.devargs->args,
				&eth_da);
		if (retval)
			return retval;
	} else {
		memset(&eth_da, 0, sizeof(eth_da));
	}

	retval = rte_eth_dev_create(&pci_dev->device, pci_dev->device.name,
			sizeof(struct txgbe_adapter),
			eth_dev_pci_specific_init, pci_dev,
			eth_txgbe_dev_init, NULL);

	if (retval || eth_da.nb_representor_ports < 1)
		return retval;

	pf_ethdev = rte_eth_dev_allocated(pci_dev->device.name);
	if (pf_ethdev == NULL)
		return -ENODEV;

	return 0;
}

static int eth_txgbe_pci_remove(struct rte_pci_device *pci_dev)
{
	struct rte_eth_dev *ethdev;

	ethdev = rte_eth_dev_allocated(pci_dev->device.name);
	if (!ethdev)
		return -ENODEV;

	return rte_eth_dev_destroy(ethdev, eth_txgbe_dev_uninit);
}

static struct rte_pci_driver rte_txgbe_pmd = {
	.id_table = pci_id_txgbe_map,
	.drv_flags = RTE_PCI_DRV_NEED_MAPPING |
		     RTE_PCI_DRV_INTR_LSC,
	.probe = eth_txgbe_pci_probe,
	.remove = eth_txgbe_pci_remove,
};

static int
txgbe_vlan_filter_set(struct rte_eth_dev *dev, uint16_t vlan_id, int on)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	struct txgbe_vfta *shadow_vfta = TXGBE_DEV_VFTA(dev);
	uint32_t vfta;
	uint32_t vid_idx;
	uint32_t vid_bit;

	vid_idx = (uint32_t)((vlan_id >> 5) & 0x7F);
	vid_bit = (uint32_t)(1 << (vlan_id & 0x1F));
	vfta = rd32(hw, TXGBE_VLANTBL(vid_idx));
	if (on)
		vfta |= vid_bit;
	else
		vfta &= ~vid_bit;
	wr32(hw, TXGBE_VLANTBL(vid_idx), vfta);

	/* update local VFTA copy */
	shadow_vfta->vfta[vid_idx] = vfta;

	return 0;
}

static void
txgbe_vlan_strip_queue_set(struct rte_eth_dev *dev, uint16_t queue, int on)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	struct txgbe_rx_queue *rxq;
	bool restart;
	uint32_t rxcfg, rxbal, rxbah;

	if (on)
		txgbe_vlan_hw_strip_enable(dev, queue);
	else
		txgbe_vlan_hw_strip_disable(dev, queue);

	rxq = dev->data->rx_queues[queue];
	rxbal = rd32(hw, TXGBE_RXBAL(rxq->reg_idx));
	rxbah = rd32(hw, TXGBE_RXBAH(rxq->reg_idx));
	rxcfg = rd32(hw, TXGBE_RXCFG(rxq->reg_idx));
	if (rxq->offloads & DEV_RX_OFFLOAD_VLAN_STRIP) {
		restart = (rxcfg & TXGBE_RXCFG_ENA) &&
			!(rxcfg & TXGBE_RXCFG_VLAN);
		rxcfg |= TXGBE_RXCFG_VLAN;
	} else {
		restart = (rxcfg & TXGBE_RXCFG_ENA) &&
			(rxcfg & TXGBE_RXCFG_VLAN);
		rxcfg &= ~TXGBE_RXCFG_VLAN;
	}
	rxcfg &= ~TXGBE_RXCFG_ENA;

	if (restart) {
		/* set vlan strip for ring */
		txgbe_dev_rx_queue_stop(dev, queue);
		wr32(hw, TXGBE_RXBAL(rxq->reg_idx), rxbal);
		wr32(hw, TXGBE_RXBAH(rxq->reg_idx), rxbah);
		wr32(hw, TXGBE_RXCFG(rxq->reg_idx), rxcfg);
		txgbe_dev_rx_queue_start(dev, queue);
	}
}

static int
txgbe_vlan_tpid_set(struct rte_eth_dev *dev,
		    enum rte_vlan_type vlan_type,
		    uint16_t tpid)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	int ret = 0;
	uint32_t portctrl, vlan_ext, qinq;

	portctrl = rd32(hw, TXGBE_PORTCTL);

	vlan_ext = (portctrl & TXGBE_PORTCTL_VLANEXT);
	qinq = vlan_ext && (portctrl & TXGBE_PORTCTL_QINQ);
	switch (vlan_type) {
	case ETH_VLAN_TYPE_INNER:
		if (vlan_ext) {
			wr32m(hw, TXGBE_VLANCTL,
				TXGBE_VLANCTL_TPID_MASK,
				TXGBE_VLANCTL_TPID(tpid));
			wr32m(hw, TXGBE_DMATXCTRL,
				TXGBE_DMATXCTRL_TPID_MASK,
				TXGBE_DMATXCTRL_TPID(tpid));
		} else {
			ret = -ENOTSUP;
			PMD_DRV_LOG(ERR, "Inner type is not supported"
				    " by single VLAN");
		}

		if (qinq) {
			wr32m(hw, TXGBE_TAGTPID(0),
				TXGBE_TAGTPID_LSB_MASK,
				TXGBE_TAGTPID_LSB(tpid));
		}
		break;
	case ETH_VLAN_TYPE_OUTER:
		if (vlan_ext) {
			/* Only the high 16-bits is valid */
			wr32m(hw, TXGBE_EXTAG,
				TXGBE_EXTAG_VLAN_MASK,
				TXGBE_EXTAG_VLAN(tpid));
		} else {
			wr32m(hw, TXGBE_VLANCTL,
				TXGBE_VLANCTL_TPID_MASK,
				TXGBE_VLANCTL_TPID(tpid));
			wr32m(hw, TXGBE_DMATXCTRL,
				TXGBE_DMATXCTRL_TPID_MASK,
				TXGBE_DMATXCTRL_TPID(tpid));
		}

		if (qinq) {
			wr32m(hw, TXGBE_TAGTPID(0),
				TXGBE_TAGTPID_MSB_MASK,
				TXGBE_TAGTPID_MSB(tpid));
		}
		break;
	default:
		PMD_DRV_LOG(ERR, "Unsupported VLAN type %d", vlan_type);
		return -EINVAL;
	}

	return ret;
}

void
txgbe_vlan_hw_filter_disable(struct rte_eth_dev *dev)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	uint32_t vlnctrl;

	PMD_INIT_FUNC_TRACE();

	/* Filter Table Disable */
	vlnctrl = rd32(hw, TXGBE_VLANCTL);
	vlnctrl &= ~TXGBE_VLANCTL_VFE;
	wr32(hw, TXGBE_VLANCTL, vlnctrl);
}

void
txgbe_vlan_hw_filter_enable(struct rte_eth_dev *dev)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	struct txgbe_vfta *shadow_vfta = TXGBE_DEV_VFTA(dev);
	uint32_t vlnctrl;
	uint16_t i;

	PMD_INIT_FUNC_TRACE();

	/* Filter Table Enable */
	vlnctrl = rd32(hw, TXGBE_VLANCTL);
	vlnctrl &= ~TXGBE_VLANCTL_CFIENA;
	vlnctrl |= TXGBE_VLANCTL_VFE;
	wr32(hw, TXGBE_VLANCTL, vlnctrl);

	/* write whatever is in local vfta copy */
	for (i = 0; i < TXGBE_VFTA_SIZE; i++)
		wr32(hw, TXGBE_VLANTBL(i), shadow_vfta->vfta[i]);
}

void
txgbe_vlan_hw_strip_bitmap_set(struct rte_eth_dev *dev, uint16_t queue, bool on)
{
	struct txgbe_hwstrip *hwstrip = TXGBE_DEV_HWSTRIP(dev);
	struct txgbe_rx_queue *rxq;

	if (queue >= TXGBE_MAX_RX_QUEUE_NUM)
		return;

	if (on)
		TXGBE_SET_HWSTRIP(hwstrip, queue);
	else
		TXGBE_CLEAR_HWSTRIP(hwstrip, queue);

	if (queue >= dev->data->nb_rx_queues)
		return;

	rxq = dev->data->rx_queues[queue];

	if (on) {
		rxq->vlan_flags = PKT_RX_VLAN | PKT_RX_VLAN_STRIPPED;
		rxq->offloads |= DEV_RX_OFFLOAD_VLAN_STRIP;
	} else {
		rxq->vlan_flags = PKT_RX_VLAN;
		rxq->offloads &= ~DEV_RX_OFFLOAD_VLAN_STRIP;
	}
}

static void
txgbe_vlan_hw_strip_disable(struct rte_eth_dev *dev, uint16_t queue)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	uint32_t ctrl;

	PMD_INIT_FUNC_TRACE();

	ctrl = rd32(hw, TXGBE_RXCFG(queue));
	ctrl &= ~TXGBE_RXCFG_VLAN;
	wr32(hw, TXGBE_RXCFG(queue), ctrl);

	/* record those setting for HW strip per queue */
	txgbe_vlan_hw_strip_bitmap_set(dev, queue, 0);
}

static void
txgbe_vlan_hw_strip_enable(struct rte_eth_dev *dev, uint16_t queue)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	uint32_t ctrl;

	PMD_INIT_FUNC_TRACE();

	ctrl = rd32(hw, TXGBE_RXCFG(queue));
	ctrl |= TXGBE_RXCFG_VLAN;
	wr32(hw, TXGBE_RXCFG(queue), ctrl);

	/* record those setting for HW strip per queue */
	txgbe_vlan_hw_strip_bitmap_set(dev, queue, 1);
}

static void
txgbe_vlan_hw_extend_disable(struct rte_eth_dev *dev)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	uint32_t ctrl;

	PMD_INIT_FUNC_TRACE();

	ctrl = rd32(hw, TXGBE_PORTCTL);
	ctrl &= ~TXGBE_PORTCTL_VLANEXT;
	ctrl &= ~TXGBE_PORTCTL_QINQ;
	wr32(hw, TXGBE_PORTCTL, ctrl);
}

static void
txgbe_vlan_hw_extend_enable(struct rte_eth_dev *dev)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	struct rte_eth_rxmode *rxmode = &dev->data->dev_conf.rxmode;
	struct rte_eth_txmode *txmode = &dev->data->dev_conf.txmode;
	uint32_t ctrl;

	PMD_INIT_FUNC_TRACE();

	ctrl  = rd32(hw, TXGBE_PORTCTL);
	ctrl |= TXGBE_PORTCTL_VLANEXT;
	if (rxmode->offloads & DEV_RX_OFFLOAD_QINQ_STRIP ||
	    txmode->offloads & DEV_TX_OFFLOAD_QINQ_INSERT)
		ctrl |= TXGBE_PORTCTL_QINQ;
	wr32(hw, TXGBE_PORTCTL, ctrl);
}

void
txgbe_vlan_hw_strip_config(struct rte_eth_dev *dev)
{
	struct txgbe_rx_queue *rxq;
	uint16_t i;

	PMD_INIT_FUNC_TRACE();

	for (i = 0; i < dev->data->nb_rx_queues; i++) {
		rxq = dev->data->rx_queues[i];

		if (rxq->offloads & DEV_RX_OFFLOAD_VLAN_STRIP)
			txgbe_vlan_strip_queue_set(dev, i, 1);
		else
			txgbe_vlan_strip_queue_set(dev, i, 0);
	}
}

void
txgbe_config_vlan_strip_on_all_queues(struct rte_eth_dev *dev, int mask)
{
	uint16_t i;
	struct rte_eth_rxmode *rxmode;
	struct txgbe_rx_queue *rxq;

	if (mask & ETH_VLAN_STRIP_MASK) {
		rxmode = &dev->data->dev_conf.rxmode;
		if (rxmode->offloads & DEV_RX_OFFLOAD_VLAN_STRIP)
			for (i = 0; i < dev->data->nb_rx_queues; i++) {
				rxq = dev->data->rx_queues[i];
				rxq->offloads |= DEV_RX_OFFLOAD_VLAN_STRIP;
			}
		else
			for (i = 0; i < dev->data->nb_rx_queues; i++) {
				rxq = dev->data->rx_queues[i];
				rxq->offloads &= ~DEV_RX_OFFLOAD_VLAN_STRIP;
			}
	}
}

static int
txgbe_vlan_offload_config(struct rte_eth_dev *dev, int mask)
{
	struct rte_eth_rxmode *rxmode;
	rxmode = &dev->data->dev_conf.rxmode;

	if (mask & ETH_VLAN_STRIP_MASK)
		txgbe_vlan_hw_strip_config(dev);

	if (mask & ETH_VLAN_FILTER_MASK) {
		if (rxmode->offloads & DEV_RX_OFFLOAD_VLAN_FILTER)
			txgbe_vlan_hw_filter_enable(dev);
		else
			txgbe_vlan_hw_filter_disable(dev);
	}

	if (mask & ETH_VLAN_EXTEND_MASK) {
		if (rxmode->offloads & DEV_RX_OFFLOAD_VLAN_EXTEND)
			txgbe_vlan_hw_extend_enable(dev);
		else
			txgbe_vlan_hw_extend_disable(dev);
	}

	return 0;
}

static int
txgbe_vlan_offload_set(struct rte_eth_dev *dev, int mask)
{
	txgbe_config_vlan_strip_on_all_queues(dev, mask);

	txgbe_vlan_offload_config(dev, mask);

	return 0;
}

static void
txgbe_vmdq_vlan_hw_filter_enable(struct rte_eth_dev *dev)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	/* VLNCTL: enable vlan filtering and allow all vlan tags through */
	uint32_t vlanctrl = rd32(hw, TXGBE_VLANCTL);

	vlanctrl |= TXGBE_VLANCTL_VFE; /* enable vlan filters */
	wr32(hw, TXGBE_VLANCTL, vlanctrl);
}

static int
txgbe_check_vf_rss_rxq_num(struct rte_eth_dev *dev, uint16_t nb_rx_q)
{
	struct rte_pci_device *pci_dev = RTE_ETH_DEV_TO_PCI(dev);

	switch (nb_rx_q) {
	case 1:
	case 2:
		RTE_ETH_DEV_SRIOV(dev).active = ETH_64_POOLS;
		break;
	case 4:
		RTE_ETH_DEV_SRIOV(dev).active = ETH_32_POOLS;
		break;
	default:
		return -EINVAL;
	}

	RTE_ETH_DEV_SRIOV(dev).nb_q_per_pool =
		TXGBE_MAX_RX_QUEUE_NUM / RTE_ETH_DEV_SRIOV(dev).active;
	RTE_ETH_DEV_SRIOV(dev).def_pool_q_idx =
		pci_dev->max_vfs * RTE_ETH_DEV_SRIOV(dev).nb_q_per_pool;
	return 0;
}

static int
txgbe_check_mq_mode(struct rte_eth_dev *dev)
{
	struct rte_eth_conf *dev_conf = &dev->data->dev_conf;
	uint16_t nb_rx_q = dev->data->nb_rx_queues;
	uint16_t nb_tx_q = dev->data->nb_tx_queues;

	if (RTE_ETH_DEV_SRIOV(dev).active != 0) {
		/* check multi-queue mode */
		switch (dev_conf->rxmode.mq_mode) {
		case ETH_MQ_RX_VMDQ_DCB:
			PMD_INIT_LOG(INFO, "ETH_MQ_RX_VMDQ_DCB mode supported in SRIOV");
			break;
		case ETH_MQ_RX_VMDQ_DCB_RSS:
			/* DCB/RSS VMDQ in SRIOV mode, not implement yet */
			PMD_INIT_LOG(ERR, "SRIOV active,"
					" unsupported mq_mode rx %d.",
					dev_conf->rxmode.mq_mode);
			return -EINVAL;
		case ETH_MQ_RX_RSS:
		case ETH_MQ_RX_VMDQ_RSS:
			dev->data->dev_conf.rxmode.mq_mode = ETH_MQ_RX_VMDQ_RSS;
			if (nb_rx_q <= RTE_ETH_DEV_SRIOV(dev).nb_q_per_pool)
				if (txgbe_check_vf_rss_rxq_num(dev, nb_rx_q)) {
					PMD_INIT_LOG(ERR, "SRIOV is active,"
						" invalid queue number"
						" for VMDQ RSS, allowed"
						" value are 1, 2 or 4.");
					return -EINVAL;
				}
			break;
		case ETH_MQ_RX_VMDQ_ONLY:
		case ETH_MQ_RX_NONE:
			/* if nothing mq mode configure, use default scheme */
			dev->data->dev_conf.rxmode.mq_mode =
				ETH_MQ_RX_VMDQ_ONLY;
			break;
		default: /* ETH_MQ_RX_DCB, ETH_MQ_RX_DCB_RSS or ETH_MQ_TX_DCB*/
			/* SRIOV only works in VMDq enable mode */
			PMD_INIT_LOG(ERR, "SRIOV is active,"
					" wrong mq_mode rx %d.",
					dev_conf->rxmode.mq_mode);
			return -EINVAL;
		}

		switch (dev_conf->txmode.mq_mode) {
		case ETH_MQ_TX_VMDQ_DCB:
			PMD_INIT_LOG(INFO, "ETH_MQ_TX_VMDQ_DCB mode supported in SRIOV");
			dev->data->dev_conf.txmode.mq_mode = ETH_MQ_TX_VMDQ_DCB;
			break;
		default: /* ETH_MQ_TX_VMDQ_ONLY or ETH_MQ_TX_NONE */
			dev->data->dev_conf.txmode.mq_mode =
				ETH_MQ_TX_VMDQ_ONLY;
			break;
		}

		/* check valid queue number */
		if ((nb_rx_q > RTE_ETH_DEV_SRIOV(dev).nb_q_per_pool) ||
		    (nb_tx_q > RTE_ETH_DEV_SRIOV(dev).nb_q_per_pool)) {
			PMD_INIT_LOG(ERR, "SRIOV is active,"
					" nb_rx_q=%d nb_tx_q=%d queue number"
					" must be less than or equal to %d.",
					nb_rx_q, nb_tx_q,
					RTE_ETH_DEV_SRIOV(dev).nb_q_per_pool);
			return -EINVAL;
		}
	} else {
		if (dev_conf->rxmode.mq_mode == ETH_MQ_RX_VMDQ_DCB_RSS) {
			PMD_INIT_LOG(ERR, "VMDQ+DCB+RSS mq_mode is"
					  " not supported.");
			return -EINVAL;
		}
		/* check configuration for vmdb+dcb mode */
		if (dev_conf->rxmode.mq_mode == ETH_MQ_RX_VMDQ_DCB) {
			const struct rte_eth_vmdq_dcb_conf *conf;

			if (nb_rx_q != TXGBE_VMDQ_DCB_NB_QUEUES) {
				PMD_INIT_LOG(ERR, "VMDQ+DCB, nb_rx_q != %d.",
						TXGBE_VMDQ_DCB_NB_QUEUES);
				return -EINVAL;
			}
			conf = &dev_conf->rx_adv_conf.vmdq_dcb_conf;
			if (!(conf->nb_queue_pools == ETH_16_POOLS ||
			       conf->nb_queue_pools == ETH_32_POOLS)) {
				PMD_INIT_LOG(ERR, "VMDQ+DCB selected,"
						" nb_queue_pools must be %d or %d.",
						ETH_16_POOLS, ETH_32_POOLS);
				return -EINVAL;
			}
		}
		if (dev_conf->txmode.mq_mode == ETH_MQ_TX_VMDQ_DCB) {
			const struct rte_eth_vmdq_dcb_tx_conf *conf;

			if (nb_tx_q != TXGBE_VMDQ_DCB_NB_QUEUES) {
				PMD_INIT_LOG(ERR, "VMDQ+DCB, nb_tx_q != %d",
						 TXGBE_VMDQ_DCB_NB_QUEUES);
				return -EINVAL;
			}
			conf = &dev_conf->tx_adv_conf.vmdq_dcb_tx_conf;
			if (!(conf->nb_queue_pools == ETH_16_POOLS ||
			       conf->nb_queue_pools == ETH_32_POOLS)) {
				PMD_INIT_LOG(ERR, "VMDQ+DCB selected,"
						" nb_queue_pools != %d and"
						" nb_queue_pools != %d.",
						ETH_16_POOLS, ETH_32_POOLS);
				return -EINVAL;
			}
		}

		/* For DCB mode check our configuration before we go further */
		if (dev_conf->rxmode.mq_mode == ETH_MQ_RX_DCB) {
			const struct rte_eth_dcb_rx_conf *conf;

			conf = &dev_conf->rx_adv_conf.dcb_rx_conf;
			if (!(conf->nb_tcs == ETH_4_TCS ||
			       conf->nb_tcs == ETH_8_TCS)) {
				PMD_INIT_LOG(ERR, "DCB selected, nb_tcs != %d"
						" and nb_tcs != %d.",
						ETH_4_TCS, ETH_8_TCS);
				return -EINVAL;
			}
		}

		if (dev_conf->txmode.mq_mode == ETH_MQ_TX_DCB) {
			const struct rte_eth_dcb_tx_conf *conf;

			conf = &dev_conf->tx_adv_conf.dcb_tx_conf;
			if (!(conf->nb_tcs == ETH_4_TCS ||
			       conf->nb_tcs == ETH_8_TCS)) {
				PMD_INIT_LOG(ERR, "DCB selected, nb_tcs != %d"
						" and nb_tcs != %d.",
						ETH_4_TCS, ETH_8_TCS);
				return -EINVAL;
			}
		}
	}
	return 0;
}

static int
txgbe_dev_configure(struct rte_eth_dev *dev)
{
	struct txgbe_interrupt *intr = TXGBE_DEV_INTR(dev);
	struct txgbe_adapter *adapter = TXGBE_DEV_ADAPTER(dev);
	int ret;

	PMD_INIT_FUNC_TRACE();

	if (dev->data->dev_conf.rxmode.mq_mode & ETH_MQ_RX_RSS_FLAG)
		dev->data->dev_conf.rxmode.offloads |= DEV_RX_OFFLOAD_RSS_HASH;

	/* multiple queue mode checking */
	ret  = txgbe_check_mq_mode(dev);
	if (ret != 0) {
		PMD_DRV_LOG(ERR, "txgbe_check_mq_mode fails with %d.",
			    ret);
		return ret;
	}

	/* set flag to update link status after init */
	intr->flags |= TXGBE_FLAG_NEED_LINK_UPDATE;

	/*
	 * Initialize to TRUE. If any of Rx queues doesn't meet the bulk
	 * allocation Rx preconditions we will reset it.
	 */
	adapter->rx_bulk_alloc_allowed = true;

	return 0;
}

static void
txgbe_dev_phy_intr_setup(struct rte_eth_dev *dev)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	struct txgbe_interrupt *intr = TXGBE_DEV_INTR(dev);
	uint32_t gpie;

	gpie = rd32(hw, TXGBE_GPIOINTEN);
	gpie |= TXGBE_GPIOBIT_6;
	wr32(hw, TXGBE_GPIOINTEN, gpie);
	intr->mask_misc |= TXGBE_ICRMISC_GPIO;
}

int
txgbe_set_vf_rate_limit(struct rte_eth_dev *dev, uint16_t vf,
			uint16_t tx_rate, uint64_t q_msk)
{
	struct txgbe_hw *hw;
	struct txgbe_vf_info *vfinfo;
	struct rte_eth_link link;
	uint8_t  nb_q_per_pool;
	uint32_t queue_stride;
	uint32_t queue_idx, idx = 0, vf_idx;
	uint32_t queue_end;
	uint16_t total_rate = 0;
	struct rte_pci_device *pci_dev;
	int ret;

	pci_dev = RTE_ETH_DEV_TO_PCI(dev);
	ret = rte_eth_link_get_nowait(dev->data->port_id, &link);
	if (ret < 0)
		return ret;

	if (vf >= pci_dev->max_vfs)
		return -EINVAL;

	if (tx_rate > link.link_speed)
		return -EINVAL;

	if (q_msk == 0)
		return 0;

	hw = TXGBE_DEV_HW(dev);
	vfinfo = *(TXGBE_DEV_VFDATA(dev));
	nb_q_per_pool = RTE_ETH_DEV_SRIOV(dev).nb_q_per_pool;
	queue_stride = TXGBE_MAX_RX_QUEUE_NUM / RTE_ETH_DEV_SRIOV(dev).active;
	queue_idx = vf * queue_stride;
	queue_end = queue_idx + nb_q_per_pool - 1;
	if (queue_end >= hw->mac.max_tx_queues)
		return -EINVAL;

	if (vfinfo) {
		for (vf_idx = 0; vf_idx < pci_dev->max_vfs; vf_idx++) {
			if (vf_idx == vf)
				continue;
			for (idx = 0; idx < RTE_DIM(vfinfo[vf_idx].tx_rate);
				idx++)
				total_rate += vfinfo[vf_idx].tx_rate[idx];
		}
	} else {
		return -EINVAL;
	}

	/* Store tx_rate for this vf. */
	for (idx = 0; idx < nb_q_per_pool; idx++) {
		if (((uint64_t)0x1 << idx) & q_msk) {
			if (vfinfo[vf].tx_rate[idx] != tx_rate)
				vfinfo[vf].tx_rate[idx] = tx_rate;
			total_rate += tx_rate;
		}
	}

	if (total_rate > dev->data->dev_link.link_speed) {
		/* Reset stored TX rate of the VF if it causes exceed
		 * link speed.
		 */
		memset(vfinfo[vf].tx_rate, 0, sizeof(vfinfo[vf].tx_rate));
		return -EINVAL;
	}

	/* Set ARBTXRATE of each queue/pool for vf X  */
	for (; queue_idx <= queue_end; queue_idx++) {
		if (0x1 & q_msk)
			txgbe_set_queue_rate_limit(dev, queue_idx, tx_rate);
		q_msk = q_msk >> 1;
	}

	return 0;
}

/*
 * Configure device link speed and setup link.
 * It returns 0 on success.
 */
static int
txgbe_dev_start(struct rte_eth_dev *dev)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	struct txgbe_hw_stats *hw_stats = TXGBE_DEV_STATS(dev);
	struct txgbe_vf_info *vfinfo = *TXGBE_DEV_VFDATA(dev);
	struct rte_pci_device *pci_dev = RTE_ETH_DEV_TO_PCI(dev);
	struct rte_intr_handle *intr_handle = &pci_dev->intr_handle;
	uint32_t intr_vector = 0;
	int err;
	bool link_up = false, negotiate = 0;
	uint32_t speed = 0;
	uint32_t allowed_speeds = 0;
	int mask = 0;
	int status;
	uint16_t vf, idx;
	uint32_t *link_speeds;

	PMD_INIT_FUNC_TRACE();

	/* TXGBE devices don't support:
	 *    - half duplex (checked afterwards for valid speeds)
	 *    - fixed speed: TODO implement
	 */
	if (dev->data->dev_conf.link_speeds & ETH_LINK_SPEED_FIXED) {
		PMD_INIT_LOG(ERR,
		"Invalid link_speeds for port %u, fix speed not supported",
				dev->data->port_id);
		return -EINVAL;
	}

	/* Stop the link setup handler before resetting the HW. */
	rte_eal_alarm_cancel(txgbe_dev_setup_link_alarm_handler, dev);

	/* disable uio/vfio intr/eventfd mapping */
	rte_intr_disable(intr_handle);

	/* stop adapter */
	hw->adapter_stopped = 0;
	txgbe_stop_hw(hw);

	/* reinitialize adapter
	 * this calls reset and start
	 */
	hw->nb_rx_queues = dev->data->nb_rx_queues;
	hw->nb_tx_queues = dev->data->nb_tx_queues;
	status = txgbe_pf_reset_hw(hw);
	if (status != 0)
		return -1;
	hw->mac.start_hw(hw);
	hw->mac.get_link_status = true;

	/* configure PF module if SRIOV enabled */
	txgbe_pf_host_configure(dev);

	txgbe_dev_phy_intr_setup(dev);

	/* check and configure queue intr-vector mapping */
	if ((rte_intr_cap_multiple(intr_handle) ||
	     !RTE_ETH_DEV_SRIOV(dev).active) &&
	    dev->data->dev_conf.intr_conf.rxq != 0) {
		intr_vector = dev->data->nb_rx_queues;
		if (rte_intr_efd_enable(intr_handle, intr_vector))
			return -1;
	}

	if (rte_intr_dp_is_en(intr_handle) && !intr_handle->intr_vec) {
		intr_handle->intr_vec =
			rte_zmalloc("intr_vec",
				    dev->data->nb_rx_queues * sizeof(int), 0);
		if (intr_handle->intr_vec == NULL) {
			PMD_INIT_LOG(ERR, "Failed to allocate %d rx_queues"
				     " intr_vec", dev->data->nb_rx_queues);
			return -ENOMEM;
		}
	}

	/* confiugre msix for sleep until rx interrupt */
	txgbe_configure_msix(dev);

	/* initialize transmission unit */
	txgbe_dev_tx_init(dev);

	/* This can fail when allocating mbufs for descriptor rings */
	err = txgbe_dev_rx_init(dev);
	if (err) {
		PMD_INIT_LOG(ERR, "Unable to initialize RX hardware");
		goto error;
	}

	mask = ETH_VLAN_STRIP_MASK | ETH_VLAN_FILTER_MASK |
		ETH_VLAN_EXTEND_MASK;
	err = txgbe_vlan_offload_config(dev, mask);
	if (err) {
		PMD_INIT_LOG(ERR, "Unable to set VLAN offload");
		goto error;
	}

	if (dev->data->dev_conf.rxmode.mq_mode == ETH_MQ_RX_VMDQ_ONLY) {
		/* Enable vlan filtering for VMDq */
		txgbe_vmdq_vlan_hw_filter_enable(dev);
	}

	/* Configure DCB hw */
	txgbe_configure_pb(dev);
	txgbe_configure_port(dev);
	txgbe_configure_dcb(dev);

	/* Restore vf rate limit */
	if (vfinfo != NULL) {
		for (vf = 0; vf < pci_dev->max_vfs; vf++)
			for (idx = 0; idx < TXGBE_MAX_QUEUE_NUM_PER_VF; idx++)
				if (vfinfo[vf].tx_rate[idx] != 0)
					txgbe_set_vf_rate_limit(dev, vf,
						vfinfo[vf].tx_rate[idx],
						1 << idx);
	}

	err = txgbe_dev_rxtx_start(dev);
	if (err < 0) {
		PMD_INIT_LOG(ERR, "Unable to start rxtx queues");
		goto error;
	}

	/* Skip link setup if loopback mode is enabled. */
	if (hw->mac.type == txgbe_mac_raptor &&
	    dev->data->dev_conf.lpbk_mode)
		goto skip_link_setup;

	if (txgbe_is_sfp(hw) && hw->phy.multispeed_fiber) {
		err = hw->mac.setup_sfp(hw);
		if (err)
			goto error;
	}

	if (hw->phy.media_type == txgbe_media_type_copper) {
		/* Turn on the copper */
		hw->phy.set_phy_power(hw, true);
	} else {
		/* Turn on the laser */
		hw->mac.enable_tx_laser(hw);
	}

	err = hw->mac.check_link(hw, &speed, &link_up, 0);
	if (err)
		goto error;
	dev->data->dev_link.link_status = link_up;

	err = hw->mac.get_link_capabilities(hw, &speed, &negotiate);
	if (err)
		goto error;

	allowed_speeds = ETH_LINK_SPEED_100M | ETH_LINK_SPEED_1G |
			ETH_LINK_SPEED_10G;

	link_speeds = &dev->data->dev_conf.link_speeds;
	if (*link_speeds & ~allowed_speeds) {
		PMD_INIT_LOG(ERR, "Invalid link setting");
		goto error;
	}

	speed = 0x0;
	if (*link_speeds == ETH_LINK_SPEED_AUTONEG) {
		speed = (TXGBE_LINK_SPEED_100M_FULL |
			 TXGBE_LINK_SPEED_1GB_FULL |
			 TXGBE_LINK_SPEED_10GB_FULL);
	} else {
		if (*link_speeds & ETH_LINK_SPEED_10G)
			speed |= TXGBE_LINK_SPEED_10GB_FULL;
		if (*link_speeds & ETH_LINK_SPEED_5G)
			speed |= TXGBE_LINK_SPEED_5GB_FULL;
		if (*link_speeds & ETH_LINK_SPEED_2_5G)
			speed |= TXGBE_LINK_SPEED_2_5GB_FULL;
		if (*link_speeds & ETH_LINK_SPEED_1G)
			speed |= TXGBE_LINK_SPEED_1GB_FULL;
		if (*link_speeds & ETH_LINK_SPEED_100M)
			speed |= TXGBE_LINK_SPEED_100M_FULL;
	}

	err = hw->mac.setup_link(hw, speed, link_up);
	if (err)
		goto error;

skip_link_setup:

	if (rte_intr_allow_others(intr_handle)) {
		/* check if lsc interrupt is enabled */
		if (dev->data->dev_conf.intr_conf.lsc != 0)
			txgbe_dev_lsc_interrupt_setup(dev, TRUE);
		else
			txgbe_dev_lsc_interrupt_setup(dev, FALSE);
		txgbe_dev_macsec_interrupt_setup(dev);
		txgbe_set_ivar_map(hw, -1, 1, TXGBE_MISC_VEC_ID);
	} else {
		rte_intr_callback_unregister(intr_handle,
					     txgbe_dev_interrupt_handler, dev);
		if (dev->data->dev_conf.intr_conf.lsc != 0)
			PMD_INIT_LOG(INFO, "lsc won't enable because of"
				     " no intr multiplex");
	}

	/* check if rxq interrupt is enabled */
	if (dev->data->dev_conf.intr_conf.rxq != 0 &&
	    rte_intr_dp_is_en(intr_handle))
		txgbe_dev_rxq_interrupt_setup(dev);

	/* enable uio/vfio intr/eventfd mapping */
	rte_intr_enable(intr_handle);

	/* resume enabled intr since hw reset */
	txgbe_enable_intr(dev);

	/*
	 * Update link status right before return, because it may
	 * start link configuration process in a separate thread.
	 */
	txgbe_dev_link_update(dev, 0);

	wr32m(hw, TXGBE_LEDCTL, 0xFFFFFFFF, TXGBE_LEDCTL_ORD_MASK);

	txgbe_read_stats_registers(hw, hw_stats);
	hw->offset_loaded = 1;

	return 0;

error:
	PMD_INIT_LOG(ERR, "failure in dev start: %d", err);
	txgbe_dev_clear_queues(dev);
	return -EIO;
}

/*
 * Stop device: disable rx and tx functions to allow for reconfiguring.
 */
static int
txgbe_dev_stop(struct rte_eth_dev *dev)
{
	struct rte_eth_link link;
	struct txgbe_adapter *adapter = TXGBE_DEV_ADAPTER(dev);
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	struct txgbe_vf_info *vfinfo = *TXGBE_DEV_VFDATA(dev);
	struct rte_pci_device *pci_dev = RTE_ETH_DEV_TO_PCI(dev);
	struct rte_intr_handle *intr_handle = &pci_dev->intr_handle;
	int vf;

	if (hw->adapter_stopped)
		return 0;

	PMD_INIT_FUNC_TRACE();

	rte_eal_alarm_cancel(txgbe_dev_setup_link_alarm_handler, dev);

	/* disable interrupts */
	txgbe_disable_intr(hw);

	/* reset the NIC */
	txgbe_pf_reset_hw(hw);
	hw->adapter_stopped = 0;

	/* stop adapter */
	txgbe_stop_hw(hw);

	for (vf = 0; vfinfo != NULL && vf < pci_dev->max_vfs; vf++)
		vfinfo[vf].clear_to_send = false;

	if (hw->phy.media_type == txgbe_media_type_copper) {
		/* Turn off the copper */
		hw->phy.set_phy_power(hw, false);
	} else {
		/* Turn off the laser */
		hw->mac.disable_tx_laser(hw);
	}

	txgbe_dev_clear_queues(dev);

	/* Clear stored conf */
	dev->data->scattered_rx = 0;
	dev->data->lro = 0;

	/* Clear recorded link status */
	memset(&link, 0, sizeof(link));
	rte_eth_linkstatus_set(dev, &link);

	if (!rte_intr_allow_others(intr_handle))
		/* resume to the default handler */
		rte_intr_callback_register(intr_handle,
					   txgbe_dev_interrupt_handler,
					   (void *)dev);

	/* Clean datapath event and queue/vec mapping */
	rte_intr_efd_disable(intr_handle);
	if (intr_handle->intr_vec != NULL) {
		rte_free(intr_handle->intr_vec);
		intr_handle->intr_vec = NULL;
	}

	adapter->rss_reta_updated = 0;
	wr32m(hw, TXGBE_LEDCTL, 0xFFFFFFFF, TXGBE_LEDCTL_SEL_MASK);

	hw->adapter_stopped = true;
	dev->data->dev_started = 0;

	return 0;
}

/*
 * Set device link up: enable tx.
 */
static int
txgbe_dev_set_link_up(struct rte_eth_dev *dev)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);

	if (hw->phy.media_type == txgbe_media_type_copper) {
		/* Turn on the copper */
		hw->phy.set_phy_power(hw, true);
	} else {
		/* Turn on the laser */
		hw->mac.enable_tx_laser(hw);
		txgbe_dev_link_update(dev, 0);
	}

	return 0;
}

/*
 * Set device link down: disable tx.
 */
static int
txgbe_dev_set_link_down(struct rte_eth_dev *dev)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);

	if (hw->phy.media_type == txgbe_media_type_copper) {
		/* Turn off the copper */
		hw->phy.set_phy_power(hw, false);
	} else {
		/* Turn off the laser */
		hw->mac.disable_tx_laser(hw);
		txgbe_dev_link_update(dev, 0);
	}

	return 0;
}

/*
 * Reset and stop device.
 */
static int
txgbe_dev_close(struct rte_eth_dev *dev)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	struct rte_pci_device *pci_dev = RTE_ETH_DEV_TO_PCI(dev);
	struct rte_intr_handle *intr_handle = &pci_dev->intr_handle;
	int retries = 0;
	int ret;

	PMD_INIT_FUNC_TRACE();

	txgbe_pf_reset_hw(hw);

	ret = txgbe_dev_stop(dev);

	txgbe_dev_free_queues(dev);

	/* reprogram the RAR[0] in case user changed it. */
	txgbe_set_rar(hw, 0, hw->mac.addr, 0, true);

	/* Unlock any pending hardware semaphore */
	txgbe_swfw_lock_reset(hw);

	/* disable uio intr before callback unregister */
	rte_intr_disable(intr_handle);

	do {
		ret = rte_intr_callback_unregister(intr_handle,
				txgbe_dev_interrupt_handler, dev);
		if (ret >= 0 || ret == -ENOENT) {
			break;
		} else if (ret != -EAGAIN) {
			PMD_INIT_LOG(ERR,
				"intr callback unregister failed: %d",
				ret);
		}
		rte_delay_ms(100);
	} while (retries++ < (10 + TXGBE_LINK_UP_TIME));

	/* cancel the delay handler before remove dev */
	rte_eal_alarm_cancel(txgbe_dev_interrupt_delayed_handler, dev);

	/* uninitialize PF if max_vfs not zero */
	txgbe_pf_host_uninit(dev);

	rte_free(dev->data->mac_addrs);
	dev->data->mac_addrs = NULL;

	rte_free(dev->data->hash_mac_addrs);
	dev->data->hash_mac_addrs = NULL;

	return ret;
}

/*
 * Reset PF device.
 */
static int
txgbe_dev_reset(struct rte_eth_dev *dev)
{
	int ret;

	/* When a DPDK PMD PF begin to reset PF port, it should notify all
	 * its VF to make them align with it. The detailed notification
	 * mechanism is PMD specific. As to txgbe PF, it is rather complex.
	 * To avoid unexpected behavior in VF, currently reset of PF with
	 * SR-IOV activation is not supported. It might be supported later.
	 */
	if (dev->data->sriov.active)
		return -ENOTSUP;

	ret = eth_txgbe_dev_uninit(dev);
	if (ret)
		return ret;

	ret = eth_txgbe_dev_init(dev, NULL);

	return ret;
}

#define UPDATE_QP_COUNTER_32bit(reg, last_counter, counter)     \
	{                                                       \
		uint32_t current_counter = rd32(hw, reg);       \
		if (current_counter < last_counter)             \
			current_counter += 0x100000000LL;       \
		if (!hw->offset_loaded)                         \
			last_counter = current_counter;         \
		counter = current_counter - last_counter;       \
		counter &= 0xFFFFFFFFLL;                        \
	}

#define UPDATE_QP_COUNTER_36bit(reg_lsb, reg_msb, last_counter, counter) \
	{                                                                \
		uint64_t current_counter_lsb = rd32(hw, reg_lsb);        \
		uint64_t current_counter_msb = rd32(hw, reg_msb);        \
		uint64_t current_counter = (current_counter_msb << 32) | \
			current_counter_lsb;                             \
		if (current_counter < last_counter)                      \
			current_counter += 0x1000000000LL;               \
		if (!hw->offset_loaded)                                  \
			last_counter = current_counter;                  \
		counter = current_counter - last_counter;                \
		counter &= 0xFFFFFFFFFLL;                                \
	}

void
txgbe_read_stats_registers(struct txgbe_hw *hw,
			   struct txgbe_hw_stats *hw_stats)
{
	unsigned int i;

	/* QP Stats */
	for (i = 0; i < hw->nb_rx_queues; i++) {
		UPDATE_QP_COUNTER_32bit(TXGBE_QPRXPKT(i),
			hw->qp_last[i].rx_qp_packets,
			hw_stats->qp[i].rx_qp_packets);
		UPDATE_QP_COUNTER_36bit(TXGBE_QPRXOCTL(i), TXGBE_QPRXOCTH(i),
			hw->qp_last[i].rx_qp_bytes,
			hw_stats->qp[i].rx_qp_bytes);
		UPDATE_QP_COUNTER_32bit(TXGBE_QPRXMPKT(i),
			hw->qp_last[i].rx_qp_mc_packets,
			hw_stats->qp[i].rx_qp_mc_packets);
	}

	for (i = 0; i < hw->nb_tx_queues; i++) {
		UPDATE_QP_COUNTER_32bit(TXGBE_QPTXPKT(i),
			hw->qp_last[i].tx_qp_packets,
			hw_stats->qp[i].tx_qp_packets);
		UPDATE_QP_COUNTER_36bit(TXGBE_QPTXOCTL(i), TXGBE_QPTXOCTH(i),
			hw->qp_last[i].tx_qp_bytes,
			hw_stats->qp[i].tx_qp_bytes);
	}
	/* PB Stats */
	for (i = 0; i < TXGBE_MAX_UP; i++) {
		hw_stats->up[i].rx_up_xon_packets +=
				rd32(hw, TXGBE_PBRXUPXON(i));
		hw_stats->up[i].rx_up_xoff_packets +=
				rd32(hw, TXGBE_PBRXUPXOFF(i));
		hw_stats->up[i].tx_up_xon_packets +=
				rd32(hw, TXGBE_PBTXUPXON(i));
		hw_stats->up[i].tx_up_xoff_packets +=
				rd32(hw, TXGBE_PBTXUPXOFF(i));
		hw_stats->up[i].tx_up_xon2off_packets +=
				rd32(hw, TXGBE_PBTXUPOFF(i));
		hw_stats->up[i].rx_up_dropped +=
				rd32(hw, TXGBE_PBRXMISS(i));
	}
	hw_stats->rx_xon_packets += rd32(hw, TXGBE_PBRXLNKXON);
	hw_stats->rx_xoff_packets += rd32(hw, TXGBE_PBRXLNKXOFF);
	hw_stats->tx_xon_packets += rd32(hw, TXGBE_PBTXLNKXON);
	hw_stats->tx_xoff_packets += rd32(hw, TXGBE_PBTXLNKXOFF);

	/* DMA Stats */
	hw_stats->rx_packets += rd32(hw, TXGBE_DMARXPKT);
	hw_stats->tx_packets += rd32(hw, TXGBE_DMATXPKT);

	hw_stats->rx_bytes += rd64(hw, TXGBE_DMARXOCTL);
	hw_stats->tx_bytes += rd64(hw, TXGBE_DMATXOCTL);
	hw_stats->rx_drop_packets += rd32(hw, TXGBE_PBRXDROP);

	/* MAC Stats */
	hw_stats->rx_crc_errors += rd64(hw, TXGBE_MACRXERRCRCL);
	hw_stats->rx_multicast_packets += rd64(hw, TXGBE_MACRXMPKTL);
	hw_stats->tx_multicast_packets += rd64(hw, TXGBE_MACTXMPKTL);

	hw_stats->rx_total_packets += rd64(hw, TXGBE_MACRXPKTL);
	hw_stats->tx_total_packets += rd64(hw, TXGBE_MACTXPKTL);
	hw_stats->rx_total_bytes += rd64(hw, TXGBE_MACRXGBOCTL);

	hw_stats->rx_broadcast_packets += rd64(hw, TXGBE_MACRXOCTL);
	hw_stats->tx_broadcast_packets += rd32(hw, TXGBE_MACTXOCTL);

	hw_stats->rx_size_64_packets += rd64(hw, TXGBE_MACRX1TO64L);
	hw_stats->rx_size_65_to_127_packets += rd64(hw, TXGBE_MACRX65TO127L);
	hw_stats->rx_size_128_to_255_packets += rd64(hw, TXGBE_MACRX128TO255L);
	hw_stats->rx_size_256_to_511_packets += rd64(hw, TXGBE_MACRX256TO511L);
	hw_stats->rx_size_512_to_1023_packets +=
			rd64(hw, TXGBE_MACRX512TO1023L);
	hw_stats->rx_size_1024_to_max_packets +=
			rd64(hw, TXGBE_MACRX1024TOMAXL);
	hw_stats->tx_size_64_packets += rd64(hw, TXGBE_MACTX1TO64L);
	hw_stats->tx_size_65_to_127_packets += rd64(hw, TXGBE_MACTX65TO127L);
	hw_stats->tx_size_128_to_255_packets += rd64(hw, TXGBE_MACTX128TO255L);
	hw_stats->tx_size_256_to_511_packets += rd64(hw, TXGBE_MACTX256TO511L);
	hw_stats->tx_size_512_to_1023_packets +=
			rd64(hw, TXGBE_MACTX512TO1023L);
	hw_stats->tx_size_1024_to_max_packets +=
			rd64(hw, TXGBE_MACTX1024TOMAXL);

	hw_stats->rx_undersize_errors += rd64(hw, TXGBE_MACRXERRLENL);
	hw_stats->rx_oversize_errors += rd32(hw, TXGBE_MACRXOVERSIZE);
	hw_stats->rx_jabber_errors += rd32(hw, TXGBE_MACRXJABBER);

	/* MNG Stats */
	hw_stats->mng_bmc2host_packets = rd32(hw, TXGBE_MNGBMC2OS);
	hw_stats->mng_host2bmc_packets = rd32(hw, TXGBE_MNGOS2BMC);
	hw_stats->rx_management_packets = rd32(hw, TXGBE_DMARXMNG);
	hw_stats->tx_management_packets = rd32(hw, TXGBE_DMATXMNG);

	/* FCoE Stats */
	hw_stats->rx_fcoe_crc_errors += rd32(hw, TXGBE_FCOECRC);
	hw_stats->rx_fcoe_mbuf_allocation_errors += rd32(hw, TXGBE_FCOELAST);
	hw_stats->rx_fcoe_dropped += rd32(hw, TXGBE_FCOERPDC);
	hw_stats->rx_fcoe_packets += rd32(hw, TXGBE_FCOEPRC);
	hw_stats->tx_fcoe_packets += rd32(hw, TXGBE_FCOEPTC);
	hw_stats->rx_fcoe_bytes += rd32(hw, TXGBE_FCOEDWRC);
	hw_stats->tx_fcoe_bytes += rd32(hw, TXGBE_FCOEDWTC);

	/* Flow Director Stats */
	hw_stats->flow_director_matched_filters += rd32(hw, TXGBE_FDIRMATCH);
	hw_stats->flow_director_missed_filters += rd32(hw, TXGBE_FDIRMISS);
	hw_stats->flow_director_added_filters +=
		TXGBE_FDIRUSED_ADD(rd32(hw, TXGBE_FDIRUSED));
	hw_stats->flow_director_removed_filters +=
		TXGBE_FDIRUSED_REM(rd32(hw, TXGBE_FDIRUSED));
	hw_stats->flow_director_filter_add_errors +=
		TXGBE_FDIRFAIL_ADD(rd32(hw, TXGBE_FDIRFAIL));
	hw_stats->flow_director_filter_remove_errors +=
		TXGBE_FDIRFAIL_REM(rd32(hw, TXGBE_FDIRFAIL));

	/* MACsec Stats */
	hw_stats->tx_macsec_pkts_untagged += rd32(hw, TXGBE_LSECTX_UTPKT);
	hw_stats->tx_macsec_pkts_encrypted +=
			rd32(hw, TXGBE_LSECTX_ENCPKT);
	hw_stats->tx_macsec_pkts_protected +=
			rd32(hw, TXGBE_LSECTX_PROTPKT);
	hw_stats->tx_macsec_octets_encrypted +=
			rd32(hw, TXGBE_LSECTX_ENCOCT);
	hw_stats->tx_macsec_octets_protected +=
			rd32(hw, TXGBE_LSECTX_PROTOCT);
	hw_stats->rx_macsec_pkts_untagged += rd32(hw, TXGBE_LSECRX_UTPKT);
	hw_stats->rx_macsec_pkts_badtag += rd32(hw, TXGBE_LSECRX_BTPKT);
	hw_stats->rx_macsec_pkts_nosci += rd32(hw, TXGBE_LSECRX_NOSCIPKT);
	hw_stats->rx_macsec_pkts_unknownsci += rd32(hw, TXGBE_LSECRX_UNSCIPKT);
	hw_stats->rx_macsec_octets_decrypted += rd32(hw, TXGBE_LSECRX_DECOCT);
	hw_stats->rx_macsec_octets_validated += rd32(hw, TXGBE_LSECRX_VLDOCT);
	hw_stats->rx_macsec_sc_pkts_unchecked +=
			rd32(hw, TXGBE_LSECRX_UNCHKPKT);
	hw_stats->rx_macsec_sc_pkts_delayed += rd32(hw, TXGBE_LSECRX_DLYPKT);
	hw_stats->rx_macsec_sc_pkts_late += rd32(hw, TXGBE_LSECRX_LATEPKT);
	for (i = 0; i < 2; i++) {
		hw_stats->rx_macsec_sa_pkts_ok +=
			rd32(hw, TXGBE_LSECRX_OKPKT(i));
		hw_stats->rx_macsec_sa_pkts_invalid +=
			rd32(hw, TXGBE_LSECRX_INVPKT(i));
		hw_stats->rx_macsec_sa_pkts_notvalid +=
			rd32(hw, TXGBE_LSECRX_BADPKT(i));
	}
	hw_stats->rx_macsec_sa_pkts_unusedsa +=
			rd32(hw, TXGBE_LSECRX_INVSAPKT);
	hw_stats->rx_macsec_sa_pkts_notusingsa +=
			rd32(hw, TXGBE_LSECRX_BADSAPKT);

	hw_stats->rx_total_missed_packets = 0;
	for (i = 0; i < TXGBE_MAX_UP; i++) {
		hw_stats->rx_total_missed_packets +=
			hw_stats->up[i].rx_up_dropped;
	}
}

static int
txgbe_dev_stats_get(struct rte_eth_dev *dev, struct rte_eth_stats *stats)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	struct txgbe_hw_stats *hw_stats = TXGBE_DEV_STATS(dev);
	struct txgbe_stat_mappings *stat_mappings =
			TXGBE_DEV_STAT_MAPPINGS(dev);
	uint32_t i, j;

	txgbe_read_stats_registers(hw, hw_stats);

	if (stats == NULL)
		return -EINVAL;

	/* Fill out the rte_eth_stats statistics structure */
	stats->ipackets = hw_stats->rx_packets;
	stats->ibytes = hw_stats->rx_bytes;
	stats->opackets = hw_stats->tx_packets;
	stats->obytes = hw_stats->tx_bytes;

	memset(&stats->q_ipackets, 0, sizeof(stats->q_ipackets));
	memset(&stats->q_opackets, 0, sizeof(stats->q_opackets));
	memset(&stats->q_ibytes, 0, sizeof(stats->q_ibytes));
	memset(&stats->q_obytes, 0, sizeof(stats->q_obytes));
	memset(&stats->q_errors, 0, sizeof(stats->q_errors));
	for (i = 0; i < TXGBE_MAX_QP; i++) {
		uint32_t n = i / NB_QMAP_FIELDS_PER_QSM_REG;
		uint32_t offset = (i % NB_QMAP_FIELDS_PER_QSM_REG) * 8;
		uint32_t q_map;

		q_map = (stat_mappings->rqsm[n] >> offset)
				& QMAP_FIELD_RESERVED_BITS_MASK;
		j = (q_map < RTE_ETHDEV_QUEUE_STAT_CNTRS
		     ? q_map : q_map % RTE_ETHDEV_QUEUE_STAT_CNTRS);
		stats->q_ipackets[j] += hw_stats->qp[i].rx_qp_packets;
		stats->q_ibytes[j] += hw_stats->qp[i].rx_qp_bytes;

		q_map = (stat_mappings->tqsm[n] >> offset)
				& QMAP_FIELD_RESERVED_BITS_MASK;
		j = (q_map < RTE_ETHDEV_QUEUE_STAT_CNTRS
		     ? q_map : q_map % RTE_ETHDEV_QUEUE_STAT_CNTRS);
		stats->q_opackets[j] += hw_stats->qp[i].tx_qp_packets;
		stats->q_obytes[j] += hw_stats->qp[i].tx_qp_bytes;
	}

	/* Rx Errors */
	stats->imissed  = hw_stats->rx_total_missed_packets;
	stats->ierrors  = hw_stats->rx_crc_errors +
			  hw_stats->rx_mac_short_packet_dropped +
			  hw_stats->rx_length_errors +
			  hw_stats->rx_undersize_errors +
			  hw_stats->rx_oversize_errors +
			  hw_stats->rx_drop_packets +
			  hw_stats->rx_illegal_byte_errors +
			  hw_stats->rx_error_bytes +
			  hw_stats->rx_fragment_errors +
			  hw_stats->rx_fcoe_crc_errors +
			  hw_stats->rx_fcoe_mbuf_allocation_errors;

	/* Tx Errors */
	stats->oerrors  = 0;
	return 0;
}

static int
txgbe_dev_stats_reset(struct rte_eth_dev *dev)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	struct txgbe_hw_stats *hw_stats = TXGBE_DEV_STATS(dev);

	/* HW registers are cleared on read */
	hw->offset_loaded = 0;
	txgbe_dev_stats_get(dev, NULL);
	hw->offset_loaded = 1;

	/* Reset software totals */
	memset(hw_stats, 0, sizeof(*hw_stats));

	return 0;
}

/* This function calculates the number of xstats based on the current config */
static unsigned
txgbe_xstats_calc_num(struct rte_eth_dev *dev)
{
	int nb_queues = max(dev->data->nb_rx_queues, dev->data->nb_tx_queues);
	return TXGBE_NB_HW_STATS +
	       TXGBE_NB_UP_STATS * TXGBE_MAX_UP +
	       TXGBE_NB_QP_STATS * nb_queues;
}

static inline int
txgbe_get_name_by_id(uint32_t id, char *name, uint32_t size)
{
	int nb, st;

	/* Extended stats from txgbe_hw_stats */
	if (id < TXGBE_NB_HW_STATS) {
		snprintf(name, size, "[hw]%s",
			rte_txgbe_stats_strings[id].name);
		return 0;
	}
	id -= TXGBE_NB_HW_STATS;

	/* Priority Stats */
	if (id < TXGBE_NB_UP_STATS * TXGBE_MAX_UP) {
		nb = id / TXGBE_NB_UP_STATS;
		st = id % TXGBE_NB_UP_STATS;
		snprintf(name, size, "[p%u]%s", nb,
			rte_txgbe_up_strings[st].name);
		return 0;
	}
	id -= TXGBE_NB_UP_STATS * TXGBE_MAX_UP;

	/* Queue Stats */
	if (id < TXGBE_NB_QP_STATS * TXGBE_MAX_QP) {
		nb = id / TXGBE_NB_QP_STATS;
		st = id % TXGBE_NB_QP_STATS;
		snprintf(name, size, "[q%u]%s", nb,
			rte_txgbe_qp_strings[st].name);
		return 0;
	}
	id -= TXGBE_NB_QP_STATS * TXGBE_MAX_QP;

	return -(int)(id + 1);
}

static inline int
txgbe_get_offset_by_id(uint32_t id, uint32_t *offset)
{
	int nb, st;

	/* Extended stats from txgbe_hw_stats */
	if (id < TXGBE_NB_HW_STATS) {
		*offset = rte_txgbe_stats_strings[id].offset;
		return 0;
	}
	id -= TXGBE_NB_HW_STATS;

	/* Priority Stats */
	if (id < TXGBE_NB_UP_STATS * TXGBE_MAX_UP) {
		nb = id / TXGBE_NB_UP_STATS;
		st = id % TXGBE_NB_UP_STATS;
		*offset = rte_txgbe_up_strings[st].offset +
			nb * (TXGBE_NB_UP_STATS * sizeof(uint64_t));
		return 0;
	}
	id -= TXGBE_NB_UP_STATS * TXGBE_MAX_UP;

	/* Queue Stats */
	if (id < TXGBE_NB_QP_STATS * TXGBE_MAX_QP) {
		nb = id / TXGBE_NB_QP_STATS;
		st = id % TXGBE_NB_QP_STATS;
		*offset = rte_txgbe_qp_strings[st].offset +
			nb * (TXGBE_NB_QP_STATS * sizeof(uint64_t));
		return 0;
	}
	id -= TXGBE_NB_QP_STATS * TXGBE_MAX_QP;

	return -(int)(id + 1);
}

static int txgbe_dev_xstats_get_names(struct rte_eth_dev *dev,
	struct rte_eth_xstat_name *xstats_names, unsigned int limit)
{
	unsigned int i, count;

	count = txgbe_xstats_calc_num(dev);
	if (xstats_names == NULL)
		return count;

	/* Note: limit >= cnt_stats checked upstream
	 * in rte_eth_xstats_names()
	 */
	limit = min(limit, count);

	/* Extended stats from txgbe_hw_stats */
	for (i = 0; i < limit; i++) {
		if (txgbe_get_name_by_id(i, xstats_names[i].name,
			sizeof(xstats_names[i].name))) {
			PMD_INIT_LOG(WARNING, "id value %d isn't valid", i);
			break;
		}
	}

	return i;
}

static int txgbe_dev_xstats_get_names_by_id(struct rte_eth_dev *dev,
	struct rte_eth_xstat_name *xstats_names,
	const uint64_t *ids,
	unsigned int limit)
{
	unsigned int i;

	if (ids == NULL)
		return txgbe_dev_xstats_get_names(dev, xstats_names, limit);

	for (i = 0; i < limit; i++) {
		if (txgbe_get_name_by_id(ids[i], xstats_names[i].name,
				sizeof(xstats_names[i].name))) {
			PMD_INIT_LOG(WARNING, "id value %d isn't valid", i);
			return -1;
		}
	}

	return i;
}

static int
txgbe_dev_xstats_get(struct rte_eth_dev *dev, struct rte_eth_xstat *xstats,
					 unsigned int limit)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	struct txgbe_hw_stats *hw_stats = TXGBE_DEV_STATS(dev);
	unsigned int i, count;

	txgbe_read_stats_registers(hw, hw_stats);

	/* If this is a reset xstats is NULL, and we have cleared the
	 * registers by reading them.
	 */
	count = txgbe_xstats_calc_num(dev);
	if (xstats == NULL)
		return count;

	limit = min(limit, txgbe_xstats_calc_num(dev));

	/* Extended stats from txgbe_hw_stats */
	for (i = 0; i < limit; i++) {
		uint32_t offset = 0;

		if (txgbe_get_offset_by_id(i, &offset)) {
			PMD_INIT_LOG(WARNING, "id value %d isn't valid", i);
			break;
		}
		xstats[i].value = *(uint64_t *)(((char *)hw_stats) + offset);
		xstats[i].id = i;
	}

	return i;
}

static int
txgbe_dev_xstats_get_(struct rte_eth_dev *dev, uint64_t *values,
					 unsigned int limit)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	struct txgbe_hw_stats *hw_stats = TXGBE_DEV_STATS(dev);
	unsigned int i, count;

	txgbe_read_stats_registers(hw, hw_stats);

	/* If this is a reset xstats is NULL, and we have cleared the
	 * registers by reading them.
	 */
	count = txgbe_xstats_calc_num(dev);
	if (values == NULL)
		return count;

	limit = min(limit, txgbe_xstats_calc_num(dev));

	/* Extended stats from txgbe_hw_stats */
	for (i = 0; i < limit; i++) {
		uint32_t offset;

		if (txgbe_get_offset_by_id(i, &offset)) {
			PMD_INIT_LOG(WARNING, "id value %d isn't valid", i);
			break;
		}
		values[i] = *(uint64_t *)(((char *)hw_stats) + offset);
	}

	return i;
}

static int
txgbe_dev_xstats_get_by_id(struct rte_eth_dev *dev, const uint64_t *ids,
		uint64_t *values, unsigned int limit)
{
	struct txgbe_hw_stats *hw_stats = TXGBE_DEV_STATS(dev);
	unsigned int i;

	if (ids == NULL)
		return txgbe_dev_xstats_get_(dev, values, limit);

	for (i = 0; i < limit; i++) {
		uint32_t offset;

		if (txgbe_get_offset_by_id(ids[i], &offset)) {
			PMD_INIT_LOG(WARNING, "id value %d isn't valid", i);
			break;
		}
		values[i] = *(uint64_t *)(((char *)hw_stats) + offset);
	}

	return i;
}

static int
txgbe_dev_xstats_reset(struct rte_eth_dev *dev)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	struct txgbe_hw_stats *hw_stats = TXGBE_DEV_STATS(dev);

	/* HW registers are cleared on read */
	hw->offset_loaded = 0;
	txgbe_read_stats_registers(hw, hw_stats);
	hw->offset_loaded = 1;

	/* Reset software totals */
	memset(hw_stats, 0, sizeof(*hw_stats));

	return 0;
}

static int
txgbe_dev_info_get(struct rte_eth_dev *dev, struct rte_eth_dev_info *dev_info)
{
	struct rte_pci_device *pci_dev = RTE_ETH_DEV_TO_PCI(dev);
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);

	dev_info->max_rx_queues = (uint16_t)hw->mac.max_rx_queues;
	dev_info->max_tx_queues = (uint16_t)hw->mac.max_tx_queues;
	dev_info->min_rx_bufsize = 1024;
	dev_info->max_rx_pktlen = 15872;
	dev_info->max_mac_addrs = hw->mac.num_rar_entries;
	dev_info->max_hash_mac_addrs = TXGBE_VMDQ_NUM_UC_MAC;
	dev_info->max_vfs = pci_dev->max_vfs;
	dev_info->max_vmdq_pools = ETH_64_POOLS;
	dev_info->vmdq_queue_num = dev_info->max_rx_queues;
	dev_info->rx_queue_offload_capa = txgbe_get_rx_queue_offloads(dev);
	dev_info->rx_offload_capa = (txgbe_get_rx_port_offloads(dev) |
				     dev_info->rx_queue_offload_capa);
	dev_info->tx_queue_offload_capa = txgbe_get_tx_queue_offloads(dev);
	dev_info->tx_offload_capa = txgbe_get_tx_port_offloads(dev);

	dev_info->default_rxconf = (struct rte_eth_rxconf) {
		.rx_thresh = {
			.pthresh = TXGBE_DEFAULT_RX_PTHRESH,
			.hthresh = TXGBE_DEFAULT_RX_HTHRESH,
			.wthresh = TXGBE_DEFAULT_RX_WTHRESH,
		},
		.rx_free_thresh = TXGBE_DEFAULT_RX_FREE_THRESH,
		.rx_drop_en = 0,
		.offloads = 0,
	};

	dev_info->default_txconf = (struct rte_eth_txconf) {
		.tx_thresh = {
			.pthresh = TXGBE_DEFAULT_TX_PTHRESH,
			.hthresh = TXGBE_DEFAULT_TX_HTHRESH,
			.wthresh = TXGBE_DEFAULT_TX_WTHRESH,
		},
		.tx_free_thresh = TXGBE_DEFAULT_TX_FREE_THRESH,
		.offloads = 0,
	};

	dev_info->rx_desc_lim = rx_desc_lim;
	dev_info->tx_desc_lim = tx_desc_lim;

	dev_info->hash_key_size = TXGBE_HKEY_MAX_INDEX * sizeof(uint32_t);
	dev_info->reta_size = ETH_RSS_RETA_SIZE_128;
	dev_info->flow_type_rss_offloads = TXGBE_RSS_OFFLOAD_ALL;

	dev_info->speed_capa = ETH_LINK_SPEED_1G | ETH_LINK_SPEED_10G;
	dev_info->speed_capa |= ETH_LINK_SPEED_100M;

	/* Driver-preferred Rx/Tx parameters */
	dev_info->default_rxportconf.burst_size = 32;
	dev_info->default_txportconf.burst_size = 32;
	dev_info->default_rxportconf.nb_queues = 1;
	dev_info->default_txportconf.nb_queues = 1;
	dev_info->default_rxportconf.ring_size = 256;
	dev_info->default_txportconf.ring_size = 256;

	return 0;
}

const uint32_t *
txgbe_dev_supported_ptypes_get(struct rte_eth_dev *dev)
{
	if (dev->rx_pkt_burst == txgbe_recv_pkts ||
	    dev->rx_pkt_burst == txgbe_recv_pkts_lro_single_alloc ||
	    dev->rx_pkt_burst == txgbe_recv_pkts_lro_bulk_alloc ||
	    dev->rx_pkt_burst == txgbe_recv_pkts_bulk_alloc)
		return txgbe_get_supported_ptypes();

	return NULL;
}

void
txgbe_dev_setup_link_alarm_handler(void *param)
{
	struct rte_eth_dev *dev = (struct rte_eth_dev *)param;
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	struct txgbe_interrupt *intr = TXGBE_DEV_INTR(dev);
	u32 speed;
	bool autoneg = false;

	speed = hw->phy.autoneg_advertised;
	if (!speed)
		hw->mac.get_link_capabilities(hw, &speed, &autoneg);

	hw->mac.setup_link(hw, speed, true);

	intr->flags &= ~TXGBE_FLAG_NEED_LINK_CONFIG;
}

/* return 0 means link status changed, -1 means not changed */
int
txgbe_dev_link_update_share(struct rte_eth_dev *dev,
			    int wait_to_complete)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	struct rte_eth_link link;
	u32 link_speed = TXGBE_LINK_SPEED_UNKNOWN;
	struct txgbe_interrupt *intr = TXGBE_DEV_INTR(dev);
	bool link_up;
	int err;
	int wait = 1;

	memset(&link, 0, sizeof(link));
	link.link_status = ETH_LINK_DOWN;
	link.link_speed = ETH_SPEED_NUM_NONE;
	link.link_duplex = ETH_LINK_HALF_DUPLEX;
	link.link_autoneg = ETH_LINK_AUTONEG;

	hw->mac.get_link_status = true;

	if (intr->flags & TXGBE_FLAG_NEED_LINK_CONFIG)
		return rte_eth_linkstatus_set(dev, &link);

	/* check if it needs to wait to complete, if lsc interrupt is enabled */
	if (wait_to_complete == 0 || dev->data->dev_conf.intr_conf.lsc != 0)
		wait = 0;

	err = hw->mac.check_link(hw, &link_speed, &link_up, wait);

	if (err != 0) {
		link.link_speed = ETH_SPEED_NUM_100M;
		link.link_duplex = ETH_LINK_FULL_DUPLEX;
		return rte_eth_linkstatus_set(dev, &link);
	}

	if (link_up == 0) {
		if (hw->phy.media_type == txgbe_media_type_fiber) {
			intr->flags |= TXGBE_FLAG_NEED_LINK_CONFIG;
			rte_eal_alarm_set(10,
				txgbe_dev_setup_link_alarm_handler, dev);
		}
		return rte_eth_linkstatus_set(dev, &link);
	}

	intr->flags &= ~TXGBE_FLAG_NEED_LINK_CONFIG;
	link.link_status = ETH_LINK_UP;
	link.link_duplex = ETH_LINK_FULL_DUPLEX;

	switch (link_speed) {
	default:
	case TXGBE_LINK_SPEED_UNKNOWN:
		link.link_duplex = ETH_LINK_FULL_DUPLEX;
		link.link_speed = ETH_SPEED_NUM_100M;
		break;

	case TXGBE_LINK_SPEED_100M_FULL:
		link.link_speed = ETH_SPEED_NUM_100M;
		break;

	case TXGBE_LINK_SPEED_1GB_FULL:
		link.link_speed = ETH_SPEED_NUM_1G;
		break;

	case TXGBE_LINK_SPEED_2_5GB_FULL:
		link.link_speed = ETH_SPEED_NUM_2_5G;
		break;

	case TXGBE_LINK_SPEED_5GB_FULL:
		link.link_speed = ETH_SPEED_NUM_5G;
		break;

	case TXGBE_LINK_SPEED_10GB_FULL:
		link.link_speed = ETH_SPEED_NUM_10G;
		break;
	}

	return rte_eth_linkstatus_set(dev, &link);
}

static int
txgbe_dev_link_update(struct rte_eth_dev *dev, int wait_to_complete)
{
	return txgbe_dev_link_update_share(dev, wait_to_complete);
}

static int
txgbe_dev_promiscuous_enable(struct rte_eth_dev *dev)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	uint32_t fctrl;

	fctrl = rd32(hw, TXGBE_PSRCTL);
	fctrl |= (TXGBE_PSRCTL_UCP | TXGBE_PSRCTL_MCP);
	wr32(hw, TXGBE_PSRCTL, fctrl);

	return 0;
}

static int
txgbe_dev_promiscuous_disable(struct rte_eth_dev *dev)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	uint32_t fctrl;

	fctrl = rd32(hw, TXGBE_PSRCTL);
	fctrl &= (~TXGBE_PSRCTL_UCP);
	if (dev->data->all_multicast == 1)
		fctrl |= TXGBE_PSRCTL_MCP;
	else
		fctrl &= (~TXGBE_PSRCTL_MCP);
	wr32(hw, TXGBE_PSRCTL, fctrl);

	return 0;
}

static int
txgbe_dev_allmulticast_enable(struct rte_eth_dev *dev)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	uint32_t fctrl;

	fctrl = rd32(hw, TXGBE_PSRCTL);
	fctrl |= TXGBE_PSRCTL_MCP;
	wr32(hw, TXGBE_PSRCTL, fctrl);

	return 0;
}

static int
txgbe_dev_allmulticast_disable(struct rte_eth_dev *dev)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	uint32_t fctrl;

	if (dev->data->promiscuous == 1)
		return 0; /* must remain in all_multicast mode */

	fctrl = rd32(hw, TXGBE_PSRCTL);
	fctrl &= (~TXGBE_PSRCTL_MCP);
	wr32(hw, TXGBE_PSRCTL, fctrl);

	return 0;
}

/**
 * It clears the interrupt causes and enables the interrupt.
 * It will be called once only during nic initialized.
 *
 * @param dev
 *  Pointer to struct rte_eth_dev.
 * @param on
 *  Enable or Disable.
 *
 * @return
 *  - On success, zero.
 *  - On failure, a negative value.
 */
static int
txgbe_dev_lsc_interrupt_setup(struct rte_eth_dev *dev, uint8_t on)
{
	struct txgbe_interrupt *intr = TXGBE_DEV_INTR(dev);

	txgbe_dev_link_status_print(dev);
	if (on)
		intr->mask_misc |= TXGBE_ICRMISC_LSC;
	else
		intr->mask_misc &= ~TXGBE_ICRMISC_LSC;

	return 0;
}

/**
 * It clears the interrupt causes and enables the interrupt.
 * It will be called once only during nic initialized.
 *
 * @param dev
 *  Pointer to struct rte_eth_dev.
 *
 * @return
 *  - On success, zero.
 *  - On failure, a negative value.
 */
static int
txgbe_dev_rxq_interrupt_setup(struct rte_eth_dev *dev)
{
	struct txgbe_interrupt *intr = TXGBE_DEV_INTR(dev);

	intr->mask[0] |= TXGBE_ICR_MASK;
	intr->mask[1] |= TXGBE_ICR_MASK;

	return 0;
}

/**
 * It clears the interrupt causes and enables the interrupt.
 * It will be called once only during nic initialized.
 *
 * @param dev
 *  Pointer to struct rte_eth_dev.
 *
 * @return
 *  - On success, zero.
 *  - On failure, a negative value.
 */
static int
txgbe_dev_macsec_interrupt_setup(struct rte_eth_dev *dev)
{
	struct txgbe_interrupt *intr = TXGBE_DEV_INTR(dev);

	intr->mask_misc |= TXGBE_ICRMISC_LNKSEC;

	return 0;
}

/*
 * It reads ICR and sets flag (TXGBE_ICRMISC_LSC) for the link_update.
 *
 * @param dev
 *  Pointer to struct rte_eth_dev.
 *
 * @return
 *  - On success, zero.
 *  - On failure, a negative value.
 */
static int
txgbe_dev_interrupt_get_status(struct rte_eth_dev *dev)
{
	uint32_t eicr;
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	struct txgbe_interrupt *intr = TXGBE_DEV_INTR(dev);

	/* clear all cause mask */
	txgbe_disable_intr(hw);

	/* read-on-clear nic registers here */
	eicr = ((u32 *)hw->isb_mem)[TXGBE_ISB_MISC];
	PMD_DRV_LOG(DEBUG, "eicr %x", eicr);

	intr->flags = 0;

	/* set flag for async link update */
	if (eicr & TXGBE_ICRMISC_LSC)
		intr->flags |= TXGBE_FLAG_NEED_LINK_UPDATE;

	if (eicr & TXGBE_ICRMISC_VFMBX)
		intr->flags |= TXGBE_FLAG_MAILBOX;

	if (eicr & TXGBE_ICRMISC_LNKSEC)
		intr->flags |= TXGBE_FLAG_MACSEC;

	if (eicr & TXGBE_ICRMISC_GPIO)
		intr->flags |= TXGBE_FLAG_PHY_INTERRUPT;

	return 0;
}

/**
 * It gets and then prints the link status.
 *
 * @param dev
 *  Pointer to struct rte_eth_dev.
 *
 * @return
 *  - On success, zero.
 *  - On failure, a negative value.
 */
static void
txgbe_dev_link_status_print(struct rte_eth_dev *dev)
{
	struct rte_pci_device *pci_dev = RTE_ETH_DEV_TO_PCI(dev);
	struct rte_eth_link link;

	rte_eth_linkstatus_get(dev, &link);

	if (link.link_status) {
		PMD_INIT_LOG(INFO, "Port %d: Link Up - speed %u Mbps - %s",
					(int)(dev->data->port_id),
					(unsigned int)link.link_speed,
			link.link_duplex == ETH_LINK_FULL_DUPLEX ?
					"full-duplex" : "half-duplex");
	} else {
		PMD_INIT_LOG(INFO, " Port %d: Link Down",
				(int)(dev->data->port_id));
	}
	PMD_INIT_LOG(DEBUG, "PCI Address: " PCI_PRI_FMT,
				pci_dev->addr.domain,
				pci_dev->addr.bus,
				pci_dev->addr.devid,
				pci_dev->addr.function);
}

/*
 * It executes link_update after knowing an interrupt occurred.
 *
 * @param dev
 *  Pointer to struct rte_eth_dev.
 *
 * @return
 *  - On success, zero.
 *  - On failure, a negative value.
 */
static int
txgbe_dev_interrupt_action(struct rte_eth_dev *dev,
			   struct rte_intr_handle *intr_handle)
{
	struct txgbe_interrupt *intr = TXGBE_DEV_INTR(dev);
	int64_t timeout;
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);

	PMD_DRV_LOG(DEBUG, "intr action type %d", intr->flags);

	if (intr->flags & TXGBE_FLAG_MAILBOX) {
		txgbe_pf_mbx_process(dev);
		intr->flags &= ~TXGBE_FLAG_MAILBOX;
	}

	if (intr->flags & TXGBE_FLAG_PHY_INTERRUPT) {
		hw->phy.handle_lasi(hw);
		intr->flags &= ~TXGBE_FLAG_PHY_INTERRUPT;
	}

	if (intr->flags & TXGBE_FLAG_NEED_LINK_UPDATE) {
		struct rte_eth_link link;

		/*get the link status before link update, for predicting later*/
		rte_eth_linkstatus_get(dev, &link);

		txgbe_dev_link_update(dev, 0);

		/* likely to up */
		if (!link.link_status)
			/* handle it 1 sec later, wait it being stable */
			timeout = TXGBE_LINK_UP_CHECK_TIMEOUT;
		/* likely to down */
		else
			/* handle it 4 sec later, wait it being stable */
			timeout = TXGBE_LINK_DOWN_CHECK_TIMEOUT;

		txgbe_dev_link_status_print(dev);
		if (rte_eal_alarm_set(timeout * 1000,
				      txgbe_dev_interrupt_delayed_handler,
				      (void *)dev) < 0) {
			PMD_DRV_LOG(ERR, "Error setting alarm");
		} else {
			/* remember original mask */
			intr->mask_misc_orig = intr->mask_misc;
			/* only disable lsc interrupt */
			intr->mask_misc &= ~TXGBE_ICRMISC_LSC;
		}
	}

	PMD_DRV_LOG(DEBUG, "enable intr immediately");
	txgbe_enable_intr(dev);
	rte_intr_enable(intr_handle);

	return 0;
}

/**
 * Interrupt handler which shall be registered for alarm callback for delayed
 * handling specific interrupt to wait for the stable nic state. As the
 * NIC interrupt state is not stable for txgbe after link is just down,
 * it needs to wait 4 seconds to get the stable status.
 *
 * @param handle
 *  Pointer to interrupt handle.
 * @param param
 *  The address of parameter (struct rte_eth_dev *) registered before.
 *
 * @return
 *  void
 */
static void
txgbe_dev_interrupt_delayed_handler(void *param)
{
	struct rte_eth_dev *dev = (struct rte_eth_dev *)param;
	struct rte_pci_device *pci_dev = RTE_ETH_DEV_TO_PCI(dev);
	struct rte_intr_handle *intr_handle = &pci_dev->intr_handle;
	struct txgbe_interrupt *intr = TXGBE_DEV_INTR(dev);
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	uint32_t eicr;

	txgbe_disable_intr(hw);

	eicr = ((u32 *)hw->isb_mem)[TXGBE_ISB_MISC];
	if (eicr & TXGBE_ICRMISC_VFMBX)
		txgbe_pf_mbx_process(dev);

	if (intr->flags & TXGBE_FLAG_PHY_INTERRUPT) {
		hw->phy.handle_lasi(hw);
		intr->flags &= ~TXGBE_FLAG_PHY_INTERRUPT;
	}

	if (intr->flags & TXGBE_FLAG_NEED_LINK_UPDATE) {
		txgbe_dev_link_update(dev, 0);
		intr->flags &= ~TXGBE_FLAG_NEED_LINK_UPDATE;
		txgbe_dev_link_status_print(dev);
		rte_eth_dev_callback_process(dev, RTE_ETH_EVENT_INTR_LSC,
					      NULL);
	}

	if (intr->flags & TXGBE_FLAG_MACSEC) {
		rte_eth_dev_callback_process(dev, RTE_ETH_EVENT_MACSEC,
					      NULL);
		intr->flags &= ~TXGBE_FLAG_MACSEC;
	}

	/* restore original mask */
	intr->mask_misc = intr->mask_misc_orig;
	intr->mask_misc_orig = 0;

	PMD_DRV_LOG(DEBUG, "enable intr in delayed handler S[%08x]", eicr);
	txgbe_enable_intr(dev);
	rte_intr_enable(intr_handle);
}

/**
 * Interrupt handler triggered by NIC  for handling
 * specific interrupt.
 *
 * @param handle
 *  Pointer to interrupt handle.
 * @param param
 *  The address of parameter (struct rte_eth_dev *) registered before.
 *
 * @return
 *  void
 */
static void
txgbe_dev_interrupt_handler(void *param)
{
	struct rte_eth_dev *dev = (struct rte_eth_dev *)param;

	txgbe_dev_interrupt_get_status(dev);
	txgbe_dev_interrupt_action(dev, dev->intr_handle);
}

static int
txgbe_flow_ctrl_get(struct rte_eth_dev *dev, struct rte_eth_fc_conf *fc_conf)
{
	struct txgbe_hw *hw;
	uint32_t mflcn_reg;
	uint32_t fccfg_reg;
	int rx_pause;
	int tx_pause;

	hw = TXGBE_DEV_HW(dev);

	fc_conf->pause_time = hw->fc.pause_time;
	fc_conf->high_water = hw->fc.high_water[0];
	fc_conf->low_water = hw->fc.low_water[0];
	fc_conf->send_xon = hw->fc.send_xon;
	fc_conf->autoneg = !hw->fc.disable_fc_autoneg;

	/*
	 * Return rx_pause status according to actual setting of
	 * RXFCCFG register.
	 */
	mflcn_reg = rd32(hw, TXGBE_RXFCCFG);
	if (mflcn_reg & (TXGBE_RXFCCFG_FC | TXGBE_RXFCCFG_PFC))
		rx_pause = 1;
	else
		rx_pause = 0;

	/*
	 * Return tx_pause status according to actual setting of
	 * TXFCCFG register.
	 */
	fccfg_reg = rd32(hw, TXGBE_TXFCCFG);
	if (fccfg_reg & (TXGBE_TXFCCFG_FC | TXGBE_TXFCCFG_PFC))
		tx_pause = 1;
	else
		tx_pause = 0;

	if (rx_pause && tx_pause)
		fc_conf->mode = RTE_FC_FULL;
	else if (rx_pause)
		fc_conf->mode = RTE_FC_RX_PAUSE;
	else if (tx_pause)
		fc_conf->mode = RTE_FC_TX_PAUSE;
	else
		fc_conf->mode = RTE_FC_NONE;

	return 0;
}

static int
txgbe_flow_ctrl_set(struct rte_eth_dev *dev, struct rte_eth_fc_conf *fc_conf)
{
	struct txgbe_hw *hw;
	int err;
	uint32_t rx_buf_size;
	uint32_t max_high_water;
	enum txgbe_fc_mode rte_fcmode_2_txgbe_fcmode[] = {
		txgbe_fc_none,
		txgbe_fc_rx_pause,
		txgbe_fc_tx_pause,
		txgbe_fc_full
	};

	PMD_INIT_FUNC_TRACE();

	hw = TXGBE_DEV_HW(dev);
	rx_buf_size = rd32(hw, TXGBE_PBRXSIZE(0));
	PMD_INIT_LOG(DEBUG, "Rx packet buffer size = 0x%x", rx_buf_size);

	/*
	 * At least reserve one Ethernet frame for watermark
	 * high_water/low_water in kilo bytes for txgbe
	 */
	max_high_water = (rx_buf_size - RTE_ETHER_MAX_LEN) >> 10;
	if (fc_conf->high_water > max_high_water ||
	    fc_conf->high_water < fc_conf->low_water) {
		PMD_INIT_LOG(ERR, "Invalid high/low water setup value in KB");
		PMD_INIT_LOG(ERR, "High_water must <= 0x%x", max_high_water);
		return -EINVAL;
	}

	hw->fc.requested_mode = rte_fcmode_2_txgbe_fcmode[fc_conf->mode];
	hw->fc.pause_time     = fc_conf->pause_time;
	hw->fc.high_water[0]  = fc_conf->high_water;
	hw->fc.low_water[0]   = fc_conf->low_water;
	hw->fc.send_xon       = fc_conf->send_xon;
	hw->fc.disable_fc_autoneg = !fc_conf->autoneg;

	err = txgbe_fc_enable(hw);

	/* Not negotiated is not an error case */
	if (err == 0 || err == TXGBE_ERR_FC_NOT_NEGOTIATED) {
		wr32m(hw, TXGBE_MACRXFLT, TXGBE_MACRXFLT_CTL_MASK,
		      (fc_conf->mac_ctrl_frame_fwd
		       ? TXGBE_MACRXFLT_CTL_NOPS : TXGBE_MACRXFLT_CTL_DROP));
		txgbe_flush(hw);

		return 0;
	}

	PMD_INIT_LOG(ERR, "txgbe_fc_enable = 0x%x", err);
	return -EIO;
}

static int
txgbe_priority_flow_ctrl_set(struct rte_eth_dev *dev,
		struct rte_eth_pfc_conf *pfc_conf)
{
	int err;
	uint32_t rx_buf_size;
	uint32_t max_high_water;
	uint8_t tc_num;
	uint8_t  map[TXGBE_DCB_UP_MAX] = { 0 };
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	struct txgbe_dcb_config *dcb_config = TXGBE_DEV_DCB_CONFIG(dev);

	enum txgbe_fc_mode rte_fcmode_2_txgbe_fcmode[] = {
		txgbe_fc_none,
		txgbe_fc_rx_pause,
		txgbe_fc_tx_pause,
		txgbe_fc_full
	};

	PMD_INIT_FUNC_TRACE();

	txgbe_dcb_unpack_map_cee(dcb_config, TXGBE_DCB_RX_CONFIG, map);
	tc_num = map[pfc_conf->priority];
	rx_buf_size = rd32(hw, TXGBE_PBRXSIZE(tc_num));
	PMD_INIT_LOG(DEBUG, "Rx packet buffer size = 0x%x", rx_buf_size);
	/*
	 * At least reserve one Ethernet frame for watermark
	 * high_water/low_water in kilo bytes for txgbe
	 */
	max_high_water = (rx_buf_size - RTE_ETHER_MAX_LEN) >> 10;
	if (pfc_conf->fc.high_water > max_high_water ||
	    pfc_conf->fc.high_water <= pfc_conf->fc.low_water) {
		PMD_INIT_LOG(ERR, "Invalid high/low water setup value in KB");
		PMD_INIT_LOG(ERR, "High_water must <= 0x%x", max_high_water);
		return -EINVAL;
	}

	hw->fc.requested_mode = rte_fcmode_2_txgbe_fcmode[pfc_conf->fc.mode];
	hw->fc.pause_time = pfc_conf->fc.pause_time;
	hw->fc.send_xon = pfc_conf->fc.send_xon;
	hw->fc.low_water[tc_num] =  pfc_conf->fc.low_water;
	hw->fc.high_water[tc_num] = pfc_conf->fc.high_water;

	err = txgbe_dcb_pfc_enable(hw, tc_num);

	/* Not negotiated is not an error case */
	if (err == 0 || err == TXGBE_ERR_FC_NOT_NEGOTIATED)
		return 0;

	PMD_INIT_LOG(ERR, "txgbe_dcb_pfc_enable = 0x%x", err);
	return -EIO;
}

int
txgbe_dev_rss_reta_update(struct rte_eth_dev *dev,
			  struct rte_eth_rss_reta_entry64 *reta_conf,
			  uint16_t reta_size)
{
	uint8_t i, j, mask;
	uint32_t reta;
	uint16_t idx, shift;
	struct txgbe_adapter *adapter = TXGBE_DEV_ADAPTER(dev);
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);

	PMD_INIT_FUNC_TRACE();

	if (!txgbe_rss_update_sp(hw->mac.type)) {
		PMD_DRV_LOG(ERR, "RSS reta update is not supported on this "
			"NIC.");
		return -ENOTSUP;
	}

	if (reta_size != ETH_RSS_RETA_SIZE_128) {
		PMD_DRV_LOG(ERR, "The size of hash lookup table configured "
			"(%d) doesn't match the number hardware can supported "
			"(%d)", reta_size, ETH_RSS_RETA_SIZE_128);
		return -EINVAL;
	}

	for (i = 0; i < reta_size; i += 4) {
		idx = i / RTE_RETA_GROUP_SIZE;
		shift = i % RTE_RETA_GROUP_SIZE;
		mask = (uint8_t)RS64(reta_conf[idx].mask, shift, 0xF);
		if (!mask)
			continue;

		reta = rd32a(hw, TXGBE_REG_RSSTBL, i >> 2);
		for (j = 0; j < 4; j++) {
			if (RS8(mask, j, 0x1)) {
				reta  &= ~(MS32(8 * j, 0xFF));
				reta |= LS32(reta_conf[idx].reta[shift + j],
						8 * j, 0xFF);
			}
		}
		wr32a(hw, TXGBE_REG_RSSTBL, i >> 2, reta);
	}
	adapter->rss_reta_updated = 1;

	return 0;
}

int
txgbe_dev_rss_reta_query(struct rte_eth_dev *dev,
			 struct rte_eth_rss_reta_entry64 *reta_conf,
			 uint16_t reta_size)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	uint8_t i, j, mask;
	uint32_t reta;
	uint16_t idx, shift;

	PMD_INIT_FUNC_TRACE();

	if (reta_size != ETH_RSS_RETA_SIZE_128) {
		PMD_DRV_LOG(ERR, "The size of hash lookup table configured "
			"(%d) doesn't match the number hardware can supported "
			"(%d)", reta_size, ETH_RSS_RETA_SIZE_128);
		return -EINVAL;
	}

	for (i = 0; i < reta_size; i += 4) {
		idx = i / RTE_RETA_GROUP_SIZE;
		shift = i % RTE_RETA_GROUP_SIZE;
		mask = (uint8_t)RS64(reta_conf[idx].mask, shift, 0xF);
		if (!mask)
			continue;

		reta = rd32a(hw, TXGBE_REG_RSSTBL, i >> 2);
		for (j = 0; j < 4; j++) {
			if (RS8(mask, j, 0x1))
				reta_conf[idx].reta[shift + j] =
					(uint16_t)RS32(reta, 8 * j, 0xFF);
		}
	}

	return 0;
}

static int
txgbe_add_rar(struct rte_eth_dev *dev, struct rte_ether_addr *mac_addr,
				uint32_t index, uint32_t pool)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	uint32_t enable_addr = 1;

	return txgbe_set_rar(hw, index, mac_addr->addr_bytes,
			     pool, enable_addr);
}

static void
txgbe_remove_rar(struct rte_eth_dev *dev, uint32_t index)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);

	txgbe_clear_rar(hw, index);
}

static int
txgbe_set_default_mac_addr(struct rte_eth_dev *dev, struct rte_ether_addr *addr)
{
	struct rte_pci_device *pci_dev = RTE_ETH_DEV_TO_PCI(dev);

	txgbe_remove_rar(dev, 0);
	txgbe_add_rar(dev, addr, 0, pci_dev->max_vfs);

	return 0;
}

static int
txgbe_dev_mtu_set(struct rte_eth_dev *dev, uint16_t mtu)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	struct rte_eth_dev_info dev_info;
	uint32_t frame_size = mtu + RTE_ETHER_HDR_LEN + RTE_ETHER_CRC_LEN;
	struct rte_eth_dev_data *dev_data = dev->data;
	int ret;

	ret = txgbe_dev_info_get(dev, &dev_info);
	if (ret != 0)
		return ret;

	/* check that mtu is within the allowed range */
	if (mtu < RTE_ETHER_MIN_MTU || frame_size > dev_info.max_rx_pktlen)
		return -EINVAL;

	/* If device is started, refuse mtu that requires the support of
	 * scattered packets when this feature has not been enabled before.
	 */
	if (dev_data->dev_started && !dev_data->scattered_rx &&
	    (frame_size + 2 * TXGBE_VLAN_TAG_SIZE >
	     dev->data->min_rx_buf_size - RTE_PKTMBUF_HEADROOM)) {
		PMD_INIT_LOG(ERR, "Stop port first.");
		return -EINVAL;
	}

	/* update max frame size */
	dev->data->dev_conf.rxmode.max_rx_pkt_len = frame_size;

	if (hw->mode)
		wr32m(hw, TXGBE_FRMSZ, TXGBE_FRMSZ_MAX_MASK,
			TXGBE_FRAME_SIZE_MAX);
	else
		wr32m(hw, TXGBE_FRMSZ, TXGBE_FRMSZ_MAX_MASK,
			TXGBE_FRMSZ_MAX(frame_size));

	return 0;
}

static uint32_t
txgbe_uta_vector(struct txgbe_hw *hw, struct rte_ether_addr *uc_addr)
{
	uint32_t vector = 0;

	switch (hw->mac.mc_filter_type) {
	case 0:   /* use bits [47:36] of the address */
		vector = ((uc_addr->addr_bytes[4] >> 4) |
			(((uint16_t)uc_addr->addr_bytes[5]) << 4));
		break;
	case 1:   /* use bits [46:35] of the address */
		vector = ((uc_addr->addr_bytes[4] >> 3) |
			(((uint16_t)uc_addr->addr_bytes[5]) << 5));
		break;
	case 2:   /* use bits [45:34] of the address */
		vector = ((uc_addr->addr_bytes[4] >> 2) |
			(((uint16_t)uc_addr->addr_bytes[5]) << 6));
		break;
	case 3:   /* use bits [43:32] of the address */
		vector = ((uc_addr->addr_bytes[4]) |
			(((uint16_t)uc_addr->addr_bytes[5]) << 8));
		break;
	default:  /* Invalid mc_filter_type */
		break;
	}

	/* vector can only be 12-bits or boundary will be exceeded */
	vector &= 0xFFF;
	return vector;
}

static int
txgbe_uc_hash_table_set(struct rte_eth_dev *dev,
			struct rte_ether_addr *mac_addr, uint8_t on)
{
	uint32_t vector;
	uint32_t uta_idx;
	uint32_t reg_val;
	uint32_t uta_mask;
	uint32_t psrctl;

	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	struct txgbe_uta_info *uta_info = TXGBE_DEV_UTA_INFO(dev);

	/* The UTA table only exists on pf hardware */
	if (hw->mac.type < txgbe_mac_raptor)
		return -ENOTSUP;

	vector = txgbe_uta_vector(hw, mac_addr);
	uta_idx = (vector >> 5) & 0x7F;
	uta_mask = 0x1UL << (vector & 0x1F);

	if (!!on == !!(uta_info->uta_shadow[uta_idx] & uta_mask))
		return 0;

	reg_val = rd32(hw, TXGBE_UCADDRTBL(uta_idx));
	if (on) {
		uta_info->uta_in_use++;
		reg_val |= uta_mask;
		uta_info->uta_shadow[uta_idx] |= uta_mask;
	} else {
		uta_info->uta_in_use--;
		reg_val &= ~uta_mask;
		uta_info->uta_shadow[uta_idx] &= ~uta_mask;
	}

	wr32(hw, TXGBE_UCADDRTBL(uta_idx), reg_val);

	psrctl = rd32(hw, TXGBE_PSRCTL);
	if (uta_info->uta_in_use > 0)
		psrctl |= TXGBE_PSRCTL_UCHFENA;
	else
		psrctl &= ~TXGBE_PSRCTL_UCHFENA;

	psrctl &= ~TXGBE_PSRCTL_ADHF12_MASK;
	psrctl |= TXGBE_PSRCTL_ADHF12(hw->mac.mc_filter_type);
	wr32(hw, TXGBE_PSRCTL, psrctl);

	return 0;
}

static int
txgbe_uc_all_hash_table_set(struct rte_eth_dev *dev, uint8_t on)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	struct txgbe_uta_info *uta_info = TXGBE_DEV_UTA_INFO(dev);
	uint32_t psrctl;
	int i;

	/* The UTA table only exists on pf hardware */
	if (hw->mac.type < txgbe_mac_raptor)
		return -ENOTSUP;

	if (on) {
		for (i = 0; i < ETH_VMDQ_NUM_UC_HASH_ARRAY; i++) {
			uta_info->uta_shadow[i] = ~0;
			wr32(hw, TXGBE_UCADDRTBL(i), ~0);
		}
	} else {
		for (i = 0; i < ETH_VMDQ_NUM_UC_HASH_ARRAY; i++) {
			uta_info->uta_shadow[i] = 0;
			wr32(hw, TXGBE_UCADDRTBL(i), 0);
		}
	}

	psrctl = rd32(hw, TXGBE_PSRCTL);
	if (on)
		psrctl |= TXGBE_PSRCTL_UCHFENA;
	else
		psrctl &= ~TXGBE_PSRCTL_UCHFENA;

	psrctl &= ~TXGBE_PSRCTL_ADHF12_MASK;
	psrctl |= TXGBE_PSRCTL_ADHF12(hw->mac.mc_filter_type);
	wr32(hw, TXGBE_PSRCTL, psrctl);

	return 0;
}

uint32_t
txgbe_convert_vm_rx_mask_to_val(uint16_t rx_mask, uint32_t orig_val)
{
	uint32_t new_val = orig_val;

	if (rx_mask & ETH_VMDQ_ACCEPT_UNTAG)
		new_val |= TXGBE_POOLETHCTL_UTA;
	if (rx_mask & ETH_VMDQ_ACCEPT_HASH_MC)
		new_val |= TXGBE_POOLETHCTL_MCHA;
	if (rx_mask & ETH_VMDQ_ACCEPT_HASH_UC)
		new_val |= TXGBE_POOLETHCTL_UCHA;
	if (rx_mask & ETH_VMDQ_ACCEPT_BROADCAST)
		new_val |= TXGBE_POOLETHCTL_BCA;
	if (rx_mask & ETH_VMDQ_ACCEPT_MULTICAST)
		new_val |= TXGBE_POOLETHCTL_MCP;

	return new_val;
}

static int
txgbe_dev_rx_queue_intr_enable(struct rte_eth_dev *dev, uint16_t queue_id)
{
	struct rte_pci_device *pci_dev = RTE_ETH_DEV_TO_PCI(dev);
	struct rte_intr_handle *intr_handle = &pci_dev->intr_handle;
	uint32_t mask;
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);

	if (queue_id < 32) {
		mask = rd32(hw, TXGBE_IMS(0));
		mask &= (1 << queue_id);
		wr32(hw, TXGBE_IMS(0), mask);
	} else if (queue_id < 64) {
		mask = rd32(hw, TXGBE_IMS(1));
		mask &= (1 << (queue_id - 32));
		wr32(hw, TXGBE_IMS(1), mask);
	}
	rte_intr_enable(intr_handle);

	return 0;
}

static int
txgbe_dev_rx_queue_intr_disable(struct rte_eth_dev *dev, uint16_t queue_id)
{
	uint32_t mask;
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);

	if (queue_id < 32) {
		mask = rd32(hw, TXGBE_IMS(0));
		mask &= ~(1 << queue_id);
		wr32(hw, TXGBE_IMS(0), mask);
	} else if (queue_id < 64) {
		mask = rd32(hw, TXGBE_IMS(1));
		mask &= ~(1 << (queue_id - 32));
		wr32(hw, TXGBE_IMS(1), mask);
	}

	return 0;
}

/**
 * set the IVAR registers, mapping interrupt causes to vectors
 * @param hw
 *  pointer to txgbe_hw struct
 * @direction
 *  0 for Rx, 1 for Tx, -1 for other causes
 * @queue
 *  queue to map the corresponding interrupt to
 * @msix_vector
 *  the vector to map to the corresponding queue
 */
void
txgbe_set_ivar_map(struct txgbe_hw *hw, int8_t direction,
		   uint8_t queue, uint8_t msix_vector)
{
	uint32_t tmp, idx;

	if (direction == -1) {
		/* other causes */
		msix_vector |= TXGBE_IVARMISC_VLD;
		idx = 0;
		tmp = rd32(hw, TXGBE_IVARMISC);
		tmp &= ~(0xFF << idx);
		tmp |= (msix_vector << idx);
		wr32(hw, TXGBE_IVARMISC, tmp);
	} else {
		/* rx or tx causes */
		/* Workround for ICR lost */
		idx = ((16 * (queue & 1)) + (8 * direction));
		tmp = rd32(hw, TXGBE_IVAR(queue >> 1));
		tmp &= ~(0xFF << idx);
		tmp |= (msix_vector << idx);
		wr32(hw, TXGBE_IVAR(queue >> 1), tmp);
	}
}

/**
 * Sets up the hardware to properly generate MSI-X interrupts
 * @hw
 *  board private structure
 */
static void
txgbe_configure_msix(struct rte_eth_dev *dev)
{
	struct rte_pci_device *pci_dev = RTE_ETH_DEV_TO_PCI(dev);
	struct rte_intr_handle *intr_handle = &pci_dev->intr_handle;
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	uint32_t queue_id, base = TXGBE_MISC_VEC_ID;
	uint32_t vec = TXGBE_MISC_VEC_ID;
	uint32_t gpie;

	/* won't configure msix register if no mapping is done
	 * between intr vector and event fd
	 * but if misx has been enabled already, need to configure
	 * auto clean, auto mask and throttling.
	 */
	gpie = rd32(hw, TXGBE_GPIE);
	if (!rte_intr_dp_is_en(intr_handle) &&
	    !(gpie & TXGBE_GPIE_MSIX))
		return;

	if (rte_intr_allow_others(intr_handle)) {
		base = TXGBE_RX_VEC_START;
		vec = base;
	}

	/* setup GPIE for MSI-x mode */
	gpie = rd32(hw, TXGBE_GPIE);
	gpie |= TXGBE_GPIE_MSIX;
	wr32(hw, TXGBE_GPIE, gpie);

	/* Populate the IVAR table and set the ITR values to the
	 * corresponding register.
	 */
	if (rte_intr_dp_is_en(intr_handle)) {
		for (queue_id = 0; queue_id < dev->data->nb_rx_queues;
			queue_id++) {
			/* by default, 1:1 mapping */
			txgbe_set_ivar_map(hw, 0, queue_id, vec);
			intr_handle->intr_vec[queue_id] = vec;
			if (vec < base + intr_handle->nb_efd - 1)
				vec++;
		}

		txgbe_set_ivar_map(hw, -1, 1, TXGBE_MISC_VEC_ID);
	}
	wr32(hw, TXGBE_ITR(TXGBE_MISC_VEC_ID),
			TXGBE_ITR_IVAL_10G(TXGBE_QUEUE_ITR_INTERVAL_DEFAULT)
			| TXGBE_ITR_WRDSA);
}

int
txgbe_set_queue_rate_limit(struct rte_eth_dev *dev,
			   uint16_t queue_idx, uint16_t tx_rate)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	uint32_t bcnrc_val;

	if (queue_idx >= hw->mac.max_tx_queues)
		return -EINVAL;

	if (tx_rate != 0) {
		bcnrc_val = TXGBE_ARBTXRATE_MAX(tx_rate);
		bcnrc_val |= TXGBE_ARBTXRATE_MIN(tx_rate / 2);
	} else {
		bcnrc_val = 0;
	}

	/*
	 * Set global transmit compensation time to the MMW_SIZE in ARBTXMMW
	 * register. MMW_SIZE=0x014 if 9728-byte jumbo is supported.
	 */
	wr32(hw, TXGBE_ARBTXMMW, 0x14);

	/* Set ARBTXRATE of queue X */
	wr32(hw, TXGBE_ARBPOOLIDX, queue_idx);
	wr32(hw, TXGBE_ARBTXRATE, bcnrc_val);
	txgbe_flush(hw);

	return 0;
}

static u8 *
txgbe_dev_addr_list_itr(__rte_unused struct txgbe_hw *hw,
			u8 **mc_addr_ptr, u32 *vmdq)
{
	u8 *mc_addr;

	*vmdq = 0;
	mc_addr = *mc_addr_ptr;
	*mc_addr_ptr = (mc_addr + sizeof(struct rte_ether_addr));
	return mc_addr;
}

int
txgbe_dev_set_mc_addr_list(struct rte_eth_dev *dev,
			  struct rte_ether_addr *mc_addr_set,
			  uint32_t nb_mc_addr)
{
	struct txgbe_hw *hw;
	u8 *mc_addr_list;

	hw = TXGBE_DEV_HW(dev);
	mc_addr_list = (u8 *)mc_addr_set;
	return txgbe_update_mc_addr_list(hw, mc_addr_list, nb_mc_addr,
					 txgbe_dev_addr_list_itr, TRUE);
}

bool
txgbe_rss_update_sp(enum txgbe_mac_type mac_type)
{
	switch (mac_type) {
	case txgbe_mac_raptor:
		return 1;
	default:
		return 0;
	}
}

static const struct eth_dev_ops txgbe_eth_dev_ops = {
	.dev_configure              = txgbe_dev_configure,
	.dev_infos_get              = txgbe_dev_info_get,
	.dev_start                  = txgbe_dev_start,
	.dev_stop                   = txgbe_dev_stop,
	.dev_set_link_up            = txgbe_dev_set_link_up,
	.dev_set_link_down          = txgbe_dev_set_link_down,
	.dev_close                  = txgbe_dev_close,
	.dev_reset                  = txgbe_dev_reset,
	.promiscuous_enable         = txgbe_dev_promiscuous_enable,
	.promiscuous_disable        = txgbe_dev_promiscuous_disable,
	.allmulticast_enable        = txgbe_dev_allmulticast_enable,
	.allmulticast_disable       = txgbe_dev_allmulticast_disable,
	.link_update                = txgbe_dev_link_update,
	.stats_get                  = txgbe_dev_stats_get,
	.xstats_get                 = txgbe_dev_xstats_get,
	.xstats_get_by_id           = txgbe_dev_xstats_get_by_id,
	.stats_reset                = txgbe_dev_stats_reset,
	.xstats_reset               = txgbe_dev_xstats_reset,
	.xstats_get_names           = txgbe_dev_xstats_get_names,
	.xstats_get_names_by_id     = txgbe_dev_xstats_get_names_by_id,
	.queue_stats_mapping_set    = txgbe_dev_queue_stats_mapping_set,
	.dev_supported_ptypes_get   = txgbe_dev_supported_ptypes_get,
	.mtu_set                    = txgbe_dev_mtu_set,
	.vlan_filter_set            = txgbe_vlan_filter_set,
	.vlan_tpid_set              = txgbe_vlan_tpid_set,
	.vlan_offload_set           = txgbe_vlan_offload_set,
	.vlan_strip_queue_set       = txgbe_vlan_strip_queue_set,
	.rx_queue_start	            = txgbe_dev_rx_queue_start,
	.rx_queue_stop              = txgbe_dev_rx_queue_stop,
	.tx_queue_start	            = txgbe_dev_tx_queue_start,
	.tx_queue_stop              = txgbe_dev_tx_queue_stop,
	.rx_queue_setup             = txgbe_dev_rx_queue_setup,
	.rx_queue_intr_enable       = txgbe_dev_rx_queue_intr_enable,
	.rx_queue_intr_disable      = txgbe_dev_rx_queue_intr_disable,
	.rx_queue_release           = txgbe_dev_rx_queue_release,
	.tx_queue_setup             = txgbe_dev_tx_queue_setup,
	.tx_queue_release           = txgbe_dev_tx_queue_release,
	.flow_ctrl_get              = txgbe_flow_ctrl_get,
	.flow_ctrl_set              = txgbe_flow_ctrl_set,
	.priority_flow_ctrl_set     = txgbe_priority_flow_ctrl_set,
	.mac_addr_add               = txgbe_add_rar,
	.mac_addr_remove            = txgbe_remove_rar,
	.mac_addr_set               = txgbe_set_default_mac_addr,
	.uc_hash_table_set          = txgbe_uc_hash_table_set,
	.uc_all_hash_table_set      = txgbe_uc_all_hash_table_set,
	.set_queue_rate_limit       = txgbe_set_queue_rate_limit,
	.reta_update                = txgbe_dev_rss_reta_update,
	.reta_query                 = txgbe_dev_rss_reta_query,
	.rss_hash_update            = txgbe_dev_rss_hash_update,
	.rss_hash_conf_get          = txgbe_dev_rss_hash_conf_get,
	.set_mc_addr_list           = txgbe_dev_set_mc_addr_list,
	.rxq_info_get               = txgbe_rxq_info_get,
	.txq_info_get               = txgbe_txq_info_get,
};

RTE_PMD_REGISTER_PCI(net_txgbe, rte_txgbe_pmd);
RTE_PMD_REGISTER_PCI_TABLE(net_txgbe, pci_id_txgbe_map);
RTE_PMD_REGISTER_KMOD_DEP(net_txgbe, "* igb_uio | uio_pci_generic | vfio-pci");

RTE_LOG_REGISTER(txgbe_logtype_init, pmd.net.txgbe.init, NOTICE);
RTE_LOG_REGISTER(txgbe_logtype_driver, pmd.net.txgbe.driver, NOTICE);

#ifdef RTE_LIBRTE_TXGBE_DEBUG_RX
	RTE_LOG_REGISTER(txgbe_logtype_rx, pmd.net.txgbe.rx, DEBUG);
#endif
#ifdef RTE_LIBRTE_TXGBE_DEBUG_TX
	RTE_LOG_REGISTER(txgbe_logtype_tx, pmd.net.txgbe.tx, DEBUG);
#endif

#ifdef RTE_LIBRTE_TXGBE_DEBUG_TX_FREE
	RTE_LOG_REGISTER(txgbe_logtype_tx_free, pmd.net.txgbe.tx_free, DEBUG);
#endif

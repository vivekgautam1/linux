// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Linaro Ltd
 * Author: Georgi Djakov <georgi.djakov@linaro.org>
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/interconnect-provider.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "smd-rpm.h"

#define RPM_MASTER_FIELD_BW	0x00007762
#define RPM_BUS_MASTER_REQ      0x73616d62
#define RPM_BUS_SLAVE_REQ       0x766c7362

#define to_qcom_provider(_provider) \
	container_of(_provider, struct qcom_icc_provider, provider)

#define DEFINE_QNODE(_name, _id, _port, _agg_ports, _buswidth,		\
			_qos_mode, _ap_owned, _mas_rpm_id, _slv_rpm_id, \
			_numlinks, ...)					\
		static struct qcom_icc_node _name = {			\
		.id = _id,						\
		.name = #_name,						\
		.port = _port,						\
		.agg_ports = _agg_ports,				\
		.buswidth = _buswidth,					\
		.qos_mode = _qos_mode,					\
		.ap_owned = _ap_owned,					\
		.mas_rpm_id = _mas_rpm_id,				\
		.slv_rpm_id = _slv_rpm_id,				\
		.num_links = _numlinks,					\
		.links = { __VA_ARGS__ },				\
	};

enum qcom_qos_mode {
	QCOM_QOS_MODE_BYPASS = 0,
	QCOM_QOS_MODE_FIXED,
	QCOM_QOS_MODE_MAX,
};

struct qcom_icc_provider {
	struct icc_provider	provider;
	void __iomem		*base;
	struct clk		*bus_clk;
	struct clk		*bus_a_clk;
	u32			base_offset;
	u32			qos_offset;
};

#define MSM8996_MAX_LINKS       38

/**
 * struct qcom_icc_node - Qualcomm specific interconnect nodes
 * @name: the node name used in debugfs
 * @links: an array of nodes where we can go next while traversing
 * @id: a unique node identifier
 * @num_links: the total number of @links
 * @port: the offset index into the masters QoS register space
 * @agg_ports: the number of aggregation ports on the bus
 * @buswidth: width of the interconnect between a node and the bus
 * @ap_owned: the AP CPU does the writing to QoS registers
 * @rpm: reference to the RPM SMD driver
 * @qos_mode: QoS mode for ap_owned resources
 * @mas_rpm_id:	RPM id for devices that are bus masters
 * @slv_rpm_id:	RPM id for devices that are bus slaves
 * @rate: current bus clock rate in Hz
 */
struct qcom_icc_node {
	unsigned char *name;
	u16 links[MSM8996_MAX_LINKS];
	u16 id;
	u16 num_links;
	u16 port;
	u16 agg_ports; /* The number of aggregation ports on the bus */
	u16 buswidth; /* width of the interconnect between a node and the bus */
	bool ap_owned; /* the AP CPU does the writing to QoS registers */
	struct qcom_smd_rpm *rpm; /* reference to the RPM driver */
	enum qcom_qos_mode qos_mode;
	int mas_rpm_id;
	int slv_rpm_id;
	u64 rate;
};

struct qcom_icc_desc {
	struct qcom_icc_node **nodes;
	size_t num_nodes;
};

DEFINE_QNODE(mas_pcie_0, 45, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, 65, -1, 1, 10061);
DEFINE_QNODE(mas_pcie_1, 100, 1, 1, 8, QCOM_QOS_MODE_FIXED, 1, 66, -1, 1, 10061);
DEFINE_QNODE(mas_pcie_2, 108, 2, 1, 8, QCOM_QOS_MODE_FIXED, 1, 119, -1, 1, 10061);
DEFINE_QNODE(mas_cnoc_a1noc, 10059,	0, 1, 8, QCOM_QOS_MODE_FIXED, 1, 116, -1, 1, 10062);
DEFINE_QNODE(mas_crypto_c0, 55,	0, 1, 8, QCOM_QOS_MODE_FIXED, 1, 23, -1, 1, 10062);
DEFINE_QNODE(mas_pnoc_a1noc, 10057,	1, 1, 8, QCOM_QOS_MODE_FIXED, 0, 117, -1, 1, 10062);
DEFINE_QNODE(mas_usb3, 61, 3, 1, 8, QCOM_QOS_MODE_FIXED, 1, 32, -1, 1, 10065);
DEFINE_QNODE(mas_ipa, 90, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, 59, -1, 1, 10065);
DEFINE_QNODE(mas_ufs, 95, 2, 1, 8, QCOM_QOS_MODE_FIXED, 1, 68, -1, 1, 10065);
DEFINE_QNODE(mas_apps_proc, 1, 0, 2, 8, QCOM_QOS_MODE_FIXED, 1, 0, -1, 3, 10056, 512, 10017);
DEFINE_QNODE(mas_oxili, 26, 1, 2, 8, QCOM_QOS_MODE_BYPASS, 1, 6, -1, 4, 10056, 680, 512, 10017);
DEFINE_QNODE(mas_mnoc_bimc, 10027, 2, 2, 8, QCOM_QOS_MODE_BYPASS, 1, 2, -1, 4, 10056, 680, 512, 10017);
DEFINE_QNODE(mas_snoc_bimc, 10031, 0, 2, 8, QCOM_QOS_MODE_BYPASS, 0, 3, -1, 2, 680, 512);
DEFINE_QNODE(mas_snoc_cnoc, 10035, 0, 1, 8, QCOM_QOS_MODE_BYPASS, 0, 52, -1, 37, 620, 716, 693, 707, 628, 631, 667, 624, 536, 691, 645, 629, 681, 715, 618, 685, 690, 635, 688, 686, 650, 625, 668, 642, 638, 689, 692, 684, 640, 683, 632, 627, 687, 697, 623, 694, 682);
DEFINE_QNODE(mas_qdss_dap, 76, 0, 1, 8, QCOM_QOS_MODE_BYPASS, 1, 49, -1, 38, 683, 716, 693, 707, 628, 667, 624, 536, 691, 645, 629, 681, 715, 620, 618, 685, 690, 635, 688, 686, 650, 625, 10034, 668, 642, 638, 689, 692, 684, 640, 631, 632, 627, 687, 697, 623, 694, 682);
DEFINE_QNODE(mas_cnoc_mnoc_mmss_cfg, 102, 0, 1, 8, QCOM_QOS_MODE_BYPASS, 1, 4, -1, 21, 695, 699, 599, 709, 596, 706, 594, 701, 598, 700, 696, 589, 590, 592, 704, 698, 705, 708, 702, 703, 601);
DEFINE_QNODE(mas_cnoc_mnoc_cfg, 103, 0, 1, 8, QCOM_QOS_MODE_BYPASS, 1, 5, -1, 1, 603);
DEFINE_QNODE(mas_cpp, 106, 5, 1, 32, QCOM_QOS_MODE_BYPASS, 1, 115, -1, 1, 10028);
DEFINE_QNODE(mas_jpeg, 62, 7, 1, 32, QCOM_QOS_MODE_BYPASS, 1, 7, -1, 1, 10028);
DEFINE_QNODE(mas_mdp_p0, 22, 1, 1, 32, QCOM_QOS_MODE_BYPASS, 1, 8, -1, 1, 10028);
DEFINE_QNODE(mas_mdp_p1, 23, 2, 1, 32, QCOM_QOS_MODE_BYPASS, 1, 61, -1, 1, 10028);
DEFINE_QNODE(mas_rotator, 25, 0, 1, 32, QCOM_QOS_MODE_BYPASS, 1, 120, -1, 1, 10028);
DEFINE_QNODE(mas_venus, 63, 3, 2, 32, QCOM_QOS_MODE_BYPASS, 1, 9, -1, 1, 10028);
DEFINE_QNODE(mas_vfe, 29, 6, 1, 32, QCOM_QOS_MODE_BYPASS, 1, 11, -1, 1, 10028);
DEFINE_QNODE(mas_snoc_vmem, 40,	0, 1, 32, QCOM_QOS_MODE_BYPASS, 1, 114, -1, 1, 708);
DEFINE_QNODE(mas_venus_vmem, 68, 0, 1, 32, QCOM_QOS_MODE_BYPASS, 1, 121, -1, 1, 708);
DEFINE_QNODE(mas_snoc_pnoc, 10041, 0, 1, 8, QCOM_QOS_MODE_BYPASS, 0, 44, -1, 9, 613, 611, 614, 606, 608, 609, 575, 615, 711);
DEFINE_QNODE(mas_sdcc_1, 78, 0, 1, 8, QCOM_QOS_MODE_BYPASS, 0, 33, -1, 1, 10058);
DEFINE_QNODE(mas_sdcc_2, 81, 0, 1, 8, QCOM_QOS_MODE_BYPASS, 0, 35, -1, 1, 10058);
DEFINE_QNODE(mas_sdcc_4, 80, 0, 1, 8, QCOM_QOS_MODE_BYPASS, 0, 36, -1, 1, 10058);
DEFINE_QNODE(mas_usb_hs, 87, 0, 1, 8, QCOM_QOS_MODE_BYPASS, 0, 42, -1, 1, 10058);
DEFINE_QNODE(mas_blsp_1, 86, 0, 1, 4, QCOM_QOS_MODE_BYPASS, 0, 41, -1, 1, 10058);
DEFINE_QNODE(mas_blsp_2, 84, 0, 1, 4, QCOM_QOS_MODE_BYPASS, 0, 39, -1, 1, 10058);
DEFINE_QNODE(mas_tsif, 82, 0, 1, 4, QCOM_QOS_MODE_BYPASS, 0, 37, -1, 1, 10058);
DEFINE_QNODE(mas_hmss, 43, 4, 1, 8, QCOM_QOS_MODE_FIXED, 1, 118, -1, 3, 712, 585, 10032);
DEFINE_QNODE(mas_qdss_bam, 53, 2, 1, 16, QCOM_QOS_MODE_FIXED, 1, 19, -1, 5, 712, 583, 585, 10032, 10042);
DEFINE_QNODE(mas_snoc_cfg, 54, 0, 1, 16, QCOM_QOS_MODE_FIXED, 1, 20, -1, 1, 587);
DEFINE_QNODE(mas_bimc_snoc_0, 10016, 0, 1, 16, QCOM_QOS_MODE_FIXED, 1, 21, -1, 9, 713, 583, 712, 522, 673, 10036, 10042, 585, 588);
DEFINE_QNODE(mas_bimc_snoc_1, 10055, 0, 1, 16, QCOM_QOS_MODE_FIXED, 1, 109, -1, 3, 714, 666, 665);
DEFINE_QNODE(mas_a0noc_snoc, 10060, 0, 1, 16, QCOM_QOS_MODE_FIXED, 1, 110, -1, 5, 10042, 585, 673, 10032, 712);
DEFINE_QNODE(mas_a1noc_snoc, 10063, 0, 1, 16, QCOM_QOS_MODE_FIXED, 0, 111, -1, 13, 713, 583, 665, 712, 714, 522, 666, 673, 10032, 10036, 10042, 585, 588);
DEFINE_QNODE(mas_a2noc_snoc, 10064, 0, 1, 16, QCOM_QOS_MODE_FIXED, 0, 112, -1, 12, 713, 583, 666, 712, 714, 588, 522, 10032, 10036, 10042, 585, 665);
DEFINE_QNODE(mas_qdss_etr, 60, 3, 1, 16, QCOM_QOS_MODE_FIXED, 1, 31, -1, 5, 712, 583, 585, 10032, 10042);
DEFINE_QNODE(slv_a0noc_snoc, 10061, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 141, 1, 10060);
DEFINE_QNODE(slv_a1noc_snoc, 10062, 0, 1, 8, QCOM_QOS_MODE_FIXED, 0, -1, 142, 1, 10063);
DEFINE_QNODE(slv_a2noc_snoc, 10065, 0, 1, 8, QCOM_QOS_MODE_FIXED, 0, -1, 143, 1, 10064);
DEFINE_QNODE(slv_ebi, 512, 0, 2, 8, QCOM_QOS_MODE_FIXED, 0, -1, 0, 0, 0);
DEFINE_QNODE(slv_hmss_l3, 680, 0, 1, 8, QCOM_QOS_MODE_FIXED, 0, -1, 160, 0, 0);
DEFINE_QNODE(slv_bimc_snoc_0, 10017, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 2, 1, 10016);
DEFINE_QNODE(slv_bimc_snoc_1, 10056, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 138, 1, 10055);
DEFINE_QNODE(slv_cnoc_a1noc, 10034, 0, 1, 4, QCOM_QOS_MODE_FIXED, 1, -1, 75, 1, 10059);
DEFINE_QNODE(slv_clk_ctl, 620, 0, 1, 4, QCOM_QOS_MODE_FIXED, 0, -1, 47, 0, 0);
DEFINE_QNODE(slv_tcsr, 623, 0, 1, 4, QCOM_QOS_MODE_FIXED, 0, -1, 50, 0, 0);
DEFINE_QNODE(slv_tlmm, 624, 0, 1, 4, QCOM_QOS_MODE_FIXED, 0, -1, 51, 0, 0);
DEFINE_QNODE(slv_crypto0_cfg, 625, 0, 1, 4, QCOM_QOS_MODE_FIXED, 1, -1, 52, 0, 0);
DEFINE_QNODE(slv_mpm, 536, 0, 1, 4, QCOM_QOS_MODE_FIXED, 1, -1, 62, 0, 0);
DEFINE_QNODE(slv_pimem_cfg, 681, 0, 1, 4, QCOM_QOS_MODE_FIXED, 1, -1, 167, 0, 0);
DEFINE_QNODE(slv_imem_cfg, 627, 0, 1, 4, QCOM_QOS_MODE_FIXED, 1, -1, 54, 0, 0);
DEFINE_QNODE(slv_message_ram, 628, 0, 1, 4, QCOM_QOS_MODE_FIXED, 0, -1, 55, 0, 0);
DEFINE_QNODE(slv_bimc_cfg, 629, 0, 1, 4, QCOM_QOS_MODE_FIXED, 1, -1, 56, 0, 0);
DEFINE_QNODE(slv_pmic_arb, 632, 0, 1, 4, QCOM_QOS_MODE_FIXED, 0, -1, 59, 0, 0);
DEFINE_QNODE(slv_prng, 618, 0, 1, 4, QCOM_QOS_MODE_FIXED, 1, -1, 44, 0, 0);
DEFINE_QNODE(slv_dcc_cfg, 682, 0, 1, 4, QCOM_QOS_MODE_FIXED, 1, -1, 155, 0, 0);
DEFINE_QNODE(slv_rbcpr_mx, 715, 0, 1, 4, QCOM_QOS_MODE_FIXED, 1, -1, 170, 0, 0);
DEFINE_QNODE(slv_qdss_cfg, 635, 0, 1, 4, QCOM_QOS_MODE_FIXED, 1, -1, 63, 0, 0);
DEFINE_QNODE(slv_rbcpr_cx, 716, 0, 1, 4, QCOM_QOS_MODE_FIXED, 1, -1, 169, 0, 0);
DEFINE_QNODE(slv_cpr_apu_cfg, 683, 0, 1, 4, QCOM_QOS_MODE_FIXED, 1, -1, 168, 0, 0);
DEFINE_QNODE(slv_cnoc_mnoc_cfg, 640, 0, 1, 4, QCOM_QOS_MODE_FIXED, 1, -1, 66, 1, 103);
DEFINE_QNODE(slv_snoc_cfg, 642, 0, 1, 4, QCOM_QOS_MODE_FIXED, 1, -1, 70, 0, 0);
DEFINE_QNODE(slv_snoc_mpu_cfg, 638, 0, 1, 4, QCOM_QOS_MODE_FIXED, 1, -1, 67, 0, 0);
DEFINE_QNODE(slv_ebi1_phy_cfg, 645, 0, 1, 4, QCOM_QOS_MODE_FIXED, 1, -1, 73, 0, 0);
DEFINE_QNODE(slv_a0noc_cfg, 686, 0, 1, 4, QCOM_QOS_MODE_FIXED, 1, -1, 144, 0, 0);
DEFINE_QNODE(slv_pcie_1_cfg, 668, 0, 1, 4, QCOM_QOS_MODE_FIXED, 1, -1, 89, 0, 0);
DEFINE_QNODE(slv_pcie_2_cfg, 684, 0, 1, 4, QCOM_QOS_MODE_FIXED, 1, -1, 165, 0, 0);
DEFINE_QNODE(slv_pcie_0_cfg, 667, 0, 1, 4, QCOM_QOS_MODE_FIXED, 1, -1, 88, 0, 0);
DEFINE_QNODE(slv_pcie20_ahb2phy, 685, 0, 1, 4, QCOM_QOS_MODE_FIXED, 1, -1, 163, 0, 0);
DEFINE_QNODE(slv_a0noc_mpu_cfg, 707, 0, 1, 4, QCOM_QOS_MODE_FIXED, 1, -1, 145, 0, 0);
DEFINE_QNODE(slv_ufs_cfg, 650, 0, 1, 4, QCOM_QOS_MODE_FIXED, 1, -1, 92, 0, 0);
DEFINE_QNODE(slv_a1noc_cfg, 687, 0, 1, 4, QCOM_QOS_MODE_FIXED, 1, -1, 147, 0, 0);
DEFINE_QNODE(slv_a1noc_mpu_cfg, 689, 0, 1, 4, QCOM_QOS_MODE_FIXED, 1, -1, 148, 0, 0);
DEFINE_QNODE(slv_a2noc_cfg, 688, 0, 1, 4, QCOM_QOS_MODE_FIXED, 1, -1, 150, 0, 0);
DEFINE_QNODE(slv_a2noc_mpu_cfg, 690, 0, 1, 4, QCOM_QOS_MODE_FIXED, 1, -1, 151, 0, 0);
DEFINE_QNODE(slv_ssc_cfg, 697, 0, 1, 4, QCOM_QOS_MODE_FIXED, 1, -1, 177, 0, 0);
DEFINE_QNODE(slv_a0noc_smmu_cfg, 691, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 146, 0, 0);
DEFINE_QNODE(slv_a1noc_smmu_cfg, 692, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 149, 0, 0);
DEFINE_QNODE(slv_a2noc_smmu_cfg, 693, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 152, 0, 0);
DEFINE_QNODE(slv_lpass_smmu_cfg, 694, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 161, 0, 0);
DEFINE_QNODE(slv_cnoc_mnoc_mmss_cfg, 631, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 58, 1, 102);
DEFINE_QNODE(slv_mmagic_cfg, 695, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 162, 0, 0);
DEFINE_QNODE(slv_cpr_cfg, 592, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 6, 0, 0);
DEFINE_QNODE(slv_misc_cfg, 594, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 8, 0, 0);
DEFINE_QNODE(slv_venus_throttle_cfg, 696, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 178, 0, 0);
DEFINE_QNODE(slv_venus_cfg, 596, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 10, 0, 0);
DEFINE_QNODE(slv_vmem_cfg, 708, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 180, 0, 0);
DEFINE_QNODE(slv_dsa_cfg, 698, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 157, 0, 0);
DEFINE_QNODE(slv_mnoc_clocks_cfg, 599, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 12, 0, 0);
DEFINE_QNODE(slv_dsa_mpu_cfg, 699, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 158, 0, 0);
DEFINE_QNODE(slv_mnoc_mpu_cfg, 601, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 14, 0, 0);
DEFINE_QNODE(slv_display_cfg, 590, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 4, 0, 0);
DEFINE_QNODE(slv_display_throttle_cfg, 700, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 156, 0, 0);
DEFINE_QNODE(slv_camera_cfg, 589, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 3, 0, 0);
DEFINE_QNODE(slv_camera_throttle_cfg, 709, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 154, 0, 0);
DEFINE_QNODE(slv_oxili_cfg, 598, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 11, 0, 0);
DEFINE_QNODE(slv_smmu_mdp_cfg, 703, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 173, 0, 0);
DEFINE_QNODE(slv_smmu_rot_cfg, 704, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 174, 0, 0);
DEFINE_QNODE(slv_smmu_venus_cfg, 705, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 175, 0, 0);
DEFINE_QNODE(slv_smmu_cpp_cfg, 701, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 171, 0, 0);
DEFINE_QNODE(slv_smmu_jpeg_cfg, 702, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 172, 0, 0);
DEFINE_QNODE(slv_smmu_vfe_cfg, 706, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 176, 0, 0);
DEFINE_QNODE(slv_mnoc_bimc, 10028, 0, 2, 32, QCOM_QOS_MODE_FIXED, 1, -1, 16, 1, 10027);
DEFINE_QNODE(slv_vmem, 710, 0, 1, 32, QCOM_QOS_MODE_FIXED, 1, -1, 179, 0, 0);
DEFINE_QNODE(slv_srvc_mnoc, 603, 0, 1, 8, QCOM_QOS_MODE_FIXED, 1, -1, 17, 0, 0);
DEFINE_QNODE(slv_pnoc_a1noc, 10058, 0, 1, 8, QCOM_QOS_MODE_FIXED, 0, -1, 139, 1, 10057);
DEFINE_QNODE(slv_usb_hs, 614, 0, 1, 4, QCOM_QOS_MODE_FIXED, 0, -1, 40, 0, 0);
DEFINE_QNODE(slv_sdcc_2, 608, 0, 1, 4, QCOM_QOS_MODE_FIXED, 0, -1, 33, 0, 0);
DEFINE_QNODE(slv_sdcc_4, 609, 0, 1, 4, QCOM_QOS_MODE_FIXED, 0, -1, 34, 0, 0);
DEFINE_QNODE(slv_tsif, 575, 0, 1, 4, QCOM_QOS_MODE_FIXED, 0, -1, 35, 0, 0);
DEFINE_QNODE(slv_blsp_2, 611, 0, 1, 4, QCOM_QOS_MODE_FIXED, 0, -1, 37, 0, 0);
DEFINE_QNODE(slv_sdcc_1, 606, 0, 1, 4, QCOM_QOS_MODE_FIXED, 0, -1, 31, 0, 0);
DEFINE_QNODE(slv_blsp_1, 613, 0, 1, 4, QCOM_QOS_MODE_FIXED, 0, -1, 39, 0, 0);
DEFINE_QNODE(slv_pdm, 615, 0, 1, 4, QCOM_QOS_MODE_FIXED, 0, -1, 41, 0, 0);
DEFINE_QNODE(slv_ahb2phy, 711, 0, 1, 4, QCOM_QOS_MODE_FIXED, 1, -1, 153, 0, 0);
DEFINE_QNODE(slv_hmss, 673, 0, 1, 16, QCOM_QOS_MODE_FIXED, 1, -1, 20, 0, 0);
DEFINE_QNODE(slv_lpass, 522, 0, 1, 16, QCOM_QOS_MODE_FIXED, 1, -1, 21, 0, 0);
DEFINE_QNODE(slv_usb3, 583, 0, 1, 16, QCOM_QOS_MODE_FIXED, 1, -1, 22, 0, 0);
DEFINE_QNODE(slv_snoc_bimc, 10032, 0, 2, 32, QCOM_QOS_MODE_FIXED, 0, -1, 24, 1, 10031);
DEFINE_QNODE(slv_snoc_cnoc, 10036, 0, 1, 16, QCOM_QOS_MODE_FIXED, 0, -1, 25, 1, 10035);
DEFINE_QNODE(slv_imem, 585, 0, 1, 16, QCOM_QOS_MODE_FIXED, 0, -1, 26, 0, 0);
DEFINE_QNODE(slv_pimem, 712, 0, 1, 16, QCOM_QOS_MODE_FIXED, 0, -1, 166, 0, 0);
DEFINE_QNODE(slv_snoc_vmem, 713, 0, 1, 16, QCOM_QOS_MODE_FIXED, 1, -1, 140, 1, 40);
DEFINE_QNODE(slv_snoc_pnoc, 10042, 0, 1, 16, QCOM_QOS_MODE_FIXED, 0, -1, 28, 1, 10041);
DEFINE_QNODE(slv_qdss_stm, 588, 0, 1, 16, QCOM_QOS_MODE_FIXED, 0, -1, 30, 0, 0);
DEFINE_QNODE(slv_pcie_0, 665, 0, 1, 16, QCOM_QOS_MODE_FIXED, 1, -1, 84, 0, 0);
DEFINE_QNODE(slv_pcie_1, 666, 0, 1, 16, QCOM_QOS_MODE_FIXED, 1, -1, 85, 0, 0);
DEFINE_QNODE(slv_pcie_2, 714, 0, 1, 16, QCOM_QOS_MODE_FIXED, 1, -1, 164, 0, 0);
DEFINE_QNODE(slv_srvc_snoc, 587, 0, 1, 16, QCOM_QOS_MODE_FIXED, 1, -1, 29, 0, 0);

static struct qcom_icc_node *msm8996_snoc_nodes[] = {
	&mas_hmss,
	&mas_qdss_bam,
	&mas_snoc_cfg,
	&mas_bimc_snoc_0,
	&mas_bimc_snoc_1,
	&mas_a0noc_snoc,
	&mas_a1noc_snoc,
	&mas_a2noc_snoc,
	&mas_qdss_etr,
	&slv_a0noc_snoc,
	&slv_a1noc_snoc,
	&slv_a2noc_snoc,
	&slv_hmss,
	&slv_lpass,
	&slv_usb3,
	&slv_snoc_bimc,
	&slv_snoc_cnoc,
	&slv_imem,
	&slv_pimem,
	&slv_snoc_vmem,
	&slv_snoc_pnoc,
	&slv_qdss_stm,
	&slv_pcie_0,
	&slv_pcie_1,
	&slv_pcie_2,
	&slv_srvc_snoc,
};

static struct qcom_icc_desc msm8996_snoc = {
	.nodes = msm8996_snoc_nodes,
	.num_nodes = ARRAY_SIZE(msm8996_snoc_nodes),
};

static struct qcom_icc_node *msm8996_bimc_nodes[] = {
	&mas_apps_proc,
	&mas_oxili,
	&mas_mnoc_bimc,
	&mas_snoc_bimc,
	&slv_ebi,
	&slv_hmss_l3,
	&slv_bimc_snoc_0,
	&slv_bimc_snoc_1,
};

static struct qcom_icc_desc msm8996_bimc = {
	.nodes = msm8996_bimc_nodes,
	.num_nodes = ARRAY_SIZE(msm8996_bimc_nodes),
};

static struct qcom_icc_node *msm8996_pnoc_nodes[] = {
	&mas_snoc_pnoc,
	&mas_sdcc_1,
	&mas_sdcc_2,
	&mas_sdcc_4,
	&mas_usb_hs,
	&mas_blsp_1,
	&mas_blsp_2,
	&mas_tsif,
	&slv_pnoc_a1noc,
	&slv_usb_hs,
	&slv_sdcc_2,
	&slv_sdcc_4,
	&slv_tsif,
	&slv_blsp_2,
	&slv_sdcc_1,
	&slv_blsp_1,
	&slv_pdm,
	&slv_ahb2phy,
};

static struct qcom_icc_desc msm8996_pnoc = {
	.nodes = msm8996_pnoc_nodes,
	.num_nodes = ARRAY_SIZE(msm8996_pnoc_nodes),
};

static struct qcom_icc_node *msm8996_cnoc_nodes[] = {
	&mas_snoc_cnoc,
	&mas_qdss_dap,
	&slv_cnoc_a1noc,
	&slv_clk_ctl,
	&slv_tcsr,
	&slv_tlmm,
	&slv_crypto0_cfg,
	&slv_mpm,
	&slv_pimem_cfg,
	&slv_imem_cfg,
	&slv_message_ram,
	&slv_bimc_cfg,
	&slv_pmic_arb,
	&slv_prng,
	&slv_dcc_cfg,
	&slv_rbcpr_mx,
	&slv_qdss_cfg,
	&slv_rbcpr_cx,
	&slv_cpr_apu_cfg,
	&slv_cnoc_mnoc_cfg,
	&slv_snoc_cfg,
	&slv_snoc_mpu_cfg,
	&slv_ebi1_phy_cfg,
	&slv_a0noc_cfg,
	&slv_pcie_1_cfg,
	&slv_pcie_2_cfg,
	&slv_pcie_0_cfg,
	&slv_pcie20_ahb2phy,
	&slv_a0noc_mpu_cfg,
	&slv_ufs_cfg,
	&slv_a1noc_cfg,
	&slv_a1noc_mpu_cfg,
	&slv_a2noc_cfg,
	&slv_a2noc_mpu_cfg,
	&slv_ssc_cfg,
	&slv_a0noc_smmu_cfg,
	&slv_a1noc_smmu_cfg,
	&slv_a2noc_smmu_cfg,
	&slv_lpass_smmu_cfg,
	&slv_cnoc_mnoc_mmss_cfg,
};

static struct qcom_icc_desc msm8996_cnoc = {
	.nodes = msm8996_cnoc_nodes,
	.num_nodes = ARRAY_SIZE(msm8996_cnoc_nodes),
};

static struct qcom_icc_node *msm8996_mnoc_nodes[] = {
	&mas_cnoc_mnoc_mmss_cfg,
	&mas_cnoc_mnoc_cfg,
	&mas_cpp,
	&mas_jpeg,
	&mas_mdp_p0,
	&mas_mdp_p1,
	&mas_rotator,
	&mas_venus,
	&mas_vfe,
	&mas_snoc_vmem,
	&mas_venus_vmem,
	&slv_mmagic_cfg,
	&slv_cpr_cfg,
	&slv_misc_cfg,
	&slv_venus_throttle_cfg,
	&slv_venus_cfg,
	&slv_vmem_cfg,
	&slv_dsa_cfg,
	&slv_mnoc_clocks_cfg,
	&slv_dsa_mpu_cfg,
	&slv_mnoc_mpu_cfg,
	&slv_display_cfg,
	&slv_display_throttle_cfg,
	&slv_camera_cfg,
	&slv_camera_throttle_cfg,
	&slv_oxili_cfg,
	&slv_smmu_mdp_cfg,
	&slv_smmu_rot_cfg,
	&slv_smmu_venus_cfg,
	&slv_smmu_cpp_cfg,
	&slv_smmu_jpeg_cfg,
	&slv_smmu_vfe_cfg,
	&slv_mnoc_bimc,
	&slv_vmem,
	&slv_srvc_mnoc,
};

static struct qcom_icc_desc msm8996_mnoc = {
	.nodes = msm8996_mnoc_nodes,
	.num_nodes = ARRAY_SIZE(msm8996_mnoc_nodes),
};

static struct qcom_icc_node *msm8996_a0noc_nodes[] = {
	&mas_pcie_0,
	&mas_pcie_1,
	&mas_pcie_2,
};

static struct qcom_icc_desc msm8996_a0noc = {
	.nodes = msm8996_a0noc_nodes,
	.num_nodes = ARRAY_SIZE(msm8996_a0noc_nodes),
};

static struct qcom_icc_node *msm8996_a1noc_nodes[] = {
	&mas_cnoc_a1noc,
	&mas_crypto_c0,
	&mas_pnoc_a1noc,
};

static struct qcom_icc_desc msm8996_a1noc = {
	.nodes = msm8996_a1noc_nodes,
	.num_nodes = ARRAY_SIZE(msm8996_a1noc_nodes),
};

static struct qcom_icc_node *msm8996_a2noc_nodes[] = {
	&mas_usb3,
	&mas_ipa,
	&mas_ufs,
};

static struct qcom_icc_desc msm8996_a2noc = {
	.nodes = msm8996_a2noc_nodes,
	.num_nodes = ARRAY_SIZE(msm8996_a2noc_nodes),
};


static int qcom_icc_init(struct icc_node *node)
{
	struct qcom_icc_provider *qp = to_qcom_provider(node->provider);
	int ret;

	/* TODO: init qos and priority */

	ret = clk_prepare_enable(qp->bus_clk);
	if (ret)
		pr_info("%s: error enabling bus clk (%d)\n", __func__, ret);
	ret = clk_prepare_enable(qp->bus_a_clk);
	if (ret)
		pr_info("%s: error enabling bus_a clk (%d)\n", __func__, ret);

	return 0;
}

static int qcom_icc_set(struct icc_node *src, struct icc_node *dst,
			u32 avg, u32 peak)
{
	struct qcom_icc_provider *qp;
	struct qcom_icc_node *qn;
	struct icc_node *node;
	struct icc_provider *provider;
	u64 avg_bw = 0;
	u64 peak_bw = 0;
	u64 rate = 0;
	int ret = 0;

	if (!src)
		node = dst;
	else
		node = src;

	qn = node->data;
	provider = node->provider;
	qp = to_qcom_provider(node->provider);

	/* convert from kbps to bps */
	avg_bw = avg * 1000ULL;
	peak_bw = peak * 1000ULL;

	/* set bandwidth */
	if (qn->ap_owned) {
		/* TODO: set QoS */
	} else {
		/* send message to the RPM processor */

		if (qn->mas_rpm_id != -1) {
			ret = qcom_icc_rpm_smd_send(QCOM_SMD_RPM_ACTIVE_STATE,
							 RPM_BUS_MASTER_REQ,
							 qn->mas_rpm_id,
							 avg_bw);
		}

		if (qn->slv_rpm_id != -1) {
			ret = qcom_icc_rpm_smd_send(QCOM_SMD_RPM_ACTIVE_STATE,
							 RPM_BUS_SLAVE_REQ,
							 qn->slv_rpm_id,
							 avg_bw);
		}
	}

	rate = max(avg_bw, peak_bw);

	do_div(rate, qn->buswidth);

	if (qn->rate != rate) {
		ret = clk_set_rate(qp->bus_clk, rate);
		if (ret) {
			pr_err("set clk rate %lld error %d\n", rate, ret);
			return ret;
		}

		ret = clk_set_rate(qp->bus_a_clk, rate);
		if (ret) {
			pr_err("set clk rate %lld error %d\n", rate, ret);
			return ret;
		}

		qn->rate = rate;
	}

	return ret;
}

static int qnoc_probe(struct platform_device *pdev)
{
	const struct qcom_icc_desc *desc;
	struct qcom_icc_node **qnodes;
	struct qcom_icc_provider *qp;
	struct resource *res;
	struct icc_provider *provider;
	size_t num_nodes, i;
	int ret;

	desc = of_device_get_match_data(&pdev->dev);
	if (!desc)
		return -EINVAL;

	qnodes = desc->nodes;
	num_nodes = desc->num_nodes;

	qp = devm_kzalloc(&pdev->dev, sizeof(*qp), GFP_KERNEL);
	if (!qp)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	qp->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(qp->base))
		return PTR_ERR(qp->base);

	qp->bus_clk = devm_clk_get(&pdev->dev, "bus_clk");
	if (IS_ERR(qp->bus_clk))
		return PTR_ERR(qp->bus_clk);

	qp->bus_a_clk = devm_clk_get(&pdev->dev, "bus_a_clk");
	if (IS_ERR(qp->bus_a_clk))
		return PTR_ERR(qp->bus_a_clk);

	provider = &qp->provider;
	provider->dev = &pdev->dev;
	provider->set = &qcom_icc_set;
	INIT_LIST_HEAD(&provider->nodes);
	provider->data = qp;

	ret = icc_provider_add(provider);
	if (ret) {
		dev_err(&pdev->dev, "error adding interconnect provider\n");
		return ret;
	}

	for (i = 0; i < num_nodes; i++) {
		struct icc_node *node;
		int ret;
		size_t j;

		node = icc_node_create(qnodes[i]->id);
		if (IS_ERR(node)) {
			ret = PTR_ERR(node);
			goto err;
		}
		node->name = qnodes[i]->name;
		node->data = qnodes[i];
		icc_node_add(node, provider);

		dev_dbg(&pdev->dev, "registered node %p %s %d\n", node,
			qnodes[i]->name, node->id);
		/* populate links */
		for (j = 0; j < qnodes[i]->num_links; j++)
			if (qnodes[i]->links[j])
				icc_link_create(node, qnodes[i]->links[j]);

		ret = qcom_icc_init(node);
		if (ret)
			dev_err(&pdev->dev, "%s init error (%d)\n", node->name,
				ret);
	}

	platform_set_drvdata(pdev, provider);

	return ret;
err:
	icc_provider_del(provider);
	return ret;
}

static int qnoc_remove(struct platform_device *pdev)
{
	struct icc_provider *provider = platform_get_drvdata(pdev);

	icc_provider_del(provider);

	return 0;
}

static const struct of_device_id qnoc_of_match[] = {
	{ .compatible = "qcom,msm8996-bimc", .data = &msm8996_bimc },
	{ .compatible = "qcom,msm8996-cnoc", .data = &msm8996_cnoc },
	{ .compatible = "qcom,msm8996-snoc", .data = &msm8996_snoc },
	{ .compatible = "qcom,msm8996-a0noc", .data = &msm8996_a0noc },
	{ .compatible = "qcom,msm8996-a1noc", .data = &msm8996_a1noc },
	{ .compatible = "qcom,msm8996-a2noc", .data = &msm8996_a2noc },
	{ .compatible = "qcom,msm8996-mmnoc", .data = &msm8996_mnoc },
	{ .compatible = "qcom,msm8996-pnoc", .data = &msm8996_pnoc },
	{ },
};
MODULE_DEVICE_TABLE(of, qnoc_of_match);

static struct platform_driver qnoc_driver = {
	.probe = qnoc_probe,
	.remove = qnoc_remove,
	.driver = {
		.name = "qnoc-msm8996",
		.of_match_table = qnoc_of_match,
	},
};
module_platform_driver(qnoc_driver);
MODULE_AUTHOR("Georgi Djakov <georgi.djakov@linaro.org>");
MODULE_DESCRIPTION("Qualcomm msm8996 NoC driver");
MODULE_LICENSE("GPL v2");

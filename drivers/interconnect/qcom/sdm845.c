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

#define DEFINE_QNODE(_name, _id, _port, _buswidth, _ap_owned,		\
			_mas_rpm_id, _slv_rpm_id, _qos_mode,		\
			_numlinks, ...)					\
		static struct qcom_icc_node _name = {			\
		.id = _id,						\
		.name = #_name,						\
		.port = _port,						\
		.buswidth = _buswidth,					\
		.qos_mode = _qos_mode,					\
		.ap_owned = _ap_owned,					\
		.mas_rpm_id = _mas_rpm_id,				\
		.slv_rpm_id = _slv_rpm_id,				\
		.num_links = _numlinks,					\
		.links = { __VA_ARGS__ },				\
	}

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
};

#define SDM845_MAX_LINKS	43

/**
 * struct qcom_icc_node - Qualcomm specific interconnect nodes
 * @name: the node name used in debugfs
 * @links: an array of nodes where we can go next while traversing
 * @id: a unique node identifier
 * @num_links: the total number of @links
 * @port: the offset index into the masters QoS register space
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
	u16 links[SDM845_MAX_LINKS];
	u16 id;
	u16 num_links;
	u16 port;
	u16 buswidth;
	bool ap_owned;
	struct qcom_smd_rpm *rpm;
	enum qcom_qos_mode qos_mode;
	int mas_rpm_id;
	int slv_rpm_id;
	u64 rate;
};

struct qcom_icc_desc {
	struct qcom_icc_node **nodes;
	size_t num_nodes;
};

DEFINE_QNODE(mas_qhm_a1noc_cfg, 121, 0, 1, 4, 0, -1, -1, 1, 744);
DEFINE_QNODE(mas_qhm_qup1, 86, 0, 1, 4, 0, -1, -1, 1, 10062);
DEFINE_QNODE(mas_qhm_tsif, 82, 0, 1, 4, 0, -1, -1, 1, 10062);
DEFINE_QNODE(mas_xm_sdc2, 81, 1, 1, 8, 1, -1, -1, 1, 10062);
DEFINE_QNODE(mas_xm_sdc4, 80, 2, 1, 8, 1, -1, -1, 1, 10062);
DEFINE_QNODE(mas_xm_ufs_card, 122, 3, 1, 8, 1, -1, -1, 1, 10062);
DEFINE_QNODE(mas_xm_ufs_mem, 123, 4, 1, 8, 1, -1, -1, 1, 10062);
DEFINE_QNODE(mas_xm_pcie_0, 45, 5, 1, 8, 1, -1, -1, 1, 10068);
DEFINE_QNODE(mas_qhm_a2noc_cfg, 124, 0, 1, 4, 0, -1, -1, 1, 746);
DEFINE_QNODE(mas_qhm_qdss_bam, 53, 0, 1, 4, 0, -1, -1, 1, 10065);
DEFINE_QNODE(mas_qhm_qup2, 84, 0, 1, 4, 0, -1, -1, 1, 10065);
DEFINE_QNODE(mas_qnm_cnoc, 118, 0, 1, 8, 1, -1, -1, 1, 10065);
DEFINE_QNODE(mas_qxm_crypto, 125, 1, 1, 8, 1, -1, -1, 1, 10065);
DEFINE_QNODE(mas_qxm_ipa, 90, 2, 1, 8, 0, -1, -1, 1, 10065);
DEFINE_QNODE(mas_xm_pcie3_1, 100, 6, 1, 8, 1, -1, -1, 1, 745);
DEFINE_QNODE(mas_xm_qdss_etr, 60, 7, 1, 8, 1, -1, -1, 1, 10065);
DEFINE_QNODE(mas_xm_usb3_0, 61, 10, 1, 8, 1, -1, -1, 1, 10065);
DEFINE_QNODE(mas_xm_usb3_1, 101, 11, 1, 8, 1, -1, -1, 1, 10065);
DEFINE_QNODE(mas_qxm_camnoc_hf0_uncomp, 146, 0, 1, 32, 0, -1, -1, 1, 778);
DEFINE_QNODE(mas_qxm_camnoc_hf1_uncomp, 147, 0, 1, 32, 0, -1, -1, 1, 778);
DEFINE_QNODE(mas_qxm_camnoc_sf_uncomp, 148, 0, 1, 32, 0, -1, -1, 1, 778);
DEFINE_QNODE(mas_qhm_spdm, 36, 0, 1, 4, 0, -1, -1, 1, 725);
DEFINE_QNODE(mas_qhm_tic, 77, 0, 1, 4, 0, -1, -1, 43, 755, 753, 589, 609, 608, 640, 757, 642, 726, 615, 688, 635, 590, 623, 682, 750, 725, 752, 668, 667, 598, 596, 575, 749, 747, 611, 583, 646, 756, 751, 676, 651, 687, 748, 618, 758, 613, 633, 625, 681, 731, 620, 627);
DEFINE_QNODE(mas_qnm_snoc, 10035, 0, 1, 8, 0, -1, -1, 42, 755, 753, 589, 609, 608, 640, 757, 642, 726, 615, 688, 635, 590, 623, 682, 750, 752, 668, 667, 598, 596, 575, 749, 747, 611, 583, 646, 756, 751, 676, 651, 687, 748, 618, 758, 613, 633, 625, 681, 731, 620, 627);
DEFINE_QNODE(mas_xm_qdss_dap, 76, 0, 1, 8, 0, -1, -1, 43, 755, 753, 589, 609, 608, 640, 757, 642, 726, 615, 688, 635, 590, 623, 682, 750, 725, 752, 668, 667, 598, 596, 575, 749, 747, 611, 583, 646, 756, 751, 676, 651, 687, 748, 618, 758, 613, 633, 625, 681, 731, 620, 627);
DEFINE_QNODE(mas_qhm_cnoc, 126, 0, 1, 4, 0, -1, -1, 2, 761, 760);
DEFINE_QNODE(mas_acm_l3, 1, 0, 1, 16, 0, -1, -1, 3, 764, 728, 763);
DEFINE_QNODE(mas_pm_gnoc_cfg, 127, 0, 1, 4, 0, -1, -1, 1, 764);
DEFINE_QNODE(mas_ipa_core_master, 143, 0, 1, 8, 0, -1, -1, 1, 777);
DEFINE_QNODE(mas_llcc_mc, 129, 0, 4, 4, 0, -1, -1, 1, 512);
DEFINE_QNODE(mas_acm_tcu, 104, 0, 1, 8, 1, -1, -1, 3, 766, 770, 776);
DEFINE_QNODE(mas_qhm_memnoc_cfg, 130, 0, 1, 4, 0, -1, -1, 2, 771, 765);
//DEFINE_QNODE(mas_qnm_apps, 131, 2 3, 2, 32, 1, -1, -1, 1, 770);
//DEFINE_QNODE(mas_qnm_mnoc_hf, 132, 4 5, 2, 32, 1, -1, -1, 2, 766, 770);
DEFINE_QNODE(mas_qnm_mnoc_sf, 133, 7, 1, 32, 1, -1, -1, 3, 766, 770, 776);
DEFINE_QNODE(mas_qnm_snoc_gc, 134, 8, 1, 8, 1, -1, -1, 1, 770);
DEFINE_QNODE(mas_qnm_snoc_sf, 135, 9, 1, 16, 1, -1, -1, 2, 766, 770);
//DEFINE_QNODE(mas_qxm_gpu, 26, 10 11, 2, 32, 1, -1, -1, 3, 766, 770, 776);
//DEFINE_QNODE(mas_qhm_mnoc_cfg, 103, 0, 1, 4, 0, -1, -1, 1, 603);
DEFINE_QNODE(mas_qxm_camnoc_hf0, 136, 1, 1, 32, 1, -1, -1, 1, 773);
DEFINE_QNODE(mas_qxm_camnoc_hf1, 145, 2, 1, 32, 1, -1, -1, 1, 773);
DEFINE_QNODE(mas_qxm_camnoc_sf, 137, 0, 1, 32, 1, -1, -1, 1, 772);
DEFINE_QNODE(mas_qxm_mdp0, 22, 3, 1, 32, 1, -1, -1, 1, 773);
DEFINE_QNODE(mas_qxm_mdp1, 23, 4, 1, 32, 1, -1, -1, 1, 773);
DEFINE_QNODE(mas_qxm_rot, 25, 5, 1, 32, 1, -1, -1, 1, 772);
DEFINE_QNODE(mas_qxm_venus0, 63, 6, 1, 32, 1, -1, -1, 1, 772);
DEFINE_QNODE(mas_qxm_venus1, 64, 7, 1, 32, 1, -1, -1, 1, 772);
DEFINE_QNODE(mas_qxm_venus_arm9, 138, 8, 1, 8, 1, -1, -1, 1, 772);
DEFINE_QNODE(mas_qhm_snoc_cfg, 54, 0, 1, 4, 0, -1, -1, 1, 587);
DEFINE_QNODE(mas_qnm_aggre1_noc, 10063, 0, 1, 16, 0, -1, -1, 6, 712, 775, 585, 673, 10036, 588);
DEFINE_QNODE(mas_qnm_aggre2_noc, 10064, 0, 1, 16, 0, -1, -1, 9, 712, 775, 666, 585, 673, 10036, 665, 672, 588);
DEFINE_QNODE(mas_qnm_gladiator_sodv, 139, 0, 1, 8, 0, -1, -1, 8, 712, 666, 585, 673, 10036, 665, 672, 588);
DEFINE_QNODE(mas_qnm_memnoc, 142, 0, 1, 8, 0, -1, -1, 5, 585, 673, 712, 10036, 588);
DEFINE_QNODE(mas_qnm_pcie_anoc, 140, 0, 1, 16, 0, -1, -1, 5, 585, 673, 10036, 775, 588);
DEFINE_QNODE(mas_qxm_pimem, 141, 3, 1, 8, 1, -1, -1, 2, 585, 774);
DEFINE_QNODE(mas_xm_gic, 149, 0, 1, 8, 1, -1, -1, 2, 585, 774);
DEFINE_QNODE(mas_alc, 144, 0, 1, 1, 0, -1, -1, 0, 0);
DEFINE_QNODE(mas_llcc_mc_display, 20000, 0, 4, 4, 0, -1, -1, 1, 20512);
//DEFINE_QNODE(mas_qnm_mnoc_hf_display, 20001, 4 5, 2, 32, 0, -1, -1, 1, 20513);
DEFINE_QNODE(mas_qnm_mnoc_sf_display, 20002, 7, 1, 32, 0, -1, -1, 1, 20513);
DEFINE_QNODE(mas_qxm_mdp0_display, 20003, 3, 1, 32, 0, -1, -1, 1, 20515);
DEFINE_QNODE(mas_qxm_mdp1_display, 20004, 4, 1, 32, 0, -1, -1, 1, 20515);
DEFINE_QNODE(mas_qxm_rot_display, 20005, 5, 1, 32, 0, -1, -1, 1, 20514);
DEFINE_QNODE(slv_qns_a1noc_snoc, 10062, 0, 1, 16, 0, -1, -1, 1, 10063);
DEFINE_QNODE(slv_srvc_aggre1_noc, 744, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qns_pcie_a1noc_snoc, 10068, 0, 1, 16, 0, -1, -1, 1, 140);
DEFINE_QNODE(slv_qns_a2noc_snoc, 10065, 0, 1, 16, 0, -1, -1, 1, 10064);
DEFINE_QNODE(slv_qns_pcie_snoc, 745, 0, 1, 16, 0, -1, -1, 1, 140);
DEFINE_QNODE(slv_srvc_aggre2_noc, 746, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qns_camnoc_uncomp, 778, 0, 1, 32, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_a1_noc_cfg, 687, 0, 1, 4, 0, -1, -1, 1, 121);
DEFINE_QNODE(slv_qhs_a2_noc_cfg, 688, 0, 1, 4, 0, -1, -1, 1, 124);
DEFINE_QNODE(slv_qhs_aop, 747, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_aoss, 748, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_camera_cfg, 589, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_clk_ctl, 620, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_compute_dsp_cfg, 749, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_cpr_cx, 651, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_crypto0_cfg, 625, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_dcc_cfg, 682, 0, 1, 4, 0, -1, -1, 1, 126);
DEFINE_QNODE(slv_qhs_ddrss_cfg, 750, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_display_cfg, 590, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_glm, 726, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_gpuss_cfg, 598, 0, 1, 8, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_imem_cfg, 627, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_ipa, 676, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_mnoc_cfg, 640, 0, 1, 4, 0, -1, -1, 1, 103);
DEFINE_QNODE(slv_qhs_pcie0_cfg, 667, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_pcie_gen3_cfg, 668, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_pdm, 615, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_phy_refgen_south, 752, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_pimem_cfg, 681, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_prng, 618, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_qdss_cfg, 635, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_qupv3_north, 611, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_qupv3_south, 613, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_sdc2, 608, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_sdc4, 609, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_snoc_cfg, 642, 0, 1, 4, 0, -1, -1, 1, 54);
DEFINE_QNODE(slv_qhs_spdm, 633, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_spss_cfg, 753, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_tcsr, 623, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_tlmm_north, 731, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_tlmm_south, 755, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_tsif, 575, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_ufs_card_cfg, 756, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_ufs_mem_cfg, 757, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_usb3_0, 583, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_usb3_1, 751, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_venus_cfg, 596, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_vsense_ctrl_cfg, 758, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qns_cnoc_a2noc, 725, 0, 1, 8, 0, -1, -1, 1, 118);
DEFINE_QNODE(slv_srvc_cnoc, 646, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_llcc, 760, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_memnoc, 761, 0, 1, 4, 0, -1, -1, 1, 130);
DEFINE_QNODE(slv_qns_gladiator_sodv, 728, 0, 1, 8, 0, -1, -1, 1, 139);
DEFINE_QNODE(slv_qns_gnoc_memnoc, 763, 0, 2, 32, 0, -1, -1, 1, 131);
DEFINE_QNODE(slv_srvc_gnoc, 764, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_ipa_core_slave, 777, 0, 1, 8, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_ebi, 512, 0, 4, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_mdsp_ms_mpu_cfg, 765, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qns_apps_io, 766, 0, 1, 32, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qns_llcc, 770, 0, 4, 16, 0, -1, -1, 1, 129);
DEFINE_QNODE(slv_qns_memnoc_snoc, 776, 0, 1, 8, 0, -1, -1, 1, 142);
DEFINE_QNODE(slv_srvc_memnoc, 771, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qns2_mem_noc, 772, 0, 1, 32, 0, -1, -1, 1, 133);
DEFINE_QNODE(slv_qns_mem_noc_hf, 773, 0, 2, 32, 0, -1, -1, 1, 132);
DEFINE_QNODE(slv_srvc_mnoc, 603, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qhs_apss, 673, 0, 1, 8, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qns_cnoc, 10036, 0, 1, 8, 0, -1, -1, 1, 10035);
DEFINE_QNODE(slv_qns_memnoc_gc, 774, 0, 1, 8, 0, -1, -1, 1, 134);
DEFINE_QNODE(slv_qns_memnoc_sf, 775, 0, 1, 16, 0, -1, -1, 1, 135);
DEFINE_QNODE(slv_qxs_imem, 585, 0, 1, 8, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qxs_pcie, 665, 0, 1, 8, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qxs_pcie_gen3, 666, 0, 1, 8, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qxs_pimem, 712, 0, 1, 8, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_srvc_snoc, 587, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_xs_qdss_stm, 588, 0, 1, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_xs_sys_tcu_cfg, 672, 0, 1, 8, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_ebi_display, 20512, 0, 4, 4, 0, -1, -1, 0, 0);
DEFINE_QNODE(slv_qns_llcc_display, 20513, 0, 4, 16, 0, -1, -1, 1, 20000);
DEFINE_QNODE(slv_qns2_mem_noc_display, 20514, 0, 1, 32, 0, -1, -1, 1, 20002);
DEFINE_QNODE(slv_qns_mem_noc_hf_display, 20515, 0, 2, 32, 0, -1, -1, 1, 20001);

struct qcom_icc_node *sdm845_snoc_nodes[] = {
	//&fab_system_noc
};

static struct qcom_icc_desc sdm845_snoc = {
	.nodes = sdm845_snoc_nodes,
	.num_nodes = ARRAY_SIZE(sdm845_snoc_nodes),
};

struct qcom_icc_node *sdm845_cnoc_nodes[] = {
	//&fab_config_noc
};

static struct qcom_icc_desc sdm845_cnoc = {
	.nodes = sdm845_cnoc_nodes,
	.num_nodes = ARRAY_SIZE(sdm845_cnoc_nodes),
};

struct qcom_icc_node *sdm845_mem_nodes[] = {
	//&fab_mem_noc
};

static struct qcom_icc_desc sdm845_mem = {
	.nodes = sdm845_mem_nodes,
	.num_nodes = ARRAY_SIZE(sdm845_mem_nodes),
};

struct qcom_icc_node *sdm845_mem_display_nodes[] = {
	//&fab_mem_noc_display
};

static struct qcom_icc_desc sdm845_mem_display = {
	.nodes = sdm845_mem_display_nodes,
	.num_nodes = ARRAY_SIZE(sdm845_mem_display_nodes),
};

struct qcom_icc_node *sdm845_mmss_nodes[] = {
	//&fab_mmss_noc
};

static struct qcom_icc_desc sdm845_mmss = {
	.nodes = sdm845_mmss_nodes,
	.num_nodes = ARRAY_SIZE(sdm845_mmss_nodes),
};

struct qcom_icc_node *sdm845_mmss_display_nodes[] = {
	//&fab_mmss_noc_display
};

static struct qcom_icc_desc sdm845_mmss_display = {
	.nodes = sdm845_mmss_display_nodes,
	.num_nodes = ARRAY_SIZE(sdm845_mmss_display_nodes),
};

struct qcom_icc_node *sdm845_aggre1_nodes[] = {
	//&fab_aggre1_noc
};

static struct qcom_icc_desc sdm845_aggre1 = {
	.nodes = sdm845_aggre1_nodes,
	.num_nodes = ARRAY_SIZE(sdm845_aggre1_nodes),
};

struct qcom_icc_node *sdm845_aggre2_nodes[] = {
	//&fab_aggre2_noc
};

static struct qcom_icc_desc sdm845_aggre2 = {
	.nodes = sdm845_aggre2_nodes,
	.num_nodes = ARRAY_SIZE(sdm845_aggre2_nodes),
};

struct qcom_icc_node *sdm845_camnoc_nodes[] = {
	//&fab_camnoc_virt
};

static struct qcom_icc_desc sdm845_camnoc = {
	.nodes = sdm845_camnoc_nodes,
	.num_nodes = ARRAY_SIZE(sdm845_camnoc_nodes),
};

struct qcom_icc_node *sdm845_dc_nodes[] = {
	//&fab_dc_noc
};

static struct qcom_icc_desc sdm845_dc = {
	.nodes = sdm845_dc_nodes,
	.num_nodes = ARRAY_SIZE(sdm845_dc_nodes),
};

struct qcom_icc_node *sdm845_gladiator_nodes[] = {
	//&fab_gladiator_noc
};

static struct qcom_icc_desc sdm845_gladiator = {
	.nodes = sdm845_gladiator_nodes,
	.num_nodes = ARRAY_SIZE(sdm845_gladiator_nodes),
};

struct qcom_icc_node *sdm845_ipa_virt_nodes[] = {
	//&fab_ipa_virt
};

static struct qcom_icc_desc sdm845_ipa_virt = {
	.nodes = sdm845_ipa_virt_nodes,
	.num_nodes = ARRAY_SIZE(sdm845_ipa_virt_nodes),
};

struct qcom_icc_node *sdm845_mc_virt_nodes[] = {
	//&fab_mc_virt
};

static struct qcom_icc_desc sdm845_mc_virt = {
	.nodes = sdm845_mc_virt_nodes,
	.num_nodes = ARRAY_SIZE(sdm845_mc_virt_nodes),
};

struct qcom_icc_node *sdm845_mc_virt_display_nodes[] = {
	//&fab_mc_virt_display
};

static struct qcom_icc_desc sdm845_mc_virt_display = {
	.nodes = sdm845_mc_virt_display_nodes,
	.num_nodes = ARRAY_SIZE(sdm845_mc_virt_display_nodes),
};

static int qcom_icc_init(struct icc_node *node)
{
	struct qcom_icc_provider *qp = to_qcom_provider(node->provider);
	/* TODO: init qos and priority */

	clk_prepare_enable(qp->bus_clk);
	clk_prepare_enable(qp->bus_a_clk);

	return 0;
}

static int qcom_icc_aggregate(struct icc_node *node, u32 avg_bw, u32 peak_bw,
				u32 *agg_avg, u32 *agg_peak)
{
	/* sum(averages) and max(peaks) */
	*agg_avg = node->avg_bw + avg_bw;
	*agg_peak = max(node->peak_bw, peak_bw);

	return 0;
}

static int qcom_icc_set(struct icc_node *src, struct icc_node *dst,
			u32 avg, u32 peak)
{
	struct qcom_icc_provider *qp;
	struct qcom_icc_node *qn;
	struct icc_node *node;
	struct icc_provider *provider;
	u64 avg_bw;
	u64 peak_bw;
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

	/* TODO: set bandwidth */

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
	provider->aggregate = &qcom_icc_aggregate;
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
	{ .compatible = "qcom,snoc-sdm845", .data = &sdm845_snoc },
	{ .compatible = "qcom,cnoc-sdm845", .data = &sdm845_cnoc },
	{ .compatible = "qcom,memnoc-sdm845", .data = &sdm845_mem },
	{ .compatible = "qcom,mem-dispay-sdm845", .data = &sdm845_mem_display },
	{ .compatible = "qcom,mmss-sdm845-mmss", .data = &sdm845_mmss },
	{ .compatible = "qcom,sdm845-mmss-display", .data = &sdm845_mmss_display },
	{ .compatible = "qcom,sdm845-mmss-display", .data = &sdm845_mmss_display },
	{ .compatible = "qcom,agg1noc-sdm845", .data = &sdm845_aggre1 },
	{ .compatible = "qcom,agg2noc-sdm845", .data = &sdm845_aggre2 },
	{ .compatible = "qcom,camnoc-sdm845", .data = &sdm845_camnoc },
	{ .compatible = "qcom,dcnoc-sdm845", .data = &sdm845_dc },
	{ .compatible = "qcom,gnoc-sdm845", .data = &sdm845_gladiator },
	{ .compatible = "qcom,ipa-virt-sdm845", .data = &sdm845_ipa_virt },
	{ .compatible = "qcom,mc-virt-sdm845", .data = &sdm845_mc_virt },
	{ .compatible = "qcom,mc-virt-display-sdm845", .data = &sdm845_mc_virt_display },
	{ },
};
MODULE_DEVICE_TABLE(of, qnoc_of_match);

static struct platform_driver qnoc_driver = {
	.probe = qnoc_probe,
	.remove = qnoc_remove,
	.driver = {
		.name = "qnoc-sdm845",
		.of_match_table = qnoc_of_match,
	},
};
module_platform_driver(qnoc_driver);
MODULE_AUTHOR("Georgi Djakov <georgi.djakov@linaro.org>");
MODULE_DESCRIPTION("Qualcomm sdm845 NoC driver");
MODULE_LICENSE("GPL v2");

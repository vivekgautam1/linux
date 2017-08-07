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

#define MSM8916_MAX_LINKS	8

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
	u16 links[MSM8916_MAX_LINKS];
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

DEFINE_QNODE(mas_video, 63, 8, 16, 1, -1, -1, QCOM_QOS_MODE_BYPASS, 2, 10000, 10002);
DEFINE_QNODE(mas_jpeg, 62, 6, 16, 1, -1, -1, QCOM_QOS_MODE_BYPASS, 2, 10000, 10002);
DEFINE_QNODE(mas_vfe, 29, 9, 16, 1, -1, -1, QCOM_QOS_MODE_BYPASS, 2, 10001, 10002);
DEFINE_QNODE(mas_mdp, 22, 7, 16, 1, -1, -1, QCOM_QOS_MODE_BYPASS, 2, 10000, 10002);
DEFINE_QNODE(mas_qdss_bam, 53, 11, 16, 1, -1, -1, QCOM_QOS_MODE_FIXED, 1, 10009);
DEFINE_QNODE(mas_snoc_cfg, 54, 0, 16, 0, 20, -1, QCOM_QOS_MODE_BYPASS, 1, 10009);
DEFINE_QNODE(mas_qdss_etr, 60, 10, 16, 1, -1, -1, QCOM_QOS_MODE_FIXED, 1, 10009);
DEFINE_QNODE(mm_int_0, 10000, 0, 16, 1, -1, -1, QCOM_QOS_MODE_FIXED, 1, 10003);
DEFINE_QNODE(mm_int_1, 10001, 0, 16, 1, -1, -1, QCOM_QOS_MODE_FIXED, 1, 10003);
DEFINE_QNODE(mm_int_2, 10002, 0, 16, 1, -1, -1, QCOM_QOS_MODE_FIXED, 1, 10004);
DEFINE_QNODE(mm_int_bimc, 10003, 0, 16, 1, -1, -1, QCOM_QOS_MODE_FIXED, 1, 10008);
DEFINE_QNODE(snoc_int_0, 10004, 0, 8, 0, 99, 130, QCOM_QOS_MODE_FIXED, 3, 588, 519, 10027);
DEFINE_QNODE(snoc_int_1, 10005, 0, 8, 0, 100, 131, QCOM_QOS_MODE_FIXED, 3, 517, 663, 664);
DEFINE_QNODE(snoc_int_bimc, 10006, 0, 8, 0, 101, 132, QCOM_QOS_MODE_FIXED, 1, 10007);
DEFINE_QNODE(snoc_bimc_0_mas, 10007, 0, 8, 0, 3, -1, QCOM_QOS_MODE_FIXED, 1, 10025);
DEFINE_QNODE(snoc_bimc_1_mas, 10008, 0, 16, 1, -1, -1, QCOM_QOS_MODE_FIXED, 1, 10026);
DEFINE_QNODE(qdss_int, 10009, 0, 8, 1, -1, -1, QCOM_QOS_MODE_FIXED, 2, 10004, 10006);
DEFINE_QNODE(bimc_snoc_slv, 10017, 0, 8, 1, -1, -1, QCOM_QOS_MODE_FIXED, 2, 10004, 10005);
DEFINE_QNODE(snoc_pnoc_mas, 10027, 0, 8, 0, -1, -1, QCOM_QOS_MODE_FIXED, 1, 10028);
DEFINE_QNODE(pnoc_snoc_slv, 10011, 0, 8, 0, -1, 45, QCOM_QOS_MODE_FIXED, 3, 10004, 10006, 10005);
DEFINE_QNODE(slv_srvc_snoc, 587, 0, 8, 0, -1, 29, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_qdss_stm, 588, 0, 4, 0, -1, 30, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_imem, 519, 0, 8, 0, -1, 26, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_apss, 517, 0, 4, 0, -1, 20, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_cats_0, 663, 0, 16, 0, -1, 106, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_cats_1, 664, 0, 8, 0, -1, 107, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(mas_apss, 1, 0, 8, 1, -1, -1, QCOM_QOS_MODE_FIXED, 3, 512, 10016, 514);
DEFINE_QNODE(mas_tcu0, 104, 5, 8, 1, -1, -1, QCOM_QOS_MODE_FIXED, 3, 512, 10016, 514);
DEFINE_QNODE(mas_tcu1, 105, 6, 8, 1, -1, -1, QCOM_QOS_MODE_FIXED, 3, 512, 10016, 514);
DEFINE_QNODE(mas_gfx, 26, 2, 8, 1, -1, -1, QCOM_QOS_MODE_FIXED, 3, 512, 10016, 514);
DEFINE_QNODE(bimc_snoc_mas, 10016, 0, 8, 1, -1, -1, QCOM_QOS_MODE_FIXED, 1, 10017);
DEFINE_QNODE(snoc_bimc_0_slv, 10025, 0, 8, 0, -1, 24, QCOM_QOS_MODE_FIXED, 1, 512);
DEFINE_QNODE(snoc_bimc_1_slv, 10026, 0, 8, 1, -1, -1, QCOM_QOS_MODE_FIXED, 1, 512);
DEFINE_QNODE(slv_ebi_ch0, 512, 0, 8, 0, -1, 0, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_apps_l2, 514, 0, 8, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(snoc_pnoc_slv, 10028, 0, 8, 0, -1, -1, QCOM_QOS_MODE_FIXED, 1, 10012);
DEFINE_QNODE(pnoc_int_0, 10012, 0, 8, 0, -1, -1, QCOM_QOS_MODE_FIXED, 8, 10010, 10018, 10019, 10020, 10021, 10022, 10023, 10024);
DEFINE_QNODE(pnoc_int_1, 10013, 0, 8, 0, -1, -1, QCOM_QOS_MODE_FIXED, 1, 10010);
DEFINE_QNODE(pnoc_m_0, 10014, 0, 8, 0, -1, -1, QCOM_QOS_MODE_FIXED, 1, 10012);
DEFINE_QNODE(pnoc_m_1, 10015, 0, 8, 0, -1, -1, QCOM_QOS_MODE_FIXED, 1, 10010);
DEFINE_QNODE(pnoc_s_0, 10018, 0, 8, 0, -1, -1, QCOM_QOS_MODE_FIXED, 5, 620, 624, 579, 622, 521);
DEFINE_QNODE(pnoc_s_1, 10019, 0, 8, 0, -1, -1, QCOM_QOS_MODE_FIXED, 5, 627, 625, 535, 577, 618);
DEFINE_QNODE(pnoc_s_2, 10020, 0, 8, 0, -1, -1, QCOM_QOS_MODE_FIXED, 5, 533, 630, 629, 641, 632);
DEFINE_QNODE(pnoc_s_3, 10021, 0, 8, 0, -1, -1, QCOM_QOS_MODE_FIXED, 5, 536, 647, 636, 635, 634);
DEFINE_QNODE(pnoc_s_4, 10022, 0, 8, 0, -1, -1, QCOM_QOS_MODE_FIXED, 3, 596, 589, 590);
DEFINE_QNODE(pnoc_s_8, 10023, 0, 8, 0, -1, -1, QCOM_QOS_MODE_FIXED, 3, 614, 606, 613);
DEFINE_QNODE(pnoc_s_9, 10024, 0, 8, 0, -1, -1, QCOM_QOS_MODE_FIXED, 3, 609, 522, 598);
DEFINE_QNODE(slv_imem_cfg, 627, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_crypto_0_cfg, 625, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_msg_ram, 535, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_pdm, 577, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_prng, 618, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_clk_ctl, 620, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_mss, 521, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_tlmm, 624, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_tcsr, 579, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_security, 622, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_spdm, 533, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_pnoc_cfg, 641, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_pmic_arb, 632, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_bimc_cfg, 629, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_boot_rom, 630, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_mpm, 536, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_qdss_cfg, 635, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_rbcpr_cfg, 636, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_snoc_cfg, 647, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_dehr_cfg, 634, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_venus_cfg, 596, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_display_cfg, 590, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_camera_cfg, 589, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_usb_hs, 614, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_sdcc_1, 606, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_blsp_1, 613, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_sdcc_2, 609, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_gfx_cfg, 598, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(slv_audio, 522, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 0, 0);
DEFINE_QNODE(mas_blsp_1, 86, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 1, 10015);
DEFINE_QNODE(mas_spdm, 36, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 1, 10014);
DEFINE_QNODE(mas_dehr, 75, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 1, 10014);
DEFINE_QNODE(mas_audio, 15, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 1, 10014);
DEFINE_QNODE(mas_usb_hs, 87, 0, 4, 0, -1, -1, QCOM_QOS_MODE_FIXED, 1, 10015);
DEFINE_QNODE(mas_pnoc_crypto_0, 55, 0, 8, 0, -1, -1, QCOM_QOS_MODE_FIXED, 1, 10013);
DEFINE_QNODE(mas_pnoc_sdcc_1, 78, 7, 8, 0, -1, -1, QCOM_QOS_MODE_FIXED, 1, 10013);
DEFINE_QNODE(mas_pnoc_sdcc_2, 81, 8, 8, 0, -1, -1, QCOM_QOS_MODE_FIXED, 1, 10013);
DEFINE_QNODE(pnoc_snoc_mas, 10010, 0, 8, 0, 29, -1, QCOM_QOS_MODE_FIXED, 1, 10011);

static struct qcom_icc_node *msm8916_snoc_nodes[] = {
	&mas_video,
	&mas_jpeg,
	&mas_vfe,
	&mas_mdp,
	&mas_qdss_bam,
	&mas_snoc_cfg,
	&mas_qdss_etr,
	&mm_int_0,
	&mm_int_1,
	&mm_int_2,
	&mm_int_bimc,
	&snoc_int_0,
	&snoc_int_1,
	&snoc_int_bimc,
	&snoc_bimc_0_mas,
	&snoc_bimc_1_mas,
	&qdss_int,
	&bimc_snoc_slv,
	&snoc_pnoc_mas,
	&pnoc_snoc_slv,
	&slv_srvc_snoc,
	&slv_qdss_stm,
	&slv_imem,
	&slv_apss,
	&slv_cats_0,
	&slv_cats_1,
};

static struct qcom_icc_desc msm8916_snoc = {
	.nodes = msm8916_snoc_nodes,
	.num_nodes = ARRAY_SIZE(msm8916_snoc_nodes),
};

static struct qcom_icc_node *msm8916_bimc_nodes[] = {
	&mas_apss,
	&mas_tcu0,
	&mas_tcu1,
	&mas_gfx,
	&bimc_snoc_mas,
	&snoc_bimc_0_slv,
	&snoc_bimc_1_slv,
	&slv_ebi_ch0,
	&slv_apps_l2,
};

static struct qcom_icc_desc msm8916_bimc = {
	.nodes = msm8916_bimc_nodes,
	.num_nodes = ARRAY_SIZE(msm8916_bimc_nodes),
};

static struct qcom_icc_node *msm8916_pnoc_nodes[] = {
	&snoc_pnoc_slv,
	&pnoc_int_0,
	&pnoc_int_1,
	&pnoc_m_0,
	&pnoc_m_1,
	&pnoc_s_0,
	&pnoc_s_1,
	&pnoc_s_2,
	&pnoc_s_3,
	&pnoc_s_4,
	&pnoc_s_8,
	&pnoc_s_9,
	&slv_imem_cfg,
	&slv_crypto_0_cfg,
	&slv_msg_ram,
	&slv_pdm,
	&slv_prng,
	&slv_clk_ctl,
	&slv_mss,
	&slv_tlmm,
	&slv_tcsr,
	&slv_security,
	&slv_spdm,
	&slv_pnoc_cfg,
	&slv_pmic_arb,
	&slv_bimc_cfg,
	&slv_boot_rom,
	&slv_mpm,
	&slv_qdss_cfg,
	&slv_rbcpr_cfg,
	&slv_snoc_cfg,
	&slv_dehr_cfg,
	&slv_venus_cfg,
	&slv_display_cfg,
	&slv_camera_cfg,
	&slv_usb_hs,
	&slv_sdcc_1,
	&slv_blsp_1,
	&slv_sdcc_2,
	&slv_gfx_cfg,
	&slv_audio,
	&mas_blsp_1,
	&mas_spdm,
	&mas_dehr,
	&mas_audio,
	&mas_usb_hs,
	&mas_pnoc_crypto_0,
	&mas_pnoc_sdcc_1,
	&mas_pnoc_sdcc_2,
	&pnoc_snoc_mas,
};

static struct qcom_icc_desc msm8916_pnoc = {
	.nodes = msm8916_pnoc_nodes,
	.num_nodes = ARRAY_SIZE(msm8916_pnoc_nodes),
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

	/* wait for RPM */
	if (!qcom_icc_rpm_smd_available())
		return -EPROBE_DEFER;

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
	{ .compatible = "qcom,msm8916-pnoc", .data = &msm8916_pnoc },
	{ .compatible = "qcom,msm8916-snoc", .data = &msm8916_snoc },
	{ .compatible = "qcom,msm8916-bimc", .data = &msm8916_bimc },
	{ },
};
MODULE_DEVICE_TABLE(of, qnoc_of_match);

static struct platform_driver qnoc_driver = {
	.probe = qnoc_probe,
	.remove = qnoc_remove,
	.driver = {
		.name = "qnoc-msm8916",
		.of_match_table = qnoc_of_match,
	},
};
module_platform_driver(qnoc_driver);
MODULE_AUTHOR("Georgi Djakov <georgi.djakov@linaro.org>");
MODULE_DESCRIPTION("Qualcomm msm8916 NoC driver");
MODULE_LICENSE("GPL v2");

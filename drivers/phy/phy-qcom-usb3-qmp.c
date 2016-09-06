/*
 * Copyright (c) 2016, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/slab.h>

#include "phy-qcom-usb3-qmp.h"

static void qcom_usb3phy_qmp_configure(struct qcom_usb3phy_qmp *uphy)
{
	int config_tbl_sz = ARRAY_SIZE(phy_init_config_tbl);
	int i;

	for (i = 0; i < config_tbl_sz; i++)
		writel_relaxed(phy_init_config_tbl[i].cfg_value,
				uphy->base + phy_init_config_tbl[i].reg_offset);

	/* flush buffered writes */
	mb();
}

/* SSPHY Initialization */
static int qcom_usb3phy_qmp_poweron(struct phy *phy)
{
	struct qcom_usb3phy_qmp *uphy = phy_get_drvdata(phy);
	unsigned init_timeout_usec = INIT_MAX_TIME_USEC;
	int ret;

	dev_err(&phy->dev, "Initializing QMP phy\n");

	ret = regulator_enable(uphy->vdda_phy);
	if (ret) {
		dev_err(&phy->dev, "%s: vdda-phy enable failed, err=%d\n",
				__func__, ret);
		return ret;
	}

	ret = regulator_enable(uphy->vdda_pll_1p8);
	if (ret) {
		dev_err(&phy->dev, "%s: vdda-pll-1p8 enable failed, err=%d\n",
				__func__, ret);
		return ret;
	}

	/* Deassert USB3 PHY CSR reset */
	if (uphy->phy_reset) {
		ret = reset_control_deassert(uphy->phy_reset);
		if (ret) {
			dev_err(&phy->dev, "phy_reset clk deassert failed\n");
			return ret;
		}
	}

	/* Deassert USB3 PHY reset */
	if (uphy->phy_phy_reset) {
		ret = reset_control_deassert(uphy->phy_phy_reset);
		if (ret) {
			dev_err(&phy->dev, "phy_phy reset deassert failed\n");
			return ret;
		}
	}

	if (!uphy->clk_enabled) {
		if (uphy->ref_clk_src)
			clk_prepare_enable(uphy->ref_clk_src);
		if (uphy->ref_clk)
			clk_prepare_enable(uphy->ref_clk);
		clk_prepare_enable(uphy->aux_clk);
		clk_prepare_enable(uphy->cfg_ahb_clk);
		clk_prepare_enable(uphy->pipe_clk);
		uphy->clk_enabled = true;
	}

	/*
	 * Pull out PHY from POWER DOWN state:
	 * This is active low enable signal to power-down PHY.
	 */
	writel_relaxed(PHY_SW_PWRDN_B,
			uphy->base + USB3_PHY_POWER_DOWN_CONTROL);
	mb();

	/* Main configuration */
	qcom_usb3phy_qmp_configure(uphy);

	/* start SerDes and Phy-Coding-Sublayer */
	writel_relaxed(PHY_SERDES_START | PHY_PCS_START,
			uphy->base + USB3_PHY_START_CTRL);
	mb();
	/* Pull PHY out of reset state */
	writel_relaxed(~PHY_SW_RESET, uphy->base + USB3_PHY_SW_RESET);
	mb();

	/* Wait for PHY initialization to be done */
	do {
		if (readl_relaxed(uphy->base + USB3_PHY_PCS_READY_STATUS) &
								MASK_PHYSTATUS)
			usleep_range(1, 2);
		else
			break;
	} while (--init_timeout_usec);

	if (!init_timeout_usec) {
		dev_err(&phy->dev, "USB3 QMP PHY initialization timeout\n");
		dev_err(&phy->dev, "USB3_PHY_PCS_READY_STATUS:%x\n",
				readl_relaxed(uphy->base +
						USB3_PHY_PCS_READY_STATUS));
		return -EBUSY;
	};

	return 0;
}

static int qcom_usb3phy_qmp_poweroff(struct phy *phy)
{
	struct qcom_usb3phy_qmp *uphy = phy_get_drvdata(phy);
	int ret;

	/* PHY reset */
	writel_relaxed(PHY_SW_RESET, uphy->base + USB3_PHY_SW_RESET);

	/* stop SerDes and Phy-Coding-Sublayer */
	writel_relaxed((~PHY_SERDES_START | ~PHY_PCS_START),
			uphy->base + USB3_PHY_START_CTRL);

	/* Put PHY into POWER DOWN state: active low */
	writel_relaxed(~PHY_SW_PWRDN_B,
			uphy->base + USB3_PHY_POWER_DOWN_CONTROL);
	mb();

	if (uphy->clk_enabled) {
		if (uphy->ref_clk_src)
			clk_disable_unprepare(uphy->ref_clk_src);
		if (uphy->ref_clk)
			clk_disable_unprepare(uphy->ref_clk);
		clk_disable_unprepare(uphy->aux_clk);
		clk_disable_unprepare(uphy->cfg_ahb_clk);
		clk_disable_unprepare(uphy->pipe_clk);
		uphy->clk_enabled = false;
	}

	/* assert USB3 PHY CSR reset */
	if (uphy->phy_reset) {
		ret = reset_control_assert(uphy->phy_reset);
		if (ret) {
			dev_err(&phy->dev, "phy_reset clk deassert failed\n");
			return ret;
		}
	}

	/* Deassert USB3 PHY reset */
	if (uphy->phy_phy_reset) {
		ret = reset_control_assert(uphy->phy_phy_reset);
		if (ret) {
			dev_err(&phy->dev, "phy_phy reset deassert failed\n");
			return ret;
		}
	}

	/* Enable regulators */
	regulator_disable(uphy->vdda_phy);
	regulator_disable(uphy->vdda_pll_1p8);

	return 0;
}

static
inline int qcom_usb3phy_qmp_regulator_init(struct qcom_usb3phy_qmp *uphy)
{
	int ret = 0;
	struct device *dev = &uphy->phy->dev;

	/* supply to PHY core block */
	uphy->vdda_phy = devm_regulator_get(dev, "vdda-phy");
	if (IS_ERR(uphy->vdda_phy)) {
		ret = PTR_ERR(uphy->vdda_phy);
		uphy->vdda_phy = NULL;
		dev_err(dev, "failed to get vdda-phy, %d\n", ret);
		goto err;
	}

	uphy->vdda_pll_1p8 = devm_regulator_get(dev, "vdda-pll-1p8");
	if (IS_ERR(uphy->vdda_pll_1p8)) {
		ret = PTR_ERR(uphy->vdda_pll_1p8);
		uphy->vdda_pll_1p8 = NULL;
		dev_err(dev, "failed to get vdda-pll-1p8, %d\n", ret);
	}

err:
	return ret;
}

static int qcom_usb3phy_qmp_clk_init(struct qcom_usb3phy_qmp *uphy)
{
	int ret = 0;
	struct device *dev = &uphy->phy->dev;

	uphy->aux_clk = devm_clk_get(dev, "aux_clk");
	if (IS_ERR(uphy->aux_clk)) {
		ret = PTR_ERR(uphy->aux_clk);
		uphy->aux_clk = NULL;
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to get aux_clk\n");
		return ret;
	}

	uphy->cfg_ahb_clk = devm_clk_get(dev, "cfg_ahb_clk");
	if (IS_ERR(uphy->cfg_ahb_clk)) {
		ret = PTR_ERR(uphy->cfg_ahb_clk);
		uphy->cfg_ahb_clk = NULL;
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to get cfg_ahb_clk\n");
		return ret;
	}

	uphy->pipe_clk = devm_clk_get(dev, "pipe_clk");
	if (IS_ERR(uphy->pipe_clk)) {
		ret = PTR_ERR(uphy->pipe_clk);
		uphy->pipe_clk = NULL;
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to get pipe_clk\n");
		return ret;
	}

	/*
	 * ref_clk and ref_clk_src handles may not be available in
	 * all hardwares. So we don't return error in these cases.
	 */
	uphy->ref_clk_src = devm_clk_get(dev, "ref_clk_src");
	if (IS_ERR(uphy->ref_clk_src)) {
		uphy->ref_clk_src = NULL;
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to get pipe_clk\n");
	}

	uphy->ref_clk = devm_clk_get(dev, "ref_clk");
	if (IS_ERR(uphy->ref_clk)) {
		uphy->ref_clk = NULL;
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to get pipe_clk\n");
	}

	return ret;
}

static const struct phy_ops qcom_usb3phy_qmp_gen_ops = {
	.power_on	= qcom_usb3phy_qmp_poweron,
	.power_off	= qcom_usb3phy_qmp_poweroff,
	.owner		= THIS_MODULE,
};

static int qcom_usb3phy_qmp_probe(struct platform_device *pdev)
{
	struct qcom_usb3phy_qmp *uphy;
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct phy *generic_phy;
	struct phy_provider *phy_provider;
	int ret = 0;

	uphy = devm_kzalloc(dev, sizeof(*uphy), GFP_KERNEL);
	if (!uphy) {
		dev_err(dev, "%s: failed to allocate usb3phy\n", __func__);
		return -ENOMEM;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "base");
	if (!res)
		return -ENODEV;
	uphy->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(uphy->base))
		return PTR_ERR(uphy->base);

	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	if (IS_ERR(phy_provider)) {
		ret = PTR_ERR(phy_provider);
		dev_err(dev, "%s: failed to register uphy %d\n", __func__, ret);
		return ret;
	}

	generic_phy = devm_phy_create(dev, NULL, &qcom_usb3phy_qmp_gen_ops);
	if (IS_ERR(generic_phy)) {
		ret = PTR_ERR(generic_phy);
		dev_err(dev, "%s: failed to create uphy %d\n", __func__, ret);
		generic_phy = NULL;
		return ret;
	}
	uphy->phy = generic_phy;

	/* initialize the clocks */
	ret = qcom_usb3phy_qmp_clk_init(uphy);
	if (ret) {
		dev_err(dev, "clock init failed \n");
		return ret;
	}

	/* regulator init */
	ret = qcom_usb3phy_qmp_regulator_init(uphy);
	if (ret) {
		dev_err(dev, "regulator init failed \n");
		return ret;
	}

	uphy->phy_reset = devm_reset_control_get_optional(dev, "phy_reset");
	if(IS_ERR(uphy->phy_reset)) {
		ret = PTR_ERR(uphy->phy_reset);
		uphy->phy_reset = NULL;
		dev_err(dev, "failed to get phy_reset\n");
		return ret;
	}

	uphy->phy_phy_reset = devm_reset_control_get_optional(dev,
							"phy_phy_reset");
	if(IS_ERR(uphy->phy_phy_reset)) {
		ret = PTR_ERR(uphy->phy_phy_reset);
		uphy->phy_phy_reset = NULL;
		dev_err(dev, "phy_phy_reset unavailable\n");
		return ret;
	}

	platform_set_drvdata(pdev, uphy);
	phy_set_drvdata(generic_phy, uphy);

	return 0;

}

static const struct of_device_id qcom_usb3phy_id_table[] = {
	{
		.compatible = "qcom,usb3phy-qmp",
	},
	{ },
};
MODULE_DEVICE_TABLE(of, qcom_usb3phy_id_table);

static struct platform_driver qcom_usb3phy_qmp_driver = {
	.probe		= qcom_usb3phy_qmp_probe,
	.driver = {
		.name	= "qcom_usb3phy_qmp",
		.of_match_table = of_match_ptr(qcom_usb3phy_id_table),
	},
};

module_platform_driver(qcom_usb3phy_qmp_driver);

MODULE_DESCRIPTION("Qualcomm USB3 QMP PHY driver");
MODULE_LICENSE("GPL v2");

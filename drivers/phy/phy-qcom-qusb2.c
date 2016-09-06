/*
 * Copyright (c) 2014-2016, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/phy/phy.h>
#include <linux/power_supply.h>
#include <linux/reset.h>

/* TCSR_PHY_CLK_SCHEME_SEL bit mask */
#define PHY_CLK_SCHEME_SEL BIT(0)

#define QUSB2PHY_PLL_STATUS	0x38
#define QUSB2PHY_PLL_LOCK	BIT(5)

#define QUSB2PHY_PORT_POWERDOWN		0xB4
#define CLAMP_N_EN			BIT(5)
#define FREEZIO_N			BIT(1)
#define POWER_DOWN			BIT(0)

#define QUSB2PHY_PLL_TEST		0x04
#define CLK_REF_SEL			BIT(7)

#define QUSB2PHY_PORT_TUNE1             0x80
#define QUSB2PHY_PORT_TUNE2             0x84
#define QUSB2PHY_PORT_TUNE3             0x88
#define QUSB2PHY_PORT_TUNE4             0x8C

/* In case Efuse register shows zero, use this value */
#define TUNE2_DEFAULT_HIGH_NIBBLE	0xB
#define TUNE2_DEFAULT_LOW_NIBBLE	0x3

/* Get TUNE2's high nibble value read from efuse */
#define TUNE2_HIGH_NIBBLE_VAL(val, pos, mask)	((val >> pos) & mask)

#define QUSB2PHY_1P8_VOL_MIN           1800000 /* uV */
#define QUSB2PHY_1P8_VOL_MAX           1800000 /* uV */
#define QUSB2PHY_1P8_HPM_LOAD          30000   /* uA */

#define QUSB2PHY_3P3_VOL_MIN		3075000 /* uV */
#define QUSB2PHY_3P3_VOL_MAX		3200000 /* uV */
#define QUSB2PHY_3P3_HPM_LOAD		30000	/* uA */

#define QUSB2PHY_REFCLK_ENABLE		BIT(0)

unsigned int tune2;
module_param(tune2, uint, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(tune2, "QUSB PHY TUNE2");

struct qusb2_phy {
	struct phy		*phy;
	void __iomem		*base;
	void __iomem		*qscratch_base;
	void __iomem		*tune2_efuse_reg;
	void __iomem		*ref_clk_base;
	void __iomem		*tcsr_phy_clk_scheme_sel;

	struct clk		*ref_clk_src;
	struct clk		*ref_clk;
	struct clk		*cfg_ahb_clk;
	struct reset_control    *phy_reset;

	struct regulator	*vdd;
	struct regulator	*vdda33;
	struct regulator	*vdda18;
	int			init_seq_len;
	int			*qusb2_phy_init_seq;

	u32			tune2_val;
	int			tune2_efuse_bit_pos;
	int			tune2_efuse_num_of_bits;

	bool			power_enabled;
	bool			clocks_enabled;
};

static void qusb2_phy_enable_clocks(struct qusb2_phy *qphy, bool on)
{
	dev_dbg(&qphy->phy->dev, "%s(): clocks_enabled:%d on:%d\n",
			__func__, qphy->clocks_enabled, on);

	if (!qphy->clocks_enabled && on) {
		clk_prepare_enable(qphy->ref_clk_src);
		clk_prepare_enable(qphy->cfg_ahb_clk);
		qphy->clocks_enabled = true;
	}

	if (qphy->clocks_enabled && !on) {
		clk_disable_unprepare(qphy->ref_clk_src);
		clk_disable_unprepare(qphy->cfg_ahb_clk);
		qphy->clocks_enabled = false;
	}

	dev_dbg(&qphy->phy->dev, "%s(): clocks_enabled:%d\n", __func__,
						qphy->clocks_enabled);
}

#if 0
static int qusb2_phy_config_vdd(struct qusb2_phy *qphy, int high)
{
	int min, ret = 0;

	min = high ? 1 : 0; /* low or none? */
	ret = regulator_set_voltage(qphy->vdd, 400000, 1015000);
	if (ret) {
		dev_err(&qphy->phy->dev, "unable to set voltage for qusb2 vdd\n");
		return ret;
	}

	dev_dbg(&qphy->phy->dev, "min_vol:%d max_vol:%d\n", 400000, 1015000);
	return ret;
}
#endif

static int qusb2_phy_enable_power(struct qusb2_phy *qphy, bool on)
{
	int ret = 0;

	dev_dbg(&qphy->phy->dev, "%s turn %s regulators. power_enabled:%d\n",
			__func__, on ? "on" : "off", qphy->power_enabled);

	if (qphy->power_enabled == on) {
		dev_dbg(&qphy->phy->dev, "PHYs' regulators are already ON.\n");
		return 0;
	}

	if (!on)
		goto disable_vdda33;

#if 0
	ret = qusb2_phy_config_vdd(qphy, true);
	if (ret) {
		dev_err(&qphy->phy->dev, "Unable to config VDD:%d\n",
							ret);
		goto err_vdd;
	}
#endif

	ret = regulator_enable(qphy->vdd);
	if (ret) {
		dev_err(&qphy->phy->dev, "Unable to enable VDD\n");
		goto err_vdd;
	}

#if 0
	ret = regulator_set_load(qphy->vdda18, QUSB2PHY_1P8_HPM_LOAD);
	if (ret < 0) {
		dev_err(&qphy->phy->dev, "Unable to set HPM of vdda18:%d\n", ret);
		goto disable_vdd;
	}

	ret = regulator_set_voltage(qphy->vdda18, QUSB2PHY_1P8_VOL_MIN,
						QUSB2PHY_1P8_VOL_MAX);
	if (ret) {
		dev_err(&qphy->phy->dev,
				"Unable to set voltage for vdda18:%d\n", ret);
		goto put_vdda18_lpm;
	}
#endif

	ret = regulator_enable(qphy->vdda18);
	if (ret) {
		dev_err(&qphy->phy->dev, "Unable to enable vdda18:%d\n", ret);
		goto disable_vdd;
	}

#if 0
	ret = regulator_set_load(qphy->vdda33, QUSB2PHY_3P3_HPM_LOAD);
	if (ret < 0) {
		dev_err(&qphy->phy->dev, "Unable to set HPM of vdda33:%d\n", ret);
		goto disable_vdda18;
	}

	ret = regulator_set_voltage(qphy->vdda33, QUSB2PHY_3P3_VOL_MIN,
						QUSB2PHY_3P3_VOL_MAX);
	if (ret) {
		dev_err(&qphy->phy->dev,
				"Unable to set voltage for vdda33:%d\n", ret);
		goto put_vdda33_lpm;
	}
#endif

	ret = regulator_enable(qphy->vdda33);
	if (ret) {
		dev_err(&qphy->phy->dev, "Unable to enable vdda33:%d\n", ret);
		goto disable_vdda18;
	}

	qphy->power_enabled = true;

	pr_debug("%s(): QUSB PHY's regulators are turned ON.\n", __func__);
	return ret;

disable_vdda33:
	ret = regulator_disable(qphy->vdda33);
	if (ret)
		dev_err(&qphy->phy->dev, "Unable to disable vdda33:%d\n", ret);

#if 0
unset_vdd33:
	ret = regulator_set_voltage(qphy->vdda33, 0, QUSB2PHY_3P3_VOL_MAX);
	if (ret)
		dev_err(&qphy->phy->dev,
			"Unable to set (0) voltage for vdda33:%d\n", ret);

put_vdda33_lpm:
	ret = regulator_set_load(qphy->vdda33, 0);
	if (ret < 0)
		dev_err(&qphy->phy->dev, "Unable to set (0) HPM of vdda33\n");
#endif

disable_vdda18:
	ret = regulator_disable(qphy->vdda18);
	if (ret)
		dev_err(&qphy->phy->dev, "Unable to disable vdda18:%d\n", ret);

#if 0
unset_vdda18:
	ret = regulator_set_voltage(qphy->vdda18, 0, QUSB2PHY_1P8_VOL_MAX);
	if (ret)
		dev_err(&qphy->phy->dev,
			"Unable to set (0) voltage for vdda18:%d\n", ret);

put_vdda18_lpm:
	ret = regulator_set_load(qphy->vdda18, 0);
	if (ret < 0)
		dev_err(&qphy->phy->dev, "Unable to set LPM of vdda18\n");
#endif

disable_vdd:
	ret = regulator_disable(qphy->vdd);
	if (ret)
		dev_err(&qphy->phy->dev, "Unable to disable vdd:%d\n",
								ret);
#if 0
unconfig_vdd:
	ret = qusb2_phy_config_vdd(qphy, false);
	if (ret)
		dev_err(&qphy->phy->dev, "Unable unconfig VDD:%d\n",
								ret);
#endif
err_vdd:
	qphy->power_enabled = false;
	dev_dbg(&qphy->phy->dev, "QUSB PHY's regulators are turned OFF.\n");
	return ret;
}

static void qusb2_phy_get_tune2_param(struct qusb2_phy *qphy)
{
	u8 num_of_bits;
	u32 bit_mask = 1;

	pr_debug("%s(): num_of_bits:%d bit_pos:%d\n", __func__,
				qphy->tune2_efuse_num_of_bits,
				qphy->tune2_efuse_bit_pos);

	/* get bit mask based on number of bits to use with efuse reg */
	if (qphy->tune2_efuse_num_of_bits) {
		num_of_bits = qphy->tune2_efuse_num_of_bits;
		bit_mask = (bit_mask << num_of_bits) - 1;
	}

	/*
	 * Read EFUSE register having TUNE2 parameter's high nibble.
	 * If efuse register shows value as 0x0, then use default value
	 * as 0xB as high nibble. Otherwise use efuse register based
	 * value for this purpose.
	 */
	qphy->tune2_val = readl_relaxed(qphy->tune2_efuse_reg);
	pr_debug("%s(): bit_mask:%d efuse based tune2 value:%d\n",
				__func__, bit_mask, qphy->tune2_val);

	qphy->tune2_val = TUNE2_HIGH_NIBBLE_VAL(qphy->tune2_val,
				qphy->tune2_efuse_bit_pos, bit_mask);

	if (!qphy->tune2_val)
		qphy->tune2_val = TUNE2_DEFAULT_HIGH_NIBBLE;

	/* Get TUNE2 byte value using high and low nibble value */
	qphy->tune2_val = ((qphy->tune2_val << 0x4) |
					TUNE2_DEFAULT_LOW_NIBBLE);
}

static void qusb2_phy_write_seq(void __iomem *base, u32 *seq, int cnt,
		unsigned long delay)
{
	int i;

	pr_debug("Seq count:%d\n", cnt);
	for (i = 0; i < cnt; i = i+2) {
		pr_debug("write 0x%02x to 0x%02x\n", seq[i], seq[i+1]);
		writel_relaxed(seq[i], base + seq[i+1]);
		if (delay)
			usleep_range(delay, (delay + 2000));
	}
}

static int qusb2_phy_init(struct phy *phy)
{
	struct qusb2_phy *qphy = phy_get_drvdata(phy);
	int ret, reset_val = 0;
	bool is_se_clk = true;

	dev_dbg(&phy->dev, "%s\n", __func__);

	ret = qusb2_phy_enable_power(qphy, true);
	if (ret)
		return ret;

	qusb2_phy_enable_clocks(qphy, true);

	/*
	 * ref clock is enabled by default after power on reset. Linux clock
	 * driver will disable this clock as part of late init if peripheral
	 * driver(s) does not explicitly votes for it. Linux clock driver also
	 * does not disable the clock until late init even if peripheral
	 * driver explicitly requests it and cannot defer the probe until late
	 * init. Hence, Explicitly disable the clock using register write to
	 * allow QUSB PHY PLL to lock properly.
	 */
	if (qphy->ref_clk_base) {
		writel_relaxed((readl_relaxed(qphy->ref_clk_base) &
					~QUSB2PHY_REFCLK_ENABLE),
					qphy->ref_clk_base);
		/* Make sure that above write complete to get ref clk OFF */
		wmb();
	}

	/* Perform phy reset */
	ret = reset_control_assert(qphy->phy_reset);
	if (ret) {
		dev_err(&phy->dev, "Failed to assert phy_reset\n");
		return ret;
	}
	usleep_range(100, 150);
	ret = reset_control_deassert(qphy->phy_reset);
	if (ret) {
		dev_err(&phy->dev, "Failed to de-assert phy_reset\n");
		return ret;
	}

	/* Disable the PHY */
	writel_relaxed(CLAMP_N_EN | FREEZIO_N | POWER_DOWN,
			qphy->base + QUSB2PHY_PORT_POWERDOWN);

	/* save reset value to override based on clk scheme */
	reset_val = readl_relaxed(qphy->base + QUSB2PHY_PLL_TEST);

	if (qphy->qusb2_phy_init_seq)
		qusb2_phy_write_seq(qphy->base, qphy->qusb2_phy_init_seq,
				qphy->init_seq_len, 0);

	/*
	 * Check for EFUSE value only if tune2_efuse_reg is available
	 * and try to read EFUSE value only once i.e. not every USB
	 * cable connect case.
	 */
	if (qphy->tune2_efuse_reg) {
		if (!qphy->tune2_val)
			qusb2_phy_get_tune2_param(qphy);

		pr_debug("%s(): Programming TUNE2 parameter as:%x\n", __func__,
				qphy->tune2_val);
		writel_relaxed(qphy->tune2_val,
				qphy->base + QUSB2PHY_PORT_TUNE2);
	}

	/* If tune2 modparam set, override tune2 value */
	if (tune2) {
		pr_debug("%s(): (modparam) TUNE2 val:0x%02x\n",
						__func__, tune2);
		writel_relaxed(tune2,
				qphy->base + QUSB2PHY_PORT_TUNE2);
	}

	/* ensure above writes are completed before re-enabling PHY */
	wmb();

	/* Enable the PHY */
	writel_relaxed(CLAMP_N_EN | FREEZIO_N,
		qphy->base + QUSB2PHY_PORT_POWERDOWN);

	/* Ensure above write is completed before turning ON ref clk */
	wmb();

	/* Require to get phy pll lock successfully */
	usleep_range(150, 160);

	if (!is_se_clk)
		reset_val &= ~CLK_REF_SEL;
	else
		reset_val |= CLK_REF_SEL;

	/* Turn on phy ref_clk if DIFF_CLK else select SE_CLK */
	if (!is_se_clk && qphy->ref_clk_base)
		writel_relaxed((readl_relaxed(qphy->ref_clk_base) |
					QUSB2PHY_REFCLK_ENABLE),
					qphy->ref_clk_base);
	else
		writel_relaxed(reset_val, qphy->base + QUSB2PHY_PLL_TEST);

	/* Make sure that above write is completed to get PLL source clock */
	wmb();

	/* Required to get PHY PLL lock successfully */
	usleep_range(100, 110);

	if (!(readb_relaxed(qphy->base + QUSB2PHY_PLL_STATUS) &
					QUSB2PHY_PLL_LOCK)) {
		dev_err(&phy->dev, "QUSB PHY PLL LOCK fails:%x\n",
			readb_relaxed(qphy->base + QUSB2PHY_PLL_STATUS));
		WARN_ON(1);
	}

	return 0;
}

static int qusb2_phy_exit(struct phy *phy)
{
	struct qusb2_phy *qphy = phy_get_drvdata(phy);

	dev_dbg(&phy->dev, "%s\n", __func__);

	qusb2_phy_enable_clocks(qphy, true);

	/* Disable the PHY */
	writel_relaxed(CLAMP_N_EN | FREEZIO_N | POWER_DOWN,
			qphy->base + QUSB2PHY_PORT_POWERDOWN);
	wmb();

	qusb2_phy_enable_clocks(qphy, false);

	return 0;
}

static const struct phy_ops qusb2_phy_gen_ops = {
	.init		= qusb2_phy_init,
	.exit		= qusb2_phy_exit,
	.owner		= THIS_MODULE,
};

static int qusb2_phy_probe(struct platform_device *pdev)
{
	struct qusb2_phy *qphy;
	struct device *dev = &pdev->dev;
	struct resource *res;
	int ret = 0, size = 0;
	bool hold_phy_reset;
	struct phy *generic_phy;
	struct phy_provider *phy_provider;

	qphy = devm_kzalloc(dev, sizeof(*qphy), GFP_KERNEL);
	if (!qphy)
		return -ENOMEM;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							"qusb2_phy_base");
	qphy->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(qphy->base))
		return PTR_ERR(qphy->base);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							"qscratch_base");
	if (res) {
		qphy->qscratch_base = devm_ioremap_resource(dev, res);
		if (IS_ERR(qphy->qscratch_base)) {
			dev_dbg(dev, "couldn't ioremap qscratch_base\n");
			qphy->qscratch_base = NULL;
		}
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							"tune2_efuse_addr");
	if (res) {
		qphy->tune2_efuse_reg = devm_ioremap_nocache(dev, res->start,
							resource_size(res));
		if (!IS_ERR_OR_NULL(qphy->tune2_efuse_reg)) {
			ret = of_property_read_u32(dev->of_node,
					"qcom,tune2-efuse-bit-pos",
					&qphy->tune2_efuse_bit_pos);
			if (!ret) {
				ret = of_property_read_u32(dev->of_node,
						"qcom,tune2-efuse-num-bits",
						&qphy->tune2_efuse_num_of_bits);
			}

			if (ret) {
				dev_err(dev, "DT Value for tune2 efuse is invalid.\n");
				return -EINVAL;
			}
		}
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							"ref_clk_addr");
	if (res) {
		qphy->ref_clk_base = devm_ioremap_nocache(dev,
				res->start, resource_size(res));
		if (IS_ERR(qphy->ref_clk_base))
			dev_dbg(dev, "ref_clk_address is not available.\n");
	}

	qphy->ref_clk_src = devm_clk_get(dev, "ref_clk_src");
	if (IS_ERR(qphy->ref_clk_src))
		dev_dbg(dev, "clk get failed for ref_clk_src\n");

	qphy->ref_clk = devm_clk_get(dev, "ref_clk");
	if (IS_ERR(qphy->ref_clk))
		dev_dbg(dev, "clk get failed for ref_clk\n");
	else
		clk_set_rate(qphy->ref_clk, 19200000);

	qphy->cfg_ahb_clk = devm_clk_get(dev, "cfg_ahb_clk");
	if (IS_ERR(qphy->cfg_ahb_clk))
		return PTR_ERR(qphy->cfg_ahb_clk);

	qphy->phy_reset = devm_reset_control_get(&pdev->dev, "phy_reset");
	if (IS_ERR(qphy->phy_reset))
		return PTR_ERR(qphy->phy_reset);

	of_get_property(dev->of_node, "qcom,qusb2-phy-init-seq", &size);
	if (size) {
		qphy->qusb2_phy_init_seq = devm_kzalloc(dev,
						size, GFP_KERNEL);
		if (qphy->qusb2_phy_init_seq) {
			qphy->init_seq_len =
				(size / sizeof(*qphy->qusb2_phy_init_seq));
			if (qphy->init_seq_len % 2) {
				dev_err(dev, "invalid init_seq_len\n");
				return -EINVAL;
			}

			of_property_read_u32_array(dev->of_node,
				"qcom,qusb2-phy-init-seq",
				qphy->qusb2_phy_init_seq,
				qphy->init_seq_len);
		} else {
			dev_err(dev, "error allocating memory for phy_init_seq\n");
		}
	}

	hold_phy_reset = of_property_read_bool(dev->of_node, "qcom,hold-reset");

	qphy->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(qphy->vdd)) {
		dev_err(dev, "unable to get vdd supply\n");
		return PTR_ERR(qphy->vdd);
	}

	qphy->vdda33 = devm_regulator_get(dev, "vdda33");
	if (IS_ERR(qphy->vdda33)) {
		dev_err(dev, "unable to get vdda33 supply\n");
		return PTR_ERR(qphy->vdda33);
	}

	qphy->vdda18 = devm_regulator_get(dev, "vdda18");
	if (IS_ERR(qphy->vdda18)) {
		dev_err(dev, "unable to get vdda18 supply\n");
		return PTR_ERR(qphy->vdda18);
	}

	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	if (IS_ERR(phy_provider)) {
		ret = PTR_ERR(phy_provider);
		dev_err(dev, "%s: failed to register phy %d\n", __func__, ret);
		return ret;
	}

	generic_phy = devm_phy_create(dev, NULL, &qusb2_phy_gen_ops);
	if (IS_ERR(generic_phy)) {
		ret = PTR_ERR(generic_phy);
		dev_err(dev, "%s: failed to create phy %d\n", __func__, ret);
		generic_phy = NULL;
		return ret;
	}

	qphy->phy = generic_phy;

	platform_set_drvdata(pdev, qphy);
	phy_set_drvdata(generic_phy, qphy);

	/*
	 * On some platforms multiple QUSB PHYs are available. If QUSB PHY is
	 * not used, there is leakage current seen with QUSB PHY related voltage
	 * rail. Hence keep QUSB PHY into reset state explicitly here.
	 */
	if (hold_phy_reset) {
		ret = reset_control_assert(qphy->phy_reset);
		if (ret) {
			dev_err(&pdev->dev, "Failed to assert phy reset\n");
			return ret;
		}
	}

	return ret;
}

static int qusb2_phy_remove(struct platform_device *pdev)
{
	struct qusb2_phy *qphy = platform_get_drvdata(pdev);

	if (qphy->clocks_enabled) {
		clk_disable_unprepare(qphy->cfg_ahb_clk);
		clk_disable_unprepare(qphy->ref_clk);
		clk_disable_unprepare(qphy->ref_clk_src);
		qphy->clocks_enabled = false;
	}

	qusb2_phy_enable_power(qphy, false);

	return 0;
}

static const struct of_device_id qusb2_phy_id_table[] = {
	{ .compatible = "qcom,qusb2phy", },
	{ },
};
MODULE_DEVICE_TABLE(of, qusb2_phy_id_table);

static struct platform_driver qusb2_phy_driver = {
	.probe		= qusb2_phy_probe,
	.remove		= qusb2_phy_remove,
	.driver = {
		.name	= "msm-qusb2-phy",
		.of_match_table = of_match_ptr(qusb2_phy_id_table),
	},
};

module_platform_driver(qusb2_phy_driver);

MODULE_DESCRIPTION("Qualcomm QUSB2 PHY driver");
MODULE_LICENSE("GPL v2");

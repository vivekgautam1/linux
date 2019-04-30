// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/spmi.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>

#define REG_REVISION_2			0x01
#define REG_PERPH_TYPE			0x04
#define qcom_LAB_TYPE			0x24
#define qcom_IBB_TYPE			0x20
#define REG_LAB_STATUS1			0x08
#define REG_LAB_ENABLE_CTL		0x46
#define LAB_STATUS1_VREG_OK_BIT		BIT(7)
#define LAB_STATUS1_SC_DETECT_BIT	BIT(6)
#define LAB_ENABLE_CTL_EN		BIT(7)
#define REG_IBB_STATUS1			0x08
#define REG_IBB_ENABLE_CTL		0x46
#define IBB_STATUS1_VREG_OK_BIT		BIT(7)
#define IBB_STATUS1_SC_DETECT_BIT	BIT(6)
#define IBB_ENABLE_CTL_MASK		(BIT(7) | BIT(6))
#define IBB_ENABLE_CTL_MODULE_EN	BIT(7)

#define SC_ERR_COUNT_INTERVAL_SEC	1
#define POLLING_SCP_DONE_COUNT		2
#define POLLING_SCP_DONE_INTERVAL_MS	5
#define SC_FAULT_COUNT_MAX		4
#define IBB_SW_CONTROL_EN		1
#define IBB_SW_CONTROL_DIS		0

static const int ibb_pwrup_dly_table[] = {
	1000,
	2000,
	4000,
	8000,
};

struct lab_regulator {
	struct regulator_desc		rdesc;
	struct regulator_dev		*rdev;
	struct mutex			lab_mutex;
	int				lab_sc_irq;
	int				soft_start;
	int				vreg_enabled;
};

struct ibb_regulator {
	struct regulator_desc		rdesc;
	struct regulator_dev		*rdev;
	struct mutex			ibb_mutex;
	int				ibb_sc_irq;
	int				soft_start;
	u32				pwrup_dly;
	int				vreg_enabled;
};

struct qcom_labibb {
	struct device			*dev;
	struct regmap			*regmap;
	u16				lab_base;
	u16				ibb_base;
	u8				lab_dig_major;
	u8				ibb_dig_major;
	struct lab_regulator		lab_vreg;
	struct ibb_regulator		ibb_vreg;
	struct mutex			bus_mutex;
	struct hrtimer			sc_err_check_timer;
	int				sc_err_count;
};

static int qcom_labibb_read(struct qcom_labibb *labibb, u16 address,
			    u8 *val, int count)
{
	int ret;

	mutex_lock(&labibb->bus_mutex);
	ret = regmap_bulk_read(labibb->regmap, address, val, count);
	if (ret < 0)
		dev_err(labibb->dev, "SPMI read failed address=0x%02x ret=%d\n",
			address, ret);
	mutex_unlock(&labibb->bus_mutex);

	return ret;
}

static int qcom_labibb_masked_write(struct qcom_labibb *labibb, u16 address,
				    u8 mask, u8 val)
{
	int ret;

	mutex_lock(&labibb->bus_mutex);
	if (address == 0) {
		dev_err(labibb->dev, "address cannot be zero address=0x%02x\n",
			address);
		ret = -EINVAL;
		goto error;
	}

	ret = regmap_update_bits(labibb->regmap, address, mask, val);
	if (ret < 0)
		dev_err(labibb->dev, "spmi write failed: addr=%03X, ret=%d\n",
			address, ret);

error:
	mutex_unlock(&labibb->bus_mutex);

	return ret;
}

static int qcom_ibb_set_mode(struct qcom_labibb *labibb, int mode)
{
	int ret;
	u8 val;

	if (mode == IBB_SW_CONTROL_EN)
		val = IBB_ENABLE_CTL_MODULE_EN;
	else if (mode == IBB_SW_CONTROL_DIS)
		val = 0;
	else
		return -EINVAL;

	ret = qcom_labibb_masked_write(labibb,
				       labibb->ibb_base + REG_IBB_ENABLE_CTL,
				       IBB_ENABLE_CTL_MASK, val);
	if (ret < 0)
		dev_err(labibb->dev, "Unable to configure IBB_ENABLE_CTL ret=%d\n",
			ret);

	return ret;
}

static int qcom_labibb_regulator_enable(struct qcom_labibb *labibb)
{
	int ret, dly, retries;
	u8 val;
	bool enabled = false;

	ret = qcom_ibb_set_mode(labibb, IBB_SW_CONTROL_EN);
	if (ret) {
		dev_err(labibb->dev, "Unable to set IBB_MODULE_EN ret = %d\n",
			ret);
		return ret;
	}

	/* total delay time */
	dly = labibb->lab_vreg.soft_start + labibb->ibb_vreg.soft_start
				+ labibb->ibb_vreg.pwrup_dly;
	/* after this delay, lab should be enabled */
	usleep_range(dly, dly + 100);

	ret = qcom_labibb_read(labibb, labibb->lab_base + REG_LAB_STATUS1,
			       &val, 1);
	if (ret < 0) {
		dev_err(labibb->dev, "read register %x failed ret = %d\n",
			REG_LAB_STATUS1, ret);
		goto err_out;
	}

	dev_dbg(labibb->dev, "soft=%d %d up=%d dly=%d\n",
		labibb->lab_vreg.soft_start, labibb->ibb_vreg.soft_start,
		labibb->ibb_vreg.pwrup_dly, dly);

	if (!(val & LAB_STATUS1_VREG_OK_BIT)) {
		dev_err(labibb->dev, "failed for LAB %x\n", val);
		goto err_out;
	}

	/* poll IBB_STATUS to make sure ibb had been enabled */
	dly = labibb->ibb_vreg.soft_start + labibb->ibb_vreg.pwrup_dly;
	retries = 10;
	while (retries--) {
		ret = qcom_labibb_read(labibb, labibb->ibb_base +
					REG_IBB_STATUS1, &val, 1);
		if (ret < 0) {
			dev_err(labibb->dev, "read register %x failed ret = %d\n",
				REG_IBB_STATUS1, ret);
			goto err_out;
		}

		if (val & IBB_STATUS1_VREG_OK_BIT) {
			enabled = true;
			break;
		}
		usleep_range(dly, dly + 100);
	}

	if (!enabled) {
		dev_err(labibb->dev, "failed for IBB %x\n", val);
		goto err_out;
	}

	labibb->lab_vreg.vreg_enabled = 1;
	labibb->ibb_vreg.vreg_enabled = 1;

	return 0;
err_out:
	ret = qcom_ibb_set_mode(labibb, IBB_SW_CONTROL_DIS);
	if (ret < 0) {
		dev_err(labibb->dev, "Unable to set IBB_MODULE_EN ret = %d\n",
			ret);
		return ret;
	}
	return -EINVAL;
}

static int qcom_labibb_regulator_disable(struct qcom_labibb *labibb)
{
	int ret, retries;
	u8 val;
	bool disabled = false;

	ret = qcom_ibb_set_mode(labibb, IBB_SW_CONTROL_DIS);
	if (ret < 0) {
		dev_err(labibb->dev, "Unable to set IBB_MODULE_EN ret = %d\n",
			ret);
		return ret;
	}

	/* poll IBB_STATUS to make sure ibb had been disabled */
	retries = 2;
	while (retries--) {
		ret = qcom_labibb_read(labibb, labibb->ibb_base +
				      REG_IBB_STATUS1, &val, 1);
		if (ret < 0) {
			dev_err(labibb->dev, "read register %x failed ret = %d\n",
				REG_IBB_STATUS1, ret);
			return ret;
		}

		if (!(val & IBB_STATUS1_VREG_OK_BIT)) {
			disabled = true;
			break;
		}
	}

	if (!disabled) {
		dev_err(labibb->dev, "failed for IBB %x\n", val);
		return -EINVAL;
	}

	labibb->lab_vreg.vreg_enabled = 0;
	labibb->ibb_vreg.vreg_enabled = 0;

	return 0;
}

static int qcom_lab_regulator_enable(struct regulator_dev *rdev)
{
	struct qcom_labibb *labibb  = rdev_get_drvdata(rdev);

	if (!labibb->lab_vreg.vreg_enabled)
		return qcom_labibb_regulator_enable(labibb);

	return 0;
}

static int qcom_lab_regulator_disable(struct regulator_dev *rdev)
{
	struct qcom_labibb *labibb  = rdev_get_drvdata(rdev);

	if (labibb->lab_vreg.vreg_enabled)
		return qcom_labibb_regulator_disable(labibb);

	return 0;
}

static int qcom_lab_regulator_is_enabled(struct regulator_dev *rdev)
{
	struct qcom_labibb *labibb  = rdev_get_drvdata(rdev);

	return labibb->lab_vreg.vreg_enabled;
}

static irqreturn_t labibb_sc_err_handler(int irq, void *_labibb)
{
	int ret, count;
	u16 reg;
	u8 sc_err_mask, val;
	char *str;
	struct qcom_labibb *labibb = (struct qcom_labibb *)_labibb;
	bool in_sc_err, lab_en, ibb_en, scp_done = false;

	if (irq == labibb->lab_vreg.lab_sc_irq) {
		reg = labibb->lab_base + REG_LAB_STATUS1;
		sc_err_mask = LAB_STATUS1_SC_DETECT_BIT;
		str = "LAB";
	} else if (irq == labibb->ibb_vreg.ibb_sc_irq) {
		reg = labibb->ibb_base + REG_IBB_STATUS1;
		sc_err_mask = IBB_STATUS1_SC_DETECT_BIT;
		str = "IBB";
	} else {
		return IRQ_HANDLED;
	}

	ret = qcom_labibb_read(labibb, reg, &val, 1);
	if (ret < 0) {
		dev_err(labibb->dev, "Read 0x%x failed, ret=%d\n", reg, ret);
		return IRQ_HANDLED;
	}
	dev_dbg(labibb->dev, "%s SC error triggered! %s_STATUS1 = %d\n",
		str, str, val);

	in_sc_err = !!(val & sc_err_mask);

	if (!in_sc_err) {
		count = POLLING_SCP_DONE_COUNT;
		do {
			reg = labibb->lab_base + REG_LAB_ENABLE_CTL;
			ret = qcom_labibb_read(labibb, reg, &val, 1);
			if (ret < 0) {
				dev_err(labibb->dev,
					"Read 0x%x failed, ret=%d\n", reg, ret);
				return IRQ_HANDLED;
			}
			lab_en = !!(val & LAB_ENABLE_CTL_EN);

			reg = labibb->ibb_base + REG_IBB_ENABLE_CTL;
			ret = qcom_labibb_read(labibb, reg, &val, 1);
			if (ret < 0) {
				dev_err(labibb->dev,
					"Read 0x%x failed, ret=%d\n", reg, ret);
				return IRQ_HANDLED;
			}
			ibb_en = !!(val & IBB_ENABLE_CTL_MODULE_EN);
			if (lab_en || ibb_en)
				msleep(POLLING_SCP_DONE_INTERVAL_MS);
			else
				break;
		} while ((lab_en || ibb_en) && count--);

		if (labibb->lab_vreg.vreg_enabled &&
		    labibb->ibb_vreg.vreg_enabled &&
		    !lab_en && !ibb_en) {
			dev_dbg(labibb->dev, "LAB/IBB has been disabled by SCP\n");
			scp_done = true;
		}
	}

	if (in_sc_err || scp_done) {
		if (hrtimer_active(&labibb->sc_err_check_timer) ||
		    hrtimer_callback_running(&labibb->sc_err_check_timer)) {
			labibb->sc_err_count++;
		} else {
			labibb->sc_err_count = 1;
			hrtimer_start(&labibb->sc_err_check_timer,
				      ktime_set(SC_ERR_COUNT_INTERVAL_SEC, 0),
				      HRTIMER_MODE_REL);
		}
	}

	return IRQ_HANDLED;
}

static enum hrtimer_restart labibb_check_sc_err_count(struct hrtimer *timer)
{
	struct qcom_labibb *labibb = container_of(timer,
			struct qcom_labibb, sc_err_check_timer);

	/*
	 * if SC fault triggers more than 4 times in 1 second,
	 * then disable the IRQs and leave as it.
	 */
	if (labibb->sc_err_count > SC_FAULT_COUNT_MAX) {
		disable_irq(labibb->lab_vreg.lab_sc_irq);
		disable_irq(labibb->ibb_vreg.ibb_sc_irq);
	}

	return HRTIMER_NORESTART;
}

static struct regulator_ops qcom_lab_ops = {
	.enable			= qcom_lab_regulator_enable,
	.disable		= qcom_lab_regulator_disable,
	.is_enabled		= qcom_lab_regulator_is_enabled,
};

static int register_qcom_lab_regulator(struct qcom_labibb *labibb,
				       struct device_node *of_node)
{
	int ret;
	struct regulator_init_data *init_data;
	struct regulator_desc *rdesc = &labibb->lab_vreg.rdesc;
	struct regulator_config cfg = {};

	if (!of_node) {
		dev_err(labibb->dev, "qcom lab regulator device tree node is missing\n");
		return -EINVAL;
	}

	init_data = of_get_regulator_init_data(labibb->dev, of_node, rdesc);
	if (!init_data) {
		dev_err(labibb->dev, "unable to get regulator init data for qcom lab regulator\n");
		return -ENOMEM;
	}
	if (labibb->lab_vreg.lab_sc_irq != -EINVAL) {
		ret = devm_request_threaded_irq(labibb->dev,
						labibb->lab_vreg.lab_sc_irq,
						NULL, labibb_sc_err_handler,
						IRQF_ONESHOT |
						IRQF_TRIGGER_RISING,
						"lab-sc-err", labibb);
		if (ret) {
			dev_err(labibb->dev, "Failed to register 'lab-sc-err' irq ret=%d\n",
				ret);
			return ret;
		}
	}

	if (init_data->constraints.name) {
		rdesc->owner		= THIS_MODULE;
		rdesc->type		= REGULATOR_VOLTAGE;
		rdesc->ops		= &qcom_lab_ops;
		rdesc->name		= init_data->constraints.name;

		cfg.dev = labibb->dev;
		cfg.init_data = init_data;
		cfg.driver_data = labibb;
		cfg.of_node = of_node;

		if (of_get_property(labibb->dev->of_node, "parent-supply",
				    NULL))
			init_data->supply_regulator = "parent";

		init_data->constraints.valid_ops_mask |=
					REGULATOR_CHANGE_VOLTAGE |
					REGULATOR_CHANGE_STATUS;

		labibb->lab_vreg.rdev = regulator_register(rdesc, &cfg);
		if (IS_ERR(labibb->lab_vreg.rdev)) {
			ret = PTR_ERR(labibb->lab_vreg.rdev);
			labibb->lab_vreg.rdev = NULL;
			dev_err(labibb->dev, "unable to get regulator init data for qcom lab regulator, ret = %d\n",
				ret);
			return ret;
		}
	} else {
		dev_err(labibb->dev, "qcom lab regulator name missing\n");
		return -EINVAL;
	}

	return 0;
}

static int qcom_ibb_dt_init(struct qcom_labibb *labibb,
			    struct device_node *of_node)
{
	int ret;
	u32 i = 0, tmp = 0;

	ret = of_property_read_u32(of_node,
				   "qcom,ibb-lab-pwrup-delay", &tmp);
	if (!ret) {
		if (tmp > 0) {
			for (i = 0; i < ARRAY_SIZE(ibb_pwrup_dly_table); i++) {
				if (ibb_pwrup_dly_table[i] == tmp)
					break;
			}

			if (i == ARRAY_SIZE(ibb_pwrup_dly_table)) {
				dev_err(labibb->dev, "Invalid value in qcom,qcom-ibb-lab-pwrup-delay\n");
				return -EINVAL;
			}
		}

		labibb->ibb_vreg.pwrup_dly = tmp;
	}

	return 0;
}

static int qcom_ibb_regulator_enable(struct regulator_dev *rdev)
{
	struct qcom_labibb *labibb  = rdev_get_drvdata(rdev);

	if (!labibb->ibb_vreg.vreg_enabled)
		return qcom_labibb_regulator_enable(labibb);

	return 0;
}

static int qcom_ibb_regulator_disable(struct regulator_dev *rdev)
{
	struct qcom_labibb *labibb  = rdev_get_drvdata(rdev);

	if (labibb->ibb_vreg.vreg_enabled)
		return qcom_labibb_regulator_disable(labibb);
	return 0;
}

static int qcom_ibb_regulator_is_enabled(struct regulator_dev *rdev)
{
	struct qcom_labibb *labibb  = rdev_get_drvdata(rdev);

	return labibb->ibb_vreg.vreg_enabled;
}

static struct regulator_ops qcom_ibb_ops = {
	.enable			= qcom_ibb_regulator_enable,
	.disable		= qcom_ibb_regulator_disable,
	.is_enabled		= qcom_ibb_regulator_is_enabled,
};

static int register_qcom_ibb_regulator(struct qcom_labibb *labibb,
				       struct device_node *of_node)
{
	int ret;
	struct regulator_init_data *init_data;
	struct regulator_desc *rdesc = &labibb->ibb_vreg.rdesc;
	struct regulator_config cfg = {};

	if (!of_node) {
		dev_err(labibb->dev, "qcom ibb regulator device tree node is missing\n");
		return -EINVAL;
	}

	init_data = of_get_regulator_init_data(labibb->dev, of_node, rdesc);
	if (!init_data) {
		dev_err(labibb->dev, "unable to get regulator init data for qcom ibb regulator\n");
		return -ENOMEM;
	}

	ret = qcom_ibb_dt_init(labibb, of_node);
	if (ret < 0) {
		dev_err(labibb->dev, "qcom-ibb: wrong DT parameter specified: ret = %d\n",
			ret);
		return ret;
	}

	if (labibb->ibb_vreg.ibb_sc_irq != -EINVAL) {
		ret = devm_request_threaded_irq(labibb->dev,
						labibb->ibb_vreg.ibb_sc_irq,
						NULL, labibb_sc_err_handler,
						IRQF_ONESHOT |
						IRQF_TRIGGER_RISING,
						"ibb-sc-err", labibb);
		if (ret) {
			dev_err(labibb->dev, "Failed to register 'ibb-sc-err' irq ret=%d\n",
				ret);
			return ret;
		}
	}

	if (init_data->constraints.name) {
		rdesc->owner		= THIS_MODULE;
		rdesc->type		= REGULATOR_VOLTAGE;
		rdesc->ops		= &qcom_ibb_ops;
		rdesc->name		= init_data->constraints.name;

		cfg.dev = labibb->dev;
		cfg.init_data = init_data;
		cfg.driver_data = labibb;
		cfg.of_node = of_node;

		if (of_get_property(labibb->dev->of_node, "parent-supply",
				    NULL))
			init_data->supply_regulator = "parent";

		init_data->constraints.valid_ops_mask |=
						REGULATOR_CHANGE_VOLTAGE |
						REGULATOR_CHANGE_STATUS;

		labibb->ibb_vreg.rdev = regulator_register(rdesc, &cfg);
		if (IS_ERR(labibb->ibb_vreg.rdev)) {
			ret = PTR_ERR(labibb->ibb_vreg.rdev);
			labibb->ibb_vreg.rdev = NULL;
			dev_err(labibb->dev, "unable to get regulator init data for qcom ibb regulator, ret = %d\n",
				ret);

			return ret;
		}
	} else {
		dev_err(labibb->dev, "qcom ibb regulator name missing\n");
		return -EINVAL;
	}

	return 0;
}

static int qcom_lab_register_irq(struct device_node *child,
				 struct qcom_labibb *labibb)
{
	int ret;

	labibb->lab_vreg.lab_sc_irq = -EINVAL;
	ret = of_irq_get_byname(child, "lab-sc-err");
	if (ret < 0)
		dev_dbg(labibb->dev, "Unable to get lab-sc-err, ret = %d\n",
			ret);
	else
		labibb->lab_vreg.lab_sc_irq = ret;

	return 0;
}

static int qcom_ibb_register_irq(struct device_node *child,
				 struct qcom_labibb *labibb)
{
	int ret;

	labibb->ibb_vreg.ibb_sc_irq = -EINVAL;
	ret = of_irq_get_byname(child, "ibb-sc-err");
	if (ret < 0)
		dev_dbg(labibb->dev, "Unable to get ibb-sc-err, ret = %d\n",
			ret);
	else
		labibb->ibb_vreg.ibb_sc_irq = ret;

	return 0;
}

static int qcom_labibb_regulator_probe(struct platform_device *pdev)
{
	struct qcom_labibb *labibb;
	unsigned int base;
	struct device_node *child;
	u8 type, revision;
	int ret;

	labibb = devm_kzalloc(&pdev->dev, sizeof(*labibb), GFP_KERNEL);
	if (!labibb)
		return -ENOMEM;

	labibb->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!labibb->regmap) {
		dev_err(&pdev->dev, "Couldn't get parent's regmap\n");
		return -EINVAL;
	}

	labibb->dev = &pdev->dev;

	mutex_init(&labibb->lab_vreg.lab_mutex);
	mutex_init(&labibb->ibb_vreg.ibb_mutex);
	mutex_init(&labibb->bus_mutex);

	if (of_get_available_child_count(pdev->dev.of_node) == 0) {
		dev_err(labibb->dev, "no child nodes\n");
		return -ENXIO;
	}

	for_each_available_child_of_node(pdev->dev.of_node, child) {
		ret = of_property_read_u32(child, "reg", &base);
		if (ret < 0) {
			dev_err(&pdev->dev,
				"Couldn't find reg in node = %s ret = %d\n",
				child->full_name, ret);
			return ret;
		}

		ret = qcom_labibb_read(labibb, base + REG_REVISION_2,
				       &revision, 1);
		if (ret < 0) {
			dev_err(labibb->dev,
				"Reading REVISION_2 failed ret=%d\n",
				ret);
			goto fail_registration;
		}

		ret = qcom_labibb_read(labibb, base + REG_PERPH_TYPE,
				       &type, 1);
		if (ret < 0) {
			dev_err(labibb->dev,
				"Peripheral type read failed ret=%d\n",
				ret);
			goto fail_registration;
		}

		switch (type) {
		case qcom_LAB_TYPE:
			labibb->lab_base = base;
			labibb->lab_dig_major = revision;
			ret = qcom_lab_register_irq(child, labibb);
			if (ret) {
				dev_err(labibb->dev, "Failed to register LAB IRQ ret=%d\n",
					ret);
				goto fail_registration;
			}
			ret = register_qcom_lab_regulator(labibb, child);
			if (ret < 0)
				goto fail_registration;
		break;

		case qcom_IBB_TYPE:
			labibb->ibb_base = base;
			labibb->ibb_dig_major = revision;
			qcom_ibb_register_irq(child, labibb);
			ret = register_qcom_ibb_regulator(labibb, child);
			if (ret < 0)
				goto fail_registration;
		break;

		default:
			dev_err(labibb->dev, "qcom_labibb: unknown peripheral type %x\n",
				type);
			ret = -EINVAL;
			goto fail_registration;
		}
	}

	hrtimer_init(&labibb->sc_err_check_timer,
		     CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	labibb->sc_err_check_timer.function = labibb_check_sc_err_count;
	dev_set_drvdata(&pdev->dev, labibb);
	pr_info("LAB/IBB registered successfully");

	return 0;

fail_registration:
	if (labibb->lab_vreg.rdev)
		regulator_unregister(labibb->lab_vreg.rdev);
	if (labibb->ibb_vreg.rdev)
		regulator_unregister(labibb->ibb_vreg.rdev);

	return ret;
}

static int qcom_labibb_regulator_remove(struct platform_device *pdev)
{
	struct qcom_labibb *labibb = dev_get_drvdata(&pdev->dev);

	if (labibb) {
		if (labibb->lab_vreg.rdev)
			regulator_unregister(labibb->lab_vreg.rdev);
		if (labibb->ibb_vreg.rdev)
			regulator_unregister(labibb->ibb_vreg.rdev);
	}
	return 0;
}

static const struct of_device_id qcom_labibb_match_table[] = {
	{ .compatible = "qcom,lab-ibb-regulator", },
	{ },
};

static struct platform_driver qcom_labibb_regulator_driver = {
	.driver		= {
		.name		= "qcom,lab-ibb-regulator",
		.of_match_table	= qcom_labibb_match_table,
	},
	.probe		= qcom_labibb_regulator_probe,
	.remove		= qcom_labibb_regulator_remove,
};

static int __init qcom_labibb_regulator_init(void)
{
	return platform_driver_register(&qcom_labibb_regulator_driver);
}
arch_initcall(qcom_labibb_regulator_init);

static void __exit qcom_labibb_regulator_exit(void)
{
	platform_driver_unregister(&qcom_labibb_regulator_driver);
}
module_exit(qcom_labibb_regulator_exit);

MODULE_DESCRIPTION("Qualcomm labibb driver");
MODULE_LICENSE("GPL v2");

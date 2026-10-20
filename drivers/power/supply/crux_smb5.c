// SPDX-License-Identifier: GPL-2.0-only
/*
 * crux: PM8150B (SMB5) 充电器驱动（最小可用版）
 *
 * 背景：mainline 没有 qpnp-smb5；qcom_battmgr 走 PMIC-GLINK(AOP) 在 sm8150
 * 上没通（属性全空）。结果：插上 PD 也只有 Type-C 侧有状态，系统看不到
 * “在充电/插头在不在”，表现就是“充电检测不灵敏”。
 *
 * 本驱动用 SPMI 直接读 SMB5 CHGR/DCDC 寄存器，注册一个标准 power_supply：
 *   - ONLINE / PRESENT  <- POWER_PATH_STATUS.bit0 (VALID_INPUT_POWER_SOURCE)
 *   - STATUS            <- CHARGING_ENABLE + CHGR_STATUS_1[2:0]
 *   - CHARGE_TYPE       <- 可写，映射到 FAST_CHARGE_CURRENT (25mA/LSB)
 *   - CONSTANT_CHARGE_CURRENT(MAX) <- 可读写
 * 并且每 1s 轮询，状态变化时 power_supply_changed() 发 uevent。
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/delay.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>

#define CHGR_BASE			0x1000
#define DCDC_BASE			0x1300

#define BATTERY_CHARGER_STATUS_1	(CHGR_BASE + 0x06)
#define CHARGING_ENABLE_CMD		(CHGR_BASE + 0x42)
#define CHGR_FAST_CHARGE_CURRENT_CFG	(CHGR_BASE + 0x61)
#define CHGR_FLOAT_VOLTAGE_CFG		(CHGR_BASE + 0x70)

#define POWER_PATH_STATUS_REG		(DCDC_BASE + 0x0b)
#define USBIN_INT_RT_STS_REG		(DCDC_BASE + 0x10)
#define USBIN_PLUGIN_RT_STS_BIT		BIT(4)
#define VALID_INPUT_POWER_SOURCE_STS	BIT(0)

#define BATTERY_CHARGER_STATUS_MASK	GENMASK(2, 0)

enum {
	INHIBIT_CHARGE = 0,
	TRICKLE_CHARGE,
	PRE_CHARGE,
	FULLON_CHARGE,
	TAPER_CHARGE,
	TERMINATE_CHARGE,
};

/* SMB5 常见换算（本机实测：VFLOAT code 0x50=80 -> 4.40V，即 10mV/LSB） */
#define ICHG_STEP_UA		25000
#define VFLOAT_BASE_UV		3600000
#define VFLOAT_STEP_UV		10000

struct crux_smb5 {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
	struct delayed_work poll_work;
	int last_online;
	int last_status;
	unsigned int ichg_ua;
	unsigned int vfloat_uv;
};

static int crux_smb5_read(struct crux_smb5 *chg, unsigned int reg, unsigned int *val)
{
	return regmap_read(chg->regmap, reg, val);
}

static bool crux_smb5_input_online(struct crux_smb5 *chg)
{
	unsigned int v = 0;

	/* 本机实测：插头在位时 USBIN_INT_RT_STS(0x1310)=0x10 -> BIT(4) USBIN_PLUGIN；
	 * 而 POWER_PATH_STATUS(0x130b) 充电时仍读0，不能用。
	 */
	if (!crux_smb5_read(chg, USBIN_INT_RT_STS_REG, &v) &&
	    (v & USBIN_PLUGIN_RT_STS_BIT))
		return true;

	if (!crux_smb5_read(chg, POWER_PATH_STATUS_REG, &v))
		return !!(v & VALID_INPUT_POWER_SOURCE_STS);
	return false;
}

static int crux_smb5_charger_status(struct crux_smb5 *chg)
{
	unsigned int stat = 0, en = 0;

	if (crux_smb5_read(chg, CHARGING_ENABLE_CMD, &en))
		en = 0;
	if (!crux_smb5_input_online(chg) || !(en & BIT(0)))
		return POWER_SUPPLY_STATUS_DISCHARGING;

	if (crux_smb5_read(chg, BATTERY_CHARGER_STATUS_1, &stat))
		return POWER_SUPPLY_STATUS_UNKNOWN;
	stat &= BATTERY_CHARGER_STATUS_MASK;

	switch (stat) {
	case INHIBIT_CHARGE:
		return POWER_SUPPLY_STATUS_NOT_CHARGING;
	case TRICKLE_CHARGE:
	case PRE_CHARGE:
	case FULLON_CHARGE:
	case TAPER_CHARGE:
		return POWER_SUPPLY_STATUS_CHARGING;
	case TERMINATE_CHARGE:
		return POWER_SUPPLY_STATUS_FULL;
	default:
		return POWER_SUPPLY_STATUS_UNKNOWN;
	}
}

static int crux_smb5_get_property(struct power_supply *psy,
				  enum power_supply_property psp,
				  union power_supply_propval *val)
{
	struct crux_smb5 *chg = power_supply_get_drvdata(psy);
	unsigned int v;
	bool online;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
	case POWER_SUPPLY_PROP_PRESENT:
		online = crux_smb5_input_online(chg);
		val->intval = online ? 1 : 0;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = crux_smb5_charger_status(chg);
		break;
	case POWER_SUPPLY_PROP_TYPE:
		val->intval = POWER_SUPPLY_TYPE_USB;
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		val->intval = crux_smb5_charger_status(chg) == POWER_SUPPLY_STATUS_CHARGING ?
			      POWER_SUPPLY_CHARGE_TYPE_FAST : POWER_SUPPLY_CHARGE_TYPE_UNKNOWN;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		if (!crux_smb5_read(chg, CHGR_FAST_CHARGE_CURRENT_CFG, &v))
			val->intval = v * ICHG_STEP_UA;
		else
			val->intval = -1;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		if (!crux_smb5_read(chg, CHGR_FLOAT_VOLTAGE_CFG, &v))
			val->intval = VFLOAT_BASE_UV + v * VFLOAT_STEP_UV;
		else
			val->intval = -1;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER: {
		static char *m = "Qualcomm";
		val->strval = m;
		break;
	}
	case POWER_SUPPLY_PROP_MODEL_NAME: {
		static char *m = "PM8150B SMB5";
		val->strval = m;
		break;
	}
	default:
		return -EINVAL;
	}

	return 0;
}

static int crux_smb5_set_property(struct power_supply *psy,
				  enum power_supply_property psp,
				  const union power_supply_propval *val)
{
	struct crux_smb5 *chg = power_supply_get_drvdata(psy);
	unsigned int code;

	switch (psp) {
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		if (val->intval <= 0)
			return -EINVAL;
		code = DIV_ROUND_CLOSEST(val->intval, ICHG_STEP_UA);
		code = min(code, 0xffu);
		if (regmap_write(chg->regmap, CHGR_FAST_CHARGE_CURRENT_CFG, code))
			return -EIO;
		dev_info(chg->dev, "ICHG -> %u mA (code=%u)\n",
			 code * ICHG_STEP_UA / 1000, code);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		if (val->intval < VFLOAT_BASE_UV)
			return -EINVAL;
		code = DIV_ROUND_CLOSEST(val->intval - VFLOAT_BASE_UV, VFLOAT_STEP_UV);
		code = min(code, 0xffu);
		if (regmap_write(chg->regmap, CHGR_FLOAT_VOLTAGE_CFG, code))
			return -EIO;
		dev_info(chg->dev, "VFLOAT -> %u mV (code=%u)\n",
			 (VFLOAT_BASE_UV + code * VFLOAT_STEP_UV) / 1000, code);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int crux_smb5_property_is_writeable(struct power_supply *psy,
					   enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		return 1;
	default:
		return 0;
	}
}

static enum power_supply_property crux_smb5_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_TYPE,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_MODEL_NAME,
};

static void crux_smb5_poll(struct work_struct *work)
{
	struct crux_smb5 *chg = container_of(to_delayed_work(work),
					     struct crux_smb5, poll_work);
	int online = crux_smb5_input_online(chg) ? 1 : 0;
	int status = crux_smb5_charger_status(chg);

	if (online != chg->last_online || status != chg->last_status) {
		dev_info(chg->dev, "state change: online %d->%d, status %d->%d\n",
			 chg->last_online, online, chg->last_status, status);
		chg->last_online = online;
		chg->last_status = status;
		power_supply_changed(chg->psy);
	}

	schedule_delayed_work(&chg->poll_work, msecs_to_jiffies(1000));
}

static struct power_supply_desc crux_smb5_desc = {
	.name = "crux-charger",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = crux_smb5_props,
	.num_properties = ARRAY_SIZE(crux_smb5_props),
	.get_property = crux_smb5_get_property,
	.set_property = crux_smb5_set_property,
	.property_is_writeable = crux_smb5_property_is_writeable,
};


static ssize_t regs_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct crux_smb5 *chg = dev_get_drvdata(dev);
	int i, n = 0;
	unsigned int v;

	for (i = 0; i < 0x80; i++) {
		if (!regmap_read(chg->regmap, CHGR_BASE + i, &v))
			n += scnprintf(buf + n, 12, "%02x:%02x ", i, v);
	}
	n += scnprintf(buf + n, 4, "\n");
	for (i = 0; i < 0x20; i++) {
		if (!regmap_read(chg->regmap, DCDC_BASE + i, &v))
			n += scnprintf(buf + n, 12, "%02x:%02x ", i, v);
	}
	n += scnprintf(buf + n, 4, "\n");
	return n;
}
static DEVICE_ATTR_RO(regs);

static int crux_smb5_probe(struct platform_device *pdev)
{
	struct power_supply_config psy_cfg = {};
	struct crux_smb5 *chg;
	unsigned int v;
	int ret;

	chg = devm_kzalloc(&pdev->dev, sizeof(*chg), GFP_KERNEL);
	if (!chg)
		return -ENOMEM;

	chg->dev = &pdev->dev;
	chg->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!chg->regmap) {
		dev_err(&pdev->dev, "no parent(pmic) regmap\n");
		return -ENODEV;
	}

	psy_cfg.drv_data = chg;
	psy_cfg.fwnode = dev_fwnode(&pdev->dev);

	chg->psy = devm_power_supply_register(&pdev->dev, &crux_smb5_desc, &psy_cfg);
	if (IS_ERR(chg->psy)) {
		dev_err(&pdev->dev, "power_supply_register failed: %ld\n",
			PTR_ERR(chg->psy));
		return PTR_ERR(chg->psy);
	}

	platform_set_drvdata(pdev, chg);

	chg->last_online = crux_smb5_input_online(chg) ? 1 : 0;
	chg->last_status = crux_smb5_charger_status(chg);

	if (!crux_smb5_read(chg, CHGR_FAST_CHARGE_CURRENT_CFG, &v))
		dev_info(&pdev->dev, "init: ichg_cfg=0x%02x (%u mA)\n", v, v * 25);
	if (!crux_smb5_read(chg, CHGR_FLOAT_VOLTAGE_CFG, &v))
		dev_info(&pdev->dev, "init: vfloat_cfg=0x%02x (%u mV)\n", v,
			 (VFLOAT_BASE_UV + v * VFLOAT_STEP_UV) / 1000);

	dev_info(&pdev->dev, "registered crux-charger: online=%d status=%d\n",
		 chg->last_online, chg->last_status);

	INIT_DELAYED_WORK(&chg->poll_work, crux_smb5_poll);
	schedule_delayed_work(&chg->poll_work, msecs_to_jiffies(1000));

	device_create_file(&pdev->dev, &dev_attr_regs);

	return 0;
}

static void crux_smb5_remove(struct platform_device *pdev)
{
	struct crux_smb5 *chg = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&chg->poll_work);
}

static const struct of_device_id crux_smb5_of_match[] = {
	{ .compatible = "crux,smb5" },
	{ .compatible = "crux,smb5-probe" }, /* 兼容旧 DT，免重启测试 */
	{ }
};
MODULE_DEVICE_TABLE(of, crux_smb5_of_match);

static struct platform_driver crux_smb5_driver = {
	.probe = crux_smb5_probe,
	.remove = crux_smb5_remove,
	.driver = {
		.name = "crux-smb5",
		.of_match_table = crux_smb5_of_match,
	},
};
module_platform_driver(crux_smb5_driver);

MODULE_DESCRIPTION("crux: PM8150B SMB5 charger driver (minimal power_supply)");
MODULE_LICENSE("GPL");
// SPDX-License-Identifier: GPL-2.0-only
/*
 * crux: PM8150B (SMB5) 充电器驱动
 *
 * 本驱动"按原厂 qpnp-smb5 的设计思路"重写，但**不照搬其实现**（原厂 14699 行）。
 * 只吸收五条核心思想：
 *   1) 参数表化：每个可调参数 = {名字, 寄存器, min, max, step}，统一编解码，
 *      不再把电流/电压写死在代码里；
 *   2) 策略集中：probe 时按"电池状态档"一次性下发整套安全参数；
 *   3) 三层温度防护：JEITA 使能 + 温度范围状态监控 + 充电安全定时器；
 *   4) 供电优先：ICL 可调、AICL 交给 PMIC 自己做、boost 阈值抬到 3.6V 增强
 *      抗 VPH_PWR 跌落能力（对应 PON FAULT_REASON1 的 SMPL 瞬时掉电）；
 *   5) 状态变化才上报 uevent，不做无意义刷新。
 *
 * 相对旧版（352 行最小驱动）修正的两个硬 bug：
 *   - DCDC_BASE 0x1300 -> 0x1100。0x1300 其实是 USBIN，旧驱动把
 *     POWER_PATH_STATUS(0x110b) 读成了 USBIN 段的寄存器，状态判断全错位。
 *   - FCC 步进 25mA -> 50mA。原厂 PM8150B 参数表：max 8A / step 50mA。
 *     旧驱动按 25mA 算，写 code=60 以为是 1500mA，实际是 3000mA。
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>

/* ---------------------------------------------------------------- 寄存器 */
#define CHGR_BASE			0x1000
#define DCDC_BASE			0x1100	/* 原厂 smb5-reg.h：0x1300 是 USBIN */
#define USBIN_BASE			0x1300
#define MISC_BASE			0x1600

/* CHGR 外设 */
#define BATTERY_CHARGER_STATUS_1	(CHGR_BASE + 0x06)
#define BATTERY_CHARGER_STATUS_MASK	GENMASK(2, 0)
#define CHARGING_ENABLE_CMD		(CHGR_BASE + 0x42)
#define CHGR_FAST_CHARGE_CURRENT_CFG	(CHGR_BASE + 0x61)
#define CHGR_FLOAT_VOLTAGE_CFG		(CHGR_BASE + 0x70)
#define CHARGE_INHIBIT_THRESHOLD_CFG	(CHGR_BASE + 0x72)
#define CHGR_ADC_RECHARGE_THRESHOLD_MSB	(CHGR_BASE + 0x7E)
#define CHGR_ADC_RECHARGE_THRESHOLD_LSB	(CHGR_BASE + 0x7F)
#define JEITA_EN_CFG			(CHGR_BASE + 0x90)
#define JEITA_EN_HOT_SL_FCV_BIT		BIT(3)
#define JEITA_EN_COLD_SL_FCV_BIT	BIT(2)
#define JEITA_EN_HOT_SL_CCC_BIT		BIT(1)
#define JEITA_EN_COLD_SL_CCC_BIT	BIT(0)
#define JEITA_CCCOMP_CFG_HOT		(CHGR_BASE + 0x92)
#define JEITA_CCCOMP_CFG_COLD		(CHGR_BASE + 0x93)
#define CHGR_FAST_CHARGE_SAFETY_TIMER_CFG (CHGR_BASE + 0xA2)
#define SAFETY_TIMER_384_MIN		0x1
#define CHGR_ENG_CHARGING_CFG		(CHGR_BASE + 0xC0)
#define CHGR_ITERM_USE_ANALOG_BIT	BIT(3)

/* DCDC 外设（0x1100） */
#define DCDC_ICL_STATUS_REG		(DCDC_BASE + 0x07)
#define DCDC_AICL_STATUS_REG		(DCDC_BASE + 0x0A)
#define AICL_DONE_BIT			BIT(0)
#define POWER_PATH_STATUS_REG		(DCDC_BASE + 0x0B)
#define VALID_INPUT_POWER_SOURCE_STS_BIT BIT(0)
#define POWER_PATH_MASK			GENMASK(2, 1)
#define USE_USBIN_BIT			BIT(4)
#define USBIN_SUSPEND_STS_BIT		BIT(6)
#define DCDC_ENG_SDCDC_CFG5_REG		(DCDC_BASE + 0xC4)
#define ENG_SDCDC_BAT_HPWR_MASK		GENMASK(7, 6)
#define BOOST_THRESH_3P6V		0xC0

/* USBIN 外设（0x1300） */
#define USBIN_INT_RT_STS_REG		(USBIN_BASE + 0x10)
#define USBIN_PLUGIN_RT_STS_BIT		BIT(4)
#define USBIN_OV_RT_STS_BIT		BIT(3)
#define USBIN_CMD_IL_REG		(USBIN_BASE + 0x40)
#define USBIN_SUSPEND_BIT		BIT(0)
#define USBIN_CURRENT_LIMIT_CFG_REG	(USBIN_BASE + 0x70)
#define USBIN_AICL_OPTIONS_CFG_REG	(USBIN_BASE + 0x80)
#define USBIN_AICL_EN_BIT		BIT(2)
#define USBIN_OPTIONS_1_CFG_REG		(USBIN_BASE + 0x62)
#define HVDCP_AUTH_ALG_EN_CFG_BIT	BIT(6)
#define HVDCP_AUTONOMOUS_MODE_EN_CFG_BIT BIT(5)
#define HVDCP_EN_BIT			BIT(2)

/* OTG（反向供电 5V，可给 USB 网卡/HUB 用） */
#define DCDC_CMD_OTG_REG		(DCDC_BASE + 0x40)
#define OTG_EN_BIT			BIT(0)
#define DCDC_OTG_CURRENT_LIMIT_CFG_REG	(DCDC_BASE + 0x52)
#define OTG_CL_STEP_UA			500000
#define OTG_CL_DEFAULT_UA		1000000
#define USBIN_ADAPTER_ALLOW_OVERRIDE_REG	(USBIN_BASE + 0x44)
#define ALLOW_FORCE_5V		0x01
#define ALLOW_FORCE_9V		0x02
#define ALLOW_FORCE_12V	0x04
#define ALLOW_CONTINUOUS	0x08
#define CMD_APSD_REG			(USBIN_BASE + 0x41)
#define APSD_RERUN_BIT			BIT(0)
#define QC_CHANGE_STATUS_REG		(USBIN_BASE + 0x09)
#define QC_2P0_STATUS_MASK		GENMASK(2, 0)
#define QC_5V_BIT			BIT(0)
#define QC_9V_BIT			BIT(1)
#define QC_12V_BIT			BIT(2)

/* MISC 外设（0x1600） */
#define TEMP_RANGE_STATUS_REG		(MISC_BASE + 0x06)
#define MISC_THERMREG_SRC_CFG_REG		(MISC_BASE + 0x70)
#define THERMREG_SW_ICL_ADJUST_BIT		BIT(7)
#define THERMREG_DIE_ADC_SRC_EN_BIT		BIT(1)
#define THERMREG_DIE_CMP_SRC_EN_BIT		BIT(0)
#define THERMREG_SRC_CFG_MASK			(BIT(7) | BIT(4) | BIT(3) | BIT(2) | \
					 BIT(1) | BIT(0))

/* BATIF 外设（0x1200） */
#define BATIF_BASE			0x1200
#define BATIF_ADC_CHANNEL_EN_REG		(BATIF_BASE + 0x82)
#define DIE_TEMP_CHANNEL_EN_BIT			BIT(2)
#define TEMP_RANGE_MASK			GENMASK(4, 1)
#define TEMP_ABOVE_RANGE_BIT		BIT(3)

/* 充电状态（CHGR_STATUS_1[2:0]） */
enum {
	INHIBIT_CHARGE = 0,
	TRICKLE_CHARGE,
	PRE_CHARGE,
	FULLON_CHARGE,
	TAPER_CHARGE,
	TERMINATE_CHARGE,
};

/* ------------------------------------------------- 参数表（学原厂的参数化） */
/*
 * base_u: 编码基准电压（FV 用，code = (val - base) / step）;
 *         其余参数 base_u = 0，code = val / step。
 */
struct smb_param {
	const char	*name;
	u16		reg;
	u32		min_u;
	u32		max_u;
	u32		step_u;
	u32		base_u;
};

static const struct smb_param crux_fcc = {
	.name	= "fast charge current",
	.reg	= CHGR_FAST_CHARGE_CURRENT_CFG,
	.min_u	= 0,
	.max_u	= 8000000,	/* 原厂 PM8150B: max 8A */
	.step_u	= 50000,	/* 原厂: 50mA/LSB（旧驱动误用 25mA） */
};

static const struct smb_param crux_fv = {
	.name	= "float voltage",
	.reg	= CHGR_FLOAT_VOLTAGE_CFG,
	.min_u	= 3600000,
	.max_u	= 4790000,
	.step_u	= 10000,
	.base_u	= 3600000,
};

static const struct smb_param crux_icl = {
	.name	= "usb input current limit",
	.reg	= USBIN_CURRENT_LIMIT_CFG_REG,
	.min_u	= 0,
	.max_u	= 5000000,
	.step_u	= 50000,
};

/* 老化电池安全策略档（可在 probe 后通过 sysfs 改，重启仍按此档） */
#define CRUX_FCC_SAFE_UA	500000	/* 500mA */
#define CRUX_FV_SAFE_UV		4350000	/* 4.35V，满充电压上限低于 4.40V */
#define CRUX_ICL_SDP_UA		500000	/* USB SDP 500mA */
/*
 * 硬 JEITA 默认关闭（0）。
 * PM8150B 的 JEITA 阈值寄存器默认值是极低温度编码
 * （0x1094=0x20 / 0x1098=0x18 / 0x109c=0x0c / 0x10a0=0x03），
 * 一旦写上 JEITA_EN_CFG=0x0f 使能，PMIC 会判定电池"过热"，
 * 把充电电流（CCC）压到 0 —— 实测 38.5℃ 时 ibat=0、电池反被放电 (-450mA)。
 * 所以温度保护改由软件 JEITA 承担（按 bq27z561 真实温度分档收 FCC）。
 * 如果以后要恢复硬 JEITA，必须先正确配置 0x1094~0x10a3 的温度阈值。
 */
#define CRUX_JEITA_EN			0x00

struct crux_smb5 {
	struct device		*dev;
	struct regmap		*regmap;
	struct power_supply	*psy;
	struct power_supply	*batt_psy;	/* bq27z561，用于软件 JEITA */
	struct delayed_work	poll_work;
	int			last_online;
	int			last_status;
	int			last_jeita_stage;
	bool			digital_temp_over;	/* PMIC 温度范围超限 */
	bool			bypass_active;		/* EOC 后切旁路：停充、系统吃 VBUS */
	unsigned long		bypass_exit;		/* 上次退出旁路的时刻 */
	u32			pd_icl_ua;	/* 已下发的 PD 联动 ICL，0=未同步 */
};

/* ------------------------------------------------ 通用参数编解码（参数表化） */
static int crux_param_write(struct crux_smb5 *chg, const struct smb_param *p, u32 val_u)
{
	u32 code;

	if (val_u < p->min_u || val_u > p->max_u)
		return -ERANGE;

	if (p->base_u) {
		if (val_u < p->base_u)
			return -ERANGE;
		code = DIV_ROUND_CLOSEST(val_u - p->base_u, p->step_u);
	} else {
		code = DIV_ROUND_CLOSEST(val_u, p->step_u);
	}
	code = min_t(u32, code, 0xffu);

	if (regmap_write(chg->regmap, p->reg, code))
		return -EIO;

	return 0;
}

static int crux_param_read(struct crux_smb5 *chg, const struct smb_param *p, u32 *val_u)
{
	unsigned int v;

	if (regmap_read(chg->regmap, p->reg, &v))
		return -EIO;

	*val_u = p->base_u + v * p->step_u;
	return 0;
}

/* ------------------------------------------------------------ 状态读取 */
static bool crux_input_online(struct crux_smb5 *chg)
{
	unsigned int v;

	/* USBIN 插头在位（每个外设的 INT_RT_STS 都在 base+0x10） */
	if (!regmap_read(chg->regmap, USBIN_INT_RT_STS_REG, &v) &&
	    (v & USBIN_PLUGIN_RT_STS_BIT))
		return true;

	/* 真正的 POWER_PATH_STATUS 现在是 0x110b（旧驱动读的是 0x130b，错位） */
	if (!regmap_read(chg->regmap, POWER_PATH_STATUS_REG, &v))
		return !!(v & VALID_INPUT_POWER_SOURCE_STS_BIT);

	return false;
}

/* 供电是否真的从 USB 走（USE_USBIN + 未 suspend），用于判断"是否在吃电池" */
static bool crux_usb_powering_system(struct crux_smb5 *chg)
{
	unsigned int v;
	int path;

	if (regmap_read(chg->regmap, POWER_PATH_STATUS_REG, &v))
		return false;

	path = FIELD_GET(POWER_PATH_MASK, v);

	return (v & VALID_INPUT_POWER_SOURCE_STS_BIT) &&
	       (v & USE_USBIN_BIT) &&
	       !(v & USBIN_SUSPEND_STS_BIT) &&
	       (path != 0);
}

static int crux_charger_status(struct crux_smb5 *chg)
{
	unsigned int stat = 0, en = 0;

	if (chg->bypass_active && crux_input_online(chg))
		return POWER_SUPPLY_STATUS_FULL;

	if (regmap_read(chg->regmap, CHARGING_ENABLE_CMD, &en))
		en = 0;

	if (!crux_input_online(chg) || !(en & BIT(0)))
		return POWER_SUPPLY_STATUS_DISCHARGING;

	if (regmap_read(chg->regmap, BATTERY_CHARGER_STATUS_1, &stat))
		return POWER_SUPPLY_STATUS_UNKNOWN;


	switch (stat & BATTERY_CHARGER_STATUS_MASK) {
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

/* 统一开关充电（sysfs 与旁路状态机共用） */
static void crux_charge_enable(struct crux_smb5 *chg, bool en)
{
	if (regmap_update_bits(chg->regmap, CHARGING_ENABLE_CMD, BIT(0),
			       en ? BIT(0) : 0))
		return;

	dev_info(chg->dev, "charge_enable -> %d\n", en);
}

/* --------------------------------------------------------- 属性 get/set */
static int crux_smb5_get_property(struct power_supply *psy,
				  enum power_supply_property psp,
				  union power_supply_propval *val)
{
	struct crux_smb5 *chg = power_supply_get_drvdata(psy);
	unsigned int v;
	u32 u32v;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = crux_input_online(chg) ? 1 : 0;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = crux_charger_status(chg);
		break;
	case POWER_SUPPLY_PROP_TYPE:
		val->intval = POWER_SUPPLY_TYPE_USB;
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		switch (crux_charger_status(chg)) {
		case POWER_SUPPLY_STATUS_CHARGING:
			if (!regmap_read(chg->regmap, BATTERY_CHARGER_STATUS_1, &v) &&
			    (v & BATTERY_CHARGER_STATUS_MASK) == TAPER_CHARGE)
				val->intval = POWER_SUPPLY_CHARGE_TYPE_STANDARD;
			else
				val->intval = POWER_SUPPLY_CHARGE_TYPE_FAST;
			break;
		case POWER_SUPPLY_STATUS_FULL:
			val->intval = POWER_SUPPLY_CHARGE_TYPE_STANDARD;
			break;
		default:
			val->intval = POWER_SUPPLY_CHARGE_TYPE_UNKNOWN;
			break;
		}
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		if (!crux_param_read(chg, &crux_fcc, &u32v))
			val->intval = u32v;
		else
			val->intval = -1;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		if (!crux_param_read(chg, &crux_fv, &u32v))
			val->intval = u32v;
		else
			val->intval = -1;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		if (!crux_param_read(chg, &crux_icl, &u32v))
			val->intval = u32v;
		else
			val->intval = -1;
		break;
	/* 供给方视角的"真实输入电流上限"（AICL 收缩后的值） */
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		if (!crux_param_read(chg, &crux_icl, &u32v))
			val->intval = u32v;
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
	int rc;

	switch (psp) {
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		if (val->intval <= 0)
			return -EINVAL;
		rc = crux_param_write(chg, &crux_fcc, val->intval);
		if (!rc)
			dev_info(chg->dev, "%s -> %d uA\n",
				 crux_fcc.name, val->intval);
		return rc;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		rc = crux_param_write(chg, &crux_fv, val->intval);
		if (!rc)
			dev_info(chg->dev, "%s -> %d uV\n",
				 crux_fv.name, val->intval);
		return rc;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		rc = crux_param_write(chg, &crux_icl, val->intval);
		if (!rc)
			dev_info(chg->dev, "%s -> %d uA\n",
				 crux_icl.name, val->intval);
		return rc;
	default:
		return -EINVAL;
	}
}

static int crux_smb5_property_is_writeable(struct power_supply *psy,
					   enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
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
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_MODEL_NAME,
};/* ------------------------------------------ 软件 JEITA（学原厂的降级思路） */
/*
 * 原厂用硬 JEITA：PMIC 内部根据电池温度自动降 FCV/FCC（JEITA_EN_CFG）。
 * 我们这里做两件事：
 *   a) 把硬 JEITA 使能（PMIC 自己按内置阈值降级）——这是主要防线；
 *   b) 再叠一层软件 JEITA：用 bq27z561 报的电池温度分档收 FCC，
 *      因为老化电池 + 3A 大电流的组合最怕温度失控。
 * 温度单位 0.1℃，阈值参考 JEITA 常规分档（0/15/45/50℃）。
 */
static int crux_jeita_stage(int temp)
{
	if (temp < 0)
		return 1;	/* 过冷，禁止充电 */
	if (temp < 150)
		return 2;	/* 冷：降流 */
	if (temp <= 450)
		return 3;	/* 正常 */
	if (temp <= 500)
		return 4;	/* 偏热：降流 */
	return 5;		/* 过热，禁止充电 */
}

static u32 crux_jeita_fcc(int stage)
{
	switch (stage) {
	case 2:
		return 150000;
	case 3:
		return CRUX_FCC_SAFE_UA;
	case 4:
		return 250000;
	default:
		return 0;	/* 1 / 5：停充 */
	}
}

static void crux_jeita_apply(struct crux_smb5 *chg)
{
	union power_supply_propval v;
	int temp, stage;
	u32 fcc;

	if (!chg->batt_psy) {
		chg->batt_psy = power_supply_get_by_name("bq27z561-0");
		if (!chg->batt_psy)
			return;
	}

	if (power_supply_get_property(chg->batt_psy, POWER_SUPPLY_PROP_TEMP, &v))
		return;

	temp = v.intval;
	stage = crux_jeita_stage(temp);
	if (stage == chg->last_jeita_stage)
		return;		/* 只在该变的时候下发 */

	chg->last_jeita_stage = stage;
	fcc = crux_jeita_fcc(stage);

	if (crux_param_write(chg, &crux_fcc, fcc))
		return;

	/* FCC=0 时同时拉掉充电使能，保证真的停充 */
	regmap_update_bits(chg->regmap, CHARGING_ENABLE_CMD, BIT(0),
			   fcc ? BIT(0) : 0);

	dev_info(chg->dev, "JEITA: temp=%d.%dC stage=%d fcc=%u mA%s\n",
		 temp / 10, temp % 10, stage, fcc / 1000,
		 fcc ? "" : " (charging inhibited)");
	power_supply_changed(chg->psy);
}

/*
 * 配置 die temp 热调节源（对应下游 smb5_configure_mitigation 的 die-temp 分支）：
 *   BATIF_ADC_CHANNEL_EN_REG  BIT2 DIE_TEMP_CHANNEL_EN      —— 使能 die temp ADC 通道
 *   MISC_THERMREG_SRC_CFG_REG BIT1 DIE_ADC_SRC_EN | BIT0 DIE_CMP_SRC_EN
 *
 * 这两处不配，TEMP_RANGE_STATUS(0x1606) 的 ABOVE/WITHIN/BELOW 位就是浮的：
 * 实测 die_temp 42.7C 时该寄存器报 0x02(BELOW) 并来回跳到 0x08(ABOVE)。
 * 配好之后状态位才反映真实 die 温度区间。
 */
static int crux_thermreg_init(struct crux_smb5 *chg)
{
	int rc;

	rc = regmap_update_bits(chg->regmap, BATIF_ADC_CHANNEL_EN_REG,
				DIE_TEMP_CHANNEL_EN_BIT, DIE_TEMP_CHANNEL_EN_BIT);
	if (rc)
		return rc;

	return regmap_update_bits(chg->regmap, MISC_THERMREG_SRC_CFG_REG,
				  THERMREG_SRC_CFG_MASK,
				  THERMREG_DIE_ADC_SRC_EN_BIT |
				  THERMREG_DIE_CMP_SRC_EN_BIT);
}

/* PMIC 自身温度范围状态（原厂靠 IIO ADC 读 die_temp，我们用状态位即可） */
static void crux_check_pmic_temp(struct crux_smb5 *chg)
{
	unsigned int v;

	/*
	 * 0x1670 MISC_THERMREG_SRC_CFG 从未配置时，TEMP_RANGE_STATUS(0x1606)
	 * 的 ABOVE 位不可靠（实测 die_temp 42.7C 仍报 ABOVE 并来回抖动）。
	 * 这里只记录、不做任何动作：真过温由 PM8150B 硬件 TSD 自动保护。
	 * 之前一置位就关 CHARGING_ENABLE，会让输入电流归零，进而诱发
	 * 充电宝小电流自动关断 -> VBUS 消失 -> 硬复位。
	 */
	if (regmap_read(chg->regmap, TEMP_RANGE_STATUS_REG, &v))
		return;

	if (!!(v & TEMP_ABOVE_RANGE_BIT) && !chg->digital_temp_over) {
		chg->digital_temp_over = true;
		dev_warn(chg->dev, "PMIC temp range: ABOVE (log only, no action)\n");
	} else if (!(v & TEMP_ABOVE_RANGE_BIT) && chg->digital_temp_over) {
		chg->digital_temp_over = false;
		dev_warn(chg->dev, "PMIC temp range: back within range\n");
	}
}

/* -------------------------------------------------------------- 轮询 */
/*
 * 原厂 smb5-lib.c：PD 合同后 smblib_usb_pd_adapter_allowance_override()
 * 写 USBIN_ADAPTER_ALLOW_OVERRIDE_REG(0x1344)，值 = FORCE_5V/9V/12V 或 CONTINUOUS。
 * 这个寄存器为 0（FORCE_NULL）时 SMB5 会把输入路径当“不允许”关掉：
 *   USBIN_PLUGIN_RT_STS=0、ICL_STATUS=0、电池放电；
 *   写入对应位后立刻恢复（实测 12V 下 0x04 → USBIN path 0x95、+448mA 充电）。
 * 所以这里每个 poll 周期按 tcpm 报的 VBUS 电压校准一次。
 */
static int crux_get_vbus_uv(struct crux_smb5 *chg)
{
	static const char *const names[] = {
		"usb",
		"tcpm-source-psy-c440000.spmi:pmic@2:typec@1500",
	};
	union power_supply_propval pval = {0, };
	struct power_supply *psy;
	int i, rc;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		psy = power_supply_get_by_name(names[i]);
		if (!psy)
			continue;
		rc = power_supply_get_property(psy, POWER_SUPPLY_PROP_VOLTAGE_NOW,
					&pval);
		power_supply_put(psy);
		if (!rc && pval.intval > 0)
			return pval.intval;
	}
	return -1;
}

static void crux_update_allowance(struct crux_smb5 *chg)
{
	int uv = crux_get_vbus_uv(chg);
	unsigned int cur = 0;
	u8 want;

	if (uv < 0)
		return;

	/*
	 * 固定 CONTINUOUS：允许适配器输出任意合法电压。
	 * 之前按"当前 VBUS"分档写 FORCE_5V，会与 tcpm 的 PD 协商互相锁死：
	 * tcpm 想升 9V -> SMB5 只允许 5V -> VBUS 被拒 -> tcpm 退回 5V -> 永远 5V。
	 */
	want = ALLOW_CONTINUOUS;

	if (regmap_read(chg->regmap, USBIN_ADAPTER_ALLOW_OVERRIDE_REG, &cur))
		return;
	if ((cur & 0x0f) == want)
		return;

	if (regmap_write(chg->regmap, USBIN_ADAPTER_ALLOW_OVERRIDE_REG, want))
		dev_err(chg->dev, "allowance write failed\n");
	else
		dev_info(chg->dev,
			 "usbin allowance: vbus=%duV 0x%02x -> 0x%02x\n",
			 uv, cur & 0x0f, want);
}

/*
 * PD 合同 -> ICL 联动（补上原厂 smblib_get_charge_current() 的 PD_VOTER）
 *
 * 原厂逻辑：PD 建立后总输入预算 = PD_VOTER 投的 ICL（= PD 合同电流），
 * 而不是 dts 里的默认 1.2A。mainline 这版 crux_smb5 没有这个投票链，
 * 结果 PD 协商到 9V/3A 了、ICL 还停在 1.2A -> 只有 10.8W。
 *
 * 这里按 poll 周期从 typec 的 usb-psy 读源声明的最大电流（PD 合同后 = 3A），
 * 夹在 [CRUX_ICL_MIN_UA, CRUX_ICL_MAX_UA] 再下发给 USBIN_CURRENT_LIMIT_CFG。
 */
#define CRUX_ICL_MAX_UA		3000000u	/* PD 3A 合同上限 */
#define CRUX_ICL_MIN_UA		1200000u	/* 低于此值撑不住系统负载 */
#define CRUX_ICL_5V_UA		1200000u	/* 拿不到源能力时的兜底 */

static void crux_pd_icl_sync(struct crux_smb5 *chg)
{
	static const char *const names[] = {
		"usb",
		"tcpm-source-psy-c440000.spmi:pmic@2:typec@1500",
	};
	union power_supply_propval pc = {0, };
	struct power_supply *psy;
	u32 want = 0;
	int i, rc;

	if (!crux_input_online(chg)) {
		chg->pd_icl_ua = 0;
		return;
	}

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		psy = power_supply_get_by_name(names[i]);
		if (!psy)
			continue;
		/* CURRENT_MAX = 源声明上限；PD 合同后 tcpm 会把它更新为合同电流 */
		rc = power_supply_get_property(psy, POWER_SUPPLY_PROP_CURRENT_MAX, &pc);
		if (rc || pc.intval <= 0)
			rc = power_supply_get_property(psy, POWER_SUPPLY_PROP_CURRENT_NOW, &pc);
		power_supply_put(psy);
		if (!rc && pc.intval > 0) {
			want = pc.intval;
			break;
		}
	}

	if (!want)
		want = CRUX_ICL_5V_UA;
	if (want > CRUX_ICL_MAX_UA)
		want = CRUX_ICL_MAX_UA;
	if (want < CRUX_ICL_MIN_UA)
		want = CRUX_ICL_MIN_UA;
	if (want == chg->pd_icl_ua) {
		u32 actual = 0;

		/*
		 * 合同没变：只保证不超过合同值（上限保护）。
		 * 若 ICL 被用户态主动压低（battery 插件满电/高温降流、
		 * 手动写入等），这里尊重，不抢回去。
		 */
		if (!crux_param_read(chg, &crux_icl, &actual) && actual > want) {
			crux_param_write(chg, &crux_icl, want);
			dev_info(chg->dev, "pd_icl: clamp %u -> %u mA\n",
				 actual / 1000, want / 1000);
		}
		return;
	}

	if (crux_param_write(chg, &crux_icl, want)) {
		dev_err(chg->dev, "pd_icl: write %u uA failed\n", want);
		return;
	}
	chg->pd_icl_ua = want;
	dev_info(chg->dev, "pd_icl: source wants %u mA -> icl set %u mA\n",
		 want / 1000, want / 1000);
}

/* --------------------------------------------------- EOC 后旁路供电 */
/*
 * 电池充满（CHGR_STATUS_1 = TERMINATE_CHARGE）后不再维持充电：
 *   1) 关掉充电使能，系统继续吃 VBUS（旁路），电池不再被顶在 4.35V；
 *   2) 电压掉到 bypass_resume_uv 以下再恢复充电，避免频繁 top-off；
 *   3) 满充那一刻的"学习容量"由电量计侧 reconcile 选出并上报（见 bq27xxx）。
 * 目的是减少电池在满电高压区的停留时间和充放循环。
 */
static bool crux_bypass = true;
module_param_named(bypass, crux_bypass, bool, 0644);
MODULE_PARM_DESC(bypass, "crux: switch to bypass supply after EOC (default on)");

#define CRUX_BYPASS_ENTER_UV	4200000		/* 进旁路要求电压确实在顶 */
#define CRUX_BYPASS_ARM		msecs_to_jiffies(60000)

static int crux_bypass_resume_uv = 4150000;
module_param_named(bypass_resume_uv, crux_bypass_resume_uv, int, 0644);
MODULE_PARM_DESC(bypass_resume_uv, "crux: bypass exit threshold in uV (default 4.15V)");

static void crux_bypass_apply(struct crux_smb5 *chg, int status)
{
	union power_supply_propval v;
	int uv;

	if (!crux_bypass)
		return;

	if (!crux_input_online(chg)) {
		if (chg->bypass_active) {
			chg->bypass_active = false;
			dev_info(chg->dev, "bypass: adapter removed\n");
		}
		return;
	}

	/* JEITA 正在禁充时不要抢控制权 */
	if (chg->last_jeita_stage && !crux_jeita_fcc(chg->last_jeita_stage))
		return;

	if (!chg->batt_psy) {
		chg->batt_psy = power_supply_get_by_name("bq27z561-0");
		if (!chg->batt_psy)
			return;
	}

	if (power_supply_get_property(chg->batt_psy,
				      POWER_SUPPLY_PROP_VOLTAGE_NOW, &v))
		uv = 0;
	else
		uv = v.intval;

	if (!chg->bypass_active) {
		/* 只认"真充满"：状态 TERMINATE 且电压确实在顶 */
		if (status != POWER_SUPPLY_STATUS_FULL)
			return;
		if (uv > 0 && uv < CRUX_BYPASS_ENTER_UV)
			return;
		/* 刚退出旁路时别马上又进去，给充电一点时间 */
		if (chg->bypass_exit &&
		    time_before(jiffies, chg->bypass_exit + CRUX_BYPASS_ARM))
			return;
		chg->bypass_active = true;
		crux_charge_enable(chg, false);
		dev_info(chg->dev,
			 "bypass: EOC at %dmV, charging stopped, system on VBUS\n",
			 uv / 1000);
		return;
	}

	if (uv > 0 && uv <= crux_bypass_resume_uv) {
		chg->bypass_active = false;
		chg->bypass_exit = jiffies;
		crux_charge_enable(chg, true);
		dev_info(chg->dev, "bypass: vbat %dmV <= %dmV, resume charging\n",
			 uv / 1000, crux_bypass_resume_uv / 1000);
	}
}

static void crux_smb5_poll(struct work_struct *work)
{
	struct crux_smb5 *chg = container_of(to_delayed_work(work),
					     struct crux_smb5, poll_work);
	int online = crux_input_online(chg) ? 1 : 0;
	int status = crux_charger_status(chg);

	crux_update_allowance(chg);
	crux_pd_icl_sync(chg);
	crux_check_pmic_temp(chg);
	crux_jeita_apply(chg);
	crux_bypass_apply(chg, status);

	if (online != chg->last_online || status != chg->last_status) {
		dev_info(chg->dev,
			 "state change: online %d->%d, status %d->%d, usb_powering=%d\n",
			 chg->last_online, online, chg->last_status, status,
			 crux_usb_powering_system(chg));
		chg->last_online = online;
		chg->last_status = status;
		power_supply_changed(chg->psy);
	}

	schedule_delayed_work(&chg->poll_work, msecs_to_jiffies(2000));
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

static ssize_t regs_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct crux_smb5 *chg = dev_get_drvdata(dev);
	int i, n = 0;
	unsigned int v;

	n += scnprintf(buf + n, 24, "CHGR:");
	for (i = 0; i <= 0xC0; i++)
		if (!regmap_read(chg->regmap, CHGR_BASE + i, &v))
			n += scnprintf(buf + n, 8, "%02x:%02x ", i, v);
	n += scnprintf(buf + n, 8, "\nDCDC:");
	for (i = 0; i <= 0xC4; i++)
		if (!regmap_read(chg->regmap, DCDC_BASE + i, &v))
			n += scnprintf(buf + n, 8, "%02x:%02x ", i, v);
	n += scnprintf(buf + n, 8, "\nUSBIN:");
	for (i = 0; i <= 0xC0; i++)
		if (!regmap_read(chg->regmap, USBIN_BASE + i, &v))
			n += scnprintf(buf + n, 8, "%02x:%02x ", i, v);
	n += scnprintf(buf + n, 2, "\n");
	return n;
}
/* reg：任意寄存器读写（调试用）
 *   echo 0x1360 > reg      -- 选中地址，cat reg 返回 "0x1360=0x0c"
 *   echo 0x1360=0x0c > reg -- 直接写
 */
static u32 crux_reg_sel;

static ssize_t reg_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct crux_smb5 *chg = dev_get_drvdata(dev);
	unsigned int v = 0;

	regmap_read(chg->regmap, crux_reg_sel, &v);
	return sysfs_emit(buf, "0x%04x=0x%02x\n", crux_reg_sel, v);
}

static ssize_t reg_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct crux_smb5 *chg = dev_get_drvdata(dev);
	unsigned int a, v;
	int rc;

	if (sscanf(buf, "%x=%x", &a, &v) == 2) {
		crux_reg_sel = a;
		rc = regmap_write(chg->regmap, a, v);
		if (rc)
			return rc;
		dev_info(chg->dev, "reg write 0x%04x = 0x%02x\n", a, v);
		return count;
	}
	if (sscanf(buf, "%x", &a) == 1) {
		crux_reg_sel = a;
		return count;
	}
	return -EINVAL;
}
static DEVICE_ATTR_RW(reg);
static DEVICE_ATTR_RO(regs);

/*
 * charge_enable：停充开关。
 * 写 0 = 停止充电（充电使能位清零）。此时 USB 仍在给系统供电
 * （POWER_PATH 走 buck），电池既不充也基本不放——这就是"旁路直供/停充养护"
 * 的实际效果，用来让老化电池在高负载时休息，避免 VSYS 被大电流拉低。
 */
static ssize_t charge_enable_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct crux_smb5 *chg = dev_get_drvdata(dev);
	unsigned int v;

	if (regmap_read(chg->regmap, CHARGING_ENABLE_CMD, &v))
		return -EIO;
	return sysfs_emit(buf, "%d\n", !!(v & BIT(0)));
}

static ssize_t charge_enable_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct crux_smb5 *chg = dev_get_drvdata(dev);
	bool en;

	if (kstrtobool(buf, &en))
		return -EINVAL;

	/* 手工开充视为退出旁路，避免状态机把用户的意图盖回去 */
	if (en)
		chg->bypass_active = false;

	crux_charge_enable(chg, en);
	return count;
}
static DEVICE_ATTR_RW(charge_enable);

/* otg：写 1 打开 5V 反向供电，写 0 关闭（开启前把限流设到 1A） */
static ssize_t otg_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct crux_smb5 *chg = dev_get_drvdata(dev);
	unsigned int v;

	if (regmap_read(chg->regmap, DCDC_CMD_OTG_REG, &v))
		return -EIO;
	return sysfs_emit(buf, "%d\n", !!(v & OTG_EN_BIT));
}

static ssize_t otg_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct crux_smb5 *chg = dev_get_drvdata(dev);
	bool en;
	int rc;

	if (kstrtobool(buf, &en))
		return -EINVAL;

	if (en) {
		rc = regmap_write(chg->regmap, DCDC_OTG_CURRENT_LIMIT_CFG_REG,
				  OTG_CL_DEFAULT_UA / OTG_CL_STEP_UA);
		if (rc)
			return rc;
	}

	rc = regmap_update_bits(chg->regmap, DCDC_CMD_OTG_REG, OTG_EN_BIT,
				en ? OTG_EN_BIT : 0);
	if (rc)
		return rc;

	dev_info(chg->dev, "otg -> %d (%u mA)\n", en,
		 en ? OTG_CL_DEFAULT_UA / 1000 : 0);
	return count;
}
static DEVICE_ATTR_RW(otg);

/*
 * hvdcp：HVDCP（QC）状态与重新协商。
 *   读：打印 APSD 识别结果和当前协商电压（5V/9V/12V）
 *   写 1：重跑一次 APSD（换充电器后若没升压，写 1 让它重新协商）
 */
static ssize_t hvdcp_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct crux_smb5 *chg = dev_get_drvdata(dev);
	unsigned int apsd = 0, qc = 0;
	const char *volt = "unknown";
	int n = 0;

	regmap_read(chg->regmap, USBIN_BASE + 0x08, &apsd);
	regmap_read(chg->regmap, QC_CHANGE_STATUS_REG, &qc);

	switch (qc & QC_2P0_STATUS_MASK) {
	case QC_5V_BIT:
		volt = "5V";
		break;
	case QC_9V_BIT:
		volt = "9V";
		break;
	case QC_12V_BIT:
		volt = "12V";
		break;
	default:
		break;
	}

	n += sysfs_emit(buf, "apds_result=0x%02x negotiated=%s\n", apsd, volt);
	n += sysfs_emit_at(buf, n, "flags: sdp=%d ocp=%d cdp=%d dcp=%d float=%d qc2=%d qc3=%d\n",
			!!(apsd & BIT(0)), !!(apsd & BIT(1)), !!(apsd & BIT(2)),
			!!(apsd & BIT(3)), !!(apsd & BIT(4)),
			!!(apsd & BIT(5)), !!(apsd & BIT(6)));
	return n;
}

static ssize_t hvdcp_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct crux_smb5 *chg = dev_get_drvdata(dev);
	bool run;
	int rc;

	if (kstrtobool(buf, &run))
		return -EINVAL;
	if (!run)
		return count;

	rc = regmap_update_bits(chg->regmap, CMD_APSD_REG, APSD_RERUN_BIT,
				APSD_RERUN_BIT);
	if (rc)
		return rc;

	dev_info(chg->dev, "APSD rerun requested\n");
	return count;
}
static DEVICE_ATTR_RW(hvdcp);

/*
 * input_current_limit：输入限流（mA），走参数表编码。
 * PD/HVDCP 升到 9V/12V 后，用户态（crux-ctl battery 插件）会把它抬到 2A，
 * 让输入功率从 7.5W 提到约 18W；5V 时保持 1.5A。
 * 范围限制 100~3000mA：SMB5 硬件更大，但老化电池 + USB 接口按 3A 以内取。
 */
static ssize_t input_current_limit_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct crux_smb5 *chg = dev_get_drvdata(dev);
	u32 v;

	if (crux_param_read(chg, &crux_icl, &v))
		return -EIO;
	return sysfs_emit(buf, "%u\n", v / 1000);
}

static ssize_t input_current_limit_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct crux_smb5 *chg = dev_get_drvdata(dev);
	u32 ma;
	int rc;

	rc = kstrtou32(buf, 10, &ma);
	if (rc)
		return rc;
	if (ma < 100 || ma > 3000)
		return -ERANGE;

	rc = crux_param_write(chg, &crux_icl, ma * 1000);
	if (rc)
		return rc;

	dev_info(chg->dev, "icl -> %u mA\n", ma);
	return count;
}
static DEVICE_ATTR_RW(input_current_limit);

/*
 * aicl：PMIC 的自动输入限流（AICL）开关。
 * 默认关闭（0），让 ICL 完全按我们下发的值执行。
 * 原因：实测 PD 12V 下 AICL 会把输入限流收缩到 0（ICL_STATUS_REG=0x00），
 * 造成 USB 明明在供电（USE_USBIN=1/VALID=1）却不取电，电池只能放电补足。
 * 写 1 可恢复 PMIC 自动收缩（用于极弱的充电器/劣质线材场景）。
 */
static ssize_t aicl_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct crux_smb5 *chg = dev_get_drvdata(dev);
	unsigned int v;

	if (regmap_read(chg->regmap, USBIN_AICL_OPTIONS_CFG_REG, &v))
		return -EIO;
	return sysfs_emit(buf, "%d\n", !!(v & USBIN_AICL_EN_BIT));
}

static ssize_t aicl_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct crux_smb5 *chg = dev_get_drvdata(dev);
	bool en;
	int rc;

	if (kstrtobool(buf, &en))
		return -EINVAL;

	rc = regmap_update_bits(chg->regmap, USBIN_AICL_OPTIONS_CFG_REG,
				USBIN_AICL_EN_BIT, en ? USBIN_AICL_EN_BIT : 0);
	if (rc)
		return rc;

	dev_info(chg->dev, "aicl -> %d\n", en);
	return count;
}
static DEVICE_ATTR_RW(aicl);





/* probe 时一次性下发"老化电池安全策略"（学原厂的集中配置） */
static void crux_apply_policy(struct crux_smb5 *chg)
{
	u32 fcc = 0, fv = 0, icl = 0;
	unsigned int v;

	/* 1) 快充电流：老化电池先给保守档 */
	if (crux_param_write(chg, &crux_fcc, CRUX_FCC_SAFE_UA))
		dev_warn(chg->dev, "set fcc failed\n");

	/* 2) 满充电压：4.35V（原 XBL 默认 4.40V 对 1153 次循环的电池偏高） */
	if (crux_param_write(chg, &crux_fv, CRUX_FV_SAFE_UV))
		dev_warn(chg->dev, "set fv failed\n");

	/* 3) 硬 JEITA：默认关闭（阈值未正确配置时会使能后直接停充）
	 *    温度保护由软件 JEITA 承担。
	 */
	if (regmap_write(chg->regmap, JEITA_EN_CFG, CRUX_JEITA_EN))
		dev_warn(chg->dev, "set jeita_en failed\n");

	/* 4) 充电安全定时器 384min（防止异常情况下无限充） */
	regmap_update_bits(chg->regmap, CHGR_FAST_CHARGE_SAFETY_TIMER_CFG,
			   GENMASK(1, 0), SAFETY_TIMER_384_MIN);

	/* 5) 终止电流走模拟通道，由 PMIC 按 FCC 比例自动判满 */
	regmap_update_bits(chg->regmap, CHGR_ENG_CHARGING_CFG,
			   CHGR_ITERM_USE_ANALOG_BIT, CHGR_ITERM_USE_ANALOG_BIT);

	/* 6) boost 阈值抬到 3.6V：VPH_PWR 跌落余量更大（针对 SMPL 硬复位） */
	if (regmap_update_bits(chg->regmap, DCDC_ENG_SDCDC_CFG5_REG,
			       ENG_SDCDC_BAT_HPWR_MASK, BOOST_THRESH_3P6V))
		dev_warn(chg->dev, "boost threshold not writable (expect 3.3V default)\n");

	/* 7) HVDCP 自主快充：PMIC 直接跟充电器协商 9V/12V，不需要 AP 参与。
	 *    原厂 qpnp-smb5.c: val = AUTH_ALG | HVDCP_EN | AUTONOMOUS
	 */
	if (regmap_update_bits(chg->regmap, USBIN_OPTIONS_1_CFG_REG,
			       HVDCP_AUTH_ALG_EN_CFG_BIT | HVDCP_EN_BIT |
			       HVDCP_AUTONOMOUS_MODE_EN_CFG_BIT,
			       HVDCP_AUTH_ALG_EN_CFG_BIT | HVDCP_EN_BIT |
			       HVDCP_AUTONOMOUS_MODE_EN_CFG_BIT))
		dev_warn(chg->dev, "hvdcp config failed\n");
	else
		dev_info(chg->dev, "hvdcp: autonomous mode enabled (QC 9V/12V)\n");

	/* 6.5) 关掉 AICL：让 ICL 固定为我们下发的值。
	 *      12V/PD 下 AICL 会误把限流收缩到 0，导致干脆不取电。
	 */
	if (regmap_update_bits(chg->regmap, USBIN_AICL_OPTIONS_CFG_REG,
			       USBIN_AICL_EN_BIT, 0))
		dev_warn(chg->dev, "aicl disable failed\n");
	else
		dev_info(chg->dev, "aicl: disabled (ICL fixed at policy value)\n");

	/* 配置改完后立刻重跑一次 APSD：PMIC 才知道按新配置重新协商电压 */
	regmap_update_bits(chg->regmap, CMD_APSD_REG, APSD_RERUN_BIT,
			   APSD_RERUN_BIT);

	/* 回读确认 */
	crux_param_read(chg, &crux_fcc, &fcc);
	crux_param_read(chg, &crux_fv, &fv);
	crux_param_read(chg, &crux_icl, &icl);
	regmap_read(chg->regmap, JEITA_EN_CFG, &v);

	dev_info(chg->dev,
		 "policy: fcc=%u mA fv=%u mV icl=%u mA jeita_en=0x%02x safety_timer=384min\n",
		 fcc / 1000, fv / 1000, icl / 1000, v);
}

static int crux_smb5_probe(struct platform_device *pdev)
{
	struct power_supply_config psy_cfg = {};
	struct crux_smb5 *chg;
	unsigned int v;

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

	chg->psy = devm_power_supply_register(&pdev->dev, &crux_smb5_desc,
					      &psy_cfg);
	if (IS_ERR(chg->psy)) {
		dev_err(&pdev->dev, "power_supply_register failed: %ld\n",
			PTR_ERR(chg->psy));
		return PTR_ERR(chg->psy);
	}

	platform_set_drvdata(pdev, chg);

	/* 先套策略，再取初始状态 */
	crux_apply_policy(chg);

	/* 配好 die temp 热调节源，让 TEMP_RANGE_STATUS 有意义 */
	if (crux_thermreg_init(chg))
		dev_warn(&pdev->dev, "thermreg src cfg failed\n");
	else
		dev_info(&pdev->dev, "thermreg: die temp ADC+CMP src enabled\n");

	chg->last_online = crux_input_online(chg) ? 1 : 0;
	chg->last_status = crux_charger_status(chg);
	chg->last_jeita_stage = -1;

	if (!regmap_read(chg->regmap, POWER_PATH_STATUS_REG, &v))
		dev_info(&pdev->dev,
			 "power_path: 0x%02x (valid_src=%d use_usbin=%d susp=%d path=%d)\n",
			 v, !!(v & VALID_INPUT_POWER_SOURCE_STS_BIT),
			 !!(v & USE_USBIN_BIT), !!(v & USBIN_SUSPEND_STS_BIT),
			 (int)FIELD_GET(POWER_PATH_MASK, v));

	dev_info(&pdev->dev, "registered crux-charger: online=%d status=%d\n",
		 chg->last_online, chg->last_status);

	INIT_DELAYED_WORK(&chg->poll_work, crux_smb5_poll);
	schedule_delayed_work(&chg->poll_work, msecs_to_jiffies(2000));

	device_create_file(&pdev->dev, &dev_attr_regs);
	device_create_file(&pdev->dev, &dev_attr_charge_enable);
	device_create_file(&pdev->dev, &dev_attr_otg);
	device_create_file(&pdev->dev, &dev_attr_hvdcp);
	device_create_file(&pdev->dev, &dev_attr_input_current_limit);
	device_create_file(&pdev->dev, &dev_attr_aicl);
	device_create_file(&pdev->dev, &dev_attr_reg);

	return 0;
}

static void crux_smb5_remove(struct platform_device *pdev)
{
	struct crux_smb5 *chg = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&chg->poll_work);

	device_remove_file(&pdev->dev, &dev_attr_regs);
	device_remove_file(&pdev->dev, &dev_attr_charge_enable);
	device_remove_file(&pdev->dev, &dev_attr_otg);
	device_remove_file(&pdev->dev, &dev_attr_hvdcp);
	device_remove_file(&pdev->dev, &dev_attr_input_current_limit);
	device_remove_file(&pdev->dev, &dev_attr_aicl);
	device_remove_file(&pdev->dev, &dev_attr_reg);

	if (chg->batt_psy)
		power_supply_put(chg->batt_psy);
}

static const struct of_device_id crux_smb5_of_match[] = {
	{ .compatible = "crux,smb5" },
	{ .compatible = "crux,smb5-probe" },
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

MODULE_DESCRIPTION("crux: PM8150B SMB5 charger driver (policy-oriented)");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("crux");
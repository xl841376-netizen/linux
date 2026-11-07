// SPDX-License-Identifier: GPL-2.0-only
/*
 * crux_smb1390.c - minimal SMB1390 charge-pump driver for Xiaomi crux (SM8150)
 *
 * Master @0x10 and Slave @0x18 sit on the same I2C bus (qupv3 i2c@890000).
 * The slave's EN input is driven by the master's EN_SL pin (hardware cascade),
 * so software only ever toggles CMD_EN_SL on the master.
 *
 * Instances are created from userspace with:
 *   echo crux_smb1390_master 0x10 > /sys/bus/i2c/devices/i2c-0/new_device
 *   echo crux_smb1390_slave  0x18 > /sys/bus/i2c/devices/i2c-0/new_device
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#define CORE_STATUS1_REG	0x1006
#define CORE_STATUS2_REG	0x1007
#define CORE_STATUS3_REG	0x1008
#define CORE_INT_RT_STS_REG	0x1010
#define CORE_CONTROL1_REG	0x1020
#define CORE_FTRIM_ILIM_REG	0x1030
#define CORE_FTRIM_LVL_REG	0x1033
#define CORE_FTRIM_MISC_REG	0x1034

#define CMD_EN_SWITCHER_BIT	BIT(0)
#define CMD_EN_SL_BIT		BIT(1)
#define CFG_ILIM_MASK		GENMASK(4, 0)

/* ILIM encoding: code = (uA - 500000) / 100000, 5 bits -> 500mA .. 3.6A */
#define ILIM_UA_BASE		500000
#define ILIM_UA_STEP		100000
#define ILIM_UA_CODE_MAX	31
/* The chip is not accurate below 900mA; the vendor driver refuses to arm it. */
#define ILIM_UA_ARM_MIN		900000

#define CP_ROLE_MASTER		0
#define CP_ROLE_SLAVE		1

struct crux_cp {
	struct i2c_client *client;
	struct regmap *regmap;
	struct mutex lock;
	bool master;
};

static const struct regmap_config crux_cp_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0x1100,
	.cache_type = REGCACHE_NONE,
};

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static int crux_cp_read(struct crux_cp *cp, u16 reg, unsigned int *val)
{
	int rc = regmap_read(cp->regmap, reg, val);

	if (rc < 0)
		dev_err(&cp->client->dev, "read 0x%04x failed: %d\n", reg, rc);

	return rc;
}

static int __maybe_unused crux_cp_read_log(struct crux_cp *cp, u16 reg,
					   unsigned int *val)
{
	unsigned int v;
	int rc = crux_cp_read(cp, reg, &v);

	if (rc < 0)
		return rc;

	*val = v;
	return 0;
}

static int crux_cp_ilim_code(unsigned int ua)
{
	return DIV_ROUND_CLOSEST((int)ua - ILIM_UA_BASE, ILIM_UA_STEP);
}

static void crux_cp_status_text(unsigned int st1, unsigned int st2,
				unsigned int st3, char *out)
{
	out[0] = '\0';

	if (st1 & BIT(0))
		strlcat(out, "WIN_OV ", 128);
	if (st1 & BIT(1))
		strlcat(out, "WIN_UV ", 128);
	if (st1 & BIT(2))
		strlcat(out, "EN_PIN_OUT ", 128);
	if (st1 & BIT(3))
		strlcat(out, "LCM_AUTO ", 128);
	if (st1 & BIT(4))
		strlcat(out, "LCM_PIN ", 128);
	if (st1 & BIT(5))
		strlcat(out, "ILIM ", 128);
	if (st1 & BIT(6))
		strlcat(out, "TEMP_ALARM ", 128);
	if (st1 & BIT(7))
		strlcat(out, "VPH_OV_SOFT ", 128);

	if (st2 & BIT(0))
		strlcat(out, "HOLD_OFF ", 128);
	if (st2 & BIT(1))
		strlcat(out, "VPH_OV_HARD ", 128);
	if (st2 & BIT(2))
		strlcat(out, "TSD ", 128);
	if (st2 & BIT(3))
		strlcat(out, "IREV ", 128);
	if (st2 & BIT(4))
		strlcat(out, "IOC ", 128);
	if (st2 & BIT(5))
		strlcat(out, "VIN_UV ", 128);
	if (st2 & BIT(6))
		strlcat(out, "VIN_OV ", 128);
	if (st2 & BIT(7))
		strlcat(out, "EN_PIN_OUT2 ", 128);

	if (st3 & BIT(0))
		strlcat(out, "EN_SL ", 128);
	if (st3 & BIT(1))
		strlcat(out, "IIN_REF_SS_DONE ", 128);
	if (st3 & BIT(2))
		strlcat(out, "FLYCAP_SS_DONE ", 128);
	if (st3 & BIT(3))
		strlcat(out, "SL_DETECTED ", 128);
}

/* ------------------------------------------------------------------ */
/* sysfs: role                                                        */
/* ------------------------------------------------------------------ */

static ssize_t role_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct crux_cp *cp = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s 0x%02x\n",
			  cp->master ? "master" : "slave", cp->client->addr);
}
static DEVICE_ATTR_RO(role);

/* ------------------------------------------------------------------ */
/* sysfs: enable (master: CMD_EN_SWITCHER; slave: its own switcher)   */
/* ------------------------------------------------------------------ */

static ssize_t enable_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct crux_cp *cp = dev_get_drvdata(dev);
	unsigned int val;
	int rc;

	rc = crux_cp_read(cp, CORE_CONTROL1_REG, &val);
	if (rc < 0)
		return rc;

	return sysfs_emit(buf, "%u\n", !!(val & CMD_EN_SWITCHER_BIT));
}

static ssize_t enable_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct crux_cp *cp = dev_get_drvdata(dev);
	unsigned int v, ilim, st1 = 0, st2 = 0, st3 = 0;
	int rc;
	char txt[128];

	rc = kstrtouint(buf, 0, &v);
	if (rc)
		return rc;

	mutex_lock(&cp->lock);

	if (v) {
		rc = crux_cp_read(cp, CORE_FTRIM_ILIM_REG, &ilim);
		if (rc)
			goto out_unlock;
		ilim &= CFG_ILIM_MASK;
		if (ilim * ILIM_UA_STEP + ILIM_UA_BASE < ILIM_UA_ARM_MIN) {
			dev_warn(&cp->client->dev,
				 "ILIM code %u (%umA) too low, refusing to arm\n",
				 ilim, (ilim * ILIM_UA_STEP + ILIM_UA_BASE) / 1000);
			rc = -EINVAL;
			goto out_unlock;
		}
	}

	rc = regmap_update_bits(cp->regmap, CORE_CONTROL1_REG,
				CMD_EN_SWITCHER_BIT,
				v ? CMD_EN_SWITCHER_BIT : 0);
	if (rc < 0) {
		dev_err(&cp->client->dev, "write CONTROL1 failed: %d\n", rc);
		goto out_unlock;
	}

	if (v) {
		msleep(20);
		crux_cp_read_log(cp, CORE_STATUS1_REG, &st1);
		crux_cp_read_log(cp, CORE_STATUS2_REG, &st2);
		crux_cp_read_log(cp, CORE_STATUS3_REG, &st3);
		crux_cp_status_text(st1, st2, st3, txt);
		dev_info(&cp->client->dev,
			 "switcher on: st1=0x%02x st2=0x%02x st3=0x%02x [%s]\n",
			 st1, st2, st3, txt);
	} else {
		dev_info(&cp->client->dev, "switcher off\n");
	}

	mutex_unlock(&cp->lock);
	return count;

out_unlock:
	mutex_unlock(&cp->lock);
	return rc;
}
static DEVICE_ATTR_RW(enable);

/* ------------------------------------------------------------------ */
/* sysfs: slave_enable (master only: CMD_EN_SL)                       */
/* ------------------------------------------------------------------ */

static ssize_t slave_enable_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct crux_cp *cp = dev_get_drvdata(dev);
	unsigned int val;
	int rc;

	if (!cp->master)
		return -EOPNOTSUPP;

	rc = crux_cp_read(cp, CORE_CONTROL1_REG, &val);
	if (rc < 0)
		return rc;

	return sysfs_emit(buf, "%u\n", !!(val & CMD_EN_SL_BIT));
}

static ssize_t slave_enable_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct crux_cp *cp = dev_get_drvdata(dev);
	unsigned int v;
	int rc;

	if (!cp->master)
		return -EOPNOTSUPP;

	rc = kstrtouint(buf, 0, &v);
	if (rc)
		return rc;

	mutex_lock(&cp->lock);
	rc = regmap_update_bits(cp->regmap, CORE_CONTROL1_REG, CMD_EN_SL_BIT,
				v ? CMD_EN_SL_BIT : 0);
	mutex_unlock(&cp->lock);
	if (rc < 0) {
		dev_err(&cp->client->dev, "write EN_SL failed: %d\n", rc);
		return rc;
	}

	dev_info(&cp->client->dev, "slave %s\n", v ? "enabled" : "disabled");
	return count;
}
static DEVICE_ATTR_RW(slave_enable);

/* ------------------------------------------------------------------ */
/* sysfs: ilim_ua                                                     */
/* ------------------------------------------------------------------ */

static ssize_t ilim_ua_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct crux_cp *cp = dev_get_drvdata(dev);
	unsigned int val;
	int rc;

	rc = crux_cp_read(cp, CORE_FTRIM_ILIM_REG, &val);
	if (rc < 0)
		return rc;

	val &= CFG_ILIM_MASK;
	return sysfs_emit(buf, "%u\n", val * ILIM_UA_STEP + ILIM_UA_BASE);
}

static ssize_t ilim_ua_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct crux_cp *cp = dev_get_drvdata(dev);
	unsigned int ua, code;
	int rc;

	rc = kstrtouint(buf, 0, &ua);
	if (rc)
		return rc;

	if (ua < ILIM_UA_ARM_MIN || ua > ILIM_UA_BASE + ILIM_UA_CODE_MAX * ILIM_UA_STEP)
		return -EINVAL;

	code = crux_cp_ilim_code(ua);

	mutex_lock(&cp->lock);
	rc = regmap_update_bits(cp->regmap, CORE_FTRIM_ILIM_REG,
				CFG_ILIM_MASK, code);
	mutex_unlock(&cp->lock);
	if (rc < 0) {
		dev_err(&cp->client->dev, "write ILIM failed: %d\n", rc);
		return rc;
	}

	dev_info(&cp->client->dev, "ILIM set to %umA (code %u)\n",
		 (code * ILIM_UA_STEP + ILIM_UA_BASE) / 1000, code);

	return count;
}
static DEVICE_ATTR_RW(ilim_ua);

/* ------------------------------------------------------------------ */
/* sysfs: status                                                      */
/* ------------------------------------------------------------------ */

static ssize_t status_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct crux_cp *cp = dev_get_drvdata(dev);
	unsigned int st1 = 0, st2 = 0, st3 = 0, rt = 0, ctl = 0, ilim = 0;
	char t1[128], t2[128], t3[128];
	int n = 0;

	crux_cp_read_log(cp, CORE_STATUS1_REG, &st1);
	crux_cp_read_log(cp, CORE_STATUS2_REG, &st2);
	crux_cp_read_log(cp, CORE_STATUS3_REG, &st3);
	crux_cp_read_log(cp, CORE_INT_RT_STS_REG, &rt);
	crux_cp_read_log(cp, CORE_CONTROL1_REG, &ctl);
	crux_cp_read_log(cp, CORE_FTRIM_ILIM_REG, &ilim);

	crux_cp_status_text(st1, st2, st3, t1);
	snprintf(t2, sizeof(t2), "%s", t1);	/* keep it simple */
	t3[0] = '\0';

	n += sysfs_emit_at(buf, n, "role      : %s 0x%02x\n",
			   cp->master ? "master" : "slave", cp->client->addr);
	n += sysfs_emit_at(buf, n, "STATUS1   : 0x%02x\n", st1);
	n += sysfs_emit_at(buf, n, "STATUS2   : 0x%02x\n", st2);
	n += sysfs_emit_at(buf, n, "STATUS3   : 0x%02x\n", st3);
	n += sysfs_emit_at(buf, n, "INT_RT    : 0x%02x\n", rt);
	n += sysfs_emit_at(buf, n, "CONTROL1  : 0x%02x [switcher=%s en_sl=%s]\n",
			   ctl, (ctl & CMD_EN_SWITCHER_BIT) ? "on" : "off",
			   (ctl & CMD_EN_SL_BIT) ? "on" : "off");
	n += sysfs_emit_at(buf, n, "ILIM      : 0x%02x -> %umA\n",
			   ilim & CFG_ILIM_MASK,
			   ((ilim & CFG_ILIM_MASK) * ILIM_UA_STEP
			    + ILIM_UA_BASE) / 1000);
	n += sysfs_emit_at(buf, n, "flags     : [%s]\n", t2);
	n += sysfs_emit_at(buf, n, "state     : %s\n",
			   (st3 & BIT(2)) ? "running (flycap charged)"
					  : ((ctl & CMD_EN_SWITCHER_BIT)
					     ? "starting" : "idle"));
	return n;
}
static DEVICE_ATTR_RO(status);

/* ------------------------------------------------------------------ */
/* sysfs: regs (raw dump of the interesting window)                   */
/* ------------------------------------------------------------------ */

static const u16 crux_cp_regs[] = {
	0x1000, 0x1001, 0x1006, 0x1007, 0x1008, 0x1010,
	0x1020, 0x1030, 0x1033, 0x1034,
};

static ssize_t regs_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct crux_cp *cp = dev_get_drvdata(dev);
	int n = 0, i;
	unsigned int v;
	int rc;

	mutex_lock(&cp->lock);
	for (i = 0; i < ARRAY_SIZE(crux_cp_regs); i++) {
		rc = regmap_read(cp->regmap, crux_cp_regs[i], &v);
		if (rc < 0)
			n += sysfs_emit_at(buf, n, "0x%04x: <err %d>\n",
					   crux_cp_regs[i], rc);
		else
			n += sysfs_emit_at(buf, n, "0x%04x: 0x%02x\n",
					   crux_cp_regs[i], v);
	}
	mutex_unlock(&cp->lock);

	return n;
}
static DEVICE_ATTR_RO(regs);

static struct device_attribute *crux_cp_attrs[] = {
	&dev_attr_role,
	&dev_attr_enable,
	&dev_attr_slave_enable,
	&dev_attr_ilim_ua,
	&dev_attr_status,
	&dev_attr_regs,
	NULL,
};

/* ------------------------------------------------------------------ */
/* probe / remove                                                     */
/* ------------------------------------------------------------------ */

static int crux_cp_probe(struct i2c_client *client)
{
	struct crux_cp *cp;
	unsigned int st1 = 0, st2 = 0, st3 = 0, ilim = 0;
	int rc, i;
	char txt[128];

	cp = devm_kzalloc(&client->dev, sizeof(*cp), GFP_KERNEL);
	if (!cp)
		return -ENOMEM;

	cp->client = client;
	mutex_init(&cp->lock);
	cp->master = (client->addr == 0x10);

	cp->regmap = devm_regmap_init_i2c(client, &crux_cp_regmap_config);
	if (IS_ERR(cp->regmap)) {
		rc = PTR_ERR(cp->regmap);
		dev_err(&client->dev, "regmap init failed: %d\n", rc);
		return rc;
	}

	i2c_set_clientdata(client, cp);
	dev_set_drvdata(&client->dev, cp);

	/* Read-only probe: never arm the switcher from here. */
	crux_cp_read_log(cp, CORE_FTRIM_ILIM_REG, &ilim);
	crux_cp_read_log(cp, CORE_STATUS1_REG, &st1);
	crux_cp_read_log(cp, CORE_STATUS2_REG, &st2);
	crux_cp_read_log(cp, CORE_STATUS3_REG, &st3);
	crux_cp_status_text(st1, st2, st3, txt);

	dev_info(&client->dev,
		 "probe %s: ILIM=%umA st1=0x%02x st2=0x%02x st3=0x%02x [%s]\n",
		 cp->master ? "master" : "slave",
		 ((ilim & CFG_ILIM_MASK) * ILIM_UA_STEP + ILIM_UA_BASE) / 1000,
		 st1, st2, st3, txt);

	for (i = 0; crux_cp_attrs[i]; i++) {
		rc = device_create_file(&client->dev, crux_cp_attrs[i]);
		if (rc) {
			dev_err(&client->dev, "create %s failed: %d\n",
				crux_cp_attrs[i]->attr.name, rc);
			while (--i >= 0)
				device_remove_file(&client->dev, crux_cp_attrs[i]);
			return rc;
		}
	}

	return 0;
}

static void crux_cp_remove(struct i2c_client *client)
{
	struct crux_cp *cp = i2c_get_clientdata(client);
	int i;

	/* never leave the pump armed behind our back */
	if (cp && cp->master)
		regmap_update_bits(cp->regmap, CORE_CONTROL1_REG,
				   CMD_EN_SWITCHER_BIT | CMD_EN_SL_BIT, 0);

	for (i = 0; crux_cp_attrs[i]; i++)
		device_remove_file(&client->dev, crux_cp_attrs[i]);
}

static const struct i2c_device_id crux_cp_id[] = {
	{ "crux_smb1390_master", CP_ROLE_MASTER },
	{ "crux_smb1390_slave", CP_ROLE_SLAVE },
	{ }
};
MODULE_DEVICE_TABLE(i2c, crux_cp_id);

static const struct of_device_id crux_cp_of_match[] = {
	{ .compatible = "crux,smb1390-master" },
	{ .compatible = "crux,smb1390-slave" },
	{ }
};
MODULE_DEVICE_TABLE(of, crux_cp_of_match);

static struct i2c_driver crux_cp_driver = {
	.driver = {
		.name = "crux_smb1390",
		.of_match_table = crux_cp_of_match,
	},
	.probe = crux_cp_probe,
	.remove = crux_cp_remove,
	.id_table = crux_cp_id,
};
module_i2c_driver(crux_cp_driver);

MODULE_DESCRIPTION("Minimal SMB1390 charge-pump driver (Xiaomi crux, SM8150)");
MODULE_AUTHOR("crux bring-up");
MODULE_LICENSE("GPL");

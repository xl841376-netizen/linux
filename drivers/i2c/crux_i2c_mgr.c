// SPDX-License-Identifier: GPL-2.0-only
/*
 * crux_i2c_mgr.c - generic I2C sub-device manager for crux
 *
 * 目标：在内核层提供一个"通用 i2c 子设备管理"能力，而不是为每颗芯片写
 *       一份专用驱动。任何挂在 i2c 总线上、又没有专属驱动的设备，都可以
 *       被本框架实例化、命名、暴露统一的 sysfs 读写通道。
 *
 * 它做的事：
 *   1) 监听 i2c bus notifier，等目标 adapter 出现（避免加载顺序问题）；
 *   2) 对参数里列出的每个地址调用 i2c_new_client_device() 实例化子设备；
 *      若该地址已被其它驱动占用（-EBUSY），只报告，不抢占；
 *   3) 每个子设备由一个通用 client 驱动接管，在 /sys/bus/i2c/devices/N-XXXX/
 *      下提供：
 *         reg_bits   (ro)  寄存器地址宽度
 *         writable   (ro)  是否允许写
 *         rw         (rw)  读: echo 1006 > rw ; 写: echo 1020=1 > rw
 *
 * 加载方式：
 *   insmod crux_i2c_mgr.ko bus_nr=0 addrs=0x10,0x18 reg_bits=16 writable=1
 *   或 /etc/modprobe.d/crux_i2c_mgr.conf:
 *     options crux_i2c_mgr bus_nr=0 addrs=0x10,0x18 reg_bits=16 writable=1
 *
 * 注意：本框架只管"通道"，不做策略。策略（什么时候开、开多大）仍然在
 *       crux 引擎的插件里，例如 cp.py。
 */

#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#define DRV_NAME	"crux_i2c_mgr"

static int bus_nr = 0;
module_param(bus_nr, int, 0644);
MODULE_PARM_DESC(bus_nr, "I2C adapter number to manage (default 0)");

static char *addrs = "0x10,0x18";
module_param(addrs, charp, 0644);
MODULE_PARM_DESC(addrs, "Comma separated client addresses (hex allowed)");

static int reg_bits = 16;
module_param(reg_bits, int, 0644);
MODULE_PARM_DESC(reg_bits, "Register address width in bits (8 or 16)");

static bool writable = true;
module_param(writable, bool, 0644);
MODULE_PARM_DESC(writable, "Allow writes through the generic rw attribute");

/* ------------------------------------------------------------------ */
/* generic client driver                                              */
/* ------------------------------------------------------------------ */

struct crux_cdev {
	struct i2c_client *client;
	u16 last_reg;
	u8 last_val;
};

static int crux_cdev_xfer(struct i2c_client *client, u16 reg, u8 *val,
			  bool is_write)
{
	struct i2c_msg msgs[2];
	u8 wbuf[3];
	int rlen = reg_bits / 8;
	int n, ret;

	if (rlen != 1 && rlen != 2)
		return -EINVAL;

	if (rlen == 2) {
		wbuf[0] = (reg >> 8) & 0xff;
		wbuf[1] = reg & 0xff;
	} else {
		if (reg > 0xff)
			return -EINVAL;
		wbuf[0] = reg & 0xff;
	}

	if (is_write) {
		wbuf[rlen] = *val;
		msgs[0].addr = client->addr;
		msgs[0].flags = 0;
		msgs[0].len = rlen + 1;
		msgs[0].buf = wbuf;
		n = 1;
	} else {
		msgs[0].addr = client->addr;
		msgs[0].flags = 0;
		msgs[0].len = rlen;
		msgs[0].buf = wbuf;

		msgs[1].addr = client->addr;
		msgs[1].flags = I2C_M_RD;
		msgs[1].len = 1;
		msgs[1].buf = val;
		n = 2;
	}

	ret = i2c_transfer(client->adapter, msgs, n);
	if (ret < 0)
		return ret;
	if (ret != n)
		return -EIO;
	return 0;
}

static ssize_t reg_bits_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	return sysfs_emit(buf, "%d\n", reg_bits);
}
static DEVICE_ATTR_RO(reg_bits);

static ssize_t writable_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	return sysfs_emit(buf, "%d\n", writable ? 1 : 0);
}
static DEVICE_ATTR_RO(writable);

static ssize_t rw_show(struct device *dev, struct device_attribute *attr,
		       char *buf)
{
	struct crux_cdev *cd = dev_get_drvdata(dev);
	u8 val = 0;
	int rc;

	if (!cd)
		return -ENODEV;

	/* 重新读一次上次操作的寄存器，返回实时值 */
	rc = crux_cdev_xfer(cd->client, cd->last_reg, &val, false);
	if (rc < 0)
		return rc;

	cd->last_val = val;
	return sysfs_emit(buf, "%02x\n", val);
}

static ssize_t rw_store(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct crux_cdev *cd = dev_get_drvdata(dev);
	unsigned int reg, val = 0;
	char *eq;
	char tmp[64];
	bool is_write;

	if (!cd)
		return -ENODEV;

	strscpy(tmp, buf, sizeof(tmp));
	strim(tmp);
	if (!tmp[0])
		return -EINVAL;

	eq = strchr(tmp, '=');
	if (eq) {
		*eq++ = '\0';
		if (kstrtouint(eq, 16, &val))
			return -EINVAL;
		is_write = true;
		if (!writable)
			return -EPERM;
	} else {
		is_write = false;
	}

	if (kstrtouint(tmp, 16, &reg))
		return -EINVAL;
	if (reg > 0xffff)
		return -EINVAL;

	if (is_write) {
		u8 v = val & 0xff;
		int rc = crux_cdev_xfer(cd->client, reg, &v, true);

		if (rc < 0)
			return rc;
		dev_dbg(dev, "write 0x%04x = 0x%02x\n", reg, v);
	} else {
		u8 v = 0;
		int rc = crux_cdev_xfer(cd->client, reg, &v, false);

		if (rc < 0)
			return rc;
		/* 读的结果通过 rw 的 show 返回不方便，直接打印到内核日志，
		 * 同时把它缓存到 last_reg/last_val，用户可 cat rw_value。 */
		cd->last_reg = reg;
		cd->last_val = v;
	}

	return count;
}
static DEVICE_ATTR_RW(rw);

static ssize_t rw_value_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct crux_cdev *cd = dev_get_drvdata(dev);
	u8 val = 0;
	int rc;

	if (!cd)
		return -ENODEV;

	rc = crux_cdev_xfer(cd->client, cd->last_reg, &val, false);
	if (rc < 0)
		return rc;

	cd->last_val = val;
	return sysfs_emit(buf, "%02x\n", val);
}
static DEVICE_ATTR_RO(rw_value);

static ssize_t last_reg_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct crux_cdev *cd = dev_get_drvdata(dev);

	if (!cd)
		return -ENODEV;

	return sysfs_emit(buf, "0x%04x\n", cd->last_reg);
}
static DEVICE_ATTR_RO(last_reg);

static struct attribute *crux_cdev_attrs[] = {
	&dev_attr_reg_bits.attr,
	&dev_attr_writable.attr,
	&dev_attr_rw.attr,
	&dev_attr_rw_value.attr,
	&dev_attr_last_reg.attr,
	NULL,
};

static const struct attribute_group crux_cdev_group = {
	.attrs = crux_cdev_attrs,
};

static int crux_cdev_probe(struct i2c_client *client)
{
	struct crux_cdev *cd;
	int rc;

	cd = devm_kzalloc(&client->dev, sizeof(*cd), GFP_KERNEL);
	if (!cd)
		return -ENOMEM;

	cd->client = client;
	cd->last_reg = 0;
	cd->last_val = 0;
	i2c_set_clientdata(client, cd);
	dev_set_drvdata(&client->dev, cd);

	rc = sysfs_create_group(&client->dev.kobj, &crux_cdev_group);
	if (rc) {
		dev_err(&client->dev, "create sysfs group failed: %d\n", rc);
		return rc;
	}

	dev_info(&client->dev, "managed at 0x%02x (reg_bits=%d writable=%d)\n",
		 client->addr, reg_bits, writable ? 1 : 0);

	return 0;
}

static void crux_cdev_remove(struct i2c_client *client)
{
	sysfs_remove_group(&client->dev.kobj, &crux_cdev_group);
}

static const struct i2c_device_id crux_cdev_id[] = {
	{ "crux_i2cdev", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, crux_cdev_id);

static const struct of_device_id crux_cdev_of[] = {
	{ .compatible = "crux,i2c-dev" },
	{ }
};
MODULE_DEVICE_TABLE(of, crux_cdev_of);

static struct i2c_driver crux_cdev_driver = {
	.driver = {
		.name = "crux_i2cdev",
		.of_match_table = crux_cdev_of,
	},
	.probe = crux_cdev_probe,
	.remove = crux_cdev_remove,
	.id_table = crux_cdev_id,
};

/* ------------------------------------------------------------------ */
/* manager: instantiate clients on the target adapter                 */
/* ------------------------------------------------------------------ */

struct crux_inst {
	struct list_head node;
	struct i2c_client *client;
	bool owned;		/* true: we created it, must unregister */
};

static LIST_HEAD(crux_insts);
static DEFINE_MUTEX(crux_lock);
static bool attached;

static int crux_parse_addrs(u16 *out, int max)
{
	char *p, *s;
	int n = 0;

	p = kstrdup(addrs, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	while ((s = strsep(&p, ", ")) != NULL) {
		unsigned int a;

		if (!*s)
			continue;
		if (kstrtouint(s, 16, &a) || a > 0x7f) {
			pr_warn(DRV_NAME ": bad address '%s'\n", s);
			continue;
		}
		if (n >= max) {
			pr_warn(DRV_NAME ": too many addresses\n");
			break;
		}
		out[n++] = (u16)a;
	}
	kfree(p);
	return n;
}

static int crux_attach_adapter(struct i2c_adapter *adap)
{
	u16 want[16];
	int nwant, i, ok = 0, busy = 0;
	struct crux_inst *inst;

	nwant = crux_parse_addrs(want, ARRAY_SIZE(want));
	if (nwant <= 0)
		return -EINVAL;

	pr_info(DRV_NAME ": adapter i2c-%d (%s) found, instantiating %d client(s)\n",
		adap->nr, adap->name, nwant);

	for (i = 0; i < nwant; i++) {
		struct i2c_board_info info = { };
		struct i2c_client *cl;

		strscpy(info.type, "crux_i2cdev", sizeof(info.type));
		info.addr = want[i];

		cl = i2c_new_client_device(adap, &info);
		if (IS_ERR(cl)) {
			pr_info(DRV_NAME ": 0x%02x not claimed (%ld) - "
				"probably already bound to a dedicated driver\n",
				want[i], PTR_ERR(cl));
			busy++;
			continue;
		}

		inst = kzalloc(sizeof(*inst), GFP_KERNEL);
		if (!inst) {
			i2c_unregister_device(cl);
			continue;
		}
		inst->client = cl;
		inst->owned = true;
		mutex_lock(&crux_lock);
		list_add_tail(&inst->node, &crux_insts);
		mutex_unlock(&crux_lock);
		ok++;
	}

	pr_info(DRV_NAME ": %d managed, %d skipped\n", ok, busy);
	return ok ? 0 : -ENODEV;
}

static int crux_i2c_notifier(struct notifier_block *nb, unsigned long action,
			     void *data)
{
	struct device *dev = data;
	struct i2c_adapter *adap;

	if (action != BUS_NOTIFY_ADD_DEVICE)
		return NOTIFY_DONE;

	adap = i2c_verify_adapter(dev);
	if (!adap || adap->nr != bus_nr)
		return NOTIFY_DONE;

	if (attached)
		return NOTIFY_DONE;

	attached = true;
	crux_attach_adapter(adap);
	return NOTIFY_OK;
}

static struct notifier_block crux_nb = {
	.notifier_call = crux_i2c_notifier,
};

static int __init crux_mgr_init(void)
{
	int rc;

	if (reg_bits != 8 && reg_bits != 16) {
		pr_err(DRV_NAME ": reg_bits must be 8 or 16\n");
		return -EINVAL;
	}

	rc = i2c_add_driver(&crux_cdev_driver);
	if (rc) {
		pr_err(DRV_NAME ": add client driver failed: %d\n", rc);
		return rc;
	}

	bus_register_notifier(&i2c_bus_type, &crux_nb);

	/* the adapter may already exist if we are loaded late */
	if (!attached) {
		struct i2c_adapter *adap = i2c_get_adapter(bus_nr);

		if (adap) {
			attached = true;
			crux_attach_adapter(adap);
			i2c_put_adapter(adap);
		}
	}

	pr_info(DRV_NAME ": loaded (bus_nr=%d addrs=%s reg_bits=%d)\n",
		bus_nr, addrs, reg_bits);
	return 0;
}

static void __exit crux_mgr_exit(void)
{
	struct crux_inst *inst, *tmp;

	bus_unregister_notifier(&i2c_bus_type, &crux_nb);

	mutex_lock(&crux_lock);
	list_for_each_entry_safe(inst, tmp, &crux_insts, node) {
		list_del(&inst->node);
		if (inst->owned)
			i2c_unregister_device(inst->client);
		kfree(inst);
	}
	mutex_unlock(&crux_lock);

	i2c_del_driver(&crux_cdev_driver);
	pr_info(DRV_NAME ": unloaded\n");
}

module_init(crux_mgr_init);
module_exit(crux_mgr_exit);

MODULE_DESCRIPTION("Generic I2C sub-device manager (crux)");
MODULE_AUTHOR("crux bring-up");
MODULE_LICENSE("GPL");
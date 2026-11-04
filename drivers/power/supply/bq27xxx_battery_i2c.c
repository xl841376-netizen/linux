// SPDX-License-Identifier: GPL-2.0
/*
 * BQ27xxx battery monitor I2C driver
 *
 * Copyright (C) 2015 Texas Instruments Incorporated - https://www.ti.com/
 *	Andrew F. Davis <afd@ti.com>
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/unaligned.h>

#include <linux/power/bq27xxx_battery.h>

static DEFINE_IDA(battery_id);

static irqreturn_t bq27xxx_battery_irq_handler_thread(int irq, void *data)
{
	struct bq27xxx_device_info *di = data;

	bq27xxx_battery_update(di);

	return IRQ_HANDLED;
}

static int bq27xxx_battery_i2c_read(struct bq27xxx_device_info *di, u8 reg,
				    bool single)
{
	struct i2c_client *client = to_i2c_client(di->dev);
	struct i2c_msg msg[2];
	u8 data[2];
	int ret;
	int retry = 0;

	if (!client->adapter)
		return -ENODEV;

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].buf = &reg;
	msg[0].len = sizeof(reg);
	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].buf = data;
	if (single)
		msg[1].len = 1;
	else
		msg[1].len = 2;

	do {
		ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
		if (ret == -EBUSY && ++retry < 3) {
			/* sleep 10 milliseconds when busy */
			usleep_range(10000, 11000);
			continue;
		}
		break;
	} while (1);

	if (ret < 0)
		return ret;

	if (!single)
		ret = get_unaligned_le16(data);
	else
		ret = data[0];

	return ret;
}

static int bq27xxx_battery_i2c_write(struct bq27xxx_device_info *di, u8 reg,
				     int value, bool single)
{
	struct i2c_client *client = to_i2c_client(di->dev);
	struct i2c_msg msg;
	u8 data[4];
	int ret;

	if (!client->adapter)
		return -ENODEV;

	data[0] = reg;
	if (single) {
		data[1] = (u8) value;
		msg.len = 2;
	} else {
		put_unaligned_le16(value, &data[1]);
		msg.len = 3;
	}

	msg.buf = data;
	msg.addr = client->addr;
	msg.flags = 0;

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret < 0)
		return ret;
	if (ret != 1)
		return -EINVAL;
	return 0;
}


/* ---------------- crux debug: MAC / DF block access ----------------
 * BQ27Z561 TRM(SLUUC54): MAC 与 DF 都通过 AltManufacturerAccess(0x3E) 的块协议。
 *   块写 = 2 字节 LE 参数(命令码 或 DF 起始地址) [+ 最多 32 字节数据]
 *   块读 = 先写 1 字节 0x3E，再读 36 字节（参数回显 + 32 数据 + 校验/长度）
 *
 * 用法（/sys/bus/i2c/devices/0-0055/）：
 *   echo "c:0x21"  > mac     # 发 MAC 命令（如 0x0021 Gauging），结果留在 mac
 *   echo "c:0x54"  > mac     # OperationStatus
 *   echo "r:0x4000" > mac    # 读 DF 0x4000 起 32 字节
 *   echo "w:0x4000:0fa00100" > mac   # 写 DF 0x4000 = 00 0f a0 01（hex，小端）
 *   cat mac                  # 输出上次块读的 36 字节
 */
#define CRUX_MAC_MAX	36
static u8 crux_mac_buf[CRUX_MAC_MAX];
static int crux_mac_len;

static int crux_bq_mac_word_cmd(struct i2c_client *client, u8 command, u16 cmd)
{
	u8 b[3];
	struct i2c_msg m;

	b[0] = command;		/* 0x00 ManufacturerAccess / 0x3E AltManufacturerAccess */
	b[1] = (u8)(cmd & 0xff);
	b[2] = (u8)(cmd >> 8);

	m.addr = client->addr;
	m.flags = 0;
	m.len = 3;
	m.buf = b;

	return i2c_transfer(client->adapter, &m, 1) == 1 ? 0 : -EIO;
}

static int crux_bq_mac_word(struct i2c_client *client, u16 cmd)
{
	return crux_bq_mac_word_cmd(client, 0x00, cmd);
}

static int crux_bq_block_write(struct i2c_client *client, u16 word,
			       const u8 *data, int n)
{
	u8 b[2 + 32];
	struct i2c_msg m;

	if (n > 32)
		return -EINVAL;
	b[0] = 0x3e;		/* AltManufacturerAccess block */
	b[1] = (u8)(2 + n);	/* SMBus block count: addr(2) + data */
	b[2] = (u8)(word & 0xff);
	b[3] = (u8)(word >> 8);
	if (n)
		memcpy(&b[4], data, n);

	m.addr = client->addr;
	m.flags = 0;
	m.len = 4 + n;
	m.buf = b;

	return i2c_transfer(client->adapter, &m, 1) == 1 ? 0 : -EIO;
}

static int crux_bq_block_read(struct i2c_client *client, u8 *out, int len)
{
	u8 reg = 0x3e;
	struct i2c_msg m[2];

	m[0].addr = client->addr;
	m[0].flags = 0;
	m[0].len = 1;
	m[0].buf = &reg;
	m[1].addr = client->addr;
	m[1].flags = I2C_M_RD;
	m[1].len = len;
	m[1].buf = out;

	return i2c_transfer(client->adapter, m, 2) == 2 ? 0 : -EIO;
}

static ssize_t mac_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	unsigned int a;
	int ret, n = 0;
	u8 data[32];
	const char *p;

	(void)attr;
	crux_mac_len = 0;

	if (!strncmp(buf, "e:", 2)) {
		if (kstrtouint(buf + 2, 0, &a))
			return -EINVAL;
		ret = crux_bq_mac_word_cmd(client, 0x3e, (u16)a);
		if (ret)
			return ret;
		return count;
	}

	if (!strncmp(buf, "d:", 2)) {
		if (kstrtouint(buf + 2, 0, &a))
			return -EINVAL;
		ret = crux_bq_mac_word_cmd(client, 0x3e, (u16)a);
		if (ret)
			return ret;
		usleep_range(20000, 25000);
		ret = crux_bq_block_read(client, crux_mac_buf, CRUX_MAC_MAX);
		if (ret)
			return ret;
		crux_mac_len = CRUX_MAC_MAX;
		return count;
	}

	if (!strncmp(buf, "c:", 2) || !strncmp(buf, "r:", 2)) {
		if (kstrtouint(buf + 2, 0, &a))
			return -EINVAL;
		if (buf[0] == 'c')
			ret = crux_bq_mac_word(client, (u16)a);
		else
			ret = crux_bq_block_write(client, (u16)a, NULL, 0);
		if (ret)
			return ret;
		usleep_range(20000, 25000);
		ret = crux_bq_block_read(client, crux_mac_buf, CRUX_MAC_MAX);
		if (ret)
			return ret;
		crux_mac_len = CRUX_MAC_MAX;
		return count;
	}

	if (!strncmp(buf, "w:", 2)) {
		p = buf + 2;
		if (kstrtouint(p, 0, &a))
			return -EINVAL;
		p = strchr(p, ':');
		if (!p)
			return -EINVAL;
		p++;
		while (p[0] && p[1] && n < (int)sizeof(data)) {
			unsigned int byte;
			char h[3] = { p[0], p[1], 0 };

			if (kstrtouint(h, 16, &byte))
				break;
			data[n++] = (u8)byte;
			p += 2;
			while (*p == ' ')
				p++;
		}
		if (!n)
			return -EINVAL;
		ret = crux_bq_block_write(client, (u16)a, data, n);
		if (ret)
			return ret;
		usleep_range(20000, 25000);
		return count;
	}

	return -EINVAL;
}

static ssize_t mac_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	int i, n = 0;

	(void)dev;
	(void)attr;
	if (!crux_mac_len)
		return sysfs_emit(buf, "none\n");
	for (i = 0; i < crux_mac_len && n < (int)PAGE_SIZE - 4; i++)
		n += sysfs_emit_at(buf, n, "%02x ", crux_mac_buf[i]);
	n += sysfs_emit_at(buf, n, "\n");
	return n;
}
static DEVICE_ATTR_RW(mac);

/* 手工 FCC 覆盖值（mAh），0 = 不覆盖，用芯片值 */
int crux_fcc_override_mah;
EXPORT_SYMBOL_GPL(crux_fcc_override_mah);

static ssize_t fcc_override_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	(void)dev;
	(void)attr;
	return sysfs_emit(buf, "%d\n", crux_fcc_override_mah);
}

static ssize_t fcc_override_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	int v;
	(void)dev;
	(void)attr;

	if (kstrtoint(buf, 0, &v) || v < 0)
		return -EINVAL;
	crux_fcc_override_mah = v;
	return count;
}
static DEVICE_ATTR_RW(fcc_override);

static int bq27xxx_battery_i2c_bulk_read(struct bq27xxx_device_info *di, u8 reg,
					 u8 *data, int len)
{
	struct i2c_client *client = to_i2c_client(di->dev);
	int ret;

	if (!client->adapter)
		return -ENODEV;

	ret = i2c_smbus_read_i2c_block_data(client, reg, len, data);
	if (ret < 0)
		return ret;
	if (ret != len)
		return -EINVAL;
	return 0;
}

static int bq27xxx_battery_i2c_bulk_write(struct bq27xxx_device_info *di,
					  u8 reg, u8 *data, int len)
{
	struct i2c_client *client = to_i2c_client(di->dev);
	struct i2c_msg msg;
	u8 buf[33];
	int ret;

	if (!client->adapter)
		return -ENODEV;

	buf[0] = reg;
	memcpy(&buf[1], data, len);

	msg.buf = buf;
	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = len + 1;

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret < 0)
		return ret;
	if (ret != 1)
		return -EINVAL;
	return 0;
}

static void bq27xxx_battery_i2c_devm_ida_free(void *data)
{
	int num = (long)data;

	ida_free(&battery_id, num);
}

static int bq27xxx_battery_i2c_probe(struct i2c_client *client)
{
	const struct i2c_device_id *id = i2c_client_get_device_id(client);
	struct bq27xxx_device_info *di;
	int ret;
	char *name;
	long num;

	/* Get new ID for the new battery device */
	num = ida_alloc(&battery_id, GFP_KERNEL);
	if (num < 0)
		return num;
	ret = devm_add_action_or_reset(&client->dev,
				       bq27xxx_battery_i2c_devm_ida_free,
				       (void *)num);
	if (ret)
		return ret;

	name = devm_kasprintf(&client->dev, GFP_KERNEL, "%s-%ld", id->name, num);
	if (!name)
		return -ENOMEM;

	di = devm_kzalloc(&client->dev, sizeof(*di), GFP_KERNEL);
	if (!di)
		return -ENOMEM;

	di->dev = &client->dev;
	di->chip = id->driver_data;
	di->name = name;

	di->bus.read = bq27xxx_battery_i2c_read;
	di->bus.write = bq27xxx_battery_i2c_write;
	di->bus.read_bulk = bq27xxx_battery_i2c_bulk_read;
	di->bus.write_bulk = bq27xxx_battery_i2c_bulk_write;

	ret = bq27xxx_battery_setup(di);
	if (ret)
		return ret;

	/* Schedule a polling after about 1 min */
	schedule_delayed_work(&di->work, 60 * HZ);

	i2c_set_clientdata(client, di);

	device_create_file(&client->dev, &dev_attr_mac);
	device_create_file(&client->dev, &dev_attr_fcc_override);


	if (client->irq) {
		ret = request_threaded_irq(client->irq,
				NULL, bq27xxx_battery_irq_handler_thread,
				IRQF_ONESHOT,
				di->name, di);
		if (ret) {
			dev_err(&client->dev,
				"Unable to register IRQ %d error %d\n",
				client->irq, ret);
			device_remove_file(&client->dev, &dev_attr_mac);
			device_remove_file(&client->dev, &dev_attr_fcc_override);
	bq27xxx_battery_teardown(di);
			return ret;
		}
	}

	return 0;
}

static void bq27xxx_battery_i2c_remove(struct i2c_client *client)
{
	struct bq27xxx_device_info *di = i2c_get_clientdata(client);

	if (client->irq)
		free_irq(client->irq, di);

	bq27xxx_battery_teardown(di);
}

static const struct i2c_device_id bq27xxx_i2c_id_table[] = {
	{ .name = "bq27200", .driver_data = BQ27000 },
	{ .name = "bq27210", .driver_data = BQ27010 },
	{ .name = "bq27500", .driver_data = BQ2750X },
	{ .name = "bq27510", .driver_data = BQ2751X },
	{ .name = "bq27520", .driver_data = BQ2752X },
	{ .name = "bq27500-1", .driver_data = BQ27500 },
	{ .name = "bq27510g1", .driver_data = BQ27510G1 },
	{ .name = "bq27510g2", .driver_data = BQ27510G2 },
	{ .name = "bq27510g3", .driver_data = BQ27510G3 },
	{ .name = "bq27520g1", .driver_data = BQ27520G1 },
	{ .name = "bq27520g2", .driver_data = BQ27520G2 },
	{ .name = "bq27520g3", .driver_data = BQ27520G3 },
	{ .name = "bq27520g4", .driver_data = BQ27520G4 },
	{ .name = "bq27521", .driver_data = BQ27521 },
	{ .name = "bq27530", .driver_data = BQ27530 },
	{ .name = "bq27531", .driver_data = BQ27531 },
	{ .name = "bq27541", .driver_data = BQ27541 },
	{ .name = "bq27542", .driver_data = BQ27542 },
	{ .name = "bq27546", .driver_data = BQ27546 },
	{ .name = "bq27742", .driver_data = BQ27742 },
	{ .name = "bq27545", .driver_data = BQ27545 },
	{ .name = "bq27411", .driver_data = BQ27411 },
	{ .name = "bq27421", .driver_data = BQ27421 },
	{ .name = "bq27425", .driver_data = BQ27425 },
	{ .name = "bq27426", .driver_data = BQ27426 },
	{ .name = "bq27441", .driver_data = BQ27441 },
	{ .name = "bq27621", .driver_data = BQ27621 },
	{ .name = "bq27z561", .driver_data = BQ27Z561 },
	{ .name = "bq28z610", .driver_data = BQ28Z610 },
	{ .name = "bq34z100", .driver_data = BQ34Z100 },
	{ .name = "bq78z100", .driver_data = BQ78Z100 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, bq27xxx_i2c_id_table);

#ifdef CONFIG_OF
static const struct of_device_id bq27xxx_battery_i2c_of_match_table[] = {
	{ .compatible = "ti,bq27200" },
	{ .compatible = "ti,bq27210" },
	{ .compatible = "ti,bq27500" },
	{ .compatible = "ti,bq27510" },
	{ .compatible = "ti,bq27520" },
	{ .compatible = "ti,bq27500-1" },
	{ .compatible = "ti,bq27510g1" },
	{ .compatible = "ti,bq27510g2" },
	{ .compatible = "ti,bq27510g3" },
	{ .compatible = "ti,bq27520g1" },
	{ .compatible = "ti,bq27520g2" },
	{ .compatible = "ti,bq27520g3" },
	{ .compatible = "ti,bq27520g4" },
	{ .compatible = "ti,bq27521" },
	{ .compatible = "ti,bq27530" },
	{ .compatible = "ti,bq27531" },
	{ .compatible = "ti,bq27541" },
	{ .compatible = "ti,bq27542" },
	{ .compatible = "ti,bq27546" },
	{ .compatible = "ti,bq27742" },
	{ .compatible = "ti,bq27545" },
	{ .compatible = "ti,bq27411" },
	{ .compatible = "ti,bq27421" },
	{ .compatible = "ti,bq27425" },
	{ .compatible = "ti,bq27426" },
	{ .compatible = "ti,bq27441" },
	{ .compatible = "ti,bq27621" },
	{ .compatible = "ti,bq27z561" },
	{ .compatible = "ti,bq28z610" },
	{ .compatible = "ti,bq34z100" },
	{ .compatible = "ti,bq78z100" },
	{},
};
MODULE_DEVICE_TABLE(of, bq27xxx_battery_i2c_of_match_table);
#endif

static struct i2c_driver bq27xxx_battery_i2c_driver = {
	.driver = {
		.name = "bq27xxx-battery",
		.of_match_table = of_match_ptr(bq27xxx_battery_i2c_of_match_table),
		.pm = &bq27xxx_battery_battery_pm_ops,
	},
	.probe = bq27xxx_battery_i2c_probe,
	.remove = bq27xxx_battery_i2c_remove,
	.id_table = bq27xxx_i2c_id_table,
};
module_i2c_driver(bq27xxx_battery_i2c_driver);

MODULE_AUTHOR("Andrew F. Davis <afd@ti.com>");
MODULE_DESCRIPTION("BQ27xxx battery monitor i2c driver");
MODULE_LICENSE("GPL");

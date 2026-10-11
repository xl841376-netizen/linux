// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 FIXME
// Generated with linux-mdss-dsi-panel-driver-generator from vendor device tree:
//   Copyright (c) 2013, The Linux Foundation. All rights reserved. (FIXME)

#include <linux/backlight.h>
void crux_rec_dump(void); /* [crux] 录制转储 */
#include <linux/mutex.h>
#include <linux/regulator/consumer.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/property.h>

#include <linux/of.h>
#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>

#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct samsung_ea8076_f1s {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset_gpio;
	struct regulator *vddio;
	struct regulator *vcie;
	struct regulator *vci;
	bool powered;
	struct backlight_device *bl;
	bool panel_ready;
	u16 last_bl_level;
	struct mutex bl_lock;
	struct work_struct bl_work;
	u16 bl_req;
	bool bl_req_valid;
	/*
	 * [crux] 帧率管理：默认档 + 本实例的 mode 表
	 * （默认档要改 PREFERRED 标志，所以不能直接改 const 表）
	 */
	unsigned int default_fps;
	struct drm_display_mode *modes;
	/*
	 * [crux] 帧就绪后再点亮：面板复位/初始化后 GRAM 里是未定义内容，
	 * 这时候把 display 打开，切换那一瞬间就是花屏。改成延迟点亮，
	 * 等 DPU 把第一帧写进 GRAM 之后再开 display。
	 */
	struct delayed_work display_on_work;
	bool display_on_pending;
};

static inline
struct samsung_ea8076_f1s *to_samsung_ea8076_f1s(struct drm_panel *panel)
{
	return container_of_const(panel, struct samsung_ea8076_f1s, panel);
}

static int samsung_ea8076_f1s_power_on(struct samsung_ea8076_f1s *ctx)
{
	int ret;

	if (ctx->powered)
		return 0;

	if (ctx->vddio) {
		ret = regulator_enable(ctx->vddio);
		if (ret) {
			dev_err(&ctx->dsi->dev, "failed to enable vddio: %d\n", ret);
			return ret;
		}
	}
	usleep_range(2000, 3000);

	if (ctx->vcie) {
		ret = regulator_enable(ctx->vcie);
		if (ret) {
			dev_err(&ctx->dsi->dev, "failed to enable vcie: %d\n", ret);
			goto err_vcie;
		}
	}

	if (ctx->vci) {
		ret = regulator_enable(ctx->vci);
		if (ret) {
			dev_err(&ctx->dsi->dev, "failed to enable vci: %d\n", ret);
			goto err_vci;
		}
	}

	usleep_range(10000, 11000);
	ctx->powered = true;

	return 0;

err_vci:
	if (ctx->vcie)
		regulator_disable(ctx->vcie);
err_vcie:
	if (ctx->vddio)
		regulator_disable(ctx->vddio);
	return ret;
}

static void samsung_ea8076_f1s_power_off(struct samsung_ea8076_f1s *ctx)
{
	if (!ctx->powered)
		return;

	if (ctx->vci)
		regulator_disable(ctx->vci);
	if (ctx->vcie)
		regulator_disable(ctx->vcie);
	usleep_range(1000, 2000);
	if (ctx->vddio)
		regulator_disable(ctx->vddio);

	ctx->powered = false;
}

static void samsung_ea8076_f1s_power_off_action(void *data)
{
	samsung_ea8076_f1s_power_off(data);
}

static void samsung_ea8076_f1s_reset(struct samsung_ea8076_f1s *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
}

static int samsung_ea8076_f1s_on(struct samsung_ea8076_f1s *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mutex_lock(&ctx->bl_lock);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfc, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x0c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x2f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd1, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfc, 0xa5, 0xa5);
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_usleep_range(&dsi_ctx, 10000, 11000);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	/*
	 * [crux] TE 输出模式按原厂来。
	 * 原厂 f1s 节点：qcom,mdss-dsi-te-dcs-command = 1；
	 * 我们这里原来写的是 VBLANK(=0)，发出去的是 0x35 00。
	 * 这两个模式下 TE 脉冲出现的位置不同，DPU 的 tear check 是按 TE
	 * 对齐送帧的，模式不对就会在面板刷新中途把帧写进去（画面随机一部分
	 * 是新帧、一部分还是旧帧，看起来就是花屏）。
	 */
	mipi_dsi_dcs_set_tear_on_multi(&dsi_ctx, MIPI_DSI_DCS_TEAR_MODE_VHBLANK);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb7, 0x01, 0x4b);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x06);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb7, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	/*
	 * crux: 显式设列地址（0x2A）0..1079。厂商序列里只有页地址（0x2B），
	 * 因为默认列窗口应该已经是全屏；实测“画面重复错位”很像列窗口不是全屏
	 * （写帧时 GRAM 行内回绕）。这里补上 0x2A 做一次对照。（07? 见记忆）
	 */
	mipi_dsi_dcs_set_column_address_multi(&dsi_ctx, 0x0000, 0x0437);
	mipi_dsi_dcs_set_page_address_multi(&dsi_ctx, 0x0000, 0x0923);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfc, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x23);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd1, 0x16);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe9,
				     0x11, 0x55, 0xa6, 0x75, 0xa3, 0xa9, 0xeb,
				     0x71, 0x00, 0x1a, 0xb8);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd9,
				     0x14, 0x00, 0x00, 0x8f, 0x6e, 0x00, 0x00,
				     0x8f, 0x2e, 0x6e, 0x34);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc0,
				     0x31, 0x01, 0x03, 0x00, 0x06, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x50, 0x00, 0x00, 0x40,
				     0x3d, 0x00, 0x5e, 0x00, 0x00, 0x00, 0x20,
				     0x07, 0x20, 0x30, 0x58, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0xe0, 0x0b, 0x75, 0xd8,
				     0x00, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x00,
				     0x00, 0xf6, 0x97, 0x0f, 0x6d, 0xe6, 0xcd,
				     0x05, 0x21, 0x8b, 0xec, 0xed, 0xed, 0xed,
				     0x0d, 0x3f, 0x60, 0x00, 0x67, 0xfd, 0x6d,
				     0x51, 0xbc, 0xdd, 0xdd, 0x00, 0x00, 0x07,
				     0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x70, 0x2f, 0x00, 0x00, 0x03, 0x2e,
				     0x3c, 0x20, 0xe0, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x06, 0x8c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc0, 0x31);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe1,
				     0x00, 0x00, 0x02, 0x02, 0x42, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe2,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x0c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe1, 0x19);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfc, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY,
				     0x20);
	if (mipi_dsi_dcs_set_display_brightness_large(ctx->dsi,
			ctx->bl ? ctx->bl->props.brightness : 100) < 0)
		dev_warn(&ctx->dsi->dev, "failed to set initial brightness\n");
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_POWER_SAVE, 0x00);
	mipi_dsi_msleep(&dsi_ctx, 67);
	ctx->panel_ready = true;
	/*
	 * [crux] 先不发 0x29。面板刚复位/初始化，GRAM 里是没有内容的，
	 * 此刻点亮就是花屏。等 DPU 把第一帧写进 GRAM（这几百毫秒）
	 * 之后再开 display，切换那一下就不会有花屏。
	 */
	/*
	 * [crux] 交接：初始化完立刻点亮，不再等帧数/兜底延时。
	 * 花屏那一段在面板驱动起来之前（GRUB 侧预置已处理），这里等只会
	 * 让内核消息晚 3 秒才看得见。
	 */
	if (mipi_dsi_dcs_set_display_on(ctx->dsi) < 0)
		dev_warn(&ctx->dsi->dev, "crux: display on failed at enable\n");
	mutex_unlock(&ctx->bl_lock);

	return dsi_ctx.accum_err;
}

static int samsung_ea8076_f1s_off(struct samsung_ea8076_f1s *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mutex_lock(&ctx->bl_lock);
	ctx->panel_ready = false;
	ctx->last_bl_level = 0;
	ctx->display_on_pending = false;
	cancel_delayed_work(&ctx->display_on_work);
	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);
	mutex_unlock(&ctx->bl_lock);

	return dsi_ctx.accum_err;
}

static int samsung_ea8076_f1s_prepare(struct drm_panel *panel)
{
	struct samsung_ea8076_f1s *ctx = to_samsung_ea8076_f1s(panel);
	int ret;

	ret = samsung_ea8076_f1s_power_on(ctx);
	if (ret < 0)
		return ret;

	samsung_ea8076_f1s_reset(ctx);

	return 0;
}

static int samsung_ea8076_f1s_enable(struct drm_panel *panel)
{
	struct samsung_ea8076_f1s *ctx = to_samsung_ea8076_f1s(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = samsung_ea8076_f1s_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		return ret;
	}

	/* [crux] 初始化已经做完，把录制连同 DCS 包打出来给交接用 */
	crux_rec_dump();

	return 0;
}

static int samsung_ea8076_f1s_unprepare(struct drm_panel *panel)
{
	struct samsung_ea8076_f1s *ctx = to_samsung_ea8076_f1s(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = samsung_ea8076_f1s_off(ctx);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);

	samsung_ea8076_f1s_power_off(ctx);

	return 0;
}

/*
 * [crux] 四档都放进模式表（原厂 dsi-panel-samsung-fhd-ea8076-f1s-cmd.dtsi
 * timing@0~@3 的 porch/clockrate）。默认仍然是 60Hz 那档（PREFERRED），
 * 其余三档要谁用谁就切，不影响现在的显示。
 * 60Hz: hfp64 hbp64 hpw20 / vbp64 vfp88 vpw20
 * 72/81/90Hz: hfp32 hbp16 hpw20 / vbp16 vfp16 vpw20
 */
#define CRUX_MODE(_fps, _hfp, _hbp, _hpw, _vbp, _vfp, _vpw, _flag)		\
	{								\
		.clock = (1080 + (_hfp) + (_hpw) + (_hbp)) *			\
			 (2340 + (_vfp) + (_vpw) + (_vbp)) * (_fps) / 1000,	\
		.hdisplay = 1080,					\
		.hsync_start = 1080 + (_hfp),				\
		.hsync_end = 1080 + (_hfp) + (_hpw),			\
		.htotal = 1080 + (_hfp) + (_hpw) + (_hbp),		\
		.vdisplay = 2340,					\
		.vsync_start = 2340 + (_vfp),				\
		.vsync_end = 2340 + (_vfp) + (_vpw),			\
		.vtotal = 2340 + (_vfp) + (_vpw) + (_vbp),		\
		.width_mm = 68,						\
		.height_mm = 147,					\
		.type = DRM_MODE_TYPE_DRIVER | (_flag),			\
	}

static const struct drm_display_mode samsung_ea8076_f1s_modes[] = {
	CRUX_MODE(60, 64, 64, 20, 64, 88, 20, 0),
	CRUX_MODE(72, 32, 16, 20, 16, 16, 20, 0),
	CRUX_MODE(81, 32, 16, 20, 16, 16, 20, 0),
	CRUX_MODE(90, 32, 16, 20, 16, 16, 20, 0),
};

static int samsung_ea8076_f1s_get_modes(struct drm_panel *panel,
					struct drm_connector *connector)
{
	int i, n = 0;

	struct samsung_ea8076_f1s *ctx = to_samsung_ea8076_f1s(panel);

	for (i = 0; i < ARRAY_SIZE(samsung_ea8076_f1s_modes); i++) {
		struct drm_display_mode *m =
			drm_mode_duplicate(connector->dev, &ctx->modes[i]);

		if (!m)
			continue;
		drm_mode_set_name(m);
		drm_mode_probed_add(connector, m);
		n++;
	}
	return n;
}

#define CRUX_RATE_DEFAULT	60

/*
 * [crux] 帧率管理：四档帧率的所有配置都出自同一张表
 * （porch/clockrate 在 samsung_ea8076_f1s_modes[]；R0c0 在引导器侧同一份表里）。
 * 默认档由设备树 samsung,default-framerate 决定，没有就用 60。
 * 引导器（GRUB cruxdisplay.mod）读同一份设备树，所以两边不会不一致。
 */
static void samsung_ea8076_f1s_rate_init(struct samsung_ea8076_f1s *ctx)
{
	unsigned int i, hit = 0;

	for (i = 0; i < ARRAY_SIZE(samsung_ea8076_f1s_modes); i++) {
		if (drm_mode_vrefresh(&ctx->modes[i]) == ctx->default_fps) {
			ctx->modes[i].type |= DRM_MODE_TYPE_PREFERRED;
			hit = 1;
		} else {
			ctx->modes[i].type &= ~DRM_MODE_TYPE_PREFERRED;
		}
	}

	if (!hit) {
		dev_warn(&ctx->dsi->dev,
			 "crux-rate: 设备树默认帧率 %u 不在表里，退回 %u\n",
			 ctx->default_fps, CRUX_RATE_DEFAULT);
		ctx->default_fps = CRUX_RATE_DEFAULT;
		samsung_ea8076_f1s_rate_init(ctx);
	}
}

/*
 * [crux] 亮度下发改为异步。回调可能被 DRM 的原子路径调用，在原子上下文里
 * 对 DSI 做同步传输会睡眠，会把整条显示路径卡住（表现就是一放开 powerdevil 就卡）。
 * 这里只记录请求并排队，实际下发在工作队列，由 bl_lock 与 on/off 串行化。
 */
static void samsung_ea8076_f1s_display_on_work(struct work_struct *work)
{
	struct samsung_ea8076_f1s *ctx = container_of(work,
		struct samsung_ea8076_f1s, display_on_work.work);
	int ret;

	mutex_lock(&ctx->bl_lock);
	if (!ctx->display_on_pending || !ctx->panel_ready) {
		mutex_unlock(&ctx->bl_lock);
		return;
	}
	ctx->display_on_pending = false;
	ret = mipi_dsi_dcs_set_display_on(ctx->dsi);
	mutex_unlock(&ctx->bl_lock);
	if (ret < 0)
		dev_err(&ctx->dsi->dev, "crux: display on failed: %d\n", ret);
	else
		dev_info(&ctx->dsi->dev, "crux: display on after first frame\n");
}

static void samsung_ea8076_f1s_bl_work(struct work_struct *work)
{
	struct samsung_ea8076_f1s *ctx = container_of(work, struct samsung_ea8076_f1s, bl_work);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	u16 level;

	mutex_lock(&ctx->bl_lock);
	level = ctx->bl_req;
	ctx->bl_req_valid = false;

	if (!ctx->panel_ready) {
		mutex_unlock(&ctx->bl_lock);
		return;
	}

	if (level == 0) {
		if (ctx->last_bl_level)
			mipi_dsi_dcs_write_seq_multi(&dsi_ctx,
				       MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x20);
	} else if (ctx->last_bl_level == 0) {
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx,
			     MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x28);
	}

	if (!dsi_ctx.accum_err) {
		int ret = mipi_dsi_dcs_set_display_brightness_large(ctx->dsi, level);

		if (ret < 0)
			dev_warn(&ctx->dsi->dev, "failed to set brightness %u\n", level);
		else
			ctx->last_bl_level = level;
	}

	mutex_unlock(&ctx->bl_lock);
}

static int samsung_ea8076_f1s_bl_update_status(struct backlight_device *bl)
{
	struct samsung_ea8076_f1s *ctx = bl_get_data(bl);

	if (!ctx->panel_ready)
		return 0;

	mutex_lock(&ctx->bl_lock);
	ctx->bl_req = bl->props.brightness;
	ctx->bl_req_valid = true;
	mutex_unlock(&ctx->bl_lock);

	schedule_work(&ctx->bl_work);

	return 0;
}

static const struct backlight_ops samsung_ea8076_f1s_bl_ops = {
	.update_status = samsung_ea8076_f1s_bl_update_status,
};

static const struct drm_panel_funcs samsung_ea8076_f1s_panel_funcs = {
	.prepare = samsung_ea8076_f1s_prepare,
	.enable = samsung_ea8076_f1s_enable,
	.unprepare = samsung_ea8076_f1s_unprepare,
	.get_modes = samsung_ea8076_f1s_get_modes,
};




/* [crux] 面板 DCS 回读调试口：判断面板到底有没有在听我们说话。
 * 写 dcs_cmd（十六进制）选命令，读 dcs_read 执行一次 DCS 读并打印 ret/val。
 * 面板有应答 => DSI 链路 + 命令都通；读超时(ret<0) => 面板没在听。
 */
static struct samsung_ea8076_f1s *crux_dcs_ctx;

/*
 * [crux] 交接：被 DSI 的 CMD_DMA_DONE 中断调用（数够 N 帧后）。
 * 这时 GRAM 里已经是一帧干净的内容，点亮就不会再出现那 0.几 秒花屏。
 */
/*
 * [crux] 早段内核日志重放。
 *
 * 背景：0.0-1.4s 这段时间里没有任何能送到面板的画布，那些 printk 只进了
 * tty0/efifb 的缓冲，屏上永远看不到。这里在面板点亮成功之后，把 ring buffer
 * 里的内容原样重打一遍（行内保留原始时间戳），屏上就是"从 0.000000 开始的
 * 完整记录"，之后再接着滚新的。
 *
 * 只在主动触发那一次动作（crux_armed），panic 等其它路径进来直接返回。
 */
void crux_panel_frames_ready(void)
{
	struct samsung_ea8076_f1s *ctx = crux_dcs_ctx;

	if (!ctx)
		return;
	if (!READ_ONCE(ctx->display_on_pending))
		return;
	schedule_delayed_work(&ctx->display_on_work, msecs_to_jiffies(1));
}
static struct dentry *crux_dcs_dbg;
static u8 crux_dcs_cmd = MIPI_DCS_GET_POWER_MODE;

static ssize_t crux_dcs_cmd_write(struct file *f, const char __user *u, size_t n, loff_t *o)
{
	char buf[16];
	unsigned int v;

	if (n == 0 || n >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, u, n))
		return -EFAULT;
	buf[n] = 0;
	if (kstrtouint(buf, 0, &v))
		return -EINVAL;
	crux_dcs_cmd = (u8)v;
	return n;
}

static ssize_t crux_dcs_read_show(struct file *f, char __user *u, size_t n, loff_t *o)
{
	struct samsung_ea8076_f1s *ctx = crux_dcs_ctx;
	u8 val = 0;
	ssize_t ret;
	char buf[128];
	int len;

	if (!ctx || !ctx->dsi)
		return -ENODEV;

	ret = mipi_dsi_dcs_read(ctx->dsi, crux_dcs_cmd, &val, 1);
	len = scnprintf(buf, sizeof(buf), "cmd=0x%02x ret=%zd val=0x%02x\n",
			crux_dcs_cmd, ret, val);
	return simple_read_from_buffer(u, n, o, buf, len);
}

static const struct file_operations crux_dcs_cmd_fops = { .write = crux_dcs_cmd_write };
static const struct file_operations crux_dcs_read_fops = { .read = crux_dcs_read_show };


/* [crux] 任意 DCS 写：写 "cmd [b1 b2 ...]" 十六进制，直接发给面板。
 * 用来现场试面板的写窗口/显示模式（0x2A/0x2B/0x12/0x13/0x30~0x33），看屏上条纹怎么变。
 */
static ssize_t crux_dcs_write_store(struct file *f, const char __user *u, size_t n, loff_t *o)
{
	struct samsung_ea8076_f1s *ctx = crux_dcs_ctx;
	char buf[192];
	u8 d[40];
	int cnt = 0, i;
	char *p;

	if (!ctx || !ctx->dsi)
		return -ENODEV;
	if (n == 0 || n >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, u, n))
		return -EFAULT;
	buf[n] = 0;

	p = buf;
	while (*p && cnt < 40) {
		unsigned int v;

		while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t' || *p == '\r')
			p++;
		if (!*p)
			break;
		if (sscanf(p, "%2x", &v) != 1)
			break;
		d[cnt++] = (u8)v;
		p += 2;
	}
	if (cnt < 1)
		return -EINVAL;

	mipi_dsi_dcs_write(ctx->dsi, d[0], d + 1, cnt - 1);
	pr_info("[crux] dcs_write cmd=%02x len=%d\n", d[0], cnt - 1);
	for (i = 0; i < cnt; i++)
		pr_info("[crux]   %02x", d[i]);
	pr_info("[crux] dcs_write done\n");
	return n;
}

static const struct file_operations crux_dcs_write_fops = { .write = crux_dcs_write_store };

/* [crux] 面板 GRAM 读回：写 "x,y" 选坐标，读回该点的像素（DCS 0x2A/0x2B 设窗 + 0x2E 读内存）。
 * 用来判断"面板里存的到底是不是我们送的那张图"——不看屏也能判定整条通路对不对。
 */
static u32 crux_gram_x;
static u32 crux_gram_y;

static ssize_t crux_gram_pos_write(struct file *f, const char __user *u, size_t n, loff_t *o)
{
	char buf[32];
	unsigned int x, y;

	if (n == 0 || n >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, u, n))
		return -EFAULT;
	buf[n] = 0;
	if (sscanf(buf, "%u,%u", &x, &y) != 2)
		return -EINVAL;
	crux_gram_x = x;
	crux_gram_y = y;
	return n;
}

static ssize_t crux_gram_read_show(struct file *f, char __user *u, size_t n, loff_t *o)
{
	struct samsung_ea8076_f1s *ctx = crux_dcs_ctx;
	u8 col[4], page[4], val[6] = {0};
	ssize_t r1, r2, r3;
	char buf[160];
	int len;

	if (!ctx || !ctx->dsi)
		return -ENODEV;

	col[0] = (crux_gram_x >> 8) & 0xff;
	col[1] = crux_gram_x & 0xff;
	col[2] = (crux_gram_x >> 8) & 0xff;
	col[3] = crux_gram_x & 0xff;
	page[0] = (crux_gram_y >> 8) & 0xff;
	page[1] = crux_gram_y & 0xff;
	page[2] = (crux_gram_y >> 8) & 0xff;
	page[3] = crux_gram_y & 0xff;

	r1 = mipi_dsi_dcs_write(ctx->dsi, MIPI_DCS_SET_COLUMN_ADDRESS, col, 4);
	r2 = mipi_dsi_dcs_write(ctx->dsi, MIPI_DCS_SET_PAGE_ADDRESS, page, 4);
	r3 = mipi_dsi_dcs_read(ctx->dsi, MIPI_DCS_READ_MEMORY_START, val, 3);

	/*
	 * 读完之后立刻把写窗口恢复成全屏（1080x2340）。
	 * 读 GRAM 时要先把 0x2A/0x2B 设成 1x1，如果不恢复，后面 DPU 送来的帧
	 * 就只会往那 1x1 窗口里写，屏上等于不再更新。
	 */
	col[0] = 0; col[1] = 0; col[2] = 0x04; col[3] = 0x37;   /* 0 ~ 1079 */
	page[0] = 0; page[1] = 0; page[2] = 0x09; page[3] = 0x23; /* 0 ~ 2339 */
	mipi_dsi_dcs_write(ctx->dsi, MIPI_DCS_SET_COLUMN_ADDRESS, col, 4);
	mipi_dsi_dcs_write(ctx->dsi, MIPI_DCS_SET_PAGE_ADDRESS, page, 4);

	len = scnprintf(buf, sizeof(buf),
			"gram(%u,%u) set_col=%zd set_page=%zd read=%zd data=%02x %02x %02x\n",
			crux_gram_x, crux_gram_y, r1, r2, r3, val[0], val[1], val[2]);
	return simple_read_from_buffer(u, n, o, buf, len);
}

static const struct file_operations crux_gram_pos_fops = { .write = crux_gram_pos_write };
static const struct file_operations crux_gram_read_fops = { .read = crux_gram_read_show };

static int samsung_ea8076_f1s_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct samsung_ea8076_f1s *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct samsung_ea8076_f1s, panel,
				   &samsung_ea8076_f1s_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	crux_dcs_ctx = ctx;
	crux_dcs_dbg = debugfs_create_dir("ea8076_f1s", NULL);
	debugfs_create_file("dcs_cmd", 0200, crux_dcs_dbg, NULL, &crux_dcs_cmd_fops);
	debugfs_create_file("dcs_read", 0444, crux_dcs_dbg, NULL, &crux_dcs_read_fops);
	debugfs_create_file("dcs_write", 0200, crux_dcs_dbg, NULL, &crux_dcs_write_fops);
	debugfs_create_file("gram_pos", 0200, crux_dcs_dbg, NULL, &crux_gram_pos_fops);
	debugfs_create_file("gram", 0444, crux_dcs_dbg, NULL, &crux_gram_read_fops);

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->vddio = devm_regulator_get_optional(dev, "vddio");
	if (IS_ERR(ctx->vddio))
		return dev_err_probe(dev, PTR_ERR(ctx->vddio),
				     "Failed to get vddio supply\n");

	ctx->vcie = devm_regulator_get_optional(dev, "vcie");
	if (IS_ERR(ctx->vcie))
		return dev_err_probe(dev, PTR_ERR(ctx->vcie),
				     "Failed to get vcie supply\n");

	ctx->vci = devm_regulator_get_optional(dev, "vci");
	if (IS_ERR(ctx->vci))
		return dev_err_probe(dev, PTR_ERR(ctx->vci),
				     "Failed to get vci supply\n");

	ret = devm_add_action_or_reset(dev, samsung_ea8076_f1s_power_off_action, ctx);
	if (ret)
		return ret;

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	/* [crux] 帧率管理：默认档由设备树 samsung,default-framerate 决定 */
	ctx->modes = devm_kmemdup(dev, samsung_ea8076_f1s_modes,
				  sizeof(samsung_ea8076_f1s_modes), GFP_KERNEL);
	if (!ctx->modes)
		return -ENOMEM;
	ctx->default_fps = CRUX_RATE_DEFAULT;
	of_property_read_u32(dev->of_node, "samsung,default-framerate",
			     &ctx->default_fps);
	samsung_ea8076_f1s_rate_init(ctx);
	dev_info(dev, "crux-rate: default %u Hz, %zu rates\n",
		 ctx->default_fps, ARRAY_SIZE(samsung_ea8076_f1s_modes));

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	/*
	 * crux: [命令模式] 面板本身就是 cmd mode，保持原厂口径：
	 * dsi_cmd_mode + burst_mode，te-pin-select=1、te-check-enable、te-using-te-pin，
	 * TE 走外部引脚。原厂在 dsi_ctrl_hw_cmn.c 里按 te_mode == DSI_TE_ON_EXT_PIN
	 * 把 DSI_TRIG_CTRL 的 BIT31 置起；主线等价物就是 dsi_host.c 里那句
	 * "Always assume dedicated TE pin"（DSI_TRIG_CTRL_TE = 0x80000000）。
	 * 曾试过强制 VIDEO|VIDEO_BURST 绕开 TE，已回退（面板不吃视频模式，会花屏）。
	 * 主线这套在 cmd mode 下走 DPU 的 tearcheck 路径（has_intf_te=1，
	 * core_major_ver>=5），实测持续报 "dpu error enc35 frame done timeout"，
	 * 帧完成等不到 TE，屏上表现为叠帧/重复错位。
	 * 帧同步改由我们自己的 crux-te-sync 模块负责（drivers/gpu/drm/panel/crux-te-sync.c）：
	 * 接管面板 TE 引脚、量出脉冲与频率，再拿它做下一帧的放行。	 */
	dsi->mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM;

	ctx->panel.prepare_prev_first = true;

	/*
	 * [crux] 交接：在 DSI 主机被重配之前，先把面板关掉
	 * （0x28 display off + 0x10 sleep in）。
	 *
	 * 交接那 0.几 秒里，DSI/DPU 被重配时会在面板 GRAM 里写进几帧错的内容，
	 * 面板当时还亮着，就把它显示出来了 —— 那就是花屏。面板先闭掉，
	 * 后面完整 init 时再醒（0x11），花屏就没有载体了。
	 */
	/*
	 * [crux] 交接：这里原来发 0x28(display off) + 0x10(sleep in) 把面板闭掉，
	 * 免得 DSI/DPU 重配时把错帧显示出来。现在 DSI 目标值已由 GRUB 侧预置，
	 * 重配时值不再变化，就没有错帧可显；面板保持亮着，画面从引导器那帧
	 * 直接换成内核控制台，中间不再黑一段。
	 */

	struct backlight_properties props = { };

	mutex_init(&ctx->bl_lock);
	INIT_WORK(&ctx->bl_work, samsung_ea8076_f1s_bl_work);
	INIT_DELAYED_WORK(&ctx->display_on_work, samsung_ea8076_f1s_display_on_work);

	props.type = BACKLIGHT_RAW;
	props.max_brightness = 2047;
	props.brightness = 100;  /* crux: 保持 5% (100/2047) */

	ctx->bl = devm_backlight_device_register(dev, "ea8076-f1s", dev, ctx,
						 &samsung_ea8076_f1s_bl_ops, &props);
	if (IS_ERR(ctx->bl))
		return dev_err_probe(dev, PTR_ERR(ctx->bl),
				     "Failed to register backlight\n");

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		cancel_delayed_work_sync(&ctx->display_on_work);
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
	}

	return 0;
}

static void samsung_ea8076_f1s_remove(struct mipi_dsi_device *dsi)
{
	struct samsung_ea8076_f1s *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id samsung_ea8076_f1s_of_match[] = {
	{ .compatible = "samsung,ea8076-f1s" }, // FIXME
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, samsung_ea8076_f1s_of_match);

static struct mipi_dsi_driver samsung_ea8076_f1s_driver = {
	.probe = samsung_ea8076_f1s_probe,
	.remove = samsung_ea8076_f1s_remove,
	.driver = {
		.name = "panel-samsung-ea8076-f1s",
		.of_match_table = samsung_ea8076_f1s_of_match,
	},
};
module_mipi_dsi_driver(samsung_ea8076_f1s_driver);

MODULE_AUTHOR("linux-mdss-dsi-panel-driver-generator <fix@me>"); // FIXME
MODULE_DESCRIPTION("DRM driver for samsung ea8076 fhd cmd dsi panel");
MODULE_LICENSE("GPL");

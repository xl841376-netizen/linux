// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2013, The Linux Foundation. All rights reserved.
 *
 * [crux 2026-09-24] 本文件不再自己往关机链注册处理器：关机流程由
 * drivers/power/reset/crux-shutdown.c（关机代理驱动）单点负责。
 * 这里只保留两件“机制”：
 *   1. 映射 ps_hold 寄存器；
 *   2. 向代理注册一个最后跑的步骤（priority -100），由代理在最后一下调用。
 * 重启一律走 PS_HOLD 掉电（整机硬复位），可用模块参数关掉：
 *   echo 0 > /sys/module/msm_poweroff/parameters/pshold_restart
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/pm.h>
#include <linux/crux_shutdown.h>

static void __iomem *msm_ps_hold;

static bool pshold_restart = true;
module_param(pshold_restart, bool, 0644);
MODULE_PARM_DESC(pshold_restart,
		 "restart via PS_HOLD power cut (default on; off falls back to PSCI)");

/*
 * [crux] 代理驱动的最后一步：写 0 到 ps_hold 寄存器放掉 PMIC 的 PS_HOLD，
 * PMIC 随即切掉所有供电轨（含外挂模组），整机真断电。写完就没打算回来：
 * 成功的话电已经没了；回来的话说明失败，进死循环等断电或人工介入。
 */
static int msm_pshold_step(enum crux_shutdown_action action, void *data)
{
	if (action == CRUX_SHUTDOWN_PREPARE || action == CRUX_SHUTDOWN_VERIFY) {
		crux_shutdown_report("msm-pshold", "ps_hold=%px %s",
				     (void *)msm_ps_hold,
				     action == CRUX_SHUTDOWN_VERIFY ?
				     "(write 0 is the cut)" : "ready");
		return 0;
	}

	/*
	 * 只有关机与重启才动 PS_HOLD。新增的动作（开机恢复等）绝不能走到下面：
	 * 这里写 0 就是把整机电源切掉，而且之后是死循环等待。
	 */
	if (action != CRUX_SHUTDOWN_POWEROFF && action != CRUX_SHUTDOWN_RESTART)
		return 0;

	if (action == CRUX_SHUTDOWN_RESTART && !pshold_restart)
		return -EOPNOTSUPP;

	/*
	 * [crux 2026-09-25] 这条路不经过 kernel_power_off()，所以 pm8941 那条
	 * “关机时把 PON 的 PS_HOLD 动作设成 SHUTDOWN”的 reboot notifier 不会跑：
	 * PMIC 就把这次拉低当成硬复位（实测 type=7），于是开机必须长按。
	 * 真关机时在这里自己把定向配好，拉低才是干净关机，开机回到短按。
	 * 重启不动它：那条路上 type7（硬复位）本来就是我们要的语义。
	 */
	if (action == CRUX_SHUTDOWN_POWEROFF) {
		crux_pon_force_type(true);
		crux_shutdown_report("msm-pshold",
				     "PON type forced to SHUTDOWN before cut");
	}

	pr_emerg("[crux] msm-pshold: pulling PS_HOLD low (%s)\n",
		 crux_shutdown_action_name(action));
	crux_shutdown_report("msm-pshold", "PS_HOLD pulled low (%s)",
			     crux_shutdown_action_name(action));

	writel(0, msm_ps_hold);

	for (;;)
		mdelay(1000);

	return 0;
}

/*
 * [crux 2026-09-25] 给代理的“最后期限”用：不管关机链走到哪一步，只要离断电
 * 太久，代理就在工作队列里直接调这里把 PS_HOLD 放掉，整机真断电。只是写一个
 * 寄存器，可以从进程上下文调用。
 */
void crux_pshold_cut(void)
{
	if (!msm_ps_hold)
		return;

	writel(0, msm_ps_hold);
}
EXPORT_SYMBOL_GPL(crux_pshold_cut);

static struct crux_shutdown_step msm_pshold_step_def = {
	.name = "msm-pshold",
	.priority = -100,	/* 最后跑 */
	.fn = msm_pshold_step,
};

static int msm_restart_probe(struct platform_device *pdev)
{
	int ret;

	msm_ps_hold = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(msm_ps_hold))
		return PTR_ERR(msm_ps_hold);

	ret = crux_shutdown_register_step(&msm_pshold_step_def);
	if (ret)
		dev_warn(&pdev->dev, "[crux] step not registered: %d\n", ret);

	return 0;
}

static const struct of_device_id of_msm_restart_match[] = {
	{ .compatible = "qcom,pshold", },
	{},
};
MODULE_DEVICE_TABLE(of, of_msm_restart_match);

static struct platform_driver msm_restart_driver = {
	.probe = msm_restart_probe,
	.driver = {
		.name = "msm-restart",
		.of_match_table = of_match_ptr(of_msm_restart_match),
	},
};
builtin_platform_driver(msm_restart_driver);

/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * [crux] 关机代理驱动对外接口（2026-09-25）
 */
#ifndef _LINUX_CRUX_SHUTDOWN_H
#define _LINUX_CRUX_SHUTDOWN_H

#include <linux/types.h>
#include <linux/list.h>

enum crux_shutdown_action {
	CRUX_SHUTDOWN_PREPARE = 0,
	CRUX_SHUTDOWN_VERIFY,
	CRUX_SHUTDOWN_POWEROFF,
	CRUX_SHUTDOWN_RESTART,
	/*
	 * 开机恢复：上一轮关机没干净收尾时（代理开机自检判脏），代理在开机后
	 * 统一调一遍。各驱动在这里把自己那一摊恢复到可用状态；动作要幂等，
	 * 不依赖用户态，不碰不可逆的东西。
	 */
	CRUX_SHUTDOWN_RECOVER,
};

/*
 * 契约：fn 必须自己判断 action，凡是自己不处理的动作一律直接 return 0。
 * 代理会对所有已注册的步骤挨个调用同一个动作，不区分谁注册了什么，
 * 所以“没判断动作”的步骤会被新动作意外触发（例如 msm-pshold 在没判断时
 * 会把 PS_HOLD 拉低，等于直接把机器断电）。
 */
struct crux_shutdown_step {
	const char *name;
	int priority;
	int (*fn)(enum crux_shutdown_action action, void *data);
	void *data;
	struct list_head list;
	bool registered;
};

int crux_shutdown_register_step(struct crux_shutdown_step *step);
void crux_shutdown_unregister_step(struct crux_shutdown_step *step);
void crux_shutdown_report(const char *name, const char *fmt, ...);
const char *crux_shutdown_action_name(enum crux_shutdown_action action);
int crux_shutdown_set_reason(const char *reason);

/*
 * 内核芯专用：device_shutdown 每处理完一个设备调一次，用来记录“谁关过、谁没有回调”。
 * 不需要任何驱动登记，新增驱动自动被记录。
 */
void crux_shutdown_device(const char *dev, const char *drv, bool has_cb, u64 usec);

/*
 * 关机裁决（0 = 干净，1 = 不干净）。代理算出裁决后调一次注册进来的接收器，
 * 由能写 PMIC 保留位的驱动把它存下来（扛得住真断电）。
 */
typedef void (*crux_shutdown_verdict_fn)(int dirty);
void crux_shutdown_set_verdict_sink(crux_shutdown_verdict_fn fn);

/*
 * [crux 2026-09-25] 关机“最后期限”用的两个出口：
 *   crux_pon_force_type() 由 pm8941-pwrkey.c 提供（把 PON 的 PS_HOLD 动作定向配好）；
 *   crux_pshold_cut()    由 msm-poweroff.c 提供（把 PS_HOLD 拉低，整机真断电）。
 * 代理在最后期限到点时按这个顺序调，不经过任何驱动的拆除流程。
 */
/*
 * [crux 2026-09-25] 触控等驱动的关机留痕：按位记，写 IMEM，下一轮开机可读出。
 *   BUS   位：总线 .shutdown 回调跑的（device_shutdown 那条路）
 *   AGENT 位：我们的关机代理主动触发的（每次有序关机都会跑）
 */
#define CRUX_PARK_FTS_BUS	BIT(0)
#define CRUX_PARK_FTS_AGENT	BIT(1)
void crux_shutdown_mark_fts_parked(unsigned int bits);

void crux_pon_force_type(bool shutdown);
void crux_pshold_cut(void);

/*
 * [crux 2026-09-25] X50 链路状态（只读 RC 本地寄存器，不产生端点 MMIO）。
 * 任何要访问外挂模组端点的地方，动手前先问一句；返回 false 就别碰。
 */
bool crux_x50_link_up(void);

#endif /* _LINUX_CRUX_SHUTDOWN_H */

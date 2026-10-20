// SPDX-License-Identifier: GPL-2.0-only
/*
 * crux_esoc.c —— crux（小米 9 Pro 5G / SM8150）外挂 X50（SDX50M）的最小 esoc 等效
 *
 * 目的：把原厂 esoc 框架里我们真正需要的那一层语义，用我们自己的最小实现补上：
 *   1) 单一入口 /dev/crux-esoc：谁要开基带、什么时候开，都得走它（而不是各自去动 GPIO）；
 *   2) 上电/复位/断电的时序由内核实现（照原厂 esoc-mdm-4x 的 sdx50m 时序；
 *      复位脚 = qcom,ap2mdm-soft-reset-gpio，低=复位、高=放开）；
 *   3) **动电源之前先把 MHI 从 PCI 上摘下来**（原厂靠 mhi_arch_qcom 注册 esoc client hook
 *      做到这件事；这里等价实现为 pci_stop_and_remove_bus_device(17cb:0305)），
 *      上电后 rescan 让 mhi_pci_generic 重新 probe —— 这是“复位不崩机”的关键；
 *   4) WAIT_FOR_REQ：把“基带已经落到 SBL、等镜像”这件事作为事件交给用户态，
 *      用户态阻塞等事件，而不是自己轮询 /dev/mhi_pipe_*。
 *
 * ABI 与厂商 include/uapi/linux/esoc_ctrl.h 对齐（ioctl 号、enum esoc_cmd / esoc_req 取值一致），
 * 所以原来的 mdm_helper 语义可以直接照搬。
 *
 * 注：这是“我们自己的最小等效”，不是移植；只实现我们这条线需要的命令，
 *     调试类命令（PREPARE_DEBUG/EXE_DEBUG/EXIT_DEBUG）返回 -EOPNOTSUPP。
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/workqueue.h>
#include <linux/completion.h>
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/pinctrl/consumer.h>
#include <linux/jiffies.h>
#include <linux/ioctl.h>
#include <linux/crux_dep.h>

#define DEV_NAME		"crux-esoc"

/* ---- 与厂商 uapi 对齐的 ABI ---- */
#define CRUX_ESOC_CODE		0xCC
#define CRUX_ESOC_CMD_EXE	_IOW(CRUX_ESOC_CODE, 1, unsigned int)
#define CRUX_ESOC_WAIT_FOR_REQ	_IOR(CRUX_ESOC_CODE, 2, unsigned int)
#define CRUX_ESOC_GET_STATUS	_IOR(CRUX_ESOC_CODE, 4, unsigned int)
#define CRUX_ESOC_GET_ERR_FATAL	_IOR(CRUX_ESOC_CODE, 5, unsigned int)
#define CRUX_ESOC_NOTIFY	_IOW(CRUX_ESOC_CODE, 3, unsigned int)

enum crux_esoc_cmd {
	CRUX_ESOC_PWR_ON = 1,
	CRUX_ESOC_PWR_OFF,
	CRUX_ESOC_FORCE_PWR_OFF,
	CRUX_ESOC_RESET,
	CRUX_ESOC_PREPARE_DEBUG,
	CRUX_ESOC_EXE_DEBUG,
	CRUX_ESOC_EXIT_DEBUG,
};

enum crux_esoc_req {
	CRUX_ESOC_REQ_IMG = 1,	/* 基带在 SBL，等 host 喂镜像（= 原厂 ESOC_REQ_IMG） */
	CRUX_ESOC_REQ_DEBUG,
	CRUX_ESOC_REQ_SHUTDOWN,
};

/* 与厂商 enum esoc_notify 取值对齐 */
enum crux_esoc_notify {
	CRUX_ESOC_IMG_XFER_DONE = 1,
	CRUX_ESOC_BOOT_DONE,
	CRUX_ESOC_BOOT_FAIL,
	CRUX_ESOC_IMG_XFER_RETRY,
	CRUX_ESOC_IMG_XFER_FAIL,
	CRUX_ESOC_UPGRADE_AVAILABLE,
	CRUX_ESOC_DEBUG_DONE,
	CRUX_ESOC_DEBUG_FAIL,
	CRUX_ESOC_PRIMARY_CRASH,
	CRUX_ESOC_PRIMARY_REBOOT,
	CRUX_ESOC_PON_RETRY,
};


/* X50 的 PCI ID（端点 / 根端口） */
#define X50_PCI_VENDOR		0x17cb
#define X50_PCI_DEV_ENDPOINT	0x0305
#define X50_PCI_DEV_ROOTPORT	0x0108

/* 时序（照原厂 sdx50m 那一套；单位 ms） */
#define T_RESET_WARM		150	/* ESOC_RESET / PWR_ON 里的脉冲 */
#define T_RESET_COLD		600	/* cold reset */
#define T_PON_SETTLE		150	/* 放开复位后等基带稳定 */
#define T_POFF_HOLD		300	/* 断电时按住不放之后 */
#define T_ERRFATAL_SETTLE	50

enum {
	GP_RESET = 0,		/* qcom,ap2mdm-soft-reset-gpio（低=复位） */
	GP_AP_STATUS,		/* qcom,ap2mdm-status-gpio */
	GP_AP_ERRFATAL,		/* qcom,ap2mdm-errfatal-gpio */
	GP_MDM_STATUS,		/* qcom,mdm2ap-status-gpio（输入，看基带状态） */
	GP_MDM_ERRFATAL,	/* qcom,mdm2ap-errfatal-gpio（输入） */
	GP_MAX,
};

static const char * const gp_con_ids[GP_MAX] = {
	[GP_RESET]	= "qcom,ap2mdm-soft-reset",
	[GP_AP_STATUS]	= "qcom,ap2mdm-status",
	[GP_AP_ERRFATAL]= "qcom,ap2mdm-errfatal",
	[GP_MDM_STATUS]	= "qcom,mdm2ap-status",
	[GP_MDM_ERRFATAL] = "qcom,mdm2ap-errfatal",
};

/* GET_STATUS 返回的位定义 */
#define ST_RESET_ASSERTED	BIT(0)	/* 复位线当前是低（复位中） */
#define ST_AP_STATUS_HIGH	BIT(1)
#define ST_MDM_STATUS_HIGH	BIT(2)
#define ST_AP_ERRFATAL_HIGH	BIT(3)
#define ST_MDM_ERRFATAL_HIGH	BIT(4)
#define ST_HOST_POWERED		BIT(5)	/* 本模块记录的“已上电” */

struct crux_esoc {
	struct device *dev;
	struct gpio_desc *gpio[GP_MAX];
	struct miscdevice mdev;
	wait_queue_head_t wq;
	u32 req;			/* 待处理的请求（0 = 无） */
	u32 boot_fail_act;
	u32 n_pon_tries;
	bool powered;
	struct mutex lock;		/* 串行化电源操作 */
	struct work_struct cmd_work;	/* 电源类命令异步执行（防 D 状态） */
	struct completion cmd_done;
	unsigned int cmd_pending;
	int cmd_ret;
	atomic_t cmd_busy;
	/* 原厂 esoc-mdm 那套：STATUS 双沿监控 + 异常复位恢复 */
	int status_irq;
	bool mdm_ready;
	bool img_xfer_done;	/* 已收到 IMG_XFER_DONE */
	bool boot_done;		/* 已收到 BOOT_DONE */
	bool pcie_resumed;	/* [crux] 已经做过一次 D3hot->D0+重训 */
	struct work_struct resume_work;
	struct work_struct pon_work;
};

/* ---- GPIO 小工具 ---- */

static void x50_reset(struct crux_esoc *e, bool assert)
{
	/* 低 = 复位（assert），高 = 放开（deassert） */
	gpiod_direction_output(e->gpio[GP_RESET], assert ? 0 : 1);
}

static unsigned int x50_read_status(struct crux_esoc *e)
{
	unsigned int st = 0;

	if (!gpiod_get_value(e->gpio[GP_RESET]))
		st |= ST_RESET_ASSERTED;
	if (gpiod_get_value(e->gpio[GP_AP_STATUS]))
		st |= ST_AP_STATUS_HIGH;
	if (gpiod_get_value(e->gpio[GP_MDM_STATUS]))
		st |= ST_MDM_STATUS_HIGH;
	if (gpiod_get_value(e->gpio[GP_AP_ERRFATAL]))
		st |= ST_AP_ERRFATAL_HIGH;
	if (gpiod_get_value(e->gpio[GP_MDM_ERRFATAL]))
		st |= ST_MDM_ERRFATAL_HIGH;
	if (e->powered)
		st |= ST_HOST_POWERED;

	return st;
}

/* ---- 原厂 “esoc client hook” 的等价物：动电源前先把 MHI 摘掉 ---- */

/* mhi_pci_generic 导出的“停链路”入口：运行时 symbol_get，避免硬依赖（模块可以不在）。
 * 掉电/复位前先停 MHI，是把链路消失变成“受控下线”的关键（否则 AER 风暴/恢复超时）。
 */
extern void mhi_pci_link_shutdown(struct pci_dev *pdev, bool keep_dev);

static void x50_pci_down(struct crux_esoc *e)
{
	struct pci_dev *pdev;

	pci_lock_rescan_remove();
	pdev = pci_get_device(X50_PCI_VENDOR, X50_PCI_DEV_ENDPOINT, NULL);
	if (pdev) {
		/* 原厂靠 mhi_arch_qcom 的 esoc client hook 在掉电前停 MHI；
		 * 这里直接调 mhi_pci_generic 导出的等价入口，避免链路中途消失
		 * 造成的 AER 风暴 / 恢复超时（那种情况下 pci 摘端点会挂住）。
		 */
		void (*link_down)(struct pci_dev *, bool) =
			(void (*)(struct pci_dev *, bool))
			symbol_get(mhi_pci_link_shutdown);

		if (link_down) {
			dev_info(e->dev, "%s: stopping MHI link on %s before power op\n",
				 DEV_NAME, pci_name(pdev));
			link_down(pdev, false);
			symbol_put(mhi_pci_link_shutdown);
		} else {
			dev_warn(e->dev, "%s: mhi_pci_link_shutdown not available\n",
				 DEV_NAME);
		}

		dev_info(e->dev, "%s: removing X50 endpoint %s before power op\n",
			 DEV_NAME, pci_name(pdev));
		pci_stop_and_remove_bus_device(pdev);
		pci_dev_put(pdev);
	} else {
		dev_info(e->dev, "%s: no X50 endpoint on the bus\n", DEV_NAME);
	}
	pci_unlock_rescan_remove();
}

static void x50_pci_up(struct crux_esoc *e)
{
	struct pci_dev *rp;
	struct pci_bus *bus;

	pci_lock_rescan_remove();
	rp = pci_get_device(X50_PCI_VENDOR, X50_PCI_DEV_ROOTPORT, NULL);
	bus = rp ? rp->bus : NULL;
	if (bus)
		pci_rescan_bus(bus);
	pci_unlock_rescan_remove();
	if (rp)
		pci_dev_put(rp);

	dev_info(e->dev, "%s: pci rescan %s\n", DEV_NAME,
		 bus ? "done (endpoint should re-enumerate)" : "skipped (no root port)");
}

/* ---- 事件：告诉等待者“现在该喂镜像了” ---- */

static void x50_queue_req(struct crux_esoc *e, u32 req)
{
	e->req = req;
	wake_up_interruptible_all(&e->wq);
	dev_info(e->dev, "%s: request queued: %u\n", DEV_NAME, req);
}

/* ---- 电源动作 ---- */

static bool x50_wait_status(struct crux_esoc *e, bool high, unsigned int timeout_ms);
static void x50_resume_work(struct work_struct *w);

static int x50_power_on(struct crux_esoc *e)
{
	dev_info(e->dev, "%s: PWR_ON\n", DEV_NAME);

	/* [crux 2026-10-19] 用户定的规矩：
	 *   首次启动（开机/冷复位上电）执行一次链路训练；
	 *   后续只有再次冷复位时才再执行一次。
	 * pcie_resumed 本身挡住同一次上电里的重复 notify，
	 * 这里在每次 PWR_ON（冷复位/上电）时清零，允许新一轮做一次。
	 */
	e->pcie_resumed = false;

	/* 1) 先让 MHI/PCIe 下线（原厂语义：复位前先停 MHI） */
	x50_pci_down(e);

	/* 2) 原厂 sdx50m PON：ERRFATAL=0 → 复位脉冲 → STATUS=1 */
	gpiod_direction_output(e->gpio[GP_AP_ERRFATAL], 0);
	msleep(T_ERRFATAL_SETTLE);

	x50_reset(e, true);
	msleep(T_RESET_WARM);
	x50_reset(e, false);
	msleep(T_PON_SETTLE);

	gpiod_direction_output(e->gpio[GP_AP_STATUS], 1);

	/*
	 * 原厂 mdm_subsys_powerup：PWR_ON 之后等 MDM2AP_STATUS 拉高（PON_SUCCESS）
	 * 才算启动完成，超时值 MDM2AP_STATUS_TIMEOUT_MS = 120000ms。
	 * 这里不阻塞 ioctl（用户态 15s 就会放弃），改为后台等待并打状态机日志。
	 * 超时只报 PON_FAIL，**不做冷复位重试**（重试会打断模组自己的启动流程）。
	 */
	schedule_work(&e->pon_work);
	e->powered = true;
	e->mdm_ready = !!gpiod_get_value(e->gpio[GP_MDM_STATUS]);
	/* 依赖契约：告诉 MHI/IPA 侧“模组已上电，可以开始枚举” */
	crux_dep_set("x50_power", CRUX_DEP_UP);

	/* 3) 重新枚举端点（mhi_pci_generic 会自动 probe） */
	x50_pci_up(e);

	/* 4) 告诉用户态：基带在 SBL，等镜像 */
	x50_queue_req(e, CRUX_ESOC_REQ_IMG);
	return 0;
}

/*
 * [crux] 原厂 mhi_arch_esoc_ops_power_on() 的等价动作：
 *   模组镜像灌完、进入任务模式时，主机要对 PCIe 做一次“强制挂起再恢复”
 *   （D3hot -> D0 + 链路重训）；否则模组从低功耗态醒不干净，MHI 通道
 *   上下文一直停在 pending，QMI/EFS 都不会应答。
 */
/* [crux 2026-10-19] 重训前置条件：链路必须在 L0
 * （DL Active=1 且 Link Training=0）。
 * 记忆台账（2026-09-21 只重训版 / 09-26 D0-D3 归属）已写明：
 *   - 链路在时才 retrain；链路不在时不重训；
 *   - D3hot 之后链路离开 L0，重训前置条件会自锁；
 *   - 链路还在跑就动它 → AER 风暴 / Recovery failed / 整机重启。
 */
/* [crux 2026-10-19] 取根端口：链路状态与重训都应在 RC 上做
 * （记忆 2026-09-21：retrain link via 0001:00:00.0）。
 * X50 端点自己的 LnkSta.bit13(DLLLA) 实测恒为 0（1022/1023），不可信。
 */
static struct pci_dev *x50_root_port(struct pci_dev *pdev)
{
	struct pci_dev *rc = pdev->bus ? pdev->bus->self : NULL;

	return rc ? rc : pdev;
}

static bool x50_link_is_l0(struct pci_dev *pdev)
{
	struct pci_dev *rc = x50_root_port(pdev);
	u16 lnksta = 0;
	int i;

	for (i = 0; i < 20; i++) {
		pcie_capability_read_word(rc, PCI_EXP_LNKSTA, &lnksta);
		if ((lnksta & PCI_EXP_LNKSTA_DLLLA) &&
		    !(lnksta & PCI_EXP_LNKSTA_LT))
			return true;
		msleep(25);
	}
	dev_info(&rc->dev, "[crux] link not L0 after 500ms (lnksta=0x%04x)\n",
		 lnksta);
	return false;
}

static void x50_pcie_boot_resume(struct crux_esoc *e, const char *why)
{
	struct pci_dev *pdev, *rc;
	u16 lnkctl = 0, lnksta = 0, aspm_old = 0;
	int i;

	pdev = pci_get_device(X50_PCI_VENDOR, X50_PCI_DEV_ENDPOINT, NULL);
	if (!pdev) {
		dev_warn(e->dev, "%s: %s: no X50 endpoint on the bus\n",
			 DEV_NAME, why);
		return;
	}

	pm_runtime_get_sync(&pdev->dev);

	/* [crux 2026-10-19] 只做一次 D3hot->D0+重训；且链路不在 L0 时不动它 */
	if (e->pcie_resumed) {
		dev_info(e->dev, "%s: %s: already resumed once, skip\n",
			 DEV_NAME, why);
		goto out;
	}

	if (pdev->current_state != PCI_D0) {
		dev_info(e->dev, "%s: %s: endpoint in D%u, forcing D0\n",
			 DEV_NAME, why, pdev->current_state);
		pci_set_power_state(pdev, PCI_D0);
		msleep(50);
	}

	/* [crux 2026-10-19] 只有链路在 L0 才做 retrain，否则跳过（避免 AER/整机重启） */
	if (!x50_link_is_l0(pdev)) {
		dev_warn(e->dev, "%s: %s: link not in L0, skip retrain\n",
			 DEV_NAME, why);
		goto out;
	}

	/* [crux 2026-10-19] 按记忆 2026-09-23 参考实现（msm_pcie_link_retrain）：
	 * 切速率/重训前先拦 L1(ASPM)，在根端口踩 Retrain Link，
	 * 100ms 有界轮询，回读校验，最后放开 ASPM。
	 */
	rc = x50_root_port(pdev);
	pcie_capability_read_word(rc, PCI_EXP_LNKCTL, &lnkctl);
	aspm_old = lnkctl & PCI_EXP_LNKCTL_ASPMC;
	pcie_capability_write_word(rc, PCI_EXP_LNKCTL, lnkctl & ~PCI_EXP_LNKCTL_ASPMC);
	usleep_range(1000, 2000);

	pcie_capability_write_word(rc, PCI_EXP_LNKCTL, (lnkctl & ~PCI_EXP_LNKCTL_ASPMC) |
				   PCI_EXP_LNKCTL_RL);

	for (i = 0; i < 100; i++) {
		pcie_capability_read_word(rc, PCI_EXP_LNKSTA, &lnksta);
		if (!(lnksta & PCI_EXP_LNKSTA_LT) &&
		    (lnksta & PCI_EXP_LNKSTA_DLLLA))
			break;
		usleep_range(900, 1100);
	}

	pcie_capability_write_word(rc, PCI_EXP_LNKCTL,
				   (lnkctl & ~PCI_EXP_LNKCTL_ASPMC) | aspm_old);

	dev_info(e->dev,
		 "%s: %s: retrain done i=%d lnksta(RC)=0x%04x speed=%u width=%u\n",
		 DEV_NAME, why, i, lnksta,
		 lnksta & PCI_EXP_LNKSTA_CLS,
		 (lnksta & PCI_EXP_LNKSTA_NLW) >> 4);
	pcie_capability_read_word(pdev, PCI_EXP_LNKSTA, &lnksta);
	dev_info(e->dev, "%s: %s: endpoint lnksta=0x%04x\n", DEV_NAME, why, lnksta);
	e->pcie_resumed = true;

out:
	pm_runtime_put(&pdev->dev);
	pci_dev_put(pdev);
}

static int x50_do_notify(struct crux_esoc *e, unsigned int note)
{
	switch (note) {
	case CRUX_ESOC_IMG_XFER_DONE:
		dev_info(e->dev, "%s: notify IMG_XFER_DONE (firmware fed)\n", DEV_NAME);
		e->img_xfer_done = true;
		/* [crux 2026-10-19] 原厂只在 BOOT_DONE(进任务模式) 做一次
		 * D3hot->D0+重训。这里原来也做，等于 3 秒内重训两次，
		 * 第二次前置条件已破坏 → AER → 整机重启。
		 */
		return 0;
	case CRUX_ESOC_BOOT_DONE:
		dev_info(e->dev, "%s: notify BOOT_DONE (mission mode)\n", DEV_NAME);
		e->boot_done = true;
		x50_pcie_boot_resume(e, "boot-done");
		return 0;
	case CRUX_ESOC_BOOT_FAIL:
		dev_err(e->dev, "%s: notify BOOT_FAIL\n", DEV_NAME);
		return 0;
	case CRUX_ESOC_PRIMARY_CRASH:
		dev_err(e->dev, "%s: notify PRIMARY_CRASH\n", DEV_NAME);
		return 0;
	case CRUX_ESOC_PRIMARY_REBOOT:
		dev_info(e->dev, "%s: notify PRIMARY_REBOOT\n", DEV_NAME);
		return 0;
	default:
		dev_info(e->dev, "%s: notify %u (ignored)\n", DEV_NAME, note);
		return 0;
	}
}

/* 原厂 mdm_wait_for_status_low 的等价物 */
static bool x50_wait_status(struct crux_esoc *e, bool high, unsigned int timeout_ms)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);

	do {
		if (!!gpiod_get_value(e->gpio[GP_MDM_STATUS]) == high)
			return true;
		msleep(20);
	} while (time_before(jiffies, deadline));

	return false;
}

/*
 * 原厂 mdm_ssr_fn 的等价物：
 *   异常复位 -> 先让 MHI 受控下线 -> 等 STATUS 低 -> 重新上电。
 * 关键：绝不能在 MHI 还挂在端点上时直接动 GPIO（那是整机被拖死的路径）。
 */
static void x50_resume_work(struct work_struct *w)
{
	struct crux_esoc *e = container_of(w, struct crux_esoc, resume_work);

	mutex_lock(&e->lock);
	if (!e->powered || gpiod_get_value(e->gpio[GP_MDM_STATUS])) {
		mutex_unlock(&e->lock);
		return;
	}

	dev_err(e->dev, "%s: modem dropped STATUS low, recovering (MHI off first)\n",
		DEV_NAME);
	crux_dep_set("x50_power", CRUX_DEP_DOWN);
	x50_pci_down(e);
	e->powered = false;
	e->mdm_ready = false;

	if (!x50_wait_status(e, false, 10000))
		dev_err(e->dev, "%s: STATUS still low after 10s\n", DEV_NAME);

	x50_power_on(e);
	mutex_unlock(&e->lock);
}

static irqreturn_t x50_status_irq(int irq, void *data)
{
	struct crux_esoc *e = data;
	bool high = !!gpiod_get_value(e->gpio[GP_MDM_STATUS]);

	if (!high && e->powered) {
		dev_err(e->dev, "%s: unexpected reset (STATUS fell while powered)\n",
			DEV_NAME);
		schedule_work(&e->resume_work);
	} else if (high) {
		dev_info(e->dev, "%s: STATUS high (modem boot done)\n", DEV_NAME);
	}

	return IRQ_HANDLED;
}

/* 原厂 pon_done 等待：后台跑，120s 超时，只打日志不动电源 */
static void x50_pon_wait_work(struct work_struct *w)
{
	struct crux_esoc *e = container_of(w, struct crux_esoc, pon_work);

	if (x50_wait_status(e, true, 120000)) {
		e->mdm_ready = true;
		dev_info(e->dev, "%s: PON_SUCCESS (STATUS high, %s)\n",
			 DEV_NAME, e->powered ? "powered" : "not powered");
	} else {
		e->mdm_ready = false;
		dev_err(e->dev, "%s: PON_FAIL (STATUS not high in 120s)\n",
			DEV_NAME);
	}
}

static int x50_reset_do(struct crux_esoc *e, unsigned int ms)
{
	dev_info(e->dev, "%s: RESET (%ums low pulse)\n", DEV_NAME, ms);

	/* [crux 2026-10-19] 冷复位允许再做一次链路训练 */
	e->pcie_resumed = false;

	crux_dep_set("x50_power", CRUX_DEP_DOWN);
	x50_pci_down(e);

	x50_reset(e, true);
	msleep(ms);
	x50_reset(e, false);
	msleep(T_PON_SETTLE);

	gpiod_direction_output(e->gpio[GP_AP_STATUS], 1);
	e->powered = true;

	x50_pci_up(e);

	return 0;
}

static int x50_power_off(struct crux_esoc *e, bool force)
{
	dev_info(e->dev, "%s: %s\n", DEV_NAME,
		 force ? "FORCE_PWR_OFF" : "PWR_OFF");

	/*
	 * FORCE 语义 = 原厂 "Pull the reset line low to turn off the device"：
	 *   **只切电，不动 MHI/PCIe**。
	 * 用在 AP 正在关机/重启的场景：AP 马上就要断电，这里既没必要、也不能去做
	 * mhi_power_down() + pci_stop_and_remove_bus_device() 这类可能长时间阻塞的收尾
	 * （它们会 flush_work 等状态机走完，设备不响应时就是永久等待），
	 * 否则整个关机流程会被卡死（实测踩过：钩子调到旧驱动的 pwr_off 路径，机器起不来）。
	 * 运行时的 RESET / PWR_ON 仍走 PWR_OFF 那条带 x50_pci_down() 的完整路径，
	 * 那条路径上 AP 还活着，必须先让 MHI 受控下线再摘端点。
	 */
	if (!force)
		x50_pci_down(e);

	gpiod_direction_output(e->gpio[GP_AP_ERRFATAL], force ? 1 : 0);
	gpiod_direction_output(e->gpio[GP_AP_STATUS], 0);
	/* 按住复位不放（原厂 power_down：摁住 + 300ms） */
	x50_reset(e, true);
	msleep(T_POFF_HOLD);

	e->powered = false;
	crux_dep_set("x50_power", CRUX_DEP_DOWN);
	return 0;
}

static int x50_do_cmd(struct crux_esoc *e, unsigned int cmd);

static void x50_cmd_worker(struct work_struct *work)
{
	struct crux_esoc *e = container_of(work, struct crux_esoc, cmd_work);

	e->cmd_ret = x50_do_cmd(e, e->cmd_pending);
	atomic_set(&e->cmd_busy, 0);
	complete(&e->cmd_done);
}

/*
 * 电源类命令走工作队列 + 有界等待：端点处于坏状态时 pci_stop_and_remove_bus_device()
 * 可能长时间阻塞，直接放在 ioctl 里会把用户态进程卡成 D 状态（kill -9 都杀不掉）。
 * 这里最多等 15s，超时就把错误交回用户态，内核线程自己继续收尾。
 */
static int x50_cmd_exe(struct crux_esoc *e, unsigned int cmd)
{
	bool async = (cmd == CRUX_ESOC_PWR_ON || cmd == CRUX_ESOC_RESET ||
		      cmd == CRUX_ESOC_PWR_OFF || cmd == CRUX_ESOC_FORCE_PWR_OFF);

	if (!async)
		return x50_do_cmd(e, cmd);

	if (atomic_xchg(&e->cmd_busy, 1)) {
		dev_warn(e->dev, "%s: previous command is still running\n", DEV_NAME);
		return -EBUSY;
	}

	e->cmd_pending = cmd;
	reinit_completion(&e->cmd_done);
	schedule_work(&e->cmd_work);

	if (!wait_for_completion_timeout(&e->cmd_done, msecs_to_jiffies(15000))) {
		dev_warn(e->dev,
			 "%s: command %u timed out (kernel op still blocked); userspace released\n",
			 DEV_NAME, cmd);
		return -ETIMEDOUT;
	}

	return e->cmd_ret;
}

static int x50_do_cmd(struct crux_esoc *e, unsigned int cmd)
{
	int ret;

	mutex_lock(&e->lock);
	switch (cmd) {
	case CRUX_ESOC_PWR_ON:
		ret = x50_power_on(e);
		break;
	case CRUX_ESOC_RESET:
		ret = x50_reset_do(e, T_RESET_WARM);
		if (!ret)
			x50_queue_req(e, CRUX_ESOC_REQ_IMG);
		break;
	case CRUX_ESOC_PWR_OFF:
	case CRUX_ESOC_FORCE_PWR_OFF:
		ret = x50_power_off(e, cmd == CRUX_ESOC_FORCE_PWR_OFF);
		break;
	case CRUX_ESOC_PREPARE_DEBUG:
	case CRUX_ESOC_EXE_DEBUG:
	case CRUX_ESOC_EXIT_DEBUG:
		dev_info(e->dev, "%s: debug command %u not implemented\n",
			 DEV_NAME, cmd);
		ret = -EOPNOTSUPP;
		break;
	default:
		dev_warn(e->dev, "%s: unknown command %u\n", DEV_NAME, cmd);
		ret = -EINVAL;
		break;
	}
	mutex_unlock(&e->lock);

	return ret;
}

static int x50_wait_for_req(struct crux_esoc *e, unsigned int __user *argp)
{
	unsigned int timeout = 0;
	unsigned int evt;
	long rc;

	if (argp && get_user(timeout, argp))
		return -EFAULT;

	if (!e->req) {
		rc = wait_event_interruptible_timeout(e->wq, e->req != 0,
						      timeout ? msecs_to_jiffies(timeout)
							      : MAX_SCHEDULE_TIMEOUT);
		if (rc == 0)
			return -ETIMEDOUT;
		if (rc < 0)
			return (int)rc;
	}

	evt = e->req;
	e->req = 0;
	if (argp && put_user(evt, argp))
		return -EFAULT;

	dev_info(e->dev, "%s: request %u delivered\n", DEV_NAME, evt);
	return 0;
}

/* ---- 字符设备 ---- */

static int crux_esoc_open(struct inode *inode, struct file *file)
{
	struct miscdevice *mdev = file->private_data;
	struct crux_esoc *e = container_of(mdev, struct crux_esoc, mdev);

	file->private_data = e;
	return 0;
}

static long crux_esoc_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	struct crux_esoc *e = file->private_data;
	unsigned int val;
	int ret;

	switch (cmd) {
	case CRUX_ESOC_CMD_EXE:
		if (get_user(val, (unsigned int __user *)arg))
			return -EFAULT;
		return x50_cmd_exe(e, val);

	case CRUX_ESOC_WAIT_FOR_REQ:
		return x50_wait_for_req(e, (unsigned int __user *)arg);

	case CRUX_ESOC_NOTIFY:
		if (get_user(val, (unsigned int __user *)arg))
			return -EFAULT;
		return x50_do_notify(e, val);

	case CRUX_ESOC_GET_STATUS:
		val = x50_read_status(e);
		if (put_user(val, (unsigned int __user *)arg))
			return -EFAULT;
		return 0;

	case CRUX_ESOC_GET_ERR_FATAL:
		val = !!gpiod_get_value(e->gpio[GP_MDM_ERRFATAL]);
		if (put_user(val, (unsigned int __user *)arg))
			return -EFAULT;
		return 0;

	default:
		dev_warn(e->dev, "%s: unknown ioctl 0x%x\n", DEV_NAME, cmd);
		ret = -ENOTTY;
		break;
	}

	return ret;
}

static const struct file_operations crux_esoc_fops = {
	.owner		= THIS_MODULE,
	.open		= crux_esoc_open,
	.unlocked_ioctl	= crux_esoc_ioctl,
	.llseek		= noop_llseek,
};

/* ---- probe / remove ---- */

static int crux_esoc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct crux_esoc *e;
	int i, ret;

	e = devm_kzalloc(dev, sizeof(*e), GFP_KERNEL);
	if (!e)
		return -ENOMEM;

	e->dev = dev;
	mutex_init(&e->lock);
	init_waitqueue_head(&e->wq);
	INIT_WORK(&e->cmd_work, x50_cmd_worker);
	init_completion(&e->cmd_done);
	atomic_set(&e->cmd_busy, 0);
	INIT_WORK(&e->resume_work, x50_resume_work);
	INIT_WORK(&e->pon_work, x50_pon_wait_work);

	/* DT 里的 pinctrl-names = default/mdm_active/mdm_suspend */
	if (IS_ERR(devm_pinctrl_get_select_default(dev)))
		dev_info(dev, "%s: no default pinctrl state\n", DEV_NAME);

	for (i = 0; i < GP_MAX; i++) {
		e->gpio[i] = devm_gpiod_get(dev, gp_con_ids[i], GPIOD_IN);
		if (IS_ERR(e->gpio[i])) {
			ret = PTR_ERR(e->gpio[i]);
			dev_err(dev, "%s: cannot get gpio %s: %d\n",
				DEV_NAME, gp_con_ids[i], ret);
			return ret;
		}
	}

	e->mdev.minor = MISC_DYNAMIC_MINOR;
	e->mdev.name = DEV_NAME;
	e->mdev.fops = &crux_esoc_fops;
	e->mdev.parent = dev;
	e->mdev.mode = 0600;

	ret = misc_register(&e->mdev);
	if (ret) {
		dev_err(dev, "%s: misc_register failed: %d\n", DEV_NAME, ret);
		return ret;
	}

	platform_set_drvdata(pdev, e);

	e->status_irq = gpiod_to_irq(e->gpio[GP_MDM_STATUS]);
	if (e->status_irq >= 0) {
		ret = devm_request_threaded_irq(dev, e->status_irq, NULL,
						x50_status_irq,
						IRQF_TRIGGER_RISING |
						IRQF_TRIGGER_FALLING |
						IRQF_ONESHOT,
						"crux-esoc-status", e);
		if (ret)
			dev_err(dev, "%s: cannot request STATUS irq %d: %d\n",
				DEV_NAME, e->status_irq, ret);
		else
			dev_info(dev, "%s: STATUS irq %d registered (both edges)\n",
				 DEV_NAME, e->status_irq);
	} else {
		dev_warn(dev, "%s: no irq for mdm2ap-status (poll only)\n", DEV_NAME);
	}

	dev_info(dev, "%s ready: /dev/%s\n", DEV_NAME, DEV_NAME);
	dev_info(dev, "%s gpios: reset=%s ap_status=%s ap_errfatal=%s mdm_status=%s mdm_errfatal=%s\n",
		 DEV_NAME, gp_con_ids[GP_RESET], gp_con_ids[GP_AP_STATUS],
		 gp_con_ids[GP_AP_ERRFATAL], gp_con_ids[GP_MDM_STATUS],
		 gp_con_ids[GP_MDM_ERRFATAL]);
	dev_info(dev, "%s initial status=0x%x\n", DEV_NAME, x50_read_status(e));
	return 0;
}

static void crux_esoc_remove(struct platform_device *pdev)
{
	struct crux_esoc *e = platform_get_drvdata(pdev);

	misc_deregister(&e->mdev);
	cancel_work_sync(&e->resume_work);
	cancel_work_sync(&e->pon_work);
	dev_info(&pdev->dev, "%s removed\n", DEV_NAME);
}

static const struct of_device_id crux_esoc_of_match[] = {
	{ .compatible = "qcom,ext-sdx50m" },
	{ }
};
MODULE_DEVICE_TABLE(of, crux_esoc_of_match);

static struct platform_driver crux_esoc_driver = {
	.probe	= crux_esoc_probe,
	.remove	= crux_esoc_remove,
	.driver	= {
		.name		= DEV_NAME,
		.of_match_table	= crux_esoc_of_match,
	},
};
module_platform_driver(crux_esoc_driver);

MODULE_AUTHOR("crux bring-up");
MODULE_DESCRIPTION("Minimal esoc-equivalent for the external SDX50M (X50) on crux");
MODULE_LICENSE("GPL v2");
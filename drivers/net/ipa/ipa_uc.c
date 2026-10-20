// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018-2024 Linaro Ltd.
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/pm_runtime.h>
#include <linux/types.h>

#include "ipa.h"
#include "ipa_interrupt.h"
#include "ipa_power.h"
#include "ipa_reg.h"
#include "ipa_uc.h"
#include <linux/jiffies.h>
#include "gsi.h"
#include "gsi_reg.h"
#include <linux/dma-mapping.h>
#include <linux/iommu.h>

/**
 * DOC:  The IPA embedded microcontroller
 *
 * The IPA incorporates a microcontroller that is able to do some additional
 * handling/offloading of network activity.  The current code makes
 * essentially no use of the microcontroller, but it still requires some
 * initialization.  It needs to be notified in the event the AP crashes.
 *
 * The microcontroller can generate two interrupts to the AP.  One interrupt
 * is used to indicate that a response to a request from the AP is available.
 * The other is used to notify the AP of the occurrence of an event.  In
 * addition, the AP can interrupt the microcontroller by writing a register.
 *
 * A 128 byte block of structured memory within the IPA SRAM is used together
 * with these interrupts to implement the communication interface between the
 * AP and the IPA microcontroller.  Each side writes data to the shared area
 * before interrupting its peer, which will read the written data in response
 * to the interrupt.  Some information found in the shared area is currently
 * unused.  All remaining space in the shared area is reserved, and must not
 * be read or written by the AP.
 */
/* Supports hardware interface version 0x2000 */

/* Delay to allow a the microcontroller to save state when crashing */
#define IPA_SEND_DELAY		100	/* microseconds */

/* [crux 2026-10-19] ipa-reg 物理基（给 uC 计算 IPA_UC_MAILBOX_m_n 地址用） */
phys_addr_t crux_ipa_reg_phys;
EXPORT_SYMBOL_GPL(crux_ipa_reg_phys);

/* IPA_UC_MAILBOX_m_n 表偏移：v3_0=0x32000, v4_5=0x82000，v4_1 取 0x32000 */
#define IPA_UC_MBOX_TBL_OFFSET	0x32000u

static struct ipa *crux_ipa_inst;

/* [crux 2026-10-19] 原厂 IPA_ACTIVE_CLIENTS_INC_SPECIAL("IMP") 的等价物：
 * 任何碰 IPA/GSI 寄存器或 uC 共享内存之前，必须先让 IPA 时钟/电源起来，
 * 否则会在 runtime suspend 状态下访存 → 总线错误 → 整机复位（已实测）。
 * ipa_mhi_proxy 是模块，通过这两个导出接口投票。
 */
int crux_ipa_pm_get(void)
{
	int ret;

	if (!crux_ipa_inst || !crux_ipa_inst->dev)
		return -ENODEV;

	ret = pm_runtime_get_sync(crux_ipa_inst->dev);
	if (ret < 0) {
		pm_runtime_put_noidle(crux_ipa_inst->dev);
		dev_err(crux_ipa_inst->dev,
			"[crux] crux_ipa_pm_get: %d\n", ret);
		return ret;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(crux_ipa_pm_get);

void crux_ipa_pm_put(void)
{
	if (!crux_ipa_inst || !crux_ipa_inst->dev)
		return;
	(void)pm_runtime_put_autosuspend(crux_ipa_inst->dev);
}
EXPORT_SYMBOL_GPL(crux_ipa_pm_put);

/* [crux 2026-10-19] uC 命令串行化（原厂 ipa3_uc_send_cmd 用 uc_ctx.uc_lock） */
static DEFINE_MUTEX(crux_uc_cmd_lock);

/* [crux] 原厂 ipa3_mhi_init_engine：mhi_base_chan_idx = gsi_chan_num(1) + first_ch_idx(100) */
#define CRUX_MHI_GSI_CHAN_NUM	1
#define CRUX_MHI_FIRST_CH_IDX	100
#define CRUX_MHI_BASE_CHAN_IDX	(CRUX_MHI_GSI_CHAN_NUM + CRUX_MHI_FIRST_CH_IDX)
#define CRUX_MHI_BASE_CHAN_SHIFT	24

u32 crux_ipa_uc_mbox_addr(u32 m, u32 n)
{
	if (!crux_ipa_reg_phys)
		return 0;
	return (u32)(crux_ipa_reg_phys + IPA_UC_MBOX_TBL_OFFSET +
		     0x80 * m + 4 * n);
}
EXPORT_SYMBOL_GPL(crux_ipa_uc_mbox_addr);

/**
 * struct ipa_uc_mem_area - AP/microcontroller shared memory area
 * @command:		command code (AP->microcontroller)
 * @reserved0:		reserved bytes; avoid reading or writing
 * @command_param:	low 32 bits of command parameter (AP->microcontroller)
 * @command_param_hi:	high 32 bits of command parameter (AP->microcontroller)
 *
 * @response:		response code (microcontroller->AP)
 * @reserved1:		reserved bytes; avoid reading or writing
 * @response_param:	response parameter (microcontroller->AP)
 *
 * @event:		event code (microcontroller->AP)
 * @reserved2:		reserved bytes; avoid reading or writing
 * @event_param:	event parameter (microcontroller->AP)
 *
 * @first_error_address: address of first error-source on SNOC
 * @hw_state:		state of hardware (including error type information)
 * @warning_counter:	counter of non-fatal hardware errors
 * @reserved3:		reserved bytes; avoid reading or writing
 * @interface_version:	hardware-reported interface version
 * @reserved4:		reserved bytes; avoid reading or writing
 *
 * A shared memory area at the base of IPA resident memory is used for
 * communication with the microcontroller.  The region is 128 bytes in
 * size, but only the first 40 bytes (structured this way) are used.
 */
struct ipa_uc_mem_area {
	u8 command;		/* enum ipa_uc_command */
	u8 reserved0[3];
	__le32 command_param;
	__le32 command_param_hi;
	u8 response;		/* enum ipa_uc_response */
	u8 reserved1[3];
	__le32 response_param;
	u8 event;		/* enum ipa_uc_event */
	u8 reserved2[3];

	__le32 event_param;
	__le32 first_error_address;
	u8 hw_state;
	u8 warning_counter;
	__le16 reserved3;
	__le16 interface_version;
	__le16 reserved4;
};

/** enum ipa_uc_command - commands from the AP to the microcontroller */
enum ipa_uc_command {
	IPA_UC_COMMAND_NO_OP		= 0x0,
	IPA_UC_COMMAND_UPDATE_FLAGS	= 0x1,
	IPA_UC_COMMAND_DEBUG_RUN_TEST	= 0x2,
	IPA_UC_COMMAND_DEBUG_GET_INFO	= 0x3,
	IPA_UC_COMMAND_ERR_FATAL	= 0x4,
	IPA_UC_COMMAND_CLK_GATE		= 0x5,
	IPA_UC_COMMAND_CLK_UNGATE	= 0x6,
	IPA_UC_COMMAND_MEMCPY		= 0x7,
	IPA_UC_COMMAND_RESET_PIPE	= 0x8,
	IPA_UC_COMMAND_REG_WRITE	= 0x9,
	IPA_UC_COMMAND_GSI_CH_EMPTY	= 0xa,
	/* [crux] 原厂 IPA_CPU_2_HW_CMD_REMOTE_IPA_INFO = FEATURE_ENUM_VAL(COMMON,11) */
	IPA_UC_COMMAND_REMOTE_IPA_INFO	= 0xb,
};

/** enum ipa_uc_response - microcontroller response codes */
enum ipa_uc_response {
	IPA_UC_RESPONSE_NO_OP		= 0x0,
	IPA_UC_RESPONSE_INIT_COMPLETED	= 0x1,
	IPA_UC_RESPONSE_CMD_COMPLETED	= 0x2,
	IPA_UC_RESPONSE_DEBUG_GET_INFO	= 0x3,
};

/** enum ipa_uc_event - common cpu events reported by the microcontroller */
enum ipa_uc_event {
	IPA_UC_EVENT_NO_OP		= 0x0,
	IPA_UC_EVENT_ERROR		= 0x1,
	IPA_UC_EVENT_LOG_INFO		= 0x2,
};

static struct ipa_uc_mem_area *ipa_uc_shared(struct ipa *ipa)
{
	const struct ipa_mem *mem = ipa_mem_find(ipa, IPA_MEM_UC_SHARED);
	u32 offset = ipa->mem_offset + mem->offset;

	return ipa->mem_virt + offset;
}

/* [crux 2026-10-19] 打印 uC 上报的能力位图：protocolMask（原厂称 featureMask）
 * bit0=COMMON bit1=MHI bit2=POWER_COLLAPSE bit3=WDI bit4=NTN
 * 位于 IPA_MEM_UC_INFO 起始 u32（IpaHwEventLogInfoData_t.protocolMask）。
 * 只有 uC 真正跑起来后里面才有值，所以 config 和 LOG_INFO 两处都打。
 */
static void crux_dump_uc_info(struct ipa *ipa, const char *when)
{
	const struct ipa_mem *info = ipa_mem_find(ipa, IPA_MEM_UC_INFO);
	u32 mask, circ_ofs;
	void *p;

	if (!info) {
		dev_info(ipa->dev, "[crux] uC info(%s): IPA_MEM_UC_INFO 未配置\n", when);
		return;
	}

	p = ipa->mem_virt + ipa->mem_offset + info->offset;
	mask = readl(p);
	circ_ofs = readl(p + 4);

	dev_info(ipa->dev,
		 "[crux] uC info(%s): protocolMask=%#x [COMMON=%u MHI=%u PWRCOLLAPSE=%u WDI=%u NTN=%u] circBufOfs=%#x\n",
		 when, mask, mask & 1u, (mask >> 1) & 1u, (mask >> 2) & 1u,
		 (mask >> 3) & 1u, (mask >> 4) & 1u, circ_ofs);
}

/* [crux 2026-10-19] 诊断：uC 命令通道到底在哪
 * 只读，不改任何发送逻辑。
 * CAF 对 v4.x 把命令寄存器放在 SRAM 窗口：
 *   ipa_wrapper_base + ipa_reg_base_ofst
 *   + ipahal_get_reg_n_ofst(IPA_SW_AREA_RAM_DIRECT_ACCESS_n, 0)
 * 主线 send_uc_command() 写的是 IPA_MEM_UC_SHARED（DRAM）。两者哪一个是对的，
 * 靠这里 dump 出来的数据说话。
 */
static void crux_dump_uc_windows(struct ipa *ipa, const char *tag)
{
	static const phys_addr_t cand[] = { 0x1e00000, 0x1e07000, 0x1e10000 };
	const struct ipa_mem *m;
	void __iomem *p;
	u32 w[10];
	int i, k;

	m = ipa_mem_find(ipa, IPA_MEM_UC_SHARED);
	if (m)
		dev_info(ipa->dev,
			 "[crux] ucwin(%s) UC_SHARED: off=%#x size=%u mem_offset=%#x mem_size=%#x base_phys=%#llx shared_phys=%#llx\n",
			 tag, m->offset, m->size,
			 ipa->mem_offset, ipa->mem_size,
			 (unsigned long long)ipa->mem_addr,
			 (unsigned long long)(ipa->mem_addr + ipa->mem_offset));

	for (i = 0; i < ARRAY_SIZE(cand); i++) {
		p = ioremap(cand[i], 0x80);
		if (!p) {
			dev_info(ipa->dev,
				 "[crux] ucwin(%s) %#llx: ioremap fail\n",
				 tag, (unsigned long long)cand[i]);
			continue;
		}
		for (k = 0; k < 10; k++)
			w[k] = readl(p + 4 * k);
		dev_info(ipa->dev,
			 "[crux] ucwin(%s) %#llx: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
			 tag, (unsigned long long)cand[i],
			 w[0], w[1], w[2], w[3], w[4],
			 w[5], w[6], w[7], w[8], w[9]);
		iounmap(p);
	}
}

/* Microcontroller event IPA interrupt handler */
static void ipa_uc_event_handler(struct ipa *ipa)
{
	struct ipa_uc_mem_area *shared = ipa_uc_shared(ipa);
	struct device *dev = ipa->dev;

	if (shared->event == IPA_UC_EVENT_ERROR)
		dev_err(dev, "microcontroller error event\n");
	else if (shared->event != IPA_UC_EVENT_LOG_INFO)
		dev_err(dev, "unsupported microcontroller event %u\n",
			shared->event);
	/* [crux 2026-10-19] LOG_INFO 也说明 uC 已经跑起来（后加载场景） */
	else if (!ipa->uc_loaded) {
		ipa->uc_loaded = true;
		dev_info(dev, "[crux] uC running (LOG_INFO), uc_loaded=1, ifver=%u\n",
			 le16_to_cpu(shared->interface_version));
		crux_dump_uc_info(ipa, "LOG_INFO");
	}
}

/* Microcontroller response IPA interrupt handler */
static void ipa_uc_response_hdlr(struct ipa *ipa)
{
	struct ipa_uc_mem_area *shared = ipa_uc_shared(ipa);
	struct device *dev = ipa->dev;

	/* An INIT_COMPLETED response message is sent to the AP by the
	 * microcontroller when it is operational.  Other than this, the AP
	 * should only receive responses from the microcontroller when it has
	 * sent it a request message.
	 *
	 * We can drop the power reference taken in ipa_uc_power() once we
	 * know the microcontroller has finished its initialization.
	 */
	switch (shared->response) {
	case IPA_UC_RESPONSE_INIT_COMPLETED:
		if (ipa->uc_powered) {
			ipa->uc_loaded = true;
			ipa_power_retention(ipa, true);
			(void)pm_runtime_put_autosuspend(dev);
			ipa->uc_powered = false;
		} else {
			/*
			 * [crux 2026-10-15] Late-load case: the microcontroller can
			 * finish its init (and send this response) before the modem
			 * powerup that takes the proxy reference, so uc_powered is
			 * still false here. The uC really did come up -- accept it
			 * instead of dropping the message.
			 */
			dev_info(dev, "[crux] init_completed (late load), set uc_loaded=1\n");
			ipa->uc_loaded = true;
		}
		break;
	default:
		dev_warn(dev, "unsupported microcontroller response %u\n",
			 shared->response);
		break;
	}
}

void ipa_uc_interrupt_handler(struct ipa *ipa, enum ipa_irq_id irq_id)
{
	/* Silently ignore anything unrecognized */
	if (irq_id == IPA_IRQ_UC_0)
		ipa_uc_event_handler(ipa);
	else if (irq_id == IPA_IRQ_UC_1)
		ipa_uc_response_hdlr(ipa);
}

/* Configure the IPA microcontroller subsystem */
void ipa_uc_config(struct ipa *ipa)
{
	struct ipa_uc_mem_area *shared = ipa_uc_shared(ipa);

	crux_ipa_inst = ipa;
	ipa->uc_powered = false;
	ipa->uc_loaded = false;
	ipa_interrupt_enable(ipa, IPA_IRQ_UC_0);
	ipa_interrupt_enable(ipa, IPA_IRQ_UC_1);

	dev_info(ipa->dev, "[crux] ipa_uc_config: reg_phys=0x%llx ifver=%u\n",
		 (unsigned long long)crux_ipa_reg_phys,
		 le16_to_cpu(shared->interface_version));
	crux_dump_uc_info(ipa, "config");
}

/* Inverse of ipa_uc_config() */
void ipa_uc_deconfig(struct ipa *ipa)
{
	struct device *dev = ipa->dev;

	ipa_interrupt_disable(ipa, IPA_IRQ_UC_1);
	ipa_interrupt_disable(ipa, IPA_IRQ_UC_0);
	if (ipa->uc_loaded)
		ipa_power_retention(ipa, false);

	if (!ipa->uc_powered)
		return;

	(void)pm_runtime_put_autosuspend(dev);
}

/* Take a proxy power reference for the microcontroller */
void ipa_uc_power(struct ipa *ipa)
{
	struct device *dev = ipa->dev;
	static bool already;
	int ret;

	if (already)
		return;
	already = true;		/* Only do this on first boot */

	/* This power reference dropped in ipa_uc_response_hdlr() above */
	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		pm_runtime_put_noidle(dev);
		dev_err(dev, "error %d getting proxy power\n", ret);
	} else {
		ipa->uc_powered = true;
	}
}

/* Send a command to the microcontroller */
static void send_uc_command(struct ipa *ipa, u32 command, u32 command_param)
{
	struct ipa_uc_mem_area *shared = ipa_uc_shared(ipa);
	const struct reg *reg;
	u32 val;

	/* Fill in the command data */
	shared->command = command;
	shared->command_param = cpu_to_le32(command_param);
	shared->command_param_hi = 0;
	shared->response = 0;
	shared->response_param = 0;

	/* [crux 2026-10-19] 共享区是 WC 映射，敲门前必须落盘（原厂同） */
	wmb();

	/* Use an interrupt to tell the microcontroller the command is ready */
	reg = ipa_reg(ipa, IPA_IRQ_UC);
	val = reg_bit(reg, UC_INTR);

	iowrite32(val, ipa->reg_virt + reg_offset(reg));

	/* [crux 2026-10-19] 敲完门立刻回读，判断写有没有真落到 SRAM */
	dev_info(ipa->dev,
		 "[crux] send_uc cmd=%#x param=%#x -> rb: cmd=%#x param=%#x rsp=%#x rparam=%#x\n",
		 command, command_param,
		 readl((void __iomem *)shared + 0x00),
		 readl((void __iomem *)shared + 0x04),
		 readl((void __iomem *)shared + 0x0c),
		 readl((void __iomem *)shared + 0x10));
}

/**
 * crux_ipa_gsi_set_mhi_base_chan() - 写 GSI CNTXT_SCRATCH_0 的 mhi_base_chan_idx
 *
 * 原厂 ipa3_mhi_init_engine() 只做这一件事：告诉 IPA 硬件 MHI 通道在 GSI 里的
 * 起始通道号，取自 gsi_ep_info(IPA_CLIENT_MHI_PROD)->ipa_gsi_chan_num(=1)
 * + params->gsi.first_ch_idx(=100) = 101，字段在 bit[31:24]。
 * 它排在 ipa3_uc_send_remote_ipa_info() 之前，是门铃代理能生效的前提。
 */
int crux_ipa_gsi_set_mhi_base_chan(void)
{
	struct ipa *ipa = crux_ipa_inst;
	const struct reg *reg;
	u32 off, old, new;
	u32 idx = CRUX_MHI_BASE_CHAN_IDX;
	int pmret;

	if (!ipa || !ipa->gsi.virt)
		return -ENODEV;

	/* [crux 2026-10-19] 写 GSI 寄存器前必须让 IPA 醒着 */
	pmret = crux_ipa_pm_get();
	if (pmret)
		return pmret;

	reg = gsi_reg(&ipa->gsi, CNTXT_SCRATCH_0);
	if (!reg)
		return -ENODEV;

	off = reg_offset(reg);
	old = ioread32(ipa->gsi.virt + off);
	new = old & ~(0xffu << CRUX_MHI_BASE_CHAN_SHIFT);
	new |= (idx & 0xffu) << CRUX_MHI_BASE_CHAN_SHIFT;
	iowrite32(new, ipa->gsi.virt + off);
	new = ioread32(ipa->gsi.virt + off);	/* 回读自证 */

	dev_info(ipa->dev,
		 "[crux] gsi scratch word0 %#x -> %#x (mhi_base_chan_idx=%u, readback=%u)\n",
		 old, new, idx, (new >> CRUX_MHI_BASE_CHAN_SHIFT) & 0xffu);

	crux_ipa_pm_put();

	return ((new >> CRUX_MHI_BASE_CHAN_SHIFT) & 0xffu) == idx ? 0 : -EIO;
}
EXPORT_SYMBOL_GPL(crux_ipa_gsi_set_mhi_base_chan);

/* [crux 2026-10-19] 对应原厂 imp_mhi_probe_cb() 的 iommu_map 步骤：
 * 原厂在发 0x0b 之前，把 X50 的 doorbell 页 identity-map 进 uC 的 SMMU
 * domain，并带 IOMMU_MMIO（device 类型访问）：
 *
 *   iommu_map(cb->mapping->domain, iova_p, pa_p, size_p,
 *             IOMMU_READ | IOMMU_WRITE | IOMMU_MMIO);
 *
 * 我们这边 IPA 的 iommus 同时声明了 0x520(AP) 与 0x522(uC)，两者在同一个
 * iommu_group，共用同一个 domain，所以直接对 ipa->dev 的 domain 做
 * identity map 即可让 uC 摸到这个 PCIe doorbell 地址。
 *
 * 不做这一步时：uC 收到 0x0b 后会去访问 remoteIPAAddr，SMMU 无映射 →
 * SMMU fault → SoC 级硬复位（这就是 stage3 一执行就整机重启的原因）。
 */
static int crux_uc_map_doorbell_identity(struct ipa *ipa, u32 addr)
{
	struct iommu_domain *domain;
	dma_addr_t iova = (dma_addr_t)(addr & PAGE_MASK);
	phys_addr_t mapped;
	int ret;

	domain = iommu_get_domain_for_dev(ipa->dev);
	if (!domain) {
		dev_err(ipa->dev, "[crux] uc map: no iommu domain for ipa\n");
		return -ENODEV;
	}

	/* 已经是 identity 映射就直接返回 */
	mapped = iommu_iova_to_phys(domain, iova);
	if (mapped == (phys_addr_t)iova)
		return 0;

	ret = iommu_map(domain, iova, (phys_addr_t)iova, PAGE_SIZE,
			IOMMU_READ | IOMMU_WRITE | IOMMU_MMIO, GFP_KERNEL);
	if (ret) {
		dev_err(ipa->dev,
			"[crux] uc map: iommu_map(%pad, size=%lu, IOMMU_MMIO) failed %d\n",
			&iova, (unsigned long)PAGE_SIZE, ret);
		return ret;
	}

	dev_info(ipa->dev,
		 "[crux] uc map: doorbell %#x identity-mapped (page %pad, IOMMU_MMIO)\n",
		 addr, &iova);

	return 0;
}

/**
 * crux_ipa_uc_send_remote_ipa_info() - 把远端 IPA（外挂基带）doorbell 登记成 uC 邮箱
 * @remote_addr:	远端 doorbell 物理地址
 * @mbox_n:	邮箱编号
 */
int crux_ipa_uc_send_remote_ipa_info(u32 remote_addr, u32 mbox_n)
{
	struct ipa *ipa = crux_ipa_inst;
	struct ipa_uc_mem_area *shared;
	const struct reg *reg;
	struct {
		__le32 remote_ipa_addr;
		__le32 mbox_n;
	} *db;
	dma_addr_t phys;
	unsigned long timeout;
	u32 cmd = IPA_UC_COMMAND_REMOTE_IPA_INFO;
	u32 stts, rp, resp_op, resp_status;
	int i, ret = 0;

	if (!ipa)
		return -ENODEV;
	if (!ipa->uc_loaded)
		return -EAGAIN;

	/*
	 * [crux 2026-10-19] 三处对齐 CAF ipa3_uc_send_remote_ipa_info：
	 *
	 * 1) 参数 buffer 必须放在 uC 能访问的地址上。原厂用 ipa3_ctx->uc_pdev
	 *    与主线区别只在于有没有独立的 uC SMMU：我们的 IPA 节点 iommus 同时
	 *    绑了 0x520(AP) 与 0x522(UC)，arm-smmu 给它们建了同一个 IOMMU
	 *    domain（dmesg: platform 1e40000.ipa: Adding to iommu group 11），
	 *    所以 dma_alloc_coherent 返回的 IOVA 两个 SID 共用；再配 GFP_DMA32
	 *    保证地址 <4G，uC（32 位）才摸得到。
	 * 2) 超时按原厂 10*HZ（原来 300 轮 * ~750us 只有 0.2s，太短）。
	 * 3) 回执要校验 originalCmdOp 与 status，不能只看 response==CMD_COMPLETED。
	 */
	mutex_lock(&crux_uc_cmd_lock);

	/* [crux 2026-10-19] IPA 空闲时会 runtime suspend（时钟关）。
	 * 此时读 SRAM 全 0、写直接丢弃 —— 之前 send 后读回 0 就是这原因。
	 * CAF 发送前同样 inc_usage_count + enable_clks。
	 */
	ret = pm_runtime_get_sync(ipa->dev);
	if (ret < 0) {
		pm_runtime_put_noidle(ipa->dev);
		dev_err(ipa->dev,
			"[crux] remote_ipa_info: pm_runtime_get_sync fail %d\n", ret);
		mutex_unlock(&crux_uc_cmd_lock);
		return ret;
	}
	dev_info(ipa->dev, "[crux] pm: suspended=%d (0=已唤醒)\n",
		 pm_runtime_status_suspended(ipa->dev));

	db = dma_alloc_coherent(ipa->dev, sizeof(*db), &phys,
				GFP_KERNEL | GFP_DMA32);
	if (!db) {
		ret = -ENOMEM;
		goto out_unlock;
	}
	if (upper_32_bits(phys)) {
		dev_warn(ipa->dev,
			 "[crux] remote_ipa_info: pa=%pad 超过 32 位，uC 访问不到\n",
			 &phys);
		ret = -ERANGE;
		goto out;
	}

	db->remote_ipa_addr = cpu_to_le32(remote_addr);
	db->mbox_n = cpu_to_le32(mbox_n);

	dev_info(ipa->dev,
		 "[crux] remote_ipa_info: param buf pa=%pad remote=%#x mbox=%u\n",
		 &phys, remote_addr, mbox_n);

	/* [crux] dump 暂时关闭，用于隔离崩点：crux_dump_uc_windows(ipa, "pre"); */
	/* [crux] shared dump 暂时关闭，用于隔离崩点 */
	dev_info(ipa->dev, "[crux] D2b: dma_alloc ok pa=%pad (dumps skipped)\n", &phys);
	msleep(300);

	/* [crux] 放到最后：先确保参数 buffer 已分配，再做 doorbell identity map */
	dev_info(ipa->dev, "[crux] D1: about to identity-map doorbell %#x\n", remote_addr);
	msleep(300);
	ret = crux_uc_map_doorbell_identity(ipa, remote_addr);
	dev_info(ipa->dev, "[crux] D2: identity-map returned %d\n", ret);
	msleep(300);
	if (ret) {
		dev_err(ipa->dev,
			"[crux] remote_ipa_info: doorbell %#x 未映射进 uC domain，拒绝发送 0x0b\n",
			remote_addr);
		goto out;
	}

	dev_info(ipa->dev, "[crux] D3: about to send uc cmd=%#x param=%#x\n",
		 cmd, lower_32_bits(phys));
	msleep(300);
	send_uc_command(ipa, cmd, lower_32_bits(phys));
	dev_info(ipa->dev, "[crux] D4: send_uc_command returned\n");
	msleep(300);

	shared = ipa_uc_shared(ipa);
	/* [crux 2026-10-19] 关键分叉：写进去的是不是立刻就被清掉
	 * t1 紧跟 send_uc_command（写 0x0b + 敲门之后的第一次读）
	 * t2 隔 1ms 再读
	 *   t1=0x0b, t2=0      → uC 看到了并清掉（协议/命令号不对）
	 *   t1=0,    t2=0      → 写根本没落到 SRAM（映射/权限问题）
	 */
	dev_info(ipa->dev, "[crux] uc-cmd t1(+0us): cmd=%#x param=%#x rsp=%#x rparam=%#x\n",
		 readl((void __iomem *)shared + 0x00),
		 readl((void __iomem *)shared + 0x04),
		 readl((void __iomem *)shared + 0x0c),
		 readl((void __iomem *)shared + 0x10));
	udelay(1000);
	dev_info(ipa->dev, "[crux] uc-cmd t2(+1ms): cmd=%#x param=%#x rsp=%#x rparam=%#x\n",
		 readl((void __iomem *)shared + 0x00),
		 readl((void __iomem *)shared + 0x04),
		 readl((void __iomem *)shared + 0x0c),
		 readl((void __iomem *)shared + 0x10));
	timeout = jiffies + 10 * HZ;
	for (i = 0; time_before(jiffies, timeout); i++) {
		if (shared->response == IPA_UC_RESPONSE_CMD_COMPLETED)
			break;
		usleep_range(1000, 2000);
	}

	rp = le32_to_cpu(shared->response_param);
	resp_op = rp & 0xffffu;		/* IpaHwCpuCmdCompletedResponseData_t.params.originalCmdOp */
	resp_status = rp >> 16;		/* .params.status，0=SUCCESS */

	reg = ipa_reg(ipa, IPA_IRQ_STTS);
	stts = ioread32(ipa->reg_virt + reg_offset(reg));

	if (shared->response != IPA_UC_RESPONSE_CMD_COMPLETED) {
		dev_warn(ipa->dev,
			 "[crux] remote_ipa_info(0x%x,m%u) TIMEOUT(%ums): cmd=%u rsp=%u rparam=%#x stts=%#x\n",
			 remote_addr, mbox_n, 10000, shared->command,
			 shared->response, rp, stts);
		ret = -ETIMEDOUT;
	} else if (resp_op != cmd) {
		dev_warn(ipa->dev,
			 "[crux] remote_ipa_info(0x%x,m%u) OP MISMATCH: expect %u got %u status=%u\n",
			 remote_addr, mbox_n, cmd, resp_op, resp_status);
		ret = -EPROTO;
	} else if (resp_status) {
		dev_warn(ipa->dev,
			 "[crux] remote_ipa_info(0x%x,m%u) uC STATUS FAIL: status=%u\n",
			 remote_addr, mbox_n, resp_status);
		ret = -EIO;
	} else {
		dev_info(ipa->dev,
			 "[crux] remote_ipa_info(0x%x,m%u) OK: status=0 rparam=%#x stts=%#x\n",
			 remote_addr, mbox_n, rp, stts);
	}
out:
	dma_free_coherent(ipa->dev, sizeof(*db), db, phys);
out_unlock:
	(void)pm_runtime_put_autosuspend(ipa->dev);
	mutex_unlock(&crux_uc_cmd_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(crux_ipa_uc_send_remote_ipa_info);

/* Tell the microcontroller the AP is shutting down */
void ipa_uc_panic_notifier(struct ipa *ipa)
{
	if (!ipa->uc_loaded)
		return;

	send_uc_command(ipa, IPA_UC_COMMAND_ERR_FATAL, 0);

	/* give uc enough time to save state */
	udelay(IPA_SEND_DELAY);
}

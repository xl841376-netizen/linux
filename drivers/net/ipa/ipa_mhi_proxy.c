// SPDX-License-Identifier: GPL-2.0
/*
 * [crux 2026-10-16] Minimal IPA-MHI proxy for the external SDX50M ("X50").
 *
 * Architecture (from the vendor sources):
 *   - The internal modem is the IPA "master"; it reaches the external modem's
 *     data path through the IPA hardware MHI-proxy doorbells (uC mailboxes).
 *   - The AP owns the MHI stack of the external modem, so it must:
 *       1) send QMI_IPA_MHI_READY_IND (0x3c) to the modem, telling it the
 *          proxy channel/event doorbell addresses,
 *       2) when the modem answers with QMI_IPA_MHI_ALLOC_CHANNEL (0x3d),
 *          program the external modem's channel/event contexts with the
 *          contexts the modem supplies (mhi_device_configure()).
 *
 * This module currently implements step 1 (bind + ready indication); the
 * 0x3d handler is next.
 */
#include <linux/module.h>
#include "ipa.h"
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/crux_dep.h>
#include <linux/mhi.h>
#include <linux/pci.h>
#include <linux/dma-direction.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/inet.h>
#include <net/sock.h>
#include <linux/qrtr.h>
#include <linux/unaligned.h>

#include "ipa_qmi_msg.h"
#include "ipa_qmi.h"

/* IPA wrapper base / uC mailbox register offset / mailbox m index.
 * VM offsets from the vendor register tables: v3_0 -> 0x32000, v4_5 -> 0x82000.
 */
static unsigned long ipa_wrapper_base = 0x1e00000;
module_param(ipa_wrapper_base, ulong, 0644);
MODULE_PARM_DESC(ipa_wrapper_base, "IPA wrapper MMIO base");

static unsigned long mbox_off = 0x32000;
module_param(mbox_off, ulong, 0644);
MODULE_PARM_DESC(mbox_off, "IPA uC mailbox register offset");

static unsigned int uc_m = 1;
module_param(uc_m, uint, 0644);
MODULE_PARM_DESC(uc_m, "IPA uC mailbox m index");

/* Destination of the modem's IPA service (qrtr node:port) */
static unsigned int qmi_node = 0;
module_param(qmi_node, uint, 0644);
static unsigned int qmi_port = 30;
module_param(qmi_port, uint, 0644);

/* Build the QMI_IPA_MHI_READY_IND (0x3c) message exactly like the userspace
 * probe that the modem accepted, and push it over a kernel QRTR socket.
 * (The QMI encoder path in the kernel rejects this message's variable length
 * struct array, so we serialize it by hand.)
 */
static struct socket *mhi_proxy_sock;
static void mhi_proxy_rx(struct work_struct *work);
static DECLARE_DELAYED_WORK(mhi_proxy_rx_work, mhi_proxy_rx);

#define READY_IND_SZ (5 + 3 + 1 + 2 * IPA_MHI_CH_DESC_SZ)

static int mhi_proxy_send_ready_ind(const struct ipa_mhi_ch_init_info *ch,
				    unsigned int n_ch)
{
	int ret;

	/* [crux 2026-10-17] 改走 IPA 驱动自带的 QMI 通道：目标端口是 modem 的 IPA
	 * service（qrtr 0:31），而且它是 request 形式（modem 会回 response，
	 * 之后才发 0x3d ALLOC_CHANNEL）。之前手捣 socket 发到 0:30，端口就不对。
	 */
	/*
	 * Do not announce MHI readiness unless the modem side actually reached
	 * mission mode.  If the EE state is stale/SBL we would be telling the
	 * modem it can use channels that our own state machine is still
	 * recovering, which is exactly the two-drivers-fighting case.
	 */
	if (!crux_dep_require("x50_ee", CRUX_DEP_UP, "ipa_mhi_proxy", false))
		return -EAGAIN;

	ret = ipa_qmi_send_mhi_ready_ind(ch, n_ch);
	pr_info("[crux mhi-proxy] ipa_qmi_send_mhi_ready_ind(%u ch) ret=%d\n",
		n_ch, ret);
	return ret;
}

/* 收包：modem 的 QMI_IPA_MHI_ALLOC_CHANNEL(0x3d) 等指示会回到我们发出的源端口，
 * 原来发完就 sock_release 导致全部丢弃。这里常驻 socket 并轮询读取。
 */
static void mhi_proxy_rx(struct work_struct *work)
{
	u8 buf[512];
	struct kvec iov;
	struct msghdr msg;
	int ret;

	if (!mhi_proxy_sock)
		return;

	while (true) {
		memset(&msg, 0, sizeof(msg));
		iov.iov_base = buf;
		iov.iov_len = sizeof(buf);
		ret = kernel_recvmsg(mhi_proxy_sock, &msg, &iov, 1,
				     sizeof(buf), MSG_DONTWAIT);
		if (ret == -EAGAIN || ret == -EWOULDBLOCK)
			break;
		if (ret <= 0) {
			pr_info("[crux mhi-proxy rx] recv err=%d\n", ret);
			break;
		}
		pr_info("[crux mhi-proxy rx] len=%d type=0x%02x id=0x%04x\n",
			ret, buf[0], (int)(buf[3] | (buf[4] << 8)));
		print_hex_dump(KERN_INFO, "[crux mhi-proxy rx] ", DUMP_PREFIX_OFFSET,
			       16, 1, buf, min(ret, 128), false);
	}

	if (mhi_proxy_sock)
		schedule_delayed_work(&mhi_proxy_rx_work, msecs_to_jiffies(200));
}

static void mhi_proxy_retry(struct work_struct *work);

static struct ipa_mhi_ch_init_info mhi_proxy_ch[2];
static struct mhi_device *mhi_proxy_dev;
static int mhi_prepared;

/* [crux 2026-10-19] 前向声明：probe 里要 INIT_WORK，定义在文件后部 */
static struct work_struct mhi_proxy_uc_stage_wk;
static void mhi_proxy_uc_stage_worker(struct work_struct *work);

static DECLARE_DELAYED_WORK(mhi_proxy_work, mhi_proxy_retry);
static void mhi_proxy_handshake(struct work_struct *work);
static DECLARE_DELAYED_WORK(mhi_proxy_hs_work, mhi_proxy_handshake);

extern u32 crux_ipa_uc_mbox_addr(u32 m, u32 n);
extern int crux_ipa_uc_send_remote_ipa_info(u32 remote_addr, u32 mbox_n);
extern int crux_ipa_gsi_set_mhi_base_chan(void);
static bool crux_connect_enable = true;   /* 默认打开（v3：驱动环 + 正确端点查找） */
module_param(crux_connect_enable, bool, 0644);

/* crux: 普通数据口（mhi_net/rmnet）路线下，proxy 必须完全避让 */
static bool crux_proxy_enable = true;
module_param(crux_proxy_enable, bool, 0644);
static unsigned int crux_gsi_base = 101;   /* MHI 起始 GSI 通道 */
static unsigned int crux_first_ch = 100;   /* modem 视角的首通道 */
module_param(crux_gsi_base, uint, 0644);
module_param(crux_first_ch, uint, 0644);
extern int crux_ipa_mhi_connect(struct ipa *ipa, u32 gsi_chan, bool toward_ipa);
extern int crux_ipa_mhi_ring(bool toward_ipa, u64 *addr, u32 *count);
extern int crux_gsi_start_with_ring(struct gsi *gsi, u32 channel_id,
                                   dma_addr_t ring_addr, u32 ring_count,
                                   void *ring_virt, u32 evt_ring);
extern int crux_ipa_mhi_map_ring(u32 iova, u32 size);
extern struct ipa *crux_ipa_instance(void);
extern int crux_ipa_pm_get(void);
extern void crux_ipa_pm_put(void);

/* [crux 2026-10-19] IPA_UC_MAILBOX_m_n = ipa-reg 物理基 + 0x32000 + 0x80*m + 4*n
 * （原公式直接用 wrapper base，漏了 ipahal reg base 0x40000 → 少了 0x40000）
 */
static u32 db_addr(unsigned int n)
{
	u32 a = crux_ipa_uc_mbox_addr(uc_m, n);

	if (!a) /* IPA 还没起来时回退旧算法（仅用于打印） */
		a = (u32)(ipa_wrapper_base + 0x40000 + mbox_off +
			  0x80 * uc_m + 0x4 * n);
	return a;
}

/* [crux 2026-10-19] X50 的 MHI doorbell 物理地址：动态探测，不写死
 *   pa = BAR0 基址 + CHDBOFF/ERDBOFF(EP 运行时上报) + id*8
 * 这与 WDI 那套（给 WLAN 用的 uC 邮箱 0x1E72xxx）是两回事，不要混用。
 */
#define MHI_CHDBOFF_REG	0x18u
#define MHI_ERDBOFF_REG	0x20u

static u64 mhi_db_phys(struct mhi_controller *cntrl, bool er, u32 id)
{
	struct pci_dev *pdev;
	u64 base;
	u32 off;

	if (!cntrl || !cntrl->cntrl_dev || !cntrl->regs)
		return 0;
	pdev = to_pci_dev(cntrl->cntrl_dev);
	if (!pdev)
		return 0;
	base = pci_resource_start(pdev, 0);
	off = readl(cntrl->regs + (er ? MHI_ERDBOFF_REG : MHI_CHDBOFF_REG));
	if (!off) {
		pr_warn("[crux mhi-proxy] %s db offset reads 0, using %s\n",
			er ? "ERDBOFF" : "CHDBOFF",
			er ? "ERDBOFF fallback 0x720" : "CHDBOFF fallback 0x300");
		off = er ? 0x720 : 0x300;
	}
	return base + off + (u64)id * 8;
}

/* Channel/event numbers cannot be read from struct mhi_chan (opaque in the
 * mainline MHI API), so keep them as parameters; defaults match the
 * IP_HW_OFFLOAD_0 pair in mhi_qcom_sdx50m_channels[].
 */
static unsigned int ul_chan = 104;
module_param(ul_chan, uint, 0644);
static unsigned int ul_er = 5;
module_param(ul_er, uint, 0644);
static unsigned int dl_chan = 105;
module_param(dl_chan, uint, 0644);
static unsigned int dl_er = 6;
module_param(dl_er, uint, 0644);

/* [crux 2026-10-19] 默认 probe 内不发 0x0b / 0x3c，保持开机行为与基线一致。
 * 只有在显式 insmod auto_uc=1 时才做 uC 登记 + ready ind，
 * 用来隔离「开机进桌面 5~10s 直接死机」究竟来自这两步还是别处。
 */
static bool auto_uc = true;  /* [crux 2026-10-19] 默认开：insmod 参数在某些路径下丢失，改默认值更可靠 */
static bool skip_ready;
module_param(skip_ready, bool, 0644);
MODULE_PARM_DESC(skip_ready, "skip MHI_READY_IND(0x3c)");

module_param(auto_uc, bool, 0644);
MODULE_PARM_DESC(auto_uc, "send REMOTE_IPA_INFO(0x0b)+MHI_READY_IND(0x3c) at probe (default 0)");

/* [crux 2026-10-17] 0x3d MHI_ALLOC_CHANNEL -> CCA/ECA -> mhi_device_configure
 * 按原厂 ipa_mhi_proxy.c:__imp_configure_mhi_device() 的语义实现。
 */
struct crux_chan_ctxt {
	__le32 chcfg;
	__le32 chtype;
	__le32 erindex;
	__le64 rbase;
	__le64 rlen;
	__le64 rp;
	__le64 wp;
} __packed;

struct crux_event_ctxt {
	__le32 intmod;
	__le32 ertype;
	__le32 msivec;
	__le64 rbase;
	__le64 rlen;
	__le64 rp;
	__le64 wp;
} __packed;

#define CHAN_CTX_BRSTMODE_MASK	GENMASK(9, 8)
#define CHAN_CTX_POLLCFG_MASK	GENMASK(15, 10)
#define EV_CTX_INTMODC_MASK	GENMASK(15, 8)
#define EV_CTX_INTMODT_MASK	GENMASK(31, 16)

/* [crux 2026-10-28] 0x3d 按原厂结构解析后，配置外部 modem 的 MHI 通道上下文。
 * 原来的手工偏移假设元素 39/45 字节，漏了编译器对齐 pad（实际 40/48），
 * 导致整条消息错位、TLV 长度读成 26626。
 */
static int crux_apply_alloc(const struct ipa_mhi_alloc_ch_req *req)
{
	int i, done = 0;
	unsigned int ul_id = ul_chan, dl_id = dl_chan;

	if (mhi_proxy_dev) {
		if (mhi_proxy_dev->ul_chan_id >= 0)
			ul_id = mhi_proxy_dev->ul_chan_id;
		if (mhi_proxy_dev->dl_chan_id >= 0)
			dl_id = mhi_proxy_dev->dl_chan_id;
	}

	if (!mhi_proxy_dev) {
		pr_err("[crux mhi-proxy] alloc: no mhi device yet\n");
		return -ENODEV;
	}
	if (!req || !req->tr_info_arr_len)
		return -EINVAL;

	pr_info("[crux mhi-proxy] alloc: dev_ul=%d dev_dl=%d use=%u/%u param=%u/%u sizeof_tr=%zu off_ch=%zu\n",
		mhi_proxy_dev->ul_chan_id, mhi_proxy_dev->dl_chan_id,
		ul_id, dl_id, ul_chan, dl_chan,
		sizeof(struct ipa_mhi_tr_info),
		offsetof(struct ipa_mhi_tr_info, ch_id));

	for (i = 0; i < req->tr_info_arr_len && i < QMI_IPA_MHI_CH_MAX; i++) {
		const struct ipa_mhi_tr_info *t = &req->tr_info_arr[i];
		pr_info("[crux mhi-proxy] alloc: raw[%d] ch_id=%u(0x%02x)\n",
			i, t->ch_id, t->ch_id);
		const struct ipa_mhi_er_info *e = NULL;
		unsigned int er_id, dir, k;
		struct mhi_buf cfg[2];
		struct crux_chan_ctxt cc;
		struct crux_event_ctxt ec;
		int ret;

		if (t->ch_id == ul_id) {
			er_id = ul_er;
			dir = DMA_TO_DEVICE;
		} else if (t->ch_id == dl_id) {
			er_id = dl_er;
			dir = DMA_FROM_DEVICE;
		} else {
			pr_info("[crux mhi-proxy] alloc: skip unknown ch %u\n", t->ch_id);
			continue;
		}

		for (k = 0; k < req->er_info_arr_len && k < QMI_IPA_MHI_CH_MAX; k++) {
			if (req->er_info_arr[k].er_id == er_id) {
				e = &req->er_info_arr[k];
				break;
			}
		}
		if (!e) {
			pr_err("[crux mhi-proxy] alloc: no ER %u for ch %u\n",
			       er_id, t->ch_id);
			return -EINVAL;
		}

		memset(&cc, 0, sizeof(cc));
		memset(&ec, 0, sizeof(ec));
		cc.chcfg = cpu_to_le32(
			FIELD_PREP(CHAN_CTX_BRSTMODE_MASK,
				   t->brst_mode_type == 1 ? 3 :
				   (t->brst_mode_type == 0 ? 2 : 0)) |
			FIELD_PREP(CHAN_CTX_POLLCFG_MASK, t->poll_cfg));
		cc.chtype = cpu_to_le32(dir);
		cc.erindex = cpu_to_le32(er_id);
		{
			bool is_tx = (t->ch_id == ul_id);
			u64 daddr = 0;
			u32 dcnt = 0;

			if (crux_ipa_mhi_ring(is_tx, &daddr, &dcnt) == 0) {
				/* 用驱动侧（GSI 已初始化）的环，modem 写同一块内存 */
				cc.rbase = cpu_to_le64(daddr);
				cc.rlen = cpu_to_le64((u64)dcnt * 16);
				cc.rp = cpu_to_le64(0);
				cc.wp = cpu_to_le64(0);
				pr_info("[crux mhi-proxy] ring: ch%u %s use drv addr=%#llx cnt=%u\n",
					t->ch_id, is_tx ? "TX" : "RX",
					(unsigned long long)daddr, dcnt);
			} else {
				/* 退路：仍用 modem 给的环 */
				cc.rbase = cpu_to_le64(t->ring_iova);
				cc.rlen = cpu_to_le64(t->ring_len);
				cc.rp = cpu_to_le64(t->rp);
				cc.wp = cpu_to_le64(t->wp);
				pr_info("[crux mhi-proxy] ring: ch%u %s fallback modem ring=%#llx\n",
					t->ch_id, is_tx ? "TX" : "RX",
					(unsigned long long)t->ring_iova);
			}
		}

		ec.intmod = cpu_to_le32(
			FIELD_PREP(EV_CTX_INTMODC_MASK, e->intmod_cycles) |
			FIELD_PREP(EV_CTX_INTMODT_MASK, e->intmod_count));
		ec.ertype = cpu_to_le32(1);
		ec.msivec = cpu_to_le32(e->msi_addr);
		ec.rbase = cpu_to_le64(e->ring_iova);
		ec.rlen = cpu_to_le64(e->ring_len);
		ec.rp = cpu_to_le64(e->rp);
		ec.wp = cpu_to_le64(e->wp);

		cfg[0].buf = (void *)&cc;
		cfg[0].len = sizeof(cc);
		cfg[0].name = "CCA";
		cfg[1].buf = (void *)&ec;
		cfg[1].len = sizeof(ec);
		cfg[1].name = "ECA";

		ret = mhi_device_configure(mhi_proxy_dev, dir, cfg, 2);
		pr_info("[crux mhi-proxy] alloc: ch%u dir%u er%u configure ret=%d\n",
			t->ch_id, dir, er_id, ret);
		if (ret)
			return ret;
		done++;
	}

	pr_info("[crux mhi-proxy] alloc: programmed %d channel(s)\n", done);

	/* [crux 2026-11-02] 把 MHI 通道接成 IPA 端点。环已经在 CCA 里换成了
	 * 驱动侧（GSI 已初始化）的环，所以这里只需要启动通道 + 配端点。
	 */
	if (done && crux_connect_enable && crux_ipa_instance()) {
		for (i = 0; i < req->tr_info_arr_len && i < QMI_IPA_MHI_CH_MAX; i++) {
			const struct ipa_mhi_tr_info *t = &req->tr_info_arr[i];
			bool tx = (t->ch_id == ul_id);
			int cret;

			if (t->ch_id != ul_id && t->ch_id != dl_id)
				continue;
			cret = crux_ipa_mhi_connect(crux_ipa_instance(), 0, tx);
			pr_info("[crux mhi-proxy] connect ch%u %s ret=%d\n",
				t->ch_id, tx ? "TX" : "RX", cret);
		}
	}

	/* 原厂 __imp_configure_mhi_device 成功后紧接 imp_mhi_prepare_for_transfer()，
	 * 让 MHI host 真正启动这些通道；只 configure 不 prepare，modem 侧 GSI
	 * 看不到通道起来。
	 */
	if (done && !mhi_prepared) {
		int pret = mhi_prepare_for_transfer(mhi_proxy_dev);

		if (pret) {
			pr_err("[crux mhi-proxy] mhi_prepare_for_transfer failed %d\n",
			       pret);
			return pret;
		}
		mhi_prepared = 1;
		pr_info("[crux mhi-proxy] mhi_prepare_for_transfer OK\n");
	}

	return done ? 0 : -EINVAL;
}

/*
 * [crux 2026-10-28] 握手全部异步：probe 里绝不能 msleep。
 * 原来这段在 probe 同步跑，最长阻塞 60s，MHI 事件处理被饿死，
 * 表现就是 X50 抬起（进 mission）时事件环堆满/超时，整机 lockup。
 */
static void mhi_proxy_handshake(struct work_struct *work)
{
	extern int ipa_qmi_rehandshake(u32 node, u32 port);
	extern bool ipa_qmi_modem_client_ready(void);
	struct mhi_device *mhi_dev = mhi_proxy_dev;
	int rh, i, ret;

	if (!mhi_dev)
		return;

	/* X50 重启后 QRTR 不会重复报 NEW_SERVER，旧 modem_client_sq 已失效，
	 * 重发 INIT_DRIVER 让 modem 重新登记 client sq，再发 0x3c。
	 */
	rh = ipa_qmi_rehandshake(0, 0);
	dev_info(&mhi_dev->dev,
		 "[crux mhi-proxy] rehandshake ret=%d, wait modem client\n", rh);
	for (i = 0; i < 30; i++) {
		if (ipa_qmi_modem_client_ready())
			break;
		msleep(2000);
	}
	dev_info(&mhi_dev->dev,
		 "[crux mhi-proxy] modem client ready=%d after %ds\n",
		 ipa_qmi_modem_client_ready(), i * 2);

	if (!ipa_qmi_modem_client_ready()) {
		dev_info(&mhi_dev->dev,
			 "[crux mhi-proxy] client not ready, skip 0x3c this round\n");
		return;
	}

	if (skip_ready) {
		dev_info(&mhi_dev->dev, "[crux mhi-proxy] skip_ready=1, no 0x3c\n");
		return;
	}

	ret = mhi_proxy_send_ready_ind(mhi_proxy_ch, ARRAY_SIZE(mhi_proxy_ch));
	dev_info(&mhi_dev->dev, "[crux mhi-proxy] MHI_READY_IND(0x3c) ret=%d\n", ret);
	if (ret)
		dev_info(&mhi_dev->dev,
			 "[crux mhi-proxy] ready ind failed, wait for modem QMI to come back\n");
}

/* [crux 2026-11-02] 原厂 mhi_qcom_power_up() 的关键一步：
 * 设备如果停在 SYS_ERR 错误态，直接走 PCIe 二次总线复位会把链路打死；
 * 正确做法是发 MHI 软复位（MHI_STATE_RESET）并等它退出错误态，等不到才报错。
 */
static int crux_mhi_syserr_soft_reset(struct mhi_controller *mhi_cntrl)
{
	enum mhi_state st = mhi_get_mhi_state(mhi_cntrl);
	int itr = DIV_ROUND_UP(mhi_cntrl->timeout_ms * 1000, 10);

	if (st != MHI_STATE_SYS_ERR)
		return 0;

	pr_info("[crux mhi-proxy] device in SYS_ERR -> MHI soft reset\n");
	mhi_set_mhi_state(mhi_cntrl, MHI_STATE_RESET);
	while (itr--) {
		st = mhi_get_mhi_state(mhi_cntrl);
		if (st != MHI_STATE_SYS_ERR) {
			/* 原厂：SSR 后 EE 无效，必须清零 */
			mhi_cntrl->ee = 0;
			pr_info("[crux mhi-proxy] soft reset done, state=%d ee=0\n", st);
			return 0;
		}
		usleep_range(10, 20);
	}
	pr_err("[crux mhi-proxy] still SYS_ERR after soft reset -> abort\n");
	return -EIO;
}

static int ipa_mhi_proxy_probe(struct mhi_device *mhi_dev,
			       const struct mhi_device_id *id)
{
	struct ipa_mhi_ch_init_info ch[2];
	int ret, i;

	if (!crux_proxy_enable) {
		dev_info(&mhi_dev->dev,
			 "[crux mhi-proxy] disabled (plain mhi_net/rmnet route) -> not binding\n");
		return -ENODEV;
	}

	/* audit: proxy must not come up before the module is powered */
	crux_dep_require("x50_power", CRUX_DEP_UP, "ipa_mhi_proxy_probe", false);

	/* [crux 2026-11-02] 先处理 SYS_ERR：软复位而不是 PCIe 硬复位 */
	if (crux_mhi_syserr_soft_reset(mhi_dev->mhi_cntrl)) {
		dev_err(&mhi_dev->dev, "SYS_ERR not recovered, aborting probe\n");
		return -EIO;
	}

	/* [crux 2026-10-19] EP 探活门控。
	 * 对应原厂 mhi_qcom_power_up() 开头先探设备状态再碰硬件；
	 * 这里只做一次 pci_read_config_word(PCI_VENDOR_ID) —— 纯读、安全。
	 * EP 已死（读失败 / 0xffff / 0x0000）时一个 BAR 都不碰，直接放弃，
	 * 避免总线错误把整机带下去。
	 */
	{
		struct device *d = mhi_dev->dev.parent;
		struct pci_dev *pdev = NULL;
		u16 vendor = 0, device = 0;
		int pret;

		while (d) {
			if (dev_is_pci(d)) {
				pdev = to_pci_dev(d);
				break;
			}
			d = d->parent;
		}

		if (!pdev) {
			dev_err(&mhi_dev->dev,
				"[crux mhi-proxy] no pci parent found, abort probe\n");
			return -ENODEV;
		}

		pret = pci_read_config_word(pdev, PCI_VENDOR_ID, &vendor);
		if (pret || vendor == 0 || vendor == (u16)~0) {
			dev_err(&mhi_dev->dev,
				"[crux mhi-proxy] EP dead: vendor=%#x ret=%d, abort probe (no BAR touched)\n",
				vendor, pret);
			return -ENODEV;
		}
		pci_read_config_word(pdev, PCI_DEVICE_ID, &device);
		dev_info(&mhi_dev->dev,
			 "[crux mhi-proxy] EP alive: %04x:%04x @ %s\n",
			 vendor, device, pci_name(pdev));
	}

	/* [crux 2026-10-19] 原厂 imp_mhi_probe_cb() 第一件事就是
	 * IPA_ACTIVE_CLIENTS_INC_SPECIAL("IMP")：给 IPA 投票开时钟，
	 * 之后才碰 GSI scratch 与 uC 共享内存。这里等价实现。
	 */
	ret = crux_ipa_pm_get();
	if (ret) {
		dev_err(&mhi_dev->dev,
			"[crux mhi-proxy] IPA pm get failed %d, abort probe\n", ret);
		return ret;
	}

	memset(ch, 0, sizeof(ch));
	ch[0].ch_id = ul_chan;
	ch[0].er_id = ul_er;
	ch[0].ch_doorbell_addr = db_addr(0);
	ch[0].er_doorbell_addr = db_addr(1);
	ch[0].direction = DMA_TO_DEVICE;
	ch[1].ch_id = dl_chan;
	ch[1].er_id = dl_er;
	ch[1].ch_doorbell_addr = db_addr(2);
	ch[1].er_doorbell_addr = db_addr(3);
	ch[1].direction = DMA_FROM_DEVICE;

	dev_info(&mhi_dev->dev,
		 "[crux mhi-proxy] probe: ul=%u(er%u) dl=%u(er%u) doorbells %#x/%#x/%#x/%#x\n",
		 ul_chan, ul_er, dl_chan, dl_er,
		 db_addr(0), db_addr(1), db_addr(2), db_addr(3));

	/* Retry until the IPA side can reach the modem's IPA service */
	for (i = 0; i < ARRAY_SIZE(ch); i++)
		mhi_proxy_ch[i] = ch[i];
	mhi_proxy_dev = mhi_dev;

	INIT_WORK(&mhi_proxy_uc_stage_wk, mhi_proxy_uc_stage_worker);

	/* [crux 2026-10-19] 原厂顺序：先把 4 个 doorbell 交给 IPA uC 登记成邮箱
	 * （ipa3_uc_send_remote_ipa_info），让 uC 做 local Q6 <-> remote Q6 的门铃代理，
	 * 之后才发 0x3c。WDI 的 mbox(16/18) 与此无关，这里用 n=0..3。
	 */
	if (auto_uc) {
		struct mhi_controller *cntrl = mhi_dev->mhi_cntrl;
		u32 db_addr[4];
		u64 ul_ch, ul_ev, dl_ch, dl_ev;
		int sret, si;

		/* [crux 2026-10-19] 分步打点：每步前后各打一行 + 100ms 间隔，
		 * 万一整机复位，tee 落盘的最后一行就是崩点。
		 */
		dev_info(&mhi_dev->dev, "[crux mhi-proxy] STEP1 gsi scratch begin\n");
		msleep(100);
		sret = crux_ipa_gsi_set_mhi_base_chan();
		dev_info(&mhi_dev->dev, "[crux mhi-proxy] STEP1 done ret=%d\n", sret);
		msleep(100);

		dev_info(&mhi_dev->dev, "[crux mhi-proxy] STEP2 doorbell pa begin\n");
		msleep(100);
		ul_ch = mhi_db_phys(cntrl, false, ul_chan);
		ul_ev = mhi_db_phys(cntrl, true, ul_er);
		dl_ch = mhi_db_phys(cntrl, false, dl_chan);
		dl_ev = mhi_db_phys(cntrl, true, dl_er);
		dev_info(&mhi_dev->dev,
			 "[crux mhi-proxy] STEP2 done ul_ch=%#llx ul_ev=%#llx dl_ch=%#llx dl_ev=%#llx\n",
			 ul_ch, ul_ev, dl_ch, dl_ev);
		msleep(100);

		if (!ul_ch || !dl_ch) {
			dev_warn(&mhi_dev->dev, "[crux mhi-proxy] doorbell pa探测失败，跳过 remote_ipa_info\n");
		} else {
			db_addr[0] = (u32)ul_ch; /* UL_CH */
			db_addr[1] = (u32)ul_ev; /* UL_EV */
			db_addr[2] = (u32)dl_ch; /* DL_CH */
			db_addr[3] = (u32)dl_ev; /* DL_EV */

			for (si = 0; si < 4; si++) {
				dev_info(&mhi_dev->dev,
					 "[crux mhi-proxy] STEP3.%d send_remote_ipa_info begin addr=%#x n=%d\n",
					 si, db_addr[si], si);
				msleep(100);
				crux_ipa_uc_send_remote_ipa_info(db_addr[si], (u32)si);
				dev_info(&mhi_dev->dev,
					 "[crux mhi-proxy] STEP3.%d done\n", si);
				msleep(100);
			}
		}
	}

	if (auto_uc) {
		/* [crux 2026-10-28] 异步执行（probe 里 sleep 会饿死 MHI 事件） */
		schedule_delayed_work(&mhi_proxy_hs_work, 0);
	} else {
		dev_info(&mhi_dev->dev, "[crux mhi-proxy] auto_uc=0: skip 0x0b/0x3c (insmod auto_uc=1 to enable)\n");
	}

	schedule_delayed_work(&mhi_proxy_rx_work, msecs_to_jiffies(500));

	/* [crux] 把 0x3d 的 CCA/ECA 下发能力注册给 IPA 的 QMI 服务 */
	ipa_qmi_register_mhi_alloc_handler(crux_apply_alloc);

	/* [crux 2026-10-19] 对应原厂 MHI 进 LPM 时的 DEC；这里 probe 结束即放 */
	crux_ipa_pm_put();

	return 0;
}

void ipa_mhi_proxy_retrigger(void)
{
	if (!mhi_proxy_dev)
		return;
	dev_info(&mhi_proxy_dev->dev, "[crux mhi-proxy] retrigger ready ind (modem QMI service arrived)\n");
	schedule_delayed_work(&mhi_proxy_work, 0);
}
EXPORT_SYMBOL_GPL(ipa_mhi_proxy_retrigger);

/* [crux 2026-10-19] 运行时分段触发 uC 路径，用于定位 auto_uc=1 的硬复位点。
 * uc_stage: 0=off 1=+gsi scratch 2=+doorbell pa 3=+remote_ipa_info(0x0b) 4=+0x3c
 * 每段自行 pm_get/pm_put，写完立刻打日志，崩了也能从最后一行定位。
 */
static void mhi_proxy_uc_stage_run(unsigned int stage)
{
	struct mhi_device *mhi_dev = mhi_proxy_dev;
	struct mhi_controller *cntrl;
	int ret;

	if (!mhi_dev) {
		pr_err("[crux mhi-proxy] uc_stage: no mhi device bound\n");
		return;
	}
	cntrl = mhi_dev->mhi_cntrl;

	dev_info(&mhi_dev->dev, "[crux mhi-proxy] uc_stage=%u begin\n", stage);

	ret = crux_ipa_pm_get();
	if (ret) {
		dev_err(&mhi_dev->dev, "[crux mhi-proxy] uc_stage=%u pm_get failed %d\n",
			stage, ret);
		return;
	}
	dev_info(&mhi_dev->dev, "[crux mhi-proxy] uc_stage=%u pm_get ok\n", stage);

	if (stage >= 1) {
		int sret = crux_ipa_gsi_set_mhi_base_chan();

		dev_info(&mhi_dev->dev,
			 "[crux mhi-proxy] uc_stage=%u gsi_mhi_base_chan ret=%d\n",
			 stage, sret);
	}

	if (stage >= 2) {
		u64 ul_ch = mhi_db_phys(cntrl, false, ul_chan);
		u64 ul_ev = mhi_db_phys(cntrl, true, ul_er);
		u64 dl_ch = mhi_db_phys(cntrl, false, dl_chan);
		u64 dl_ev = mhi_db_phys(cntrl, true, dl_er);

		dev_info(&mhi_dev->dev,
			 "[crux mhi-proxy] uc_stage=%u doorbell pa: %#llx/%#llx/%#llx/%#llx\n",
			 stage, ul_ch, ul_ev, dl_ch, dl_ev);
	}

	if (stage >= 3) {
		u64 ul_ch = mhi_db_phys(cntrl, false, ul_chan);
		u64 ul_ev = mhi_db_phys(cntrl, true, ul_er);
		u64 dl_ch = mhi_db_phys(cntrl, false, dl_chan);
		u64 dl_ev = mhi_db_phys(cntrl, true, dl_er);

		dev_info(&mhi_dev->dev, "[crux mhi-proxy] uc_stage=%u sending 0x0b (UL_CH)\n", stage);
		msleep(300);
		crux_ipa_uc_send_remote_ipa_info((u32)ul_ch, 0);
		dev_info(&mhi_dev->dev, "[crux mhi-proxy] uc_stage=%u 0x0b (UL_CH) returned\n", stage);
		msleep(300);
		crux_ipa_uc_send_remote_ipa_info((u32)ul_ev, 1);
		dev_info(&mhi_dev->dev, "[crux mhi-proxy] uc_stage=%u 0x0b (UL_EV) returned\n", stage);
		msleep(300);
		crux_ipa_uc_send_remote_ipa_info((u32)dl_ch, 2);
		dev_info(&mhi_dev->dev, "[crux mhi-proxy] uc_stage=%u 0x0b (DL_CH) returned\n", stage);
		msleep(300);
		crux_ipa_uc_send_remote_ipa_info((u32)dl_ev, 3);
		dev_info(&mhi_dev->dev, "[crux mhi-proxy] uc_stage=%u 0x0b (DL_EV) returned\n", stage);
		msleep(300);
	}

	if (stage >= 4) {
		ret = mhi_proxy_send_ready_ind(mhi_proxy_ch, ARRAY_SIZE(mhi_proxy_ch));
		dev_info(&mhi_dev->dev, "[crux mhi-proxy] uc_stage=%u 0x3c ret=%d\n",
			 stage, ret);
	}

	crux_ipa_pm_put();
	dev_info(&mhi_dev->dev, "[crux mhi-proxy] uc_stage=%u done\n", stage);
}

static unsigned int uc_stage_req;

static void mhi_proxy_uc_stage_worker(struct work_struct *work)
{
	mhi_proxy_uc_stage_run(uc_stage_req);
}


static int uc_stage_set(const char *val, const struct kernel_param *kp)
{
	unsigned int v = 0;
	int r = kstrtouint(val, 0, &v);

	if (r)
		return r;
	if (v > 4)
		return -EINVAL;
	*(unsigned int *)kp->arg = v;
	if (!mhi_proxy_dev)
		return -ENODEV;
	schedule_work(&mhi_proxy_uc_stage_wk);
	return 0;
}

static const struct kernel_param_ops uc_stage_ops = {
	.set = uc_stage_set,
	.get = param_get_uint,
};
module_param_cb(uc_stage, &uc_stage_ops, &uc_stage_req, 0200);
MODULE_PARM_DESC(uc_stage, "run uC stage at runtime: 1 gsi 2 doorbell 3 0x0b 4 0x3c");

static void mhi_proxy_retry(struct work_struct *work)
{
	static int tries;
	int ret;

	ret = mhi_proxy_send_ready_ind(mhi_proxy_ch, ARRAY_SIZE(mhi_proxy_ch));
	dev_info(&mhi_proxy_dev->dev,
		 "[crux mhi-proxy] retry#%d MHI_READY_IND(0x3c) ret=%d\n",++tries, ret);
	(void)tries;
}

/* The DL offload channel is LPM-notify capable, so the MHI core requires a
 * status_cb from the driver (mhi_probe() rejects the bind otherwise).
 */
static void ipa_mhi_proxy_status_cb(struct mhi_device *mhi_dev, enum mhi_callback cb)
{
	switch (cb) {
	case MHI_CB_FATAL_ERROR:
		dev_err(&mhi_dev->dev, "[crux mhi-proxy] fatal error\n");
		break;
	case MHI_CB_EE_MISSION_MODE:
		dev_info(&mhi_dev->dev, "[crux mhi-proxy] mission mode\n");
		break;
	case MHI_CB_PENDING_DATA:
		dev_info(&mhi_dev->dev, "[crux mhi-proxy] pending data\n");
		break;
	default:
		dev_info(&mhi_dev->dev, "[crux mhi-proxy] cb %d\n", cb);
		break;
	}
}

static void ipa_mhi_proxy_remove(struct mhi_device *mhi_dev)
{
	cancel_delayed_work_sync(&mhi_proxy_hs_work);
	cancel_delayed_work_sync(&mhi_proxy_work);
	cancel_delayed_work_sync(&mhi_proxy_rx_work);
	ipa_qmi_register_mhi_alloc_handler(NULL);
	if (mhi_proxy_sock) {
		sock_release(mhi_proxy_sock);
		mhi_proxy_sock = NULL;
	}
	dev_info(&mhi_dev->dev, "[crux mhi-proxy] remove\n");
}



static const struct mhi_device_id ipa_mhi_proxy_id_table[] = {
	{ .chan = "IP_HW_OFFLOAD_0" },
	{ }
};
static struct mhi_driver ipa_mhi_proxy_driver = {
	.id_table = ipa_mhi_proxy_id_table,
	.probe = ipa_mhi_proxy_probe,
	.remove = ipa_mhi_proxy_remove,
	.status_cb = ipa_mhi_proxy_status_cb,
	.driver = {
		.name = "ipa_mhi_proxy",
		.owner = THIS_MODULE,
	},
};
module_mhi_driver(ipa_mhi_proxy_driver);

MODULE_DESCRIPTION("IPA-MHI proxy (external modem to modem path)");
MODULE_LICENSE("GPL v2");

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

/* [crux 2026-10-19] 前向声明：probe 里要 INIT_WORK，定义在文件后部 */
static struct work_struct mhi_proxy_uc_stage_wk;
static void mhi_proxy_uc_stage_worker(struct work_struct *work);

static DECLARE_DELAYED_WORK(mhi_proxy_work, mhi_proxy_retry);

extern u32 crux_ipa_uc_mbox_addr(u32 m, u32 n);
extern int crux_ipa_uc_send_remote_ipa_info(u32 remote_addr, u32 mbox_n);
extern int crux_ipa_gsi_set_mhi_base_chan(void);
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

#define TR_ENT_SZ	39	/* ch_id(1) poll_cfg(2) brst(4) iova(8) len(8) rp(8) wp(8) */
#define ER_ENT_SZ	45	/* er_id(1) cycles(4) count(4) msi(4) iova(8) len(8) rp(8) wp(8) */
#define CHAN_CTX_BRSTMODE_MASK	GENMASK(9, 8)
#define CHAN_CTX_POLLCFG_MASK	GENMASK(15, 10)
#define EV_CTX_INTMODC_MASK	GENMASK(15, 8)
#define EV_CTX_INTMODT_MASK	GENMASK(31, 16)

static const struct crux_event_ctxt *crux_find_er(const u8 *er, u32 er_len, u8 er_id)
{
	static struct crux_event_ctxt out;
	int i, n;

	if (!er || er_len < 1)
		return NULL;
	n = er[0];
	if (1 + n * ER_ENT_SZ > er_len)
		return NULL;

	for (i = 0; i < n; i++) {
		const u8 *p = er + 1 + i * ER_ENT_SZ;

		if (p[0] != er_id)
			continue;
		memset(&out, 0, sizeof(out));
		out.intmod = cpu_to_le32(
			FIELD_PREP(EV_CTX_INTMODC_MASK, get_unaligned_le32(p + 5)) |
			FIELD_PREP(EV_CTX_INTMODT_MASK, get_unaligned_le32(p + 1)));
		out.ertype = cpu_to_le32(1);
		out.msivec = cpu_to_le32(get_unaligned_le32(p + 9));
		out.rbase = cpu_to_le64(get_unaligned_le64(p + 13));
		out.rlen = cpu_to_le64(get_unaligned_le64(p + 21));
		out.rp = cpu_to_le64(get_unaligned_le64(p + 29));
		out.wp = cpu_to_le64(get_unaligned_le64(p + 37));
		return &out;
	}
	return NULL;
}

static int crux_apply_alloc(const u8 *tr, u32 tr_len, const u8 *er, u32 er_len)
{
	int i, n, done = 0;

	if (!mhi_proxy_dev) {
		pr_err("[crux mhi-proxy] alloc: no mhi device yet\n");
		return -ENODEV;
	}
	if (!tr || tr_len < 1)
		return -EINVAL;
	n = tr[0];
	if (1 + n * TR_ENT_SZ > tr_len) {
		pr_err("[crux mhi-proxy] alloc: bad TR len %u (n=%d)\n", tr_len, n);
		return -EINVAL;
	}

	for (i = 0; i < n; i++) {
		const u8 *p = tr + 1 + i * TR_ENT_SZ;
		u8 ch_id = p[0];
		u16 poll = get_unaligned_le16(p + 1);
		u32 brst = get_unaligned_le32(p + 3);
		unsigned int er_id, dir;
		struct mhi_buf cfg[2];
		struct crux_chan_ctxt cc;
		const struct crux_event_ctxt *ec;
		int ret;

		if (ch_id == ul_chan) {
			er_id = ul_er;
			dir = DMA_TO_DEVICE;
		} else if (ch_id == dl_chan) {
			er_id = dl_er;
			dir = DMA_FROM_DEVICE;
		} else {
			pr_info("[crux mhi-proxy] alloc: skip unknown ch %u\n", ch_id);
			continue;
		}

		ec = crux_find_er(er, er_len, er_id);
		if (!ec) {
			pr_err("[crux mhi-proxy] alloc: no ER %u for ch %u\n", er_id, ch_id);
			return -EINVAL;
		}

		memset(&cc, 0, sizeof(cc));
		cc.chcfg = cpu_to_le32(
			FIELD_PREP(CHAN_CTX_BRSTMODE_MASK, brst == 1 ? 3 : (brst == 0 ? 2 : 0)) |
			FIELD_PREP(CHAN_CTX_POLLCFG_MASK, poll));
		cc.chtype = cpu_to_le32(dir);
		cc.erindex = cpu_to_le32(er_id);
		cc.rbase = cpu_to_le64(get_unaligned_le64(p + 7));
		cc.rlen = cpu_to_le64(get_unaligned_le64(p + 15));
		cc.rp = cpu_to_le64(get_unaligned_le64(p + 23));
		cc.wp = cpu_to_le64(get_unaligned_le64(p + 31));

		cfg[0].buf = (void *)&cc;
		cfg[0].len = sizeof(cc);
		cfg[0].name = "CCA";
		cfg[1].buf = (void *)ec;
		cfg[1].len = sizeof(*ec);
		cfg[1].name = "ECA";

		ret = mhi_device_configure(mhi_proxy_dev, dir, cfg, 2);
		pr_info("[crux mhi-proxy] alloc: ch%u dir%u er%u configure ret=%d\n",
			ch_id, dir, er_id, ret);
		if (ret)
			return ret;
		done++;
	}

	pr_info("[crux mhi-proxy] alloc: programmed %d channel(s)\n", done);
	return done ? 0 : -EINVAL;
}

static int ipa_mhi_proxy_probe(struct mhi_device *mhi_dev,
			       const struct mhi_device_id *id)
{
	struct ipa_mhi_ch_init_info ch[2];
	int ret, i;

	/* audit: proxy must not come up before the module is powered */
	crux_dep_require("x50_power", CRUX_DEP_UP, "ipa_mhi_proxy_probe", false);

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
		/* [crux 2026-10-19] 发 0x3c 之前重新握手一次：
		 * X50 重启后 QRTR 不会重复报 NEW_SERVER，旧 modem_client_sq 已失效。
		 * 重发 INIT_DRIVER 让 modem 重新登记 client sq，再发 0x3c。
		 */
		{
			extern int ipa_qmi_rehandshake(u32 node, u32 port);
			extern bool ipa_qmi_modem_client_ready(void);
			int rh, i;

			rh = ipa_qmi_rehandshake(0, 0);
			dev_info(&mhi_dev->dev,
				 "[crux mhi-proxy] rehandshake ret=%d, wait modem client\n",
				 rh);
			/* [crux 2026-10-19] 与基带配合：等基带真正登记 client 端点。
			 * 最多等 ~60s，期间内核自动重试 INIT_DRIVER。
			 */
			for (i = 0; i < 30; i++) {
				if (ipa_qmi_modem_client_ready())
					break;
				msleep(2000);
			}
			dev_info(&mhi_dev->dev,
				 "[crux mhi-proxy] modem client ready=%d after %ds\n",
				 ipa_qmi_modem_client_ready(), i * 2);
		}
		ret = mhi_proxy_send_ready_ind(ch, ARRAY_SIZE(ch));
		dev_info(&mhi_dev->dev, "[crux mhi-proxy] MHI_READY_IND(0x3c) ret=%d\n", ret);
		if (ret) /* [crux] 原厂语义：失败不重试，等 modem IPA QMI 服务到达时再触发 */
			dev_info(&mhi_dev->dev, "[crux mhi-proxy] ready ind failed, wait for modem QMI to come back\n");
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

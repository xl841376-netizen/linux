// SPDX-License-Identifier: GPL-2.0-only
/*
 * msm_npu 主线移植 compat 层
 *
 * 1) subsystem_get("npu")：执行验证过的 PAS 黄金序列（原 npu_boot24 那套），
 *    让驱动自己按正确顺序点火（先建 IPC 再加载固件）。
 * 2) 其余下游专有接口留空实现。
 *
 * 注意：固件区一旦分配就不释放（NPU 一直在跑，释放会被复用）。
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/elf.h>
#include <linux/dma-mapping.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/iommu.h>
#include <linux/firmware.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/soc/qcom/mdt_loader.h>
#include <linux/soc/qcom/qcom_aoss.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/clk.h>

#define NPU_GDSCR	0x9911028UL
#define NPUCC_BASE	0x9910000UL
#define NPU_MEM_SIZE	0x80000UL
#define NPU_FW		"qcom/sm8150/xiaomi/crux/npu.mbn"
#define VMID_HLOS_V	0x3

#define QCOM_MDT_TYPE_MASK	(7 << 24)
#define QCOM_MDT_TYPE_HASH	(2 << 24)
#define QCOM_MDT_RELOCATABLE	BIT(27)

static unsigned int do_aop = 1, do_clks = 1;
module_param(do_aop, uint, 0644);
module_param(do_clks, uint, 0644);

static void *npu_region_va;
static dma_addr_t npu_region_pa;
static u32 npu_pas_id = 23;
static int npu_booted;
static struct device *npu_owner_dev;
static struct iommu_domain *npu_dom;

static const unsigned int npu_cc_regs[] = {
	0x1058, 0x1090, 0x1088, 0x1068, 0x101c, 0x106c, 0x1074,
	0x1080, 0x1078, 0x1048, 0x107c, 0x1050, 0x105c, 0x108c,
	0x1060, 0x1064, 0x3020,
};

/* ----------------------------- AOP -------------------------------- */
static struct qmp *npu_get_qmp(struct device *dev)
{
	struct device_node *np;
	struct platform_device *qpdev;
	struct qmp *qmp;

	if (dev && dev->of_node && of_find_property(dev->of_node, "qcom,qmp", NULL)) {
		qmp = qmp_get(dev);
		if (!IS_ERR_OR_NULL(qmp))
			return qmp;
	}
	np = of_find_compatible_node(NULL, NULL, "qcom,sm8150-aoss-qmp");
	if (!np)
		np = of_find_compatible_node(NULL, NULL, "qcom,aoss-qmp");
	if (!np)
		return NULL;
	qpdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!qpdev)
		return NULL;
	qmp = platform_get_drvdata(qpdev);
	return qmp;
}

static int npu_notify_aop(struct device *dev, unsigned int val)
{
	struct qmp *qmp = npu_get_qmp(dev);
	int ret;

	if (!qmp) {
		pr_warn("msm_npu-pas: AOP qmp 不可用\n");
		return -ENODEV;
	}
	ret = qmp_send(qmp, "{class: bcm, res: npu_on, val:%u}", val);
	pr_info("msm_npu-pas: AOP qmp_send(npu_on=%u) = %d\n", val, ret);
	return ret;
}

/* ------------------------- 电源 / 时钟 ------------------------------ */
static int gdsc_on(void)
{
	void __iomem *m = ioremap(NPU_GDSCR, 4);
	u32 before, after;
	int i;

	if (!m)
		return -ENOMEM;
	before = readl(m);
	if (before & 1) {
		writel(before & ~1u, m);
		for (i = 0; i < 20; i++) {
			udelay(50);
			if (!(readl(m) & 1))
				break;
		}
	}
	after = readl(m);
	iounmap(m);
	pr_info("msm_npu-pas: GDSCR %08x -> %08x\n", before, after);
	return (after & 1) ? -EIO : 0;
}

static void npu_clks_on(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(npu_cc_regs); i++) {
		void __iomem *m = ioremap(NPUCC_BASE + npu_cc_regs[i], 4);

		if (!m)
			continue;
		writel(readl(m) | 1u, m);
		iounmap(m);
	}
	udelay(200);
}

static int npu_vote_pds(struct device *dev)
{
	static const char * const names[] = { "cx", "mx" };
	int i, n = 0;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		struct device *pd = dev_pm_domain_attach_by_name(dev, names[i]);

		if (IS_ERR_OR_NULL(pd))
			continue;
		dev_pm_genpd_set_performance_state(pd, INT_MAX);
		pm_runtime_get_sync(pd);
		n++;
	}
	pr_info("msm_npu-pas: pds voted = %d\n", n);
	return n;
}


/* 设置 NPU 源时钟频率（最高档：npu_core 400MHz / cal_dp 908MHz）
 * 时钟由 npucc-sm8150 驱动提供；msm_npu 驱动不再引用 clocks，避免影响 TZ 认证。
 */
static void npu_set_src_rates(void)
{
	static const struct { const char *name; unsigned long rate; } tbl[] = {
		{ "npu_cc_npu_core_clk_src", 400000000 },
		{ "npu_cc_cal_dp_clk_src", 908000000 },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(tbl); i++) {
		struct clk *c = clk_get(NULL, tbl[i].name);

		if (IS_ERR(c)) {
			pr_warn("msm_npu-pas: clk_get(%s) = %ld\n", tbl[i].name,
				PTR_ERR(c));
			continue;
		}
		pr_info("msm_npu-pas: %s 设频率 %lu -> %ld\n", tbl[i].name,
			tbl[i].rate, clk_set_rate(c, tbl[i].rate));
		clk_prepare_enable(c);
		clk_put(c);
	}
}


/* ---------- 直接编程 NPUCC 的 RCG（避开 clk 框架，避免影响 TZ 认证）----------
 * npu_core_clk_src @0x1030: 400MHz  <- P_NPU_CC_PLL1_OUT_EVEN(cfg1), prediv=3
 * cal_dp_clk_src   @0x1004: 908MHz  <- P_NPU_CC_CRC_DIV(cfg2),      prediv=1
 * RCG 布局（clk-rcg2）：cmd=base, cfg=base+4；cfg[2:0]=src, cfg[12:8]=pre_div
 */
#define NPUCFG_SRC_SHIFT 0
#define NPUCFG_DIV_SHIFT 8
#define NPUCFG_SRC_MASK  0x7
#define NPUCFG_DIV_MASK  0x1f

static unsigned int do_rates = 0;
module_param(do_rates, uint, 0644);

static void npu_rcg_program(unsigned int off, unsigned int src, unsigned int prediv)
{
	void __iomem *m = ioremap(NPUCC_BASE + off + 4, 4);
	u32 v;

	if (!m)
		return;
	v = readl(m);
	v &= ~((NPUCFG_SRC_MASK << NPUCFG_SRC_SHIFT) |
	       (NPUCFG_DIV_MASK << NPUCFG_DIV_SHIFT));
	v |= (src << NPUCFG_SRC_SHIFT) | (prediv << NPUCFG_DIV_SHIFT);
	writel(v, m);
	pr_info("msm_npu-pas: RCG 0x%x cfg=%08x (src=%u prediv=%u)\n", off, v, src, prediv);
	iounmap(m);
}

static void npu_set_rates_direct(void)
{
	if (!do_rates)
		return;
	npu_rcg_program(0x1030, 1, 3);   /* npu_core_clk_src -> 400MHz */
	npu_rcg_program(0x1004, 2, 1);   /* cal_dp_clk_src   -> 908MHz */
}

/* ---------------------- 固件布局解析 & 落地校验 ---------------------- */
struct seg_info {
	unsigned long paddr, filesz, memsz;
};

static bool phdr_ok(const Elf32_Phdr *p)
{
	if (p->p_type != PT_LOAD)
		return false;
	if ((p->p_flags & QCOM_MDT_TYPE_MASK) == QCOM_MDT_TYPE_HASH)
		return false;
	if (!p->p_memsz)
		return false;
	return true;
}

static int npu_parse_layout(const struct firmware *fw, struct seg_info *seg,
			    int max_seg, unsigned long *min_addr,
			    unsigned long *max_addr, bool *reloc)
{
	const Elf32_Ehdr *eh = (const Elf32_Ehdr *)fw->data;
	const Elf32_Phdr *ph = (const Elf32_Phdr *)(fw->data + eh->e_phoff);
	int i, n = 0;

	*min_addr = ~0UL;
	*max_addr = 0;
	*reloc = false;

	for (i = 0; i < eh->e_phnum && n < max_seg; i++) {
		if (!phdr_ok(&ph[i]))
			continue;
		if (ph[i].p_flags & QCOM_MDT_RELOCATABLE)
			*reloc = true;
		seg[n].paddr = ph[i].p_paddr;
		seg[n].filesz = ph[i].p_filesz;
		seg[n].memsz = ph[i].p_memsz;
		if (ph[i].p_paddr < *min_addr)
			*min_addr = ph[i].p_paddr;
		if (ph[i].p_paddr + ph[i].p_memsz > *max_addr)
			*max_addr = ph[i].p_paddr + ph[i].p_memsz;
		n++;
	}
	return n;
}

static void npu_verify_region(const struct firmware *fw, void *va,
			      const struct seg_info *seg, int n,
			      unsigned long min_addr, bool reloc)
{
	const Elf32_Ehdr *eh = (const Elf32_Ehdr *)fw->data;
	const Elf32_Phdr *ph = (const Elf32_Phdr *)(fw->data + eh->e_phoff);
	int i, k = 0, bad = 0;

	for (i = 0; i < eh->e_phnum && k < n; i++) {
		unsigned long off;
		const void *src;
		void *dst;

		if (!phdr_ok(&ph[i]))
			continue;
		src = fw->data + ph[i].p_offset;
		off = reloc ? (seg[k].paddr - min_addr) : seg[k].paddr;
		dst = (char *)va + off;
		if (memcmp(dst, src, seg[k].filesz))
			bad++;
		k++;
	}
	pr_info("msm_npu-pas: 固件落地校验 %d 段 / %d 段不匹配\n", n, bad);
}

/* --------------------------- 主流程 -------------------------------- */
static struct device *npu_find_boot_dev(void)
{
	struct device_node *np;
	struct platform_device *pdev;
	struct device *dev = NULL;

	np = of_find_compatible_node(NULL, NULL, "qcom,npu-boot");
	if (!np)
		np = of_find_compatible_node(NULL, NULL, "qcom,msm-npu");
	if (!np)
		return NULL;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (pdev)
		dev = &pdev->dev;
	return dev;
}

static int npu_pas_boot(void)
{
	struct device *dev = npu_owner_dev;
	struct iommu_domain *dom;
	const struct firmware *fw;
	struct seg_info seg[8];
	unsigned long min_addr = 0, max_addr = 0;
	dma_addr_t phys = 0;
	void *va = NULL, *md;
	size_t md_len = 0, setup_size;
	phys_addr_t reloc = 0;
	const char *fwname = "npu";
	bool relocatable = false;
	int ret, nseg;

	if (npu_booted) {
		pr_info("msm_npu-pas: NPU 已点火，跳过\n");
		return 0;
	}
	if (!dev) {
		pr_err("msm_npu-pas: 找不到 NPU 设备节点\n");
		return -ENODEV;
	}

	of_property_read_u32(dev->of_node, "qcom,pas-id", &npu_pas_id);
	of_property_read_string(dev->of_node, "qcom,firmware-name", &fwname);
	pr_info("msm_npu-pas: 开始点火 pas_id=%u\n", npu_pas_id);

	if (do_aop) {
		npu_notify_aop(dev, 1);
		msleep(20);
	}
	npu_vote_pds(dev);
	gdsc_on();
	npu_set_src_rates();
	if (do_clks)
		npu_clks_on();

	of_reserved_mem_device_init(dev);

	if (npu_region_va) {
		va = npu_region_va;
		phys = npu_region_pa;
		pr_info("msm_npu-pas: 复用已分配固件区\n");
	} else {
		va = dma_alloc_coherent(dev, NPU_MEM_SIZE, &phys, GFP_KERNEL);
		if (!va) {
			pr_err("msm_npu-pas: dma_alloc_coherent 失败\n");
			return -ENOMEM;
		}
	}
	memset(va, 0, NPU_MEM_SIZE);
	pr_info("msm_npu-pas: 固件区 phys=%pa va=%p\n", &phys, va);

	if (!npu_dom) {
		dom = iommu_paging_domain_alloc(dev);
		if (dom) {
			ret = iommu_attach_device(dom, dev);
			if (!ret)
				iommu_map(dom, phys, phys, NPU_MEM_SIZE,
					  IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
			npu_dom = dom;
		}
	}

	ret = request_firmware(&fw, NPU_FW, dev);
	if (ret) {
		pr_err("msm_npu-pas: request_firmware = %d\n", ret);
		return ret;
	}
	nseg = npu_parse_layout(fw, seg, ARRAY_SIZE(seg), &min_addr, &max_addr,
				&relocatable);
	setup_size = max_addr - min_addr;
	pr_info("msm_npu-pas: 布局 segs=%d reloc=%d span=0x%lx\n",
		nseg, relocatable, (unsigned long)setup_size);

	md = qcom_mdt_read_metadata(fw, &md_len, fwname, dev);
	if (IS_ERR(md)) {
		release_firmware(fw);
		return PTR_ERR(md);
	}
	ret = qcom_scm_pas_init_image(npu_pas_id, md, md_len, NULL);
	kfree(md);
	if (ret)
		goto out;
	ret = qcom_scm_pas_mem_setup(npu_pas_id, phys, setup_size);
	if (ret)
		goto out;
	ret = qcom_mdt_load_no_init(dev, fw, fwname, va, phys, NPU_MEM_SIZE,
				    &reloc);
	if (ret)
		goto out;
	npu_verify_region(fw, va, seg, nseg, min_addr, relocatable);
	ret = qcom_scm_pas_auth_and_reset(npu_pas_id);
	pr_info("msm_npu-pas: auth_and_reset = %d\n", ret);
out:
	release_firmware(fw);
	if (ret) {
		pr_err("msm_npu-pas: 点火失败 %d\n", ret);
		dma_free_coherent(dev, NPU_MEM_SIZE, va, phys);
		return ret;
	}
	if (!npu_region_va) {
		npu_region_va = va;
		npu_region_pa = phys;
	}
	npu_booted = 1;
	pr_info("msm_npu-pas: *** NPU BOOTED ***\n");
	return 0;
}

/* --------------------------- 对外接口 ------------------------------ */
static int npu_subsys_dummy;

void *subsystem_get(const char *name)
{
	if (!name || strcmp(name, "npu"))
		return ERR_PTR(-ENODEV);
	if (!npu_owner_dev)
		npu_owner_dev = npu_find_boot_dev();
	if (npu_pas_boot())
		return ERR_PTR(-EIO);
	return &npu_subsys_dummy;
}
EXPORT_SYMBOL_GPL(subsystem_get);

void subsystem_put(void *subsystem)
{
	int ret;

	if (!npu_booted)
		return;
	/* 像安卓那样：最后一个用户关闭就停掉子系统，下次 open 重新完整加载 */
	ret = qcom_scm_pas_shutdown(npu_pas_id);
	pr_info("msm_npu-pas: pas_shutdown(%u) = %d\n", npu_pas_id, ret);
	npu_booted = 0;
}
EXPORT_SYMBOL_GPL(subsystem_put);

int subsystem_restart(const char *name) { return 0; }
EXPORT_SYMBOL_GPL(subsystem_restart);

/* 供模块退出时释放固件区（reserved-memory pool 会被占满） */
void npu_compat_free_region(void)
{
	if (!npu_region_va)
		return;
	if (npu_booted) {
		qcom_scm_pas_shutdown(npu_pas_id);
		npu_booted = 0;
	}
	dma_free_coherent(npu_owner_dev, NPU_MEM_SIZE, npu_region_va,
			  npu_region_pa);
	npu_region_va = NULL;
	npu_region_pa = 0;
	pr_info("msm_npu-pas: 固件区已释放\n");
}
EXPORT_SYMBOL_GPL(npu_compat_free_region);

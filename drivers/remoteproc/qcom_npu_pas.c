// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm SM8150 NPU (HTA) PAS remoteproc driver
 *
 * 把 msm_npu 的私有 PAS 点火序列（GDSC/时钟 -> IOMMU -> dma 固件区 ->
 * init_image -> mem_setup -> load -> auth_and_reset）装进标准 remoteproc
 * 生命周期，使 NPU 出现在 /sys/class/remoteproc/ 并与 cdsp fastrpc 并存。
 *
 * 当前为最小可用版本（已上机验证）：auto_boot=false，手动 start/stop。
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/elf.h>
#include <linux/firmware.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/remoteproc.h>
#include <linux/soc/qcom/mdt_loader.h>

#define NPU_GDSCR		0x9911028UL
#define NPUCC_BASE		0x9910000UL
#define NPU_CORE_BASE		0x9900000UL
#define NPU_CORE_SIZE		0x10000UL
#define NPU_FW_SIZE		0x80000UL
#define NPU_FW_NAME		"qcom/sm8150/xiaomi/crux/npu.mbn"

#define QCOM_MDT_TYPE_MASK	(7 << 24)
#define QCOM_MDT_TYPE_HASH	(2 << 24)
#define QCOM_MDT_RELOCATABLE	BIT(27)

struct seg_info {
	unsigned long paddr, filesz, memsz;
};

struct npu_pas {
	struct rproc *rproc;
	struct device *dev;
	u32 pas_id;
	const char *fw_name;
	void __iomem *core;
	void __iomem *gdscr;
	void __iomem *npucc;
	struct device *pd_cx;
	struct device *pd_mx;
	void *region_va;
	dma_addr_t region_pa;
	struct iommu_domain *dom;
	bool iommu_attached;
	bool booted;
};

static struct rproc *npu_rproc;

static const unsigned int npu_cc_regs[] = {
	0x1058, 0x1090, 0x1088, 0x1068, 0x101c, 0x106c, 0x1074,
	0x1080, 0x1078, 0x1048, 0x107c, 0x1050, 0x105c, 0x108c,
	0x1060, 0x1064, 0x3020,
};

static int npu_gdsc_on(struct npu_pas *npu)
{
	u32 before, after;
	int i;

	before = readl(npu->gdscr);
	if (before & 1) {
		writel(before & ~1u, npu->gdscr);
		for (i = 0; i < 20; i++) {
			udelay(50);
			if (!(readl(npu->gdscr) & 1))
				break;
		}
	}
	after = readl(npu->gdscr);
	dev_info(npu->dev, "npu-pas: GDSCR %08x -> %08x\n", before, after);
	return (after & 1) ? -EIO : 0;
}

static void npu_clks_on(struct npu_pas *npu)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(npu_cc_regs); i++)
		writel(readl(npu->npucc + npu_cc_regs[i]) | 1u,
		       npu->npucc + npu_cc_regs[i]);
	udelay(200);
}

static int npu_vote_pds(struct npu_pas *npu)
{
	int n = 0;

	if (npu->pd_cx) {
		dev_pm_genpd_set_performance_state(npu->pd_cx, INT_MAX);
		pm_runtime_get_sync(npu->pd_cx);
		n++;
	}
	if (npu->pd_mx) {
		dev_pm_genpd_set_performance_state(npu->pd_mx, INT_MAX);
		pm_runtime_get_sync(npu->pd_mx);
		n++;
	}
	dev_info(npu->dev, "npu-pas: pds voted = %d\n", n);
	return n;
}

static void npu_unvote_pds(struct npu_pas *npu)
{
	if (npu->pd_mx) {
		pm_runtime_put_sync(npu->pd_mx);
		dev_pm_genpd_set_performance_state(npu->pd_mx, 0);
	}
	if (npu->pd_cx) {
		pm_runtime_put_sync(npu->pd_cx);
		dev_pm_genpd_set_performance_state(npu->pd_cx, 0);
	}
}

static void npu_set_src_rates(void)
{
	static const struct {
		const char *name;
		unsigned long rate;
	} tbl[] = {
		{ "npu_cc_npu_core_clk_src", 400000000 },
		{ "npu_cc_cal_dp_clk_src", 908000000 },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(tbl); i++) {
		struct clk *c = clk_get(NULL, tbl[i].name);

		if (IS_ERR(c)) {
			pr_warn("npu-pas: clk_get(%s) = %ld\n", tbl[i].name,
				PTR_ERR(c));
			continue;
		}
		clk_set_rate(c, tbl[i].rate);
		clk_prepare_enable(c);
		clk_put(c);
	}
}

static int npu_parse_layout(const struct firmware *fw, struct seg_info *segs,
			    int max_segs, unsigned long *min_addr,
			    unsigned long *max_addr, bool *relocatable)
{
	const struct elf32_hdr *ehdr = (const struct elf32_hdr *)fw->data;
	const struct elf32_phdr *phdr;
	int i, n = 0;

	*min_addr = ULONG_MAX;
	*max_addr = 0;
	*relocatable = false;

	if (fw->size < sizeof(*ehdr))
		return -EINVAL;
	if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG))
		return -EINVAL;
	if (ehdr->e_ident[EI_CLASS] != ELFCLASS32)
		return -EINVAL;

	phdr = (const struct elf32_phdr *)(fw->data + ehdr->e_phoff);
	for (i = 0; i < ehdr->e_phnum; i++) {
		if (phdr[i].p_type != PT_LOAD)
			continue;
		if ((phdr[i].p_flags & QCOM_MDT_TYPE_MASK) == QCOM_MDT_TYPE_HASH)
			continue;
		if (!phdr[i].p_memsz)
			continue;
		if (n >= max_segs)
			return -E2BIG;
		if (phdr[i].p_flags & QCOM_MDT_RELOCATABLE)
			*relocatable = true;
		segs[n].paddr = phdr[i].p_paddr;
		segs[n].filesz = phdr[i].p_filesz;
		segs[n].memsz = phdr[i].p_memsz;
		if (phdr[i].p_paddr < *min_addr)
			*min_addr = phdr[i].p_paddr;
		if (phdr[i].p_paddr + phdr[i].p_memsz > *max_addr)
			*max_addr = phdr[i].p_paddr + phdr[i].p_memsz;
		n++;
	}
	if (n == 0)
		return -EINVAL;
	return n;
}

static int npu_pas_boot(struct npu_pas *npu)
{
	struct seg_info seg[8];
	const struct firmware *fw;
	unsigned long min_addr = 0, max_addr = 0;
	dma_addr_t phys = 0;
	void *va, *md;
	size_t md_len = 0, setup_size;
	phys_addr_t reloc = 0;
	bool relocatable = false;
	int ret, nseg;

	if (npu->booted)
		return 0;

	ret = npu_gdsc_on(npu);
	if (ret)
		return ret;
	npu_vote_pds(npu);
	npu_clks_on(npu);
	npu_set_src_rates();

	of_reserved_mem_device_init(npu->dev);

	va = dma_alloc_coherent(npu->dev, NPU_FW_SIZE, &phys, GFP_KERNEL);
	if (!va) {
		dev_err(npu->dev, "npu-pas: dma_alloc_coherent failed\n");
		return -ENOMEM;
	}

	npu->dom = iommu_paging_domain_alloc(npu->dev);
	if (npu->dom) {
		ret = iommu_attach_device(npu->dom, npu->dev);
		if (!ret) {
			npu->iommu_attached = true;
			iommu_map(npu->dom, phys, phys, NPU_FW_SIZE,
				  IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
		}
	}

	ret = request_firmware(&fw, npu->fw_name, npu->dev);
	if (ret) {
		dev_err(npu->dev, "npu-pas: request_firmware(%s) = %d\n",
			npu->fw_name, ret);
		goto err_free;
	}

	nseg = npu_parse_layout(fw, seg, ARRAY_SIZE(seg), &min_addr, &max_addr,
				&relocatable);
	if (nseg < 0) {
		ret = nseg;
		goto err_fw;
	}
	setup_size = max_addr - min_addr;
	dev_info(npu->dev, "npu-pas: segs=%d reloc=%d span=0x%lx\n",
		 nseg, relocatable, (unsigned long)setup_size);

	md = qcom_mdt_read_metadata(fw, &md_len, npu->fw_name, npu->dev);
	if (IS_ERR(md)) {
		ret = PTR_ERR(md);
		goto err_fw;
	}
	ret = qcom_scm_pas_init_image(npu->pas_id, md, md_len, NULL);
	kfree(md);
	if (ret)
		goto err_fw;

	ret = qcom_scm_pas_mem_setup(npu->pas_id, phys, setup_size);
	if (ret)
		goto err_fw;

	memset(va, 0, NPU_FW_SIZE);
	ret = qcom_mdt_load_no_init(npu->dev, fw, npu->fw_name, va, phys,
				    NPU_FW_SIZE, &reloc);
	if (ret)
		goto err_fw;

	ret = qcom_scm_pas_auth_and_reset(npu->pas_id);
	dev_info(npu->dev, "npu-pas: auth_and_reset = %d\n", ret);
	if (ret)
		goto err_fw;

	release_firmware(fw);
	npu->region_va = va;
	npu->region_pa = phys;
	npu->booted = true;
	dev_info(npu->dev, "npu-pas: NPU BOOTED\n");
	return 0;

err_fw:
	release_firmware(fw);
err_free:
	dma_free_coherent(npu->dev, NPU_FW_SIZE, va, phys);
	return ret;
}

static void npu_pas_shutdown(struct npu_pas *npu)
{
	if (!npu->booted)
		return;

	qcom_scm_pas_shutdown(npu->pas_id);
	if (npu->dom && npu->iommu_attached && npu->region_va) {
		iommu_unmap(npu->dom, npu->region_pa, NPU_FW_SIZE);
		iommu_detach_device(npu->dom, npu->dev);
		npu->iommu_attached = false;
	}
	if (npu->region_va) {
		dma_free_coherent(npu->dev, NPU_FW_SIZE, npu->region_va,
				  npu->region_pa);
		npu->region_va = NULL;
		npu->region_pa = 0;
	}
	npu->booted = false;
	npu_unvote_pds(npu);
}

static int npu_rproc_start(struct rproc *rproc)
{
	struct npu_pas *npu = rproc->priv;

	return npu_pas_boot(npu);
}

static int npu_rproc_stop(struct rproc *rproc)
{
	struct npu_pas *npu = rproc->priv;

	npu_pas_shutdown(npu);
	if (npu->gdscr)
		writel(readl(npu->gdscr) | 1u, npu->gdscr);
	return 0;
}

static int npu_rproc_load(struct rproc *rproc, const struct firmware *fw)
{
	/* 固件由 start 里的 qcom_mdt_load_no_init 落地 */
	return 0;
}

static int npu_rproc_sanity_check(struct rproc *rproc,
				  const struct firmware *fw)
{
	return 0;
}

static void *npu_rproc_da_to_va(struct rproc *rproc, u64 da, size_t len,
				bool *is_iomem)
{
	struct npu_pas *npu = rproc->priv;

	if (npu->region_va && da >= npu->region_pa &&
	    da + len <= npu->region_pa + NPU_FW_SIZE) {
		if (is_iomem)
			*is_iomem = false;
		return npu->region_va + (da - npu->region_pa);
	}
	return NULL;
}

static const struct rproc_ops npu_rproc_ops = {
	.start = npu_rproc_start,
	.stop = npu_rproc_stop,
	.load = npu_rproc_load,
	.sanity_check = npu_rproc_sanity_check,
	.da_to_va = npu_rproc_da_to_va,
};

static int npu_pas_probe(struct platform_device *pdev)
{
	struct npu_pas *npu;
	struct rproc *rproc;
	int ret;

	npu = devm_kzalloc(&pdev->dev, sizeof(*npu), GFP_KERNEL);
	if (!npu)
		return -ENOMEM;
	npu->dev = &pdev->dev;
	npu->fw_name = NPU_FW_NAME;

	if (of_property_read_u32(pdev->dev.of_node, "qcom,pas-id",
				 &npu->pas_id))
		npu->pas_id = 23;

	npu->pd_cx = dev_pm_domain_attach_by_name(&pdev->dev, "cx");
	if (IS_ERR(npu->pd_cx))
		npu->pd_cx = NULL;
	npu->pd_mx = dev_pm_domain_attach_by_name(&pdev->dev, "mx");
	if (IS_ERR(npu->pd_mx))
		npu->pd_mx = NULL;

	npu->core = devm_ioremap(&pdev->dev, NPU_CORE_BASE, NPU_CORE_SIZE);
	npu->gdscr = devm_ioremap(&pdev->dev, NPU_GDSCR, 4);
	npu->npucc = devm_ioremap(&pdev->dev, NPUCC_BASE, 0x4000);
	if (!npu->core || !npu->gdscr || !npu->npucc)
		return -ENOMEM;

	rproc = rproc_alloc(&pdev->dev, "npu", &npu_rproc_ops, npu->fw_name,
			    sizeof(*npu));
	if (!rproc)
		return -ENOMEM;
	npu->rproc = rproc;
	rproc->priv = npu;
	rproc->auto_boot = false;
	rproc->has_iommu = false;

	ret = rproc_add(rproc);
	if (ret) {
		rproc_free(rproc);
		return ret;
	}

	platform_set_drvdata(pdev, npu);
	npu_rproc = rproc;
	dev_info(&pdev->dev, "npu-pas: rproc registered (pas_id=%u fw=%s)\n",
		 npu->pas_id, npu->fw_name);
	return 0;
}

static void npu_pas_remove(struct platform_device *pdev)
{
	struct npu_pas *npu = platform_get_drvdata(pdev);

	if (!npu)
		return;
	npu_pas_shutdown(npu);
	if (npu->pd_mx)
		dev_pm_domain_detach(npu->pd_mx, false);
	if (npu->pd_cx)
		dev_pm_domain_detach(npu->pd_cx, false);
	npu_rproc = NULL;
	rproc_del(npu->rproc);
	rproc_free(npu->rproc);
}

struct rproc *qcom_npu_get_rproc(void)
{
	return npu_rproc;
}
EXPORT_SYMBOL_GPL(qcom_npu_get_rproc);

static const struct of_device_id npu_pas_match[] = {
	{ .compatible = "qcom,npu-boot" },
	{ .compatible = "qcom,sm8150-npu-pas" },
	{ }
};
MODULE_DEVICE_TABLE(of, npu_pas_match);

static struct platform_driver npu_pas_driver = {
	.probe = npu_pas_probe,
	.remove = npu_pas_remove,
	.driver = {
		.name = "qcom-npu-pas",
		.of_match_table = npu_pas_match,
	},
};
module_platform_driver(npu_pas_driver);

MODULE_DESCRIPTION("Qualcomm SM8150 NPU PAS remoteproc driver");
MODULE_LICENSE("GPL v2");

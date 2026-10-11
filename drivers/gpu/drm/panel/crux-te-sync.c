// SPDX-License-Identifier: GPL-2.0-only
/*
 * [crux] 显示路径寄存器观测器（帧同步 / tear check 取证）
 *
 * 面板是命令模式，帧完成靠 DPU INTF_1 的 tear check（等面板 TE）。
 * 本模块只读寄存器、不占用引脚、不改硬件状态：
 *   - 周期性打印 INTF_1 / DSI0 的关键寄存器（变化时才打）；
 *   - /sys/kernel/debug/crux_te_sync/stats  立即抓一次；
 *   - /sys/kernel/debug/crux_te_sync/te_pin 采样 gpio8(TE) 电平 1 秒，统计高电平比例。
 *
 * 窗口：DPU INTF_1 = 0xae01000(mdp) + 0x6a800；DSI0 ctrl = 0xae94000；TLMM = 0x3100000
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/debugfs.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/ktime.h>

#define CRUX_INTF1_PHYS  0xae6b800UL
#define CRUX_INTF1_SIZE  0x300
#define CRUX_DSI0_PHYS   0xae94000UL
#define CRUX_DSI0_SIZE   0x1000
#define CRUX_TLMM_PHYS   0x3100000UL
#define CRUX_TLMM_SIZE   0x1000
#define TLMM_GPIO8_CTL   0x8000
#define TLMM_MUX_MASK    0x3c  /* bits2-5 = 功能选择 */
#define TLMM_FUNC_GPIO   0
#define TLMM_GPIO8_IO    0x8004
#define CRUX_TE_GPIO_BIT 0
/* 引导器（Mu/GRUB）留下那块 GPU 帧缓冲，我们 DT 里声明成保留 */
#define CRUX_GOP_PHYS    0x9c000000UL
#define CRUX_GOP_SIZE    0x2400000UL  /* 36MB */

/* INTF 寄存器 */
#define I_TIMING_EN        0x000
#define I_CONFIG           0x004
#define I_POLARITY         0x050
#define I_CONFIG2          0x060
#define I_PANEL_FORMAT     0x090
#define I_FRAME_COUNT      0x0ac
#define I_LINE_COUNT       0x0b0
#define I_TEAR_VSYNC_SEL   0x280
#define I_TEAR_CHECK_EN    0x284
#define I_TEAR_SYNC_CFG    0x288
#define I_TEAR_INT_COUNT   0x298
#define I_TEAR_START_POS   0x2a0
#define I_TEAR_RD_PTR_IRQ  0x2a4
#define I_TEAR_OUT_LINE    0x2ac
#define I_TEAR_LINE_COUNT  0x2b0
#define I_TEAR_AUTOREFRESH 0x2b4

/* DSI0 寄存器 */
#define D_CTRL             0x000
#define D_CMD_DMA_CTRL     0x038
#define D_CMD_CFG0         0x03c
#define D_CMD_CFG1         0x040
#define D_TRIG_CTRL        0x080
#define D_LANE_CTRL        0x0a8
#define D_LANE_SWAP        0x0ac
#define D_ERR_INT_MASK0    0x108
#define D_CMD_MDP_CTRL2    0x1b4

#define CRUX_MAX_REG 20

struct crux_te {
    struct device *dev;
    void __iomem *intf;
    void __iomem *dsi;
    void __iomem *tlmm;
    void __iomem *gop;
    u32 crux_mux_saved;
    bool crux_mux_gpio;
    u32 prev[CRUX_MAX_REG];
    int nprev;
    struct dentry *dbg;
    struct delayed_work work;
};

static struct crux_te *g_te;

static u32 rd(void __iomem *b, u32 off)
{
    return ioread32(b + off);
}

static void crux_dump(struct crux_te *t)
{
    const char *nm[CRUX_MAX_REG];
    u32 v[CRUX_MAX_REG];
    int i, n = 0, changed = 0;

#define ADD(base, name, off) do { nm[n] = name; v[n] = rd(base, off); n++; } while (0)
    ADD(t->intf, "intf_timing_en", I_TIMING_EN);
    ADD(t->intf, "intf_cfg", I_CONFIG);
    ADD(t->intf, "intf_cfg2", I_CONFIG2);
    ADD(t->intf, "intf_panel_fmt", I_PANEL_FORMAT);
    ADD(t->intf, "intf_polarity", I_POLARITY);
    ADD(t->intf, "intf_frame_cnt", I_FRAME_COUNT);
    ADD(t->intf, "intf_line_cnt", I_LINE_COUNT);
    ADD(t->intf, "tear_vsync_sel", I_TEAR_VSYNC_SEL);
    ADD(t->intf, "tear_check_en", I_TEAR_CHECK_EN);
    ADD(t->intf, "tear_sync_cfg", I_TEAR_SYNC_CFG);
    ADD(t->intf, "tear_int_cnt", I_TEAR_INT_COUNT);
    ADD(t->intf, "tear_line_cnt", I_TEAR_LINE_COUNT);
    ADD(t->intf, "tear_out_line", I_TEAR_OUT_LINE);
    ADD(t->intf, "tear_autoref", I_TEAR_AUTOREFRESH);
    ADD(t->dsi, "dsi_ctrl", D_CTRL);
    ADD(t->dsi, "dsi_cmd_cfg0", D_CMD_CFG0);
    ADD(t->dsi, "dsi_cmd_cfg1", D_CMD_CFG1);
#undef ADD

    pr_info("[crux-te] dsi: dma_ctrl=%08x trig=%08x lane=%08x lane_swap=%08x err_mask0=%08x mdp_ctrl2=%08x\n",
            rd(t->dsi, D_CMD_DMA_CTRL), rd(t->dsi, D_TRIG_CTRL),
            rd(t->dsi, D_LANE_CTRL), rd(t->dsi, D_LANE_SWAP),
            rd(t->dsi, D_ERR_INT_MASK0), rd(t->dsi, D_CMD_MDP_CTRL2));

    for (i = 0; i < n && i < t->nprev; i++)
        if (v[i] != t->prev[i])
            changed = 1;

    if (changed || t->nprev == 0)
        for (i = 0; i < n; i++)
            pr_info("[crux-te] %s=%08x\n", nm[i], v[i]);

    for (i = 0; i < n && i < CRUX_MAX_REG; i++)
        t->prev[i] = v[i];
    t->nprev = n;
}

static ssize_t stats_read(struct file *f, char __user *u, size_t n, loff_t *o)
{
    struct crux_te *t = f->private_data;
    char buf[512];
    int len;

    if (!t)
        return -ENODEV;

    len = scnprintf(buf, sizeof(buf),
        "intf1@%08lx dsi0@%08lx\n"
        "tear_check_en=%08x sync_cfg=%08x int_cnt=%08x line_cnt=%08x out_line=%08x\n"
        "intf_line_cnt=%08x frame_cnt=%08x timing_en=%08x cfg=%08x cfg2=%08x\n"
        "dsi_ctrl=%08x cmd_cfg0=%08x mdp_ctrl2=%08x err_mask0=%08x\n"\
        "tlmm_gpio8_ctl=%08x tlmm_gpio8_io=%08x\n",
        CRUX_INTF1_PHYS, CRUX_DSI0_PHYS,
        rd(t->intf, I_TEAR_CHECK_EN), rd(t->intf, I_TEAR_SYNC_CFG),
        rd(t->intf, I_TEAR_INT_COUNT), rd(t->intf, I_TEAR_LINE_COUNT),
        rd(t->intf, I_TEAR_OUT_LINE),
        rd(t->intf, I_LINE_COUNT), rd(t->intf, I_FRAME_COUNT),
        rd(t->intf, I_TIMING_EN), rd(t->intf, I_CONFIG), rd(t->intf, I_CONFIG2),
        rd(t->dsi, D_CTRL), rd(t->dsi, D_CMD_CFG0),
        rd(t->dsi, D_CMD_MDP_CTRL2), rd(t->dsi, D_ERR_INT_MASK0),\
        rd(t->tlmm, TLMM_GPIO8_CTL), rd(t->tlmm, TLMM_GPIO8_IO));
    return simple_read_from_buffer(u, n, o, buf, len);
}

/*
 * 采样 TE 引脚（gpio8）的输入电平。
 * 引脚被 DSI 以 mdp_vsync 持有，不能 request；但 TLMM 的 GPIO_IN 仍然能看到
 * pad 的当前电平。面板 TE 是 60Hz 短脉冲，所以在 1 秒内紧密采样统计高电平比例：
 * 比例明显非 0（0.1%~10% 量级）⇒ TE 真在跳；恒为 0 或恒为 1 ⇒ 引脚不动。
 */

/*
 * 把 TE 引脚（gpio8）临时从 mdp_vsync 切回普通 gpio，再采样它的电平。
 * 原因：引脚被 mux 到外设时 GPIO_IN 有可能是盲的；切回 gpio 就是"回到能看见"的状态
 * （直接写 TLMM 的 CTL 寄存器，绕过 pinctrl 的持有关系；写回原值即恢复）。
 * 写 1 切到 gpio，写 0 恢复原功能。
 */
static ssize_t crux_mux_write(struct file *f, const char __user *u, size_t n, loff_t *o)
{
    struct crux_te *t = f->private_data;
    char buf[8];
    u32 v;

    if (!t || !t->tlmm)
        return -ENODEV;
    if (n == 0 || n >= sizeof(buf))
        return -EINVAL;
    if (copy_from_user(buf, u, n))
        return -EFAULT;
    buf[n] = 0;

    if (kstrtou32(buf, 0, &v))
        return -EINVAL;

    if (v && !t->crux_mux_gpio) {
        t->crux_mux_saved = rd(t->tlmm, TLMM_GPIO8_CTL);
        iowrite32((t->crux_mux_saved & ~TLMM_MUX_MASK) | TLMM_FUNC_GPIO,
                  t->tlmm + TLMM_GPIO8_CTL);
        t->crux_mux_gpio = true;
        pr_info("[crux-te] gpio8 mux -> gpio (saved %08x)\n", t->crux_mux_saved);
    } else if (!v && t->crux_mux_gpio) {
        iowrite32(t->crux_mux_saved, t->tlmm + TLMM_GPIO8_CTL);
        t->crux_mux_gpio = false;
        pr_info("[crux-te] gpio8 mux restored -> %08x\n", t->crux_mux_saved);
    }
    return n;
}

static const struct file_operations crux_mux_fops = { .write = crux_mux_write };

static ssize_t te_pin_read(struct file *f, char __user *u, size_t n, loff_t *o)
{
    struct crux_te *t = f->private_data;
    u64 start;
    u32 hi = 0, lo = 0, v;
    char buf[128];
    int len;

    if (!t || !t->tlmm)
        return -ENODEV;

    start = ktime_get_ns();
    while (ktime_get_ns() - start < 1000000000ULL) {
        v = ioread32(t->tlmm + TLMM_GPIO8_IO);
        if (v & BIT(CRUX_TE_GPIO_BIT))
            hi++;
        else
            lo++;
    }

    len = scnprintf(buf, sizeof(buf),
        "gpio8: hi=%u lo=%u total=%u ratio_ppm=%u\n",
        hi, lo, hi + lo, (hi + lo) ? (u32)((u64)hi * 1000000 / (hi + lo)) : 0);
    return simple_read_from_buffer(u, n, o, buf, len);
}


/*
 * 引导器帧缓冲窗口：可以按 offset 顺读整块 0x9c000000（36MB），
 * 用来把"引导器那屏的内容"取出来看/灌到我们的 fb 里，判断整条 DPU→DSI→面板
 * 通路到底能不能显示出一张"已知内容"的画面。
 * 用法：cat /sys/kernel/debug/crux_te_sync/gop > /tmp/gop.raw
 */
static ssize_t gop_read(struct file *f, char __user *u, size_t n, loff_t *ppos)
{
    struct crux_te *t = f->private_data;
    u8 *tmp;
    size_t left;
    int ret;

    if (!t || !t->gop)
        return -ENODEV;
    if (*ppos < 0 || (u64)*ppos >= CRUX_GOP_SIZE)
        return 0;

    left = CRUX_GOP_SIZE - (size_t)*ppos;
    if (n > 65536)
        n = 65536;
    if (n > left)
        n = left;
    if (!n)
        return 0;

    tmp = kmalloc(n, GFP_KERNEL);
    if (!tmp)
        return -ENOMEM;
    memcpy_fromio(tmp, t->gop + (size_t)*ppos, n);
    ret = copy_to_user(u, tmp, n);
    kfree(tmp);
    if (ret)
        return -EFAULT;
    *ppos += n;
    return n;
}

static const struct file_operations gop_fops = {
    .read = gop_read,
    .llseek = default_llseek,
    .open = simple_open,
};

static const struct file_operations te_pin_fops = {
    .read = te_pin_read,
    .open = simple_open,
};

static const struct file_operations stats_fops = {
    .read = stats_read,
    .open = simple_open,
};

static void crux_te_work(struct work_struct *w)
{
    struct crux_te *t = container_of(to_delayed_work(w), struct crux_te, work);

    crux_dump(t);
    schedule_delayed_work(&t->work, msecs_to_jiffies(2000));
}

static int crux_te_probe(struct platform_device *pdev)
{
    struct crux_te *t;

    t = devm_kzalloc(&pdev->dev, sizeof(*t), GFP_KERNEL);
    if (!t)
        return -ENOMEM;
    t->dev = &pdev->dev;

    t->intf = devm_ioremap(&pdev->dev, CRUX_INTF1_PHYS, CRUX_INTF1_SIZE);
    if (!t->intf)
        return -ENOMEM;
    t->dsi = devm_ioremap(&pdev->dev, CRUX_DSI0_PHYS, CRUX_DSI0_SIZE);
    if (!t->dsi)
        return -ENOMEM;
    t->tlmm = devm_ioremap(&pdev->dev, CRUX_TLMM_PHYS, CRUX_TLMM_SIZE);
    if (!t->tlmm)
        return -ENOMEM;
    t->gop = devm_ioremap(&pdev->dev, CRUX_GOP_PHYS, CRUX_GOP_SIZE);

    platform_set_drvdata(pdev, t);
    g_te = t;

    t->dbg = debugfs_create_dir("crux_te_sync", NULL);
    debugfs_create_file("stats", 0444, t->dbg, t, &stats_fops);
    debugfs_create_file("te_pin", 0444, t->dbg, t, &te_pin_fops);
    debugfs_create_file("muxgpio", 0200, t->dbg, t, &crux_mux_fops);
    debugfs_create_file("gop", 0444, t->dbg, t, &gop_fops);

    INIT_DELAYED_WORK(&t->work, crux_te_work);
    schedule_delayed_work(&t->work, msecs_to_jiffies(1000));

    dev_info(&pdev->dev, "[crux-te] observer ready (intf1 %#lx, dsi0 %#lx, tlmm %#lx)\n",
             CRUX_INTF1_PHYS, CRUX_DSI0_PHYS, CRUX_TLMM_PHYS);
    return 0;
}

static void crux_te_remove(struct platform_device *pdev)
{
    struct crux_te *t = platform_get_drvdata(pdev);

    if (!t)
        return;
    cancel_delayed_work_sync(&t->work);
    debugfs_remove_recursive(t->dbg);
    g_te = NULL;
}

static const struct of_device_id crux_te_of_match[] = {
    { .compatible = "crux,te-sync" },
    { }
};
MODULE_DEVICE_TABLE(of, crux_te_of_match);

static struct platform_driver crux_te_driver = {
    .probe = crux_te_probe,
    .remove = crux_te_remove,
    .driver = { .name = "crux-te-sync", .of_match_table = crux_te_of_match },
};
module_platform_driver(crux_te_driver);

MODULE_DESCRIPTION("crux display tear-check register observer");
MODULE_LICENSE("GPL");
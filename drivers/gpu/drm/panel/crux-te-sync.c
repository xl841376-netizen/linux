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
 * 窗口：DPU INTF_1 = 0xae01000(mdp) + 0x6a800；DSI0 ctrl = 0xae94000；
 *       TLMM = 0x3900000（north tile！gpio8 属于 north，不是 0x3100000/west）
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/debugfs.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/jiffies.h>
#include <linux/suspend.h>
#include <linux/pm_runtime.h>
#include <linux/device.h>

#define CRUX_INTF1_PHYS  0xae6b800UL
#define CRUX_INTF1_SIZE  0x300
#define CRUX_DSI0_PHYS   0xae94000UL
#define CRUX_DSI0_SIZE   0x1000
#define CRUX_TLMM_PHYS   0x3900000UL /* [crux 2026-11-07] north tile：gpio8 在 north，之前写成 west(0x3100000) 是错的 */
#define CRUX_TLMM_SIZE   0x10000 /* [crux 2026-11-07] 必须覆盖 gpio8 的 0x8000 偏移；原来只映射 0x1000 是越界读 */
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
/* [crux 2026-10-12] 官方语义（原厂 sde_hw_intf.c:630）：0x298 = [31:16] 读指针帧计数 | [15:0] 读指针当前行号 */
#define I_TEAR_INT_COUNT   0x298
#define I_TEAR_SYNC_CFG_HEIGHT 0x28c   /* 主线写入 vtotal*2，可反推 vtotal */
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
    struct device *dpu_dev;    /* [crux 2026-11-06] DPU 平台设备，用它的 runtime PM 判活 */
    void __iomem *intf;
    void __iomem *dsi;
    void __iomem *tlmm;       /* = tiles[NORTH]，gpio8 所在 */
    void __iomem *tiles[4];   /* [crux 2026-11-07] west/east/north/south */
    void __iomem *gop;
    u32 crux_mux_saved;
    bool crux_mux_gpio;
    u32 prev[CRUX_MAX_REG];
    int nprev;
    struct dentry *dbg;
    struct delayed_work work;
    unsigned long last_warn;   /* [crux2026-10-11] 异常上报节流用 */

    /*
     * [crux 2026-11-05] suspend 期间禁止采样。
     * 2026-11-04 23:53:26 的 synchronous external abort（pc: crux_te_work+0x2c）
     * 就是 suspend 时 DSI/INTF 已下电、而 20ms 轮询还在读寄存器。
     * 用 pm_notifier 在“设备真正 suspend 之前”就停掉 work。
     */
    bool suspended;
    struct notifier_block pm_nb;

    /*
     * [crux 2026-10-12] 速率统计：从 TEAR 读指针换算“面板实际帧率”。
     * 0x298 低位是读指针当前行号，行推进速率 / vtotal 就是面板真实帧率；
     * 这比看 TE 引脚靠谱（gpio8 被 DSI 以 mdp_vsync 持有，GPIO_IN 可能是盲的）。
     */
    spinlock_t rate_lock;
    u64 lines_total;        /* 读指针累计推进的行数 */
    u64 frames_total;       /* 读指针帧字段累计增量 */
    u32 last_line, last_frame;
    u32 vtotal;             /* 由 0x28C/2 得到 */
    unsigned long long samples;
    unsigned long last_move;/* 最近一次读指针推进 */
};

static struct crux_te *g_te;

/*
 * [crux 2026-11-06] 寄存器读取闸门（第二版）。
 *
 * 现场证据（journalctl -b -1, 01:42:26）：
 *   Internal error: synchronous external abort ... Workqueue: events crux_te_work
 *   pc : crux_te_work+0x50
 * 那次不是 hangcheck recover，而是**屏幕 blank（DPMS off）**：DPU 整块进入
 * runtime suspend，INTF/DSI 断电，20ms 轮询还在读 -> abort。
 *
 * 实测判据（2026-11-06 01:50 验证）：
 *   DSI dpms On  -> /sys/.../ae01000.display-controller/power/runtime_status = active
 *   DSI dpms Off -> 同一文件 = suspended
 * 所以只要"拿到 DPU 的 runtime PM 引用"才读寄存器，就能同时覆盖
 * blank / suspend / 复位后下电 三种情况；读不到引用就跳过这一轮。
 */
extern bool msm_display_hw_unavailable(void);
extern void msm_display_hw_commit_done(void);

#define CRUX_DPU_DEV_NAME "ae01000.display-controller"

/* 成功拿到引用返回 true（调用方读完必须 crux_hw_put） */
static bool crux_hw_get(struct crux_te *t)
{
    if (READ_ONCE(t->suspended))
        return false;
    if (msm_display_hw_unavailable())
        return false;
    if (!t->dpu_dev)
        return false;
    /* 返回 0 表示 DPU 已 runtime suspend，寄存器不可碰 */
    if (pm_runtime_get_if_active(t->dpu_dev) <= 0)
        return false;
    return true;
}

static void crux_hw_put(struct crux_te *t)
{
    pm_runtime_put(t->dpu_dev);
}

/* 调试口拒绝读时只打印一次，避免刷屏 */
static void crux_warn_unavailable(struct crux_te *t, const char *file)
{
    if (time_after(jiffies, t->last_warn + 10 * HZ)) {
        t->last_warn = jiffies;
        pr_warn("[crux-te] %s: 显示控制器复位中/已下电，拒绝读 INTF/DSI 寄存器（防 external abort）\n",
                file);
    }
}

/*
 * [crux 2026-10-11] 日志分流：默认只在“明确异常”时打印。
 * 原来每 2 秒无条件打 1 行 + 变化时再打满 17 行（frame/line/tear_* 这些
 * 单调计数器每轮都在变）=> 约 9 行/秒、每天 78 万行、journal 涨到 1.6G。
 * 现在：
 *   ① 首次采样只建立基线，不打日志；
 *   ② 单调计数器（frame_cnt/line_cnt/tear_*_cnt/out_line）的变化不算异常；
 *   ③ 只有“稳定寄存器偏离基线”才算异常 -> pr_warn，且最多 60s 报一次；
 *   ④ 需要全量时：echo 1 > /sys/module/crux_te_sync/parameters/verbose
 *      （或随时读 /sys/kernel/debug/crux_te_sync/stats，不受影响）。
 */
static bool crux_verbose;
module_param_named(verbose, crux_verbose, bool, 0644);
MODULE_PARM_DESC(verbose, "每轮打印全部 INTF/DSI 寄存器（默认 0：只在异常时打印）");

static u32 rd(void __iomem *b, u32 off)
{
    return ioread32(b + off);
}

static void crux_dump(struct crux_te *t)
{
    const char *nm[CRUX_MAX_REG];
    u32 v[CRUX_MAX_REG];
    bool mono[CRUX_MAX_REG];
    int i, n = 0, changed = 0;

#define ADD(base, name, off)  do { nm[n] = name; v[n] = rd(base, off); mono[n] = false; n++; } while (0)
#define ADDM(base, name, off) do { nm[n] = name; v[n] = rd(base, off); mono[n] = true;  n++; } while (0)
    ADD(t->intf, "intf_timing_en", I_TIMING_EN);
    ADD(t->intf, "intf_cfg", I_CONFIG);
    ADD(t->intf, "intf_cfg2", I_CONFIG2);
    ADD(t->intf, "intf_panel_fmt", I_PANEL_FORMAT);
    ADD(t->intf, "intf_polarity", I_POLARITY);
    ADDM(t->intf, "intf_frame_cnt", I_FRAME_COUNT);
    ADDM(t->intf, "intf_line_cnt", I_LINE_COUNT);
    ADD(t->intf, "tear_vsync_sel", I_TEAR_VSYNC_SEL);
    ADD(t->intf, "tear_check_en", I_TEAR_CHECK_EN);
    ADD(t->intf, "tear_sync_cfg", I_TEAR_SYNC_CFG);
    ADDM(t->intf, "tear_int_cnt", I_TEAR_INT_COUNT);
    ADDM(t->intf, "tear_line_cnt", I_TEAR_LINE_COUNT);
    ADDM(t->intf, "tear_out_line", I_TEAR_OUT_LINE);
    ADD(t->intf, "tear_autoref", I_TEAR_AUTOREFRESH);
    ADD(t->dsi, "dsi_ctrl", D_CTRL);
    ADD(t->dsi, "dsi_cmd_cfg0", D_CMD_CFG0);
    ADD(t->dsi, "dsi_cmd_cfg1", D_CMD_CFG1);
#undef ADD
#undef ADDM

    /* 首次采样：只建立基线，不打日志 */
    if (t->nprev == 0) {
        for (i = 0; i < n; i++)
            t->prev[i] = v[i];
        t->nprev = n;
        return;
    }

    /* 调试模式：恢复原来的全量 dump */
    if (crux_verbose) {
        pr_info("[crux-te] dsi: dma_ctrl=%08x trig=%08x lane=%08x lane_swap=%08x err_mask0=%08x mdp_ctrl2=%08x\n",
                rd(t->dsi, D_CMD_DMA_CTRL), rd(t->dsi, D_TRIG_CTRL),
                rd(t->dsi, D_LANE_CTRL), rd(t->dsi, D_LANE_SWAP),
                rd(t->dsi, D_ERR_INT_MASK0), rd(t->dsi, D_CMD_MDP_CTRL2));
        for (i = 0; i < n; i++)
            pr_info("[crux-te] %s=%08x\n", nm[i], v[i]);
        return;
    }

    /* 默认路径：只有“稳定寄存器偏离基线”才算明确异常 */
    for (i = 0; i < n; i++)
        if (!mono[i] && v[i] != t->prev[i])
            changed++;

    if (changed && time_after(jiffies, t->last_warn + 60 * HZ)) {
        t->last_warn = jiffies;
        /* [crux] 每次切 90Hz 档都会重编程 TE，基线偏移属预期行为，按 info 记录 */
        pr_info("[crux-te] 稳定寄存器偏离基线：%d 个\n", changed);
        for (i = 0; i < n; i++) {
            if (mono[i] || v[i] == t->prev[i])
                continue;
            pr_info("[crux-te] %s: 基线 %08x -> 现在 %08x\n",
                    nm[i], t->prev[i], v[i]);
        }
    }

    /*
     * [crux 2026-10-11] 报过就接受为“新基线”：同一个值不再重复刷屏；
     * 只有它再次变化时才会再报一次（真正的边沿触发）。
     */
    for (i = 0; i < n; i++)
        t->prev[i] = v[i];
}

/* [crux 2026-10-12] 高频采样 TEAR 读指针，累计推进量（用于算面板真实帧率） */
static void crux_sample(struct crux_te *t)
{
    u32 v = rd(t->intf, I_TEAR_INT_COUNT);
    u32 frame = v >> 16, line = v & 0xffff;
    u32 vt = rd(t->intf, I_TEAR_SYNC_CFG_HEIGHT) >> 1;  /* 0x28C = vtotal*2 */
    unsigned long flags;

    spin_lock_irqsave(&t->rate_lock, flags);
    if (t->samples) {
        u32 dl = (line - t->last_line) & 0xffff;
        u32 df = (frame - t->last_frame) & 0xffff;

        if (dl > 0x8000)
            dl = 0;                 /* 大幅回退视为回绕/复位，不计 */
        if (df > 0x8000)
            df = 0;
        t->lines_total += dl;
        t->frames_total += df;
        if (dl || df)
            t->last_move = jiffies;
    }
    t->last_line = line;
    t->last_frame = frame;
    t->vtotal = vt;
    t->samples++;
    spin_unlock_irqrestore(&t->rate_lock, flags);
}

/* [crux 2026-10-12] 在给定窗口内紧采样 gpio8 电平，返回边沿数（TE 脉冲数≈边沿/2） */
static unsigned int crux_gpio_edges(struct crux_te *t, u64 window_ns, u32 *hi, u32 *lo)
{
    u64 start = ktime_get_ns();
    unsigned int edges = 0;
    u32 last = ioread32(t->tlmm + TLMM_GPIO8_IO) & BIT(CRUX_TE_GPIO_BIT);
    u32 h = 0, l = 0, v;

    while (ktime_get_ns() - start < window_ns) {
        v = ioread32(t->tlmm + TLMM_GPIO8_IO) & BIT(CRUX_TE_GPIO_BIT);
        if (v)
            h++;
        else
            l++;
        if (v != last) {
            edges++;
            last = v;
        }
    }
    *hi = h;
    *lo = l;
    return edges;
}

/*
 * /sys/kernel/debug/crux_te_sync/rate —— 一秒窗口的帧率实测
 *   lines_advanced / line_rate  : TEAR 读指针推进的行数与行率
 *   est_refresh                 : 行率 / vtotal = 面板实际帧率（最关键的读数）
 *   frame_field_rate            : 0x298 高位帧字段的速率（供对照）
 *   te_pin(100ms)               : gpio8 电平/边沿统计（边沿为 0 说明引脚看不到 TE）
 */
static ssize_t rate_read(struct file *f, char __user *u, size_t n, loff_t *o)
{
    struct crux_te *t = f->private_data;
    char buf[768];
    u64 ns, l1, l2, f1, f2, dlines, dframes;
    u64 line_mhz, refresh_mhz, frame_mhz, te_mhz;
    u32 vt, v, hi = 0, lo = 0, edges;
    unsigned long long s1, s2;
    unsigned long flags;
    int len;

    if (!t || !t->intf)
        return -ENODEV;

    if (!crux_hw_get(t)) {
        crux_warn_unavailable(t, "rate");
        return -EIO;
    }

    spin_lock_irqsave(&t->rate_lock, flags);
    l1 = t->lines_total; f1 = t->frames_total; s1 = t->samples; vt = t->vtotal;
    spin_unlock_irqrestore(&t->rate_lock, flags);
    ns = ktime_get_ns();

    msleep(1000);

    spin_lock_irqsave(&t->rate_lock, flags);
    l2 = t->lines_total; f2 = t->frames_total; s2 = t->samples;
    if (t->vtotal)
        vt = t->vtotal;
    spin_unlock_irqrestore(&t->rate_lock, flags);
    ns = ktime_get_ns() - ns;

    dlines = l2 - l1;
    dframes = f2 - f1;

    /* 内核不能碰浮点：统一用整数算，结果单位为 mHz（=Hz*1000） */
    line_mhz = ns ? (dlines * 1000000000000ULL) / ns : 0;
    refresh_mhz = vt ? line_mhz / vt : 0;
    frame_mhz = ns ? (dframes * 1000000000000ULL) / ns : 0;

    v = rd(t->intf, I_TEAR_INT_COUNT);
    edges = crux_gpio_edges(t, 100ULL * NSEC_PER_MSEC, &hi, &lo);
    te_mhz = (u64)edges * 5000ULL;   /* 100ms 内 边沿/2 个脉冲 -> Hz = edges*5 */

    len = scnprintf(buf, sizeof(buf),
        "window_ms=%llu samples=%llu->%llu\n"
        "vtotal(from 0x28C/2)=%u\n"
        "rd_ptr_now: frame=%u line=%u\n"
        "lines_advanced=%llu  line_rate=%llu.%03llu Hz\n"
        "est_refresh(line_rate/vtotal)=%llu.%03llu Hz\n"
        "frame_field_rate=%llu.%03llu /s\n"
        "te_pin(100ms): edges=%u hi=%u lo=%u  est_te_rate=%llu.%03llu Hz\n",
        (unsigned long long)(ns / NSEC_PER_MSEC), s1, s2, vt,
        v >> 16, v & 0xffff,
        (unsigned long long)dlines,
        (unsigned long long)(line_mhz / 1000), (unsigned long long)(line_mhz % 1000),
        (unsigned long long)(refresh_mhz / 1000), (unsigned long long)(refresh_mhz % 1000),
        (unsigned long long)(frame_mhz / 1000), (unsigned long long)(frame_mhz % 1000),
        edges, hi, lo,
        (unsigned long long)(te_mhz / 1000), (unsigned long long)(te_mhz % 1000));

    crux_hw_put(t);
    return simple_read_from_buffer(u, n, o, buf, len);
}

static const struct file_operations rate_fops = {
    .read = rate_read,
    .open = simple_open,
};

static ssize_t stats_read(struct file *f, char __user *u, size_t n, loff_t *o)
{
    struct crux_te *t = f->private_data;
    char buf[512];
    int len;

    if (!t)
        return -ENODEV;

    if (!crux_hw_get(t)) {
        crux_warn_unavailable(t, "stats");
        return -EIO;
    }

    len = scnprintf(buf, sizeof(buf),
        "intf1@%08lx dsi0@%08lx\n"
        "sync_cfg_height=%08x (vtotal=%u)\n"
        "rd_ptr: frame=%u line=%u  [0x298 官方语义]\n"
        "tear_check_en=%08x sync_cfg=%08x int_cnt=%08x line_cnt=%08x out_line=%08x\n"
        "intf_line_cnt=%08x frame_cnt=%08x timing_en=%08x cfg=%08x cfg2=%08x\n"
        "dsi_ctrl=%08x cmd_cfg0=%08x mdp_ctrl2=%08x err_mask0=%08x\n"\
        "tlmm_gpio8_ctl=%08x tlmm_gpio8_io=%08x\n",
        CRUX_INTF1_PHYS, CRUX_DSI0_PHYS,
        rd(t->intf, I_TEAR_SYNC_CFG_HEIGHT), rd(t->intf, I_TEAR_SYNC_CFG_HEIGHT) >> 1,
        rd(t->intf, I_TEAR_INT_COUNT) >> 16, rd(t->intf, I_TEAR_INT_COUNT) & 0xffff,
        rd(t->intf, I_TEAR_CHECK_EN), rd(t->intf, I_TEAR_SYNC_CFG),
        rd(t->intf, I_TEAR_INT_COUNT), rd(t->intf, I_TEAR_LINE_COUNT),
        rd(t->intf, I_TEAR_OUT_LINE),
        rd(t->intf, I_LINE_COUNT), rd(t->intf, I_FRAME_COUNT),
        rd(t->intf, I_TIMING_EN), rd(t->intf, I_CONFIG), rd(t->intf, I_CONFIG2),
        rd(t->dsi, D_CTRL), rd(t->dsi, D_CMD_CFG0),
        rd(t->dsi, D_CMD_MDP_CTRL2), rd(t->dsi, D_ERR_INT_MASK0),\
        rd(t->tlmm, TLMM_GPIO8_CTL), rd(t->tlmm, TLMM_GPIO8_IO));
    crux_hw_put(t);
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

    if (!crux_hw_get(t)) {
        crux_warn_unavailable(t, "muxgpio");
        return -EIO;
    }

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
    crux_hw_put(t);
    return n;
}

static ssize_t crux_setmux_write(struct file *f, const char __user *u,
                                 size_t n, loff_t *o)
{
    struct crux_te *t = f->private_data;
    char buf[8];
    u32 v, cur;

    if (!t || !t->tlmm)
        return -ENODEV;
    if (n == 0 || n >= sizeof(buf))
        return -EINVAL;
    if (copy_from_user(buf, u, n))
        return -EFAULT;
    buf[n] = 0;
    if (kstrtou32(buf, 0, &v))
        return -EINVAL;
    if (v > 15)
        return -EINVAL;

    if (!crux_hw_get(t)) {
        crux_warn_unavailable(t, "setmux");
        return -EIO;
    }

    cur = rd(t->tlmm, TLMM_GPIO8_CTL);
    iowrite32((cur & ~TLMM_MUX_MASK) | ((v << 2) & TLMM_MUX_MASK),
              t->tlmm + TLMM_GPIO8_CTL);
    pr_info("[crux-te] gpio8 mux: %08x -> func %u (%08x)\n",
            cur, v, rd(t->tlmm, TLMM_GPIO8_CTL));
    crux_hw_put(t);
    return n;
}

/*
 * [crux 2026-11-07] 逐 tile 读某个 pin 的 CTL/IO，用于验证四个 tile 的
 * 地址映射是否和 pinctrl 的视图一致。写 "tile pin"，tile: 0=west 1=east 2=north 3=south。
 */
static ssize_t crux_pinread_write(struct file *f, const char __user *u,
                                  size_t n, loff_t *o)
{
    struct crux_te *t = f->private_data;
    char buf[24];
    int tile, pin;
    u32 ctl, io;

    if (!t)
        return -ENODEV;
    if (n == 0 || n >= sizeof(buf))
        return -EINVAL;
    if (copy_from_user(buf, u, n))
        return -EFAULT;
    buf[n] = 0;
    if (sscanf(buf, "%d %d", &tile, &pin) != 2)
        return -EINVAL;
    if (tile < 0 || tile > 3 || pin < 0 || pin > 175)
        return -EINVAL;

    if (!crux_hw_get(t)) {
        crux_warn_unavailable(t, "pinread");
        return -EIO;
    }
    ctl = rd(t->tiles[tile], 0x1000 * pin);
    io  = rd(t->tiles[tile], 0x1000 * pin + 4);
    crux_hw_put(t);

    pr_info("[crux-te] tile%d pin%d: ctl=%08x (mux=%u) io=%08x\n",
            tile, pin, ctl, (ctl >> 2) & 0xf, io);
    return n;
}

static const struct file_operations crux_pinread_fops = {
    .owner = THIS_MODULE,
    .open  = simple_open,
    .write = crux_pinread_write,
};

static const struct file_operations crux_setmux_fops = {
    .owner = THIS_MODULE,
    .open  = simple_open,
    .write = crux_setmux_write,
};

static const struct file_operations crux_mux_fops = {
    .owner = THIS_MODULE,
    .open  = simple_open,
    .write = crux_mux_write,
};

static ssize_t te_pin_read(struct file *f, char __user *u, size_t n, loff_t *o)
{
    struct crux_te *t = f->private_data;
    u64 start;
    u32 hi = 0, lo = 0, v;
    char buf[128];
    int len;

    if (!t || !t->tlmm)
        return -ENODEV;
    if (!crux_hw_get(t)) {
        crux_warn_unavailable(t, "te_pin");
        return -EIO;
    }

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
    crux_hw_put(t);
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

    /*
     * suspend 里 DSI/INTF 已断电；hangcheck recover 之后到下一次成功提交之前
     * 同样断电。两种情况读寄存器都会 synchronous external abort。
     */
    if (!crux_hw_get(t))
        goto requeue;

    /* [crux 2026-10-12] 20ms 一次采样（算行率），每 100 次（2s）再做一次变更检测 */
    crux_sample(t);
    if ((t->samples % 100) == 0)
        crux_dump(t);
    crux_hw_put(t);

requeue:
    if (!t->suspended)
        schedule_delayed_work(&t->work, msecs_to_jiffies(20));
}

/*
 * [crux 2026-11-05] 在设备 suspend 之前就停掉轮询。
 * PM_SUSPEND_PREPARE 早于所有设备的 ->suspend 回调，所以显示还没断电；
 * 用 dev_pm_ops 的话要赌我们和 DSI/DPU 的 suspend 顺序，赌错就是 abort。
 */
static int crux_te_pm_notify(struct notifier_block *nb, unsigned long action, void *data)
{
    struct crux_te *t = container_of(nb, struct crux_te, pm_nb);

    switch (action) {
    case PM_SUSPEND_PREPARE:
    case PM_HIBERNATION_PREPARE:
        WRITE_ONCE(t->suspended, true);
        cancel_delayed_work_sync(&t->work);
        break;
    case PM_POST_SUSPEND:
    case PM_POST_HIBERNATION:
        WRITE_ONCE(t->suspended, false);
        schedule_delayed_work(&t->work, msecs_to_jiffies(200));
        break;
    default:
        break;
    }
    return NOTIFY_OK;
}


/* [crux 2026-11-07] 只映射、不 request_mem_region：
 * TLMM/INTF/DSI 这些窗口已经被 pinctrl/dpu/dsi 驱动占用，
 * devm_platform_ioremap_resource_byname() 会 -EBUSY。 */
static void __iomem *crux_map_by_name(struct platform_device *pdev, const char *name)
{
    struct resource *res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);

    if (!res)
        return ERR_PTR(-ENODEV);
    return devm_ioremap(&pdev->dev, res->start, resource_size(res));
}

static int crux_te_probe(struct platform_device *pdev)
{
    struct crux_te *t;

    t = devm_kzalloc(&pdev->dev, sizeof(*t), GFP_KERNEL);
    if (!t)
        return -ENOMEM;
    t->dev = &pdev->dev;

    /*
     * [crux 2026-11-07] 窗口全部从 DT 的 reg/reg-names 取。
     * 之前硬编码：tile 写错（west 当成 north）+ 映射长度不足（0x1000
     * 覆盖不了 gpio8 的 0x8000 偏移），导致读到窗口外的假数据。
     * 现在缺窗口直接 probe 失败，换板子/换实例会立刻暴露。
     */
    t->intf = crux_map_by_name(pdev, "intf1");
    if (IS_ERR(t->intf))
        return PTR_ERR(t->intf);
    t->dsi = crux_map_by_name(pdev, "dsi0");
    if (IS_ERR(t->dsi))
        return PTR_ERR(t->dsi);
    t->tiles[0] = crux_map_by_name(pdev, "tlmm-west");
    if (IS_ERR(t->tiles[0]))
        return PTR_ERR(t->tiles[0]);
    t->tiles[1] = crux_map_by_name(pdev, "tlmm-east");
    if (IS_ERR(t->tiles[1]))
        return PTR_ERR(t->tiles[1]);
    t->tiles[2] = crux_map_by_name(pdev, "tlmm-north");
    if (IS_ERR(t->tiles[2]))
        return PTR_ERR(t->tiles[2]);
    t->tiles[3] = crux_map_by_name(pdev, "tlmm-south");
    if (IS_ERR(t->tiles[3]))
        return PTR_ERR(t->tiles[3]);
    t->tlmm = t->tiles[2];   /* gpio8 在 north tile */
    t->gop = devm_ioremap(&pdev->dev, CRUX_GOP_PHYS, CRUX_GOP_SIZE);
    if (!t->gop)
        return -ENOMEM;

    /*
     * [crux 2026-11-06] 拿住 DPU 的引用：后面所有寄存器读取都靠它的
     * runtime PM 状态判活（blank/suspend 时它会是 suspended）。
     */
    t->dpu_dev = bus_find_device_by_name(&platform_bus_type, NULL,
                                         CRUX_DPU_DEV_NAME);
    if (!t->dpu_dev)
        return -EPROBE_DEFER;

    platform_set_drvdata(pdev, t);
    g_te = t;

    spin_lock_init(&t->rate_lock);

    t->dbg = debugfs_create_dir("crux_te_sync", NULL);
    debugfs_create_file("stats", 0444, t->dbg, t, &stats_fops);
    debugfs_create_file("rate", 0444, t->dbg, t, &rate_fops);
    debugfs_create_file("te_pin", 0444, t->dbg, t, &te_pin_fops);
    debugfs_create_file("muxgpio", 0200, t->dbg, t, &crux_mux_fops);
    debugfs_create_file("setmux", 0200, t->dbg, t, &crux_setmux_fops);
    debugfs_create_file("pinread", 0200, t->dbg, t, &crux_pinread_fops);
    debugfs_create_file("gop", 0444, t->dbg, t, &gop_fops);

    INIT_DELAYED_WORK(&t->work, crux_te_work);
    t->pm_nb.notifier_call = crux_te_pm_notify;
    register_pm_notifier(&t->pm_nb);
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
    unregister_pm_notifier(&t->pm_nb);
    cancel_delayed_work_sync(&t->work);
    debugfs_remove_recursive(t->dbg);
    if (t->dpu_dev) {
        put_device(t->dpu_dev);
        t->dpu_dev = NULL;
    }
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
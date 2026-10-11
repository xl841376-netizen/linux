// SPDX-License-Identifier: GPL-2.0-only
/*
 * [crux] 关机代理驱动（2026-09-25 第二版）
 *
 * 一句话：主干交给主线的 device_shutdown（自动、有序、同步），
 * 本驱动只做三件事：记录、校验、最后那一下。
 *
 *  1. 记录（层一）：drivers/base/core.c 的 device_shutdown 对每个设备调用
 *     crux_shutdown_device()，本驱动把“有回调的设备”按执行顺序记进 report，
 *     并统计走过多少设备。新驱动不需要登记，写了 .shutdown 就自动出现。
 *  2. 校验（层二 / 层三）：POWER_OFF_PREPARE（允许睡眠）阶段
 *     a) 等 UFS 上的文件系统变成只读或卸载（“UFS 最后卸载”的判定点）；
 *     b) 跑各驱动注册的 VERIFY 步骤（例如读回 PON 的 PS_HOLD 动作）；
 *     c) 查设备树 crux,shutdown-critical 列出的必查项有没有真的走完关机。
 *     三张全局表（clk / regulator / pm_genpd）由用户态在同一阶段快照，一并落盘。
 *  3. 最后那一下（层四）：POWER_OFF / RESTART 各占 HIGH(192) 唯一名额，
 *     跑完返回 NOTIFY_STOP 掐断链子；真正断电由注册进来的 ps-hold 步骤做。
 *
 * 结果去处：
 *  - 用户态收尾阶段（文件系统还写得动）读 /sys/kernel/crux-shutdown/report 落进 UFS；
 *  - 内核收尾之后的补充部分（device_shutdown 之后才产生的那些）在最后用 pr_emerg
 *    打到内核日志/屏幕，便于出问题时直接拍屏。
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/reboot.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/string.h>
#include <linux/stdarg.h>
#include <linux/nvmem-consumer.h>
#include <linux/crux_shutdown.h>

#define CRUX_REPORT_SIZE	32768
#define CRUX_REASON_MAX		64
#define CRUX_UFS_WAIT_MS	5000
#define CRUX_UFS_POLL_MS	200
#define CRUX_DEV_MAX		96
#define CRUX_DEV_NAME		32
#define CRUX_CRIT_MAX		16

static char crux_report_buf[CRUX_REPORT_SIZE];
static size_t crux_report_len;
static size_t crux_snap_len;		/* 用户态取走报告时的位置 */
static char crux_stage[32] = "boot";
static char crux_reason[CRUX_REASON_MAX] = "normal";
static LIST_HEAD(crux_step_list);
static DEFINE_SPINLOCK(crux_step_lock);
static void __iomem *crux_imem_reason;
/* [crux 2026-09-25] 关机留痕字：上一轮有没有跑到触控的关机回调 */
static void __iomem *crux_imem_parked;
#define CRUX_PARKED_MAGIC 0x46540000	/* 上半字标记 "FT"，低半字放来源位 */
static int crux_last_err;
static bool crux_cut_started;
static struct kobject *crux_shutdown_kobj;

/* 层一：记录 */
static char crux_dev_done[CRUX_DEV_MAX][CRUX_DEV_NAME];
static char crux_dev_drv[CRUX_DEV_MAX][CRUX_DEV_NAME];
static int crux_dev_done_count;
static unsigned int crux_dev_walked;
static unsigned int crux_dev_with_cb;

/* 层三：必查项 */
static const char *crux_crit[CRUX_CRIT_MAX];
static int crux_crit_count;

const char *crux_shutdown_action_name(enum crux_shutdown_action action)
{
	switch (action) {
	case CRUX_SHUTDOWN_PREPARE:
		return "prepare";
	case CRUX_SHUTDOWN_VERIFY:
		return "verify";
	case CRUX_SHUTDOWN_POWEROFF:
		return "poweroff";
	case CRUX_SHUTDOWN_RESTART:
		return "restart";
	case CRUX_SHUTDOWN_RECOVER:
		return "recover";
	}
	return "?";
}
EXPORT_SYMBOL_GPL(crux_shutdown_action_name);

static void crux_report_add(const char *name, const char *fmt, va_list args)
{
	size_t left;
	int n;

	if (crux_report_len >= CRUX_REPORT_SIZE - 1)
		return;

	left = CRUX_REPORT_SIZE - crux_report_len;
	n = scnprintf(crux_report_buf + crux_report_len, left, "[%s] ",
		      name ? name : "-");
	crux_report_len += n;

	left = CRUX_REPORT_SIZE - crux_report_len;
	if (left < 2)
		return;

	n = vsnprintf(crux_report_buf + crux_report_len, left - 1, fmt, args);
	if (n > 0)
		crux_report_len += min_t(size_t, (size_t)n, left - 2);

	crux_report_buf[crux_report_len++] = '\n';
	crux_report_buf[crux_report_len] = '\0';
}

/* 只往静态缓冲追加，原子上下文安全 */
void crux_shutdown_report(const char *name, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	crux_report_add(name, fmt, args);
	va_end(args);
}
EXPORT_SYMBOL_GPL(crux_shutdown_report);

/* 层一：device_shutdown 每处理完一个设备调一次 */
void crux_shutdown_device(const char *dev, const char *drv, bool has_cb, u64 usec)
{
	crux_dev_walked++;
	if (!has_cb)
		return;

	crux_dev_with_cb++;
	if (crux_dev_done_count < CRUX_DEV_MAX) {
		strscpy(crux_dev_done[crux_dev_done_count], dev ? dev : "?",
			CRUX_DEV_NAME);
		strscpy(crux_dev_drv[crux_dev_done_count], drv ? drv : "-",
			CRUX_DEV_NAME);
		crux_dev_done_count++;
	}

	crux_shutdown_report("device", "%s drv=%s cb=yes %llu us",
			     dev ? dev : "?", drv ? drv : "-",
			     (unsigned long long)usec);
}
EXPORT_SYMBOL_GPL(crux_shutdown_device);

int crux_shutdown_set_reason(const char *reason)
{
	if (!reason)
		return -EINVAL;

	strscpy(crux_reason, reason, sizeof(crux_reason));
	return 0;
}
EXPORT_SYMBOL_GPL(crux_shutdown_set_reason);

int crux_shutdown_register_step(struct crux_shutdown_step *step)
{
	struct crux_shutdown_step *pos;
	unsigned long flags;
	bool inserted = false;

	if (!step || !step->fn || !step->name)
		return -EINVAL;

	if (step->registered) {
		pr_warn("[crux] step %s already registered\n", step->name);
		return -EBUSY;
	}

	INIT_LIST_HEAD(&step->list);

	spin_lock_irqsave(&crux_step_lock, flags);
	list_for_each_entry(pos, &crux_step_list, list) {
		if (pos->priority < step->priority) {
			list_add_tail(&step->list, &pos->list);
			inserted = true;
			break;
		}
	}
	if (!inserted)
		list_add_tail(&step->list, &crux_step_list);
	step->registered = true;
	spin_unlock_irqrestore(&crux_step_lock, flags);

	crux_shutdown_report("agent", "step registered: %s (prio %d)",
			     step->name, step->priority);
	return 0;
}
EXPORT_SYMBOL_GPL(crux_shutdown_register_step);

void crux_shutdown_unregister_step(struct crux_shutdown_step *step)
{
	unsigned long flags;

	if (!step || !step->registered)
		return;

	spin_lock_irqsave(&crux_step_lock, flags);
	list_del_init(&step->list);
	step->registered = false;
	spin_unlock_irqrestore(&crux_step_lock, flags);
}
EXPORT_SYMBOL_GPL(crux_shutdown_unregister_step);

static void crux_run_steps(enum crux_shutdown_action action)
{
	struct crux_shutdown_step *step;

	crux_last_err = 0;
	list_for_each_entry(step, &crux_step_list, list) {
		int ret = step->fn(action, step->data);

		if (ret)
			crux_last_err = ret;
		crux_shutdown_report(step->name, "%s: %s (%d)",
				     crux_shutdown_action_name(action),
				     ret ? "failed" : "ok", ret);
	}
}

struct crux_ufs_state {
	int total;
	int rw;
};

static void crux_sb_check(struct super_block *sb, void *arg)
{
	struct crux_ufs_state *st = arg;
	struct block_device *bdev = sb->s_bdev;
	const char *dname;

	if (!bdev || !bdev->bd_disk)
		return;

	dname = bdev->bd_disk->disk_name;
	if (!dname || strncmp(dname, "sd", 2))
		return;

	st->total++;
	if (sb_rdonly(sb))
		return;

	st->rw++;
	crux_shutdown_report("ufs", "still rw: %s fstype=%s", dname,
			     sb->s_type ? sb->s_type->name : "?");
}

/* 只允许在能睡眠的上下文调用 */
static int crux_check_ufs(bool wait)
{
	unsigned int waited = 0;
	struct crux_ufs_state st;

	for (;;) {
		memset(&st, 0, sizeof(st));
		iterate_supers(crux_sb_check, &st);
		crux_shutdown_report("ufs", "check: %u ufs fs, %u still rw",
				     st.total, st.rw);
		if (!st.rw || !wait || waited >= CRUX_UFS_WAIT_MS)
			return st.rw;
		msleep(CRUX_UFS_POLL_MS);
		waited += CRUX_UFS_POLL_MS;
	}
}

/* 层三：必查项有没有真的走完关机（比对设备名或驱动名） */
static int crux_check_critical(void)
{
	int i, j, missing = 0;

	if (!crux_crit_count) {
		crux_shutdown_report("critical", "no crux,shutdown-critical list");
		return 0;
	}

	for (i = 0; i < crux_crit_count; i++) {
		bool found = false;
		char want[CRUX_DEV_NAME];

		strscpy(want, crux_crit[i], sizeof(want));

		for (j = 0; j < crux_dev_done_count; j++) {
			if (!strcmp(crux_dev_done[j], want) ||
			    !strcmp(crux_dev_drv[j], want)) {
				found = true;
				break;
			}
		}

		if (!found)
			missing++;
		crux_shutdown_report("critical", "%s: %s", want,
				     found ? "shutdown callback ran"
					   : "MISSING (no callback or not seen)");
	}

	return missing;
}



/*
 * 开机自检（2026-09-25）：上一轮关机有没有干净收尾，由本驱动自己看。
 * 裁决落在 SDAM 常电域，真断电也留得住，所以这里读到的是权威事实，
 * 不再依赖 initramfs 或用户态留下的文件。
 * 语义：读到 DIRTY 算脏；连记录都没有（魔数不在）算“无法证明干净”，也按脏处理，
 * 后续恢复动作宁可多跑一次，也不漏掉一次硬复位之后的烂摊子。
 */
/*
 * [crux 2026-09-25] 自检从 5 秒提前到 1 秒：用户态要拿这个结论决定
 * “这一轮到底要不要起桌面”，所以要赶在显示管理器之前把裁决摆出来。
 * 单元晚出现时下面照旧每 2 秒重试，最多 25 次。
 */
#define CRUX_BOOT_CHECK_FIRST_MS	1000
#define CRUX_BOOT_CHECK_RETRY_MS	2000
#define CRUX_BOOT_CHECK_TRIES		25


/*
 * [crux 2026-09-25] 关机的“最后期限”（用户口径：不能卡在半死不死的状态里等人长按）。
 * 判脏那一刻起挂一条 45 秒的线；一旦进了内核关机流程，再收成 15 秒。
 * 时间到就直接把 PON 的 PS_HOLD 动作配好、把 PS_HOLD 拉低，整机真断电——
 * device_shutdown 里哪一颗驱动赖着不走，都不再拖着整机等长按。
 */
#define CRUX_FORCE_CUT_BOOT_MS		45000
#define CRUX_FORCE_CUT_KERNEL_MS	15000

static struct delayed_work crux_force_cut_work;
static bool crux_force_cut_started;
static bool crux_force_cut_off = true;

static void crux_force_cut_workfn(struct work_struct *w)
{
	if (crux_force_cut_started)
		return;

	crux_force_cut_started = true;

	pr_emerg("[crux] shutdown deadline: forcing %s cut now\n",
		 crux_force_cut_off ? "poweroff" : "restart");
	crux_shutdown_report("agent", "deadline hit: forcing %s cut",
			     crux_force_cut_off ? "poweroff" : "restart");

	crux_pon_force_type(crux_force_cut_off);
	crux_pshold_cut();

	for (;;)
		msleep(1000);
}

static void crux_force_cut_arm(unsigned int ms, bool off)
{
	crux_force_cut_off = off;
	mod_delayed_work(system_wq, &crux_force_cut_work, msecs_to_jiffies(ms));
	pr_emerg("[crux] shutdown deadline armed: %u ms (%s)\n", ms,
		 off ? "poweroff" : "restart");
}

/*
 * reboot notifier 在 kernel_shutdown_prepare() 里、device_shutdown() 之前跑，
 * 所以这条线是“设备拆除阶段卡住”的唯一兜底。
 */
static int crux_force_cut_notify(struct notifier_block *nb, unsigned long action,
				 void *data)
{
	if (action == SYS_POWER_OFF)
		crux_force_cut_arm(CRUX_FORCE_CUT_KERNEL_MS, true);
	else if (action == SYS_RESTART)
		crux_force_cut_arm(CRUX_FORCE_CUT_KERNEL_MS, false);

	return NOTIFY_DONE;
}

static struct notifier_block crux_force_cut_nb = {
	.notifier_call = crux_force_cut_notify,
};

static void crux_critical_init(void)
{
	struct device_node *np;
	int n, i;

	np = of_find_node_by_path("/");
	if (!np)
		return;

	n = of_property_count_strings(np, "crux,shutdown-critical");
	if (n <= 0) {
		of_node_put(np);
		return;
	}
	if (n > CRUX_CRIT_MAX)
		n = CRUX_CRIT_MAX;

	for (i = 0; i < n; i++) {
		const char *s;

		if (of_property_read_string_index(np, "crux,shutdown-critical", i,
						  &s))
			break;
		crux_crit[crux_crit_count++] = s;
	}
	of_node_put(np);

	crux_shutdown_report("agent", "critical list: %d entries",
			     crux_crit_count);
}

static int crux_imem_init(void)
{
	struct device_node *np, *child;
	void __iomem *base;
	u32 reg[2];
	int ret;

	np = of_find_compatible_node(NULL, NULL, "qcom,msm-imem");
	if (!np)
		return -ENODEV;

	child = of_get_child_by_name(np, "restart_reason");
	if (!child) {
		of_node_put(np);
		return -ENODEV;
	}

	ret = of_property_read_u32_array(child, "reg", reg, 2);
	of_node_put(child);
	if (ret) {
		of_node_put(np);
		return ret;
	}

	base = of_iomap(np, 0);
	if (!base) {
		of_node_put(np);
		return -ENOMEM;
	}

	/*
	 * [crux 2026-09-25] 这里以前用 base + reg[1]（取到了长度 4），两个字都落到
	 * IMEM 0x4，那个字每轮开机被引导链改写，所以留痕一直读成 invalid。
	 *
	 * 但也不能改成写 reg[0] 指向的厂商 restart_reason(0x65c)：那个字引导链在读，
	 * 我们写的是明文码、不是本机那种混淆格式，写进去只会给引导链添乱。
	 * 所以自己的两个字都放在实测空闲的连续零区里（parked@0x868、reason@0x86c），
	 * 厂商那个字只读不写。
	 */
	crux_imem_reason = NULL;	/* parked 定下来之后按 parked + 4 算 */

	/* [crux 2026-09-25] 留痕字：优先用自己那个子节点，没有就用 reason 后面一个字 */
	child = of_get_child_by_name(np, "crux_parked");
	crux_imem_parked = NULL;
	if (child) {
		u32 preg[2];

		if (!of_property_read_u32_array(child, "reg", preg, 2))
			crux_imem_parked = base + preg[0];
		of_node_put(child);
	}
	if (!crux_imem_parked)
		crux_imem_parked = base + 0x868;	/* 实测唯一的连续零区起点 */
	crux_imem_reason = crux_imem_parked + 4;	/* 自己的 reason 字，紧挨着 */

	/* [crux 2026-09-25] np 一直留到子节点都查完再放，之前是在这之前就 put 了 */
	of_node_put(np);
	return 0;
}

/*
 * [crux 2026-09-25] 触控关机回调的留痕：把魔术字写进 IMEM，扛得住真断电，
 * 下一轮开机读出来就知道上一轮的 device_shutdown 到底有没有调到它。
 */
void crux_shutdown_mark_fts_parked(unsigned int bits)
{
	u32 v;

	if (!crux_imem_parked)
		return;
	v = readl(crux_imem_parked);
	if ((v & 0xffff0000) != CRUX_PARKED_MAGIC)
		v = CRUX_PARKED_MAGIC;		/* 上一轮的字无效，重起一个字 */
	v |= (bits & 0xffff);
	writel(v, crux_imem_parked);
	pr_emerg("[crux] FTS parked marker: bits=0x%x -> imem 0x%08x\n",
		 bits & 0xffff, v);
}
EXPORT_SYMBOL_GPL(crux_shutdown_mark_fts_parked);

static void crux_write_restart_reason(const char *reason)
{
	u32 code = 0x77665501;	/* 普通重启 */

	if (!strcmp(reason, "bootloader"))
		code = 0x77665500;
	else if (!strcmp(reason, "recovery"))
		code = 0x77665502;
	else if (!strcmp(reason, "rtc"))
		code = 0x77665503;
	else if (!strcmp(reason, "dm-verity-corrupted"))
		code = 0x77665508;
	else if (!strcmp(reason, "dm-verity-enforcing"))
		code = 0x77665509;
	else if (!strcmp(reason, "keys-clear"))
		code = 0x7766550a;
	else if (!strncmp(reason, "oem-", 4))
		code = 0x6f656d00 | (simple_strtoul(reason + 4, NULL, 16) & 0xff);

	if (!crux_imem_reason) {
		crux_shutdown_report("reason", "%s (imem absent, code 0x%08x)",
				     reason, code);
		return;
	}

	writel(code, crux_imem_reason);
	crux_shutdown_report("reason", "%s -> 0x%08x written to imem",
			     reason, code);
}

static ssize_t report_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	return sysfs_emit(buf, "%s", crux_report_buf);
}

static ssize_t stage_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	return sysfs_emit(buf, "%s\n", crux_stage);
}

static ssize_t reason_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	return sysfs_emit(buf, "%s\n", crux_reason);
}

static ssize_t reason_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	size_t n = count;

	while (n && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		n--;
	if (n >= CRUX_REASON_MAX)
		n = CRUX_REASON_MAX - 1;
	memcpy(crux_reason, buf, n);
	crux_reason[n] = '\0';
	crux_shutdown_report("reason", "set by userspace: %s", crux_reason);
	return count;
}

/* 用户态收尾阶段取报告：也给一份“预检”，此时 device_shutdown 还没跑 */
static ssize_t prepare_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	crux_shutdown_report("agent", "PREPARE requested by userspace");
	strscpy(crux_stage, "prepare", sizeof(crux_stage));
	crux_check_ufs(false);
	crux_run_steps(CRUX_SHUTDOWN_VERIFY);
	crux_run_steps(CRUX_SHUTDOWN_PREPARE);
	crux_snap_len = crux_report_len;
	strscpy(crux_stage, "prepare-done", sizeof(crux_stage));
	return count;
}

static struct kobj_attribute attr_report = __ATTR_RO(report);
static struct kobj_attribute attr_stage = __ATTR_RO(stage);
static struct kobj_attribute attr_reason = __ATTR_RW(reason);
static struct kobj_attribute attr_prepare = __ATTR(prepare, 0200, NULL,
						   prepare_store);

static ssize_t parked_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	/* [crux 2026-09-25] 如实标注：魔术字在不在、低半字有没有来源位，
	 * 两个字段分开说，免得再出现“agent 说跑了、这里说 invalid”。
	 */
	u32 v = crux_imem_parked ? readl(crux_imem_parked) : 0;
	u32 bits = v & 0xffff;
	bool magic_ok = ((v & 0xffff0000) == CRUX_PARKED_MAGIC);
	const char *st;

	if (!crux_imem_parked)
		st = "no-word";
	else if (magic_ok)
		st = "valid";
	else if (bits)
		st = "bits-only(魔术字被清，该字被别人动过)";
	else
		st = "empty";

	return sysfs_emit(buf,
			  "%s 总线回调=%s 关机代理=%s 来源位=0x%x 原字=0x%08x\n",
			  st,
			  (bits & CRUX_PARK_FTS_BUS) ? "yes" : "no",
			  (bits & CRUX_PARK_FTS_AGENT) ? "yes" : "no",
			  bits, v);
}
static struct kobj_attribute attr_parked = __ATTR_RO(parked);

static struct attribute *crux_shutdown_attrs[] = {
	&attr_report.attr,
	&attr_stage.attr,
	&attr_reason.attr,
	&attr_prepare.attr,
	&attr_parked.attr,
	NULL,
};
ATTRIBUTE_GROUPS(crux_shutdown);

/*
 * 能睡眠的阶段。此刻用户态已经退出、device_shutdown 也已经跑完，
 * 所以这里看到的状态才是“收尾到底做没做”的真实状态。
 */
static int crux_poweroff_prepare(struct sys_off_data *data)
{
	int ufs_rw, missing, verr;

	strscpy(crux_stage, "poweroff-prepare", sizeof(crux_stage));
	crux_shutdown_report("agent", "POWEROFF_PREPARE: devices walked=%u with_cb=%u",
			     crux_dev_walked, crux_dev_with_cb);

	ufs_rw = crux_check_ufs(true);
	if (ufs_rw)
		crux_shutdown_report("agent",
			"WARN some UFS fs still rw, going on anyway");
	else
		crux_shutdown_report("agent", "UFS quiesced");

	missing = crux_check_critical();
	crux_run_steps(CRUX_SHUTDOWN_VERIFY);
	verr = crux_last_err;

	/* [crux 2026-09-27] 裁决单元那套已清掉：异常与否由文件系统上的
	 * /var/lib/crux/shutdown-clean 决定，这里只把三个数当信息记下来。 */
	crux_shutdown_report("verdict", "info: ufs_rw=%d missing_critical=%d verify_err=%d",
			     ufs_rw, missing, verr);



	return 0;
}

static void crux_print_tail(void)
{
	if (crux_report_len <= crux_snap_len)
		return;

	pr_emerg("[crux] shutdown report (after userspace snapshot):\n%s",
		 crux_report_buf + crux_snap_len);
}

/*
 * [crux 2026-10-11] 内核侧兜底原因：用户态没写（最小 rootfs 里没有写入者）时，
 * 也要知道这次关机是谁发起的，写进 IMEM / report。
 */
static void crux_reason_default(const char *def)
{
	if (!strcmp(crux_reason, "normal"))
		strscpy(crux_reason, def, sizeof(crux_reason));
}

/* 不能睡眠的阶段：跑各驱动的收尾步骤，最后一下硬断电 */
static int crux_poweroff(struct sys_off_data *data)
{
	if (crux_cut_started)
		return NOTIFY_DONE;

	crux_cut_started = true;
	strscpy(crux_stage, "poweroff", sizeof(crux_stage));
	crux_reason_default("kernel-poweroff");
	crux_shutdown_report("agent", "POWEROFF begin, reason=%s", crux_reason);
	crux_run_steps(CRUX_SHUTDOWN_POWEROFF);
	crux_print_tail();

	strscpy(crux_stage, "poweroff-not-cut", sizeof(crux_stage));
	pr_emerg("[crux] shutdown agent: poweroff chain stopped\n");

	return crux_last_err ? NOTIFY_DONE : NOTIFY_STOP;
}

static int crux_restart(struct sys_off_data *data)
{
	if (crux_cut_started)
		return NOTIFY_DONE;

	crux_cut_started = true;
	strscpy(crux_stage, "restart", sizeof(crux_stage));
	/*
	 * [crux 2026-10-11] 重启模式（data->cmd）优先：recovery / bootloader 这种
	 * 要原样保留，代理写 IMEM 时会翻成对应码；其余一律记成 kernel-restart。
	 */
	if (data && data->cmd && data->cmd[0] &&
	    (!strcmp(data->cmd, "recovery") || !strcmp(data->cmd, "bootloader")))
		crux_reason_default(data->cmd);
	else
		crux_reason_default("kernel-restart");
	crux_shutdown_report("agent", "RESTART begin, reason=%s", crux_reason);
	pr_emerg("[crux] shutdown agent: restart (reason %s)\n", crux_reason);

	crux_write_restart_reason(crux_reason);
	crux_check_critical();
	crux_run_steps(CRUX_SHUTDOWN_VERIFY);
	crux_run_steps(CRUX_SHUTDOWN_RESTART);
	crux_print_tail();

	return crux_last_err ? NOTIFY_DONE : NOTIFY_STOP;
}

static int __init crux_shutdown_agent_init(void)
{
	struct sys_off_handler *h;
	int ret;

	ret = crux_imem_init();
	crux_shutdown_report("agent", "init: imem %s",
			     ret ? "absent" : "ready");
	crux_critical_init();

	crux_snap_len = crux_report_len;

	h = register_sys_off_handler(SYS_OFF_MODE_POWER_OFF_PREPARE,
				     SYS_OFF_PRIO_DEFAULT,
				     crux_poweroff_prepare, NULL);
	if (IS_ERR(h))
		pr_warn("[crux] shutdown agent: prepare handler failed\n");

	h = register_sys_off_handler(SYS_OFF_MODE_POWER_OFF,
				     SYS_OFF_PRIO_HIGH, crux_poweroff, NULL);
	if (IS_ERR(h))
		pr_err("[crux] shutdown agent: poweroff handler failed\n");

	h = register_sys_off_handler(SYS_OFF_MODE_RESTART, SYS_OFF_PRIO_HIGH,
				     crux_restart, NULL);
	if (IS_ERR(h))
		pr_err("[crux] shutdown agent: restart handler failed\n");

	crux_shutdown_kobj = kobject_create_and_add("crux-shutdown", kernel_kobj);
	if (crux_shutdown_kobj) {
		ret = sysfs_create_groups(crux_shutdown_kobj,
					  crux_shutdown_groups);
		if (ret)
			pr_warn("[crux] shutdown agent: sysfs failed %d\n", ret);
	}

	INIT_DELAYED_WORK(&crux_force_cut_work, crux_force_cut_workfn);
	if (register_reboot_notifier(&crux_force_cut_nb))
		pr_warn("[crux] shutdown agent: reboot notifier failed\n");


	pr_info("[crux] shutdown agent: ready\n");
	return 0;
}
device_initcall(crux_shutdown_agent_init);

MODULE_DESCRIPTION("Crux shutdown agent");
MODULE_LICENSE("GPL");

/* [crux port 7.3] 下面三个符号在 6.17 由 pm8941-pwrkey / msm-poweroff / x50-ssctl 提供，
 * 7.3 树尚未打这些补丁，先给占位实现保证链接；接管后请删除。
 */
/* crux_pshold_cut() 现由 msm-poweroff.c 提供（真实现） */
bool crux_x50_link_up(void) { return false; }

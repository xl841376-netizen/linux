// SPDX-License-Identifier: GPL-2.0
/*
 * crux_dep - provider/consumer state contract (see include/linux/crux_dep.h)
 */
#include <linux/crux_dep.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>

#define CRUX_DEP_MAX_ENTRIES	64
#define CRUX_DEP_WAIT_SLICE_MS	20

struct crux_dep_entry {
	struct list_head node;
	const char *name;
	enum crux_dep_state state;
	unsigned int updates;
};

static DEFINE_MUTEX(crux_dep_lock);
static DEFINE_SPINLOCK(crux_dep_slock);
static LIST_HEAD(crux_dep_entries);
static unsigned int crux_dep_count;

/* 编译期已知的依赖名：开机预注册，运行时只查找，不在原子上下文里分配 */
static const char *const crux_dep_builtin[] = {
	"ufs",
	"x50_ee",
	"x50_power",
};

const char *crux_dep_state_name(enum crux_dep_state state)
{
	switch (state) {
	case CRUX_DEP_DOWN:	return "down";
	case CRUX_DEP_UP:	return "up";
	case CRUX_DEP_ERROR:	return "error";
	default:		return "unknown";
	}
}

static struct crux_dep_entry *crux_dep_find(const char *name)
{
	struct crux_dep_entry *e;

	list_for_each_entry(e, &crux_dep_entries, node) {
		if (!strcmp(e->name, name))
			return e;
	}
	return NULL;
}

static struct crux_dep_entry *crux_dep_lookup(const char *name)
{
	struct crux_dep_entry *e = crux_dep_find(name);

	if (e || crux_dep_count >= CRUX_DEP_MAX_ENTRIES)
		return e;

	/* 可能被 irqs_disabled/tasklet 调用：必须 GFP_ATOMIC */
	e = kzalloc(sizeof(*e), GFP_ATOMIC);
	if (!e)
		return NULL;

	e->name = kstrdup_const(name, GFP_ATOMIC);
	if (!e->name) {
		kfree(e);
		return NULL;
	}
	e->state = CRUX_DEP_UNKNOWN;
	list_add_tail(&e->node, &crux_dep_entries);
	crux_dep_count++;
	return e;
}

void crux_dep_set(const char *name, enum crux_dep_state state)
{
	struct crux_dep_entry *e;
	unsigned long flags;

	if (!name)
		return;

	/* may be called from tasklet/softirq (MHI ctrl event) -> must not sleep */
	spin_lock_irqsave(&crux_dep_slock, flags);
	e = crux_dep_lookup(name);
	if (e) {
		if (e->state != state) {
			pr_info("crux_dep: %s -> %s\n", name,
				crux_dep_state_name(state));
			e->state = state;
		}
		e->updates++;
	}
	spin_unlock_irqrestore(&crux_dep_slock, flags);
}
EXPORT_SYMBOL_GPL(crux_dep_set);

enum crux_dep_state crux_dep_get(const char *name)
{
	enum crux_dep_state state = CRUX_DEP_UNKNOWN;
	struct crux_dep_entry *e;

	unsigned long flags;

	if (!name)
		return CRUX_DEP_UNKNOWN;

	spin_lock_irqsave(&crux_dep_slock, flags);
	e = crux_dep_find(name);
	if (e)
		state = e->state;
	spin_unlock_irqrestore(&crux_dep_slock, flags);
	return state;
}
EXPORT_SYMBOL_GPL(crux_dep_get);

int crux_dep_wait(const char *name, enum crux_dep_state state,
		  unsigned long timeout_ms)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);

	for (;;) {
		if (crux_dep_get(name) == state)
			return 0;
		if (time_after(jiffies, deadline))
			return -ETIMEDOUT;
		msleep_interruptible(CRUX_DEP_WAIT_SLICE_MS);
	}
}
EXPORT_SYMBOL_GPL(crux_dep_wait);

bool crux_dep_require(const char *name, enum crux_dep_state state,
		      const char *who, bool quiet)
{
	enum crux_dep_state cur = crux_dep_get(name);

	if (cur == state)
		return true;

	if (!quiet)
		pr_warn_ratelimited("crux_dep: %s needs %s=%s but it is %s; taking default path\n",
				    who ? who : "consumer", name,
				    crux_dep_state_name(state),
				    crux_dep_state_name(cur));
	return false;
}
EXPORT_SYMBOL_GPL(crux_dep_require);

#define CRUX_DEP_GATE_MAX_RETRIERS	16

struct crux_dep_retrier {
	const char *who;
	unsigned int tries;
};

static DEFINE_SPINLOCK(crux_dep_gate_slock);
static struct crux_dep_retrier crux_dep_retriers[CRUX_DEP_GATE_MAX_RETRIERS];

static bool crux_dep_gate_bump(const char *who, unsigned int max_tries,
			       unsigned int *tries)
{
	struct crux_dep_retrier *r = NULL;
	unsigned long flags;
	int i;
	bool give_up = false;

	spin_lock_irqsave(&crux_dep_gate_slock, flags);
	for (i = 0; i < CRUX_DEP_GATE_MAX_RETRIERS; i++) {
		if (crux_dep_retriers[i].who == who ||
		    (crux_dep_retriers[i].who &&
		     !strcmp(crux_dep_retriers[i].who, who))) {
			r = &crux_dep_retriers[i];
			break;
		}
		if (!crux_dep_retriers[i].who) {
			r = &crux_dep_retriers[i];
			r->who = who;
			r->tries = 0;
			break;
		}
	}
	if (r) {
		r->tries++;
		*tries = r->tries;
		if (max_tries && r->tries >= max_tries) {
			give_up = true;
			r->tries = 0;
		}
	}
	spin_unlock_irqrestore(&crux_dep_gate_slock, flags);
	return give_up;
}

static void crux_dep_gate_reset(const char *who)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&crux_dep_gate_slock, flags);
	for (i = 0; i < CRUX_DEP_GATE_MAX_RETRIERS; i++) {
		if (crux_dep_retriers[i].who &&
		    !strcmp(crux_dep_retriers[i].who, who)) {
			crux_dep_retriers[i].tries = 0;
			break;
		}
	}
	spin_unlock_irqrestore(&crux_dep_gate_slock, flags);
}

int crux_dep_gate(const char *who, const struct crux_dep_req *reqs,
		  unsigned int n, int on_missing)
{
	unsigned int i;
	unsigned int missing = 0;
	unsigned int tries = 0;

	if (!reqs || !n)
		return 0;

	for (i = 0; i < n; i++) {
		enum crux_dep_state cur = crux_dep_get(reqs[i].name);

		if (cur == reqs[i].state)
			continue;

		missing++;
		pr_info("crux_dep: precheck %s: %s needs %s, is %s\n",
			who ? who : "consumer", reqs[i].name,
			crux_dep_state_name(reqs[i].state),
			crux_dep_state_name(cur));
	}

	if (!missing) {
		crux_dep_gate_reset(who ? who : "consumer");
		return 0;
	}

	/* 只看不判：等待本身不计入失败次数，条件一到位就自然通过。 */
	crux_dep_gate_bump(who ? who : "consumer", 0, &tries);

	pr_info("crux_dep: precheck %s: %u/%u unmet (waited %u), %s\n",
		who ? who : "consumer", missing, n, tries,
		on_missing == -EPROBE_DEFER ? "retry later" : "not starting");
	return on_missing;
}
EXPORT_SYMBOL_GPL(crux_dep_gate);

static int crux_dep_show(struct seq_file *s, void *unused)
{
	struct crux_dep_entry *e;

	mutex_lock(&crux_dep_lock);
	seq_printf(s, "%-24s %-8s %s\n", "name", "state", "updates");
	list_for_each_entry(e, &crux_dep_entries, node)
		seq_printf(s, "%-24s %-8s %u\n", e->name,
			   crux_dep_state_name(e->state), e->updates);
	mutex_unlock(&crux_dep_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(crux_dep);

static int __init crux_dep_init(void)
{
	unsigned long flags;
	unsigned int i;

	/* 预注册内置名字：name 指向字符串字面量，生命周期与内核相同，无需复制 */
	spin_lock_irqsave(&crux_dep_slock, flags);
	for (i = 0; i < ARRAY_SIZE(crux_dep_builtin); i++) {
		struct crux_dep_entry *e;

		if (crux_dep_find(crux_dep_builtin[i]))
			continue;
		if (crux_dep_count >= CRUX_DEP_MAX_ENTRIES)
			break;
		e = kzalloc(sizeof(*e), GFP_ATOMIC);
		if (!e)
			break;
		e->name = crux_dep_builtin[i];
		e->state = CRUX_DEP_UNKNOWN;
		list_add_tail(&e->node, &crux_dep_entries);
		crux_dep_count++;
	}
	spin_unlock_irqrestore(&crux_dep_slock, flags);

	debugfs_create_file("states", 0444, NULL, NULL, &crux_dep_fops);
	pr_info("crux_dep: provider/consumer state contract ready\n");
	return 0;
}
arch_initcall(crux_dep_init);

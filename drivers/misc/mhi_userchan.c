// SPDX-License-Identifier: GPL-2.0-only
/*
 * mhi_userchan.c - MHI 通道用户态直通（/dev/mhi_pipe_<n>）
 *
 * 为外挂 X50（SDX50M）用户态 SAHARA / EFS 灌装而写。语义与 CAF MHI-UCI 对齐：
 *   read()  <- DL 通道（设备 -> 主机），每次返回一个 MHI 包（超出用户缓冲的部分丢弃）
 *   write() -> UL 通道（主机 -> 设备），超过 MHI buffer_len 的写会拆成多个包排队
 *   poll()  POLLIN / POLLOUT / POLLERR
 *   open()  对通道对做 mhi_prepare_for_transfer() 并预投递 RX buffer
 *   close()（最后一个引用）做 mhi_unprepare_from_transfer()
 *
 * 节点命名：/dev/mhi_pipe_<UL 通道号>；只有 DL 的通道用 /dev/mhi_pipe_<DL 通道号>
 *   —— SAHARA 对 = /dev/mhi_pipe_2，BL（仅 DL）= /dev/mhi_pipe_25
 *
 * 注意 1：MHI TRE 的长度域只有 16 位，所以 write() 必须按 buffer_len 拆包。
 * 注意 2：modem 从 SBL 切到 AMSS 时，MHI 会把 SAHARA/BL 通道整体撤掉、换成 AMSS 通道，
 *         此时 mhi_device 会被 remove，而用户态 fd 可能还开着 —— 必须在 remove 时
 *         把 uc 标记 removed 并延后释放（否则 release() 会对已释放对象做 unprepare，
 *         触发 use-after-free / 内核崩溃）。本文件已按此处理。
 */
#define pr_fmt(fmt) "mhi_userchan: " fmt

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mhi.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

static unsigned int nr_rx_bufs = 32;
module_param(nr_rx_bufs, uint, 0644);
MODULE_PARM_DESC(nr_rx_bufs, "预投递的接收缓冲个数（默认 32）");

struct uc_pkt {
	struct list_head node;
	void *buf;
	size_t len;
};

struct uc_dev {
	struct miscdevice misc;
	char name[32];
	struct mhi_device *mhi_dev;
	size_t buf_len;
	unsigned int refcnt;
	bool removed;			/* mhi_device 已 remove，uc 等最后一个 fd 关闭再释放 */
	struct mutex lock;		/* 串行化 prepare/unprepare/释放 */
	struct list_head rx_list;
	spinlock_t rx_lock;
	wait_queue_head_t rx_wq;
	struct list_head node;
};

static LIST_HEAD(uc_devs);
static DEFINE_MUTEX(uc_devs_lock);

static struct uc_dev *uc_lookup(unsigned int minor)
{
	struct uc_dev *uc;

	list_for_each_entry(uc, &uc_devs, node)
		if (uc->misc.minor == minor)
			return uc;
	return NULL;
}

/* 返回真表示 uc 可用；uc->lock 需已持有 */
static bool uc_live(struct uc_dev *uc)
{
	return !uc->removed && uc->mhi_dev;
}

static void uc_free_pkts(struct list_head *head)
{
	struct uc_pkt *pkt, *tmp;

	list_for_each_entry_safe(pkt, tmp, head, node) {
		list_del(&pkt->node);
		kfree(pkt->buf);
		kfree(pkt);
	}
}

static void uc_dl_xfer_cb(struct mhi_device *mhi_dev, struct mhi_result *res)
{
	struct uc_dev *uc = dev_get_drvdata(&mhi_dev->dev);
	struct uc_pkt *pkt;
	unsigned long flags;

	if (!uc || !res->buf_addr)
		return;

	if (uc->removed) {
		kfree(res->buf_addr);
		return;
	}

	if (res->transaction_status) {
		/* 收包出错：缓冲直接还给 MHI，不投给用户 */
		if (mhi_queue_buf(mhi_dev, DMA_FROM_DEVICE, res->buf_addr,
				  uc->buf_len, MHI_EOT))
			kfree(res->buf_addr);
		return;
	}

	pkt = kmalloc(sizeof(*pkt), GFP_ATOMIC);
	if (!pkt)
		return;			/* 丢包（极少见） */

	pkt->buf = res->buf_addr;
	pkt->len = res->bytes_xferd;

	spin_lock_irqsave(&uc->rx_lock, flags);
	list_add_tail(&pkt->node, &uc->rx_list);
	spin_unlock_irqrestore(&uc->rx_lock, flags);

	wake_up_interruptible(&uc->rx_wq);
}

static void uc_ul_xfer_cb(struct mhi_device *mhi_dev, struct mhi_result *res)
{
	kfree(res->buf_addr);
}

static int uc_open(struct inode *inode, struct file *file)
{
	unsigned int minor = iminor(inode);
	struct uc_dev *uc;
	int ret = 0;

	mutex_lock(&uc_devs_lock);
	uc = uc_lookup(minor);
	mutex_unlock(&uc_devs_lock);
	if (!uc)
		return -ENODEV;

	mutex_lock(&uc->lock);
	if (!uc_live(uc)) {
		ret = -ENODEV;
		goto out;
	}

	if (uc->refcnt++ == 0) {
		int i;

		ret = mhi_prepare_for_transfer(uc->mhi_dev);
		if (ret) {
			uc->refcnt--;
			goto out;
		}

		if (uc->mhi_dev->dl_chan) {
			for (i = 0; i < nr_rx_bufs; i++) {
				void *buf = kmalloc(uc->buf_len, GFP_KERNEL);

				if (!buf)
					break;
				if (mhi_queue_buf(uc->mhi_dev, DMA_FROM_DEVICE, buf,
						  uc->buf_len, MHI_EOT)) {
					kfree(buf);
					break;
				}
			}
		}
	}
out:
	mutex_unlock(&uc->lock);
	if (!ret)
		file->private_data = uc;
	return ret;
}

static ssize_t uc_read(struct file *file, char __user *ubuf, size_t count,
		       loff_t *ppos)
{
	struct uc_dev *uc = file->private_data;
	struct uc_pkt *pkt;
	unsigned long flags;
	ssize_t ret;
	size_t n;

	if (!uc->mhi_dev || !uc->mhi_dev->dl_chan)
		return -EPERM;

	for (;;) {
		spin_lock_irqsave(&uc->rx_lock, flags);
		if (!list_empty(&uc->rx_list)) {
			pkt = list_first_entry(&uc->rx_list, struct uc_pkt, node);
			list_del(&pkt->node);
			spin_unlock_irqrestore(&uc->rx_lock, flags);
			break;
		}
		spin_unlock_irqrestore(&uc->rx_lock, flags);

		if (uc->removed)
			return -ENODEV;
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(uc->rx_wq,
					     !list_empty(&uc->rx_list)))
			return -ERESTARTSYS;
	}

	n = min(count, pkt->len);
	ret = copy_to_user(ubuf, pkt->buf, n) ? -EFAULT : (ssize_t)n;

	/* 缓冲还回去继续收（设备已 remove 就直接释放） */
	if (!uc->removed && uc->mhi_dev &&
	    !mhi_queue_buf(uc->mhi_dev, DMA_FROM_DEVICE, pkt->buf, uc->buf_len,
			   MHI_EOT))
		;			/* 归还成功 */
	else
		kfree(pkt->buf);
	kfree(pkt);

	return ret;
}

static ssize_t uc_write(struct file *file, const char __user *ubuf,
			size_t count, loff_t *ppos)
{
	struct uc_dev *uc = file->private_data;
	size_t done = 0;

	if (!uc->mhi_dev || !uc->mhi_dev->ul_chan)
		return -EPERM;

	while (done < count) {
		size_t n = min(count - done, uc->buf_len);
		void *buf = kmalloc(n, GFP_KERNEL);
		int ret;

		if (uc->removed)
			return done ? (ssize_t)done : -ENODEV;

		if (!buf)
			return done ? (ssize_t)done : -ENOMEM;

		if (copy_from_user(buf, ubuf + done, n)) {
			kfree(buf);
			return done ? (ssize_t)done : -EFAULT;
		}

		ret = mhi_queue_buf(uc->mhi_dev, DMA_TO_DEVICE, buf, n, MHI_EOT);
		if (ret) {
			kfree(buf);
			if (ret == -EAGAIN) {
				if (file->f_flags & O_NONBLOCK)
					return done ? (ssize_t)done : -EAGAIN;
				msleep(1);
				continue;
			}
			return done ? (ssize_t)done : ret;
		}

		done += n;
	}

	return (ssize_t)done;
}

static __poll_t uc_poll(struct file *file, poll_table *wait)
{
	struct uc_dev *uc = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &uc->rx_wq, wait);

	if (uc->removed)
		return EPOLLERR | EPOLLHUP;

	if (!list_empty(&uc->rx_list))
		mask |= EPOLLIN | EPOLLRDNORM;

	if (uc->mhi_dev && uc->mhi_dev->ul_chan &&
	    !mhi_queue_is_full(uc->mhi_dev, DMA_TO_DEVICE))
		mask |= EPOLLOUT | EPOLLWRNORM;

	return mask;
}

static int uc_release(struct inode *inode, struct file *file)
{
	struct uc_dev *uc = file->private_data;
	LIST_HEAD(dead);
	bool last, removed;
	unsigned long flags;

	if (!uc)
		return 0;

	mutex_lock(&uc->lock);
	last = (--uc->refcnt == 0);
	removed = uc->removed;
	if (last && !removed && uc->mhi_dev)
		mhi_unprepare_from_transfer(uc->mhi_dev);
	if (last) {
		spin_lock_irqsave(&uc->rx_lock, flags);
		list_splice_init(&uc->rx_list, &dead);
		spin_unlock_irqrestore(&uc->rx_lock, flags);
	}
	mutex_unlock(&uc->lock);

	uc_free_pkts(&dead);

	if (last && removed) {
		mutex_destroy(&uc->lock);
		kfree(uc);
	}

	return 0;
}

static const struct file_operations uc_fops = {
	.owner = THIS_MODULE,
	.open = uc_open,
	.read = uc_read,
	.write = uc_write,
	.poll = uc_poll,
	.release = uc_release,
};

static int uc_probe(struct mhi_device *mhi_dev, const struct mhi_device_id *id)
{
	struct uc_dev *uc;
	int ret;

	uc = kzalloc(sizeof(*uc), GFP_KERNEL);
	if (!uc)
		return -ENOMEM;

	uc->mhi_dev = mhi_dev;
	uc->buf_len = mhi_dev->mhi_cntrl->buffer_len ? : PAGE_SIZE;
	INIT_LIST_HEAD(&uc->rx_list);
	spin_lock_init(&uc->rx_lock);
	init_waitqueue_head(&uc->rx_wq);
	mutex_init(&uc->lock);

	if (mhi_dev->ul_chan)
		snprintf(uc->name, sizeof(uc->name), "mhi_pipe_%d",
			 mhi_dev->ul_chan_id);
	else
		snprintf(uc->name, sizeof(uc->name), "mhi_pipe_%d",
			 mhi_dev->dl_chan_id);

	uc->misc.name = uc->name;
	uc->misc.minor = MISC_DYNAMIC_MINOR;
	uc->misc.fops = &uc_fops;
	uc->misc.mode = 0600;

	ret = misc_register(&uc->misc);
	if (ret) {
		dev_err(&mhi_dev->dev, "misc_register(%s) 失败 %d\n",
			uc->name, ret);
		kfree(uc);
		return ret;
	}

	dev_set_drvdata(&mhi_dev->dev, uc);

	mutex_lock(&uc_devs_lock);
	list_add_tail(&uc->node, &uc_devs);
	mutex_unlock(&uc_devs_lock);

	dev_info(&mhi_dev->dev,
		 "userspace pipe /dev/%s 就绪（ul=%d dl=%d buf_len=%zu）\n",
		 uc->name, mhi_dev->ul_chan_id, mhi_dev->dl_chan_id,
		 uc->buf_len);

	return 0;
}

static void uc_remove(struct mhi_device *mhi_dev)
{
	struct uc_dev *uc = dev_get_drvdata(&mhi_dev->dev);
	bool do_free;

	if (!uc)
		return;

	dev_set_drvdata(&mhi_dev->dev, NULL);

	mutex_lock(&uc_devs_lock);
	list_del(&uc->node);
	mutex_unlock(&uc_devs_lock);

	misc_deregister(&uc->misc);

	/* 还有 fd 开着就只标记，等 release 再释放；否则就地释放 */
	mutex_lock(&uc->lock);
	uc->removed = true;
	uc->mhi_dev = NULL;
	do_free = (uc->refcnt == 0);
	mutex_unlock(&uc->lock);

	if (do_free) {
		LIST_HEAD(dead);

		spin_lock_irq(&uc->rx_lock);
		list_splice_init(&uc->rx_list, &dead);
		spin_unlock_irq(&uc->rx_lock);
		uc_free_pkts(&dead);
		mutex_destroy(&uc->lock);
		kfree(uc);
	}
}

static const struct mhi_device_id uc_id_table[] = {
	{ .chan = "SAHARA" },
	{ .chan = "BL" },
	{ .chan = "EFS" },
	{}
};
MODULE_DEVICE_TABLE(mhi, uc_id_table);

static struct mhi_driver uc_driver = {
	.id_table = uc_id_table,
	.probe = uc_probe,
	.remove = uc_remove,
	.ul_xfer_cb = uc_ul_xfer_cb,
	.dl_xfer_cb = uc_dl_xfer_cb,
	.driver = {
		.name = "mhi_userchan",
	},
};

module_mhi_driver(uc_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MHI 通道用户态直通（/dev/mhi_pipe_N），供外挂 X50 SAHARA 灌装");
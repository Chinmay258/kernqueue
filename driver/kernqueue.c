// SPDX-License-Identifier: GPL-2.0
/*
 * kernqueue.c - a character device exposing a bounded, blocking byte FIFO.
 *
 * /dev/kernqueue is one shared queue: every open file description reads from
 * and writes to the same ring. Semantics are modelled on pipes:
 *   - read() returns between 1 and count bytes, whatever is queued; it blocks
 *     while the queue is empty unless O_NONBLOCK is set (then -EAGAIN).
 *   - write() of count <= KQ_ATOMIC_MAX bytes is atomic: it waits for count
 *     bytes of space and queues all of them, or fails without queuing any.
 *   - write() of count > KQ_ATOMIC_MAX bytes waits for at least one free byte
 *     and queues as much as fits; the caller handles the short write.
 *
 * Locking (see docs/design.md for the full argument):
 *   - dev->lock (a mutex) protects the ring indices, buffer contents, the
 *     statistics and open_files. It is a mutex, not a spinlock, because the
 *     critical sections call copy_to_user()/copy_from_user(), which may fault
 *     and sleep.
 *   - Nobody sleeps on a wait queue while holding dev->lock; waiters drop it,
 *     sleep, then retake it and recheck, because another task may have
 *     consumed the data or space in between.
 *   - ring.count is also read without the lock (wait conditions, poll) via
 *     READ_ONCE, paired with WRITE_ONCE in kq_ring.h. Those lockless reads are
 *     only hints; every decision that matters is re-made under the lock.
 *
 * Wake-ups (module parameter exclusive_wake, default on):
 *   - off: every waiter is woken on every state change. Simple, but with N
 *     waiters each event causes N wake-ups of which N-1 find nothing to do
 *     (a thundering herd; measured in docs/benchmark_results.md).
 *   - on: blocked readers and writers sleep as *exclusive* waiters, so an
 *     event wakes one of them. Writers use a wake function that only accepts
 *     the wake-up when the free space covers that writer's own need, so a
 *     large writer cannot swallow a wake-up meant for a small one. A task
 *     that leaves data or space behind (or bails out after being woken)
 *     passes the wake-up on, so nothing can be left asleep while it could
 *     make progress. poll() waiters are never exclusive and always wake.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/lockdep.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include <kernqueue.h>

#include "kq_ring.h"

static unsigned int capacity = KQ_DEFAULT_CAPACITY;
module_param(capacity, uint, 0444);
MODULE_PARM_DESC(capacity, "Ring size in bytes, 4096..16777216 (default 65536)");

static bool exclusive_wake = true;
module_param(exclusive_wake, bool, 0444);
MODULE_PARM_DESC(exclusive_wake, "Wake one suitable waiter per event instead of all (default Y)");

struct kq_dev {
	struct mutex lock;		/* protects everything below except the wait queues */
	struct kq_ring ring;
	u8 *buf;
	struct kq_stats stats;		/* cumulative counters only; see kq_get_stats() */
	unsigned int open_files;
	wait_queue_head_t readq;	/* readers waiting for data */
	wait_queue_head_t writeq;	/* writers waiting for space */
	struct cdev cdev;
};

static struct kq_dev kq;
static dev_t kq_devt;
static struct class *kq_class;

/*
 * Free space a write of count bytes needs before it may proceed: all of it for
 * an atomic write, one byte otherwise.
 */
static size_t kq_write_need(size_t count)
{
	return count <= KQ_ATOMIC_MAX ? count : 1;
}

/* Lockless wake-up conditions. cap never changes after init. */
static bool kq_has_data(struct kq_dev *dev)
{
	return READ_ONCE(dev->ring.count) > 0;
}

static bool kq_has_space(struct kq_dev *dev, size_t need)
{
	return dev->ring.cap - READ_ONCE(dev->ring.count) >= need;
}

/*
 * Each call wakes every poll() waiter plus, in exclusive mode, one blocked
 * reader (or writer); with exclusive_wake off, every blocked task. Called
 * after dropping dev->lock, so a woken task does not immediately block on it.
 */
static void kq_wake_readers(struct kq_dev *dev)
{
	wake_up_interruptible_poll(&dev->readq, EPOLLIN | EPOLLRDNORM);
}

static void kq_wake_writers(struct kq_dev *dev)
{
	wake_up_interruptible_poll(&dev->writeq, EPOLLOUT | EPOLLWRNORM);
}

/*
 * Exclusive mode wakes one reader per event, so a reader that leaves data
 * behind, or was woken and then bailed out, must hand the wake-up on to the
 * next sleeping reader; likewise for writers and free space. Writers whose
 * need does not fit are skipped by kq_writer_wake(), so handing on does not
 * recreate the herd. With exclusive_wake off everybody was woken already.
 */
static void kq_pass_on_readers(struct kq_dev *dev)
{
	if (exclusive_wake && kq_has_data(dev))
		kq_wake_readers(dev);
}

static void kq_pass_on_writers(struct kq_dev *dev)
{
	if (exclusive_wake && kq_has_space(dev, 1))
		kq_wake_writers(dev);
}

static int kq_wait_for_data(struct kq_dev *dev)
{
	if (exclusive_wake)
		return wait_event_interruptible_exclusive(dev->readq,
							  kq_has_data(dev));
	return wait_event_interruptible(dev->readq, kq_has_data(dev));
}

/* A blocked writer: its wait-queue entry plus the free space it needs. */
struct kq_writer_wait {
	struct wait_queue_entry wq;
	struct kq_dev *dev;
	size_t need;
};

/*
 * Wake function of an exclusive writer, run by the waker under the wait
 * queue's spinlock. Returning 0 tells __wake_up_common() this entry did not
 * take the wake-up, so it moves on to the next waiter without using up the
 * wake-one budget: the wake-up goes to the first writer that can proceed.
 * The check is a hint like the others; the writer re-checks under dev->lock.
 */
static int kq_writer_wake(struct wait_queue_entry *wq, unsigned int mode,
			  int flags, void *key)
{
	struct kq_writer_wait *w = container_of(wq, struct kq_writer_wait, wq);

	if (!kq_has_space(w->dev, w->need))
		return 0;
	return autoremove_wake_function(wq, mode, flags, key);
}

/*
 * Sleep until need bytes are free (or a signal arrives). The exclusive path
 * is an open-coded wait_event_interruptible_exclusive() so that the entry can
 * use kq_writer_wake(). As in the macro, the condition is tested after
 * queueing and before the signal check, so a writer that was chosen by a
 * wake-up and has room proceeds even if a signal is also pending.
 */
static int kq_wait_for_space(struct kq_dev *dev, size_t need)
{
	struct kq_writer_wait w = { .dev = dev, .need = need };
	int ret = 0;

	if (!exclusive_wake)
		return wait_event_interruptible(dev->writeq,
						kq_has_space(dev, need));

	init_wait(&w.wq);
	w.wq.func = kq_writer_wake;
	for (;;) {
		prepare_to_wait_exclusive(&dev->writeq, &w.wq, TASK_INTERRUPTIBLE);
		if (kq_has_space(dev, need))
			break;
		if (signal_pending(current)) {
			ret = -ERESTARTSYS;
			break;
		}
		schedule();
	}
	finish_wait(&dev->writeq, &w.wq);
	return ret;
}

/* Warn once if the ring bookkeeping is ever inconsistent (cheap next to the copy). */
static void kq_check_ring(struct kq_dev *dev)
{
	lockdep_assert_held(&dev->lock);
	WARN_ON_ONCE(!kq_ring_valid(&dev->ring));
}

static int kq_open(struct inode *inode, struct file *filp)
{
	struct kq_dev *dev = container_of(inode->i_cdev, struct kq_dev, cdev);

	filp->private_data = dev;
	/*
	 * A FIFO has no file position: stream_open() makes lseek/pread/pwrite
	 * fail with -ESPIPE and tells the VFS not to serialize on f_pos.
	 */
	stream_open(inode, filp);

	if (mutex_lock_interruptible(&dev->lock))
		return -ERESTARTSYS;
	dev->open_files++;
	mutex_unlock(&dev->lock);

	pr_debug("open by pid %d, flags 0x%x\n", current->pid, filp->f_flags);
	return 0;
}

/*
 * Called once per file description, when its last reference goes away, so no
 * read/write/ioctl can be running on this filp. Queued data stays.
 */
static int kq_release(struct inode *inode, struct file *filp)
{
	struct kq_dev *dev = filp->private_data;

	mutex_lock(&dev->lock);
	dev->open_files--;
	mutex_unlock(&dev->lock);

	pr_debug("release by pid %d\n", current->pid);
	return 0;
}

/*
 * Copy up to n queued bytes to user space and dequeue exactly what arrived.
 * Returns the number of bytes copied (0 if the first byte faulted).
 */
static size_t kq_copy_out(struct kq_dev *dev, char __user *ubuf, size_t n)
{
	struct kq_span span;
	size_t done = 0;
	int i;

	lockdep_assert_held(&dev->lock);
	kq_ring_read_spans(&dev->ring, n, &span);
	for (i = 0; i < 2 && span.len[i]; i++) {
		unsigned long left = copy_to_user(ubuf + done,
						  dev->buf + span.off[i],
						  span.len[i]);

		done += span.len[i] - left;
		if (left)
			break;
	}
	kq_ring_consume(&dev->ring, done);
	kq_check_ring(dev);
	return done;
}

/*
 * Copy n bytes from user space into the free region. For an atomic write a
 * fault publishes nothing: the bytes already copied sit beyond tail, where no
 * reader can see them, and the next write overwrites them. For a larger write
 * the copied prefix is published, like a pipe.
 */
static size_t kq_copy_in(struct kq_dev *dev, const char __user *ubuf, size_t n,
			 bool atomic)
{
	struct kq_span span;
	size_t done = 0;
	int i;

	lockdep_assert_held(&dev->lock);
	kq_ring_write_spans(&dev->ring, n, &span);
	for (i = 0; i < 2 && span.len[i]; i++) {
		unsigned long left = copy_from_user(dev->buf + span.off[i],
						    ubuf + done, span.len[i]);

		done += span.len[i] - left;
		if (left)
			break;
	}
	if (atomic && done != n)
		done = 0;
	kq_ring_produce(&dev->ring, done);
	kq_check_ring(dev);
	return done;
}

static ssize_t kq_read(struct file *filp, char __user *ubuf, size_t count,
		       loff_t *ppos)
{
	struct kq_dev *dev = filp->private_data;
	size_t done;

	if (count == 0)
		return 0;

	if (mutex_lock_interruptible(&dev->lock))
		return -ERESTARTSYS;

	while (dev->ring.count == 0) {
		if (filp->f_flags & O_NONBLOCK) {
			dev->stats.read_eagain++;
			mutex_unlock(&dev->lock);
			return -EAGAIN;
		}
		dev->stats.read_waits++;
		mutex_unlock(&dev->lock);

		/*
		 * The wait queues us *before* testing the condition, so a write
		 * that lands between our unlock and the test still wakes us: no
		 * lost wake-up.
		 */
		if (kq_wait_for_data(dev) || mutex_lock_interruptible(&dev->lock)) {
			/* We may have been the one reader woken for this data. */
			kq_pass_on_readers(dev);
			return -ERESTARTSYS;
		}
	}

	done = kq_copy_out(dev, ubuf, min(count, dev->ring.count));
	if (done) {
		dev->stats.read_calls++;
		dev->stats.bytes_read += done;
	}
	mutex_unlock(&dev->lock);

	if (done)
		kq_wake_writers(dev);	/* space was freed */
	kq_pass_on_readers(dev);	/* data left for the next reader */
	if (!done)
		return -EFAULT;
	return done;
}

static ssize_t kq_write(struct file *filp, const char __user *ubuf,
			size_t count, loff_t *ppos)
{
	struct kq_dev *dev = filp->private_data;
	bool atomic = count <= KQ_ATOMIC_MAX;
	size_t need = kq_write_need(count);
	size_t done;

	if (count == 0)
		return 0;

	if (mutex_lock_interruptible(&dev->lock))
		return -ERESTARTSYS;

	while (kq_ring_free(&dev->ring) < need) {
		if (filp->f_flags & O_NONBLOCK) {
			dev->stats.write_eagain++;
			mutex_unlock(&dev->lock);
			return -EAGAIN;
		}
		dev->stats.write_waits++;
		mutex_unlock(&dev->lock);

		if (kq_wait_for_space(dev, need) ||
		    mutex_lock_interruptible(&dev->lock)) {
			/* We may have been the one writer woken for this space. */
			kq_pass_on_writers(dev);
			return -ERESTARTSYS;
		}
	}

	/* count is a size_t from user space; clamp before any arithmetic. */
	done = kq_copy_in(dev, ubuf,
			  atomic ? count : min(count, kq_ring_free(&dev->ring)),
			  atomic);
	if (done) {
		dev->stats.write_calls++;
		dev->stats.bytes_written += done;
	}
	mutex_unlock(&dev->lock);

	if (done)
		kq_wake_readers(dev);	/* data was queued */
	kq_pass_on_writers(dev);	/* space left for the next writer */
	if (!done)
		return -EFAULT;
	return done;
}

/*
 * Register on both wait queues first, then sample the state. If the state
 * changes after the sample, the wake-up finds our poll entry and the VFS calls
 * us again, so readiness cannot be missed.
 */
static __poll_t kq_poll(struct file *filp, poll_table *wait)
{
	struct kq_dev *dev = filp->private_data;
	size_t used;
	__poll_t mask = 0;

	poll_wait(filp, &dev->readq, wait);
	poll_wait(filp, &dev->writeq, wait);

	used = READ_ONCE(dev->ring.count);
	if ((filp->f_mode & FMODE_READ) && used > 0)
		mask |= EPOLLIN | EPOLLRDNORM;
	/*
	 * Writable means "any write of up to KQ_ATOMIC_MAX bytes will not
	 * block", the same promise pipes make for PIPE_BUF. Reporting EPOLLOUT
	 * for smaller amounts of space would let a poll loop spin on -EAGAIN.
	 */
	if ((filp->f_mode & FMODE_WRITE) && dev->ring.cap - used >= KQ_ATOMIC_MAX)
		mask |= EPOLLOUT | EPOLLWRNORM;
	return mask;
}

static long kq_get_stats(struct kq_dev *dev, struct kq_stats __user *uptr)
{
	struct kq_stats snap;

	if (mutex_lock_interruptible(&dev->lock))
		return -ERESTARTSYS;
	snap = dev->stats;
	snap.capacity = dev->ring.cap;
	snap.used = dev->ring.count;
	snap.open_files = dev->open_files;
	mutex_unlock(&dev->lock);

	/*
	 * Copy the snapshot after unlocking so a faulting user pointer never
	 * holds up readers and writers. struct kq_stats is all __u64, so it has
	 * no padding that could leak kernel stack bytes.
	 */
	if (copy_to_user(uptr, &snap, sizeof(snap)))
		return -EFAULT;
	return 0;
}

static long kq_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct kq_dev *dev = filp->private_data;

	/* Unknown commands must return -ENOTTY; isatty() and friends rely on it. */
	if (_IOC_TYPE(cmd) != KQ_IOC_MAGIC)
		return -ENOTTY;

	switch (cmd) {
	case KQ_IOC_GET_STATS:
		return kq_get_stats(dev, (struct kq_stats __user *)arg);

	case KQ_IOC_RESET:
		/* Discarding data is a modification; require a writable fd. */
		if (!(filp->f_mode & FMODE_WRITE))
			return -EBADF;
		if (mutex_lock_interruptible(&dev->lock))
			return -ERESTARTSYS;
		kq_ring_reset(&dev->ring);
		dev->stats.resets++;
		mutex_unlock(&dev->lock);
		pr_debug("reset by pid %d\n", current->pid);
		/*
		 * The queue is now empty, so every blocked writer may proceed: in
		 * exclusive mode the first one wakes and each hands on to the next.
		 */
		kq_wake_writers(dev);
		return 0;

	case KQ_IOC_CLEAR_STATS:
		if (!(filp->f_mode & FMODE_WRITE))
			return -EBADF;
		if (mutex_lock_interruptible(&dev->lock))
			return -ERESTARTSYS;
		memset(&dev->stats, 0, sizeof(dev->stats));
		mutex_unlock(&dev->lock);
		return 0;

	default:
		return -ENOTTY;
	}
}

/*
 * No .llseek: stream_open() clears FMODE_LSEEK, and vfs_llseek() then returns
 * -ESPIPE without calling into the driver. compat_ptr_ioctl is enough for
 * 32-bit callers because every argument is a pointer to a layout-stable struct
 * (or unused).
 */
static const struct file_operations kq_fops = {
	.owner		= THIS_MODULE,
	.open		= kq_open,
	.release	= kq_release,
	.read		= kq_read,
	.write		= kq_write,
	.poll		= kq_poll,
	.unlocked_ioctl	= kq_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static int __init kq_init(void)
{
	struct device *device;
	int ret;

	if (capacity < KQ_MIN_CAPACITY || capacity > KQ_MAX_CAPACITY) {
		pr_err("capacity=%u is outside [%u, %u]\n", capacity,
		       KQ_MIN_CAPACITY, KQ_MAX_CAPACITY);
		return -EINVAL;
	}

	mutex_init(&kq.lock);
	init_waitqueue_head(&kq.readq);
	init_waitqueue_head(&kq.writeq);
	kq_ring_init(&kq.ring, capacity);

	/*
	 * kvzalloc() tries kmalloc and falls back to vmalloc, so a 16 MiB ring
	 * does not need physically contiguous pages. The buffer is only touched
	 * from process context, which vmalloc memory allows.
	 */
	kq.buf = kvzalloc(capacity, GFP_KERNEL);
	if (!kq.buf)
		return -ENOMEM;

	ret = alloc_chrdev_region(&kq_devt, 0, 1, KQ_DEVICE_NAME);
	if (ret)
		goto err_free_buf;

	/* class_create() lost its owner argument in Linux 6.4. */
	kq_class = class_create(KQ_DEVICE_NAME);
	if (IS_ERR(kq_class)) {
		ret = PTR_ERR(kq_class);
		goto err_unregister;
	}

	/*
	 * Create the device (and so the /dev node) before cdev_add(): opening
	 * the node before cdev_add() just fails with -ENXIO. cdev_add() goes
	 * last because the device is live the moment it succeeds, and nothing
	 * after it can fail and force a teardown under active users.
	 */
	device = device_create(kq_class, NULL, kq_devt, NULL, KQ_DEVICE_NAME);
	if (IS_ERR(device)) {
		ret = PTR_ERR(device);
		goto err_class;
	}

	cdev_init(&kq.cdev, &kq_fops);
	kq.cdev.owner = THIS_MODULE;
	ret = cdev_add(&kq.cdev, kq_devt, 1);
	if (ret)
		goto err_device;

	pr_info("ready: /dev/%s (%d:%d), capacity %u bytes\n", KQ_DEVICE_NAME,
		MAJOR(kq_devt), MINOR(kq_devt), capacity);
	return 0;

err_device:
	device_destroy(kq_class, kq_devt);
err_class:
	class_destroy(kq_class);
err_unregister:
	unregister_chrdev_region(kq_devt, 1);
err_free_buf:
	kvfree(kq.buf);
	return ret;
}

/*
 * The module cannot be unloaded while any file is open (fops.owner pins it),
 * so no task can be inside a file operation or asleep on our wait queues here
 * and freeing the buffer is safe.
 */
static void __exit kq_exit(void)
{
	cdev_del(&kq.cdev);
	device_destroy(kq_class, kq_devt);
	class_destroy(kq_class);
	unregister_chrdev_region(kq_devt, 1);
	kvfree(kq.buf);
	pr_info("unloaded\n");
}

module_init(kq_init);
module_exit(kq_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KernQueue: bounded blocking FIFO character device");

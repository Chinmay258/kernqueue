// SPDX-License-Identifier: GPL-2.0
/*
 * kq_debugcheck.c - a deliberately buggy module that proves the debug
 * kernel's checkers are live, so a clean kernqueue run under that kernel
 * means something.
 *
 * Each load triggers exactly one bug, chosen by the "bug" parameter, and then
 * fails with -EAGAIN so the module never stays resident:
 *   bug=lock_order    mutexes taken A->B, later B->A    -> lockdep
 *                     (one thread, so it cannot actually deadlock)
 *   bug=atomic_sleep  might_fault() under a spinlock    -> DEBUG_ATOMIC_SLEEP
 *                     (the check copy_to_user() performs; nothing sleeps).
 *                     Lockdep reports it too, as "Invalid wait context",
 *                     because might_fault() also takes mmap_lock (a sleeping
 *                     lock) for lockdep's benefit.
 *   bug=oob           one-byte write past a kmalloc(64) -> KASAN
 *   bug=data_race     two kthreads, bound to two        -> KCSAN
 *                     different CPUs, incrementing a
 *                     plain int for 2 s. They must run
 *                     on different CPUs: KCSAN sees a race
 *                     when one thread touches the address
 *                     while the other stalls on a watchpoint.
 *
 * Only for a disposable debug kernel. Run it after the real tests, in the
 * order above: any lockdep report (including atomic_sleep's) turns lockdep
 * off for the rest of the boot, and a KASAN report marks it unreliable.
 * oob needs a KASAN kernel and data_race a KCSAN kernel (they exclude each
 * other).
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/compiler.h>
#include <linux/cpumask.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>

static char *bug = "";
module_param(bug, charp, 0444);
MODULE_PARM_DESC(bug, "lock_order | atomic_sleep | oob | data_race");

static DEFINE_SPINLOCK(kqd_spin);
static DEFINE_MUTEX(kqd_a);
static DEFINE_MUTEX(kqd_b);
static int kqd_counter;	/* shared with no synchronisation, on purpose */

static noinline void kqd_atomic_sleep(void)
{
	spin_lock(&kqd_spin);
	might_fault();
	spin_unlock(&kqd_spin);
}

static noinline void kqd_lock_order(void)
{
	mutex_lock(&kqd_a);
	mutex_lock(&kqd_b);
	mutex_unlock(&kqd_b);
	mutex_unlock(&kqd_a);

	mutex_lock(&kqd_b);
	mutex_lock(&kqd_a);
	mutex_unlock(&kqd_a);
	mutex_unlock(&kqd_b);
}

static noinline void kqd_oob(void)
{
	size_t len = 64;
	char *p = kmalloc(len, GFP_KERNEL);

	if (!p)
		return;
	OPTIMIZER_HIDE_VAR(len);	/* keep the compiler from flagging it */
	/*
	 * volatile (checkpatch warns) guarantees the bad store is emitted; this
	 * exact form is what was run and detected under the debug kernel.
	 */
	((volatile char *)p)[len] = 1;
	kfree(p);
}

static int kqd_racer(void *unused)
{
	unsigned long end = jiffies + msecs_to_jiffies(2000);

	while (time_before(jiffies, end)) {
		kqd_counter++;
		barrier();	/* force a real load and store every iteration */
	}
	/*
	 * Stay alive until kthread_stop(): it waits for this function to
	 * return, so no thread can still be running module text when the
	 * failed init frees it.
	 */
	while (!kthread_should_stop())
		schedule_timeout_interruptible(1);
	return 0;
}

/* Create a racer bound to cpu; it starts running on wake_up_process(). */
static struct task_struct *kqd_racer_on(unsigned int cpu)
{
	struct task_struct *t = kthread_create(kqd_racer, NULL, "kqd_racer/%u", cpu);

	if (!IS_ERR(t))
		kthread_bind(t, cpu);
	return t;
}

static noinline void kqd_data_race(void)
{
	unsigned int cpu_a = cpumask_first(cpu_online_mask);
	unsigned int cpu_b = cpumask_next(cpu_a, cpu_online_mask);
	struct task_struct *a, *b;

	if (cpu_b >= nr_cpu_ids) {
		pr_err("data_race needs two online CPUs\n");
		return;
	}
	a = kqd_racer_on(cpu_a);
	if (IS_ERR(a))
		return;
	b = kqd_racer_on(cpu_b);
	if (IS_ERR(b)) {
		kthread_stop(a);	/* never woken: kthread_stop() starts and reaps it */
		return;
	}
	wake_up_process(a);
	wake_up_process(b);
	kthread_stop(b);
	kthread_stop(a);
}

static int __init kqd_init(void)
{
	if (!strcmp(bug, "atomic_sleep"))
		kqd_atomic_sleep();
	else if (!strcmp(bug, "lock_order"))
		kqd_lock_order();
	else if (!strcmp(bug, "oob"))
		kqd_oob();
	else if (!strcmp(bug, "data_race"))
		kqd_data_race();
	else
		return -EINVAL;

	pr_info("triggered bug=%s\n", bug);
	return -EAGAIN;
}
module_init(kqd_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Deliberate bugs to verify lockdep, DEBUG_ATOMIC_SLEEP, KASAN and KCSAN");

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
 *
 * Only for a disposable debug kernel. Run it after the real tests, in the
 * order above: any lockdep report (including atomic_sleep's) turns lockdep
 * off for the rest of the boot, and a KASAN report marks it unreliable.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/compiler.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>

static char *bug = "";
module_param(bug, charp, 0444);
MODULE_PARM_DESC(bug, "lock_order | atomic_sleep | oob");

static DEFINE_SPINLOCK(kqd_spin);
static DEFINE_MUTEX(kqd_a);
static DEFINE_MUTEX(kqd_b);

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

static int __init kqd_init(void)
{
	if (!strcmp(bug, "atomic_sleep"))
		kqd_atomic_sleep();
	else if (!strcmp(bug, "lock_order"))
		kqd_lock_order();
	else if (!strcmp(bug, "oob"))
		kqd_oob();
	else
		return -EINVAL;

	pr_info("triggered bug=%s\n", bug);
	return -EAGAIN;
}
module_init(kqd_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Deliberate bugs to verify lockdep, DEBUG_ATOMIC_SLEEP and KASAN");
